// laige-render deterministic depth sort tests (M2-SORT-01): the
// stable, deterministic, pre-allocated 32-bit-key radix sort in
// laige/render/depth_sort.h.
//
// Pure integer math — no GL context, no GL environment needed: every
// suite runs in every local tree and in CI. The golden suite pins the
// hand-computed sorted order + the capacity edge behavior; the edges
// suite pins the 0/1/all-equal/monotone shapes; the stability suite
// pins the determinism contract on shuffled inputs (the roadmap
// property: same input multiset -> the documented stable order,
// oracle-checked); the property suite pins the 10k-key oracle
// comparison + the zero-allocation proof; the budget suite gates the
// PRD §8.1 `depth_sort_10k` entry (10 000 keys sorted, mean <= 1.0 ms)
// on the non-instrumented trees (methodology §4: instrumentation
// inflates absolute cost — the sanitizer trees run the same workload
// leak-free under ASan/TSan instead).
//
// Seed: the repo-wide documented default seed via
// tests/support/laige_test_seed.h (docs/testing.md §4), one named
// substream per randomized suite.

#include "laige/render/depth_sort.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/prng.h"
#include "laige_test_seed.h"

namespace {

using laige::render::DepthSort;

// One named substream id per randomized suite (docs/testing.md §4).
constexpr std::uint32_t kStabilitySubstreamId = 0x44535331;  // "DSS1"
constexpr std::uint32_t kPropertySubstreamId = 0x44535332;   // "DSS2"

// A deterministic 32-bit mix of i (the key generator — the
// precomputed workloads are built OUTSIDE every measured window: no
// RNG and no division in the measured path, the iso_depth_table /
// iso_picking test discipline). The splitmix64 finalizer (Stafford
// 2018 — the Prng's own mixers) spreads the low bits of i, and the
// optional mask carves a realistic isometric key range (the M2-ISO-01
// fine-depth field is 22 bits; a 10k scene carries ~2.4 equal-key
// pairs per key on average).
std::uint32_t mixKey(std::uint64_t i, std::uint32_t mask = 0xFFFFFFFFu) {
  std::uint64_t v =
      i * laige::Prng::kMixMultiplierA + laige::Prng::kSplitmix64Increment;
  v ^= v >> 30;
  v *= laige::Prng::kMixMultiplierA;
  v ^= v >> 27;
  v *= 0x94D049BB133111EBull;
  v ^= v >> 31;
  return static_cast<std::uint32_t>(v) & mask;
}

// The reference stable order: (key, input index) pairs sorted by key
// with the index as the stable tie-break (std::stable_sort on pairs —
// a test-only oracle; the engine's sort is checked against it).
std::vector<std::pair<std::uint32_t, std::uint32_t>> oracleStableOrder(
    const std::vector<std::uint32_t>& keys) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> v;
  v.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    v.emplace_back(keys[i], static_cast<std::uint32_t>(i));
  }
  std::stable_sort(v.begin(), v.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  return v;
}

// The engine's sorted order must equal the oracle's (key, index)
// sequence exactly — sortedness + stability + determinism in one
// comparison.
void expectSortedMatches(const DepthSort& sorter,
                         const std::vector<std::uint32_t>& keys) {
  const std::vector<std::pair<std::uint32_t, std::uint32_t>> oracle =
      oracleStableOrder(keys);
  EXPECT_EQ(sorter.sortedCount(), keys.size());
  const auto sk = sorter.sortedKeys();
  const auto si = sorter.sortedIndices();
  for (std::size_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(sk[i], oracle[i].first) << "i=" << i;
    EXPECT_EQ(si[i], oracle[i].second) << "i=" << i;
  }
}

// A successful create, or a hard test failure (the iso_camera makeIso
// pattern).
DepthSort makeSorter(std::size_t capacity, const char* what) {
  auto r = DepthSort::create(capacity);
  if (r.isError()) {
    ADD_FAILURE() << "DepthSort::create(" << capacity
                  << ") rejected: " << what;
    return DepthSort{};  // the default state: empty, capacity 0
  }
  return std::move(r).takeValue();
}

}  // namespace

