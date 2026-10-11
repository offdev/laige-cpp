// laige-render isometric reference scene (M2-SCENE-01): the
// worst-case isometric reference scene of PRD §4 — the deterministic,
// seed-driven scene every visual and performance acceptance test
// renders (the 50k-sprite §8.1 budget scene, the M2-PERF-01 workload,
// the M2-AC-01/M2-GOLD-01/M2-SAMPLE-01 fixture).
//
// PRD §4: "It is the reference scene for all visual and performance
// acceptance tests (worst-case overlap for depth sorting)."
// Roadmap M2-SCENE-01: a headless, seed-deterministic builder that
// emits the worst-case isometric scene — 50k overlapping sprites
// (random positions on tiled ground with stepped heights), 3 parallax
// layers, particle emitters, a UI panel, and the tile-height terrain —
// as data the engine can load (in-memory builder + simple binary
// serialization used by the tests; the full scene FORMAT is M3, the
// versioned binary format below is the M2 stand-in). ARCH-010: the
// scene is a test fixture — same seed, same bytes, every build and
// platform (presentation state only, ARCH-009 — never sim state,
// never in the replay hash).
//
//   IsoRefSceneVariant          Small (1k sprites) | Full (50k sprites)
//   IsoRefSceneVec2             A pair of floats (the scene file's
//                               point values — the backend conversion
//                               is the RENDER-006 load boundary)
//   IsoRefSceneSprite           One scene sprite (position, standing
//                               step height, atlas, blend, frame)
//   IsoRefSceneParallaxLayer    One scene parallax layer (image source)
//   IsoRefSceneEmitter          One scene particle emitter definition
//   IsoRefSceneUiWidget         One scene UI widget spec
//   IsoReferenceScene           The whole scene (terrain grid + sprites
//                               + layers + emitters + UI), plain values
//   buildIsoReferenceScene      The deterministic in-memory builder
//   serializeIsoReferenceScene  The scene -> versioned binary bytes
//   readIsoReferenceScene       The bytes -> scene (validated)
//   writeIsoReferenceScene      The scene -> file (atomic temp + rename)
//   IsoReferenceSceneWorld      The engine-side world built from a scene
//                               (TileMap + ParallaxLayers +
//                               ParticleSystem + UiTree + StringTable)
//   loadIsoReferenceScene       The scene -> world (setup path)
//   declareIsoReferenceScene    The world's per-frame declare (one call
//                               = the whole world pass into a batcher)
//
// ---------------------------------------------------------------------------
// The scene contents (the worst-case composition)
// ---------------------------------------------------------------------------
//
// The scene is built from a single seed (any u64 — 0 is a valid seed)
// through three deterministic substreams (the M0-CORE-06 Prng
// substream pattern; the ids are stable subsystem identifiers):
//
//   terrain  (substream 1)  the stepped-terrain plateau heights
//   sprites  (substream 2)  the sprite positions, atlases, blends, frames
//   emitters (substream 3)  the emitter life/size variation
//
// The FULL variant (the §8.1 budget scene) is:
//
//   - a 256 x 256 tile grid (65 536 tiles) at the world origin,
//     partitioned into 16 x 16-tile PLATEAUS (16 x 16 = 256 plateaus):
//     each plateau has a uniform integer height — `step` x 2 world
//     units for a PRNG-drawn step in [0, 24) — so the terrain is a
//     terraced, stepped surface (the "stepped heights" of the step
//     scope) with 24 distinct height values in [0, 46] (well inside
//     the M2-ISO-01 |z| <= 2047 domain);
//   - 50 000 sprites at uniform-random ground positions: each sprite's
//     position is `tile + fraction`, the fraction a uniform dyadic in
//     [0, 1) at 2^-16 resolution (exact in both SimMath backends —
//     ADR 0002), so the sprite stands on the PRNG's chosen tile and
//     its depth key is `isoDepthKey(pos, the tile's height, layer 0)`
//     (the scene file stores the standing tile's height with the
//     sprite — one source of truth, the tile's own);
//   - 3 IMAGE parallax layers (background factor 0, mid factor 0.5,
//     foreground factor 1.0 — the M2-PAR-01 preset factors), 128 x 128
//     world units each, centered on the scene's ground;
//   - 8 particle emitters (continuous rate 2/tick, 40..120 tick lives,
//     additive orange sparks rising off the ground);
//   - a 2-widget UI (one opaque panel, one button with the "OK"
//     label) on the 1280 x 720 reference screen (the M2-GOLD-01
//     resolution).
//
// The SMALL variant is the same composition scaled down (a 32 x 32
// grid of 8 x 8-tile plateaus, 1 000 sprites) — the fast unit-test
// fixture (the step scope's "small 1k-sprite variant").
//
// Worst-case properties (why this is the reference scene):
//
//   - 50 000 sprites over 65 536 tiles means several sprites per
//     depth row: the (x + y) - z value quantizes to at most ~8 300
//     distinct 32-bit keys, so the M2-SORT-01 radix sort sees a dense
//     multi-sprite tie band on every row (the worst-case overlap for
//     depth sorting, PRD §4);
//   - the 50k sprites + 65k tile quads + parallax quads + live
//     particles fill a ~116k-item frame — the batcher's frame budget
//     (M2-SPRITE-01) is exercised at its reference size;
//   - the scene's atlas-id plan (below) keeps the §8.1 "50k visible
//     sprites, 3 parallax layers, UI" workload to a fixed 14
//     (atlas, material, blend) groups = 14 draw calls in the world
//     pass — comfortably under the 30-draw-call budget, with the group
//     count pinned (the M2-PERF-01 gate asserts it).
//
// ---------------------------------------------------------------------------
// The atlas-id plan (the cross-group render order, parallax.h)
// ---------------------------------------------------------------------------
//
// The batcher's cross-group draw order is ascending
// (atlasId, materialId, blend) (M2-SPRITE-01), so the scene assigns
// atlas ids that make group order = painter's order (the parallax.h
// scene-setup convention — background layers first, ground next,
// world objects, particles, foreground last):
//
//   0  background parallax layer        (depth layer -2)
//   1  mid parallax layer               (depth layer -1)
//   2  ground tilemap                   (layer 0, all tiles)
//   3..6  the 4 sprite atlases          (layer 0, Alpha/Additive)
//   7  the particle atlas              (layer 0, Additive)
//   8  foreground parallax layer       (depth layer +1)
//
//   = 2 + 1 + 4 x 2 blends + 1 + 1 = 14 groups (kIsoRefSceneSprite
//   PassGroups), 14 instanced draw calls at the submit stage — the
//   scene's documented draw-call budget property (PRD §8.1: <= 30).
//
// The UI pass is a separate batcher (M2-UI-02) with its own ids:
// widget art atlas 100, font atlas 101 (unused when the pass declares
// without a default font — the label-less world pass).
//
// ---------------------------------------------------------------------------
// The deterministic builder (ARCH-010 scope: presentation fixture)
// ---------------------------------------------------------------------------
//
// `buildIsoReferenceScene(seed, variant)` is a PURE function of
// (seed, variant): unsigned integer PRNG arithmetic (the M0-CORE-06
// xorshift128+ contract) plus the exact 2^-16 position scale (one
// multiply by a power of two — no FMA, no libm), so the built scene
// and its serialization are bit-identical across builds, compilers,
// and P0 platforms for the same seed (the same scope as the Prng —
// ARCH-010; the scene is presentation state, never sim state, so the
// sim determinism scope is untouched). The builder is O(scene size)
// time and O(scene size) memory (the two vectors), setup path only.
//
// ---------------------------------------------------------------------------
// The binary format (version 1; SCALE-005 / ARCH-007)
// ---------------------------------------------------------------------------
//
// The scene file is a flat little-endian binary, written field by
// field (the layout is explicit — struct padding never leaks in):
//
//   offset  size  field
//   ------  ----  ----------------------------------------------------
//   0       4     version (u32; MUST be 1 for this reader)
//   4       1     variant (u8: 0 = small, 1 = full)
//   5       3     reserved (zero)
//   8       8     seed (u64)
//   16      4     originTileX (i32)
//   20      4     originTileY (i32)
//   24      4     widthTiles (i32)
//   28      4     heightTiles (i32)
//   32      4     plateauTiles (i32)
//   36      4     stepHeight (i32)
//   40      4     maxSteps (i32)
//   44      4     tileCount (u32)
//   48      4     spriteCount (u32)
//   52      4     emitterCount (u32)
//   56      4     uiWidgetCount (u32)
//   60      4     parallaxCount (u32)
//   64      4     reserved (zero)
//   68      4*nt  tileHeights (i32 per tile, row-major, tileX fastest)
//   ...     14*ns sprites: x (f32), y (f32), stepHeight (i16), atlasId
//           (u8), blend (u8), frame (u8), reserved (u8)
//   ...     67*np  parallax layers: factor (f32), center (2 f32),
//           offset (2 f32), size (2 f32), uv (4 f32), atlasId (u32),
//           materialId (u32), blend (u8), scrollMode (u8), enabled
//           (u8), pad (u8), depthLayer (i32), scrollSpeed (2 f32)
//   ...     56*ne  emitters: originX/Y (2 f32), depth (i32), velMin/
//           velMax (4 f32), lifeMin/lifeMax (2 u32), sizeMin/sizeMax
//           (2 f32), tint (4 u8), fadeTicks (u32), continuousRate
//           (u32)
//   ...     23*nui UI widgets: kind (u8), opaque (u8), x/y/w/h (4 i32),
//           atlasId (u32), label (u8)
//
// All floats are IEEE-754 binary32, little-endian. Multi-byte
// integers are little-endian. There is no hash/checksum: the reader's
// domain validation (below) is the integrity contract, and the
// determinism check (two same-seed serializations compare byte-
// identical) is the generator's. Versioning (ARCH-007): a reader
// rejects any version it does not support with `MalformedInput` —
// it never guesses; future format versions bump `version` and get a
// new reader.
//
// The reader treats the bytes as UNTRUSTED input (SCALE-004): every
// field is bounds- and domain-checked before use, the exact total
// length is required (trailing bytes are rejected), and any
// violation fails the whole read with `MalformedInput` (nothing is
// partially applied — the result is a value, nothing mutates).
//
// ---------------------------------------------------------------------------
// Ownership, threading, lifetimes (CORE-009)
// ---------------------------------------------------------------------------
//
// The scene values (`IsoReferenceScene` and its members) are plain
// data — copyable, no ownership. `IsoReferenceSceneWorld<Backend>` is
// move-only (the SpriteBatcher precedent) and owns its five engine
// objects plus a copy of the scene that built it; one owner (the
// scene setup / the test), the render phase reads it through
// `declareIsoReferenceScene` (a non-mutating pass except the
// parallax scroll offsets — the M2-PAR-01 presentation state,
// advanced once per frame by the declare, the documented frame
// pacing). Everything is single-threaded per the engine's frame
// protocol (CONC-001): setup on the scene thread, declare on the
// render thread's batch stage — the same one-owner/one-phase story as
// every other M2 render object.
//
// ---------------------------------------------------------------------------
// Failure (CORE-008, API-008 — first failure wins, no log on domain
// failures; the house convention: domain validation is silent, the
// caller maps the code)
// ---------------------------------------------------------------------------
//
//   buildIsoReferenceScene    total function (no failure — a plain
//                             value; the domains are the builder's
//                             own constants, in domain by
//                             construction)
//   serializeIsoReferenceScene total function (one allocation — the
//                             output vector, setup path)
//   readIsoReferenceScene     version mismatch, short/long input, or
//                             any field outside its domain ->
//                             MalformedInput (the whole read fails)
//   writeIsoReferenceScene    open/write/rename failure -> IoError
//   loadIsoReferenceScene     a failed engine create/rebuild/add
//                             -> that object's error (InvalidArgument
//                             / BudgetExhausted); the world is left
//                             stopped on failure (nothing owned)
//   declareIsoReferenceScene  a failed declare sub-step -> that step's
//                             error (the items already declared by
//                             this call remain in the frame — the
//                             batcher owns the frame state, the
//                             TileMap::declareTo precedent); a
//                             stopped world -> InvalidArgument
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003 — the budgets this step is PART of)
// ---------------------------------------------------------------------------
//
//   build          O(scene size) time + O(scene size) memory (two
//                  vectors), setup only; the full scene serializes to
//                  ~0.94 MB. The step's < 5 s generation budget is a
//                  tool property (docs/api/iso_reference_scene.md),
//                  not a §8.1 budgets.json entry.
//   load           O(scene size), setup only (the tilemap rebuild,
//                  the eight addEmitters, the two addChildren, one
//                  intern — all documented setup paths).
//   declare        O(frame items) per frame, NO ALLOCATION (the
//                  batcher/pool storage is pre-sized; the per-sprite
//                  isoDepthKey is a handful of integer ops; the UV
//                  rects are precomputed at load — 4 of them, not
//                  per sprite). The declare is one component of the
//                  §8.1 "50k sprites ... <= 2 ms CPU" composite
//                  budget — M2-PERF-01 measures the whole frame
//                  (tile quads included), not this step alone.
//
// Misuse warnings:
//
//   - The scene file is a TEST FIXTURE, not an asset: it carries no
//     image data (the atlases are the scene's ids, the game/tests
//     bind their textures), and it is NOT versioned scene SAVE data
//     (ARCH-007 applies to the file's own format only — the M3 scene
//     format supersedes it).
//   - The world's parallax auto layers ADVANCE on every
//     `declareIsoReferenceScene` call: declare once per frame, in
//     the render phase, never per sprite or in a tight retry loop
//     (the scroll offsets are presentation state — a double advance
//     is a wrong scroll, not an error the engine can detect).
//   - Size the world pass's batcher to the scene's frame: the full
//     scene declares ~116k items per frame (see `worldFrameCount`);
//     an undersized batcher drops the oldest items (the batcher's
//     overflow policy — silent degradation, LOG-004), the frame is
//     still built.

