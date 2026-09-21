// laige-sim replay recording (M1-DET-02).
//
// FR-1.4 (determinism mode), FR-11.3 (replay), PRD Appendix A (replay =
// the input log + the seed); ARCH-007 (persistent and replay data MUST
// be versioned; readers MUST reject or migrate unsupported data
// explicitly); SCALE-005 (serialization formats specify byte order,
// bounds, version, compatibility, and malformed-input behavior);
// ADR 0002 (the backend id is part of replay identity: replay = inputs
// + seed + math backend + config hash); ARCH-010 (the deterministic
// state is a pure function of (config, seed, registration order, N,
// inputs)); CORE-005 (the size bounds below are named constants).
//
// This header carries the FULL replay contract: the RECORDED half —
// the versioned, byte-exact log of one deterministic run (the replay
// identity in a fixed header plus one opaque input frame per completed
// tick) — and the EXECUTION half (M1-DET-03): loading a log, checking
// its identity against the caller's (world, config), feeding its
// frames back through the sim tick by tick, and producing the per-tick
// state hashes (World::stateHash) that the laige-replay runner
// (tools/replay) prints and compares against a baseline.
//
//   ReplayIdentity    The replay identity (ADR 0002) as a plain value.
//   ReplayFrame       One recorded input frame: the completed tick
//                     number plus the opaque byte blob.
//   ReplayLog         A parsed replay log: the identity plus the
//                     frames in tick order.
//   ReplayRecorder    The write side: create() -> writeFrame() per
//                     completed tick -> finish(). Atomic (temp file +
//                     rename), size-bounded (no unbounded growth), and
//                     a partial log is never left at the final path.
//   parseReplay       The byte-level reader (memory form).
//   loadReplay        The file reader (bounded read + parseReplay).
//   componentSchemaHash
//                     The component registry's deterministic hash.
//   configHash        The EngineConfig's deterministic hash (the
//                     simulation-affecting fields only — M1-CFG-01's
//                     declared presentation fields are excluded).
//   makeReplayIdentity
//                     The full identity from (world, config).
//   ReplayIdentityDiff
//                     The field-by-field identity comparison result.
//   replayIdentityDiff
//                     Compare a log's identity against (world, config).
//   ReplayRunResult   One replay's per-tick state hashes.
//   runReplay         The execution half: identity check + the
//                     deterministic tick-by-tick replay + the hashes.
//
// ---------------------------------------------------------------------------
// The log format (version 1; SCALE-005: byte order, bounds, version,
// compatibility, malformed-input behavior)
// ---------------------------------------------------------------------------
//
// A replay log is one flat byte stream. EVERY integer is LITTLE-ENDIAN
// (host-endianness-independent, SCALE-005):
//
//   header (kReplayHeaderSize = 40 bytes, fixed):
//     offset  0  magic "LGRP" (4 bytes)
//     offset  4  formatVersion  u16  (= kReplayFormatVersion = 1)
//     offset  6  reserved       u16  (= 0)
//     offset  8  seed           u64
//     offset 16  tickRateHz     u32
//     offset 20  componentSchemaHash u64
//     offset 28  mathBackendId  u32  (the laige::SimMathBackend value)
//     offset 32  configHash     u64
//
//   frame (kReplayFrameRecordOverhead = 12 bytes + byteLength, one per
//   completed tick, in tick order):
//     offset 0  tick       u64  (= 1 + the number of frames before it —
//                              the strict 1, 2, 3, ... sequence)
//     offset 8  byteLength u32  (<= kMaxReplayFrameBytes)
//     offset 12 frame bytes (byteLength of them)
//
//   trailer (kReplayTrailerSize = 16 bytes, fixed, last in the file):
//     offset 0  frameCount  u64  (= the number of frames in the body)
//     offset 8  fileHash    u64  (FNV-1a 64 over EVERY byte before the
//                                trailer — the canonical byte-stream
//                                FNV-1a: offset basis
//                                0xcbf29ce484222325, prime
//                                0x100000001b3, fnv.org)
//
// Versioning (ARCH-007): readers accept formatVersion == 1 and reject
// every other version explicitly (MalformedInput — the "reject" branch
// of ARCH-007; a migration path, if one is ever needed, lands with the
// format step that raises the version).
//
// Bounds (SCALE-005): a frame's byteLength is u32 but is capped at
// kMaxReplayFrameBytes (1 MiB) — M1 carries zero-length frames (no
// input system exists yet; M3-INPUT-03 defines the payload shape), and
// the cap keeps the format's frame records bounded long before the
// 32-bit field's limit. The TOTAL log is bounded by the size limit the
// recorder was created with (kDefaultReplaySizeLimit when 0) — the cap
// covers header + frames + trailer, and an unbounded log is therefore
// unrepresentable in the write path (PERF-008 / SCALE-003: the growth
// is bounded, and the breach is a BudgetExhausted error, never silent).
// The reader applies its own bound (loadReplay's maxBytes, default
// kDefaultReplaySizeLimit): an oversized file is a MalformedInput.
//
// Malformed-input behavior (SCALE-005, ARCH-007, CORE-008): every
// structural violation — bad magic, unsupported version, non-zero
// reserved field, truncated frame or trailer, out-of-sequence tick,
// frame length above kMaxReplayFrameBytes, a frame overlapping the
// trailer, trailer frameCount mismatch, fileHash mismatch, trailing
// garbage — is a MalformedInput Status (never a crash, never a silent
// skip). File open/read failures are IoError.
//
// ---------------------------------------------------------------------------
// The replay identity (ADR 0002)
// ---------------------------------------------------------------------------
//
// replay identity = inputs + seed + math backend + config hash. The
// log's header records the identity WITHOUT the inputs (the frames
// ARE the inputs, per tick):
//
//   seed                 the master simulation seed (config.seed)
//   tickRateHz           the simulation tick rate (config.tickRateHz)
//   componentSchemaHash  FNV-1a 64 over the component registry: the
//                        word stream [count, then per type id in
//                        ascending order: id, size, alignment]
//   mathBackendId        the SimMathBackend value (0 = fpx16_16,
//                        1 = fp32_pinned — ADR 0002: the backend id is
//                        part of replay identity; cross-backend
//                        replays are not bit-exact and not supported)
//   configHash           FNV-1a 64 over the EngineConfig's canonical
//                        field encoding (tag word 1 — the
//                        simulation-affecting fields only; M1-CFG-01's
//                        declared presentation fields are excluded, so
//                        the encoding is unchanged by the final schema)
//
// Both hashes are pure-integer FNV-1a over u64 word streams,
// big-endian byte order per word (the house hash convention —
// determinism_tests, prng_tests, laige-detcheck): no addresses, no
// unordered containers, no platform state enter the words (ARCH-010).
// A replay recorded under identity X is bit-exact only when replayed
// under X (M1-DET-03 compares the identity at load; a mismatch is a
// rejected replay, never a silent divergence).
//
// ---------------------------------------------------------------------------
// Recorder contract (write side)
// ---------------------------------------------------------------------------
//
//   ReplayRecorder::create(identity, path, maxBytes)
//     Opens the temp file `path + ".tmp"` (same filesystem as `path`),
//     writes the header, and returns the recorder. The final `path`
//     appears only on finish() (atomic temp + rename — no partial log
//     is ever visible at the final path):
//
//       path empty                       -> InvalidArgument
//       maxBytes == 0                    -> kDefaultReplaySizeLimit
//       maxBytes < kMinReplaySizeLimit   -> InvalidArgument (below the
//       (header + trailer: no complete log could ever fit)
//       temp open / header write failure -> IoError
//
//   writeFrame(tick, data, len)  one call per completed tick, in order:
//
//       tick != frameCount + 1         -> InvalidArgument (strict
//                                          1, 2, 3, ... sequence — the
//                                          engine's GameLoop onTick hook
//                                          hands it the completed tick
//                                          count, game_loop.h)
//       len > kMaxReplayFrameBytes     -> InvalidArgument
//       bytesWritten + 12 + len > maxBytes
//                                       -> BudgetExhausted (the size
//                                          limit; the recorder is now
//                                          failed — every later call
//                                          returns the same error)
//       write failure                  -> IoError (the recorder is now
//                                         failed)
//
//     M1 frames are ZERO-LENGTH (no input system exists yet — the
//     engine writes nullptr/0 per completed tick; M3-INPUT-03 defines
//     the payload shape and source). The frame record itself still
//     carries the tick and the length field, so a non-empty blob is
//     legal today and round-trips (the format is forward-ready).
//
//   finish()  flushes, writes the trailer, closes, and renames the
//     temp file onto `path` (atomic: POSIX std::rename; MSVC
//     MoveFileExA(MOVEFILE_REPLACE_EXISTING) — std::rename fails when
//     the destination exists there, the CPP-009 platform boundary):
//
//       already finished / failed      -> the sticky Status
//       trailer would exceed maxBytes  -> BudgetExhausted (temp removed)
//       flush / close failure          -> IoError (temp removed)
//       rename failure                 -> IoError (the temp file is
//                                         LEFT for inspection — the
//                                         complete data is in it; the
//                                         caller's Error log names it)
//
//   Lifetime: move-only. The destructor of an UNFINISHED recorder
//     removes the temp file (silent cleanup — the failure was already
//     reported through the Status the caller holds; the engine's
//     shutdown() adds the structured replay/record_aborted warn).
//     bytesWritten() counts header + frame bytes (the trailer is
//     counted on finish); frameCount() the written frames.
//
// ---------------------------------------------------------------------------
// Threading, failure, performance
// ---------------------------------------------------------------------------
//
// Single-owner, the engine's owner thread (CONC-001; the recorder is
// driven from the GameLoop's per-tick hook — no internal state is
// shared across threads). All operations return Status / Result; the
// recorder logs nothing itself (the engine emits the structured
// replay/* events — LOG-001/002).
//
// Performance: create() is cold (one open + one 40-byte write).
// writeFrame() costs one 12-byte stdio write plus the payload copy —
// stdio's 8 KiB buffer flushes to the OS only every ~680 zero-length
// frames, so the per-tick cost is a bounded memcpy, not a syscall.
// finish() is cold (flush + 16-byte trailer + rename). The recorder
// allocates nothing per call (the stdio buffer is the one setup
// allocation, owned by the FILE). REPLAY RECORDING IS AN OPT-IN DEBUG
// FEATURE (the laige-run --replay flag; debug builds only): the
// default run path pays one null check per completed tick and no file
// work (DBG-004: the disabled cost is negligible).
//
// Misuse warnings:
//   - Call Engine::startReplayRecording AFTER all component/system
//     registration and BEFORE run_headless: the component schema hash
//     (part of the identity) is captured at recording start — a
//     component registered afterwards is missing from it, and the
//     recorded log's identity would silently disagree with the world
//     that ran (M1-DET-03's identity check is what catches that).
//   - One recording per engine run; the engine rejects a second
//     startReplayRecording (replay/record_already_started).
//   - The final path must be on a filesystem that supports atomic
//     rename (POSIX rename / Win32 MoveFileEx — true of every P0
//     platform's local filesystems; network shares that lack it
//     surface as the documented rename-failure IoError).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "laige/errors.h"
#include "laige/result.h"
#include "laige/sim/component.h"   // ComponentInfo (componentSchemaHash)
#include "laige/sim/determinism.h" // SimMathBackend (the identity's backend id)
#include "laige/sim/entity.h"      // World (componentSchemaHash, makeReplayIdentity)

