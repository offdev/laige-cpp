// laige-run (M1-HEAD-01): the headless engine run binary.
//
// FR-1.6 / AC-6.2: the entire engine (except presentation) runs
// without a window/GPU — required for servers, CI, and replay
// tooling. This binary is the M1 entry point for the headless form:
// it loads the declarative config (the version 1 JSON schema —
// M1-CFG-01: laige/sim/config.h, loadGameConfig), builds the Engine,
// and runs it for the requested number of ticks.
//
// Usage (docs/api/engine.md, the "laige-run" section):
//
//   laige-run --headless <config.json> [--ticks N] [--replay <log>]
//               [--prof-out <report>]
//
//   --headless <config.json>  run the engine headless with the given
//                             JSON config (required; the windowed mode
//                             is M2)
//   --ticks N                 run until N completed ticks — a bounded
//                             run completes EXACTLY N ticks (frame
//                             budget 1: a late frame drops its extra
//                             due tick, it does not overshoot); N = 0
//                             or omitted: the server form — run until
//                             the process ends
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
//   --prof-out <report>       PROFILE REPORT (M1-PROF-01, FR-11.1
//                             file export): write the run's profile
//                             report (the version 1 JSON schema —
//                             laige/sim/profiler.h: the always-on
//                             counters, the tick/frame time windows,
//                             the world's entity/alloc fields, and
//                             every system's M1-SYS-03 timing window)
//                             at <report>, at the end of the run.
//                             EVERY build (diagnostics, not replay
//                             state). A write failure does not fail
//                             the run — it is reported on stderr and
//                             the exit code becomes 2.
//
// Exit codes (documented, stable for CI grepping):
//   0  the run completed (the requested ticks reached; the summary
//      line carries the loop accounting)
//   1  the engine run reported a failure Status (a failed frame —
//      the engine still shut down, CONC-006)
//   2  usage, IO, config-parse, engine-create, or profile-report
//      write error (the message carries the NFR-13.3 5-field error
//      text where one applies)
//
// The one-line summary goes to stdout (machine-greppable, detcheck
// precedent):
//
//   laige-run headless ticks=<T> dropped_ticks=<D>
//               dropped_frames=<F> status=ok|<codeId>
//
// followed by the profiler's one-line summary (M1-PROF-01, FR-11.1
// "exposed in the CLI") — the counters, the two time windows' stats,
// and the world-pulled fields:
//
//   laige-run profile: ticks=<T> frames=<F> tick_ms: n=… min=… …
//               frame_ms: n=… … entities_alive=… entities_total=… …
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
#include <string>
#include <string_view>
#include <utility>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim/config.h"
#include "laige/sim/engine.h"
#include "laige/sim/game_loop.h"
#include "laige/sim/replay.h"

