// laige-render deterministic depth sort (M2-SORT-01): the stable,
// deterministic, pre-allocated sorter for 32-bit isometric depth keys.
//
// FR-2.2: "deterministic, stable z-order ... bucket/radix sort, no
// per-frame allocation". RENDER-003: "visibility and render ordering
// MUST be deterministic within the documented contract. Tie-breaking
// MUST be explicit and stable." AC-4.3: 10k overlapping sprites sorted
// + drawn at 60 FPS (the `depth_sort_10k` budget, PRD §8.1). G-R11: the
// render ordering is engine-owned — game code never hand-rolls the
// sort (S-5).
//
//   DepthSort      One pre-allocated stable 32-bit-key sorter
//
// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
//
// The frame's sprites each carry the M2-ISO-01 32-bit depth key
// (`isoDepthKey<Backend>`, unsigned order = back-to-front painter's
// order). `DepthSort` owns ONE pre-sized flat storage for up to
// `capacity` keys (created once at scene set-up) and sorts the frame's
// key list in place of the batcher, per frame:
//
//   sort(keys)      copy the frame's n keys (0 <= n <= capacity) into
//                   the storage, tagged with their input positions
//                   0..n-1, and sort the (key, index) records by key —
//                   STABLE: records with equal keys keep their input
//                   order.
//
// The sorted order is then read back as two parallel spans:
// `sortedKeys()[i]` and `sortedIndices()[i]` (the original input
// position of the i-th sorted key). The batcher (M2-SPRITE-01) walks
// the sorted order and maps each index to its sprite pool entry — the
// sorter itself knows nothing about sprites (no payload coupling:
// it permutes keys + positions, nothing else).
//
// ---------------------------------------------------------------------------
// The stable tie-break (RENDER-003)
// ---------------------------------------------------------------------------
//
// The total render order is the lexicographic (key, entity id) tuple
// (`isoDepthOrderLess`, iso_depth_key.h §4.3):
//
//   - this sort is STABLE: equal keys keep their INPUT order;
//   - the batcher (M2-SPRITE-01) inserts the frame's keys in the
//     engine's deterministic entity-id iteration order (FR-1.2),
//
// so the sorted output orders equal-key sprites by entity id — the
// (key, entity id) total order, without the sorter ever seeing the
// ids (the input position IS the entity order). A sort that were not
// stable would make equal-key (same screen row) sprites flicker
// between frames; the stability property is what makes the render
// order a function of (keys, insertion order) alone.
//
// ---------------------------------------------------------------------------
// The algorithm: 4 x 8-bit LSD radix (stable bucket) passes
// ---------------------------------------------------------------------------
//
// One stable counting (bucket) sort per 8-bit digit, least significant
// digit first (LSD), 256 buckets per pass, 4 passes over the 32-bit
// key:
//
//   1. count   the 256 digit counts of the current buffer (O(n));
//   2. prefix  the in-place cumulative start positions (O(256));
//   3. scatter the records into the auxiliary buffer in INPUT order —
//              record i lands at start[digit] + (its rank among
//              earlier same-digit records): a stable pass;
//   4. swap the roles of the two buffers.
//
// Invariant (the standard LSD radix-sort argument — Knuth, TAOCP
// Vol. 3 §7.2.1): after the pass on digit k, the buffer is sorted by
// the low k*8 bits, and records with equal low bits keep their input
// order (stability). Induction over the 4 passes: the final buffer is
// sorted by all 32 bits, stably over the whole key — i.e. it is
// exactly the stable sort of the input key sequence. The 4 passes are
// even, so the result lands back in the base buffers (no O(n)
// copy-out — the static_assert below pins the parity).
//
// 8-bit digits (256 buckets) are the fixed-cost sweet spot: 1 KB of
// counter state zeroed per pass (trivial), and the passes are
// data-movement-bound — halving the digit width doubles the passes
// over the data, and 16-bit digits would zero a 256 KB counter table
// per frame for no measured gain (PERF-002/003; the budget suite
// records the absolute cost).
//
// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------
//
// A pure function of the input (key sequence, n): integer arithmetic
// only — no floating point, no hashing, no RNG, no timing, no
// platform-dependent container order. Same input sequence -> bit-
// identical sorted order, on every platform, every build (RENDER-003
// "deterministic"; ARCH-010 scope: this is render-side presentation
// state, ARCH-009 — never part of the sim state hash or replay state).
// The SAME multiset in a DIFFERENT input order yields a (different,
// equally valid) stable order — the order is a function of the
// SEQUENCE, and that is the contract: with the batcher's entity-id
// insertion order, two frames with the same scene state produce the
// same sorted order.
//
// ---------------------------------------------------------------------------
// Ownership, threading, failure
// ---------------------------------------------------------------------------
//
// One owner: the render batch path (the M2-SPRITE-01 batcher owns one
// sorter per sprite pass). Set-up phase: `create` (one flat
// allocation, 16 bytes per capacity slot — 800 KB at 50k). Render
// phase: `sort` (zero allocation, no logging — the per-frame hot
// path, PERF-003/LOG-003). The phases never overlap (the frame
// pipeline's tick -> handoff -> render ordering, M2-GL-02); the
// sorter is never shared with a concurrent writer (CONC-001; not
// thread-safe by design — the single-owner pattern of
// IsoDepthKeyTable and the batcher).
//
// Failure (no silent failure, CORE-008):
//
//   - create(0) or create(capacity > kDepthSortMaxCapacity) ->
//     InvalidArgument (no allocation; the capacity domain is the
//     index width: n positions must fit the uint32 index field);
//   - sort(n > capacity) -> BudgetExhausted, the sorter is UNCHANGED
//     (the previous frame's sorted order is intact). No log inside
//     the hot path: the Status is the failure channel — the batcher
//     handles and logs it (LOG-002: no duplicate logging at every
//     layer), e.g. by clamping the frame's visible set to capacity
//     (the batcher's own documented overflow policy, M2-SPRITE-01).
//   - the default-constructed EMPTY sorter (capacity 0) sorts only
//     an empty key list; a non-empty sort is BudgetExhausted (the
//     stopped-state pattern — total, never UB).
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003/004, DOC-004)
// ---------------------------------------------------------------------------
//
//   create   O(capacity) setup: one flat allocation, 16 B/slot.
//   sort     O(4n + 4*256): 4 count passes + 4 scatters + 4 prefix
//            walks (1024 compares); zero allocation, no logging, no
//            locks, no GL. Budget: 10 000 keys sorted, mean <= 1.0 ms
//            (PRD §8.1, `depth_sort_10k` — 6% of the 16.7 ms 60 FPS
//            frame budget of AC-4.3, half of the 2 ms 50k render-CPU
//            budget: the sort must stay a minority cost of the
//            render path) —
//            docs/benchmarks/baselines/m2-depth-sort.md.
//
// Misuse warnings:
//
//   - Do not sort screen-space coordinates or per-sprite floats
//     (PRD §4, G-R11): the keys are the world-space M2-ISO-01 keys —
//     the sort's order contract is their unsigned integer order.
//   - Do not feed the sorter more keys than `create` sized it for
//     (BudgetExhausted per frame; size it to the scene's sprite
//     budget at set-up — API-006).
//   - Do not rely on the order of DIFFERENT input sequences with the
//     same multiset (stability is per-sequence — the preamble's
//     determinism section); the batcher's entity-id insertion order
//     is what makes the per-frame order reproducible.
//
// Canonical narrative: docs/concepts/coordinates.md §4.6 (ARCH-008);
// API contract: docs/api/depth_sort.md.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "laige/errors.h"
#include "laige/result.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The radix digit width in bits: 8 -> 256 buckets per pass (the
// fixed-cost sweet spot, the preamble's algorithm section).
inline constexpr std::int32_t kDepthSortDigitBits = 8;
// The number of LSD passes over a 32-bit key (exactly 32 / 8).
inline constexpr std::int32_t kDepthSortPasses = 32 / kDepthSortDigitBits;
// The bucket count per pass (2^digit bits).
inline constexpr std::uint32_t kDepthSortBuckets =
    1u << kDepthSortDigitBits;
