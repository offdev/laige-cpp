// laige-core budget harness (M0-CORE-08).
//
// PRD 8.1: performance budgets are *hard* and measured in CI; the policy
// is that a PR regressing any budget by > 10% (or breaching the absolute
// target) fails CI. CORE-001: no performance claim without a reproducible
// measurement. AGENTS 12: benchmark reports MUST record hardware, OS,
// compiler and version, build type, relevant flags, dataset/workload,
// warm-up, sample count, summary statistics, and before/after results.
//
// This header ships the three library pieces of the harness (the
// `laige-bench` executable in tools/bench is the operator-facing front
// end for them):
//
//   Histogram     Fixed-capacity sample store (rolling window) with exact
//                 min/mean/p50/p95/p99/max over the stored window.
//                 record() is O(1) and allocates nothing (PERF-003);
//                 stats() is a cold-path O(n log n) pass.
//   TimeIt        Steady-clock scope timer in milliseconds.
//   loadBudgets / budgetCheck
//                 Named budget entries from budgets.json (repo root) plus
//                 the pass/fail check that formats the AGENTS 12 report
//                 (before/after numbers; the caller supplies the
//                 machine/build context it owns).
//
// ---------------------------------------------------------------------------
// Histogram contract
// ---------------------------------------------------------------------------
//
// Window semantics (PERF-008 backpressure, no unbounded growth): the
// histogram keeps at most Options::capacity samples — a rolling window.
// record() beyond capacity drops the *oldest* sample; totalRecorded()
// keeps counting every sample ever recorded, so a truncated window is
// observable (CORE-008: silent truncation is not allowed). A benchmark
// that needs every sample sets capacity >= runs.
//
// Percentiles (nearest-rank, the documented exact definition): for the
// sorted stored window v[0..n-1] (n >= 1) and percentile p (0..100),
// rank r = ceil(p*n/100) in exact integer math (clamped to >= 1); the
// percentile is v[r-1]. p=0 is the min, p=100 the max, a one-sample
// window returns that sample for every p. Nearest-rank is chosen over
// linear interpolation: no fractional indices, no extra allocation,
// bit-identical on every platform (CORE-004).
//
// Statistics scope: stats() describes exactly the stored window (the
// last min(totalRecorded, capacity) samples). mean is computed over
// that window (no running sum, so no float drift). When n == 0 the six
// statistics are NaN — callers must check n (budgetCheck turns an empty
// histogram into a loud NO_SAMPLES failure instead of reading NaN).
//
// Errors: none — a histogram cannot fail (record() into a full window
// drops the oldest by contract; capacity 0 is legal and drops all).
//
// Ownership/threading (CPP-002, CONC-001): a histogram is a value type
// (copy = O(capacity) deep copy, cold path; move = O(1)). It has exactly
// one owner thread while mutable; stats() on a fully built histogram is
// safe to read from any thread (publish contract, like Result/Status).
//
// Performance: construction performs the two backing allocations (setup
// path). record() = one index arithmetic + one store: O(1), no
// allocation, no lock, no I/O (hot-path safe). stats() sorts a
// pre-allocated scratch buffer: O(n log n) time, no allocation; it is
// the cold path (budget checks, reports — never frame/tick loops).
//
// ---------------------------------------------------------------------------
// TimeIt contract
// ---------------------------------------------------------------------------
//
// Clock: std::chrono::steady_clock (monotonic; immune to wall-clock
// adjustments — the right clock for durations). Unit: milliseconds
// (double). Construction is the scope start; elapsedMs() reads the
// elapsed time. No allocation, no lock. A constructed TimeIt is immutable
// (start point fixed), so reading it from other threads is safe; reset()
// must come from the owner.
//
// Misuse: TimeIt measures *durations*, not timestamps — for "what time
// is it" use the logging facade's timestamps (system_clock, RFC 3339).
//
// ---------------------------------------------------------------------------
// Budget entries and checks (budgets.json, repo root)
// ---------------------------------------------------------------------------
//
// budgets.json is the versioned home of the PRD 8.1 budgets (ARCH-007:
// readers reject unsupported versions and fields explicitly — see
// loadBudgets below and docs/api/budget_harness.md for the schema).
//
//   target     the hard PRD 8.1 limit, in `unit`. Every 8.1 budget is an
//              at-most upper bound. target == 0 is a *hard zero budget*
//              (e.g. sim heap allocations per frame), NOT "not set".
//   measured   the last recorded value of this budget (the "before"
//              number of the AGENTS 12 before/after pair). 0 is the
//              M0 convention for "not yet measured"; the benchmark
//              runner writes a real value when it measures the budget.
//   metric     which histogram statistic the check evaluates.
//
// budgetCheck semantics:
//   - histogram empty (n == 0)     -> passed = false, result NO_SAMPLES
//                                     (a workload that recorded nothing
//                                     is a broken harness — loud, never
//                                     silent, CORE-008)
//   - target > 0                   -> passed iff measured <= target
//   - target == 0 (hard zero)      -> passed iff measured == 0
//
// The report is stable, machine-greppable text (format in
// docs/api/budget_harness.md): the first line carries
// `budget=<name> result=<PASS|FAIL|NO_SAMPLES> metric=<m> unit=<u>`;
// the following lines carry after/before/target, the summary statistics,
// and the caller context.
//
// Errors: loadBudgets returns Result<BudgetTable, ErrorCode>:
//   - unreadable file                          -> ErrorCode::IoError (5)
//   - malformed JSON or schema violation       -> ErrorCode::MalformedInput (3)
//     (bad/unknown version, unknown or missing field, bad metric or
//      unit or name charset, duplicate name, negative or non-finite
//      number, file above the 1 MiB bound)
// budgetCheck itself cannot fail (a check on any state is total) — its
// outcome is the passed flag plus the report.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//   - Recording wall-clock timestamps into a Histogram is a unit error:
//     the harness measures durations (typically TimeIt::elapsedMs()).
//   - budgetCheck allocates (the report string) and runs stats() —
//     O(n log n). It is a cold path: never call it from a frame or tick
//     loop (PERF-002).
//   - A Histogram with capacity < runs silently (by contract) truncates
//     its window: check totalRecorded() == count() when every sample
//     matters.
//   - loadBudgets does file I/O: setup/reporting paths only.
//   - Discarding the Result from loadBudgets is a likely logic bug
//     (CORE-008); the Result is the only failure channel.

