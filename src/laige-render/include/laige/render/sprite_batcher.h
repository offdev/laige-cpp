// laige-render sprite batcher (M2-SPRITE-01): the engine-owned
// "declare, don't draw" sprite declaration window + batch builder.
//
// S-5 (PRD §9.1): "Rendering goes through the batcher. Scene content
// is declared (layers, sprites, tiles, depth, material); the engine
// batches. There is no 'draw this quad now' call in the safe API."
// FR-2.1: "Batched textured quads; one draw call per (atlas, material,
// blend) group per frame; GPU-instanced; supports rotation, scale,
// tint, per-sprite depth, UV sub-rect". FR-2.2: "no per-frame
// allocation". G-R11: isometric depth is engine-owned — the per-sprite
// depth override exists as a COUNTED + WARNED escape hatch ("prefer
// tile height"). RENDER-003: deterministic ordering, explicit stable
// tie-break. RENDER-001: one draw call per group (the submit stage's
// read, M2-SPRITE-02).
//
//   BlendMode         The per-sprite blend state (group-key field 3)
//   SpriteUvRect      The per-sprite UV sub-rect in the atlas
//   SpriteTint        The per-sprite RGBA tint
//   SpriteItem        One declared sprite (the value the game declares)
//   SpriteBatch       One output group: (atlas, material, blend) + the
//                     in-group instance list (back-to-front)
//   SpriteBatcher     The frame declaration window + batch builder
//
// ---------------------------------------------------------------------------
// The frame protocol (the frame pipeline's cull/batch stage, M2-GL-02)
// ---------------------------------------------------------------------------
//
// One batcher per sprite pass, owned by the render thread's cull/batch
// stage (the frame pipeline's stage callback). Per frame:
//
//   beginFrame()    close the previous window, open a new one (the
//                   previous frame's declared items are released —
//                   the pool's reset)
//   add(item) × n   declare the frame's n sprites IN THE ENGINE'S
//                   DETERMINISTIC ENTITY-ID ITERATION ORDER (FR-1.2) —
//                   the declaration order is the insertion order that
//                   M2-SORT-01's stability turns into the (key, entity
//                   id) total order (RENDER-003)
//   build()         sort the frame's depth keys (M2-SORT-01), group
//                   into (atlas, material, blend) batches, publish
//                   batches() for the submit stage (M2-SPRITE-02) —
//                   frameBuilt() gates that submit
//
// The declared items are PRESENTATION state (ARCH-009): the game
// re-declares from its per-frame snapshot (the interpolated positions,
// M1-LOOP-02) — nothing in the batcher survives a frame except the
// since-construction counters (G-R11/G-R4-style accounting, PRD §10.4).
//
// ---------------------------------------------------------------------------
// The batch model: groups, order, determinism (RENDER-001/003)
// ---------------------------------------------------------------------------
//
// build() produces the frame's GROUPS — one per DISTINCT
// (atlas, material, blend) combination — and, per group, the group's
// instances in the frame's GLOBAL back-to-front order (the M2-SORT-01
// sorted order RESTRICTED to the group). The submit stage then makes
// one instanced draw call per group (FR-2.1, M2-SPRITE-02): the
// per-group (atlas, material, blend) state is set once, so texture
// binds and blend changes are O(group count), not O(sprite count)
// (RENDER-001).
//
// Deterministic (RENDER-003, ARCH-010 scope — presentation-only, never
// the sim state hash or replay state):
//
//   - the SORT is the M2-SORT-01 stable radix sort: a pure function of
//     the key SEQUENCE (same sequence → bit-identical order, every
//     platform/build);
//   - the group TABLE is built in ascending (atlas, material, blend)
//     order — the group order is a function of the DISTINCT group
//     keys alone (independent of the frame's membership or insertion
//     order);
//   - the in-group INSTANCE order is the global sorted order
//     restricted to the group: equal keys keep their DECLARATION
//     order (the stable sort + the FR-1.2 entity-id insertion order —
//     the (key, entity id) total order of iso_depth_key.h).
//
// Two frames with the same scene state (same items, same declaration
// order) produce bit-identical batches. Pure integer arithmetic over
// the item data: the item's floats (pos, uv, rotation, scale, tint)
// and its animation frame index (frameIndex — M2-SPRITE-03) are
// carried through untouched — no float arithmetic here.
//
// ---------------------------------------------------------------------------
// Overflow: bounded, drop oldest + warn (PERF-008, S-2)
// ---------------------------------------------------------------------------
//
// The frame budget is fixed at set-up (Options::maxSprites — the
// declared scene budget, S-6, API-006). A frame that declares MORE
// than the budget does not grow, fail, or truncate silently: each
// excess declaration DROPS THE OLDEST live declaration (the ring head
// is overwritten) and takes the new one — one rate-limited Warn per
// drop (LOG-004), the cumulative count in droppedTotal(). The visible
// set is bounded by construction (never unbounded, PERF-008); the
// degradation is logged (CORE-008) and accounted (PRD §10.4).
//
// ---------------------------------------------------------------------------
// G-R11: the counted + warned depth-override escape hatch
// ---------------------------------------------------------------------------
//
// The normal path: the caller computes the item's depth key with the
// engine-owned `isoDepthKey<Backend>` (M2-ISO-01) and declares it. The
// escape hatch: the caller may set `depthKey` BY HAND (e.g. a
// hand-tuned UI z) and flag `depthOverride = true`. The batcher
// SORTS the key either way (the order is what the key says) but COUNTS
// every override declaration (overrideCount() per frame, overrideTotal()
// cumulative) and WARNS once per frame that overrides were used
// (`sprite_batcher/depth_override_used`, rate-limited — LOG-004),
// advising "prefer tile height" (PRD §9.3 G-R11: per-sprite depth
// overrides counted + warned).
//
// ---------------------------------------------------------------------------
// Storage layout (CORE-005, PERF-004; ~136 bytes per capacity slot —
// 6.8 MB at the 50k stress budget, PRD §8.1)
// ---------------------------------------------------------------------------
//
//   sprite pool    ArenaPool<SpriteItem>, 76 B/slot (budgeted,
//                  accounted — PRD §10.4; the pool's reset() is the
//                  per-frame release)
//   key scratch    capacity u32 (the frame's keys in declaration order
//                  — the M2-SORT-01 input)
//   instance order capacity u32 (the published per-group instance
//                  segments, back-to-front)
//   group table    capacity GroupDesc (20 B: atlas, material, blend,
//                  start, count — the working table; the published
//                  view is SpriteBatch)
//   group cursor   capacity u32 (the scatter cursor, zeroed per group)
//   batch array    capacity SpriteBatch (28 B: the published groups)
//
// One allocation per structure at set-up; EVERY per-frame path
// (beginFrame, add, build) allocates nothing (PERF-003 — the
// zero-allocation proof test).
//
// ---------------------------------------------------------------------------
// Ownership, threading, failure (CORE-009, CONC-001, CORE-008)
// ---------------------------------------------------------------------------
//
// One owner: the render thread's cull/batch stage (the frame pipeline,
// M2-GL-02). Set-up: `create` (one allocation per structure). Render
// phase: `beginFrame` / `add` / `build` (the declaration + batch
// stages — after the tick→handoff, before the submit stage). The phases
// never overlap and the batcher is never shared with a concurrent
// writer — not thread-safe by design (the single-owner pattern of
// DepthSort and IsoDepthKeyTable).
//
// Failure (no silent failure, CORE-008):
//
//   - create(maxSprites 0 or > kSpriteBatcherMaxCapacity) →
//     InvalidArgument (no allocation);
//   - add() on the EMPTY (default-constructed, capacity 0) batcher →
//     BudgetExhausted (the stopped-state pattern — total, never UB);
//   - add() with the window CLOSED (after build) → InvalidArgument
//     (call beginFrame first — the frame protocol is explicit);
//   - frame overflow → drop oldest + one rate-limited Warn (above);
//   - build() with the window closed (double build) → InvalidArgument;
//   - build() sort overflow (n > capacity) → BudgetExhausted —
//     unreachable: the ring keeps n ≤ capacity (the overflow policy
//     is what bounds n).
//
// The moved-from batcher is the stopped state (capacity 0 — the
// ArenaPool/DepthSort move contracts).
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003/004, DOC-004)
// ---------------------------------------------------------------------------
//
//   create     O(maxSprites): one allocation per structure (6 flat
//              buffers + the pool + the sorter). Setup path only.
//   beginFrame O(previous frame count): the pool reset (trivial
//              destructors) + counter resets. No allocation.
//   add        O(1): one pool create (first frame's fill) or one ring
//              overwrite. No allocation; the overflow Warn is a cold,
//              rate-limited path (LOG-003: a disabled event costs one
//              atomic load + branch).
//   build      O(4n + 4·256)  the M2-SORT-01 radix sort
//            + O(n·log G)     two group lookups per instance (binary
//                             search over the ≤ G-group table)
//            + O(G·(log G + G)) the group-table growth (a binary
//                             search + an O(G) descriptor shift per NEW
//                             group)
//            + O(n)           start prefix + instance scatter
//
//     n = the frame's declared sprites (≤ capacity), G = the frame's
//     DISTINCT (atlas, material, blend) count. G is the scene's
//     atlas × material × blend combination count — small by design
//     (the PRD §8.1 50k-stress budget caps draw calls at 30, so
//     G ≪ n in every realistic scene). Zero allocation, no logging on
//     the path, no locks, no GL.
//
//   Call site: once per frame per sprite pass, in the RENDER phase —
//   never per sprite, never in the simulation tick (ARCH-002). The
//   sort dominates (the M2-SORT-01 `depth_sort_10k` budget); the
//   grouping adds O(n log G) — the composite 50k render-CPU budget
//   (2 ms, PRD §8.1 `sprites_50k_cpu`) is measured when the submit
//   stage lands (M2-PERF-01).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - Do not hand-roll z-ordering in game code (G-R11): compute the
//     key with `isoDepthKey<Backend>` (M2-ISO-01). The manual
//     depthKey + depthOverride is the counted + warned escape hatch
//     ("prefer tile height") — not the default path.
//   - Do not derive the key from screen-space coordinates (PRD §4):
//     the key is world-space by contract (the M2-ISO-01 domain).
//   - Declare in the deterministic entity-id iteration order
//     (FR-1.2): a nondeterministic declaration order makes the
//     equal-key (same screen row) order nondeterministic — RENDER-003
//     breaks in exactly the places painter's order is visible.
//   - Do not declare more than the frame budget without a real
//     reason: overflow drops the OLDEST declarations (the scene is
//     visibly truncated) — size the budget to the scene's worst-case
//     visible count at set-up (API-006).
//   - Read the output (batches(), at(), get()) only WITHIN the frame —
//     the next beginFrame()/build() invalidates it.
//
// Canonical narrative: docs/concepts/coordinates.md §4.7 (ARCH-008);
// API contract: docs/api/sprite_batcher.md.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/pools.h"
#include "laige/render/depth_sort.h"
#include "laige/render/matrices.h"
#include "laige/result.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The most sprites a batcher may budget: the frame declaration slot is
// a 32-bit index (the pool slot, the key scratch, the instance array),
// so the budget domain is the index width (the DepthSort precedent).
inline constexpr std::uint32_t kSpriteBatcherMaxCapacity = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// The per-sprite blend state (FR-2.1: one draw call per
// (atlas, material, BLEND) group)
// ---------------------------------------------------------------------------

