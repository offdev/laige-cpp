// laige-sim frame graph / budget report suite (M1-PROF-02; PRD
// FR-11.2, §9.1 S-6, §9.3 G-R5).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - a synthetic over-budget system appears in the report with
//     correct numbers (the declared budget echoed, the measured p99
//     above it, the runs/warns/errors counters exact, the
//     over_budget: line, overall=FAIL)
//   - the declared budgets' semantics: healthy world passes
//     (overall=PASS, over_budget: none), a missing budgets.json
//     entry is a loud NO_ENTRY, and a zero-tick run is a loud
//     NO_SAMPLES (never silent — CORE-008)
//   - the Engine wiring: the opt-in report is built and CACHED at the
//     end of the run on every run path (readable after the shutdown),
//     the G-R5 events fold into the per-frame records (the
//     overrun_warns / critical_errors deltas), and the start
//     validation (empty path / double start / stopped engine /
//     load failure)
//   - the FrameBudgetRecorder ring: the fixed window keeps the
//     newest frames oldest-first and its record path allocates
//     nothing (PERF-003, non-sanitizer trees)
//
// The threshold assertions are preemption-tolerant (the
// system_timing_tests precedent): shared-runner deschedules can only
// STRETCH a measured run (never shorten it), so the synthetic burn
// (2 ms vs a 0.1 ms budget) is chosen to exceed BOTH documented
// G-R5 multipliers (1× and 3×) on its nominal floor — the warn and
// the critical fire on every completed tick whatever preemption
// stretches, so the counter assertions are exact. A bounded run
// lands EXACTLY on maxTicks (the frame budget 1 contract — a late
// frame drops its extra due tick, it never overshoots), so runs ==
// maxTicks is exact too; the per-frame distribution can vary under
// drops, so the per-frame assertions branch on each frame's own
// tick count.
//
// Runs as CTest `budget_report` (the step's Verify command:
// `ctest -R budget_report`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the suites below.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim/entity.h"
#include "laige/sim/engine.h"
#include "laige/sim/frame_budget.h"
#include "laige/sim/profiler.h"
#include "laige/sim/system.h"

#if defined(LAIGE_ALLOC_COUNTER)
#include "logging_alloc_counter.h"
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the
// build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "budget_report_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "budget_report_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "budget_report_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define BUDGET_REPORT_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define BUDGET_REPORT_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

static_assert(BUDGET_REPORT_TESTS_ACTIVE_CPLUSPLUS >= 202002L,
              "budget_report_tests must be built with C++20 "
              "(NFR-8.10); see laige_apply_engine_policy().");

// LAIGE_COMPONENT specializes laige::detail::ComponentTraits, which
// must be specialized at global scope (the component_registry_tests
// pattern).
struct BRTag {
  std::int32_t v{};
};
LAIGE_COMPONENT(BRTag)
// M1-DET-01 (G-R8): integer-only storage (determinism.h trait).
LAIGE_DETERMINISM_SAFE(BRTag, std::int32_t)

namespace {

using laige::Access;
using laige::ErrorCode;
using laige::FrameBudgetRecord;
using laige::FrameBudgetRecorder;
using laige::FrameBudgetReport;
using laige::Io;
using laige::Profiler;
using laige::Result;
using laige::Status;
using laige::SystemDef;
using laige::SystemSchedule;
using laige::World;

// ---------------------------------------------------------------------------
// The synthetic systems (the system_timing_tests burn pattern)
// ---------------------------------------------------------------------------

// The burn target (ms) of BRBurn — test plumbing: the tests set it
// before running a tick (the sim is single-threaded, PRD §10.2). 0
// means a healthy no-burn run.
double brBurnMs = 0.0;

// The burn loop's accumulator (namespace scope: written, never read —
// the loop's work must not be eliminated, and a namespace-scope
// variable carries no unused-variable diagnostic).
volatile std::uint64_t gBurnSink = 0;

// Burn roughly `ms` milliseconds of wall time (the system_timing
// pattern).
void burnMs(double ms) {
  if (ms <= 0.0) return;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              std::chrono::steady_clock::now() - start).count() < ms) {
    gBurnSink = gBurnSink + 1;
  }
}

LAIGE_SYSTEM(BRBurn, 1)
void BRBurn(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
  burnMs(brBurnMs);
}

LAIGE_SYSTEM(BRNoop, 1)
void BRNoop(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

// ---------------------------------------------------------------------------
// World + def builders
// ---------------------------------------------------------------------------

World makeWorld() {
  auto w = World::create(World::Options{16});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(16) failed: "
                  << laige::errorName(w.error());
    abort();
  }
  World world = std::move(w).takeValue();
  if (!world.registerComponent<BRTag>().ok()) {
    ADD_FAILURE() << "registerComponent<BRTag> failed";
    abort();
  }
  return world;
}

