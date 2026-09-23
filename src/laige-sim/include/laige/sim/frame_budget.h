// laige-sim frame graph / budget report (M1-PROF-02; PRD FR-11.2,
// §9.1 S-6, §9.3 G-R5).
//
// FR-11.2 (frame graph / budget report): per-frame breakdown against
// declared budgets; over-budget systems flagged. This header ships
// the headless half of that surface: the cheap per-frame records the
// Engine accumulates in its run loop, the fixed window over them, and
// the cold report that evaluates every declared budget (system time,
// total tick time, allocation count) — measured vs declared, with a
// pass/flag per budget and the over-budget systems listed (the same
// counters the M1-SYS-03 G-R5 warn/error events count).
//
//   FrameBudgetRecord    one completed frame's cheap budget scalars
//   FrameBudgetRecorder  the fixed ring over the last frames (no
//                        allocation after construction)
//   kFrameBudgetWindow   the default (and max) retained frame count
//   FrameBudgetReport    the evaluated report: the overall pass/flag
//                        plus the formatted text
//   FrameBudgetReportOptions
//                        the report's options (the retained frame
//                        count, the AGENTS §12 caller context)
//   buildFrameBudgetReport
//                        the cold report builder (allocates —
//                        reporting is never a hot path)
//
// ---------------------------------------------------------------------------
// Declared budgets (the "declared" side of measured vs declared)
// ---------------------------------------------------------------------------
//
// Three declared budget families feed the report:
//
//   - system time: each system's DECLARED per-tick time budget —
//     SystemDef::budgetMs (M1-SYS-01, fpx16_16 ms, exact). The
//     config's system_time_default_ms (BudgetsConfig) is the declared
//     default for systems that do not declare one; M1 systems always
//     declare one (registration rejects a zero/negative budget), so
//     the report evaluates each system against its own declared
//     budget.
//   - total tick time: the PRD §8.1 hard budgets sim_tick_avg (mean
//     tick time, ms) and sim_tick_p99 (p99 tick time, ms) — the named
//     budgets.json entries (M0-CORE-08) evaluated against the
//     Profiler's tick-time window (the M1-PROF-01 per-completed-tick
//     samples).
//   - allocation count: the PRD §8.1 hard-zero budget sim_heap_allocs
//     (heap allocations per frame, target 0) evaluated against the
//     per-frame sim allocation deltas (the pool accounting's
//     totalReservations delta, World::archetypeStats — M1-ECS-03;
//     M1-ALLOC-01 will refine the per-frame count once it exists).
//
// ---------------------------------------------------------------------------
// The per-frame record (cheap, hot path)
// ---------------------------------------------------------------------------
//
// The Engine's run loop (engine.h run_headless) records one
// FrameBudgetRecord per COMPLETED frame — the same frames the
// Profiler records (a failed frame is not a completed frame; the
// start-reference first frame is not a run frame — the M1-PROF-01
// run loop contract). The fields:
//
//   frame             the 0-based index of the frame within the run's
//                     run frames (the start-reference frame is not one
//                     of them)
//   tickAfter         the completed tick count after this frame (the
//                     cumulative tick index; 0..maxTicks)
//   ticks             the completed ticks WITHIN this frame
//                     (tickAfter minus the previous frame's tickAfter)
//   frameMs           the frame's sim work + presentation refresh, in
//                     ms — the exact value handed to the Profiler's
//                     recordFrame (0.0 when the profiler is disabled:
//                     no measurement, no clock reads — DBG-004)
//   simAllocs         the frame's sim allocation count: the
//                     World::archetypeStats().totalReservations delta
//                     across the frame (0 = the steady-state target)
//   overrunWarns      the system/budget_overrun WARN events (G-R5)
//                     issued during this frame — the per-frame delta
//                     of the per-system warn counters (M1-SYS-03)
//   criticalErrors    the system/budget_critical ERROR events (G-R5)
//                     issued during this frame (as above)
//
// Record cost (PERF-003, LOG-003): two O(1) counter reads (the loop's
// tick count, the pool reservations), two O(systemCount) passes over
// the per-system timing counters (the G-R5 warn/error deltas), one
// O(1) ring write — no allocation, no logging. The measured times are
// diagnostics (ARCH-009): they never enter authoritative simulation
// state, hashes, or replays.
//
// ---------------------------------------------------------------------------
// The report (cold path)
// ---------------------------------------------------------------------------
//
// buildFrameBudgetReport evaluates the declared budgets against the
// run's measurements and formats the machine-greppable report
// (AGENTS §12 field format — the same field layout the M0-CORE-08
// budgetCheck emits for the budgets.json entries):
//
//   laige-budget-report version=1
//   laige-budget-report context: workload=... build=... machine=... warmup=...
//   laige-budget-report frames: n=<retained> total=<recorded>
//   laige-budget-report frame=<i> ticks=<t> tick_after=<T> frame_ms=<v>
//       sim_allocs=<a> overrun_warns=<w> critical_errors=<e> result=PASS|FAIL
//   budget=sim_tick_avg result=<PASS|FAIL|NO_SAMPLES|NO_ENTRY> metric=mean unit=ms
//     after=<v> before=<v> target=<v>
//     stats: n=... min=... mean=... p50=... p95=... p99=... max=...
//     context: workload=... build=... machine=... warmup=...
//   budget=sim_tick_p99 ...
//   budget=sim_heap_allocs ...
//   laige-budget-report system id=<i> name=<n> budget_ms=<b>
//       measured_p99_ms=<m> result=<PASS|FAIL|NO_SAMPLES> warns=<w> errors=<e>
//       window: n=... ...
//   laige-budget-report over_budget: id=<i> name=<n> p99_ms=<m> budget_ms=<b>
//       (one line per over-budget system, ascending id; `none` when none)
//   laige-budget-report overall=PASS|FAIL
//
// Pass/flag semantics (the budgets are AT-MOST upper bounds — the
// M0-CORE-08 convention):
//
//   - a frame: result=FAIL iff sim_allocs > 0 (the hard-zero
//     allocation budget) or a G-R5 event fired during the frame
//     (overrunWarns / criticalErrors > 0 — the frame's ticks exceeded
//     a declared system budget). frame_ms is informational (no
//     declared per-frame tick budget exists in M1 — the tick budgets
//     are rolling statistics, evaluated over the window below).
//   - sim_tick_avg / sim_tick_p99: the M0-CORE-08 budgetCheck over the
//     Profiler's tick window (an empty window is a NO_SAMPLES
//     failure — never silent, CORE-008); a missing budgets.json
//     entry is a NO_ENTRY failure (a configuration error, loud).
//   - sim_heap_allocs: the M0-CORE-08 budgetCheck (metric max) over a
//     histogram of the retained frames' sim_allocs deltas (an empty
//     window — no completed frames — is NO_SAMPLES; a missing entry
//     is NO_ENTRY).
//   - a system: measured = its M1-SYS-03 rolling window's p99 (the
//     same statistic the G-R5 warn carries); result=FAIL iff p99 >
//     the declared budget (a sustained overrun — a single
//     over-budget tick that recovered stays visible in the warns /
//     errors counters and the per-frame results); an empty window
//     (the system never ran) is NO_SAMPLES.
//   - overall: PASS iff every frame, every budget entry, and every
//     system passed (NO_SAMPLES / NO_ENTRY count as failures — a
//     broken harness is loud, not green).
//
// `passed` mirrors the overall line. The report is the operator
// surface (laige-run --budget-report, docs/api/frame_budget.md);
// CI gates on it through --fail-on-budget (exit 3 on overall=FAIL).
//
// Ownership (CONC-001): the recorder is a value type with one owner
// thread while mutable (the Engine's run thread); the report reads
// it cold after the run (the engine builds the cached report before
// the ordered shutdown releases the world and the profiler).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "laige/budget_harness.h"  // BudgetTable, BudgetReportContext, Histogram
#include "laige/sim/entity.h"      // World (the cold pull: systems, windows, pools)
#include "laige/sim/profiler.h"    // Profiler (the tick window, M1-PROF-01)
#include "laige/sim/system.h"      // SystemId (the per-system section)