namespace {

void printUsage(std::FILE* out) {
  std::fprintf(out,
      "Usage: laige-run --headless <config.json> [--ticks N] "
      "[--replay <log>] [--prof-out <report>]\n"
      "\n"
      "  --headless <config.json>  run the engine headless with the "
      "given\n"
      "                            JSON config (required; the windowed\n"
      "                            mode is M2)\n"
      "  --ticks N                 run until N completed ticks — a\n"
      "                            bounded run completes EXACTLY N\n"
      "                            ticks (frame budget 1: a late frame\n"
      "                            drops its extra due tick, it does not\n"
      "                            overshoot); N = 0 or omitted: the\n"
      "                            server form — run until the process\n"
      "                            ends\n"
      "  --replay <log>            record the run's replay log at\n"
      "                            <log> (opt-in; DEBUG BUILDS ONLY —\n"
      "                            release builds exit 2; written\n"
      "                            atomically; appears at <log> only\n"
      "                            when the run succeeds; the size\n"
      "                            limit is kDefaultReplaySizeLimit,\n"
      "                            128 MiB)\n"
      "  --prof-out <report>       write the run's profile report\n"
      "                            (the version 1 JSON schema: the\n"
      "                            counters, the tick/frame time\n"
      "                            windows, the world fields, and the\n"
      "                            per-system timings) at <report>, at\n"
      "                            the end of the run (M1-PROF-01; EVERY\n"
      "                            build; a write failure exits 2 — the\n"
      "                            run itself completes)\n"
      "  --help, -h                this help\n"
      "\n"
      "Exit codes: 0 = ok, 1 = engine run failure, 2 = usage / IO /\n"
      "config / profile-report-write error.\n");
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

}  // namespace

int main(int argc, char** argv) {
  bool headless = false;
  std::string configPath;
  std::uint64_t maxTicks = 0;
  std::string replayPath;
  std::string profOutPath;

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
    } else if (arg == "--prof-out") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "laige-run: --prof-out needs a report path\n");
        printUsage(stderr);
        return 2;
      }
      profOutPath = argv[++i];
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

  // The version 1 declarative config (M1-CFG-01): bounded read +
  // parse + version gate + schema validation in one cold call; the
  // rejection events are the config/* events of laige/sim/config.h.
  const laige::Result<laige::EngineConfig, laige::ErrorCode> config =
      laige::loadGameConfig(configPath);
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
  // Profile report (M1-PROF-01, FR-11.1 file export): opt-in, EVERY
  // build (diagnostics, not replay state). The report is written at
  // the end of the run (engine.h "The profiler"); a start failure is
  // an exit-2 usage error (the run did not happen).
  if (!profOutPath.empty()) {
    const laige::Status reportStatus =
        engine.startProfileReport(profOutPath);
    if (reportStatus.isError()) {
      std::fprintf(stderr, "laige-run: prof-out: %s\n",
                   laige::errorText(reportStatus.error()));
      return 2;
    }
  }
  // The frame budget (the run_headless contract, engine.h): a bounded
  // run uses budget 1 — each frame runs AT MOST one tick, so the run
  // lands EXACTLY on maxTicks under any cadence (a late frame drops
  // its extra due tick, the M1-LOOP-01 overload behavior, counted in
  // dropped_ticks — it never overshoots the target). The engine's
  // default catch-up budget would let a late frame complete several
  // due ticks at once and end the run up to budget - 1 ticks OVER
  // the requested count (measured on the macOS CI runners:
  // --ticks 32 completing 33); a recorded replay log (--replay) must
  // carry a platform-stable tick count, and "--ticks N" reads as
  // "exactly N ticks". The server form (maxTicks == 0) keeps the
  // default budget: the run never ends on its own, and a stalled
  // frame must be able to catch up.
  const std::uint32_t frameBudgetTicks =
      (maxTicks != 0) ? 1u : laige::kDefaultMaxCatchUpTicks;
  const laige::Status runStatus =
      engine.run_headless(maxTicks, frameBudgetTicks);
  const laige::GameLoopStats stats = engine.stats();
  std::fprintf(stdout,
               "laige-run headless ticks=%llu dropped_ticks=%llu "
               "dropped_frames=%llu status=%s\n",
               static_cast<unsigned long long>(stats.ticks),
               static_cast<unsigned long long>(stats.droppedTicks),
               static_cast<unsigned long long>(stats.droppedFrames),
               runStatus.ok() ? "ok" : laige::errorName(runStatus.error()));
  // The profiler's one-line summary (M1-PROF-01, FR-11.1 "exposed in
  // the CLI"): the counters, the two time windows' stats, and the
  // world-pulled fields — from the engine's cached per-run snapshot
  // (the world is released in the shutdown; engine.h "The
  // profiler").
  std::fprintf(stdout, "%s\n",
               laige::formatProfileSummaryLine(engine.profileStats())
                   .c_str());
  // The run always ends in the ordered shutdown (CONC-006); this
  // second call exercises the idempotency (the M1-HEAD-01 test).
  engine.shutdown();
  // A profile-report write failure is an IO-class error (exit 2) when
  // the run itself completed; a failed run stays exit 1 (the report
  // error, if any, is surfaced on stderr for visibility).
  if (!runStatus.ok()) {
    if (!engine.profileReportStatus().ok()) {
      std::fprintf(stderr, "laige-run: prof-out: %s\n",
                   laige::errorText(engine.profileReportStatus().error()));
    }
    return 1;
  }
  if (!engine.profileReportStatus().ok()) {
    std::fprintf(stderr, "laige-run: prof-out: %s\n",
                 laige::errorText(engine.profileReportStatus().error()));
    return 2;
  }
  return 0;
}