#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "laige/result.h"

namespace laige {

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

// Summary statistics over the samples currently stored in a Histogram
// (rolling window). When n == 0 the six statistics are NaN (check n;
// budgetCheck turns an empty histogram into a loud NO_SAMPLES failure).
struct HistogramStats {
  std::uint64_t n;      // samples stored in the window
  double min;          // smallest stored sample
  double mean;         // arithmetic mean of the stored window
  double p50;          // nearest-rank 50th percentile (median rank)
  double p95;          // nearest-rank 95th percentile
  double p99;          // nearest-rank 99th percentile
  double max;          // largest stored sample
};

// A fixed-capacity, allocation-free sample store (rolling window).
// See the header preamble for the full contract (window semantics,
// nearest-rank percentile definition, performance, threading).
class Histogram {
 public:
  struct Options {
    // Capacity: the maximum number of samples kept. 0 is legal: every
    // record() is dropped and stats() is always empty (useful as a
    // churn-only counter, and as the loud-failure state for budgetCheck).
    std::size_t capacity = 0;
  };

  // Setup path: performs the two backing allocations (window + scratch
  // sort buffer). O(capacity) time and space.
  explicit Histogram(Options options) noexcept
      : capacity_(options.capacity),
        window_(options.capacity),
        scratch_(options.capacity),
        cursor_(0),
        count_(0),
        total_(0) {}

  // Value semantics: copy is O(capacity) (deep, cold path), move O(1).
  Histogram(const Histogram&) = default;
  Histogram(Histogram&&) = default;
  Histogram& operator=(const Histogram&) = default;
  Histogram& operator=(Histogram&&) = default;

  // Record one sample (unit: whatever the caller measures — typically
  // milliseconds from a TimeIt). O(1), no allocation, no lock, noexcept.
  // When the window is full the oldest sample is dropped; totalRecorded()
  // keeps counting, so truncation is observable.
  void record(double value) noexcept {
    ++total_;
    if (capacity_ == 0) return;
    window_[cursor_] = value;
    cursor_ = (cursor_ + 1) % capacity_;
    if (count_ < capacity_) ++count_;
  }

