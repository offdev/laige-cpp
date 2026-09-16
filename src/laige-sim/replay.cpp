// laige-sim replay recording (M1-DET-02).
//
// Implementation of the types declared in
// include/laige/sim/replay.h — see that header for the format spec
// (SCALE-005), the recorder contract, the identity hashes, and the
// performance notes, and docs/api/replay.md for the API document.
//
// House hash conventions (the docs/testing.md / determinism_tests /
// laige-detcheck convention): FNV-1a 64 — offset basis
// 0xcbf29ce484222325, prime 0x100000001b3 (fnv.org). Word-stream
// hashes (componentSchemaHash, configHash) run big-endian byte order
// per u64 word; the log's trailer fileHash is the canonical
// byte-stream FNV-1a over the file's raw bytes.
//
// Platform boundary (CPP-009, the logging.cpp / laige-run.cpp
// precedent): MSVC's CRT deprecates plain fopen (C4996, fatal under
// the engine's /WX policy) and opens it with _SH_SECURE when used via
// fopen_s (denying re-open); _fsopen(..., _SH_DENYNO) is the plain-
// fopen sharing semantics every other supported compiler provides.
// std::rename fails on MSVC when the destination exists, so the
// atomic finalization uses MoveFileExA(MOVEFILE_REPLACE_EXISTING).

#include "laige/sim/replay.h"  // the contract (this header)

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <share.h>   // _SH_DENYNO: plain-fopen sharing for _fsopen
#include <windows.h> // MoveFileExA / MOVEFILE_REPLACE_EXISTING
#endif

#include "laige/logging.h"  // the structured replay/* events (runReplay)
#include "laige/sim/engine.h"  // EngineConfig (configHash, makeReplayIdentity)

namespace laige {

namespace {

// FNV-1a 64 constants (fnv.org — the house convention).
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// Canonical byte-stream FNV-1a 64 over raw bytes (the trailer's
// fileHash).
std::uint64_t fnv1a64Bytes(const std::uint8_t* bytes, std::size_t count) {
  std::uint64_t h = kFnvOffsetBasis;
  for (std::size_t i = 0; i < count; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= kFnvPrime;
  }
  return h;
}

// Continue a running byte-stream FNV-1a 64 (from an existing hash)
// over further bytes — FNV-1a is a streaming hash: the state carries
// over, so the recorder's running hash folds the payload in order
// after the record bytes.
std::uint64_t fnv1a64BytesExtend(std::uint64_t h, const std::uint8_t* bytes,
                                 std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= kFnvPrime;
  }
  return h;
}

// Word-stream FNV-1a 64 over u64 words, big-endian byte order per word
// (the house convention: determinism_tests, prng_tests, detcheck).
std::uint64_t fnv1a64Words(const std::uint64_t* words, std::size_t count) {
  std::uint64_t h = kFnvOffsetBasis;
  for (std::size_t i = 0; i < count; ++i) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (words[i] >> shift) & 0xFFull;
      h *= kFnvPrime;
    }
  }
  return h;
}

// Little-endian encoders (SCALE-005: the format is little-endian on
// every platform, so the encoding is explicit).
void encodeU16le(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}
void encodeU32le(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}
void encodeU64le(std::uint8_t* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}
// Little-endian decoders (the bytes are validated in range by the
// caller's structure checks before use).
std::uint16_t decodeU16le(const std::uint8_t* in) {
  return static_cast<std::uint16_t>(in[0]) |
         static_cast<std::uint16_t>(in[1]) << 8;
}
std::uint32_t decodeU32le(const std::uint8_t* in) {
  return static_cast<std::uint32_t>(in[0]) |
         static_cast<std::uint32_t>(in[1]) << 8 |
         static_cast<std::uint32_t>(in[2]) << 16 |
         static_cast<std::uint32_t>(in[3]) << 24;
}
std::uint64_t decodeU64le(const std::uint8_t* in) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | in[i];
  }
  return v;
}

