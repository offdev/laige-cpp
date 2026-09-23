# The frame graph / budget report (`FrameBudgetRecorder`, M1-PROF-02)

The per-frame budget report (M1-PROF-02; PRD FR-11.2, §9.1 S-6, §9.3
G-R5; AGENTS CORE-001, CORE-008): for each **declared budget** —
each system's declared time budget, the total tick-time budgets
(`sim_tick_avg` / `sim_tick_p99`), and the sim allocation-count
budget (`sim_heap_allocs`) — the report lists the **measured value
vs the declared value with a pass/flag**, plus the **over-budget
systems list** (the G-R5 event feed, FR-11.2). This step ships the
report core (`buildFrameBudgetReport`), the fixed per-frame
`FrameBudgetRecorder` ring, the `Engine` opt-in wiring (per-frame
accumulation + the cached per-run report), and the `laige-run
--budget-report` CLI flag with its `--fail-on-budget` CI gate.

Public header:
`src/laige-sim/include/laige/sim/frame_budget.h`
(`FrameBudgetRecord`, `kFrameBudgetWindow`, `FrameBudgetRecorder`,
`FrameBudgetReportOptions`, `FrameBudgetReport`,
`buildFrameBudgetReport`, the full contract); implementation:
`src/laige-sim/frame_budget.cpp`. Wiring: `src/laige-sim/engine.cpp`
(recorder ownership, the per-frame record in `runFrames`, the
report build at the run's end, `startBudgetReport`). CLI:
`tools/run/laige-run.cpp` (`--budget-report`, `--budgets`,
`--fail-on-budget`). Unit suite: `ctest -R budget_report`
(`tests/laige-sim/budget_report_tests.cpp`) plus the CLI smoke
`ctest -R laige_run_budget` (`tools/run/CMakeLists.txt`).

```cpp
// The engine owns the recorder (fixed storage — created in
// Engine::create, no allocation). Opt in to the per-run report
// (EVERY build — diagnostics, not replay state), after all
// registration, before the run:
const laige::Status r =
    engine.startBudgetReport("budgets.json");  // lastNFrames omitted: all retained
engine.run_headless(10'000);
// The report is BUILT and CACHED at the end of the run (on EVERY
// path — a failed run's report describes what happened), readable
// after the shutdown (the world and the profiler are released by
// then — the profileStats() cache precedent):
const laige::FrameBudgetReport& rep = engine.lastBudgetReport();
if (!rep.passed) { /* the caller gates — laige-run --fail-on-budget
                      maps overall=FAIL to exit 3 */ }
```

## The declared budgets (what is evaluated)

| Declared budget | Source | Measured over |
|---|---|---|
| **System time** — one per registered system | `SystemDef::budgetMs` (M1-SYS-01; fpx16_16 ms — exact, ADR 0002). M1 systems always declare one; the config's `system_time_default_ms` is the registration default and is not separately evaluated here | the system's M1-SYS-03 rolling window (`World::systemTimingWindow(id)`), **p99** — the same statistic the G-R5 `budget_overrun` warn carries |
| **Total tick time** — `sim_tick_avg` | `budgets.json` (M0-CORE-08 table; PRD §8.1 target 3 ms) | the `Profiler`'s tick window (`Profiler::tickWindow()`), **mean** |
| **Total tick time** — `sim_tick_p99` | `budgets.json` (PRD §8.1 target 5 ms) | the same tick window, **p99** |
| **Allocation count** — `sim_heap_allocs` | `budgets.json` (PRD §8.1 hard-zero budget, target 0) | the per-frame `FrameBudgetRecord::simAllocs` deltas of the retained frames (a cold local histogram), **max** |

Every budget is an **at-most** upper bound (the M0-CORE-08
convention; `target: 0` is a hard zero budget, not "unset"). The
per-frame line's `frame_ms` is **informational** — M1 declares no
per-frame tick budget (the tick budgets are rolling statistics over
the tick window).

## The per-frame record (`FrameBudgetRecord`)

One value per completed frame, accumulated on the hot path
(never on a failed frame — the frame did not complete):

| Field | Meaning |
|---|---|
| `frame` | 0-based index within the run's run frames (the start-reference first frame is not one — the M1-PROF-01 run loop contract) |
| `tickAfter` | completed tick count after the frame (a bounded run ends with `tickAfter == maxTicks`) |
| `ticks` | ticks completed within the frame (0 when the frame ran ahead of the tick rate; several in a catch-up frame) |
| `frameMs` | the frame's sim work + presentation refresh in ms (0.0 when the profiler is disabled) |
| `simAllocs` | the frame's sim allocation count (the `World::archetypeStats().totalReservations` delta; 0 = steady state — the `sim_heap_allocs` budget's sample) |
| `overrunWarns` | the G-R5 `system/budget_overrun` WARN events issued during the frame (the per-frame delta of the per-system warn counters) |
| `criticalErrors` | the G-R5 `system/budget_critical` ERROR events issued during the frame (as above) |

