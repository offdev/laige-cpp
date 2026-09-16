# Replay recording (`ReplayRecorder`, M1-DET-02)

The M1 replay **recording** (M1-DET-02; PRD FR-1.4, FR-11.3, PRD
Appendix A — replay = input log + seed, ADR 0002, ARCH-007, SCALE-005):
a **versioned replay log format** plus the `ReplayRecorder` (the atomic,
size-bounded writer), the `parseReplay`/`loadReplay` readers, and the
replay-identity hashes. The engine records one frame per completed
tick (`Engine::startReplayRecording`) and `laige-run --replay <path>`
wires the flag that M1-HEAD-01 stubbed. Replay **execution** (the
`laige-replay` runner, `world.state_hash`) is M1-DET-03; this step
lands the recording half.

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
- **Debug builds only**: in a release build the flag fails with
  `InvalidArgument` (`replay/record_disabled`), the same contract as
  the engine call.
- **Failure is exit 2** (the flag's contract, like a config error):
  `laige-run: replay: {error text}` on stderr, the run does not
  happen. A mid-run recording failure exits `1` (the run failed — the
  `status=` summary line carries the error name) with no partial log
  at `LOG`.

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
- `ctest -R fuzz_replay_parse` — the parser's malformed-input surface
  under the bounded-every-commit fuzz gate (1000 deterministic inputs;
  the corpus includes a valid v1 log as a mutate/truncate base; TEST-005,
  NFR-8.7).
- The include-graph lint and the API manifest (`laige-api.json`)
  cover the new public header (regenerated in this change).

## Related

- [api/engine.md](engine.md) — `Engine`, the run contract, the
  `startReplayRecording` wiring, the `laige-run` CLI.
- [concepts/determinism.md](../concepts/determinism.md) — the
  determinism scope, the replay identity (ADR 0002), the
  SimMath-only rule.
- [api/determinism.md](determinism.md) — the G-R8 trait and the
  `SimMathBackend` ids.
- [ADR 0002 — Deterministic math strategy](../decisions/0002-deterministic-math.md)
  — replay identity: inputs + seed + backend + config.
- [testing.md](../testing.md) — the fuzz-runner and seed conventions
  this suite follows.