namespace laige {

// One completed frame's cheap budget scalars (M1-PROF-02). Fixed
// storage, since-construction units documented in the header
// preamble. A record is a value: built by the Engine's run loop and
// stored in the FrameBudgetRecorder ring (no pointer, no lifetime
// question — CPP-002).
struct FrameBudgetRecord {
  // The 0-based index of the frame within the run's run frames (the
  // start-reference first frame is not one of them — the M1-PROF-01
  // run loop contract).
  std::uint64_t frame{};
  // The completed tick count after this frame (the cumulative tick
  // index; a bounded run ends with tickAfter == the run target).
  std::uint64_t tickAfter{};
  // The completed ticks WITHIN this frame (0 when the frame ran ahead
  // of the tick rate; several in a catch-up frame — the M1-LOOP-01
  // accumulator).
  std::uint32_t ticks{};
  // The frame's sim work + presentation refresh in ms (0.0 when the
  // profiler is disabled — no measurement — the preamble).
  double frameMs{};  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured frame time never enters sim state, hashes, or replays (M1-PROF-02, ARCH-009)
  // The frame's sim allocation count (the pool-reservations delta;
  // 0 = the steady-state target — the PRD §8.1 hard-zero budget).
  std::uint64_t simAllocs{};
  // The G-R5 system/budget_overrun WARN events issued during this
  // frame (the per-frame delta of the per-system warn counters).
  std::uint32_t overrunWarns{};
  // The G-R5 system/budget_critical ERROR events issued during this
  // frame (as above).
  std::uint32_t criticalErrors{};
};

// The number of frames the FrameBudgetRecorder retains (CORE-005). At
// the default 60 Hz tick rate the window spans ~0.53 s — long enough
// for a frame graph to show a regression, small enough that the ring
// (32 × 48 B) is negligible fixed storage. 0 is not a capacity for
// the ring itself (a 0-frame report is the loud NO_SAMPLES state —
// the budgetCheck precedent); kFrameBudgetWindow is the default and
// the max for FrameBudgetReportOptions::lastNFrames.
inline constexpr std::uint32_t kFrameBudgetWindow = 32;

// The fixed ring over the last frames' records (M1-PROF-02). The
// Engine owns one (setup path: no allocation — the ring is a fixed
// array member; the profiler's fixed-storage precedent).
class FrameBudgetRecorder {
 public:
  // Construct the recorder (setup path): no allocation (the fixed
  // ring), nothing recorded (count() 0, totalFrames() 0).
  FrameBudgetRecorder() = default;