  // Drop every stored sample (count -> 0). totalRecorded() survives
  // (since-construction churn; per-frame profilers diff it). Idempotent.
  void reset() noexcept {
    cursor_ = 0;
    count_ = 0;
  }

  // Samples currently stored in the window (<= capacity).
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

  // Samples recorded since construction, including dropped ones (churn).
  [[nodiscard]] std::uint64_t totalRecorded() const noexcept { return total_; }

  // Exact statistics over the stored window (nearest-rank percentiles —
  // see the preamble). Cold path: O(n log n) time, no allocation (sorts
  // the pre-allocated scratch buffer; logically const — scratch_ is a
  // reusable work buffer, not state).
  [[nodiscard]] HistogramStats stats() const {
    HistogramStats s{};
    const std::uint64_t n = count_;
    s.n = n;
    if (n == 0) {
      s.min = s.mean = s.p50 = s.p95 = s.p99 = s.max = std::nan("");
      return s;
    }
    // Copy the stored window (oldest -> newest) into the scratch buffer;
    // the window itself is never reordered.
    if (n == capacity_) {
      // Full ring: the oldest sample sits at cursor_ (the next slot to
      // overwrite), so the window starts there and wraps.
      std::copy_n(window_.cbegin() + std::ptrdiff_t(cursor_),
                  std::ptrdiff_t(capacity_ - cursor_), scratch_.begin());
      std::copy_n(window_.cbegin(), std::ptrdiff_t(cursor_),
                  scratch_.begin() + std::ptrdiff_t(capacity_ - cursor_));
    } else {
      // Not yet full: the window is the plain prefix [0, n).
      std::copy_n(window_.cbegin(), std::ptrdiff_t(n), scratch_.begin());
    }
    std::sort(scratch_.begin(), scratch_.begin() + std::ptrdiff_t(n));

    s.min = scratch_.front();
    s.max = scratch_[n - 1];
    double sum = 0.0;
    for (std::uint64_t k = 0; k < n; ++k) sum += scratch_[k];
    s.mean = sum / double(n);
    s.p50 = percentile(scratch_, n, 50);
    s.p95 = percentile(scratch_, n, 95);
    s.p99 = percentile(scratch_, n, 99);
    return s;
  }

 private:
  // Nearest-rank percentile over a sorted window v[0..n-1] (n >= 1), p
  // in [0, 100]: rank r = ceil(p*n/100) in exact integer math (clamped
  // to >= 1); the percentile is v[r-1]. (Preamble: the documented
  // definition; p=0 is the min, p=100 the max.)
  [[nodiscard]] static double percentile(const std::vector<double>& sorted,
                                          std::uint64_t n, int p) {
    std::uint64_t rank = (std::uint64_t(p) * n + 99) / 100;
    if (rank < 1) rank = 1;
    return sorted[rank - 1];
  }

  std::size_t capacity_;
  std::vector<double> window_;  // ring storage (setup allocation)
  mutable std::vector<double> scratch_;  // stats() sort buffer (setup alloc)
  std::size_t cursor_;  // next slot to write (the oldest slot when full)
  std::uint64_t count_;  // samples stored (<= capacity_)
  std::uint64_t total_;  // samples recorded since construction
};

// ---------------------------------------------------------------------------
// TimeIt
// ---------------------------------------------------------------------------

// A scope timer over std::chrono::steady_clock (monotonic — see the
// preamble). Milliseconds as a double. No allocation, no lock.
class TimeIt {
 public:
  // Starts the scope now.
  TimeIt() noexcept : start_(std::chrono::steady_clock::now()) {}

  // Restarts the scope (owner thread only).
  void reset() noexcept { start_ = std::chrono::steady_clock::now(); }

