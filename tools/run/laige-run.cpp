// laige-run (M1-HEAD-01): the headless engine run binary.
//
// FR-1.6 / AC-6.2: the entire engine (except presentation) runs
// without a window/GPU — required for servers, CI, and replay
// tooling. This binary is the M1 entry point for the headless form:
// it loads the declarative config (JSON — the provisional M1-HEAD-01
// config surface; M1-CFG-01 owns the full schema), builds the
// Engine, and runs it for the requested number of ticks.
//
// Usage (docs/api/engine.md, the "laige-run" section):
//
//   laige-run --headless <config.json> [--ticks N] [--replay <log>]
//
//   --headless <config.json>  run the engine headless with the given
//                             JSON config (required; the windowed mode
//                             is M2)
//   --ticks N                 run until N completed ticks (N = 0 or
//                             omitted: the server form — run until the
//                             process ends)
//   --replay <log>            REPLAY RECORDING (M1-DET-02): record the
//                             run's replay log (versioned format,
//                             laige/sim/replay.h) at <log> — opt-in,
//                             DEBUG BUILDS ONLY (release builds exit 2
//                             with the structured replay/record_
//                             disabled warn). The log is written
//                             atomically (temp + rename) and appears
//                             at <log> only when the run succeeds.
//                             Size limit: kDefaultReplaySizeLimit
//                             (128 MiB)
//
// Exit codes (documented, stable for CI grepping):
//   0  the run completed (the requested ticks reached; the summary
//      line carries the loop accounting)
//   1  the engine run reported a failure Status (a failed frame —
//      the engine still shut down, CONC-006)
//   2  usage, IO, config-parse, or engine-create error (the message
//      carries the NFR-13.3 5-field error text where one applies)
//
// The one-line summary goes to stdout (machine-greppable, detcheck
// precedent):
//
//   laige-run headless ticks=<T> dropped_ticks=<D>
//               dropped_frames=<F> status=ok|<codeId>
//
// Headless invariants (ARCH-003, verified by the include-graph lint
// — NFR-8.11): this binary and everything it links (laige-sim,
// laige-core) touch no GL, window, audio, or input API; the
// PresentationSnapshot it drives is the headless-compatible
// presentation state (positions + alpha), not a GPU.

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for the _fsopen below
#endif

#include "laige/errors.h"
#include "laige/json.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim/engine.h"
#include "laige/sim/game_loop.h"
#include "laige/sim/replay.h"

namespace {

// The 1 MiB config-document bound (ADR 0003 / the JsonOptions
// default): the read stops one byte past the bound so an oversized
// file is a MalformedInput, not a truncated parse.
inline constexpr std::size_t kMaxConfigBytes = 1u << 20;

void printUsage(std::FILE* out) {
  std::fprintf(out,
      "Usage: laige-run --headless <config.json> [--ticks N] "
      "[--replay <log>]\n"
      "\n"
      "  --headless <config.json>  run the engine headless with the "
      "given\n"
      "                            JSON config (required; the windowed\n"
      "                            mode is M2)\n"
      "  --ticks N                 run until N completed ticks (N = 0\n"
      "                            or omitted: the server form — run\n"
      "                            until the process ends)\n"
      "  --replay <log>            record the run's replay log at\n"
      "                            <log> (opt-in; DEBUG BUILDS ONLY —\n"
      "                            release builds exit 2; written\n"
      "                            atomically; appears at <log> only\n"
      "                            when the run succeeds; the size\n"
      "                            limit is kDefaultReplaySizeLimit,\n"
      "                            128 MiB)\n"
      "  --help, -h                this help\n"
      "\n"
      "Exit codes: 0 = ok, 1 = engine run failure, 2 = usage / IO / "
      "config error.\n");
}

// True when `text` parses as an unsigned 64-bit decimal integer
// (digits only, no overflow); stores the value in `out` on success.
bool parseTicks(std::string_view text, std::uint64_t* out) {
  if (text.empty()) return false;
  for (const char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.data(), &end, 10);
  if (errno == ERANGE || end == nullptr || *end != '\0') return false;
  *out = static_cast<std::uint64_t>(v);
  return true;
}

// Portable config-file open (CPP-009 compile-time platform boundary —
// the logging.cpp precedent). MSVC's CRT deprecates plain `fopen`
// (C4996, fatal under the engine's /WX policy); `fopen_s` cannot be
// used here: it opens with the `_SH_SECURE` sharing mode, which denies
// re-opening of the file (the windows-msvc CI runs of M0-CORE-02 read
// back an empty file for exactly that reason). `_fsopen(path, mode,
// _SH_DENYNO)` is the CRT's documented way to open with plain-`fopen`
// sharing semantics, which every other supported compiler's `fopen`
// provides.
#if defined(_MSC_VER)
inline std::FILE* openConfigFile(const char* path, const char* mode) {
  return ::_fsopen(path, mode, _SH_DENYNO);
}
#else
inline std::FILE* openConfigFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif

// Reads the config file into a bounded buffer (the 1 MiB ADR 0003
// bound). Returns the error Status; on success the document is in
// `out`.
laige::Status readConfigFile(const std::string& path, std::string* out) {
  std::FILE* file = openConfigFile(path.c_str(), "rb");
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
    if (out->size() > kMaxConfigBytes) {
      std::fclose(file);
      return laige::ErrorCode::MalformedInput;
    }
  }
  std::fclose(file);
  return laige::Status{};
}

}  // namespace