namespace laige {

struct EngineConfig;  // declared in laige/sim/engine.h; only a const
                     // reference is used (makeReplayIdentity, configHash)

// -----------------------------------------------------------------------
// Format constants (SCALE-005; CORE-005)
// -----------------------------------------------------------------------

// The log's magic (the first 4 bytes: "LGRP" — Laige GRePlay).
inline constexpr std::uint8_t kReplayMagic[4] = {'L', 'G', 'R', 'P'};

// The supported format version (ARCH-007: readers accept 1, reject
// everything else explicitly).
inline constexpr std::uint16_t kReplayFormatVersion = 1;

// The fixed header size in bytes (see the header layout above).
inline constexpr std::size_t kReplayHeaderSize = 40;

// The per-frame fixed record size in bytes (tick u64 + length u32).
inline constexpr std::size_t kReplayFrameRecordOverhead = 12;

// The fixed trailer size in bytes (frameCount u64 + fileHash u64).
inline constexpr std::size_t kReplayTrailerSize = 16;

// The smallest size limit that can ever hold a complete log
// (header + trailer, zero frames).
inline constexpr std::uint64_t kMinReplaySizeLimit = 56;

// The maximum frame blob in bytes (1 MiB): the u32 length field's
// documented domain cap for M1 opaque frames (M3-INPUT-03 defines the
// payload shape; the cap stands until a format version raises it).
inline constexpr std::uint32_t kMaxReplayFrameBytes = 1u << 20;

// The default total log size limit (128 MiB = header + frames +
// trailer): about 11.6M zero-length frames, about 5.2 hours of
// 60 Hz simulation. 0 passed to create() / loadReplay means this.
inline constexpr std::uint64_t kDefaultReplaySizeLimit = 128ull << 20;

// -----------------------------------------------------------------------
// The replay identity (ADR 0002)
// -----------------------------------------------------------------------

// The replay identity: the header's five identity fields. A plain
// value (PERF-005); compared field-by-field at replay time
// (M1-DET-03) — a log is bit-exact only under its own identity.
struct ReplayIdentity {
  // The master simulation seed (config.seed).
  std::uint64_t seed{};
  // The simulation tick rate in hertz (config.tickRateHz).
  std::uint32_t tickRateHz{};
  // FNV-1a 64 over the component registry (see the header preamble
  // "The replay identity").
  std::uint64_t componentSchemaHash{};
  // The laige::SimMathBackend value (0 = FixedPoint16_16, 1 =
  // FloatPinned32 — ADR 0002's backend ids).
  std::uint32_t mathBackendId{};
  // FNV-1a 64 over the EngineConfig's simulation-affecting field
  // encoding (M1-CFG-01's presentation fields are excluded).
  std::uint64_t configHash{};
};

// One recorded input frame: the completed tick number (the strict
// 1, 2, 3, ... sequence) plus the opaque byte blob (M1: zero bytes —
// no input system exists yet; M3-INPUT-03 defines the payload shape).
struct ReplayFrame {
  // The completed tick this frame belongs to (1-based, in order).
  std::uint64_t tick{};
  // The frame's opaque input bytes (empty in M1).
  std::vector<std::uint8_t> data;
};

// A parsed replay log (the loadReplay / parseReplay result): the
// identity plus every frame in tick order. Cold-path value: the frame
// storage is owned (one allocation per frame blob; M1 blobs are
// empty, so M1 logs cost one vector each).
struct ReplayLog {
  // The log's replay identity (the header).
  ReplayIdentity identity{};
  // Every recorded frame, in tick order (empty when the log has no
  // frames — legal: a zero-tick run).
  std::vector<ReplayFrame> frames;
};

// -----------------------------------------------------------------------
// The recorder (write side)
// -----------------------------------------------------------------------

// The replay log writer: create() -> writeFrame() per completed tick
// -> finish() (see the header preamble "Recorder contract" for the
// full error table). Move-only; the engine owns one per run (opt-in,
// debug builds only). The recorder logs nothing — the engine emits
// the structured replay/* events (LOG-001/002).
class ReplayRecorder {
 public:
  // Create the recorder for `path` (the FINAL path — the temp file
  // `path + ".tmp"` is the only thing created now): validates the
  // bounds, opens the temp file, and writes the header carrying
  // `identity`. Error table in the header preamble; @budget one
  // file open + one 40-byte write (cold path); allocates the stdio
  // buffer (one setup allocation, owned by the FILE).
  [[nodiscard]] static Result<ReplayRecorder, ErrorCode>
  create(const ReplayIdentity& identity, std::string_view path,
         std::uint64_t maxBytes) noexcept;

