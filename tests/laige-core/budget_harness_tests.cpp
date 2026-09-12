// laige-core budget harness suite (M0-CORE-08).
//
// Step Verify scope (roadmap/M0-foundations.md):
//   - `ctest -R budget_harness` green: synthetic workload produces
//     percentiles; budget check fails loudly when a threshold is
//     exceeded.
// Suites: BudgetHarnessHistogram (window semantics, nearest-rank
// percentiles, empty state), BudgetHarnessTimeIt (steady-clock scope
// timer), BudgetHarnessCheck (pass/fail semantics, the AGENTS 12 report
// format, before/after pair), BudgetHarnessTable (loadBudgets against
// the repo-root budgets.json + the schema v1 rejection corpus).

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "laige/budget_harness.h"
#include "laige/prng.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "budget_harness_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "budget_harness_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "budget_harness_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

namespace {

using laige::BudgetEntry;
using laige::BudgetMetric;
using laige::BudgetReportContext;
using laige::BudgetTable;
using laige::ErrorCode;
using laige::Histogram;
using laige::HistogramStats;
using laige::TimeIt;

// A histogram holding exactly 1.0 .. 100.0 (capacity 100 — no drops).
Histogram Histogram1To100() {
  Histogram h(Histogram::Options{100});
  for (int i = 1; i <= 100; ++i) h.record(double(i));
  return h;
}

BudgetEntry Entry(const char* name, BudgetMetric metric, const char* unit,
                  double target, double measured = 0.0) {
  return BudgetEntry{name, metric, unit, target, measured, "test workload"};
}

BudgetReportContext ReportCtx() {
  return BudgetReportContext{"synthetic workload", "test compiler, Debug",
                             "test machine", 42};
}

void ExpectReportContains(const std::string& report, const char* needle) {
  EXPECT_NE(std::string::npos, report.find(needle))
      << "expected the report to contain '" << needle << "'; report:\n"
      << report;
}

// Portable file open (CPP-009 platform boundary, as in logging.cpp and
// the logging suite): MSVC's plain fopen is deprecated (C4996, fatal
// under /WX), so use _fsopen with _SH_DENYNO.
#if defined(_MSC_VER)
std::FILE* openFile(const char* path, const char* mode) {
  return ::_fsopen(path, mode, _SH_DENYNO);
}
#else
std::FILE* openFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif

std::string WriteTempJson(const char* name, const std::string content) {
  const std::string path = std::string("laige_budget_test_") + name + ".json";
  std::remove(path.c_str());  // never test against a stale file (CORE-008)
  std::FILE* f = openFile(path.c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "WriteTempJson: open(\"%s\", \"wb\") failed "
                         "(errno=%d)\n",
                 path.c_str(), errno);
    return {};
  }
  std::fwrite(content.data(), 1, content.size(), f);
  std::fclose(f);
  return path;
}

// The repo-root budgets.json, wired by CTest (ENVIRONMENT
// LAIGE_BUDGETS_PATH); the fallback covers running the binary from the
// source root by hand. MSVC deprecates plain getenv (C4996, fatal under
// /WX); getenv_s has the same lookup semantics (CPP-009, pattern:
// logging.cpp).
std::string BudgetsFilePath() {
#if defined(_MSC_VER)
  // Largest environment value the suite reads (the budgets file path;
  // fits far inside the bound). Named per CORE-005; a value beyond it is
  // treated as unset (the documented fallback applies). MSVC-only for the
  // same reason as above (CORE-010: no unused symbols under -Werror).
  constexpr std::size_t kEnvValueMaxBytes = 4096;
  char buf[kEnvValueMaxBytes];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "LAIGE_BUDGETS_PATH") != 0)
    return "budgets.json";
  return (len > 0) ? std::string(buf, len) : std::string("budgets.json");
#else
  const char* env = std::getenv("LAIGE_BUDGETS_PATH");
  return (env != nullptr && env[0] != '\0') ? std::string(env)
                                            : std::string("budgets.json");
#endif
}

