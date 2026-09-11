// laige-core logging facade suite (M0-CORE-02).
//
// Step Verify scope (roadmap/M0-foundations.md):
//   - `ctest -R logging` green
//   - disabled levels allocate nothing: asserted with the test-only
//     process-wide allocation counter (logging_alloc_counter.cpp —
//     the M0 stand-in for M0-CORE-05's pool accounting, which does not
//     exist yet), backed by the ASan/TSan build trees and a timing
//     property test
//   - rate-limit summary emitted after N repeats (LOG-004)
//
// NFR-8.10 self-checks: this translation unit compiles with
// -fno-exceptions -fno-rtti (laige_apply_engine_policy); the
// static_asserts below make a policy violation fail the build.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"

#include "logging_alloc_counter.h"

#if defined(__unix__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "logging_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "logging_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "logging_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(_MSC_VER)
#  define LOGGING_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define LOGGING_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if LOGGING_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "logging_tests must be built as C++20 (NFR-8.10); see "
              "laige_apply_engine_policy().");
#endif

namespace {

using laige::log::Level;
using laige::log::Logger;
using laige::log::LogRecord;
using laige::log::Severity;

// A sink that records every emitted record in memory (test oracle).
struct CapturedRecord {
  Severity severity{};
  std::string subsystem;
  std::string event;
  std::string message;
  std::vector<std::pair<std::string, std::string>> fields;
  std::chrono::system_clock::time_point timestamp{};
  std::uint32_t threadId{};
};

class CaptureSink : public laige::log::Sink {
 public:
  void emit(const LogRecord& r) override {
    std::lock_guard lk(mutex_);
    CapturedRecord c;
    c.severity = r.severity;
    c.subsystem.assign(r.subsystem);
    c.event.assign(r.event);
    c.message.assign(r.message);
    for (const laige::log::Field& f : r.fields) {
      c.fields.emplace_back(std::string(f.name), f.value);
    }
    c.timestamp = r.timestamp;
    c.threadId = r.threadId;
    records_.push_back(std::move(c));
  }
  void flush() override {
    std::lock_guard lk(mutex_);
    ++flushCount_;
  }
  void reset() {
    std::lock_guard lk(mutex_);
    records_.clear();
    flushCount_ = 0;
  }
  std::vector<CapturedRecord> takeRecords() {
    std::lock_guard lk(mutex_);
    std::vector<CapturedRecord> out = std::move(records_);
    records_.clear();
    return out;
  }
  std::size_t recordCount() const {
    std::lock_guard lk(mutex_);
    return records_.size();
  }
  std::size_t flushCount() const {
    std::lock_guard lk(mutex_);
    return flushCount_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<CapturedRecord> records_;
  std::size_t flushCount_ = 0;
};

CaptureSink& capture() {
  static CaptureSink cap;
  return cap;
}

// Forwards into the global capture sink so the logger can own its sink
// (LoggerOptions moves the unique_ptr in) while the test keeps an
// observable handle. Test shim: it intentionally does not flush in its
// destructor (the logger never relies on a forwarding shim for
// LOG-007; the real sinks honor the Sink contract).
class ForwardingSink : public laige::log::Sink {
 public:
  explicit ForwardingSink(CaptureSink& target) : target_(target) {}
  void emit(const LogRecord& r) override { target_.emit(r); }
  void flush() override { target_.flush(); }

