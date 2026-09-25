# Baseline: `m1-sim-tick` — M1-BENCH-01 simulation-tick budget

Recorded by **M1-BENCH-01** (2026-09-25). This is the **fourth**
baseline file; it is immutable (methodology §4 — superseding it later
adds a new file, it is never edited).

## What this baseline measures

The `sim-tick` suite of `laige-bench` (M1-BENCH-01): the PRD §8.1
"Simulation tick" workload — **10 000 entities** (the scene budget at
100%), **2 000 of them dynamic bodies** — driven at **60 Hz** through
the engine's `GameLoop`, checked against **both** `sim_tick` budgets
(`sim_tick_avg`, `sim_tick_p99`) on **both** SimMath backends (ADR
0002: `fixed_point_16_16`, the default, and `float_pinned_32`).

Workload shape (the suite is in `tools/bench/laige-bench.cpp`):

- `World` capacity 10 000, deterministic, seed `0x1F055EED` (the
  repo-wide test-seed convention, docs/testing.md §4; methodology §6).
- 10 000 entities; the first 2 000 are dynamic bodies carrying the
  built-in `Position2D<Backend>` plus the workload's velocity component
  `BenchVel<Backend>` (a `{Vec2 v}` component marked
  `LAIGE_COMPONENT` + `LAIGE_DETERMINISM_SAFE` per backend). The
  remaining 8 000 are bare entities (no components) — the static
  scenery's M1 stand-in.
- Initial state is deterministic and index-derived (no RNG in the
  measured path): the 2 000 dynamic bodies scatter over a 50×50 grid
  span — one body per cell of the first 2 000 cells (column-major:
  `x = col − 25 ∈ −25..24`, `y = row ∈ 0..49`) — with per-tick
  velocities `vx ∈ -3..3`, `vy ∈ -5..5` world units (max coordinate
  magnitude 20 049 units — 49 + 5·4000 — over the full 4 000-tick run,
  inside `fpx16_16`'s ±32 768 Q16.16 range, so no saturating-overflow
  edge is ever touched in the measured window).
- Two systems, registered in execution order (no `depends_on`):
  - `BenchMove` (1 ms declared budget) — `pos += vel` over every
    dynamic body, through the active backend's `SimMath::add` (the
    pinned op surface, ADR 0002 — one correctly-rounded addition per
    component, never fused);
  - `BenchHash` (2 ms declared budget) — the per-tick deterministic
    state hash (`World::stateHash`, M1-DET-03) over the live state,
    labeled with the completed-tick count — the M1 determinism work
    made part of the measured tick.
- The `GameLoop` (60 Hz) runs on an **exact synthetic clock**: each
  measured iteration advances the injectable `nowNs` by exactly
  `16 666 667` ns (one due tick — the exact integer due computation,
  M1-LOOP-01) and runs one `frame()`. One measured sample is one
  completed tick (beginFrame + the systems + the loop's bookkeeping).
  The presentation snapshot (M1-LOOP-02) is not part of it —
  presentation is separate from the authoritative tick by ARCH-009.
- **Warm-up:** 1 000 ticks discarded; **measured:** 3 000 ticks
  (`--runs=3000 --warmup=1000`), every sample kept (histogram capacity
  3 000, no truncation).
- **Zero allocations:** in debug non-sanitizer builds, every measured
  tick additionally passes the engine's own G-R1 per-tick
  zero-allocation assertion (M1-ALLOC-01 — `GameLoop::runOneTick` arms
  the process-wide allocation watch around the tick body). An
  allocating tick aborts the run, so a PASS here is a zero-alloc-
  asserted tick, not just a fast one.

The measured sample is the **ECS-only slice** of the PRD §8.1
workload: the 2 000 dynamic bodies move by kinematic integration
(movement + state hash). The PRD's remaining tick content (physics
bodies arrive with M3, presentation with the M4 render milestone) is
out of scope here — this is the slice M1 ships, measured at M1's
10 000-entity scene budget.

The gate (roadmap M1-BENCH-01): `sim_tick_avg` mean ≤ 3.0 ms and
`sim_tick_p99` p99 ≤ 5.0 ms, **both backends**, enforced on CI by the
`laige_bench_sim_tick_fpx16` / `laige_bench_sim_tick_fp32` ctest
entries (one run checks both budgets via the repeatable
`--budget=` form; a failed check exits 2 and fails the CI job). The
entries are excluded from the sanitizer trees: instrumentation
inflates the tick's absolute cost (the m1-profiler-cost baseline
precedent — ASan+UBSan measures ≈1.64 ms/tick on this workload, 2.7×
the canonical 0.60 ms), so a sanitizer measurement would measure the
instrumentation, not the sim; those trees verify the workload's
safety properties instead (the leak-free ASan run, the race-free TSan
run — see Interpretation).

