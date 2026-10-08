// laige-render particle rendering (M2-PART-02): the rendering half of
// FR-2.7 — the CPU-simulated particles (M2-PART-01) declared into the
// sprite batcher as sprites.
//
// FR-2.7: "Lightweight GPU particle system (CPU-simulated, 2D +
// depth), budgeted, pooled." The simulation half (M2-PART-01) owns
// the bounded pool + the per-tick advance (`laige/sim/particles.h`);
// this header is the render pass. S-5 (PRD §9.1): rendering goes
// through the batcher — the live particles are DECLARED into the
// sprite batcher as sprites, never drawn directly. One
// `declareParticles` call declares every live particle of one
// ParticleSystem under one shared (atlas, material, blend) group, so
// the whole emitter set costs ONE instanced draw call per frame
// (RENDER-001, FR-2.1) — the submit stage (M2-SPRITE-02) makes one
// instanced draw call per (atlas, material, blend) group.
//
//   ParticleDeclareOptions        The per-declare options (the shared
//                                 atlas, material, blend, UV, layer)
//   particleDepthToStepHeight     The explicit particle-depth ->
//                                 step-height conversion (RENDER-006)
//   declareParticles              The O(live) particle -> sprite pass
//
// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
//
// An emitter SET is one ParticleSystem: the system's bounded pool +
// emitters are the set's budget and identity (M2-PART-01). A game
// with several particle styles (smoke, sparks, magic) keeps one
// ParticleSystem per style — each is declared with its own
// `ParticleDeclareOptions` (its own shared atlas, blend, layer) and
// therefore lands in its own (atlas, material, blend) group: one
// draw call per emitter set, the FR-2.7 promise, with no per-particle
// state changes and no texture switches inside the set (RENDER-001).
//
// The per-particle declaration (every field is presentation state —
// ARCH-009; the particle's simulation state is read, never touched):
//
//   pos         the particle's world position, sim units -> render
//               floats at the RENDER-006 conversion boundary (one
//               documented rounding per component, per backend —
//               fpx16_16: `fpx16_16::toFloat`; fp32_pinned: identity,
//               the fp32 Scalar IS float)
//   depthKey    the engine-owned M2-ISO-01 key:
//               `isoDepthKey<Backend>(pos, z, layer)` with
//               `z = particleDepthToStepHeight(depth)` — the
//               particle's CONSTANT depth value (world units)
//               quantized to integer world units, the step-height
//               domain (the section below). `depthOverride` stays
//               FALSE (G-R11: the key is engine-owned; the counted +
//               warned manual-override escape hatch is never used)
//   scale       (size, size) in world units (the particle's constant
//               size — a square sprite)
//   tint        the base RGB channels (u8 -> float [0, 1]) and the
//               CURRENT fade alpha (`ParticleSystem::fadeAlpha`,
//               exact u32 arithmetic, M2-PART-01) — one
//               presentation-only rounding per channel (ARCH-009)
//   uv          the shared atlas sub-rect (the options' UV — every
//               particle of the set draws the same sprite frame)
//   blend       the options' blend (Additive is the particle
//               convention — the BlendMode document)
//   atlasId / materialId / layer  the options, carried unchanged
//   frameIndex  0 (particle frames are M3 animation work)
//   rotation    0 (no per-particle rotation in this step)
//
// ---------------------------------------------------------------------------
// The depth conversion (RENDER-006 — the explicit sim -> render
// boundary)
// ---------------------------------------------------------------------------
//
// The M2-ISO-01 key takes the step height as INTEGER world units
// (the tile-map height domain — `isoDepthKey<Backend>(pos,
// stepHeight, layer)`, `|stepHeight| <= kIsoDepthMaxStepHeight`
// (2047)). A particle's depth is a constant Scalar in world units
// (the emitter's depth, M2-PART-01), so the conversion is:
//
//   1. CLAMP to [−2047, +2047] FIRST — total function: a finite
//      depth outside the key's z domain saturates at the boundary
//      (the isoDepthKey packing-boundary saturation contract); the
//      comparisons are per backend (fpx16_16: raw integer order —
//      the backend has no NaN; fp32_pinned: IEEE — a NaN depth,
//      unreachable in practice because the emitter's depth is
//      validated finite at `addEmitter`, falls through to the
//      rounding, which maps it to +0)
//   2. ROUND to nearest, ties-to-even — one rounding:
//      fpx16_16: `fpx16_16::toInt32` (the fpx16_16.h rounding
//      policy); fp32_pinned: `std::nearbyint` (the same
//      round-nearest-even policy in the pinned default rounding
//      mode) followed by the exact int32 conversion (|d| ≤ 2047
//      after the clamp). The two backends AGREE for every dyadic
//      depth in the domain (1/16 world-unit lattice — the
//      isoDepthKey cross-backend exactness zone).
//
// A sub-unit depth (e.g. 0.5 world units) therefore renders at the
// nearest integer height (0.5 -> 0, ties to even — 1.5 -> 2): the
// engine's depth model is integer world units end to end (the tile
// heights), and a sub-unit particle offset is below the key's
// 1/16-unit fine-depth resolution only when it rounds away to the
// same integer — the documented quantization, not an error.
//
// ---------------------------------------------------------------------------
// Order and determinism (RENDER-003, ARCH-010 scope: presentation
// only)
// ---------------------------------------------------------------------------
//
// The declaration order is the live-array order — the M2-PART-01
// spawn order with swap removal (deterministic per backend, per
// seed; the span order is a SIMULATION fact, read here in the render
// phase). The batcher's stable sort (M2-SORT-01) then makes the
// in-group order the (key, declaration position) total order: equal
// keys (particles on the same screen row) keep spawn order — the
// particle analog of the (key, entity id) tie-break (particles have
// no entity ids; the live order is their stable identity).
//
// ---------------------------------------------------------------------------
// The frame protocol + failure contract
// ---------------------------------------------------------------------------
//
// One call per frame per emitter set, in the RENDER phase (the
// M2-GL-02 cull/batch stage — after the simulation phase, the
// `liveParticles()` read contract, M2-PART-01), inside an OPEN
// window:
//
//   batcher.beginFrame()
//   declareParticles(batcher, system, options);   // per emitter set
//   ... (sprites, tiles, parallax declarations)
//   batcher.build();
//
// `declareParticles` reads `system.liveParticles()` and adds each
// particle's item in live order. The FIRST failed add fails the call
// with the batcher's error (the `TileMap::declareTo` precedent):
//
//   - stopped batcher (create failure / capacity 0)
//         -> BudgetExhausted (the batcher's stopped-state contract)
//   - closed window (build already ran / no beginFrame yet)
//         -> InvalidArgument (the batcher's frame-protocol contract)
//
// On a partial failure the items already declared by the call REMAIN
// in the frame (the batcher owns the frame state — the game
// re-declares the whole frame every frame; there is no rollback and
// no partial-frame state to repair). The batcher's own overflow
// policy (drop the oldest declaration + the rate-limited warn)
// applies when the frame budget is exceeded — a truncated particle
// set is the documented degradation, never a crash (NFR-8.5).
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003/004, DOC-004)
// ---------------------------------------------------------------------------
//
//   declareParticles  O(live): one isoDepthKey + one batcher add per
//                     particle (the add is O(1) — one pool create or
//                     ring overwrite). No allocation, no logging on
//                     the happy path (the batcher's overflow warn is
//                     its own cold, rate-limited path, LOG-003/004),
//                     no GL. The O(live) pass is the step's budget:
//                     the `particle_render_10k` entry in
//                     budgets.json (the 10 000-particle conversion,
//                     mean, ms — the `DepthSortBudget`/
//                     `IsoPickBudget` gate pattern).
//
//   particleDepthToStepHeight  O(1): two comparisons + one rounding;
//   no allocation. Pure — callable from any thread at any phase.
//
// Call site: once per frame per emitter set, in the RENDER phase —
// never in the simulation tick (ARCH-002), never per particle from
// game code (S-5).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
// - Do not declare particles per frame from the SIMULATION tick
//   (ARCH-002): `declareParticles` is a render-phase read of the
//   sim state — call it in the cull/batch stage, after the tick.
// - Do not hand-roll the depth key (G-R11): the key comes from
//   `isoDepthKey` through this header (the manual `depthKey` +
//   `depthOverride` pair on `SpriteItem` is the counted + warned
//   escape hatch — particles never need it).
// - Do not mix emitter sets under one options value: one options
//   value = one (atlas, material, blend) group = one draw call; a
//   different atlas or blend is a different set (declare it
//   separately) — the group count is the draw-call count
//   (RENDER-001).
// - Do not assume the span order is a render order (M2-PART-01): it
//   is a spawn order with swap removal; the batcher's depth sort is
//   the render order (the live order is only the stable tie-break).
// - Size the batcher's capacity for the scene's worst-case visible
//   sprite count INCLUDING the particle sets: the overflow drops the
//   OLDEST declaration of the whole frame (every declared content
//   shares the one ring) — a particle-heavy scene needs the budget
//   to fit (API-006).
//
// Canonical narrative: docs/concepts/coordinates.md §4.11 (ARCH-008);
// API contract: docs/api/particle_render.md.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/render/iso_depth_key.h"
#include "laige/render/sprite_batcher.h"
#include "laige/result.h"
#include "laige/sim/particles.h"
#include "laige/sim_math.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The u8 color channel -> float [0, 1] divisor (one presentation-only
// rounding per channel — ARCH-009; 255/255 is exactly 1.0).
inline constexpr float kTintChannelDivisor = 255.0f;

