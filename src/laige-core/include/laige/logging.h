// laige-core structured logging facade (M0-CORE-02).
//
// AGENTS.md §14 (LOG-001…LOG-007), FR-12.2. This is the ONE logging
// facade: all engine logging goes through it, and the backend (Sink)
// is replaceable.
//
// Design summary (the full contract is in docs/api/logging.md):
//   - Events carry a severity (Trace…Fatal per AGENTS §14), a stable
//     subsystem name, a stable event name, a concise message,
//     structured fields, and a thread identity (LOG-001/LOG-002).
//   - Level gating is a global minimum (one atomic load) plus a
//     per-subsystem level table. The LAIGE_LOG_* macros put the gate
//     BEFORE argument evaluation, so a disabled event costs exactly
//     that check: no message/field evaluation, no formatting, no
//     allocation, no lock (LOG-003, PERF-003).
//   - Repeated failures (Warn/Error/Fatal) are rate-limited per
//     (subsystem, event, severity); suppressed repeats are counted and
//     reported through a dedicated `rate_limited` summary event
//     (LOG-004).
//   - Controlled shutdown (shutdown()) and crash handling
//     (installCrashHandling()) preserve buffered output (LOG-007).
//     Sink creation failure returns Status and the fallback is
//     documented (FileSink::create).
//
// Ownership/lifetime:
//   - Logger::instance() is a process-lifetime Meyers singleton
//     (thread-safe construction, C++11 [stmt.dcl]). It owns the
//     current sink (unique_ptr); a sink passed via LoggerOptions is
//     moved into it.
//   - LogRecord's string_views (subsystem/event/message) and its field
//     span are non-owning: they must outlive the Sink::emit() call.
//     The LAIGE_LOG_* macros guarantee this; a direct emit() call must
//     guarantee it as well.
//
// Threading (CONC-001/002):
//   - Recording enabled events is safe from any thread: level/rate
//     state is serialized on one facade mutex, each sink serializes
//     its own output.
//   - init(), setSubsystemLevel(), setGlobalMinimum(), and
//     installCrashHandling() are init-phase operations: they MUST NOT
//     run concurrently with logging or with each other from other
//     threads (mutation in an explicit phase).
//   - Lock ordering: the facade state mutex is never held while a sink
//     is called, and sinks never call back into the facade.
//
// Performance (DOC-004; measured by the LogPerformance suite):
//   - Disabled event: one atomic load + one branch. No allocation, no
//     formatting, no lock.
//   - Enabled event: state-mutex section (level lookup, rate decision),
//     then one sink write. Allocations: Field value strings (only when
//     fields are given), one rate-state entry per distinct
//     (subsystem, event, severity) key, one summary record per rate
//     window rollover.
//   - FR-12.2: no logging in sim/render hot paths by default — Trace is
//     off by default and hot-path diagnostics belong to Trace/Debug
//     behind the level gate.

#pragma once

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "laige/result.h"

namespace laige::log {

// ---------------------------------------------------------------------------
// Severity and level (AGENTS §14 severity contract)
// ---------------------------------------------------------------------------

// Event severity. Contract per level (AGENTS.md §14):
//   Trace  very high-volume diagnostic detail; disabled by default
//   Debug  developer-facing state
//   Info   low-volume lifecycle / significant state transitions
//   Warn   degraded behavior the engine recovered from
//   Error  an operation or subsystem failed
//   Fatal  continued execution is unsafe: the facade records the event,
//          flushes, and terminates the process (std::abort) — controlled
//          termination after preserving diagnostics
enum class Severity : std::uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Fatal = 5,
};

// Minimum-severity filter, used either logger-wide (global minimum) or
// for one subsystem (per-subsystem scope, FR-12.2). An event is
// recorded only when severity >= the applicable level. Off disables
// everything (the cheap switch for release/server profiles).
enum class Level : std::uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Fatal = 5,
  Off = 6,
};

// The stable lowercase token for a severity, as rendered in log lines.
// O(1), no allocation, thread-safe.
[[nodiscard]] inline const char* severityName(Severity severity) noexcept {
  switch (severity) {
    case Severity::Trace: return "trace";
    case Severity::Debug: return "debug";
    case Severity::Info: return "info";
    case Severity::Warn: return "warn";
    case Severity::Error: return "error";
    case Severity::Fatal: return "fatal";
  }
  return "unknown";  // unreachable: the enum domain is exhaustive
}