// Encode the identity header into kReplayHeaderSize bytes (freshly
// initialized).
void encodeHeader(const ReplayIdentity& identity, std::uint8_t* header) {
  static_assert(sizeof(kReplayMagic) == 4, "the magic is 4 bytes");
  std::memcpy(header, kReplayMagic, sizeof(kReplayMagic));
  encodeU16le(header + 4, kReplayFormatVersion);
  encodeU16le(header + 6, 0);  // reserved
  encodeU64le(header + 8, identity.seed);
  encodeU32le(header + 16, identity.tickRateHz);
  encodeU64le(header + 20, identity.componentSchemaHash);
  encodeU32le(header + 28, identity.mathBackendId);
  encodeU64le(header + 32, identity.configHash);
}

// The portable binary open (the laige-run.cpp openConfigFile
// precedent; see the translation-unit preamble).
std::FILE* openBinaryFile(const std::string& path, const char* mode) {
#if defined(_MSC_VER)
  return ::_fsopen(path.c_str(), mode, _SH_DENYNO);
#else
  return std::fopen(path.c_str(), mode);
#endif
}

// The atomic finalization rename (see the translation-unit preamble):
// POSIX std::rename atomically replaces an existing destination; MSVC
// needs MoveFileExA with MOVEFILE_REPLACE_EXISTING.
bool atomicReplace(const std::string& from, const std::string& to) {
#if defined(_MSC_VER)
  return ::MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// The tag word of the provisional EngineConfig field encoding
// (M1-HEAD-01 surface; M1-CFG-01 refines it — the tag identifies this
// encoding in the hash word stream).
inline constexpr std::uint64_t kConfigHashTag = 1;

}  // namespace

// -----------------------------------------------------------------------
// ReplayRecorder
// -----------------------------------------------------------------------

ReplayRecorder::ReplayRecorder(std::string path, std::string tmpPath,
                               std::uint64_t maxBytes, std::FILE* file,
                               const ReplayIdentity& identity)
    : path_(std::move(path)),
      tmpPath_(std::move(tmpPath)),
      maxBytes_(maxBytes),
      identity_(identity),
      file_(file) {}

Result<ReplayRecorder, ErrorCode>
ReplayRecorder::create(const ReplayIdentity& identity, std::string_view path,
                       std::uint64_t maxBytes) noexcept {
  if (path.empty()) {
    return Result<ReplayRecorder, ErrorCode>(ErrorCode::InvalidArgument);
  }
  // 0 means the default cap (the loadReplay/create documented default).
  const std::uint64_t cap =
      maxBytes == 0 ? kDefaultReplaySizeLimit : maxBytes;
  if (cap < kMinReplaySizeLimit) {
    // Below header + trailer: no complete log could ever fit — the
    // limit is outside the documented domain (API-008).
    return Result<ReplayRecorder, ErrorCode>(ErrorCode::InvalidArgument);
  }
  // The temp file lives next to the final path (same filesystem —
  // the rename is atomic).
  const std::string tmpPath = std::string(path) + ".tmp";
  std::FILE* file = openBinaryFile(tmpPath, "wb");
  if (file == nullptr) {
    return Result<ReplayRecorder, ErrorCode>(ErrorCode::IoError);
  }
  // The header (kReplayHeaderSize bytes, freshly initialized — the
  // encode writes every byte of it).
  std::uint8_t header[kReplayHeaderSize] = {};
  encodeHeader(identity, header);
  if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
    std::fclose(file);
    (void)std::remove(tmpPath.c_str());
    return Result<ReplayRecorder, ErrorCode>(ErrorCode::IoError);
  }
  ReplayRecorder recorder(std::string(path), tmpPath, cap, file, identity);
  recorder.bytesWritten_ = kReplayHeaderSize;
  recorder.fileHash_ = fnv1a64Bytes(header, sizeof(header));
  return Result<ReplayRecorder, ErrorCode>::success(std::move(recorder));
}

ReplayRecorder::ReplayRecorder(ReplayRecorder&& other) noexcept
    : path_(std::move(other.path_)),
      tmpPath_(std::move(other.tmpPath_)),
      maxBytes_(other.maxBytes_),
      identity_(other.identity_),
      file_(other.file_),
      bytesWritten_(other.bytesWritten_),
      frameCount_(other.frameCount_),
      fileHash_(other.fileHash_),
      error_(other.error_),
      finished_(other.finished_) {
  // The source loses the open file and becomes finished: its
  // destructor cleans nothing (the moved-out GameLoop precedent).
  other.file_ = nullptr;
  other.finished_ = true;
}

