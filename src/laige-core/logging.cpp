// laige-core logging facade implementation (M0-CORE-02).
//
// File-scope design notes:
//   - Timestamps use one documented clock and format (AGENTS §14):
//     std::chrono::system_clock rendered in UTC as RFC 3339
//     "YYYY-MM-DDTHH:MM:SS.ffffffZ". The days→civil-date conversion is
//     Howard Hinnant's calendar-date algorithm (C++ calendar-date
//     proposal P0437R1, public domain; also published on cppreference)
//     — no libc date functions, hence thread-safe, allocation-free,
//     and identical on every P0 platform (no platform-specific date
//     behavior can leak into log lines).
//   - Line format (both sinks):
//       <timestamp> [<severity>] <subsystem>/<event>: <message>
//       [ | <key>=<value>]*
//     Field values are emitted verbatim; callers should keep them free
//     of spaces and '=' (machine-parseable keys, LOG-001).
//   - Write failures are counted (failedWrites) and never silent
//     (CORE-008); a sink that fails to open returns Status(IoError)
//     before it exists (LOG-007 minimal fallback: keep the console
//     sink).
//   - Crash handling (LOG-007): POSIX sigaction for SEGV/ABRT/BUS/FPE/
//     ILL with SA_RESETHAND (one-shot, then the default disposition),
//     a raw write(2) notice (async-signal-safe: no stdio lock, no
//     allocation), a sink flush (try_lock per the Sink contract), and
//     a re-raise so normal crash diagnostics (core dump, debugger,
//     crash reporter) proceed unchanged. Windows uses a vectored SEH
//     filter with the same flush and EXCEPTION_CONTINUE_EXECUTION.

#include "laige/logging.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for the _fsopen below
#endif