// The stable event name of the rate-limit summary (LOG-001: machine
// searchable). A summary reports suppressed repeats of another event.
inline constexpr const char* kRateLimitedEvent = "rate_limited";

// ---------------------------------------------------------------------------
// Structured fields (LOG-001)
// ---------------------------------------------------------------------------

// One structured key/value pair of a log event.
//
// `name` is a non-owning view into caller storage (a literal in
// practice; keys must be stable and machine-searchable, LOG-001).
// `value` is owned and is constructed only when the event is actually
// recorded — the LAIGE_LOG_* macros guarantee that disabled events
// never construct their fields (LOG-003).
struct Field {
  std::string_view name;
  std::string value;
};

namespace detail {

// The stack buffer field() renders scalar values into (CORE-005):
// 64 B holds int64 (-20 digits), float/double shortest round-trip
// forms (<= 25), hex pointers (<= 18), and bools with margin.
inline constexpr std::size_t kFieldBufferBytes = 64;

// The overloads below render one scalar value into [first, first + n)
// and return the byte count n, or -1 when the value does not fit.
// They run only on the enabled path (called from field()).
int formatScalar(char* first, std::size_t size, bool value);
int formatScalar(char* first, std::size_t size, char value);
int formatScalar(char* first, std::size_t size, const char* value);
int formatScalar(char* first, std::size_t size, const void* value);

template <typename T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool> &&
           !std::is_same_v<T, char>)
int formatScalar(char* first, std::size_t size, T value) {
  const auto [ptr, ec] = std::to_chars(first, first + size, value);
  return (ec == std::errc{}) ? static_cast<int>(ptr - first) : -1;
}

template <typename T>
  requires(std::is_floating_point_v<T>)
int formatScalar(char* first, std::size_t size, T value) {
  // Shortest round-trip decimal (std::chars_format::general default):
  // locale-independent and bit-stable, matching the engine's
  // deterministic numeric policy (ADR 0002 scope).
  const auto [ptr, ec] = std::to_chars(first, first + size, value);
  return (ec == std::errc{}) ? static_cast<int>(ptr - first) : -1;
}

template <typename T>
  requires(std::is_enum_v<T>)
int formatScalar(char* first, std::size_t size, T value) {
  return formatScalar(first, size,
                      static_cast<std::underlying_type_t<T>>(value));
}

}  // namespace detail

// Build a log field from a scalar value (see Field).
//
// MISUSE WARNING: constructing a Field allocates (the value string).
// Call it only inside a LAIGE_LOG_* macro (or where you know the event
// will be recorded) — a disabled event must not construct fields
// (LOG-003).
//
// String-like values (std::string, std::string_view, const char*, char
// arrays) are copied in full (no truncation). Other scalars (integral,
// floating-point, bool, char, enum, pointer) are rendered
// locale-free into a 64-byte stack buffer (no allocation beyond the
// Field's value string).
template <typename T>
Field field(std::string_view name, const T& value) {
  using D = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<D, std::string> ||
                std::is_same_v<D, std::string_view> ||
                std::is_same_v<D, const char*> ||
                (std::is_array_v<D> &&
                 std::is_same_v<std::remove_extent_t<D>, char>)) {
    return Field{name, std::string(value)};
  } else {
    char buf[detail::kFieldBufferBytes];
    const int n = detail::formatScalar(buf, sizeof(buf), value);
    return Field{name,
                 std::string(buf, static_cast<std::size_t>(n < 0 ? 0 : n))};
  }
}

// ---------------------------------------------------------------------------
// Records and sinks
// ---------------------------------------------------------------------------

// One recorded log event — what a Sink receives.
//
// The string_views and the field span are non-owning: they point at
// caller storage that must outlive the Sink::emit() call.
// (LOG-002: `message` is concise human-readable text; `fields` carry
// the structured detail, so failures state what failed and why.)
struct LogRecord {
  // One documented clock (AGENTS §14): std::chrono::system_clock,
  // rendered by the sinks in UTC as "YYYY-MM-DDTHH:MM:SS.ffffffZ"
  // (RFC 3339). Diagnostics only — never part of authoritative state
  // (ARCH-009).
  std::chrono::system_clock::time_point timestamp;
  Severity severity;
  std::string_view subsystem;  // stable, machine-searchable (LOG-001)
  std::string_view event;      // stable, machine-searchable (LOG-001)
  std::string_view message;    // concise human text (LOG-002)
  std::span<const Field> fields;
  // Emitting-thread identity (std::hash of std::thread::id; the
  // "thread or job identity" field of AGENTS §14).
  std::uint32_t threadId;
};

