// laige-sim replay recording suite (M1-DET-02).
//
// Step Verify scope (roadmap/M1-heartbeat.md, `ctest -R replay_record`):
//   - round trip: record N ticks -> parse back -> identical bytes and
//     fields (the step's "record N ticks -> parse back -> identical
//     bytes" clause)
//   - malformed log files (truncation, bad version, and the full
//     SCALE-005 violation table) -> Status error, NEVER a crash
//   - the recorder: atomic temp+rename (no partial log at the final
//     path), the size limit (BudgetExhausted, no unbounded growth),
//     the strict tick sequence
//   - the replay identity (ADR 0002): pure-integer FNV-1a hashes,
//     stable for (world, config)
//   - the engine: one zero-length frame per completed tick (M1: no
//     input system yet), a recording failure stops the run, the
//     structured replay/* events
//
// Hash conventions: the identity hashes are word-stream FNV-1a 64,
// big-endian byte order per u64 (the house convention —
// determinism_tests, prng_tests, laige-detcheck); the log trailer's
// fileHash is the canonical byte-stream FNV-1a 64 (fnv.org), per the
// format spec in laige/sim/replay.h (SCALE-005).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/result.h"
#include "laige/sim/determinism.h"
#include "laige/sim/engine.h"
#include "laige/sim/entity.h"
#include "laige/sim/replay.h"
#include "laige_test_seed.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the
// build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "replay_record_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "replay_record_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "replay_record_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

namespace {

// The test's Prng substream id (docs/testing.md §4: one named id per
// randomized test file — "REPL").
constexpr std::uint32_t kReplayTestSubstreamId = 0x52455041u;

// FNV-1a 64 constants (fnv.org — the house convention).
constexpr std::uint64_t kFnvBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// Byte-stream FNV-1a 64 (the trailer fileHash convention).
std::uint64_t fnv1aBytes(const std::uint8_t* bytes, std::size_t count) {
  std::uint64_t h = kFnvBasis;
  for (std::size_t i = 0; i < count; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= kFnvPrime;
  }
  return h;
}

// Little-endian appends (the format's byte order, SCALE-005).
void appendU16le(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}
void appendU32le(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}
void appendU64le(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

// A test identity (the engine's fields are checked separately).
laige::ReplayIdentity testIdentity() {
  laige::ReplayIdentity identity;
  identity.seed = 42;
  identity.tickRateHz = 60;
  identity.componentSchemaHash = 0x0123456789ABCDEFull;
  identity.mathBackendId =
      static_cast<std::uint32_t>(laige::SimMathBackend::FixedPoint16_16);
  identity.configHash = 0xFEDCBA9876543210ull;
  return identity;
}

// Builds `n` frames with PRNG-drawn payloads (0 .. maxLen-1 bytes
// each) — the round-trip's non-trivial case (M1 engine frames are
// zero-length; the format must still carry arbitrary blobs).
std::vector<laige::ReplayFrame> makeTestFrames(std::size_t n,
                                               std::size_t maxLen) {
  laige::Prng rng = laige::testing::TestPrng(kReplayTestSubstreamId);
  std::vector<laige::ReplayFrame> frames;
  frames.reserve(n);
  for (std::uint64_t tick = 1; tick <= n; ++tick) {
    laige::ReplayFrame frame;
    frame.tick = tick;
    const std::size_t len =
        maxLen == 0 ? 0
                    : static_cast<std::size_t>(
                          rng.next_range(0, static_cast<std::uint32_t>(maxLen)));
    frame.data.resize(len);
    for (std::uint8_t& byte : frame.data) {
      byte = static_cast<std::uint8_t>(rng.next_u64());
    }
    frames.push_back(std::move(frame));
  }
  return frames;
}

// Records `frames` to `path` through the public recorder (create ->
// writeFrame -> finish); returns the finish Status (the caller checks
// the frame writes through the recorder when a mid-way failure is
// expected).
laige::Status recordFrames(const std::string& path,
                           const laige::ReplayIdentity& identity,
                           const std::vector<laige::ReplayFrame>& frames,
                           std::uint64_t maxBytes) {
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
      laige::ReplayRecorder::create(identity, path, maxBytes);
  if (created.isError()) {
    return laige::Status(created.error());
  }
  laige::ReplayRecorder recorder = std::move(created).takeValue();
  for (const laige::ReplayFrame& frame : frames) {
    const laige::Status w = recorder.writeFrame(
        frame.tick, frame.data.data(), frame.data.size());
    if (w.isError()) {
      return w;
    }
  }
  return recorder.finish();
}

// A temp file path (gtest's temp dir; unique per test name).
std::string tempPath(const char* name) {
  return std::string(::testing::TempDir()) + name;
}

// One log event captured from the facade (the engine_tests.cpp
// MemorySink pattern — Warn+ only, rate limiting off).
class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
  };

