# hello.laige — the headless template game (M1-SAMPLE-01)

The canonical Laige game shape in its smallest complete form (PRD §13,
NFR-13.5): one component (`PlayerPos`), one system (`MovePlayer`), one
entity (the player), deterministic by default. It is the reference
pattern for AI agents and new developers: every mark, declaration,
registration, and tick primitive a Laige game uses appears here, in
order.

## Project files

| File | Role |
|---|---|
| `hello.laige` | The project manifest (provisional M1 format: `laige.project` v1 JSON. The asset-pipeline format it replaces is owned by M3-ASSET-01). |
| `config.json` | The canonical sample config in the engine's declarative JSON surface (the version 1 schema, M1-CFG-01 — `laige-run`'s format). The values are identical to the config embedded in `hello.cpp` (see "Config" below). |
| `hello.cpp` | The game source. |
| `hello-baseline.cpp` | The `--expect` baseline check (M1-DET-04) — separate TU so `hello.cpp` stays inside the PRD §9.4 line budget. |
| `hello-fp32.cpp` | The `float_pinned_32` build variant (M1-DET-04, ADR 0002): defines the backend macros and includes `hello.cpp` — same game source, backend swapped by the build. |
| `baselines/` | The committed per-tick hash-stream baselines, one per backend (M1-DET-04; [baselines/README.md](baselines/README.md)). |
| `LICENSE` | Per-sample license (ADR 0001: each sample ships its own license; MIT, matching the repository). |

## Build and run

The sample builds with the repository (no separate configure):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The binaries land at the **source-tree** path `samples/hello/bin/`
(a `RUNTIME_OUTPUT_DIRECTORY` override — the CI detcheck job invokes
them from the repository root without arguments). Run them from
anywhere. Note the path is shared across build trees: building a second
tree (e.g. `build-asan/`) overwrites it, so rebuild the tree you intend
to run before executing the binary.

```sh
./samples/hello/bin/hello                    # 300 ticks, no replay (fixed_point_16_16)
./samples/hello/bin/hello --replay LOG       # 300 ticks + record LOG (debug builds)
./samples/hello/bin/hello --log LOG          # replay LOG, print its hash stream
./samples/hello/bin/hello --expect BASELINE  # compare the stream against a baseline
./samples/hello/bin/hello-fp32               # the same game on float_pinned_32
```

- **stdout** carries exactly the hash stream — nothing else: `N+1`
  lines of `<tick> <hash>` (tick `0` = the initial state, then one line
  per completed tick; the hash is 16 lowercase hex digits, the
  `World::stateHash` value — the detcheck scenario contract,
  [docs/api/detcheck.md](../../docs/api/detcheck.md)). `N` is 300
  (the M1 CI scenario length) or, in `--log` mode, the log's frame
  count.
- **stderr** carries the one-line summary (`hello headless ticks=300
  status=ok` / `hello replay ticks=300 status=ok`) and diagnostics.
- **Exit codes:** `0` ok; `1` a run failure (a tick failed, a replay
  write failed, the log finalization failed, or — with `--expect` — a
  per-tick identity mismatch with the baseline, with the
  first-divergence report on stderr); `2` usage, IO, or
  replay-identity error (nothing was run), including a baseline
  read/contract failure (`hello: baseline: ...`).
- `--replay` and `--log` are mutually exclusive; `--expect` composes
  with either run form; an unknown option or a missing option value is
  a usage error (`2`).

## The game

The player starts at the box center `(0, 0)` and moves 1 unit per tick
along the diagonal, wrapping in a 32-unit box (−16..16 on each axis): a
full crossing is 32 ticks (~0.53 s at 60 Hz). The step is exact in
`fpx16_16` (ADR 0002): one step crosses at most one wrap boundary.

The file reads top to bottom in the order a game is built:

1. **The component** — `PlayerPos` (the player's 2D position), marked
   with `LAIGE_COMPONENT` and `LAIGE_DETERMINISM_SAFE` (FR-1.2, G-R8).
2. **The system** — `MovePlayer`, declared with
   `LAIGE_SYSTEM(MovePlayer, 1)` (1 ms budget, FR-1.3) and declared
   `Write` access on `PlayerPos`.
3. **The run** — `main` shows the setup phase (world creation, component
   and system registration, entity creation + component placement, the
   pre-run system schedule), the tick loop (one `beginFrame()` + one
   `runSystems()` per tick — the `runReplay` shape), and the ordered
   teardown (CONC-006: finalize the replay log only on success, clear
   the world, shut down the logging facade).

The tick loop uses the engine's own tick primitives rather than
`laige::Engine::run_headless`: the detcheck contract needs a
per-completed-tick state hash on stdout, and the M1 Engine owns its own
loop and exposes no per-tick hook. The Engine remains the runner for
the built-in (no-game) scenario (`laige-run`); this template is the
runner for a game scenario, and `laige-replay`'s scope note
([docs/api/replay.md](../../docs/api/replay.md)) documents the split.

## Config

The canonical sample config is **embedded** in `hello.cpp` (the
aggregate at the top of `main`): 60 Hz, scene budget 8, the engine
churn default (256), the house seed `0x1F055EED`
([docs/testing.md](../../docs/testing.md)), deterministic — the
backend field is `fixed_point_16_16` in `hello` and
`float_pinned_32` in the `hello-fp32` build variant (the
`LAIGE_HELLO_BACKEND_ID` macro, ADR 0002: the backend is part of the
replay identity, so each binary's stream belongs to exactly one
backend). `config.json` is the declarative record of the same values
in the engine's JSON surface (the version 1 schema: `"version": 1`
plus the same keys — [docs/api/config.md](../../docs/api/config.md))
— it is what `laige-run --headless samples/hello/config.json`
consumes and what the M1-DET-04 baseline tooling reads; the template
binary keeps its CLI minimal (CORE-004) and does not re-read it.