// The most capacity a sorter may own: the input position must fit the
// 32-bit index field (the sortedIndices span is uint32 — n positions
// must fit).
inline constexpr std::uint32_t kDepthSortMaxCapacity = 0xFFFFFFFFu;
// Bytes of storage per capacity slot: the four uint32 buffers (current
// keys, auxiliary keys, current indices, auxiliary indices).
inline constexpr std::size_t kDepthSortBytesPerSlot = 16;

// The result buffer depends on the pass count being EVEN: 4 passes
// swap the buffer roles 4 times, so the sorted records land back in
// the base buffers (no O(n) copy-out). Changing the digit width must
// keep the pass count even (the digit width itself is free — the
// parity is what the storage layout assumes).
static_assert(kDepthSortPasses % 2 == 0,
              "the LSD passes must be even: the sorted result lands "
              "back in the base buffers without a copy-out pass");

// ---------------------------------------------------------------------------
// The pre-allocated stable 32-bit depth-key sorter (M2-SORT-01)
// ---------------------------------------------------------------------------

// One sorter per sprite pass: the frame's 32-bit depth keys, sorted
// stably into back-to-front order (the model, the tie-break, the
// algorithm, the determinism, the ownership, and the performance
// contracts: the header preamble).
//
// Move-only (CORE-009): the sorter owns its flat record storage; it is
// created by create() and handed to its owner (the batcher).
class DepthSort {
 public:
  // The default state: the EMPTY sorter (capacity 0, no storage). An
  // empty key list sorts fine (n = 0); any non-empty sort is
  // BudgetExhausted (n > capacity) — the stopped-state pattern of the
  // module's value objects (IsoCamera), total and never UB.
  DepthSort() noexcept = default;

