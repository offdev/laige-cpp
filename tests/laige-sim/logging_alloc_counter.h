// Test-only process-wide allocation counter (M1-ECS-03).
//
// The M1-ECS-03 churn test asserts its zero-allocation property the
// way M0-CORE-02's logging test did: a strong global
// operator new/new[] override counts every heap allocation in the test
// process, so the churn window's `allocCounter() == 0` is proof that
// the add/remove churn touches no heap — only the pre-reserved SoA
// column blocks (the "pool accounting" is the ArchetypeStats
// reservation delta, asserted alongside in the test).
//
// logging_alloc_counter.cpp defines the program's global operator
// new/new[] (the strong definition overrides the CRT's weak default
// for the whole test executable), so every heap allocation made
// anywhere in the process — test framework, engine under test, test
// code — is counted.
//
// TEST-ONLY: never link this translation unit into an engine library
// or a tool — it would replace the real allocator for that binary. It
// is also excluded from the sanitizer build trees (LAIGE_ASAN/
// LAIGE_TSAN): the sanitizer runtimes define their own new/delete, so
// the overrides cannot be linked there (see this directory's
// CMakeLists.txt; the zero-allocation property is verified in those
// trees by the leak-free sanitizer run of the same churn loop plus
// the ArchetypeStats reservation-delta assertion).
//
// (Same pattern as tests/laige-core/logging_alloc_counter.{h,cpp} —
// one copy per test executable, since two strong definitions in one
// binary would collide.)

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