// The blend state a sprite is drawn with (the group key's third field).
// The enumerator values are stable (PRD §9.4: additive only); the
// submit stage (M2-SPRITE-02) maps each value to its GL blend state.
enum class BlendMode : std::uint8_t {
  // Standard alpha blend (src·srcAlpha + dst·(1 − srcAlpha)): the
  // default for textured sprites.
  Alpha = 0,
  // Additive (src + dst): particles, light, glow (M2-PART-01).
  Additive = 1,
};

// ---------------------------------------------------------------------------
// The per-sprite presentation values
// ---------------------------------------------------------------------------

// The sprite's UV sub-rect inside its atlas, normalized [0, 1]²
// (u1 > u0, v1 > v0 — the caller's invariant; spriteFrameUv,
// laige/render/sprite_frames.h, computes these from the atlas sheet
// frame layout — M2-SPRITE-03).
struct SpriteUvRect {
  float u0{};
  float v0{};
  float u1{};
  float v1{};
};

// The sprite's multiplicative RGBA tint (1, 1, 1, 1 = untinted).
struct SpriteTint {
  float r{1.0f};
  float g{1.0f};
  float b{1.0f};
  float a{1.0f};
};

// ---------------------------------------------------------------------------
// One declared sprite (M2-SPRITE-01)
// ---------------------------------------------------------------------------

