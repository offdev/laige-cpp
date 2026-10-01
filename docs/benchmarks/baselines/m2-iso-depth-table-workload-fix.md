# Baseline: `m2-iso-depth-table-workload-fix` — division-free
# `iso_depthkey_rebuild` workload

Recorded by the **M2-ISO-02 workload fix** (2026-10-01, follow-up to
M2-ISO-02). This is the **sixth** baseline file; it is immutable
(methodology §4 — superseding it later adds a new file, it is never
edited). It **supersedes**
[`m2-iso-depth-table.md`](m2-iso-depth-table.md) as the latest recorded
value of `iso_depthkey_rebuild` (that baseline stays in place as the
before number of this before/after pair; `budgets.json` `measured` is
updated by this baseline).

## Why this baseline exists

The CI `Linux x64 (clang++)` reference lane (run 36885653175, job
110448173121, 2026-10-01 — the merge of M2-CAM-01 to master) failed
`IsoDepthTableBudget.TenThousandDirtyCells`: mean **0.20889 ms**
(fpx16_16) / **0.209044 ms** (fp32_pinned) against the 0.2 ms budget.
Root cause (disassembly of the CMake-Debug `-O0` build): the measured
window's `applyEdit` loop computed `i / 100`, `i % 100` and
`(gx + gy) % 5` per call, and at `-O0` that is **three real
`div`/`idiv` instructions per iteration** (two ~20-30-cycle 32-bit
`div`s plus one `idiv`, on top of the `setTile` call) — the gate was
measuring the *harness's* division codegen, not the engine's 10 000
`setTile` calls. The `m2-iso-depth-table.md` baseline's "34% inside the
gate on clang -O0" (0.149184 ms) was recorded on this machine with
**Clang 22.1.8**; the CI lane uses the ubuntu-24.04 runner's apt
**Clang 18.1.3** on a slower shared runner — the CI number is
~1.40× the local one, over the 0.2 ms bar (0.2089 / 0.149184).

The fix (same change as this baseline): the 10 000
`(tileX, tileY, height)` edit triples are **precomputed once outside
every measured window** (the warm-up and the measured runs both start
below the precomputation), and the measured loop iterates the
precomputed sequence — **byte-identical `setTile` call sequence**
(same order, same arguments) with **zero integer division** (verified
by disassembly of the fixed `-O0` closure: 0 `div`/`idiv` in the
measured loop; the only per-call calls are `setTile` and
`Status::ok()`). The workload definition (10 000 dirty cells after a
terrain edit, 100×100 block at (14,14), column-major, height
`(gx+gy) % 5`) and the 0.2 ms target are **unchanged** — this removes
a harness artifact from the measured window, it does not relax the
budget or change the workload's engine work (methodology §1: the
benchmark is not changed to make a regression disappear — no
regression occurred; the artifact had never been calibrated on the
CI clang-18 lane).

## What this baseline measures

The `IsoDepthTableBudget` suite of `laige-render_tests` (M2-ISO-02,
workload fixed by this change): the PRD §8.1 "Isometric depth key
rebuild (10k dirty cells after a terrain edit)" workload — **10 000
dirty cells** — checked against the **`iso_depthkey_rebuild`** budget
(mean ≤ 0.2 ms) on **both** SimMath backends (ADR 0002:
`fixed_point_16_16`, the default, and `float_pinned_32`).

Workload shape (the suite is in
`tests/laige-render/iso_depth_table_tests.cpp`):

- A **128×128 tile grid** (16 384 covered cells — 64 chunks of 16×16)
  created with `IsoDepthKeyTable<Backend>::create` (origin (0,0), flat
  ground), filled by `rebuild` with the deterministic terrain height
  `(7·gx + 11·gy) % 5`.