SystemDef makeDef(const char* name, laige::SystemFn fn,
                  laige::fpx16_16 budgetMs) {
  return SystemDef{name, fn, budgetMs, nullptr};
}

// The synthetic burn's declared budget (ms): 0.1 — the burn floor
// (2 ms) is 20× it, so BOTH G-R5 multipliers (1× warn, 3× critical)
// are exceeded nominally and stay exceeded under any preemption
// stretch (the preemption-tolerance note above).
laige::fpx16_16 burnBudgetMs() { return laige::fpx16_16::fromFloat(0.1f); }

// ---------------------------------------------------------------------------
// The test budgets.json files (schema v1 — written to the CWD by the
// tests, removed afterward; the paths are unique per file so parallel
// CTest entries in the same CWD never collide).
// ---------------------------------------------------------------------------

inline constexpr const char* kHealthyBudgetsPath = "brt_healthy_budgets.json";
inline constexpr const char* kNoAllocsBudgetsPath = "brt_noallocs_budgets.json";
inline constexpr const char* kBadVersionBudgetsPath =
    "brt_badversion_budgets.json";

// The version-1 table the report's tick/alloc budgets need: generous
// tick targets (1000 ms — the tick budgets are NOT the test subject;
// the per-system declared budget is) and the hard-zero allocation
// budget (the record path's zero-alloc property, sim_heap_allocs).
// The per-system section evaluates the SystemDef budgets, not
// budgets.json — so the synthetic system's FAIL is the only FAIL
// source in the over-budget test (machine-deterministic).
bool writeHealthyBudgets(const char* path) {
  const char* text =
      "{\n"
      "  \"version\": 1,\n"
      "  \"description\": \"M1-PROF-02 test budgets (brt suite)\",\n"
      "  \"budgets\": [\n"
      "    { \"name\": \"sim_tick_avg\", \"metric\": \"mean\",\n"
      "      \"unit\": \"ms\", \"target\": 1000.0, \"measured\": 0,\n"
      "      \"workload\": \"test\" },\n"
      "    { \"name\": \"sim_tick_p99\", \"metric\": \"p99\",\n"
      "      \"unit\": \"ms\", \"target\": 1000.0, \"measured\": 0,\n"
      "      \"workload\": \"test\" },\n"
      "    { \"name\": \"sim_heap_allocs\", \"metric\": \"max\",\n"
      "      \"unit\": \"allocs_per_frame\", \"target\": 0, \"measured\": 0,\n"
      "      \"workload\": \"test\" }\n"
      "  ]\n"
      "}\n";
  std::remove(path);  // clean a crashed previous run's leftover
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  const bool ok =
      std::fwrite(text, 1, std::strlen(text), f) == std::strlen(text);
  std::fclose(f);
  return ok;
}

// The same table WITHOUT the sim_heap_allocs entry (the NO_ENTRY
// test's fixture).
bool writeNoAllocsBudgets(const char* path) {
  const char* text =
      "{\n"
      "  \"version\": 1,\n"
      "  \"description\": \"M1-PROF-02 test budgets without sim_heap_allocs\",\n"
      "  \"budgets\": [\n"
      "    { \"name\": \"sim_tick_avg\", \"metric\": \"mean\",\n"
      "      \"unit\": \"ms\", \"target\": 1000.0, \"measured\": 0,\n"
      "      \"workload\": \"test\" },\n"
      "    { \"name\": \"sim_tick_p99\", \"metric\": \"p99\",\n"
      "      \"unit\": \"ms\", \"target\": 1000.0, \"measured\": 0,\n"
      "      \"workload\": \"test\" }\n"
      "  ]\n"
      "}\n";
  std::remove(path);
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  const bool ok =
      std::fwrite(text, 1, std::strlen(text), f) == std::strlen(text);
  std::fclose(f);
  return ok;
}

// An unsupported schema version (the load-failure fixture).
bool writeBadVersionBudgets(const char* path) {
  const char* text = "{\"version\": 99, \"budgets\": []}\n";
  std::remove(path);
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  const bool ok =
      std::fwrite(text, 1, std::strlen(text), f) == std::strlen(text);
  std::fclose(f);
  return ok;
}

laige::BudgetTable loadTableOrDie(const char* path) {
  Result<laige::BudgetTable, ErrorCode> table = laige::loadBudgets(path);
  if (table.isError()) {
    ADD_FAILURE() << "loadBudgets(" << path << ") failed: "
                  << laige::errorName(table.error());
    abort();
  }
  return std::move(table).takeValue();
}

// ---------------------------------------------------------------------------
// Report text helpers (machine-greppable line parsing)
// ---------------------------------------------------------------------------

