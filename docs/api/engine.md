# Headless engine run (`Engine`, M1-HEAD-01)

The M1 headless engine object (M1-HEAD-01; PRD FR-1.6, ARCH-003,
ARCH-009, ARCH-010, CONC-006, PRD §10.2; AGENTS CORE-002/005/008,
PERF-002/003, LOG-003): wires **config → world → systems → loop**
into one owned object whose `run_headless(maxTicks)` starts the
simulation, ticks it in real time at the configured rate, and shuts
it down cleanly. It is the first full-stack surface of the engine and
the CI smoke-test target (`laige-run --headless`, this repo's `tools/run`).
It also owns the always-on profiler (M1-PROF-01, PRD FR-11.1 —
[api/profiler.md](profiler.md)): the per-tick and per-frame time
windows, the render/network counters, and the opt-in per-run profile
report.

Public header: `src/laige-sim/include/laige/sim/engine.h` (`Engine`,
the range constants, the full contract); implementation:
`src/laige-sim/engine.cpp`. The config surface (`EngineConfig`,
`loadGameConfig`, the override merge, the hot reloader) is
`src/laige-sim/include/laige/sim/config.h` — see
[api/config.md](config.md). CLI: `tools/run/laige-run.cpp` (target
`laige-run`). Unit suite: `ctest -R engine`
(`tests/laige-sim/engine_tests.cpp`) and `ctest -R config`
(`tests/laige-sim/game_config_tests.cpp`); smoke test:
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
   `churnPerFrameBudget`, seed = `seed`, deterministic mode =
   `determinism.enabled`), constructs the always-on profiler
   (M1-PROF-01: one object + its two fixed window storages — engine
   setup, not run setup), and registers the built-in component
   matching the configured SimMath backend **first** (ARCH-010:
   stable component-type ordering): `sim::Position2DFpx16` for
   `fpx16_16` (the ADR 0002 default) or `sim::Position2DFp32` for
   `fp32_pinned`. No frame is run and no loop exists yet; the engine
   is in the *not started* state.
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
   snapshot → world release → profiler release → logging flush
   (CONC-006). A profile report that was started but never finalized
   (a pre-run teardown) is abandoned with the structured
   `profiler/report_aborted` warn (no file to clean up — the write
   happens only at run end). Calling it after a finished run (or
   twice) is a safe no-op; `world()` reads back `nullptr` and
   `run_headless` on a stopped engine returns `InvalidArgument`
   without logging (the moved-out `GameLoop` precedent, M1-LOOP-01).

The destructor calls `shutdown()`, so a forgotten shutdown never
leaks the world; the explicit call is the documented teardown (it
retires the logging facade, which `Logger::init` re-arms for a later
engine in the process).

## The config surface (M1-CFG-01: the final versioned schema)

`EngineConfig` and the JSON/file loaders now live in
[api/config.md](config.md) (`laige/sim/config.h`): the **version 1**
declarative schema (the REQUIRED `version` key, ARCH-007), the
`tick_rate_hz` / `entity_budget` / `churn_per_frame_budget` / `seed`
/ `determinism` keys, and the declared presentation blocks
(`budgets`, `camera`, `asset_roots` — stored now, consumed in M2).
The provisional M1-HEAD-01 surface was folded into it: the five
original `EngineConfig` members keep their order (existing aggregate
initializers compile unchanged), the provisional JSON keys carried
over, and the migration from a provisional document is adding
`"version": 1`.

What the engine consumes directly:

- `Engine::create(config)` re-validates the typed config's tick rate
  (the config surface's rejection, `config/tick_rate_invalid`,
  subsystem `config`) — the struct is public, so a hand-built
  out-of-range config is rejected identically.
- The **seed is part of replay identity** (ADR 0002) and is logged on
  `engine/run_started`; `determinism.enabled` selects deterministic
  mode and `determinism.math` the SimMath backend the engine
  registers (see [concepts/determinism.md](../concepts/determinism.md)
  for the scope and [api/determinism.md](determinism.md) for the
  types).
