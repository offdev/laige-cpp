// laige-replay (M1-DET-03): the headless replay runner.
//
// FR-1.4 (determinism mode), FR-11.3 (replay: every debug run can be
// recorded and replayed bit-exactly; diff two replays by frame/state),
// PRD Appendix A (replay = input log + seed), ADR 0002 (the replay
// identity: inputs + seed + math backend + config), ARCH-007 (versioned
// replay data; readers reject explicitly), CORE-008 (a mismatch is a
// loud, actionable report — never a silent divergence).
//
// Usage (docs/api/replay.md, the "laige-replay" section):
//
//   laige-replay --log <log> --config <config.json> [--expect <baseline>]
//
// Loads the recorded replay log (the M1-DET-02 versioned format,
// laige/sim/replay.h), checks its replay identity against (the
// engine's world, the config) — a mismatch is a REJECTED REPLAY,
// never a silent divergence (ADR 0002) — replays the log headlessly
// on the engine's world (runReplay: one beginFrame() + one
// runSystems per recorded tick), and prints the per-tick state hashes
// (World::stateHash) as the hash line contract:
//
//   <tick> <hash>
//
// one line per tick, tick 0 (the initial state) first, then one line
// per completed tick; <hash> is 16 lowercase hex digits. This is the
// same contract laige-detcheck enforces on scenario binaries
// (docs/api/detcheck.md).
//
// Exit codes (documented, stable for CI grepping):
//   0  the replay completed and, when --expect was given, every hash
//      matches the baseline (the summary line goes to stderr; the hash
//      lines are the ONLY stdout content — a redirect captures a clean
//      baseline file)
//   1  a per-tick hash mismatch with the --expect baseline (the first
//      divergence is reported to stderr)
//   2  usage, IO, config-parse, log-load, identity-mismatch, or
//      engine-create error (the message carries the actionable text)
//
// Headless invariants (ARCH-003, NFR-8.11): this binary and everything
// it links (laige-sim, laige-core) touch no GL, window, audio, or
// input API.
//
// The M1 scenario surface: this binary replays logs recorded by
// `laige-run --headless` (the engine's built-in registrations — the
// binary registers no game components of its own). A game scenario
// (M1-SAMPLE-01's hello.laige) replays through the same library
// surface (laige/sim/replay.h runReplay) from the scenario's own
// binary, which registers the scenario's components and systems on
// the world before the run (the registration order is part of the
// replay identity — ADR 0002).

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for the _fsopen below
#endif

#include "laige/errors.h"
#include "laige/json.h"
#include "laige/result.h"
#include "laige/sim/engine.h"
#include "laige/sim/replay.h"

