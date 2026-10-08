# Baseline: `m2-particle-render` — M2-PART-02 10k declare-pass budget

Recorded by **M2-PART-02** (2026-10-07). This is the **tenth**
baseline file; it is immutable (methodology §4 — superseding it later
adds a new file, it is never edited). It retires the `0 = not yet
measured` M0 convention for the **`particle_render_10k`** budget
(10 000-particle particle → sprite conversion, one O(n) declare pass,
mean ≤ 2.0 ms — the FR-2.7 "particles render as batched sprites"
conversion cost; the step-level entry, not a PRD §8.1 table row,
needs no PRD revision — the `depth_sort_10k` precedent).

## What this baseline measures

The `ParticleRenderBudget` suite of `laige-render_tests`
(M2-PART-02; the suite is in
`tests/laige-render/particle_render_tests.cpp`): the FR-2.7
conversion workload — **one `declareParticles` pass over a
10 000-particle `ParticleSystem`** — checked against the
**`particle_render_10k`** budget (mean ≤ 2.0 ms).

Workload shape:

- One measured sample is **one `declareParticles` call** over 10 000
  live particles: the O(n) particle → sprite conversion (one depth
  key, one tint, one batcher `add` per particle — the batcher's
  `build`/sort cost is the separate `depth_sort_10k` budget's
  territory; `beginFrame`/`build` are OUTSIDE the measured window,
  the frame protocol keeps the two quantities apart).
- The system is deterministic (seed `0xC0FFEE00`): one emitter with a
  velocity box (0.25..0.5 world units/tick per axis, dyadic — the
  cross-backend exactness zone), depth 0.5 (sub-unit — exercises the
  depth quantization in the measured path), life 20..40, size
  0.25..0.5, tint (255, 128, 64, 255), fade window 8. A burst of
  10 000 particles, then 10 update ticks (the positions spread by
  the velocity box; all alive — min life 20 > 10), both SimMath
  backends (ADR 0002: the gate is backend-complete; the recorded
  value is the worse of the two).
- **Warm-up:** 100 declare+build frames discarded; **measured:**
  3 000 declare passes (`n=3000`), histogram capacity 3 000, no
  truncation.