#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/prng.h"
#include "laige/render/matrices.h"
#include "laige/render/parallax.h"
#include "laige/render/particle_render.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/sprite_frames.h"
#include "laige/render/tilemap.h"
#include "laige/render/ui_widgets.h"
#include "laige/result.h"
#include "laige/sim/particles.h"
#include "laige/sim_math.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The scene file's format version (ARCH-007 — readers reject other
// versions; v1 is this header's layout).
inline constexpr std::uint32_t kIsoRefSceneFormatVersion = 1u;

// The deterministic builder's PRNG substream ids (stable subsystem
// identifiers, the M0-CORE-06 pattern): terrain, sprites, emitters.
inline constexpr std::uint32_t kIsoRefSceneSubstreamTerrain = 1u;
inline constexpr std::uint32_t kIsoRefSceneSubstreamSprites = 2u;
inline constexpr std::uint32_t kIsoRefSceneSubstreamEmitters = 3u;

// The two scene sizes (the step scope: the 50k worst-case scene and
// the small 1k-sprite variant for fast unit tests).
inline constexpr std::uint32_t kIsoRefSceneFullSprites = 50000u;
inline constexpr std::uint32_t kIsoRefSceneSmallSprites = 1000u;

// The terrain grids (tiles per axis): the full 256 x 256 and the
// small 32 x 32 (both powers of two — the plateau partition divides
// evenly).
inline constexpr std::int32_t kIsoRefSceneFullGridTiles = 256;
inline constexpr std::int32_t kIsoRefSceneSmallGridTiles = 32;

// The plateau partition (tiles per plateau per axis): 16 x 16 plateaus
// over the full grid, 8 x 8 over the small one.
inline constexpr std::int32_t kIsoRefSceneFullPlateauTiles = 16;
inline constexpr std::int32_t kIsoRefSceneSmallPlateauTiles = 8;

// The stepped-terrain height model: each plateau sits at a PRNG-drawn
// step in [0, kIsoRefSceneTerrainSteps), at `kIsoRefSceneStepHeight`
// world units per step -> heights in [0, (steps-1) x step] = [0, 46]
// (inside the M2-ISO-01 |z| <= 2047 domain with wide margin).
inline constexpr std::int32_t kIsoRefSceneTerrainSteps = 24;
inline constexpr std::int32_t kIsoRefSceneStepHeight = 2;

// The scene's fixed content counts (the full and small variants share
// them — the size differs only in the grid and the sprite count).
inline constexpr std::uint32_t kIsoRefSceneParallaxLayerCount = 3u;
inline constexpr std::uint32_t kIsoRefSceneEmitterCount = 8u;
inline constexpr std::uint32_t kIsoRefSceneUiWidgetCount = 2u;

// The UI reference screen (the M2-GOLD-01 reference resolution — the
// world's UiTree screen rect).
inline constexpr std::int32_t kIsoRefSceneUiScreenWidth = 1280;
inline constexpr std::int32_t kIsoRefSceneUiScreenHeight = 720;
// The world's UI budgets (scene-scale: the scene declares 2 widgets;
// the tree pool and event list are sized small on purpose).
inline constexpr std::uint32_t kIsoRefSceneUiMaxWidgets = 16u;
inline constexpr std::uint32_t kIsoRefSceneUiMaxEvents = 64u;

// The sprite sheet: a 2 x 2 grid of 16 x 16 texel frames (4 frames),
// packed (no spacing, no border) -> a tight 32 x 32 sheet. The frame
// UVs are precomputed at load (4 rects), never per sprite.
inline constexpr std::uint32_t kIsoRefSceneSpriteSheetFrames = 4u;
inline constexpr std::uint32_t kIsoRefSceneSpriteSheetFrameTexels = 16u;

// The atlas-id plan (the header's plan section — the cross-group
// painter's order, parallax.h):
//   0 bg layer | 1 mid layer | 2 ground | 3..6 sprites | 7 particles
//   | 8 fg layer; UI pass: 100 widget art, 101 font.
inline constexpr std::uint32_t kIsoRefSceneBgAtlas = 0u;
inline constexpr std::uint32_t kIsoRefSceneMidAtlas = 1u;
inline constexpr std::uint32_t kIsoRefSceneGroundAtlas = 2u;
inline constexpr std::uint32_t kIsoRefSceneSpriteAtlasBase = 3u;
inline constexpr std::uint32_t kIsoRefSceneSpriteAtlasCount = 4u;
inline constexpr std::uint32_t kIsoRefSceneParticleAtlas = 7u;
inline constexpr std::uint32_t kIsoRefSceneFgAtlas = 8u;
inline constexpr std::uint32_t kIsoRefSceneUiWidgetAtlas = 100u;
inline constexpr std::uint32_t kIsoRefSceneUiFontAtlas = 101u;

