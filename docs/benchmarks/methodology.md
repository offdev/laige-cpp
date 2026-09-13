# Benchmark methodology

The normative measurement methodology for Laige performance work
(M0-DOC-01; AGENTS §12 report requirements, CORE-001 "measure first",
PRD §8.1 budget policy). It defines what every performance report must
contain, how the `budgets.json` entries map onto it, the baseline-file
convention, and the regression policy that makes budgets CI-enforced.

The measurement machinery is the budget harness
([api/budget_harness.md](../api/budget_harness.md)): `laige::Histogram`,
`laige::TimeIt`, `loadBudgets`, `budgetCheck`, and the operator tool
`laige-bench` (canonical command in
[building.md](../getting-started/building.md)).

## 1. Required report fields (AGENTS §12)

Every performance report — a baseline file under
[`baselines/`](baselines/README.md), a budget result archived with a
milestone, or a before/after pair for an architectural change
(TEST-009) — MUST record:

| # | Field | What it records |
|---|---|---|
| 1 | Hardware | CPU model/class, RAM, GPU where relevant |
| 2 | OS | Name and version, architecture (x64 / arm64) |
| 3 | Compiler and version | e.g. `g++ 16.2.1`, `MSVC 2022` |
| 4 | Build type | `Debug` (canonical) or `Release`; sanitizer trees named explicitly (`build-asan`, `build-tsan`) |
| 5 | Relevant flags | The engine policy (NFR-8.10) always applies; record any additional flags (e.g. the SimMath pinned set, a sanitizer flag set) |
| 6 | Dataset / workload | The named workload and its defining parameters — the `workload` text of the budget entry is the reference |
| 7 | Warm-up | Iterations discarded before sampling (`--warmup`) |
| 8 | Sample count | `n` — the number of recorded samples (the `n=` of the stats line) |
| 9 | Summary statistics | min / mean / p50 / p95 / p99 / max over the stored window (the `stats:` line) |
| 10 | Before / after | The checked budget's `before=` (last recorded — the entry's `measured`) and `after=` (this run) values |

Reporting rules (AGENTS §12, PERF-009, PERF-010):

- **Percentiles, not just averages.** Tail latency is a first-class
  result: report p95/p99 and max alongside the mean, never the mean
  alone.
- **Prefer repeatable automated benchmarks over ad hoc timings.** Runs
  go through the canonical `laige-bench` command so a later reader can
  reproduce the number byte-for-byte.
- **Representative workloads** — small, medium, and stress — per
  PERF-010; a microbenchmark alone cannot validate an architectural
  change.
- **Never change a benchmark solely to make a regression disappear.**
  If the workload no longer represents the product, revise it in a
  documented step (and the PRD §8.1 table with it, §5 below).

## 2. How `budgets.json` maps to the report

`budgets.json` (repo root) is the machine-readable budget table; schema
version 1, strictly validated by `loadBudgets` (ARCH-007; the schema is
documented in [api/budget_harness.md](../api/budget_harness.md)). Every
entry carries exactly six fields, and each maps to a report role:

| `budgets.json` field | Report role |
|---|---|
| `name` | The budget's identity — the `budget=<name>` of the report's first line and the section/baseline name |
| `metric` | Which statistic the check evaluates (`mean` / `min` / `max` / `p50` / `p95` / `p99`) — the report's `metric=` field |
| `unit` | The unit of `target`, `measured`, and every measured value |
| `target` | The hard PRD §8.1 limit — an at-most upper bound; `target == 0` is a **hard-zero budget** (e.g. `sim_heap_allocs`), not "not set" |
| `measured` | The last recorded value — the **before** number of the before/after pair. Updated when a run is recorded as the new baseline; `0` is the M0 convention for *not yet measured* |
| `workload` | The workload the budget applies to — the reference text of report field 6. The caller harness measures exactly this workload and echoes it in `context: workload=` |

`budgetCheck` produces the before/after pair automatically: `after` is
the current run's value of `metric` over the histogram window, `before`
is the entry's `measured` field. Recording a run as the new baseline
means writing its `after` value back into `measured` in `budgets.json`
and committing the baseline file (§4) and the table in the **same
change** (CORE-006).

## 3. The harness report and §12 field coverage

`budgetCheck` emits the stable 4-line report (LOG-001 machine-greppable;
the format's single source of truth is `laige::formatStatsLine` — see
[api/budget_harness.md](../api/budget_harness.md)):