ReplayRecorder& ReplayRecorder::operator=(ReplayRecorder&& other) noexcept {
  if (this != &other) {
    // Release this recorder's current state first (a no-op when it
    // is already finished; otherwise the temp file is removed — the
    // moved-into recorder takes over the file).
    if (!finished_ && file_ != nullptr) {
      std::fclose(file_);
    }
    if (!finished_) {
      (void)std::remove(tmpPath_.c_str());
    }
    path_ = std::move(other.path_);
    tmpPath_ = std::move(other.tmpPath_);
    maxBytes_ = other.maxBytes_;
    identity_ = other.identity_;
    file_ = other.file_;
    bytesWritten_ = other.bytesWritten_;
    frameCount_ = other.frameCount_;
    fileHash_ = other.fileHash_;
    error_ = other.error_;
    finished_ = other.finished_;
    other.file_ = nullptr;
    other.finished_ = true;
  }
  return *this;
}

ReplayRecorder::~ReplayRecorder() noexcept {
  // Unfinished cleanup (silent — the failure was already reported
  // through a Status the caller holds; the engine's shutdown() adds
  // the structured replay/record_aborted warn): close the file and
  // remove the temp file. The final path is never touched here.
  if (!finished_) {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
    (void)std::remove(tmpPath_.c_str());
  }
  file_ = nullptr;
}

Status ReplayRecorder::writeFrame(std::uint64_t tick,
                                  const std::uint8_t* data,
                                  std::size_t len) noexcept {
  if (error_.isError()) {
    return error_;  // sticky: a failed recorder stays failed
  }
  if (finished_) {
    error_ = Status(ErrorCode::InvalidArgument);
    return error_;  // a finished recorder accepts no frames
  }
  if (tick != frameCount_ + 1) {
    // The strict 1, 2, 3, ... sequence (the engine's onTick hook
    // hands it the completed tick count — game_loop.h).
    error_ = Status(ErrorCode::InvalidArgument);
    return error_;
  }
  if (len > kMaxReplayFrameBytes) {
    error_ = Status(ErrorCode::InvalidArgument);
    return error_;
  }
  if (bytesWritten_ + kReplayFrameRecordOverhead + len > maxBytes_) {
    // The total size limit (header + frames + trailer) — the log's
    // growth is bounded (PERF-008 / SCALE-003) and the breach is an
    // explicit budget error (CORE-008).
    error_ = Status(ErrorCode::BudgetExhausted);
    return error_;
  }
  if (file_ == nullptr) {
    // Unreachable (the recorder owns the file until finish); keep
    // the function total rather than crash (CORE-008).
    assert(file_ != nullptr && "writeFrame on a recorder without a file");
    error_ = Status(ErrorCode::IoError);
    return error_;
  }
  // The 12-byte record (tick u64 + byteLength u32, little-endian)
  // followed by the payload.
  std::uint8_t record[kReplayFrameRecordOverhead] = {};
  encodeU64le(record, tick);
  encodeU32le(record + 8, static_cast<std::uint32_t>(len));
  std::size_t written = std::fwrite(record, 1, sizeof(record), file_);
  if (written != sizeof(record) ||
      (len != 0 && std::fwrite(data, 1, len, file_) != len)) {
    std::fclose(file_);
    file_ = nullptr;
    (void)std::remove(tmpPath_.c_str());
    error_ = Status(ErrorCode::IoError);
    return error_;
  }
  const std::uint64_t frameBytes =
      static_cast<std::uint64_t>(kReplayFrameRecordOverhead) + len;
  bytesWritten_ += frameBytes;
  ++frameCount_;
  // The running hash extends over the record bytes, then the payload
  // (FNV-1a is streaming — the state carries over).
  fileHash_ = fnv1a64BytesExtend(fileHash_, record, sizeof(record));
  if (len != 0) {
    fileHash_ = fnv1a64BytesExtend(fileHash_, data, len);
  }
  return Status{};
}