// The scene's world-pass group count: 2 (bg/mid) + 1 (ground) +
// 4 x 2 (sprite atlases x Alpha/Additive) + 1 (particles) + 1 (fg) =
// 14 (atlas, material, blend) groups = 14 instanced draw calls —
// the pinned draw-call property under the §8.1 30-draw-call budget.
inline constexpr std::uint32_t kIsoRefSceneSpritePassGroups = 14u;

// The UI label table (the format's fixed strings — no strings in the
// file, only these ids): 0 = no label, 1 = "OK" (the button).
inline constexpr std::uint8_t kIsoRefSceneLabelNone = 0u;
inline constexpr std::uint8_t kIsoRefSceneLabelOk = 1u;

// The scene variant (the file's u8 field; the builder's parameter).
enum class IsoRefSceneVariant : std::uint8_t {
  Small = 0,  // the 1k-sprite fast unit-test fixture
  Full = 1,   // the 50k-sprite §8.1 worst-case scene
};

// The scene file's point values (a pair of floats — the file's
// float32 fields; the backend conversion at load is the RENDER-006
// boundary).
struct IsoRefSceneVec2 {
  float x{};
  float y{};
};

// One scene sprite (the 14-byte wire record): a ground-plane position
// (exact k x 2^-16 world units, in [0, grid) x [0, grid)), the standing
// tile's step height (the depth-key `z`), the sprite's atlas (the plan
// above), its blend state (0 = Alpha, 1 = Additive — the BatchMode
// wire values), and its sheet frame (0..3).
struct IsoRefSceneSprite {
  float x{};
  float y{};
  std::int16_t stepHeight{};
  std::uint8_t atlasId{};
  std::uint8_t blend{};
  std::uint8_t frame{};
  std::uint8_t reserved{};
};

// One scene parallax layer (the 67-byte wire record; image source only
// — the reference scene's layers are all images): the M2-PAR-01
// ParallaxLayerDef's fields in file order.
struct IsoRefSceneParallaxLayer {
  float factor{};
  IsoRefSceneVec2 center{};
  IsoRefSceneVec2 offset{};
  IsoRefSceneVec2 size{};
  SpriteUvRect uv{};
  std::uint32_t atlasId{};
  std::uint32_t materialId{};
  std::uint8_t blend{};
  std::uint8_t scrollMode{};  // 0 = Manual, 1 = Auto
  std::uint8_t enabled{};
  std::uint8_t reserved{};
  std::int32_t depthLayer{};
  IsoRefSceneVec2 scrollSpeed{};
};

// One scene particle emitter definition (the 56-byte wire record): the
// M2-PART-01 ParticleEmitterDef's fields in file order (depth is the
// M2-ISO-01 step-height domain, i32 in the file).
struct IsoRefSceneEmitter {
  float originX{};
  float originY{};
  std::int32_t depth{};
  float velMinX{};
  float velMinY{};
  float velMaxX{};
  float velMaxY{};
  std::uint32_t lifeMin{};
  std::uint32_t lifeMax{};
  float sizeMin{};
  float sizeMax{};
  std::uint8_t tint[4]{};
  std::uint32_t fadeTicks{};
  std::uint32_t continuousRate{};
};

// One scene UI widget spec (the 23-byte wire record): the M2-UI-01
// UiWidgetSpec's fields in file order (the label is the fixed label
// table id, not a string).
struct IsoRefSceneUiWidget {
  std::uint8_t kind{};  // the UiWidgetKind value
  std::uint8_t opaque{};
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t width{};
  std::int32_t height{};
  std::uint32_t atlasId{};
  std::uint8_t label{};
};

// The whole reference scene (plain values; the header's model section
// is the content contract). Copyable (the vectors carry the storage).
struct IsoReferenceScene {
  // The file's format version (the reader's gate — always
  // kIsoRefSceneFormatVersion for builder output).
  std::uint32_t version{kIsoRefSceneFormatVersion};
  // The variant the scene was built as (Small/Full — the shape
  // constants below are consistent with it by construction).
  IsoRefSceneVariant variant{IsoRefSceneVariant::Full};
  // The seed that built the scene (the determinism record, ARCH-010).
  std::uint64_t seed{};
  // The terrain grid (row-major heights, tileX fastest — the tilemap's
  // flat order).
  std::int32_t originTileX{};
  std::int32_t originTileY{};
  std::int32_t widthTiles{};
  std::int32_t heightTiles{};
  std::int32_t plateauTiles{};
  std::int32_t stepHeight{};
  std::int32_t maxSteps{};
  std::uint32_t tileCount{};
  // The content counts (the array sizes below hold the max; the
  // counts say how many are live).
  std::uint32_t spriteCount{};
  std::uint32_t emitterCount{};
  std::uint32_t uiWidgetCount{};
  std::uint32_t parallaxCount{};
  // The storage (fixed max sizes; the counts bound the live range).
  std::vector<std::int32_t> tileHeights;
  std::vector<IsoRefSceneSprite> sprites;
  IsoRefSceneParallaxLayer parallax[kIsoRefSceneParallaxLayerCount]{};
  IsoRefSceneEmitter emitters[kIsoRefSceneEmitterCount]{};
  IsoRefSceneUiWidget uiWidgets[kIsoRefSceneUiWidgetCount]{};
};

