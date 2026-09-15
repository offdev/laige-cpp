# Headless engine run (`Engine`, M1-HEAD-01)

The M1 headless engine object (M1-HEAD-01; PRD FR-1.6, ARCH-003,
ARCH-009, ARCH-010, CONC-006, PRD §10.2; AGENTS CORE-002/005/008,
PERF-002/003, LOG-003): wires **config → world → systems → loop**
into one owned object whose `run_headless(maxTicks)` starts the
simulation, ticks it in real time at the configured rate, and shuts
it down cleanly. It is the first full-stack surface of the engine and
the CI smoke-test target (`laige-run --headless`, this repo's `tools/run`).

Public header: `src/laige-sim/include/laige/sim/engine.h` (`Engine`,
`EngineConfig`, `parseEngineConfig`, the range constants, the full
contract); implementation: `src/laige-sim/engine.cpp`. CLI:
`tools/run/laige-run.cpp` (target `laige-run`). Unit suite:
`ctest -R engine` (`tests/laige-sim/engine_tests.cpp`); smoke test:
`ctest -R laige_run_smoke` (every P0 OS job, 1000 ticks @ 60 Hz).

```cpp
laige::EngineConfig config;
config.tickRateHz = 60;
config.entityCapacity = 4096;
config.churnPerFrameBudget = 256;

laige::Engine engine = laige::Engine::create(config).value();

// Game code registers on the world BEFORE the run (the schedule is
// computed once at the start of the run):
engine.world()->registerComponent<MyComponent>();
engine.world()->registerSystem(MySystem_Def);

const laige::Status status = engine.run_headless(10'000);
// The run always ends in the ordered shutdown — success or failure:
// engine.isShutDown() == true, engine.world() == nullptr.
```

## The lifecycle (CONC-006)

1. **`Engine::create(config)`** — validates the config, creates the
   `World` (capacity = `entityCapacity`, churn budget =
   `churnPerFrameBudget`), and registers the built-in
   `sim::Position2DFpx16` component **first** (ARCH-010: stable
   component-type ordering; the ADR 0002 default SimMath backend,
   `fpx16_16`). No frame is run and no loop exists yet; the engine is
   in the *not started* state.
2. **Game registration** — the game registers its components and
   systems on `engine.world()` before the run (see Misuse warnings).
3. **`run_headless(maxTicks)`** — computes the system schedule,
   creates the `GameLoop` (M1-LOOP-01) with the engine's per-tick hook,
   runs one initial `frame()` (which runs 0 ticks and establishes the
   loop's start reference), creates the presentation snapshot anchored
   on the loop's exact start reference (ARCH-009), and then drives
   frames at wall-clock pacing until `maxTicks` ticks have run (or
   forever, when `maxTicks == 0` — the server form). **The run always
   ends in the ordered shutdown** — a failed start or a failed frame
   mid-run is not an exception: the engine returns the failure `Status`
   and is already shut down.
4. **`shutdown()`** — ordered and **idempotent**: loop → world clear →
   snapshot → world release → logging flush (CONC-006). Calling it
   after a finished run (or twice) is a safe no-op; `world()` reads
   back `nullptr` and `run_headless` on a stopped engine returns
   `InvalidArgument` without logging (the moved-out `GameLoop`
   precedent, M1-LOOP-01).

The destructor calls `shutdown()`, so a forgotten shutdown never
leaks the world; the explicit call is the documented teardown (it
retires the logging facade, which `Logger::init` re-arms for a later
engine in the process).

## The config surface (provisional)

`EngineConfig{tickRateHz, entityCapacity, churnPerFrameBudget}` and
`parseEngineConfig(const JsonValue&)` are the **provisional** config
surface for M1-HEAD-01. M1-CFG-01 owns the final versioned config
schema (PRD §10, ARCH-007: persistent data MUST be versioned); when
M1-CFG-01 lands, the JSON parse moves behind its versioned reader and
this surface is folded into it. The provisional keys:

| key | type | range | default |
|---|---|---|---|
| `tick_rate_hz` | exact integer | 20–120 | `kDefaultTickRateHz` (60) |
| `entity_budget` | exact integer | 0–65536 | `0` (an empty scene — a valid world that creates no entities; entity creation on it fails `BudgetExhausted`) |
| `churn_per_frame_budget` | exact integer | 0–4294967295 | `kDefaultChurnPerFrameBudget` (256) |

- **Unknown keys** are ignored with one rate-limited
  `config/unknown_key` warn per key (forward-compatible with
  M1-CFG-01's additions; LOG-004).
- **Rejections** (first failure wins, one rate-limited warn each):
  `config/not_an_object` (document is not a JSON object),
  `config/tick_rate_invalid` (absent/out of range/non-integer),
  `config/entity_budget_invalid`, `config/churn_budget_invalid` —
  each maps to `ErrorCode::InvalidArgument` (NFR-13.3 grammar:
  `{codeId}|{what}|{why}|{fix}|{docAnchor}`, see `errors.md`).
- `Engine::create` re-validates the `EngineConfig` struct itself (the
  struct is public; the JSON path is not the only constructor), so a
  hand-built out-of-range config is rejected identically.

`parseEngineConfig` is a cold path (O(document keys); it allocates
only for the warn fields when a key is rejected) and is the only
place the JSON document is read — the `EngineConfig` struct is the
value the engine consumes.

## The run contract (`run_headless`)

`Status run_headless(std::uint64_t maxTicks,
                     std::uint32_t frameBudgetTicks = kDefaultMaxCatchUpTicks)`

- **`maxTicks == 0`** — the **server form**: run until the process
  ends (the headless simulation never self-terminates).
- **`maxTicks > 0`** — the bounded run: complete **exactly** at
  `maxTicks` on a healthy machine. With `frameBudgetTicks == 1` each
  frame runs at most one tick, so a late frame drops a tick rather
  than overshooting — the tick count lands exactly on the target
  under any cadence (the M1-LOOP-01 catch-up contract). With the
  default budget (5), an overloaded frame may run up to 5 ticks to
  catch up, so a late frame can overshoot by at most budget − 1
  ticks before the target check stops the run.
- **Pacing** — one `steady_clock` read per frame, one bounded
  `sleep_for` until the next tick's due time (the exact integer due
  computation, M1-LOOP-01). 1000 ticks @ 60 Hz ≈ 16.7 s of wall
  clock — the `laige_run_smoke` ctest budget (TIMEOUT 300) and its
  `status=ok` assertion (CI asserts the run completed, not the tick
  count; drops are the documented overload behavior).
- **Lifecycle logs** — one `engine/run_started` (Info) before setup,
  one `engine/run_finished` (Info) after the last frame with the
  final accounting (`ticks`, `dropped_ticks`, `dropped_frames`,
  `status`); both are structured, stable, and machine-greppable
  (AGENTS §14).

**Failure behavior** (the run always ends in shutdown):

| condition | result |
|---|---|
| engine already stopped | `InvalidArgument`, **no log** (the stopped-state failure is a pure failure) |
| `frameBudgetTicks == 0` | `InvalidArgument` + one `loop/catchup_invalid` warn (the GameLoop's validation) |
| schedule failure | the `scheduleSystems` Status (`system/*` — world-emitted) |
| a failed frame mid-run | that frame's Status (`system/*` — the loop's `runSystems` dispatch) |
| success | `ok` |

## Presentation wiring (ARCH-009)

The engine owns a
`PresentationSnapshot<sim::Fpx16_16>` — the **default** SimMath
backend per ADR 0002 (determinism math selection is M1-DET-01's
decision; the engine does not expose a backend knob in M1). Wiring:

- The loop is created with the engine's per-tick hook from the first
  `frame()`, so the snapshot exists before the hook can fire (the
  first frame runs 0 ticks by the M1-LOOP-01 contract).
- The snapshot is created **after** the first frame, anchored on the
  loop's exact `startReferenceNs()` (the presentation.h alpha
  contract: `alpha` is derived from `(now − startReference)`, never
  from a floating accumulator).
- Per frame the engine reads the clock once and passes that reading
  to `onRenderFrame(now)`; the tick path (`onTick(tick)`) refreshes
  prev/curr for every live position entity. Presentation state is
  never authoritative (ARCH-009) — `sample_position`/interpolation
  consume only the snapshot.

## Determinism scope (ARCH-010)

Headless runs are deterministic **within the same build, platform,
architecture, and compiler**: the tick cadence is integer arithmetic,
the system order is the validated schedule, and the SimMath backend
is pinned (`fpx16_16`). Wall-clock pacing (the sleep) does **not**
enter the simulation — it only decides when frames run; dropped ticks
are the documented, logged overload behavior, not nondeterminism.
Cross-build/platform determinism, replay, and state hashing are
**M1-DET-02** (the `--replay` flag is its stub today).

## `laige-run` (the CLI)

```
laige-run --headless CONFIG.json [--ticks N] [--replay LOG]
```

- `--headless CONFIG` — required: the JSON config file (bounded read,
  1 MiB max; over-bound → `MalformedInput`; read error → `IoError`).
- `--ticks N` — the bounded run target (decimal digits only;
  default 0 = the server form).
- `--replay LOG` — **stubbed** for M1-DET-02: accepted, ignored, and
  announced with one `replay/replay_deferred` warn (the flag is
  reserved so game-tool scripts can be written now).
- `--help` / `-h` — usage, exit 0.

**Exit codes:** `0` = the run completed; `1` = the engine run failed
(the `Status`'s error name is printed on stderr); `2` = usage, file,
or config error. On completion the run prints one machine-greppable
summary line on stdout:

```
laige-run headless ticks=1000 dropped_ticks=0 dropped_frames=0 status=ok
```

The CLI then calls `engine.shutdown()` a second time — the
double-shutdown idempotency the step verifies — and exits.

## Performance (PERF-002/003)

- **Per frame** (the headless run loop): one clock read, one bounded
  `GameLoop::frame()` (itself one clock read + integer ops + up to
  `frameBudgetTicks` system dispatches — PERF-002 bounded), one
  snapshot `onRenderFrame` (a few integer ops), one sleep. **No
  allocation and no logging on the healthy path** (PERF-003,
  LOG-003).
- **Setup, once per run:** exactly three one-shot allocations — the
  `GameLoop` object, the `PresentationSnapshot` object, and the
  presentation slot record table (24 B/entity slot, sized by the
  scene budget — the presentation.h storage contract). Verified
  per-frame-zero by `ctest -R engine`
  (`HeadlessFramePathAllocatesNothing`: the allocation count is
  identical for 1, 2, 3, and 10 ticks). The M1-ALLOC-01 pool
  accounting will supersede the probe once it exists.
- **Complexity** — `run_headless` is O(maxTicks × per-tick system
  work), bounded per frame by `frameBudgetTicks`. The drop path is
  cold: one rate-limited warn per overload frame (M1-LOOP-01).
- **Traps** — registering systems after the run started does not
  update the schedule (see below); running at 120 Hz on a 60 Hz
  display doubles the tick rate (validate your frame budget against
  your systems' declared budgets, M1-SYS-03).

## Misuse warnings

- **Register game components/systems on `world()` BEFORE
  `run_headless`** — the schedule is computed once at the start of
  the run. A registration after the schedule makes the schedule
  stale; the next frame fails `system/schedule_stale` (the
  M1-SYS-02 contract) and the run returns that status.
- **One run per engine** — a second `run_headless` on a finished
  engine returns `InvalidArgument` (no log). Create a new engine
  (the config is cheap; the world's pools are sized at create).
- **Do not call `world()` after shutdown** for writes — it reads back
  `nullptr` by contract (queries are safe: they return null, they do
  not dereference).
- **`maxTicks == 0` never returns** — the server form runs until the
  process ends. Use the bounded form for tests and CI.
- **The config is validated at `create`, not at run** — a
  hand-built `EngineConfig` bypassing the JSON path is still
  validated (same codes), so there is no unvalidated path.

## Testing and CI

- `ctest -R engine` — 20 tests: create/config validation, the JSON
  parse surface, the bounded run + loop accounting, the zero-frame
  budget rejection, the stopped-state second run, the double-shutdown
  idempotency, the world release, and the zero-allocation frame-path
  probe (non-sanitizer trees).
- `ctest -R laige_run_smoke` — `laige-run --headless
  tests/laige-sim/fixtures/headless_smoke.json --ticks 1000` (60 Hz,
  10 000 slots): must exit 0 and print `status=ok` on every P0 OS
  job; TIMEOUT 300 s (≈16.7 s nominal); the TSan job sets
  `TSAN_OPTIONS=halt_on_error=1`.
- The include-graph lint (`tools/laige-include-lint`) guarantees the
  headless path carries no GPU/window symbols (ARCH-003): `laige-run`
  links only `laige-sim` → `laige-core`.
