// laige-render parallax layers (M2-PAR-01): the named background /
// midground / foreground layer model of FR-2.3, with the parallax
// factor, the world-space offset, the UV scroll (auto or manual), and
// the batch path into the sprite batcher.
//
// FR-2.3: "Parallax layers: named background/midground/foreground
// layers with parallax factor, offset, UV scroll, blend". S-5 (PRD
// §9.1): rendering goes through the batcher — a parallax layer's quads
// are declared into the sprite batcher as sprites with engine-computed
// depth keys, never drawn directly. ARCH-009: the layer state
// (definitions + the scroll offsets) is presentation state — it reads
// the camera's presentation position, never sim state, and is never
// part of replay state or the simulation state hash. G-R11: the
// layer's depth key is engine-owned (the M2-ISO-01 key with the
// layer's depth-layer value — the game never writes the key).
// RENDER-003: the declaration order is deterministic (ascending layer
// id, then the fixed wrap-quad order); the render order is documented
// below ("The render order").
//
//   ParallaxScrollMode  Manual (caller-driven) | Auto (per-frame
//                       engine advance)
//   ParallaxSource      Image (a single texture) | Tilemap (a tilemap
//                       layer — declared with M2-TILE-02)
//   ParallaxLayerDef    One named layer's definition (the value the
//                       game declares)
//   ParallaxLayers      The scene's layer registry: the bounded slot
//                       table + the per-frame protocol
//
// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
//
// A layer is a WORLD-SPACE RECTANGLE of one texture (the Image
// source) or of one tilemap (the Tilemap source, M2-TILE-02). The
// layer's content position at camera position p (the camera's
// ground-plane (x, y) — the M2-CAM-01 presentation position) is:
//
//   worldOffset(p) = factor * (p - center) + offset        (1)
//
// the EXACT formula (the roadmap's pinned contract — the tests pin
// it bit-exactly for dyadic values):
//
//   factor   the parallax factor in [0, 1] (one source of truth):
//            0 = the layer is FIXED IN WORLD SPACE at `offset` (it
//            moves full-speed against the camera — maximum parallax,
//            a close-by background); 1 = the layer moves 1:1 WITH
//            the camera (it is fixed on screen — no parallax, the
//            sky layer); between: the layer's screen position
//            shifts by (factor - 1) * (p - center).
//   center   the REFERENCE camera position (the camera position at
//            which the layer sits at exactly `offset`; default
//            (0, 0) — the scene's world origin)
//   offset   the layer's world-space offset at p = center (default
//            (0, 0) — the content origin at the world origin)
//
// Everything is WORLD space (PRD §4, RENDER-006): (1) is a world-space
// translation; nothing in it is screen space. The camera position is
// the presentation-side input (ARCH-009) — the sim never sees it.
//
// The Image source renders the layer's texture over the world
// rectangle [worldOffset(p), worldOffset(p) + size) — `size` is the
// rectangle's extent in world units (the texture spans the whole
// rectangle once, and WRAPS at the rectangle's edges under the UV
// scroll — below). The texture's v axis (v = 0 = first uploaded texel
// row, the M2-SPRITE-02 contract) maps to the world +y direction
// (downward on screen under the iso projections — coordinates.md).
//
// The Tilemap source references a tilemap (its `tilemapId` — the
// game's registry id; M3-ASSET-01 owns the asset system): the tilemap
// defines its own world grid, and M2-TILE-02 declares its tiles with
// this layer's `worldOffset` translation and its `depthLayer` (the
// tilemap's Options::layer). In M2-PAR-01 a Tilemap-source layer is
// DATA ONLY: `declareTo` skips it (the hook), its `size`/`uv` fields
// are not validated.
//
// ---------------------------------------------------------------------------
// The render order (the documented background-first contract)
// ---------------------------------------------------------------------------
//
// The batcher draws one group per distinct (atlas, material, blend)
// in its DETERMINISTIC group order — ascending (atlas, material,
// blend) (RENDER-003, M2-SPRITE-01) — and, within a group, the
// instances in the M2-ISO-01 key order (layer field dominant). The
// parallax layer's quads carry the key's LAYER field:
//
//   kParallaxDepthLayerBackground  = -2   (the `bg` preset)
//   kParallaxDepthLayerMidground   = -1   (the `mid` preset)
//   kIsoDepthGroundLayer           =  0   (the ground — M2-ISO-01)
//   kParallaxDepthLayerForeground  = +1   (the `fg` preset)
//
//   - WITHIN a shared (atlas, material, blend) group: the layer
//     field dominates the key, so every background-layer quad sorts
//     before every ground object and every foreground quad AFTER it,
//     whatever the quads' v — background first, engine-guaranteed
//     (the M2-ISO-01 "layer dominates" contract).
//   - ACROSS groups: the batcher's group order (ascending
//     (atlas, material, blend)) is the draw order — the scene's
//     SET-UP must assign the parallax layers' atlas ids so the group
//     order matches the depth order: background layers' atlas ids
//     BELOW the world content's, foreground layers' ABOVE it (the
//     same convention as the M2-TILE-01 texture-id assignment).
//   - A layer's own quads (the wrap split, below) tile its
//     rectangle WITHOUT overlap: their relative order is the
//     deterministic (key, declaration) total order and is visually
//     irrelevant.
//
// The canonical presets (the "named layers"):
//
//   kParallaxLayerBackground  = 0   (bg — factor ~0, depth layer -2)
//   kParallaxLayerMidground   = 1   (mid — factor ~0.5, depth layer -1)
//   kParallaxLayerForeground  = 2   (fg — factor 1, depth layer +1)
//   kParallaxLayerCustomBase  = 3   (custom layers: any id >= 3, any
//     distinct depth-layer value in the M2-ISO-01 domain
//     [-512, +511] — more negative = further back)
//
// ---------------------------------------------------------------------------
// The UV scroll (auto or manual) + the exact wrap
// ---------------------------------------------------------------------------
//
// Each layer carries a CURRENT UV OFFSET in [0, 1)^2 — the texture's
// wrap position within its rectangle. Per texture: the sample UV at
// a world point w in the rectangle is
//
//   u = frac((w.x - X) / size.x + uvOffset.x)      X, Y = the
//   v = frac((w.y - Y) / size.y + uvOffset.y)      rectangle's
//                                               world origin corner
//
// (the M2-SPRITE-02 UV convention: v = 0 is the texture TOP).
// Increasing the offset scrolls the texture toward +u (+x world) and
// +v (+y world).
//
//   Manual mode: the caller drives the offset — `setUvOffset` (any
//     finite value; it is WRAPPED to [0, 1)^2).
//   Auto mode: the engine advances the offset by `scrollSpeed`
//     (UV units PER FRAME — frames are the presentation pace; frame-
//     rate independence is the caller's concern, the M2-CAM-01
//     lerp precedent) on each `advanceScrolls()` call — once per
//     frame, before the declarations.
//
// The WRAP is exact at the texture boundary: `wrap(x) = x - floor(x)`
// (in [0, 1) for every finite x; 1.0 wraps to exactly 0.0; -0.25
// wraps to exactly 0.75 — dyadic values wrap bit-exactly, which the
// tests pin).
//
// RENDERING the wrap through the batcher (the 2 x 2 split): a single
// SpriteItem carries ONE UV rect — no wrap — so a scrolled layer
// declares its rectangle as the four wrap-aligned quads (fixed order
// q00, q10, q01, q11; a quad whose range is empty — uvOffset 0 on
// that axis — is skipped):
//
//   wx = X + (1 - uvOffset.x) * size.x    (the u-wrap world line)
//   wy = Y + (1 - uvOffset.y) * size.y    (the v-wrap world line)
//
//   q00  world [X,    wx) x [Y,  wy)   base uv [ox, 1) x [oy, 1)
//   q10  world [wx,   X+sx) x [Y,  wy)  base uv [0,  ox) x [oy, 1)
//   q01  world [X,    wx) x [wy, Y+sy)  base uv [ox, 1) x [0,  oy)
//   q11  world [wx,   X+sx) x [wy, Y+sy) base uv [0,  ox) x [0,  oy)
//
// each quad's UV rect is the layer's atlas sub-rect (the def's `uv`)
// mapped over its base uv range. At uvOffset (0, 0) exactly ONE quad
// (q00, the full rectangle, the full texture) is declared — the
// un-scrolled cost. The quads tile the rectangle exactly (their
// world areas sum to size.x * size.y) and sample the texture exactly
// once — the wrap is exact at every boundary.
//
// ---------------------------------------------------------------------------
// The per-frame protocol (the frame pipeline's cull/batch stage)
// ---------------------------------------------------------------------------
//
// One ParallaxLayers per scene, owned by the render set-up /
// cull-batch stage (one owner — CONC-001). Per frame:
//
//   advanceScrolls()               the auto layers' UV advance
//                                   (O(layers), no allocation)
//   batcher.beginFrame()
//   parallax.declareTo(batcher, cameraPos)   the layer quads
//   ... the scene's other content (the M2-TILE-01 tilemap, the
//   sprites — the batcher's frame window stays open) ...
//   batcher.build()
//
// `declareTo` is READ-ONLY over the registry (the declarations are
// built from the current state); the ONLY per-frame mutation is
// `advanceScrolls` (the scroll offsets). The declared quads are
// PRESENTATION state (ARCH-009) — the batcher owns them for one frame.
//
// ---------------------------------------------------------------------------
// Determinism (RENDER-003, ARCH-010 scope — presentation-only)
// ---------------------------------------------------------------------------
//
// The declaration of a frame is a pure function of (the registry
// state — the definitions + the current scroll offsets, and the
// camera position): same state + same position → bit-identical
// quads, every frame, every platform (fixed float op order; the keys
// are the M2-ISO-01 keys of the scene's backend — ADR 0002 scope).
// The declaration ORDER is deterministic: ascending layer id, then
// the fixed quad order q00, q10, q01, q11 — the (key, insertion)
// total order (RENDER-003) resolves ties for equal keys.
//
// ---------------------------------------------------------------------------
// Ownership, threading, performance
// ---------------------------------------------------------------------------
//
// Move-only (the M2-TILE-01 pattern). ONE heap allocation (the
// pre-sized slot table, at create — the setup path). Per frame:
// advanceScrolls is O(layers) adds/wraps; declareTo is O(layers x 4)
// adds into the batcher's PRE-ALLOCATED frame — NO per-frame
// allocation (FR-2.2, PERF-003), no logging (LOG-003), no GL calls
// (this header is GL-free — the batch path is bookkeeping; the
// submit is the M2-SPRITE-02 stage). Not thread-safe (one owner —
// the cull/batch stage; CONC-001).
//
// No standalone budgets.json entry: the per-frame declare cost
// (O(layers x 4) adds) is PART of the composite 50k render-CPU
// budget (PRD §8.1, `sprites_50k_cpu` — M2-PERF-01 measures the
// reference scene with the parallax quads included; the M2-SCENE-01
// reference scene has 3 layers, at most 12 quads — trivial against
// the 50k sprites).
//
// ---------------------------------------------------------------------------
// Failure behavior (CORE-008, first failure wins)
// ---------------------------------------------------------------------------
//
//   create:    maxLayers outside [1, kParallaxLayersMaxLayers] ->
//              InvalidArgument (no log — the M2-SPRITE-01 create
//              precedent).
//   setLayer:  the documented validation order, first failure wins:
//              id -> factor -> center -> offset -> scroll_speed ->
//              size (Image) -> uv (Image) -> depth_layer; each
//              failure = InvalidArgument + one rate-limited
//              parallax/layer_invalid warn (fields layer, field —
//              the setup/config boundary, the M2-CAM-01 options
//              precedent); a rejected definition leaves the slot
//              UNCHANGED.
//   setUvOffset: unknown layer id -> InvalidArgument (no log —
//              precondition, the M2-TILE-01 rejected-edit
//              precedent); non-finite value -> InvalidArgument + one
//              rate-limited parallax/uv_offset_invalid warn.
//   declareTo: stopped registry (maxLayers 0) -> InvalidArgument;
//              built frame (the window closed) -> InvalidArgument,
//              NOTHING declared; the batcher's own overflow/stopped
//              failures (BudgetExhausted) propagate from the first
//              failed add.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
// - Don't call `worldOffsetAt`/`uvOffsetAt`/`layerAt` for an id that
//   is not set (asserted precondition — check `has()` first when the
//   id comes from untrusted input).
// - Don't call `advanceScrolls` more than once per frame (the auto
//   speed is PER FRAME — the caller paces it once per frame, the
//   batcher's beginFrame pace).
// - Don't declare a Tilemap-source layer through `declareTo` in
//   M2: it is skipped (no log — the M2-TILE-02 step implements the
//   tilemap declare path on this layer's `worldOffset` +
//   `depthLayer`).
// - Don't expect the parallax factor to be "how much the layer
//   moves": it is the (1) coefficient — factor 1 is the SCREEN-FIXED
//   layer (no parallax), factor 0 the full-parallax one. The formula
//   (1) is the contract; the tests pin it.
// - Don't hand-write the layer quads' depth keys: they are the
//   M2-ISO-01 keys (G-R11) — the def's `depthLayer` is the only
//   ordering input.
// - Don't mix backends within one scene: the registry's backend is
//   the scene's backend (the keys must agree with the M2-TILE-01
//   table's — the M2-ISO-01 cross-backend consistency contract).

