// laige-render tilemap (M2-TILE-01/02): the chunked tile grid data,
// the auto-depth wiring of the M2-ISO-02 depth key table, the static
// tile-quad batch path into the sprite batcher, the data-driven tile
// animation frame cycle (M2-TILE-02), and the parallax tile layer
// declaration (M2-TILE-02, the M2-PAR-01 Tilemap-source hook).
//
// FR-2.6: "Tilemap: chunks, per-tile depth/height, auto-depth" +
// "tile animation" + "parallax tile layers" — a chunked grid of
// tiles; per tile: a texture reference, a depth/height value, and an
// animation reference; a tile's Y height is AUTOMATICALLY reflected
// in its depth key (the wiring into M2-ISO-02); an animated tile's UV
// frame CYCLES from its animation's documented tick rate (M2-TILE-02);
// a tilemap can be a PARALLAX LAYER's content (M2-TILE-02 — the
// tiles are declared under the layer's worldOffset translation,
// M2-PAR-01). S-5 (PRD §9.1): rendering goes through the batcher —
// the tilemap's quads are declared into the sprite batcher as
// sprites, never drawn directly. ARCH-009: the tile data and the
// animation frame state are presentation-side (headless-buildable,
// sim-side in the sense that they never need GL); the render phase
// consumes them read-only. G-R11: the tile's depth key is
// engine-owned (the table's / the M2-ISO-01 function's) — game code
// never writes the key. RENDER-003: the declaration order is
// deterministic (the tile grid's row-major iteration).
//
//   TileData               one tile's data (texture id, height,
//                          animation id)
//   TileAnimationDef       one tile animation's definition (the frame
//                          count, the tick rate, the sheet layout)
//   TileMap<Backend>       the chunked tile grid (owns the table +
//                          the animation slots)
//   TileMap::DeclareOptions the per-frame declare options
//
// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
//
// The tilemap covers a RECTANGLE of tile coordinates
//
//   [originTileX, originTileX + widthTiles) x
//   [originTileY,   originTileY + heightTiles)
//
// of integer tile indices — the same grid the M2-ISO-02 depth key
// table holds. Tile `(gx, gy)` is the world cell `[gx, gx+1) x
// [gy, gy+1)` at the world origin (coordinates.md §5.1 — the grid the
// M2-CAM-02 grid-snap camera and the M2-ISO-03 picker resolve to), so
// the tilemap is grid-locked by construction: its cells agree with
// `isoDepthKey` at the same positions (the table's bit-identity
// contract).
//
// The tilemap OWNS one `IsoDepthKeyTable<Backend>` (the same options —
// the table's covered region is the chunk-aligned SUPERSET of the
// requested grid) and stores, per tile of the REQUESTED grid, the
// remaining per-tile data in one flat pre-sized array (row-major,
// tileX fastest — the table's flat order):
//
//   textureId    the texture/atlas reference (the game assigns it —
//                M3-ASSET-01 owns the asset system; the batcher's
//                group key carries it unchanged)
//   animationId  the animation reference (M2-TILE-02 — the batch path
//                CONSUMES it, below): 0 = a STATIC tile (the fixed
//                frame); 1..maxAnimations = the animation slot the
//                tile cycles from (the animation's sheet texture is
//                the tile's own textureId — the game assigns the
//                matching texture)
//
// The tile's HEIGHT lives in the table alone (one source of truth —
// the auto-depth wiring): a height edit through the tilemap goes into
// `setTile`/`rebuild` below, which route into the table's
// `setTile`/`rebuild` and recompute exactly that cell's key (the
// M2-ISO-02 incremental-update contract, radius 0). `tileAt` returns
// the three fields combined (the height read from the table).
//
// ---------------------------------------------------------------------------
// The quad model (the batch path)
// ---------------------------------------------------------------------------
//
// A tile renders as a SPRITE (the M2-SPRITE-01 quad):
//
//   pos          the tile's CENTER (gx + 0.5, gy + 0.5) — the same
//                point the table quantizes (the float conversion is
//                exact for |gx| <= 32766 — dyadic)
//   scale        (1, 1) — the unit quad spans the tile's world cell
//   rotation     0
//   uv           the tile's CURRENT frame (M2-TILE-02): the STATIC
//                tile's fixed frame (`DeclareOptions::uv` — default
//                (0, 0, 1, 1) = the full tile texture); the ANIMATED
//                tile's frame UV sub-rect (its animation's sheet
//                frame — the `spriteFrameUv` output, M2-SPRITE-03)
//   frameIndex   the tile's CURRENT animation frame (0 for the static
//                tile — the M2-SPRITE-03 hook the M3 animation
//                drives; the batcher carries it untouched)
//   depthKey     the table's key for the cell (AUTO-DEPTH — the game
//                never computes it, G-R11; the item's
//                `depthOverride` stays false)
//   atlasId      the tile's textureId
//
// BOUNDED DRAW CALLS: `declareTo` declares one sprite per tile; the
// batcher's (atlas, material, blend) grouping then renders the
// tilemap in one draw call PER DISTINCT (textureId, material, blend)
// combination (FR-2.1, M2-SPRITE-01) — tiles of one chunk sharing one
// texture and blend form ONE group (one draw call per chunk group).
// The number of draw calls is a function of the distinct group keys,
// never of the tile count (RENDER-001).
//
// ---------------------------------------------------------------------------
// Tile animation (M2-TILE-02 — the data-driven frame cycle)
// ---------------------------------------------------------------------------
//
// An ANIMATION is a slot of the tilemap's pre-sized animation table
// (`Options::maxAnimations`, domain [1, kTileMapMaxAnimations]): the
// scene SETS it (setup / config path — `setAnimation`, like the
// parallax `setLayer`): the frame COUNT, the documented TICK RATE
// (`frameTicks` — the simulation ticks per frame), and the tile
// SHEET's frame layout (the M2-SPRITE-03 `SpriteFrameLayout`: frame
// size, row/col, margins, texels). A tile is ANIMATED when its
// `animationId` is in [1, maxAnimations]; 0 is the static sentinel
// (the flat/empty default — the tile's UV is the fixed frame).
//
// The frame CYCLE is the engine's per-tick advance (the scene owner
// calls `advanceAnimations()` ONCE PER SIM TICK — the sim phase, the
// M2-GL-02 frame pipeline's sim-side pacing; ARCH-002: the rate is
// per SIM tick, never per render frame — the cycle is
// frame-rate-independent): every SET animation advances one tick per
// call, and its frame steps forward every `frameTicks` ticks:
//
//   frame(ticks) = (ticks / frameTicks) mod frameCount
//
// (the slot's tick counter implements exactly this — no division per
// call). The cycle WRAPS (frameCount-1 -> 0); the cycled frames are
// the sheet's FIRST `frameCount` frames, row-major (frame 0 = the
// sheet's top-left, the M2-SPRITE-03 layout). A (re)set RESETS the
// phase (frame 0, tick 0). All tiles of one animation share its
// phase (frame 0 at the first tick; per-tile phase offsets are the
// M3 animation editor's control — the roadmap's "data-driven" scope:
// the editor authors the defs, the engine cycles them).
//
// The frame's UV is PRECOMPUTED at `setAnimation` (one `spriteFrameUv`
// per frame — the setup path): the per-frame declare path only READS
// the stored UV + frame (zero allocation, no division — FR-2.2).
// The sheet is the layout's TIGHT sheet (the M2-SPRITE-03 formula —
// derived, not a def field); a padded atlas is the M3-ASSET-01 asset
// system's concern. The frame state is PRESENTATION state (ARCH-009)
// — never part of replay state or the simulation state hash (the
// parallax scroll-offset precedent).
//
// ---------------------------------------------------------------------------
// The parallax tile layer (M2-TILE-02 — the M2-PAR-01 Tilemap-source
// hook)
// ---------------------------------------------------------------------------
//
// A parallax layer with the `Tilemap` source (the M2-PAR-01
// registry) names a tilemap; the tilemap declares its quads UNDER
// that layer (the `declareTo` overload below): every tile quad is
// TRANSLATED by the layer's `worldOffset(cameraPos)` (the M2-PAR-01
// formula (1) — the layer's factor/center/offset against the
// camera's presentation position, world space, RENDER-006) and its
// depth key is the M2-ISO-01 key of the TRANSLATED tile center at
// the tilemap's own `layer` (the scene-setup convention: the layer's
// def `depthLayer` equals the tilemap's `Options::layer` — the
// bg/mid/fg values, parallax.h — the tilemap is the one source of
// truth for its content's layer).
//
// The table's keys are at the UNtranslated positions, so the
// translated keys are computed per tile per frame (the translation
// is camera-dependent — not precomputable; O(tileCount) key
// computations, zero allocation, no GL — part of the composite 50k
// budget; the qBase + qOffset derivation is the documented upgrade
// path if a profile ever shows it matters).
//
// ---------------------------------------------------------------------------
// Determinism (RENDER-003, ARCH-010 scope — presentation-only)
// ---------------------------------------------------------------------------
//
// `declareTo` declares the tiles in the tile grid's ROW-MAJOR order
// (tileY outer, tileX fastest — the flat array's order, cache-friendly).
// The tile's grid position is a static tile's stable identity (the
// FR-1.2 entity-order analog): the declaration order is the insertion
// order the M2-SORT-01 stable sort turns into the (key, insertion
// position) total order — same tile data + same animation state →
// bit-identical batches, every frame and every platform. The
// animation state is a deterministic function of the
// `advanceAnimations` call sequence (no RNG, no clock — the scene
// paces the ticks).
//
// ---------------------------------------------------------------------------
// Ownership, lifetime, threading
// ---------------------------------------------------------------------------
//
// One owner — the sim/scene-owner thread (the scene; the M2-GL-02
// frame pipeline's cull/batch stage owns the render-side declaration).
// The tilemap is move-only (CORE-009); the storage is pre-sized at
// creation (the ONLY allocations are `create` — one table storage +
// one 8 B/tile data array + the animation slot array — `rebuild`'s
// setup-path temporary, and `setAnimation`'s per-frame-UV array —
// the setup/config path, never the per-frame path). Writes (`setTile`,
// `rebuild`, `setAnimation`) happen in the sim/config phase; the
// per-tick `advanceAnimations` advances the presentation frame state
// (sim phase); reads (`tileAt`, `depthKeyAt`, `tileHeightAt`,
// `covers`, `declareTo`) in the render phase; the phases do not
// overlap (CONC-001, the table's contract — one owner per datum). Not
// thread-safe by design.
//
// The per-frame `declareTo` path ALLOCATES NOTHING (FR-2.2
// "no per-frame allocation"): the adds are O(1) batcher operations
// over pre-allocated storage (the zero-allocation proof, the tests).
//
// ---------------------------------------------------------------------------
// Failure behavior (no silent failure, CORE-008)
// ---------------------------------------------------------------------------
//
// All failure paths return the `Status`/`Result` — the caller handles
// and logs (LOG-002: the engine does not duplicate a per-call log).
// Rejected operations leave ALL state (tile data, the table, and the
// animation slots) unchanged.
//
//   setAnimation    id 0 (the static sentinel) or > maxAnimations;
//                   frameCount outside [1, kTileAnimMaxFrames];
//                   frameTicks < 1; a zero sheet extent; frameCount
//                   beyond the sheet's frame count; the tight sheet
//                   beyond the float-exact domain (2^24 texels) —
//                   all InvalidArgument, no log, state unchanged
//   setTile/rebuild an animationId beyond maxAnimations (0 is always
//                   valid — the static sentinel) — InvalidArgument,
//                   no log, state unchanged (the WHOLE span
//                   validated before any write, rebuild)
//   declareTo       a built frame's closed window (nothing declared);
//                   an animated tile whose animation slot is UNSET
//                   (first failure wins — nothing declared past it
//                   this frame; the slot's set is the scene's setup
//                   responsibility)
//   declareTo (parallax) a built frame's closed window; an unset
//                   layer id or a non-Tilemap-source layer
//                   (InvalidArgument, no log — the setup mispairing
//                   is the caller's); a DISABLED layer declares
//                   NOTHING (OK, no log — the layer's documented
//                   skip, the parallax declareTo's precedent)
//
// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------
//
// No standalone `budgets.json` entry: the per-frame declare cost
// (O(tiles) adds + the sort; the parallax path's O(tiles) key
// computations) is PART of the composite 50k render-CPU budget
// (PRD §8.1, `sprites_50k_cpu` — M2-PERF-01 measures the reference
// scene with tile quads included; `declareTo` declares the WHOLE
// requested grid — visible-rect culling lands with M2-PERF-01, and
// the budget's worst case is the full grid anyway). The per-tick
// `advanceAnimations` is O(animations) — negligible next to the
// sim tick's budget (the animations are a scene-config count, not a
// per-entity count).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
// - Don't hand-write the tile's depth: the key is the table's
//   (auto-depth, G-R11). Setting `depthOverride` on a tile sprite
//   defeats the wiring (and counts/warns through the batcher).
// - Don't declare more than the batcher's frame budget: the frame is
//   bounded (M2-SPRITE-01 overflow policy — drop the oldest + warn).
// - Don't call `declareTo` on a built frame (the window is closed —
//   `beginFrame` first); the precondition is checked (first failure
//   wins, nothing declared).
// - `tileAt`/`depthKeyAt`/`tileHeightAt` on a tile OUTSIDE the
//   requested grid is undefined behavior (program bug): check
//   `covers()` when tile coordinates come from untrusted input (the
//   batch path reads only the requested grid by construction — it
//   declares none of the table's superset margin).
// - Don't recreate the tilemap per frame: the table IS the
//   precomputation (FR-2.2 "not recomputed per frame" — the M2-ISO-02
//   anti-pattern).
// - `advanceAnimations` is ONCE PER SIM TICK: the frame rate is
//   `frameTicks` sim ticks, not render frames (ARCH-002); calling it
//   per render frame changes the animation speed (and is wrong under
//   variable tick pacing).
// - A parallax tile LAYER's def `depthLayer` must equal the
//   tilemap's `Options::layer` (the scene-setup convention — both are
//   the content's layer; the declaration uses the tilemap's): the
//   bg/mid/fg tilemaps get layer values -2/-1/+1 (parallax.h).
// - `DeclareOptions::uv` is the STATIC tiles' fixed frame: animated
//   tiles carry their animation's frame UV (this field is ignored
//   for them).

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "laige/errors.h"
#include "laige/result.h"
#include "laige/render/iso_depth_key.h"
#include "laige/render/iso_depth_table.h"
#include "laige/render/matrices.h"
#include "laige/render/parallax.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/sprite_frames.h"
#include "laige/sim_math.h"