The gate (roadmap M2-PART-02 Verify): mean ≤ 2.0 ms, enforced on CI
by the `particle_render` ctest entry. The gated branch (load
`budgets.json`, `budgetCheck`, the stable 4-line report,
`EXPECT(passed)`) compiles only on **Linux non-instrumented trees**
(`LAIGE_PARTICLE_RENDER_BUDGET` — the `depth_sort` / `iso_picking`
precedent): instrumentation inflates the absolute cost
(methodology §4), so the sanitizer trees run the **same workload
ungated** and verify its safety properties instead (leak-free ASan,
race-free TSan); the absolute target is reference-platform-scoped
(methodology §5), and the P0 `linux-clang` job runs on the same
reference runner, so the gate is verified there too (the
cross-compiler run below). The zero-allocation property of the
declare loop is pinned separately by `ParticleRenderZeroAlloc`
(1 000 frames of 10 000-particle declare+build loops = 0 heap blocks
under the allocation watch, the non-sanitizer trees).

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.9-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` (canonical gate run; the cross-compiler run below uses `Clang 22.1.8`) |
| 4 | Build type | `Debug` (canonical, `build/` tree — CMake Debug: `-O0 -g` + the engine policy flags) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`. No sanitizers (canonical tree). |
| 6 | Dataset / workload | `particle_render_10k` — one `declareParticles` over 10 000 live particles (seed 0xC0FFEE00, one velocity-box emitter, 10 update ticks, both SimMath backends — the conversion pass ONLY) |
| 7 | Warm-up | 100 declare+build frames discarded |
| 8 | Sample count | `n=3000` declare passes (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | fpx16_16: min=1.06083 mean=**1.07798** p50=1.07614 p95=1.09171 **p99=1.108** max=1.40197 · fp32_pinned: min=0.988138 mean=0.996543 p50=0.995282 p95=1.0048 p99=1.02189 max=1.06674 ms |
| 10 | Before / after | `before=0` (M0 convention — not yet measured) · `after` (mean, worse backend fpx16_16): **1.07798** → **recorded 1.07798** · `target`: mean ≤ 2.0 ms — **PASS** (1.86× inside the gate) |

## Verbatim run output

### Run — canonical tree (Debug, g++), budget-checked

Command (run from the repository root; `budgets.json` resolved via
`LAIGE_BUDGETS_PATH`; `LAIGE_BENCH_MACHINE` carries the operator's
machine description into the report's `context.machine` field):

```console
$ LAIGE_BUDGETS_PATH=$PWD/budgets.json \
    LAIGE_BENCH_MACHINE="AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM" \
    ./build/bin/laige-render_tests --gtest_filter='ParticleRenderBudget*'
```

```text
budget=particle_render_10k result=PASS metric=mean unit=ms
  after=1.07798 before=1.09108 target=2
  stats: n=3000 min=1.06083 mean=1.07798 p50=1.07614 p95=1.09171 p99=1.108 max=1.40197
  context: workload=10 000-particle particle-to-sprite conversion, one O(n) declare pass (M2-PART-02) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine=AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM warmup=100
budget=particle_render_10k result=PASS metric=mean unit=ms
  after=0.996543 before=1.09108 target=2
  stats: n=3000 min=0.988138 mean=0.996543 p50=0.995282 p95=1.0048 p99=1.02189 max=1.06674
  context: workload=10 000-particle particle-to-sprite conversion, one O(n) declare pass (M2-PART-02) build=GCC 16.2.1 20260810, CMake Debug, engine policy (NFR-8.10) machine=AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM warmup=100
```

Exit code: `0`. (The report's `before=1.09108` is the first
recorded measurement of this budget — the pre-finalization run;
this run is the recorded value.)

### Gate — CI shape, canonical tree, `ctest -R particle_render`

```console
$ ctest --test-dir build -R "particle_render" --output-on-failure
```

```text
Test project /home/anon/devel/laige-cpp/build
    Start 55: particle_render
1/1 Test #55: particle_render ..................   Passed   24.60 sec

100% tests passed out of 1

Total Test time (real) =   24.61 sec
```

Commit: the M2-PART-02 commit on branch `feat/m2-part-02-particle-render`
(the `particle_render` ctest entry is the CI perf lane — the P0 jobs
run the full `ctest` on the canonical tree and the `linux-clang`
job).

## Cross-compiler run (same workload, same n=3000, clang -O0)

The gate is reference-platform-scoped (methodology §5), and the P0
`linux-clang` CI job runs on the same reference runner — so the gate
must clear the 2.0 ms bar on the clang -O0 tree as well. Recorded as
the cross-compiler evidence (the canonical-tree run above is the
recorded value; this run is the CI-shape check):

Command (run from the repository root):

```console
$ LAIGE_BUDGETS_PATH=$PWD/budgets.json \
    LAIGE_BENCH_MACHINE="AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM" \
    ./build-clang/bin/laige-render_tests --gtest_filter='ParticleRenderBudget*'
```

```text
budget=particle_render_10k result=PASS metric=mean unit=ms
  after=1.18578 before=1.07798 target=2
  stats: n=3000 min=1.15908 mean=1.18578 p50=1.18317 p95=1.20071 p99=1.21506 max=2.0245
  context: workload=10 000-particle particle-to-sprite conversion, one O(n) declare pass (M2-PART-02) build=Clang 23.1.1, CMake Debug, engine policy (NFR-8.10) machine=AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM warmup=100
budget=particle_render_10k result=PASS metric=mean unit=ms
  after=1.04057 before=1.07798 target=2
  stats: n=3000 min=0.993628 mean=1.04057 p50=1.0405 p95=1.05659 p99=1.06883 max=1.78253
  context: workload=10 000-particle particle-to-sprite conversion, one O(n) declare pass (M2-PART-02) build=Clang 23.1.1, CMake Debug, engine policy (NFR-8.10) machine=AMD Ryzen 9 7950X3D (16 cores / 32 threads), max 5 763 MHz, 64 GB RAM warmup=100
[       OK ] ParticleRenderBudget.TenThousandParticles (22115 ms)
[==========] 1 test from 1 test suite ran. (22115 ms total)
[  PASSED  ] 1 test.
```

Exit code: `0`. The clang -O0 tree clears the gate with 1.69× headroom
(worse backend fpx16_16: 1.18578 ms against the 2.0 ms bar).

## Interpretation

- **1.86× inside the gate on the canonical tree.** The 2.0 ms target
  is a step-level budget (the FR-2.7 conversion cost), calibrated at
  ≈2× the canonical -O0 measurement (1.08 ms) — the depth_sort_10k
  precedent's headroom discipline (the M2-ISO-02 rebaseline lesson:
  calibrate the gate with margin on the CI reference toolchain, not
  the local machine). The measured -O0 cost is division-bound: five
  per-particle conversions (4 u8→float tint channels + the u32 fade
  division) dominate at `-O0`; the depth key is integer math
  (cheap in fpx, one rounding + pack in fp32).
- **Cross-backend spread ≈ 8%** (fpx 1.078 ms vs fp32 0.997 ms —
  fpx slower: the fixed-point key path carries the Q16.16 rounding
  + the raw clamps). The recorded value is the worse backend
  (methodology §5 convention).
- **The conversion is O(live) with zero allocations** — the
  `ParticleRenderZeroAlloc` suite pins 1 000 frames of 10 000-particle
  declare+build loops at 0 heap blocks (PERF-003, FR-2.7 "pooled");
  the budget measures the per-particle conversion work, and the
  batcher's sort cost is accounted separately (`depth_sort_10k`,
  0.225852 ms recorded — the two budgets keep the frame protocol's
  phases apart).
- **One draw call per emitter set regardless of particle count**
  (RENDER-001): 10 000 particles of one set declare into ONE
  `(atlas, material, blend)` group — the conversion cost scales with
  particle count, the GPU submission cost does not.