Status ReplayRecorder::finish() noexcept {
  if (error_.isError()) {
    return error_;  // sticky: a failed recorder cannot finalize
  }
  if (finished_) {
    error_ = Status(ErrorCode::InvalidArgument);
    return error_;  // finish is one-shot
  }
  if (file_ == nullptr) {
    // Unreachable (the recorder owns the file until finish); keep
    // the function total rather than crash (CORE-008).
    assert(file_ != nullptr && "finish on a recorder without a file");
    error_ = Status(ErrorCode::IoError);
    return error_;
  }
  // The trailer must fit inside the total cap (header + frames +
  // trailer all count).
  if (bytesWritten_ + kReplayTrailerSize > maxBytes_) {
    std::fclose(file_);
    file_ = nullptr;
    (void)std::remove(tmpPath_.c_str());
    error_ = Status(ErrorCode::BudgetExhausted);
    return error_;
  }
  // The trailer: frameCount u64 + fileHash u64 (the FNV-1a 64 over
  // every byte before the trailer — exactly the running hash).
  std::uint8_t trailer[kReplayTrailerSize] = {};
  encodeU64le(trailer, frameCount_);
  encodeU64le(trailer + 8, fileHash_);
  if (std::fwrite(trailer, 1, sizeof(trailer), file_) != sizeof(trailer) ||
      std::fflush(file_) != 0) {
    std::fclose(file_);
    file_ = nullptr;
    (void)std::remove(tmpPath_.c_str());
    error_ = Status(ErrorCode::IoError);
    return error_;
  }
  bytesWritten_ += kReplayTrailerSize;
  if (std::fclose(file_) != 0) {
    file_ = nullptr;
    // The data was flushed; the temp file is LEFT (the complete log
    // is in it — the caller's Error log names it for inspection).
    error_ = Status(ErrorCode::IoError);
    return error_;
  }
  if (!atomicReplace(tmpPath_, path_)) {
    // Rename failure: the temp file is LEFT for inspection (the
    // complete data is in it). Documented in the header preamble.
    error_ = Status(ErrorCode::IoError);
    return error_;
  }
  finished_ = true;
  return Status{};
}

Status ReplayRecorder::status() const noexcept { return error_; }

bool ReplayRecorder::finished() const noexcept { return finished_; }

std::uint64_t ReplayRecorder::bytesWritten() const noexcept {
  return bytesWritten_;
}

std::uint64_t ReplayRecorder::frameCount() const noexcept {
  return frameCount_;
}

const char* ReplayRecorder::path() const noexcept { return path_.c_str(); }

// -----------------------------------------------------------------------
// The reader (parse side)
// -----------------------------------------------------------------------