 private:
  CaptureSink& target_;
};

// (Re)configure the process logger for one test: fresh forwarding sink,
// real clock unless the options carry one. Takes the options by value
// (LoggerOptions owns a unique_ptr and is move-only) — callers pass an
// rvalue.
void resetLogger(laige::log::LoggerOptions tweaks =
                     laige::log::LoggerOptions()) {
  if (tweaks.sink == nullptr) {
    tweaks.sink = std::make_unique<ForwardingSink>(capture());
  }
  EXPECT_TRUE(Logger::instance().init(std::move(tweaks)).ok());
  capture().reset();
}

// Fake clock (rate-limit tests): advances manually, same thread.
std::chrono::system_clock::time_point gFakeNow{};
std::chrono::system_clock::time_point fakeClock() { return gFakeNow; }

void resetRateLogger(std::chrono::milliseconds window) {
  laige::log::LoggerOptions o;
  o.clock = &fakeClock;
  o.rateWindow = window;
  resetLogger(std::move(o));
}

// The field value for `name` in a captured record (""); tests only rely
// on non-empty expected values, so "" doubles as "absent".
std::string fieldAt(const CapturedRecord& r, std::string_view name) {
  for (const auto& [k, v] : r.fields) {
    if (k == name) return v;
  }
  return {};
}

// True for a well-formed log line start:
// "YYYY-MM-DDTHH:MM:SS.ffffffZ " (27 chars + a space). Hand-rolled so
// this suite stays free of the exception machinery it tests the engine
// against (std::regex would work but is not needed).
bool hasTimestampPrefix(std::string_view line) {
  if (line.size() < 28) return false;
  static const char kSpecial[28] = {0,  0,  0,  0, '-', 0,  0,  '-',
                                    0,  0,  'T', 0,  0,  ':',  0,  0,
                                    ':', 0,  0,  '.',  0,  0,  0,  0,  0,
                                    0,  'Z', ' '};
  for (int i = 0; i < 28; ++i) {
    const char c = line[i];
    if (kSpecial[i] != 0) {
      if (c != kSpecial[i]) return false;
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

std::string tempFilePath(const char* name) {
  return std::string("laige_logging_test_") + name + ".log";
}

// Portable file open/remove for the log-file tests (CPP-009 compile-time
// platform boundary): MSVC's CRT deprecates plain `fopen` (C4996, an error
// under the engine's /WX policy) in favor of the secure variant `fopen_s` —
// same success semantics, via an out-parameter and an errno_t return.
// `remove` has no secure variant in the Windows 10+ UCRT (and is not
// deprecated), so both branches use the standard `std::remove`.
#if defined(_MSC_VER)
std::FILE* openLogFile(const char* path, const char* mode) {
  std::FILE* stream = nullptr;
  return (::fopen_s(&stream, path, mode) == 0) ? stream : nullptr;
}
#else
std::FILE* openLogFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif
bool removeLogFile(const char* path) { return std::remove(path) == 0; }

std::string readWholeFile(const std::string& path) {
  std::FILE* f = openLogFile(path.c_str(), "rb");
  if (f == nullptr) return {};
  std::string out;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  std::fclose(f);
  return out;
}

LogRecord makeRecord(Severity severity, std::string_view subsystem,
                     std::string_view event, std::string_view message,
                     std::span<const laige::log::Field> fields) {
  return LogRecord{std::chrono::system_clock::now(), severity, subsystem,
                   event, message, fields, 0};
}

}  // namespace

// ---------------------------------------------------------------------------
// Level gating: global minimum + per-subsystem scopes
// ---------------------------------------------------------------------------

TEST(LogGate, DefaultLevelsGateTrace) {
  resetLogger();
  const Logger& lg = Logger::instance();
  // Trace is disabled by default (AGENTS §14); the rest pass.
  EXPECT_FALSE(lg.enabled(Severity::Trace, "sys"));
  EXPECT_TRUE(lg.enabled(Severity::Debug, "sys"));
  EXPECT_TRUE(lg.enabled(Severity::Info, "sys"));
  EXPECT_TRUE(lg.enabled(Severity::Warn, "sys"));
  EXPECT_TRUE(lg.enabled(Severity::Error, "sys"));
  EXPECT_TRUE(lg.enabled(Severity::Fatal, "sys"));
  EXPECT_EQ(lg.globalMinimum(), Level::Debug);
  // Unregistered subsystems take the default subsystem level.
  EXPECT_EQ(lg.subsystemLevel("unregistered"), Level::Debug);
}

TEST(LogGate, PerSubsystemLevels) {
  resetLogger();
  Logger& lg = Logger::instance();
  lg.setSubsystemLevel("quiet", Level::Off);
  EXPECT_FALSE(lg.enabled(Severity::Error, "quiet"));
  EXPECT_FALSE(lg.enabled(Severity::Fatal, "quiet"));
  EXPECT_EQ(lg.subsystemLevel("quiet"), Level::Off);

  lg.setSubsystemLevel("critical", Level::Fatal);
  EXPECT_FALSE(lg.enabled(Severity::Error, "critical"));
  EXPECT_TRUE(lg.enabled(Severity::Fatal, "critical"));
  EXPECT_EQ(lg.subsystemLevel("critical"), Level::Fatal);

  // Re-setting the same subsystem updates in place.
  lg.setSubsystemLevel("quiet", Level::Warn);
  EXPECT_TRUE(lg.enabled(Severity::Warn, "quiet"));
  EXPECT_FALSE(lg.enabled(Severity::Info, "quiet"));
}

TEST(LogGate, GlobalMinimumDominates) {
  resetLogger();
  Logger& lg = Logger::instance();
  lg.setSubsystemLevel("loose", Level::Debug);
  lg.setGlobalMinimum(Level::Error);
  EXPECT_FALSE(lg.enabled(Severity::Warn, "loose"));
  EXPECT_TRUE(lg.enabled(Severity::Error, "loose"));
  EXPECT_EQ(lg.globalMinimum(), Level::Error);

  lg.setGlobalMinimum(Level::Off);
  EXPECT_FALSE(lg.enabled(Severity::Fatal, "loose"));
  EXPECT_FALSE(lg.enabled(Severity::Fatal, "anything"));
}

TEST(LogGate, DisabledEventReachesNoSink) {
  resetLogger();
  Logger::instance().setSubsystemLevel("spam", Level::Info);
  for (int i = 0; i < 100; ++i) {
    LAIGE_LOG_TRACE("spam", "spam_event", "spam message",
                    laige::log::field("i", i));
  }
  EXPECT_EQ(capture().recordCount(), 0u);
}

// ---------------------------------------------------------------------------
// Records: fields, identity, timestamp
// ---------------------------------------------------------------------------

TEST(LogRecord, FieldFormattingScalars) {
  resetLogger();
  const int x = 42;
  LAIGE_LOG_INFO("demo", "fields", "scalar fields",
                 laige::log::field("i32", 7),
                 laige::log::field("neg", -3),
                 laige::log::field("i64", INT64_MAX),
                 laige::log::field("u64", UINT64_MAX),
                 laige::log::field("d", 0.1),
                 laige::log::field("f", 1.5f),
                 laige::log::field("b", true),
                 laige::log::field("c", 'x'),
                 laige::log::field("e", Severity::Warn),
                 laige::log::field("p", &x),
                 laige::log::field("pnull", static_cast<const int*>(nullptr)));
  const std::vector<CapturedRecord> recs = capture().takeRecords();
  ASSERT_EQ(recs.size(), 1u);
  const CapturedRecord& r = recs[0];
  EXPECT_EQ(r.severity, Severity::Info);
  EXPECT_EQ(fieldAt(r, "i32"), "7");
  EXPECT_EQ(fieldAt(r, "neg"), "-3");
  EXPECT_EQ(fieldAt(r, "i64"), std::to_string(INT64_MAX));
  EXPECT_EQ(fieldAt(r, "u64"), std::to_string(UINT64_MAX));
  EXPECT_EQ(fieldAt(r, "d"), "0.1");
  EXPECT_EQ(fieldAt(r, "f"), "1.5");
  EXPECT_EQ(fieldAt(r, "b"), "true");
  EXPECT_EQ(fieldAt(r, "c"), "x");
  EXPECT_EQ(fieldAt(r, "e"), "3");  // enum renders its underlying value
  // Pointer fields are platform-rendered (%p); assert presence only.
  EXPECT_FALSE(fieldAt(r, "p").empty());
  EXPECT_FALSE(fieldAt(r, "pnull").empty());
}

TEST(LogRecord, StringFieldsCopiedInFull) {
  resetLogger();
  const std::string longValue(256, 'a');
  LAIGE_LOG_INFO("demo", "string_fields", "string-like fields",
                 laige::log::field("s", longValue),
                 laige::log::field("sv", std::string_view("literal")),
                 laige::log::field("arr", "char array"));
  const std::vector<CapturedRecord> recs = capture().takeRecords();
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(fieldAt(recs[0], "s"), longValue);
  EXPECT_EQ(fieldAt(recs[0], "sv"), "literal");
  EXPECT_EQ(fieldAt(recs[0], "arr"), "char array");
}

TEST(LogRecord, IdentityAndTimestamp) {
  resetLogger();
  const auto before = std::chrono::system_clock::now();
  LAIGE_LOG_INFO("sub", "evt", "message");
  const auto after = std::chrono::system_clock::now();
  const std::vector<CapturedRecord> recs = capture().takeRecords();
  ASSERT_EQ(recs.size(), 1u);
  const CapturedRecord& r = recs[0];
  EXPECT_EQ(r.subsystem, "sub");
  EXPECT_EQ(r.event, "evt");
  EXPECT_EQ(r.message, "message");
  EXPECT_TRUE(r.fields.empty());
  EXPECT_EQ(r.threadId, static_cast<std::uint32_t>(
                            std::hash<std::thread::id>{}(
                                std::this_thread::get_id())));
  // One documented clock (system_clock): the timestamp brackets the
  // emission instant.
  EXPECT_GE(r.timestamp, before);
  EXPECT_LE(r.timestamp, after);
}

// ---------------------------------------------------------------------------
// Sinks: line format, file sink, create failure
// ---------------------------------------------------------------------------

TEST(LogSinks, ConsoleSinkLineFormat) {
  const std::string path = tempFilePath("console");
  // Binary capture stream: this test asserts the sink's exact byte
  // output (the sink's contract is '\n'-terminated lines written to
  // the stream); a Windows text-mode stream would translate the
  // sink's '\n' to '\r\n' before the bytes reach the file.
  std::FILE* f = openLogFile(path.c_str(), "wb+");
  ASSERT_NE(f, nullptr);
  {
    laige::log::ConsoleSink sink(f);
    const laige::log::Field fields[] = {laige::log::field("connection_id", 42)};
    sink.emit(makeRecord(Severity::Warn, "network", "packet_dropped",
                         "Dropped packet outside receive window",
                         std::as_const(fields)));
    sink.emit(makeRecord(Severity::Info, "engine", "boot", "engine started",
                         std::span<const laige::log::Field>{}));
    sink.flush();
  }
  std::fclose(f);
  const std::string content = readWholeFile(path);
  removeLogFile(path.c_str());

  const std::string expected1 =
      "[warn] network/packet_dropped: Dropped packet outside receive "
      "window | connection_id=42\n";
  const std::string expected2 =
      "[info] engine/boot: engine started\n";
  const std::size_t pos2 = content.find(expected2);
  ASSERT_NE(pos2, std::string::npos) << content;
  ASSERT_EQ(content.size(), pos2 + expected2.size()) << content;
  ASSERT_TRUE(hasTimestampPrefix(content.substr(0, pos2 - 28)));
  EXPECT_EQ(content.substr(28, pos2 - 28 - 28), expected1);
}

TEST(LogSinks, ConsoleSinkDoesNotOwnStream) {
  // A ConsoleSink must leave its stream usable after destruction
  // (LOG-007: stderr must stay usable for crash diagnostics).
  const std::string path = tempFilePath("console2");
  std::FILE* f = openLogFile(path.c_str(), "w+");
  ASSERT_NE(f, nullptr);
  {
    laige::log::ConsoleSink sink(f);
    sink.emit(makeRecord(Severity::Info, "s", "e", "m",
                         std::span<const laige::log::Field>{}));
  }  // sink destroyed; stream must still be open
  const int n = std::fputc('\n', f);
  EXPECT_NE(n, EOF);
  std::fclose(f);
  removeLogFile(path.c_str());
}

TEST(LogSinks, FileSinkLineFormat) {
  const std::string path = tempFilePath("file");
  removeLogFile(path.c_str());
  const laige::Result<std::unique_ptr<laige::log::FileSink>> created =
      laige::log::FileSink::create(path);
  ASSERT_TRUE(created.ok());
  created.value()->emit(
      makeRecord(Severity::Error, "assets", "decode_failed", "bad texture",
                 std::span<const laige::log::Field>{}));
  created.value()->emit(makeRecord(Severity::Debug, "assets", "cache_hit",
                                   "hit", std::span<const laige::log::Field>{}));
  created.value()->flush();
  EXPECT_EQ(created.value()->failedWrites(), 0u);
  const std::string content = readWholeFile(path);
  removeLogFile(path.c_str());

  const std::string expected1 =
      "[error] assets/decode_failed: bad texture\n";
  const std::string expected2 = "[debug] assets/cache_hit: hit\n";
  const std::size_t pos2 = content.find(expected2);
  ASSERT_NE(pos2, std::string::npos) << content;
  ASSERT_EQ(content.size(), pos2 + expected2.size()) << content;
  ASSERT_TRUE(hasTimestampPrefix(content.substr(0, pos2 - 28)));
  EXPECT_EQ(content.substr(28, pos2 - 28 - 28), expected1);
}

TEST(LogSinks, FileSinkFlushesOnDestruction) {
  // LOG-007 safe fallback: a destroyed sink must not drop buffered
  // output even without an explicit flush().
  const std::string path = tempFilePath("destructor");
  removeLogFile(path.c_str());
  {
    const auto created = laige::log::FileSink::create(path);
    ASSERT_TRUE(created.ok());
    created.value()->emit(makeRecord(Severity::Info, "s", "e", "m",
                                     std::span<const laige::log::Field>{}));
    // no explicit flush
  }  // destructor flushes
  const std::string content = readWholeFile(path);
  removeLogFile(path.c_str());
  ASSERT_TRUE(hasTimestampPrefix(content));
  EXPECT_NE(content.find("[info] s/e: m\n"), std::string::npos) << content;
}

TEST(LogSinks, FileSinkCreateFailureReturnsIoError) {
  // LOG-007: open failure is a Status (never an exception, never
  // silent); the caller falls back to the current console sink.
  const auto r = laige::log::FileSink::create(
      std::string("/nonexistent_dir_laige_xyz/") + tempFilePath("missing"));
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::IoError);
  // The failure code maps to the registry entry (NFR-13.3 text).
  const laige::ErrorEntry& info = laige::errorInfo(r.error());
  EXPECT_STREQ(info.codeId, "io_error");
  EXPECT_STREQ(laige::errorText(r.error()), info.text);
}

// ---------------------------------------------------------------------------
// Rate limiting (LOG-004)
// ---------------------------------------------------------------------------

TEST(LogRateLimit, SuppressesRepeatsWithinWindow) {
  resetRateLogger(std::chrono::seconds(1));
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
  for (int i = 0; i < 5; ++i) {
    gFakeNow += std::chrono::milliseconds(100);
    LAIGE_LOG_WARN("net", "drop", "dropped", laige::log::field("i", i));
  }
  const std::vector<CapturedRecord> recs = capture().takeRecords();
  ASSERT_EQ(recs.size(), 1u);  // first only; the rest suppressed
  EXPECT_EQ(recs[0].event, "drop");
}

TEST(LogRateLimit, SummaryEmittedAfterWindow) {
  resetRateLogger(std::chrono::seconds(1));
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
  // One emitted, two suppressed within the window.
  for (int i = 0; i < 3; ++i) {
    gFakeNow += std::chrono::milliseconds(200);
    LAIGE_LOG_WARN("net", "drop", "dropped", laige::log::field("i", i));
  }
  // After the window: the summary reports the suppressed repeats, then
  // the current event is recorded. (Records: first event, summary,
  // current event.)
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000)) +
             std::chrono::milliseconds(1500);
  LAIGE_LOG_WARN("net", "drop", "dropped", laige::log::field("i", 3));

