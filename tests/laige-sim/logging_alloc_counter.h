// Test-only allocation-counter facade (M1-ECS-03; moved to laige-core
// in M1-ALLOC-01).
//
// The process-wide global operator new/new[] overrides that back this
// counter no longer live in this test tree: they moved into
// laige-core (src/laige-core/alloc_watch.cpp, compiled in every
// non-sanitizer tree — the LAIGE_ALLOC_WATCH definition marks it),
// so the engine's per-tick zero-allocation assertion (M1-ALLOC-01,
// G-R1) and the test-side zero-allocation probes share ONE counting
// backend. This header keeps the original test API on top of it:
//
//   - resetAllocCounter() arms a fresh watch window;
//   - allocCounter() reads the window's allocation count.
//
// TEST-FACADE ONLY: the counter is a diagnostic, not an API.
//
// Sanitizer trees: the watch is compiled out there (the sanitizer
// runtimes define their own new/delete), LAIGE_ALLOC_COUNTER is not
// defined, and the counter API is unused — the zero-allocation
// properties are verified by the leak-free sanitizer run of the same
// loop plus the pool reservation-delta assertion (the established
// fallback pattern, see tests/laige-sim/CMakeLists.txt).

#pragma once

#include <cstdint>

#include "laige/alloc_watch.h"

namespace laige::test {

// Reset the counter to zero (start a fresh watch window). Call it
// after the test framework has finished its startup allocations and
// before the region under test.
inline void resetAllocCounter() { laige::allocWatchArm(); }

// The number of heap allocations since the last reset.
inline std::uint64_t allocCounter() {
  return laige::allocWatchRead().allocs;
}

}  // namespace laige::test