// The deterministic in-memory builder (the header's model section):
// the scene is a pure function of (seed, variant) — same seed, same
// scene, every build and platform (ARCH-010, the Prng scope).
// O(scene size) time + O(scene size) memory (two vectors), setup path.
// @budget O(scene size); two allocations.
[[nodiscard]] inline IsoReferenceScene
buildIsoReferenceScene(std::uint64_t seed, IsoRefSceneVariant variant) noexcept {
  // The variant's shape (the header's model section — the only
  // variant-dependent values).
  const std::int32_t gridTiles =
      (variant == IsoRefSceneVariant::Full) ? kIsoRefSceneFullGridTiles
                                             : kIsoRefSceneSmallGridTiles;
  const std::int32_t plateauTiles =
      (variant == IsoRefSceneVariant::Full) ? kIsoRefSceneFullPlateauTiles
                                             : kIsoRefSceneSmallPlateauTiles;
  const std::uint32_t spriteCount =
      (variant == IsoRefSceneVariant::Full) ? kIsoRefSceneFullSprites
                                             : kIsoRefSceneSmallSprites;

  IsoReferenceScene scene;
  scene.variant = variant;
  scene.seed = seed;
  scene.originTileX = 0;
  scene.originTileY = 0;
  scene.widthTiles = gridTiles;
  scene.heightTiles = gridTiles;
  scene.plateauTiles = plateauTiles;
  scene.stepHeight = kIsoRefSceneStepHeight;
  scene.maxSteps = kIsoRefSceneTerrainSteps;
  scene.tileCount = static_cast<std::uint32_t>(gridTiles) *
                    static_cast<std::uint32_t>(gridTiles);
  scene.spriteCount = spriteCount;
  scene.emitterCount = kIsoRefSceneEmitterCount;
  scene.uiWidgetCount = kIsoRefSceneUiWidgetCount;
  scene.parallaxCount = kIsoRefSceneParallaxLayerCount;
  scene.tileHeights.resize(scene.tileCount);
  scene.sprites.resize(spriteCount);

  // -----------------------------------------------------------------
  // The terrain (substream 1): one PRNG step per plateau, row-major;
  // the plateau's height spreads over its tiles.
  // -----------------------------------------------------------------
  laige::Prng terrain =
      laige::Prng::deriveSubstream(seed, kIsoRefSceneSubstreamTerrain);
  const std::uint32_t plateausPerAxis =
      static_cast<std::uint32_t>(gridTiles / plateauTiles);
  for (std::uint32_t py = 0; py < plateausPerAxis; ++py) {
    for (std::uint32_t px = 0; px < plateausPerAxis; ++px) {
      const std::int32_t h = static_cast<std::int32_t>(
                                 terrain.next_range(
                                     0u, static_cast<std::uint32_t>(kIsoRefSceneTerrainSteps))) *
                             kIsoRefSceneStepHeight;
      for (std::uint32_t ty = py * plateauTiles;
           ty < (py + 1u) * plateauTiles; ++ty) {
        for (std::uint32_t tx = px * plateauTiles;
             tx < (px + 1u) * plateauTiles; ++tx) {
          scene.tileHeights[ty * static_cast<std::uint32_t>(gridTiles) + tx] =
              h;
        }
      }
    }
  }

  // -----------------------------------------------------------------
  // The sprites (substream 2): uniform random ground positions at
  // 2^-16 resolution (exact in both backends), one draw each for
  // x, y, atlas, blend, frame (the 5-draw per-sprite sequence — the
  // determinism contract).
  // -----------------------------------------------------------------
  laige::Prng sprites =
      laige::Prng::deriveSubstream(seed, kIsoRefSceneSubstreamSprites);
  // The position's raw domain: [0, gridTiles x 2^16) in 2^-16 units —
  // the raw value's low 16 bits are the within-tile fraction, the
  // rest the tile index (floor(x) == raw >> 16 exactly).
  const std::uint32_t posDomain = static_cast<std::uint32_t>(gridTiles) <<
                                  16;
  constexpr float kPosUnit = 1.0f / 65536.0f;  // 2^-16, exact
  for (std::uint32_t i = 0; i < spriteCount; ++i) {
    const std::uint32_t rawX = sprites.next_range(0u, posDomain);
    const std::uint32_t rawY = sprites.next_range(0u, posDomain);
    const std::uint32_t tileX = rawX >> 16;
    const std::uint32_t tileY = rawY >> 16;
    IsoRefSceneSprite& s = scene.sprites[i];
    // The position is exact: raw < 2^24 is float-exact, and the
    // multiply by the power of two kPosUnit is exact (no FMA).
    s.x = static_cast<float>(rawX) * kPosUnit;
    s.y = static_cast<float>(rawY) * kPosUnit;
    // The sprite stands on its tile (the terrain's source of truth).
    s.stepHeight = static_cast<std::int16_t>(
        scene.tileHeights[tileY * static_cast<std::uint32_t>(gridTiles) +
                          tileX]);
    s.atlasId = static_cast<std::uint8_t>(kIsoRefSceneSpriteAtlasBase +
                                          sprites.next_range(
                                              0u, kIsoRefSceneSpriteAtlasCount));
    s.blend = static_cast<std::uint8_t>(sprites.next_range(0u, 2u));
    s.frame = static_cast<std::uint8_t>(sprites.next_range(
        0u, kIsoRefSceneSpriteSheetFrames));
  }

  // -----------------------------------------------------------------
  // The parallax layers (fixed — variant-independent): the M2-PAR-01
  // preset factors, 128 x 128 world-unit layers centered on the
  // ground (offset -64), the header's plan section's atlas ids.
  // -----------------------------------------------------------------
  constexpr float kHalfLayer = 64.0f;   // the layer half-extent, world units
  constexpr float kScrollUnit = 1.0f / 2048.0f;  // 2^-11 UV units/frame
  scene.parallax[0] = IsoRefSceneParallaxLayer{
      0.0f,          // factor: fixed in world space (max parallax)
      IsoRefSceneVec2{0.0f, 0.0f},  // center
      IsoRefSceneVec2{-kHalfLayer, -kHalfLayer},  // offset
      IsoRefSceneVec2{128.0f, 128.0f},  // size
      SpriteUvRect{0.0f, 0.0f, 1.0f, 1.0f},  // uv
      kIsoRefSceneBgAtlas,  // atlasId
      0u,                   // materialId
      0u,                   // blend (Alpha)
      1u,                   // scrollMode (Auto)
      1u,                   // enabled
      0u,                   // reserved
      -2,                   // depthLayer (background)
      IsoRefSceneVec2{kScrollUnit, -kScrollUnit}};  // scrollSpeed
  scene.parallax[1] = IsoRefSceneParallaxLayer{
      0.5f,  // factor: half-speed (the midground)
      IsoRefSceneVec2{0.0f, 0.0f},  // center
      IsoRefSceneVec2{-kHalfLayer, -kHalfLayer},  // offset
      IsoRefSceneVec2{128.0f, 128.0f},  // size
      SpriteUvRect{0.0f, 0.0f, 1.0f, 1.0f},  // uv
      kIsoRefSceneMidAtlas,  // atlasId
      0u,                    // materialId
      0u,                    // blend (Alpha)
      0u,                    // scrollMode (Manual)
      1u,                    // enabled
      0u,                    // reserved
      -1,                    // depthLayer (midground)
      IsoRefSceneVec2{0.0f, 0.0f}};  // scrollSpeed
  scene.parallax[2] = IsoRefSceneParallaxLayer{
      1.0f,  // factor: 1:1 with the camera (screen-fixed)
      IsoRefSceneVec2{0.0f, 0.0f},  // center
      IsoRefSceneVec2{-kHalfLayer, -kHalfLayer},  // offset
      IsoRefSceneVec2{128.0f, 128.0f},  // size
      SpriteUvRect{0.0f, 0.0f, 1.0f, 1.0f},  // uv
      kIsoRefSceneFgAtlas,  // atlasId
      0u,                   // materialId
      0u,                   // blend (Alpha)
      1u,                   // scrollMode (Auto)
      1u,                   // enabled
      0u,                   // reserved
      1,                    // depthLayer (foreground)
      IsoRefSceneVec2{-kScrollUnit, kScrollUnit}};  // scrollSpeed

  // -----------------------------------------------------------------
  // The particle emitters (substream 3 for the life/size variation):
  // 8 additive orange-spark emitters at random ground tile centers,
  // rate 2/tick, 40..120 tick lives, 0.1..0.49 world-unit sizes.
  // -----------------------------------------------------------------
  laige::Prng emitters =
      laige::Prng::deriveSubstream(seed, kIsoRefSceneSubstreamEmitters);
  for (std::uint32_t i = 0; i < scene.emitterCount; ++i) {
    IsoRefSceneEmitter& e = scene.emitters[i];
    // The origin: a uniform tile center (integer + 0.5, exact float).
    e.originX = static_cast<float>(emitters.next_range(
        0u, static_cast<std::uint32_t>(gridTiles))) + 0.5f;
    e.originY = static_cast<float>(emitters.next_range(
        0u, static_cast<std::uint32_t>(gridTiles))) + 0.5f;
    e.depth = 0;
    // The velocity box: a slow upward drift (world units per tick).
    e.velMinX = -0.03f;
    e.velMinY = -0.08f;
    e.velMaxX = 0.03f;
    e.velMaxY = 0.08f;
    e.lifeMin = 40u;
    e.lifeMax = 60u + emitters.next_range(0u, 61u);  // [60, 120]
    e.sizeMin = 0.1f;
    // The size max, dyadic-exact (no float composition — the
    // cross-platform determinism contract, ADR 0002: k x 2^-8 + 2^-2
    // rounds nowhere, FMA-independent): [0.25, 0.25 + 19 x 2^-8).
    e.sizeMax = 0.25f +
        static_cast<float>(emitters.next_range(0u, 20u)) *
            (1.0f / 256.0f);
    e.tint[0] = 255u;
    e.tint[1] = 170u;
    e.tint[2] = 80u;
    e.tint[3] = 255u;
    e.fadeTicks = 20u;
    e.continuousRate = 2u;
  }

  // -----------------------------------------------------------------
  // The UI (fixed — variant-independent): one opaque panel + one
  // "OK" button, top-left of the 1280 x 720 reference screen.
  // -----------------------------------------------------------------
  scene.uiWidgets[0] = IsoRefSceneUiWidget{
      0u,  // kind (Panel)
      1u,  // opaque
      16, 16, 480, 240,  // x, y, width, height
      kIsoRefSceneUiWidgetAtlas,  // atlasId
      kIsoRefSceneLabelNone};  // label
  scene.uiWidgets[1] = IsoRefSceneUiWidget{
      1u,  // kind (Button)
      0u,  // opaque
      32, 32, 160, 48,  // x, y, width, height
      kIsoRefSceneUiWidgetAtlas,  // atlasId
      kIsoRefSceneLabelOk};  // label
  return scene;
}

namespace detail {

// The byte-wise little-endian writer helpers (the format section —
// explicit fields, struct padding never leaks into the wire).
inline void pushU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
  out.push_back(v);
}
inline void pushU32le(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}
inline void pushU64le(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}
inline void pushI32le(std::vector<std::uint8_t>& out, std::int32_t v) {
  pushU32le(out, static_cast<std::uint32_t>(v));
}
inline void pushI16le(std::vector<std::uint8_t>& out, std::int16_t v) {
  const std::uint32_t u = static_cast<std::uint32_t>(v);
  out.push_back(static_cast<std::uint8_t>(u & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((u >> 8) & 0xFFu));
}
inline void pushF32(std::vector<std::uint8_t>& out, float v) {
  std::uint32_t bits;
  static_assert(sizeof(bits) == sizeof(v));
  std::memcpy(&bits, &v, sizeof(bits));
  pushU32le(out, bits);
}

// The byte-wise little-endian reader cursor (the format section —
// every read bounds-checks; a short input fails the whole read).
struct ByteCursor {
  const std::uint8_t* p{};
  std::size_t n{};
  bool ok{true};

  bool takeU8(std::uint8_t& v) {
    if (!ok || n == 0) {
      ok = false;
      return false;
    }
    v = *p++;
    --n;
    return true;
  }
  bool takeU32(std::uint32_t& v) {
    std::uint8_t b[4]{};
    for (int i = 0; i < 4; ++i) {
      if (!takeU8(b[i])) return false;
    }
    // Explicit little-endian assembly (the format section — the
    // byte order is the wire contract, not the host's).
    v = static_cast<std::uint32_t>(b[0]) |
        (static_cast<std::uint32_t>(b[1]) << 8) |
        (static_cast<std::uint32_t>(b[2]) << 16) |
        (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
  }
  bool takeU64(std::uint64_t& v) {
    std::uint8_t b[8]{};
    for (int i = 0; i < 8; ++i) {
      if (!takeU8(b[i])) return false;
    }
    v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(b[i]) << (8 * i);
    }
    return true;
  }
  bool takeI32(std::int32_t& v) {
    std::uint32_t u;
    if (!takeU32(u)) return false;
    v = static_cast<std::int32_t>(u);
    return true;
  }
  bool takeI16(std::int16_t& v) {
    std::uint32_t u = 0;
    std::uint8_t b0{}, b1{};
    if (!takeU8(b0) || !takeU8(b1)) return false;
    u = static_cast<std::uint32_t>(b0) | (static_cast<std::uint32_t>(b1) << 8);
    v = static_cast<std::int16_t>(static_cast<std::uint16_t>(u));
    return true;
  }
  bool takeF32(float& v) {
    std::uint32_t bits;
    if (!takeU32(bits)) return false;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&v, &bits, sizeof(v));
    return true;
  }
};

}  // namespace detail