// The value the game DECLARES for one sprite (S-5: declaration, not
// draw). A plain value — no ownership, nothing to release (the pool
// owns the storage). All fields are presentation state (ARCH-009):
// the caller fills them from its per-frame snapshot.
struct SpriteItem {
  // The world ground-plane position, world units (the presentation
  // snapshot's interpolated position, M1-LOOP-02 — NEVER screen
  // space, PRD §4).
  Vec2 pos{0.0f, 0.0f};
  // The M2-ISO-01 depth key (`isoDepthKey<Backend>(pos, stepHeight,
  // layer)`): the back-to-front order value. G-R11: engine-owned —
  // compute it with isoDepthKey, never from screen space.
  std::uint32_t depthKey{};
  // The G-R11 escape-hatch flag: true when the caller set depthKey by
  // hand (not via isoDepthKey). Counted per frame + warned at build
  // ("prefer tile height"). The batcher sorts the key either way.
  bool depthOverride{};
  // The UV sub-rect in the atlas (SpriteUvRect above).
  SpriteUvRect uv{};
  // The animation frame index this item was declared with
  // (M2-SPRITE-03 — data-driven: the caller sets it; M3 animation
  // will drive the frame advance). The engine does not interpret it:
  // the caller also sets `uv` to this frame's UV sub-rect (see
  // spriteFrameUv, laige/render/sprite_frames.h) — `uv` is what the
  // renderer draws, the index is the declaration's frame record
  // (presentation state, ARCH-009). The batcher carries it through
  // untouched.
  std::uint32_t frameIndex{};
  // The rotation in radians (0 = unrotated; the submit stage applies
  // it in screen space, M2-SPRITE-02).
  float rotation{};
  // The world-unit scale (x, y) — non-uniform free (1, 1 = unscaled).
  Vec2 scale{1.0f, 1.0f};
  // The multiplicative RGBA tint (SpriteTint above).
  SpriteTint tint{};
  // The atlas/texture reference (a stable asset handle — the asset
  // system lands with M3-ASSET-01; for now the game assigns it).
  std::uint32_t atlasId{};
  // The material reference (0 = the default material; the material
  // system is future work — the group key carries it per FR-2.1).
  std::uint32_t materialId{};
  // The blend state (the group key's third field).
  BlendMode blend{};
};