  ReplayRecorder(const ReplayRecorder&) = delete;
  ReplayRecorder& operator=(const ReplayRecorder&) = delete;
  // Move transfers the open file; the source becomes finished (a
  // finished recorder does nothing — the moved-out GameLoop
  // precedent).
  ReplayRecorder(ReplayRecorder&& other) noexcept;
  ReplayRecorder& operator=(ReplayRecorder&& other) noexcept;
  // An unfinished recorder removes its temp file (the final path is
  // never touched; the failure was already reported through a Status).
  ~ReplayRecorder() noexcept;

  // Record one input frame for completed tick `tick` carrying `len`
  // bytes at `data` (len == 0: data may be nullptr). Error table in
  // the header preamble; @budget one 12-byte (+ len) stdio write, no
  // allocation (the stdio buffer holds the data until it flushes).
  [[nodiscard]] Status writeFrame(std::uint64_t tick,
                                  const std::uint8_t* data,
                                  std::size_t len) noexcept;

  // Finalize: flush, write the trailer, close, and atomically rename
  // the temp file onto the final path. Error table in the header
  // preamble; @budget one flush + one 16-byte write + one rename
  // (cold path).
  [[nodiscard]] Status finish() noexcept;

  // The sticky failure Status (ok while nothing has failed; the last
  // error otherwise — the caller reports it, the recorder does not
  // log). O(1), no side effects.
  [[nodiscard]] Status status() const noexcept;