// A replaceable logging backend (FR-12.2: sink-swappable).
//
// Contract for implementations:
//   - emit() is called only for events the facade has enabled, never
//     concurrently with itself. Implementations still serialize their
//     own output (CONC-001) and MUST NOT call back into the facade
//     (lock ordering: facade state → sink, never the reverse).
//   - flush() MUST be callable from a crash handler (LOG-007): no
//     blocking on an already-held lock (try_lock), no allocation.
//   - A sink MUST flush in its own destructor, so a destroyed sink
//     never drops buffered output (LOG-007 safe fallback). The base
//     destructor is therefore default.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void emit(const LogRecord& record) = 0;
  virtual void flush() = 0;
};

// Sink writing one line per event to a std::FILE stream (default:
// stderr). The sink does NOT own the stream — it never fopens or
// fcloses it (a ConsoleSink(stderr) must outlive the process and the
// process must keep stderr usable for crash diagnostics).
//
// Complexity per event: O(1) writes + O(fields). A failed write is
// counted (failedWrites()), never thrown and never silent (CORE-008).
class ConsoleSink : public Sink {
 public:
  explicit ConsoleSink(std::FILE* stream) : stream_(stream) {}
  ~ConsoleSink() override { flush(); }

  void emit(const LogRecord& record) override;
  void flush() override;

  // Records whose line could not be written (0 = healthy).
  [[nodiscard]] std::uint64_t failedWrites() const noexcept {
    return failedWrites_;
  }

 private:
  std::mutex mutex_;
  std::FILE* stream_;
  std::uint64_t failedWrites_ = 0;
};

// Sink appending one line per event to a file.
//
// Create via FileSink::create() — open failure returns
// Status::failure(ErrorCode::IoError); the LOG-007 minimal fallback is
// to keep the current sink (console) and report the failure with its
// NFR-13.3 registry text. The sink owns the FILE* it opens.
class FileSink : public Sink {
 public:
  // Opens `path` in binary append mode (platform-stable on-disk
  // format: LF-terminated lines, no Windows text-mode CRLF
  // translation) with plain-`fopen` sharing semantics: the file may be
  // opened read-only concurrently — even by the same process — on every
  // platform, including Windows (where the secure `fopen_s` would deny
  // even that). Never throws (NFR-8.10): a failed open is a Status
  // carrying ErrorCode::IoError.
  [[nodiscard]] static laige::Result<std::unique_ptr<FileSink>>
  create(std::string path);

  ~FileSink() override { flush(); }

  void emit(const LogRecord& record) override;
  void flush() override;

  // Records whose line could not be written (0 = healthy).
  [[nodiscard]] std::uint64_t failedWrites() const noexcept {
    return failedWrites_;
  }
  [[nodiscard]] std::string_view path() const noexcept { return path_; }

 private:
  // Use create(); the constructor exists only for the factory.
  FileSink(std::string path, std::FILE* stream)
      : path_(std::move(path)),
        stream_(stream, closeFile) {}

  // unique_ptr deleter (fclose returns int, so it cannot be named
  // directly as a void(*)(FILE*) deleter).
  static void closeFile(std::FILE* f) {
    if (f != nullptr) std::fclose(f);
  }

  std::string path_;
  std::unique_ptr<std::FILE, void (*)(std::FILE*)> stream_;
  std::mutex mutex_;
  std::uint64_t failedWrites_ = 0;
};

// ---------------------------------------------------------------------------
// The facade
// ---------------------------------------------------------------------------

