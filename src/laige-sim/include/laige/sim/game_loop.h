// laige-sim fixed-timestep game loop core (M1-LOOP-01).
//
// FR-1.1 (fixed-timestep simulation — default 60 Hz, configurable
// 20–120 Hz — decoupled from the presentation cadence); ARCH-002
// (the simulation MUST NOT depend on render frame rate); PRD §10.2
// (simulation is single-threaded). This header ships the fixed-
// timestep core of the M1 game loop — the accumulator loop that
// advances the simulation in integer ticks and runs the world's
// scheduled systems once per tick:
//
//   GameLoop        The accumulator loop: owns the tick state (the
//                   completed tick count, the drop counters) and
//                   drives World::beginFrame + World::runSystems.
//   GameLoop::Options
//                   The typed configuration: the tick rate (20–120 Hz,
//                   validated) and the max catch-up ticks per frame.
//   GameLoopStats   The since-construction snapshot (frames, ticks,
//                   dropped ticks, dropped frames) for the profiler
//                   (M1-PROF-01).
//   kMinTickRateHz / kDefaultTickRateHz / kMaxTickRateHz
//                   The documented tick-rate range (FR-1.1).
//   kDefaultMaxCatchUpTicks
//                   The documented max catch-up default.
//
// ---------------------------------------------------------------------------
// The accumulator contract (FR-1.1, ARCH-002)
// ---------------------------------------------------------------------------
//
// The loop decouples two cadences:
//
//   - Presentation frames — the caller invokes frame() once per
//     presentation frame (the headless engine run loop from M1-HEAD-01,
//     the windowed frame pipeline from M2 on). A frame is a
//     presentation boundary; it carries no simulation time.
//   - Simulation ticks — the simulation advances in INTEGER ticks at
//     a fixed rate (Options::tickRateHz): one tick per 1/tickRateHz
//     seconds, all simulation time in integer ticks (PRD §10.3).
//
// The accumulator is the difference between the ticks that are DUE
// at the current clock reading and the ticks already run — computed
// EXACTLY, with no floating point and no rounding drift:
//
//   due(now) = floor(elapsedNs × tickRateHz / 10⁹)
//
// evaluated in pure integer arithmetic as
//
//   (elapsedNs / 10⁹) × rate + (elapsedNs mod 10⁹) × rate / 10⁹
//
// (the seconds/sub-seconds split keeps every intermediate product
// overflow-free). Each frame() runs min(due − ticksRun,
// maxCatchUpTicks) ticks — the classic fixed-timestep accumulator:
// one tick per frame at a healthy cadence, several ticks in a catch-
// up frame, zero ticks in a frame faster than the tick rate. The
// remainder (due − ticksRun − toRun) carries over automatically — it
// is re-derived from the clock on the next frame, so no remainder
// state is stored (no floating accumulator to drift; ARCH-010).
//
// The first frame() call establishes the start reference and runs
// zero ticks; every later frame advances the simulation.
//
// ---------------------------------------------------------------------------
// Configuration and validation (FR-1.1, API-006/008)
// ---------------------------------------------------------------------------
//
// GameLoop::create(world, schedule, options) validates the typed
// configuration (normative order, first failure wins; every failure
// is one rate-limited structured warn — subsystem "loop" — plus
// InvalidArgument, FR-12.3/CORE-008: never silent):
//
//   tickRateHz < kMinTickRateHz (20) or
//   tickRateHz > kMaxTickRateHz (120)
//                                 -> InvalidArgument + warn
//                                      (loop/tick_rate_invalid)
//   maxCatchUpTicks == 0         -> InvalidArgument + warn
//                                      (loop/catchup_invalid) — a zero
//                                      limit would never run a tick
//
// The loop holds NON-OWNING views of the world and the schedule:
// both must outlive the loop (the engine object owns all three —
// M1-HEAD-01). One live loop per world (misuse warning below).
//
// ---------------------------------------------------------------------------
// Overload behavior (the drop path, FR-12.3: never silent)
// ---------------------------------------------------------------------------
//
// When a frame's due-tick demand exceeds the max catch-up limit, the
// frame runs exactly maxCatchUpTicks ticks and the remaining due
// ticks are DROPPED — never run silently later in the same frame,
// never silently swallowed:
//
//   dropped = want − maxCatchUpTicks        (want = due − ticksRun)
//
// One structured warn per overload frame — loop/tick_dropped (the
// NFR-13.3 5-field message grammar, build-stable; the dynamic values
// are structured fields) — carries the per-frame drop amount and the
// running total, and is rate-limited per (subsystem, event, severity)
// by the logging facade (LOG-004): a sustained overload logs once per
// rate window plus a rate_limited summary, never a log storm. The
// loop's counters (GameLoopStats: droppedTicks, droppedFrames) keep
// counting every drop even when the event is suppressed — the state
// is observable, never silent.
//
// The frame work stays BOUNDED by maxCatchUpTicks (PERF-002/008): a
// permanently overloaded machine drops ticks every frame (one rate-
// limited event per window) and the accumulator never grows unbound
// — after each drop the unrun demand is the sub-tick remainder
// (due mod the tick time) plus the next frame's own time.
//
// ---------------------------------------------------------------------------
// beginFrame wiring (M1-ECS-06 guardrails)
// ---------------------------------------------------------------------------
//
// frame() drives World::beginFrame() exactly ONCE per frame — before
// the frame's ticks — and World::runSystems(schedule) once per tick
// (entity.h: "the owning loop drives it once per frame"). The G-R3/
// G-R4 per-frame windows are therefore per PRESENTATION frame: a
// catch-up frame running N ticks counts all N ticks of churn against
// one per-frame budget (the guardrail flags the heavier work — the
// documented overload signal, entity.h "Guardrails"). Before this
// loop existed, the per-tick beginFrame() pattern in the scheduler
// docs was the manual form; it remains the test form (one frame per
// tick).
//
// ---------------------------------------------------------------------------
// Per-tick presentation hook (M1-LOOP-02)
// ---------------------------------------------------------------------------
//
// Options::onTick (a plain function pointer — no std::function,
// PERF-006) fires ONCE per COMPLETED tick, after the tick's system
// phase, as onTick(context, world, tick) with the completed tick
// number (1-based, == currentTick() after the call). A failed tick
// is not counted and does NOT fire the hook (the state it would
// have observed never completed — the preamble "Failure behavior").
// The hook is the M1-LOOP-02 PresentationSnapshot's per-tick refresh
// driver (presentation.h: the engine wires the snapshot's onTick
// through this callback); M1-HEAD-01 owns the wiring. nullptr
// (the default) is the M1-LOOP-01 behavior — no hook.
//
// The callback runs inside frame(), on the loop's owner thread, in
// the system phase (API-004), strictly between two ticks of the
// frame (world mutations there are legal — no iteration is active):
// it must be bounded and allocation-free on the success path (the
// snapshot's onTick is the reference contract — one bounded
// archetype scan, no allocation, no logging, PERF-002/003).
//
// ---------------------------------------------------------------------------
// Failure behavior (CORE-008)
// ---------------------------------------------------------------------------
//
// frame() returns the runSystems Status: a stale or malformed
// schedule (a system registered after scheduling, a hand-built
// schedule) is InvalidArgument — the failure is raised by runSystems
// (system/schedule_stale, system/schedule_invalid; the loop adds no
// event of its own). A failed tick is NOT counted in currentTick():
// the tick's system phase did not complete. The loop is otherwise
// untouched — the tick count freezes, the next frame() re-derives the
// demand from the clock and fails the same way (rate-limited), until
// the caller recreates the loop with a recomputed schedule. The
// failure path never runs a system (runSystems validates before
// dispatch), so the world is not mutated by a failed frame.
//
// A moved-from loop is a valid but STOPPED loop: frame() returns
// InvalidArgument without touching the world and without logging (the
// moved-from-world pure-failure precedent, entity.h). Move transfers
// the tick state (the factory's Result move, the World move
// precedent: the source is left in a well-defined state, never
// usable for driving).
//
// ---------------------------------------------------------------------------
// Determinism scope (ARCH-009/ARCH-010)
// ---------------------------------------------------------------------------
//
// The tick sequence — currentTick() after any frame sequence — is a
// pure function of (the clock readings, tickRateHz, maxCatchUpTicks):
// integer arithmetic only, no floating point, no randomness, no
// platform state. Two runs (two processes, two builds) over the same
// clock sequence produce bit-identical tick counts (replay state from
// M1-DET-01/02: the tick counter is part of the state hash). The
// clock READINGS themselves are wall-clock facts (platform-
// sensitive — ARCH-009): with the default steady clock, cross-
// platform identity of tick counts over real time is not promised;
// the windowed clock (M2-GL-02) and the replay runner (M1-DET-03)
// supply the canonical time base. frames/droppedTicks/droppedFrames
// are presentation/diagnostic state (the frame cadence is the
// caller's, not the simulation's) — never authoritative.
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003)
// ---------------------------------------------------------------------------
//
// Per frame (the hot path): one clock read (the injected ClockFn or
// the steady clock), a few integer ops (the due computation, the
// bounded run loop), and up to maxCatchUpTicks runSystems dispatches
// — BOUNDED by the config (PERF-002: no unbounded loop). No
// allocation and no logging on the success path (PERF-003, LOG-003;
// the HealthyFramesAllocateNothing test asserts it). The loop
// bookkeeping per tick is a few integer ops — negligible against the
// 3 ms sim_tick_avg budget (PRD §8.1) next to the runSystems dispatch
// cost, which M1-SYS-03 measures. The drop path is cold (an overload
// episode): one rate-limited warn with field construction.
//
// ---------------------------------------------------------------------------
// Threading
// ---------------------------------------------------------------------------
//
// The loop has exactly one owner thread (CONC-001; PRD §10.2):
// frame() and the queries run on the world's single owner thread,
// strictly interleaved with the systems' execution (API-004: the
// system phase). The non-owning world/schedule views are never
// dereferenced off-thread (the views point at single-owner state).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - One live loop per world. Two live loops on one world double-
//     tick the simulation (two runSystems per tick, two beginFrame()
//     per frame) — an API-004 violation the engine cannot detect;
//     the moved-from stop is what makes the factory's move safe.
//   - The clock source must be MONOTONIC (steady_clock is by
//     definition; a synthetic test clock must be advanced, never
//     rewound). A backward reading asserts in debug builds and
//     clamps to the start reference in release (the frame
//     contributes no time) — never undefined behavior.
//   - The world and the schedule must outlive the loop (non-owning
//     views). Recomputing the schedule after a registration change
//     without recreating the loop leaves the old schedule stale —
//     frame() then fails every frame (system/schedule_stale).
//   - maxCatchUpTicks is a bound on PER-FRAME work, not a rate
//     knob: it cannot make the simulation run faster; it only bounds
//     how many ticks one frame may run.
//   - A dropped tick is a lost simulation step (documented overload
//     degradation, FR-12.3): it is logged and counted, but the game
//     continues without it — the fix is the tick rate, the per-tick
//     work, or the catch-up bound (the event's {fix} field).
//   - The onTick callback is loop-owned for the loop's lifetime:
//     its context must outlive the loop (the non-owning-view
//     precedent). A callback that blocks or allocates breaks the
//     frame budget (PERF-002/003) — the snapshot's onTick is the
//     reference contract.

