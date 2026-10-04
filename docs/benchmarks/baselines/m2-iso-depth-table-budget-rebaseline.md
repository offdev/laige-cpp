# Baseline: `m2-iso-depth-table-budget-rebaseline` — re-baselined
# `iso_depthkey_rebuild` target (0.2 → 0.3 ms)

Recorded by the **M2-ISO-02 budget revision** (2026-10-04, follow-up to
the M2-ISO-02 workload fix). This is the **eighth** baseline file; it is
immutable (methodology §4 — superseding it later adds a new file, it is
never edited). It **supersedes**
[`m2-iso-depth-table-workload-fix.md`](m2-iso-depth-table-workload-fix.md)
as the latest recorded value of `iso_depthkey_rebuild` (that baseline
stays in place as the before number of this before/after pair;
`budgets.json` `measured` is updated by this baseline, and — per the
NFR-8.1 policy — the absolute `target` is revised in the same change,
together with the PRD revision).

## Why this baseline exists

The `iso_depthkey_rebuild` gate (mean ≤ 0.2 ms, 10k dirty cells after a
terrain edit, PRD §8.1) failed the CI `Linux x64 (clang++)` reference
lane **repeatedly on unchanged engine code**:

- Run 36885653175 (2026-10-01, pre-workload-fix): mean 0.20889 /
  0.209044 ms — FAIL. Root cause was a *harness* artifact (three
  `div`/`idiv` per iteration in the measured window at `-O0`), removed
  by the division-free workload fix (recorded in
  `m2-iso-depth-table-workload-fix.md`).
- After that fix the engine code did **not change**, yet the gate kept
  flapping on the same reference lane (Clang 18.1.3, CMake Debug,
  ubuntu-24.04 shared runner):

| CI run (all `Linux x64 (clang++)`) | fpx16_16 mean | fp32_pinned mean | result |
|---|---|---|---|
| 37192995927 attempt 1 (2026-10-04) | 0.194142 ms | 0.201495 ms | fpx16 PASS / fp32 FAIL |
| 37192995927 attempt 2 (2026-10-04, slow shared runner) | 0.266709 ms | 0.267328 ms | both FAIL |
| 37194767648 (2026-10-04, master merge lane, commit `70830a7`) | 0.202146 / 0.202204 ms | 0.202018 / 0.194168 ms | FAIL (both entries) |

The reference lane measures this workload at **0.194–0.267 ms** —
straddling the 0.2 ms bar, so the gate's outcome is decided by how busy
the shared runner is at the moment, not by the engine. Evidence that
this is runner variance and not a regression:

- The engine's `setTile` update path is unchanged since the
  2026-10-01 workload fix (five merges landed in between —
  M2-ISO-03, M2-SORT-01, M2-SPRITE-01/02, the MSVC cast fix — none
  touches `iso_depth_table`).
- The `Linux x64 (g++)` lane of run 37194767648 **passed** the same
  workload on the same commit.
