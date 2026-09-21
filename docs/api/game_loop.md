# Fixed-timestep game loop core (`GameLoop`, M1-LOOP-01)

The M1 game loop's fixed-timestep core (M1-LOOP-01; PRD FR-1.1,
ARCH-002, PRD §10.2/§10.3; AGENTS CORE-005/008, PERF-002/003,
ARCH-009/010): advances the simulation in **integer ticks** at a
fixed, validated rate (default 60 Hz, 20–120 Hz), decoupled from the
presentation frame cadence, and runs the world's scheduled systems
once per tick. Public header:
`src/laige-sim/include/laige/sim/game_loop.h` (`GameLoop`,
`GameLoop::Options`, `GameLoopStats`, the range constants, the full
contract); implementation: `src/laige-sim/game_loop.cpp`. Unit suite:
`ctest -R game_loop` (`tests/laige-sim/game_loop_tests.cpp`).

The loop is the first consumer of the M1 system framework: it owns
the `beginFrame()`/`runSystems()` cadence the scheduler docs sketch
("M1-LOOP-01 owns this" — `include/laige/sim/system.h`):

```cpp
World world = ...;                       // M1-ECS
world.registerSystem(...);              // M1-SYS-01
SystemSchedule sched;
world.scheduleSystems(sched);           // M1-SYS-02

GameLoop loop = GameLoop::create(world, sched, opts).value();
while (running) {
  if (!loop.frame().ok()) { /* setup error: recreate the loop */ }
  // presentation state for this frame: loop.currentTick(), ...
}
```

## The two cadences (FR-1.1, ARCH-002)

The loop separates two clocks:

- **Presentation frames** — the caller invokes `frame()` once per
  presentation frame (the headless engine run loop from M1-HEAD-01,
  the windowed frame pipeline from M2). A frame is a presentation
  boundary; it carries no simulation time.
- **Simulation ticks** — the simulation advances in **integer ticks**
  at `Options::tickRateHz`: one tick per `1/tickRateHz` seconds, all
  simulation time in integer ticks (PRD §10.3). The render frame rate
  never enters the simulation (ARCH-002): frames faster than the tick
  rate run zero ticks, slower frames run several (catch-up).

## The exact due computation (no floating point, ARCH-010)

The accumulator is `due(now) − ticksRun`, where

```
due(now) = floor(elapsedNs × tickRateHz / 10⁹)
```

is evaluated in **pure integer arithmetic** —
`(elapsedNs / 10⁹) × rate + (elapsedNs mod 10⁹) × rate / 10⁹` — so
no intermediate product overflows and no rounding drift accumulates.
The remainder (the unrun due ticks) needs no stored state: it is
re-derived from the clock on every frame. A synthetic 10 s clock at
60 Hz yields **exactly 600 ticks** (the `ExactTicksOverTenSeconds`
test; a floating ms accumulator floors to 599 over the same
sequence).

## Configuration and validation (FR-1.1, API-006)

| Field | Range | Default | Reject (all: `InvalidArgument` + one rate-limited warn) |
|---|---|---|---|
| `tickRateHz` | 20–120 Hz (`kMinTickRateHz`–`kMaxTickRateHz`) | `kDefaultTickRateHz` (60) | `loop/tick_rate_invalid` (field `tick_rate_hz`) |
| `maxCatchUpTicks` | ≥ 1 | `kDefaultMaxCatchUpTicks` (5) | `loop/catchup_invalid` (field `max_catch_up`) |
| `onTick` / `onTickContext` | a `noexcept` tick callback + context (see below) | `nullptr` / `nullptr` | — (no validation: the callback's contract is the caller's) |
| `profiler` | a non-owning `laige::Profiler*` (M1-PROF-01; see the Profiler feed section) | `nullptr` | — (no validation: the profiler's lifetime is the caller's) |

The clock source is `Options::nowNs` — a function returning
nanoseconds on a monotonic epoch time base; `nullptr` uses the
headless monotonic clock (`steady_clock`). A synthetic clock (tests)
or the M2 windowed clock (M2-GL-02) supplies its own.

`maxCatchUpTicks` bounds **per-frame work**, not rate: at 60 Hz one
catch-up frame may run at most 5 ticks (~83 ms of simulation time).
A healthy 30 Hz machine runs 2 ticks per frame — no drops; a drop
fires only when a frame exceeds `(maxCatchUpTicks + 1)` ticks of
simulation time, i.e. a real overload.

## Overload behavior: drops, never silent (FR-12.3, PERF-008)

When a frame's due-tick demand (`want = due − ticksRun`) exceeds the
max catch-up limit, the frame runs exactly `maxCatchUpTicks` ticks
and the rest are **dropped** — exactly `want − maxCatchUpTicks`
of them — counted and logged (the `OverloadDropsExactlyTheDocumentedAmountAndLogsOnce`
test pins the amounts: demands of 10/18/26 with a cap of 2 drop
8/16/24, total 48):