// ---------------------------------------------------------------------------
// The per-declare options
// ---------------------------------------------------------------------------

// The options one `declareParticles` call declares under. Plain
// value (the scene config — the batcher's group key carries
// atlasId/materialId/blend unchanged; no validation here: the atlas
// id is a game-assigned stable handle, the UV rect is the caller's
// invariant (u1 > u0, v1 > v0 — the SpriteUvRect contract), the
// layer is the isoDepthKey's (clamped to its packing domain)).
struct ParticleDeclareOptions {
  // The shared particle atlas (the game assigns it — M3-ASSET-01
  // owns the asset system; for now the game assigns the texture id).
  std::uint32_t atlasId{};
  // The material id (0 = the default material — the group key
  // carries it per FR-2.1).
  std::uint32_t materialId{};
  // The blend state every declared particle is drawn with. Additive
  // is the particle convention (FR-2.7 additive effects — the
  // BlendMode document).
  BlendMode blend{BlendMode::Additive};
  // The UV sub-rect every particle of the set draws from in the
  // shared atlas (default: the whole atlas — a single-sprite sheet).
  SpriteUvRect uv{0.0f, 0.0f, 1.0f, 1.0f};
  // The isoDepthKey layer (M2-ISO-01; default: the ground layer —
  // the particle rides the world's layer; a parallax-layer particle
  // set is a scene-setup choice, M2-PAR-01).
  std::int32_t layer{kIsoDepthGroundLayer};
};