A frame **FAILs** iff `simAllocs > 0` or either G-R5 counter is
non-zero (that frame ran a system over budget, or the sim
allocated).

## The recorder (`FrameBudgetRecorder`)

A fixed ring of `kFrameBudgetWindow` (32) records — no allocation
after construction (the engine's setup creates it as a fixed array
member). `recordFrame` is O(1); recording beyond the window drops
the **oldest** record, and `totalFrames()` keeps counting every
recorded frame (a truncated window is observable — CORE-008: no
silent truncation). `at(i)` reads the retained records oldest-first
(the ring wraps — the retained set is not a contiguous span);
`reset()` clears it. At 60 Hz the window spans ~0.53 s.

## The report (AGENTS §12 field format)

`buildFrameBudgetReport(recorder, profiler, world, budgets,
options)` is a **cold** format pass (it allocates — reporting is
never a hot path): the machine-greppable text, one `key=value` field
per token, plus `passed` (the overall pass/flag). Layout:

```
laige-budget-report version=1
laige-budget-report context: workload=headless build= machine= warmup=0
laige-budget-report frames: n=4 total=31
laige-budget-report frame=27 ticks=1 tick_after=27 frame_ms=0.000781 sim_allocs=0 overrun_warns=0 critical_errors=0 result=PASS
laige-budget-report frame=28 ticks=1 tick_after=28 frame_ms=0.001202 sim_allocs=0 overrun_warns=0 critical_errors=0 result=PASS
laige-budget-report frame=29 ticks=1 tick_after=29 frame_ms=0.001303 sim_allocs=0 overrun_warns=0 critical_errors=0 result=PASS
laige-budget-report frame=30 ticks=1 tick_after=30 frame_ms=0.002435 sim_allocs=0 overrun_warns=0 critical_errors=0 result=PASS
budget=sim_tick_avg result=PASS metric=mean unit=ms
  after=0.00105637 before=0 target=3
  stats: n=30 min=0.000421 mean=0.00105637 p50=0.000832 p95=0.002475 p99=0.002545 max=0.002545
  context: workload=headless build= machine= warmup=0
budget=sim_tick_p99 result=PASS metric=p99 unit=ms
  after=0.002545 before=0 target=5
  stats: n=30 min=0.000421 mean=0.00105637 p50=0.000832 p95=0.002475 p99=0.002545 max=0.002545
  context: workload=headless build= machine= warmup=0
budget=sim_heap_allocs result=PASS metric=max unit=allocs_per_frame
  after=0 before=0 target=0
  stats: n=4 min=0 mean=0 p50=0 p95=0 p99=0 max=0
  context: workload=headless build= machine= warmup=0
laige-budget-report over_budget: none
laige-budget-report overall=PASS
```

The three `budget=...` blocks are the M0-CORE-08 `budgetCheck`
report, embedded verbatim (`docs/api/budget_harness.md`). The
per-system section (one line per registered system, ascending id)
and the over-budget list follow the same field format:

```
laige-budget-report system id=1 name=BRBurn budget_ms=0.100006 runs=10 last_ms=2.00036 measured_p99_ms=2.00036 result=FAIL warns=10 errors=10 window: n=10 min=2.00021 mean=2.00037 p50=2.00036 p95=2.00036 p99=2.00036 max=2.00038
laige-budget-report over_budget: id=1 name=BRBurn p99_ms=2.00036 budget_ms=0.100006
laige-budget-report overall=FAIL
```

(`budget_ms` is the fpx16_16 declared budget rendered as a double —
0.1 ms becomes `0.100006` at 16.16 resolution; the value is exact,
the rendering is %.6g.) A real 0-system run's sample report is
committed at
`tests/laige-sim/fixtures/budget_report_sample.txt` (a **sample,
not a golden** — the report carries wall-clock values, so no
byte-exact golden test; the `budget_report` suite asserts the
machine-greppable structure instead).

## Pass/flag semantics

- **Frame:** FAIL iff `simAllocs > 0` or a G-R5 event fired in the
  frame (the record above).
- **System:** **FAIL** iff the rolling window's p99 > the declared
  `SystemDef` budget (a sustained overrun — the G-R5 counters
  `warns`/`errors` count every single overrun; the rolling p99 is
  the sustained signal). **NO_SAMPLES** iff the window is empty
  (the system never ran — loud, never silent; the `budgetCheck`
  precedent).
- **Declared budget:** the M0-CORE-08 `budgetCheck` result
  (PASS/FAIL; **NO_SAMPLES** for an empty window — e.g. a zero-tick
  run; **NO_ENTRY** when the `budgets.json` entry is missing — a
  configuration error, loud).
- **Overall:** `overall=PASS` iff every section passes — a NO_SAMPLE
  state or a NO_ENTRY folds to FAIL (a broken harness is loud, not
  green — CORE-008). `FrameBudgetReport::passed` mirrors it.

A single **recovered** overrun (warn + next ticks fast) is visible
in the per-frame records (that frame FAILs) and the `warns`/`errors`
counters, without failing the system's rolling p99 — the at-most
budgets measure sustained behavior, while the per-frame records
keep the transient visible.

## The Engine surface

| Member | Contract |
|---|---|
| `startBudgetReport(budgetsPath, lastNFrames = kFrameBudgetWindow)` | Opt-in, **EVERY build** (diagnostics, not replay state — like `startProfileReport`). Called after all registration, before `run_headless`; one report per run. Loads the `budgets.json` table now (cold setup path) and builds + caches the report at the run's end. `lastNFrames` bounds the report's per-frame section (1..`kFrameBudgetWindow`; larger values clamp). Errors: stopped engine → `InvalidArgument` (no log — the stopped-state precedent); empty path → `InvalidArgument` + warn; double start → `InvalidArgument` + warn; load failure → `IoError`/`MalformedInput` + warn. |
| `budgetReportRequested()` | True when the report was started. O(1). |
| `lastBudgetReport()` | The last run's report (`passed` + `report`), **cached** — readable after the shutdown (the world and the profiler are released; the `profileStats()` cache precedent). Empty/`false` before the first run. O(1), no allocation. |

The per-frame **accumulation is always on** (a disabled cost of two
O(1) reads, two O(systemCount) G-R5 counter passes, and one O(1)
ring write per frame — no allocation, no logging; PERF-003,
LOG-003). Only the **report** is opt-in.

Structured events (LOG-001, subsystem `budget`, NFR-13.3 5-field
messages): `report_started` (Info), `report_path_invalid` (Warn),
`report_already_started` (Warn), `report_load_failed` (Error). The
G-R5 budget events themselves stay under the `system` subsystem
(M1-SYS-03); the report folds their per-frame deltas into the
records — it never re-issues them.

