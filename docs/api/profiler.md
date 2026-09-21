# The always-on profiler (`Profiler`, M1-PROF-01)

The engine's built-in profiler (M1-PROF-01; PRD FR-11.1, FR-11.2,
PRD §15.1 DBG-008; AGENTS CORE-001, PERF-003, DBG-004):
**always-on (cheap) counters** — per-system time, entity counts,
alloc counts (target: 0 in sim), draw calls, texture binds, net
bytes, tick time, frame time percentiles — exposed in the editor
overlay (M2), the CLI, and file export. This step ships the
headless half of that surface: the counter core, the cold
snapshot/report API, the `GameLoop` per-tick timing hook, the
`Engine` frame timing + per-run report, and the `laige-run
--prof-out` CLI flag.

Public header:
`src/laige-sim/include/laige/sim/profiler.h` (`Profiler`,
`ProfilerStats`, `ProfileFormat`, `kProfilerTickWindowSamples`,
`kProfilerFrameWindowSamples`, the report formatters, the full
contract); implementation: `src/laige-sim/profiler.cpp`. Wiring:
`src/laige-sim/game_loop.cpp` (`runOneTick` per-tick timing) and
`src/laige-sim/engine.cpp` (profiler ownership, frame timing,
per-run report finalization). CLI: `tools/run/laige-run.cpp`
(`--prof-out`). Unit suite: `ctest -R profiler`
(`tests/laige-sim/profiler_tests.cpp`).

```cpp
// The engine owns the profiler (created in Engine::create, ON by
// default). Read the live counters or the last run's cached
// snapshot:
const laige::Profiler* p = engine.profiler();   // nullptr after shutdown
const laige::ProfilerStats s = engine.profileStats();

// Opt in to the per-run report (EVERY build — diagnostics, not
// replay state), after all registration, before the run:
const laige::Status r = engine.startProfileReport("/tmp/run.json");
engine.run_headless(10'000);
// The report appears at /tmp/run.json at the END of the run (JSON,
// version 1 schema below). A write failure does NOT fail the run —
// it is sticky in engine.profileReportStatus() and logged
// (profiler/report_write_failed).

// The loop can also take a profiler directly (non-owning view):
laige::Profiler prof(laige::Profiler::Options{});
laige::GameLoop::Options opts;
opts.profiler = &prof;   // per-completed-tick timing, success only
```

## What is measured (the FR-11.1 counter table)