#pragma once

#include <cstdint>

#include "laige/sim/entity.h"  // World, SystemSchedule (via system.h), Result

namespace laige {

// The supported tick-rate range (FR-1.1: default 60 Hz, configurable
// 20–120 Hz). Named constants (CORE-005): a rate outside this range
// is rejected at loop construction.
inline constexpr std::uint32_t kMinTickRateHz = 20;

// The default tick rate (FR-1.1).
inline constexpr std::uint32_t kDefaultTickRateHz = 60;

inline constexpr std::uint32_t kMaxTickRateHz = 120;

// The default max catch-up ticks per frame (CORE-005): at the default
// 60 Hz, one catch-up frame may run at most 5 ticks (~83 ms of
// simulation time) before the frame's demand is dropped and logged.
// A healthy machine runs 1 tick per frame (frames slower than the
// tick rate run 2–3, still under the bound); a drop fires only when
// a frame exceeds (maxCatchUpTicks + 1) ticks of simulation time —
// a real overload, not a cadence difference. Raising it is typed
// configuration (an ADR if the engine default changes), not a knob.
inline constexpr std::uint32_t kDefaultMaxCatchUpTicks = 5;

// The since-construction accounting snapshot of one GameLoop
// (M1-PROF-01 feed; a plain value, the EntityStats/
// SystemTimingStats precedent):
//
//   frames         frame() calls since construction (including the
//                  first, start-establishing call)
//   ticks          completed ticks (== currentTick())
//   droppedTicks   ticks dropped since construction (the sum of the
//                  per-frame drop amounts)
//   droppedFrames  frames in which a drop occurred
struct GameLoopStats {
  std::uint64_t frames{};
  std::uint64_t ticks{};
  std::uint64_t droppedTicks{};
  std::uint64_t droppedFrames{};
};

// The fixed-timestep accumulator loop (M1-LOOP-01): see the header
// preamble for the accumulator, configuration, overload, beginFrame,
// failure, determinism, performance, and threading contracts.
class GameLoop {
 public:
  // The typed loop configuration (API-006): the tick rate (20–120 Hz,
  // validated at construction), the max catch-up ticks per frame
  // (>= 1, validated), and the clock source.
  struct Options {
    // The simulation tick rate in HERTZ (FR-1.1: 20–120 validated;
    // default kDefaultTickRateHz).
    std::uint32_t tickRateHz{kDefaultTickRateHz};
    // The max ticks one frame may run before its due-tick demand is
    // dropped (and logged): >= 1 (default kDefaultMaxCatchUpTicks).
    std::uint32_t maxCatchUpTicks{kDefaultMaxCatchUpTicks};
    // The clock source: nanoseconds since a fixed monotonic epoch
    // (the same time base as the default clock below). nullptr uses
    // the headless monotonic clock (steady_clock); a test or the M2
    // windowed clock supplies its own (injectable for tests — the
    // LoggerOptions::ClockFn precedent).
    using ClockFn = std::int64_t (*)();
    ClockFn nowNs{nullptr};
    // Optional per-completed-tick callback (M1-LOOP-02; see the
    // preamble "Per-tick presentation hook"): fires after every
    // completed tick as onTick(context, world, tick). nullptr
    // (default): no hook (the M1-LOOP-01 behavior). Plain function
    // pointer — no std::function (PERF-006); the callback must be
    // bounded and allocation-free (the snapshot's onTick is the
    // reference contract).
    using TickFn = void (*)(void* context, World& world,
                             std::uint64_t tick) noexcept;
    TickFn onTick{nullptr};
    // The onTick callback's user context (opaque; must outlive the
    // loop — the engine passes the PresentationSnapshot, M1-HEAD-01).
    void* onTickContext{nullptr};
  };