- **One measured iteration = 10 000 `setTile` calls** dirtying a
  100×100 block starting at (14,14) — column-major (tileX the outer
  index, tileY the inner — the same order the original `i`-loop
  produced) — each cell's new height `(gx+gy) % 5`. The (tileX, tileY,
  height) sequence is precomputed once per run, outside every measured
  window (this change).
- **Warm-up:** 100 iterations discarded; **measured:** 3 000
  iterations (`n=3000`), histogram capacity 3 000, no truncation.
- One sample is one 10 000-call iteration — the mean per 10k dirty
  cells is the PRD budget's unit.

The gate (mean ≤ 0.2 ms on both backends) is enforced on CI by the
`iso_depth_table` ctest entry on the Linux non-instrumented trees
(`LAIGE_ISO_DEPTH_BUDGET`); the sanitizer trees run the same workload
ungated; the absolute target is reference-platform-scoped
(methodology §5) — both P0 Linux lanes (g++ and clang++) run on the
ubuntu-24.04 reference runner, so the gate must clear there on both
toolchains.

**This is a `budgets.json` workload:** this baseline updates
`measured` for `iso_depthkey_rebuild` — the recorded value is the
**worse (max) of the two backends** on the canonical Debug tree
(0.0814067 ms).

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.6-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` (canonical gate run; the cross-compiler run below uses `Clang 22.1.8`) |
| 4 | Build type | `Debug` (canonical, `build/` tree — CMake Debug: `-O0 -g` + the engine policy flags) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002) on the engine TUs: `-ffp-contract=off -fno-associative-math`. No sanitizers (canonical tree). |
| 6 | Dataset / workload | `iso_depth_table` — 128×128 grid (16 384 cells, 64 chunks), terrain `(7gx+11gy)%5`; one iteration = 10 000 `setTile` calls over a 100×100 block at (14,14), column-major, height `(gx+gy)%5`, edit sequence precomputed outside the measured window; both SimMath backends |
| 7 | Warm-up | 100 iterations discarded |
| 8 | Sample count | `n=3000` iterations per backend (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | `fpx16_16`: min=0.07868 mean=0.0814067 p50=0.080282 p95=0.086093 **p99=0.090812** max=0.195461 ms · `fp32_pinned`: min=0.0789 mean=0.0807234 p50=0.080382 p95=0.08424 **p99=0.085682** max=0.097866 ms |
| 10 | Before / after | `before=0.087398` (the M2-ISO-02 recorded value — the old workload shape) · `after` (mean): `fpx16=0.0814067`, `fp32=0.0807234` → **recorded 0.0814067** (worse of backends) · `target`: mean ≤ 0.2 ms — both backends **PASS** (2.5× inside the gate) |

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
  after=0.0814067 before=0.087398 target=0.2
  stats: n=3000 min=0.07868 mean=0.0814067 p50=0.080282 p95=0.086093 p99=0.090812 max=0.195461
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine= warmup=100

budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.0807234 before=0.087398 target=0.2
  stats: n=3000 min=0.0789 mean=0.0807234 p50=0.080382 p95=0.08424 p99=0.085682 max=0.097866
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

Exit code: `0`.

### Gate — CI shape, canonical tree, `ctest -R iso_depth_table`

```console
$ ctest --test-dir build -R "iso_depth_table" --output-on-failure
```

```text
Test project /home/anon/devel/laige-cpp/build
    Start 41: iso_depth_table
1/1 Test #41: iso_depth_table ..................   Passed    0.53 sec

100% tests passed out of 1

Total Test time (real) =   0.54 sec
```

Commit: the M2-ISO-02 workload-fix commit on branch
`m2-iso-02-budget-workload-fix`.

## Cross-compiler run (same workload, same n=3000, clang -O0)

The gate must clear the 0.2 ms bar on the clang -O0 tree as well (the
P0 `linux-clang` CI job — the reference runner's apt Clang
18.1.3, not this machine's Clang 22.1.8; the CI lane is the ultimate
check, recorded in the merge-lane run of this fix). Local Clang 22.1.8
evidence:

```text
budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.138931 before=0.087398 target=0.2
  stats: n=3000 min=0.136169 mean=0.138931 p50=0.138233 p95=0.143052 p99=0.145727 max=0.16854
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 22.1.8, CMake Debug, engine policy (NFR-8.10) machine= warmup=100