void ExpectMalformedFile(const std::string& path) {
  const laige::Result<BudgetTable, ErrorCode> r = laige::loadBudgets(path);
  EXPECT_TRUE(r.isError()) << "expected a load failure for " << path;
  if (r.isError()) {
    EXPECT_EQ(ErrorCode::MalformedInput, r.error())
        << laige::errorText(r.error());
  }
}

void ExpectIoErrorFile(const std::string& path) {
  const laige::Result<BudgetTable, ErrorCode> r = laige::loadBudgets(path);
  EXPECT_TRUE(r.isError()) << "expected a load failure for " << path;
  if (r.isError()) {
    EXPECT_EQ(ErrorCode::IoError, r.error()) << laige::errorText(r.error());
  }
}

// Schema-v1 document builders for the rejection corpus: Doc(entry,
// version) and E(overrides) emit one minimal valid entry by default.
std::string E(const char* name = "b1", const char* metric = "mean",
              const char* unit = "ms", const char* target = "1.0",
              const char* measured = "0", const char* workload = "wl") {
  return std::string("{\"name\":\"") + name + "\",\"metric\":\"" + metric +
         "\",\"unit\":\"" + unit + "\",\"target\":" + target +
         ",\"measured\":" + measured + ",\"workload\":\"" + workload + "\"}";
}

std::string Doc(const std::string& entry, const std::string& version = "1") {
  return std::string("{\"version\": ") + version + ", \"budgets\": [" +
         entry + "]}";
}

}  // namespace

// ---------------------------------------------------------------------------
// BudgetHarnessHistogram — window semantics + exact statistics
// ---------------------------------------------------------------------------

TEST(BudgetHarnessHistogram, KnownPercentilesAndMean) {
  const Histogram h = Histogram1To100();
  const HistogramStats s = h.stats();
  EXPECT_EQ(std::uint64_t(100), s.n);
  EXPECT_DOUBLE_EQ(1.0, s.min);
  EXPECT_DOUBLE_EQ(100.0, s.max);
  EXPECT_DOUBLE_EQ(50.5, s.mean);
  // nearest-rank: p50 -> v[49] = 50, p95 -> v[94] = 95, p99 -> v[98] = 99
  EXPECT_DOUBLE_EQ(50.0, s.p50);
  EXPECT_DOUBLE_EQ(95.0, s.p95);
  EXPECT_DOUBLE_EQ(99.0, s.p99);
}

TEST(BudgetHarnessHistogram, SingleSampleWindow) {
  Histogram h(Histogram::Options{4});
  h.record(7.25);
  const HistogramStats s = h.stats();
  EXPECT_EQ(std::uint64_t(1), s.n);
  EXPECT_DOUBLE_EQ(7.25, s.min);
  EXPECT_DOUBLE_EQ(7.25, s.max);
  EXPECT_DOUBLE_EQ(7.25, s.mean);
  EXPECT_DOUBLE_EQ(7.25, s.p50);
  EXPECT_DOUBLE_EQ(7.25, s.p95);
  EXPECT_DOUBLE_EQ(7.25, s.p99);
}

TEST(BudgetHarnessHistogram, FullRingWrapsOldest) {
  Histogram h(Histogram::Options{3});
  for (int i = 1; i <= 4; ++i) h.record(double(i));  // window {2,3,4}
  EXPECT_EQ(std::uint64_t(3), h.count());
  EXPECT_EQ(std::uint64_t(4), h.totalRecorded());  // truncation is observable
  const HistogramStats s = h.stats();
  EXPECT_EQ(std::uint64_t(3), s.n);
  EXPECT_DOUBLE_EQ(2.0, s.min);
  EXPECT_DOUBLE_EQ(4.0, s.max);
  EXPECT_DOUBLE_EQ(3.0, s.mean);
  // nearest-rank on {2,3,4}: p50 -> v[1] = 3; p95 -> v[3]... n=3:
  // ceil(0.95*3)=3 -> v[2] = 4; p99 -> 4
  EXPECT_DOUBLE_EQ(3.0, s.p50);
  EXPECT_DOUBLE_EQ(4.0, s.p95);
  EXPECT_DOUBLE_EQ(4.0, s.p99);
}

