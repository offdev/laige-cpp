# Baseline: `m2-particle-render-budget-rebaseline` — re-baselined
# `particle_render_10k` target (2.0 → 3.0 ms)

Recorded by the **M2-PART-02 budget revision** (2026-10-08, CI follow-up
to M2-PART-02). This is the **eleventh** baseline file; it is immutable
(methodology §4 — superseding it later adds a new file, it is never
edited). It **supersedes**
[`m2-particle-render.md`](m2-particle-render.md) as the latest recorded
value of `particle_render_10k` (that baseline stays in place as the
before number of this before/after pair; `budgets.json` `measured` is
updated by this baseline, and the absolute `target` is revised in the
same change). `particle_render_10k` is a **step-level budget** (not a
PRD §8.1 table row — the `depth_sort_10k` precedent), so no PRD revision
accompanies this change; the revision is recorded in the milestone
change log (methodology §5, DOC-007).

## Why this baseline exists

The `particle_render_10k` gate (mean ≤ 2.0 ms, 10 000-particle declare
pass, the `particle_render` ctest entry) failed the CI `Linux x64
(clang++)` reference lane on **every merge to master since M2-PART-02
landed**, on unchanged engine code:

| CI run (all `Linux x64 (clang++)` — Clang 18.1.3, CMake Debug, ubuntu-24.04 shared runner) | fpx16_16 mean | fp32_pinned mean | result |
|---|---|---|---|
| 37780159655 (2026-10-08, master `bdb323b` — the M2-PART-02 merge) | 2.67853 ms | 1.88209 ms | FAIL (worse backend fpx16_16) |
| 37782008880 (2026-10-08, master `2f2c5a9` — the M0-CI-01 merge) | 2.62795 ms | 1.94712 ms | FAIL (worse backend fpx16_16) |
| 37850579155 (2026-10-08, master `4f81866` — the M2-TEXT-01 merge) | 1.91031 ms | 2.07786 ms | FAIL (worse backend fp32_pinned) |

The reference lane measures this workload at **1.91–2.68 ms** —
consistently above the 2.0 ms bar, never below it. Evidence that this is
a calibration gap and not an engine regression:

- The `declareParticles` path is unchanged since M2-PART-02 (PR #83,
  2026-10-08): the two merges between these recordings (M0-CI-01 — CI
  workflow configuration only; M2-TEXT-01 — the font atlas) do not touch
  `particle_render.h` or the batcher.
- The `Linux x64 (g++)` lane of run 37850579155 **passed** the same
  gated workload on the same commit (`4f81866`) — the g++ toolchain on
  the same runner sits under the bar.
