# Baseline: `m2-iso-depth-table` — M2-ISO-02 depth-key-table budget

Recorded by **M2-ISO-02** (2026-09-30). This is the **fifth** baseline
file; it is immutable (methodology §4 — superseding it later adds a
new file, it is never edited). It is also the **first M2 baseline**
and the first budget besides `sim_tick_*` to update `budgets.json`
`measured`.

## What this baseline measures

The `IsoDepthTableBudget` suite of `laige-render_tests` (M2-ISO-02):
the PRD §8.1 "Isometric depth key rebuild (10k dirty cells after a
terrain edit)" workload — **10 000 dirty cells** — checked against
the **`iso_depthkey_rebuild`** budget (mean ≤ 0.2 ms) on **both**
SimMath backends (ADR 0002: `fixed_point_16_16`, the default, and
`float_pinned_32`).

Workload shape (the suite is in
`tests/laige-render/iso_depth_table_tests.cpp`):

- A **128×128 tile grid** (16 384 covered cells — 64 chunks of 16×16,
  the chunk-aligned superset of the grid) created with
  `IsoDepthKeyTable<Backend>::create` (origin (0,0), flat ground),
  then filled by `rebuild` with the deterministic terrain height
  `(7·gx + 11·gy) % 5` (a mixed low relief — heights 0..4 — that
  exercises the full in-domain quantization without ever touching
  the saturating edge).
- **One measured iteration = 10 000 `setTile` calls** dirtying a
  100×100 block starting at (14,14) — column-major `i`-order (the
  batch order a terrain edit would apply in) — each cell's new
  height `(gx+gy) % 5`, the steady state of repeated edits (no cell
  is written twice within one iteration).
- **Warm-up:** 100 iterations discarded; **measured:** 3 000
  iterations (`n=3000`), histogram capacity 3 000, no truncation.
- One sample is one 10 000-call iteration — the mean per 10k dirty
  cells is the PRD budget's unit.
- **Zero allocations:** the `IsoDepthTableUpdate` suite separately
  asserts 100 consecutive successful `setTile` calls = 0 allocations
  under the process-wide allocation watch (the non-sanitizer
  trees).

The gate (roadmap M2-ISO-02): mean ≤ 0.2 ms on **both backends**,
enforced on CI by the `iso_depth_table` ctest entry. The gated
branch (load `budgets.json`, `budgetCheck`, the stable 4-line
report, `EXPECT(passed)`) compiles only on **Linux non-instrumented
trees** (`LAIGE_ISO_DEPTH_BUDGET`): instrumentation inflates the
update's absolute cost (the m1-profiler-cost baseline precedent), so
the sanitizer trees run the **same workload ungated** and verify its
safety properties instead (leak-free ASan, race-free TSan); the
absolute target is reference-platform-scoped (methodology §5), and
the P0 `linux-clang` job runs on the same reference runner, so the
gate is verified there too (the cross-compiler run below).

**This is a `budgets.json` workload:** this baseline updates
`measured` for `iso_depthkey_rebuild` (the M0 convention's
`0 = not yet measured` is retired by this measurement). The recorded
value is the **worse (max) of the two backends** on the canonical
Debug tree — the budget covers both backends (ADR 0002: both must
pass), and recording the worse keeps the 10% regression band
(docs/benchmarks/methodology.md §5) conservative.

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.6-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` (canonical gate run; the cross-compiler run below uses `Clang 22.1.8`) |
| 4 | Build type | `Debug` (canonical, `build/` tree — CMake Debug: `-O0 -g` + the engine policy flags) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002) on the engine TUs: `-ffp-contract=off -fno-associative-math`. No sanitizers (canonical tree). |
| 6 | Dataset / workload | `iso_depth_table` — 128×128 grid (16 384 cells, 64 chunks), terrain `(7gx+11gy)%5`; one iteration = 10 000 `setTile` calls over a 100×100 block at (14,14), column-major, height `(gx+gy)%5`; both SimMath backends |
| 7 | Warm-up | 100 iterations discarded |
| 8 | Sample count | `n=3000` iterations per backend (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | `fpx16_16`: min=0.083809 mean=0.087398 p50=0.085683 p95=0.098877 **p99=0.107505** max=0.166777 ms · `fp32_pinned`: min=0.083769 mean=0.0864039 p50=0.085692 p95=0.090152 **p99=0.098938** max=0.130709 ms |
| 10 | Before / after | `before=0` (M0 convention — not yet measured) · `after` (mean): `fpx16=0.087398`, `fp32=0.0864039` → **recorded 0.087398** (worse of backends) · `target`: mean ≤ 0.2 ms — both backends **PASS** (2.3× inside the gate) |

## Verbatim run output

### Run — canonical tree (Debug, g++), both backends, budget-checked

Command (run from the repository root; `budgets.json` resolved via
`LAIGE_BUDGETS_PATH`):

```console
$ LAIGE_BUDGETS_PATH=$PWD/budgets.json \
    ./build/bin/laige-render_tests --gtest_filter='IsoDepthTableBudget*'