// True when `text` contains `needle`.
bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// The single line of `text` that starts with `prefix` (up to the
// newline), empty when absent.
std::string lineWith(const std::string& text, std::string_view prefix) {
  const std::size_t pos = text.find(prefix);
  if (pos == std::string::npos) return {};
  // Back up to the line start (the prefix may sit mid-line if it
  // appeared earlier — not the case for the report's prefixes, but
  // the back-up keeps this helper total).
  const std::size_t start = text.rfind('\n', pos) + 1;
  const std::size_t end = text.find('\n', start);
  return text.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
}

// Count the lines of `text` that start with `prefix`.
std::size_t countLinesWith(const std::string& text, std::string_view prefix) {
  std::size_t n = 0;
  std::size_t from = 0;
  for (;;) {
    const std::size_t end = text.find('\n', from);
    if (text.compare(from, prefix.size(), prefix.data(), prefix.size()) ==
        0) {
      ++n;
    }
    if (end == std::string::npos) break;
    from = end + 1;
  }
  return n;
}

// Count the non-overlapping occurrences of `needle` in `text` (the
// budgetCheck result= lines sit mid-block, not at line starts).
std::size_t countOccurrences(const std::string& text,
                             std::string_view needle) {
  std::size_t n = 0;
  std::size_t from = 0;
  for (;;) {
    const std::size_t pos = text.find(needle, from);
    if (pos == std::string::npos) break;
    ++n;
    from = pos + needle.size();
  }
  return n;
}

// The numeric field `name=<decimal>` of one report line (parsed as a
// double); returns NaN when the field is absent (the caller
// assert-expects presence — a NaN comparison is false and reads
// loudly wrong).
double fieldOf(const std::string& line, const char* name) {
  const std::string needle = std::string(name) + "=";
  const std::size_t pos = line.find(needle);
  if (pos == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
  const std::size_t start = pos + needle.size();
  char* endp = nullptr;
  const double v = std::strtod(line.c_str() + start, &endp);
  if (endp == nullptr || (*endp != ' ' && *endp != '\0')) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return v;
}

// The report's lines (newline-separated; the trailing newline ends
// the last line).
std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t from = 0;
  for (;;) {
    const std::size_t end = text.find('\n', from);
    lines.push_back(text.substr(from, end == std::string::npos
                                      ? std::string::npos
                                      : end - from));
    if (end == std::string::npos) break;
    from = end + 1;
  }
  return lines;
}

// The unsigned field `name=<decimal>` of one report line (the
// frame lines' counters); 0 when the field is absent (a present
// field always parses — the format is fixed, frame_budget.cpp).
std::uint64_t uintField(const std::string& line, const char* name) {
  const std::string needle = std::string(name) + "=";
  const std::size_t pos = line.find(needle);
  if (pos == std::string::npos) return 0;
  const std::size_t start = pos + needle.size();
  char* endp = nullptr;
  const std::uint64_t v = std::strtoull(line.c_str() + start, &endp, 10);
  if (endp == nullptr || (*endp != ' ' && *endp != '\0')) return 0;
  return v;
}

// ---------------------------------------------------------------------------
// Log capture (the profiler_tests pattern: Warn and up only, rate
// limiting OFF — the G-R5 counters are logged every occurrence)
// ---------------------------------------------------------------------------

class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
  };

  void emit(const laige::log::LogRecord& record) override {
    if (record.severity < laige::log::Severity::Warn) return;
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

MemorySink* installCaptureSink() {
  auto mem = std::make_unique<MemorySink>();
  MemorySink* ptr = mem.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(mem);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

std::size_t countEvents(const MemorySink& sink, std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.event == event) ++n;
  }
  return n;
}

// Creates an engine, failing loudly on a setup error (the
// profiler_tests makeEngine pattern).
laige::Engine makeEngine(laige::EngineConfig config) {
  Result<laige::Engine, laige::ErrorCode> r = laige::Engine::create(config);
  if (!r.ok()) {
    ADD_FAILURE() << "Engine::create failed: " << laige::errorName(r.error());
    abort();
  }
  return std::move(r).takeValue();
}

}  // namespace

// ---------------------------------------------------------------------------
// The FrameBudgetRecorder ring (the fixed per-frame window)
// ---------------------------------------------------------------------------

