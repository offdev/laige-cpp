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
//   laige-replay --diff <logA> <logB> --config <config.json> [--entries <n>]
//
// Log mode loads the recorded replay log (the M1-DET-02 versioned
// format, laige/sim/replay.h), checks its replay identity against
// (the engine's world, the config) — a mismatch is a REJECTED
// REPLAY, never a silent divergence (ADR 0002) — replays the log
// headlessly on the engine's world (runReplay: one beginFrame() +
// one runSystems per recorded tick), and prints the per-tick state
// hashes (World::stateHash) as the hash line contract:
//
//   <tick> <hash>
//
// one line per tick, tick 0 (the initial state) first, then one line
// per completed tick; <hash> is 16 lowercase hex digits. This is the
// same contract laige-detcheck enforces on scenario binaries
// (docs/api/detcheck.md).
//
// Exit codes (documented, stable for CI grepping):
//   0  log mode: the replay completed and, when --expect was given,
//      every hash matches the baseline (the summary line goes to
//      stderr; the hash lines are the ONLY stdout content — a redirect
//      captures a clean baseline file). diff mode: the two replays'
//      component states are identical over the aligned ticks.
//   1  log mode: a per-tick hash mismatch with the --expect baseline
//      (the first divergence is reported to stderr). diff mode: a
//      component-state or length divergence was found (the report is
//      on stdout).
//   2  usage, IO, config-parse, log-load, identity-mismatch, or
//      engine-create error (the message carries the actionable text)
//
// Diff mode (M1-DET-05, laige/sim/replay_diff.h — the full contract):
// loads both logs, derives a per-log config (the base config with
// THAT log's header seed — the seed is the identity field the diff
// legitimately varies), identity-checks each log against its (world,
// config) — a mismatch is a REJECTED DIFF (never a silent divergence,
// ADR 0002; both logs are reported), then diffReplays: the lock-step
// tick walk aligned on World::componentStateHash (the PRNG substream
// state is excluded — it is a function of the master seed; the honest
// full-state report is the full_state_divergent flag), and the bounded
// state-difference report at the first divergent tick (World::stateDiff,
// ≤ --entries items — 0 = count only, default 16, cap 64). The report
// is the ONLY stdout content (a stable key=value grammar, documented
// in the usage text and docs/api/replay.md); the summary and
// diagnostics go to stderr.
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
#include "laige/sim/replay_diff.h"  // diff mode (M1-DET-05)

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
      "       laige-replay --diff <logA> <logB> --config "
      "<config.json> [--entries <n>]\n"
      "\n"
      "  --log <log>             the recorded replay log (required in\n"
      "                          log mode; the laige/sim/replay.h\n"
      "                          version 1 format)\n"
      "  --config <config.json>  the engine config of the recorded\n"
      "                          run (required; the replay identity is\n"
      "                          checked against it — ADR 0002: a\n"
      "                          mismatch is a rejected replay). In diff\n"
      "                          mode the base config: each log's header\n"
      "                          seed is copied onto a per-log copy\n"
      "                          before the identity check (the seed is\n"
      "                          the identity field the diff\n"
      "                          legitimately varies)\n"
      "  --expect <baseline>     compare the replay's per-tick hashes\n"
      "                          against the baseline hash stream\n"
      "                          (one '<tick> <hash>' line per tick,\n"
      "                          tick 0 first — the detcheck scenario\n"
      "                          contract); a mismatch exits 1 with the\n"
      "                          first divergence report\n"
      "  --diff <logA> <logB>    diff two recorded replay logs (mutually\n"
      "                          exclusive with --log/--expect): replay\n"
      "                          both headlessly, align by tick, and\n"
      "                          report the first divergent tick plus\n"
      "                          the bounded state diff (the M1-DET-05\n"
      "                          report — the \"Diff-mode stdout\" section\n"
      "                          below)\n"
      "  --entries <n>           diff mode only: the bounded report size\n"
      "                          — the maximum number of differing\n"
      "                          (entity, component) item lines (0 =\n"
      "                          count only; 1..64; default 16)\n"
      "  --help, -h              this help\n"
      "\n"
      "Log-mode stdout: exactly one '<tick> <hash>' line per tick\n"
      "(tick 0 = the initial state; 16 lowercase hex hash digits) —\n"
      "nothing else. A redirect captures a clean baseline file. All\n"
      "diagnostics and the summary line go to stderr.\n"
      "\n"
      "Diff-mode stdout: a stable structured report (the M1-DET-05\n"
      "contract, docs/api/replay.md): a banner line\n"
      "'replay_diff result=<identical|divergent|length_divergence>',\n"
      "then one line each of frames_a, frames_b,\n"
      "first_divergent_tick, length_divergence, full_state_divergent,\n"
      "differing_items, reported_items, then one\n"
      "'item slot=<s> component=<id> presence=<both|a_only|b_only>\n"
      " a=<hex|-> b=<hex|->' line per reported difference (component\n"
      "0 = an entity-presence difference; '-' = that side absent).\n"
      "\n"
      "Exit codes — log mode: 0 = ok (and the hashes match the\n"
      "baseline when --expect was given), 1 = a per-tick hash mismatch\n"
      "with the baseline, 2 = usage / IO / config / log / identity\n"
      "error. Diff mode: 0 = the replays' component states are\n"
      "identical, 1 = a divergence (component-state or length) was\n"
      "found, 2 = usage / IO / config / log / identity / engine error.\n");
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

