// laige-sim profiler core (M1-PROF-01; PRD FR-11.1).
//
// FR-11.1 (built-in profiler): always-on (cheap) counters — per-system
// time, entity counts, alloc counts (target: 0 in sim), draw calls,
// texture binds, net bytes, tick time, frame time percentiles —
// exposed in the editor overlay (M2), the CLI, and file export. This
// header ships the headless half of that surface: the always-on
// counters and their cold-path snapshot/report API. The per-system
// time histograms themselves are the M1-SYS-03 rolling windows
// (system.h — read through World::systemTimingWindow, never copied),
// and the per-frame budget report over them is M1-PROF-02.
//
//   kProfilerTickWindowSamples   the default tick-time window capacity
//   kProfilerFrameWindowSamples  the default frame-time window capacity
//   ProfilerStats    one snapshot value (the counters, the window
//                    stats, the world-pulled fields)
//   Profiler         the always-on counters (fixed storage, no
//                    allocation after construction)
//   ProfileFormat    the report format (Text or Json)
//   formatProfileSummaryLine
//                    the CLI one-line summary (stdout)
//   formatProfileText  / formatProfileJson
//                    the full report (cold path)
//   writeProfile     the report written to a file (cold path)
//
// ---------------------------------------------------------------------------
// The counter model (always-on, fixed storage)
// ---------------------------------------------------------------------------
//
// The Profiler owns only what it measures at the frame/tick boundary:
//
//   - tick time: one rolling Histogram (ms) fed by the GameLoop's
//     per-completed-tick timing (game_loop.h, Options::profiler —
//     the loop times each tick with the M0-CORE-08 TimeIt and hands
//     the sample to recordTick; a failed tick is not recorded — the
//     GameLoop tick-count contract)
//   - frame time: one rolling Histogram (ms) fed by the Engine's
//     headless run loop (engine.h): the frame's sim work
//     (GameLoop::frame) plus the presentation refresh, EXCLUDING the
//     pacing sleep (the sleep is cadence, not work — the M1-HEAD-01
//     run loop contract). The first frame (start reference, zero
//     ticks) is not a runFrames frame and is not recorded.
//   - draw calls / texture binds / net bytes: plain counters
//     (always 0 in headless M1 — the fields exist per FR-11.1; the
//     render and network subsystems arrive in M2/M3 and feed them
//     through addDrawCalls / addTextureBinds / addNetBytes)
//
// The remaining FR-11.1 counters are pulled COLD from their owners —
// no duplicated state, one source of truth each:
//
//   - per-system time histograms: World::systemTimingWindow(id)
//     (M1-SYS-03) — the report reads them per system (and the
//     frame graph's budget report, frame_budget.h — M1-PROF-02 —
//     reads the tick window itself through tickWindow())
//   - entity counts (total / alive): World::stats() (inUse /
//     totalCreated, M1-ECS-01)
//   - sim alloc count (sum of the pool accounting, target 0):
//     World::archetypeStats().totalReservations (M1-ECS-03 — the
//     pool-backed sim storage's reserved column blocks)
//
// snapshot() returns the profiler's own counters (no world access);
// snapshot(world) adds the world-pulled fields (worldAvailable is
// true; the no-arg form leaves them zero / false).
//
// ---------------------------------------------------------------------------
// Hot-path cost (PERF-003, DBG-004)
// ---------------------------------------------------------------------------
//
// Enabled (the default): two steady_clock reads per completed tick
// (GameLoop::runOneTick), two steady_clock reads per frame (Engine
// run loop), one O(1) ring write per tick and per frame — no
// allocation and no logging (PERF-003). Disabled (setEnabled(false),
// or the loop's Options::profiler is nullptr): one branch — the
// measurement's cost is the enabled instrumentation itself, bounded
// at 1% of a 10k-entity tick by the measured disabled-cost baseline
// (docs/benchmarks/baselines/m1-profiler-cost.md; CORE-001, DBG-004).
//
// The measured times are diagnostics (ARCH-009): they never enter
// authoritative simulation state, state hashes, or replays (wall-
// clock readings are platform-sensitive — the M1-SYS-03 precedent).
//
// ---------------------------------------------------------------------------
// Ownership, threading, determinism
// ---------------------------------------------------------------------------
//
// A Profiler is move-only (the GameLoop precedent): construction
// performs exactly two backing allocations (the two window storages —
// setup path, PERF-003); every later operation allocates nothing.
// A moved-from profiler is STOPPED: records are no-ops and snapshots
// return empty values (the GameLoop moved-out contract, no log).
// It has exactly one owner thread (CONC-001; PRD §10.2) and holds no
// world reference (the world data is pulled by argument, cold), so it
// may be released independently of the world.
//
// Determinism (ARCH-010): the counter and window contents are
// wall-clock-derived diagnostic state — never replay state.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "laige/budget_harness.h"  // Histogram, HistogramStats (M0-CORE-08)
#include "laige/result.h"          // Result, Status, ErrorCode