// ---------------------------------------------------------------------------
// One output group (the submit stage's read, M2-SPRITE-02)
// ---------------------------------------------------------------------------

// One (atlas, material, blend) group of the frame's sorted sprites:
// the state the submit stage sets ONCE (texture bind + material +
// blend — RENDER-001) and the group's instances in back-to-front
// order (one instanced draw call per group, FR-2.1).
struct SpriteBatch {
  std::uint32_t atlasId{};
  std::uint32_t materialId{};
  BlendMode blend{};
  // The group's instances, back-to-front: the frame's global depth
  // order (M2-SORT-01) RESTRICTED to this group. Each element is the
  // frame-scoped pool slot of the sprite (read the item with the
  // batcher's at(slot) / get(slot)). Invalidated by the next
  // beginFrame()/build() — read within the frame.
  std::span<const std::uint32_t> instances{};
};

namespace detail {

// One group's working descriptor (build's internal table — the public
// view is SpriteBatch). The table is kept in ascending
// (atlas, material, blend) order (the deterministic group order,
// RENDER-003). 20 bytes.
struct GroupDesc {
  std::uint32_t atlasId{};
  std::uint32_t materialId{};
  BlendMode blend{};
  std::uint32_t start{};  // the group's first instance-order position
  std::uint32_t count{};   // the group's instance count
};

}  // namespace detail

// ---------------------------------------------------------------------------
// The sprite batcher (M2-SPRITE-01)
// ---------------------------------------------------------------------------

// The frame declaration window + batch builder (the preamble: the
// frame protocol, the batch model, the overflow policy, the G-R11
// accounting, the storage, the ownership, and the performance
// contracts).
//
// Move-only (CORE-009): the batcher owns the sprite pool, the sorter,
// and the flat frame storage; it is created by create() and handed to
// its owner (the cull/batch stage).
class SpriteBatcher {
 public:
  // The frame budget (the declared scene sprite budget, S-6, API-006):
  // the most sprites one frame may declare. Size it to the scene's
  // worst-case visible count at scene set-up; the batcher never grows
  // beyond it (the overflow policy: drop oldest + warn, the preamble).
  struct Options {
    std::uint32_t maxSprites{};
  };

  // The default state: the EMPTY batcher (capacity 0, no storage).
  // beginFrame()/build() work (the output is empty); add() fails with
  // BudgetExhausted — the stopped-state pattern of the module's value
  // objects (DepthSort), total and never UB.
  SpriteBatcher() noexcept : items_(ArenaPool<SpriteItem>::Options{0}) {}

  // The set-up path (scene load): one allocation per storage
  // structure (the preamble's layout — ~136 B/capacity slot). Fails
  // (InvalidArgument, no allocation) when maxSprites is 0 or exceeds
  // kSpriteBatcherMaxCapacity (the slot-width domain).
  //
  // @budget O(maxSprites); 8 allocations, setup only.
  [[nodiscard]] static Result<SpriteBatcher> create(Options options)
      noexcept;

  // The frame protocol, part 1 (the preamble): close the previous
  // window and open a new one — the previous frame's declared items
  // are released (the pool's reset). Idempotent; a no-op on the
  // stopped batcher.
  //
  // @budget O(previous frame count); no allocation.
  void beginFrame() noexcept;