  void emit(const laige::log::LogRecord& record) override {
    if (record.severity < laige::log::Severity::Warn) return;
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    for (const auto& f : record.fields) {
      e.fields.emplace_back(std::string(f.name), f.value);
    }
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

std::size_t countEvents(const MemorySink& sink, std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.event == event) ++n;
  }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// ReplayFormat: the log format's round trip + the malformed-input table
// (SCALE-005, ARCH-007)
// ---------------------------------------------------------------------------

TEST(ReplayFormat, RoundTripBytes) {
  // Record 8 frames (PRNG payloads, up to 16 bytes) to a temp file,
  // read the raw bytes back, parse, and compare every field — the
  // step's "record N ticks -> parse back -> identical bytes" clause.
  const std::string path = tempPath("replay_roundtrip.log");
  const std::vector<laige::ReplayFrame> frames = makeTestFrames(8, 16);
  const laige::Status status =
      recordFrames(path, testIdentity(), frames, 0 /* default cap */);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(std::filesystem::exists(path));

  std::vector<std::uint8_t> raw;
  {
    std::size_t size = static_cast<std::size_t>(std::filesystem::file_size(path));
    raw.resize(size);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fread(raw.data(), 1, raw.size(), f), raw.size());
    std::fclose(f);
  }
  const laige::Result<laige::ReplayLog, laige::ErrorCode> parsed =
      laige::parseReplay(raw.data(), raw.size());
  ASSERT_TRUE(parsed.ok());
  const laige::ReplayLog& log = parsed.value();
  const laige::ReplayIdentity expected = testIdentity();
  EXPECT_EQ(log.identity.seed, expected.seed);
  EXPECT_EQ(log.identity.tickRateHz, expected.tickRateHz);
  EXPECT_EQ(log.identity.componentSchemaHash, expected.componentSchemaHash);
  EXPECT_EQ(log.identity.mathBackendId, expected.mathBackendId);
  EXPECT_EQ(log.identity.configHash, expected.configHash);
  ASSERT_EQ(log.frames.size(), frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    EXPECT_EQ(log.frames[i].tick, frames[i].tick);
    EXPECT_EQ(log.frames[i].data, frames[i].data);  // identical bytes
  }
  // The trailer's fileHash matches the local canonical FNV-1a.
  const std::size_t bodyEnd = raw.size() - laige::kReplayTrailerSize;
  std::uint64_t trailerHash = 0;
  for (int i = 7; i >= 0; --i) {
    trailerHash = (trailerHash << 8) | raw[bodyEnd + 8 + i];
  }
  EXPECT_EQ(fnv1aBytes(raw.data(), bodyEnd), trailerHash);
  // Machine-greppable line (docs/testing.md §4).
  std::printf("replay-roundtrip frames=%zu bytes=%zu filehash=0x%016llx\n",
              log.frames.size(), raw.size(),
              static_cast<unsigned long long>(trailerHash));
}

TEST(ReplayFormat, RoundTripFile) {
  // The file form (loadReplay): the same round trip through the public
  // file reader.
  const std::string path = tempPath("replay_roundtrip_file.log");
  const std::vector<laige::ReplayFrame> frames = makeTestFrames(5, 8);
  const laige::Status status =
      recordFrames(path, testIdentity(), frames, 0);
  ASSERT_TRUE(status.ok());
  const laige::Result<laige::ReplayLog, laige::ErrorCode> parsed =
      laige::loadReplay(path);
  ASSERT_TRUE(parsed.ok());
  const laige::ReplayLog& log = parsed.value();
  ASSERT_EQ(log.frames.size(), frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    EXPECT_EQ(log.frames[i].tick, frames[i].tick);
    EXPECT_EQ(log.frames[i].data, frames[i].data);
  }
  EXPECT_EQ(log.identity.seed, 42u);
}

TEST(ReplayFormat, EncodingIsDeterministic) {
  // The same frames recorded twice produce byte-identical logs (no
  // timestamps, no addresses — the format is a pure function of
  // identity + frames).
  const std::string pathA = tempPath("replay_det_a.log");
  const std::string pathB = tempPath("replay_det_b.log");
  const std::vector<laige::ReplayFrame> frames = makeTestFrames(4, 12);
  ASSERT_TRUE(recordFrames(pathA, testIdentity(), frames, 0).ok());
  ASSERT_TRUE(recordFrames(pathB, testIdentity(), frames, 0).ok());
  std::vector<std::uint8_t> a, b;
  a.resize(static_cast<std::size_t>(std::filesystem::file_size(pathA)));
  b.resize(static_cast<std::size_t>(std::filesystem::file_size(pathB)));
  std::FILE* fa = std::fopen(pathA.c_str(), "rb");
  std::FILE* fb = std::fopen(pathB.c_str(), "rb");
  ASSERT_NE(fa, nullptr);
  ASSERT_NE(fb, nullptr);
  ASSERT_EQ(std::fread(a.data(), 1, a.size(), fa), a.size());
  ASSERT_EQ(std::fread(b.data(), 1, b.size(), fb), b.size());
  std::fclose(fa);
  std::fclose(fb);
  EXPECT_EQ(a, b);
}