namespace laige {

class World;  // the World home is entity.h; the cold pull takes a reference

// The default tick-time rolling window capacity (CORE-005): 512
// samples ≈ 8.5 s of tick history at the default 60 Hz — long enough
// for a stable p99, small enough that the O(n log n) stats pass stays
// cold. Overridable per profiler (Options); the M1-SYS-03 per-system
// windows keep their own fixed capacity (kSystemTimingWindowSamples).
inline constexpr std::uint32_t kProfilerTickWindowSamples = 512;

// The default frame-time rolling window capacity (CORE-005): 256
// samples ≈ 4.3 s of frame history at the default 60 Hz.
inline constexpr std::uint32_t kProfilerFrameWindowSamples = 256;

// One profiler snapshot (a plain value; the CLI one-line summary, the
// report writers, the engine's per-run cache, and the M1-PROF-02 frame
// graph consume it). The counters are since-construction and never
// truncate (the histogram windows may roll — their `n` fields say so);
// the HistogramStats fields are NaN when the corresponding window is
// empty (check n — the M0-CORE-08 contract).
struct ProfilerStats {
  // Completed ticks recorded (== the tick-time samples recorded).
  std::uint64_t ticks{};
  // Frames recorded (== the frame-time samples recorded).
  std::uint64_t frames{};
  // Draw calls submitted (0 in headless M1 — the M2 render feed).
  std::uint64_t drawCalls{};
  // Texture binds (0 in headless M1 — the M2 render feed).
  std::uint64_t textureBinds{};
  // Network bytes (0 in headless M1 — the M3 network feed).
  std::uint64_t netBytes{};
  // The tick-time window stats (ms; NaN when n == 0).
  HistogramStats tickTimeMs{};
  // The frame-time window stats (ms; NaN when n == 0).
  HistogramStats frameTimeMs{};
  // World-pulled fields: snapshot(world) sets worldAvailable to true
  // and fills them; the no-arg snapshot() leaves them at zero / false.
  bool worldAvailable{};
  // Live entities right now (World::stats().inUse).
  std::uint32_t entitiesAlive{};
  // Entities created since world construction (totalCreated).
  std::uint32_t entitiesTotal{};
  // The declared scene budget (World::stats().capacity).
  std::uint32_t entityCapacity{};
  // The sim alloc count: the sum of the pool accounting since world
  // construction (M1-ECS-03 World::archetypeStats().totalReservations —
  // the pool-backed sim storage's reserved column blocks). Target 0
  // for the steady-state per-frame DELTA (FR-11.1; M1-ALLOC-01
  // asserts it per tick).
  std::uint64_t simAllocs{};
  // Registered systems (World::systemCount()).
  std::uint32_t systems{};
};

// The always-on profiler counters (M1-PROF-01). See the header
// preamble for the counter model, the hot-path cost, and the
// ownership contract.
class Profiler {
 public:
  // The typed profiler configuration (API-006): the rolling window
  // capacities (0 is legal — every record is dropped and stats() is
  // always empty, the M0-CORE-08 capacity-0 semantics) and the
  // enabled flag.
  struct Options {
    // The tick-time rolling window capacity (default
    // kProfilerTickWindowSamples).
    std::uint32_t tickWindowSamples{kProfilerTickWindowSamples};
    // The frame-time rolling window capacity (default
    // kProfilerFrameWindowSamples).
    std::uint32_t frameWindowSamples{kProfilerFrameWindowSamples};
    // The always-on counters on/off (default on). Disabled: every
    // record call is a no-op (one branch — DBG-004).
    bool enabled{true};
  };

  // Construct the profiler (setup path): exactly two backing
  // allocations (the two window storages). No failure mode — every
  // configuration is representable (a zero capacity is legal).
  explicit Profiler(Options options);

  // Move transfers the state; the source becomes a STOPPED profiler
  // (records are no-ops, snapshots return empty values — the GameLoop
  // moved-out precedent).
  Profiler(Profiler&& other) noexcept;
  Profiler& operator=(Profiler&& other) noexcept;
  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;

  // Record one completed tick's measured time (ms, from the caller's
  // TimeIt). A no-op when disabled or stopped. The caller measures
  // only the completed tick (the GameLoop runOneTick contract).
  // @budget O(1); no allocation (hot path).
  void recordTick(double ms) noexcept;  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured tick time never enters sim state, hashes, or replays (M1-PROF-01, ARCH-009)

  // Record one frame's measured time (ms, from the caller's TimeIt):
  // the frame's sim work plus the presentation refresh, excluding the
  // pacing sleep (the header preamble). A no-op when disabled or
  // stopped.
  // @budget O(1); no allocation (hot path).
  void recordFrame(double ms) noexcept;  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured frame time never enters sim state, hashes, or replays (M1-PROF-01, ARCH-009)