  // The frame protocol, part 2 (the preamble): declare ONE sprite for
  // the current frame (the window is open: after create()/beginFrame(),
  // until build()). The frame's DECLARATION ORDER is the insertion
  // order: declare in the engine's deterministic entity-id iteration
  // order (FR-1.2) — that is what makes the (key, entity id) total
  // order reproducible (RENDER-003).
  //
  // Overflow policy (PERF-008, S-2 — never grow unbounded): a frame
  // beyond the budget drops the OLDEST live declaration (the ring
  // head is overwritten) and takes the new one; one rate-limited Warn
  // per drop (`sprite_batcher/frame_overflow_dropped`, LOG-004), the
  // cumulative count in droppedTotal().
  //
  // Fails: capacity 0 → BudgetExhausted; window closed (after build)
  // → InvalidArgument (call beginFrame first). Returns the
  // frame-scoped slot (read the item with at(slot)/get(slot); valid
  // until it leaves the ring window or the next beginFrame).
  //
  // @budget O(1); no allocation on the success path (the pool create
  // is the first frame's fill).
  [[nodiscard]] Result<std::uint32_t> add(SpriteItem item) noexcept;

  // The frame protocol, part 3 (the preamble): the batch stage. Sorts
  // the frame's depth keys (M2-SORT-01), groups into
  // (atlas, material, blend) batches (deterministic group order:
  // ascending (atlas, material, blend) — RENDER-003), scatters each
  // group's instances in global back-to-front order, and publishes
  // batches(). Closes the declaration window.
  //
  // Fails: window closed (double build) → InvalidArgument; sort
  // overflow (n > capacity — unreachable: the ring keeps n ≤
  // capacity) → BudgetExhausted.
  //
  // @budget O(4n + 4·256 + n·log G + G·(log G + G)), G = the frame's
  // distinct (atlas, material, blend) count; zero allocation, no GL,
  // the G-R11 warn is a cold rate-limited path (LOG-003).
  [[nodiscard]] Status build() noexcept;

  // The frame's output (read within the frame; invalidated by the
  // next beginFrame()/build()).
  // @budget O(1).
  [[nodiscard]] std::size_t batchCount() const noexcept { return batchCount_; }
  // @budget O(1); the span aliases the batcher's batch array.
  [[nodiscard]] std::span<const SpriteBatch> batches() const noexcept {
    return std::span<const SpriteBatch>(batches_.get(), batchCount_);
  }
  // The frame's declared count, after the overflow drops.
  // @budget O(1).
  [[nodiscard]] std::size_t frameCount() const noexcept { return count_; }
  // True only between build() and the next beginFrame(): the submit
  // stage's (M2-SPRITE-02) precondition — an unbuilt window (including
  // one with declared items) must never be drawn as an empty frame
  // (CORE-008). False before the first build and after every
  // beginFrame(); true on a built empty frame (build() on the
  // stopped state works and builds the empty frame).
  // @budget O(1).
  [[nodiscard]] bool frameBuilt() const noexcept { return frameBuilt_; }
  // G-R11: the current frame's manual depth-override declaration
  // count (reset by beginFrame).
  // @budget O(1).
  [[nodiscard]] std::uint32_t overrideCount() const noexcept {
    return overrideCount_;
  }
  // G-R11: the since-construction override total (the profiler's
  // cumulative guardrail feed, PRD §9.3).
  // @budget O(1).
  [[nodiscard]] std::uint64_t overrideTotal() const noexcept {
    return overrideTotal_;
  }
  // The since-construction dropped-oldest total (the overflow policy's
  // accounting, the preamble).
  // @budget O(1).
  [[nodiscard]] std::uint64_t droppedTotal() const noexcept {
    return droppedTotal_;
  }
  // The frame budget (the create() argument; 0 in the stopped state).
  // @budget O(1).
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
  // The sprite pool's accounting snapshot (PRD §10.4, DBG-008).
  // @budget O(1); no allocation.
  [[nodiscard]] PoolStats itemPoolStats() const noexcept {
    return items_.stats();
  }
  // The item declared at the frame-scoped slot (the null-safe read —
  // nullptr when the slot is not in the current frame window).
  // @budget O(1); no allocation.
  [[nodiscard]] const SpriteItem* get(std::uint32_t slot) noexcept;
  // The item declared at the frame-scoped slot (debug: asserts the
  // slot is live — the S-9 fail-loudly contract; release: undefined on
  // a stale slot, the engine Result convention).
  // @budget O(1); no allocation.
  [[nodiscard]] const SpriteItem& at(std::uint32_t slot);