Result<ReplayLog, ErrorCode>
parseReplay(const std::uint8_t* data, std::size_t size) noexcept {
  // The structural violation table (header preamble): every branch
  // below is a MalformedInput — never a crash, never a silent skip
  // (SCALE-005, ARCH-007, CORE-008).
  if (size != 0 && data == nullptr) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  if (size < kReplayHeaderSize) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  if (std::memcmp(data, kReplayMagic, sizeof(kReplayMagic)) != 0) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  const std::uint16_t version = decodeU16le(data + 4);
  if (version != kReplayFormatVersion) {
    // ARCH-007: unsupported versions are rejected explicitly.
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  if (decodeU16le(data + 6) != 0) {  // reserved
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  ReplayLog log;
  log.identity.seed = decodeU64le(data + 8);
  log.identity.tickRateHz = decodeU32le(data + 16);
  log.identity.componentSchemaHash = decodeU64le(data + 20);
  log.identity.mathBackendId = decodeU32le(data + 28);
  log.identity.configHash = decodeU64le(data + 32);

  // The frame body: every frame must end at or before the trailer's
  // start (size - kReplayTrailerSize); a frame header there with
  // fewer than 12 body bytes left is a truncation.
  const std::size_t bodyEnd = size >= kReplayTrailerSize
                                  ? size - kReplayTrailerSize
                                  : 0;
  if (kReplayHeaderSize > bodyEnd) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  std::size_t pos = kReplayHeaderSize;
  std::uint64_t count = 0;
  while (pos < bodyEnd) {
    const std::size_t remaining = bodyEnd - pos;
    if (remaining < kReplayFrameRecordOverhead) {
      return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
    }
    const std::uint64_t tick = decodeU64le(data + pos);
    const std::uint32_t len = decodeU32le(data + pos + 8);
    if (tick != count + 1) {
      return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
    }
    if (len > kMaxReplayFrameBytes) {
      return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
    }
    if (remaining < kReplayFrameRecordOverhead + len) {
      return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
    }
    ReplayFrame frame;
    frame.tick = tick;
    frame.data.assign(data + pos + kReplayFrameRecordOverhead,
                      data + pos + kReplayFrameRecordOverhead + len);
    log.frames.push_back(std::move(frame));
    pos += kReplayFrameRecordOverhead + len;
    ++count;
  }
  // The trailer: exactly the last 16 bytes (pos == bodyEnd).
  if (pos != bodyEnd) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  const std::uint64_t trailerCount = decodeU64le(data + pos);
  const std::uint64_t trailerHash = decodeU64le(data + pos + 8);
  if (trailerCount != count) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  // The fileHash covers every byte before the trailer (the canonical
  // byte-stream FNV-1a — the recorder's running hash).
  if (fnv1a64Bytes(data, bodyEnd) != trailerHash) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  return Result<ReplayLog, ErrorCode>::success(std::move(log));
}

Result<ReplayLog, ErrorCode>
loadReplay(std::string_view path, std::uint64_t maxBytes) noexcept {
  if (path.empty()) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
  }
  const std::uint64_t cap =
      maxBytes == 0 ? kDefaultReplaySizeLimit : maxBytes;
  std::FILE* file = openBinaryFile(std::string(path), "rb");
  if (file == nullptr) {
    return Result<ReplayLog, ErrorCode>(ErrorCode::IoError);
  }
  // The bounded read (the ADR 0003 JSON-bound precedent: an oversized
  // file is a MalformedInput, not a truncated parse).
  std::vector<std::uint8_t> bytes;
  std::uint8_t chunk[8192];
  std::uint64_t total = 0;
  for (;;) {
    const std::size_t n = std::fread(chunk, 1, sizeof(chunk), file);
    if (n == 0) {
      if (std::ferror(file)) {
        std::fclose(file);
        return Result<ReplayLog, ErrorCode>(ErrorCode::IoError);
      }
      break;  // clean EOF
    }
    total += n;
    if (total > cap) {
      std::fclose(file);
      return Result<ReplayLog, ErrorCode>(ErrorCode::MalformedInput);
    }
    bytes.insert(bytes.end(), chunk, chunk + n);
  }
  std::fclose(file);
  return parseReplay(bytes.data(), bytes.size());
}

// -----------------------------------------------------------------------
// The identity computation
// -----------------------------------------------------------------------

std::uint64_t componentSchemaHash(const World& world) noexcept {
  // [componentCount, then per type id in ascending order: id, size,
  // alignment] — at most 1 + 3 * kMaxComponentTypes words. The stack
  // array keeps the hot setup path allocation-free (PERF-003); 769
  // words is 6 KiB, a setup-path one-shot.
  std::uint64_t words[1 + 3 * kMaxComponentTypes];
  const std::uint32_t count = world.componentCount();
  words[0] = count;
  std::size_t i = 1;
  for (std::uint32_t id = 1; id <= count; ++id) {
    // Dense ids 1..count are registered by the World's bookkeeping
    // invariant (component.h: ids are assigned densely from 1 in
    // registration order); the assert documents it, and result.h's
    // house contract makes the unchecked value() legal here (debug
    // asserts, release invariant).
    const Result<ComponentInfo, ErrorCode> info =
        world.componentInfo(ComponentTypeId{id});
    assert(info.ok() && "componentInfo failed for a dense registry id");
    const ComponentInfo& ci = info.value();
    words[i++] = id;
    words[i++] = ci.size;
    words[i++] = ci.alignment;
  }
  return fnv1a64Words(words, i);
}

std::uint64_t configHash(const EngineConfig& config) noexcept {
  // The provisional EngineConfig field encoding (tag word 1 — the
  // M1-HEAD-01 surface; M1-CFG-01 refines the schema and this
  // encoding with it, under the format's versioning).
  std::uint64_t words[7];
  words[0] = kConfigHashTag;
  words[1] = config.tickRateHz;
  words[2] = config.entityCapacity;
  words[3] = config.churnPerFrameBudget;
  words[4] = config.seed;
  words[5] = config.determinism.enabled ? 1ull : 0ull;
  words[6] = static_cast<std::uint64_t>(config.determinism.math);
  return fnv1a64Words(words, 7);
}

ReplayIdentity makeReplayIdentity(const World& world,
                                  const EngineConfig& config) noexcept {
  return ReplayIdentity{
      config.seed,
      config.tickRateHz,
      componentSchemaHash(world),
      static_cast<std::uint32_t>(config.determinism.math),
      configHash(config)};
}

// -----------------------------------------------------------------------
// Replay execution (M1-DET-03)
// -----------------------------------------------------------------------

ReplayIdentityDiff
replayIdentityDiff(const ReplayLog& log, const World& world,
                   const EngineConfig& config) noexcept {
  const ReplayIdentity expected = makeReplayIdentity(world, config);
  return ReplayIdentityDiff{
      log.identity.seed != expected.seed,
      log.identity.tickRateHz != expected.tickRateHz,
      log.identity.componentSchemaHash != expected.componentSchemaHash,
      log.identity.mathBackendId != expected.mathBackendId,
      log.identity.configHash != expected.configHash};
}

Result<ReplayRunResult, ErrorCode>
runReplay(const ReplayLog& log, World& world,
          const EngineConfig& config) noexcept {
  // Step 1: the identity (replay.h preamble "The replay identity": a
  // replay recorded under identity X is bit-exact only when replayed
  // under X — a mismatch is a rejected replay, never a silent
  // divergence).
  const ReplayIdentityDiff diff = replayIdentityDiff(log, world, config);
  if (!diff.empty()) {
    // The differing fields are structured values, never message text
    // (LOG-002; the system_timing.cpp precedent).
    LAIGE_LOG_WARN("replay", "identity_mismatch",
                   "Replay identity mismatch: the log was recorded under "
                   "a different identity than (world, config); the replay "
                   "is rejected (ADR 0002)",
                   laige::log::field("seed", diff.seed),
                   laige::log::field("tick_rate", diff.tickRateHz),
                   laige::log::field("component_schema",
                                     diff.componentSchemaHash),
                   laige::log::field("math_backend", diff.mathBackendId),
                   laige::log::field("config", diff.configHash));
    return Result<ReplayRunResult, ErrorCode>(ErrorCode::InvalidArgument);
  }
  // Step 2: the determinism mode (determinism.h "Determinism mode
  // semantics": a run recorded with determinism disabled is not
  // replayable — the seed and the substreams are part of the replay
  // identity only in deterministic mode).
  if (!config.determinism.enabled) {
    LAIGE_LOG_WARN("replay", "determinism_disabled",
                   "The replay was recorded with determinism disabled; "
                   "such runs are not replayable (determinism.h)",
                   laige::log::field("seed", log.identity.seed));
    return Result<ReplayRunResult, ErrorCode>(ErrorCode::InvalidArgument);
  }
  // Step 3: the deterministic tick driver. One frame per tick: the
  // original run's frame grouping is a wall-clock fact, not part of the
  // deterministic contract (engine.h "Determinism scope"); the
  // guardrail warn events a replay emits may therefore differ from
  // the recorded run's (diagnostics, excluded from the state hash —
  // entity.h scope). The recorded frame bytes are opaque in M1: no
  // input system consumes them yet (M3-INPUT-03 defines consumption —
  // non-empty frames are accepted and ignored).
  SystemSchedule schedule;
  const Status schedStatus = world.scheduleSystems(schedule);
  if (!schedStatus.ok()) {
    // The world already logged the failure (system/schedule_*); no
    // partial result (CORE-008).
    return Result<ReplayRunResult, ErrorCode>(schedStatus.error());
  }
  ReplayRunResult result;
  result.tickHashes.reserve(log.frames.size() + 1);
  // Tick 0: the initial state (the hash line contract's first line).
  result.tickHashes.push_back(world.stateHash(0));
  for (std::size_t i = 0; i < log.frames.size(); ++i) {
    world.beginFrame();
    static_cast<void>(log.frames[i].data);  // opaque in M1 (above)
    const Status tickStatus = world.runSystems(schedule);
    if (!tickStatus.ok()) {
      // The world already logged the failed system; the replay stops —
      // no partial hashes (CORE-008).
      return Result<ReplayRunResult, ErrorCode>(tickStatus.error());
    }
    result.tickHashes.push_back(world.stateHash(i + 1));
  }
  return Result<ReplayRunResult, ErrorCode>::success(std::move(result));
}

}  // namespace laige