namespace laige::render {

// The frame budget of ONE tile animation (M2-TILE-02): the practical
// tile-sheet frame count (a 16 x 4 sheet); a longer cycle is the
// game's split into two animations (the M3 animation editor owns the
// authoring). The per-animation setup storage is frameCount x
// sizeof(SpriteUvRect) (1 KB at the cap).
inline constexpr std::uint32_t kTileAnimMaxFrames = 64;

// The tilemap's animation slot count: the default — the reference
// scene's handful of animated tiles + headroom for custom (the
// M2-SCENE-01 precedent, the parallax default-layers pattern) — and
// the cap: the practical tileset's full animation catalog (256 slots
// ~= 14 KB of setup storage).
inline constexpr std::uint32_t kTileMapDefaultAnimations = 8;
inline constexpr std::uint32_t kTileMapMaxAnimations = 256;

// One tile's data (the value the game writes on load/edit and reads
// back; plain value — no GL, no allocation).
struct TileData {
  // The texture/atlas reference (the game assigns it — M3-ASSET-01).
  std::uint32_t textureId{};
  // The tile's step height in world units (the standing surface's
  // elevation — the auto-depth source, the M2-ISO-01 `z` input).
  std::int32_t height{};
  // The animation reference (M2-TILE-02 — the batch path consumes
  // it): 0 = a STATIC tile (the fixed frame); 1..maxAnimations = the
  // animation slot the tile cycles from (its sheet texture is the
  // tile's own textureId — the game assigns the matching texture).
  std::uint32_t animationId{};

