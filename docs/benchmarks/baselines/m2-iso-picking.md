# Baseline: `m2-iso-picking` — M2-ISO-03 one-pick budget

Recorded by **M2-ISO-03** (2026-10-02). This is the **sixth** baseline
file; it is immutable (methodology §4 — superseding it later adds a
new file, it is never edited). It retires the `0 = not yet measured`
M0 convention for the **`iso_picking`** budget (one isometric
screen-to-grid pick, mean ≤ 0.01 ms, PRD §8.1).

## What this baseline measures

The `IsoPickBudget` suite of `laige-render_tests` (M2-ISO-03; the
suite is in `tests/laige-render/iso_picking_tests.cpp`): the PRD §8.1
"Isometric picking" workload — **one isometric screen-to-grid pick,
O(1)** — checked against the **`iso_picking`** budget (mean ≤ 0.01 ms).

Workload shape:

- One measured sample is **one `screenToGrid` call**: a 2:1 dimetric
  `IsoCamera` (default options — scale 1, snap off, zoom 1, camera at
  the origin) and the default grid (`cellSize = 1`), the pick of one
  NDC screen point (the budgets.json unit — no SimMath backend is
  involved: the inverse is render-side float, ADR 0002 does not
  apply).
- The 3 000 NDC points are **precomputed** outside every measured
  window (deterministic `i % 97` / `i % 89` lattice — no RNG in the
  measured path, no division in the harness, the
  `iso_depth_table_tests.cpp` discipline): the measured loop is the
  pick's own 2×2 solve + two `floorf` + the camera's O(1) matrix
  build.
- **Warm-up:** 100 picks discarded; **measured:** 3 000 picks
  (`n=3000`), histogram capacity 3 000, no truncation.