// The scene -> versioned binary bytes (the format section): a pure,
// deterministic function of the scene (same scene, same bytes — the
// determinism test compares two serializations). One allocation (the
// output vector), setup path. @budget O(scene size); one allocation.
[[nodiscard]] inline std::vector<std::uint8_t> serializeIsoReferenceScene(
    const IsoReferenceScene& scene) noexcept {
  std::vector<std::uint8_t> out;
  // The size prefix (the format section's layout).
  out.reserve(1024u + static_cast<std::size_t>(scene.tileCount) * 4u +
                static_cast<std::size_t>(scene.spriteCount) * 14u +
                static_cast<std::size_t>(scene.parallaxCount) * 67u +
                static_cast<std::size_t>(scene.emitterCount) * 56u +
                static_cast<std::size_t>(scene.uiWidgetCount) * 23u);
  using detail::pushF32;
  using detail::pushI16le;
  using detail::pushI32le;
  using detail::pushU32le;
  using detail::pushU64le;
  using detail::pushU8;
  pushU32le(out, scene.version);
  pushU8(out, static_cast<std::uint8_t>(scene.variant));
  pushU8(out, 0u);
  pushU8(out, 0u);
  pushU8(out, 0u);
  pushU64le(out, scene.seed);
  pushI32le(out, scene.originTileX);
  pushI32le(out, scene.originTileY);
  pushI32le(out, scene.widthTiles);
  pushI32le(out, scene.heightTiles);
  pushI32le(out, scene.plateauTiles);
  pushI32le(out, scene.stepHeight);
  pushI32le(out, scene.maxSteps);
  pushU32le(out, scene.tileCount);
  pushU32le(out, scene.spriteCount);
  pushU32le(out, scene.emitterCount);
  pushU32le(out, scene.uiWidgetCount);
  pushU32le(out, scene.parallaxCount);
  pushU32le(out, 0u);  // reserved
  for (std::uint32_t i = 0; i < scene.tileCount; ++i) {
    pushI32le(out, scene.tileHeights[i]);
  }
  for (std::uint32_t i = 0; i < scene.spriteCount; ++i) {
    const IsoRefSceneSprite& s = scene.sprites[i];
    pushF32(out, s.x);
    pushF32(out, s.y);
    pushI16le(out, s.stepHeight);
    pushU8(out, s.atlasId);
    pushU8(out, s.blend);
    pushU8(out, s.frame);
    pushU8(out, s.reserved);
  }
  for (std::uint32_t i = 0; i < scene.parallaxCount; ++i) {
    const IsoRefSceneParallaxLayer& l = scene.parallax[i];
    pushF32(out, l.factor);
    pushF32(out, l.center.x);
    pushF32(out, l.center.y);
    pushF32(out, l.offset.x);
    pushF32(out, l.offset.y);
    pushF32(out, l.size.x);
    pushF32(out, l.size.y);
    pushF32(out, l.uv.u0);
    pushF32(out, l.uv.v0);
    pushF32(out, l.uv.u1);
    pushF32(out, l.uv.v1);
    pushU32le(out, l.atlasId);
    pushU32le(out, l.materialId);
    pushU8(out, l.blend);
    pushU8(out, l.scrollMode);
    pushU8(out, l.enabled);
    pushU8(out, l.reserved);
    pushI32le(out, l.depthLayer);
    pushF32(out, l.scrollSpeed.x);
    pushF32(out, l.scrollSpeed.y);
  }
  for (std::uint32_t i = 0; i < scene.emitterCount; ++i) {
    const IsoRefSceneEmitter& e = scene.emitters[i];
    pushF32(out, e.originX);
    pushF32(out, e.originY);
    pushI32le(out, e.depth);
    pushF32(out, e.velMinX);
    pushF32(out, e.velMinY);
    pushF32(out, e.velMaxX);
    pushF32(out, e.velMaxY);
    pushU32le(out, e.lifeMin);
    pushU32le(out, e.lifeMax);
    pushF32(out, e.sizeMin);
    pushF32(out, e.sizeMax);
    for (int c = 0; c < 4; ++c) pushU8(out, e.tint[c]);
    pushU32le(out, e.fadeTicks);
    pushU32le(out, e.continuousRate);
  }
  for (std::uint32_t i = 0; i < scene.uiWidgetCount; ++i) {
    const IsoRefSceneUiWidget& w = scene.uiWidgets[i];
    pushU8(out, w.kind);
    pushU8(out, w.opaque);
    pushI32le(out, w.x);
    pushI32le(out, w.y);
    pushI32le(out, w.width);
    pushI32le(out, w.height);
    pushU32le(out, w.atlasId);
    pushU8(out, w.label);
  }
  return out;
}