TEST(ReplayFormat, ZeroFrameLog) {
  // A zero-tick run records a header + trailer only — legal and
  // round-trips.
  const std::string path = tempPath("replay_zero.log");
  const laige::Status status = recordFrames(path, testIdentity(), {}, 0);
  ASSERT_TRUE(status.ok());
  const laige::Result<laige::ReplayLog, laige::ErrorCode> parsed =
      laige::loadReplay(path);
  ASSERT_TRUE(parsed.ok());
  EXPECT_TRUE(parsed.value().frames.empty());
  EXPECT_EQ(parsed.value().identity.seed, 42u);
}

TEST(ReplayFormat, FrameAtTheCapRoundTrips) {
  // A frame of EXACTLY kMaxReplayFrameBytes is legal and round-trips.
  laige::ReplayFrame frame;
  frame.tick = 1;
  frame.data.assign(laige::kMaxReplayFrameBytes, 0xA5u);
  const std::string path = tempPath("replay_maxframe.log");
  const std::vector<laige::ReplayFrame> frames{frame};
  ASSERT_TRUE(recordFrames(path, testIdentity(), frames, 0).ok());
  const laige::Result<laige::ReplayLog, laige::ErrorCode> parsed =
      laige::loadReplay(path);
  ASSERT_TRUE(parsed.ok());
  ASSERT_EQ(parsed.value().frames.size(), 1u);
  EXPECT_EQ(parsed.value().frames[0].data.size(),
            static_cast<std::size_t>(laige::kMaxReplayFrameBytes));
}

// Hand-builds a v1 log (bypassing the recorder) so the parser's
// individual violation branches can be exercised in isolation.
std::vector<std::uint8_t> buildLog(std::uint16_t version,
                                   const std::uint8_t* magic,
                                   std::uint64_t seed, std::uint32_t rate,
                                   std::uint64_t schemaHash,
                                   std::uint32_t mathId,
                                   std::uint64_t configHash,
                                   std::vector<std::pair<std::uint64_t,
                                                        std::vector<std::uint8_t>>>
                                       frames,
                                   std::uint64_t trailerCount,
                                   std::uint64_t trailerHashOverride) {
  std::vector<std::uint8_t> log;
  log.insert(log.end(), magic, magic + 4);
  appendU16le(log, version);
  appendU16le(log, 0);  // reserved
  appendU64le(log, seed);
  appendU32le(log, rate);
  appendU64le(log, schemaHash);
  appendU32le(log, mathId);
  appendU64le(log, configHash);
  for (const auto& [tick, data] : frames) {
    appendU64le(log, tick);
    appendU32le(log, static_cast<std::uint32_t>(data.size()));
    log.insert(log.end(), data.begin(), data.end());
  }
  appendU64le(log, trailerCount);
  if (trailerHashOverride != 0) {
    appendU64le(log, trailerHashOverride);
  } else {
    appendU64le(log, fnv1aBytes(log.data(), log.size()));
  }
  return log;
}