// The field-by-field identity mismatch report for one log against one
// (world, config) (the ADR 0002 report — CORE-008: a mismatch is a
// rejected replay/diff, never a silent divergence). Returns true when
// the identity mismatches (the report was printed).
bool reportIdentityMismatch(std::FILE* out, const char* label,
                            const laige::ReplayLog& log, laige::World& world,
                            const laige::EngineConfig& config) {
  const laige::ReplayIdentityDiff diff =
      laige::replayIdentityDiff(log, world, config);
  if (diff.empty()) return false;
  const laige::ReplayIdentity recorded = log.identity;
  const laige::ReplayIdentity expected =
      laige::makeReplayIdentity(world, config);
  char a[17], b[17];
  std::fprintf(out,
               "laige-replay: replay identity mismatch (%s) — the log "
               "was recorded under a different identity; a mismatch is "
               "a rejected replay, never a silent divergence (ADR 0002)\n",
               label);
  reportIdentityField(out, "seed", diff.seed,
                      (formatHex16(a, recorded.seed), a),
                      (formatHex16(b, expected.seed), b));
  reportIdentityField(out, "tick_rate_hz", diff.tickRateHz,
                      (std::to_string(recorded.tickRateHz)).c_str(),
                      (std::to_string(expected.tickRateHz)).c_str());
  reportIdentityField(out, "component_schema_hash",
                      diff.componentSchemaHash,
                      (formatHex16(a, recorded.componentSchemaHash), a),
                      (formatHex16(b, expected.componentSchemaHash), b));
  reportIdentityField(out, "math_backend_id", diff.mathBackendId,
                      (std::to_string(recorded.mathBackendId)).c_str(),
                      (std::to_string(expected.mathBackendId)).c_str());
  reportIdentityField(out, "config_hash", diff.configHash,
                      (formatHex16(a, recorded.configHash), a),
                      (formatHex16(b, expected.configHash), b));
  std::fprintf(out,
               "laige-replay: re-record the log or pass the config "
               "(and registrations) that produced it\n");
  return true;
}

