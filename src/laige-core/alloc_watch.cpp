// laige-core allocation watch counting backend (M1-ALLOC-01).
//
// Implementation of the allocWatchArm / allocWatchRead / allocWatchLive
// declared in include/laige/alloc_watch.h — see that header for the
// full contract (the armed-window model, the per-tick assertion it
// feeds, the scope of the counting backend, the cost, and the
// threading rules) and docs/api/alloc_watch.md for the API document.
//
// The strong definitions of the global operator new/new[] below are
// linked ahead of the CRT's weak defaults (GCC/Clang: the library
// definitions are weak; MSVC: the linker only pulls in a CRT
// allocator module to resolve undefined symbols, which this object
// already defines), so every heap allocation made anywhere in the
// process (static build trees) — or inside the engine images (shared
// build trees; the platform interposition scope in the header) —
// passes through watchRecord().
//
// This translation unit is compiled ONLY in the non-sanitizer trees
// (the CMake gate sets LAIGE_ALLOC_WATCH on the target in exactly
// those trees): the sanitizer runtimes define their own new/delete,
// so the overrides cannot be linked there (the header's scope
// section). Test binaries that need the same counter use the
// laige::test compatibility facade (tests/**/
// logging_alloc_counter.h), which wraps this backend — one strong
// definition per binary (the M0-CORE-02 test-TU precedent, moved
// here in M1-ALLOC-01).

#include "laige/alloc_watch.h"  // the contract (this header)

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <new>

namespace laige {

namespace detail {

// The watch's process state (see the header): exactly one armed
// window at a time, owned by the sim owner thread. Relaxed atomics:
// the only writes from the owner are the arm's two stores; a
// concurrent allocation from another thread reads the armed flag and
// increments the counters — observation data, not simulation state
// (CONC-001, ARCH-009).
inline std::atomic<bool> kWatchArmed{false};
inline std::atomic<std::uint64_t> kWatchAllocs{0};
inline std::atomic<const void*> kWatchFirstSite{nullptr};

// The logging-facade emit depth (the attribution contract, header):
// nonzero while the diagnostic subsystem is emitting an event — its
// heap work is not the sim loop's (G-R1 / sim_heap_allocs measures
// the sim loop's storage and systems, not the diagnostic subsystem's
// event memory). A counter, not a flag: nested emits (a sink that
// logs) keep the guard active until the outermost emit finishes.
inline std::atomic<std::uint32_t> kLoggingEmitDepth{0};

}  // namespace detail

namespace detail {

LoggingAllocationGuard::LoggingAllocationGuard() noexcept {
  kLoggingEmitDepth.fetch_add(1, std::memory_order_relaxed);
}

LoggingAllocationGuard::~LoggingAllocationGuard() noexcept {
  kLoggingEmitDepth.fetch_sub(1, std::memory_order_relaxed);
}

}  // namespace detail

void allocWatchArm() noexcept {
  // Data before the flag: the owner's two stores land first, so a
  // reader that sees armed==true always sees the reset values (the
  // relaxed order is enough for the single-owner-thread model).
  detail::kWatchFirstSite.store(nullptr, std::memory_order_relaxed);
  detail::kWatchAllocs.store(0, std::memory_order_relaxed);
  detail::kWatchArmed.store(true, std::memory_order_relaxed);
}

AllocWatchReading allocWatchRead() noexcept {
  const std::uint64_t allocs =
      detail::kWatchAllocs.load(std::memory_order_relaxed);
  const void* site = detail::kWatchFirstSite.load(std::memory_order_relaxed);
  return AllocWatchReading{allocs, site};
}

bool allocWatchLive() noexcept { return true; }

}  // namespace laige

namespace {

// The allocating call's own return address — the actionable offending
// call site (FR-12.3). Evaluated DIRECTLY in the operator new frame
// (a helper function would add a frame and shift the return address
// into the helper, not the allocating caller). GCC/Clang: the
// builtin; MSVC: the _ReturnAddress() intrinsic from <intrin.h>;
// any other compiler: no site (the count still works — the assert's
// message then points at the log event only).
#if defined(__GNUC__) || defined(__clang__)
#  define LAIGE_ALLOC_CALLER_SITE() __builtin_return_address(0)
#elif defined(_MSC_VER)
#  include <intrin.h>  // _ReturnAddress
#  define LAIGE_ALLOC_CALLER_SITE() _ReturnAddress()
#else
#  define LAIGE_ALLOC_CALLER_SITE() static_cast<const void*>(nullptr)
#endif

// The counting body of the operator new overrides (see the header's
// cost contract): armed → count the allocation and capture the first
// site; unarmed → exactly one atomic load + one branch. The
// attribution contract (header): while the logging facade is
// emitting an event (kLoggingEmitDepth > 0) the diagnostic
// subsystem's heap work is not the sim loop's — it is not counted.
// `site` is the allocating call's own address (the
// LAIGE_ALLOC_CALLER_SITE builtin evaluated in the operator new
// frame — one level above here).
inline void watchRecord(const void* site) noexcept {
  if (laige::detail::kLoggingEmitDepth.load(std::memory_order_relaxed) >
      0) {
    return;
  }
  if (!laige::detail::kWatchArmed.load(std::memory_order_relaxed)) {
    return;
  }
  laige::detail::kWatchAllocs.fetch_add(1, std::memory_order_relaxed);
  // The first site wins: the relaxed CAS fails once the first
  // offender is recorded, so later offenders cost the failed CAS
  // only (windows are short and allocation-free by contract).
  const void* expected = nullptr;
  (void)laige::detail::kWatchFirstSite.compare_exchange_strong(
      expected, site, std::memory_order_relaxed,
      std::memory_order_relaxed);
}

}  // namespace

void* operator new(std::size_t size) {
  watchRecord(LAIGE_ALLOC_CALLER_SITE());
  void* p = std::malloc(size);
  if (p == nullptr) std::terminate();  // no exceptions (NFR-8.10)
  return p;
}

void* operator new[](std::size_t size) {
  watchRecord(LAIGE_ALLOC_CALLER_SITE());
  void* p = std::malloc(size);
  if (p == nullptr) std::terminate();
  return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  void* p = std::malloc(size);
  if (p != nullptr) watchRecord(LAIGE_ALLOC_CALLER_SITE());
  return p;  // a failed nothrow alloc is not counted
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  void* p = std::malloc(size);
  if (p != nullptr) watchRecord(LAIGE_ALLOC_CALLER_SITE());
  return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

// The sized deallocations too: libstdc++ deallocates through
// operator delete(p, size), and routing them through std::free keeps
// every allocation/deallocation pair malloc/free-consistent (the
// M1-ECS-03 test-TU precedent).
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
