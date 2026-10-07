// laige-render tilemap (M2-TILE-01): the chunked tile grid data, the
// auto-depth wiring of the M2-ISO-02 depth key table, and the static
// tile-quad batch path into the sprite batcher.
//
// FR-2.6: "Tilemap: chunks, per-tile depth/height, auto-depth" — a
// chunked grid of tiles; per tile: a texture reference, a depth/height
// value, and an animation id (data only in M2 — M2-TILE-02 drives the
// frame cycle); a tile's Y height is AUTOMATICALLY reflected in its
// depth key (the wiring into M2-ISO-02). S-5 (PRD §9.1): rendering
// goes through the batcher — the tilemap's quads are declared into the
// sprite batcher as sprites with a fixed frame, never drawn directly.
// ARCH-009: the tile data is presentation-side (headless-buildable,
// sim-side in the sense that it never needs GL); the render phase
// consumes it read-only. G-R11: the tile's depth key is engine-owned
// (the table's) — game code never writes the key. RENDER-003: the
// declaration order is deterministic (the tile grid's row-major
// iteration).
//
//   TileData               one tile's data (texture id, height,
//                          animation id)
//   TileMap<Backend>       the chunked tile grid (owns the table)
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
//   animationId  the animation-table reference (DATA ONLY in M2 —
//                M2-TILE-02 cycles frames from it; the batcher and the
//                render never touch it)
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
// A tile renders as a SPRITE with a FIXED frame (the scope's "tiles
// are sprites with a fixed frame"):
//
//   pos          the tile's CENTER (gx + 0.5, gy + 0.5) — the same
//                point the table quantizes (the float conversion is
//                exact for |gx| <= 32766 — dyadic)
//   scale        (1, 1) — the unit quad spans the tile's world cell
//   rotation     0
//   uv           the fixed frame — the `DeclareOptions::uv` rect for
//                every tile (default (0, 0, 1, 1) = the full tile
//                texture); per-tile UV frames (tile-sheet frames,
//                animated frames) are the asset/animation steps
//                (M2-TILE-02, M3-ASSET-01 — `SpriteItem.uv` already
//                carries them)
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
// Determinism (RENDER-003, ARCH-010 scope — presentation-only)
// ---------------------------------------------------------------------------
//
// `declareTo` declares the tiles in the tile grid's ROW-MAJOR order
// (tileY outer, tileX fastest — the flat array's order, cache-friendly).
// The tile's grid position is a static tile's stable identity (the
// FR-1.2 entity-order analog): the declaration order is the insertion
// order the M2-SORT-01 stable sort turns into the (key, insertion
// position) total order — same tile data → bit-identical batches,
// every frame and every platform.
//
// ---------------------------------------------------------------------------
// Ownership, lifetime, threading
// ---------------------------------------------------------------------------
//
// One owner — the sim/scene-owner thread (the scene; the M2-GL-02
// frame pipeline's cull/batch stage owns the render-side declaration).
// The tilemap is move-only (CORE-009); the storage is pre-sized at
// creation (the ONLY allocations are `create` — one table storage +
// one 8 B/tile data array — and `rebuild`'s setup-path temporary,
// below). Writes (`setTile`, `rebuild`) happen in the sim phase;
// reads (`tileAt`, `depthKeyAt`, `tileHeightAt`, `covers`,
// `declareTo`) in the render phase; the phases do not overlap
// (CONC-001, the table's contract — one owner per datum). Not
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
// Rejected operations leave ALL state (tile data and the table)
// unchanged.
//
// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------
//
// No standalone `budgets.json` entry: the per-frame declare cost
// (O(tiles) adds + the sort) is PART of the composite 50k render-CPU
// budget (PRD §8.1, `sprites_50k_cpu` — M2-PERF-01 measures the
// reference scene with tile quads included; `declareTo` declares the
// WHOLE requested grid — visible-rect culling lands with M2-PERF-01,
// and the budget's worst case is the full grid anyway).
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
#include "laige/render/sprite_batcher.h"
#include "laige/sim_math.h"

namespace laige::render {

// One tile's data (the value the game writes on load/edit and reads
// back; plain value — no GL, no allocation).
struct TileData {
  // The texture/atlas reference (the game assigns it — M3-ASSET-01).
  std::uint32_t textureId{};
  // The tile's step height in world units (the standing surface's
  // elevation — the auto-depth source, the M2-ISO-01 `z` input).
  std::int32_t height{};
  // The animation-table reference (DATA ONLY in M2 — M2-TILE-02
  // cycles frames from it; the batch path never touches it).
  std::uint32_t animationId{};

  friend constexpr bool operator==(const TileData&, const TileData&) = default;
};

// The tile grid + its depth table (M2-ISO-02) + the batch path.
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
  // The create options — the table's options themselves (one type,
  // passed through to the table's create — the validation is the
  // table's, first failure wins, InvalidArgument): `originTileX/Y`
  // (default 0), `widthTiles`/`heightTiles` (required >= 1),
  // `chunkTiles` (default `kIsoDepthTableChunkTiles` = 16, a power of
  // two >= 1), `maxChunks` (default
  // `kIsoDepthTableDefaultMaxChunks` = 1024), `layer` (default
  // `kIsoDepthGroundLayer` = 0 — a parallax tile LAYER gets its own
  // tilemap with its layer value, M2-PAR-01 — the values are
  // documented in laige/render/parallax.h).
  using Options = IsoDepthKeyTable<Backend>::Options;