  SpriteBatcher(const SpriteBatcher&) = delete;
  SpriteBatcher& operator=(const SpriteBatcher&) = delete;
  // Move: the members' moves (the pool and the sorter leave the source
  // stopped; the flat buffers transfer). The source becomes the stopped
  // state (CORE-009).
  SpriteBatcher(SpriteBatcher&&) noexcept = default;
  SpriteBatcher& operator=(SpriteBatcher&&) noexcept = default;

 private:
  // Constructed by create() only (the capacity is validated there —
  // 1..kSpriteBatcherMaxCapacity, so every allocation below is sized).
  explicit SpriteBatcher(std::uint32_t capacity, DepthSort sorter) noexcept
      : capacity_(capacity),
        frameOpen_(true),
        items_(ArenaPool<SpriteItem>::Options{capacity}),
        sorter_(std::move(sorter)),
        keyScratch_(std::make_unique<std::uint32_t[]>(capacity)),
        instanceOrder_(std::make_unique<std::uint32_t[]>(capacity)),
        groups_(std::make_unique<detail::GroupDesc[]>(capacity)),
        groupCursor_(std::make_unique<std::uint32_t[]>(capacity)),
        batches_(std::make_unique<SpriteBatch[]>(capacity)) {}

  // The pool slot of the i-th declaration (0 = the oldest, in the ring
  // window order — the declaration/insertion order, FR-1.2). Only
  // called with capacity_ > 0 and i < count_ (the build() guards).
  std::uint32_t slotAt(std::uint32_t i) const noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(head_) + i) % capacity_);
  }

  // The lexicographic group-key comparison (the deterministic group
  // order, RENDER-003): (atlas, material, blend).
  static bool groupLess(const detail::GroupDesc& g, std::uint32_t atlas,
                        std::uint32_t material,
                        std::uint8_t blend) noexcept {
    if (g.atlasId != atlas) return g.atlasId < atlas;
    if (g.materialId != material) return g.materialId < material;
    return static_cast<std::uint8_t>(g.blend) < blend;
  }
  static bool groupEqual(const detail::GroupDesc& g, std::uint32_t atlas,
                         std::uint32_t material,
                         std::uint8_t blend) noexcept {
    return g.atlasId == atlas && g.materialId == material &&
           g.blend == static_cast<BlendMode>(blend);
  }

  // The group index of (atlas, material, blend) in the sorted table
  // [0, groupCount_), inserting a NEW group at the search position
  // (the table stays sorted — the deterministic group order). A new
  // group appears at most once per frame and the distinct-group count
  // never exceeds n ≤ capacity_, so lo < capacity_ (the table has
  // room) at every insertion.
  //
  // ponytail: the insertion is an O(G) descriptor shift (20 B each) —
  // G is the scene's (atlas × material × blend) combination count,
  // small by design (the PRD §8.1 50k-stress budget caps draw calls at
  // 30, so G << n in every realistic scene); if a pathological G ever
  // measures hot, upgrade to an open-addressing table (RENDER-001: the
  // state-change count is the metric that would show it).
  std::uint32_t findOrInsertGroup(std::uint32_t atlas, std::uint32_t material,
                                  BlendMode blend) noexcept {
    std::uint32_t lo = 0;
    std::uint32_t hi = groupCount_;
    while (lo < hi) {
      const std::uint32_t mid = lo + (hi - lo) / 2;
      if (groupLess(groups_[mid], atlas, material,
                    static_cast<std::uint8_t>(blend))) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo < groupCount_ &&
        groupEqual(groups_[lo], atlas, material,
                   static_cast<std::uint8_t>(blend))) {
      return lo;
    }
    std::memmove(&groups_[lo + 1], &groups_[lo],
                 static_cast<std::size_t>(groupCount_ - lo) *
                     sizeof(detail::GroupDesc));
    groups_[lo] = detail::GroupDesc{atlas, material, blend, 0, 0};
    ++groupCount_;
    return lo;
  }

  // 0 in the default (stopped) state.
  std::uint32_t capacity_{0};
  // The declaration window is open: after create()/beginFrame(), until
  // build(). false in the default state.
  bool frameOpen_{false};
  // build() has closed the current window: true between build() and
  // the next beginFrame() (the submit stage's gate, frameBuilt()).
  // false in the default state (nothing has been built yet).
  bool frameBuilt_{false};
  // The ring head: the pool slot of the frame's OLDEST declaration
  // (0 while the frame has never been full — head_ advances only on
  // overflow).
  std::uint32_t head_{0};
  // The frame's declared count (≤ capacity_; == capacity_ only when
  // the frame is full).
  std::uint32_t count_{0};
  // The group table's live size (the sorted prefix of groups_).
  std::uint32_t groupCount_{0};
  // The published batch count (== groupCount_ after build; 0 before).
  std::uint32_t batchCount_{0};
  // The current frame's manual depth-override declaration count
  // (G-R11; reset by beginFrame).
  std::uint32_t overrideCount_{0};
  // The since-construction override total (G-R11).
  std::uint64_t overrideTotal_{0};
  // The since-construction dropped-oldest total (the overflow policy).
  std::uint64_t droppedTotal_{0};
  // The declared items (budgeted, accounted — PRD §10.4). The pool's
  // reset() is the per-frame release; the batcher's ring (head_,
  // count_) is the frame window over the pool's slots.
  ArenaPool<SpriteItem> items_;
  // The frame's 32-bit depth keys, sorted back-to-front (M2-SORT-01;
  // created with the same capacity, so n ≤ capacity_ always).
  DepthSort sorter_;
  // The frame's keys in declaration (ring window) order — the sorter's
  // input (pre-allocated, PERF-003).
  std::unique_ptr<std::uint32_t[]> keyScratch_;
  // The published per-group instance segments (pool slots, back-to-
  // front — the preamble's storage layout).
  std::unique_ptr<std::uint32_t[]> instanceOrder_;
  // The working group table (the preamble's layout).
  std::unique_ptr<detail::GroupDesc[]> groups_;
  // The per-group scatter cursor (zeroed for [0, groupCount_) per
  // build).
  std::unique_ptr<std::uint32_t[]> groupCursor_;
  // The published batch descriptors (the batches() span aliases this).
  std::unique_ptr<SpriteBatch[]> batches_;
};