#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/render/iso_depth_key.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_batcher.h"
#include "laige/result.h"
#include "laige/sim_math.h"

namespace laige::render {

// The named-layer presets (the "named layers" of FR-2.3): the three
// canonical ids — the scene's custom layers use any id >=
// kParallaxLayerCustomBase (and any distinct depth-layer value —
// "The render order" above). "Named" = the stable id (the scene
// config, M3, maps the id to the scene-format name — no std::string
// in frame data, PRD §10.4).
inline constexpr std::uint32_t kParallaxLayerBackground = 0;
inline constexpr std::uint32_t kParallaxLayerMidground = 1;
inline constexpr std::uint32_t kParallaxLayerForeground = 2;
inline constexpr std::uint32_t kParallaxLayerCustomBase = 3;

// The M2-ISO-01 key layer values of the presets (the M2-ISO-01
// reserved parallax domain: background layers sort BEFORE the
// ground, foreground layers AFTER it — "layer dominates" in the
// key): the standard three leave every other domain value free for
// custom layers (more negative = further back).
inline constexpr std::int32_t kParallaxDepthLayerBackground = -2;
inline constexpr std::int32_t kParallaxDepthLayerMidground = -1;
inline constexpr std::int32_t kParallaxDepthLayerForeground = 1;

// The registry's slot domain (API-006): [1, kParallaxLayersMaxLayers];
// the default covers the PRD reference scene's 3 preset layers +
// custom headroom. 64 slots x ~104 B = 6.7 KB (the setup path).
inline constexpr std::uint32_t kParallaxLayersDefaultLayers = 8;
inline constexpr std::uint32_t kParallaxLayersMaxLayers = 64;

// Manual (the caller drives the UV offset via setUvOffset) | Auto
// (the engine advances it by scrollSpeed per frame —
// advanceScrolls).
enum class ParallaxScrollMode : std::uint8_t {
  Manual,
  Auto,
};

// Image (a single texture — the atlasId + the atlas sub-rect `uv`)
// | Tilemap (a tilemap layer — the tilemapId; declared with
// M2-TILE-02, data only in M2-PAR-01).
enum class ParallaxSource : std::uint8_t {
  Image,
  Tilemap,
};

// One named layer's definition (the value the game declares through
// setLayer; plain value — no GL, no allocation). All fields are
// validated (the setLayer validation order) except `source`,
// `materialId`, `atlasId`, `tilemapId`, `enabled` (opaque
// references/flags — the batcher and the asset system own their
// domains).
struct ParallaxLayerDef {
  // The layer id: [0, the registry's maxLayers); the presets are the
  // kParallaxLayer* constants.
  std::uint32_t id{};
  // Image (the default — the single-texture layer) | Tilemap
  // (M2-TILE-02).
  ParallaxSource source{ParallaxSource::Image};
  // The parallax factor (1): [0, 1]; 0 = fixed in world space
  // (maximum parallax), 1 = fixed on screen (no parallax).
  float factor{};
  // The reference camera position (1): the camera position at which
  // the layer sits at exactly `offset` (default (0, 0) — the world
  // origin).
  Vec2 center{};
  // The layer's world-space offset at p = center (1) (default
  // (0, 0) — the content origin).
  Vec2 offset{};
  // The layer rectangle's extent in world units (Image source only —
  // both > 0; ignored for the Tilemap source, which defines its own
  // grid).
  Vec2 size{1.0f, 1.0f};
  // The layer's texture sub-rect in its atlas (Image source only —
  // the M2-SPRITE-01 UV domain: 0 <= u0 < u1 <= 1, 0 <= v0 < v1 <= 1;
  // default the full texture).
  SpriteUvRect uv{0.0f, 0.0f, 1.0f, 1.0f};
  // The layer's texture (atlas) reference (Image source — the batch
  // group key's field 0; the scene's atlas-id convention, "The
  // render order", orders the layer's group).
  std::uint32_t atlasId{};
  // The tilemap's registry id (Tilemap source only — M2-TILE-02).
  std::uint32_t tilemapId{};
  // The material reference (0 = the default material — the group
  // key's field 1).
  std::uint32_t materialId{};
  // The blend state of every layer quad (the group key's field 3).
  BlendMode blend{BlendMode::Alpha};
  // The M2-ISO-01 key layer value of the layer's quads: the presets
  // are the kParallaxDepthLayer* constants; a custom layer picks any
  // value in the M2-ISO-01 domain [-512, +511] (more negative =
  // further back).
  std::int32_t depthLayer{};
  // Manual (the default) | Auto.
  ParallaxScrollMode scrollMode{ParallaxScrollMode::Manual};
  // The auto-mode advance: UV units PER FRAME per axis (any finite
  // value — negative scrolls the opposite way; ignored in Manual
  // mode, but still validated finite).
  Vec2 scrollSpeed{};
  // A disabled layer is skipped by declareTo (its state — including
  // the scroll offset — still evolves through advanceScrolls).
  bool enabled{true};