| Condition | Event | Severity |
|---|---|---|
| `want > maxCatchUpTicks` (one frame) | `loop/tick_dropped` | Warn |

Event fields (structured, never message text — the NFR-13.3
5-field message grammar is build-stable): `dropped` (this frame's
amount), `total_dropped` (since construction), `max_catch_up`,
`tick_rate_hz`. The event is rate-limited per (subsystem, event,
severity) by the logging facade (LOG-004): a sustained overload logs
**once per episode** (one event per rate window) plus a
`rate_limited` summary at shutdown — never a log storm. The loop's
counters keep counting every drop even while the event is suppressed
(observable state, never silent).

The frame work stays **bounded** by `maxCatchUpTicks` (PERF-002: no
unbounded loop; PERF-008: backpressure). After a drop, the unrun
demand is the sub-tick remainder plus the next frame's time, so the
accumulator never grows unboundedly: a permanently overloaded machine
drops a bounded number of ticks per frame and the degradation is
always visible.

## beginFrame wiring (M1-ECS-06 guardrails)

`frame()` drives `World::beginFrame()` exactly **once per frame** —
before the frame's ticks — and `World::runSystems(schedule)` once per
tick (the entity.h contract: "the owning loop drives it once per
frame"). The G-R3/G-R4 per-frame windows are therefore per
**presentation frame**: a catch-up frame running N ticks counts all N
ticks of churn against one per-frame budget (the guardrail flags the
heavier work — the documented overload signal). Before this loop
existed, the per-tick `beginFrame()` pattern in the scheduler docs was
the manual form; it remains the test form (one frame per tick).

## The per-tick presentation hook (M1-LOOP-02)

`Options::onTick` is a `void (*)(void* context, World&,
std::uint64_t tick) noexcept` callback fired **after every completed
tick** — after `runSystems` for that tick succeeded, with the new
tick number (`currentTick()` already incremented). A failed tick does
**not** fire it (the tick is not counted and the state it would have
observed never happened — a stale schedule's tick, for example).

This is the M1 presentation-state wiring seam (M1-LOOP-02): the
`PresentationSnapshot` (presentation.md) refreshes its `prev`/`curr`
pair per completed tick by wrapping its `onTick` in a static thunk
behind `onTickContext` — the headless engine (M1-HEAD-01) does exactly
this. The callback must not allocate or block (it runs inside the
tick's budget, PERF-002/003), and its ownership of the context is
the caller's (the context must outlive the loop). `nullptr` (the
default) fires nothing — the loop is unchanged.

`GameLoop::startReferenceNs()` exposes the loop's clock reading at
its first frame — the anchor base the presentation snapshot's alpha
is computed against (presentation.md, "The alpha contract"). Read it
after the loop's first frame.

## Failure behavior (CORE-008)

`frame()` returns the `runSystems` Status:

- A stale or malformed schedule (a system registered after
  scheduling, a hand-built schedule) is `InvalidArgument` — raised by
  `runSystems` (`system/schedule_stale` / `system/schedule_invalid`);
  the loop adds no event of its own. A failed tick is **not counted**
  in `currentTick()` (its system phase did not complete), and no
  system runs in a failed frame (validation precedes dispatch — the
  world is untouched).
- The loop is otherwise untouched: the tick count freezes and each
  later `frame()` re-derives the demand from the clock and fails the
  same way (rate-limited) until the caller recreates the loop with a
  recomputed schedule.
- A **moved-from loop is stopped**: `frame()` returns `InvalidArgument`
  without touching the world and without logging (the moved-from-world
  pure-failure precedent, entity.h). Move transfers the tick state —
  the source becomes a valid but stopped loop (the `World` move
  precedent: the source is left in a well-defined state).

## Determinism scope (ARCH-009/010)

The tick sequence — `currentTick()` after any frame sequence — is a
**pure function of (the clock readings, tickRateHz, maxCatchUpTicks)**:
integer arithmetic only, no floating point, no randomness, no
platform state. Two runs over the same clock sequence produce
bit-identical tick counts (the tick counter is replay state —
M1-DET-01/02 include it in the state hash). The clock readings
themselves are wall-clock facts (platform-sensitive, ARCH-009): with
the default steady clock, cross-platform identity over real time is
not promised; the windowed clock (M2-GL-02) and the replay runner
(M1-DET-03) supply the canonical time base. `frames` /
`droppedTicks` / `droppedFrames` are presentation/diagnostic state
(the frame cadence is the caller's, not the simulation's) — never
authoritative.

## Profiler feed (M1-PROF-01)

`GameLoop::stats()` returns the since-construction `GameLoopStats`
snapshot (`frames`, `ticks`, `droppedTicks`, `droppedFrames`): a pure
O(1) query, no allocation (the `World::stats()` /
`SystemTimingStats` precedent).

When `Options::profiler` is set (non-owning — the profiler must
outlive the loop, the `onTickContext` lifetime contract),
`runOneTick` times each tick's body (the frame's `beginFrame` + one
`runSystems` dispatch) with the M0-CORE-08 `TimeIt` and hands the
measured ms to `Profiler::recordTick` — **on success only**: a failed
tick is not counted and not recorded (the tick-count contract,
[api/profiler.md](profiler.md)). The frame-time feed is the engine's
(engine.h "The profiler"); the loop records ticks only.

- **Enabled profiler (attached):** two `steady_clock` reads per
  completed tick + one O(1) ring write — no allocation. The measured
  enabled cost is bounded at 1% of a 10k-entity tick
  ([baselines/m1-profiler-cost.md](../benchmarks/baselines/m1-profiler-cost.md),
  CORE-001, DBG-004).
- **Profiler null or disabled:** one branch per tick, nothing else
  (DBG-004: disabled instrumentation costs a branch).
- **Determinism:** the measured sample is a wall-clock diagnostic
  (ARCH-009) — it never enters the tick count, the state hash, or a
  replay.

## Performance (DOC-004)

- **Per frame (hot path):** one clock read, a few integer ops (the
  exact due computation), and up to `maxCatchUpTicks` `runSystems`
  dispatches — **bounded by the config** (PERF-002: no unbounded
  loop). No allocation, no logging on the success path (PERF-003,
  LOG-003); the `HealthyFramesAllocateNothing` test asserts
  `allocs == 0` over 300 catch-up frames (600 ticks, zero drops).
- **Per tick:** the loop bookkeeping is a few integer ops (the due
  computation, the cap compare, the counter bump) — no allocation, no
  lock, no I/O — negligible against the 3 ms `sim_tick_avg` budget
  (PRD §8.1; `budgets.json`) next to the `runSystems` dispatch cost,
  which M1-SYS-03 measures. The `onTick` hook, when set, adds one
  indirect call per completed tick (the snapshot's own cost is
  presentation.md's — bounded, allocation-free). A profiler attached
  and enabled (M1-PROF-01) adds two `steady_clock` reads + one O(1)
  ring write per completed tick (the `runOneTick` `TimeIt`) — no
  allocation; the measured enabled cost is bounded at 1% of a
  10k-entity tick (the m1-profiler-cost baseline, CORE-001/DBG-004).
  Profiler null or disabled: one branch per tick, nothing else.
- **Cold path (overload):** one rate-limited `tick_dropped` warn with
  field construction — only while a frame exceeds the catch-up bound.
- **Complexity:** `frame()` is O(maxCatchUpTicks × per-tick system
  work); `currentTick()`/`tickRateHz()`/`maxCatchUpTicks()`/`stats()`
  are O(1). No operation scales with entities or components beyond
  the systems' own declared cost (PERF-007).

## Threading (CONC-001, PRD §10.2)

The loop has exactly **one owner thread**: `frame()` and the queries
run on the world's single owner thread, strictly interleaved with the
systems' execution (API-004: the system phase). The non-owning
world/schedule views point at single-owner state and are never
dereferenced off-thread.

## Misuse warnings

- **One live loop per world.** Two live loops on one world double-tick
  the simulation (two `runSystems` per tick, two `beginFrame()` per
  frame) — an API-004 violation the engine cannot detect; the
  moved-from stop is what makes the factory's move safe.
- **The clock source must be monotonic** (`steady_clock` is by
  definition; a synthetic test clock advances, never rewinds). A
  backward reading **below the start reference** asserts in debug
  builds and clamps to the start reference in release (the frame
  contributes no time — never undefined behavior).
- **The world, the schedule, and (when set) the profiler must outlive
  the loop** (non-owning views). A profiler that was disabled or
  moved out mid-run is safe (records are no-ops — the Profiler's
  stopped contract, [api/profiler.md](profiler.md)); a dangling
  profiler pointer is a lifetime bug the engine's ownership rules
  exist to prevent (the engine creates the profiler in
  `Engine::create` and releases it in the ordered shutdown AFTER the
  loop — engine.h). Recomputing the schedule after a registration
  change without recreating the loop leaves the old schedule stale —
  `frame()` then fails every frame (`system/schedule_stale`).
- **`maxCatchUpTicks` is not a rate knob**: it bounds per-frame work;
  it cannot make the simulation run faster.
- **A dropped tick is a lost simulation step** (documented overload
  degradation, FR-12.3): logged and counted, but the game continues
  without it — the fix is the tick rate, the per-tick work, or the
  catch-up bound (the event's `{fix}` field).
