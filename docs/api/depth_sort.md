# Deterministic depth sort (`laige::render::DepthSort`)

The stable, deterministic, pre-allocated sorter for 32-bit isometric
depth keys (M2-SORT-01; FR-2.2 "deterministic, stable z-order ...
bucket/radix sort, no per-frame allocation"; AGENTS RENDER-003,
API-001/006, CORE-005/008/009, PERF-002/003/004, DOC-004, S-5/G-R11).
Public header: `src/laige-render/include/laige/render/depth_sort.h`
(header-only — the sort is pure integer arithmetic over the
pre-allocated buffers; there is no implementation file). Unit suite:
`ctest -R depth_sort` (`tests/laige-render/depth_sort_tests.cpp`) —
pure integer math, no GL environment required: it runs in every
local tree and in CI, with the hand-computed sorted order, the
0/1/all-equal edge cases, the determinism property on 64 shuffled
inputs against the stable-sort oracle, the 10k-key oracle comparison
+ the zero-allocation proof, and the PRD §8.1 `depth_sort_10k` budget
gate.

The sort is the render-side half of the M2-ISO-01 depth-key design:
the KEY is the world-space order value (PRD §4, G-R11); the SORT is
what turns the per-sprite keys into the frame's back-to-front draw
order. The engine owns both (G-R11, S-5) — game code never hand-rolls
the ordering, and never sorts screen-space coordinates.

## The API

```cpp
class DepthSort {
 public:
  // Default state: the EMPTY sorter (capacity 0, no storage).
  DepthSort() noexcept = default;

  // Set-up path: one flat allocation of 16 bytes per capacity slot.
  [[nodiscard]] static Result<DepthSort> create(std::size_t capacity) noexcept;

  // Per-frame path: stably sort the frame's n keys (0 <= n <= capacity)
  // into the internal storage. O(4n + 4*256), zero allocation.
  [[nodiscard]] Status sort(std::span<const std::uint32_t> keys) noexcept;

  // The sorted order after sort() (invalidated by the next sort()):
  [[nodiscard] std::size_t sortedCount() const noexcept;
  [[nodiscard] std::span<const std::uint32_t> sortedKeys() const noexcept;
  [[nodiscard] std::span<const std::uint32_t> sortedIndices() const noexcept;
  [[nodiscard] std::size_t capacity() const noexcept;
  // move-only (copy deleted)
};
```

- **`create(capacity)`** is the **set-up path** (scene load): one flat
  allocation of `4 * capacity` 32-bit words (16 B/slot — 800 KB at
  50k), plus the fixed 1 KB counter table (a class member, never on
  the heap). Fails with `InvalidArgument` (no allocation) when
  `capacity` is 0 or exceeds `kDepthSortMaxCapacity` (0xFFFFFFFF —
  the sorted-index field is 32-bit, so `n` positions must fit it).
  Size the capacity to the scene's sprite budget (API-006); the batcher
  (M2-SPRITE-01) creates one sorter per sprite pass at set-up.
- **`sort(keys)`** is the **per-frame path**: copies the frame's
  `n` keys into the storage tagged with their input positions
  0..n-1, and runs the stable radix sort (below). The sorted order is
  read back as `sortedKeys()` / `sortedIndices()` — parallel spans,
  `sortedKeys()[i]` the i-th key in back-to-front order,
  `sortedIndices()[i]` that key's ORIGINAL input position. The batcher
  walks the sorted order and maps each index to its sprite pool entry;
  the sorter itself is payload-agnostic (it permutes keys +
  positions, nothing else).
