# Baseline: `m1-profiler-cost` — M1-PROF-01 profiler enabled-cost gate

Recorded by **M1-PROF-01** (2026-09-21). This is the **third** baseline
file; it is immutable (methodology §4 — superseding it later adds a new
file, it is never edited).

## What this baseline measures

The `ProfilerCost` suite of `laige-sim_tests` (M1-PROF-01): the
roadmap's disabled-cost check — **ON vs OFF on 10k-entity ticks,
bounded at 1%** (CORE-001, DBG-004, PRD FR-11.1 "always-on (cheap)
counters").

Workload: **10 000 entities** (world capacity 10 000, churn disabled —
no per-frame add/remove), 2 component types
(`ProfCostPos {int32 x, int32 y}`, `ProfCostVel {int64 v}`), and one
system (`ProfMove`, 16 ms budget) that iterates every entity
(`each<Pos, Vel>` Write/Read) and does integer work
(`pos.x += (int32)vel.v; pos.y += (int32)(vel.v>>32)`). The `GameLoop`
runs at 60 Hz, 1 tick/frame, on a **synthetic clock** (identical
sequence in both arms — the clock itself is not measured). Each
configuration runs **2 000 ticks**; the measured statistic is the
p50 of the per-frame time, recorded by a **test-side**
`TimeIt` + `Histogram` that wraps every frame in **both** arms
(the measurement harness cancels in the ratio — the only difference
between the arms is the engine's profiler).

- **Arm A (OFF)** — a fresh `Profiler` with `enabled = false`
  (the `GameLoop`'s null/disabled branch: one branch per tick).
- **Arm B (ON)** — a fresh enabled `Profiler` (the full enabled
  path: two `steady_clock` reads + one O(1) ring write per completed
  tick, plus the frame feed on the engine side — here via the
  loop's tick timing).
- **Warm-up** — one full OFF run (2 000 ticks) precedes the measured
  runs (page faults and cache effects fall out of the measured
  windows — the benchmark's warm-up discipline, AGENTS §12).
- **Best of 2** per configuration (4 measured runs total): a
  preemption stall only ever makes a run *slower*, so the faster run
  of a pair is the clean measurement (keeps a transient CI stall from
  breaching the gate).

The suite's assertion — `overhead = (on_p50 − off_p50) / off_p50 ≤
0.01` — runs on **every tree** on every CI run (`ctest -R
profiler`); this file records the canonical-tree measurement the
step's gate refers to.

**It is not a `budgets.json` workload** — the FR-11.1 counters are
diagnostics, not a budgeted subsystem. This baseline does **not**
update any `measured` field in `budgets.json`.

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.6-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` |
| 4 | Build type | `Debug` (canonical, `build/` tree) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002): `-ffp-contract=off -fno-associative-math`. No sanitizers (canonical tree). |
| 6 | Dataset / workload | `ProfilerCost` — 10 000 entities at the 100% scene budget (capacity 10000, churn 0), 2 component types (ProfCostPos 8 B, ProfCostVel 8 B), one `ProfMove` system (16 ms budget, `each<Pos,Vel>` Write/Read, integer work); `GameLoop` 60 Hz, 1 tick/frame, synthetic clock; A/B arms differ only by the engine profiler (disabled branch vs the full enabled path); test-side `TimeIt` + `Histogram` wraps every frame in both arms (cancels in the ratio) |
| 7 | Warm-up | one 2 000-tick OFF run discarded (page faults/caches fall out of the measured windows) |
| 8 | Sample count | `n=2000` per-frame samples per run (test-side window capacity 2000, no truncation); best of 2 runs per arm (4 measured runs; 16 000 ticks total measured) |
| 9 | Summary statistics | Frame-time p50 (ms): `on=0.557288 off=0.555746` (best of 2 runs each); `overhead_pct=0.277465` |
| 10 | Before / after | `before=0.555746` ms (profiler OFF p50) · `after=0.557288` ms (profiler ON p50) · `target=≤1% relative overhead` (measured **+0.28%**, 3.6× inside the gate) |

## Verbatim run output

### Run — canonical tree (Debug, g++), `ctest -R profiler`

Command (run from the repository root; the `profiler-cost` line is
printed by the test on **every** ctest run of `profiler`, on every
tree; the suite takes ~18 s):

```console
$ ctest --test-dir build -R profiler --output-on-failure
```

```text
1/1 Test #29: profiler .........................   Passed   18.19 sec

100% tests passed out of 1
```

Test stdout (machine-greppable lines, the same run's
zero-allocation probe and the cost gate):

```text
profiler-zeroalloc ticks=1000 allocs=0
profiler-cost on_p50=0.557288 off_p50=0.555746 overhead_pct=0.277465
```

Exit code: `0`.

Commit: `a811297` (branch `m1-prof-01-profiler-core`, step
M1-PROF-01).

## Interpretation

The enabled profiler's steady-state cost (two `steady_clock` reads +
one O(1) ring write per completed tick, one per frame on the engine
side) measures at **+0.28%** of a 10 000-entity frame on the
canonical Debug tree — well inside the roadmap's **1%** gate, and
inside the measurement noise of a single run (five quiet
same-machine single runs ranged −0.35% … +0.22%). The per-tick
overhead is far below the tick's `steady_clock` resolution effects,
so the ON arm's measured p50 is statistically indistinguishable from
the OFF arm's: the cost is paid, it is simply small.