namespace {

// The 1 MiB config-document bound (the laige-run / ADR 0003 bound).
inline constexpr std::size_t kMaxConfigBytes = 1u << 20;

// The baseline-stream bounds (CORE-005): at most kMaxExpectTicks
// hash lines (the detcheck 65536-tick contract) at at most
// kMaxBaselineLineBytes each — worst case 65536 * 65 + 65536
// separators ~ 4.3 MiB; the file read cap is the round bound above.
inline constexpr std::uint64_t kMaxExpectTicks = 65536;
inline constexpr std::size_t kMaxBaselineLineBytes = 64;
inline constexpr std::size_t kMaxBaselineBytes = 8u << 20;

void printUsage(std::FILE* out) {
  std::fprintf(out,
      "Usage: laige-replay --log <log> --config <config.json> "
      "[--expect <baseline>]\n"
      "\n"
      "  --log <log>             the recorded replay log (required;\n"
      "                          the laige/sim/replay.h version 1\n"
      "                          format)\n"
      "  --config <config.json>  the engine config of the recorded\n"
      "                          run (required; the replay identity is\n"
      "                          checked against it — ADR 0002: a\n"
      "                          mismatch is a rejected replay)\n"
      "  --expect <baseline>     compare the replay's per-tick hashes\n"
      "                          against the baseline hash stream\n"
      "                          (one '<tick> <hash>' line per tick,\n"
      "                          tick 0 first — the detcheck scenario\n"
      "                          contract); a mismatch exits 1 with the\n"
      "                          first divergence report\n"
      "  --help, -h              this help\n"
      "\n"
      "stdout: exactly one '<tick> <hash>' line per tick (tick 0 = the\n"
      "initial state; 16 lowercase hex hash digits) — nothing else. A\n"
      "redirect captures a clean baseline file. All diagnostics and\n"
      "the summary line go to stderr.\n"
      "\n"
      "Exit codes: 0 = ok (and the hashes match the baseline when\n"
      "--expect was given), 1 = a per-tick hash mismatch with the\n"
      "baseline, 2 = usage / IO / config / log / identity error.\n");
}

// Portable file open (CPP-009 compile-time platform boundary — the
// laige-run.cpp precedent: MSVC's CRT deprecates plain fopen and
// fopen_s's _SH_SECURE sharing denies re-open; _fsopen(_SH_DENYNO) is
// the plain-fopen sharing semantics every other compiler provides).
#if defined(_MSC_VER)
inline std::FILE* openFile(const char* path, const char* mode) {
  return ::_fsopen(path, mode, _SH_DENYNO);
}
#else
inline std::FILE* openFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif

// Reads a file into a bounded buffer (cap bytes; over the cap is a
// MalformedInput, the ADR 0003 / loadReplay precedent).
laige::Status readFileBounded(const std::string& path, std::size_t cap,
                              std::string* out) {
  std::FILE* file = openFile(path.c_str(), "rb");
  if (file == nullptr) {
    return laige::ErrorCode::IoError;
  }
  out->clear();
  char chunk[8192];
  for (;;) {
    const std::size_t n = std::fread(chunk, 1, sizeof(chunk), file);
    if (n == 0) {
      if (std::ferror(file)) {
        std::fclose(file);
        return laige::ErrorCode::IoError;
      }
      break;  // clean EOF
    }
    out->append(chunk, n);
    if (out->size() > cap) {
      std::fclose(file);
      return laige::ErrorCode::MalformedInput;
    }
  }
  std::fclose(file);
  return laige::Status{};
}

// Formats one identity field report line: `  <name>: log=<a> cfg=<b>`.
void reportIdentityField(std::FILE* out, const char* name, bool differs,
                         const char* logValue, const char* cfgValue) {
  std::fprintf(out, "  %-20s %s  log=%s  config=%s\n", name,
               differs ? "DIFFERS" : "match", logValue, cfgValue);
}

// The 16 lowercase hex digits of a u64 (the canonical hash text form).
void formatHex16(char* out /* 17 bytes */, std::uint64_t v) {
  std::snprintf(out, 17, "%016llx",
                static_cast<unsigned long long>(v));
}

// One baseline line (the hash line contract, detcheck scenario form):
// '<tick> <hash>' — tick the exact decimal of the line index (no
// padding, no leading zeros), one space, 16 lowercase hex digits.
// Returns the parsed hash on success; the caller owns `hashHex`
// (17 bytes, NUL-terminated).
bool parseBaselineLine(std::string_view line, std::size_t lineIndex,
                       char* hashHex) {
  if (line.size() > kMaxBaselineLineBytes) return false;
  // The tick token: the exact decimal of lineIndex (a leading zero or
  // padding would parse a different number — the contract allows no
  // leading zeros, so the string form is the strict check).
  const std::string expectedTick = std::to_string(lineIndex);
  if (line.compare(0, expectedTick.size(), expectedTick) != 0) return false;
  std::size_t pos = expectedTick.size();
  if (pos >= line.size() || line[pos] != ' ') return false;
  ++pos;
  const std::size_t rest = line.size() - pos;
  if (rest != 16) return false;
  std::uint64_t hash = 0;
  for (std::size_t i = 0; i < rest; ++i) {
    const char c = line[pos + i];
    int nibble = -1;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;  // lowercase only (the contract form)
    }
    if (nibble < 0) return false;  // uppercase or non-hex rejected
    hash = (hash << 4) | static_cast<std::uint64_t>(nibble);
  }
  formatHex16(hashHex, hash);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string logPath;
  std::string configPath;
  std::string baselinePath;
  bool hasLog = false;
  bool hasConfig = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--log") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "laige-replay: --log needs a log path\n");
        printUsage(stderr);
        return 2;
      }
      hasLog = true;
      logPath = argv[++i];
    } else if (arg == "--config") {
      if (i + 1 >= argc) {
        std::fprintf(stderr,
                     "laige-replay: --config needs a config path\n");
        printUsage(stderr);
        return 2;
      }
      hasConfig = true;
      configPath = argv[++i];
    } else if (arg == "--expect") {
      if (i + 1 >= argc) {
        std::fprintf(stderr,
                     "laige-replay: --expect needs a baseline path\n");
        printUsage(stderr);
        return 2;
      }
      baselinePath = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      printUsage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "laige-replay: unknown option '%s'\n",
                   arg.c_str());
      printUsage(stderr);
      return 2;
    }
  }
  if (!hasLog || !hasConfig) {
    std::fprintf(stderr,
                 "laige-replay: --log <log> and --config <config.json> "
                 "are required\n");
    printUsage(stderr);
    return 2;
  }

  // The config (the laige-run load path: bounded read + JSON + the
  // provisional engine-config surface).
  std::string document;
  const laige::Status readStatus =
      readFileBounded(configPath, kMaxConfigBytes, &document);
  if (readStatus.isError()) {
    std::fprintf(stderr, "laige-replay: config: %s\n",
                 laige::errorText(readStatus.error()));
    return 2;
  }
  const laige::Result<laige::JsonValue> parsed = laige::parseJson(document);
  if (parsed.isError()) {
    std::fprintf(stderr, "laige-replay: config: %s\n",
                 laige::errorText(parsed.error()));
    return 2;
  }
  const laige::Result<laige::EngineConfig, laige::ErrorCode> config =
      laige::parseEngineConfig(parsed.value());
  if (config.isError()) {
    std::fprintf(stderr, "laige-replay: config: %s\n",
                 laige::errorText(config.error()));
    return 2;
  }

  // The log (M1-DET-02 format; the bounded read + the structural
  // validation are loadReplay's).
  const laige::Result<laige::ReplayLog, laige::ErrorCode> log =
      laige::loadReplay(logPath);
  if (log.isError()) {
    std::fprintf(stderr, "laige-replay: log: %s\n",
                 laige::errorText(log.error()));
    return 2;
  }
  const std::uint64_t frameCount = log.value().frames.size();

  // The engine (the built-in registrations — this binary registers no
  // game components of its own; the M1 scenario surface, see the
  // header preamble).
  laige::Result<laige::Engine, laige::ErrorCode> engineResult =
      laige::Engine::create(config.value());
  if (engineResult.isError()) {
    std::fprintf(stderr, "laige-replay: engine: %s\n",
                 laige::errorText(engineResult.error()));
    return 2;
  }
  laige::Engine engine = std::move(engineResult).takeValue();
  laige::World& world = *engine.world();

  // The replay identity (ADR 0002): a mismatch is a REJECTED replay —
  // never a silent divergence. The report names every differing field
  // with both sides' values (actionable, LOG-002 shape).
  const laige::ReplayIdentityDiff diff =
      laige::replayIdentityDiff(log.value(), world, config.value());
  if (!diff.empty()) {
    const laige::ReplayIdentity recorded = log.value().identity;
    const laige::ReplayIdentity expected =
        laige::makeReplayIdentity(world, config.value());
    char a[17], b[17];
    std::fprintf(stderr,
                 "laige-replay: replay identity mismatch — the log was "
                 "recorded under a different identity; a mismatch is a "
                 "rejected replay, never a silent divergence (ADR "
                 "0002)\n");
    reportIdentityField(stderr, "seed", diff.seed,
                        (formatHex16(a, recorded.seed), a),
                        (formatHex16(b, expected.seed), b));
    reportIdentityField(stderr, "tick_rate_hz", diff.tickRateHz,
                        (std::to_string(recorded.tickRateHz)).c_str(),
                        (std::to_string(expected.tickRateHz)).c_str());
    reportIdentityField(stderr, "component_schema_hash",
                        diff.componentSchemaHash,
                        (formatHex16(a, recorded.componentSchemaHash), a),
                        (formatHex16(b, expected.componentSchemaHash), b));
    reportIdentityField(stderr, "math_backend_id", diff.mathBackendId,
                        (std::to_string(recorded.mathBackendId)).c_str(),
                        (std::to_string(expected.mathBackendId)).c_str());
    reportIdentityField(stderr, "config_hash", diff.configHash,
                        (formatHex16(a, recorded.configHash), a),
                        (formatHex16(b, expected.configHash), b));
    std::fprintf(stderr,
                 "laige-replay: re-record the log or pass the config "
                 "(and registrations) that produced it\n");
    engine.shutdown();
    return 2;
  }

  // The execution half (M1-DET-03): identity-checked, deterministic,
  // tick by tick; the per-tick state hashes.
  const laige::Result<laige::ReplayRunResult, laige::ErrorCode> replay =
      laige::runReplay(log.value(), world, config.value());
  if (replay.isError()) {
    std::fprintf(stderr, "laige-replay: replay: %s\n",
                 laige::errorText(replay.error()));
    engine.shutdown();
    return 2;
  }
  const std::vector<std::uint64_t>& hashes = replay.value().tickHashes;

  // The hash lines: stdout ONLY (a redirect captures a clean
  // baseline).
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    char hex[17];
    formatHex16(hex, hashes[i]);
    std::printf("%llu %s\n", static_cast<unsigned long long>(i), hex);
  }
  std::fflush(stdout);

  // The optional baseline comparison (the --expect half of the
  // step's scope).
  std::string result = "ok";
  int exitCode = 0;
  if (!baselinePath.empty()) {
    if (frameCount + 1 > kMaxExpectTicks) {
      std::fprintf(stderr,
                   "laige-replay: the replay has %llu hash lines, above "
                   "the baseline bound kMaxExpectTicks=%llu — a "
                   "harness limit (CORE-005)\n",
                   static_cast<unsigned long long>(frameCount + 1),
                   static_cast<unsigned long long>(kMaxExpectTicks));
      exitCode = 2;
    } else {
      std::string baseline;
      const laige::Status baseRead =
          readFileBounded(baselinePath, kMaxBaselineBytes, &baseline);
      if (baseRead.isError()) {
        std::fprintf(stderr, "laige-replay: baseline: %s\n",
                     laige::errorText(baseRead.error()));
        exitCode = 2;
      } else {
        // Split into lines (one separator per line; an optional
        // trailing newline and an optional trailing \r are tolerated —
        // the detcheck contract).
        std::vector<std::string> lines;
        std::size_t start = 0;
        for (;;) {
          const std::size_t nl = baseline.find('\n', start);
          if (nl == std::string::npos) {
            lines.push_back(baseline.substr(start));
            break;
          }
          std::string line = baseline.substr(start, nl - start);
          if (!line.empty() && line.back() == '\r') line.pop_back();
          lines.push_back(std::move(line));
          start = nl + 1;
          if (start >= baseline.size()) break;  // trailing newline
        }
        if (!lines.empty() && lines.back().empty() &&
            baseline.size() > 0 && baseline.back() == '\n') {
          lines.pop_back();  // the optional trailing newline
        }

        bool lengthMismatch = false;
        std::size_t firstDiff = 0;
        char baseHex[17] = {0}, repHex[17] = {0};
        bool hashDiff = false;
        if (lines.size() != hashes.size()) {
          lengthMismatch = true;
          firstDiff = lines.size() < hashes.size()
                          ? lines.size()
                          : hashes.size();
        } else {
          for (std::size_t i = 0; i < lines.size(); ++i) {
            char parsedHex[17];
            if (!parseBaselineLine(lines[i], i, parsedHex)) {
              std::fprintf(stderr,
                           "laige-replay: malformed baseline line %llu: "
                           "'%s' (the contract is '<tick> <hash>': the "
                           "line index, one space, 16 lowercase hex "
                           "digits)\n",
                           static_cast<unsigned long long>(i),
                           lines[i].c_str());
              exitCode = 2;
              break;
            }
            if (std::strcmp(parsedHex,
                            (formatHex16(repHex, hashes[i]), repHex)) != 0) {
              hashDiff = true;
              firstDiff = i;
              // Exactly 16 hex characters (parseBaselineLine wrote a
              // 16-digit hash + NUL into parsedHex via formatHex16): a
              // plain byte copy. MSVC's CRT deprecates strncpy (C4996,
              // fatal under the engine's /WX policy) — the C4996 class
              // the openFile/_fsopen precedent already handles here.
              std::memcpy(baseHex, parsedHex, 16);
              baseHex[16] = '\0';
              break;
            }
          }
        }
        if (exitCode == 0 && (lengthMismatch || hashDiff)) {
          if (hashDiff) {
            std::fprintf(stderr,
                         "laige-replay: hash mismatch at tick %llu "
                         "(first divergence)\n"
                         "  baseline: %llu %s\n"
                         "  replay:   %llu %s\n",
                         static_cast<unsigned long long>(firstDiff),
                         static_cast<unsigned long long>(firstDiff),
                         baseHex, static_cast<unsigned long long>(firstDiff),
                         repHex);
            result = "mismatch";
          } else {
            std::fprintf(stderr,
                         "laige-replay: baseline stream length "
                         "mismatch: the baseline has %llu lines, the "
                         "replay produces %llu (first missing/extra at "
                         "tick %llu)\n",
                         static_cast<unsigned long long>(lines.size()),
                         static_cast<unsigned long long>(hashes.size()),
                         static_cast<unsigned long long>(firstDiff));
            result = "length_mismatch";
          }
          exitCode = 1;
        }
      }
    }
  }

  // The summary (stderr — stdout carries only the hash lines).
  std::fprintf(stderr, "laige-replay frames=%llu hash_lines=%llu "
                       "result=%s\n",
               static_cast<unsigned long long>(frameCount),
               static_cast<unsigned long long>(hashes.size()), result.c_str());

  // The ordered shutdown (CONC-006).
  engine.shutdown();
  return exitCode;
}