A budget **FAIL never fails the run** (diagnostics never gate the
simulation — CORE-002's priority order): the caller gates
(`laige-run --fail-on-budget`, or `lastBudgetReport().passed`).

## The CLI (`laige-run`)

```
laige-run --headless <config.json> --budget-report [N] \
          [--budgets <path>] [--fail-on-budget]
```

- `--budget-report [N]` — print the last N frames' budget report to
  stdout at the end of the run (after the profile summary line), in
  the AGENTS §12 field format. N: 1..`kFrameBudgetWindow` (32);
  omitted = all retained frames. The report is printed **even on a
  failed run** (the run failure dominates the exit code).
- `--budgets <path>` — the `budgets.json` file (schema v1).
  Resolution: this argument, then the `LAIGE_BUDGETS_PATH` env var,
  then `budgets.json` in the working directory (the `laige-bench`
  resolution order).
- `--fail-on-budget` — exit **3** when the run **completed** but the
  report is `overall=FAIL` (the CI gate — PRD §8.1 budget policy).
  A failed run exits 1 regardless (the gate never masks a run
  failure).

Exit codes: 0 = ok; 1 = engine run failure; 2 = usage / IO / config /
profile-report-write / **budget-report-start** error; **3 = budget
failure** (`--fail-on-budget`; the run completed, the report is
`overall=FAIL`).

## Performance (PERF-001/003, LOG-003)

- **Per frame (hot path):** two O(1) reads (the loop's tick count,
  the pool reservations), two O(systemCount) passes over the
  per-system G-R5 counters, one O(1) ring write — no allocation, no
  logging. The engine's run-setup allocation count is unchanged at
  exactly three one-shot objects (the M1-HEAD-01 zero-allocation
  test still pins it: the ring is created in `Engine::create`,
  before the measured window).
- **Record path (fixed storage):** `recordFrame` is O(1), no
  allocation — verified by `budget_report`'s
  `RecorderRecordPathAllocatesNothing` (the test-only operator-new
  counter, non-sanitizer trees; the sanitizer trees prove it
  leak-free).
- **Report build (cold, once per run):** one O(systemCount) pass +
  one O(n log n) histogram per budget + one format pass; it
  **allocates** — reporting is never a hot path (the
  `profiler.cpp` / `budget_harness.cpp` precedent). Built at the
  run's end, before the shutdown, on every run path.
- **Ring storage:** 32 × 48 B = 1.5 KiB fixed (negligible).

## Determinism (ARCH-009)

The report is **diagnostics only**: the measured times and
allocation counts never enter sim state, state hashes, or replays
(the `LAIGE-DETERM-EXCEPTION` G-R8 markers at each `double` use
record the boundary). The `budgets.json` table and the declared
`SystemDef` budgets are configuration inputs (deterministic); the
measured values are wall-clock facts about the run.

## Testing & CI

- `ctest -R budget_report` — the unit suite (14 tests): the
  recorder ring semantics, the synthetic over-budget system's
  correct numbers (declared budget echoed, measured p99 above it,
  exact `runs`/`warns`/`errors`, the `over_budget:` line,
  `overall=FAIL`), the healthy-world PASS, the loud NO_ENTRY /
  NO_SAMPLES semantics, the engine's cached-after-shutdown report,
  the G-R5 fold into the per-frame records, the start validation,
  and the record-path zero-allocation. The machine-greppable
  `budget-report-overbudget` / `budget-report-engine` /
  `budget-report-recorder-zeroalloc` lines land in the ctest
  output.
- `ctest -R laige_run_budget` — the CLI smoke (every P0 OS job): a
  30-tick run with `--budget-report 4 --budgets <repo>/budgets.json
  --fail-on-budget`; passes iff the report prints `overall=PASS` and
  the run exits 0.
- The `budget_report` suite is in the TSAN property list
  (`tests/laige-sim/CMakeLists.txt`) and required green under ASan
  (the leak-free property of the ring and the cached report).
