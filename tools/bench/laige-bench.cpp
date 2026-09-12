// laige-bench (M0-CORE-08) — the canonical benchmark command
// (docs/getting-started/building.md is the source of truth):
//
//   ./build/bin/laige-bench --suite=<name> [--runs=N] [--warmup=N]
//                           [--budget=<budget-name>] [--budgets=<path>]
//                           [--report=<path>]
//
// Runs the named synthetic suite: `warmup` discarded iterations, then
// `runs` timed iterations recorded into a laige::Histogram; prints the
// AGENTS 12 summary (sample count, min/mean/p50/p95/p99/max, context).
// With --budget=<name> the result is additionally checked against that
// budgets.json entry (loadBudgets + budgetCheck) and the pass/fail
// report is printed; a failed check exits non-zero so the command is
// CI-gateable (PRD 8.1 policy: a budget regression fails CI).
//
// Exit codes:
//   0  ok (and the budget check passed, when --budget was given)
//   1  usage error, unknown suite, or budgets.json unreadable/malformed
//   2  the budget check failed (loud failure, CORE-008)
//
// The synthetic suite is a deterministic, allocation-free stand-in
// workload: it exists so the harness (timer, histogram, report, budget
// check) is testable end to end in M0, before the real PRD 8.1
// workloads land with their subsystems (M1+). The M0-EXIT-01 gate runs
// `--suite=synthetic` and records the result in
// docs/benchmarks/baselines/m0-synthetic.md.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "laige/budget_harness.h"

namespace {

// --- The synthetic suite ----------------------------------------------------

// LCG64 constants (Marsaglia, "Random Numbers", 2003 — the 64-bit LCG;
// cited per CPP-014). The constants are arbitrary and named (CORE-005):
// the workload models no physical quantity — its job is to exercise the
// measurement pipeline (timer, histogram, report, budget check).
constexpr std::uint64_t kSyntheticLcgMultiplier = 6364136223846793005ULL;
constexpr std::uint64_t kSyntheticLcgIncrement = 1442695040888963407ULL;
constexpr int kSyntheticSteps = 4096;
constexpr std::uint64_t kSyntheticState0 = 0x1234567890ABCDEFULL;

// Measured iteration: a fixed 4096-step mixed 64-bit-integer + double
// pipeline. Deterministic, bounded, allocation-free. The accumulator is
// stored through a module-level volatile so the pipeline cannot be
// optimized away.
volatile std::int64_t g_syntheticSink = 0;

void syntheticIteration() {
  double scale = 1.0;
  std::uint64_t state = kSyntheticState0;
  for (int i = 0; i < kSyntheticSteps; ++i) {
    state = state * kSyntheticLcgMultiplier + kSyntheticLcgIncrement;
    scale += 0.5 * static_cast<double>((state >> 33) & 0xFF);
  }
  g_syntheticSink = static_cast<std::int64_t>(scale * 1.0e6) +
                    static_cast<std::int64_t>(state >> 32);
}

struct Suite {
  const char* name;
  const char* description;  // printed with --list; docs live in this file
  void (*iteration)();
};

const Suite kSuites[] = {
    {"synthetic",
     "deterministic 4096-step LCG+double pipeline; harness stand-in "
     "workload (M0; the M0-EXIT-01 baseline workload)",
     syntheticIteration},
};

// --- Argument parsing -------------------------------------------------------

struct Config {
  std::string suite;
  std::uint64_t runs = 1000;
  std::uint64_t warmup = 100;
  std::string budgetName;
  std::string budgetsPath;  // empty -> env LAIGE_BUDGETS_PATH -> "budgets.json"
  std::string reportPath;
};

bool parseUint(std::string_view text, std::uint64_t& out) {
  if (text.empty()) return false;
  std::uint64_t v = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + std::uint64_t(c - '0');
  }
  out = v;
  return true;
}