TEST(BudgetHarnessHistogram, CapacityZeroDropsEverything) {
  Histogram h(Histogram::Options{0});
  for (int i = 0; i < 5; ++i) h.record(double(i));
  EXPECT_EQ(std::uint64_t(0), h.count());
  EXPECT_EQ(std::uint64_t(5), h.totalRecorded());
  const HistogramStats s = h.stats();
  EXPECT_EQ(std::uint64_t(0), s.n);
}

TEST(BudgetHarnessHistogram, EmptyStatsAreNan) {
  const Histogram h(Histogram::Options{16});
  const HistogramStats s = h.stats();
  EXPECT_EQ(std::uint64_t(0), s.n);
  EXPECT_TRUE(std::isnan(s.min));
  EXPECT_TRUE(std::isnan(s.mean));
  EXPECT_TRUE(std::isnan(s.p50));
  EXPECT_TRUE(std::isnan(s.p95));
  EXPECT_TRUE(std::isnan(s.p99));
  EXPECT_TRUE(std::isnan(s.max));
}

TEST(BudgetHarnessHistogram, StatsInvariantsOverDeterminedSamples) {
  // 20k samples from the deterministic PRNG (M0-CORE-06): the ordering
  // invariants must hold for any window, and no sample may be dropped
  // (capacity == runs).
  const std::uint64_t runs = 20000;
  Histogram h(Histogram::Options{runs});
  laige::Prng rng(0x1234567890ABCDEF);
  for (std::uint64_t i = 0; i < runs; ++i) h.record(rng.next_float01());
  EXPECT_EQ(runs, h.totalRecorded());
  const HistogramStats s = h.stats();
  EXPECT_EQ(runs, s.n);
  EXPECT_LE(s.min, s.p50);
  EXPECT_LE(s.p50, s.p95);
  EXPECT_LE(s.p95, s.p99);
  EXPECT_LE(s.p99, s.max);
  EXPECT_LE(s.min, s.mean);
  EXPECT_LE(s.mean, s.max);
}

TEST(BudgetHarnessHistogram, ResetDropsWindowKeepsChurn) {
  Histogram h(Histogram::Options{8});
  for (int i = 1; i <= 10; ++i) h.record(double(i));
  EXPECT_EQ(std::uint64_t(8), h.count());
  EXPECT_EQ(std::uint64_t(10), h.totalRecorded());
  h.reset();
  EXPECT_EQ(std::uint64_t(0), h.count());
  EXPECT_EQ(std::uint64_t(10), h.totalRecorded());  // churn survives
  EXPECT_EQ(std::uint64_t(0), h.stats().n);
  h.record(7.0);
  EXPECT_EQ(std::uint64_t(1), h.count());
  EXPECT_DOUBLE_EQ(7.0, h.stats().mean);
}

TEST(BudgetHarnessHistogram, FormatStatsLineIsStable) {
  const HistogramStats s = Histogram1To100().stats();
  const std::string line = laige::formatStatsLine(s);
  EXPECT_EQ("stats: n=100 min=1 mean=50.5 p50=50 p95=95 p99=99 max=100",
            line);
}

// ---------------------------------------------------------------------------
// BudgetHarnessTimeIt — steady-clock scope timer
// ---------------------------------------------------------------------------

// Bounded busy work (~microseconds). The volatile counter (updated with
// an ordinary assignment — `++` on a volatile is deprecated, C++23)
// keeps the loop from constant-folding away at high optimization levels,
// so the timer tests measure real elapsed time in every build type.
std::int64_t Spin(std::int64_t iterations) {
  std::int64_t acc = 0;
  volatile std::int64_t i = 0;
  while (i < iterations) {
    acc += i * 3 - 1;
    i = i + 1;
  }
  return acc;
}

TEST(BudgetHarnessTimeIt, ElapsedIsNonNegativeAndMonotonic) {
  TimeIt t;
  const double e0 = t.elapsedMs();
  EXPECT_GE(e0, 0.0);
  const volatile std::int64_t acc = Spin(2000000);
  (void)acc;
  const double e1 = t.elapsedMs();
  EXPECT_GE(e1, e0);
}

TEST(BudgetHarnessTimeIt, ElapsedIsPositiveAfterWork) {
  TimeIt t;
  const volatile std::int64_t acc = Spin(2000000);
  (void)acc;
  EXPECT_GT(t.elapsedMs(), 0.0);
}