// ---------------------------------------------------------------------------
// Implementation (the class is header-only — the batch is pure integer
// bookkeeping over the pre-allocated storage)
// ---------------------------------------------------------------------------

inline Result<SpriteBatcher> SpriteBatcher::create(Options options) noexcept {
  // Capacity validation (first failure wins, InvalidArgument — no
  // allocation on the failure path): the slot-width domain.
  const std::uint32_t capacity = options.maxSprites;
  if (capacity < 1 || capacity > kSpriteBatcherMaxCapacity) {
    return Result<SpriteBatcher>::failure(ErrorCode::InvalidArgument);
  }
  auto sorter = DepthSort::create(capacity);
  if (!sorter.ok()) {
    return Result<SpriteBatcher>::failure(sorter.error());
  }
  return Result<SpriteBatcher>::success(
      SpriteBatcher(capacity, std::move(sorter).takeValue()));
}

inline void SpriteBatcher::beginFrame() noexcept {
  // Release the previous frame's declared items (the pool's reset —
  // O(inUse), trivial destructors) and open a fresh window. The
  // since-construction counters (overrideTotal_, droppedTotal_)
  // survive: they are the profiler's cumulative feeds (PRD §10.4).
  items_.reset();
  head_ = 0;
  count_ = 0;
  groupCount_ = 0;
  batchCount_ = 0;
  overrideCount_ = 0;
  frameOpen_ = true;
  frameBuilt_ = false;
}

inline Result<std::uint32_t> SpriteBatcher::add(SpriteItem item) noexcept {
  if (capacity_ == 0) {
    // The stopped state (the preamble's failure section).
    return Result<std::uint32_t>::failure(ErrorCode::BudgetExhausted);
  }
  if (!frameOpen_) {
    // The frame protocol is explicit: build() closed the window —
    // call beginFrame() before the next frame's declarations.
    return Result<std::uint32_t>::failure(ErrorCode::InvalidArgument);
  }
  // G-R11: count the manual depth-override declarations (per frame +
  // cumulative — the warn fires at build, the preamble).
  if (item.depthOverride) {
    ++overrideCount_;
    ++overrideTotal_;
  }
  if (count_ < capacity_) {
    // The frame is not full: the next pool slot (the invariant — the
    // pool's inUse tracks count_ 1:1 while the frame is not full).
    const std::uint32_t slot = count_;
    const auto r = items_.create(item);
    if (!r.ok()) {
      // Unreachable (the invariant above) — keep the failure channel
      // honest (CORE-008).
      return Result<std::uint32_t>::failure(r.error());
    }
    ++count_;
    return Result<std::uint32_t>::success(slot);
  }
  // The frame is full: the documented overflow policy (the preamble) —
  // drop the OLDEST declaration (the ring head) and take the new one.
  // Bounded, never growing (PERF-008); logged (LOG-002/004 — the
  // facade rate-limits the repeats).
  const std::uint32_t slot = head_;
  items_.at(slot) = item;
  head_ = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(head_) + 1) % capacity_);
  ++droppedTotal_;
  LAIGE_LOG_WARN("sprite_batcher", "frame_overflow_dropped",
                 "Sprite declaration exceeded the frame budget; the oldest "
                 "declaration was dropped",
                 laige::log::field("capacity", capacity_),
                 laige::log::field("frame_count", count_),
                 laige::log::field("dropped_total", droppedTotal_));
  return Result<std::uint32_t>::success(slot);
}