TEST(BudgetReport, RecorderKeepsTheNewestFramesOldestFirst) {
  FrameBudgetRecorder rec;
  EXPECT_EQ(rec.count(), 0u);
  EXPECT_EQ(rec.totalFrames(), 0u);

  const std::uint64_t total = laige::kFrameBudgetWindow + 8;  // 40
  for (std::uint64_t i = 0; i < total; ++i) {
    FrameBudgetRecord r;
    r.frame = i;
    r.tickAfter = i;
    r.ticks = 1;
    r.frameMs = 0.001 * static_cast<double>(i + 1);
    r.simAllocs = 0;
    r.overrunWarns = 0;
    r.criticalErrors = 0;
    rec.recordFrame(r);
  }

  // The ring is bounded: the newest kFrameBudgetWindow frames are
  // retained, oldest-first; totalFrames keeps counting.
  EXPECT_EQ(rec.count(), laige::kFrameBudgetWindow);
  EXPECT_EQ(rec.totalFrames(), total);
  EXPECT_EQ(rec.at(0).frame, static_cast<std::uint64_t>(total - rec.count()));
  EXPECT_EQ(rec.at(rec.count() - 1).frame, total - 1);
  EXPECT_NEAR(rec.at(rec.count() - 1).frameMs, 0.001 * static_cast<double>(total),
              1e-9);

  // Below capacity: every frame is retained.
  FrameBudgetRecorder small;
  for (std::uint64_t i = 0; i < 4; ++i) {
    FrameBudgetRecord r;
    r.frame = i;
    small.recordFrame(r);
  }
  EXPECT_EQ(small.count(), 4u);
  EXPECT_EQ(small.totalFrames(), 4u);
  EXPECT_EQ(small.at(0).frame, 0u);
  EXPECT_EQ(small.at(3).frame, 3u);

  // reset(): back to the fresh state.
  rec.reset();
  EXPECT_EQ(rec.count(), 0u);
  EXPECT_EQ(rec.totalFrames(), 0u);
}

#if defined(LAIGE_ALLOC_COUNTER)
TEST(BudgetReport, RecorderRecordPathAllocatesNothing) {
  // The ring is fixed storage created with the object: recording past
  // the window boundary (the wrap) touches no heap (PERF-003). The
  // record path is what the engine's per-frame hot path pays.
  FrameBudgetRecorder warm;
  warm.recordFrame(FrameBudgetRecord{});  // one-time state, pre-window
  FrameBudgetRecorder rec;
  laige::test::resetAllocCounter();
  for (std::uint64_t i = 0; i < 1000; ++i) {
    FrameBudgetRecord r;
    r.frame = i;
    r.tickAfter = i;
    r.ticks = 1;
    r.simAllocs = 0;
    rec.recordFrame(r);
  }
  const std::uint64_t allocs = laige::test::allocCounter();
  std::printf("budget-report-recorder-zeroalloc frames=1000 allocs=%llu\n",
              static_cast<unsigned long long>(allocs));
  EXPECT_EQ(rec.count(), laige::kFrameBudgetWindow);
  EXPECT_EQ(rec.totalFrames(), 1000u);
  EXPECT_EQ(allocs, 0u);
}
#endif

// ---------------------------------------------------------------------------
// buildFrameBudgetReport: the healthy world (overall=PASS)
// ---------------------------------------------------------------------------