  friend constexpr bool operator==(const TileData&, const TileData&) = default;
};

// One tile animation's definition (M2-TILE-02 — the data-driven frame
// cycle; plain value — no GL, no allocation).
struct TileAnimationDef {
  // The frames the cycle plays: the sheet's FIRST frameCount frames,
  // row-major (frame 0 = the sheet's top-left); the domain
  // [1, kTileAnimMaxFrames] (and <= the sheet's frame count).
  std::uint32_t frameCount{};
  // The documented rate: the SIMULATION ticks per frame (>= 1) — the
  // frame advances every frameTicks calls to advanceAnimations
  // (ARCH-002: per sim tick, never per render frame).
  std::uint32_t frameTicks{};
  // The tile sheet's frame layout (texels — the M2-SPRITE-03 layout:
  // frameWidth/Height, columns/rows, frameSpacing, sheetBorder).
  SpriteFrameLayout layout{};
};

// The tile grid + its depth table (M2-ISO-02) + the animation slots
// (M2-TILE-02) + the batch path.
//
// Templated over the SimMath backends (the presentation.h pattern —
// the keys are backend-typed; fpx16_16 and fp32_pinned agree
// bit-for-bit on the grid-locked tile centers — dyadic, inside the
// exactness zone).
template <typename Backend>
class TileMap {
  static_assert(std::is_same_v<Backend, laige::sim::Fpx16_16> ||
                    std::is_same_v<Backend, laige::sim::Fp32Pinned>,
                "TileMap is templated over the SimMath backends");