// ---------------------------------------------------------------------------
// DepthSortGolden — the hand-computed sorted order (the roadmap's
// "stability on equal keys") + the capacity edge behavior.
// ---------------------------------------------------------------------------

TEST(DepthSortGolden, HandComputedOrderAndCapacityEdges) {
  // keys[0..5] = {5, 3, 5, 1, 3, 9}: the sorted keys are
  // {1, 3, 3, 5, 5, 9}; the equal keys keep their input order (3:
  // index 1 before 4; 5: index 0 before 2):
  const std::vector<std::uint32_t> keys{5, 3, 5, 1, 3, 9};
  const std::vector<std::uint32_t> expectKeys{1, 3, 3, 5, 5, 9};
  const std::vector<std::uint32_t> expectIdx{3, 1, 4, 0, 2, 5};
  DepthSort sorter = makeSorter(16, "the golden capacity 16");
  ASSERT_TRUE(
      sorter.sort(std::span<const std::uint32_t>(keys)).ok());
  const auto sk = sorter.sortedKeys();
  const auto si = sorter.sortedIndices();
  for (std::size_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(sk[i], expectKeys[i]) << "i=" << i;
    EXPECT_EQ(si[i], expectIdx[i]) << "i=" << i;
  }

  // The capacity edge (the roadmap's 0/1/all-equal edge cases live in
  // DepthSortEdges; here the failure channel): a sort beyond capacity
  // is BudgetExhausted and leaves the sorter UNCHANGED (the previous
  // frame's order is intact — sortedCount still 6, the same order):
  DepthSort small = makeSorter(2, "the small capacity 2");
  const std::uint32_t overflow[3] = {7, 1, 7};
  const laige::Status failed = small.sort(
      std::span<const std::uint32_t>(overflow, 3));
  ASSERT_TRUE(failed.isError());
  EXPECT_EQ(failed.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(small.sortedCount(), 0u);
  EXPECT_TRUE(small.sortedKeys().empty());
  // At capacity exactly: fine (the bound is n <= capacity):
  const std::uint32_t fit[2] = {7, 1};
  ASSERT_TRUE(small.sort(std::span<const std::uint32_t>(fit, 2)).ok());
  EXPECT_EQ(small.sortedKeys()[0], 1u);
  EXPECT_EQ(small.sortedKeys()[1], 7u);
  EXPECT_EQ(small.sortedIndices()[0], 1u);
  EXPECT_EQ(small.sortedIndices()[1], 0u);

  // The create validation (InvalidArgument, no allocation): 0 and
  // beyond the index-width domain:
  EXPECT_TRUE(DepthSort::create(0).isError());
  EXPECT_EQ(DepthSort::create(0).error(), laige::ErrorCode::InvalidArgument);
  EXPECT_TRUE(DepthSort::create(laige::render::kDepthSortMaxCapacity + 1)
                  .isError());
  EXPECT_EQ(
      DepthSort::create(laige::render::kDepthSortMaxCapacity + 1).error(),
      laige::ErrorCode::InvalidArgument);
  // The domain top is accepted (create only allocates 16 B/slot — the
  // capacity 0xFFFFFFFF would be a 16 GiB setup allocation, so the
  // test checks validation only via the one-slot boundary below):
  DepthSort one = makeSorter(1, "the single-slot capacity");
  EXPECT_EQ(one.capacity(), 1u);
}

// ---------------------------------------------------------------------------
// DepthSortEdges — the roadmap's 0/1/all-equal key edge cases.
// ---------------------------------------------------------------------------

TEST(DepthSortEdges, ZeroOneAllEqualAndMonotone) {
  // n = 0: legal (an empty frame's sort), empty spans, count 0:
  DepthSort sorter = makeSorter(128, "the edge capacity 128");
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>()).ok());
  EXPECT_EQ(sorter.sortedCount(), 0u);
  EXPECT_TRUE(sorter.sortedKeys().empty());
  EXPECT_TRUE(sorter.sortedIndices().empty());

  // The default-constructed EMPTY sorter (the stopped state): an
  // empty sort is fine, any non-empty sort is BudgetExhausted:
  DepthSort stopped;
  EXPECT_EQ(stopped.capacity(), 0u);
  ASSERT_TRUE(stopped.sort(std::span<const std::uint32_t>()).ok());
  const std::uint32_t oneKey[1] = {1u};
  const laige::Status stoppedFail = stopped.sort(
      std::span<const std::uint32_t>(oneKey, 1));
  ASSERT_TRUE(stoppedFail.isError());
  EXPECT_EQ(stoppedFail.error(), laige::ErrorCode::BudgetExhausted);

  // n = 1: the identity permutation:
  const std::uint32_t one[1] = {0xDEADBEEFu};
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(one, 1)).ok());
  EXPECT_EQ(sorter.sortedKeys()[0], 0xDEADBEEFu);
  EXPECT_EQ(sorter.sortedIndices()[0], 0u);

  // All equal (the maximum stability stress — every record in one
  // bucket at every pass): the input order is preserved exactly:
  std::vector<std::uint32_t> equal(100, 0x1234u);
  ASSERT_TRUE(
      sorter.sort(std::span<const std::uint32_t>(equal)).ok());
  for (std::size_t i = 0; i < equal.size(); ++i) {
    EXPECT_EQ(sorter.sortedKeys()[i], 0x1234u) << "i=" << i;
    EXPECT_EQ(sorter.sortedIndices()[i], i) << "i=" << i;
  }

  // Strictly ascending: the identity permutation (no movement):
  std::vector<std::uint32_t> asc(64);
  for (std::size_t i = 0; i < asc.size(); ++i) asc[i] = static_cast<std::uint32_t>(i);
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(asc)).ok());
  for (std::size_t i = 0; i < asc.size(); ++i) {
    EXPECT_EQ(sorter.sortedKeys()[i], asc[i]) << "i=" << i;
    EXPECT_EQ(sorter.sortedIndices()[i], i) << "i=" << i;
  }

  // Strictly descending: the exact reversal:
  std::vector<std::uint32_t> desc(64);
  for (std::size_t i = 0; i < desc.size(); ++i) {
    desc[i] = static_cast<std::uint32_t>(63 - i);
  }
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(desc)).ok());
  for (std::size_t i = 0; i < desc.size(); ++i) {
    EXPECT_EQ(sorter.sortedKeys()[i], i) << "i=" << i;
    EXPECT_EQ(sorter.sortedIndices()[i], 63u - i) << "i=" << i;
  }

  // Two alternating keys (bucket-2 stress across all four digit
  // passes — the key's low digit differs, the high digits are equal):
  // oracle-checked:
  std::vector<std::uint32_t> alt(128);
  for (std::size_t i = 0; i < alt.size(); ++i) {
    alt[i] = (i & 1u) ? 0x00000002u : 0x00000001u;
  }
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(alt)).ok());
  expectSortedMatches(sorter, alt);
}