TEST(BudgetHarnessTimeIt, ResetRestartsScope) {
  TimeIt t;
  const volatile std::int64_t acc = Spin(2000000);
  (void)acc;
  const double before = t.elapsedMs();
  t.reset();
  const double after = t.elapsedMs();
  EXPECT_GE(after, 0.0);
  EXPECT_LT(after, before);
}

// ---------------------------------------------------------------------------
// BudgetHarnessCheck — pass/fail semantics + AGENTS 12 report
// ---------------------------------------------------------------------------

TEST(BudgetHarnessCheck, PassesWhenMeasuredUnderTarget) {
  Histogram h(Histogram::Options{8});
  for (int i = 1; i <= 5; ++i) h.record(double(i));  // mean = 3
  const laige::BudgetCheckResult r =
      laige::budgetCheck(Entry("mean_metric", BudgetMetric::Mean, "ms",
                               4.0, /*measured=*/2.5),
                         h, ReportCtx());
  EXPECT_TRUE(r.passed);
  EXPECT_DOUBLE_EQ(3.0, r.measured);
  EXPECT_DOUBLE_EQ(4.0, r.target);
  EXPECT_DOUBLE_EQ(2.5, r.before);
  ExpectReportContains(r.report,
                       "budget=mean_metric result=PASS metric=mean unit=ms");
  ExpectReportContains(r.report, "after=3");
  ExpectReportContains(r.report, "before=2.5");
  ExpectReportContains(r.report, "target=4");
  ExpectReportContains(r.report, "n=5");
}

TEST(BudgetHarnessCheck, FailsLoudlyWhenTargetExceeded) {
  // The step's Verify clause: the budget check fails loudly when a
  // threshold is exceeded — a distinct structured failure, never silent.
  Histogram h(Histogram::Options{4});
  for (int i = 0; i < 3; ++i) h.record(5.0);  // mean = 5 > target 4
  const laige::BudgetCheckResult r =
      laige::budgetCheck(Entry("over", BudgetMetric::Mean, "ms", 4.0), h,
                         ReportCtx());
  EXPECT_FALSE(r.passed);
  EXPECT_DOUBLE_EQ(5.0, r.measured);
  ExpectReportContains(r.report, "result=FAIL");
  ExpectReportContains(r.report, "after=5");
  ExpectReportContains(r.report, "target=4");
  ExpectReportContains(r.report, "n=3");
}

TEST(BudgetHarnessCheck, FailsLoudlyOnEmptyHistogram) {
  // A workload that recorded nothing is a broken harness: loud NO_SAMPLES,
  // never "passed" on NaN statistics (CORE-008).
  Histogram h(Histogram::Options{4});
  const laige::BudgetCheckResult r =
      laige::budgetCheck(Entry("empty", BudgetMetric::Mean, "ms", 4.0), h,
                         ReportCtx());
  EXPECT_FALSE(r.passed);
  EXPECT_TRUE(std::isnan(r.measured));
  ExpectReportContains(r.report, "result=NO_SAMPLES");
  ExpectReportContains(r.report, "n=0");
}

TEST(BudgetHarnessCheck, ZeroTargetBudgetRequiresExactlyZero) {
  // target == 0 is a hard-zero budget (sim heap allocations), not
  // "not set": measured 0 passes, any non-zero fails.
  Histogram zeros(Histogram::Options{3});
  zeros.record(0.0);
  zeros.record(0.0);
  zeros.record(0.0);
  const laige::BudgetCheckResult ok = laige::budgetCheck(
      Entry("allocs", BudgetMetric::Max, "allocs_per_frame", 0.0), zeros,
      ReportCtx());
  EXPECT_TRUE(ok.passed);
  EXPECT_DOUBLE_EQ(0.0, ok.measured);
  ExpectReportContains(ok.report, "result=PASS");

  Histogram nonzero(Histogram::Options{3});
  nonzero.record(0.0);
  nonzero.record(1.0);
  nonzero.record(0.0);
  const laige::BudgetCheckResult bad = laige::budgetCheck(
      Entry("allocs", BudgetMetric::Max, "allocs_per_frame", 0.0), nonzero,
      ReportCtx());
  EXPECT_FALSE(bad.passed);
  EXPECT_DOUBLE_EQ(1.0, bad.measured);
  ExpectReportContains(bad.report, "result=FAIL");
}