 public:
  // The create options: the table's grid options + the animation slot
  // count (first failure wins — `maxAnimations` first, then the
  // table's create validates the grid options — all InvalidArgument):
  // `originTileX/Y` (default 0), `widthTiles`/`heightTiles` (required
  // >= 1), `chunkTiles` (default `kIsoDepthTableChunkTiles` = 16, a
  // power of two >= 1), `maxChunks` (default
  // `kIsoDepthTableDefaultMaxChunks` = 1024), `layer` (default
  // `kIsoDepthGroundLayer` = 0 — a parallax tile LAYER gets its own
  // tilemap with its layer value, M2-PAR-01 — the values are
  // documented in laige/render/parallax.h), `maxAnimations`
  // (default `kTileMapDefaultAnimations` = 8; the domain
  // [1, `kTileMapMaxAnimations` = 256]).
  struct Options {
    std::int32_t originTileX = 0;
    std::int32_t originTileY = 0;
    std::int32_t widthTiles = 0;    // required >= 1
    std::int32_t heightTiles = 0;   // required >= 1
    std::int32_t chunkTiles = kIsoDepthTableChunkTiles;
    std::int32_t maxChunks = kIsoDepthTableDefaultMaxChunks;
    std::int32_t layer = kIsoDepthGroundLayer;
    std::uint32_t maxAnimations = kTileMapDefaultAnimations;
  };

  // The per-frame declare options (the fixed-frame quad model, above):
  // the group-key fields for every tile sprite + the static tiles'
  // fixed UV frame.
  struct DeclareOptions {
    // The material reference (0 = the default material).
    std::uint32_t materialId{0};
    // The blend state of every tile sprite (the group key's third
    // field).
    BlendMode blend{BlendMode::Alpha};
    // The STATIC tiles' fixed frame: every static tile quad's UV
    // sub-rect (default (0, 0, 1, 1) = the full tile texture).
    // ANIMATED tiles carry their animation's frame UV (the layout) —
    // this field is ignored for them.
    SpriteUvRect uv{0.0f, 0.0f, 1.0f, 1.0f};
  };

  // Creates the tilemap (setup path — the allocations: the table's
  // storage + the per-tile data array + the animation slot array):
  // validates `maxAnimations` (first), then the table's create
  // validates the grid options (first failure wins, InvalidArgument),
  // pre-sizes the table's initial grid chunks, the per-tile data
  // array (8 B/tile, the requested grid), and the animation slot
  // array (unset slots). Flat/empty init: every tile is
  // {textureId 0, height 0, animationId 0} — no tile is set, no
  // animation is set.
  [[nodiscard]]
  static Result<TileMap> create(const Options& options) noexcept;

  // The per-tile edit (sim phase). Stores the tile's textureId and
  // animationId and routes the height into the table's setTile —
  // the AUTO-DEPTH wiring: the table stores the height and recomputes
  // exactly that cell's key (the M2-ISO-02 incremental update, radius
  // 0; the other cells' keys are untouched). O(1), zero allocation.
  //
  // Rejected edits (tile outside the REQUESTED grid — the superset
  // margin cells are table cells, not tiles; height outside the key
  // domain, |h| <= kIsoDepthMaxStepHeight; animationId beyond
  // maxAnimations — 0 is always valid, the static sentinel) leave
  // the tile data AND the table UNCHANGED (validated before any
  // write); the returned Status is the failure channel the caller
  // handles and logs (LOG-002).
  [[nodiscard]]
  Status setTile(std::int32_t tileX, std::int32_t tileY,
                 std::uint32_t textureId, std::int32_t height,
                 std::uint32_t animationId) noexcept;

  // Scene load (setup path — the only place besides create that
  // allocates: one temporary covered-height span for the table's
  // from-scratch rebuild). Stores the whole tile grid: the per-tile
  // data and the heights through the table's rebuild (every key
  // through the full M2-ISO-01 function). `tiles` covers the
  // REQUESTED grid — `widthTiles() * heightTiles()` entries,
  // row-major, tileX fastest.
  //
  // Property: rebuild(final grid) == any sequence of setTile calls
  // reaching the same grid (the M2-ISO-02 property, through the
  // tilemap — the test pins it).
  //
  // Rejected loads (wrong span size; any height outside the key
  // domain; any animationId beyond maxAnimations — validated over
  // the WHOLE span before any write) leave the tile data AND the
  // table UNCHANGED.
  [[nodiscard]]
  Status rebuild(std::span<const TileData> tiles) noexcept;

  // Sets or REPLACES the animation slot `id` (setup / config path —
  // the scene config's hot-reload, the parallax setLayer precedent):
  // the frame count, the tick rate, and the sheet layout (the header's
  // animation section). A success RESETS the slot's phase (frame 0,
  // tick 0) and replaces the precomputed frame-UV array (one setup
  // allocation — the only per-animation allocation).
  //
  // Rejections (InvalidArgument, no log, slot unchanged — the header's
  // failure section): id 0 (the static sentinel) or > maxAnimations;
  // frameCount outside [1, kTileAnimMaxFrames]; frameTicks < 1; a
  // zero sheet extent; frameCount beyond the sheet's frame count
  // (columns x rows); the tight sheet beyond the float-exact domain
  // (kSpriteFrameMaxAtlasTexels = 2^24 texels — the M2-SPRITE-03
  // domain, guarded against the adversarial layout's u64 overflow).
  [[nodiscard]]
  Status setAnimation(std::uint32_t id, const TileAnimationDef& def)
      noexcept;

  // The sim phase's per-tick call (ONCE PER SIM TICK — the header's
  // animation section): advances every SET animation's tick, and
  // steps its frame forward every frameTicks ticks (the documented
  // rate). O(maxAnimations), zero allocation, no logging, no GL.
  // Unset slots are skipped (no state). The presentation frame state
  // is never part of the simulation state hash (ARCH-009).
  void advanceAnimations() noexcept;

  // The render path (the frame pipeline's cull/batch stage): declares
  // this tilemap's static tile quads into the batcher's current frame
  // window — one SpriteItem per tile of the REQUESTED grid, in the
  // grid's row-major order (tileY outer, tileX fastest — the preamble's
  // quad model: the tile's center, the table's key (auto-depth), the
  // tile's texture, the tile's current frame — the static fixed frame
  // or the animation's frame UV). O(tileCount), zero allocation, no
  // GL, no logging — the adds are O(1) batcher operations.
  //
  // Precondition: the batcher's frame window is open (a built frame's
  // window is closed — call beginFrame first; the check is here,
  // first failure wins, NOTHING declared on failure). An animated
  // tile whose animation slot is UNSET fails the declare (first
  // failure wins — the header's failure section); the slot's set is
  // the scene's setup responsibility. On a stopped batcher
  // (capacity 0) the first add fails — BudgetExhausted, nothing
  // declared. The WHOLE requested grid is declared (visible-rect
  // culling lands with M2-PERF-01 — the composite 50k budget's worst
  // case is the full grid).
  [[nodiscard]]
  Status declareTo(SpriteBatcher& batcher,
                   const DeclareOptions& options) noexcept;

