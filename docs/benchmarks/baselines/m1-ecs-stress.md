# Baseline: `m1-ecs-stress` — M1 ECS stress + memory accounting workload

Recorded by **M1-ECS-07** (2026-09-14). This is the **second** baseline
file; it is immutable (methodology §4 — superseding it later adds a new
file, it is never edited).

## What this baseline measures

The `EcsStress` suite of `laige-sim_tests` (M1-ECS-07): **10k entities,
6 component types, 10k frames of add/remove churn**. Each frame is a
driven simulation frame (`beginFrame()` for the G-R3/G-R4 guardrails),
followed by 64 seeded Tag adds + 64 Tag removes (cyclic permutation
order; 128 ops/frame, within the default G-R4 budget of 256) and one
`each<Pos, Tag>` iteration (Read, Read) over every Tagged entity
(visit count + 64-bit FNV-1a checksum over slot/generation/tag value).
A 700-frame warm-up (one full 625-frame cohort period plus margin)
precedes the measured window so every archetype's columns reach their
high water before it.

The step's verified properties:

- **No leaks** — the green `ctest -R ecs_stress` run in the ASan tree
  (leak check; Run B below).
- **Pool high-water stable** — zero `totalReservations`/
  `totalArchetypeGrowth` delta across the 10k-frame window; the
  high water (9 archetypes, 24,592 reserved rows, 549,024 reserved
  bytes) was reached in the warm-up and never changed in the window.
- **Iteration within the documented cost** (query.h: bounded archetype
  scan + one visit per matching entity) — window-wide
  `ns_per_visit` throughput floor of 600 ns/visit; measured
  **58.7 ns/visit** on the canonical Debug g++ tree.
- **Zero-allocation window** (test-only operator-new counter,
  non-sanitizer trees; sanitizer trees prove it via the leak-free run
  plus the reservation delta).
- **Memory accounting** (PRD §8.1 base memory, accounted bytes):
  110,000 entity bookkeeping bytes (11 B/slot × 10k) + 549,024
  reserved row bytes = 659,024 B of accounted ECS storage at the
  100%-full scene.

**It is not a `budgets.json` workload** — the `sim_tick_*` budgets
measure the 10k-entity/2k-dynamic-body tick with systems (M1-BENCH-01),
and `engine_base_rss` measures the *empty-scene* running engine — so
this baseline **does not update any `measured` field in `budgets.json`**
(all entries keep `measured: 0` until their subsystem lands).

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.2-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` |
| 4 | Build type | `Debug` (canonical, `build/` tree) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002): `-ffp-contract=off -fno-associative-math`. No sanitizers (canonical tree). |
| 6 | Dataset / workload | `EcsStress` — 10k entities at the 100% scene budget (capacity 10000), 6 component types (Pos/Vel/Flag/Quad/Pair/Tag, 4/8/8/16/8/4 B), base archetypes {Pos,Vel}, {Pos,Vel,Flag}, {Pos,Quad}, {Pos,Pair} (+ transient {Pos}); per frame: `beginFrame` + 64 Tag adds + 64 Tag removes (seeded cyclic permutation) + `each<Pos,Tag>` iteration; deterministic under the default test seed `0x1F055EED` |
| 7 | Warm-up | 700 discarded frames (`kWarmupFrames` = one full 625-frame cohort period + margin; archetype high water reached) |
| 8 | Sample count | `n=10000` per-frame iteration samples (histogram window `capacity == frames`; no truncation); window ops: 160,000 adds + 160,000 removes; 49,920,000 visits |
| 9 | Summary statistics | Per-frame iteration (ms): `min=0.275945 mean=0.292961 p50=0.292867 p95=0.301392 p99=0.305571 max=0.433534`; window-wide `ns_per_visit=58.7` |
| 10 | Before / after | `before=0` (first recording — no prior `m1-ecs-stress` baseline) · `after=58.7` ns/visit · `target=n/a` (not a `budgets.json` entry — see above; the iteration cost is part of the `sim_tick_*` workloads measured by M1-BENCH-01) |

## Verbatim run output

### Run A — canonical statistics run (Debug, g++)

Command (run from the repository root; the three `ecs-stress` lines are
printed by the test on **every** ctest run of `ecs_stress`, on every
tree):

```console
$ ctest --test-dir build -R ecs_stress --output-on-failure
```

```text
1/1 Test #17: ecs_stress .......................   Passed   14.74 sec

