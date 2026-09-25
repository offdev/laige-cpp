# Budget harness (`laige::Histogram`, `laige::TimeIt`, `budgetCheck`)

The measurement half of the PRD §8.1 performance-budget policy (M0-CORE-08;
CORE-001: no performance claim without a reproducible measurement; AGENTS
§12 report requirements). Public header:
`src/laige-core/include/laige/budget_harness.h`; implementation:
`src/laige-core/budget_harness.cpp`. The operator-facing tool is
`laige-bench` (`tools/bench/laige-bench.cpp`; canonical command form in
[building.md](../getting-started/building.md)). Unit suite:
`ctest -R budget_harness` (`tests/laige-core/budget_harness_tests.cpp`).

## Quick start

```cpp
#include <laige/budget_harness.h>

// Measure one workload: one TimeIt per iteration, recorded in a
// histogram sized to the run (every sample kept).
laige::Histogram hist(laige::Histogram::Options{1000});
for (int i = 0; i < 1000; ++i) {
  laige::TimeIt t;                 // scope start
  workload();
  hist.record(t.elapsedMs());      // milliseconds
}
const laige::HistogramStats s = hist.stats();  // min/mean/p50/p95/p99/max

// Check against a named PRD 8.1 budget (budgets.json, repo root).
auto table = laige::loadBudgets("budgets.json");   // Result<BudgetTable, ErrorCode>
if (table.isError()) { /* log table.errorText() (LOG-002); abort the run */ }
laige::BudgetReportContext ctx;
ctx.workload = "worst-case isometric reference scene @ 1080p";
ctx.build    = "GCC 16.2.1, Debug";       // caller records what it owns
ctx.machine  = "mid-range laptop (2019-2023 class), Linux";
ctx.warmup   = 100;
const laige::BudgetCheckResult r =
    laige::budgetCheck(*table.value().find("frame_time_render"), hist, ctx);
if (!r.passed) { /* print r.report; fail the CI run (PRD 8.1 policy) */ }
```

The end-to-end form is the tool:

```console
./build/bin/laige-bench --suite=<name> [--runs=N] [--warmup=N]
                       [--budget=<budget-name>]... [--budgets=<path>]
                       [--math=<fixed_point_16_16|float_pinned_32>]
                       [--report=<path>] [--list]
```

- `--suite` — required; `synthetic` (the M0 harness stand-in) or
  `sim-tick` (the M1-BENCH-01 PRD §8.1 workload: 10k entities, 2k
  dynamic bodies, 60 Hz, movement + per-tick state hash — its baseline:
  `docs/benchmarks/baselines/m1-sim-tick.md`). `--list` prints the
  suite table.
