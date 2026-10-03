# Sprite batcher (`laige::render::SpriteBatcher`)

The engine-owned "declare, don't draw" sprite declaration window +
batch builder (M2-SPRITE-01; S-5: "Rendering goes through the
batcher. Scene content is declared; the engine batches"; FR-2.1:
"one draw call per (atlas, material, blend) group per frame"; FR-2.2
"no per-frame allocation"; G-R11: per-sprite depth overrides counted
+ warned; AGENTS RENDER-001/003, PERF-003/008, CORE-005/008/009,
API-001/006, DOC-004). Public header:
`src/laige-render/include/laige/render/sprite_batcher.h` (header-only
— the batch is pure integer bookkeeping over pre-allocated storage;
there is no implementation file). Unit suite: `ctest -R batcher`
(`tests/laige-render/sprite_batcher_tests.cpp`) — pure integer
bookkeeping, no GL environment required: it runs in every local tree
and in CI, with the grouping correctness, the in-group order, the
drop-oldest overflow policy, the G-R11 counted + warned override, the
stopped-state / protocol edges, the determinism property against the
stable-sort oracle, and the 1 000-frame zero-allocation proof.

The batcher is the render-side consumer of the M2-SORT-01 sorter: the
sorter turns the frame's 32-bit depth keys into the back-to-front
order, and the batcher turns that order into the frame's (atlas,
material, blend) GROUPS — one instanced draw call per group at submit
(M2-SPRITE-02). The engine owns both (S-5, G-R11) — game code declares
sprites; it never batches, sorts, or draws.

## The API

```cpp
struct SpriteItem {
  Vec2 pos{};                    // world ground-plane position, world units
  std::uint32_t depthKey{};      // the M2-ISO-01 key (isoDepthKey<Backend>)
  bool depthOverride{};          // G-R11 escape hatch (counted + warned)
  SpriteUvRect uv{};             // UV sub-rect in the atlas, [0,1]^2
  float rotation{};              // radians
  Vec2 scale{1, 1};              // world-unit scale (x, y)
  SpriteTint tint{};             // multiplicative RGBA (1,1,1,1 = none)
  std::uint32_t atlasId{};       // atlas/texture ref (group key field 1)
  std::uint32_t materialId{};    // material ref (group key field 2)
  BlendMode blend{};             // group key field 3 (Alpha default)
};

struct SpriteBatch {             // one output group (the submit stage's read)
  std::uint32_t atlasId{};
  std::uint32_t materialId{};
  BlendMode blend{};
  std::span<const std::uint32_t> instances{};  // pool slots, back-to-front
};

class SpriteBatcher {
 public:
  struct Options { std::uint32_t maxSprites{}; };  // the frame budget
  SpriteBatcher() noexcept;                        // stopped state (capacity 0)
  [[nodiscard]] static Result<SpriteBatcher> create(Options) noexcept;
  void beginFrame() noexcept;                      // open the frame window
  [[nodiscard]] Result<std::uint32_t> add(SpriteItem item) noexcept;
  [[nodiscard]] Status build() noexcept;          // sort + group + publish
  [[nodiscard]] std::size_t batchCount() const noexcept;
  [[nodiscard]] std::span<const SpriteBatch> batches() const noexcept;
  [[nodiscard]] std::size_t frameCount() const noexcept;
  [[nodiscard]] std::uint32_t overrideCount() const noexcept;  // this frame (G-R11)
  [[nodiscard]] std::uint64_t overrideTotal() const noexcept;  // cumulative (G-R11)
  [[nodiscard]] std::uint64_t droppedTotal() const noexcept;   // cumulative overflow
  [[nodiscard]] std::uint32_t capacity() const noexcept;
  [[nodiscard]] PoolStats itemPoolStats() const noexcept;
  [[nodiscard]] const SpriteItem* get(std::uint32_t slot) noexcept;  // null-safe
  [[nodiscard]] const SpriteItem& at(std::uint32_t slot);            // asserts live
  // move-only (copy deleted)
};
```