inline Status SpriteBatcher::build() noexcept {
  if (capacity_ > 0 && !frameOpen_) {
    // Double build: the window was closed by the previous build —
    // call beginFrame() first (the frame protocol is explicit).
    return Status(ErrorCode::InvalidArgument);
  }
  const std::uint32_t n = count_;
  batchCount_ = 0;
  groupCount_ = 0;
  if (n == 0) {
    frameOpen_ = false;
    frameBuilt_ = true;
    return Status{};
  }
  // 1. The frame's keys in declaration (ring window) order — the
  //    sorter's INPUT order (FR-1.2: the entity-id iteration order —
  //    the stable tie-break's carrier, M2-SORT-01).
  for (std::uint32_t i = 0; i < n; ++i) {
    keyScratch_[i] = items_.at(slotAt(i)).depthKey;
  }
  const Status s = sorter_.sort(
      std::span<const std::uint32_t>(keyScratch_.get(), n));
  if (!s.ok()) return s;  // BudgetExhausted unreachable (n <= capacity_)
  const auto sorted = sorter_.sortedIndices();
  // 2. Pass 1: the group membership counts (the table grows in sorted
  //    (atlas, material, blend) order — the deterministic group order).
  for (std::uint32_t i = 0; i < n; ++i) {
    const SpriteItem& it = items_.at(slotAt(sorted[i]));
    const std::uint32_t g =
        findOrInsertGroup(it.atlasId, it.materialId, it.blend);
    ++groups_[g].count;
  }
  // 3. The groups' start offsets (the prefix) + the zero cursors.
  std::uint32_t start = 0;
  for (std::uint32_t g = 0; g < groupCount_; ++g) {
    groups_[g].start = start;
    groupCursor_[g] = 0;
    start += groups_[g].count;
  }
  // 4. Pass 2: the instance scatter — each instance lands in its
  //    group's segment in GLOBAL back-to-front order (the sorted order
  //    restricted to the group; the (key, declaration order) total
  //    order, the preamble).
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t pos = sorted[i];
    const SpriteItem& it = items_.at(slotAt(pos));
    const std::uint32_t g =
        findOrInsertGroup(it.atlasId, it.materialId, it.blend);
    instanceOrder_[groups_[g].start + groupCursor_[g]++] = slotAt(pos);
  }
  // 5. The published batch descriptors (the submit stage's read,
  //    M2-SPRITE-02).
  for (std::uint32_t g = 0; g < groupCount_; ++g) {
    batches_[g] = SpriteBatch{
        groups_[g].atlasId, groups_[g].materialId, groups_[g].blend,
        std::span<const std::uint32_t>(
            instanceOrder_.get() + groups_[g].start, groups_[g].count)};
  }
  batchCount_ = groupCount_;
  frameOpen_ = false;
  frameBuilt_ = true;
  // G-R11: the manual depth-override escape hatch is counted +
  // warned (the preamble — the facade rate-limits the per-frame
  // repeats, LOG-004).
  if (overrideCount_ > 0) {
    LAIGE_LOG_WARN("sprite_batcher", "depth_override_used",
                   "Per-sprite depth override in use; prefer tile height "
                   "(isoDepthKey)",
                   laige::log::field("count", overrideCount_),
                   laige::log::field("override_total", overrideTotal_));
  }
  return Status{};
}

inline const SpriteItem* SpriteBatcher::get(std::uint32_t slot) noexcept {
  if (capacity_ == 0 || slot >= capacity_) return nullptr;
  // The slot is in the current frame window iff its ring age (distance
  // from the head, forward) is < the frame count.
  const std::uint32_t age = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(slot) - static_cast<std::uint64_t>(head_)) %
      static_cast<std::uint64_t>(capacity_));
  return (age < count_) ? &items_.at(slot) : nullptr;
}

inline const SpriteItem& SpriteBatcher::at(std::uint32_t slot) {
  const SpriteItem* p = get(slot);
  assert(p != nullptr &&
         "SpriteBatcher::at: slot is not live in the current frame");
  return *p;
}

}  // namespace laige::render