  // True once finish() succeeded. O(1), no side effects.
  [[nodiscard]] bool finished() const noexcept;

  // Bytes written so far (header + frame bytes; the trailer is
  // counted when finish() writes it). O(1), no side effects.
  [[nodiscard]] std::uint64_t bytesWritten() const noexcept;

  // Frames recorded so far. O(1), no side effects.
  [[nodiscard]] std::uint64_t frameCount() const noexcept;

  // The FINAL path (the rename destination; the temp file is
  // `path() + ".tmp"`). Valid for the recorder's lifetime.
  [[nodiscard]] const char* path() const noexcept;

 private:
  // The factory path (create): constructed only with an open temp
  // file and a written header (API-008: no empty state).
  ReplayRecorder(std::string path, std::string tmpPath,
                 std::uint64_t maxBytes, std::FILE* file,
                 const ReplayIdentity& identity);

  // The final path (the rename destination).
  std::string path_{};
  // The temp file path (path_ + ".tmp"; same filesystem).
  std::string tmpPath_{};
  // The total size cap (header + frames + trailer).
  std::uint64_t maxBytes_{};
  // The identity written into the header (echoed by parseReplay).
  ReplayIdentity identity_{};
  // The open temp file (nullptr after finish / on a moved-from
  // recorder). Owned by the recorder (closed in finish or the
  // destructor).
  std::FILE* file_{nullptr};
  // Bytes written so far (header + frames; the trailer on finish).
  std::uint64_t bytesWritten_{};
  // Frames recorded so far.
  std::uint64_t frameCount_{};
  // Running FNV-1a 64 over every byte written so far (the trailer's
  // fileHash covers exactly these).
  std::uint64_t fileHash_{};
  // The sticky failure (ok until the first error).
  Status error_{};
  // True once finish() succeeded (or on a moved-from recorder).
  bool finished_{false};
};

// -----------------------------------------------------------------------
// The reader (parse side)
// -----------------------------------------------------------------------

// Parse a replay log from memory (the byte-level reader; the
// loadReplay file wrapper calls it). Accepts formatVersion == 1 and
// rejects every structural violation with MalformedInput (never a
// crash — SCALE-005 / ARCH-007; the violation table in the header
// preamble). Cold path (the parser's only allocations are the parsed
// log's frame storage).
//
//   data == nullptr && size != 0 -> MalformedInput
// @budget O(size) time, O(total frame bytes) allocation.
[[nodiscard]] Result<ReplayLog, ErrorCode>
parseReplay(const std::uint8_t* data, std::size_t size) noexcept;

// Load and parse a replay log from `path`: reads the WHOLE file (a
// bounded read — an oversized file, size > maxBytes, is a
// MalformedInput, the ADR 0003 JSON-bound precedent) and passes it to
// parseReplay. maxBytes == 0 means kDefaultReplaySizeLimit.
//
//   path empty              -> MalformedInput
//   open / read failure     -> IoError
//   size > maxBytes         -> MalformedInput
// @budget O(file size) time, O(file size) allocation (the bounded read).
[[nodiscard]] Result<ReplayLog, ErrorCode>
loadReplay(std::string_view path,
           std::uint64_t maxBytes = kDefaultReplaySizeLimit) noexcept;

// -----------------------------------------------------------------------
// The identity computation
// -----------------------------------------------------------------------

// FNV-1a 64 over the world's component registry: the word stream
// [componentCount, then per type id in ascending order: id, size,
// alignment] (the house word-stream hash convention — big-endian byte
// order per u64 word; no addresses enter the words, ARCH-010). O(n)
// in the registered types (setup path — the registry is fixed before
// the run), no allocation.
// @budget O(componentCount); no allocation.
[[nodiscard]] std::uint64_t componentSchemaHash(const World& world) noexcept;

// FNV-1a 64 over the EngineConfig's canonical field encoding (tag
// word 1 — the simulation-affecting fields: tickRateHz,
// entityCapacity, churnPerFrameBudget, seed, determinism.enabled,
// determinism.math). M1-CFG-01's declared presentation fields
// (budgets, camera, asset_roots) are deliberately excluded — they
// never touch simulation — so the encoding is unchanged by the final
// schema and every committed baseline stays valid. O(1), no
// allocation.
// @budget O(1); no allocation.
[[nodiscard]] std::uint64_t configHash(const EngineConfig& config) noexcept;

// Assemble the full replay identity (ADR 0002) from the world's
// component registry and the engine config. O(n) in the registered
// types, no allocation.
// @budget O(componentCount); no allocation.
[[nodiscard]] ReplayIdentity makeReplayIdentity(const World& world,
                                                const EngineConfig& config) noexcept;

// -----------------------------------------------------------------------
// Replay execution (M1-DET-03)
// -----------------------------------------------------------------------
//
// The execution half: a recorded log is replayed headlessly on a
// fully-registered world (the SAME registrations as the recording
// run — the caller's responsibility: the component registry and the
// system order are what the identity and the sim behavior depend on)
// and the per-tick state hashes (World::stateHash) are produced.
//
// The HASH LINE CONTRACT (the laige-replay stdout form; the same
// contract laige-detcheck enforces on scenario binaries —
// docs/api/detcheck.md): one line per tick, in order,
//
//   <tick> <hash>
//
// where <tick> is a non-negative decimal (no leading zeros) and
// <hash> is the 16 lowercase hex digits of a 64-bit state hash. The
// FIRST line is tick 0 (the INITIAL state, before any tick); line i+1
// is the state after completed tick i. A log with N frames therefore
// produces N+1 lines (ticks 0..N).

// The field-by-field result of replayIdentityDiff: one bit per
// identity field (ADR 0002); empty() == every field matches.
struct ReplayIdentityDiff {
  bool seed{};
  bool tickRateHz{};
  bool componentSchemaHash{};
  bool mathBackendId{};
  bool configHash{};