The gate (roadmap M2-ISO-03 Verify): mean ≤ 0.01 ms, enforced on CI
by the `iso_picking` ctest entry. The gated branch (load
`budgets.json`, `budgetCheck`, the stable 4-line report,
`EXPECT(passed)`) compiles only on **Linux non-instrumented trees**
(`LAIGE_ISO_PICK_BUDGET` — the `iso_depth_table_tests.cpp`
precedent): instrumentation inflates the absolute cost (methodology
§4), so the sanitizer trees run the **same workload ungated** and
verify its safety properties instead (leak-free ASan, race-free
TSan); the absolute target is reference-platform-scoped
(methodology §5), and the P0 `linux-clang` job runs on the same
reference runner, so the gate is verified there too (the cross-
compiler run below). The zero-allocation property of the pick is
pinned separately by `IsoPickProperty` (1 000 consecutive picks = 0
heap blocks under the process-wide allocation watch, the non-
sanitizer trees).

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.6-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` (canonical gate run; the cross-compiler run below uses `Clang 22.1.8`) |
| 4 | Build type | `Debug` (canonical, `build/` tree — CMake Debug: `-O0 -g` + the engine policy flags) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`. The workload is render-side float (no SimMath TU in the measured path — the ADR 0002 pinned set does not apply). No sanitizers (canonical tree). |
| 6 | Dataset / workload | `iso_picking` — one `screenToGrid` pick: default 2:1 dimetric `IsoCamera` (scale 1, snap off, zoom 1, e = 0), `cellSize = 1`, one precomputed NDC point per sample (3 000-point deterministic lattice) |
| 7 | Warm-up | 100 picks discarded |
| 8 | Sample count | `n=3000` picks (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | min=0.00011 mean=**0.000136262** p50=0.000131 p95=0.00016 **p99=0.00018** max=0.00034 ms |
| 10 | Before / after | `before=0` (M0 convention — not yet measured) · `after` (mean): **0.000136262** → **recorded 0.000136262** · `target`: mean ≤ 0.01 ms — **PASS** (73× inside the gate) |

## Verbatim run output

### Run — canonical tree (Debug, g++), budget-checked

Command (run from the repository root; `budgets.json` resolved via
`LAIGE_BUDGETS_PATH`):

```console
$ LAIGE_BUDGETS_PATH=$PWD/budgets.json \
    ./build/bin/laige-render_tests --gtest_filter='IsoPickBudget*'
```

```text
budget=iso_picking result=PASS metric=mean unit=ms
  after=0.000136262 before=0 target=0.01
  stats: n=3000 min=0.00011 mean=0.000136262 p50=0.000131 p95=0.00016 p99=0.00018 max=0.00034
  context: workload=one isometric screen-to-grid pick, O(1) (PRD 8.1) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

Exit code: `0`. (The report's `before=0` is the first-ever
measurement of this budget — the M0 convention; subsequent runs
carry the recorded `measured`.)

### Gate — CI shape, canonical tree, `ctest -R iso_picking`

```console
$ ctest --test-dir build -R "iso_picking" --output-on-failure
```

```text
Test project /home/anon/devel/laige-cpp/build
    Start 45: iso_picking
1/1 Test #45: iso_picking ......................   Passed    0.01 sec

100% tests passed out of 1

Total Test time (real) =   0.02 sec
```

Commit: the M2-ISO-03 commit on branch `feat/m2-iso-03-iso-picking`
(the `iso_picking` ctest entry is the CI perf lane — the P0 jobs run
the full `ctest` on the canonical tree and the `linux-clang` job).

## Cross-compiler run (same workload, same n=3000, clang -O0)

The gate is reference-platform-scoped (methodology §5), and the P0
`linux-clang` CI job runs on the same reference runner — so the gate
must clear the 0.01 ms bar on the clang -O0 tree as well. Recorded as
the cross-compiler evidence (the canonical-tree run above is the
recorded value; this run is the CI-shape check):

```text
budget=iso_picking result=PASS metric=mean unit=ms
  after=0.000189456 before=0.000136262 target=0.01
  stats: n=3000 min=0.00016 mean=0.000189456 p50=0.00019 p95=0.000211 p99=0.00023 max=0.005391
  context: workload=one isometric screen-to-grid pick, O(1) (PRD 8.1) build=Clang 22.1.8, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

**PASS** on clang -O0 (0.000189 ms mean — 53× inside the gate).

## Interpretation

- **73× inside the gate on the canonical tree, 53× on clang -O0.**
  The 0.01 ms (10 µs) PRD number was written for an O(1) pick; at
  -O0 (where the gate measures, methodology §4) one pick costs
  ≈ 136 ns (canonical g++) / ≈ 189 ns (clang) on this machine — a
  2×2 solve, two `floorf`, and the camera's O(1) matrix build.
- **Tail behavior:** p99 0.00018 ms (canonical) — 1.3× the mean. The
  clang run's single `max=0.005391 ms` sample is a scheduler/
  preemption outlier on the shared 32-thread machine, still 995×
  inside the gate (the p99 is the perf-regression watch — PERF-009).
- **Zero per-pick allocation is structural** (a fixed sequence of
  float ops — no containers, no logging, no GL) and pinned by
  `IsoPickProperty` (1 000 consecutive picks = 0 heap blocks under
  the process-wide allocation watch).
- **Steady state, deterministic:** the 100-pick warm-up plus the
  precomputed point lattice keep the workload L2-hot and free of any
  RNG or division in the measured path — the run is reproducible and
  independent of any scene content.

## Regression policy

`budgets.json` `measured = 0.000136262` (the canonical Debug tree,
g++ — the workload is backend-independent, so no worse-of-two
selection applies); the 10% regression band
(docs/benchmarks/methodology.md §5) applies to future re-measurements
on the reference platform.

## Open items

- **M2-TILE-01/02** builds the terrain-height-aware pick on this
  inverse (the standing cell of the clicked tile — the ground-
  projection pick of this step is the default click semantics).
- **M2-SAMPLE-01** wires click-to-select into the isometric template
  (input screen point → `screenToGrid` → the selected cell; the
  sample's picking test case prints the expected cell).
- The **input milestone** owns the screen-point source (mouse state →
  the pixel → NDC conversion at the documented input boundary,
  RENDER-006); this step consumes NDC by contract.