  // Field-wise (SpriteUvRect carries no operator== — the M2-SPRITE-01
  // value type): the uv rect is compared per field.
  friend bool operator==(const ParallaxLayerDef& a, const ParallaxLayerDef& b) {
    return a.id == b.id && a.source == b.source && a.factor == b.factor &&
           a.center == b.center && a.offset == b.offset && a.size == b.size &&
           a.uv.u0 == b.uv.u0 && a.uv.v0 == b.uv.v0 && a.uv.u1 == b.uv.u1 &&
           a.uv.v1 == b.uv.v1 && a.atlasId == b.atlasId &&
           a.tilemapId == b.tilemapId && a.materialId == b.materialId &&
           a.blend == b.blend && a.depthLayer == b.depthLayer &&
           a.scrollMode == b.scrollMode && a.scrollSpeed == b.scrollSpeed &&
           a.enabled == b.enabled;
  }
};

// The scene's parallax layer registry: the bounded slot table + the
// per-frame protocol (the header preamble). Templated over the SimMath
// backends (the M2-TILE-01 pattern): the declared quads' depth keys
// are the scene's backend's M2-ISO-01 keys (the cross-backend
// consistency contract). Header-only — pure value math + batcher
// bookkeeping, no GL, no allocation in the per-frame path.
template <typename Backend>
class ParallaxLayers {
  static_assert(std::is_same_v<Backend, laige::sim::Fpx16_16> ||
                    std::is_same_v<Backend, laige::sim::Fp32Pinned>,
                "ParallaxLayers is templated over the SimMath backends");

