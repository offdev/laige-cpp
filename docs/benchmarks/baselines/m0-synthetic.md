# Baseline: `m0-synthetic` — M0 exit gate, synthetic harness workload

Recorded by **M0-EXIT-01** (2026-09-13). This is the **first** baseline
file; it is immutable (methodology §4 — superseding it later adds a new
file, it is never edited).

## What this baseline measures

The `synthetic` suite of `laige-bench` (M0-CORE-08): a deterministic,
allocation-free 4096-step Marsaglia LCG64 + double pipeline with named
constants and no RNG. **It models no physical quantity** — it is the
harness stand-in that proves the measurement pipeline (timer → histogram
→ statistics → budget check → report) end to end before the real PRD
§8.1 workloads land with their subsystems in M1+. Its numbers are
therefore **not** measurements of any real engine budget, and this
baseline **does not update any `measured` field in `budgets.json`**
(all 15 entries keep `measured: 0` = not yet measured until their
subsystem lands).

## AGENTS §12 metadata

| # | Field | Value |
|---|---|---|
| 1 | Hardware | AMD Ryzen 9 7950X3D (16 cores / 32 threads), 64 GB RAM |
| 2 | OS | CachyOS (Arch-based Linux), kernel `7.2.2-1-cachyos`, x86_64 |
| 3 | Compiler and version | `g++ (GCC) 16.2.1 20260810` |
| 4 | Build type | `Debug` (canonical, `build/` tree) |
| 5 | Relevant flags | Engine policy (NFR-8.10): `-Wall -Werror -fno-exceptions -fno-rtti`; SimMath pinned set (ADR 0002): `-ffp-contract=off -fno-associative-math`. No sanitizers (canonical tree). |
| 6 | Dataset / workload | `synthetic` — deterministic 4096-step LCG64+double pipeline (Marsaglia constants), allocation-free, no RNG; harness stand-in, no physical meaning |
| 7 | Warm-up | 200 discarded iterations (`--warmup=200`) |
| 8 | Sample count | `n=2000` (histogram window `capacity >= runs`; no truncation) |
| 9 | Summary statistics | `min=0.009708 mean=0.00995194 p50=0.009768 p95=0.010019 p99=0.014748 max=0.023965` (ms) |
| 10 | Before / after | `before=0` (not yet measured — first recording) · `after=0.00995588` ms (the `--budget` proof run's `mean`) · `target=3` ms (`sim_tick_avg`) |

## Verbatim run output

### Run A — canonical statistics run

Command (run from the repository root; `LAIGE_BENCH_MACHINE` set to the
machine string so the `context:` line carries it):

```console
$ ./build/bin/laige-bench --suite=synthetic --runs=2000 --warmup=200
```

```text
suite=synthetic runs=2000 warmup=200
  stats: n=2000 min=0.009708 mean=0.00995194 p50=0.009768 p95=0.010019 p99=0.014748 max=0.023965
  context: workload=synthetic build=GCC 16.2.1 20260810, Debug machine=AMD Ryzen 9 7950X3D 16C/32T, 64 GB RAM, CachyOS (Arch-based) x86_64 warmup=200
```

Exit code: `0`.

### Run B — end-to-end `budgetCheck` pipeline proof

Command:

```console
$ ./build/bin/laige-bench --suite=synthetic --runs=2000 --warmup=200 --budget=sim_tick_avg
```

```text
suite=synthetic runs=2000 warmup=200
budget=sim_tick_avg result=PASS metric=mean unit=ms
  after=0.00995588 before=0 target=3
  stats: n=2000 min=0.009658 mean=0.00995588 p50=0.009909 p95=0.009969 p99=0.014588 max=0.023154
  context: workload=10k entities, 2k dynamic bodies (PRD 8.1) build=GCC 16.2.1 20260810, Debug machine=AMD Ryzen 9 7950X3D 16C/32T, 64 GB RAM, CachyOS (Arch-based) x86_64 warmup=200
```

Exit code: `0`.

**Interpretation note (CORE-008, no silent claims):** Run B checks the
synthetic stand-in against the `sim_tick_avg` `budgets.json` entry
purely to exercise `loadBudgets` + `budgetCheck` end to end (the gate's
"budget harness runs end-to-end" clause). The `context: workload=` line
echoes the budget entry's workload text — it is **not** a claim that a
10k-entity simulation tick was measured. The check passing
(`after < target`, `before == 0` so no regression band applies per
methodology §5) proves the pass/fail/exit-code machinery, not a real
budget.

## Measured on

- Commit: `829026f60c0f7c779c82882e608ed04e15cadc5a` (`[M0-DOC-01] docs/ skeleton + index (#14)`, `master`)
- Date: 2026-09-13 (09:45 UTC session time)
- Local trees for cross-check (same commit, all `ctest` 32/32, zero
  warnings under the NFR-8.10 policy): `build` (g++ 16.2.1 static),
  `build-shared` (static→shared, NFR-8.9), `build-asan` (ASan+UBSan
  fatal), `build-tsan` (TSan `halt_on_error=1`), `build-clang`
  (clang++ 22.1.8).

## Related gate evidence

- CI green on all 3 P0 OSes (merge lane): [run 34749756361](https://github.com/offdev/laige-cpp/actions/runs/34749756361) on this commit — all 10 jobs `success`, each under 1.2 min; `ctest` **32/32 passed** in every P0 OS job (linux-gcc, linux-clang, linux-asan+UBSan, linux-tsan, windows-msvc, macos-arm64, macos-intel).
- Fuzz lane (bounded, in every P0 job's ctest): `fuzz_json_parse` — `laige-fuzz json_parse --runs=1000` clean locally (seed `0x1F055EED`).
- Determinism checker self-check: `laige-detcheck --scenario=synthetic` → `result=OK ticks=256`.
- API manifest drift: `laige-api-scanner --root . --check laige-api.json` → up to date (376 symbols).
- Include-graph lint: `python3 tools/laige-include-lint` → OK, `count: 1 (budget: 10, PRD §11)`.