void usage(std::FILE* out) {
  std::fprintf(out,
               "usage: laige-bench --suite=<name> [--runs=N] "
               "[--warmup=N] [--budget=<budget-name>] "
               "[--budgets=<path>] [--report=<path>] [--list]\n");
}

bool parseArgs(int argc, char** argv, Config& cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    const bool hasValue = eq != std::string::npos && eq + 1 < arg.size();
    const std::string key = hasValue ? arg.substr(0, eq) : arg;
    const std::string value = hasValue ? arg.substr(eq + 1) : "";

    if (key == "--suite") {
      if (!hasValue) return false;
      cfg.suite = value;
    } else if (key == "--runs") {
      if (!hasValue || !parseUint(value, cfg.runs) || cfg.runs == 0)
        return false;
    } else if (key == "--warmup") {
      if (!hasValue || !parseUint(value, cfg.warmup)) return false;
    } else if (key == "--budget") {
      if (!hasValue) return false;
      cfg.budgetName = value;
    } else if (key == "--budgets") {
      if (!hasValue) return false;
      cfg.budgetsPath = value;
    } else if (key == "--report") {
      if (!hasValue) return false;
      cfg.reportPath = value;
    } else if (key == "--list") {
      for (const Suite& s : kSuites)
        std::printf("%-12s %s\n", s.name, s.description);
      std::exit(0);
    } else {
      return false;  // unknown argument
    }
  }
  return !cfg.suite.empty();  // --suite is required
}

// --- Output ------------------------------------------------------------------

// The compile-time build identity for the AGENTS 12 "build" context
// field (compiler + version + build type). CMake stamps the build type
// via LAIGE_BENCH_BUILD_TYPE.
#define LAIGE_BENCH_STR2(x) #x
#define LAIGE_BENCH_STR(x) LAIGE_BENCH_STR2(x)
#if defined(__GNUC__)
constexpr char kCompilerId[] = "GCC " __VERSION__;
#elif defined(__clang__)
constexpr char kCompilerId[] = "Clang " __VERSION__;
#elif defined(_MSC_VER)
constexpr char kCompilerId[] = "MSVC " LAIGE_BENCH_STR(_MSC_VER);
#else
constexpr char kCompilerId[] = "unknown";
#endif
#ifndef LAIGE_BENCH_BUILD_TYPE
#define LAIGE_BENCH_BUILD_TYPE "unknown"
#endif

// --- Platform boundary (CPP-009, pattern: logging.cpp) ----------------------
//
// MSVC deprecates plain getenv/fopen (C4996, fatal under the engine /WX
// policy, NFR-8.10); the Windows branch uses the CRT's documented
// replacements with the same lookup/open semantics every other supported
// compiler provides. _fsopen(_SH_DENYNO) keeps plain-fopen sharing
// semantics (no _SH_SECURE re-open denial, see logging.cpp).

// Largest environment value this tool reads (a machine description or a
// budgets file path; both fit far inside the bound). Named per CORE-005;
// a value beyond it is treated as unset (the documented fallback applies).
constexpr std::size_t kEnvValueMaxBytes = 4096;