// The bytes -> scene (the format section + failure section): the
// versioned, domain-validated read of an UNTRUSTED byte stream
// (SCALE-004) — any violation fails the whole read with
// `MalformedInput` (nothing is partially applied). @budget O(size);
// two allocations (the vectors, on success only).
[[nodiscard]] inline laige::Result<IsoReferenceScene, laige::ErrorCode>
readIsoReferenceScene(std::span<const std::uint8_t> bytes) noexcept {
  using E = laige::ErrorCode;
  auto bad = [] {
    return laige::Result<IsoReferenceScene, E>::failure(E::MalformedInput);
  };
  detail::ByteCursor c{bytes.data(), bytes.size(), true};
  IsoReferenceScene scene;

  // The header (the format section's layout).
  std::uint32_t version{};
  std::uint8_t variant{};
  std::uint8_t reserved[3]{};
  std::uint32_t reservedTail{};
  std::int32_t stepHeight{}, maxSteps{};
  std::uint32_t tileCount{}, spriteCount{}, emitterCount{}, uiCount{},
      parallaxCount{};
  if (!c.takeU32(version) || version != kIsoRefSceneFormatVersion ||
      !c.takeU8(variant) || (variant != 0u && variant != 1u) ||
      !c.takeU8(reserved[0]) || !c.takeU8(reserved[1]) ||
      !c.takeU8(reserved[2]) || !c.takeU64(scene.seed) ||
      !c.takeI32(scene.originTileX) || !c.takeI32(scene.originTileY) ||
      !c.takeI32(scene.widthTiles) || !c.takeI32(scene.heightTiles) ||
      !c.takeI32(scene.plateauTiles) || !c.takeI32(stepHeight) ||
      !c.takeI32(maxSteps) || !c.takeU32(tileCount) ||
      !c.takeU32(spriteCount) || !c.takeU32(emitterCount) ||
      !c.takeU32(uiCount) || !c.takeU32(parallaxCount) ||
      !c.takeU32(reservedTail)) {
    return bad();
  }
  scene.variant = static_cast<IsoRefSceneVariant>(variant);
  scene.stepHeight = stepHeight;
  scene.maxSteps = maxSteps;
  scene.tileCount = tileCount;
  scene.spriteCount = spriteCount;
  scene.emitterCount = emitterCount;
  scene.uiWidgetCount = uiCount;
  scene.parallaxCount = parallaxCount;

  // The grid domain (the header's model section's constants): the
  // counts are consistent with the grid, the plateau divides both
  // axes, and the height model stays in the M2-ISO-01 domain.
  const std::uint64_t tiles =
      static_cast<std::uint64_t>(scene.widthTiles) *
      static_cast<std::uint64_t>(scene.heightTiles);
  constexpr std::uint64_t kMaxTiles =
      static_cast<std::uint64_t>(kIsoRefSceneFullGridTiles) *
      static_cast<std::uint64_t>(kIsoRefSceneFullGridTiles);
  if (scene.widthTiles < 1 || scene.heightTiles < 1 ||
      scene.plateauTiles < 1 ||
      scene.widthTiles % scene.plateauTiles != 0 ||
      scene.heightTiles % scene.plateauTiles != 0 || tiles != tileCount ||
      tileCount == 0 || tileCount > kMaxTiles ||
      stepHeight < 0 || stepHeight > 1024 || maxSteps < 1 ||
      maxSteps > 1024 ||
      static_cast<std::int64_t>(stepHeight) *
              static_cast<std::int64_t>(maxSteps) >
          kIsoDepthMaxStepHeight ||
      spriteCount > kIsoRefSceneFullSprites ||
      emitterCount > kIsoRefSceneEmitterCount ||
      uiCount > kIsoRefSceneUiWidgetCount ||
      parallaxCount > kIsoRefSceneParallaxLayerCount) {
    return bad();
  }

  // The terrain heights (row-major, the M2-ISO-01 |z| domain).
  scene.tileHeights.resize(tileCount);
  for (std::uint64_t i = 0; i < tileCount; ++i) {
    std::int32_t h{};
    if (!c.takeI32(h) || h < -kIsoDepthMaxStepHeight ||
        h > kIsoDepthMaxStepHeight) {
      return bad();
    }
    scene.tileHeights[i] = h;
  }

  // The sprites (the 14-byte record, the header's model section).
  scene.sprites.resize(spriteCount);
  for (std::uint64_t i = 0; i < spriteCount; ++i) {
    IsoRefSceneSprite& s = scene.sprites[i];
    if (!c.takeF32(s.x) || !c.takeF32(s.y) || !c.takeI16(s.stepHeight) ||
        !c.takeU8(s.atlasId) || !c.takeU8(s.blend) || !c.takeU8(s.frame) ||
        !c.takeU8(s.reserved) || !std::isfinite(s.x) || !std::isfinite(s.y) ||
        s.x < 0.0f || s.x >= static_cast<float>(scene.widthTiles) ||
        s.y < 0.0f || s.y >= static_cast<float>(scene.heightTiles) ||
        s.stepHeight < -kIsoDepthMaxStepHeight ||
        s.stepHeight > kIsoDepthMaxStepHeight || s.blend > 1u ||
        s.frame >= kIsoRefSceneSpriteSheetFrames) {
      return bad();
    }
  }

  // The parallax layers (the 67-byte record, the M2-PAR-01 domains).
  for (std::uint64_t i = 0; i < parallaxCount; ++i) {
    IsoRefSceneParallaxLayer& l = scene.parallax[i];
    if (!c.takeF32(l.factor) || !c.takeF32(l.center.x) ||
        !c.takeF32(l.center.y) || !c.takeF32(l.offset.x) ||
        !c.takeF32(l.offset.y) || !c.takeF32(l.size.x) ||
        !c.takeF32(l.size.y) || !c.takeF32(l.uv.u0) || !c.takeF32(l.uv.v0) ||
        !c.takeF32(l.uv.u1) || !c.takeF32(l.uv.v1) ||
        !c.takeU32(l.atlasId) || !c.takeU32(l.materialId) ||
        !c.takeU8(l.blend) || !c.takeU8(l.scrollMode) ||
        !c.takeU8(l.enabled) || !c.takeU8(l.reserved) ||
        !c.takeI32(l.depthLayer) || !c.takeF32(l.scrollSpeed.x) ||
        !c.takeF32(l.scrollSpeed.y) || !std::isfinite(l.factor) ||
        l.factor < 0.0f || l.factor > 1.0f ||
        !std::isfinite(l.center.x) || !std::isfinite(l.center.y) ||
        !std::isfinite(l.offset.x) || !std::isfinite(l.offset.y) ||
        !std::isfinite(l.size.x) || !std::isfinite(l.size.y) ||
        l.size.x <= 0.0f || l.size.y <= 0.0f || !std::isfinite(l.uv.u0) ||
        !std::isfinite(l.uv.v0) || !std::isfinite(l.uv.u1) ||
        !std::isfinite(l.uv.v1) || l.uv.u0 < 0.0f || l.uv.u1 > 1.0f ||
        l.uv.v0 < 0.0f || l.uv.v1 > 1.0f || !(l.uv.u0 < l.uv.u1) ||
        !(l.uv.v0 < l.uv.v1) || !std::isfinite(l.scrollSpeed.x) ||
        !std::isfinite(l.scrollSpeed.y) || l.blend > 1u ||
        l.scrollMode > 1u || l.enabled > 1u ||
        l.depthLayer < static_cast<std::int32_t>(-kIsoDepthLayerBias) ||
        l.depthLayer > kIsoDepthLayerMax) {
      return bad();
    }
  }

  // The emitters (the 56-byte record, the M2-PART-01 domains).
  for (std::uint64_t i = 0; i < emitterCount; ++i) {
    IsoRefSceneEmitter& e = scene.emitters[i];
    if (!c.takeF32(e.originX) || !c.takeF32(e.originY) ||
        !c.takeI32(e.depth) || !c.takeF32(e.velMinX) ||
        !c.takeF32(e.velMinY) || !c.takeF32(e.velMaxX) ||
        !c.takeF32(e.velMaxY) || !c.takeU32(e.lifeMin) ||
        !c.takeU32(e.lifeMax) || !c.takeF32(e.sizeMin) ||
        !c.takeF32(e.sizeMax) || !c.takeU8(e.tint[0]) ||
        !c.takeU8(e.tint[1]) || !c.takeU8(e.tint[2]) || !c.takeU8(e.tint[3]) ||
        !c.takeU32(e.fadeTicks) || !c.takeU32(e.continuousRate) ||
        !std::isfinite(e.originX) || !std::isfinite(e.originY) ||
        !std::isfinite(e.velMinX) || !std::isfinite(e.velMinY) ||
        !std::isfinite(e.velMaxX) || !std::isfinite(e.velMaxY) ||
        !std::isfinite(e.sizeMin) || !std::isfinite(e.sizeMax) ||
        e.depth < -kIsoDepthMaxStepHeight || e.depth > kIsoDepthMaxStepHeight ||
        e.lifeMin < 1u || e.lifeMax > kParticleMaxLifeTicks ||
        e.lifeMin > e.lifeMax || e.sizeMin < 0.0f || e.sizeMax < e.sizeMin ||
        e.fadeTicks > e.lifeMax) {
      return bad();
    }
  }

  // The UI widgets (the 23-byte record, the M2-UI-01 domains).
  for (std::uint64_t i = 0; i < uiCount; ++i) {
    IsoRefSceneUiWidget& w = scene.uiWidgets[i];
    if (!c.takeU8(w.kind) || !c.takeU8(w.opaque) || !c.takeI32(w.x) ||
        !c.takeI32(w.y) || !c.takeI32(w.width) || !c.takeI32(w.height) ||
        !c.takeU32(w.atlasId) || !c.takeU8(w.label) ||
        w.kind >= kUiWidgetKindCount || w.opaque > 1u || w.label > 1u ||
        w.width < 0 || w.height < 0 ||
        static_cast<std::uint32_t>(w.width) > kUiMaxDimPx ||
        static_cast<std::uint32_t>(w.height) > kUiMaxDimPx ||
        std::abs(w.x) > kUiMaxCoordPx || std::abs(w.y) > kUiMaxCoordPx) {
      return bad();
    }
  }

  // The exact-length contract (the format section): trailing bytes
  // are rejected — a longer file is not this scene.
  if (c.n != 0) return bad();
  return laige::Result<IsoReferenceScene, E>::success(std::move(scene));
}