- The recorded numbers (0.0814 ms canonical g++ local, 0.139 ms local
  Clang -O0) predate the gate's calibration to the CI lane: the 0.2 ms
  bar was never re-verified with margin on the reference toolchain —
  the exact lesson the workload-fix baseline recorded ("first-crossings
  of a budget gate on a new CI toolchain must be verified there").

## The revision

Per the PRD §8.1 policy ("a PR that regresses any budget by > 10% (or
breaches absolute target) fails CI **unless the budget is revised via a
PRD revision**", NFR-8.1) and methodology §5 ("accepted budget
revisions change `budgets.json` in the same PR as the PRD revision"):

- **PRD §8.1** (v0.4): `≤ 0.2 ms` → `≤ 0.3 ms` (mean, 10k dirty cells).
- **`budgets.json`**: `target` 0.2 → **0.3**; `measured` 0.0814067
  (local canonical tree) → **0.202204** — the latest recorded value,
  the worse of the two backends on the CI reference lane, run
  37194767648 (2026-10-04).
- **Workload, engine code, measurement method: unchanged.** This is a
  budget re-baseline, not a benchmark change (methodology §1) — the
  workload is still the PRD §8.1 10k-dirty-cells edit, 100 warm-up +
  3 000 measured iterations, mean metric, both SimMath backends.

**Margin:** 0.3 ms sits 48% above the latest recorded reference-lane
value (0.202204) and 12.4% above the worst observed reference-lane mean
(0.267328, slow-runner outlier). The gate's metric is the **mean**
(n=3000, warmup=100): single-iteration spikes (worst observed max
0.300107 ms, same outlier run) do not drive the gate. 0.3 ms for 10k
dirty cells is 1.8% of the 16.7 ms 60 FPS frame budget — the product
promise stays tight.

## AGENTS §12 metadata (the recorded run)

| # | Field | Value |
|---|---|---|
| 1 | Hardware | GitHub Actions ubuntu-24.04 hosted runner (2 vCPU shared) — the CI reference machine |
| 2 | OS | ubuntu-24.04 |
| 3 | Compiler and version | Clang 18.1.3 (runner apt package), CMake Debug |
| 4 | Build type | `Debug` (`-O0 -g` + engine policy flags) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002) on the engine TUs |
| 6 | Dataset / workload | `iso_depth_table` — 128×128 grid (16 384 cells, 64 chunks), terrain `(7gx+11gy)%5`; one iteration = 10 000 `setTile` calls over a 100×100 block at (14,14), column-major, height `(gx+gy)%5`, edit sequence precomputed outside the measured window; both SimMath backends |
| 7 | Warm-up | 100 iterations discarded |
| 8 | Sample count | `n=3000` iterations per backend (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | see the verbatim reports below (per run) |
| 10 | Before / after | `before=0.0814067` (the M2-ISO-02 workload-fix recorded value, canonical local tree) · `after` (mean, worse backend, run 37194767648) = 0.202204 → **recorded 0.202204** · `target`: 0.2 → **0.3 ms** (PRD v0.4 revision) |

Commit measured on: `70830a7` (master, the PR #72 merge commit — the
run that motivated this revision).

## Verbatim run output (CI reference lane, run 37194767648)

### `iso_depth_table` ctest entry, first backend pair

```text
budget=iso_depthkey_rebuild result=FAIL metric=mean unit=ms
  after=0.202146 before=0.0814067 target=0.2
  stats: n=3000 min=0.1855 mean=0.202146 p50=0.201084 p95=0.208645 p99=0.211189 max=0.236467
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

(second backend of the same entry: `after=0.202018`, stats
`min=0.18515 p50=0.201134 p95=0.208595 p99=0.211289 max=0.23845`)

### `laige-render_tests` (full binary) run of the same job

```text
budget=iso_depthkey_rebuild result=FAIL metric=mean unit=ms
  after=0.202204 before=0.0814067 target=0.2
  stats: n=3000 min=0.18561 mean=0.202204 p50=0.201334 p95=0.209597 p99=0.213092 max=0.282557
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

```text
budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.194168 before=0.0814067 target=0.2
  stats: n=3000 min=0.191289 mean=0.194168 p50=0.192781 p95=0.200302 p99=0.204068 max=0.285451
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

(The second backend passed 0.07% under the bar in the same job — the
coin-flip shape of a zero-margin gate.)

### PR-lane evidence (run 37192995927)

Attempt 1 (job 111408940973):

```text
budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.194142 before=0.0814067 target=0.2
  stats: n=3000 min=0.18234 mean=0.194142 p50=0.192915 p95=0.200226 p99=0.205123 max=0.237111
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100

budget=iso_depthkey_rebuild result=FAIL metric=mean unit=ms
  after=0.201495 before=0.0814067 target=0.2
  stats: n=3000 min=0.185765 mean=0.201495 p50=0.200497 p95=0.208028 p99=0.210752 max=0.233195
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

Attempt 2 (job 111410540820, slow shared runner — the observed worst
case):

```text
budget=iso_depthkey_rebuild result=FAIL metric=mean unit=ms
  after=0.266709 before=0.0814067 target=0.2
  stats: n=3000 min=0.184 mean=0.266709 p50=0.267799 p95=0.276597 p99=0.282203 max=0.300107
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100

budget=iso_depthkey_rebuild result=FAIL metric=mean unit=ms
  after=0.267328 before=0.0814067 target=0.2
  stats: n=3000 min=0.254928 mean=0.267328 p50=0.266224 p95=0.279408 p99=0.283082 max=0.29891
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

## Before/after (CORE-001)

| Quantity | Before (M2-ISO-02 workload fix) | After (this revision) |
|---|---|---|
| PRD §8.1 target (mean) | 0.2 ms | **0.3 ms** |
| `budgets.json` `target` | 0.2 | **0.3** |
| `budgets.json` `measured` | 0.0814067 (local canonical g++ -O0) | **0.202204** (CI reference lane, worse backend) |
| Reference-lane observed range | 0.20889 ms (pre-fix, harness artifact) | 0.194–0.267 ms (engine unchanged) |

## Interpretation

- **The gate is calibrated, not relaxed:** the target is now 48% above
  the latest recorded reference-lane value and covers every observed
  reference-lane mean with ≥ 12% margin. A future engine change that
  regresses `setTile` by > 10% against `before = 0.202204` still trips
  the 10% band (methodology §5), and a regression beyond 0.3 ms trips
  the absolute gate.
- **Lesson (extends the workload-fix lesson):** a budget target set
  from local-machine runs must be verified with margin on the CI
  reference toolchain before it guards master — otherwise the gate is
  a coin flip and CI failures stop tracking engine regressions.
- **Tail behavior** of the recorded runs: p99 ≈ 1.05–1.06× the mean;
  the outlier run's max (0.300107 ms) is a single-iteration
  scheduler/preemption spike — the mean (the gate's metric) is 12%
  below the new bar even there.
- **Regression policy:** `budgets.json` `measured = 0.202204`; the 10%
  regression band (methodology §5) applies to future re-measurements
  on the reference platform; `target = 0.3 ms` is the absolute gate.

## Open items

- Same as `m2-iso-depth-table-workload-fix.md`: **M2-TILE-01** wires
  the tilemap height grid to this table; **M2-SORT-01 /
  M2-SPRITE-01/02** consume `keyAt` for batched depth-ordered
  submission (both already landed).
- If the gate ever fails the reference lane above 0.3 ms, the evidence
  indicates reference-runner degradation — the remedy is another PRD
  revision through this same process, not a silent target move.