100% tests passed out of 1
```

Test stdout (machine-greppable lines):

```text
ecs-stress window: frames=10000 adds=160000 removes=160000 visits=49920000 tags_peak=4992 checksum=0xa1578baae8788fcf
ecs-stress iteration: stats: n=10000 min=0.275945 mean=0.292961 p50=0.292867 p95=0.301392 p99=0.305571 max=0.433534 ns_per_visit=58.7
ecs-stress memory: entities=10000 entity_bytes_capacity=110000 entity_bytes_inuse=110000 archetypes=9 rows_live=10000 rows_reserved=24592 bytes_reserved=549024
```

Exit code: `0`.

### Run B — ASan/UBSan tree (the step's required no-leak Verify)

```console
$ ctest --test-dir build-asan -R ecs_stress --output-on-failure
```

```text
1/1 Test #17: ecs_stress .......................   Passed   37.65 sec
```

Exit code: `0` — no AddressSanitizer/UBSanitizer report, leak-free
(the ASan run of the same 10k-frame window is the no-leak check the
step's Verify clause names).

### Run C — cross-compiler determinism spot check (clang++, same machine)

```console
$ ./build-clang/bin/laige-sim_tests --gtest_filter=EcsStress.*
```

```text
ecs-stress window: frames=10000 adds=160000 removes=160000 visits=49920000 tags_peak=4992 checksum=0xa1578baae8788fcf
ecs-stress iteration: stats: n=10000 min=0.332031 mean=0.36074 p50=0.355916 p95=0.391624 p99=0.42103 max=0.505089 ns_per_visit=72.3
ecs-stress memory: entities=10000 entity_bytes_capacity=110000 entity_bytes_inuse=110000 archetypes=9 rows_live=10000 rows_reserved=24592 bytes_reserved=549024
```

The `window:` and `memory:` lines are **byte-identical** to Run A
(g++ 16.2.1 vs clang++ 22.1.8): the workload is pure integer bookkeeping
(ARCH-010 — no floating point, no randomness beyond the seeded PRNG, no
addresses, no unordered iteration), so only the wall-clock `iteration:`
line differs (58.7 vs 72.3 ns/visit; both far under the 600 ns floor).
Two consecutive runs of Run A on the same binary also reproduced the
`window:`/`memory:` lines byte-identically.

## Cross-tree results (local, this commit)

| Tree | Build | `ctest` result |
|---|---|---|
| `build` | Debug, g++ 16.2.1 (static) | 40/40 passed (full suite) |
| `build-release` | Release, g++ 16.2.1 | `laige-sim_tests` + `ecs_stress` passed (1.2 s) |
| `build-asan` | Debug, g++ 16.2.1, ASan+UBSan | 40/40 passed; `ecs_stress` 37.7 s, leak-free |
| `build-clang` | Debug, clang++ 22.1.8 | 40/40 passed |
| `build-tsan` | Debug, g++ 16.2.1, TSan (`halt_on_error=1`) | `laige-sim_tests` + `ecs_stress` passed (144.9 s) |
| `build-shared` | Debug, g++ 16.2.1 (shared, NFR-8.9) | `laige-sim_tests` + `ecs_stress` passed |

Zero new warnings under the NFR-8.10 policy on all trees.

## Measured on

- Commit: `<filled in the same PR that lands this file>`
- Date: 2026-09-14 (13:00 UTC session time)