// The scene -> file (the failure section): serialize to `<path>.tmp`
// in the same directory, flush, and rename into `<path>` (atomic —
// a killed run never leaves a partial file at the target, the
// laige-run replay-log precedent). @budget O(scene size); two
// allocations (the bytes + the temp name).
[[nodiscard]] inline laige::Status writeIsoReferenceScene(
    const IsoReferenceScene& scene, std::string_view path) noexcept {
  const std::string target(path);
  const std::string temp = target + ".tmp";
  const std::vector<std::uint8_t> bytes = serializeIsoReferenceScene(scene);
  std::FILE* f{};
#if defined(_MSC_VER)
  if (fopen_s(&f, temp.c_str(), "wb") != 0) {
    return laige::Status::failure(laige::ErrorCode::IoError);
  }
#else
  f = std::fopen(temp.c_str(), "wb");
#endif
  if (f == nullptr) {
    return laige::Status::failure(laige::ErrorCode::IoError);
  }
  const bool wrote =
      (bytes.empty() ||
       std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
  const bool flushed = std::fclose(f) == 0;
  f = nullptr;
  if (!wrote || !flushed) {
    std::remove(temp.c_str());
    return laige::Status::failure(laige::ErrorCode::IoError);
  }
  if (std::rename(temp.c_str(), target.c_str()) != 0) {
    std::remove(temp.c_str());
    return laige::Status::failure(laige::ErrorCode::IoError);
  }
  return laige::Status{};
}

namespace detail {

// The float point -> the backend's Vec2 (the RENDER-006 load boundary;
// the parallax test oracle's pattern). The scene file's floats are
// the canonical values; the backend conversion is the one documented
// rounding per component.
template <typename Backend>
inline typename laige::sim::SimMath<Backend>::Vec2
isoRefBackendVec2(float x, float y) noexcept {
  if constexpr (std::is_same_v<Backend, laige::sim::Fp32Pinned>) {
    return typename laige::sim::SimMath<Backend>::Vec2{x, y};
  } else {
    return typename laige::sim::SimMath<Backend>::Vec2{
        laige::fpx16_16::fromFloat(x), laige::fpx16_16::fromFloat(y)};
  }
}

// The integer / float -> the backend's Scalar (the same boundary):
// exact for integers (|v| <= 2047 << 16 fits the Q16.16 raw domain),
// one documented rounding for the floats.
template <typename Backend>
inline typename laige::sim::SimMath<Backend>::Scalar
isoRefScalarFromInt(std::int32_t v) noexcept {
  if constexpr (std::is_same_v<Backend, laige::sim::Fp32Pinned>) {
    return static_cast<float>(v);
  } else {
    return laige::fpx16_16::fromInt32(v);
  }
}
template <typename Backend>
inline typename laige::sim::SimMath<Backend>::Scalar
isoRefScalarFromFloat(float v) noexcept {
  if constexpr (std::is_same_v<Backend, laige::sim::Fp32Pinned>) {
    return v;
  } else {
    return laige::fpx16_16::fromFloat(v);
  }
}

}  // namespace detail

// The engine-side world of one loaded reference scene (the header's
// ownership section): the five engine objects the scene builds —
// the terrain TileMap, the ParallaxLayers registry, the Particle
// System, the UI UiTree, and the StringTable (the button label's
// interned "OK") — plus a copy of the scene they were built from
// and the 4 precomputed sprite-sheet frame UVs (the header's model
// section — 4 rects, never per sprite).
//
// Move-only (the SpriteBatcher precedent). The default-constructed /
// moved-from / failed-load state is stopped: `valid()` false, the
// accessors return the stopped objects (whose own operations fail or
// no-op per their own contracts — the tilemap's accessor ASSERTs the
// live state, the SpriteBatcher::at precedent), and
// `declareIsoReferenceScene` fails `InvalidArgument`. One owner (the
// scene setup / the test); the render phase reads it through
// `declareIsoReferenceScene` (the only mutation: the parallax scroll
// offsets — the M2-PAR-01 presentation state, the documented
// once-per-frame pacing).
template <typename Backend>
class IsoReferenceSceneWorld {
  static_assert(std::is_same_v<Backend, laige::sim::Fpx16_16> ||
                    std::is_same_v<Backend, laige::sim::Fp32Pinned>,
                "IsoReferenceSceneWorld is templated over the SimMath "
                "backends");

 public:
  // The stopped state (default / moved-from / failed load).
  IsoReferenceSceneWorld() noexcept = default;

  // Move-only (the house pattern — the moved-from world is stopped).
  IsoReferenceSceneWorld(const IsoReferenceSceneWorld&) = delete;
  IsoReferenceSceneWorld& operator=(const IsoReferenceSceneWorld&) = delete;
  IsoReferenceSceneWorld(IsoReferenceSceneWorld&& o) noexcept
      : scene_(std::move(o.scene_)),
        tiles_(std::move(o.tiles_)),
        parallax_(std::move(o.parallax_)),
        particles_(std::move(o.particles_)),
        ui_(std::move(o.ui_)),
        strings_(std::move(o.strings_)),
        frameUv_(o.frameUv_),
        loaded_(o.loaded_) {
    o.loaded_ = false;
  }
  IsoReferenceSceneWorld& operator=(IsoReferenceSceneWorld&& o) noexcept {
    if (this != &o) {
      scene_ = std::move(o.scene_);
      tiles_ = std::move(o.tiles_);
      parallax_ = std::move(o.parallax_);
      particles_ = std::move(o.particles_);
      ui_ = std::move(o.ui_);
      strings_ = std::move(o.strings_);
      frameUv_ = o.frameUv_;
      loaded_ = o.loaded_;
      o.loaded_ = false;
    }
    return *this;
  }

  // True when the world holds a loaded scene (loadIsoReferenceScene
  // succeeded).
  [[nodiscard]] bool valid() const noexcept { return loaded_; }

  // The scene this world was built from (the owned copy — the
  // world is self-contained; M2-PERF-01/M2-AC-01 read the scene's
  // parameters through this).
  [[nodiscard]] const IsoReferenceScene& scene() const noexcept {
    return scene_;
  }
  // The terrain tilemap. The world's live-state contract: a stopped
  // world (default / moved-from / failed load) has no tilemap — the
  // caller checks `valid()` first (the SpriteBatcher::at assert
  // precedent — assertions protect programmer invariants, CPP-012).
  [[nodiscard]] TileMap<Backend>& tiles() noexcept {
    assert(tiles_ != nullptr &&
           "IsoReferenceSceneWorld::tiles: the world is stopped");
    return *tiles_;
  }
  [[nodiscard]] ParallaxLayers<Backend>& parallax() noexcept {
    return parallax_;
  }
  [[nodiscard]] ParticleSystem<Backend>& particles() noexcept {
    return particles_;
  }
  [[nodiscard]] UiTree& ui() noexcept { return ui_; }
  [[nodiscard]] StringTable& strings() noexcept { return strings_; }

  // The world pass's declared frame, MINIMUM (the content count):
  // the scene's sprites + tiles + 1 quad per parallax layer. A
  // scrolled auto layer SPLITs into up to 4 quads (the parallax.h
  // wrap contract), so the batcher's frame budget must cover the
  // WORST case, `worstCaseFrameCount()`, PLUS the live particles
  // (the pool's live count, bounded by its cap).
  [[nodiscard]] std::uint32_t contentFrameCount() const noexcept {
    return scene_.spriteCount + scene_.tileCount + scene_.parallaxCount;
  }
  [[nodiscard]] std::uint32_t worstCaseFrameCount() const noexcept {
    return scene_.spriteCount + scene_.tileCount +
           4u * scene_.parallaxCount;
  }

 private:
  // The loader's and the per-frame declare's access (the header's
  // ownership section).
  template <typename B>
  friend laige::Result<IsoReferenceSceneWorld<B>, laige::ErrorCode>
  loadIsoReferenceScene(const IsoReferenceScene&) noexcept;
  template <typename B>
  friend laige::Status declareIsoReferenceScene(
      IsoReferenceSceneWorld<B>&, SpriteBatcher&, Vec2) noexcept;

  IsoReferenceScene scene_{};
  std::unique_ptr<TileMap<Backend>> tiles_{};  // empty <=> stopped
  ParallaxLayers<Backend> parallax_{};
  ParticleSystem<Backend> particles_{};
  UiTree ui_{};
  StringTable strings_{};
  std::array<SpriteUvRect, kIsoRefSceneSpriteSheetFrames> frameUv_{};
  bool loaded_{false};
};

// The scene -> the engine-side world (the header's ownership/failure
// sections): the setup-path build of the scene's five engine objects
// (the O(scene size) setup — the tilemap's rebuild, the three
// setLayer, the eight addEmitter, the two addChild, one intern, the
// 4 frame-UV precomputations). First failure wins: a failed step
// fails the whole load with that step's error (InvalidArgument /
// BudgetExhausted), and the returned world is the stopped state
// (nothing owned). @budget O(scene size); a handful of setup
// allocations (the objects' own — the house pattern).
template <typename Backend>
[[nodiscard]] inline laige::Result<IsoReferenceSceneWorld<Backend>,
                                   laige::ErrorCode>
loadIsoReferenceScene(const IsoReferenceScene& scene) noexcept {
  using E = laige::ErrorCode;
  using World = IsoReferenceSceneWorld<Backend>;
  IsoReferenceSceneWorld<Backend> world;

  // (1) The terrain tilemap (one (atlas, material, blend) group for
  //     the whole ground — the header's plan section).
  typename TileMap<Backend>::Options topts{};
  topts.originTileX = scene.originTileX;
  topts.originTileY = scene.originTileY;
  topts.widthTiles = scene.widthTiles;
  topts.heightTiles = scene.heightTiles;
  topts.layer = kIsoDepthGroundLayer;
  topts.maxAnimations = 1u;  // the scene has no animated tiles
  auto tmr = TileMap<Backend>::create(topts);
  if (tmr.isError()) {
    return laige::Result<World, E>::failure(tmr.error());
  }
  // The house unique_ptr idiom (gl_context.h / parallax.h): empty <=>
  // stopped (TileMap has no default constructor — the create-only
  // pattern).
  world.tiles_ = std::make_unique<TileMap<Backend>>(std::move(tmr).takeValue());
  // The tile data (the scene's terrain heights — one source of truth).
  std::vector<TileData> tileData(scene.tileCount);
  for (std::uint32_t i = 0; i < scene.tileCount; ++i) {
    tileData[i].textureId = kIsoRefSceneGroundAtlas;
    tileData[i].height = scene.tileHeights[i];
    tileData[i].animationId = 0u;
  }
  if (!world.tiles_->rebuild(tileData).ok()) {
    return laige::Result<World, E>::failure(E::InvalidArgument);
  }

  // (2) The parallax registry (the scene's 3 image layers, in
  //     declaration order 0..2 — bg, mid, fg).
  typename ParallaxLayers<Backend>::Options plopts{};
  auto plr = ParallaxLayers<Backend>::create(plopts);
  if (plr.isError()) {
    return laige::Result<World, E>::failure(plr.error());
  }
  world.parallax_ = std::move(plr).takeValue();
  for (std::uint32_t i = 0; i < scene.parallaxCount; ++i) {
    const IsoRefSceneParallaxLayer& l = scene.parallax[i];
    // The layer def's points are the RENDER Vec2 (floats) — the
    // parallax's presentation-space model (RENDER-006); the backend
    // conversion happens inside its key computation.
    ParallaxLayerDef def{};
    def.id = i;
    def.source = ParallaxSource::Image;
    def.factor = l.factor;
    def.center = Vec2{l.center.x, l.center.y};
    def.offset = Vec2{l.offset.x, l.offset.y};
    def.size = Vec2{l.size.x, l.size.y};
    def.uv = l.uv;
    def.atlasId = l.atlasId;
    def.materialId = l.materialId;
    def.blend = static_cast<BlendMode>(l.blend);
    def.depthLayer = l.depthLayer;
    def.scrollMode = static_cast<ParallaxScrollMode>(l.scrollMode);
    def.scrollSpeed = Vec2{l.scrollSpeed.x, l.scrollSpeed.y};
    def.enabled = l.enabled != 0u;
    const auto sr = world.parallax_.setLayer(def);
    if (!sr.ok()) {
      return laige::Result<World, E>::failure(sr.error());
    }
  }

  // (3) The particle system (the scene's emitters; the system's Prng
  //     is seeded from the scene's seed — the M0-CORE-06 substream
  //     pattern at the engine boundary).
  typename ParticleSystem<Backend>::Options popts{};
  popts.maxEmitters = scene.emitterCount;
  popts.seed = scene.seed;
  auto psr = ParticleSystem<Backend>::create(popts);
  if (psr.isError()) {
    return laige::Result<World, E>::failure(psr.error());
  }
  world.particles_ = std::move(psr).takeValue();
  for (std::uint32_t i = 0; i < scene.emitterCount; ++i) {
    const IsoRefSceneEmitter& e = scene.emitters[i];
    ParticleEmitterDef<Backend> ed{};
    ed.origin = detail::isoRefBackendVec2<Backend>(e.originX, e.originY);
    ed.depth = detail::isoRefScalarFromInt<Backend>(e.depth);
    ed.velMin = detail::isoRefBackendVec2<Backend>(e.velMinX, e.velMinY);
    ed.velMax = detail::isoRefBackendVec2<Backend>(e.velMaxX, e.velMaxY);
    ed.lifeMin = e.lifeMin;
    ed.lifeMax = e.lifeMax;
    ed.sizeMin = detail::isoRefScalarFromFloat<Backend>(e.sizeMin);
    ed.sizeMax = detail::isoRefScalarFromFloat<Backend>(e.sizeMax);
    for (int c = 0; c < 4; ++c) ed.tint[c] = e.tint[c];
    ed.fadeTicks = e.fadeTicks;
    ed.continuousRate = e.continuousRate;
    const auto ar = world.particles_.addEmitter(ed);
    if (!ar.ok()) {
      return laige::Result<World, E>::failure(ar.error());
    }
  }

  // (4) The string table (the button's "OK" label) + the UI tree
  //     (the scene's 2 widgets on the reference screen).
  auto str = StringTable::create(StringTable::Options{});
  if (str.isError()) {
    return laige::Result<World, E>::failure(str.error());
  }
  world.strings_ = std::move(str).takeValue();
  const std::uint32_t okCodes[2] = {'O', 'K'};
  const auto okRef = world.strings_.intern(
      std::span<const std::uint32_t>(okCodes, 2));
  if (okRef.isError()) {
    return laige::Result<World, E>::failure(okRef.error());
  }
  UiTree::Options uo{};
  uo.maxWidgets = kIsoRefSceneUiMaxWidgets;
  uo.maxEvents = kIsoRefSceneUiMaxEvents;
  uo.screenWidth = kIsoRefSceneUiScreenWidth;
  uo.screenHeight = kIsoRefSceneUiScreenHeight;
  auto utr = UiTree::create(uo);
  if (utr.isError()) {
    return laige::Result<World, E>::failure(utr.error());
  }
  world.ui_ = std::move(utr).takeValue();
  for (std::uint32_t i = 0; i < scene.uiWidgetCount; ++i) {
    const IsoRefSceneUiWidget& w = scene.uiWidgets[i];
    UiWidgetSpec spec{};
    spec.kind = static_cast<UiWidgetKind>(w.kind);
    spec.x = w.x;
    spec.y = w.y;
    spec.width = w.width;
    spec.height = w.height;
    spec.opaque = w.opaque != 0u;
    spec.atlasId = w.atlasId;
    // The widget's art is its whole atlas (the scene assigns the
    // texture — M3-ASSET-01).
    spec.image = SpriteUvRect{0.0f, 0.0f, 1.0f, 1.0f};
    if (w.label == kIsoRefSceneLabelOk) {
      spec.label = okRef.value();
    }
    const auto acr = world.ui_.addChild(spec);
    if (!acr.ok()) {
      return laige::Result<World, E>::failure(acr.error());
    }
  }

  // (5) The 4 precomputed sheet frame UVs (the tight 32 x 32 sheet —
  //     the header's model section): precomputed ONCE at load, never
  //     per sprite (the 50k-frame zero-allocation budget, PERF-003).
  SpriteFrameLayout layout{};
  layout.frameWidth = kIsoRefSceneSpriteSheetFrameTexels;
  layout.frameHeight = kIsoRefSceneSpriteSheetFrameTexels;
  layout.columns = 2u;
  layout.rows = 2u;
  layout.frameSpacing = 0u;
  layout.sheetBorder = 0u;
  constexpr std::uint32_t kSheetTexels = 2u * kIsoRefSceneSpriteSheetFrameTexels;
  for (std::uint32_t f = 0; f < kIsoRefSceneSpriteSheetFrames; ++f) {
    const auto uv = spriteFrameUv(f, layout, kSheetTexels, kSheetTexels);
    if (uv.isError()) {
      return laige::Result<World, E>::failure(uv.error());
    }
    world.frameUv_[f] = uv.value();
  }

  world.scene_ = scene;
  world.loaded_ = true;
  return laige::Result<World, E>::success(std::move(world));
}

// The world's per-frame declare (the header's model/performance
// sections): ONE call = the whole world pass into the batcher — the
// parallax layers (after their once-per-frame scroll advance), the
// terrain tiles, the 50k sprites (the scene's array order — the
// RENDER-003 deterministic insertion), and the live particles (one
// additive group). O(frame items), NO ALLOCATION (the batcher's
// storage is pre-sized; the per-sprite isoDepthKey is integer math;
// the UV rects are the world's 4 precomputed values).
//
// The batcher must be live with its frame window OPEN (the frame
// protocol, the declareParticles/TileMap::declareTo precedent); a
// stopped world fails `InvalidArgument`. First failure wins: the
// sub-step's error is the call's error, and the items already
// declared by this call remain in the frame (the batcher owns the
// frame state — the game re-declares the whole frame every frame).
// `cameraPos` is the RENDER Vec2 (the float presentation position —
// the parallax's declareTo input, RENDER-006); the sprite depth keys
// convert from the scene's floats to the backend's Vec2 inside the
// loop (the isoDepthKey's domain).
template <typename Backend>
[[nodiscard]] inline laige::Status declareIsoReferenceScene(
    IsoReferenceSceneWorld<Backend>& world, SpriteBatcher& batcher,
    Vec2 cameraPos) noexcept {
  if (!world.valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  const IsoReferenceScene& scene = world.scene();
  // (1) The parallax layers: the auto layers' once-per-frame advance
  //     (the documented pacing — the header's misuse section), then
  //     the layer quads at the camera's presentation position.
  world.parallax().advanceScrolls();
  laige::Status st = world.parallax().declareTo(batcher, cameraPos);
  if (!st.ok()) return st;
  // (2) The terrain tiles (one group — the plan section).
  typename TileMap<Backend>::DeclareOptions tileOpts{};
  st = world.tiles().declareTo(batcher, tileOpts);
  if (!st.ok()) return st;
  // (3) The sprites: the scene's declaration order (the array order),
  //     each on its standing tile's height (the auto-depth wiring).
  for (std::uint32_t i = 0; i < scene.spriteCount; ++i) {
    const IsoRefSceneSprite& s = scene.sprites[i];
    SpriteItem item;
    item.pos = Vec2{s.x, s.y};
    item.depthKey = isoDepthKey<Backend>(
        detail::isoRefBackendVec2<Backend>(s.x, s.y), s.stepHeight,
        kIsoDepthGroundLayer);
    item.uv = world.frameUv_[s.frame];
    item.frameIndex = s.frame;
    item.atlasId = s.atlasId;
    item.blend = static_cast<BlendMode>(s.blend);
    item.materialId = 0u;
    const auto added = batcher.add(std::move(item));
    if (!added.ok()) {
      return laige::Status::failure(added.error());
    }
  }
  // (4) The live particles (one additive group — the plan section).
  ParticleDeclareOptions popts{};
  popts.atlasId = kIsoRefSceneParticleAtlas;
  popts.blend = BlendMode::Additive;
  return declareParticles(batcher, world.particles(), popts);
}

}  // namespace laige::render
