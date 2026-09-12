# Determinism checker (`laige-detcheck`) and the scenario contract

The M0 skeleton of the PRD FR-11.5 determinism checker (roadmap step
**M0-TOOL-02**; AGENTS ARCH-010, TEST-004). Implementation:
`tools/detcheck/laige-detcheck.cpp` (the normative scenario contract is
in its header comment, which this page mirrors); CTest suite:
`ctest -R detcheck` (`tests/detcheck`). Canonical command form:
[building.md](../getting-started/building.md).

`laige-detcheck` runs a named scenario in **two build configurations**
(e.g. Debug+ASan vs Release, or two compiler builds) and asserts that the
per-tick state hashes are identical — bit-identity of the deterministic
state across configurations (NFR-8.3, FR-1.4). The engine state-hash API
arrives with **M1-DET-03** (`world.state_hash`); this tool therefore works
against the **hash-file output contract** defined below and compares two
such streams.

## Scenario contract

A scenario is a deterministic program that simulates a fixed number of
ticks from a fixed seed and prints exactly one line per simulated tick,
in order, to **stdout**:

```text
<tick> <hash>
```

- **`<tick>`** — non-negative decimal integer, no padding or leading
  zeros. The first line is tick `0`; each later tick is exactly one
  higher (no gaps, no duplicates).
- **`<hash>`** — exactly **16 lowercase hex digits**, the canonical text
  form of a 64-bit state hash (M1-DET-03's `world.state_hash`). The hash
  *algorithm* is not part of the contract: `laige-detcheck` compares the
  lines byte-for-byte, so only run-to-run identity matters.
- One line separator per line; a trailing newline on the final line is
  optional, and an optional trailing `\r` is tolerated (Windows CRLF).
- **stderr** is ignored by the checker (it remains visible in the CI job
  log).
- The scenario exits `0` on completion; any other exit code is a
  scenario failure.

The checker enforces the contract strictly (a malformed scenario is a
loud exit-2 error, never a silent mismatch — CORE-008) and bounds the
output: at most **65536** ticks and 64 bytes per line.

## Command line

```text
laige-detcheck --scenario=<name> [--ticks=N] [--seed=HEX|DEC]
laige-detcheck --run-a=<scenario-bin-A> --run-b=<scenario-bin-B>
               [-- scenario-args...]
```

**Mode 1 — `--scenario`** (M0 built-in scenarios, in-process):

| Name | Meaning |
|---|---|
| `synthetic` | 32 `fpx16_16` bodies + seeded `laige::Prng` input, run twice in two identical configurations — the self-check |
| `synthetic-perturbed` | the same, but run-b adds 1 unit to body 3's x at tick 7 — the perturbation fixture that proves the failure path |

`--ticks` (1..65536, default 256) and `--seed` (0xHEX or decimal, default
`0x1de7c0de`) apply to the built-in scenario only.

**Mode 2 — `--run-a`/`--run-b`** (the real mode, activated by
**M1-DET-04**): two builds of the same scenario source (two build
configurations) are executed and their hash streams compared. Everything
after the `--` separator is passed to both scenario binaries, so
scenario arguments can never collide with tool flags.

## Report and exit codes

stdout (stable and machine-greppable — LOG-001):

```text
detcheck scenario=synthetic result=OK ticks=256
  run-a: synthetic[seed=0x1de7c0de ticks=256 build=Debug]
  run-b: synthetic[seed=0x1de7c0de ticks=256 build=Debug]
```

```text
detcheck scenario=synthetic-perturbed result=DIVERGED first_diff_tick=7
  run-a: 7 485959cdde7acb9c
  run-b: 7 93a3363d5a1dffa9
```

In mode 2 the first line is
`detcheck scenario=<basename-a> vs <basename-b> result=... ticks=<n>` and
the `run-a`/`run-b` lines carry the full scenario paths. When one stream
ends early, the tick lines become stream-length notes
(`stream ends: <n> ticks`) with `result=DIVERGED`.

| Exit | Meaning |
|---|---|
| 0 | the two runs agree on every tick (deterministic) |
| 1 | divergence detected (a determinism failure — loud, CORE-008) |
| 2 | usage error, unknown scenario, a scenario run failed (non-zero exit, spawn failure), or a scenario violated the output contract (malformed line, tick gap, unbounded output) |

Windows only: a scenario run that produces no output and exits with an OS
process-start failure code (e.g. `0xC0000142` `STATUS_FATAL_APP_EXIT`,
seen right after a fresh build while a file filter still scans the new
`.exe`) is retried exactly once after a short delay before being
reported. The tool waits for the child process to terminate before reading
its exit code, so the `STILL_ACTIVE` sentinel (`259`) is never reported as
a scenario result. The retry cannot mask scenario behavior: a
deterministic scenario fails identically on the retry and the failure is
still reported as exit 2.

## The built-in synthetic workload

32 bodies of Q16.16 position/velocity (the default deterministic backend,
ADR 0002). Each tick: fixed-order integration (`x += vx`, `y += vy`),
wrap into a 64-unit box, then one seeded input event — the Prng picks the
body index and a nudge in [-4, 3] applied to x. Per-tick hash: FNV-1a 64
(the house constants, same as the `math_fixed` known-answer test) over
(tick, seed, every body's four raw words), big-endian per word
(endianness-independent).

**Determinism scope (ARCH-010):** pure unsigned-integer arithmetic
(`fpx16_16` ops + xorshift128+) — bit-exact across build, platform, ISA,
and compiler by the language standard; no float anywhere in the workload.
**Hash scope (M0):** tick counter + seed + body words. The Prng position
is a pure function of (seed, nudge history) in this workload; the
definitive scope — including PRNG state and the exact hash function — is
defined by M1-DET-03's `world.state_hash`, which replaces the ad-hoc FNV
computation of scenarios (the tool's line-by-line comparison is unchanged).

## Performance and bounds

A CI tool, not a hot path: one scenario run is O(ticks × 32); captured
streams are bounded (65536 lines × 64 bytes ≈ 1.5 MiB worst case per
run). Process execution is a plain pipe capture (fork/exec on POSIX,
`CreateProcessW` on Windows) — no shell, no temporary files, bounded
memory, and the scenario's stderr stays on the CI log.

## Test suite

`ctest -R detcheck` (`tests/detcheck`) — the tool tested with a synthetic
two-run scenario: the built-in self-check, the built-in perturbation
fixture, and the cross-binary mode against fixture scenario binaries
(one source, five compiled variants: clean / perturbed / bad output /
early exit / short stream). Each test is a generated `cmake -P` check
script asserting both the exit code and the required output fragments
(same pattern as `tests/api`).

## CI status

The `detcheck` CI job (`.github/workflows/ci.yml` and `ci-pull.yml`) runs
the built-in self-check on every PR and merge (like `include-lint` and
`api-manifest`, independent of the `ci:*` label selector — it is a
tooling check, not an additional P0 OS build). The real-scenario
comparison — two build configurations of M1-SAMPLE-01's `hello` — is
**skipped until that sample exists**; **M1-DET-04** activates it and
records the result per ARCH-010.