  // The parallax tile layer declare path (M2-TILE-02 — the M2-PAR-01
  // Tilemap-source hook): declares this tilemap's tile quads
  // TRANSLATED by the parallax layer `layerId`'s worldOffset
  // (the M2-PAR-01 formula (1) — the layer's factor/center/offset
  // against `cameraPos`, the camera's presentation position) — the
  // header's parallax section: the depth key is the M2-ISO-01 key of
  // the TRANSLATED tile center at the tilemap's own `layer` (the
  // scene-setup convention: the layer's def `depthLayer` equals the
  // tilemap's `Options::layer`; the declaration uses the tilemap's
  // value — one source of truth). O(tileCount), zero allocation, no
  // GL, no logging.
  //
  // Precondition: the batcher's frame window is open (checked here,
  // first failure wins, nothing declared). An unset layer id or a
  // non-Tilemap-source layer is InvalidArgument (no log — the setup
  // mispairing is the caller's). A DISABLED layer declares NOTHING
  // (OK, no log — the layer's documented skip). The animated/static
  // frame handling is the same as `declareTo`.
  [[nodiscard]]
  Status declareTo(SpriteBatcher& batcher, const DeclareOptions& options,
                   const ParallaxLayers<Backend>& layers,
                   std::uint32_t layerId, Vec2 cameraPos) noexcept;

  // The read path (render phase, O(1), zero allocation, no GL):
  [[nodiscard]] TileData tileAt(std::int32_t tileX,
                                std::int32_t tileY) const noexcept;
  [[nodiscard]] std::uint32_t depthKeyAt(std::int32_t tileX,
                                         std::int32_t tileY) const noexcept;
  [[nodiscard]] std::int32_t tileHeightAt(std::int32_t tileX,
                                          std::int32_t tileY) const noexcept;
  // Whether the tile lies in the REQUESTED grid (the batch path and
  // the tileAt/depthKeyAt/tileHeightAt preconditions). The table's
  // covered region may extend past the requested grid (its chunk-
  // aligned superset) — those cells are table cells, not tiles.
  [[nodiscard]] bool covers(std::int32_t tileX,
                            std::int32_t tileY) const noexcept;

  // Introspection (O(1); the table-backed values forward the table):
  std::int32_t originTileX() const noexcept { return options_.originTileX; }
  std::int32_t originTileY() const noexcept { return options_.originTileY; }
  std::int32_t widthTiles() const noexcept { return options_.widthTiles; }
  std::int32_t heightTiles() const noexcept { return options_.heightTiles; }
  std::int32_t layer() const noexcept { return table_.layer(); }
  std::int32_t chunkTiles() const noexcept { return table_.chunkTiles(); }
  // The animation slot count (the tilemap's `Options::maxAnimations`).
  std::uint32_t maxAnimations() const noexcept {
    return options_.maxAnimations;
  }
  // The requested grid's tile count (widthTiles * heightTiles).
  std::size_t tileCount() const noexcept {
    return static_cast<std::size_t>(options_.widthTiles) *
           static_cast<std::size_t>(options_.heightTiles);
  }
  // Whether the animation slot `id` is SET (the declare path's
  // check; id 0 is the static sentinel — never set).
  [[nodiscard]] bool hasAnimation(std::uint32_t id) const noexcept {
    return id >= 1 && id <= options_.maxAnimations && anims_[id].valid;
  }
  // The set animation's definition.
  // Precondition: hasAnimation(id) (asserted — check when the id
  // comes from untrusted input).
  [[nodiscard]] const TileAnimationDef& animationAt(std::uint32_t id)
      const noexcept {
    assert(hasAnimation(id) && "TileMap::animationAt: animation not set");
    return anims_[id].def;
  }
  // The set animation's CURRENT frame (0..frameCount-1) — the frame
  // its tiles carry on the next declare (diagnostics view — DBG-008).
  // Precondition: hasAnimation(id) (asserted).
  [[nodiscard]] std::uint32_t animationFrame(std::uint32_t id) const noexcept {
    assert(hasAnimation(id) &&
           "TileMap::animationFrame: animation not set");
    return anims_[id].frame;
  }

  TileMap(const TileMap&) = delete;
  TileMap& operator=(const TileMap&) = delete;
  TileMap(TileMap&&) noexcept = default;
  TileMap& operator=(TileMap&&) noexcept = default;
  ~TileMap() = default;

 private:
  explicit TileMap(Options options, IsoDepthKeyTable<Backend> table) noexcept
      : options_(options),
        table_(std::move(table)),
        tiles_(std::make_unique<TileSlot[]>(tileCount())),
        // maxAnimations + 1: the id domain is 1..maxAnimations
        // (inclusive) — index 0 is the unused sentinel slot:
        anims_(std::make_unique<AnimSlot[]>(options.maxAnimations + 1)) {}

  // The per-tile data slot (the requested grid; the height lives in
  // the table — one source of truth). 8 B/tile.
  struct TileSlot {
    std::uint32_t textureId{};
    std::uint32_t animationId{};
  };

  // One animation slot (pre-sized at create; the frame-UV array is
  // the setAnimation setup allocation):
  struct AnimSlot {
    TileAnimationDef def{};
    std::uint32_t frame{0};  // the current frame (0..frameCount-1)
    std::uint32_t tick{0};   // the ticks since the frame started
    bool valid{false};
    // The precomputed frame UVs (frameCount rects — setAnimation).
    std::unique_ptr<SpriteUvRect[]> frameUv;
  };

  // The float -> backend conversion (the ParallaxLayers::backendVec2
  // pattern — the Fp32Pinned identity / the fpx16_16's fromFloat;
  // the translated tile centers are the parallax path's key input).
  typename laige::sim::SimMath<Backend>::Vec2 backendVec2(float x,
                                                          float y) const
      noexcept {
    using M = laige::sim::SimMath<Backend>;
    if constexpr (std::is_same_v<Backend, laige::sim::Fp32Pinned>) {
      return typename M::Vec2{x, y};
    } else {
      return typename M::Vec2{laige::fpx16_16::fromFloat(x),
                              laige::fpx16_16::fromFloat(y)};
    }
  }

