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
//               [--prof-out <report>] [--budget-report [N]]
//               [--budgets <path>] [--fail-on-budget]
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
//   --budget-report [N]       BUDGET REPORT (M1-PROF-02, FR-11.2):
//                             print the last N frames' budget report
//                             to stdout at the end of the run (the
//                             AGENTS §12 field format: every declared
//                             budget — system time, total tick time,
//                             allocation count — measured vs declared
//                             with a pass/flag, plus the over-budget
//                             systems list). N: 1..kFrameBudgetWindow
//                             (32); omitted: all retained frames.
//                             EVERY build (diagnostics, not replay
//                             state). A budgets.json load failure
//                             exits 2 (the run did not happen).
//   --budgets <path>          the budgets.json file (schema v1 —
//                             laige/budget_harness.h, the M0-CORE-08
//                             table). Resolution: this arg, then the
//                             LAIGE_BUDGETS_PATH env var, then
//                             "budgets.json" in the working directory
//                             (the laige-bench resolution order).
//   --fail-on-budget          CI gate (PRD §8.1 budget policy): exit
//                             3 when the run COMPLETED but the budget
//                             report is overall=FAIL. Without it the
//                             report is printed and the run exits 0.
//
// Exit codes (documented, stable for CI grepping):
//   0  the run completed (the requested ticks reached; the summary
//      line carries the loop accounting)
//   1  the engine run reported a failure Status (a failed frame —
//      the engine still shut down, CONC-006)
//   2  usage, IO, config-parse, engine-create, profile-report-write,
//      or budget-report-start error (the message carries the
//      NFR-13.3 5-field error text where one applies)
//   3  budget failure (--fail-on-budget: the run completed, the
//      report is overall=FAIL)
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
#include <cstddef>
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
      "[--replay <log>] [--prof-out <report>] [--budget-report [N]]\n"
      "            [--budgets <path>] [--fail-on-budget]\n"
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
      "  --budget-report [N]       print the last N frames' budget\n"
      "                            report to stdout at the end of the\n"
      "                            run (M1-PROF-02, FR-11.2 — the\n"
      "                            AGENTS §12 field format: every\n"
      "                            declared budget (system time, total\n"
      "                            tick time, allocation count) measured\n"
      "                            vs declared with a pass/flag, plus\n"
      "                            the over-budget systems list). N: 1..32\n"
      "                            (kFrameBudgetWindow); omitted: all\n"
      "                            retained frames. The report needs\n"
      "                            budgets.json — see --budgets\n"
      "  --budgets <path>          the budgets.json file (schema v1 —\n"
      "                            docs/api/budget_harness.md); default:\n"
      "                            the LAIGE_BUDGETS_PATH env var, then\n"
      "                            \"budgets.json\" in the working\n"
      "                            directory (the laige-bench\n"
      "                            resolution order)\n"
      "  --fail-on-budget          exit 3 when the run COMPLETED but\n"
      "                            the budget report is overall=FAIL\n"
      "                            (the CI gate — PRD §8.1 budget\n"
      "                            policy); without it the report is\n"
      "                            printed and the run exits 0\n"
      "  --help, -h                this help\n"
      "\n"
      "Exit codes: 0 = ok, 1 = engine run failure, 2 = usage / IO /\n"
      "config / profile-report-write / budget-report-start error,\n"
      "3 = budget failure (--fail-on-budget; the run completed, the\n"
      "report is overall=FAIL).\n");
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

// Read an environment variable as a std::string (empty when unset).
// The laige-bench.cpp precedent (platform boundary, CPP-009): MSVC
// deprecates plain getenv (C4996, fatal under the engine's /WX
// policy, NFR-8.10), so the Windows branch uses the CRT's documented
// replacement, getenv_s, with the same lookup semantics.
#if defined(_MSC_VER)
// Largest environment value this tool reads (a budgets file path;
// far inside the bound). Named per CORE-005; a value beyond it is
// treated as unset (the documented fallback applies). MSVC-only:
// getenv_s needs a caller-sized buffer, so the constant has no use
// outside this branch (CORE-010: no unused symbols under -Werror).
constexpr std::size_t kEnvValueMaxBytes = 4096;

std::string envValue(const char* name) {
  char buf[kEnvValueMaxBytes];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), name) != 0) return {};
  return std::string(buf, len);
}
#else
std::string envValue(const char* name) {
  const char* v = std::getenv(name);
  return (v != nullptr) ? std::string(v) : std::string();
}
#endif

}  // namespace

int main(int argc, char** argv) {
  bool headless = false;
  std::string configPath;
  std::uint64_t maxTicks = 0;
  std::string replayPath;
  std::string profOutPath;
  bool budgetReport = false;
  std::uint32_t budgetReportN = laige::kFrameBudgetWindow;
  std::string budgetsPath;
  bool failOnBudget = false;

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
    } else if (arg == "--budget-report") {
      budgetReport = true;
      // Optional N: a decimal in 1..kFrameBudgetWindow (the omitted
      // form reports all retained frames — kFrameBudgetWindow).
      if (i + 1 < argc) {
        std::uint64_t n = 0;
        if (parseTicks(argv[i + 1], &n) && n >= 1 &&
            n <= laige::kFrameBudgetWindow) {
          budgetReportN = static_cast<std::uint32_t>(n);
          ++i;
        }
      }
    } else if (arg == "--budgets") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "laige-run: --budgets needs a budgets.json "
                             "path\n");
        printUsage(stderr);
        return 2;
      }
      budgetsPath = argv[++i];
    } else if (arg == "--fail-on-budget") {
      failOnBudget = true;
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
  // Budget report (M1-PROF-02, FR-11.2): opt-in, EVERY build. The
  // budgets.json path resolves --budgets arg, then the
  // LAIGE_BUDGETS_PATH env var, then "budgets.json" in the working
  // directory (the laige-bench resolution order). The engine loads
  // the table now (the cold setup path) and builds the report at the
  // end of the run (engine.h "The frame graph / budget report"); a
  // start failure is an exit-2 usage/IO error (the run did not
  // happen).
  if (budgetReport) {
    std::string resolvedBudgetsPath = budgetsPath;
    if (resolvedBudgetsPath.empty()) {
      resolvedBudgetsPath = envValue("LAIGE_BUDGETS_PATH");
    }
    if (resolvedBudgetsPath.empty()) resolvedBudgetsPath = "budgets.json";
    const laige::Status budgetStatus =
        engine.startBudgetReport(resolvedBudgetsPath, budgetReportN);
    if (budgetStatus.isError()) {
      std::fprintf(stderr, "laige-run: budget-report: %s (path: %s)\n",
                   laige::errorText(budgetStatus.error()),
                   resolvedBudgetsPath.c_str());
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
  // Budget report (M1-PROF-02, FR-11.2): the engine built it at the
  // end of the run, BEFORE the shutdown (the world and the profiler
  // were still live) and cached it (engine.h "The frame graph /
  // budget report"). Printed on stdout after the profile line — the
  // machine-greppable AGENTS §12 field format. Printed even on a
  // failed run (the run failure dominates the exit code — the
  // report's numbers describe what happened).
  int budgetExit = 0;
  if (budgetReport) {
    const laige::FrameBudgetReport& report = engine.lastBudgetReport();
    std::fputs(report.report.c_str(), stdout);
    // The CI gate (PRD §8.1 budget policy): the run COMPLETED and the
    // report is overall=FAIL. A failed run exits 1 regardless (the
    // gate never masks a run failure).
    if (failOnBudget && !report.passed && runStatus.ok()) budgetExit = 3;
  }
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
  return budgetExit;
}