// ---------------------------------------------------------------------------
// The depth conversion (RENDER-006)
// ---------------------------------------------------------------------------

// The particle's constant depth (world units, the SimMath backend's
// Scalar) quantized to the M2-ISO-01 step-height domain: clamp to
// [−kIsoDepthMaxStepHeight, +kIsoDepthMaxStepHeight] (total function —
// saturation at the packing boundary, the isoDepthKey contract), then
// round to nearest, ties-to-even (the two backends agree for every
// dyadic depth in the domain — the header's depth-conversion
// section). O(1); no allocation; pure.
template <typename Backend>
[[nodiscard]] inline std::int32_t particleDepthToStepHeight(
    typename sim::SimMath<Backend>::Scalar depth) noexcept {
  using Scalar = typename sim::SimMath<Backend>::Scalar;
  Scalar d = depth;
  if constexpr (std::is_same_v<Backend, sim::Fpx16_16>) {
    // Raw integer order (the backend has no NaN): the domain bounds
    // as Q16.16 raws (|2047| * 2^16 fits int32 with room to spare).
    constexpr std::int32_t kMinRaw =
        static_cast<std::int32_t>(-kIsoDepthMaxStepHeight) << 16;
    constexpr std::int32_t kMaxRaw = kIsoDepthMaxStepHeight << 16;
    if (d.raw < kMinRaw) {
      d = laige::fpx16_16{kMinRaw};
    } else if (d.raw > kMaxRaw) {
      d = laige::fpx16_16{kMaxRaw};
    }
    // One rounding: toInt32 (round to nearest, ties-to-even — the
    // fpx16_16.h policy); |d| <= 2047 keeps the result in range.
    return laige::fpx16_16::toInt32(d);
  } else {
    // IEEE order (a NaN depth falls through — the unreachable
    // case, the header's section; nearbyint(NaN) is +0 by IEEE).
    constexpr float kMinF =
        static_cast<float>(-kIsoDepthMaxStepHeight);
    constexpr float kMaxF = static_cast<float>(kIsoDepthMaxStepHeight);
    if (d < kMinF) {
      d = kMinF;
    } else if (d > kMaxF) {
      d = kMaxF;
    }
    // One rounding: nearbyint (round to nearest, ties-to-even in the
    // pinned default rounding mode — the same policy as
    // fpx16_16::toInt32, so both backends agree on dyadic depths),
    // then the exact int32 conversion (|d| <= 2047 after the clamp).
    return static_cast<std::int32_t>(std::nearbyint(d));
  }
}