  // The flat index of the requested-grid tile (row-major, tileX
  // fastest). Precondition: covers().
  std::size_t requestedIndex(std::int32_t tileX,
                             std::int32_t tileY) const noexcept {
    return static_cast<std::size_t>(tileY - options_.originTileY) *
               static_cast<std::size_t>(options_.widthTiles) +
           static_cast<std::size_t>(tileX - options_.originTileX);
  }

  // The per-tile frame fields (shared by both declare paths): the
  // static tile's fixed frame (`options.uv`, frameIndex 0) or the
  // animated tile's animation's CURRENT frame (the precomputed frame
  // UV, frameIndex = the frame). First failure wins: an animated tile
  // whose animation slot is UNSET fails the declare (the header's
  // failure section — the slot's set is the scene's setup
  // responsibility). `i` indexes tiles_ (the requested grid's row-
  // major order — the caller's loop position).
  Status applyTileFrame(SpriteItem& item, const DeclareOptions& options,
                        std::size_t i) const noexcept {
    const std::uint32_t animId = tiles_[i].animationId;
    if (animId == 0) {
      item.uv = options.uv;
      item.frameIndex = 0;
      return Status{};
    }
    // animId is in [1, maxAnimations] (setTile/rebuild validate the
    // domain — the index is safe):
    const AnimSlot& a = anims_[animId];
    if (!a.valid) return Status(ErrorCode::InvalidArgument);
    item.frameIndex = a.frame;
    item.uv = a.frameUv[a.frame];
    return Status{};
  }