 public:
  // The create options (API-006): `maxLayers` in
  // [1, kParallaxLayersMaxLayers] (default
  // kParallaxLayersDefaultLayers = 8).
  struct Options {
    std::uint32_t maxLayers{kParallaxLayersDefaultLayers};
  };

  // Creates the registry (setup path — the only allocation: the
  // pre-sized slot table). maxLayers outside [1,
  // kParallaxLayersMaxLayers] -> InvalidArgument (no log — the
  // M2-SPRITE-01 create precedent). The stopped state (failed create
  // / default) follows the RenderThread precedent: `valid()` false,
  // every operation InvalidArgument, no log.
  [[nodiscard]] static laige::Result<ParallaxLayers, laige::ErrorCode>
  create(Options options) noexcept;

  // The stopped state (default / failed create): `valid()` false,
  // every operation InvalidArgument, no log — the RenderThread
  // stopped-state precedent. Nothing owned (the slot table is
  // null).
  ParallaxLayers() noexcept = default;

  // True iff the registry is live (create succeeded).
  [[nodiscard]] bool valid() const noexcept { return maxLayers_ > 0; }

  // The registry's slot domain (0 in the stopped state).
  [[nodiscard]] std::uint32_t maxLayers() const noexcept {
    return maxLayers_;
  }