TEST(BudgetHarnessCheck, ChecksTheNamedMetric) {
  const Histogram h = Histogram1To100();
  const BudgetReportContext ctx;
  EXPECT_DOUBLE_EQ(50.0, laige::budgetCheck(
                             Entry("m", BudgetMetric::P50, "ms", 100.0), h,
                             ctx)
                             .measured);
  EXPECT_DOUBLE_EQ(95.0, laige::budgetCheck(
                             Entry("m", BudgetMetric::P95, "ms", 100.0), h,
                             ctx)
                             .measured);
  EXPECT_DOUBLE_EQ(99.0, laige::budgetCheck(
                             Entry("m", BudgetMetric::P99, "ms", 100.0), h,
                             ctx)
                             .measured);
  EXPECT_DOUBLE_EQ(1.0, laige::budgetCheck(
                            Entry("m", BudgetMetric::Min, "ms", 100.0), h, ctx)
                            .measured);
  EXPECT_DOUBLE_EQ(100.0, laige::budgetCheck(
                              Entry("m", BudgetMetric::Max, "ms", 100.0), h,
                              ctx)
                              .measured);
}

TEST(BudgetHarnessCheck, ReportCarriesCallerContext) {
  Histogram h(Histogram::Options{2});
  h.record(1.0);
  h.record(2.0);
  const BudgetReportContext ctx{"iso scene", "clang++ 22.1.8 Debug",
                                "ci runner", 17};
  const laige::BudgetCheckResult r =
      laige::budgetCheck(Entry("ctx", BudgetMetric::Mean, "ms", 10.0), h, ctx);
  ExpectReportContains(r.report, "workload=iso scene");
  ExpectReportContains(r.report, "build=clang++ 22.1.8 Debug");
  ExpectReportContains(r.report, "machine=ci runner");
  ExpectReportContains(r.report, "warmup=17");
}

TEST(BudgetHarnessCheck, ReportFirstLineIsStable) {
  // The first line is the machine-greppable contract (LOG-001):
  // budget=<name> result=<R> metric=<m> unit=<u>
  Histogram h(Histogram::Options{2});
  h.record(1.0);
  h.record(2.0);  // p95 of {1,2} = 2 > target 1.5 -> FAIL
  const laige::BudgetCheckResult r =
      laige::budgetCheck(Entry("stable", BudgetMetric::P95, "ms", 1.5), h,
                         ReportCtx());
  const std::string first =
      r.report.substr(0, r.report.find('\n'));
  EXPECT_EQ("budget=stable result=FAIL metric=p95 unit=ms", first);
}

// ---------------------------------------------------------------------------
// BudgetHarnessTable — loadBudgets: repo file + schema v1 corpus
// ---------------------------------------------------------------------------

TEST(BudgetHarnessTable, LoadsTheRepoBudgetsFile) {
  const laige::Result<BudgetTable, ErrorCode> table =
      laige::loadBudgets(BudgetsFilePath());
  ASSERT_TRUE(table.ok()) << laige::errorText(table.error());
  const BudgetTable& t = table.value();

  // One entry per PRD 8.1 target (15 rows: sim tick, 50k sprites, cold
  // start, build time, and zone server each carry two budgets).
  EXPECT_EQ(std::size_t(15), t.size());

  const BudgetEntry* frame = t.find("frame_time_render");
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(BudgetMetric::P95, frame->metric);
  EXPECT_EQ("ms", frame->unit);
  EXPECT_DOUBLE_EQ(8.3, frame->target);
  EXPECT_DOUBLE_EQ(0.0, frame->measured);  // not yet measured (M0)
  EXPECT_FALSE(frame->workload.empty());

  const BudgetEntry* allocs = t.find("sim_heap_allocs");
  ASSERT_NE(allocs, nullptr);
  EXPECT_DOUBLE_EQ(0.0, allocs->target);  // hard-zero budget, not "unset"
  EXPECT_EQ(BudgetMetric::Max, allocs->metric);
  EXPECT_EQ("allocs_per_frame", allocs->unit);

  EXPECT_EQ(nullptr, t.find("no_such_budget"));

  // Re-verify every entry is well-formed (a hand-edited budgets.json must
  // fail here, not just in the loader).
  for (const BudgetEntry& e : t.entries()) {
    EXPECT_FALSE(e.name.empty());
    EXPECT_FALSE(e.unit.empty());
    EXPECT_FALSE(e.workload.empty());
    EXPECT_TRUE(std::isfinite(e.target) && e.target >= 0.0) << e.name;
    EXPECT_TRUE(std::isfinite(e.measured) && e.measured >= 0.0) << e.name;
  }
}