TEST(ReplayFormat, MalformedInputTable) {
  // The full SCALE-005 violation table: every case is a MalformedInput
  // (never a crash, never a silent skip — CORE-008, ARCH-007).
  const std::uint8_t magic[4] = {'L', 'G', 'R', 'P'};
  const std::uint8_t badMagic[4] = {'L', 'G', 'R', 'Q'};
  const std::vector<std::uint8_t> body(1, 0xABu);
  auto expectMalformed = [&](const std::vector<std::uint8_t>& log,
                             const char* name) {
    const laige::Result<laige::ReplayLog, laige::ErrorCode> r =
        laige::parseReplay(log.data(), log.size());
    EXPECT_TRUE(r.isError()) << name;
    if (r.isError()) {
      EXPECT_EQ(r.error(), laige::ErrorCode::MalformedInput) << name;
    }
  };

  // 1. Truncation: EVERY cut of a valid log is malformed (never a
  //    crash).
  {
    const std::vector<std::uint8_t> valid = buildLog(
        laige::kReplayFormatVersion, magic, 42, 60, 7, 0, 8,
        {{1, body}}, 1, 0);
    for (std::size_t cut = 1; cut < valid.size(); ++cut) {
      expectMalformed(std::vector<std::uint8_t>(valid.begin(),
                                                valid.begin() +
                                                    static_cast<std::ptrdiff_t>(cut)),
                      "truncated");
    }
  }
  // 2. Bad magic.
  expectMalformed(
      buildLog(laige::kReplayFormatVersion, badMagic, 42, 60, 7, 0, 8, {},
               0, 0),
      "bad_magic");
  // 3. Unsupported version.
  expectMalformed(
      buildLog(2, magic, 42, 60, 7, 0, 8, {}, 0, 0), "bad_version");
  // 4. Non-zero reserved field (built on a valid log, patched).
  {
    std::vector<std::uint8_t> log = buildLog(laige::kReplayFormatVersion,
                                             magic, 42, 60, 7, 0, 8, {}, 0, 0);
    log[6] = 1;
    expectMalformed(log, "reserved_nonzero");
  }
  // 5. Frame length above kMaxReplayFrameBytes (a 0-length header
  //    lying about a 1 MiB + 1 payload: the length-field check must
  //    fire before any overrun read).
  {
    std::vector<std::uint8_t> log =
        buildLog(laige::kReplayFormatVersion, magic, 42, 60, 7, 0, 8, {}, 0, 0);
    // Insert a frame record whose length field is kMaxReplayFrameBytes+1
    // (12 bytes of record, no payload).
    std::vector<std::uint8_t> patched;
    patched.insert(patched.end(), log.begin(),
                   log.begin() + static_cast<std::ptrdiff_t>(
                       laige::kReplayHeaderSize));
    appendU64le(patched, 1);
    appendU32le(patched, laige::kMaxReplayFrameBytes + 1);
    patched.insert(patched.end(), log.end() - laige::kReplayTrailerSize,
                   log.end());
    expectMalformed(patched, "frame_length_over_cap");
  }
  // 6. Out-of-sequence tick (frame 2 carries tick 3).
  expectMalformed(
      buildLog(laige::kReplayFormatVersion, magic, 42, 60, 7, 0, 8,
               {{1, body}, {3, body}}, 2, 0),
      "tick_sequence");
  // 7. Trailer frameCount mismatch (the hash is correct — the count
  //    branch fires on its own).
  expectMalformed(
      buildLog(laige::kReplayFormatVersion, magic, 42, 60, 7, 0, 8, {{1, body}},
               2, 0),
      "trailer_count_mismatch");
  // 8. fileHash mismatch (a flipped body byte; the hash branch fires).
  {
    std::vector<std::uint8_t> log = buildLog(laige::kReplayFormatVersion,
                                             magic, 42, 60, 7, 0, 8, {{1, body}},
                                             1, 0);
    log[laige::kReplayHeaderSize + 12] ^= 0xFFu;  // flip the payload byte
    expectMalformed(log, "hash_mismatch");
  }
  // 9. Trailing garbage (append a byte past the trailer).
  {
    std::vector<std::uint8_t> log = buildLog(laige::kReplayFormatVersion,
                                             magic, 42, 60, 7, 0, 8, {}, 0, 0);
    log.push_back(0u);
    expectMalformed(log, "trailing_garbage");
  }
  // 10. Empty input and null data.
  expectMalformed({}, "empty_input");
  const laige::Result<laige::ReplayLog, laige::ErrorCode> nullData =
      laige::parseReplay(nullptr, 16);
  ASSERT_TRUE(nullData.isError());
  EXPECT_EQ(nullData.error(), laige::ErrorCode::MalformedInput);
}

TEST(ReplayFormat, LoadReplayFileErrors) {
  // The file wrapper's error table: missing file -> IoError; empty
  // path -> MalformedInput; an oversized file (size > cap) ->
  // MalformedInput (the ADR 0003 bound precedent).
  const laige::Result<laige::ReplayLog, laige::ErrorCode> missing =
      laige::loadReplay(tempPath("replay_missing.log"));
  ASSERT_TRUE(missing.isError());
  EXPECT_EQ(missing.error(), laige::ErrorCode::IoError);

  const laige::Result<laige::ReplayLog, laige::ErrorCode> emptyPath =
      laige::loadReplay("");
  ASSERT_TRUE(emptyPath.isError());
  EXPECT_EQ(emptyPath.error(), laige::ErrorCode::MalformedInput);

  const std::string path = tempPath("replay_oversized.log");
  ASSERT_TRUE(recordFrames(path, testIdentity(), {}, 0).ok());
  const std::size_t size =
      static_cast<std::size_t>(std::filesystem::file_size(path));
  // cap = size - 1: the reader's total > cap check rejects (the
  // ADR 0003 bound precedent — an oversized file is a MalformedInput,
  // not a truncated parse).
  const laige::Result<laige::ReplayLog, laige::ErrorCode> oversize =
      laige::loadReplay(path, size - 1);
  ASSERT_TRUE(oversize.isError());
  EXPECT_EQ(oversize.error(), laige::ErrorCode::MalformedInput);
}

// ---------------------------------------------------------------------------
// ReplayRecorder: atomicity, the size limit, the tick sequence
// ---------------------------------------------------------------------------