**This is a `budgets.json` workload:** this baseline updates
`measured` for `sim_tick_avg` and `sim_tick_p99` (the M0 convention's
`0 = not yet measured` is retired by this measurement). The recorded
value is the **worse (max) of the two backends** on the canonical
Debug tree — the budget covers both backends (ADR 0002: both must
pass), and recording the worse keeps the 10% regression band
(docs/benchmarks/methodology.md §5) conservative.

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.6-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` (canonical gate runs; the cross-compiler runs below use `Clang 22.1.8`) |
| 4 | Build type | `Debug` (canonical, `build/` tree) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002) on the benchmark TU: `-ffp-contract=off -fno-associative-math`. No sanitizers (canonical tree). |
| 6 | Dataset / workload | `sim-tick` — 10 000 entities at the 100% scene budget, 2 000 dynamic bodies (`Position2D<B>` + `BenchVel<B>`), 8 000 bare entities; systems `BenchMove` (1 ms budget, `pos += vel` via `SimMath::add`) and `BenchHash` (2 ms budget, per-tick `World::stateHash`); `GameLoop` 60 Hz, 1 tick/frame, exact synthetic clock (`16 666 667` ns/frame); both SimMath backends |
| 7 | Warm-up | 1 000 ticks discarded |
| 8 | Sample count | `n=3000` tick samples per backend (histogram capacity 3000, no truncation) |
| 9 | Summary statistics | `fpx16_16`: mean=0.598405 p50=0.598226 p95=0.601423 **p99=0.606001** max=0.813886 ms · `fp32_pinned`: mean=0.591438 p50=0.590442 p95=0.601623 **p99=0.615179** max=0.902675 ms (min: 0.588077 / 0.581054) |
| 10 | Before / after | `before=0` (M0 convention — not yet measured) · `after`: sim_tick_avg (mean) `fpx16=0.598405`, `fp32=0.591438` → **recorded 0.598405** (worse of backends); sim_tick_p99 (p99) `fpx16=0.606001`, `fp32=0.615179` → **recorded 0.615179** (worse of backends) · `target`: mean ≤ 3.0 ms, p99 ≤ 5.0 ms — both backends **PASS** (5.0× / 8.1× inside the gates) |

## Verbatim run output

### Run — canonical tree (Debug, g++), both backends, budget-checked

Command (run from the repository root; `budgets.json` resolved via
`LAIGE_BUDGETS_PATH`):

```console
$ LAIGE_BUDGETS_PATH=$PWD/budgets.json ./build/bin/laige-bench \
    --suite=sim-tick --math=fixed_point_16_16 \
    --runs=3000 --warmup=1000 --budget=sim_tick_avg --budget=sim_tick_p99
```

(The report's `before=` carries the `measured` value recorded by this
step's first commit — 0.59283 / 0.618314 — before the initial-state
grid-shape fix below re-measured the workload; the first-ever
measurement of these budgets was `before=0`, the M0 convention.)

```text
sim-tick: backend=fixed_point_16_16 entities=10000 dynamic=2000 rate_hz=60 ticks=4000 final_hash=0x7840644a24334cd0
suite=sim-tick runs=3000 warmup=1000
budget=sim_tick_avg result=PASS metric=mean unit=ms
  after=0.598405 before=0.59283 target=3
  stats: n=3000 min=0.588077 mean=0.598405 p50=0.598226 p95=0.601423 p99=0.606001 max=0.813886
  context: workload=10k entities, 2k dynamic bodies (PRD 8.1) build=GCC 16.2.1 20260810, Debug machine= warmup=1000

budget=sim_tick_p99 result=PASS metric=p99 unit=ms
  after=0.606001 before=0.618314 target=5
  stats: n=3000 min=0.588077 mean=0.598405 p50=0.598226 p95=0.601423 p99=0.606001 max=0.813886
  context: workload=10k entities, 2k dynamic bodies (PRD 8.1) build=GCC 16.2.1 20260810, Debug machine= warmup=1000
```

Exit code: `0`. (Same command with `--math=float_pinned_32`:
`sim-tick: backend=float_pinned_32 … ticks=4000 final_hash=0x7a60d70232e448c7`;
`sim_tick_avg after=0.591438` PASS, `sim_tick_p99 after=0.615179` PASS;
stats `n=3000 min=0.581054 mean=0.591438 p50=0.590442 p95=0.601623
p99=0.615179 max=0.902675`; exit code `0`.)

### Gate — CI shape, canonical tree, `ctest -R laige_bench_sim_tick`

```console
$ ctest --test-dir build -R "laige_bench_sim_tick" --output-on-failure
```

```text
Test project /home/anon/devel/laige-cpp/build
    Start 91: laige_bench_sim_tick_fpx16
1/2 Test #91: laige_bench_sim_tick_fpx16 .......   Passed    2.48 sec
    Start 92: laige_bench_sim_tick_fp32
2/2 Test #92: laige_bench_sim_tick_fp32 ........   Passed    2.45 sec

100% tests passed out of 2

