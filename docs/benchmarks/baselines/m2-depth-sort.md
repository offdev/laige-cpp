# Baseline: `m2-depth-sort` — M2-SORT-01 10k-sort budget

Recorded by **M2-SORT-01** (2026-10-03). This is the **seventh**
baseline file; it is immutable (methodology §4 — superseding it later
adds a new file, it is never edited). It retires the `0 = not yet
measured` M0 convention for the **`depth_sort_10k`** budget (10 000
32-bit depth keys, one stable radix sort, mean ≤ 1.0 ms, PRD §8.1 —
the AC-4.3 "10k sprites sorted per frame at 60 FPS" cost).

## What this baseline measures

The `DepthSortBudget` suite of `laige-render_tests` (M2-SORT-01; the
suite is in `tests/laige-render/depth_sort_tests.cpp`): the PRD §8.1
"10k sprites sorted per frame" workload — **one stable radix sort of
10 000 precomputed 32-bit depth keys** — checked against the
**`depth_sort_10k`** budget (mean ≤ 1.0 ms).

Workload shape:

- One measured sample is **one `DepthSort::sort` call** over 10 000
  keys: a `DepthSort` created at capacity 10 000 (160 KB flat
  storage — 16 B/slot), the 4 × 8-bit LSD passes over contiguous
  pre-allocated buffers (no SimMath backend involved — the sort is
  pure 32-bit integer math, ADR 0002 does not apply).
- The 10 000 keys are **precomputed** outside every measured window
  (the deterministic splitmix64-style `mixKey` finalizer — no RNG in
  the measured path, no division in the harness, the
  `iso_depth_table_tests.cpp` discipline), carved to the M2-ISO-01
  22-bit fine-depth range (`mixKey(i) & 0x3FFFFF`): the keys carry a
  realistic isometric distribution (~2.4 equal keys per value among
  10k sprites — the equal-key/stability load of an overlapping
  scene), not a degenerate all-distinct or all-equal case.
- **Warm-up:** 100 sorts discarded; **measured:** 3 000 sorts
  (`n=3000`), histogram capacity 3 000, no truncation. The 3 000
  measured sorts are the roadmap's stress test ("10k sprites sorted
  per frame for 3 000 frames", part of AC-4.3).

