// Test-only global operator new/new[] overrides (M1-ECS-03; see the
// header).
//
// A strong definition of the global operator new/new[] in this
// translation unit is linked ahead of the CRT's weak defaults
// (GCC/Clang/AppleClang: the library definitions are weak; MSVC: the
// linker only pulls in a CRT allocator module to resolve undefined
// symbols, which this object already defines). Every heap allocation
// made by any translation unit in the test executable therefore
// passes through the counters below.
//
// (Same pattern as tests/laige-core/logging_alloc_counter.cpp — one
// copy per test executable, since two strong definitions in one
// binary would collide.)

#include "logging_alloc_counter.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <new>

namespace laige::test {

void resetAllocCounter() {
  detail::allocCount.store(0, std::memory_order_relaxed);
}

std::uint64_t allocCounter() {
  return detail::allocCount.load(std::memory_order_relaxed);
}

}  // namespace laige::test

namespace {

void count() noexcept {
  laige::test::detail::allocCount.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void* operator new(std::size_t size) {
  count();
  void* p = std::malloc(size);
  if (p == nullptr) std::terminate();  // no exceptions (NFR-8.10)
  return p;
}

void* operator new[](std::size_t size) {
  count();
  void* p = std::malloc(size);
  if (p == nullptr) std::terminate();
  return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  void* p = std::malloc(size);
  if (p != nullptr) count();
  return p;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  void* p = std::malloc(size);
  if (p != nullptr) count();
  return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

// The sized deallocations too: libstdc++ deallocates through
// operator delete(p, size), and without these overrides the CRT
// defaults would present the free to the sanitizer as a delete of a
// malloc-style allocation (ASan alloc-dealloc-mismatch). Routing them
// through std::free keeps every allocation/deallocation pair
// malloc/free-consistent under the sanitizers.
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
