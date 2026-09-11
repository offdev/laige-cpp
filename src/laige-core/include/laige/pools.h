// laige-core memory pools (M0-CORE-05).
//
// PRD §10.4 (memory model): engine-owned, budgeted, accounted pools are
// the home for per-frame and long-lived engine data. S-2 (PRD §9.1): the
// safe API has no per-frame heap — data goes through engine pools. This
// header ships the two pool kinds:
//
//   ArenaPool<T>  Arena-scoped, bump-only storage with a reset-per-frame
//                 lifetime: one contiguous pre-allocated block; create()
//                 advances a cursor, reset() destroys every live element
//                 and rewinds it. Slots are valid until the next reset().
//                 No per-element destruction, no in-frame recycling.
//
//   Pool<T>       Stable-handle pool: a fixed set of slots addressed by
//                 generation-checked handles (CPP-007). Elements are
//                 created and destroyed in any order; freed slots are
//                 recycled LIFO, so an element's slot — and therefore its
//                 handle — is stable for the element's lifetime.
//
// ---------------------------------------------------------------------------
// Common contracts (both pools)
// ---------------------------------------------------------------------------
//
// Budget (PERF-003, PERF-008, SCALE-003): the element capacity is fixed
// at construction (Options::capacity) and is a *declared budget* (S-6).
// A create() beyond the budget returns ErrorCode::BudgetExhausted — a
// pool never grows silently (S-2, G-R1). Raising a budget is a typed
// configuration change (API-006), never a runtime behavior.
//
// Allocation (PERF-003, PERF-002): construction performs the pool's only
// backing allocation (a setup path, never a hot path). Every subsequent
// create()/destroy()/reset()/clear() is O(1) (Pool's free list is LIFO),
// allocates nothing, and does no I/O or synchronization. The only real
// work of a create is the element's own constructor (placement new).
//
// Accounting (PRD §10.4: "all memory is accounted in the profiler";
// FR-11.4 peak tracking; G-R4 churn counting): stats() returns a
// PoolStats value — element counts (capacity, inUse, peakInUse), the
// totalCreated churn counter, and backing-store byte counts. The pool
// publishes; the M1 profiler (FR-11.4 memory inspector) pulls. There is
// no registration or callback (CORE-004: the smallest complete design;
// a sink interface would be speculative until the profiler lands).
//
// Errors (FR-12.1, CORE-008): failures are Result/Status values — never
// exceptions, never silent:
//
//   create()   beyond budget          -> ErrorCode::BudgetExhausted
//   destroy()  invalid/stale handle   -> ErrorCode::InvalidArgument
//
// The pool itself does not log: a failing create stays a branch on the
// cold path, and the owning system logs the failure under its own
// subsystem name (LOG-002, G-R1 "pool overflow -> logged degradation").
//
// Ownership (CPP-002, CONC-001): a pool owns its elements and its
// backing store. It is copyable-never, movable (O(1) pointer swap), and
// its destructor/clear()/reset() destroys every live element — no leak,
// ASan-verifiable. A pool has exactly one owner thread: it is NOT
// thread-safe. Use it from one owner (typically one system's update
// phase); sharing across threads requires an explicit engine
// synchronization boundary (CONC-002), which M1 defines per subsystem.
//
// Determinism (ARCH-010, PRD §10.3): pool allocation is pure integer
// bookkeeping — no floating point, no platform intrinsics, no
// randomness, no ordering that depends on anything but the operation
// sequence. The same create/destroy sequence produces bit-identical
// handle sequences on every platform, so handles are safe to replicate
// and are replay state from M1 on.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//   - A PoolHandle held past destroy()/clear() is use-after-free. The
//     engine's S-9 contract: fail loudly in debug — at() asserts — and
//     check explicitly in every build: isValid() to query, get() for the
//     null-safe read (nullptr when stale).
//   - ArenaPool slots die with the next reset(): never carry a slot
//     index across frames, and never destroy single arena elements
//     (reset is the only release; that is the arena's contract).
//   - A PoolHandle is meaningful only in the pool that produced it:
//     index+generation pairs from different pools are not comparable.
//   - T must not throw from its constructor (NFR-8.10: the engine builds
//     with -fno-exceptions; a throwing T would terminate the process).

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "laige/result.h"