TEST(BudgetReport, HealthyWorldReportPasses) {
  ASSERT_TRUE(writeHealthyBudgets(kHealthyBudgetsPath));
  World w = makeWorld();
  // Budget 100 ms: a noop system's microsecond ticks stay under it
  // (the system_timing_tests noise-floor precedent for 100 ms
  // budgets — a stretched noop tick is a legitimate diagnostic
  // value, and it would carry its own G-R5 event, which the
  // zero-event assertions below reject).
  ASSERT_TRUE(w.registerSystem(makeDef("BRNoop", &BRNoop,
                                       laige::fpx16_16::fromInt32(100)),
                               Io<BRTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());

  Profiler prof(Profiler::Options{});
  FrameBudgetRecorder rec;
  for (std::uint32_t i = 0; i < 10; ++i) {
    w.beginFrame();
    ASSERT_TRUE(w.runSystems(sched).ok());
    // The tick window's samples (10 us — far under the 1000 ms
    // test targets) and the per-frame record (a healthy frame).
    prof.recordTick(0.01);
    FrameBudgetRecord r;
    r.frame = i;
    r.tickAfter = i;
    r.ticks = 1;
    r.frameMs = 0.01;
    r.simAllocs = 0;  // the steady state: no sim allocations
    rec.recordFrame(r);
  }

  const laige::BudgetTable budgets = loadTableOrDie(kHealthyBudgetsPath);
  const FrameBudgetReport report = buildFrameBudgetReport(rec, prof, w,
                                                          budgets);
  std::remove(kHealthyBudgetsPath);

  EXPECT_TRUE(report.passed);
  EXPECT_TRUE(contains(report.report, "laige-budget-report version=1"));
  EXPECT_TRUE(contains(report.report, "frames: n=10 total=10"));
  // All three declared budgets pass (the M0-CORE-08 report block,
  // embedded verbatim — one line each here).
  EXPECT_TRUE(
      contains(report.report,
               "budget=sim_tick_avg result=PASS metric=mean unit=ms"));
  EXPECT_TRUE(
      contains(report.report,
               "budget=sim_tick_p99 result=PASS metric=p99 unit=ms"));
  EXPECT_TRUE(contains(
      report.report,
      "budget=sim_heap_allocs result=PASS metric=max unit=allocs_per_frame"));
  // The per-system section: the noop system's declared budget vs its
  // rolling window — PASS, zero events.
  const std::string sys =
      lineWith(report.report, "laige-budget-report system id=1 name=BRNoop ");
  ASSERT_FALSE(sys.empty());
  EXPECT_TRUE(contains(sys, "budget_ms=100"));
  EXPECT_TRUE(contains(sys, "runs=10"));
  EXPECT_TRUE(contains(sys, "result=PASS"));
  EXPECT_TRUE(contains(sys, "warns=0"));
  EXPECT_TRUE(contains(sys, "errors=0"));
  // The over-budget list is empty, and the overall folds to PASS.
  EXPECT_TRUE(contains(report.report, "laige-budget-report over_budget: none"));
  EXPECT_TRUE(contains(report.report, "laige-budget-report overall=PASS"));
}

// ---------------------------------------------------------------------------
// buildFrameBudgetReport: the synthetic over-budget system (the step's
// "synthetic over-budget system ... with correct numbers")
// ---------------------------------------------------------------------------

TEST(BudgetReport, OverBudgetSystemAppearsWithCorrectNumbers) {
  ASSERT_TRUE(writeHealthyBudgets(kHealthyBudgetsPath));
  World w = makeWorld();
  // Declared budget 0.1 ms (fpx16_16, exact — ADR 0002), burn floor
  // 2 ms: 20× the budget — both G-R5 multipliers exceeded nominally
  // and under any preemption stretch (the file-header note).
  ASSERT_TRUE(w.registerSystem(makeDef("BRBurn", &BRBurn, burnBudgetMs()),
                               Io<BRTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());

  const std::uint32_t kTicks = 10;
  Profiler prof(Profiler::Options{});
  FrameBudgetRecorder rec;
  brBurnMs = 2.0;
  for (std::uint32_t i = 0; i < kTicks; ++i) {
    w.beginFrame();
    ASSERT_TRUE(w.runSystems(sched).ok());
    // The tick window samples track the burn (~2 ms each — far under
    // the 1000 ms test targets, so the tick budgets stay PASS and
    // the system budget is the report's only FAIL source).
    prof.recordTick(2.0);
    // The per-frame record: one burn tick per frame, one warn and one
    // critical per tick (the G-R5 events — 2 ms >= 1x0.1 ms and
    // 2 ms >= 3x0.1 ms on the nominal floor).
    FrameBudgetRecord r;
    r.frame = i;
    r.tickAfter = i;
    r.ticks = 1;
    r.frameMs = 2.0;
    r.simAllocs = 0;
    r.overrunWarns = 1;
    r.criticalErrors = 1;
    rec.recordFrame(r);
  }
  brBurnMs = 0.0;

  const laige::BudgetTable budgets = loadTableOrDie(kHealthyBudgetsPath);
  const FrameBudgetReport report = buildFrameBudgetReport(rec, prof, w,
                                                          budgets);
  std::remove(kHealthyBudgetsPath);

  // The overall: FAIL (a FAIL system — the tick/alloc budgets and the
  // frames' sim_allocs stay clean, so this is the system section).
  EXPECT_FALSE(report.passed);
  EXPECT_TRUE(contains(report.report, "laige-budget-report overall=FAIL"));

  // The per-frame section: every recorded frame ran the burn — one
  // warn + one critical, every frame FAIL.
  EXPECT_EQ(countLinesWith(report.report, "laige-budget-report frame="), 10u);
  for (const std::string& line : splitLines(report.report)) {
    if (line.rfind("laige-budget-report frame=", 0) != 0) continue;
    EXPECT_EQ(uintField(line, "ticks"), 1u);
    EXPECT_EQ(uintField(line, "sim_allocs"), 0u);
    EXPECT_EQ(uintField(line, "overrun_warns"), 1u);
    EXPECT_EQ(uintField(line, "critical_errors"), 1u);
    EXPECT_TRUE(contains(line, "result=FAIL"));
  }

  // The per-system section: the declared budget echoed (0.1 ms, the
  // fpx16_16 rounding to 16.16 — ~0.100006), runs exact, the
  // measured p99 above the budget, the G-R5 counters exact (10
  // warns + 10 criticals — both multipliers exceeded on the
  // nominal floor), result=FAIL.
  const std::string sys =
      lineWith(report.report, "laige-budget-report system id=1 name=BRBurn ");
  ASSERT_FALSE(sys.empty());
  const double sysBudget = fieldOf(sys, "budget_ms");
  EXPECT_NEAR(sysBudget, 0.1, 1e-4);
  EXPECT_TRUE(contains(sys, "runs=10"));
  const double p99 = fieldOf(sys, "measured_p99_ms");
  EXPECT_GE(p99, 2.0);
  EXPECT_TRUE(contains(sys, "result=FAIL"));
  EXPECT_TRUE(contains(sys, "warns=10"));
  EXPECT_TRUE(contains(sys, "errors=10"));

  // The over-budget systems list: one line for the FAIL system,
  // ascending id, with the p99 and the echoed budget.
  const std::string ob =
      lineWith(report.report, "laige-budget-report over_budget: id=1 ");
  ASSERT_FALSE(ob.empty());
  EXPECT_TRUE(contains(ob, "name=BRBurn "));
  EXPECT_GE(fieldOf(ob, "p99_ms"), 2.0);
  EXPECT_NEAR(fieldOf(ob, "budget_ms"), 0.1, 1e-4);
  EXPECT_EQ(countLinesWith(report.report, "laige-budget-report over_budget: "
                                          "id="),
            1u);

  // The machine-greppable summary (the ctest output, the
  // profiler-zeroalloc precedent).
  std::printf(
      "budget-report-overbudget runs=%u warns=10 errors=10 "
      "p99_ms=%.6g budget_ms=%.6g overall=FAIL\n",
      static_cast<unsigned>(kTicks), p99, sysBudget);
}

// ---------------------------------------------------------------------------
// The declared-budget semantics: NO_ENTRY and NO_SAMPLES (loud, never
// silent — CORE-008)
// ---------------------------------------------------------------------------

TEST(BudgetReport, MissingBudgetEntryIsNoEntry) {
  ASSERT_TRUE(writeNoAllocsBudgets(kNoAllocsBudgetsPath));
  World w = makeWorld();
  ASSERT_TRUE(w.registerSystem(makeDef("BRNoop", &BRNoop,
                                       laige::fpx16_16::fromInt32(100)),
                               Io<BRTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());

  Profiler prof(Profiler::Options{});
  FrameBudgetRecorder rec;
  for (std::uint32_t i = 0; i < 5; ++i) {
    w.beginFrame();
    ASSERT_TRUE(w.runSystems(sched).ok());
    prof.recordTick(0.01);
    FrameBudgetRecord r;
    r.frame = i;
    r.tickAfter = i;
    r.ticks = 1;
    rec.recordFrame(r);
  }

  const laige::BudgetTable budgets = loadTableOrDie(kNoAllocsBudgetsPath);
  const FrameBudgetReport report = buildFrameBudgetReport(rec, prof, w,
                                                          budgets);
  std::remove(kNoAllocsBudgetsPath);

  // The missing entry is a configuration error: a loud NO_ENTRY line,
  // and the overall folds to FAIL (never green over a broken harness).
  EXPECT_FALSE(report.passed);
  EXPECT_TRUE(
      contains(report.report, "budget=sim_heap_allocs result=NO_ENTRY"));
  EXPECT_TRUE(contains(report.report, "laige-budget-report overall=FAIL"));
  // The present entries still evaluate normally.
  EXPECT_TRUE(
      contains(report.report,
               "budget=sim_tick_avg result=PASS metric=mean unit=ms"));
}

TEST(BudgetReport, EmptyRecorderIsLoudNoSamples) {
  ASSERT_TRUE(writeHealthyBudgets(kHealthyBudgetsPath));
  World w = makeWorld();
  ASSERT_TRUE(w.registerSystem(makeDef("BRNoop", &BRNoop,
                                       laige::fpx16_16::fromInt32(100)),
                               Io<BRTag, Access::Read>{})
                  .ok());

  // No completed ticks: the tick window, the per-frame alloc
  // histogram, and the system's rolling window are all empty — three
  // loud NO_SAMPLES states (two tick budgets, one alloc budget) plus
  // the system's NO_SAMPLES line, and an empty frame section.
  Profiler prof(Profiler::Options{});
  FrameBudgetRecorder rec;  // fresh: zero records
  const laige::BudgetTable budgets = loadTableOrDie(kHealthyBudgetsPath);
  const FrameBudgetReport report = buildFrameBudgetReport(rec, prof, w,
                                                          budgets);
  std::remove(kHealthyBudgetsPath);

  EXPECT_FALSE(report.passed);
  EXPECT_TRUE(contains(report.report, "laige-budget-report frames: n=0 total=0"));
  EXPECT_EQ(countLinesWith(report.report, "laige-budget-report frame="), 0u);
  // Four loud NO_SAMPLES states: the two tick budgets, the per-frame
  // alloc histogram, and the system's rolling window (never silent
  // over an empty harness — CORE-008).
  EXPECT_EQ(countOccurrences(report.report, "result=NO_SAMPLES"), 4u);
  EXPECT_TRUE(contains(report.report, "laige-budget-report overall=FAIL"));
}

// ---------------------------------------------------------------------------
// The Engine wiring: the cached per-run report (built before the
// shutdown, readable after it), the G-R5 fold into the per-frame
// records, and the start validation
// ---------------------------------------------------------------------------

TEST(BudgetReport, EngineOverBudgetRunReport) {
  MemorySink* mem = installCaptureSink();
  ASSERT_TRUE(writeHealthyBudgets(kHealthyBudgetsPath));
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  // The test component must be registered before the system that
  // declares it (the M1-SYS-01 io_unregistered check — the
  // system_timing makeWorld pattern).
  ASSERT_TRUE(engine.world()->registerComponent<BRTag>().ok());
  ASSERT_TRUE(engine.world()
                  ->registerSystem(makeDef("EngBurn", &BRBurn, burnBudgetMs()),
                                   Io<BRTag, Access::Read>{})
                  .ok());
  // The report's per-frame section is bounded to the last 4 retained
  // frames (the startBudgetReport lastNFrames bound).
  ASSERT_TRUE(engine.startBudgetReport(kHealthyBudgetsPath, 4).ok());
  EXPECT_TRUE(engine.budgetReportRequested());

  const std::uint64_t kTicks = 10;
  brBurnMs = 2.0;
  const Status st = engine.run_headless(kTicks, 1);
  brBurnMs = 0.0;
  ASSERT_TRUE(st.ok());
  std::remove(kHealthyBudgetsPath);

  // The report is CACHED: readable after the run's ordered shutdown
  // (the world and the profiler are released — the profileStats()
  // precedent; engine.profiler() is nullptr now).
  EXPECT_EQ(engine.profiler(), nullptr);
  const FrameBudgetReport& rep = engine.lastBudgetReport();
  EXPECT_FALSE(rep.passed);
  EXPECT_TRUE(contains(rep.report, "laige-budget-report overall=FAIL"));
  // The frame section is bounded to 4 of the run's frames; the run
  // completed EXACTLY 10 ticks (the frame budget 1 contract).
  EXPECT_TRUE(contains(rep.report, "frames: n=4 total="));
  // The per-frame invariant (preemption-tolerant): every retained
  // frame that ran a tick ran the burn — one warn, one critical,
  // FAIL — and its sim_allocs stayed 0 (the steady state).
  const std::size_t frameLines =
      countLinesWith(rep.report, "laige-budget-report frame=");
  EXPECT_GE(frameLines, 4u);
  for (const std::string& line : splitLines(rep.report)) {
    if (line.rfind("laige-budget-report frame=", 0) != 0) continue;
    const std::uint64_t ticks = uintField(line, "ticks");
    if (ticks == 1) {
      // The frame ran the burn: one warn, one critical, FAIL.
      EXPECT_EQ(uintField(line, "overrun_warns"), 1u);
      EXPECT_EQ(uintField(line, "critical_errors"), 1u);
      EXPECT_TRUE(contains(line, "result=FAIL"));
    }
    // The steady state: no sim allocations in any retained frame.
    EXPECT_EQ(uintField(line, "sim_allocs"), 0u);
  }
  // The per-system section (the authoritative G-R5 counters — exact,
  // both multipliers exceeded on the nominal floor) and the
  // over-budget list.
  const std::string sys =
      lineWith(rep.report, "laige-budget-report system id=1 name=EngBurn ");
  ASSERT_FALSE(sys.empty());
  EXPECT_TRUE(contains(sys, "runs=10"));
  EXPECT_TRUE(contains(sys, "warns=10"));
  EXPECT_TRUE(contains(sys, "errors=10"));
  EXPECT_TRUE(contains(sys, "result=FAIL"));
  EXPECT_GE(fieldOf(sys, "measured_p99_ms"), 2.0);
  const std::string ob =
      lineWith(rep.report, "laige-budget-report over_budget: id=1 ");
  ASSERT_FALSE(ob.empty());
  EXPECT_TRUE(contains(ob, "name=EngBurn "));

  // The G-R5 events themselves fired, every occurrence (the capture
  // sink has rate limiting OFF): 10 warns, 10 criticals, subsystem
  // "system".
  EXPECT_EQ(countEvents(*mem, "budget_overrun"), 10u);
  EXPECT_EQ(countEvents(*mem, "budget_critical"), 10u);

  // The machine-greppable summary (the ctest output).
  std::printf("budget-report-engine ticks=%llu warns=10 errors=10 "
              "overall=FAIL frames=%zu\n",
              static_cast<unsigned long long>(kTicks), frameLines);
  restoreLogger();
}

TEST(BudgetReport, EngineHealthyRunReportPasses) {
  MemorySink* mem = installCaptureSink();
  ASSERT_TRUE(writeHealthyBudgets(kHealthyBudgetsPath));
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.world()->registerComponent<BRTag>().ok());
  // Budget 100 ms: the noop's microsecond ticks stay under it (the
  // noise-floor precedent; a stretched tick would carry its own G-R5
  // event, which the zero-event assertions below reject).
  ASSERT_TRUE(engine.world()
                  ->registerSystem(makeDef("EngNoop", &BRNoop,
                                           laige::fpx16_16::fromInt32(100)),
                                   Io<BRTag, Access::Read>{})
                  .ok());
  ASSERT_TRUE(engine.startBudgetReport(kHealthyBudgetsPath).ok());
  const Status st = engine.run_headless(10, 1);
  ASSERT_TRUE(st.ok());
  std::remove(kHealthyBudgetsPath);

  const FrameBudgetReport& rep = engine.lastBudgetReport();
  EXPECT_TRUE(rep.passed);
  EXPECT_TRUE(contains(rep.report, "laige-budget-report overall=PASS"));
  EXPECT_TRUE(contains(rep.report, "laige-budget-report over_budget: none"));
  EXPECT_TRUE(contains(
      rep.report,
      "budget=sim_heap_allocs result=PASS metric=max unit=allocs_per_frame"));
  // The healthy path is silent (LOG-003): no G-R5 event at all.
  EXPECT_EQ(countEvents(*mem, "budget_overrun"), 0u);
  EXPECT_EQ(countEvents(*mem, "budget_critical"), 0u);
  restoreLogger();
}

TEST(BudgetReport, EngineZeroTickRunIsLoud) {
  // A zero frame budget is rejected before the loop exists — the run
  // still builds and caches the budget report (every run path;
  // CORE-008: no silent omission): a zero-tick report, loud
  // NO_SAMPLES lines included (the profile-report zero-tick
  // precedent).
  MemorySink* mem = installCaptureSink();
  ASSERT_TRUE(writeHealthyBudgets(kHealthyBudgetsPath));
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.world()->registerComponent<BRTag>().ok());
  ASSERT_TRUE(engine.world()
                  ->registerSystem(makeDef("EngNoop", &BRNoop,
                                           laige::fpx16_16::fromInt32(100)),
                                   Io<BRTag, Access::Read>{})
                  .ok());
  ASSERT_TRUE(engine.startBudgetReport(kHealthyBudgetsPath).ok());
  const Status st = engine.run_headless(2, 0);
  ASSERT_TRUE(st.isError());
  EXPECT_EQ(st.error(), ErrorCode::InvalidArgument);
  std::remove(kHealthyBudgetsPath);

  const FrameBudgetReport& rep = engine.lastBudgetReport();
  EXPECT_FALSE(rep.passed);
  EXPECT_TRUE(contains(rep.report, "laige-budget-report frames: n=0 total=0"));
  EXPECT_TRUE(contains(rep.report, "laige-budget-report overall=FAIL"));
  EXPECT_TRUE(contains(rep.report, "result=NO_SAMPLES"));
  // No completed frame: no G-R5 event either (the silence is
  // asserted, not assumed).
  EXPECT_EQ(countEvents(*mem, "budget_overrun"), 0u);
  EXPECT_EQ(countEvents(*mem, "budget_critical"), 0u);
  restoreLogger();
}

TEST(BudgetReport, DoubleBudgetStartRejected) {
  MemorySink* mem = installCaptureSink();
  ASSERT_TRUE(writeHealthyBudgets(kHealthyBudgetsPath));
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.startBudgetReport(kHealthyBudgetsPath).ok());
  const Status bad = engine.startBudgetReport(kHealthyBudgetsPath);
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*mem, "report_already_started"), 1u);
  std::remove(kHealthyBudgetsPath);
  engine.shutdown();
  restoreLogger();
}

