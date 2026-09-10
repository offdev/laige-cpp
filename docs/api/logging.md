# Structured logging facade (`laige::log`)

The one logging facade for the engine (M0-CORE-02, AGENTS.md §14).
Public header: `src/laige-core/include/laige/logging.h`; implementation:
`src/laige-core/logging.cpp`. All engine logging goes through this
facade; the backend (`Sink`) is replaceable (FR-12.2).

## Quick start

```cpp
#include <laige/logging.h>

// One-time, init phase:
auto sink = laige::log::FileSink::create("engine.log");
if (!sink.ok()) {
  // LOG-007 minimal fallback: keep the console sink, report the failure.
  laige::log::Logger::instance()
      .emit(laige::log::Severity::Error, "logging", "file_sink_create_failed",
            "log file could not be opened",
            laige::log::field("error", laige::errorText(sink.error())));
} else {
  laige::log::LoggerOptions o;
  o.sink = std::move(sink).takeValue();   // hand ownership to the facade
  laige::log::Logger::instance().init(std::move(o));
  laige::log::Logger::instance().installCrashHandling();
}

// Hot path (lazy: disabled events do none of this work):
LAIGE_LOG_WARN("network", "packet_dropped",
               "Dropped packet outside receive window",
               laige::log::field("connection_id", conn_id),
               laige::log::field("sequence", seq));
```

## The macros

`LAIGE_LOG_TRACE / _DEBUG / _INFO / _WARN / _ERROR / _FATAL`
take `(subsystem, event, message, field(...), ...)`.

- **Lazy by construction.** The level gate is evaluated first; only if
  it passes are `message` and the `field(...)` arguments evaluated and
  formatted. A disabled event costs exactly one atomic load and a
  branch — no argument evaluation, no formatting, no allocation, no
  lock (LOG-003, PERF-003).
- **Stable names.** `subsystem`, `event`, and `message` are passed to
  both the gate and the record, so pass string literals (LOG-001).
  Event names and field keys are stable and machine-searchable; do not
  embed variable text in them.
- **`field(name, value)`** builds a structured pair: string-like values
  are copied in full; scalars (integral, floating-point, `bool`,
  `char`, enum, pointer) render locale-free into a 64-byte stack
  buffer via `std::to_chars`. Field **values must not contain spaces
  or `=`** (the line format is `| k=v` machine-parseable); put
  variable text in `message`, not in field values.
- **No-argument form works:** `LAIGE_LOG_INFO("net", "started",
  "listening")` — no `field(...)` needed.

## Line format

Each record is one line (see `ConsoleSinkLineFormat` /
`FileSinkLineFormat` tests):

```text
<timestamp> [severity] <subsystem>/<event>: <message> | k1=v1 | k2=v2
```

- **Timestamp:** one documented clock — `std::chrono::system_clock`,
  rendered in UTC as `YYYY-MM-DDTHH:MM:SS.ffffffZ` (RFC 3339, 27
  chars). Diagnostics only; never part of authoritative state
  (ARCH-009).
- **Severity token:** the stable lowercase name (`trace` … `fatal`).
- **Fields:** ` | k=v` per field, in call order.
- A `rate_limited` summary event (below) uses the event fields
  `event=<original event>` and `suppressed=<count>`.

## Severity and levels

`Severity` (Trace…Fatal) follows the AGENTS §14 severity contract.
`Level` adds `Off`. Two independent gates decide whether an event is
recorded (both must pass):

1. **Global minimum** (`LoggerOptions::globalMinimum`, default
   `Debug`): checked first, one atomic load.
2. **Per-subsystem level** (`setSubsystemLevel`, unregistered
   subsystems take `LoggerOptions::defaultSubsystemLevel`, default
   `Debug`).

`Trace` is disabled by default (AGENTS §14); `Off` disables everything
(the cheap switch for release/server profiles). `Logger::enabled()` is
the same check the macros use — use it to guard expensive work you
want to skip together with the log call.

## Rate limiting (LOG-004)

When enabled (`LoggerOptions::rateLimiting`, default `true`), repeated
failures are rate-limited **per `(subsystem, event, severity)`** key,
for **Warn, Error, and Fatal** only (Trace/Debug/Info are never
rate-limited — they are volume-managed by the level gate).