  // Elapsed time in milliseconds since construction/reset.
  [[nodiscard]] double elapsedMs() const noexcept {
    return std::chrono::duration_cast<
               std::chrono::duration<double, std::milli>>(
               std::chrono::steady_clock::now() - start_)
        .count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// Budget entries and checks (budgets.json)
// ---------------------------------------------------------------------------

// The statistic of a Histogram that a budget entry checks.
enum class BudgetMetric : std::uint8_t { Mean, Min, Max, P50, P95, P99 };

// One named budget from budgets.json (PRD 8.1 row -> entry). See the
// preamble for the target/measured/unit/metric contract.
struct BudgetEntry {
  std::string name;       // stable snake_case id (machine-greppable)
  BudgetMetric metric;    // which statistic the check evaluates
  std::string unit;       // identifier unit (ms, draw_calls, allocs_per_frame, ...)
  double target;          // hard PRD 8.1 limit (at-most); 0 = hard zero budget
  double measured;        // last recorded value; 0 = not yet measured
  std::string workload;   // the workload the budget applies to
};

// The parsed budgets.json (schema v1). Immutable after loading; safe to
// read from any thread. Small by design (one row per PRD 8.1 budget):
// find() is a linear scan on the cold path (no hash map, PERF-006).
class BudgetTable {
 public:
  BudgetTable() = default;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  // The entries in file order.
  [[nodiscard]] const std::vector<BudgetEntry>& entries() const noexcept {
    return entries_;
  }

  // The entry with the given name, or nullptr when absent.
  [[nodiscard]] const BudgetEntry* find(std::string_view name) const noexcept {
    for (const BudgetEntry& e : entries_)
      if (name == e.name) return &e;
    return nullptr;
  }

 private:
  std::vector<BudgetEntry> entries_;
  friend Result<BudgetTable, ErrorCode> loadBudgets(std::string_view path);
};

// Caller-supplied context for the AGENTS 12 report fields the harness
// cannot know (machine/build facts). The caller (benchmark runner /
// operator) fills these; budgetCheck formats them verbatim into the
// report (diagnostic text, not engine state). Defaults to all-empty.
struct BudgetReportContext {
  const char* workload = "";  // dataset/workload name
  const char* build = "";     // compiler + version, build type, flags
  const char* machine = "";   // hardware + OS
  std::uint32_t warmup = 0;   // warm-up iterations performed
};

// The stable one-line text form of a HistogramStats value:
// `stats: n=<n> min=<v> mean=<v> p50=<v> p95=<v> p99=<v> max=<v>`
// (6 significant digits, locale-free; "nan" for an empty histogram).
// The report lines of budgetCheck and the laige-bench tool both use
// this, so the stats text has one source (LOG-001 stable fields).
// Cold path: allocates one string.
[[nodiscard]] std::string formatStatsLine(const HistogramStats& s);

// The outcome of a budget check plus the formatted AGENTS 12 report
// (stable multi-line text; format in docs/api/budget_harness.md).
struct BudgetCheckResult {
  bool passed;
  double measured;  // the entry's metric over the histogram window
                    // (NaN when the histogram was empty)
  double target;    // the entry's target
  double before;    // the entry's last recorded value (before/after pair)
  std::string report;
};

// Loads and validates budgets.json (schema v1) from `path`.
//
// Cold path: file I/O + parse (PERF-002 is a hot-path rule; this is a
// setup/reporting path by design). Bounds: the file must not exceed 1 MiB
// (kBudgetsMaxDocumentBytes, the same bound as JsonOptions' default) and
// the document is parsed by the bounded JSON parser (ADR 0003: depth 32).
// Schema validation is strict (ARCH-007): unknown version, unknown or
// missing field, bad name/metric/unit, duplicate name, negative or
// non-finite number -> MalformedInput. Unreadable file -> IoError.
// A failed load produces no table (all-or-nothing).
[[nodiscard]] Result<BudgetTable, ErrorCode> loadBudgets(std::string_view path);

// Checks `entry` against `histogram` (semantics in the preamble:
// NO_SAMPLES / target>0 at-most / target==0 hard zero). before/after are
// entry.measured / the current measurement (the AGENTS 12 before/after
// pair). Cold path: O(n log n) (the stats pass) plus report string
// building (allocates — reporting is never a hot path). Thread-safe on
// const inputs; the histogram must not be mutated concurrently
// (CONC-001 single-owner rule).
[[nodiscard]] BudgetCheckResult budgetCheck(const BudgetEntry& entry,
                                            const Histogram& histogram,
                                            const BudgetReportContext& context = {});

}  // namespace laige
