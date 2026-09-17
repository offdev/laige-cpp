# hello scenario — committed per-tick hash baselines (M1-DET-04)

The reference per-tick `World::stateHash` streams of the hello scenario
(300 ticks, 301 lines: tick 0 = initial state, then one line per
completed tick; each line `<tick> <hash>` with 16 lowercase hex digits —
the [detcheck scenario contract](../../docs/api/detcheck.md)). One file
per SimMath backend (ADR 0002):

| File | Backend | Config |
|---|---|---|
| `fixed_point_16_16/hash_stream.txt` | `fixed_point_16_16` (Q16.16, `SimMathFpx16` — the default) | canonical sample config (60 Hz, budget 8, churn 256, seed `0x1F055EED`) |
| `float_pinned_32/hash_stream.txt` | `float_pinned_32` (IEEE float, `SimMathFp32` — same-build/same-ISA scope) | identical config, backend field differs (the replay identity, ADR 0002) |

Both streams start on the same tick-0 hash (`1d0bee038913bfc2`): the
initial state (one entity, `PlayerPos` at the box center, zero PRNG
draws) is byte-identical across backends, and the per-tick hashes
diverge from tick 1 as the positions differ.

## Reference build

- **Tree:** the canonical `Debug` g++ tree (`build/`):
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++`
  (the canonical command in [building.md](../../docs/getting-started/building.md)).
- **Compiler:** g++ (GCC) 16.2.1; engine policy + the SimMath pinned set
  (ADR 0002: `-ffp-contract=off -fno-associative-math`), no sanitizers.
- **Generated (M1-DET-04):** 2026-09-17 on AMD Ryzen 9 7950X3D /
  CachyOS x86_64.

`fixed_point_16_16` is bit-exact **by the C++20 standard** (signed
integer arithmetic, no compiler freedom): any conforming build must
reproduce it — the CI matrix proves it across g++/clang++/ASan/Release
and, per P0 OS job, on macOS/Windows. `float_pinned_32` is
same-build/same-ISA by scope (ADR 0002); the scenario's op surface is
single-rounding IEEE add/sub with no FMA and no reassociation (the
pinned flags), so the stream is expected platform-invariant — the CI
determinism matrix ([docs/benchmarks/determinism-matrix.md](../../docs/benchmarks/determinism-matrix.md))
is the evidence; a desynced pair is declared unsupported for
`float_pinned_32` (ADR 0002), never re-baselined silently.

## How a build checks a baseline

`hello --expect BASELINE` (the scenario-side equivalent of
`laige-replay --expect` — the scenario's own binary carries the check
because a game log cannot be replayed by `laige-replay`; see
[detcheck.md](../../docs/api/detcheck.md)): exit 0 on identity, exit 1
with the first-divergence report, exit 2 on a baseline read/contract
error. The P0 OS jobs' ctest runs both backends against these baselines
on every platform (tests `hello_baseline_fpx` / `hello_baseline_fp32`);
the merge `detcheck` job adds the cross-compiler / sanitizer / Release
pairs.

## Regeneration policy

Regenerate only with a **deliberate, documented change** to the scenario
or its config (a new system, a constant change, a seed or tick-rate
change, an engine state-hash fix) — never to make a red CI green (that
would mask a real desync; CORE-008). Procedure: rebuild the canonical
Debug g++ tree, then

```console
$ ./samples/hello/bin/hello \
    > samples/hello/baselines/fixed_point_16_16/hash_stream.txt
$ ./samples/hello/bin/hello-fp32 \
    > samples/hello/baselines/float_pinned_32/hash_stream.txt
```

and verify the cross-compiler identity before committing (rebuild the
clang++ tree, re-run both binaries, `cmp` the streams against the new
baselines) — the same evidence the CI matrix produces. Every
regeneration is recorded in the
[determinism-matrix report](../../docs/benchmarks/determinism-matrix.md)
with the reason and the commit that changed the stream.