  // Construct the loop on `world` running `schedule` (setup phase,
  // after World::scheduleSystems — the schedule must describe the
  // world's CURRENT registry, and both must outlive the loop).
  // O(1); no allocation (the loop state is fixed scalars).
  //
  //   tickRateHz outside 20–120     -> InvalidArgument + warn
  //                                      (loop/tick_rate_invalid)
  //   maxCatchUpTicks == 0          -> InvalidArgument + warn
  //                                      (loop/catchup_invalid)
  [[nodiscard]] static Result<GameLoop, ErrorCode>
  create(World& world, const SystemSchedule& schedule,
         Options options) noexcept;

  // Advance one presentation frame (the hot path; see the preamble
  // "Performance"): read the clock, run the frame's due ticks (up to
  // maxCatchUpTicks), drop the excess with a rate-limited warn.
  //
  //   moved-from loop               -> InvalidArgument (no log, no
  //                                      world access — the stopped
  //                                      state)
  //   a system phase failure        -> the runSystems Status (the
  //                                      tick is not counted; the
  //                                      world is not mutated)
  //   success                       -> ok
  // O(maxCatchUpTicks × the systems' own work) — BOUNDED (PERF-002);
  // no allocation, no logging on the success path.
  // @budget O(maxCatchUpTicks × per-tick system work); bounded, no allocation.
  [[nodiscard]] Status frame() noexcept;