namespace laige {

// ---------------------------------------------------------------------------
// Shared pool value types
// ---------------------------------------------------------------------------

// One pool's accounting snapshot (PRD §10.4, FR-11.4, G-R4). A plain
// value the M1 profiler aggregates; there is no registration (CORE-004).
//
//   capacity      element budget (Options::capacity)
//   inUse         live elements right now
//   peakInUse     high-water mark of inUse since construction
//   totalCreated  successful create() calls since construction (churn)
//   bytesCapacity backing store bytes (element slots + handle bookkeeping)
//   bytesInUse    element bytes occupied by live elements
struct PoolStats {
  std::uint32_t capacity{};
  std::uint32_t inUse{};
  std::uint32_t peakInUse{};
  std::uint64_t totalCreated{};
  std::size_t bytesCapacity{};
  std::size_t bytesInUse{};
};

// A stable handle into a Pool<T> (CPP-007): a 32-bit slot index plus a
// 32-bit generation. A handle is valid while the slot is live and its
// current generation equals the handle's. A generation bump on free
// invalidates every stale handle to that slot; a generation wrap after
// 2^32 frees of one slot is defined unsigned wraparound and is
// effectively unreachable (documented, not assertable — see
// Pool::isValid). A default handle {0, 0} is never valid.
//
// A handle is only meaningful in the pool that produced it (see the
// header misuse warnings). Comparison operators compare the pair.
struct PoolHandle {
  std::uint32_t index{};
  std::uint32_t generation{};
};

inline bool operator==(PoolHandle a, PoolHandle b) noexcept {
  return a.index == b.index && a.generation == b.generation;
}
inline bool operator!=(PoolHandle a, PoolHandle b) noexcept {
  return !(a == b);
}

// Raw aligned storage for one element: a byte array with exactly the
// size of T (1 byte for an empty T) and its alignment, so placement
// new below is always well-formed (CPP-004). A plain struct — not
// std::aligned_storage, which is deprecated since C++23 and whose
// layout has changed across implementations — so ElementSlot is
// well-formed on every P0 toolchain.
template <typename T>
struct ElementSlot {
  alignas(T) std::byte data[(sizeof(T) == 0) ? 1 : sizeof(T)];
};

// ---------------------------------------------------------------------------
// ArenaPool<T> — arena-scoped, budgeted, reset-per-frame storage
// ---------------------------------------------------------------------------

template <typename T>
class ArenaPool {
 public:
  // The element budget, fixed at construction (S-6). A budget of 0 is
  // legal: every create() fails with BudgetExhausted.
  struct Options {
    std::uint32_t capacity{};
  };

  // One backing allocation for the whole arena (setup path, PERF-002).
  explicit ArenaPool(Options options) noexcept
      : capacity_(options.capacity),
        block_(capacity_ > 0
                   ? std::make_unique<ElementSlot<T>[]>(capacity_)
                   : nullptr),
        inUse_(0), peakInUse_(0), totalCreated_(0) {}

  ArenaPool(const ArenaPool&) = delete;
  ArenaPool& operator=(const ArenaPool&) = delete;

  // Destroys every live element (no leak; ASan-verifiable). O(inUse).
  ~ArenaPool() noexcept { reset(); }

  // Move is an O(1) pointer swap; the source becomes an empty arena
  // (capacity 0, every create() fails).
  ArenaPool(ArenaPool&& other) noexcept
      : capacity_(other.capacity_), block_(std::move(other.block_)),
        inUse_(other.inUse_), peakInUse_(other.peakInUse_),
        totalCreated_(other.totalCreated_) {
    other.capacity_ = 0;
    other.inUse_ = 0;
    other.peakInUse_ = 0;
    other.totalCreated_ = 0;
  }
  // Move assignment: destroy the elements this currently owns, take
  // over other's storage, and leave other a valid empty pool — the same
  // net effect as a move construction. O(1).
  ArenaPool& operator=(ArenaPool&& other) noexcept {
    if (this == &other) return *this;
    reset();  // destroy what this currently owns
    capacity_ = other.capacity_;
    block_ = std::move(other.block_);
    inUse_ = other.inUse_;
    peakInUse_ = other.peakInUse_;
    totalCreated_ = other.totalCreated_;
    other.capacity_ = 0;
    other.inUse_ = 0;
    other.peakInUse_ = 0;
    other.totalCreated_ = 0;
    return *this;
  }

  // Create one element in the next free slot (bump). O(1), no
  // allocation; the element's constructor runs here (the op's only real
  // work). Returns the slot index (valid until the next reset()).
  // Beyond the budget: ErrorCode::BudgetExhausted (the arena never
  // grows silently, S-2).
  template <typename... Args>
  [[nodiscard]] Result<std::uint32_t, ErrorCode> create(Args&&... args) {
    if (inUse_ == capacity_) return ErrorCode::BudgetExhausted;
    ::new (static_cast<void*>(&block_[inUse_])) T(
        std::forward<Args>(args)...);
    const std::uint32_t slot = inUse_;
    ++inUse_;
    if (inUse_ > peakInUse_) peakInUse_ = inUse_;
    ++totalCreated_;
    return slot;
  }