- The replay identity's `configHash` covers only the
  simulation-affecting fields — the declared presentation values
  (budgets/camera/asset roots) are excluded, so the hash encoding is
  unchanged by the final schema ([api/replay.md](replay.md)).

The full key table, the versioning rules, the rejection table, the
`loadGameConfig` file loader, the `EngineConfigOverride` merge, and
the debug-only `ConfigHotReloader` are in
[api/config.md](config.md).

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
- **Lifecycle logs** — one `engine/run_started` (Info) before setup
  (fields `tick_rate_hz`, `tick_target`, `frame_budget_ticks`,
  `seed`, `determinism`, `math` — the math field is the backend id
  string `fpx16_16` or `fp32_pinned`), one `engine/run_finished`
  (Info) after the last frame with the final accounting (`ticks`,
  `dropped_ticks`, `dropped_frames`, `status`); both are structured,
  stable, and machine-greppable (AGENTS §14).

**Failure behavior** (the run always ends in shutdown):

| condition | result |
|---|---|
| engine already stopped | `InvalidArgument`, **no log** (the stopped-state failure is a pure failure) |
| `frameBudgetTicks == 0` | `InvalidArgument` + one `loop/catchup_invalid` warn (the GameLoop's validation) |
| schedule failure | the `scheduleSystems` Status (`system/*` — world-emitted) |
| a failed frame mid-run | that frame's Status (`system/*` — the loop's `runSystems` dispatch) |
| success | `ok` |

## Presentation wiring (ARCH-009)

The engine owns a `PresentationSnapshot<B>` for the **configured**
SimMath backend `B` (ADR 0002, M1-DET-01): `sim::Fpx16_16` for the
default `fpx16_16`, `sim::Fp32Pinned` for `fp32_pinned` — the same
backend the engine registered as the built-in component at init, so
the snapshot and the sim agree. The handle is type-erased on the
engine (`detail::PresentationHandle`); the backend is fixed at
`create` and is part of replay identity (a replay must use the same
backend — ADR 0002). Wiring:

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

Headless runs in deterministic mode (the default) are
**bit-identical within the same build, platform, architecture, and
compiler**: the tick cadence is integer arithmetic, the system order
is the validated schedule, the SimMath backend is the configured one
(`fpx16_16` by default — bit-exact by the language standard, ADR
0002), and any randomness is a named input (the per-system PRNG
substreams derived from `seed`, a fixed call order). Wall-clock
pacing (the sleep) does **not** enter the simulation — it only
decides when frames run; dropped ticks are the documented, logged
overload behavior, not nondeterminism. The same-build guarantee is
verified by `ctest -R determinism_mode` (the
`DeterminismMode.*` suites: identical 256-tick state-hash streams in
two consecutive runs; seed divergence). The full scope statement —
what is deterministic, the per-backend scopes, what is not yet — is
[concepts/determinism.md](../concepts/determinism.md).
Cross-build/platform determinism is **M1-DET-04** (the detcheck
matrix); replay **recording** landed with **M1-DET-02**
([api/replay.md](replay.md) — `Engine::startReplayRecording`, the
versioned log format, and `laige-run --replay`); replay **execution**
landed with **M1-DET-03** — `World::stateHash` (the deterministic
state hash, [api/entity.md](entity.md)) and `runReplay` / the
`laige-replay` runner ([api/replay.md](replay.md), "The execution
half").

## The profiler (M1-PROF-01)

The engine owns exactly one always-on `Profiler`
([api/profiler.md](profiler.md) for the full contract): created in
`Engine::create`, handed to the `GameLoop` as a non-owning view
(per-completed-tick timing), and driven for the frame-time feed by
the engine's run loop (the frame's sim work + the presentation
refresh, excluding the pacing sleep; the first frame — start
reference, zero ticks — is not recorded). A failed frame is not
recorded (the frame did not complete).

```cpp
const laige::Profiler* p = engine.profiler();  // nullptr after shutdown
const laige::ProfilerStats s = engine.profileStats();  // the last run's cache

// Opt-in per-run report (EVERY build — diagnostics, not replay state):
const laige::Status r = engine.startProfileReport("/tmp/run.json");
// written at the END of the run, version-1 JSON (profiler.md schema);
// a write failure does NOT fail the run — it is sticky in
// engine.profileReportStatus() and logged (profiler/report_write_failed)
```