Per key, within one window (`LoggerOptions::rateWindow`, default
1000 ms):

- the **first event is always recorded**;
- each repeat is counted and suppressed (no sink call);
- when the next event for the key lands **after the window**, the
  facade first emits a `rate_limited` summary event
  (`severity` = the key's severity, fields `event=<original>`,
  `suppressed=<count>`) and then records the current event.
- Pending suppressed counts are drained at `shutdown()` (so a shutdown
  immediately after a burst still reports the total).

Keys are independent: `("net", "drop", Warn)` and `("net", "timeout",
Warn)` do not share a window.

## Ownership and lifetime

- `Logger::instance()` is a **process-lifetime Meyers singleton**
  (thread-safe construction). It owns the current sink
  (`unique_ptr`); a sink passed via `LoggerOptions` is moved in and
  flushed before replacement.
- `LogRecord` string views and the field span are **non-owning**:
  caller storage must outlive the `Sink::emit()` call — the macros
  guarantee this (all arguments are literals/locals alive for the
  `do { … }` statement).
- `FileSink` **owns** the `FILE*` it opens (closed on destruction,
  after a final flush). `ConsoleSink` does **not** own its stream
  (`stderr` must outlive the process).
- `init()` takes `LoggerOptions` **by value** and consumes the sink
  ownership — call it with an rvalue.

## Threading and permitted execution phase (CONC-001/002)

- **Logging from any thread is safe:** enabled events are serialized
  on one facade mutex (level/rate state), and each sink serializes its
  own output.
- **Init-phase APIs** — `init()`, `setSubsystemLevel()`,
  `subsystemLevel()`, `setGlobalMinimum()`, `globalMinimum()` (the
  reader is const-safe), `installCrashHandling()` — MUST NOT run
  concurrently with logging or with each other from other threads.
  Configure before the engine starts its threads; mutate afterwards
  only from a single control thread with no concurrent logging
  (mutation in an explicit phase, API-004).
- **Lock ordering:** the facade state mutex is never held while a sink
  is called, and sinks never call back into the facade.
- `shutdown()` is idempotent (CONC-006): it retires the facade, drains
  pending rate-limit summaries, and flushes. Log calls after shutdown
  are discarded (no sink calls, no crash).

## Complexity, allocation, and blocking

| Operation | Time | Allocation | Blocking |
|---|---|---|---|
| Disabled event (macro) | one atomic load + branch | none | none |
| `enabled()` | one atomic load; only if that passes, one mutex section + small linear scan | none | mutex (contended only by logging/config) |
| Enabled event | one mutex section + one sink write | Field value strings (only when fields given); one rate-state entry per distinct key (amortized, bounded by distinct event names); one summary record per window rollover | mutex + sink I/O |
| `flush()` | one sink flush | none | sink I/O (file write) |
| `shutdown()` | drains pending summaries + one flush | summary records for pending counts | sink I/O |
| `FileSink::create()` | one `fopen` (append) | the sink object | file open |
| `installCrashHandling()` | one `sigaction` per signal (POSIX) / one handler (Windows) | none | none |

Hot-path budget (CORE-002): the disabled path is the budgeted one —
measured at 46 ns/event vs 1207 ns/event enabled on the M0 hardware
(see Performance). FR-12.2: no logging in sim/render hot paths by
default — hot-path diagnostics belong to Trace/Debug behind the level
gate.

**Blocking/I/O:** sink writes are the only I/O in the facade. A
`FileSink` write can block on a slow/full disk; that cost is paid
only for enabled events. Nothing in the facade blocks on a lock held
across I/O (the state mutex is released before the sink call).

## Determinism and network authority

Log timestamps come from the wall clock and are diagnostics only
(ARCH-009): they never enter authoritative simulation state and never
affect replay determinism (ARCH-010). Event *names* and *field keys*
are stable strings (LOG-001) — suitable for log mining, not for
protocol state.

## Failure behavior (CORE-008, LOG-007)

- **Nothing throws.** `FileSink::create()` returns
  `Result<unique_ptr<FileSink>>`; a failed open is
  `ErrorCode::IoError` with the NFR-13.3 registry text
  (`docs/api/errors.md#io-error`). The LOG-007 minimal fallback: keep
  the current sink (console) and report the failure — the engine keeps
  running.
