// laige-core allocation watch (M1-ALLOC-01; PRD §8.1, G-R1).
//
// G-R1 (PRD §9.3, "Zero sim-loop allocations"): the simulation's
// steady-state tick path MUST allocate nothing. The budgets.json
// `sim_heap_allocs` entry encodes the budget (target 0 allocs per
// frame, "asserted in debug builds"); this header is the debug-side
// enforcement mechanism the guardrail table names: "Debug:
// allocation counter + assert". The release side of the guardrail
// (pool overflow → logged degradation, no crash) already lives in the
// pool accounting (pools.h, M0-CORE-05) and the per-frame simAllocs
// delta (M1-PROF-01/02 frame report) — nothing new ships there.
//
// The watch is a process-wide heap-allocation counter with an ARMED
// WINDOW:
//
//   - allocWatchArm()    starts a fresh window: it resets the window's
//                        allocation count and clears the first-site
//                        capture. One armed window at a time; the
//                        window is live until the next arm().
//   - allocWatchRead()   reads the window: the allocation count since
//                        arm() plus the call site of the FIRST
//                        offending allocation (nullptr while none).
//
// The counting backend is a strong definition of the global operator
// new/new[] (alloc_watch.cpp, compiled in every non-sanitizer build
// tree): while a window is armed, every heap allocation made by ANY
// translation unit — engine storage, the pools, a system's local
// std::vector, even a hot-path log — increments the window count, and
// the caller's return address of the first offending allocation is
// captured (the actionable call site, FR-12.3). While no window is
// armed, each allocation pays exactly one atomic load + one branch.
//
// ---------------------------------------------------------------------------
// The per-tick assertion (the laige-sim half of M1-ALLOC-01)
// ---------------------------------------------------------------------------
//
// In DEBUG builds, GameLoop::runOneTick() (laige/sim/game_loop.h,
// "The zero-allocation check") arms a fresh window before the tick
// body and reads it after a COMPLETED tick: a tick with a nonzero
// window count fails with one structured Error event
// (alloc/sim_tick_allocation — the offending call site in the site
// field) followed by the debug assert. That check is the standing
// hot-path guardrail for every later sim/render step (roadmap
// README §6, "Global invariants"). Release builds carry no check and
// no crash: a game system that allocates in release degrades through
// the already-logged pool accounting (the pool overflow path,
// pools.h) — never a silent success, never an assert.
//
// ---------------------------------------------------------------------------
// Scope of the counting backend (read before relying on it)
// ---------------------------------------------------------------------------
//
//   - Static build trees (the default): the strong operator new
//     overrides sit in the executable's link, so an armed window sees
//     EVERY heap allocation in the process (engine, pools, game
//     systems, test frameworks).
//   - Shared build trees: the overrides live inside the laige-core
//     image. On POSIX, dynamic linking interposes them process-wide
//     (an executable's operator new call resolves to the library's
//     definition); on Windows there is no cross-image interposition,
//     so an armed window sees the allocations made inside the engine
//     images — the engine allocators and the pools, which IS the sim
//     loop's storage — but not allocations made in the executable
//     itself. The canonical (static) trees give full process coverage
//     on every P0 OS.
//   - Sanitizer trees (LAIGE_ASAN / LAIGE_TSAN): the sanitizer
//     runtimes own operator new/delete, so the counting backend is
//     NOT compiled in and this header degrades to inline no-ops
//     (allocWatchLive() is false). There, the zero-allocation property
//     is verified by the leak-free sanitizer run of the same loop
//     plus the pool reservation-delta assertion (the established
//     fallback pattern — tests/laige-core, M0-CORE-02/05;
//     tests/laige-sim, M1-ECS-03/07).
//   - The LAIGE_ALLOC_WATCH=1 compile definition (set on laige-core
//     and inherited by every consumer) marks every tree where the
//     counting backend is compiled in.
//
// ---------------------------------------------------------------------------
// Cost (PERF-003, DBG-004)
// ---------------------------------------------------------------------------
//
//   - Per allocation, window disarmed: one logging-depth load + one
//     armed-flag load + two branches (no counter traffic, no
//     allocation, no logging).
//   - Per allocation, window armed (not inside a diagnostic emit):
//     one logging-depth load, one armed-flag load, one fetch_add, and
//     one compare-and-swap that fails once the first site is recorded
//     (windows are short and allocation-free by contract, so the CAS
//     is cheap in practice).
//   - Per allocation, inside a diagnostic emit: one logging-depth
//     load + one branch (the emission's own work, not the sim loop's
//     — the attribution contract).
//
//   Attribution (G-R1 measures the SIM LOOP's heap — sim_heap_allocs,
//   budgets.json): allocations made by the logging facade while it
//   emits an event are NOT attributed to the window. The diagnostic
//   subsystem's event memory (rate-state, message formatting, the
//   sink) is its own subsystem with its own memory contract (LOG-003
//   bounds HOT-PATH logging separately); a cold-path event the engine
//   must log (G-R5 budget overrun, a replay write failure, a
//   guardrail warn) degrades loudly, never trips G-R1. The facade's
//   emit path wraps its work in detail::LoggingAllocationGuard —
//   everything a tick does that is not the diagnostic subsystem's
//   emit (a system's local std::vector, engine storage growth, any
//   other heap use) still counts and still fails the per-tick assert.
//   - Per completed tick, debug builds only: one arm (three atomic
//     stores — the first-site, the count, and the armed flag) + one
//     read (two atomic loads) — no allocation, no logging on the
//     healthy path (LOG-003). Release builds: the entire check is
//     compiled out.
//
// Threading (CONC-001): the armed window has exactly one owner — the
// sim owner thread (the simulation is single-threaded, PRD §10.2);
// the loop's tick path is the only caller that arms, so windows never
// nest. The counters are relaxed atomics: an allocation from another
// thread during an armed window is counted (it did happen during the
// tick — the diagnostic says so) but it is observation data, not
// simulation state (ARCH-009).

