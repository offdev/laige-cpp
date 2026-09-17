# Determinism matrix — M1-SAMPLE-01 hello scenario (M1-DET-04)

Recorded by **M1-DET-04** (2026-09-17). This is the determinism report
required by the step's scope: the M1-SAMPLE-01 hello scenario is
activated in `laige-detcheck` with a committed per-tick hash-stream
baseline, and every P0 OS CI job asserts per-tick identity against it.
The guarantee's **scope is stated here per ARCH-010** (a determinism
guarantee without a scope is a defect).

## What the matrix asserts

The scenario emits one `<tick> <hash>` line per tick (301 lines: tick 0
= initial state, then one per completed tick) — the
[detcheck scenario contract](../api/detcheck.md). The per-tick hash is
`World::stateHash` (FNV-1a 64 over tick, live handles, archetype
signatures + raw component bytes, per-system PRNG state). Two runs are
identical iff inputs + seed + math backend + config (+ component schema
hash) match — the replay identity (ADR 0002). The matrix checks that
identity across:

1. **Every P0 OS job (merge and PR):** the job's native build runs
   `hello --expect` and `hello-fp32 --expect` against the committed
   baselines (`samples/hello/baselines/`) in its ctest suite (tests
   `hello_baseline_fpx`, `hello_baseline_fp32`). A platform whose build
   desyncs from the reference build fails there — loudly.
2. **The merge `detcheck` job (ubuntu-24.04):** four cross-build pairs,
   both backends (`.github/workflows/ci.yml`, job "Determinism check"):

   | Pair | Run A | Run B | Proves |
   |---|---|---|---|
   | A1 | g++ `Debug` | clang++ `Debug` | cross-**compiler** (Linux) |
   | A2 | g++ `Debug` | clang++ `Debug` | cross-compiler, `float_pinned_32` |
   | B1 | clang++ `Debug`+ASan | g++ `Release` | cross-**configuration** (+ sanitizer) |
   | B2 | clang++ `Debug`+ASan | g++ `Release` | cross-configuration, `float_pinned_32` |

   each via `laige-detcheck --run-a/--run-b` + `--compare-combined`
   (two-stage, per-tick comparison; exit 0 match / 1 diverged / 2 error).
3. **Reference-baseline sanity (same job):** the canonical Debug g++
   build must reproduce its own committed baselines on every merge — a
   stale baseline is a red job, never a silent re-baseline (CORE-008).

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810`; `clang++ 22.1.8` (pairs A); `clang++ 22.1.8 -fsanitize=address,undefined` (pair B, run A); `g++ 16.2.1` (pair B, run B) |
| 4 | Build type | `Debug` (reference + pair A + pair B run A with ASan); `Release` (pair B run B) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002): `-ffp-contract=off -fno-associative-math`; ASan pair: `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer` |
| 6 | Dataset / workload | hello scenario: 1 entity, 1 component (`PlayerPos{M::Vec2}`), 1 system (`MovePlayer`: +1 unit/tick diagonal, wrap in a 32-unit box); 300 ticks, 60 Hz, seed `0x1F055EED` (the canonical sample config, `samples/hello/config.json`) |
| 7 | Warm-up | n/a (state hashes, not timings) |
| 8 | Sample count | 301 hash lines per run (tick 0 + 300 completed ticks); 4 pairs × 2 backends compared locally, 4 pairs × 2 backends in CI |
| 9 | Summary statistics | See the result tables below — the "metric" is bit-identity (0/1), not a distribution |
| 10 | Before / after | `before=n/a` (first recording) · `after=4/4 pairs OK, 2/2 baselines reproduced` (local, 2026-09-17) · `target=all pairs OK on every merge, both backends, all P0 OS jobs` |

## Determinism scope (ARCH-010)

| Backend | Guarantee scope | Basis |
|---|---|---|
| `fixed_point_16_16` | **Cross-platform, cross-compiler, cross-configuration:** any conforming C++20 build of this repo reproduces the stream (every P0 OS, both Linux compilers, Debug/Release, ASan/TSan lanes). | Q16.16 is signed integer arithmetic — exact by the C++20 standard, no compiler freedom. The matrix (rows 1–3 above) is the proof. |
| `float_pinned_32` | **Same-build/same-ISA by scope** (ADR 0002); the per-platform support list below is generated from the matrix results. A desynced pair is declared unsupported — the baseline is never re-fitted to the diverging build. | IEEE 754 single-precision; identical streams require identical rounding behavior. This scenario's op surface is single-rounding add/sub with comparisons — no FMA, no reassociation (pinned flags) — so identity is expected on all P0 platforms; the matrix confirms it. |

## `float_pinned_32` per-platform support list

Generated from the matrix (the first row set lands from the first merge
run of the detcheck job and the P0 jobs; entries are added/changed only
by matrix results or a documented baseline regeneration).

| Platform | Build | Baseline check (ctest) | detcheck pairs | Status |
|---|---|---|---|---|
| linux-gcc | g++ `Debug` | reproduced (local, 2026-09-17) | pair A/B run A (local, 2026-09-17) | **supported** |
| linux-clang | clang++ `Debug` | reproduced (local, 2026-09-17) | pair A/B run A (local, 2026-09-17) | **supported** |
| linux-gcc (Debug+ASan / Release lanes) | clang++ `Debug+ASan`, g++ `Release` | reproduced (local, 2026-09-17) | pair B (local, 2026-09-17) | **supported** |
| macos-arm64 | AppleClang `Debug` | pending first merge run | pending (P0 job baseline check) | pending — expected supported |
| macos-intel | AppleClang `Debug` | pending first merge run | pending (P0 job baseline check) | pending — expected supported |
| windows-msvc | MSVC `Debug` | pending first merge run | pending (P0 job baseline check) | pending — expected supported |

If a pending platform's `hello_baseline_fp32` fails on its P0 job, that
platform is moved to **unsupported** in this table (ADR 0002:
`float_pinned_32` is then excluded from its determinism guarantee —
games on that platform use `fixed_point_16_16`, which has no such
restriction) and the test is converted to a documented skip with a
pointer to this table. `fixed_point_16_16` failures are never skipped:
they are engine bugs (CORE-008).

## Local evidence (2026-09-17, this recording)

Baselines generated from the canonical Debug g++ tree (`build/`):

```console
$ ./samples/hello/bin/hello \
    > samples/hello/baselines/fixed_point_16_16/hash_stream.txt