// The 2 * n lowercase hex digits of a byte string (the diff report's
// a=/b= fields); "-" when the side is absent (null).
std::string formatHexBytes(const std::uint8_t* bytes, std::size_t n) {
  if (bytes == nullptr) return "-";
  static const char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(kHexDigits[bytes[i] >> 4]);
    out.push_back(kHexDigits[bytes[i] & 0x0Fu]);
  }
  return out;
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
  std::string diffPathA;
  std::string diffPathB;
  bool hasLog = false;
  bool hasConfig = false;
  bool hasDiff = false;
  std::uint32_t diffEntries = laige::kReplayDiffDefaultEntries;

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
    } else if (arg == "--diff") {
      if (i + 2 >= argc) {
        std::fprintf(stderr,
                     "laige-replay: --diff needs two log paths\n");
        printUsage(stderr);
        return 2;
      }
      hasDiff = true;
      diffPathA = argv[++i];
      diffPathB = argv[++i];
    } else if (arg == "--entries") {
      if (i + 1 >= argc) {
        std::fprintf(stderr,
                     "laige-replay: --entries needs a number\n");
        printUsage(stderr);
        return 2;
      }
      // strtoul (no exceptions — NFR-8.10): reject non-numeric,
      // negative (a leading '-' is rejected explicitly — strtoul would
      // otherwise parse the digits after it), and over-bound values.
      const char* number = argv[++i];
      char* end = nullptr;
      const unsigned long value = std::strtoul(number, &end, 10);
      if (number[0] == '-' || end == number || *end != '\0' ||
          value > laige::kMaxReplayDiffEntries) {
        std::fprintf(stderr,
                     "laige-replay: --entries needs an integer in 0..%u\n",
                     static_cast<unsigned>(laige::kMaxReplayDiffEntries));
        printUsage(stderr);
        return 2;
      }
      diffEntries = static_cast<std::uint32_t>(value);
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
  if (hasDiff) {
    if (!hasConfig) {
      std::fprintf(stderr,
                   "laige-replay: --config <config.json> is required\n");
      printUsage(stderr);
      return 2;
    }
    if (hasLog || !baselinePath.empty()) {
      std::fprintf(stderr,
                   "laige-replay: --diff is mutually exclusive with "
                   "--log/--expect\n");
      printUsage(stderr);
      return 2;
    }
  } else if (!hasLog || !hasConfig) {
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

  if (hasDiff) {
    // ---------------------------------------------------------------
    // Diff mode (M1-DET-05): replay both logs on two worlds, align by
    // tick, report the first divergence + the bounded state diff
    // (laige/sim/replay_diff.h — the full contract).
    // ---------------------------------------------------------------

    // The two logs (M1-DET-02 format; the bounded read + the
    // structural validation are loadReplay's).
    const laige::Result<laige::ReplayLog, laige::ErrorCode> logAR =
        laige::loadReplay(diffPathA);
    if (logAR.isError()) {
      std::fprintf(stderr, "laige-replay: logA: %s\n",
                   laige::errorText(logAR.error()));
      return 2;
    }
    const laige::Result<laige::ReplayLog, laige::ErrorCode> logBR =
        laige::loadReplay(diffPathB);
    if (logBR.isError()) {
      std::fprintf(stderr, "laige-replay: logB: %s\n",
                   laige::errorText(logBR.error()));
      return 2;
    }
    const laige::ReplayLog& logA = logAR.value();
    const laige::ReplayLog& logB = logBR.value();

    // The per-log configs: the base config with THAT log's header
    // seed (the seed is the identity field the diff legitimately
    // varies — the other config fields stay as passed and are
    // identity-checked below).
    laige::EngineConfig configA = config.value();
    configA.seed = logA.identity.seed;
    laige::EngineConfig configB = config.value();
    configB.seed = logB.identity.seed;

    // The two engines (the built-in registrations — this binary
    // registers no game components of its own; the M1 scenario
    // surface, see the header preamble: a log recorded with game-
    // scenario registrations is a schema-mismatch rejected diff).
    laige::Result<laige::Engine, laige::ErrorCode> engineAResult =
        laige::Engine::create(configA);
    if (engineAResult.isError()) {
      std::fprintf(stderr, "laige-replay: engineA: %s\n",
                   laige::errorText(engineAResult.error()));
      return 2;
    }
    laige::Engine engineA = std::move(engineAResult).takeValue();
    laige::Result<laige::Engine, laige::ErrorCode> engineBResult =
        laige::Engine::create(configB);
    if (engineBResult.isError()) {
      std::fprintf(stderr, "laige-replay: engineB: %s\n",
                   laige::errorText(engineBResult.error()));
      engineA.shutdown();
      return 2;
    }
    laige::Engine engineB = std::move(engineBResult).takeValue();
    laige::World& worldA = *engineA.world();
    laige::World& worldB = *engineB.world();

    // The replay identity, per log (ADR 0002): a mismatch is a
    // REJECTED diff — never a silent divergence. The report names
    // every failing log with every differing field (actionable,
    // LOG-002 shape). Both are checked so one report covers both.
    bool identityMismatch = false;
    identityMismatch |=
        reportIdentityMismatch(stderr, "logA", logA, worldA, configA);
    identityMismatch |=
        reportIdentityMismatch(stderr, "logB", logB, worldB, configB);
    if (identityMismatch) {
      engineA.shutdown();
      engineB.shutdown();
      return 2;
    }

    // The diff driver (M1-DET-05): identity + determinism checked,
    // lock-step tick walk, the first divergence + the bounded state
    // diff at that tick.
    const laige::Result<laige::ReplayDiffResult, laige::ErrorCode> diff =
        laige::diffReplays(logA, logB, worldA, worldB, configA, configB,
                           diffEntries);
    if (diff.isError()) {
      std::fprintf(stderr, "laige-replay: diff: %s\n",
                   laige::errorText(diff.error()));
      engineA.shutdown();
      engineB.shutdown();
      return 2;
    }
    const laige::ReplayDiffResult& result = diff.value();

    // The structured report: stdout ONLY (the stable M1-DET-05 report
    // grammar, docs/api/replay.md — machine-greppable for CI and the
    // editor replay viewer, M5-ED-15).
    std::printf("replay_diff result=%s\n",
                result.identical ? "identical"
                : result.lengthDivergence ? "length_divergence"
                                          : "divergent");
    std::printf("frames_a=%llu\n",
                static_cast<unsigned long long>(result.framesA));
    std::printf("frames_b=%llu\n",
                static_cast<unsigned long long>(result.framesB));
    std::printf("first_divergent_tick=%llu\n",
                static_cast<unsigned long long>(
                    result.firstDivergentTick));
    std::printf("length_divergence=%s\n",
                result.lengthDivergence ? "true" : "false");
    std::printf("full_state_divergent=%s\n",
                result.fullStateDivergent ? "true" : "false");
    std::printf("differing_items=%u\n", result.differingItems);
    std::printf("reported_items=%u\n",
                static_cast<unsigned>(result.entries.size()));
    for (const laige::World::StateDiffItem& item : result.entries) {
      std::printf("item slot=%u component=%u presence=%s a=%s b=%s\n",
                  static_cast<unsigned>(item.slot),
                  static_cast<unsigned>(item.componentId),
                  item.componentId == 0
                      ? (item.presentInA ? "a_only" : "b_only")
                      : (item.presentInA && item.presentInB
                            ? "both"
                            : (item.presentInA ? "a_only" : "b_only")),
                  formatHexBytes(item.bytesA, item.size).c_str(),
                  formatHexBytes(item.bytesB, item.size).c_str());
    }
    std::fflush(stdout);

    // The human summary: stderr (stdout carries the report only).
    if (result.identical) {
      std::fprintf(stderr,
                   "laige-replay: replays identical over %llu ticks "
                   "(component state)\n",
                   static_cast<unsigned long long>(result.framesA + 1));
    } else if (result.lengthDivergence) {
      std::fprintf(stderr,
                   "laige-replay: length divergence: replay A has %llu "
                   "frames, replay B has %llu — first tick only one "
                   "replay has: %llu\n",
                   static_cast<unsigned long long>(result.framesA),
                   static_cast<unsigned long long>(result.framesB),
                   static_cast<unsigned long long>(
                       result.firstDivergentTick));
    } else {
      std::fprintf(stderr,
                   "laige-replay: first divergence at tick %llu — %u "
                   "differing state items, %u reported (stdout)\n",
                   static_cast<unsigned long long>(
                       result.firstDivergentTick),
                   result.differingItems,
                   static_cast<unsigned>(result.entries.size()));
    }
    if (result.fullStateDivergent) {
      // Never silent (CORE-008): the component state may be identical
      // while the PRNG draw position diverged (different seeds).
      std::fprintf(stderr,
                   "laige-replay: note: the PRNG substream state "
                   "diverged (different seeds/inputs) even where the "
                   "component state matches (full_state_divergent=true)\n");
    }

    // The ordered shutdown (CONC-006), both engines.
    engineA.shutdown();
    engineB.shutdown();
    return result.identical ? 0 : 1;
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
  if (reportIdentityMismatch(stderr, "the log", log.value(), world,
                             config.value())) {
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