- **Failed writes are counted, never silent:** `ConsoleSink::
  failedWrites()` / `FileSink::failedWrites()` count records whose
  line could not be written (poll it as a diagnostic, DBG-008).
- **Fatal** records the event, flushes, and terminates the process via
  `std::abort()` — controlled termination after preserving
  diagnostics (AGENTS §14). If crash handling is installed, the
  SIGABRT handler flushes once more (idempotent) before the default
  crash handling continues.
- **Crash handling** (`installCrashHandling()`, idempotent):
  - POSIX: `sigaction` for SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL
    with `SA_RESETHAND` (one-shot: the handler restores the default
    disposition so the following `raise` produces the normal core
    dump).
  - Windows: a vectored SEH handler continuing execution after the
    flush.
  - The handler writes a fixed notice with raw `write(2)` (no stdio
    lock, no allocation), calls `crashFlush()` (sinks use `try_lock`,
    so a flush from a signal never blocks on a lock the interrupted
    emit may hold), then re-raises the signal.
  - Registration failure returns `Status::failure(ErrorCode::IoError)`
    (logged by the caller, CORE-008).
- **Init failure** cannot happen: `init()` always succeeds (sink
  creation happens before it, via `FileSink::create()`).

## Performance (DOC-004)

- **Disabled path** (the hot-path contract, LOG-003/PERF-003): one
  atomic load + branch. **Zero allocations, zero formatting, zero
  locks.** Verified by `LogPerformance.DisabledTraceSpamAllocatesNothing`
  (100k disabled Trace events → 0 allocations via a test-only process
  `operator new` counter; the sanitizer trees run the same spam loop
  leak-free instead — the counter cannot be linked against the
  sanitizer runtimes, see `tests/laige-core/CMakeLists.txt`).
- **Enabled path:** one facade mutex section (level lookup + rate
  decision over small vectors), then one sink write. Allocations are
  proportional to field count and bounded by distinct event names —
  no per-event state growth.
- **Batching guidance:** the facade has no per-event queue; sinks
  write one line per event. For very high-volume enabled logging,
  prefer a rate-limited aggregate event (already built in, LOG-004)
  or raise the subsystem level — do not add a user-space queue in
  front of the facade (it would duplicate what `shutdown()`/
  `flush()` already guarantee).
- **Traps:**
  - Calling `field(...)` outside a `LAIGE_LOG_*` macro for an event
    that may be disabled **allocates for nothing** (LOG-003) — check
    `enabled()` first or use the macro.
  - Putting variable text in field values breaks the `k=v` format
    (spaces/`=` are forbidden in values).
  - Logging with a long-held external lock held: the facade's mutex
    is taken per event; holding your own lock across a logging call is
    allowed but serializes the sink behind your lock — keep critical
    sections short.
- **Measured (M0 hardware: see `LogPerformance.DisabledPathCheaperThanEnabled`):**
  disabled ≈ 46 ns/event, enabled (Debug, 1 field) ≈ 1207 ns/event,
  N=200000, Debug build. The disabled path is ~26× cheaper.

## Misuse warning

```cpp
// WRONG: field() evaluated before the gate — allocates even when
// disabled.
LAIGE_LOG_TRACE("net", "verbose", "x",
                laige::log::field("payload", expensiveString()));

// RIGHT: the macro defers field construction until the gate passes.
LAIGE_LOG_TRACE("net", "verbose", "x",
                laige::log::field("len", expensiveString().size()));

// WRONG: rate limiting does not apply to Info; this spam is only
// managed by the level gate.
LAIGE_LOG_INFO("net", "flapping", "still flapping");

// RIGHT: use Warn (recovered degradation) so the LOG-004 summary
// aggregates the repeats.
LAIGE_LOG_WARN("net", "flapping", "link flapping; recovered");
```

## Testing

`tests/laige-core/logging_tests.cpp` — CTest entry **`logging`**
(selected via `--gtest_filter` on the shared `laige-core_tests`
executable): gate semantics, field formatting, both sinks, rate
limiting, Fatal termination (forked child), crash/shutdown behavior,
concurrency (TSan lane), and the performance properties above.
Verification for this step: `ctest -R logging` green on the static,
shared, ASan, and TSan trees (GCC), plus Clang static/shared.