#pragma once

#include <cstddef>
#include <cstdint>

namespace laige {

// One armed-window reading (see the header preamble): the heap-
// allocation count since the last arm() and the call site of the
// first offending allocation (nullptr while none).
struct AllocWatchReading {
  std::uint64_t allocs{};    // heap allocations since arm()
  const void* firstSite{};  // first offending call site (nullptr if none)
};

// Start a fresh watch window: reset the window's allocation count and
// clear the first-site capture (see the header preamble for the
// window model, the cost, and the threading contract). O(1), no
// allocation.
void allocWatchArm() noexcept;

// Read the current armed window (see AllocWatchReading). O(1), no
// allocation.
[[nodiscard]] AllocWatchReading allocWatchRead() noexcept;

// True when the process-wide counting backend is compiled into this
// build (every non-sanitizer tree); false in the sanitizer trees,
// where the runtimes own operator new/delete and the watch is a
// no-op (the header's scope section).
[[nodiscard]] bool allocWatchLive() noexcept;

namespace detail {

// The logging facade's emit-path guard (the attribution contract,
// above). The logging facade wraps each event emission in this guard
// so the diagnostic subsystem's own heap work (rate-state, message
// formatting, the sink) is not attributed to the sim loop's G-R1
// window. Reentrant (nested emits keep the guard active); O(1), no
// allocation. TEST/ENGINE-INTERNAL: not part of the public API — it
// exists only so the facade (logging.cpp) can mark its own emit work.
class LoggingAllocationGuard {
 public:
  LoggingAllocationGuard() noexcept;
  ~LoggingAllocationGuard() noexcept;
  LoggingAllocationGuard(const LoggingAllocationGuard&) = delete;
  LoggingAllocationGuard& operator=(const LoggingAllocationGuard&) =
      delete;
};

}  // namespace detail

#if !defined(LAIGE_ALLOC_WATCH)
// The no-op fallback (the sanitizer trees): the counting backend is
// not compiled in, so an armed window never sees anything. The
// functions are inline no-ops — the engine's per-tick check and the
// test-side probes (tests/**/logging_alloc_counter.h) compile
// unchanged and read zero.
inline void allocWatchArm() noexcept {}
inline AllocWatchReading allocWatchRead() noexcept {
  return AllocWatchReading{};
}
inline bool allocWatchLive() noexcept { return false; }
namespace detail {
inline LoggingAllocationGuard::LoggingAllocationGuard() noexcept {}
inline LoggingAllocationGuard::~LoggingAllocationGuard() noexcept {}
}  // namespace detail
#endif

}  // namespace laige
