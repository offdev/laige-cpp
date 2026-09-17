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
landed with **M1-DET-03** (`World::stateHash`,
[api/entity.md](entity.md)); this tool works against the **hash-file
output contract** defined below and compares two such streams. The
real scenario now exists: **M1-SAMPLE-01**'s `hello` (samples/hello)
prints exactly this contract — the tick-0 line plus one
`World::stateHash` line per completed tick — and the CI job's real-
scenario step runs it; **M1-DET-04** activates the two-configuration
comparison of it (the tool's line-by-line comparison is unchanged).

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
laige-detcheck --compare-combined=<combined-stream-file>
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

Mode 2 is **two-stage**, and `--compare-combined` is its second stage:

- **Phase 1 — `--run-a`/`--run-b`** spawns both scenario binaries. Each
  child inherits this process's **stdout** (no capture pipe is created for
  it), so its tick lines land in whatever captures the checker's stdout,
  delimited by the marker lines the checker itself emits:

  ```text
  @@DETCHK-RUN-A-BEGIN@@
  <tick A stream>
  @@DETCHK-RUN-A-END <exitcode>@@
  @@DETCHK-RUN-B-BEGIN@@
  <tick B stream>
  @@DETCHK-RUN-B-END <exitcode>@@
  ```

  Phase 1 exits `0` when both scenario processes ran to completion, `2`
  on a spawn failure or a non-zero scenario exit (the reason on stderr).
  It does **not** read back or compare the streams.

- **Phase 2 — `--compare-combined=<file>`** reads the combined stream the
  caller wrote (phase 1's captured stdout), splits it at the markers,
  re-runs the scenario contract on each run, compares the two streams,
  and reports (the report below).

The split is forced by the CI Windows runner: it does not deliver handles
the checker process creates (pipes or files, even with the `INHERIT` bit
set, even after duplication) to child processes through `STARTUPINFO` —
only handles the process itself inherited from its parent are delivered
(measured in the M0-TEST-01 CI, runs 24/25). A scenario child therefore
cannot be handed a capture pipe; its stdout must be the checker's own
stdout, which the CTest check script (`execute_process`) captures and
hands back in phase 2. The mechanism is identical on every platform, so
the two-stage flow is exercised by the local suite on POSIX as well.

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

In mode 2 (phase 2) the first line is
`detcheck scenario=combined result=... ticks=<n>` with `run-a`/`run-b`
labels. When one stream ends early, the tick lines become stream-length
notes (`stream ends: <n> ticks`) with `result=DIVERGED`.

| Exit | Meaning |
|---|---|
| 0 | the two runs agree on every tick (deterministic) |
| 1 | divergence detected (a determinism failure — loud, CORE-008) |
| 2 | usage error, unknown scenario, a scenario run failed (non-zero exit, spawn failure), or a scenario violated the output contract (malformed line, tick gap, unbounded output) |

`--run-a`/`--run-b` (phase 1) exits `0` when both scenario processes ran
to completion and `2` on any spawn or scenario failure; the `0`/`1`
comparison result comes from the `--compare-combined` phase.

On Windows, phase 1 spawns with `CreateProcessW` (no `STARTUPINFO` — no
handles are handed to the child, see above), waits for the child with
`WaitForSingleObject`, and reads its exit code only after termination, so
the `STILL_ACTIVE` sentinel (`259`) is never reported as a scenario exit
code; a signalled child is reported as `128 + signal`, a non-zero scenario
failure either way.

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
M1-DET-03's `World::stateHash` ([api/entity.md](entity.md)), which the
`hello` scenario (M1-SAMPLE-01) prints directly on its stdout stream.

## Performance and bounds

A CI tool, not a hot path: one scenario run is O(ticks × 32); captured
streams are bounded (65536 lines × 64 bytes ≈ 1.5 MiB worst case per
run). Process execution is plain inheritance (fork/exec on POSIX,
`CreateProcessW` on Windows) — no shell, no capture pipe, bounded memory,
and the scenario's stderr stays on the CI log. Phase 2 reads the combined
stream file back with a bounded read capped by the contract (2 × 65536
lines + markers ≈ 1.5 MiB worst case); the file is written by the check
script between phases and lives in the build tree.

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
tooling check, not an additional P0 OS build). It also runs the **real
scenario** (M1-SAMPLE-01): both `--run-a` and `--run-b` point at
`samples/hello/bin/hello` (the no-arg 300-tick headless run — the step
activates once the sample binary exists, which it now does), so every
PR and merge executes the template game's detcheck contract: both runs
must complete (exit 0) and print the 301-line hash stream. **M1-DET-04**
replaces this same-configuration step with the two-configuration
comparison (`build-asan/bin/hello` vs `build/bin/hello`) and records the
result per ARCH-010.