The gate (roadmap M2-SORT-01 Verify): mean ≤ 1.0 ms, enforced on CI
by the `depth_sort` ctest entry. The gated branch (load
`budgets.json`, `budgetCheck`, the stable 4-line report,
`EXPECT(passed)`) compiles only on **Linux non-instrumented trees**
(`LAIGE_DEPTH_SORT_BUDGET` — the `iso_depth_table_tests.cpp` /
`iso_picking_tests.cpp` precedent): instrumentation inflates the
absolute cost (methodology §4), so the sanitizer trees run the
**same workload ungated** and verify its safety properties instead
(leak-free ASan, race-free TSan); the absolute target is
reference-platform-scoped (methodology §5), and the P0 `linux-clang`
job runs on the same reference runner, so the gate is verified there
too (the cross-compiler run below). The zero-allocation property of
the sort is pinned separately by `DepthSortProperty` (1 000
consecutive 10k sorts = 0 heap blocks under the process-wide
allocation watch, the non-sanitizer trees).

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.6-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` (canonical gate run; the cross-compiler run below uses `Clang 22.1.8`) |
| 4 | Build type | `Debug` (canonical, `build/` tree — CMake Debug: `-O0 -g` + the engine policy flags) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`. The workload is render-side 32-bit integer math (no SimMath TU in the measured path — the ADR 0002 pinned set does not apply). No sanitizers (canonical tree). |
| 6 | Dataset / workload | `depth_sort_10k` — one `DepthSort::sort` of 10 000 precomputed keys: capacity 10 000 (160 KB flat storage, 16 B/slot), keys = `mixKey(i) & 0x3FFFFF` (the M2-ISO-01 22-bit fine-depth range; ~2.4 equal keys per value — the realistic overlapping-scene distribution) |
| 7 | Warm-up | 100 sorts discarded |
| 8 | Sample count | `n=3000` sorts (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | min=0.203106 mean=**0.225852** p50=0.219948 p95=0.264653 **p99=0.28983** max=0.332742 ms |
| 10 | Before / after | `before=0` (M0 convention — not yet measured) · `after` (mean): **0.225852** → **recorded 0.225852** · `target`: mean ≤ 1.0 ms — **PASS** (4.4× inside the gate) |

## Verbatim run output

### Run — canonical tree (Debug, g++), budget-checked

Command (run from the repository root; `budgets.json` resolved via
`LAIGE_BUDGETS_PATH`; `LAIGE_BENCH_MACHINE` carries the operator's
machine description into the report's `context.machine` field):

```console
$ LAIGE_BUDGETS_PATH=$PWD/budgets.json \
    LAIGE_BENCH_MACHINE="AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM" \
    ./build/bin/laige-render_tests --gtest_filter='DepthSortBudget*'
```

```text
budget=depth_sort_10k result=PASS metric=mean unit=ms
  after=0.225852 before=0 target=1
  stats: n=3000 min=0.203106 mean=0.225852 p50=0.219948 p95=0.264653 p99=0.28983 max=0.332742
  context: workload=10 000 32-bit depth keys, one stable radix sort (M2-SORT-01, PRD 8.1) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine=AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM warmup=100
```

Exit code: `0`. (The report's `before=0` is the first-ever
measurement of this budget — the M0 convention; subsequent runs
carry the recorded `measured`.)

### Gate — CI shape, canonical tree, `ctest -R depth_sort`

```console
$ ctest --test-dir build -R "depth_sort" --output-on-failure
```

```text
Test project /home/anon/devel/laige-cpp/build
    Start 46: depth_sort
1/1 Test #46: depth_sort .......................   Passed    1.04 sec

100% tests passed out of 1

Total Test time (real) =   1.06 sec
```

Commit: the M2-SORT-01 commit on branch `feat/m2-sort-01-depth-sort`
(the `depth_sort` ctest entry is the CI perf lane — the P0 jobs run
the full `ctest` on the canonical tree and the `linux-clang` job).

## Cross-compiler run (same workload, same n=3000, clang -O0)

The gate is reference-platform-scoped (methodology §5), and the P0
`linux-clang` CI job runs on the same reference runner — so the gate
must clear the 1.0 ms bar on the clang -O0 tree as well. Recorded as
the cross-compiler evidence (the canonical-tree run above is the
recorded value; this run is the CI-shape check):

```text
budget=depth_sort_10k result=PASS metric=mean unit=ms
  after=0.161389 before=0.225852 target=1
  stats: n=3000 min=0.158391 mean=0.161389 p50=0.160615 p95=0.165715 p99=0.168611 max=0.290372
  context: workload=10 000 32-bit depth keys, one stable radix sort (M2-SORT-01, PRD 8.1) build=Clang 22.1.8, CMake Debug, engine policy (NFR-8.10) machine=AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM warmup=100
```

**PASS** on clang -O0 (0.161389 ms mean — 6.2× inside the gate; the
canonical g++ -O0 run is the slower, reference-recorded value — the
sort is data-movement-bound, and g++ -O0's loop codegen is the
conservative side here).

## Interpretation

- **4.4× inside the gate on the canonical tree, 6.2× on clang -O0.**
  The 1.0 ms PRD number was written for the AC-4.3 60 FPS frame
  budget (16.7 ms): at -O0 (where the gate measures, methodology §4)
  one 10k sort costs ≈ 226 µs (canonical g++) / ≈ 161 µs (clang) on
  this machine — 4 count passes + 4 stable scatters + 4 prefix walks
  over contiguous 16 B/slot storage, plus the 1 KB counter zeroing
  per pass (1 KB × 4 = 4 KB total counter traffic per sort,
  negligible next to the 640 KB of record movement).
- **Tail behavior:** p99 0.28983 ms (canonical) — 1.28× the mean; the
  run is tight (p95/p50 = 1.20×). No pathological tail: the sort
  touches only its own pre-allocated buffers (nothing to page in,
  nothing to lock, no GL, no allocation — PERF-002/003).
- **Zero per-sort allocation is structural** (fixed-size buffer
  traffic — no containers, no logging, no GL) and pinned by
  `DepthSortProperty` (1 000 consecutive 10k sorts = 0 heap blocks
  under the process-wide allocation watch).
- **Steady state, deterministic:** the 100-sort warm-up plus the
  precomputed key array keep the workload cache-hot and free of any
  RNG or division in the measured path — the run is reproducible and
  independent of any scene content. The sorted order itself is
  bit-identical across runs (the `DepthSortStability` /
  `DepthSortProperty` suites pin the oracle equivalence, not just
  the cost).
- **Scaling to the 50k budget (watch item):** the sort is O(4n):
  50k keys at the canonical rate ≈ 1.1 ms mean — still under half of
  the 2 ms 50k render-CPU budget, with batching and submission to
  share the remainder. M2-SPRITE-01/02 and M2-PERF-01 measure the
  full 50k pass (sort + batch + submit) and will re-gate this
  baseline's line if it regresses > 10%.

## Regression policy

`budgets.json` `measured = 0.225852` (the canonical Debug tree, g++ —
the workload is compiler-dependent, so the recorded value is the
canonical-tree one; the clang -O0 cross-run above is the CI-shape
evidence); the 10% regression band
(docs/benchmarks/methodology.md §5) applies to future re-measurements
on the reference platform.

## Open items

- **M2-SPRITE-01** owns the batcher that consumes the sorted order
  (the per-sprite-pass `DepthSort`, one instance per pass, sized to
  the scene's sprite budget at set-up).
- **M2-PERF-01** measures the full 50k-sprite render pass (sort +
  batch + submit) against the `sprites_50k_cpu` /
  `sprites_50k_draw_calls` budgets.
- **AC-4.3** (10k overlapping sprites at 60 FPS) is composed of this
  budget plus the batch/submission costs — the M2 exit gate
  (M2-EXIT-01) carries the full evidence.