int main(int argc, char** argv) {
  bool headless = false;
  std::string configPath;
  std::uint64_t maxTicks = 0;
  std::string replayPath;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--headless") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "laige-run: --headless needs a config path\n");
        printUsage(stderr);
        return 2;
      }
      headless = true;
      configPath = argv[++i];
    } else if (arg == "--ticks") {
      if (i + 1 >= argc || !parseTicks(argv[++i], &maxTicks)) {
        std::fprintf(stderr, "laige-run: --ticks needs a non-negative "
                             "integer\n");
        printUsage(stderr);
        return 2;
      }
    } else if (arg == "--replay") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "laige-run: --replay needs a log path\n");
        printUsage(stderr);
        return 2;
      }
      replayPath = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      printUsage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "laige-run: unknown option '%s'\n",
                   arg.c_str());
      printUsage(stderr);
      return 2;
    }
  }
  if (!headless || configPath.empty()) {
    std::fprintf(stderr, "laige-run: --headless <config.json> is required "
                         "(the windowed mode is M2)\n");
    printUsage(stderr);
    return 2;
  }

  std::string document;
  const laige::Status readStatus = readConfigFile(configPath, &document);
  if (readStatus.isError()) {
    std::fprintf(stderr, "laige-run: config: %s\n",
                 laige::errorText(readStatus.error()));
    return 2;
  }
  const laige::Result<laige::JsonValue> parsed =
      laige::parseJson(document);
  if (parsed.isError()) {
    std::fprintf(stderr, "laige-run: config: %s\n",
                 laige::errorText(parsed.error()));
    return 2;
  }
  const laige::Result<laige::EngineConfig, laige::ErrorCode> config =
      laige::parseEngineConfig(parsed.value());
  if (config.isError()) {
    std::fprintf(stderr, "laige-run: config: %s\n",
                 laige::errorText(config.error()));
    return 2;
  }
  laige::Result<laige::Engine, laige::ErrorCode> engineResult =
      laige::Engine::create(config.value());
  if (engineResult.isError()) {
    std::fprintf(stderr, "laige-run: engine: %s\n",
                 laige::errorText(engineResult.error()));
    return 2;
  }
  laige::Engine engine = std::move(engineResult).takeValue();
  // Replay recording (M1-DET-02): opt-in, debug builds only. The
  // engine's built-in registration is complete at creation (laige-run
  // registers no game components of its own), so the identity capture
  // is at the right phase: after all registration, before the run.
  // A failure here is an exit-2 usage/IO error (the flag's contract):
  // the run did not happen.
  if (!replayPath.empty()) {
    const laige::Status replayStatus = engine.startReplayRecording(
        replayPath, laige::kDefaultReplaySizeLimit);
    if (replayStatus.isError()) {
      std::fprintf(stderr, "laige-run: replay: %s\n",
                   laige::errorText(replayStatus.error()));
      return 2;
    }
  }
  const laige::Status runStatus =
      engine.run_headless(maxTicks, laige::kDefaultMaxCatchUpTicks);
  const laige::GameLoopStats stats = engine.stats();
  std::fprintf(stdout,
               "laige-run headless ticks=%llu dropped_ticks=%llu "
               "dropped_frames=%llu status=%s\n",
               static_cast<unsigned long long>(stats.ticks),
               static_cast<unsigned long long>(stats.droppedTicks),
               static_cast<unsigned long long>(stats.droppedFrames),
               runStatus.ok() ? "ok" : laige::errorName(runStatus.error()));
  // The run always ends in the ordered shutdown (CONC-006); this
  // second call exercises the idempotency (the M1-HEAD-01 test).
  engine.shutdown();
  return runStatus.ok() ? 0 : 1;
}