Total Test time (real) =   4.94 sec
```

Commit: the M1-BENCH-01 commits on branch `m1-bench-01-sim-tick`
(the ctest entries are the CI perf lane — the P0 jobs run the full
`ctest` on the canonical tree; see the M1-EXIT-01 gate).

## Cross-tree and cross-compiler runs (same command, same 4 000 ticks)

The `final_hash` line is the run's determinism fingerprint (a pure
function of the workload and backend, `World::stateHash` — ARCH-010
scope per ADR 0002). All non-instrumented trees below agree to the
bit, across build types and compilers; the instrumented trees agree on
state (the hash is computed identically) while the absolute tick cost
rises with the instrumentation.

| Tree | Build | `fpx16_16` mean / p99 (ms) | `fp32_pinned` mean / p99 (ms) | final_hash (fpx16 / fp32) | Budget gate |
|---|---|---|---|---|---|
| `build/` (canonical) | Debug g++ 16.2.1 | 0.598405 / 0.606001 | 0.591438 / 0.615179 | `0x7840644a24334cd0` / `0x7a60d70232e448c7` | PASS (gate) |
| `build-release/` | Release g++ 16.2.1 | 0.0801446 / 0.08439 | 0.0782755 / 0.082517 | identical | PASS |
| `build-clang/` | Debug Clang 22.1.8 | 0.913303 / 0.935979 | 0.892101 / 0.904358 | identical | PASS |
| `build-shared/` | Debug g++ 16.2.1 (shared libs) | 0.623729 / 0.639315 | 0.623554 / 0.678189 | identical | PASS |
| `build-asan/` | Debug Clang 22.1.8 (+ASan/UBSan) | 1.63971 / 1.74115 (n=500) | — | identical | excluded (instrumentation) |
| `build-tsan/` | Debug Clang 22.1.8 (+TSan) | 2.44972 / 2.52165 (n=500) | — | identical | excluded (instrumentation) |

(The ASan/TSan rows are the shorter 600-tick safety runs — leak-free
ASan exit 0, race-free TSan exit 0 with `halt_on_error=1` — not the
budget gate. Under TSan the `BenchHash` system's 2 ms declared budget
is exceeded (2.04 ms window average) and the engine's G-R5
`budget_overrun` warn fires once (rate-limited) — a correct
observation of the instrumentation's 4× slowdown, not a workload
defect; the budget gate applies to non-instrumented trees only, as in
m1-profiler-cost.)

## Interpretation

At M1's ECS-only slice — 10 000 entities, 2 000 kinematic dynamic
bodies, movement + per-tick state hash — a complete 60 Hz tick
measures **0.60 ms mean / 0.62 ms p99** (the worse of the backends:
mean `fpx16_16` 0.598405, p99 `fp32_pinned` 0.615179) on the canonical
Debug tree: **5.0× inside** the 3.0 ms mean budget and **8.1× inside**
the 5.0 ms p99 budget. The p99/mean ratio (1.01–1.04) shows a flat,
allocation-free tick with no visible churn or GC-like spikes; the max
(0.81 / 0.90 ms) is a preemption-class outlier, not a systemic tail.
Both SimMath backends pass identically — the Q16.16 fixed-point path
costs no more than the pinned-float path at this scale (both are
register-resident integer/FP work; the fixed-point addition's
per-component shift/mask is cheaper than a division, and no division
occurs in the measured path).

The Debug-vs-Release spread (0.60 ms → 0.08 ms, ≈7.5×) is the
instrumentation and no-optimization cost of the canonical tree — the
CI gate deliberately measures the Debug tree, so the budget protects
the configuration the tests run in (methodology §5). The Clang Debug
run (0.91 ms mean) is 1.5× the GCC Debug run — compiler-level
codegen differences in the debug build; both are far inside budget,
and the Linux P0 CI lane covers both compilers.

Every tick passed the engine's G-R1 per-tick zero-allocation assertion
in the debug trees (an allocating tick would have aborted the run) —
the M1-ALLOC-01 property is asserted across all ticks of both
backends (4 000 each — warm-up and measured alike), not just sampled.
The `final_hash` fingerprint is bit-identical across the four
non-instrumented trees and across the two compilers, at both backends
— the ADR 0002 cross-build bit-exactness for `fpx16_16` (guaranteed by
the C++20 standard) and the per-ISA agreement of the pinned `fp32`
path on x86-64 (the detcheck matrix's supported-platform scope, ADR
0002 / ARCH-010), now demonstrated on this larger 4 000-tick workload
(beyond the hello baseline's scope).

**Initial-state grid-shape fix (recorded here, same PR):** the step's
first commit stacked the 2 000 dynamic bodies in a single grid column
(`x = (i / 2000) % 50 − 25` — the divisor was the body count, not the
grid span) instead of scattering one body per cell of the first 2 000
50×50 cells as documented. The fix (`x = (i / 50) % 50 − 25`) was
re-measured on every tree: the `final_hash` fingerprint changed (as it
must — the initial state changed), the new values above are the
recorded ones, and the numbers moved <2% (0.59283 → 0.598405 mean
fpx16_16; 0.618314 → 0.606001 p99) — the workload's iteration cost is
archetype-bound, not position-value-bound, so the budget result is
unchanged: both backends still PASS with the same margins.

The measured tick is the simulation tick only (beginFrame + the two
systems + the loop's bookkeeping). The PRD §8.1 tick's remaining
content — physics (M3), the presentation snapshot (M4) — lands in
later milestones; when it does, this workload's systems grow and a
superseding baseline (methodology §4) will re-measure the full tick
against the same budgets.