  // The count of SET slots (setup-path accounting — changes only on
  // setLayer).
  [[nodiscard]] std::uint32_t layerCount() const noexcept {
    return layerCount_;
  }

  // True iff `id` is a live, SET layer.
  [[nodiscard]] bool has(std::uint32_t id) const noexcept {
    return maxLayers_ > 0 && id < maxLayers_ && slots_[id].valid;
  }

  // The set layer's definition.
  // Precondition: has(id) (asserted — check when the id comes from
  // untrusted input).
  [[nodiscard]] const ParallaxLayerDef& layerAt(std::uint32_t id)
      const noexcept {
    assert(has(id) && "ParallaxLayers::layerAt: layer not set");
    return slots_[id].def;
  }

  // The layer's CURRENT UV offset (in [0, 1)^2 — (0, 0) for an unset
  // slot). Precondition: has(id) (asserted).
  [[nodiscard]] Vec2 uvOffsetAt(std::uint32_t id) const noexcept {
    assert(has(id) && "ParallaxLayers::uvOffsetAt: layer not set");
    return slots_[id].uvOffset;
  }

  // The layer's world-space offset at camera position `cameraPos`
  // (the camera's ground-plane (x, y)) — the EXACT formula (1):
  // factor * (cameraPos - center) + offset. Pure O(1); no allocation,
  // no logging, no GL. Precondition: has(id) (checked-free read — the
  // M2-TILE-01 tileAt pattern; assert in debug).
  [[nodiscard]] Vec2 worldOffsetAt(std::uint32_t id, Vec2 cameraPos)
      const noexcept {
    assert(has(id) && "ParallaxLayers::worldOffsetAt: layer not set");
    const ParallaxLayerDef& d = slots_[id].def;
    return Vec2{d.factor * (cameraPos.x - d.center.x) + d.offset.x,
                d.factor * (cameraPos.y - d.center.y) + d.offset.y};
  }