  // The M2/M3 feeds for the FR-11.1 render/network counters (always
  // 0 in headless M1 — the fields exist now; the render and network
  // subsystems arrive later and call these). A no-op when disabled or
  // stopped.
  // @budget O(1); no allocation.
  void addDrawCalls(std::uint64_t count) noexcept;
  void addTextureBinds(std::uint64_t count) noexcept;
  void addNetBytes(std::uint64_t count) noexcept;

  // The tick-time stats over the stored window (the M0-CORE-08
  // stats() — cold path). NaN when the window is empty (check n).
  // @budget O(n log n) cold path; no allocation.
  [[nodiscard]] HistogramStats tickTime() const noexcept;

  // The frame-time stats over the stored window (cold path, as above).
  // @budget O(n log n) cold path; no allocation.
  [[nodiscard]] HistogramStats frameTime() const noexcept;

  // The tick-time window itself (the M1-PROF-02 frame graph's budget
  // checks read it cold through the M0-CORE-08 budgetCheck — the
  // header preamble's "the per-frame budget report over them is
  // M1-PROF-02"). Non-owning const view into the profiler's fixed
  // window (valid until the profiler is destroyed or moved — the
  // profiler is owned by the Engine for its whole lifetime).
  // @budget O(1); no allocation.
  [[nodiscard]] const Histogram& tickWindow() const noexcept;

  // The snapshot of the profiler's own counters (no world access).
  // Cold path (the two stats passes); no side effects.
  // @budget O(n log n) cold path; no allocation.
  [[nodiscard]] ProfilerStats snapshot() const noexcept;

  // The snapshot plus the world-pulled fields (the entity counts, the
  // sim alloc count, the system count — pulled from World::stats /
  // World::archetypeStats / World::systemCount; the header preamble).
  // Cold path; no world mutation.
  // @budget O(n log n) cold path; no allocation, no world mutation.
  [[nodiscard]] ProfilerStats snapshot(const World& world) const noexcept;

  // The enabled flag (the GameLoop and the Engine read it to decide
  // whether to measure at all — the hot-path branch).
  [[nodiscard]] bool enabled() const noexcept;

  // Toggle the always-on counters at runtime (the DBG-002 profile
  // switch: on is the "Always" profile, off turns the counters off).
  // No side effects: already-recorded state is preserved.
  void setEnabled(bool on) noexcept;

 private:
  Histogram tickWindow_;
  Histogram frameWindow_;
  // The since-construction sample counts are the windows' own
  // `totalRecorded()` (the windows roll, but that counter does not
  // truncate — the ProfilerStats ticks/frames fields, M0-CORE-08).
  // The FR-11.1 render/network counters (0 in headless M1).
  std::uint64_t drawCalls_{};
  std::uint64_t textureBinds_{};
  std::uint64_t netBytes_{};
  bool enabled_{};
  // Cleared on move-out: a stopped profiler records nothing.
  bool active_{true};
};

// The report format (M1-PROF-01 file export): the human-readable
// greppable text form and the machine-readable JSON form.
enum class ProfileFormat : std::uint8_t {
  Text = 0,
  Json = 1,
};

// The CLI one-line summary (FR-11.1 "exposed in ... the CLI"): the
// run's counters plus the two window stat lines and the world-pulled
// fields, in one machine-greppable line (docs/api/engine.md, the
// laige-run section). Cold path (the two stats passes); allocates
// (a report string — reporting is never a hot path).
// @budget O(n log n) cold path; allocates.
[[nodiscard]] std::string formatProfileSummaryLine(const ProfilerStats& stats);

// The full report in the greppable text form (one section per line —
// the counters, the two windows, the world fields, and one line per
// registered system with its M1-SYS-03 window stats). Cold path;
// allocates. `world` is required (the per-system and world-pulled
// sections) — a moved-from / released world is not a legal argument.
// @budget O(systemCount × n log n) cold path; allocates.
[[nodiscard]] std::string formatProfileText(const Profiler& profiler,
                                             const World& world);

// The full report in the JSON form (version 1; schema documented in
// docs/api/profiler.md). Cold path; allocates.
// @budget O(systemCount × n log n) cold path; allocates.
[[nodiscard]] std::string formatProfileJson(const Profiler& profiler,
                                            const World& world);

// Write the report to `path` (truncating; the file appears only when
// the write fully succeeds — no partial report on failure). Cold
// path (the format pass plus one file write). Every failure is a
// Result: unreadable/unwritable path -> IoError (CORE-008: never
// silent). Returns the bytes written on success.
// @budget O(systemCount × n log n) cold path; allocates; one file write.
[[nodiscard]] Result<std::uint64_t, ErrorCode> writeProfile(
    const Profiler& profiler, const World& world, std::string_view path,
    ProfileFormat format);

}  // namespace laige