  // The set-up path: one flat allocation of 4 * capacity * 4 bytes
  // (kDepthSortBytesPerSlot per slot) + the fixed 1 KB counter table
  // (a class member — no heap). O(capacity) time and space.
  //
  // Fails (InvalidArgument, no allocation) when capacity is 0 or
  // exceeds kDepthSortMaxCapacity (the index-width domain).
  [[nodiscard]] static Result<DepthSort> create(std::size_t capacity)
      noexcept;

  // The per-frame path: stably sorts the frame's n keys (0 <= n <=
  // capacity) into the internal storage. O(4n + 4*256); zero
  // allocation; no logging; no GL. Deterministic (the preamble).
  //
  // Fails (BudgetExhausted, the sorter UNCHANGED — the previous
  // frame's sorted order is intact) when n > capacity. The Status is
  // the failure channel (the caller handles and logs it — LOG-002).
  [[nodiscard]] Status sort(std::span<const std::uint32_t> keys) noexcept;

  // The frame's sorted order after sort() (O(1) accessors; the spans
  // alias the internal storage and are invalidated by the next
  // sort() — read them within the frame):
  //
  //   sortedKeys()[i]     the i-th key in back-to-front order
  //   sortedIndices()[i]  the input position of that key (0..n-1)
  //
  // Both spans have sortedCount() elements (0 after a n = 0 sort; the
  // previous frame's order after a failed sort — unchanged).
  [[nodiscard]] std::size_t sortedCount() const noexcept { return count_; }
  [[nodiscard]] std::span<const std::uint32_t> sortedKeys() const noexcept {
    return std::span<const std::uint32_t>(curKeys_, count_);
  }
  [[nodiscard]] std::span<const std::uint32_t> sortedIndices() const
      noexcept {
    return std::span<const std::uint32_t>(curIdx_, count_);
  }
  // The sorter's capacity (the create() argument).
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  DepthSort(const DepthSort&) = delete;
  DepthSort& operator=(const DepthSort&) = delete;
  // Explicit move (the raw buffer pointers make the defaulted move
  // unsafe for the moved-from object — it would dangle into the
  // destination, the IsoDepthKeyTable pattern): the owner moves with
  // the storage; the source becomes empty (buffers null, count 0,
  // capacity 0 — CORE-009).
  DepthSort(DepthSort&& other) noexcept { *this = std::move(other); }
  DepthSort& operator=(DepthSort&& other) noexcept {
    if (this != &other) {
      store_ = std::move(other.store_);
      capacity_ = other.capacity_;
      count_ = other.count_;
      curKeys_ = other.curKeys_;
      curIdx_ = other.curIdx_;
      auxKeys_ = other.auxKeys_;
      auxIdx_ = other.auxIdx_;
      other.curKeys_ = nullptr;
      other.curIdx_ = nullptr;
      other.auxKeys_ = nullptr;
      other.auxIdx_ = nullptr;
      other.capacity_ = 0;
      other.count_ = 0;
    }
    return *this;
  }

 private:
  // Constructed by create() only (the capacity is validated there):
  // one flat allocation, the four buffers at fixed strides.
  explicit DepthSort(std::size_t capacity) noexcept
      : capacity_(capacity),
        store_(std::make_unique<std::uint32_t[]>(
            static_cast<std::size_t>(kDepthSortBytesPerSlot / 4u) *
            capacity)) {
    curKeys_ = store_.get();
    auxKeys_ = curKeys_ + capacity_;
    curIdx_ = auxKeys_ + capacity_;
    auxIdx_ = curIdx_ + capacity_;
  }