  // The per-frame declare options (the fixed-frame quad model, above):
  // the group-key fields for every tile sprite + the fixed UV frame.
  struct DeclareOptions {
    // The material reference (0 = the default material).
    std::uint32_t materialId{0};
    // The blend state of every tile sprite (the group key's third
    // field).
    BlendMode blend{BlendMode::Alpha};
    // The fixed frame: every tile quad's UV sub-rect (default
    // (0, 0, 1, 1) = the full tile texture).
    SpriteUvRect uv{0.0f, 0.0f, 1.0f, 1.0f};
  };

  // Creates the tilemap (setup path — the only allocations besides
  // rebuild's setup temporary): validates the options (the table's
  // create — first failure wins, InvalidArgument), pre-sizes the
  // table's initial grid chunks, and pre-sizes the per-tile data
  // array (8 B/tile, the requested grid). Flat/empty init: every tile
  // is {textureId 0, height 0, animationId 0} — no tile is set.
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
  // domain, |h| <= kIsoDepthMaxStepHeight) leave the tile data AND
  // the table UNCHANGED (validated before any write); the returned
  // Status is the failure channel the caller handles and logs
  // (LOG-002).
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
  // domain — validated over the WHOLE span before any write) leave
  // the tile data AND the table UNCHANGED.
  [[nodiscard]]
  Status rebuild(std::span<const TileData> tiles) noexcept;

  // The render path (the frame pipeline's cull/batch stage): declares
  // this tilemap's static tile quads into the batcher's current frame
  // window — one SpriteItem per tile of the REQUESTED grid, in the
  // grid's row-major order (tileY outer, tileX fastest — the preamble's
  // quad model: the tile's center, the table's key (auto-depth), the
  // tile's texture, the fixed frame). O(tileCount), zero allocation,
  // no GL, no logging — the adds are O(1) batcher operations.
  //
  // Precondition: the batcher's frame window is open (a built frame's
  // window is closed — call beginFrame first; the check is here,
  // first failure wins, NOTHING declared on failure). On a stopped
  // batcher (capacity 0) the first add fails — BudgetExhausted,
  // nothing declared. The WHOLE requested grid is declared
  // (visible-rect culling lands with M2-PERF-01 — the composite
  // 50k budget's worst case is the full grid).
  [[nodiscard]]
  Status declareTo(SpriteBatcher& batcher,
                   const DeclareOptions& options) noexcept;

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
  // The requested grid's tile count (widthTiles * heightTiles).
  std::size_t tileCount() const noexcept {
    return static_cast<std::size_t>(options_.widthTiles) *
           static_cast<std::size_t>(options_.heightTiles);
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
        tiles_(std::make_unique<TileSlot[]>(tileCount())) {}

  // The per-tile data slot (the requested grid; the height lives in
  // the table — one source of truth). 8 B/tile.
  struct TileSlot {
    std::uint32_t textureId{};
    std::uint32_t animationId{};
  };

  // The flat index of the requested-grid tile (row-major, tileX
  // fastest). Precondition: covers().
  std::size_t requestedIndex(std::int32_t tileX,
                             std::int32_t tileY) const noexcept {
    return static_cast<std::size_t>(tileY - options_.originTileY) *
               static_cast<std::size_t>(options_.widthTiles) +
           static_cast<std::size_t>(tileX - options_.originTileX);
  }

  Options options_;
  IsoDepthKeyTable<Backend> table_;
  std::unique_ptr<TileSlot[]> tiles_;
};

template <typename Backend>
Result<TileMap<Backend>> TileMap<Backend>::create(const Options& options)
    noexcept {
  // The table's create validates the identical options (first failure
  // wins — the table's documented order); the tilemap adds nothing to
  // validate (no duplicated validation, CORE-004). The create path is
  // the only allocation besides rebuild's setup temporary.
  auto table = IsoDepthKeyTable<Backend>::create(options);
  if (!table.ok()) return Result<TileMap>::failure(table.error());
  TileMap map(options, std::move(table).takeValue());
  return Result<TileMap>::success(std::move(map));
}

template <typename Backend>
Status TileMap<Backend>::setTile(std::int32_t tileX, std::int32_t tileY,
                                 std::uint32_t textureId, std::int32_t height,
                                 std::uint32_t animationId) noexcept {
  // Boundary validation (first failure wins — both failures are
  // InvalidArgument, so the order is unobservable): the tile lies in
  // the requested grid and the height is in the key domain (the
  // table's setTile contract).
  if (tileX < options_.originTileX ||
      tileX >= options_.originTileX + options_.widthTiles ||
      tileY < options_.originTileY ||
      tileY >= options_.originTileY + options_.heightTiles ||
      height < -kIsoDepthMaxStepHeight ||
      height > kIsoDepthMaxStepHeight) {
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
  // The height domain — validated over the WHOLE span before any
  // write (a rejected load leaves both the data and the table
  // unchanged):
  for (std::size_t i = 0; i < cells; ++i) {
    if (tiles[i].height < -kIsoDepthMaxStepHeight ||
        tiles[i].height > kIsoDepthMaxStepHeight) {
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
Status TileMap<Backend>::declareTo(SpriteBatcher& batcher,
                                   const DeclareOptions& options) noexcept {
  // The frame protocol (the batcher's window): a built frame's window
  // is closed — call beginFrame first (checked here; first failure
  // wins, nothing declared).
  if (batcher.frameBuilt()) return Status(ErrorCode::InvalidArgument);
  // The fixed-frame tile quad (the preamble's quad model). The base
  // item carries the per-call options; the per-tile fields are set in
  // the loop (the add takes the item by value — the declared value is
  // a copy, no aliasing of the base).
  SpriteItem item;
  item.uv = options.uv;
  item.materialId = options.materialId;
  item.blend = options.blend;
  item.frameIndex = 0;  // static tile (M2-TILE-02 drives animation)
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