// Init-phase configuration for Logger::init().
struct LoggerOptions {
  // The sink to use; null → a ConsoleSink on stderr. The logger takes
  // ownership (unique_ptr).
  std::unique_ptr<Sink> sink = nullptr;
  // Global minimum severity, checked before the per-subsystem level —
  // one atomic load, the cheap first gate.
  Level globalMinimum = Level::Debug;
  // Level applied to subsystems not registered via
  // setSubsystemLevel(). Trace is disabled by default (AGENTS §14).
  Level defaultSubsystemLevel = Level::Debug;
  // LOG-004: repeated failures are rate-limited per
  // (subsystem, event, severity) for Warn/Error/Fatal.
  bool rateLimiting = true;
  // Rate window: at most one event per key per window reaches the
  // sink; the rest are counted and reported in a `rate_limited`
  // summary event when the next event for the key lands after the
  // window (and at shutdown for pending counts).
  std::chrono::milliseconds rateWindow = std::chrono::milliseconds(1000);
  // Clock for timestamps and rate decisions; null →
  // std::chrono::system_clock::now(). Called only for enabled events
  // (never on the disabled path); injectable for tests.
  using ClockFn = std::chrono::system_clock::time_point (*)();
  ClockFn clock = nullptr;
};

// The one logging facade (AGENTS §14): a process-lifetime Meyers
// singleton. See the header top for ownership, threading, and
// performance contracts; the full API contract is in
// docs/api/logging.md.
class Logger {
 public:
  [[nodiscard]] static Logger& instance();

  // Init-phase configuration (MUST NOT run concurrently with logging
  // from other threads). Replaces the current sink (flushed first) and
  // resets subsystem levels, rate state, and the retired flag; a
  // previously installed crash handler is re-registered by a later
  // installCrashHandling() call. Always succeeds: a sink that can fail
  // is created via FileSink::create() before init (hand its sink over
  // with Result::takeValue()). Takes options by value and consumes the
  // sink ownership — call with an rvalue.
  [[nodiscard]] laige::Status init(LoggerOptions options);

  // Per-subsystem level filter (FR-12.2 per-subsystem scopes).
  // Init-phase API. The subsystem name is copied into the facade.
  void setSubsystemLevel(std::string_view subsystem, Level level);

  // The effective level for `subsystem` (its registered level, or
  // defaultSubsystemLevel_ when unregistered). Init-phase API.
  [[nodiscard]] Level subsystemLevel(std::string_view subsystem) const;

  void setGlobalMinimum(Level level);
  [[nodiscard]] Level globalMinimum() const noexcept;

  // Cheap gate behind LAIGE_LOG_*: true only when an event of
  // `severity` from `subsystem` will be recorded. Cost: one atomic
  // load, plus (only if that passes) one mutex section over a small
  // linear scan — no allocation, no formatting (LOG-003).
  [[nodiscard]] bool enabled(Severity severity,
                             std::string_view subsystem) const;

  // Record an enabled event. Fields are moved into the record; the
  // subsystem/event/message string_views must outlive the call.
  // Direct calls evaluate their arguments eagerly — prefer the
  // LAIGE_LOG_* macros (lazy). Fatal events flush and then terminate
  // the process (AGENTS §14 controlled termination).
  template <typename... Fields>
  void emit(Severity severity, std::string_view subsystem,
            std::string_view event, std::string_view message,
            Fields&&... fields) {
    std::array<Field, sizeof...(Fields)> storage{std::move(fields)...};
    record(severity, subsystem, event, message, std::as_const(storage));
  }

  // Flush the sink (LOG-007).
  void flush();

  // Controlled shutdown (CONC-006, idempotent): drain pending
  // rate-limit summaries, flush the sink, and retire the facade —
  // log calls after shutdown are discarded (no sink calls).
  void shutdown();

  // Install crash handlers (LOG-007): SIGSEGV/SIGABRT/SIGBUS/SIGFPE/
  // SIGILL on POSIX (sigaction, one-shot SA_RESETHAND), a vectored SEH
  // filter on Windows. The handler writes a raw notice to stderr
  // (write(2): no stdio lock, no allocation), flushes the sink
  // (try_lock, allocation-free), and lets the default crash handling
  // continue (core dump / debugger / abort). Init-phase API;
  // idempotent.
  [[nodiscard]] laige::Status installCrashHandling();

  // The current sink (diagnostics, DBG-008); never null.
  [[nodiscard]] const Sink* sink() const noexcept { return sink_.get(); }

  // Flush from a crash handler: no facade lock (the signal may have
  // interrupted a dispatch holding it), no allocation (LOG-007).
  void crashFlush() const {
    if (sink_) sink_->flush();
  }

 private:
  struct SubsystemEntry {
    std::string name;
    Level level;
  };