## Replay

`--replay LOG` records the run's replay log (zero-length M1 frames —
no input system exists yet) through `laige::ReplayRecorder`
([docs/api/replay.md](../../docs/api/replay.md)); the log's identity is
the world's component schema + the config (seed, tick rate, math
backend, config hash — ADR 0002). **Recording is a debug-build
feature** (matching the engine's `startReplayRecording` policy): a
Release (NDEBUG) binary rejects `--replay` with exit 2.

`--log LOG` replays the log on a freshly set-up world: it loads the
log, checks the replay identity against this (world, config) — a
mismatch is a rejected replay (`2`), never a silent divergence — and
re-runs the log's frame count through the same tick loop, printing the
replayed hash stream. The replayed stream is bit-identical to the
original (determinism, S-7): the `hello_replay` CTest test asserts
byte-identity between the recorded and replayed runs.

Note: `laige-replay` (the engine tool) replays only logs recorded by
`laige-run` (the engine's built-in registrations); a game scenario's
log is replayed by the scenario's own binary — which is why this
template carries the `--log` mode.

## Baselines and the determinism matrix (M1-DET-04)

`--expect BASELINE` compares the run's per-tick hash stream against a
committed baseline with the `laige-replay --expect` contract (the
scenario's own binary carries the check because a game log cannot be
replayed by `laige-replay` — the replay identity, ADR 0002):
**exit 0** identity, **exit 1** first-divergence report (or stream-
length mismatch) on stderr, **exit 2** baseline read/contract error
before any tick is run. The committed baselines
([baselines/](baselines/README.md)) are the 301-line streams of the
**reference build (canonical Debug g++)**, one per backend;
`fixed_point_16_16` must be reproduced by every conforming build,
`float_pinned_32` is same-build/same-ISA (a desynced build is declared
unsupported — never re-baselined).

In CI: every P0 OS job's ctest runs `hello_baseline_fpx` /
`hello_baseline_fp32` (this job's own build vs the baselines, both
backends) plus the failure fixtures, and the merge `detcheck` job adds
the two-configuration pairs (g++ vs clang++ Debug, Debug+ASan vs
Release, both backends, via `laige-detcheck --run-a/--run-b`). Scope
and results:
[docs/benchmarks/determinism-matrix.md](../../docs/benchmarks/determinism-matrix.md).

## Tests

`ctest --test-dir build -R '^hello'` (registered in `tests/sample`):

| Test | Checks |
|---|---|
| `hello_scenario` | The CI scenario: no-arg run, exit 0, exactly 301 hash lines on stdout, `ticks=300 status=ok` on stderr. |
| `hello_replay` | Record (`--replay`) then replay (`--log`): exit 0, 301 hash lines, and the replayed stdout byte-identical to the original. |
| `hello_replay_missing` | `--log` on a missing log: exit 2, the `replay` error on stderr. |
| `hello_replay_malformed` | `--log` on a corrupt log (fixture): exit 2, the `replay` error on stderr. |
| `hello_mode_exclusive` | `--replay` + `--log` together: exit 2. |
| `hello_usage_unknown` | Unknown option: exit 2. |
| `hello_usage_missing_value` | A dangling option: exit 2. |
| `hello_config_valid` | `config.json` parses cleanly on the engine's config surface (`laige-run --headless` consumes it). |
| `hello_line_budget` | The game-code budget (PRD §9.4): non-comment, non-blank lines of `hello.cpp` < 100. |
| `hello_baseline_fpx` | **M1-DET-04:** this build's `hello --expect` the committed `fixed_point_16_16` baseline: exit 0, 301 hash lines — per-tick identity vs the reference build on every P0 OS job. |
| `hello_baseline_fp32` | **M1-DET-04:** the same assertion for `hello-fp32` vs the `float_pinned_32` baseline. |
| `hello_baseline_mismatch` | **M1-DET-04:** a derived baseline with one flipped hash: exit 1, the `laige-replay --expect` first-divergence report. |
| `hello_baseline_truncated` | **M1-DET-04:** a derived baseline cut to 160 lines: exit 1, the stream-length-mismatch report. |
| `hello_baseline_malformed` | **M1-DET-04:** a derived baseline with a bad hash token: exit 2 before any tick is run (the load-time contract, CORE-008). |
| `hello_baseline_missing` | **M1-DET-04:** a missing baseline file: exit 2, the actionable read error. |

## Line budget (PRD §9.4, NFR-13.5)

The game source is budgeted at **< 100 lines of code** (counted as
non-blank, non-comment lines of `hello.cpp`; the `hello_line_budget`
test enforces it). The file's comments carry the NFR-13.5
"heavily commented" requirement and do not count against the budget.