  // The per-frame release: destroy every live element and rewind the
  // cursor. O(inUse), no allocation. peakInUse/totalCreated survive the
  // reset (they are since-construction counters; a per-frame profiler
  // diffs them). Idempotent.
  void reset() noexcept {
    for (std::uint32_t i = 0; i < inUse_; ++i) {
      static_cast<T*>(static_cast<void*>(&block_[i]))->~T();
    }
    inUse_ = 0;
  }

  // Element access. at() asserts the slot is live (debug: a loud crash
  // with a message; release: undefined behavior — the engine Result
  // convention). get() is the null-safe read: nullptr for a slot >=
  // inUse (after reset, or never created).
  [[nodiscard]] T& at(std::uint32_t slot) {
    assert(slot < inUse_ &&
           "ArenaPool::at: slot is not live (reset or never created)");
    return *static_cast<T*>(static_cast<void*>(&block_[slot]));
  }
  [[nodiscard]] T* get(std::uint32_t slot) noexcept {
    return slot < inUse_
               ? static_cast<T*>(static_cast<void*>(&block_[slot]))
               : nullptr;
  }

  // True while the slot is live (i.e. until the next reset()).
  [[nodiscard]] bool isValid(std::uint32_t slot) const noexcept {
    return slot < inUse_;
  }

  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint32_t inUse() const noexcept { return inUse_; }

  // Accounting snapshot (PRD §10.4). bytesCapacity is the whole backing
  // block; bytesInUse is the element footprint of the live prefix.
  [[nodiscard]] PoolStats stats() const noexcept {
    const std::size_t stride = sizeof(ElementSlot<T>);
    return PoolStats{
        capacity_,  inUse_,         peakInUse_,   totalCreated_,
        static_cast<std::size_t>(capacity_) * stride,
        static_cast<std::size_t>(inUse_) * stride};
  }

 private:
  std::uint32_t capacity_;
  std::unique_ptr<ElementSlot<T>[]> block_;
  std::uint32_t inUse_;
  std::uint32_t peakInUse_;
  std::uint64_t totalCreated_;
};

// ---------------------------------------------------------------------------
// Pool<T> — stable-handle, generation-checked, bounded pool
// ---------------------------------------------------------------------------

template <typename T>
class Pool {
 public:
  // The element budget, fixed at construction (S-6). A budget of 0 is
  // legal: every create() fails with BudgetExhausted.
  struct Options {
    std::uint32_t capacity{};
  };

  // Backing allocations for the whole pool (setup path, PERF-002): the
  // element store, the per-slot generation table, the LIFO free-list
  // stack (pre-filled 0..capacity-1), and the per-slot alive flag.
  explicit Pool(Options options) noexcept
      : capacity_(options.capacity),
        block_(capacity_ > 0 ? std::make_unique<ElementSlot<T>[]>(capacity_)
                             : nullptr),
        generations_(capacity_ > 0 ? std::make_unique<std::uint32_t[]>(capacity_)
                                   : nullptr),
        freeStack_(capacity_ > 0 ? std::make_unique<std::uint32_t[]>(capacity_)
                                 : nullptr),
        alive_(capacity_ > 0 ? std::make_unique<std::uint8_t[]>(capacity_)
                             : nullptr),
        freeCount_(0), inUse_(0), peakInUse_(0), totalCreated_(0) {
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      generations_[i] = 1;  // generation 0 is reserved: never a handle gen
      freeStack_[i] = i;
      ++freeCount_;
    }
  }

  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  // Destroys every live element (no leak; ASan-verifiable). O(capacity).
  ~Pool() noexcept { clear(); }

  // Move is an O(1) pointer swap; the source becomes an empty pool
  // (capacity 0: every create() fails, every handle invalid).
  Pool(Pool&& other) noexcept
      : capacity_(other.capacity_), block_(std::move(other.block_)),
        generations_(std::move(other.generations_)),
        freeStack_(std::move(other.freeStack_)),
        alive_(std::move(other.alive_)), freeCount_(other.freeCount_),
        inUse_(other.inUse_), peakInUse_(other.peakInUse_),
        totalCreated_(other.totalCreated_) {
    other.capacity_ = 0;
    other.freeCount_ = 0;
    other.inUse_ = 0;
    other.peakInUse_ = 0;
    other.totalCreated_ = 0;
  }
  // Move assignment: destroy the elements this currently owns, take
  // over other's storage, and leave other a valid empty pool — the same
  // net effect as a move construction. O(1).
  Pool& operator=(Pool&& other) noexcept {
    if (this == &other) return *this;
    clear();  // destroy what this currently owns
    capacity_ = other.capacity_;
    block_ = std::move(other.block_);
    generations_ = std::move(other.generations_);
    freeStack_ = std::move(other.freeStack_);
    alive_ = std::move(other.alive_);
    freeCount_ = other.freeCount_;
    inUse_ = other.inUse_;
    peakInUse_ = other.peakInUse_;
    totalCreated_ = other.totalCreated_;
    other.capacity_ = 0;
    other.freeCount_ = 0;
    other.inUse_ = 0;
    other.peakInUse_ = 0;
    other.totalCreated_ = 0;
    return *this;
  }