- `--budget` — **repeatable**: one run checks every named budget against
  the same histogram (each entry's `metric` picks its statistic),
  printing one report per entry. A duplicate name is a usage error.
  The M1 gate form: `--budget=sim_tick_avg --budget=sim_tick_p99`.
- `--math` — the SimMath backend (ADR 0002) the `sim-tick` suite runs
  on; `fixed_point_16_16` (the default) or `float_pinned_32`. Other
  suites ignore it.
- `--budgets` — the budgets file (default: `$LAIGE_BUDGETS_PATH`, then
  `budgets.json` in the working directory — the ctest gate entries set
  the env var to the repo root).
- `--report` — append the printed report to a file.

Exit code 0 on pass, 2 on a failed budget check (any one of the named
budgets failing fails the run — PRD 8.1 policy: a budget regression
fails CI), 1 on usage/load errors.

## `laige::Histogram`

A fixed-capacity **rolling-window** sample store. `record()` is O(1),
allocates nothing, and takes no lock — the only hot-path-safe operation.
Construction performs the two backing allocations (setup path).

**Window semantics.** The histogram keeps at most `Options::capacity`
samples. `record()` beyond capacity drops the *oldest* sample;
`totalRecorded()` counts every sample ever recorded, so truncation is
observable (`count() < totalRecorded()` means the window dropped samples —
a benchmark that needs every sample sets `capacity >= runs`).

**Statistics scope.** `stats()` describes exactly the stored window (the
last `min(totalRecorded, capacity)` samples). `mean` is computed over that
window (no running sum — no float drift). When `n == 0` the six
statistics are NaN; callers check `n` (and `budgetCheck` turns an empty
histogram into a loud `NO_SAMPLES` failure instead of reading NaN).

**Percentiles (nearest-rank, the exact documented definition).** For the
sorted stored window `v[0..n-1]` (n ≥ 1) and percentile p (0..100):
rank `r = ceil(p·n/100)` in exact integer math, clamped to ≥ 1; the
percentile is `v[r-1]`. p=0 is the min, p=100 the max, and a one-sample
window returns that sample for every p. Nearest-rank (over linear
interpolation): no fractional indices, no extra allocation, bit-identical
on every platform (CORE-004).

**Errors.** None — a histogram cannot fail. (Overflow of a full window is
the documented drop-oldest behavior; `capacity 0` is legal.)

**Performance.** `record()` — O(1), no allocation, no lock, no I/O (hot
path). `stats()` — O(n log n) time (sorts a pre-allocated scratch buffer),
no allocation (cold path: reports, budget checks — never frame/tick loops).
Copy is O(capacity) (deep, cold path); move is O(1).

**Threading (CONC-001).** One owner thread while mutable; `stats()` on a
fully built histogram is a safe const read (publish contract, like
`Result`/`Status`).

**Misuse.** Recording wall-clock timestamps is a unit error (the harness
measures durations, typically `TimeIt::elapsedMs()`). Ignoring
`totalRecorded() > count()` on a percentile claim is a silent-window bug.

## `laige::TimeIt`

A scope timer over `std::chrono::steady_clock` (monotonic — immune to
wall-clock adjustments; the right clock for durations). Milliseconds as a
double. No allocation, no lock; the start point is immutable after
construction, so `elapsedMs()` is a safe const read; `reset()` belongs to
the owner thread. For "what time is it" use the logging facade's
timestamps (system_clock, RFC 3339) — not `TimeIt`.

## `loadBudgets` / `budgetCheck` / `BudgetTable`

**`loadBudgets(path)`** — `Result<BudgetTable, ErrorCode>`. Cold path:
file I/O + bounded JSON parse (ADR 0003: 1 MiB document, depth 32 — the
file must also not exceed 1 MiB before it is read into memory).
Errors (never silent, CORE-008):

| Failure | Code |
|---|---|
| Unreadable file | `ErrorCode::IoError` (5) |
| Malformed JSON | `ErrorCode::MalformedInput` (3) |
| Unsupported version (`!= 1`) | `MalformedInput` |
| Unknown/missing field, bad `name`/`metric`/`unit`, duplicate name, negative or non-finite number | `MalformedInput` |

The loaded `BudgetTable` is immutable and safe to read from any thread;
`find(name)` is a linear scan (the table is small by design — no hash map,
PERF-006). A failed load produces no table (all-or-nothing).

**`budgetCheck(entry, histogram, context)`** — `BudgetCheckResult`
(`passed`, `measured`, `target`, `before`, `report`). Total: it cannot
fail as an operation — its *outcome* is the pass/fail flag:

| State | `passed` | `result` in the report |
|---|---|---|
| Histogram empty (n = 0) | `false` | `NO_SAMPLES` — a workload that recorded nothing is a broken harness; loud, never silent (CORE-008) |
| `target > 0` | `measured <= target` | `PASS` / `FAIL` |
| `target == 0` (hard-zero budget) | `measured == 0` | `PASS` / `FAIL` |

`before`/`after` are the AGENTS §12 before/after pair: the entry's last
recorded value (`measured` field of `budgets.json`) vs. the current
measurement. Cold path: O(n log n) (the stats pass) plus report string
building (allocates — reporting is never a hot path). Thread-safe on const
inputs (the histogram must not be mutated concurrently — CONC-001).

**Report format (stable, machine-greppable — LOG-001).** The first line is
the grep contract; numbers are `%.6g` in the "C" locale (`nan`/`inf` as
plain text):

```text
budget=<name> result=<PASS|FAIL|NO_SAMPLES> metric=<mean|min|max|p50|p95|p99> unit=<unit>
  after=<measured> before=<last recorded> target=<limit>
  stats: n=<n> min=<v> mean=<v> p50=<v> p95=<v> p99=<v> max=<v>
  context: workload=<s> build=<s> machine=<s> warmup=<n>
```

The `context` fields are the AGENTS §12 machine/build facts **the caller
harness records** (the tool supplies compiler/build type, the operator
supplies the machine — see `LAIGE_BENCH_MACHINE`). The stats line comes
from `laige::formatStatsLine`, the single source of the stats text.

## `budgets.json` schema

Repo root, versioned (ARCH-007: the reader rejects unsupported versions and
fields explicitly). Schema version **1**:

```json
{
  "version": 1,
  "description": "...",          // optional; human notes (JSON has no comments)
  "budgets": [
    {
      "name": "frame_time_render",   // required; snake_case id; unique
      "metric": "p95",               // required; mean|min|max|p50|p95|p99
      "unit": "ms",                   // required; identifier (ms, draw_calls, ...)
      "target": 8.3,                  // required; finite, >= 0 (the PRD 8.1 limit)
      "measured": 0,                  // required; finite, >= 0 (last recorded value)
      "workload": "worst-case ..."    // required; non-empty (what the budget applies to)
    }
  ]
}
```

- **`target`** is the hard PRD §8.1 limit — every budget is an *at-most*
  upper bound. `target == 0` is a **hard-zero budget** (e.g.
  `sim_heap_allocs`: zero steady-state heap allocations per frame), **not**
  "not set".
- **`measured`** is the last recorded value of the budget (the "before"
  number). `0` is the M0 convention for *not yet measured*; when a budget
  is first measured, the benchmark runner records the value here (this
  file is the baseline home — see `docs/benchmarks/`, M0-EXIT-01 writes
  the first baseline).
- **All 15 PRD §8.1 targets** are present as named entries (sim tick,
  50k-sprite scene, cold start, and the zone server each carry two
  budgets). `sim_tick_avg`/`sim_tick_p99` carry their first recorded
  values (M1-BENCH-01, `docs/benchmarks/baselines/m1-sim-tick.md` —
  the worse of the two SimMath backends, canonical Debug tree); every
  other entry stays 0 = not yet measured until its subsystem lands.
- **Versioning:** an incompatible schema change bumps `version` and ships
  a migration note here; `loadBudgets` rejects every other version
  (never guesses).
- **Validation (strict, ARCH-007):** exactly the six entry fields — a
  missing or unknown field is `MalformedInput`; `name` must be unique
  snake_case; `target`/`measured` must be finite and ≥ 0 (a well-formed
  overflow token such as `1e999` parses to `+inf` and is rejected —
  ADR 0003).

## Performance (DOC-004)

| Operation | Complexity | Allocation | Path |
|---|---|---|---|
| `Histogram::record` | O(1) | none | **hot path safe** (one index arithmetic + one store) |
| `Histogram::stats` | O(n log n) | none (pre-allocated scratch) | cold (reports) |
| `TimeIt::elapsedMs` | O(1) | none | hot path safe (one clock read) |
| `budgetCheck` | O(n log n) + report build | report string (one or two) | cold only |
| `loadBudgets` | O(file) parse | file buffer + table | setup only (file I/O) |

Traps: calling `budgetCheck`/`stats` from a frame or tick loop (cold-path
work in a hot loop, PERF-002); recording into a histogram whose capacity
is smaller than the run and then claiming "all samples"; using
`TimeIt` for timestamps (it measures durations).

## Determinism

The harness measures wall-clock durations: it is *not* deterministic
simulation state and plays no part in replay/lockstep (ARCH-010). Both
suites are deterministic by construction, which is what makes their
baselines reproducible: `synthetic` (fixed LCG constants — Marsaglia
2003 — no RNG, no allocation; `docs/benchmarks/baselines/m0-synthetic.md`,
M0-EXIT-01) and `sim-tick` (fixed seed `0x1F055EED`, deterministic
index-derived initial state, no randomness in the measured path — the
run's `final_hash` line is a bit-identical fingerprint across runs,
build types, and compilers per the ADR 0002 scope;
`docs/benchmarks/baselines/m1-sim-tick.md`, M1-BENCH-01).