#if defined(_MSC_VER)
std::string envValue(const char* name) {
  char buf[kEnvValueMaxBytes];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), name) != 0) return {};
  return std::string(buf, len);
}
std::FILE* openReportFile(const char* path, const char* mode) {
  return ::_fsopen(path, mode, _SH_DENYNO);
}
#else
std::string envValue(const char* name) {
  const char* v = std::getenv(name);
  return (v != nullptr) ? std::string(v) : std::string();
}
std::FILE* openReportFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  using laige::BudgetReportContext;
  using laige::Histogram;
  using laige::TimeIt;

  Config cfg;
  if (!parseArgs(argc, argv, cfg)) {
    usage(stderr);
    return 1;
  }

  const Suite* suite = nullptr;
  for (const Suite& s : kSuites)
    if (s.name == cfg.suite) suite = &s;
  if (suite == nullptr) {
    std::fprintf(stderr, "laige-bench: unknown suite '%s' (--list)\n",
                 cfg.suite.c_str());
    return 1;
  }

  // Warm-up (discarded) — the timer and the CPU caches settle before the
  // measured region (AGENTS 12 records the warm-up count in the report).
  for (std::uint64_t i = 0; i < cfg.warmup; ++i) suite->iteration();

  // Measured region: one TimeIt per iteration, recorded into a
  // histogram sized to the run (every sample is kept — no truncation).
  Histogram histogram(Histogram::Options{cfg.runs});
  for (std::uint64_t i = 0; i < cfg.runs; ++i) {
    TimeIt timer;
    suite->iteration();
    histogram.record(timer.elapsedMs());
  }

  // Context the tool owns (AGENTS 12: the caller harness records
  // hardware/OS/compiler/build/workload). The operator may set
  // LAIGE_BENCH_MACHINE for the machine line; the baseline document
  // records the rest (docs/benchmarks/, M0-EXIT-01).
  const std::string envMachine = envValue("LAIGE_BENCH_MACHINE");
  const std::string build =
      std::string(kCompilerId) + ", " + LAIGE_BENCH_BUILD_TYPE;

  std::string output;
  output += "suite=";
  output += suite->name;
  output += " runs=";
  output += std::to_string(cfg.runs);
  output += " warmup=";
  output += std::to_string(cfg.warmup);

  int exitCode = 0;
  if (cfg.budgetName.empty()) {
    // Plain run: the summary statistics block only (the workload line
    // carries the suite's own identity).
    const laige::HistogramStats s = histogram.stats();
    output += "\n  ";
    output += laige::formatStatsLine(s);
    output += "\n  context: workload=";
    output += suite->name;
    output += " build=";
    output += build;
    output += " machine=";
    output += envMachine;  // empty when the env var is unset
    output += " warmup=";
    output += std::to_string(cfg.warmup);
    output += "\n";
  } else {
    // Budget run: load budgets.json, check the named entry, print the
    // full AGENTS 12 report (pass/fail + before/after + statistics).
    std::string budgetsPath = cfg.budgetsPath;
    if (budgetsPath.empty()) {
      const std::string envPath = envValue("LAIGE_BUDGETS_PATH");
      budgetsPath =
          envPath.empty() ? std::string("budgets.json") : envPath;
    }
    const laige::Result<laige::BudgetTable, laige::ErrorCode> table =
        laige::loadBudgets(budgetsPath);
    if (table.isError()) {
      std::fprintf(stderr, "laige-bench: loadBudgets(\"%s\") failed: %s\n",
                   budgetsPath.c_str(), laige::errorText(table.error()));
      return 1;
    }
    const laige::BudgetEntry* entry = table.value().find(cfg.budgetName);
    if (entry == nullptr) {
      std::fprintf(stderr,
                   "laige-bench: no budget named '%s' in %s\n",
                   cfg.budgetName.c_str(), budgetsPath.c_str());
      return 1;
    }

    BudgetReportContext ctx;
    ctx.workload = entry->workload.c_str();
    ctx.build = build.c_str();
    ctx.machine = envMachine.c_str();  // outlives the budgetCheck call below
    ctx.warmup = static_cast<std::uint32_t>(cfg.warmup);

    const laige::BudgetCheckResult check =
        laige::budgetCheck(*entry, histogram, ctx);
    output += "\n";
    output += check.report;
    exitCode = check.passed ? 0 : 2;
  }

  std::fputs(output.c_str(), stdout);
  std::fflush(stdout);

  if (!cfg.reportPath.empty()) {
    std::FILE* f = openReportFile(cfg.reportPath.c_str(), "a");
    if (f == nullptr) {
      std::fprintf(stderr, "laige-bench: cannot open report file '%s'\n",
                   cfg.reportPath.c_str());
      return 1;
    }
    std::fputs(output.c_str(), f);
    std::fclose(f);
  }
  return exitCode;
}