  // Create one element in the next free slot (LIFO recycle). O(1), no
  // allocation; the element's constructor runs here. Beyond the budget:
  // ErrorCode::BudgetExhausted (the pool never grows silently, S-2).
  template <typename... Args>
  [[nodiscard]] Result<PoolHandle, ErrorCode> create(Args&&... args) {
    if (freeCount_ == 0) return ErrorCode::BudgetExhausted;
    const std::uint32_t slot = freeStack_[--freeCount_];
    ::new (static_cast<void*>(&block_[slot])) T(
        std::forward<Args>(args)...);
    alive_[slot] = 1;
    ++inUse_;
    if (inUse_ > peakInUse_) peakInUse_ = inUse_;
    ++totalCreated_;
    return PoolHandle{slot, generations_[slot]};
  }

  // Destroy one live element and return its slot to the free list.
  // O(1), no allocation. The slot's generation is bumped, so every
  // stale handle to it now fails isValid() (CPP-007). Invalid or stale
  // handle: ErrorCode::InvalidArgument (a caller bug — S-9).
  [[nodiscard]] Status destroy(PoolHandle handle) noexcept {
    if (!isValid(handle)) return ErrorCode::InvalidArgument;
    static_cast<T*>(static_cast<void*>(&block_[handle.index]))->~T();
    alive_[handle.index] = 0;
    ++generations_[handle.index];
    freeStack_[freeCount_++] = handle.index;
    --inUse_;
    return Status{};
  }

  // Element access. at() asserts the handle is live (debug: a loud crash
  // with a message; release: undefined behavior — the engine Result
  // convention). get() is the null-safe read: nullptr when the handle is
  // stale, cleared, or from another pool.
  [[nodiscard]] T& at(PoolHandle handle) {
    assert(isValid(handle) &&
           "Pool::at: handle is stale or invalid (destroyed, cleared, or "
           "from another pool)");
    return *static_cast<T*>(static_cast<void*>(&block_[handle.index]));
  }
  [[nodiscard]] T* get(PoolHandle handle) noexcept {
    return isValid(handle)
               ? static_cast<T*>(static_cast<void*>(&block_[handle.index]))
               : nullptr;
  }

  // Generation-checked liveness (CPP-007): the slot exists, is live, and
  // its current generation matches the handle's. A handle for a freed
  // slot mismatches on the generation bump and can never pass again —
  // except after 2^32 frees of that one slot (defined unsigned wrap;
  // effectively unreachable, documented here as the one case the
  // generation scheme does not rule out).
  [[nodiscard]] bool isValid(PoolHandle handle) const noexcept {
    return handle.index < capacity_ && handle.generation != 0 &&
           alive_[handle.index] != 0 &&
           generations_[handle.index] == handle.generation;
  }

  // Destroy every live element; every handle becomes stale; the capacity
  // is unchanged and the pool is immediately reusable. O(capacity), no
  // allocation. (Reset-per-frame workloads should use ArenaPool<T>
  // instead — its reset() is O(inUse) and has no free-list bookkeeping.)
  void clear() noexcept {
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      if (alive_[i] != 0) {
        static_cast<T*>(static_cast<void*>(&block_[i]))->~T();
        alive_[i] = 0;
        ++generations_[i];
        freeStack_[freeCount_++] = i;
      }
    }
    inUse_ = 0;
  }

  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint32_t inUse() const noexcept { return inUse_; }

  // Accounting snapshot (PRD §10.4). bytesCapacity is the whole backing
  // store (element slots + per-slot generation/alive/free-list
  // bookkeeping); bytesInUse is the element footprint of live elements.
  [[nodiscard]] PoolStats stats() const noexcept {
    const std::size_t stride = sizeof(ElementSlot<T>);
    // Per-slot bookkeeping: generation (4 B) + alive flag (1 B) +
    // free-list stack entry (4 B).
    const std::size_t bookkeepingPerSlot = 9;
    return PoolStats{
        capacity_,  inUse_,         peakInUse_,   totalCreated_,
        static_cast<std::size_t>(capacity_) *
            (stride + bookkeepingPerSlot),
        static_cast<std::size_t>(inUse_) * stride};
  }

 private:
  std::uint32_t capacity_;
  std::unique_ptr<ElementSlot<T>[]> block_;
  std::unique_ptr<std::uint32_t[]> generations_;
  std::unique_ptr<std::uint32_t[]> freeStack_;
  std::unique_ptr<std::uint8_t[]> alive_;
  std::uint32_t freeCount_;
  std::uint32_t inUse_;
  std::uint32_t peakInUse_;
  std::uint64_t totalCreated_;
};

}  // namespace laige