budget=iso_depthkey_rebuild result=PASS metric=mean unit=ms
  after=0.136335 before=0.087398 target=0.2
  stats: n=3000 min=0.133112 mean=0.136335 p50=0.135748 p95=0.140798 p99=0.143312 max=0.151387
  context: workload=10k dirty cells after a terrain edit (PRD 8.1) build=Clang 22.1.8, CMake Debug, engine policy (NFR-8.10) machine= warmup=100
```

Both backends PASS on clang -O0 (0.139 ms mean — 30% inside the gate
locally; the CI reference runner is ~1.4× this machine on the old
workload, which the division-free window no longer carries).

## Before/after (CORE-001)

| Tree (CMake Debug, -O0) | Before (old workload) | After (division-free) |
|---|---|---|
| canonical g++ 16.2.1 (worse backend) | 0.087398 ms | 0.0814067 ms |
| clang 22.1.8 (worse backend) | 0.149184 ms | 0.138931 ms |
| **CI clang 18.1.3 reference lane** | **0.20889 ms — FAIL** | pending the merge-lane run of this fix |

The local deltas (−7% / −7%) are smaller than the CI artifact because
this machine's out-of-order core partially overlaps the `div`
latency; on the CI reference runner the divisions were critical-path,
which is exactly why the artifact only failed there. The engine's
`setTile` cost is unchanged — this change touches the test harness
only (no engine code, no API, no target).

## Interpretation

- **2.5× inside the gate on the canonical tree, 30% on local clang
  -O0.** At -O0 (where the gate measures, methodology §4) the division-
  free update costs ≈ 8.1 ns per cell on this machine — well inside
  the 20 ns per cell the 0.2 ms budget allows for 10 000 calls.
- **The measured window is now the engine's cost, nothing else:** the
  fixed `-O0` measured loop contains 0 `div`/`idiv` instructions
  (disassembly-verified); its per-call work is the `setTile` call
  (boundary check, flat index, one `CellRecord` write, the key
  recompute) plus the `Status::ok()` check a real caller performs.
- **Both backends agree to < 1%** (0.0814067 vs 0.0807234) — `setTile`
  is backend-independent by design (it stores the quantized ground
  contribution once at creation and then does integer math only).
- **Tail behavior:** p99 0.0908 ms (fpx16) / 0.0857 ms (fp32) — ~1.1×
  the mean; the fpx16 max (0.1955 ms) is a scheduler/preemption
  outlier on a shared 32-thread machine, the mean (the gate's
  metric) is 2.4× below the 0.2 ms bar (PERF-009: p99 stays the
  perf-regression watch).
- **Regression policy:** `budgets.json` `measured = 0.0814067` (the
  worse of the two backends); the 10% regression band (methodology
  §5) applies to future re-measurements on the reference platform.
- **Lesson (recorded for the next budget gate):** the
  `m2-iso-depth-table.md` baseline's cross-compiler evidence was
  collected on this machine's Clang 22.1.8, while the CI `linux-clang`
  lane uses the runner's apt Clang 18.1.3 on a slower shared runner —
  the gate's platform is the CI lane itself (methodology §5: "CI is
  the gate"), so first-crossings of a budget gate on a new CI
  toolchain must be verified there, not only on the local
  reference-class tree.

## Open items

- Same as `m2-iso-depth-table.md`: **M2-TILE-01** wires the tilemap
  height grid to this table (terrain edits call `setTile` per dirty
  tile); **M2-SORT-01 / M2-SPRITE-01/02** consume `keyAt` for batched
  depth-ordered submission.