  Options options_;
  IsoDepthKeyTable<Backend> table_;
  std::unique_ptr<TileSlot[]> tiles_;
  std::unique_ptr<AnimSlot[]> anims_;
};

template <typename Backend>
Result<TileMap<Backend>> TileMap<Backend>::create(const Options& options)
    noexcept {
  // `maxAnimations` first (the tilemap's own field — first failure
  // wins; both failures are InvalidArgument), then the table's create
  // validates the identical grid options (its documented order). The
  // create path is the setup allocation (table + tile data +
  // animation slots — the constructor).
  if (options.maxAnimations < 1 ||
      options.maxAnimations > kTileMapMaxAnimations) {
    return Result<TileMap>::failure(ErrorCode::InvalidArgument);
  }
  typename IsoDepthKeyTable<Backend>::Options tableOptions;
  tableOptions.originTileX = options.originTileX;
  tableOptions.originTileY = options.originTileY;
  tableOptions.widthTiles = options.widthTiles;
  tableOptions.heightTiles = options.heightTiles;
  tableOptions.chunkTiles = options.chunkTiles;
  tableOptions.maxChunks = options.maxChunks;
  tableOptions.layer = options.layer;
  auto table = IsoDepthKeyTable<Backend>::create(tableOptions);
  if (!table.ok()) return Result<TileMap>::failure(table.error());
  TileMap map(options, std::move(table).takeValue());
  return Result<TileMap>::success(std::move(map));
}

template <typename Backend>
Status TileMap<Backend>::setTile(std::int32_t tileX, std::int32_t tileY,
                                 std::uint32_t textureId, std::int32_t height,
                                 std::uint32_t animationId) noexcept {
  // Boundary validation (first failure wins — all failures are
  // InvalidArgument, so the order is unobservable): the tile lies in
  // the requested grid, the height is in the key domain (the table's
  // setTile contract), and the animationId is in the slot domain
  // (0 = the static sentinel; 1..maxAnimations — the slot may be
  // UNSET: the declare path's check, not the edit's).
  if (tileX < options_.originTileX ||
      tileX >= options_.originTileX + options_.widthTiles ||
      tileY < options_.originTileY ||
      tileY >= options_.originTileY + options_.heightTiles ||
      height < -kIsoDepthMaxStepHeight ||
      height > kIsoDepthMaxStepHeight ||
      animationId > options_.maxAnimations) {
    return Status(ErrorCode::InvalidArgument);
  }
  // The auto-depth wiring (FR-2.6): the height goes into the table,
  // which recomputes exactly this cell's key (radius 0).
  const Status s = table_.setTile(tileX, tileY, height);
  if (!s.ok()) return s;  // unreachable (validated above) — honest channel
  // The tile data is written AFTER the depth edit succeeded: a
  // rejected edit leaves both the data and the table unchanged.
  TileSlot& slot = tiles_[requestedIndex(tileX, tileY)];
  slot.textureId = textureId;
  slot.animationId = animationId;
  return Status{};
}

template <typename Backend>
Status TileMap<Backend>::rebuild(std::span<const TileData> tiles) noexcept {
  const std::size_t cells = tileCount();
  if (tiles.size() != cells) return Status(ErrorCode::InvalidArgument);
  // The height + animationId domains — validated over the WHOLE span
  // before any write (a rejected load leaves both the data and the
  // table unchanged):
  for (std::size_t i = 0; i < cells; ++i) {
    if (tiles[i].height < -kIsoDepthMaxStepHeight ||
        tiles[i].height > kIsoDepthMaxStepHeight ||
        tiles[i].animationId > options_.maxAnimations) {
      return Status(ErrorCode::InvalidArgument);
    }
  }
  // The table's rebuild span is the COVERED (chunk-aligned superset)
  // rectangle: map the requested heights into it (the superset margin
  // stays flat ground — one setup-path temporary, the scene-load
  // context). The requested grid lies inside the covered region, so
  // the loop consumes exactly `cells` entries:
  const std::size_t coveredW = static_cast<std::size_t>(
      table_.coveredTileMaxX() - table_.coveredTileMinX());
  std::vector<std::int32_t> heights(table_.coveredCellCount(), 0);
  std::size_t i = 0;
  for (std::int32_t ty = table_.coveredTileMinY();
       ty < table_.coveredTileMaxY(); ++ty) {
    for (std::int32_t tx = table_.coveredTileMinX();
         tx < table_.coveredTileMaxX(); ++tx) {
      if (covers(tx, ty)) {
        heights[static_cast<std::size_t>(ty - table_.coveredTileMinY()) *
                    coveredW +
                static_cast<std::size_t>(tx - table_.coveredTileMinX())] =
            tiles[i].height;
        ++i;
      }
    }
  }
  // The requested grid lies inside the covered region (a rectangle in
  // a rectangle): the loop consumed exactly `cells` entries.
  assert(i == cells);
  const Status s = table_.rebuild(heights);
  if (!s.ok()) return s;  // unreachable (validated above) — honest channel
  // The per-tile data (row-major, tileX fastest — the span's order):
  for (std::size_t j = 0; j < cells; ++j) {
    tiles_[j].textureId = tiles[j].textureId;
    tiles_[j].animationId = tiles[j].animationId;
  }
  return Status{};
}

template <typename Backend>
Status TileMap<Backend>::setAnimation(std::uint32_t id,
                                      const TileAnimationDef& def) noexcept {
  // Boundary validation (first failure wins — all failures are
  // InvalidArgument, so the order is unobservable): the id domain
  // (0 is the static sentinel), the frame count / tick rate domains,
  // the sheet extents, the frame count vs the sheet's frame count,
  // and the tight sheet's float-exact domain (the M2-SPRITE-03
  // domain — the overflow guards below, the sprite_frames.h
  // adversarial-layout precedent: the layout is untrusted metadata,
  // CPP-004 / SCALE-004).
  if (id == 0 || id > options_.maxAnimations) {
    return Status(ErrorCode::InvalidArgument);
  }
  if (def.frameCount < 1 || def.frameCount > kTileAnimMaxFrames) {
    return Status(ErrorCode::InvalidArgument);
  }
  if (def.frameTicks < 1) {
    return Status(ErrorCode::InvalidArgument);
  }
  const SpriteFrameLayout& layout = def.layout;
  if (layout.frameWidth == 0 || layout.frameHeight == 0 ||
      layout.columns == 0 || layout.rows == 0) {
    return Status(ErrorCode::InvalidArgument);
  }
  // frameCount <= the sheet's frame count (columns x rows — u64: the
  // product can exceed 2^32 for adversarial layouts):
  const std::uint64_t sheetFrames =
      static_cast<std::uint64_t>(layout.columns) * layout.rows;
  if (def.frameCount > sheetFrames) {
    return Status(ErrorCode::InvalidArgument);
  }
  // The tight sheet (the M2-SPRITE-03 sheet model — derived from the
  // layout; the guards bound each term to the domain BEFORE the
  // multiplications — no u64 wrap, CPP-004):
  if (layout.sheetBorder > kSpriteFrameMaxAtlasTexels / 2) {
    return Status(ErrorCode::InvalidArgument);
  }
  if (layout.frameWidth > kSpriteFrameMaxAtlasTexels / layout.columns) {
    return Status(ErrorCode::InvalidArgument);
  }
  if (layout.frameHeight > kSpriteFrameMaxAtlasTexels / layout.rows) {
    return Status(ErrorCode::InvalidArgument);
  }
  if (layout.frameSpacing > kSpriteFrameMaxAtlasTexels / layout.columns) {
    return Status(ErrorCode::InvalidArgument);
  }
  const std::uint64_t sheetW =
      2u * static_cast<std::uint64_t>(layout.sheetBorder) +
      static_cast<std::uint64_t>(layout.columns) * layout.frameWidth +
      static_cast<std::uint64_t>(layout.columns - 1) * layout.frameSpacing;
  const std::uint64_t sheetH =
      2u * static_cast<std::uint64_t>(layout.sheetBorder) +
      static_cast<std::uint64_t>(layout.rows) * layout.frameHeight +
      static_cast<std::uint64_t>(layout.rows - 1) * layout.frameSpacing;
  if (sheetW > kSpriteFrameMaxAtlasTexels ||
      sheetH > kSpriteFrameMaxAtlasTexels) {
    return Status(ErrorCode::InvalidArgument);
  }
  // The frame UVs (the setup allocation — the only per-animation
  // allocation): spriteFrameUv is the authoritative conversion; under
  // the validated tight sheet it cannot fail (every frame rect fits
  // the tight sheet exactly — the fit check is provably inactive) —
  // the check is the honest channel for the provably dead path:
  auto frameUv = std::make_unique<SpriteUvRect[]>(def.frameCount);
  for (std::uint32_t f = 0; f < def.frameCount; ++f) {
    const auto r =
        spriteFrameUv(f, layout, static_cast<std::uint32_t>(sheetW),
                      static_cast<std::uint32_t>(sheetH));
    if (!r.ok()) return Status(ErrorCode::InvalidArgument);
    frameUv[f] = *r.valueIfOk();
  }
  // The (re)set: replace the slot, RESET the phase (frame 0, tick 0):
  AnimSlot& slot = anims_[id];
  slot.def = def;
  slot.frame = 0;
  slot.tick = 0;
  slot.frameUv = std::move(frameUv);
  slot.valid = true;
  return Status{};
}

template <typename Backend>
void TileMap<Backend>::advanceAnimations() noexcept {
  // The sim phase's per-tick advance (the header's animation
  // section): every SET animation's tick advances; its frame steps
  // every frameTicks ticks (the tick counter implements
  // frame = ticks / frameTicks mod frameCount without a per-call
  // division; the tick never reaches frameTicks without the reset,
  // so it cannot overflow). Unset slots carry no state.
  for (std::uint32_t a = 1; a <= options_.maxAnimations; ++a) {
    AnimSlot& s = anims_[a];
    if (!s.valid) continue;
    ++s.tick;
    if (s.tick == s.def.frameTicks) {
      s.tick = 0;
      s.frame = (s.frame + 1) % s.def.frameCount;
    }
  }
}

template <typename Backend>
Status TileMap<Backend>::declareTo(SpriteBatcher& batcher,
                                   const DeclareOptions& options) noexcept {
  // The frame protocol (the batcher's window): a built frame's window
  // is closed — call beginFrame first (checked here; first failure
  // wins, nothing declared).
  if (batcher.frameBuilt()) return Status(ErrorCode::InvalidArgument);
  // The tile quad (the preamble's quad model). The base item carries
  // the per-call options; the per-tile fields (position, key,
  // texture, the current frame) are set in the loop (the add takes
  // the item by value — the declared value is a copy, no aliasing of
  // the base).
  SpriteItem item;
  item.materialId = options.materialId;
  item.blend = options.blend;
  item.rotation = 0.0f;
  item.scale = Vec2{1.0f, 1.0f};  // the unit quad (the tile's cell)
  item.depthOverride = false;     // the key is the table's (auto-depth)
  std::size_t i = 0;
  for (std::int32_t ty = options_.originTileY;
       ty < options_.originTileY + options_.heightTiles; ++ty) {
    for (std::int32_t tx = options_.originTileX;
         tx < options_.originTileX + options_.widthTiles; ++tx) {
      // The tile's center (the float conversion is exact — dyadic,
      // |tx| <= 32766; the same point the table quantizes).
      item.pos = Vec2{static_cast<float>(tx) + 0.5f,
                      static_cast<float>(ty) + 0.5f};
      // The auto-depth key (the table — the game never writes it,
      // G-R11).
      item.depthKey = table_.keyAt(tx, ty);
      item.atlasId = tiles_[i].textureId;
      // The tile's current frame (the static fixed frame or the
      // animation's frame UV — the unset-slot failure, first failure
      // wins, nothing declared past it this frame):
      const Status s = applyTileFrame(item, options, i);
      if (!s.ok()) return s;
      ++i;
      const auto r = batcher.add(item);
      if (!r.ok()) return r.error();
    }
  }
  return Status{};
}

template <typename Backend>
Status TileMap<Backend>::declareTo(SpriteBatcher& batcher,
                                   const DeclareOptions& options,
                                   const ParallaxLayers<Backend>& layers,
                                   std::uint32_t layerId, Vec2 cameraPos)
    noexcept {
  // The frame protocol (the batcher's window): a built frame's window
  // is closed — call beginFrame first (checked here; first failure
  // wins, nothing declared).
  if (batcher.frameBuilt()) return Status(ErrorCode::InvalidArgument);
  // The layer pairing (the setup mispairing is the caller's — no log,
  // the Status is the channel): the layer is SET and is a Tilemap
  // source (this tilemap is its content — the parallax.h hook).
  if (!layers.has(layerId)) return Status(ErrorCode::InvalidArgument);
  const ParallaxLayerDef& def = layers.layerAt(layerId);
  if (def.source != ParallaxSource::Tilemap) {
    return Status(ErrorCode::InvalidArgument);
  }
  // A DISABLED layer declares nothing (OK, no log — the layer's
  // documented skip, the parallax declareTo's precedent):
  if (!def.enabled) return Status{};
  // The layer's world-space translation (the M2-PAR-01 formula (1) —
  // world space, RENDER-006; the camera position is the presentation
  // input, ARCH-009):
  const Vec2 o = layers.worldOffsetAt(layerId, cameraPos);
  // The content's layer (the tilemap's — the scene-setup convention:
  // the def's depthLayer equals it; one source of truth):
  const std::int32_t layer = table_.layer();
  SpriteItem item;
  item.materialId = options.materialId;
  item.blend = options.blend;
  item.rotation = 0.0f;
  item.scale = Vec2{1.0f, 1.0f};  // the unit quad (the tile's cell)
  item.depthOverride = false;     // the key is engine-owned (G-R11)
  std::size_t i = 0;
  for (std::int32_t ty = options_.originTileY;
       ty < options_.originTileY + options_.heightTiles; ++ty) {
    for (std::int32_t tx = options_.originTileX;
         tx < options_.originTileX + options_.widthTiles; ++tx) {
      // The TRANSLATED tile center (the world-space translation):
      item.pos = Vec2{static_cast<float>(tx) + 0.5f + o.x,
                      static_cast<float>(ty) + 0.5f + o.y};
      // The depth key of the translated center (the table's keys are
      // at the untranslated positions — the camera-dependent
      // translation is not precomputable; the M2-ISO-01 function at
      // the tile's stored height, the tilemap's layer — G-R11):
      item.depthKey = isoDepthKey<Backend>(
          backendVec2(item.pos.x, item.pos.y), table_.tileHeightAt(tx, ty),
          layer);
      item.atlasId = tiles_[i].textureId;
      // The tile's current frame (the static fixed frame or the
      // animation's frame UV — the unset-slot failure, first failure
      // wins, nothing declared past it this frame):
      const Status s = applyTileFrame(item, options, i);
      if (!s.ok()) return s;
      ++i;
      const auto r = batcher.add(item);
      if (!r.ok()) return r.error();
    }
  }
  return Status{};
}

template <typename Backend>
TileData TileMap<Backend>::tileAt(std::int32_t tileX,
                                  std::int32_t tileY) const noexcept {
  // Precondition: covers() (the check-free read — the table's keyAt
  // pattern; the misuse warnings above).
  const std::size_t i = requestedIndex(tileX, tileY);
  return TileData{tiles_[i].textureId,
                  table_.tileHeightAt(tileX, tileY),
                  tiles_[i].animationId};
}

template <typename Backend>
std::uint32_t TileMap<Backend>::depthKeyAt(std::int32_t tileX,
                                           std::int32_t tileY) const noexcept {
  // Precondition: covers() (the requested grid lies in the table's
  // covered region — the table's keyAt contract).
  return table_.keyAt(tileX, tileY);
}

template <typename Backend>
std::int32_t TileMap<Backend>::tileHeightAt(std::int32_t tileX,
                                            std::int32_t tileY) const noexcept {
  // Precondition: covers() (the table's tileHeightAt contract).
  return table_.tileHeightAt(tileX, tileY);
}

template <typename Backend>
bool TileMap<Backend>::covers(std::int32_t tileX,
                              std::int32_t tileY) const noexcept {
  return tileX >= options_.originTileX &&
         tileX < options_.originTileX + options_.widthTiles &&
         tileY >= options_.originTileY &&
         tileY < options_.originTileY + options_.heightTiles;
}

}  // namespace laige::render
