// laige-sim per-system timing + budget enforcement (M1-SYS-03; PRD
// §9.3 G-R5).
//
// Implementation of the timing methods declared in
// include/laige/sim/entity.h — see that header (the member docs),
// the system.h "Timing and budget enforcement" section, and
// docs/api/system_timing.md for the full contract:
//
//   - World::runSystems (systems.cpp) times each system's own run
//     (TimeIt — the M0-CORE-08 steady_clock scope timer, ms as a
//     double) and hands the measurement to World::checkSystemBudget.
//   - checkSystemBudget records the sample in the system's rolling
//     window (the M0-CORE-08 Histogram, fixed capacity
//     kSystemTimingWindowSamples — O(1), no allocation) and
//     enforces the declared budget:
//         measured >  1 × budget  -> system/budget_overrun  (Warn)
//         measured >= 3 × budget  -> system/budget_critical (Error)
//     (kBudgetCriticalMultiplier; PRD §9.3 G-R5 "over 3× → error
//     event"). Both events follow the NFR-13.3 5-field message
//     grammar (build-stable text; the dynamic values are structured
//     fields) and are rate-limited per (subsystem, event, severity)
//     (LOG-004); the warn carries the window's rolling p99, and both
//     events count in SystemTimingStats. An over-budget system is
//     still run (observation, never an execution gate).
//   - systemTimingStats / systemTimingWindow are the profiler feed
//     (M1-PROF-01/02): cheap per-frame scalars plus the rolling
//     window for the frame graph's budgetCheck.
//
// Hot-path cost (per system per tick): two steady_clock reads, one
// O(1) ring write, two comparisons — no allocation and no logging on
// the success path (PERF-003, LOG-003). The measured times are
// diagnostics (ARCH-009): they never enter authoritative simulation
// state, hashes, or replays.

#include "laige/sim/entity.h"

#include <cassert>
#include <cstdint>

#include "laige/logging.h"

namespace laige {

namespace {

// The stable subsystem name for system events (LOG-001; systems.cpp).
inline constexpr const char* kSystemSubsystem = "system";

// NFR-13.3 5-field grammar, identical in every build ({code} |
// {what} | {why} | {fix} | {doc_anchor}). The dynamic values (measured
// time, budget, rolling p99, window fill) are structured fields, never
// message text: the machine-parseable message stays build-stable.
inline constexpr const char* kBudgetOverrunMessage =
    "budget_overrun | a system exceeded its declared per-tick time "
    "budget this tick | sustained overruns consume tick time and mask "
    "performance regressions | profile the system's per-tick work "
    "(query scope, batching, bounded iteration) and reduce it, or "
    "raise the declared budget at registration | "
    "docs/api/system_timing.md";

inline constexpr const char* kBudgetCriticalMessage =
    "budget_critical | a system ran 3x or more over its declared "
    "per-tick time budget this tick | the system is consuming a large "
    "fraction of the tick and endangers the tick-time targets | cut "
    "the system's per-tick work (query scope, batching, spatial "
    "partitioning) and re-measure, or document the higher budget "
    "through typed configuration | docs/api/system_timing.md";

}  // namespace

Result<SystemTimingStats, ErrorCode>
World::systemTimingStats(SystemId id) const noexcept {
  // Ids are dense from 1, so a valid registered id is exactly the
  // range [1, systemCount_] (system.h contract; the World::system
  // precedent: an invalid id is a pure-query failure, no warn).
  if (id.value == 0 || id.value > systemCount_ ||
      systemTiming_ == nullptr) {
    return ErrorCode::InvalidArgument;
  }
  const detail::SystemTimingRecord& rec = systemTiming_[id.value - 1];
  return SystemTimingStats{rec.runs, rec.lastMs, rec.warns, rec.errors};
}

const Histogram* World::systemTimingWindow(SystemId id) const noexcept {
  if (id.value == 0 || id.value > systemCount_ ||
      systemTiming_ == nullptr) {
    return nullptr;
  }
  return systemTiming_[id.value - 1].window.get();
}

void World::checkSystemBudget(std::uint32_t id, double measuredMs) noexcept {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured run time never enters sim state, hashes, or replays (M1-SYS-03, ARCH-009)
  // The timing table exists whenever a system is registered
  // (allocated in create() alongside the registry, moved with it),
  // and runSystems reaches here only with a non-empty registry —
  // the invariant is structural (assert, CPP-012).
  assert(systemTiming_ != nullptr);
  detail::SystemTimingRecord& rec = systemTiming_[id - 1];
  const detail::SystemRecord& recDef = systems_[id - 1];

  // The rolling window sample: O(1), no allocation (PERF-003). The
  // oldest sample drops when the window is full (the M0-CORE-08
  // contract — totalRecorded() keeps counting).
  rec.window->record(measuredMs);
  ++rec.runs;
  rec.lastMs = measuredMs;

  // The declared budget in ms, converted EXACTLY: fpx16_16 raw / 2^16
  // is a power-of-two scale, and the raw range (±2^31) fits the
  // double mantissa, so the comparison operands are exact.
  const double budgetMs =  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic (M1-SYS-03, ARCH-009): fpx16_16 raw / 2^16 is a power-of-two scale, exact in the double mantissa
      static_cast<double>(recDef.def.budgetMs.raw) / 65536.0;  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic (M1-SYS-03, ARCH-009)
  const double criticalMs =  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic (M1-SYS-03, ARCH-009)
      static_cast<double>(kBudgetCriticalMultiplier) * budgetMs;  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic (M1-SYS-03, ARCH-009)

  if (measuredMs > budgetMs || measuredMs >= criticalMs) {
    // Cold path — only while the system is over budget. One window
    // stats pass (O(W log W) over the fixed W =
    // kSystemTimingWindowSamples, no allocation: the Histogram's
    // pre-allocated scratch buffer) feeds both events' rolling-p99
    // field. The field values construct only because the event's
    // level gate passed (LOG-003); rate-limited repeats still pay
    // this bounded cold cost while the breach persists.
    const HistogramStats stats = rec.window->stats();
    if (measuredMs > budgetMs) {
      // Strictly above the declared budget (measured == budget is
      // legal — the G-R4 strictly-greater precedent).
      ++rec.warns;
      LAIGE_LOG_WARN(kSystemSubsystem, "budget_overrun",
                     kBudgetOverrunMessage,
                     laige::log::field("system", recDef.def.name),
                     laige::log::field("id", id),
                     laige::log::field("measured_ms", measuredMs),
                     laige::log::field("budget_ms", budgetMs),
                     laige::log::field("p99_ms", stats.p99),
                     laige::log::field("window_samples", stats.n));
    }
    if (measuredMs >= criticalMs) {
      // 3× or more over the declared budget (PRD §9.3 G-R5).
      ++rec.errors;
      LAIGE_LOG_ERROR(kSystemSubsystem, "budget_critical",
                     kBudgetCriticalMessage,
                     laige::log::field("system", recDef.def.name),
                     laige::log::field("id", id),
                     laige::log::field("measured_ms", measuredMs),
                     laige::log::field("budget_ms", budgetMs),
                     laige::log::field("p99_ms", stats.p99),
                     laige::log::field("window_samples", stats.n));
    }
  }
}

}  // namespace laige