$ ./samples/hello/bin/hello-fp32 \
    > samples/hello/baselines/float_pinned_32/hash_stream.txt
$ wc -l samples/hello/baselines/*/hash_stream.txt
   301 samples/hello/baselines/fixed_point_16_16/hash_stream.txt
   301 samples/hello/baselines/float_pinned_32/hash_stream.txt
```

Cross-compiler spot check (clang++ tree's streams vs the g++ baselines,
both backends): `cmp` reports byte-identical.

The four detcheck pairs, both backends (each: `--run-a/--run-b` phase 1
+ `--compare-combined` phase 2):

```text
detcheck scenario=combined result=OK ticks=301   (pair A, fixed_point_16_16)
detcheck scenario=combined result=OK ticks=301   (pair A, float_pinned_32)
detcheck scenario=combined result=OK ticks=301   (pair B, fixed_point_16_16)
detcheck scenario=combined result=OK ticks=301   (pair B, float_pinned_32)
```

Reference-baseline sanity (the merge job's first check):

```console
$ ./samples/hello/bin/hello --expect samples/hello/baselines/fixed_point_16_16/hash_stream.txt
# exit 0; hello headless ticks=300 status=ok
$ ./samples/hello/bin/hello-fp32 --expect samples/hello/baselines/float_pinned_32/hash_stream.txt
# exit 0; hello headless ticks=300 status=ok
```

**Perturbation proof (the step's Verify clause):** changing the
scratch system's SimMath constant (`kVelocity` 1 → 2 units/tick in
`MovePlayer`), rebuilding the canonical tree, and re-running
`hello --expect` fails exactly as the matrix requires —

```text
hello: hash mismatch at tick 1 (first divergence)
  baseline: 1 34ad0d068f9da541
  run:      1 d64c528f09f771f1
# exit 1
```

— and reverting the constant restores exit 0. The failure paths are
pinned by ctest in every P0 job (tests `hello_baseline_mismatch` —
first-divergence report, `hello_baseline_truncated` — stream-length
mismatch, `hello_baseline_malformed` — load-time contract error,
`hello_baseline_missing` — read error; the same paths `laige-replay
--expect` exercises in `tests/replay`).

## Regeneration policy

See [samples/hello/baselines/README.md](../../samples/hello/baselines/README.md):
regenerate only for a deliberate documented scenario/config/engine-hash
change, always from the canonical Debug g++ tree, always with the
cross-compiler identity re-verified, and always recorded here with the
reason and commit. This file gains one "Regeneration" subsection per
event (methodology §4: reports are append-only history).

## First GitHub run (PR #41, 2026-09-17)

The step's PR run (`CI (pull request)`, run 35238175154) — the first
GitHub execution of the matrix:

- **Linux x64 (g++), Linux x64 (clang++), Linux x64 ASan+UBSan, Linux
  x64 TSan** — all green, each running the full 82-test ctest suite
  including the six `hello_baseline_*` tests (per-tick identity of the
  job's own build against both committed baselines, plus the failure
  fixtures). Job log (Linux x64 g++): `73/82 hello_baseline_fpx
  Passed`, … `78/82 hello_baseline_missing Passed`.
- **Determinism check (tooling)** — green: synthetic self-check
  (`detcheck scenario=synthetic result=OK ticks=256`) followed by the
  both-backend baseline comparison (`hello --expect` /
  `hello-fp32 --expect`, both exit 0, `hello headless ticks=300
  status=ok`).
- macOS/Windows P0 jobs: skipped without `ci:*` labels on the PR (the
  label selector); they run on the merge via `ci.yml` (all P0 jobs).

## Open until the first merge

- The merge `detcheck` job (the four-configuration matrix: pair A
  g++ vs clang++, pair B Debug+ASan vs Release, both backends, plus
  the reference-baseline sanity) and the macOS/Windows P0 baseline
  checks run with this step's merge (`ci.yml`). Until
  then, the support-list rows marked *pending* above are projections
  from the local evidence plus the PR run above; the first merge run
  converts them into matrix results (or, for a desynced
  `float_pinned_32` pair, into an "unsupported" declaration per
  ADR 0002).