  // Registers or REPLACES the layer (setup / config path — the scene
  // config's hot-reload of non-simulation config, FR-1.5). Validates
  // the def (the documented order, first failure wins); a rejection
  // leaves the slot unchanged (InvalidArgument + one rate-limited
  // parallax/layer_invalid warn — fields layer, field). A successful
  // (re)set RESETS the layer's UV offset to (0, 0) (the scroll
  // restarts — the def carries no scroll state). O(1); no GL.
  [[nodiscard]] laige::Status setLayer(const ParallaxLayerDef& def) noexcept;

  // Sets the layer's current UV offset (any finite value — wrapped
  // to [0, 1)^2). Works in BOTH scroll modes (a manual nudge on an
  // Auto layer composes with the per-frame advance). Unknown layer id
  // -> InvalidArgument (no log — precondition); non-finite value ->
  // InvalidArgument + one rate-limited parallax/uv_offset_invalid
  // warn; a rejection leaves the offset unchanged. O(1); no GL.
  [[nodiscard]] laige::Status setUvOffset(std::uint32_t id, Vec2 uvOffset)
      noexcept;

  // Advances the AUTO layers' UV offsets by their scrollSpeed (the
  // exact wrap, the header preamble) — ONCE PER FRAME, before the
  // declarations (the caller paces it at the frame's pace; the auto
  // speed is PER FRAME — frame-rate independence is the caller's
  // concern, the M2-CAM-01 lerp precedent). Manual layers are
  // untouched. O(layers); no allocation, no logging, no GL. No-op in
  // the stopped state.
  void advanceScrolls() noexcept;

  // The render path (the frame pipeline's cull/batch stage):
  // declares every SET, ENABLED Image-source layer's wrap quads into
  // `batcher` at camera position `cameraPos` (the 2 x 2 split, the
  // header preamble; the quads carry the def's group-key fields,
  // rotation 0, the full-default tint, and the M2-ISO-01 key of the
  // quad's world center at the def's depthLayer — engine-owned,
  // G-R11). Tilemap-source layers are skipped (the M2-TILE-02 hook —
  // no log). Declaration order: ascending layer id, then the fixed
  // quad order (RENDER-003). Precondition: the batcher's window is
  // open (a built frame -> InvalidArgument, nothing declared); the
  // stopped registry -> InvalidArgument. The batcher's own failures
  // (BudgetExhausted) propagate from the first failed add.
  // O(layers x 4) batcher adds; NO allocation, no logging, no GL.
  [[nodiscard]] laige::Status declareTo(SpriteBatcher& batcher,
                                        Vec2 cameraPos) const noexcept;

 private:
  // One slot's state: the definition + the current UV offset + the
  // set flag (the slot table is pre-sized at create — no per-frame
  // growth, PERF-003).
  struct LayerSlot {
    ParallaxLayerDef def;
    Vec2 uvOffset{0.0f, 0.0f};
    bool valid{false};
  };

  explicit ParallaxLayers(std::uint32_t maxLayers) noexcept
      : maxLayers_(maxLayers),
        layerCount_(0),
        slots_(std::make_unique<LayerSlot[]>(maxLayers)) {}

  // The [0, 1) wrap (the texture boundary): x - floor(x) — in [0, 1)
  // for every finite x; exact for dyadic x (1.0 -> exactly 0.0,
  // -0.25 -> exactly 0.75 — the tests pin it).
  // std::floor's float overload (the C++ standard guarantees it;
  // std::floorf does not — not every libstdc++ exposes the C suffix
  // overloads in std).
  static float wrapUv(float x) noexcept { return x - std::floor(x); }

  // The quad's M2-ISO-01 key position: the float world point in the
  // scene's backend (Fp32Pinned: identity; Fpx16_16: the backend's
  // Q16.16 conversion — the M2-TILE-01 float-conversion pattern).
  [[nodiscard]] typename laige::sim::SimMath<Backend>::Vec2
  backendVec2(float x, float y) const noexcept {
    using M = laige::sim::SimMath<Backend>;
    if constexpr (std::is_same_v<Backend, laige::sim::Fp32Pinned>) {
      return typename M::Vec2{x, y};
    } else {
      return typename M::Vec2{laige::fpx16_16::fromFloat(x),
                              laige::fpx16_16::fromFloat(y)};
    }
  }