- **Failure** (`sort`): `n > capacity` → `BudgetExhausted`, the sorter
  is **unchanged** (the previous frame's sorted order is intact). No
  log inside the hot path — the `Status` is the failure channel; the
  batcher handles and logs it (LOG-002), e.g. by clamping the frame's
  visible set to capacity (its own documented overflow policy,
  M2-SPRITE-01).

## The stable tie-break (RENDER-003)

The total render order is the lexicographic **(key, entity id)**
tuple (`isoDepthOrderLess`, `docs/concepts/coordinates.md` §4.3). This
sort realizes it in two halves:

1. this sort is **stable**: equal keys keep their **input** order;
2. the batcher inserts the frame's keys in the engine's
   **deterministic entity-id iteration order** (FR-1.2),

so equal-key sprites (same screen row) are ordered by entity id —
without the sorter ever seeing the ids (the input position IS the
entity order). A non-stable sort would make equal-key sprites flicker
between frames; the stability property is what makes the render order
a pure function of (keys, insertion order).

## The algorithm: 4 × 8-bit LSD radix (stable bucket) passes

One **stable counting (bucket) sort** per 8-bit digit, least
significant digit first (LSD), 256 buckets per pass, 4 passes over the
32-bit key:

1. **count** the 256 digit counts of the current buffer (O(n));
2. **prefix** the in-place cumulative start positions (O(256));
3. **scatter** the (key, index) records into the auxiliary buffer in
   INPUT order — record `i` lands at `start[digit]` + its rank among
   earlier same-digit records: a stable pass;
4. **swap** the roles of the two buffers.

Invariant (the standard LSD radix-sort argument — Knuth, *The Art of
Computer Programming* Vol. 3 §7.2.1): after the pass on digit `k`, the
buffer is sorted by the low `k*8` bits, and records with equal low
bits keep their input order (stability). By induction over the 4
passes, the final buffer is sorted by all 32 bits, stably over the
whole key — exactly the stable sort of the input key sequence. The 4
passes are even, so the result lands back in the base buffers (no
O(n) copy-out — pinned by a `static_assert` in the header).

**8-bit digits** (256 buckets) are the fixed-cost sweet spot: 1 KB of
counter state zeroed per pass (trivial), and the passes are
data-movement-bound — 4-bit digits would double the passes over the
data, and 16-bit digits would zero a 256 KB counter table per frame
for no measured gain. The digit width is free to change (the storage
layout only requires an EVEN pass count — the `static_assert`).

## Determinism (RENDER-003, ARCH-009/010)

A **pure function of the input (key sequence, n)**: integer
arithmetic only — no floating point, no hashing, no RNG, no timing, no
platform-dependent container order. Same input sequence → bit-
identical sorted order, on every platform, every build (scope:
render-side presentation state — never part of the sim state hash or
replay state; ARCH-009). The SAME multiset in a DIFFERENT input order
yields a different (equally valid) stable order — the order is a
function of the SEQUENCE; with the batcher's entity-id insertion
order, two frames with the same scene state produce the same sorted
order. The property test pins this on 64 shuffled inputs against the
stable-sort oracle (`DepthSortStability`).

## Performance (PERF-002/003/004, DOC-004)

- **Complexity:** `create` O(capacity) (one flat allocation, 16 B/slot);
  `sort` **O(4n + 4·256)** — 4 count passes + 4 scatters + 4 prefix
  walks (1024 compares) over contiguous pre-allocated buffers.
- **Allocation:** zero per frame (PERF-003) — proven by the
  zero-allocation test (1 000 consecutive 10k sorts = 0 heap blocks,
  non-sanitizer trees).
- **Blocking/IO/GPU:** none — pure integer arithmetic; no logging on
  the hot path (LOG-003: disabled cost zero).
- **Budget:** 10 000 keys sorted, **mean ≤ 1.0 ms** (PRD §8.1,
  `depth_sort_10k` budgets.json entry — 6% of the 16.7 ms 60 FPS
  frame budget of AC-4.3, half of the 2 ms 50k render-CPU budget: the
  sort must stay a minority cost of the render path). Measured on the
  reference platform: **0.225852 ms mean** (the canonical Debug g++
  tree, n=3000 sorts of 10k keys — baseline
  [m2-depth-sort.md](../benchmarks/baselines/m2-depth-sort.md));
  4.4× inside the target. The 3 000 measured sorts ARE the roadmap's
  stress test (10k sprites sorted per frame for 3 000 frames, part of
  AC-4.3).
- **Call site:** once per frame per sprite pass, in the render phase
  (the M2-SPRITE-01/02 batch stage) — never per sprite, never in the
  simulation tick (ARCH-002).

## Ownership, lifetime, threading (CORE-009, CONC-001)

- **Owner:** one sorter per sprite pass — the batcher (M2-SPRITE-01)
  owns it and its storage (move-only; copy deleted).
- **Phases:** `create` at scene set-up; `sort` in the render phase
  (the frame pipeline's tick → handoff → render ordering, M2-GL-02).
  The phases never overlap; the sorter is never shared with a
  concurrent writer — not thread-safe by design (the single-owner
  pattern of `IsoDepthKeyTable` and the batcher).
- **Lifetime:** the sorted spans alias the internal storage and are
  invalidated by the next `sort()` — read them within the frame.
- **Moved-from state:** empty (capacity 0) — a moved-from sorter
  rejects non-empty sorts with `BudgetExhausted` (total, never UB).

## Misuse warnings

- **Do not sort screen-space coordinates or per-sprite floats**
  (PRD §4, G-R11): the inputs are the world-space M2-ISO-01 keys —
  the order contract is their unsigned integer order.
- **Do not feed more keys than `create` sized it for** — every
  overflowing frame is a `BudgetExhausted` (the sorter stays unchanged
  and the batcher must handle it); size to the scene's sprite budget
  at set-up.
- **Do not rely on the order across DIFFERENT input sequences with
  the same multiset** — stability is per-sequence; the batcher's
  entity-id insertion order is what makes the per-frame order
  reproducible.
- **Do not sort before the frame's keys are final** (the keys are the
  interpolated presentation positions' M2-ISO-01 keys —
  `docs/api/iso_depth_key.md`) — sorting mid-frame against stale keys
  renders a stale order (the frame pipeline's ordering contract).

## Related

- [`api/iso_depth_key.md`](iso_depth_key.md) — the 32-bit depth key
  this sort orders (M2-ISO-01).
- [`concepts/coordinates.md` §4.6](../concepts/coordinates.md) — the
  render-order narrative (ARCH-008).
- [`api/iso_depth_table.md`](iso_depth_table.md) — the precomputed
  tile-grid keys the batcher reads for static terrain (M2-ISO-02).
- [`benchmarks/baselines/m2-depth-sort.md`](../benchmarks/baselines/m2-depth-sort.md)
  — the recorded `depth_sort_10k` baseline.