- **When** — `startProfileReport` once, after all registration,
  before `run_headless` (like `startReplayRecording`). A second call
  fails `InvalidArgument` + `profiler/report_already_started`; an
  empty path fails `profiler/report_path_invalid`; a stopped engine
  fails silently.
- **Finalization** — on **every** run path (success, failed frame,
  failed start): the per-run summary describes what actually
  happened (a zero-tick run writes a zero-tick report, CORE-008).
  A pre-run shutdown abandons it with `profiler/report_aborted`.
- **Accessors** — `profileStats()` is the run's cached snapshot
  (the world is released in shutdown, so the CLI reads the cache);
  `profileReportStatus()` / `profileReportActive()` are the
  report's sticky outcome / lifecycle state.
- **Cost** — enabled: two `steady_clock` reads + one O(1) ring write
  per frame and per completed tick, no allocation; disabled
  (`profiler()->setEnabled(false)`): one branch each. The measured
  enabled cost is bounded at 1% of a 10k-entity tick —
  [baselines/m1-profiler-cost.md](../benchmarks/baselines/m1-profiler-cost.md)
  (CORE-001, DBG-004).
- **Structured events** (subsystem `profiler`): `report_started`,
  `report_written` (Info), `report_write_failed` (Error),
  `report_aborted`, `report_already_started`, `report_path_invalid`
  (Warn).

## Replay recording (M1-DET-02)

The engine records replays **opt-in** (see
[api/replay.md](replay.md) for the full format and recorder contract):

```cpp
Status Engine::startReplayRecording(std::string_view path,
                                    std::uint64_t maxBytes) noexcept;
bool   Engine::replayRecordingActive() const noexcept;
std::uint64_t Engine::replayBytesWritten() const noexcept;
```

- **When** — once, **after all component/system registration, before
  `run_headless`**: the replay identity (seed, tick rate, component
  schema hash, math backend id, config hash — ADR 0002) is captured
  from the live world + config at call time.
- **What** — one zero-length frame per **completed** tick (M1: no
  input system yet; the frame bytes are the future input blob,
  M3-INPUT-03), written to `path + ".tmp"` and published at `path`
  only on a successful bounded run (atomic temp+rename).
- **Failures** — a recording failure mid-run **stops the run**:
  `run_headless` returns the recorder's `Status` (no partial log at
  the final path); the ordered shutdown still runs. A cap below
  header+trailer (56) or a frame over 1 MiB is an `InvalidArgument` at
  the call/write; a total-size breach is `BudgetExhausted`.
- **Debug builds only** — `NDEBUG` makes the call an `InvalidArgument`
  with a `replay/record_disabled` warn.
- **Cost** — disabled: one null check per tick; enabled: one bounded
  stdio write per completed tick (the explicit, opt-in cost —
  PERF-002/003).
- **Structured events** (subsystem `replay`): `record_started`,
  `record_finished`, `record_failed`, `record_aborted`,
  `record_already_started`, `record_start_failed`, `record_disabled`.

## `laige-run` (the CLI)

```
laige-run --headless CONFIG.json [--ticks N] [--replay LOG]
          [--prof-out REPORT]
```

- `--headless CONFIG` — required: the JSON config file (bounded read,
  1 MiB max; over-bound → `MalformedInput`; read error → `IoError`).
- `--ticks N` — the bounded run target (decimal digits only;
  default 0 = the server form). A bounded run completes EXACTLY N
  ticks: the CLI drives it with frame budget 1, so a late frame
  drops its extra due tick (counted in `dropped_ticks`) instead of
  overshooting the target by up to budget - 1 (the `run_headless`
  contract). The completed tick count is therefore platform-stable —
  what the `--replay` log contract (api/replay.md) relies on.
- `--replay LOG` — **records the run** (M1-DET-02; it was the
  M1-HEAD-01 stub): opt-in, **debug builds only** (release builds
  reject it with `InvalidArgument` + a `replay/record_disabled`
  warn), default size cap 128 MiB, atomic publish at `LOG` on a
  clean run. A start or mid-run recording failure exits `2` (start)
  or `1` (mid-run — the `status=` line carries the error name) with
  no partial log at `LOG`.