TEST(BudgetReport, EmptyBudgetsPathRejected) {
  MemorySink* mem = installCaptureSink();
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  const Status bad = engine.startBudgetReport("");
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*mem, "report_path_invalid"), 1u);
  engine.shutdown();
  restoreLogger();
}

TEST(BudgetReport, UnreadableBudgetsPathLoadFailed) {
  MemorySink* mem = installCaptureSink();
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  const Status bad =
      engine.startBudgetReport("/nonexistent-laige-dir/budgets.json");
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::IoError);
  EXPECT_EQ(countEvents(*mem, "report_load_failed"), 1u);
  EXPECT_FALSE(engine.budgetReportRequested());  // not started
  engine.shutdown();
  restoreLogger();
}

TEST(BudgetReport, MalformedBudgetsFileLoadFailed) {
  MemorySink* mem = installCaptureSink();
  ASSERT_TRUE(writeBadVersionBudgets(kBadVersionBudgetsPath));
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  const Status bad = engine.startBudgetReport(kBadVersionBudgetsPath);
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::MalformedInput);
  EXPECT_EQ(countEvents(*mem, "report_load_failed"), 1u);
  std::remove(kBadVersionBudgetsPath);
  engine.shutdown();
  restoreLogger();
}

TEST(BudgetReport, StoppedEngineRejectsBudgetStart) {
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  engine.shutdown();
  const Status bad = engine.startBudgetReport(kHealthyBudgetsPath);
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::InvalidArgument);
}