// ---------------------------------------------------------------------------
// DepthSortStability — the roadmap's determinism property on shuffled
// inputs: the sorted KEY sequence is a function of the multiset alone
// (shuffling the input never changes it), while the full (key, index)
// order is the STABLE order of the particular input sequence
// (oracle-checked on every shuffle). Same input twice -> bit-identical
// output.
// ---------------------------------------------------------------------------

TEST(DepthSortStability, ShuffledInputsAreDeterministicAndStable) {
  constexpr std::size_t kN = 4096;
  // A 512-value multiset (~8 copies of each key — heavy equal-key
  // load: the stability stress):
  std::vector<std::uint32_t> base(kN);
  for (std::size_t i = 0; i < kN; ++i) base[i] = mixKey(i, 511u);
  // The multiset's sorted key sequence (the key half of the oracle):
  std::vector<std::uint32_t> sortedMultiset;
  sortedMultiset.reserve(kN);
  for (const auto& p : oracleStableOrder(base)) {
    sortedMultiset.push_back(p.first);
  }

  DepthSort sorter = makeSorter(kN, "the stability capacity 4096");

  // Same input twice -> bit-identical (key, index) sequence:
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(base)).ok());
  const std::vector<std::uint32_t> keysOnce(sorter.sortedKeys().begin(),
                                            sorter.sortedKeys().end());
  const std::vector<std::uint32_t> idxOnce(sorter.sortedIndices().begin(),
                                           sorter.sortedIndices().end());
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(base)).ok());
  const std::vector<std::uint32_t> keysTwice(sorter.sortedKeys().begin(),
                                             sorter.sortedKeys().end());
  const std::vector<std::uint32_t> idxTwice(sorter.sortedIndices().begin(),
                                             sorter.sortedIndices().end());
  EXPECT_EQ(keysOnce, keysTwice);
  EXPECT_EQ(idxOnce, idxTwice);

  // 64 shuffled permutations (Fisher-Yates over the fixed multiset,
  // TestPrng substream): every shuffle's sorted-key sequence equals
  // the multiset's (determinism over the multiset), and the full
  // (key, index) order equals the stable oracle of THAT input
  // sequence (stability + determinism over the sequence):
  std::vector<std::uint32_t> buf = base;
  laige::Prng prng = laige::testing::TestPrng(kStabilitySubstreamId);
  for (int p = 0; p < 64; ++p) {
    for (std::size_t i = kN; i > 1; --i) {
      const std::size_t j = prng.next_range(0, static_cast<std::uint32_t>(i));
      std::swap(buf[i - 1], buf[j]);
    }
    ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(buf)).ok());
    const std::vector<std::uint32_t> sortedKeys(sorter.sortedKeys().begin(),
                                                sorter.sortedKeys().end());
    EXPECT_EQ(sortedKeys, sortedMultiset) << "shuffle " << p;
    expectSortedMatches(sorter, buf);
  }
}