The frame protocol (the frame pipeline's cull/batch stage, M2-GL-02):
`beginFrame()` → `add(item)` × n → `build()`, per frame.

- **`create(options)`** is the **set-up path** (scene load): one
  allocation per storage structure (the sprite pool, the M2-SORT-01
  sorter, the key scratch, the instance array, the group table, the
  cursor array, the batch array — ~132 bytes per capacity slot, 6.6 MB
  at the 50k stress budget). Fails with `InvalidArgument` (no
  allocation) when `maxSprites` is 0 or exceeds
  `kSpriteBatcherMaxCapacity` (0xFFFFFFFF — the slot width). Size the
  budget to the scene's worst-case visible count (API-006); the
  batcher never grows beyond it.
- **`beginFrame()`** closes the previous window and opens a fresh one
  (the previous frame's declared items are released — the pool's
  reset). Idempotent; a no-op on the stopped batcher.
- **`add(item)`** declares one sprite for the current frame and
  returns its frame-scoped slot. The frame's **declaration order is
  the insertion order**: declare in the engine's deterministic
  entity-id iteration order (FR-1.2) — that is what makes the
  (key, entity id) total order reproducible (RENDER-003). Overflow
  policy: a frame beyond the budget **drops the oldest declaration**
  (the ring head is overwritten) and takes the new one, one
  rate-limited Warn per drop (`sprite_batcher/frame_overflow_dropped`)
  — bounded, never growing (PERF-008, S-2). Fails: capacity 0 →
  `BudgetExhausted`; window closed (after `build`) → `InvalidArgument`.
- **`build()`** is the **batch stage**: sorts the frame's depth keys
  (M2-SORT-01), groups into (atlas, material, blend) batches (the
  deterministic group order below), scatters each group's instances in
  global back-to-front order, and publishes `batches()`. Closes the
  declaration window. Fails: double build → `InvalidArgument`; sort
  overflow → `BudgetExhausted` (unreachable — the ring keeps
  n ≤ capacity).
- **Output** (`batchCount()`, `batches()`, `get(slot)`, `at(slot)`):
  read **within the frame** — the next `beginFrame()`/`build()`
  invalidates it. The instance spans alias the batcher's storage.
- **G-R11 accounting**: `overrideCount()` (this frame, reset by
  `beginFrame`) and `overrideTotal()` (cumulative) — the profiler's
  feeds (PRD §10.4); `droppedTotal()` (cumulative overflow);
  `itemPoolStats()` (the pool's accounting snapshot).

## Grouping and order (RENDER-001/003)

The frame's GROUPS are the DISTINCT (atlas, material, blend)
combinations of its declared sprites. One instanced draw call per
group per frame (FR-2.1, M2-SPRITE-02): the per-group state (texture
bind, material, blend) is set ONCE, so texture binds and blend changes
are O(group count), not O(sprite count) (RENDER-001).

- **Group order**: ascending (atlas, material, blend) — a function of
  the DISTINCT group keys alone (independent of membership and
  insertion order). The submit stage walks the groups in this order.
- **In-group instance order**: the frame's GLOBAL back-to-front order
  (M2-SORT-01) **restricted to the group** — the (key, declaration
  order) total order of `iso_depth_key.h`, per group.

**Determinism** (RENDER-003, ARCH-010 scope — presentation-only, never
the sim state hash or replay state): a pure function of the
declaration SEQUENCE (items, order) — integer arithmetic only (the
item's floats are carried through untouched), no hashing, no RNG, no
platform container order. Same declaration sequence → bit-identical
batches, every platform/build (the `SpriteBatcherDeterminism` suite
pins this on 3 000-sprite frames against the stable-sort oracle). The
SAME multiset in a DIFFERENT declaration order yields a different
(equally valid) in-group order for equal keys — the batcher's
entity-id insertion order is what makes the per-frame order
reproducible.

## Overflow: bounded, drop oldest + warn (PERF-008, S-2)

The frame budget is fixed at set-up. A frame that declares more than
the budget does not grow, fail, or truncate silently: each excess
declaration **drops the oldest live declaration** (the ring head) and
takes the new one — one rate-limited Warn per drop
(`sprite_batcher/frame_overflow_dropped`, LOG-004), the cumulative
count in `droppedTotal()`. The visible set is bounded by
construction; the degradation is logged (CORE-008) and accounted
(PRD §10.4). Size the budget to the scene's worst case at set-up — a
repeatedly overflowing scene is a budget bug, not a normal state.

## G-R11: the counted + warned depth-override escape hatch

The normal path: the caller computes the key with the engine-owned
`isoDepthKey<Backend>` (M2-ISO-01) and declares it. The escape hatch:
the caller sets `depthKey` by hand and flags `depthOverride = true`.
The batcher SORTS the key either way (the order is what the key says)
but COUNTS every override declaration (`overrideCount()` per frame,
`overrideTotal()` cumulative) and WARNS once per frame that overrides
were used (`sprite_batcher/depth_override_used`, rate-limited —
LOG-004), advising "prefer tile height" (PRD §9.3 G-R11: per-sprite
depth overrides counted + warned).

## Performance (PERF-002/003/004, DOC-004)

- **Complexity:** `create` O(maxSprites) (8 allocations, ~132 B/slot);
  `beginFrame` O(previous frame count) (the pool reset); `add`
  **O(1)** (one pool create or one ring overwrite); `build`
  **O(4n + 4·256)** (the M2-SORT-01 sort) **+ O(n·log G)** (two group
  lookups per instance) **+ O(G·(log G + G))** (the group-table
  growth: a binary search + an O(G) descriptor shift per NEW group)
  **+ O(n)** (start prefix + instance scatter) — with n = the frame's
  declared sprites (≤ capacity) and G = the frame's DISTINCT
  (atlas, material, blend) count.
- **Allocation:** zero per frame (PERF-003) — proven by the
  zero-allocation test (1 000 frames × 512 declarations = 0 heap
  blocks, non-sanitizer trees; the sanitizer trees run the same
  workload leak-free).
- **Blocking/IO/GPU:** none — pure integer bookkeeping; the only
  logging is the two cold, rate-limited Warn paths (LOG-003: a
  disabled event costs one atomic load + branch).
- **Budget:** the step has no standalone `budgets.json` entry — the
  sort cost is the M2-SORT-01 `depth_sort_10k` budget (10k keys sorted
  mean ≤ 1.0 ms, 4.4× inside), and the composite 50k render-CPU budget
  (2 ms, PRD §8.1 `sprites_50k_cpu`) is measured when the submit stage
  lands (M2-PERF-01). The grouping adds O(n log G) — G ≪ n in every
  realistic scene (the 50k-stress budget caps draw calls at 30).
- **Call site:** once per frame per sprite pass, in the render phase
  (the frame pipeline's cull/batch stage) — never per sprite, never in
  the simulation tick (ARCH-002).

## Ownership, lifetime, threading (CORE-009, CONC-001)

- **Owner:** one batcher per sprite pass — owned by the render
  thread's cull/batch stage (the frame pipeline's stage callback,
  M2-GL-02). Move-only; copy deleted.
- **Phases:** `create` at scene set-up; `beginFrame` / `add` /
  `build` in the render phase (after the tick→handoff, before the
  submit stage). The phases never overlap; the batcher is never shared
  with a concurrent writer — not thread-safe by design (the
  single-owner pattern of `DepthSort` and `IsoDepthKeyTable`).
- **Lifetime:** the declared items die with the frame (the pool's
  reset at `beginFrame`); the since-construction counters
  (`overrideTotal()`, `droppedTotal()`) and the pool's accounting
  survive. The output spans alias the batcher's storage — read within
  the frame.
- **Moved-from state:** the stopped state (capacity 0) — total, never
  UB (add → `BudgetExhausted`; build → empty output).

## Misuse warnings

- **Do not hand-roll z-ordering in game code (G-R11):** compute the
  key with `isoDepthKey<Backend>` (M2-ISO-01). The manual depthKey +
  `depthOverride` is the counted + warned escape hatch ("prefer tile
  height") — not the default path.
- **Do not derive the key from screen-space coordinates (PRD §4):**
  the key is world-space by contract (the M2-ISO-01 domain).
- **Declare in the deterministic entity-id iteration order (FR-1.2):**
  a nondeterministic declaration order makes the equal-key (same
  screen row) order nondeterministic — RENDER-003 breaks in exactly
  the places painter's order is visible.
- **Do not declare more than the frame budget without a real reason:**
  overflow drops the OLDEST declarations (the scene is visibly
  truncated) — size the budget to the scene's worst-case visible count
  at set-up (API-006).
- **Do not carry a slot across frames:** the slot is frame-scoped
  (the ring reuses it, the next `beginFrame` releases it). Read items
  within the frame.

## Related

- [`api/depth_sort.md`](depth_sort.md) — the M2-SORT-01 stable radix
  sort this batcher consumes.
- [`api/iso_depth_key.md`](iso_depth_key.md) — the 32-bit depth key
  the items carry (M2-ISO-01).
- [`concepts/coordinates.md` §4.7](../concepts/coordinates.md) — the
  render-order narrative (ARCH-008).
- [`api/frame_pipeline.md`](frame_pipeline.md) — the render thread +
  frame handoff the batcher plugs into (M2-GL-02).
- [`api/pools.md`](pools.md) — the pool storage behind the declared
  items (M0-CORE-05).