```text
budget=<name> result=<PASS|FAIL|NO_SAMPLES> metric=<m> unit=<u>
  after=<v> before=<v> target=<v>
  stats: n=<n> min=<v> mean=<v> p50=<v> p95=<v> p99=<v> max=<v>
  context: workload=<s> build=<s> machine=<s> warmup=<n>
```

Coverage of the §1 field table:

| Report field | Provided by |
|---|---|
| 10 (before / after / target) | the harness, from the entry and the histogram |
| 8 (`n`), 9 (statistics) | the `stats:` line |
| 6 (workload), 7 (warm-up) | the `context:` line — **caller-supplied**: the operator sets `--workload`/`--warmup` |
| 4 (build), 1 (machine, partially) | the `context:` line — the tool supplies compiler + build type; the operator supplies the machine (`LAIGE_BENCH_MACHINE`) |
| 2 (OS), 3 (compiler version), 5 (flags) | **not in the report** — a baseline file or archived CI log must add them |

Consequently a baseline file is: the 4-line report verbatim, plus the
remaining §12 fields (hardware, OS, compiler + version, flags), plus the
exact command and the commit it was measured on.

## 4. Baseline files

- **Location and naming:** `docs/benchmarks/baselines/<milestone>-<workload>.md`
  (e.g. `m0-synthetic.md` — written by M0-EXIT-01 — and
  `m1-profiler-cost.md`, written by M1-PROF-01).
- **Content:** the complete §1 record for one measurement: the §12
  fields, the stable 4-line report verbatim, the exact canonical command,
  and the measured commit.
- **Immutability:** baseline files are records, not live state. A new run
  supersedes an old baseline only by adding a new baseline file and
  updating `measured` in `budgets.json` — never by editing an existing
  baseline (the before/after history must survive for regressions and
  audits).
- **`budgets.json`'s `measured`** always holds the *latest* recorded value
  of each budget (its `0` entries mean "not yet measured" until the
  subsystem that owns them lands).

## 5. Regression policy (PRD §8.1, NFR-8.1)

- **Budgets are part of CI.** A PR that regresses any budget by **> 10%**
  against its last recorded value (**`before`**) — or breaches the
  absolute **`target`** — fails CI unless the budget is revised via a
  PRD revision.
- **CI-gateable:** `laige-bench --budget=<name>` exits `0` on pass and
  **`2` on a failed budget check** (`1` = usage or load failure); the
  CI lane asserts the exit code.
- **The 10% band needs a baseline:** it applies only when
  `before > 0`. A first measurement (`before == 0`, not yet measured)
  must still pass the absolute target.
- **Hard-zero budgets have no band:** `target == 0` passes only when the
  measured value is exactly 0 (any positive measurement is a failure).
- **`NO_SAMPLES` is a failure, not a skip:** a workload that recorded
  nothing is a broken harness (CORE-008 — never silent).
- **Accepted budget revisions** change `budgets.json` in the same PR as
  the PRD revision, and are noted in the milestone change log
  (DOC-007) — a budget number is never silently moved.

## 6. Workload discipline

- **Deterministic workloads first.** The M0 reference workload
  (`laige-bench --suite=synthetic`) is deterministic by construction
  (fixed Marsaglia LCG64 constants, no RNG, no allocation) — which is
  what makes its baseline reproducible across machines and commits.
- **No silent window truncation.** Set the histogram
  `capacity >= runs` for a claim over "all samples"; if a rolling window
  is used deliberately, check `totalRecorded()` and say so in the report
  (the harness never hides a drop — CORE-008).
- **Warm up, then sample.** Discard the first N iterations (canonical
  `--warmup=100`) so first-touch costs do not pollute samples; record N
  in the report.
- **Tail latency always.** Budgets are at-most upper bounds on a named
  percentile (or max/mean) — the `metric` field of the entry — so the
  reported statistic must be the one the budget checks, not a friendlier
  one.
- **Randomized workloads use the repo-wide seed convention**
  ([testing.md §4](../testing.md)): fixed default seed `0x1F055EED`,
  overridable, so CI runs of the same commit are comparable.
- **Record the environment.** Runs on different P0 platforms are not
  comparable numbers: the §12 hardware/OS fields exist so a baseline
  never travels without its machine.