  // Declares one wrap quad (the quad's world rectangle [min, max) +
  // its ATLAS uv rect) into the batcher: center + world-unit scale +
  // rotation 0 + the engine key of the center at the def's
  // depthLayer. Propagates the batcher's add result.
  [[nodiscard]] laige::Status declareQuad(SpriteBatcher& batcher,
                                          const ParallaxLayerDef& def,
                                          Vec2 worldMin, Vec2 worldMax,
                                          SpriteUvRect atlasUv)
      const noexcept {
    SpriteItem item{};
    const float cx = (worldMin.x + worldMax.x) * 0.5f;
    const float cy = (worldMin.y + worldMax.y) * 0.5f;
    item.pos = Vec2{cx, cy};
    item.scale = Vec2{worldMax.x - worldMin.x, worldMax.y - worldMin.y};
    item.rotation = 0.0f;
    item.uv = atlasUv;
    item.frameIndex = 0;
    item.atlasId = def.atlasId;
    item.materialId = def.materialId;
    item.blend = def.blend;
    item.depthOverride = false;
    item.depthKey = isoDepthKey<Backend>(backendVec2(cx, cy), 0,
                                         def.depthLayer);
    const auto r = batcher.add(item);
    if (!r.ok()) return r.error();
    return laige::Status{};
  }