- `--prof-out REPORT` — **writes the run's profile report**
  (M1-PROF-01, FR-11.1 file export): the version-1 JSON schema
  ([api/profiler.md](profiler.md)) written at `REPORT` at the end of
  the run. **Every build** (diagnostics, not replay state — no
  debug-only gate). A start failure exits `2` (the run did not
  happen); a write failure at run end does **not** fail the run —
  the run completes (exit `0`), the report error is printed on
  stderr, and the exit code becomes `2`.
- `--help` / `-h` — usage, exit 0.

**Exit codes:** `0` = the run completed (and the profile report, if
requested, was written); `1` = the engine run failed (the `Status`'s
error name is printed on stderr); `2` = usage, file, config, or
profile-report-write error. On completion the run prints one
machine-greppable summary line on stdout — **byte-stable, CI greps
it** (`laige_run_smoke`'s `PASS_REGULAR_EXPRESSION "status=ok"`):

```
laige-run headless ticks=1000 dropped_ticks=0 dropped_frames=0 status=ok
```

followed by the profiler's one-line summary (M1-PROF-01, FR-11.1
"exposed in the CLI" — **always** printed, from the engine's cached
per-run snapshot):

```
laige-run profile: ticks=1000 frames=1001 tick_ms: n=1000 min=... frame_ms: n=1001 ... entities_alive=... sim_allocs=... draw_calls=0 texture_binds=0 net_bytes=0
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
- **Profiler (M1-PROF-01, always-on by default):** the ENABLED frame
  path adds two `steady_clock` reads (the `TimeIt` around the frame's
  sim work + presentation refresh) and one O(1) ring write
  (`Profiler::recordFrame`); the ENABLED tick path adds two clock
  reads + one ring write per completed tick (the `GameLoop`'s
  `runOneTick`). No allocation, no logging. DISABLED
  (`profiler()->setEnabled(false)`): one branch each. The measured
  enabled cost is bounded at **1% of a 10k-entity tick** —
  [baselines/m1-profiler-cost.md](../benchmarks/baselines/m1-profiler-cost.md)
  (CORE-001, DBG-004). The profiler's own setup (one object + two
  fixed window storages) happens in `Engine::create`, not in the
  run's setup path — the "exactly three one-shot allocations per run"
  claim above stays true.
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
- **Start replay recording after all registration, before the
  run, and only in debug builds** — the identity is captured at call
  time (a later registration makes the recorded schema hash stale),
  a second start fails `InvalidArgument`, and `NDEBUG` builds reject
  the call by contract (see the Replay recording section above and
  [api/replay.md](replay.md)).
- **Start the profile report once, after registration, before the
  run** — a second `startProfileReport` fails `InvalidArgument`
  (`profiler/report_already_started`); a report write failure does
  NOT fail the run — it is sticky in `profileReportStatus()` (the
  `laige-run` CLI maps it to exit 2). See the The profiler section
  above and [api/profiler.md](profiler.md).

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
- `ctest -R replay_record` — the M1-DET-02 replay suite (the format
  round trip, the malformed-input table, the recorder contract, the
  identity hashes, the engine's per-tick recording + failure stop);
  `ctest -R fuzz_replay_parse` covers the parser's fuzz surface.
- `ctest -R profiler` — the M1-PROF-01 profiler suite (the counter
  model's exact percentiles, rollover, and no-op-when-disabled, the
  cold world-pulled snapshot, the `GameLoop`'s per-completed-tick
  timing, the engine's per-run cache + opt-in JSON report, the record
  path's zero-allocation, and the enabled-cost check bounded at 1% of
  a 10k-entity tick — the machine-greppable `profiler-zeroalloc` /
  `profiler-cost` lines land in the ctest output).
- The include-graph lint (`tools/laige-include-lint`) guarantees the
  headless path carries no GPU/window symbols (ARCH-003): `laige-run`
  links only `laige-sim` → `laige-core`.