  const std::vector<CapturedRecord> recs = capture().takeRecords();
  ASSERT_EQ(recs.size(), 3u);
  EXPECT_EQ(recs[0].event, "drop");
  const CapturedRecord& summary = recs[1];
  EXPECT_EQ(summary.severity, Severity::Warn);
  EXPECT_EQ(summary.subsystem, "net");
  EXPECT_EQ(summary.event, laige::log::kRateLimitedEvent);
  EXPECT_EQ(fieldAt(summary, "suppressed"), "2");
  EXPECT_EQ(fieldAt(summary, "event"), "drop");
  EXPECT_NE(
      summary.message.find("suppressed 2 repeats of 'net/drop'"),
      std::string::npos)
      << summary.message;
  const CapturedRecord& evt = recs[2];
  EXPECT_EQ(evt.event, "drop");
}

TEST(LogRateLimit, WindowEdgeEmitsWithoutSummary) {
  resetRateLogger(std::chrono::seconds(1));
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
  LAIGE_LOG_WARN("net", "drop", "dropped", laige::log::field("i", 0));
  gFakeNow += std::chrono::seconds(1);  // exactly one window later
  LAIGE_LOG_WARN("net", "drop", "dropped", laige::log::field("i", 1));
  const std::vector<CapturedRecord> recs = capture().takeRecords();
  ASSERT_EQ(recs.size(), 2u);
  EXPECT_EQ(recs[0].event, "drop");
  EXPECT_EQ(recs[1].event, "drop");  // no rate_limited summary in between
}

TEST(LogRateLimit, KeysAreIndependent) {
  resetRateLogger(std::chrono::seconds(1));
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
  LAIGE_LOG_WARN("a", "x", "m", laige::log::field("i", 0));  // emitted
  LAIGE_LOG_WARN("a", "x", "m", laige::log::field("i", 1));  // suppressed
  LAIGE_LOG_WARN("b", "x", "m", laige::log::field("i", 0));  // other sub
  LAIGE_LOG_WARN("a", "y", "m", laige::log::field("i", 0));  // other event
  EXPECT_EQ(capture().recordCount(), 3u);
}

TEST(LogRateLimit, OnlyFailuresAreRateLimited) {
  resetRateLogger(std::chrono::seconds(1));
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
  for (int i = 0; i < 5; ++i) {
    gFakeNow += std::chrono::milliseconds(100);
    LAIGE_LOG_INFO("n", "i", "m", laige::log::field("i", i));
  }
  for (int i = 0; i < 5; ++i) {
    gFakeNow += std::chrono::milliseconds(100);
    LAIGE_LOG_DEBUG("n", "d", "m", laige::log::field("i", i));
  }
  EXPECT_EQ(capture().recordCount(), 10u);  // none suppressed
}

TEST(LogRateLimit, CanBeDisabled) {
  laige::log::LoggerOptions o;
  o.clock = &fakeClock;
  o.rateWindow = std::chrono::seconds(1);
  o.rateLimiting = false;
  resetLogger(std::move(o));
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
  for (int i = 0; i < 5; ++i) {
    gFakeNow += std::chrono::milliseconds(100);
    LAIGE_LOG_WARN("n", "w", "m", laige::log::field("i", i));
  }
  EXPECT_EQ(capture().recordCount(), 5u);
}

TEST(LogRateLimit, ShutdownDrainsPendingSummaries) {
  resetRateLogger(std::chrono::seconds(1));
  gFakeNow = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
  for (int i = 0; i < 3; ++i) {
    gFakeNow += std::chrono::milliseconds(100);
    LAIGE_LOG_ERROR("db", "query_failed", "query failed",
                    laige::log::field("i", i));
  }
  Logger::instance().shutdown();
  const std::vector<CapturedRecord> recs = capture().takeRecords();
  ASSERT_GE(recs.size(), 2u);
  const CapturedRecord& summary = recs.back();
  EXPECT_EQ(summary.event, laige::log::kRateLimitedEvent);
  EXPECT_EQ(fieldAt(summary, "suppressed"), "2");
  EXPECT_EQ(fieldAt(summary, "event"), "query_failed");

  // Shutdown is idempotent (CONC-006): a second call adds nothing.
  const std::size_t n = capture().recordCount();
  Logger::instance().shutdown();
  EXPECT_EQ(capture().recordCount(), n);
}

// ---------------------------------------------------------------------------
// Fatal severity and crash/shutdown flushing (LOG-007)
// ---------------------------------------------------------------------------

TEST(LogFatal, Gate) {
  resetLogger();
  EXPECT_TRUE(Logger::instance().enabled(Severity::Fatal, "s"));
  Logger::instance().setGlobalMinimum(Level::Off);
  EXPECT_FALSE(Logger::instance().enabled(Severity::Fatal, "s"));
}

TEST(LogFatal, ChildEmitsFlushesAndTerminates) {
// AGENTS §14: Fatal records the event, flushes, and terminates the
// process. Exercised in a forked child so the test process survives:
// the child must die on SIGABRT with the fatal line flushed to the
// file sink. POSIX only (fork); the Windows jobs skip with a reason.
#if defined(__unix__)
  const std::string path = tempFilePath("fatal");
  removeLogFile(path.c_str());
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: fresh file sink, one Fatal event. Never returns — Fatal
    // terminates the process via std::abort().
    auto created = laige::log::FileSink::create(path);
    if (!created.ok()) _exit(117);
    laige::log::LoggerOptions o;
    o.sink = std::move(created).takeValue();
    if (!Logger::instance().init(std::move(o)).ok()) _exit(119);
    LAIGE_LOG_FATAL("crash_test", "fatal_child",
                    "deliberate fatal event from the test child");
    _exit(118);  // unreachable
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
      << "child should die on SIGABRT (controlled termination after "
         "emit + flush)";
  const std::string content = readWholeFile(path);
  removeLogFile(path.c_str());
  EXPECT_NE(content.find("[fatal] crash_test/fatal_child"), std::string::npos)
      << content;
#else
  GTEST_SKIP() << "fork() is not available on Windows; the Fatal "
                 "termination contract is exercised on the POSIX jobs.";
#endif
}

TEST(LogCrash, InstallCrashHandlingIsIdempotent) {
  resetLogger();
  EXPECT_TRUE(Logger::instance().installCrashHandling().ok());
  EXPECT_TRUE(Logger::instance().installCrashHandling().ok());
}

TEST(LogCrash, ShutdownFlushesSink) {
  resetLogger();
  LAIGE_LOG_INFO("s", "e", "m");
  const std::size_t flushesBefore = capture().flushCount();
  Logger::instance().shutdown();
  EXPECT_GT(capture().flushCount(), flushesBefore);
}

TEST(LogCrash, PostShutdownLogsAreDiscarded) {
  resetLogger();
  Logger::instance().shutdown();
  LAIGE_LOG_INFO("s", "e", "m", laige::log::field("x", 1));
  EXPECT_EQ(capture().recordCount(), 0u);
  EXPECT_FALSE(Logger::instance().enabled(Severity::Info, "s"));
}

// ---------------------------------------------------------------------------
// Concurrency (TSan coverage comes from the CI tsan lane)
// ---------------------------------------------------------------------------

TEST(LogConcurrency, ConcurrentEmitsAreAllRecorded) {
  resetLogger();
  constexpr int kThreads = 4;
  constexpr int kPerThread = 250;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t]() {
      const std::string sub = "t" + std::to_string(t);
      for (int i = 0; i < kPerThread; ++i) {
        LAIGE_LOG_DEBUG(sub.c_str(), "ev", "m", laige::log::field("i", i));
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(capture().recordCount(),
            static_cast<std::size_t>(kThreads * kPerThread));
}

// ---------------------------------------------------------------------------
// Performance: LOG-003 (no work when disabled) + timing property
// ---------------------------------------------------------------------------

TEST(LogPerformance, DisabledTraceSpamAllocatesNothing) {
  resetLogger();  // also constructs the capture() static before the reset
  Logger::instance().setSubsystemLevel("spam", Level::Info);
#if defined(LAIGE_ALLOC_COUNTER)
  laige::test::resetAllocCounter();
#endif
  for (int i = 0; i < 100000; ++i) {
    LAIGE_LOG_TRACE("spam", "trace_spam", "spam message",
                    laige::log::field("i", i));
  }
  EXPECT_EQ(capture().recordCount(), 0u);
#if defined(LAIGE_ALLOC_COUNTER)
  // The roadmap's Verify property: trace-level spam in a disabled
  // subsystem shows zero allocations. (M0-CORE-05's pool accounting is
  // the future refinement; the sanitizer trees run this same spam loop
  // leak-free instead of counting — see tests/laige-core/CMakeLists.txt.)
  EXPECT_EQ(laige::test::allocCounter(), 0u);
#endif
}

TEST(LogPerformance, DisabledPathCheaperThanEnabled) {
  resetLogger();
  Logger::instance().setGlobalMinimum(Level::Trace);
  Logger::instance().setSubsystemLevel("off", Level::Info);  // gate 2 rejects
  Logger::instance().setSubsystemLevel("on", Level::Trace);  // gate 2 passes
  constexpr int N = 200000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) {
    LAIGE_LOG_TRACE("off", "ev", "m", laige::log::field("i", i));
  }
  const auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) {
    LAIGE_LOG_TRACE("on", "ev", "m", laige::log::field("i", i));
  }
  const auto t2 = std::chrono::steady_clock::now();
  const long long disabledNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const long long enabledNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
  std::printf("LogPerformance: disabled=%lld ns/event, enabled=%lld "
              "ns/event (N=%d)\n",
              disabledNs / N, enabledNs / N, N);
  // Property: the disabled path (gate + branch, no sink, no allocation)
  // is strictly cheaper than the enabled path (sink write + field
  // construction).
  EXPECT_LT(disabledNs, enabledNs);
}