  std::uint32_t maxLayers_{0};
  std::uint32_t layerCount_{0};
  std::unique_ptr<LayerSlot[]> slots_;  // empty <=> stopped
};

template <typename Backend>
laige::Result<ParallaxLayers<Backend>, laige::ErrorCode>
ParallaxLayers<Backend>::create(Options options) noexcept {
  if (options.maxLayers < 1 || options.maxLayers > kParallaxLayersMaxLayers) {
    return laige::Result<ParallaxLayers<Backend>, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  return laige::Result<ParallaxLayers<Backend>, laige::ErrorCode>::success(
      ParallaxLayers<Backend>(options.maxLayers));
}

template <typename Backend>
laige::Status ParallaxLayers<Backend>::setLayer(const ParallaxLayerDef& def)
    noexcept {
  if (maxLayers_ == 0) {
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  // The documented validation order (first failure wins — the
  // M2-CAM-01 options precedent): id -> factor -> center -> offset
  // -> scroll_speed -> size (Image) -> uv (Image) -> depth_layer.
  // Tilemap-source layers carry the tilemap's geometry (M2-TILE-02):
  // their size/uv are not validated here (ignored).
  const char* field = nullptr;
  if (def.id >= maxLayers_) {
    field = "id";
  } else if (!std::isfinite(def.factor) || def.factor < 0.0f ||
             def.factor > 1.0f) {
    field = "factor";
  } else if (!std::isfinite(def.center.x) || !std::isfinite(def.center.y)) {
    field = "center";
  } else if (!std::isfinite(def.offset.x) || !std::isfinite(def.offset.y)) {
    field = "offset";
  } else if (!std::isfinite(def.scrollSpeed.x) ||
             !std::isfinite(def.scrollSpeed.y)) {
    field = "scroll_speed";
  } else if (def.source == ParallaxSource::Image &&
             (!std::isfinite(def.size.x) || !std::isfinite(def.size.y) ||
              def.size.x <= 0.0f || def.size.y <= 0.0f)) {
    field = "size";
  } else if (def.source == ParallaxSource::Image &&
             (def.uv.u0 < 0.0f || def.uv.u1 > 1.0f ||
              def.uv.u0 >= def.uv.u1 || def.uv.v0 < 0.0f ||
              def.uv.v1 > 1.0f || def.uv.v0 >= def.uv.v1)) {
    field = "uv";
  } else if (def.depthLayer < -static_cast<std::int32_t>(kIsoDepthLayerBias) ||
             def.depthLayer > kIsoDepthLayerMax) {
    field = "depth_layer";
  }
  if (field != nullptr) {
    LAIGE_LOG_WARN("parallax", "layer_invalid",
                   "Rejected parallax layer definition",
                   laige::log::field("layer", def.id),
                   laige::log::field("field", field));
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  const std::uint32_t id = def.id;
  if (!slots_[id].valid) {
    ++layerCount_;
  }
  slots_[id].def = def;
  slots_[id].uvOffset = Vec2{0.0f, 0.0f};  // a (re)set restarts the scroll
  slots_[id].valid = true;
  return laige::Status{};
}

template <typename Backend>
laige::Status ParallaxLayers<Backend>::setUvOffset(std::uint32_t id,
                                                   Vec2 uvOffset) noexcept {
  if (maxLayers_ == 0 || id >= maxLayers_ || !slots_[id].valid) {
    // Precondition failure (unknown layer) — no log (the M2-TILE-01
    // rejected-edit precedent).
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  if (!std::isfinite(uvOffset.x) || !std::isfinite(uvOffset.y)) {
    LAIGE_LOG_WARN("parallax", "uv_offset_invalid",
                   "Rejected non-finite parallax UV offset",
                   laige::log::field("layer", id));
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  slots_[id].uvOffset =
      Vec2{wrapUv(uvOffset.x), wrapUv(uvOffset.y)};
  return laige::Status{};
}

template <typename Backend>
void ParallaxLayers<Backend>::advanceScrolls() noexcept {
  for (std::uint32_t i = 0; i < maxLayers_; ++i) {
    LayerSlot& s = slots_[i];
    if (!s.valid || s.def.scrollMode != ParallaxScrollMode::Auto) {
      continue;
    }
    s.uvOffset.x = wrapUv(s.uvOffset.x + s.def.scrollSpeed.x);
    s.uvOffset.y = wrapUv(s.uvOffset.y + s.def.scrollSpeed.y);
  }
}

template <typename Backend>
laige::Status ParallaxLayers<Backend>::declareTo(SpriteBatcher& batcher,
                                                 Vec2 cameraPos) const
    noexcept {
  if (maxLayers_ == 0) {
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  // The batcher's window must be open (a built frame rejects — the
  // M2-TILE-01 declareTo precedent; nothing declared).
  if (batcher.frameBuilt()) {
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  for (std::uint32_t i = 0; i < maxLayers_; ++i) {
    const LayerSlot& s = slots_[i];
    if (!s.valid || !s.def.enabled) {
      continue;
    }
    // The Tilemap source declares through the M2-TILE-02 tilemap
    // path (this layer's worldOffset + depthLayer) — skipped in
    // M2-PAR-01 (no log — the documented hook).
    if (s.def.source == ParallaxSource::Tilemap) {
      continue;
    }
    // The layer rectangle's world origin corner (formula 1).
    const Vec2 o = worldOffsetAt(i, cameraPos);
    const float ox = s.uvOffset.x;  // in [0, 1)
    const float oy = s.uvOffset.y;  // in [0, 1)
    const float sx = s.def.size.x;
    const float sy = s.def.size.y;
    // The wrap world lines (the texture wraps where the base uv +
    // offset reaches 1).
    const float wx = o.x + (1.0f - ox) * sx;
    const float wy = o.y + (1.0f - oy) * sy;
    const float xr = o.x + sx;
    const float yr = o.y + sy;
    // The layer's atlas sub-rect -> the quad's ATLAS uv rect over its
    // base uv range (the 2 x 2 split, the header preamble).
    const SpriteUvRect U = s.def.uv;
    const auto mapUv = [U](float bu0, float bv0, float bu1, float bv1) {
      return SpriteUvRect{U.u0 + bu0 * (U.u1 - U.u0),
                          U.v0 + bv0 * (U.v1 - U.v0),
                          U.u0 + bu1 * (U.u1 - U.u0),
                          U.v0 + bv1 * (U.v1 - U.v0)};
    };
    // q00: base uv [ox, 1) x [oy, 1) — always (the full rectangle at
    // offset (0, 0)).
    laige::Status st = declareQuad(batcher, s.def, Vec2{o.x, o.y},
                                   Vec2{wx, wy}, mapUv(ox, oy, 1.0f, 1.0f));
    if (!st.ok()) return st;
    // q10: base uv [0, ox) x [oy, 1) — only when ox > 0.
    if (ox > 0.0f) {
      st = declareQuad(batcher, s.def, Vec2{wx, o.y}, Vec2{xr, wy},
                       mapUv(0.0f, oy, ox, 1.0f));
      if (!st.ok()) return st;
    }
    // q01: base uv [ox, 1) x [0, oy) — only when oy > 0.
    if (oy > 0.0f) {
      st = declareQuad(batcher, s.def, Vec2{o.x, wy}, Vec2{wx, yr},
                       mapUv(ox, 0.0f, 1.0f, oy));
      if (!st.ok()) return st;
    }
    // q11: base uv [0, ox) x [0, oy) — only when both > 0.
    if (ox > 0.0f && oy > 0.0f) {
      st = declareQuad(batcher, s.def, Vec2{wx, wy}, Vec2{xr, yr},
                       mapUv(0.0f, 0.0f, ox, oy));
      if (!st.ok()) return st;
    }
  }
  return laige::Status{};
}

}  // namespace laige::render