  // True when no field differs (the identities match).
  [[nodiscard]] bool empty() const noexcept {
    return !seed && !tickRateHz && !componentSchemaHash &&
           !mathBackendId && !configHash;
  }
};

// Compare a loaded log's replay identity against the identity the
// caller's (world, config) would produce (makeReplayIdentity). A
// non-empty result is a REJECTED REPLAY (never a silent divergence —
// the header preamble "The replay identity"): the log was recorded
// under a different identity and must not be replayed on this world.
// O(n) in the registered types, no allocation, no side effects.
// @budget O(componentCount); no allocation.
[[nodiscard]] ReplayIdentityDiff
replayIdentityDiff(const ReplayLog& log, const World& world,
                   const EngineConfig& config) noexcept;

// The result of runReplay: the per-tick state hashes (the hash line
// contract above): tickHashes[i] == the world's stateHash(i) after the
// replay — index 0 is the initial state, indices 1..frameCount one
// entry per completed tick.
struct ReplayRunResult {
  // frameCount + 1 entries (tick 0 .. frameCount).
  std::vector<std::uint64_t> tickHashes;
};

// Replay `log` headlessly on `world`:
//
//   1. Check the log's identity against (world, config) (step 0 below);
//   2. Check the determinism mode: a log recorded with determinism
//      DISABLED is not replayable (determinism.h: the seed and the
//      substreams are part of the replay identity only in deterministic
//      mode) — rejected;
//   3. Drive exactly log.frames.size() ticks, each one
//      world.beginFrame() + world.runSystems(schedule) (the schedule is
//      computed once, before the first tick; one frame per tick — the
//      original run's frame grouping is a wall-clock fact, not part of
//      the deterministic contract, engine.h; the recorded frame bytes
//      are fed as opaque blobs: M1 has no input system to consume
//      them, M3-INPUT-03 defines consumption — non-empty frames are
//      accepted and ignored);
//   4. Return the per-tick state hashes (World::stateHash — tick 0
//      before the loop, then one hash per completed tick).
//
//   replay identity mismatch (any field — see replayIdentityDiff)
//                                   -> ErrorCode::InvalidArgument + one
//                                      structured warn
//                                      (replay/identity_mismatch naming
//                                      the differing fields, LOG-002)
//   determinism disabled (config.determinism.enabled == false — the
//                                   identity already matched, so the
//                                   log's header says the same)
//                                   -> ErrorCode::InvalidArgument + one
//                                      structured warn
//                                      (replay/determinism_disabled)
//   scheduleSystems fails            -> the world's Status (the world
//                                      already logged it; no partial
//                                      result)
//   a tick's runSystems fails        -> the world's Status (the world
//                                      already logged it; the replay
//                                      stops — no partial hashes)
//
// Cold path (a caller's run, never the engine's per-tick hot path):
// O(frameCount × per-tick system work + per-tick state hash);
// allocates only the result vector (the hash itself allocates
// nothing — entity.h stateHash contract). The caller's world is the
// same kind the recording run used: built-in + game registrations,
// same order (ARCH-010).
// @budget O(frameCount × tick work + state hash); one allocation
// (the result vector).
[[nodiscard]] Result<ReplayRunResult, ErrorCode>
runReplay(const ReplayLog& log, World& world,
          const EngineConfig& config) noexcept;

}  // namespace laige