  // The frame's n keys into the (key, index) records (index = the
  // input position 0..n-1 — the stable tie-break's carrier).
  void loadRecords(const std::uint32_t* keys, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      curKeys_[i] = keys[i];
      curIdx_[i] = static_cast<std::uint32_t>(i);
    }
  }

  // One stable counting (bucket) pass over the pass's 8-bit digit
  // (the preamble's algorithm): count (O(n)), in-place prefix starts
  // (O(256)), stable scatter into the auxiliary buffers in INPUT
  // order (O(n)), then the role swap.
  void runPass(std::uint32_t shift, std::size_t n) noexcept {
    std::fill_n(counts_, kDepthSortBuckets, 0u);
    for (std::size_t i = 0; i < n; ++i) {
      ++counts_[(curKeys_[i] >> shift) & (kDepthSortBuckets - 1u)];
    }
    // In-place prefix: counts_[b] becomes the start position of
    // bucket b (the running total before b). One counter array serves
    // both phases — the fixed 1 KB table is the only per-pass state.
    std::uint32_t sum = 0;
    for (std::uint32_t b = 0; b < kDepthSortBuckets; ++b) {
      const std::uint32_t c = counts_[b];
      counts_[b] = sum;
      sum += c;
    }
    // Stable scatter: input order, so same-digit records keep their
    // relative order (the LSD invariant, the preamble).
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint32_t b =
          (curKeys_[i] >> shift) & (kDepthSortBuckets - 1u);
      const std::uint32_t p = counts_[b]++;
      auxKeys_[p] = curKeys_[i];
      auxIdx_[p] = curIdx_[i];
    }
    // The role swap (4 passes = even: the result lands back in the
    // base buffers — the static_assert in the preamble).
    std::swap(curKeys_, auxKeys_);
    std::swap(curIdx_, auxIdx_);
  }

  std::size_t capacity_{0};  // 0 in the default (empty) state
  std::size_t count_{0};  // the last successful sort's n
  // The flat record storage: 4 * capacity uint32 (the constructor's
  // layout). 16 bytes per slot (kDepthSortBytesPerSlot).
  std::unique_ptr<std::uint32_t[]> store_;
  // The current-buffer pointers (the role swap of runPass mutates
  // these — at rest after a sort they point at the sorted records in
  // the base buffers).
  std::uint32_t* curKeys_{nullptr};
  std::uint32_t* auxKeys_{nullptr};
  std::uint32_t* curIdx_{nullptr};
  std::uint32_t* auxIdx_{nullptr};
  // The fixed 1 KB counter table (a class member — pre-allocated,
  // never on the heap, zeroed per pass — zero per-frame allocation,
  // PERF-003).
  std::uint32_t counts_[kDepthSortBuckets]{};
};

// ---------------------------------------------------------------------------
// Implementation (the class is header-only — the sort is pure
// integer arithmetic over the pre-allocated buffers)
// ---------------------------------------------------------------------------

inline Result<DepthSort> DepthSort::create(std::size_t capacity) noexcept {
  // Capacity validation (first failure wins, InvalidArgument — no
  // allocation on the failure path): the index-width domain.
  if (capacity < 1 || capacity > kDepthSortMaxCapacity) {
    return Result<DepthSort>::failure(ErrorCode::InvalidArgument);
  }
  return Result<DepthSort>::success(DepthSort(capacity));
}

inline Status DepthSort::sort(std::span<const std::uint32_t> keys) noexcept {
  const std::size_t n = keys.size();
  // The capacity bound (BudgetExhausted — the sorter stays unchanged:
  // the previous frame's sorted order is intact; the caller handles
  // and logs this — the preamble's failure section).
  if (n > capacity_) {
    return Status(ErrorCode::BudgetExhausted);
  }
  loadRecords(keys.data(), n);
  // The 4 LSD passes, least significant digit first (the preamble's
  // algorithm and invariant):
  for (std::int32_t pass = 0; pass < kDepthSortPasses; ++pass) {
    runPass(static_cast<std::uint32_t>(pass) *
                static_cast<std::uint32_t>(kDepthSortDigitBits),
            n);
  }
  count_ = n;
  return Status{};
}

}  // namespace laige::render