```

```text
budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.087398 before=0 target=0.2
  stats: n=3000 min=0.083809 mean=0.087398 p50=0.085683 p95=0.098877 p99=0.107505 max=0.166777
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine= warmup=100

budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.0864039 before=0 target=0.2
  stats: n=3000 min=0.083769 mean=0.0864039 p50=0.085692 p95=0.090152 p99=0.098938 max=0.130709
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

Exit code: `0`. (The report's `before=0` is the first-ever
measurement of this budget — the M0 convention; subsequent runs
carry the recorded `measured`.)

### Gate — CI shape, canonical tree, `ctest -R iso_depth_table`

```console
$ ctest --test-dir build -R "iso_depth_table" --output-on-failure
```

```text
Test project /home/anon/devel/laige-cpp/build
    Start 41: iso_depth_table
1/1 Test #41: iso_depth_table ..................   Passed    0.55 sec

100% tests passed out of 1

Total Test time (real) =   0.55 sec
```

Commit: the M2-ISO-02 commit on branch `m2-iso-02-depth-key-table`
(the `iso_depth_table` ctest entry is the CI perf lane — the P0
jobs run the full `ctest` on the canonical tree and the `linux-clang`
job).

## Cross-compiler run (same workload, same n=3000, clang -O0)

The gate is reference-platform-scoped (methodology §5), and the P0
`linux-clang` CI job runs on the same reference runner — so the
gate must clear the 0.2 ms bar on the clang -O0 tree as well.
Recorded as the cross-compiler evidence (the canonical-tree run
above is the recorded value; this run is the CI-shape check):

```text
budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.149184 before=0.087398 target=0.2
  stats: n=3000 min=0.14764 mean=0.149184 p50=0.148612 p95=0.1526 p99=0.153903 max=0.171837
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 22.1.8, CMake Debug, engine policy (NFR-8.10) machine= warmup=100

budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.149168 before=0.087398 target=0.2
  stats: n=3000 min=0.14735 mean=0.149168 p50=0.148562 p95=0.15253 p99=0.153802 max=0.160706
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 22.1.8, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

Both backends PASS on clang -O0 (0.149 ms mean — 34% inside the
gate).

## Measurement history (CORE-001)

The recorded numbers are the end of a measured design iteration, not
the first cut:

1. **Per-chunk storage, flat `setTile`** (the first design): 0.326 ms
   mean on canonical gcc -O0 (FAIL), 0.239 / 0.212 ms on clang -O0
   (FAIL). The -O0 gate made `setTile`'s helper calls and the
   `std::vector<Chunk>` addressing real per-call costs.
2. **Flat storage + cached raw pointer** (the shipped design): one
   pre-sized allocation for the covered region; the update path is a
   compare-only coverage test, one flat index, and a direct
   `CellRecord` write — no container calls (`unique_ptr::operator[]`
   is a six-level call chain on the -O0 trees). Result: **0.0874 /
   0.1492 ms** on the two gate-relevant trees — both PASS with
   margin.

## Interpretation

- **2.3× inside the gate on the canonical tree, 34% on clang
  -O0.** The 0.2 ms PRD number was written for an optimized
  implementation; at -O0 (where the gate measures, methodology §4)
  the flat update costs ≈ 8.7 ns per cell on this machine — well
  inside the 200 ns/cell the budget allows.
- **The workload is steady-state, not cold.** The 100 warm-up
  iterations plus the repeated 100×100 block keep the 192 KiB cell
  storage L2-hot; a cold-miss-dominated run would be slower. The
  block placement (14,14) and the deterministic terrain keep the
  run reproducible and independent of any scene content.
- **Both backends agree to < 1.2%** (0.087398 vs 0.0864039) —
  `setTile` is backend-independent by design (it stores the
  quantized ground contribution once at creation and then does
  integer math only); the backends appear only in `create`/
  `rebuild`/growth.
- **Tail behavior:** p99 0.1075 ms (fpx16) / 0.0989 ms (fp32) —
  1.2–1.3× the mean; the max (0.1668 / 0.1307 ms) is a
  scheduler/preemption outlier on a shared 32-thread machine, still
  far inside the 0.2 ms gate (the p99 is the perf-regression watch —
  PERF-009).
- **Regression policy:** `budgets.json` `measured = 0.087398` (the
  worse of the two backends); the 10% regression band
  (methodology §5) applies to future re-measurements on the
  reference platform.

## Open items

- **M2-TILE-01** wires the tilemap height grid to this table (the
  tile quad's depth is the `keyAt` of the tile's standing cell;
  terrain edits call `setTile` per dirty tile).
- **M2-SORT-01 / M2-SPRITE-01/02** consume `keyAt` for batched
  depth-ordered submission; moving sprites keep per-frame
  `isoDepthKey` (M2-ISO-01) — the table is the terrain's
  precomputation, not a per-sprite cache.