// ---------------------------------------------------------------------------
// DepthSortProperty — the 10k-key oracle comparison (the AC-4.3
// workload shape) + the zero-allocation proof (1000 consecutive sorts
// = 0 heap blocks under the allocation watch, the
// iso_picking / iso_depth_table precedent — non-sanitizer trees).
// ---------------------------------------------------------------------------

TEST(DepthSortProperty, TenKKeysAgainstOracleAndZeroAlloc) {
  constexpr std::size_t kN = 10000;
  std::vector<std::uint32_t> keys(kN);
  laige::Prng prng = laige::testing::TestPrng(kPropertySubstreamId);
  for (std::size_t i = 0; i < kN; ++i) {
    keys[i] = static_cast<std::uint32_t>(prng.next_u64());
  }
  DepthSort sorter = makeSorter(kN, "the property capacity 10000");
  ASSERT_TRUE(sorter.sort(std::span<const std::uint32_t>(keys)).ok());
  expectSortedMatches(sorter, keys);
  // The sorted keys are non-decreasing (the back-to-front order):
  const auto sk = sorter.sortedKeys();
  for (std::size_t i = 1; i < kN; ++i) {
    EXPECT_LE(sk[i - 1], sk[i]) << "i=" << i;
  }

  // Zero-allocation proof (where the allocation watch is live — the
  // non-sanitizer trees; the sanitizer runtimes own operator new, the
  // iso_depth_table_tests.cpp precedent): 1 000 consecutive 10k sorts
  // allocate nothing — the sort is fixed-size buffer traffic,
  // structurally zero-heap (PERF-003).
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    for (int i = 0; i < 1000; ++i) {
      const laige::Status s = sorter.sort(
          std::span<const std::uint32_t>(keys));
      if (!s.ok()) {
        ADD_FAILURE() << "sort " << i << " failed: "
                      << laige::errorText(s.error());
        break;
      }
    }
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "1000 sorts of 10k keys allocated " << reading.allocs
        << " heap blocks (first site: "
        << reinterpret_cast<std::uintptr_t>(reading.firstSite) << ")";
  }
}