namespace detail {
// The sim Scalar -> render float conversion at the RENDER-006
// boundary (one documented rounding per backend — fpx16_16:
// `fpx16_16::toFloat`; fp32_pinned: identity, the fp32 Scalar IS
// float).
template <typename Backend>
[[nodiscard]] float scalarToRenderFloat(
    typename sim::SimMath<Backend>::Scalar v) noexcept {
  if constexpr (std::is_same_v<Backend, sim::Fpx16_16>) {
    return laige::fpx16_16::toFloat(v);
  } else {
    return v;
  }
}
}  // namespace detail

// ---------------------------------------------------------------------------
// The particle -> sprite pass
// ---------------------------------------------------------------------------

// Declares every live particle of `system` into `batcher` under
// `options` (the header's model section): O(live), no allocation,
// the first failed add fails the call with the batcher's error
// (the header's frame-protocol section). `batcher` must be alive
// (created with a nonzero capacity) and the frame window must be
// OPEN (beginFrame called, build not yet run) — otherwise the first
// add fails (BudgetExhausted / InvalidArgument respectively).
template <typename Backend>
[[nodiscard]] inline Status declareParticles(
    SpriteBatcher& batcher, const ParticleSystem<Backend>& system,
    const ParticleDeclareOptions& options) noexcept {
  using P = ParticleSystem<Backend>::Particle;
  const std::span<const P> particles = system.liveParticles();
  for (std::size_t i = 0; i < particles.size(); ++i) {
    const P& p = particles[i];
    SpriteItem item;
    // World position (sim units -> render floats, the RENDER-006
    // boundary — one documented rounding per component).
    item.pos = Vec2{
        detail::scalarToRenderFloat<Backend>(p.pos.x),
        detail::scalarToRenderFloat<Backend>(p.pos.y)};
    // The engine-owned depth key (G-R11): the particle's constant
    // depth feeds the M2-ISO-01 key as its step height (the
    // header's depth-conversion section). depthOverride stays false.
    item.depthKey = isoDepthKey<Backend>(
        p.pos, particleDepthToStepHeight<Backend>(p.depth),
        options.layer);
    item.uv = options.uv;
    item.frameIndex = 0;
    item.rotation = 0.0f;
    // The square sprite (the particle's constant size, world units).
    const float size = detail::scalarToRenderFloat<Backend>(p.size);
    item.scale = Vec2{size, size};
    // The base RGB (u8 -> float) + the current fade alpha (exact u32
    // arithmetic, M2-PART-01) — one presentation-only rounding per
    // channel.
    item.tint = SpriteTint{
        static_cast<float>(p.tint[0]) / kTintChannelDivisor,
        static_cast<float>(p.tint[1]) / kTintChannelDivisor,
        static_cast<float>(p.tint[2]) / kTintChannelDivisor,
        static_cast<float>(ParticleSystem<Backend>::fadeAlpha(p)) /
            kTintChannelDivisor};
    item.blend = options.blend;
    item.atlasId = options.atlasId;
    item.materialId = options.materialId;
    const auto added = batcher.add(std::move(item));
    if (!added.ok()) {
      // The first failed add fails the call (the TileMap::declareTo
      // precedent): the items already declared by this call remain
      // in the frame (the batcher owns the frame state — the game
      // re-declares the whole frame every frame, the header's
      // frame-protocol section).
      return Status::failure(added.error());
    }
  }
  return Status{};
}

}  // namespace laige::render