  // Record one completed frame (hot path — the Engine's run loop).
  // O(1): one ring write; no allocation, no logging (PERF-003,
  // LOG-003). Beyond kFrameBudgetWindow the OLDEST record is dropped
  // (the M0-CORE-08 window semantics); totalFrames() keeps counting
  // every recorded frame, so a truncated window is observable
  // (CORE-008: silent truncation is not allowed).
  // @budget O(1); no allocation (hot path).
  void recordFrame(FrameBudgetRecord record) noexcept;

  // Clear the ring and the counters (idempotent). Cold path (the
  // tests; a run never resets mid-flight).
  // @budget O(kFrameBudgetWindow) cold path; no allocation.
  void reset() noexcept;

  // The frames recorded since construction / the last reset (not
  // truncated — CORE-008).
  // @budget O(1); no allocation.
  [[nodiscard]] std::uint64_t totalFrames() const noexcept;

  // The records currently retained (0..kFrameBudgetWindow).
  // @budget O(1); no allocation.
  [[nodiscard]] std::uint32_t count() const noexcept;

  // The retained record at position `i` in OLDEST-FIRST order
  // (0 = the oldest retained frame; `i` in 0..count() - 1). The last
  // min(totalFrames(), kFrameBudgetWindow) recorded frames in time
  // order (the M0-CORE-08 rolling-window order). Points into the
  // recorder's fixed storage (valid until the next recordFrame /
  // reset). The ring wraps, so the retained records are not a
  // contiguous span — read them through this accessor (the cold
  // report's loop, the tests).
  // @budget O(1); no allocation.
  [[nodiscard]] const FrameBudgetRecord& at(std::uint32_t i) const noexcept;

 private:
  // The ring: record i (in write order) sits at ring_[i mod window],
  // so after total_ writes the oldest RETAINED record sits at
  // ring_[(total_ - count_) mod window] — the oldest-first head
  // (at() adds the offset and wraps). All three counters are fixed
  // state (no separate head pointer — nothing to keep consistent).
  FrameBudgetRecord ring_[kFrameBudgetWindow]{};
  // The one-past-the-next-write index in ring_ (0..window);
  // total_ % window.
  std::size_t cursor_{0};
  // The records currently retained (0..window).
  std::uint32_t count_{0};
  // The frames recorded since construction / reset (unbounded).
  std::uint64_t total_{0};
};

// The report options (API-006).
struct FrameBudgetReportOptions {
  // The number of frames to include, oldest first (1..
  // kFrameBudgetWindow; 0 = kFrameBudgetWindow — all retained).
  // Frames beyond the retained window are not recoverable (the ring
  // dropped them — CORE-008).
  std::uint32_t lastNFrames{kFrameBudgetWindow};
  // The AGENTS §12 caller context for the report (the workload /
  // build / machine the numbers were measured on, the warmup sample
  // count — the M0-CORE-08 BudgetReportContext).
  BudgetReportContext context{};
};

// The evaluated report (M1-PROF-02): the overall pass/flag plus the
// formatted text (the header preamble's layout). A value: the Engine
// caches the last run's report (cold string — reporting is never a
// hot path).
struct FrameBudgetReport {
  // The overall pass/flag (the `overall=` line): false when any frame,
  // budget entry, or system failed (NO_SAMPLES / NO_ENTRY included).
  bool passed{};
  // The machine-greppable report text (AGENTS §12 field format).
  std::string report;
};

// Build the frame graph / budget report (cold path). `recorder` is
// the run's per-frame records, `profiler` the always-on counters (the
// tick window feeds the sim_tick_avg / sim_tick_p99 checks), `world`
// the run's world (the per-system declared budgets and the M1-SYS-03
// windows — read cold, never mutated), `budgets` the budgets.json
// table (the sim_tick_avg / sim_tick_p99 / sim_heap_allocs entries).
// The report evaluates every declared budget (the preamble's
// semantics) and formats the text. Every failure state is loud in the
// text (NO_SAMPLES / NO_ENTRY / FAIL lines) — never silent
// (CORE-008).
// @budget O(systemCount × n log n + window) cold path; allocates
// (the report string).
[[nodiscard]] FrameBudgetReport buildFrameBudgetReport(
    const FrameBudgetRecorder& recorder, const Profiler& profiler,
    const World& world, const BudgetTable& budgets,
    const FrameBudgetReportOptions& options = {});

}  // namespace laige