- The recorded local numbers (1.07798 ms canonical g++ 16.2.1 -O0,
  1.18578 ms local Clang 23.1.1 -O0 — `m2-particle-render.md`) predate
  the gate's verification on the CI reference toolchain: the 2.0 ms bar
  was calibrated ≈1.86×/1.69× the local measurements, but the CI
  runner's **Clang 18.1.3** (the ubuntu-24.04 apt package — not the
  local Clang 23.1.1) on the shared runner measures ≈1.9–2.7× the local
  canonical number. The margin never existed on the gate platform — the
  exact M2-ISO-02 rebaseline lesson ("a budget target set from
  local-machine runs must be verified with margin on the CI reference
  toolchain before it guards master — otherwise the gate is a coin flip
  and CI failures stop tracking engine regressions").

## The revision

Per methodology §5 ("accepted budget revisions change `budgets.json` in
the same PR … and are noted in the milestone change log (DOC-007) — a
budget number is never silently moved"):

- **`budgets.json`**: `target` 2.0 → **3.0**; `measured` 1.07798 (local
  canonical tree) → **2.07786** — the latest recorded value, the worse
  of the two backends on the CI reference lane, run 37850579155
  (2026-10-08, commit `4f81866`).
- **PRD: unchanged.** `particle_render_10k` is a step-level budget (not
  a PRD §8.1 table row — the `depth_sort_10k` precedent), so no PRD
  revision is required; the revision is recorded in the milestone
  change log.
- **Workload, engine code, measurement method: unchanged.** This is a
  budget re-baseline, not a benchmark change (methodology §1) — the
  workload is still the 10 000-particle declare pass, 100 warm-up +
  3 000 measured iterations, mean metric, both SimMath backends, the
  declare pass ONLY (`beginFrame`/`build` outside the measured window).

**Margin:** 3.0 ms sits 44% above the latest recorded reference-lane
value (2.07786) and 12.0% above the worst observed reference-lane mean
(2.67853, the slow-runner run of the M2-PART-02 merge). The gate's
metric is the **mean** (n=3000, warmup=100): single-iteration spikes
(worst observed max 3.37746 ms, run 37782008880) do not drive the gate.

## AGENTS §12 metadata (the recorded run)

| # | Field | Value |
|---|---|---|
| 1 | Hardware | GitHub Actions ubuntu-24.04 hosted runner (2 vCPU shared) — the CI reference machine |
| 2 | OS | ubuntu-24.04 |
| 3 | Compiler and version | Clang 18.1.3 (runner apt package), CMake Debug |
| 4 | Build type | `Debug` (`-O0 -g` + engine policy flags) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002) on the engine TUs |
| 6 | Dataset / workload | `particle_render_10k` — one `declareParticles` over 10 000 live particles (seed `0xC0FFEE00`, one velocity-box emitter, 10 update ticks, both SimMath backends — the conversion pass ONLY) |
| 7 | Warm-up | 100 declare+build frames discarded |
| 8 | Sample count | `n=3000` declare passes per backend (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | see the verbatim reports below (per run) |
| 10 | Before / after | `before=1.07798` (the M2-PART-02 recorded value, canonical local tree) · `after` (mean, worse backend, run 37850579155) = 2.07786 → **recorded 2.07786** · `target`: 2.0 → **3.0 ms** |

Commit measured on: `4f81866` (master, the M2-TEXT-01 merge commit — the
run that motivated this revision).

## Verbatim run output (CI reference lane)

### Run 37850579155, `laige-render_tests` ctest entry, commit `4f81866`

```text
budget=particle_render_10k result=PASS metric=mean unit=ms
  after=1.91031 before=1.07798 target=2
  stats: n=3000 min=1.84471 mean=1.91031 p50=1.90473 p95=1.96429 p99=1.99178 max=3.1355
  context: workload=10 000-particle particle-to-sprite conversion, one O(n) declare pass (M2-PART-02) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

```text
budget=particle_render_10k result=FAIL metric=mean unit=ms
  after=2.07786 before=1.07798 target=2
  stats: n=3000 min=1.88252 mean=2.07786 p50=2.0697 p95=2.19037 p99=2.26337 max=2.77443
  context: workload=10 000-particle particle-to-sprite conversion, one O(n) declare pass (M2-PART-02) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

### Run 37780159655, `particle_render` ctest entry, commit `bdb323b`
(worst observed mean)

```text
budget=particle_render_10k result=FAIL metric=mean unit=ms
  after=2.67853 before=1.07798 target=2
  stats: n=3000 min=2.49206 mean=2.67853 p50=2.68158 p95=2.72115 p99=2.74836 max=3.00505
  context: workload=10 000-particle particle-to-sprite conversion, one O(n) declare pass (M2-PART-02) build=Clang 18.1.3, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

(the second backend of the same entry: `after=1.88209`, stats
`min=1.82838 p50=1.88027 p95=1.90874 p99=1.93179 max=2.36404`)

## Before/after (CORE-001)

| Quantity | Before (M2-PART-02) | After (this revision) |
|---|---|---|
| `budgets.json` `target` | 2.0 ms | **3.0 ms** |
| `budgets.json` `measured` | 1.07798 (local canonical g++ -O0) | **2.07786** (CI reference lane, worse backend) |
| Reference-lane observed range | n/a (the gate never cleared the bar) | 1.91–2.68 ms (engine unchanged) |
| Local canonical (g++ 16.2.1 -O0) | 1.07798 ms | unchanged (local machine) |
| Local cross-compiler (Clang 23.1.1 -O0) | 1.18578 ms | unchanged (local machine) |

## Interpretation

- **The gate is calibrated, not relaxed:** the target is now 44% above
  the latest recorded reference-lane value and covers every observed
  reference-lane mean with ≥ 12% margin. A future engine change that
  regresses `declareParticles` by > 10% against `before = 2.07786` still
  trips the 10% band (methodology §5), and a regression beyond 3.0 ms
  trips the absolute gate.
- **Lesson (confirms the M2-ISO-02 rebaseline lesson):** a budget target
  set from local-machine runs must be verified with margin on the CI
  reference toolchain before it guards master — the local Clang 23.1.1
  -O0 number (1.18578 ms) is not a proxy for the runner's Clang 18.1.3
  (1.91–2.68 ms), a ≈1.6–2.3× gap on the same workload.
- **Cross-backend spread on the reference lane is runner-dominated**
  (fpx16_16 worse by 35–43% in the two earlier runs, fp32_pinned worse
  by ≈9% in the latest): the recorded value is the worse of the two
  backends (methodology §5 convention); the backend ordering flipping
  between runs is shared-runner variance, not an engine divergence (the
  engine is bit-identical — the determinism KAT hashes are unchanged).
- **Regression policy:** `budgets.json` `measured = 2.07786`; the 10%
  regression band (methodology §5) applies to future re-measurements on
  the reference platform; `target = 3.0 ms` is the absolute gate.

## Open items

- If the gate ever fails the reference lane above 3.0 ms, the evidence
  indicates reference-runner degradation — the remedy is another budget
  revision through this same process (new baseline file + `budgets.json`
  + change log), not a silent target move.
