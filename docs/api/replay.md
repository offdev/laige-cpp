# Replay recording and execution (`ReplayRecorder`, `runReplay`, M1-DET-02/03)

The M1 replay system in both halves: the **recording** (M1-DET-02;
PRD FR-1.4, FR-11.3, PRD Appendix A — replay = input log + seed,
ADR 0002, ARCH-007, SCALE-005) — a **versioned replay log format**
plus the `ReplayRecorder` (the atomic, size-bounded writer), the
`parseReplay`/`loadReplay` readers, and the replay-identity hashes —
and the **execution** (M1-DET-03): `World::stateHash` (the
deterministic state hash), `runReplay` (the identity-checked,
tick-by-tick re-run that produces the per-tick hash stream), and the
`laige-replay` runner that prints the stream and compares it against a
baseline. The engine records one frame per completed tick
(`Engine::startReplayRecording`), `laige-run --replay <path>` wires the
flag that M1-HEAD-01 stubbed, and `laige-replay --log <log> --config
<cfg> [--expect <baseline>]` closes the loop.

Public header: `src/laige-sim/include/laige/sim/replay.h` (the full
contract: format layout, identity hashes, the error tables, the
performance notes); implementation: `src/laige-sim/replay.cpp`.
Engine wiring: `src/laige-sim/engine.cpp` (`Engine::startReplayRecording`,
the per-tick write in the loop's `onTick` hook, finalization in
`run_headless`, abandonment in `shutdown`). CLI: `tools/run/laige-run.cpp`
(the `--replay` flag). Unit suite: `ctest -R replay_record`
(`tests/laige-sim/replay_record_tests.cpp`); fuzz: `ctest -R
fuzz_replay_parse` (the parser's malformed-input surface, TEST-005).

```cpp
laige::EngineConfig config;
config.tickRateHz = 60;
config.seed = 42;
laige::Engine engine = laige::Engine::create(config).value();

// Game code registers on the world (as always, before the run)...
engine.world()->registerComponent<MyComponent>();
engine.world()->registerSystem(MySystem_Def);

// ...then opts into replay recording, after all registration:
const laige::Status rec =
    engine.startReplayRecording("/tmp/run.log",
                                laige::kDefaultReplaySizeLimit);
if (rec.isError()) { /* actionable: rec.error() + rec.errorText() */ }

const laige::Status status = engine.run_headless(10'000);
// The log appears at "/tmp/run.log" only after the run completes
// successfully (atomic temp+rename); on a failed run there is no
// partial log at the final path.
```

## The log format (version 1, SCALE-005)

The format is **versioned** (ARCH-007): a reader rejects unsupported
versions explicitly, and the header's `formatVersion` is the single
version gate. All integers are **little-endian** on every platform
(SCALE-005: byte order specified, not assumed). Layout:

```
header (kReplayHeaderSize = 40 bytes, fixed):
  offset  0  magic "LGRP"            4 bytes
  offset  4  formatVersion           u16  (= kReplayFormatVersion = 1)
  offset  6  reserved                u16  (must be 0)
  offset  8  seed                    u64   (the run's master seed)
  offset 16  tickRateHz              u32
  offset 20  componentSchemaHash     u64   (ADR 0002 replay identity)
  offset 28  mathBackendId           u32   (the SimMathBackend value)
  offset 32  configHash              u64   (the EngineConfig encoding)
frame records (n of them, n == the trailer's frameCount):
  offset  0  tick                    u64   (1, 2, 3, ... — strictly
                                             sequential from 1)
  offset  8  byteLength              u32   (<= kMaxReplayFrameBytes = 1 MiB)
  offset 12  data                    byteLength bytes (the input frame;
                                             M1 frames are zero-length —
                                             the input data shape lands
                                             with M3-INPUT-03)
trailer (kReplayTrailerSize = 16 bytes, fixed, last in the file):
  offset  0  frameCount              u64
  offset  8  fileHash                u64   (FNV-1a 64, byte-stream,
                                             over every byte before the
                                             trailer)
```

The header's five identity fields are the **replay identity** of ADR
0002: seed, tick rate, component schema hash, math backend id, and
config hash. A log is only replayable by a run whose identity matches
(exact equality of all five) — the runner's check lands with
M1-DET-03. The `fileHash` is the canonical byte-stream FNV-1a 64
(offset basis `0xcbf29ce484222325`, prime `0x100000001b3` — fnv.org)
over header + frames: it catches truncation and bit rot, and makes the
log self-verifying.

**Malformed-input behavior** (SCALE-005: the parser's malformed-input
behavior is specified — every violation below is a `MalformedInput`
`Status`, never a crash and never a silent skip, CORE-008/ARCH-007):

| violation | result |
|---|---|
| size < header, null data with size > 0 | `MalformedInput` |
| bad magic | `MalformedInput` |
| unsupported `formatVersion` | `MalformedInput` (ARCH-007: explicit reject) |
| non-zero reserved field | `MalformedInput` |
| frame length > `kMaxReplayFrameBytes` | `MalformedInput` (checked before any overrun read) |
| frame record/payload extends past the body | `MalformedInput` |
| tick not `previous + 1` (first must be 1) | `MalformedInput` |
| trailer frameCount ≠ frames parsed | `MalformedInput` |
| trailer fileHash ≠ computed hash | `MalformedInput` |
| trailing bytes past the trailer | `MalformedInput` |

## The identity hashes (ADR 0002)

Both identity hashes are **word-stream FNV-1a 64, big-endian byte order
per u64 word** (the house convention: the determinism state hashes, the
Prng golden vectors, `laige-detcheck`) — pure integers, no addresses,
no wall clock (ARCH-010):

- **`componentSchemaHash(const World&)`** — over
  `[componentCount, then per registered type in id order: id, size,
  alignment]`. It is a function of the *registration order* (ids are
  dense in registration order): two worlds that registered the same
  types in the same order hash identically; a different order hashes
  differently. O(types), stack-only (769 words max — no allocation).
- **`configHash(const EngineConfig&)`** — over
  `[tag 1, tickRateHz, entityCapacity, churnPerFrameBudget, seed,
  determinism.enabled, determinism.math]`. The tag word identifies this
  provisional encoding; M1-CFG-01 refines the config schema and this
  encoding with it, under the format's versioning.
- **`makeReplayIdentity(const World&, const EngineConfig&)`** —
  assembles the header's `ReplayIdentity` from the two hashes plus the
  config's seed/tick rate/backend.

These are the *replay identity*, not the sim state: M1-DET-03's
`world.state_hash` hashes the live sim state (components, PRNG state,
tick) on top of the identity the header already carries.

## The recorder (`ReplayRecorder`)

Write side, move-only: `create(identity, path, maxBytes)` →
`writeFrame(tick, data, len)` per completed tick → `finish()`.

- **Atomic** — the recorder writes `path + ".tmp"` (same filesystem as
  `path`, so the final `rename` is atomic) and publishes `path` only
  on a successful `finish()`. An interrupted or failed recorder leaves
  **no file at the final path** (the temp is removed by the
  destructor); a rename failure leaves the temp for inspection (the
  complete data is in it — the caller's Error log names it).
- **Size-bounded** — `maxBytes` bounds header + frames + trailer
  together (`0` means `kDefaultReplaySizeLimit`, 128 MiB). A write that
  would exceed the cap is a `BudgetExhausted` `Status` (CORE-008: no
  unbounded growth, PERF-008/SCALE-003); a cap below
  `kMinReplaySizeLimit` (header + trailer = 56) is rejected at
  `create` as `InvalidArgument` (a complete log could never fit).
  `finish()` on a cap that cannot fit the trailer is the same
  `BudgetExhausted`.
- **Strict tick sequence** — frames must arrive as tick 1, 2, 3, ...
  (the engine's `onTick` hook hands the recorder the completed tick
  count); any other tick is an `InvalidArgument`.
- **Sticky failure** — the first failure sets the recorder's state;
  every later `writeFrame`/`finish` returns the same `Status` (no
  partial recovery, no second failure of a different code).
- **Error table** — empty path / cap below minimum / bad tick /
  frame over `kMaxReplayFrameBytes` → `InvalidArgument`; cap breach →
  `BudgetExhausted`; open/write/rename I/O → `IoError`.

## The readers (`parseReplay`, `loadReplay`)

- **`parseReplay(const uint8_t*, size)`** — the in-memory parser; the
  full malformed-input table above. O(size), one output allocation per
  frame (the `ReplayLog`'s frames); no per-byte allocation.
- **`loadReplay(path, maxBytes = kDefaultReplaySizeLimit)`** — the file
  wrapper: a bounded read (the ADR 0003 JSON-bound precedent — a file
  larger than `maxBytes` is a `MalformedInput`, not a truncated
  parse), then `parseReplay`. Missing/unreadable file → `IoError`;
  empty path → `MalformedInput`.

`ReplayLog` is the parsed value: `identity` (the header's five fields)
plus `frames` (`tick` + the payload bytes). The frame payload is an
**opaque byte blob** by design — M1 records zero-length frames (there
is no input system yet); the input data shape lands with M3-INPUT-03,
and the format version is the migration point (ARCH-007).

## Engine integration (`Engine::startReplayRecording`)

```
Status Engine::startReplayRecording(std::string_view path,
                                    std::uint64_t maxBytes) noexcept;
bool   Engine::replayRecordingActive() const noexcept;
std::uint64_t Engine::replayBytesWritten() const noexcept;
```

- **Opt-in** — recording is off by default; the disabled cost is one
  null check per tick (LOG-003/PERF-003: no formatting, no
  allocation, no I/O when off).
- **Phase** — call it **once, after all component/system
  registration, before `run_headless`**: the identity is captured from
  the live world + config at call time, so a registration after the
  call makes the recorded identity stale (the schedule-stale
  precedent, M1-SYS-02).
- **Per tick** — the engine's loop `onTick` hook writes one
  zero-length frame per completed tick (M1: no input yet; the frame
  bytes are the future input blob).
- **Failure stops the run** — a recorder failure mid-run is not
  swallowed: `run_headless` returns the recorder's `Status` (at most
  one frame of extra ticks, the loop's bounded frame contract), the
  log is **not** published (no partial file at the final path), and
  the ordered shutdown still runs.
- **Finalization** — on a successful bounded run, `run_headless`
  calls `finish()` (trailer + atomic rename) and logs
  `replay/record_finished` (Info: path, bytes, frames); a failed run's
  shutdown logs `replay/record_aborted` (Warn: path, bytes) and
  discards the temp.
- **Debug builds only** — in release builds (`NDEBUG`) the call is an
  `InvalidArgument` with a `replay/record_disabled` warn (recording
  is a development tool; the format and the engine wiring exist in
  every build, the opt-in does not).
- **Structured events** (AGENTS §14, subsystem `replay`):
  `record_started` (Info: path, size_limit, seed, math_backend,
  config_hash, schema_hash), `record_finished` (Info),
  `record_failed` (Error: path, tick, error), `record_aborted`
  (Warn), `record_already_started` (Warn), `record_start_failed`
  (Warn), `record_disabled` (Warn, release builds).

## `laige-run --replay` (the CLI)

```
laige-run --headless CONFIG.json [--ticks N] [--replay LOG]
```

`--replay LOG` now records the run (it was the M1-HEAD-01 stub):

- The engine's identity is captured at start (laige-run registers no
  game components of its own — the built-in registration is complete at
  `create`), the run records one zero-length frame per completed tick,
  and on a clean bounded run the log is atomically published at `LOG`.
- **The recorded tick count is platform-stable:** a bounded
  `--ticks N` run completes exactly N ticks (the CLI's frame budget 1
  — api/engine.md), so a log recorded for N ticks carries exactly N
  frame records (N + 1 hash lines on replay) on every platform. A
  run under the engine's default catch-up budget can overshoot the
  target by up to budget - 1 ticks under overload; the CLI never
  uses that budget for bounded runs.
- **Debug builds only**: in a release build the flag fails with
  `InvalidArgument` (`replay/record_disabled`), the same contract as
  the engine call.
- **Failure is exit 2** (the flag's contract, like a config error):
  `laige-run: replay: {error text}` on stderr, the run does not
  happen. A mid-run recording failure exits `1` (the run failed — the
  `status=` summary line carries the error name) with no partial log
  at `LOG`.

## The state hash (`World::stateHash`, M1-DET-03)

`std::uint64_t World::stateHash(std::uint64_t tick) const noexcept;`
— the 64-bit FNV-1a hash of the authoritative sim state at `tick`
completed ticks. Full contract in
[api/entity.md](entity.md) ("The deterministic state hash"): the
canonical stream (tick → live handles → per-archetype component bytes
in lexicographic signature order → per-system PRNG substream state),
the scope in/out lists (history and non-authoritative state excluded —
convergent worlds hash identically), and the complexity/allocation
contract (cold path, `O(capacity + live bytes + kMaxArchetypes²)`, no
allocation, `const`, no logging).

## The identity check (`replayIdentityDiff`, M1-DET-03)

```cpp
struct ReplayIdentityDiff { /* one bool per identity field */ };
ReplayIdentityDiff replayIdentityDiff(const ReplayLog&, const World&,
                                      const EngineConfig&) noexcept;
```

ADR 0002's replay identity is enforced **field by field**: a log
replayed against a `(world, config)` whose seed, `tickRateHz`,
`componentSchemaHash`, `mathBackendId`, or `configHash` differs is a
**rejected replay** — `runReplay` fails with `ErrorCode::InvalidArgument`
plus the `replay/identity_mismatch` structured warn naming exactly
which fields differ (both sides' values — the log's and the
world+config's). The caller must surface it (CORE-008: never silently
replay a foreign log); `replayIdentityDiff` is exposed for callers who
want the per-field report without a full run.

## The execution half (`runReplay`, M1-DET-03)

```cpp
struct ReplayRunResult { std::vector<std::uint64_t> tickHashes; };
Result<ReplayRunResult, ErrorCode> runReplay(const ReplayLog&, World&,
                                             const EngineConfig&) noexcept;
```

One deterministic replay of a loaded log against a world:

1. **Identity check** — `replayIdentityDiff`; any differing field is
   `ErrorCode::InvalidArgument` (the `replay/identity_mismatch` warn
   names the fields and both sides' values).
2. **Determinism check** — the world's deterministic mode must be
   enabled (it is by default); a disabled world fails with
   `ErrorCode::InvalidArgument` + the `replay/determinism_disabled`
   warn — replaying a non-deterministic sim would produce
   meaningless hashes.
3. **Schedule** — `world.scheduleSystems(...)` once, before the loop
   (the schedule is a function of the registrations, which the
   identity already pinned); a schedule failure returns the world's
   `Status` (already logged by the world).
4. **Loop** — one frame per tick: `beginFrame(); runSystems(schedule);`
   (the log's frames are zero-length in M1 — there is no input system
   yet; non-empty frames are accepted and ignored until M3-INPUT-03
   defines consumption). A failing tick returns the world's `Status`
   and the replay stops (no partial hashes in the result).

The result's `tickHashes[i]` is `world.stateHash(i)` after `i`
completed ticks — **size `frameCount + 1`**: index 0 is the initial
state's hash (before any tick), and index `i` (≥ 1) is the state
after tick `i`. That is the **hash line contract** the `laige-replay`
runner prints and compares:

```
<tick> <hash>            tick 0,1,2,...,N — one line per completed tick,
                          plus the tick-0 initial line (N+1 lines total)
<hash> = 16 lowercase hex digits (the canonical FNV-1a 64 text form)
```

No allocation on the replay *per tick* beyond the result vector's
single growth; the per-tick work is exactly the normal engine tick
(the replay is the sim, not a second engine).

## The `laige-replay` runner (M1-DET-03)

```
laige-replay --log LOG --config CONFIG.json [--expect BASELINE]
```

- **stdout is only hash lines** (the contract above) — pipeable and
  diff-able; the summary and every diagnostic go to **stderr**.
- **Exit codes:** `0` — replayed and (if `--expect`) matched;
  `1` — **hash mismatch** (the first diverging tick is reported) or
  **stream-length mismatch** (the first missing/extra tick);
  `2` — usage/identity/log/baseline error (nothing was replayed).
- **`--expect BASELINE`** — compares the replayed stream against a
  baseline file of `<tick> <hash>` lines (CRLF tolerated, trailing
  newline optional). The first mismatch is reported on stderr as an
  actionable line pair —
  `hash mismatch at tick 5 (first divergence)` + the baseline and
  replay values — and the run exits `1`. A different stream length is
  reported as `baseline stream length mismatch` + `first
  missing/extra at tick N`, also `1`.
- **Baseline grammar is strict** — exactly `<tick> <16 lowercase hex>`,
  tick strictly `0, 1, 2, ...`; any other line is a malformed-baseline
  failure (`2`) naming the offending line.
- **Bounds (CORE-005):** config read ≤ 1 MiB; baseline ≤ 8 MiB /
  65 536 lines / 64 bytes per line (named constants in
  `tools/replay/laige-replay.cpp`); no unbounded read before parsing
  (the ADR 0003 bounded-read precedent).
- **Scope:** `laige-replay` replays logs recorded by `laige-run
  --headless --replay` (the engine's built-in registration). A
  game-scenario log is replayed by the scenario binary itself: it loads
  the log (`loadReplay`), checks the replay identity against its own
  (world, config) (`replayIdentityDiff` — a mismatch is a rejected
  replay, never a silent divergence), and re-runs the log's frame count
  through the same tick loop `runReplay` drives (M1-SAMPLE-01's
  `hello --log` wires this; M1-DET-04's detcheck `--run-a/--run-b`
  compares two scenarios' hash streams).

## Performance (PERF-002/003, LOG-003)

- **Disabled** — one null check per tick in the engine's onTick hook;
  no allocation, no logging, no I/O.
- **Enabled** — one bounded stdio write per completed tick (the 12-byte
  record; M1 frames carry no payload), one FNV-1a extension over the
  record bytes, one size check. The write is to a page-cache-backed
  local file (PERF-002: the recording is an explicit, bounded, opt-in
  cost — the API makes it visible, it is never hidden). `create` is
  cold (one open + one 40-byte header write); `finish` is cold (one
  16-byte trailer write + one rename).
- **Bounds** — total log size ≤ `maxBytes` (128 MiB default); a single
  frame ≤ 1 MiB; the reader's read is bounded by `maxBytes` before
  parsing.
- **Traps** — recording a long run at the default cap publishes the
  log only if it fits; a `BudgetExhausted` mid-run stops the run (by
  design — a truncated log would be useless).

## Misuse warnings

- **Start recording after all registration, before the run** — the
  identity is captured at call time; registering components after it
  makes the recorded schema hash stale (the log would be rejected by
  the M1-DET-03 runner's identity check).
- **One recording per run** — a second `startReplayRecording` on the
  same engine is an `InvalidArgument` plus a `record_already_started`
  warn.
- **Release builds cannot record** — `NDEBUG` is the contract
  (debug-only tooling); use a debug build for capture.
- **Do not hand-write frame ticks** — `writeFrame` expects the strict
  1, 2, 3, ... sequence; a hand-built tick violates the format.
- **The cap is total, not per-frame** — header + all frames + trailer
  must fit; size your cap against the expected tick count
  (≈ `40 + 12 × ticks + 16` for M1 zero-length frames).

## Testing and CI

- `ctest -R replay_record` — the step's Verify: the format round trip
  (record → parse → identical bytes, including the PRNG-payload case
  and the 1 MiB frame boundary), the full malformed-input table
  (every truncation cut of a valid log, bad magic/version, length
  overrun, tick sequence, trailer count, fileHash, trailing garbage),
  the recorder contract (atomic publish, no partial file on
  interruption, the size limit at the exact boundary, sticky failure,
  move semantics), the identity hashes (registration-order stability,
  per-field sensitivity), and the engine integration (8-tick run → 8
  empty frames + matching identity; the mid-run failure stop with the
  `record_failed`/`record_aborted` events; double-start and
  stopped-engine failures).
- `ctest -R replay_replay` — the M1-DET-03 Verify (the
  `StateHash.*` + `DetReplay.*` suites in
  `tests/laige-sim/replay_replay_tests.cpp`): the state hash's known-
  answer vector, capacity/handle/component/archetype/PRNG sensitivity,
  convergent-world equality (different histories, same live state,
  same hash), the zero-allocation proof; and the replay half — the
  **500-tick record → replay integration** (identical hash streams,
  the step's headline test), a perturbed-world divergence at the exact
  tick, the per-field identity-mismatch rejection, the
  determinism-disabled rejection, and an engine round trip (record
  under `run_headless`, replay through two fresh engines, world-level
  twin state comparison).
- `ctest -R "^replay_"` — the `laige-replay` runner's CTest entries
  (`tests/replay/`, one generated check script per case): the smoke
  stream contract (N+1 lines, tick sequence, 16-hex hashes), the
  double-run determinism, `--expect` match / perturbed (divergence at
  the right tick) / truncated (length report) / malformed (line
  report) / identity mismatch (field report) / usage error / missing
  log — exit codes 0/1/2 asserted per case.
- `ctest -R fuzz_replay_parse` — the parser's malformed-input surface
  under the bounded-every-commit fuzz gate (1000 deterministic inputs;
  the corpus includes a valid v1 log as a mutate/truncate base; TEST-005,
  NFR-8.7).
- The include-graph lint and the API manifest (`laige-api.json`)
  cover the new public declarations (`stateHash`, `replayIdentityDiff`,
  `runReplay`; regenerated in this change).

## Related

- [api/entity.md](entity.md) — `World`, the handle contract, and
  `World::stateHash` (the state-hash scope and canonical stream).
- [api/engine.md](engine.md) — `Engine`, the run contract, the
  `startReplayRecording` wiring, the `laige-run` CLI.
- [api/detcheck.md](detcheck.md) — the cross-configuration hash-stream
  comparison (M1-DET-04) built on the same hash lines.
- [concepts/determinism.md](../concepts/determinism.md) — the
  determinism scope, the replay identity (ADR 0002), the
  SimMath-only rule.
- [api/determinism.md](determinism.md) — the G-R8 trait and the
  `SimMathBackend` ids.
- [ADR 0002 — Deterministic math strategy](../decisions/0002-deterministic-math.md)
  — replay identity: inputs + seed + backend + config.
- [testing.md](../testing.md) — the fuzz-runner and seed conventions
  this suite follows.