  // The number of completed ticks (0 before the first; the first
  // tick to complete is tick 1). O(1), no side effects.
  [[nodiscard]] std::uint64_t currentTick() const noexcept;

  // The clock reading that established the start reference (0
  // before the first frame) — the time-base origin of the due
  // computation (the preamble "The exact due computation"). The
  // M1-LOOP-02 PresentationSnapshot takes this as its start
  // reference (presentation.h: the tick anchors A(T) = startNs +
  // T × 10⁹ / rate must use the loop's own time base). O(1), no
  // side effects.
  [[nodiscard]] std::int64_t startReferenceNs() const noexcept;

  // The configured tick rate (Hz). O(1), no side effects.
  [[nodiscard]] std::uint32_t tickRateHz() const noexcept;

  // The configured max catch-up ticks per frame. O(1), no side
  // effects.
  [[nodiscard]] std::uint32_t maxCatchUpTicks() const noexcept;

  // The since-construction accounting snapshot (GameLoopStats). O(1),
  // no allocation, no side effects (a pure query, the
  // World::stats() precedent).
  [[nodiscard]] GameLoopStats stats() const noexcept;

  // Move transfers the tick state; the source becomes a valid but
  // STOPPED loop (frame() returns InvalidArgument, no log — see the
  // preamble "Failure behavior"; the World moved-from precedent: the
  // source is left in a well-defined state).
  GameLoop(GameLoop&& other) noexcept;
  GameLoop& operator=(GameLoop&& other) noexcept;
  GameLoop(const GameLoop&) = delete;
  GameLoop& operator=(const GameLoop&) = delete;

 private:
  // The factory path (create): the configuration is already
  // validated (API-008: an invalid configuration is unrepresentable).
  GameLoop(World& world, const SystemSchedule& schedule,
           Options options) noexcept;

  // One simulation tick: the frame's beginFrame() (once per frame —
  // see the preamble "beginFrame wiring") plus one runSystems
  // dispatch. A successful tick is counted and (if configured,
  // M1-LOOP-02) fires the Options::onTick hook after the system
  // phase; a failed tick is neither — the hook observes only
  // completed ticks.
  [[nodiscard]] Status runOneTick() noexcept;

  // Non-owning views (the world and the schedule outlive the loop).
  World* world_;
  const SystemSchedule* schedule_;
  Options options_;
  // Cleared on move-out: a stopped loop drives nothing.
  bool valid_{true};
  // True after the first frame() established the start reference.
  bool started_{false};
  // The clock reading of the first frame (the time base origin).
  std::int64_t startNs_{0};
  // Since-construction counters (GameLoopStats feed).
  std::uint64_t frames_{0};
  std::uint64_t ticks_{0};
  std::uint64_t droppedTicks_{0};
  std::uint64_t droppedFrames_{0};
};

}  // namespace laige
