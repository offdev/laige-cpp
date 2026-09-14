# Per-system timing and budget enforcement (`World::runSystems`, G-R5)

The M1 system framework's per-system time budget (M1-SYS-03; PRD
§9.3 G-R5, FR-11.1/11.2, FR-12.3; AGENTS CORE-001, PERF-002/003,
ARCH-009): measures every system's run time per tick into a fixed
rolling histogram, enforces the declared per-system budget with
structured warn/error events, and feeds the profiler and the frame
graph report (M1-PROF-01/02). Public header:
`src/laige-sim/include/laige/sim/system.h` (`SystemTimingStats`,
`kSystemTimingWindowSamples`, `kBudgetCriticalMultiplier`, the full
contract) plus the `World::systemTimingStats`/`systemTimingWindow`
members in `src/laige-sim/include/laige/sim/entity.h`; implementation:
`src/laige-sim/system_timing.cpp` (the check and the queries) and the
per-system measurement inside `World::runSystems`
(`src/laige-sim/systems.cpp`). Unit suite: `ctest -R system_timing`
(`tests/laige-sim/system_timing_tests.cpp`).

The loop (M1-LOOP-01) runs the schedule every tick, and the timing
is automatic — nothing per system is wired by the game:

```cpp
for (tick) {
  world.beginFrame();
  world.runSystems(schedule);  // measures every system (M1-SYS-03)
  // ... read the feed, e.g. for the frame graph:
  //   auto st = world.systemTimingStats(id);
}
```

## What is measured

`World::runSystems` times each system's **own run**: the `TimeIt`
scope (the M0-CORE-08 `steady_clock` timer — monotonic, ms as a
double) starts before the run function is called and is read back
immediately after it returns. The per-tick, per-system bookkeeping
(the schedule dispatch, the `SystemContext` construction) is outside
the window — the measurement is the system's work, not the
scheduler's. The sample is handed to the system's rolling window and
the budget check in schedule order.

## The rolling window

One `Histogram` per system (M0-CORE-08): fixed capacity
`kSystemTimingWindowSamples` (64 samples — ~1.1 s at the default
60 Hz tick rate), rolling across **ticks** (`beginFrame()` does not
touch it — it is not a per-frame window). `record()` is O(1) and
allocates nothing; recording beyond the capacity drops the **oldest**
sample, and `totalRecorded()` keeps counting every sample ever
recorded, so the truncation is observable (the M0-CORE-08 contract).
The capacity is fixed at world construction (a setup-path
allocation); raising it is an ADR, not a knob.

`stats()` over the window (nearest-rank percentiles — the M0-CORE-08
definition) is a cold path: O(n log n) with no allocation. The
engine calls it only while a system is over budget (the warn/error
path).

## Budget enforcement (PRD §9.3 G-R5)

After every measured run, the sample is compared against the system's
declared `SystemDef` budget (fpx16_16 ms, converted to double
**exactly** — raw/2^16 is a power-of-two scale, so the comparison
operands are exact):

| Condition | Event | Severity |
|---|---|---|
| `measured > 1 × budget` | `system/budget_overrun` | Warn |
| `measured >= 3 × budget` (`kBudgetCriticalMultiplier`) | `system/budget_critical` | Error |

The warn fires strictly above the budget (`measured == budget` is
legal — the G-R4 strictly-greater precedent); the error at 3× or more
("over 3× → error event"). A run that is 3× over fires **both** —
the warn first, then the error, in the same tick (they are separate
rate-limit keys).

Both events:

- follow the NFR-13.3 5-field message grammar
  (`{code} | {what} | {why} | {fix} | {doc_anchor}`) — build-stable
  message text; the dynamic values are structured **fields**, never
  message text (machine-parseable output stays build-stable);
- are rate-limited per `(subsystem, event, severity)` with the
  facade's default 1 s window (LOG-004) — a sustained overrun logs
  once per window plus a `rate_limited` summary, never a log storm;
- count separately in `SystemTimingStats` (`warns`, `errors`) even
  when the event is suppressed — the counters are since-construction.

The fields on both events:

| Field | Meaning |
|---|---|
| `system` | the registered system name |
| `id` | the `SystemId` value |
| `measured_ms` | this tick's measured run time (ms) |
| `budget_ms` | the declared budget (ms, exact) |
| `p99_ms` | the rolling window's p99 after this sample |
| `window_samples` | the samples stored in the window |

An over-budget system is **still run**: the timing is observation and
reporting, never an execution gate (the engine does not skip, defer,
or cancel a system — the breach is surfaced, FR-12.3: never hidden).

## The profiler feed (M1-PROF-01/02)

- `World::systemTimingStats(id)` — `Result<SystemTimingStats,
  ErrorCode>`: the run count, the last measured ms, and the
  warn/error counts. O(1), no allocation, no side effects; the
  per-frame pull for the profiler.
- `World::systemTimingWindow(id)` — `const Histogram*` (nullptr for an
  invalid id or a moved-from world): the rolling window itself, for
  the frame graph report's `budgetCheck` (the M0-CORE-08 check; cold
  path).

Both are pure queries: an invalid `SystemId` (0, above
`systemCount()`) or a moved-from world is
`ErrorCode::InvalidArgument` / nullptr — no log, no warn (the
`World::system` precedent).

## Determinism scope (ARCH-009/ARCH-010)

The measured times are **diagnostics only**: wall-clock readings are
platform-sensitive, so they never enter authoritative simulation
state, state hashes, or replays. The only world state the timing
adds is the window contents and the counters — observability, not
sim state (the PRD's separation of authoritative from
presentation/diagnostic state). The warn/error **events** carry the
measurements as log output; the log stream is not replay state.

## Performance

Per tick, per system (the hot path):

- two `steady_clock::now()` reads (the `TimeIt` scope);
- one O(1) ring write (`Histogram::record`);
- two comparisons (warn and error thresholds);
- **no allocation, no logging** on the success path (PERF-003,
  LOG-003). At the 10k-entity reference tick with ≤ 256 systems this
  is a few hundred nanoseconds — far below the 3 ms `sim_tick_avg`
  budget (PRD §8.1).

The warn/error path is cold (a budget being breached): it performs one
window `stats()` pass — O(W log W) over the fixed W = 64 samples, no
allocation (the pre-allocated scratch buffer) — and constructs the
field values. Note the facade's level gate, not the rate state,
controls field construction: a system that stays over budget pays
this bounded cold cost on every tick while the breach persists (the
event itself is rate-limited to one per window). That is deliberate —
a persistently broken system is worth a few microseconds of
diagnostic overhead, and the cost disappears when the breach is
fixed.

World construction pays the setup cost once: the fixed
kMaxSystems record table plus one 64-sample Histogram per record
(256 records × 2 small allocations ≈ 256 KB, one-time, never a hot
path; the `engine_base_rss` 100 MB budget, PRD §8.1).

## Threading and failure

The timing state is owned by the world and written strictly on the
world's single owner thread (CONC-001; PRD §10.2: simulation is
single-threaded): `checkSystemBudget` runs inside `runSystems`; the
queries are pure reads of the same single-owner state (a `const
Histogram&` read is safe on a fully built window, the M0-CORE-08
publish contract). The table travels with the world on move and
survives `clear()` (like the system registry); a moved-from world has
no timing state (the queries fail as pure queries).

Failure behavior: nothing in the timing path can fail at runtime —
the checks are comparisons, the window cannot overflow (bounded), and
the events are logged, not thrown (FR-12.1, NFR-8.10: no
exceptions).

## Misuse warnings

- **The budget is declared at registration** — a system that cannot
  finish within its declared budget will warn (and eventually error)
  every tick; either cut the work or declare a budget the system can
  actually meet (CORE-005: the budget is a named, honest number, not
  a hope).
- **The window is not per-frame** — do not expect `beginFrame()` to
  reset it; the p99 always describes the last 64 ticks, wherever they
  fall in the frame cycle.
- **Do not treat the events as control flow** — they never stop the
  system; the fix is in the system's work or its budget (the event's
  `{fix}` field says so).
- **Do not keep the `systemTimingWindow` reference past the world**
  (it is a non-owning view into the world's single-owner state).