namespace laige::log {

namespace {

// Howard Hinnant's calendar-date algorithm (P0437R1 civil_from_days,
// public domain; see also cppreference, "How to determine the calendar
// date from the number of days since the epoch"): days since 1970-01-01
// → proleptic Gregorian date. No libc date functions: thread-safe, no
// allocation, identical results on every P0 platform.
struct Civil {
  int year;
  std::uint16_t month;  // 1-12
  std::uint16_t day;    // 1-31
};

Civil civilFromDays(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::uint64_t doe =
      static_cast<std::uint64_t>(z - era * 146097);  // [0, 146096]
  const std::uint64_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const std::uint64_t doy =
      doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
  const std::uint64_t mp = (5 * doy + 2) / 153;  // [0, 11]
  const std::uint64_t day = doy - (153 * mp + 2) / 5 + 1;  // [1, 31]
  const std::uint64_t month = mp < 10 ? mp + 3 : mp - 9;  // [1, 12]
  return Civil{year + (month <= 2 ? 1 : 0),
               static_cast<std::uint16_t>(month),
               static_cast<std::uint16_t>(day)};
}

// Render the record's timestamp in the documented format into `out`
// (27 chars + NUL; `out` must be at least 64 bytes, see the comment at
// the snprintf).
void formatTimestamp(std::chrono::system_clock::time_point tp, char* out) {
  const std::int64_t wholeSeconds =
      std::chrono::time_point_cast<std::chrono::seconds>(tp)
          .time_since_epoch()
          .count();
  std::int64_t days = wholeSeconds / 86400;
  std::int64_t secondOfDay = wholeSeconds % 86400;
  if (secondOfDay < 0) {
    // division truncates toward zero; normalize to days + [0, 86400)
    secondOfDay += 86400;
    --days;
  }
  const std::int64_t micros =
      std::chrono::duration_cast<std::chrono::microseconds>(
          tp.time_since_epoch())
          .count() %
      1000000;
  const unsigned frac =
      static_cast<unsigned>(micros < 0 ? micros + 1000000 : micros);
  const Civil c = civilFromDays(days);
  const unsigned h = static_cast<unsigned>(secondOfDay / 3600);
  const unsigned m = static_cast<unsigned>((secondOfDay % 3600) / 60);
  const unsigned s = static_cast<unsigned>(secondOfDay % 60);
  // 64 B: the documented form is 27 chars, and the buffer also satisfies
  // the compiler's worst-case width analysis for the %d year (CORE-010:
  // no -Wformat-truncation under -Werror).
  std::snprintf(out, 64, "%04d-%02u-%02uT%02u:%02u:%02u.%06uZ", c.year,
                static_cast<unsigned>(c.month), static_cast<unsigned>(c.day),
                h, m, s, frac);
}

// Portable file open (CPP-009 compile-time platform boundary).
//
// MSVC's CRT deprecates plain `fopen` (C4996, an error under the
// engine's /WX policy). The secure variant `fopen_s` cannot be used
// here: it opens with the `_SH_SECURE` sharing mode, which denies
// *all* sharing for write access (the UCRT maps `_SH_SECURE` to
// share=0 unless the access is read-only). A file sink opened that
// way cannot even be re-opened read-only by the same process while it
// holds the file — the windows-msvc CI runs of M0-CORE-02 read back an
// empty file for exactly that reason. `_fsopen(path, mode,
// _SH_DENYNO)` is the CRT's documented way to open with plain-`fopen`
// sharing semantics (concurrent read/write sharing allowed), which is
// the behavior every other supported compiler's `fopen` provides.
#if defined(_MSC_VER)
std::FILE* openFile(const char* path, const char* mode) {
  return ::_fsopen(path, mode, _SH_DENYNO);
}
#else
std::FILE* openFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif

// Render one full line into `stream` (see the file header for the
// format). Returns 0 on success, -1 if any write failed.
int writeLine(std::FILE* stream, const LogRecord& r) {
  char ts[64];
  formatTimestamp(r.timestamp, ts);
  int written = std::fprintf(
      stream, "%s [%s] %.*s/%.*s: %.*s", ts, severityName(r.severity),
      static_cast<int>(r.subsystem.size()), r.subsystem.data(),
      static_cast<int>(r.event.size()), r.event.data(),
      static_cast<int>(r.message.size()), r.message.data());
  if (written < 0) return -1;
  for (const Field& f : r.fields) {
    const int n = std::fprintf(stream, " | %.*s=%s",
                               static_cast<int>(f.name.size()),
                               f.name.data(), f.value.c_str());
    if (n < 0) return -1;
  }
  return (std::fputc('\n', stream) == EOF) ? -1 : 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// detail::formatScalar — the non-template overloads (see logging.h)
// ---------------------------------------------------------------------------

namespace detail {

int formatScalar(char* first, std::size_t size, bool value) {
  const char* s = value ? "true" : "false";
  const std::size_t n = std::strlen(s);
  if (n > size) return -1;
  std::memcpy(first, s, n);
  return static_cast<int>(n);
}

int formatScalar(char* first, std::size_t size, char value) {
  if (size == 0) return -1;
  first[0] = value;
  return 1;
}

int formatScalar(char* first, std::size_t size, const char* value) {
  const std::size_t n = (value != nullptr) ? std::strlen(value) : 0;
  if (n > size) return -1;
  if (n > 0) std::memcpy(first, value, n);
  return static_cast<int>(n);
}

int formatScalar(char* first, std::size_t size, const void* value) {
  const int n = std::snprintf(first, size, "%p", value);
  return (n < 0 || n >= static_cast<int>(size)) ? -1 : n;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------

void ConsoleSink::emit(const LogRecord& record) {
  std::lock_guard lk(mutex_);
  if (writeLine(stream_, record) == -1) ++failedWrites_;
}

void ConsoleSink::flush() {
  // try_lock: a flush from a crash handler must not block on a lock the
  // interrupted emit() may still hold (LOG-007).
  if (mutex_.try_lock()) {
    if (std::fflush(stream_) != 0) ++failedWrites_;
    mutex_.unlock();
  }
}

// ---------------------------------------------------------------------------
// FileSink
// ---------------------------------------------------------------------------

laige::Result<std::unique_ptr<FileSink>> FileSink::create(std::string path) {
  // Binary append mode: the on-disk format is platform-stable — every
  // line ends with a single '\n' (LF) on every platform, never
  // translated to '\r\n' by Windows text mode (the machine-searchable
  // format of LOG-001; see docs/api/logging.md, "Line format").
  std::FILE* stream = openFile(path.c_str(), "ab");
  if (stream == nullptr) {
    // LOG-007 minimal fallback: the caller keeps its current sink
    // (console) and reports the failure; the Status carries the
    // NFR-13.3 registry text (docs/api/errors.md#io-error).
    return laige::Result<std::unique_ptr<FileSink>>::failure(
        laige::ErrorCode::IoError);
  }
  // The factory is the sole creation path and encapsulates the one
  // owning new (std::make_unique cannot call the private constructor).
  return std::unique_ptr<FileSink>(new FileSink(std::move(path), stream));
}

void FileSink::emit(const LogRecord& record) {
  std::lock_guard lk(mutex_);
  if (writeLine(stream_.get(), record) == -1) ++failedWrites_;
}

void FileSink::flush() {
  // try_lock: same crash-handler constraint as ConsoleSink::flush().
  if (mutex_.try_lock()) {
    if (std::fflush(stream_.get()) != 0) ++failedWrites_;
    mutex_.unlock();
  }
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::Logger()
    : globalMinimum_(static_cast<std::uint8_t>(Level::Debug)),
      sink_(std::make_unique<ConsoleSink>(stderr)) {}

laige::Status Logger::init(LoggerOptions options) {
  std::unique_ptr<Sink> sink =
      (options.sink != nullptr) ? std::move(options.sink)
                                : std::make_unique<ConsoleSink>(stderr);
  {
    std::lock_guard lk(stateMutex_);
    if (sink_ != nullptr) sink_->flush();  // replaced sink flushes first
    sink_ = std::move(sink);
    globalMinimum_.store(static_cast<std::uint8_t>(options.globalMinimum),
                         std::memory_order_relaxed);
    defaultSubsystemLevel_ = options.defaultSubsystemLevel;
    rateLimiting_ = options.rateLimiting;
    rateWindow_ = options.rateWindow;
    clock_ = options.clock;
    subsystems_.clear();
    rates_.clear();
    crashHandlingInstalled_ = false;
  }
  retired_.store(false, std::memory_order_relaxed);
  return {};
}

void Logger::setSubsystemLevel(std::string_view subsystem, Level level) {
  std::lock_guard lk(stateMutex_);
  for (SubsystemEntry& e : subsystems_) {
    if (e.name == subsystem) {
      e.level = level;
      return;
    }
  }
  subsystems_.push_back(SubsystemEntry{std::string(subsystem), level});
}

Level Logger::subsystemLevel(std::string_view subsystem) const {
  std::lock_guard lk(stateMutex_);
  for (const SubsystemEntry& e : subsystems_) {
    if (e.name == subsystem) return e.level;
  }
  return defaultSubsystemLevel_;
}

void Logger::setGlobalMinimum(Level level) {
  globalMinimum_.store(static_cast<std::uint8_t>(level),
                       std::memory_order_relaxed);
}

Level Logger::globalMinimum() const noexcept {
  return static_cast<Level>(globalMinimum_.load(std::memory_order_relaxed));
}

bool Logger::enabled(Severity severity, std::string_view subsystem) const {
  if (retired_.load(std::memory_order_relaxed)) return false;
  // Gate 1: the global minimum (one atomic load — the cheap reject for
  // Trace under the default configuration).
  if (static_cast<std::uint8_t>(severity) <
      globalMinimum_.load(std::memory_order_relaxed)) {
    return false;
  }
  // Gate 2: the per-subsystem level (small linear scan; N is the number
  // of registered subsystems, expected to be well under a few dozen).
  std::lock_guard lk(stateMutex_);
  for (const SubsystemEntry& e : subsystems_) {
    if (e.name == subsystem) {
      return static_cast<std::uint8_t>(severity) >=
             static_cast<std::uint8_t>(e.level);
    }
  }
  return static_cast<std::uint8_t>(severity) >=
         static_cast<std::uint8_t>(defaultSubsystemLevel_);
}

void Logger::record(Severity severity, std::string_view subsystem,
                    std::string_view event, std::string_view message,
                    std::span<const Field> fields) {
  if (retired_.load(std::memory_order_relaxed)) return;
  const auto now = (clock_ != nullptr)
                       ? clock_()
                       : std::chrono::system_clock::now();
  const std::uint32_t threadId = static_cast<std::uint32_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
  const LogRecord record{now, severity, subsystem, event, message, fields,
                         threadId};
  dispatch(record);
}

void Logger::dispatch(const LogRecord& record) {
  // The rate decision runs under the state mutex; the sink is called
  // only after the lock is released (lock ordering: state → sink).
  // `summary` is a copy: the lock release can let another thread
  // reallocate rates_, so no pointer into it survives the section.
  RateEntry summary;
  bool hasSummary = false;
  {
    std::lock_guard lk(stateMutex_);
    if (rateLimiting_ && rateLimited(record.severity)) {
      RateEntry* e = findRate(record.subsystem, record.event,
                              record.severity);
      if (e == nullptr) {
        e = &rates_.emplace_back(
            RateEntry{std::string(record.subsystem), std::string(record.event),
                      record.severity, false, {}, 0});
      }
      // everEmitted short-circuits before the window subtraction (no
      // sentinel-value overflow; CPP-004).
      if (!e->everEmitted || record.timestamp - e->lastEmit >= rateWindow_) {
        // Window elapsed (or first event of the key): record this event,
        // and report the suppressed repeats of the just-ended window.
        if (e->suppressed > 0) {
          summary = *e;  // copies the strings; summary.suppressed = count
          e->suppressed = 0;
          hasSummary = true;
        }
        e->everEmitted = true;
        e->lastEmit = record.timestamp;
      } else {
        ++e->suppressed;  // LOG-004: counted; reported at window end
        return;
      }
    }
  }
  if (hasSummary) {
    emitRateSummary(summary);
  }
  sink_->emit(record);
  if (record.severity == Severity::Fatal) {
    // AGENTS §14: Fatal initiates controlled termination after
    // preserving useful diagnostics (emit above, flush here). std::abort
    // raises SIGABRT, which the crash handler (if installed) flushes
    // once more (idempotent) before the default handling takes over.
    sink_->flush();
    std::abort();
  }
}

bool Logger::rateLimited(Severity severity) const noexcept {
  // LOG-004 scopes rate limiting to repeated failures.
  return severity == Severity::Warn || severity == Severity::Error ||
         severity == Severity::Fatal;
}

Logger::RateEntry* Logger::findRate(std::string_view subsystem,
                            std::string_view event, Severity severity) {
  for (RateEntry& e : rates_) {
    if (e.severity == severity && e.subsystem == subsystem && e.event == event) {
      return &e;
    }
  }
  return nullptr;
}

void Logger::emitRateSummary(const Logger::RateEntry& entry) {
  // A dedicated, stable summary event (kRateLimitedEvent, LOG-001) with
  // the original event name and the suppressed count (entry.suppressed)
  // as fields, so rate-limited failures stay machine-searchable. Runs
  // after the state lock is released; called from dispatch() (window
  // rollover) and shutdown() (pending drain).
  const std::uint64_t suppressed = entry.suppressed;
  char countBuf[32];
  const auto [ptr, ec] =
      std::to_chars(countBuf, countBuf + sizeof(countBuf), suppressed);
  const std::size_t n = (ec == std::errc{}) ? static_cast<std::size_t>(ptr - countBuf)
                                            : 0;
  const std::string count(countBuf, n);
  const std::string message = "suppressed " + count + " repeats of '" +
                              entry.subsystem + "/" + entry.event + "'";
  std::array<Field, 2> fields{
      Field{"event", entry.event},
      Field{"suppressed", count},
  };
  const auto now = (clock_ != nullptr)
                       ? clock_()
                       : std::chrono::system_clock::now();
  const LogRecord summary{now, entry.severity, entry.subsystem,
                          kRateLimitedEvent, message, std::as_const(fields),
                          static_cast<std::uint32_t>(
                              std::hash<std::thread::id>{}(
                                  std::this_thread::get_id()))};
  sink_->emit(summary);
}

void Logger::flush() {
  if (sink_ != nullptr) sink_->flush();
}

// TEMPORARY CI DIAGNOSTIC (delete before merge): the allocation-probe
// hook (see the header). Null by default; a test installs a probe that
// prints the running allocation counter, so the Windows-only
// allocation inside shutdown() can be bisected per statement.
namespace {
Logger::AllocProbeFn gAllocProbe = nullptr;
}  // namespace

void Logger::setAllocProbe(AllocProbeFn fn) { gAllocProbe = fn; }

static void probeAlloc() {
  if (gAllocProbe != nullptr) gAllocProbe();
}

void Logger::shutdown() {
  probeAlloc();  // SHUT-1: entry
  std::vector<RateEntry> pending;
  probeAlloc();  // SHUT-2: after the pending vector declaration
  // TEMPORARY round-3 discriminators (delete before merge): empty
  // vectors of other element types, and a second vector<RateEntry>,
  // to localize the Windows-only allocation (vector-specific?
  // element-type-specific? first-of-kind in the process?).
  std::vector<int> probeIntVec;
  probeAlloc();  // SHUT-3: after the empty vector<int>
  std::vector<std::string> probeStrVec;
  probeAlloc();  // SHUT-4: after the empty vector<string>
  std::vector<RateEntry> probeRateVec2;
  probeAlloc();  // SHUT-5: after the second vector<RateEntry>
  (void)probeIntVec;  // TEMPORARY round-3 discriminators (not used)
  (void)probeStrVec;
  (void)probeRateVec2;
  {
    std::lock_guard lk(stateMutex_);
    probeAlloc();  // SHUT-6: after the lock acquire
    retired_.store(true, std::memory_order_relaxed);
    probeAlloc();  // SHUT-7: after the retired store
    for (const RateEntry& e : rates_) {
      if (e.suppressed > 0) pending.push_back(e);  // copy: state is cleared
    }
    probeAlloc();  // SHUT-8: after the rate-state drain
    rates_.clear();
    probeAlloc();  // SHUT-9: after the clear
  }
  probeAlloc();  // SHUT-10: after the lock release
  for (const RateEntry& e : pending) {
    emitRateSummary(e);
  }
  probeAlloc();  // SHUT-11: after the pending summaries
  if (sink_ != nullptr) sink_->flush();
  probeAlloc();  // SHUT-12: after the sink flush
}

// ---------------------------------------------------------------------------
// Crash handling (LOG-007)
// ---------------------------------------------------------------------------

#if defined(_WIN32)

namespace {

// Vectored SEH filter: flush the sink, then continue to the default SEH
// handling (the OS error dialog / debugger / abort path).
LONG WINAPI crashFilter(EXCEPTION_POINTERS*) {
  Logger::instance().crashFlush();
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

laige::Status Logger::installCrashHandling() {
  std::lock_guard lk(stateMutex_);
  if (crashHandlingInstalled_) return {};  // idempotent
  if (AddVectoredExceptionHandler(1 /*FIRST*/, crashFilter) == 0) {
    // Registration failed: report it (CORE-008), leave state unchanged.
    return laige::Status::failure(laige::ErrorCode::IoError);
  }
  crashHandlingInstalled_ = true;
  return {};
}

#else  // POSIX

namespace {

// LOG-007: preserve output on crash. The notice goes out with a raw
// write(2) — no stdio lock, no allocation, no locale — so it is
// async-signal-safe. After flushing, re-raise the signal so the default
// disposition (core dump / debugger / abort) continues unchanged.
void crashHandler(int signum, siginfo_t*, void*) {
  static const char msg[] = "laige: fatal signal received; flushing logs\n";
  (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
  Logger::instance().crashFlush();
  raise(signum);
}

}  // namespace

laige::Status Logger::installCrashHandling() {
  std::lock_guard lk(stateMutex_);
  if (crashHandlingInstalled_) return {};  // idempotent
  static const int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE,
                                      SIGILL};
  struct sigaction sa{};
  sa.sa_sigaction = &crashHandler;
  sa.sa_flags = SA_RESETHAND;  // one-shot: default disposition resumes
  sigemptyset(&sa.sa_mask);
  for (const int signum : kCrashSignals) {
    if (sigaction(signum, &sa, nullptr) != 0) {
      return laige::Status::failure(laige::ErrorCode::IoError);
    }
  }
  crashHandlingInstalled_ = true;
  return {};
}

#endif  // _WIN32

}  // namespace laige::log