| Counter | Source | M1 state |
|---|---|---|
| tick time (p50/p95/p99/mean/min/max over a rolling window) | the `GameLoop`'s per-completed-tick `TimeIt` → `recordTick` | measured |
| frame time (same percentiles, rolling window) | the `Engine` run loop's `TimeIt` (frame sim work + presentation refresh, **excluding the pacing sleep**) → `recordFrame` | measured |
| per-system time (p50/p95/p99/min/max over the M1-SYS-03 window) | `World::systemTimingWindow(id)` (M1-SYS-03) — read cold in the report, never copied | pulled per system |
| entity counts (total / alive) | `World::stats()` (`totalCreated` / `inUse`) | pulled at snapshot |
| entity capacity | `World::stats().capacity` (the declared scene budget) | pulled at snapshot |
| sim alloc count (target: 0 in sim) | `World::archetypeStats().totalReservations` (M1-ECS-03 pool accounting — the pool-backed sim storage's reserved column blocks) | pulled at snapshot |
| systems | `World::systemCount()` | pulled at snapshot |
| draw calls / texture binds / net bytes | `Profiler::addDrawCalls` / `addTextureBinds` / `addNetBytes` (the M2/M3 feeds) | **0 in headless M1** — the fields exist per FR-11.1; the render (M2) and network (M3) subsystems feed them |

The `Profiler` owns **only** what it measures at the frame/tick
boundary (the two windows, the counters). Everything else is pulled
**cold** from its owner at snapshot time — no duplicated state, one
source of truth each. `snapshot()` returns the profiler's own
counters (no world access); `snapshot(world)` adds the world-pulled
fields (`worldAvailable` true; the no-arg form leaves them zero /
false).

## The windows (M0-CORE-08 `Histogram`)

Two fixed rolling windows, sized at construction:

| Window | Default capacity | ~ history at 60 Hz |
|---|---|---|
| tick time | `kProfilerTickWindowSamples` (512) | 8.5 s |
| frame time | `kProfilerFrameWindowSamples` (256) | 4.3 s |

`record()` is O(1) and allocates nothing (the setup path performed
the backing allocations; every later operation allocates nothing).
Recording beyond the capacity drops the **oldest** sample, and the
since-construction counters (`ProfilerStats.ticks` / `.frames` —
the windows' `totalRecorded()`) keep counting, so the truncation is
observable. `stats()` (nearest-rank percentiles) is a cold path:
O(n log n) over the stored window, no allocation, NaN when empty
(check `n` — the M0-CORE-08 contract).

**Semantics:** the tick sample covers the tick body (the frame's
`beginFrame` + one `runSystems` dispatch); a **failed tick is not
recorded** (the GameLoop tick-count contract). The frame sample
covers the frame's sim work plus the presentation refresh,
**excluding the pacing sleep** (the sleep is cadence, not work); the
first frame (start reference, zero ticks) is not a run-frame and is
not recorded; a failed frame is not recorded.

## Ownership, threading, determinism

A `Profiler` is **move-only** (the `GameLoop` precedent):
construction performs the backing allocations (setup path,
PERF-003); every later operation allocates nothing. A moved-from
profiler is **stopped**: records are no-ops and snapshots return
empty values (no log). It has exactly one owner thread (CONC-001;
PRD §10.2) and holds **no world reference** (the world data is
pulled by argument, cold), so it may be released independently of
the world — the engine releases it in the ordered shutdown
(“pools” step, after the world; engine.h).

**Determinism (ARCH-010/ARCH-009):** the counter and window
contents are wall-clock-derived **diagnostic** state — they never
enter authoritative simulation state, state hashes, or replays.

## The reports (FR-11.1 file export)

`writeProfile(profiler, world, path, format)` formats the full
report and writes it to `path` (truncating; the file appears only
when the write fully succeeds — **no partial report** on failure).
Every failure is a `Result` (`ErrorCode::IoError` — CORE-008: never
silent); success returns the bytes written. Cold path only
(reporting is never a hot path).

### The text form (`ProfileFormat::Text`)

One greppable section per line:

```
laige-profile version=1
laige-profile counters: ticks=100 frames=101 draw_calls=0 texture_binds=0 net_bytes=0
laige-profile tick_ms: n=100 min=0.000191 mean=0.0013269 p50=0.001042 p95=0.002725 p99=0.002966 max=0.003197
laige-profile frame_ms: n=101 min=0.00017 mean=0.00221881 p50=0.001854 p95=0.004429 p99=0.00482 max=0.00496
laige-profile world: entities_alive=0 entities_total=0 entity_capacity=10000 sim_allocs=0 systems=1
laige-profile system id=1 name=EngTickCounter budget_ms=1 runs=100 last_ms=0.0004 warns=0 errors=0 window: n=100 min=... 
```

An empty window renders `n=0` (the report never emits NaN text —
LOG-001); a non-empty window renders the M0-CORE-08 stats fields
(the `stats: ` prefix of `formatStatsLine` is stripped — the line
carries its own context). One line per registered system (the
M1-SYS-03 feed): the declared budget (fpx16_16 → double, exact
power-of-two scale), the run scalars, and the per-system window's
stats.

### The JSON form (`ProfileFormat::Json`, version 1)

The `--prof-out` report. The `n==0` windows serialize as JSON
`null` (the report never carries NaN — the `serializeJson`
precondition); number values are the core JSON canonical form
(`laige/json.h` — a small integer may render in `%g` form, e.g.
`1e+02`; parse it, don't string-compare it):

```json
{
  "version": 1,
  "counters": { "ticks": 100, "frames": 101, "draw_calls": 0,
                "texture_binds": 0, "net_bytes": 0 },
  "tick_time_ms":  { "n": 100, "min": 0.000191, "mean": 0.0013269,
                     "p50": 0.001042, "p95": 0.002725, "p99": 0.002966,
                     "max": 0.003197 },
  "frame_time_ms": { "n": 101, "min": 0.00017, "mean": 0.00221881,
                     "p50": 0.001854, "p95": 0.004429, "p99": 0.00482,
                     "max": 0.00496 },
  "world":  { "entities_alive": 0, "entities_total": 0,
              "entity_capacity": 10000, "sim_allocs": 0,
              "systems": 1 },
  "systems": [ { "id": 1, "name": "EngTickCounter",
                 "budget_ms": 1, "runs": 100, "last_ms": 0.0004,
                 "warns": 0, "errors": 0,
                 "window_ms": { "n": 100, "min": 0.0002, "mean": 0.0004,
                                "p50": 0.0004, "p95": 0.0005, "p99": 0.0005,
                                "max": 0.0006 } } ]
}
```

`window_ms` (and the top-level windows) are `null` when the window is
empty. The CLI one-line summary (always printed by `laige-run`,
engine.md) is `formatProfileSummaryLine(stats)` — the same fields
compressed into one greppable line.

## The engine's per-run report (the `--prof-out` surface)

- **`startProfileReport(path)`** — once, after all registration,
  before `run_headless`; **every build** (the report is
  diagnostics, not replay state — no `NDEBUG` gate, unlike replay
  recording). Stopped engine → `InvalidArgument` (no log); empty
  path → `InvalidArgument` + `profiler/report_path_invalid`;
  already started → `InvalidArgument` +
  `profiler/report_already_started`.
- **Finalization** — at the end of the run, on **every path**
  (success, failed frame, failed start alike — the per-run summary
  describes what actually happened; a zero-tick run writes a
  zero-tick report, CORE-008), before the shutdown (the world-pulled
  fields' source — the world — is still live).
- **A write failure does NOT fail the run** (diagnostics never gate
  the simulation — CORE-002's priority order): it is recorded in
  the sticky `profileReportStatus()`, logged
  (`profiler/report_write_failed`, Error), and left for the caller
  — `laige-run` maps it to exit 2 (the run itself is exit 0/1).
- **`profileStats()`** — the last run's cached snapshot: the world
  is released in the shutdown, so the CLI reads the cache, not the
  live state.
- **Shutdown abandonment** — a started report whose run never ran
  (a pre-run teardown) is abandoned with
  `profiler/report_aborted` (Warn); there is no file to clean up
  (the write happens only at run end).
- **Structured events** (subsystem `profiler`, the NFR-13.3 5-field
  grammar, LOG-001/002):

| Event | Severity | When |
|---|---|---|
| `report_started` | Info | `startProfileReport` accepted |
| `report_written` | Info | the run-end write succeeded (fields `path`, `bytes`) |
| `report_write_failed` | Error | the run-end write failed (fields `path`, `error`) |
| `report_aborted` | Warn | shutdown abandoned a started report (field `path`) |
| `report_already_started` | Warn | a second `startProfileReport` |
| `report_path_invalid` | Warn | an empty report path |

## Performance (DOC-004)

- **Enabled (the default):** two `steady_clock` reads per completed
  tick (the `GameLoop::runOneTick` `TimeIt`) + one O(1) ring write;
  two `steady_clock` reads per frame (the engine run loop) + one
  O(1) ring write. **No allocation and no logging** (PERF-003,
  LOG-003; the `profiler-zeroalloc` test asserts the record path
  allocates nothing: 1000 tick + 1000 frame records + 3 adders + one
  snapshot, `allocs=0`).
- **Disabled** (`setEnabled(false)`, or `Options::profiler ==
  nullptr`): one branch per tick and per frame, nothing else
  (DBG-004: disabled instrumentation has negligible cost).
- **Measured enabled cost (the gate, CORE-001/DBG-004):** ON vs OFF
  over 10k-entity ticks must stay within **1%** —
  [baselines/m1-profiler-cost.md](../benchmarks/baselines/m1-profiler-cost.md)
  (the `ProfilerCost.EnabledCostBoundedToOnePercent` test enforces
  the bound on every tree and prints the machine-greppable
  `profiler-cost` line; measured ≈ 0.17% on the canonical tree).
- **Cold path:** `snapshot()` / `tickTime()` / `frameTime()` are
  O(n log n) over the stored window (no allocation — the
  `Histogram`'s pre-reserved scratch buffer); the report formatters
  and `writeProfile` are O(systemCount × n log n) + one file write —
  reporting is never a hot path (the M0-CORE-08 precedent).
- **Traps:** `stats()` on an empty window is NaN (check `n`); the
  windows roll (the since-construction counters in
  `ProfilerStats` say how much was dropped); the per-system windows
  are the M1-SYS-03 feeds (their capacity is the world's, not the
  profiler's — the two do not share storage).

## Misuse warnings

- **One profiler per loop/engine; the profiler must outlive its
  consumers.** `GameLoop::Options::profiler` is a **non-owning**
  view (the `onTickContext` lifetime contract); the engine creates
  its profiler in `Engine::create` and releases it in the ordered
  shutdown **after** the loop (engine.h). A moved-from profiler is
  safe (stopped: records are no-ops); a dangling pointer is a
  lifetime bug.
- **Do not read `profileStats()` before the first run** — it is
  zeroed (the snapshot is captured at the end of a run).
- **`startProfileReport` is once per run, before the run** — a
  second call fails; a post-run call on a stopped engine fails
  silently.
- **Do not treat the measured times as simulation state** — they are
  wall-clock diagnostics (ARCH-009): never in the tick count, the
  state hash, or a replay (the M1-SYS-03 precedent).
- **`sim_allocs` is a total, not a per-frame delta** — the steady-
  state **per-frame delta** is the FR-11.1 target of 0 (M1-ALLOC-01
  asserts it per tick via the allocation hook).

## Testing and CI

- `ctest -R profiler` — the full M1-PROF-01 suite
  (`ProfilerCounters`, `ProfilerSnapshot`, `ProfilerGameLoop`,
  `ProfilerEngine`, `ProfilerReport`, `ProfilerZeroAlloc`,
  `ProfilerCost`): the counter model's exact percentiles (1..100 →
  p50=50, p95=95, p99=99), window rollover, the zero-capacity drop,
  the disabled no-op + preserved state, the moved-from stop, the
  cold world-pulled snapshot, the per-completed-tick timing
  (failed ticks unrecorded), the engine's per-run cache, the report
  (written on every run path, version-1 JSON parseable, the M1-SYS-03
  window feed, double-start / empty-path / stopped-engine
  rejections, write failure sticky without failing the run, pre-run
  shutdown abandonment with no file on disk, the greppable text
  form), the record path's zero-allocation (non-sanitizer trees —
  the `profiler-zeroalloc` line), and the enabled-cost check
  (ON vs OFF over 10k-entity ticks, ≤ 1% — the `profiler-cost`
  line).
- `ctest -R laige_run_smoke` — the CLI smoke (the byte-stable
  `status=ok` line; the profile summary line follows it).
- The TSan job runs the `profiler` entry with
  `TSAN_OPTIONS=halt_on_error=1`.
- The disabled-cost baseline:
  [baselines/m1-profiler-cost.md](../benchmarks/baselines/m1-profiler-cost.md)
  (AGENTS §12 metadata + verbatim run output).