  // Rate-limit state for one (subsystem, event, severity) key
  // (LOG-004). everEmitted marks the first event of a key, which is
  // always recorded (it also keeps the window arithmetic free of the
  // sentinel-value overflow a min() lastEmit would cause, CPP-004).
  struct RateEntry {
    std::string subsystem;
    std::string event;
    Severity severity;
    bool everEmitted = false;
    std::chrono::system_clock::time_point lastEmit{};
    std::uint64_t suppressed = 0;
  };

  Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void record(Severity severity, std::string_view subsystem,
              std::string_view event, std::string_view message,
              std::span<const Field> fields);
  void dispatch(const LogRecord& record);
  [[nodiscard]] bool rateLimited(Severity severity) const noexcept;
  RateEntry* findRate(std::string_view subsystem, std::string_view event,
                      Severity severity);
  // `entry.suppressed` carries the count reported by the summary.
  void emitRateSummary(const RateEntry& entry);

  // Gate 1: global minimum (one atomic load).
  std::atomic<std::uint8_t> globalMinimum_;
  // Retired after shutdown(); log calls are discarded.
  std::atomic<bool> retired_{false};
  mutable std::mutex stateMutex_;
  // Guarded by stateMutex_: small by design (a few dozen entries).
  std::vector<SubsystemEntry> subsystems_;
  std::vector<RateEntry> rates_;
  std::unique_ptr<Sink> sink_;
  Level defaultSubsystemLevel_{Level::Debug};
  bool rateLimiting_{true};
  std::chrono::milliseconds rateWindow_{std::chrono::milliseconds(1000)};
  LoggerOptions::ClockFn clock_{nullptr};
  bool crashHandlingInstalled_{false};
};

}  // namespace laige::log

// ---------------------------------------------------------------------------
// The public logging macros (AGENTS §14 example shape)
//
//   LAIGE_LOG_WARN("network", "packet_dropped",
//                  "Dropped packet outside receive window",
//                  laige::log::field("connection_id", id),
//                  laige::log::field("sequence", seq));
//
// Lazy by construction: the level gate is evaluated first, and only if
// it passes are the message/field arguments evaluated and formatted —
// a disabled event costs one atomic load + branch (LOG-003).
// `subsystem`, `event`, and `message` are stable names and are passed
// twice (gate + record), so pass string literals.
//
// `##__VA_ARGS__` elides the separating comma when no fields are given:
// the portable comma-elision idiom for GCC/Clang/AppleClang and MSVC
// (its default preprocessor). If a future step enables
// /Zc:preprocessor on the engine targets, these macros need to be
// revisited (noted now per CORE-004 rather than handled now).
// ---------------------------------------------------------------------------

#define LAIGE_LOG(severity, subsystem, event, message, ...)             \
  do {                                                                  \
    if (::laige::log::Logger::instance().enabled(                     \
            static_cast<::laige::log::Severity>(severity), subsystem)) { \
      ::laige::log::Logger::instance().emit(                           \
          static_cast<::laige::log::Severity>(severity), subsystem, event, \
          message, ##__VA_ARGS__);                                     \
    }                                                                   \
  } while (0)

#define LAIGE_LOG_TRACE(subsystem, event, message, ...)                 \
  LAIGE_LOG(::laige::log::Severity::Trace, subsystem, event, message,  \
            ##__VA_ARGS__)
#define LAIGE_LOG_DEBUG(subsystem, event, message, ...)                 \
  LAIGE_LOG(::laige::log::Severity::Debug, subsystem, event, message,  \
            ##__VA_ARGS__)
#define LAIGE_LOG_INFO(subsystem, event, message, ...)                  \
  LAIGE_LOG(::laige::log::Severity::Info, subsystem, event, message,   \
            ##__VA_ARGS__)
#define LAIGE_LOG_WARN(subsystem, event, message, ...)                  \
  LAIGE_LOG(::laige::log::Severity::Warn, subsystem, event, message,   \
            ##__VA_ARGS__)
#define LAIGE_LOG_ERROR(subsystem, event, message, ...)                 \
  LAIGE_LOG(::laige::log::Severity::Error, subsystem, event, message,  \
            ##__VA_ARGS__)
#define LAIGE_LOG_FATAL(subsystem, event, message, ...)                 \
  LAIGE_LOG(::laige::log::Severity::Fatal, subsystem, event, message,  \
            ##__VA_ARGS__)