// ---------------------------------------------------------------------------
// DepthSortBudget — the PRD §8.1 `depth_sort_10k` gate
// ---------------------------------------------------------------------------
//
// The budget workload (deterministic — no RNG in the measured path,
// the m1-sim-tick pattern): one measured sample is ONE stable sort of
// 10 000 PRECOMPUTED 32-bit keys (the budgets.json unit: "10 000
// 32-bit depth keys, one stable radix sort"). The keys carry a
// realistic isometric distribution (the M2-ISO-01 22-bit fine-depth
// range, ~2.4 equal keys per value among 10k sprites — the
// equal-key/stability load of an overlapping scene). 3 000 measured
// sorts over the 3 000-frame stress (the roadmap's "10k sprites
// sorted per frame for 3 000 frames", part of AC-4.3), warm-up 100.
// Metric: mean; target: 1.0 ms (6% of the 16.7 ms 60 FPS frame budget,
// half of the 2 ms 50k render-CPU budget).
//
// The absolute 1.0 ms target applies only on the reference platform
// (the non-instrumented Linux trees — methodology §4/§5): elsewhere
// the suite runs the SAME workload ungated and verifies its safety
// properties instead (leak-free under ASan/TSan). The gated branch
// (load budgets.json, budgetCheck, the stable 4-line report,
// EXPECT(passed)) compiles only on Linux non-instrumented trees
// (LAIGE_DEPTH_SORT_BUDGET — the iso_depth_table / iso_picking
// precedent).

constexpr std::int32_t kBudgetWarmup = 100;
constexpr std::int32_t kBudgetRuns = 3000;
constexpr std::size_t kBudgetKeys = 10000;

// The machine line for the budget report (the iso_picking_tests.cpp
// pattern; the LAIGE_BENCH_MACHINE env var, when set, carries the
// operator's machine description).
std::string MachineLine() {
#if defined(_MSC_VER)
  constexpr std::size_t kMax = 4096;
  char buf[kMax];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "LAIGE_BENCH_MACHINE") != 0) {
    return std::string();
  }
  return std::string(buf, len);
#else
  const char* env = std::getenv("LAIGE_BENCH_MACHINE");
  return (env != nullptr) ? std::string(env) : std::string();
#endif
}

// The compile-time build identity for the AGENTS §12 "build" context
// field (the iso_picking_tests.cpp kCompilerId pattern): compiler +
// version, then the CMake build type stamped by
// tests/laige-render/CMakeLists.txt (LAIGE_DEPTH_SORT_BUILD_TYPE).
#define LAIGE_DEPTH_SORT_STR2(x) #x
#define LAIGE_DEPTH_SORT_STR(x) LAIGE_DEPTH_SORT_STR2(x)
#if defined(__clang__)
constexpr char kCompilerId[] = "Clang " LAIGE_DEPTH_SORT_STR(__clang_major__)
    "." LAIGE_DEPTH_SORT_STR(__clang_minor__) "."
    LAIGE_DEPTH_SORT_STR(__clang_patchlevel__);
#elif defined(__GNUC__)
constexpr char kCompilerId[] = "GCC " __VERSION__;
#elif defined(_MSC_VER)
// _MSC_FULL_VER is an INTEGER literal (e.g. 194434433), not a string —
// it must be stringified (the laige-bench kCompilerId pattern):
constexpr char kCompilerId[] = "MSVC " LAIGE_DEPTH_SORT_STR(_MSC_FULL_VER);
#else
constexpr char kCompilerId[] = "unknown compiler";
#endif