TEST(BudgetHarnessTable, RejectsSchemaViolations) {
  const std::string badJson = WriteTempJson("bad_json", "{");
  ExpectMalformedFile(badJson);
  std::remove(badJson.c_str());

  const std::string badVersion = WriteTempJson("bad_version", Doc(E(), "2"));
  ExpectMalformedFile(badVersion);
  std::remove(badVersion.c_str());

  const std::string notObject = WriteTempJson("not_object", "[1, 2]");
  ExpectMalformedFile(notObject);
  std::remove(notObject.c_str());

  const std::string missingField =
      WriteTempJson("missing_field",
                    Doc("{\"name\":\"b1\",\"metric\":\"mean\",\"unit\":\"ms\","
                        "\"target\":1.0,\"measured\":0}"));
  ExpectMalformedFile(missingField);
  std::remove(missingField.c_str());

  const std::string unknownField =
      WriteTempJson("unknown_field",
                    Doc(E() + ",\"extra\":1}"));
  ExpectMalformedFile(unknownField);
  std::remove(unknownField.c_str());

  const std::string badMetric = WriteTempJson("bad_metric", Doc(E("b1", "p5")));
  ExpectMalformedFile(badMetric);
  std::remove(badMetric.c_str());

  const std::string dupName =
      WriteTempJson("dup_name", Doc(E() + ", " + E()));
  ExpectMalformedFile(dupName);
  std::remove(dupName.c_str());

  const std::string negTarget =
      WriteTempJson("neg_target", Doc(E("b1", "mean", "ms", "-1.0")));
  ExpectMalformedFile(negTarget);
  std::remove(negTarget.c_str());

  // 1e999 is well-formed JSON that the parser stores as +inf (ADR 0003):
  // the schema rejects non-finite numbers explicitly.
  const std::string infTarget =
      WriteTempJson("inf_target", Doc(E("b1", "mean", "ms", "1e999")));
  ExpectMalformedFile(infTarget);
  std::remove(infTarget.c_str());

  const std::string emptyName =
      WriteTempJson("empty_name", Doc(E("", "mean", "ms", "1.0")));
  ExpectMalformedFile(emptyName);
  std::remove(emptyName.c_str());

  const std::string badNameChars =
      WriteTempJson("bad_name_chars", Doc(E("b 1")));
  ExpectMalformedFile(badNameChars);
  std::remove(badNameChars.c_str());

  const std::string badUnit =
      WriteTempJson("bad_unit", Doc(E("b1", "mean", "ms x", "1.0")));
  ExpectMalformedFile(badUnit);
  std::remove(badUnit.c_str());

  const std::string emptyWorkload =
      WriteTempJson("empty_workload", Doc(E("b1", "mean", "ms", "1.0", "0",
                                            "")));
  ExpectMalformedFile(emptyWorkload);
  std::remove(emptyWorkload.c_str());
}

TEST(BudgetHarnessTable, RejectsMissingFile) {
  ExpectIoErrorFile("laige_budget_test_does_not_exist.json");
}

TEST(BudgetHarnessTable, EmptyBudgetsArrayIsValid) {
  const std::string path = WriteTempJson("empty", Doc(""));
  const laige::Result<BudgetTable, ErrorCode> r = laige::loadBudgets(path);
  ASSERT_TRUE(r.ok()) << laige::errorText(r.error());
  EXPECT_EQ(std::size_t(0), r.value().size());
  std::remove(path.c_str());
}