TEST(ReplayRecorder, AtomicWrite) {
  // A successful finish leaves the final file and removes the temp.
  const std::string path = tempPath("replay_atomic.log");
  const std::string tmp = path + ".tmp";
  const std::vector<laige::ReplayFrame> frames = makeTestFrames(3, 4);
  ASSERT_TRUE(recordFrames(path, testIdentity(), frames, 0).ok());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(tmp));
}

TEST(ReplayRecorder, InterruptedLeavesNoFile) {
  // Destroying an unfinished recorder leaves NO file at the final
  // path (the temp is removed — no partial replay on disk).
  const std::string path = tempPath("replay_interrupted.log");
  const std::string tmp = path + ".tmp";
  {
    laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
        laige::ReplayRecorder::create(testIdentity(), path, 0);
    ASSERT_TRUE(created.ok());
    laige::ReplayRecorder recorder = std::move(created).takeValue();
    ASSERT_TRUE(recorder.writeFrame(1, nullptr, 0).ok());
    // No finish(): the destructor cleans up.
  }
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(tmp));
}

TEST(ReplayRecorder, SizeLimitStopsFrames) {
  // cap = kMinReplaySizeLimit (header + trailer): frame 1 fits
  // (40 + 12 = 52 <= 56), frame 2 exceeds the cap -> BudgetExhausted;
  // the failure is sticky and finish cannot recover it.
  const std::string path = tempPath("replay_sized.log");
  laige::Status second{}, again{}, finish{};
  bool finished = false;
  {
    laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
        laige::ReplayRecorder::create(testIdentity(), path,
                                      laige::kMinReplaySizeLimit);
    ASSERT_TRUE(created.ok());
    laige::ReplayRecorder recorder = std::move(created).takeValue();
    ASSERT_TRUE(recorder.writeFrame(1, nullptr, 0).ok());
    second = recorder.writeFrame(2, nullptr, 0);
    // Sticky: every later call reports the same error.
    again = recorder.writeFrame(2, nullptr, 0);
    finish = recorder.finish();
    finished = recorder.finished();
    if (recorder.status().isError()) {
      EXPECT_EQ(recorder.status().error(), laige::ErrorCode::BudgetExhausted);
    }
  }
  ASSERT_TRUE(second.isError());
  EXPECT_EQ(second.error(), laige::ErrorCode::BudgetExhausted);
  ASSERT_TRUE(again.isError());
  EXPECT_EQ(again.error(), second.error());
  ASSERT_TRUE(finish.isError());
  EXPECT_EQ(finish.error(), second.error());
  EXPECT_FALSE(finished);
  // No partial log at the final path; the temp is removed by the
  // destructor (now run).
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

TEST(ReplayRecorder, SizeLimitAtFinish) {
  // cap = 67 (header 40 + frame 1 record 12 + frame 2 record 12 +
  // 3 payload bytes = 67): both frames fit exactly, but the trailer
  // (16 bytes) would exceed the cap -> finish is a BudgetExhausted
  // (the cap counts header + frames + trailer together).
  const std::string path = tempPath("replay_finishcap.log");
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
      laige::ReplayRecorder::create(testIdentity(), path, 67);
  ASSERT_TRUE(created.ok());
  laige::ReplayRecorder recorder = std::move(created).takeValue();
  ASSERT_TRUE(recorder.writeFrame(1, nullptr, 0).ok());
  const std::uint8_t payload[3] = {0x01u, 0x02u, 0x03u};
  ASSERT_TRUE(recorder.writeFrame(2, payload, 3).ok());
  EXPECT_EQ(recorder.bytesWritten(), 67u);
  const laige::Status finish = recorder.finish();
  ASSERT_TRUE(finish.isError());
  EXPECT_EQ(finish.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_FALSE(recorder.finished());
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

TEST(ReplayRecorder, TickSequence) {
  // The strict 1, 2, 3, ... sequence: a first frame at tick 2 is an
  // InvalidArgument; a repeated tick 1 is an InvalidArgument.
  {
    const std::string path = tempPath("replay_seq_a.log");
    laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
        laige::ReplayRecorder::create(testIdentity(), path, 0);
    ASSERT_TRUE(created.ok());
    laige::ReplayRecorder recorder = std::move(created).takeValue();
    const laige::Status s = recorder.writeFrame(2, nullptr, 0);
    ASSERT_TRUE(s.isError());
    EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  }
  {
    const std::string path = tempPath("replay_seq_b.log");
    laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
        laige::ReplayRecorder::create(testIdentity(), path, 0);
    ASSERT_TRUE(created.ok());
    laige::ReplayRecorder recorder = std::move(created).takeValue();
    ASSERT_TRUE(recorder.writeFrame(1, nullptr, 0).ok());
    const laige::Status s = recorder.writeFrame(1, nullptr, 0);
    ASSERT_TRUE(s.isError());
    EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  }
}

TEST(ReplayRecorder, WriteAfterFinishFails) {
  // A finished recorder accepts no frames (InvalidArgument).
  const std::string path = tempPath("replay_afterfinish.log");
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
      laige::ReplayRecorder::create(testIdentity(), path, 0);
  ASSERT_TRUE(created.ok());
  laige::ReplayRecorder recorder = std::move(created).takeValue();
  ASSERT_TRUE(recorder.writeFrame(1, nullptr, 0).ok());
  ASSERT_TRUE(recorder.finish().ok());
  EXPECT_TRUE(recorder.finished());
  const laige::Status s = recorder.writeFrame(2, nullptr, 0);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  const laige::Status finish = recorder.finish();  // finish is one-shot
  ASSERT_TRUE(finish.isError());
  EXPECT_EQ(finish.error(), s.error());
}

TEST(ReplayRecorder, CreateValidation) {
  // The create error table: empty path, a cap below the minimum
  // (kMinReplaySizeLimit), and the default cap (0).
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> emptyPath =
      laige::ReplayRecorder::create(testIdentity(), "", 0);
  ASSERT_TRUE(emptyPath.isError());
  EXPECT_EQ(emptyPath.error(), laige::ErrorCode::InvalidArgument);

  const std::string path = tempPath("replay_min.log");
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> tiny =
      laige::ReplayRecorder::create(testIdentity(), path,
                                    laige::kMinReplaySizeLimit - 1);
  ASSERT_TRUE(tiny.isError());
  EXPECT_EQ(tiny.error(), laige::ErrorCode::InvalidArgument);

  // maxBytes == 0 means the default cap; the recorder then reports
  // the header size as bytes written.
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> ok =
      laige::ReplayRecorder::create(testIdentity(), path, 0);
  ASSERT_TRUE(ok.ok());
  laige::ReplayRecorder recorder = std::move(ok).takeValue();
  EXPECT_EQ(recorder.bytesWritten(), laige::kReplayHeaderSize);
  EXPECT_EQ(recorder.frameCount(), 0u);
  EXPECT_STREQ(recorder.path(), path.c_str());
  EXPECT_TRUE(recorder.status().ok());
}

TEST(ReplayRecorder, MoveTransfersTheFile) {
  // A moved-from recorder is finished (its destructor cleans nothing);
  // the destination owns the file and can finish normally.
  const std::string path = tempPath("replay_move.log");
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> created =
      laige::ReplayRecorder::create(testIdentity(), path, 0);
  ASSERT_TRUE(created.ok());
  laige::ReplayRecorder source = std::move(created).takeValue();
  ASSERT_TRUE(source.writeFrame(1, nullptr, 0).ok());
  laige::ReplayRecorder destination(std::move(source));
  EXPECT_TRUE(source.finished());  // the source is now inert
  ASSERT_TRUE(destination.writeFrame(2, nullptr, 0).ok());
  ASSERT_TRUE(destination.finish().ok());
  EXPECT_TRUE(std::filesystem::exists(path));
}

// ---------------------------------------------------------------------------
// ReplayIdentity: the pure-integer FNV-1a hashes (ADR 0002, ARCH-010)
// ---------------------------------------------------------------------------

// Test components (global scope: LAIGE_COMPONENT specializes the
// trait at global scope). Two distinct types, so the schema-hash
// ordering test can contrast registration orders.
struct ReplayTag {
  std::int32_t value{};
};
LAIGE_COMPONENT(ReplayTag);

struct ReplayTag2 {
  std::int32_t value{};
  std::int32_t other{};
};
LAIGE_COMPONENT(ReplayTag2);

TEST(ReplayIdentity, SchemaHashIsRegistrationOrder) {
  // Two worlds that register the same types in the SAME order hash
  // identically; a different registration order (different dense ids)
  // hashes differently. No addresses enter the hash (ARCH-010).
  laige::World::Options options;
  options.capacity = 16;
  laige::Result<laige::World, laige::ErrorCode> r1 =
      laige::World::create(options);
  laige::Result<laige::World, laige::ErrorCode> r2 =
      laige::World::create(options);
  laige::Result<laige::World, laige::ErrorCode> r3 =
      laige::World::create(options);
  ASSERT_TRUE(r1.ok() && r2.ok() && r3.ok());
  laige::World w1 = std::move(r1).takeValue();
  laige::World w2 = std::move(r2).takeValue();
  laige::World w3 = std::move(r3).takeValue();
  // w1, w2: A then B. w3: B then A.
  ASSERT_TRUE(w1.registerComponent<ReplayTag>().ok());
  ASSERT_TRUE(w2.registerComponent<ReplayTag>().ok());
  ASSERT_TRUE(w1.registerComponent<ReplayTag2>().ok());
  ASSERT_TRUE(w2.registerComponent<ReplayTag2>().ok());
  ASSERT_TRUE(w3.registerComponent<ReplayTag2>().ok());
  ASSERT_TRUE(w3.registerComponent<ReplayTag>().ok());

  const std::uint64_t h1 = laige::componentSchemaHash(w1);
  const std::uint64_t h2 = laige::componentSchemaHash(w2);
  const std::uint64_t h3 = laige::componentSchemaHash(w3);
  EXPECT_EQ(h1, h2);  // same order -> same hash
  EXPECT_NE(h1, h3);  // different order -> different ids -> different
  // Machine-greppable line (docs/testing.md §4).
  std::printf("replay-identity schema-order h1=0x%016llx h3=0x%016llx\n",
              static_cast<unsigned long long>(h1),
              static_cast<unsigned long long>(h3));
}

TEST(ReplayIdentity, ConfigHashDiffersPerField) {
  // The config hash is a pure function of the config's fields: same
  // config -> same hash; any field change -> different hash.
  laige::EngineConfig base;
  base.tickRateHz = 60;
  base.entityCapacity = 1024;
  base.churnPerFrameBudget = 256;
  base.seed = 7;

  laige::EngineConfig other = base;
  EXPECT_EQ(laige::configHash(base), laige::configHash(other));

  other.seed = 8;
  EXPECT_NE(laige::configHash(base), laige::configHash(other));

  other = base;
  other.tickRateHz = 120;
  EXPECT_NE(laige::configHash(base), laige::configHash(other));

  other = base;
  other.entityCapacity = 2048;
  EXPECT_NE(laige::configHash(base), laige::configHash(other));

  other = base;
  other.determinism.enabled = false;
  EXPECT_NE(laige::configHash(base), laige::configHash(other));

  other = base;
  other.determinism.math = laige::SimMathBackend::FloatPinned32;
  EXPECT_NE(laige::configHash(base), laige::configHash(other));
}

TEST(ReplayIdentity, MakeIdentityEchoesTheFields) {
  // makeReplayIdentity assembles (seed, tickRate, schemaHash, mathId,
  // configHash) from (world, config).
  laige::World::Options options;
  options.capacity = 8;
  options.seed = 99;
  laige::Result<laige::World, laige::ErrorCode> wResult =
      laige::World::create(options);
  ASSERT_TRUE(wResult.ok());
  laige::World w = std::move(wResult).takeValue();
  ASSERT_TRUE(w.registerComponent<ReplayTag>().ok());

  laige::EngineConfig config;
  config.tickRateHz = 90;
  config.entityCapacity = 8;
  config.seed = 99;
  config.determinism.enabled = true;
  config.determinism.math = laige::SimMathBackend::FixedPoint16_16;

  const laige::ReplayIdentity identity =
      laige::makeReplayIdentity(w, config);
  EXPECT_EQ(identity.seed, 99u);
  EXPECT_EQ(identity.tickRateHz, 90u);
  EXPECT_EQ(identity.mathBackendId,
            static_cast<std::uint32_t>(laige::SimMathBackend::FixedPoint16_16));
  EXPECT_EQ(identity.componentSchemaHash, laige::componentSchemaHash(w));
  EXPECT_EQ(identity.configHash, laige::configHash(config));
}

// ---------------------------------------------------------------------------
// ReplayEngine: the engine's per-tick empty-frame recording
// (M1-DET-02: FR-1.4, PRD Appendix A, ADR 0002)
// ---------------------------------------------------------------------------

TEST(ReplayEngine, RecordsEmptyFramesPerTick) {
  // An engine run with recording ON records one zero-length frame per
  // COMPLETED tick, and the log's identity matches the run's identity
  // (seed, tick rate, schema hash, math backend, config hash).
  laige::EngineConfig config;
  config.tickRateHz = 60;
  config.entityCapacity = 8;
  config.seed = 42;

  laige::Result<laige::Engine, laige::ErrorCode> created =
      laige::Engine::create(config);
  ASSERT_TRUE(created.ok());
  laige::Engine engine = std::move(created).takeValue();
  // Capture the expected identity BEFORE the run (the world is owned
  // by the engine and released in the run's shutdown).
  const laige::ReplayIdentity expected =
      laige::makeReplayIdentity(*engine.world(), config);

  const std::string path = tempPath("replay_engine.log");
  ASSERT_TRUE(engine.startReplayRecording(path, 0).ok());
  EXPECT_TRUE(engine.replayRecordingActive());
  EXPECT_EQ(engine.replayBytesWritten(), laige::kReplayHeaderSize);

  ASSERT_TRUE(engine.run_headless(8, laige::kDefaultMaxCatchUpTicks).ok());
  EXPECT_EQ(engine.stats().ticks, 8u);
  EXPECT_TRUE(engine.isShutDown());

  // The log appears at the final path only after the successful run.
  ASSERT_TRUE(std::filesystem::exists(path));
  const laige::Result<laige::ReplayLog, laige::ErrorCode> parsed =
      laige::loadReplay(path);
  ASSERT_TRUE(parsed.ok());
  const laige::ReplayLog& log = parsed.value();
  EXPECT_EQ(log.identity.seed, expected.seed);
  EXPECT_EQ(log.identity.tickRateHz, expected.tickRateHz);
  EXPECT_EQ(log.identity.componentSchemaHash, expected.componentSchemaHash);
  EXPECT_EQ(log.identity.mathBackendId, expected.mathBackendId);
  EXPECT_EQ(log.identity.configHash, expected.configHash);
  // 8 zero-length frames: header + 8 x 12-byte records + trailer.
  ASSERT_EQ(log.frames.size(), 8u);
  for (std::size_t i = 0; i < log.frames.size(); ++i) {
    EXPECT_EQ(log.frames[i].tick, i + 1);
    EXPECT_TRUE(log.frames[i].data.empty());  // M1: no input yet
  }
  // Machine-greppable line (docs/testing.md §4).
  std::printf("replay-engine ticks=%u bytes=%zu schema=0x%016llx\n",
              static_cast<unsigned>(engine.stats().ticks),
              log.frames.size(),
              static_cast<unsigned long long>(expected.componentSchemaHash));
}

TEST(ReplayEngine, RecordingFailureStopsTheRun) {
  // A size-limit breach mid-run STOPS the run: run_headless returns
  // the BudgetExhausted Status, no partial log is left at the final
  // path, and the structured events land (replay/record_failed Error,
  // replay/record_aborted Warn in the ordered shutdown).
  laige::EngineConfig config;
  config.tickRateHz = 60;
  config.entityCapacity = 8;
  config.seed = 42;

  laige::Result<laige::Engine, laige::ErrorCode> created =
      laige::Engine::create(config);
  ASSERT_TRUE(created.ok());
  laige::Engine engine = std::move(created).takeValue();
  const std::string path = tempPath("replay_engine_fail.log");
  // cap = header + trailer: frame 1 fits, frame 2 breaches.
  ASSERT_TRUE(
      engine.startReplayRecording(path, laige::kMinReplaySizeLimit).ok());

  MemorySink* sink = installCaptureSink();
  const laige::Status status =
      engine.run_headless(8, laige::kDefaultMaxCatchUpTicks);
  // Read the capture BEFORE restoreLogger (re-init retires the sink —
  // the engine_tests.cpp pattern: sink assertions come first).
  const std::size_t recordFailed = countEvents(*sink, "record_failed");
  const std::size_t recordAborted = countEvents(*sink, "record_aborted");
  restoreLogger();

  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_TRUE(engine.isShutDown());  // the ordered shutdown still ran
  EXPECT_FALSE(std::filesystem::exists(path));  // no partial log
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
  EXPECT_EQ(recordFailed, 1u);
  EXPECT_EQ(recordAborted, 1u);
}

TEST(ReplayEngine, DoubleStartFails) {
  // One recording per run: the second startReplayRecording is an
  // InvalidArgument plus the structured warn (replay/record_already_
  // started).
  laige::EngineConfig config;
  config.tickRateHz = 60;
  config.entityCapacity = 8;
  laige::Result<laige::Engine, laige::ErrorCode> created =
      laige::Engine::create(config);
  ASSERT_TRUE(created.ok());
  laige::Engine engine = std::move(created).takeValue();

  MemorySink* sink = installCaptureSink();
  const std::string path = tempPath("replay_double.log");
  ASSERT_TRUE(engine.startReplayRecording(path, 0).ok());
  const laige::Status second = engine.startReplayRecording(path, 0);
  // Read the capture BEFORE restoreLogger (re-init retires the sink).
  const std::size_t alreadyStarted =
      countEvents(*sink, "record_already_started");
  restoreLogger();
  ASSERT_TRUE(second.isError());
  EXPECT_EQ(second.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(alreadyStarted, 1u);
}

TEST(ReplayEngine, StartAfterRunFails) {
  // A stopped engine (post-run) is a no-op failure without logging —
  // the stopped-state precedent.
  laige::EngineConfig config;
  config.tickRateHz = 60;
  config.entityCapacity = 8;
  laige::Result<laige::Engine, laige::ErrorCode> created =
      laige::Engine::create(config);
  ASSERT_TRUE(created.ok());
  laige::Engine engine = std::move(created).takeValue();
  ASSERT_TRUE(engine.run_headless(1, laige::kDefaultMaxCatchUpTicks).ok());

  MemorySink* sink = installCaptureSink();
  const std::size_t entriesBefore = sink->entries.size();
  const laige::Status status =
      engine.startReplayRecording(tempPath("replay_postrun.log"), 0);
  // Read the capture BEFORE restoreLogger (re-init retires the sink).
  const std::size_t entriesAfter = sink->entries.size();
  restoreLogger();
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(entriesAfter, entriesBefore);  // no log (precedent)
  EXPECT_FALSE(engine.replayRecordingActive());
  EXPECT_EQ(engine.replayBytesWritten(), 0u);
}