laige::BudgetCheckResult runBudget(const laige::BudgetEntry* entry) {
  // The precomputed keys (deterministic; every draw lives OUTSIDE the
  // measured windows — the mixKey finalizer, no RNG, no division):
  std::vector<std::uint32_t> keys(kBudgetKeys);
  for (std::size_t i = 0; i < kBudgetKeys; ++i) {
    keys[i] = mixKey(i, 0x3FFFFFu);  // the 22-bit fine-depth range
  }
  DepthSort sorter = makeSorter(kBudgetKeys, "the budget capacity 10000");
  auto run = [&sorter, &keys]() {
    (void)sorter.sort(std::span<const std::uint32_t>(keys));
  };
  if (entry == nullptr) {
    // The ungated run: the workload's value here is the
    // leak/race/correctness coverage the sanitizer runtimes (and the
    // non-reference runners) provide — warm-up only, no assertions.
    for (std::int32_t i = 0; i < kBudgetWarmup; ++i) run();
    return {};
  }
  for (std::int32_t i = 0; i < kBudgetWarmup; ++i) run();
  laige::Histogram hist(
      laige::Histogram::Options{static_cast<std::size_t>(kBudgetRuns)});
  for (std::int32_t i = 0; i < kBudgetRuns; ++i) {
    laige::TimeIt timer;
    run();
    hist.record(timer.elapsedMs());
  }
  const std::string buildLine = std::string(kCompilerId) +
#if defined(LAIGE_DEPTH_SORT_BUILD_TYPE)
      ", CMake " LAIGE_DEPTH_SORT_BUILD_TYPE
#else
      ", CMake build type unknown"
#endif
      ", engine policy (NFR-8.10)";
  const std::string machine = MachineLine();
  laige::BudgetReportContext ctx;
  ctx.workload = entry->workload.c_str();
  ctx.build = buildLine.c_str();
  ctx.machine = machine.c_str();
  ctx.warmup = static_cast<std::uint32_t>(kBudgetWarmup);
  return laige::budgetCheck(*entry, hist, ctx);
}

#if defined(LAIGE_DEPTH_SORT_BUDGET)
// The repo budgets.json (the LAIGE_BUDGETS_PATH ctest environment
// variable, set by the entry below).
std::string BudgetsFilePath() {
  const char* p = std::getenv("LAIGE_BUDGETS_PATH");
  if (p == nullptr || p[0] == '\0') {
    std::fprintf(stderr,
                 "DepthSortBudget: the LAIGE_BUDGETS_PATH env var is "
                 "unset or empty; the `depth_sort` ctest entry sets it — "
                 "the gated budget run cannot find budgets.json\n");
    std::abort();
  }
  return p;
}
#endif

TEST(DepthSortBudget, TenKSort) {
#if defined(LAIGE_DEPTH_SORT_BUDGET)
  // The gated branch (the reference platform — the non-instrumented
  // Linux trees): load the budgets.json `depth_sort_10k` entry and
  // gate the workload's metric against it (the iso_picking_tests.cpp
  // pattern). The 4-line AGENTS §12 report lands in the ctest log
  // (AGENTS §12).
  const std::string path = BudgetsFilePath();
  const laige::Result<laige::BudgetTable, laige::ErrorCode> loaded =
      laige::loadBudgets(path);
  ASSERT_TRUE(loaded.ok()) << "loadBudgets(\"" << path << "\") failed: "
                           << laige::errorText(loaded.error());
  const laige::BudgetEntry* entry = loaded.value().find("depth_sort_10k");
  ASSERT_NE(entry, nullptr) << "budgets.json has no depth_sort_10k entry";
  ASSERT_EQ(entry->metric, laige::BudgetMetric::Mean);
  const laige::BudgetCheckResult res = runBudget(entry);
  std::fputs(res.report.c_str(), stdout);
  std::fflush(stdout);
  EXPECT_TRUE(res.passed) << res.report;
#else
  // The ungated run: a test body without assertions passes — the
  // workload's value here is the leak/race/correctness coverage the
  // sanitizer runtimes (and the non-reference runners) provide.
  (void)runBudget(nullptr);
#endif
}
