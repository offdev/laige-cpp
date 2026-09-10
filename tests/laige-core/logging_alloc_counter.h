// Test-only process-wide allocation counter (M0-CORE-02).
//
// logging_alloc_counter.cpp defines the program's global operator
// new/new[] (the strong definition overrides the CRT's weak default
// for the whole test executable), so every heap allocation made
// anywhere in the process — test framework, engine under test, test
// code — is counted. It is the M0 stand-in for M0-CORE-05's pool
// accounting for this roadmap step's "disabled levels allocate
// nothing" assertion (M0-CORE-05 does not exist yet).
//
// TEST-ONLY: never link this translation unit into an engine library
// or a tool — it would replace the real allocator for that binary. It
// is also excluded from the sanitizer build trees (LAIGE_ASAN/
// LAIGE_TSAN): the sanitizer runtimes define their own new/delete, so
// the overrides cannot be linked there (see tests/laige-core/
// CMakeLists.txt; the zero-allocation property is verified in those
// trees by the leak-free runs of the same spam loop plus the timing
// property test).

#pragma once

#include <atomic>
#include <cstdint>

namespace laige::test {

namespace detail {

// The process-wide heap-allocation count (see the file header).
inline std::atomic<std::uint64_t> allocCount{0};

}  // namespace detail

// Reset the counter to zero. Call it after the test framework has
// finished its startup allocations and before the region under test.
void resetAllocCounter();

// The number of heap allocations since the last reset.
std::uint64_t allocCounter();

}  // namespace laige::test
