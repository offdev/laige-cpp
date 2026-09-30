// laige-render per-scene-chunk isometric depth key table (M2-ISO-02):
// the precomputed, incrementally-updated mapping from the tile grid to
// engine-owned isometric depth keys.
//
// FR-2.2: "in isometric mode the depth key = ground (x + y)
// contribution + tile/step height, precomputed at scene build and
// INCREMENTALLY UPDATED on tile/height changes (not recomputed per
// frame)". G-R11: the isometric depth is engine-owned — the game
// reads keys from this table, it never writes z-order math (S-5).
// PRD §8.1: "isometric depth-key rebuild (10k dirty cells after a
// terrain edit) <= 0.2 ms" — the budgets.json `iso_depthkey_rebuild`
// entry, gated by the IsoDepthTableBudget suite (`ctest -R
// iso_depth_table`, the non-instrumented trees — methodology §4).
//
//   IsoDepthKeyTable<Backend>   one table per scene region / tile layer
//
// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
//
// The scene's tile grid is a rectangle of integer tile coordinates
//
//   [originTileX, originTileX + widthTiles) x
//   [originTileY,   originTileY + heightTiles)
//
// The table organizes it in square chunks of `chunkTiles` x
// `chunkTiles` tiles (a power of two; default 16 — the standard
// tilemap chunk size, M2-TILE-01) and owns ONE pre-sized flat storage
// for the covered region (the setup path — the only places the table
// allocates are creation and ensureChunk growth, which re-allocates
// once, still setup):
//
//   one CellRecord per covered cell, flat row-major, tileX fastest:
//   { key, qBase, height }
//
//   height   the tile's step height in world units — the last value
//            the caller set (0 on a fresh table: flat ground)
//   qBase    the backend-quantized ground contribution of the cell's
//            CENTER (gx + 0.5, gy + 0.5):  q = round((x + y) * 16) —
//            a pure function of the cell, computed once at table
//            creation (or growth) through the same backend add +
//            quantize the M2-ISO-01 key uses
//            (detail::IsoDepthSumQuant); it never changes (tile
//            positions do not move — M2-TILE-01)
//   key      the current depth key — bit-identical to
//            isoDepthKey<Backend>(center, height, layer) (the
//            derivation below; the tests pin it)
//
// Covered region: growth happens chunk by chunk, so the covered
// region is the chunk-aligned SUPERSET of the requested grid — it may
// extend up to chunkTiles − 1 tiles past a requested edge (the
// partial edge cells carry real records; the scene simply leaves them
// flat unless it sets them). rebuild() takes the covered rectangle
// (coveredCellCount() cells), not the requested one.
//
// ---------------------------------------------------------------------------
// Incremental update contract (FR-2.2 "incrementally updated")
// ---------------------------------------------------------------------------
//
// setTile(gx, gy, h) — the tile-height edit path (a terrain edit —
// M2-TILE-01's per-tile height write): stores h and recomputes the
// keys of the AFFECTED CELLS only — the edited cell plus its
// documented neighborhood, radius kIsoDepthTableUpdateRadius:
//
//   the radius is 0 for the M2-ISO-01 key formula: a cell's key is a
//   function of the cell's OWN (x, y, height, layer) alone (the
//   formula has no cross-cell coupling), and the table stores each
//   cell's own height — so editing one tile changes exactly that
//   tile's key.
//
// Each affected cell is recomputed from its OWN stored height (the
// edited cell from the new one), so the neighborhood loop is correct
// for any radius a future formula may need; widening the named
// constant (e.g. a blended standing-surface formula) changes no API.
// The radius is a named constant, not a knob: no caller chooses it
// (CORE-004, CORE-005).
//
// setTile is O(1), zero-allocation, branch-light, and GL-free — a
// sim-phase call (the threading contract below); the 10k-dirty-cell
// budget is measured over it (docs/benchmarks/baselines/
// m2-iso-depth-table.md).
//
// rebuild(heights) — the from-scratch path (scene load): stores the
// whole height grid and recomputes EVERY key through the full
// M2-ISO-01 function (independent of the stored qBase). Property:
// rebuild(final grid) == any sequence of setTile calls reaching the
// same grid — the property test pins it (the roadmap's "rebuild-from-
// scratch == incremental result").
//
// ensureChunk(gx, gy) — the growth path (setup; the streamed-world
// case, SCALE-001): extends the covered region to include the chunk
// containing the tile (the region stays a chunk-aligned rectangle).
// Growth is BOUNDED by Options::maxChunks (exceeding it returns
// BudgetExhausted) and LOGGED (one Debug event per created chunk, one
// rate-limited Warn on the cap) — the roadmap's "growth bounded +
// logged".
//
// ---------------------------------------------------------------------------
// The qBase derivation (why setTile is bit-identical to isoDepthKey)
// ---------------------------------------------------------------------------
//
// isoDepthKey computes:
//
//   s   = M::add(x, y)                        (one backend add)
//   q   = quantize(s)                         (one rounding)
//   d   = clamp(q - h * 16, -2^21, 2^21 - 1)
//   key = (l + 512) << 22 | (d + 2^21)
//
// The table stores q = qBase (the same backend add + quantize, at
// table creation) and pre-packs the layer field (the layer is fixed
// per table and validated into the 10-bit field at create). setTile
// then applies only d = qBase - h * 16 + the OR — the exact remaining
// terms of the formula, with no backend arithmetic and no clamp (in
// the validated domain the clamp is provably inactive: |qBase| <=
// 16 * 65534 and |h| <= 2047, so |d| <= 1081280 < 2^21, and int32
// arithmetic cannot overflow — the derivation in the setTile body).
// h is integral, so 16 * h needs no rounding. Bit-identity follows
// (the tests pin it, including the rebuild-vs-incremental property).
//
// ---------------------------------------------------------------------------
// Domain (CORE-005)
// ---------------------------------------------------------------------------
//
//   tile coords  kIsoDepthTableMinTileCoord <= gx, gy <=
//                kIsoDepthTableMaxTileCoord   (-32767 .. 32766: every
//                tile CENTER (gx + 0.5) then lies in the depth key
//                domain |x|, |y| <= kIsoDepthMaxWorldUnits — the +0.5
//                center offset shrinks the positive bound by one)
//   heights      |h| <= kIsoDepthMaxStepHeight (2047) — the key domain
//   layer        |l| <= kIsoDepthLayerMax (511) — the key domain
//   chunkTiles   a power of two >= 1 (chunk coordinates stay
//                shift/mask — division-free — on the cold paths;
//                growth granularity)
//
// create() validates the options (first failure wins, InvalidArgument,
// the documented order in the function); setTile/rebuild validate the
// cell/heights at the boundary (InvalidArgument) — a rejected call
// leaves the table unchanged, and the returned Status is the failure
// channel the caller handles and logs (LOG-002: no duplicate engine
// log per call).
//
// ---------------------------------------------------------------------------
// Ownership, threading, determinism (CORE-009, CONC-001, ADR 0002)
// ---------------------------------------------------------------------------
//
// One owner: the sim/scene-owner thread. Writes (setTile, rebuild,
// ensureChunk) happen in the simulation phase (a terrain edit is a
// sim-side command — M2-TILE-01); reads (keyAt, covers, tileHeightAt)
// happen in the render phase (the tile batch path — M2-TILE-01 /
// M2-SPRITE-01). The phases never overlap (the frame pipeline's
// tick -> handoff -> render ordering, M2-GL-02), so no synchronization
// is needed; the table is never shared with a concurrent writer.
//
// Presentation-only (ARCH-009): the contents are a deterministic
// function of (options, edit sequence, backend) — presentation state,
// never authoritative sim state, replay state, or the sim state hash.
// Per the backend's ADR 0002 scope: fpx16_16 bit-exact everywhere;
// fp32_pinned bit-exact per build/ISA. Grid-locked cells agree across
// backends (dyadic centers in the exactness zone) — the tests pin it.
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003/004, DOC-004)
// ---------------------------------------------------------------------------
//
//   create / ensureChunk / rebuild   setup paths: one flat allocation
//                                    for the covered region at creation
//                                    (ensureChunk growth re-allocates
//                                    once + moves the old records);
//                                    rebuild is O(covered cells)
//                                    backend work (the from-scratch
//                                    contract).
//   setTile                          O(1): a boundary check, one flat
//                                    index, one pack; zero allocation,
//                                    no logging on success, no GL.
//                                    Budget: 10k dirty cells <= 0.2 ms
//                                    mean (PRD §8.1).
//   keyAt / covers / tileHeightAt    O(1) compares + index; no
//                                    allocation.
//
// Misuse warnings:
//
//   - keyAt / tileHeightAt on a cell the table does not cover is
//     undefined behavior (a program bug): check covers() when the
//     tile coordinate comes from untrusted input. The batch path
//     reads only covered tiles by construction (it culls against the
//     covered region). No per-call check or assert: the render read
//     path is allocation- and branch-light by contract (PERF-002/006,
//     the isoDepthKey total-within-domain pattern).
//   - Do not rebuild the table per frame to "refresh" keys — that is
//     the FR-2.2 anti-pattern; the table IS the precomputation.
//     Per-frame keys for MOVING sprites come from isoDepthKey
//     (M2-ISO-01), not from this table (which holds the static tile
//     grid).
//   - Pass the tile's own height to setTile (its standing-surface
//     elevation), not the sprite's top.
//   - rebuild's span is the COVERED rectangle (the chunk-aligned
//     superset), row-major, tileX fastest — coveredCellCount() cells.
//
// Canonical narrative: docs/concepts/coordinates.md §4.5 (ARCH-008);
// API contract: docs/api/iso_depth_table.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/render/iso_depth_key.h"
#include "laige/result.h"
#include "laige/sim_math.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The default chunk side in tiles: a square chunk holds
// chunkTiles^2 cells. A power of two (chunk coordinates are shift/mask
// — division-free — on the cold paths); 16 x 16 = 256 tiles is the
// standard tilemap chunk size (M2-TILE-01).
inline constexpr std::int32_t kIsoDepthTableChunkTiles = 16;

// The default growth cap: the most chunks a table may ever own
// (initial grid + streamed extensions). 1024 chunks x 256 cells x
// 12 B (CellRecord) ~ 3 MiB worst case — bounded and documented
// (CORE-006, API-006).
inline constexpr std::int32_t kIsoDepthTableDefaultMaxChunks = 1024;

// The documented update neighborhood radius (the "Incremental update
// contract" preamble): 0 for the M2-ISO-01 key formula — a cell's key
// is a function of the cell's own (x, y, height, layer) and the table
// stores each cell's own height, so a height edit affects exactly the
// edited cell.
inline constexpr std::int32_t kIsoDepthTableUpdateRadius = 0;

// Tile-coordinate bounds such that every tile center (gx + 0.5,
// gy + 0.5) lies in the depth key domain |x|, |y| <=
// kIsoDepthMaxWorldUnits (32767): gx + 0.5 <= 32767 => gx <= 32766;
// gx + 0.5 >= -32767 => gx >= -32767. The +0.5 center offset makes the
// bound one smaller on the positive side.
inline constexpr std::int32_t kIsoDepthTableMinTileCoord = -32767;
inline constexpr std::int32_t kIsoDepthTableMaxTileCoord = 32766;

namespace detail {

// The tile-center world coordinate of tile g (g + 0.5 world units) in
// the backend scalar. Exact in both backends: dyadic and |g + 0.5| <
// 2^23 (fpx16_16's fromFloat is exact for dyadic values — fpx16_16.h;
// fp32 represents half-integers to 2^23 exactly). The tests pin the
// cross-backend agreement.
template <typename Backend>
struct IsoTableCenter {
  using Scalar = typename sim::SimMath<Backend>::Scalar;
  static Scalar make(std::int32_t tileCoord) noexcept;
};

template <>
struct IsoTableCenter<sim::Fp32Pinned> {
  static float make(std::int32_t g) noexcept {
    return static_cast<float>(g) + 0.5f;
  }
};

template <>
struct IsoTableCenter<sim::Fpx16_16> {
  static fpx16_16 make(std::int32_t g) noexcept {
    return fpx16_16::fromFloat(static_cast<float>(g) + 0.5f);
  }
};

// floor(tile / 2^shift), tile possibly negative, shift >= 0 —
// division-free (the chunk side is a power of two). CPP-004: no
// implementation-defined right shift of a negative value; the negative
// branch rounds (-tile + 2^shift - 1) toward zero and negates (|tile|
// <= 32767 in the table domain, so -tile cannot overflow).
constexpr std::int32_t chunkCoord(std::int32_t tile, std::int32_t shift)
    noexcept {
  if (tile >= 0) return tile >> shift;
  return -((static_cast<std::uint32_t>(-tile) +
            static_cast<std::uint32_t>(1u << shift) - 1u) >> shift);
}

// log2 of a power of two (chunkTiles is validated a power of two >= 1).
constexpr std::int32_t chunkShiftOf(std::int32_t chunkTiles) noexcept {
  std::int32_t shift = 0;
  while ((1 << shift) < chunkTiles) ++shift;
  return shift;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The per-scene-chunk isometric depth key table (M2-ISO-02)
// ---------------------------------------------------------------------------

// One table per scene region / tile layer: the precomputed
// tile-grid -> depth-key map with incremental updates (the model, the
// update contract, the domain, the ownership/threading/determinism,
// and the performance contract: the header preamble).
//
// Move-only (CORE-009): the table owns its flat cell storage; it is
// created by create() and handed to its owner (the scene/tilemap).
template <typename Backend>
class IsoDepthKeyTable {
  using M = sim::SimMath<Backend>;
  using Scalar = typename M::Scalar;
  using Vec2 = typename M::Vec2;

 public:
  // The construction options. create() validates (first failure wins,
  // InvalidArgument — the documented order: grid extents, chunkTiles,
  // maxChunks >= 1, layer domain, tile domain, grid-vs-cap).
  struct Options {
    // The requested tile grid (scene region): the tiles
    // [originTileX, originTileX + widthTiles) x
    // [originTileY, originTileY + heightTiles). The covered region is
    // the chunk-aligned superset of it (preamble).
    std::int32_t originTileX = 0;
    std::int32_t originTileY = 0;
    std::int32_t widthTiles = 0;    // required >= 1
    std::int32_t heightTiles = 0;   // required >= 1
    // The chunk side in tiles (a power of two >= 1).
    std::int32_t chunkTiles = kIsoDepthTableChunkTiles;
    // The growth cap: the most chunks the table may ever own (initial
    // grid + streamed extensions); required >= the initial grid's
    // chunk count.
    std::int32_t maxChunks = kIsoDepthTableDefaultMaxChunks;
    // The render layer stamped into every cell key (the ground layer
    // by default; a parallax tile LAYER gets its own table with its
    // layer value — M2-PAR-01).
    std::int32_t layer = kIsoDepthGroundLayer;
  };

  // Creates the table: validates the options, pre-sizes the flat
  // storage of the covered region (one allocation), and initializes
  // every cell to flat ground (height 0 — keys from qBase alone).
  // Setup path (allocating; not for the per-frame path). One Info
  // event (render/iso_depth_table_created) + one Debug event per chunk
  // (render/iso_depth_table_chunk_created).
  [[nodiscard]] static Result<IsoDepthKeyTable> create(const Options& options)
      noexcept;

  // The from-scratch path (scene load): stores the whole height grid
  // and recomputes EVERY key through the full M2-ISO-01 function
  // (independent of the stored qBase). `heights` is the covered
  // rectangle (coveredCellCount() cells), row-major, tileX fastest.
  // Setup path; O(covered cells) backend work; no allocation.
  //
  // Fails (InvalidArgument, table unchanged) when the span size does
  // not match the covered cell count or any height is outside the key
  // domain.
  [[nodiscard]] Status rebuild(std::span<const std::int32_t> heights) noexcept;

  // The incremental update path (FR-2.2): stores the new tile height
  // and recomputes the keys of the affected cells only — the edited
  // cell plus its documented neighborhood (radius 0 for the current
  // key formula — the preamble). O(1); zero allocation; no logging on
  // success; no GL. A sim-phase call (the ownership preamble).
  //
  // Fails (InvalidArgument, table unchanged) when the cell is not
  // covered (use ensureChunk for streamed regions) or the height is
  // outside the key domain.
  //
  // Budget-critical (the PRD §8.1 10k-dirty-cell gate): the body is
  // FLAT — no helper calls and no container calls (they would each be
  // a real function call on the -O0 trees the gate is measured on —
  // methodology §4). The coverage, flat-index, and pack steps below
  // are the same expressions the helpers would compute (the tests pin
  // the equivalence: setTile's keys equal isoDepthKey's, and it
  // rejects the same cells covers() rejects).
  [[nodiscard]] Status setTile(std::int32_t tileX, std::int32_t tileY,
                               std::int32_t height) noexcept;

  // The growth path (setup; streamed regions — SCALE-001): extends the
  // covered region to include the chunk containing the tile (the
  // region stays a chunk-aligned rectangle). Bounded by
  // Options::maxChunks (exceeding it: BudgetExhausted + one rate-
  // limited Warn event, render/iso_depth_table_growth_cap) and logged
  // (one Debug event per created chunk). No-op success when the tile
  // is already covered. Fails (InvalidArgument) when the chunk's
  // extreme tile centers would leave the key domain.
  [[nodiscard]] Status ensureChunk(std::int32_t tileX, std::int32_t tileY)
      noexcept;

  // The render read path (O(1), no allocation; a render-phase call):
  // the current depth key of the cell. Precondition: covers() — a
  // read outside the covered region is undefined behavior (a program
  // bug; the preamble's misuse warnings).
  [[nodiscard]] std::uint32_t keyAt(std::int32_t tileX, std::int32_t tileY)
      const noexcept;

  // The last tile height set on the cell (diagnostics view — DBG-008).
  // Same precondition and cost as keyAt.
  [[nodiscard]] std::int32_t tileHeightAt(std::int32_t tileX,
                                          std::int32_t tileY) const noexcept;

  // True when the cell lies in the covered region (O(1)).
  [[nodiscard]] bool covers(std::int32_t tileX, std::int32_t tileY) const
      noexcept;

  // Introspection (O(1)).
  [[nodiscard]] std::int32_t layer() const noexcept { return options_.layer; }
  [[nodiscard]] std::int32_t chunkTiles() const noexcept {
    return options_.chunkTiles;
  }
  // The requested grid (the Options' values).
  [[nodiscard]] std::int32_t originTileX() const noexcept {
    return options_.originTileX;
  }
  [[nodiscard]] std::int32_t originTileY() const noexcept {
    return options_.originTileY;
  }
  [[nodiscard]] std::int32_t widthTiles() const noexcept {
    return options_.widthTiles;
  }
  [[nodiscard]] std::int32_t heightTiles() const noexcept {
    return options_.heightTiles;
  }
  // The covered (chunk-aligned) tile rectangle: [minX, maxX) x
  // [minY, maxY).
  [[nodiscard]] std::int32_t coveredTileMinX() const noexcept {
    return covMinX_;
  }
  [[nodiscard]] std::int32_t coveredTileMinY() const noexcept {
    return covMinY_;
  }
  [[nodiscard]] std::int32_t coveredTileMaxX() const noexcept {
    return covMaxX_;
  }
  [[nodiscard]] std::int32_t coveredTileMaxY() const noexcept {
    return covMaxY_;
  }
  [[nodiscard]] std::size_t chunkCount() const noexcept {
    return static_cast<std::size_t>(covWidth_ / options_.chunkTiles) *
           static_cast<std::size_t>(
               (covMaxY_ - covMinY_) / options_.chunkTiles);
  }
  [[nodiscard]] std::size_t maxChunks() const noexcept {
    return static_cast<std::size_t>(maxChunks_);
  }
  [[nodiscard]] std::size_t coveredCellCount() const noexcept {
    return static_cast<std::size_t>(covWidth_) *
           static_cast<std::size_t>(covMaxY_ - covMinY_);
  }

  IsoDepthKeyTable(const IsoDepthKeyTable&) = delete;
  IsoDepthKeyTable& operator=(const IsoDepthKeyTable&) = delete;
  // Explicit move (the raw cellBase_ member makes the defaulted move
  // unsafe for the moved-from object — it would dangle into the
  // destination): the owner moves with the storage; the source
  // becomes empty (cells_ null, cellBase_ null — CORE-009).
  IsoDepthKeyTable(IsoDepthKeyTable&& other) noexcept {
    *this = std::move(other);
  }
  IsoDepthKeyTable& operator=(IsoDepthKeyTable&& other) noexcept {
    if (this != &other) {
      options_ = std::move(other.options_);
      layerField_ = other.layerField_;
      chunkShift_ = other.chunkShift_;
      covMinX_ = other.covMinX_;
      covMinY_ = other.covMinY_;
      covMaxX_ = other.covMaxX_;
      covMaxY_ = other.covMaxY_;
      covWidth_ = other.covWidth_;
      maxChunks_ = other.maxChunks_;
      cellBase_ = other.cellBase_;
      cellCount_ = other.cellCount_;
      cells_ = std::move(other.cells_);
      other.cellBase_ = nullptr;
    }
    return *this;
  }

 private:
  // One table cell: the current key, the cell's quantized (x + y) base,
  // and the tile height the key was last computed from (the update
  // path recomputes affected cells from their OWN stored heights).
  struct CellRecord {
    std::uint32_t key{};
    std::int32_t qBase{};
    std::int32_t height{};
  };

  // Constructed by create() only (the options are validated there; the
  // layer is in-domain, so the pack below is exact — no clamp).
  explicit IsoDepthKeyTable(Options options) noexcept
      : options_(options) {
    layerField_ = (static_cast<std::uint32_t>(
                       static_cast<std::uint32_t>(options_.layer) +
                       kIsoDepthLayerBias)
                   << kIsoDepthFineBits);
  }

  // Flat index of a covered cell: row-major over the given rectangle,
  // tileX fastest (the documented rebuild-span order).
  // Precondition: covers() (the caller guarantees — the read path is
  // check-free by contract, the isoDepthKey pattern).
  std::size_t cellIndex(std::int32_t tileX, std::int32_t tileY) const
      noexcept {
    return static_cast<std::size_t>(tileY - covMinY_) *
               static_cast<std::size_t>(covWidth_) +
           static_cast<std::size_t>(tileX - covMinX_);
  }

  // Flat index over an explicit rectangle (the ensureChunk growth
  // walk — cold; the member-based cellIndex above serves the hot
  // paths).
  static std::size_t flatIndex(std::int32_t tileX, std::int32_t tileY,
                               std::int32_t minX, std::int32_t minY,
                               std::int32_t width) noexcept {
    return static_cast<std::size_t>(tileY - minY) *
               static_cast<std::size_t>(width) +
           static_cast<std::size_t>(tileX - minX);
  }

  // Packs (the stored layer field, a cell's qBase, a height) into the
  // 32-bit key — the exact remainder of isoDepthKey's formula (the
  // preamble derivation): d = clamp(q - 16*h, -2^21, 2^21-1).
  std::uint32_t packKey(std::int32_t qBase, std::int32_t height) const
      noexcept {
    const std::int64_t d = detail::clampInt64(
        static_cast<std::int64_t>(qBase) -
            static_cast<std::int64_t>(height) * kIsoDepthQuantScale,
        -static_cast<std::int64_t>(kIsoDepthFineBias),
        static_cast<std::int64_t>(kIsoDepthFineBias) - 1);
    return layerField_ |
           static_cast<std::uint32_t>(static_cast<std::uint64_t>(d) +
                                      kIsoDepthFineBias);
  }

  // Initializes one cell to flat ground (height 0): its qBase through
  // the backend add + quantize (the M2-ISO-01 sum path —
  // detail::IsoDepthSumQuant) and its key from qBase alone (packKey).
  // Cold path (create / ensureChunk only).
  void fillFlatCell(CellRecord& rec, std::int32_t tileX,
                    std::int32_t tileY) noexcept {
    rec.height = 0;  // flat ground until the scene sets a height
    const Vec2 center{detail::IsoTableCenter<Backend>::make(tileX),
                      detail::IsoTableCenter<Backend>::make(tileY)};
    const Scalar s = M::add(center.x, center.y);
    rec.qBase = static_cast<std::int32_t>(
        detail::IsoDepthSumQuant<Backend>::quantize(s));
    rec.key = packKey(rec.qBase, rec.height);
  }

  // One Debug event per created chunk (the cold logging path).
  static void logChunkCreated(std::int32_t cx, std::int32_t cy) noexcept {
    LAIGE_LOG_DEBUG("render", "iso_depth_table_chunk_created",
                    "iso depth key table chunk created",
                    laige::log::field("chunk_x", cx),
                    laige::log::field("chunk_y", cy));
  }

  Options options_;
  std::uint32_t layerField_{};  // pre-packed layer field (create-validated)
  std::int32_t chunkShift_{0};  // log2(chunkTiles) — cold paths only
  // The covered region: the chunk-aligned tile rectangle [covMinX_,
  // covMaxX_) x [covMinY_, covMaxY_). The precomputed bounds keep the
  // update path compare-only (no arithmetic in the coverage test):
  std::int32_t covMinX_{0};
  std::int32_t covMinY_{0};
  std::int32_t covMaxX_{0};
  std::int32_t covMaxY_{0};
  std::int32_t covWidth_{0};  // covMaxX_ - covMinX_ (the flat stride)
  std::int32_t maxChunks_{0};
  // The flat cell storage (row-major, tileX fastest) — one allocation
  // at creation; ensureChunk growth re-allocates once (setup):
  std::unique_ptr<CellRecord[]> cells_;
  std::size_t cellCount_{0};
  // cellBase_ == cells_.get() — cached so the hot path (setTile) reads
  // a raw pointer instead of going through unique_ptr::operator[],
  // which is a six-level call chain (get -> _M_ptr -> get ->
  // __get_helper -> _M_head -> _Head_base::_M_head, plus a
  // glibcxx assert) on the -O0 trees the budget gate is measured on
  // (methodology §4). Kept in sync by create() and ensureChunk();
  // nulled on the moved-from object (the explicit move below).
  CellRecord* cellBase_{nullptr};
};

// ---------------------------------------------------------------------------
// Implementation (the class is header-only — template over the
// SimMath backends, the PresentationSnapshot pattern)
// ---------------------------------------------------------------------------

template <typename Backend>
Result<IsoDepthKeyTable<Backend>>
IsoDepthKeyTable<Backend>::create(const Options& options) noexcept {
  // Option validation — first failure wins (the documented order):
  if (options.widthTiles < 1 || options.heightTiles < 1) {
    return Result<IsoDepthKeyTable>::failure(ErrorCode::InvalidArgument);
  }
  if (options.chunkTiles < 1 ||
      (options.chunkTiles & (options.chunkTiles - 1)) != 0) {
    return Result<IsoDepthKeyTable>::failure(ErrorCode::InvalidArgument);
  }
  if (options.maxChunks < 1) {
    return Result<IsoDepthKeyTable>::failure(ErrorCode::InvalidArgument);
  }
  if (options.layer < -static_cast<std::int32_t>(kIsoDepthLayerBias) ||
      options.layer > kIsoDepthLayerMax) {
    return Result<IsoDepthKeyTable>::failure(ErrorCode::InvalidArgument);
  }
  // The grid's extreme tile centers must lie in the key domain:
  // originTile + widthTiles - 1 <= kIsoDepthTableMaxTileCoord and
  // originTile >= kIsoDepthTableMinTileCoord (the constants' preamble).
  if (options.originTileX < kIsoDepthTableMinTileCoord ||
      options.originTileY < kIsoDepthTableMinTileCoord ||
      options.originTileX + options.widthTiles >
          kIsoDepthTableMaxTileCoord + 1 ||
      options.originTileY + options.heightTiles >
          kIsoDepthTableMaxTileCoord + 1) {
    return Result<IsoDepthKeyTable>::failure(ErrorCode::InvalidArgument);
  }
  const std::int32_t shift = detail::chunkShiftOf(options.chunkTiles);
  const std::int32_t nX0 =
      (options.widthTiles + options.chunkTiles - 1) >> shift;  // ceil
  const std::int32_t nY0 =
      (options.heightTiles + options.chunkTiles - 1) >> shift;  // ceil
  // The requested grid must fit the cap (a misconfiguration, not a
  // runtime growth):
  if (static_cast<std::int64_t>(nX0) * static_cast<std::int64_t>(nY0) >
      static_cast<std::int64_t>(options.maxChunks)) {
    return Result<IsoDepthKeyTable>::failure(ErrorCode::InvalidArgument);
  }

  IsoDepthKeyTable table(options);
  table.chunkShift_ = shift;
  table.maxChunks_ = options.maxChunks;
  // The covered region: the chunk-aligned superset of the requested
  // grid (the preamble model):
  table.covMinX_ = detail::chunkCoord(options.originTileX, shift) *
                   options.chunkTiles;
  table.covMinY_ = detail::chunkCoord(options.originTileY, shift) *
                   options.chunkTiles;
  table.covMaxX_ = (detail::chunkCoord(
                        options.originTileX + options.widthTiles - 1, shift) +
                    1) *
                   options.chunkTiles;
  table.covMaxY_ = (detail::chunkCoord(
                        options.originTileY + options.heightTiles - 1, shift) +
                    1) *
                   options.chunkTiles;
  table.covWidth_ = table.covMaxX_ - table.covMinX_;
  table.cellCount_ = static_cast<std::size_t>(table.covWidth_) *
                     static_cast<std::size_t>(table.covMaxY_ -
                                              table.covMinY_);
  table.cells_ = std::make_unique<CellRecord[]>(table.cellCount_);
  table.cellBase_ = table.cells_.get();
  // Flat-ground init (setup path): every cell's qBase through the
  // backend add + quantize, its key from qBase alone (height 0):
  for (std::int32_t ty = table.covMinY_; ty < table.covMaxY_; ++ty) {
    for (std::int32_t tx = table.covMinX_; tx < table.covMaxX_; ++tx) {
      table.fillFlatCell(table.cellBase_[table.cellIndex(tx, ty)], tx, ty);
    }
  }
  // One Debug event per chunk (the chunk-aligned rectangle's chunks):
  const std::int32_t baseCX = table.covMinX_ / options.chunkTiles;
  const std::int32_t baseCY = table.covMinY_ / options.chunkTiles;
  const std::int32_t nX =
      (table.covMaxX_ - table.covMinX_) / options.chunkTiles;
  const std::int32_t nY =
      (table.covMaxY_ - table.covMinY_) / options.chunkTiles;
  for (std::int32_t cy = 0; cy < nY; ++cy) {
    for (std::int32_t cx = 0; cx < nX; ++cx) {
      table.logChunkCreated(baseCX + cx, baseCY + cy);
    }
  }

  LAIGE_LOG_INFO("render", "iso_depth_table_created",
                 "iso depth key table created",
                 laige::log::field("chunks", static_cast<std::uint32_t>(nX) *
                                                  nY),
                 laige::log::field("cells",
                                   static_cast<std::uint32_t>(
                                       table.coveredCellCount())),
                 laige::log::field("chunk_tiles", options.chunkTiles),
                 laige::log::field("layer", options.layer));
  return Result<IsoDepthKeyTable>::success(std::move(table));
}

template <typename Backend>
Status IsoDepthKeyTable<Backend>::rebuild(
    std::span<const std::int32_t> heights) noexcept {
  const std::size_t cells = coveredCellCount();
  if (heights.size() != cells) {
    return Status(ErrorCode::InvalidArgument);
  }
  for (std::size_t i = 0; i < cells; ++i) {
    if (heights[i] < -kIsoDepthMaxStepHeight ||
        heights[i] > kIsoDepthMaxStepHeight) {
      return Status(ErrorCode::InvalidArgument);
    }
  }
  // From scratch: every key through the full M2-ISO-01 function
  // (independent of the stored qBase — the rebuild contract; the
  // property test relies on this independence). Row-major over the
  // covered rectangle, tileX fastest (the documented span order).
  std::size_t i = 0;
  for (std::int32_t ty = covMinY_; ty < covMaxY_; ++ty) {
    for (std::int32_t tx = covMinX_; tx < covMaxX_; ++tx) {
      CellRecord& rec = cellBase_[cellIndex(tx, ty)];
      rec.height = heights[i];
      const Vec2 center{detail::IsoTableCenter<Backend>::make(tx),
                        detail::IsoTableCenter<Backend>::make(ty)};
      rec.key = isoDepthKey<Backend>(center, rec.height, options_.layer);
      ++i;
    }
  }
  return Status{};
}

template <typename Backend>
Status IsoDepthKeyTable<Backend>::setTile(std::int32_t tileX,
                                          std::int32_t tileY,
                                          std::int32_t height) noexcept {
  // Boundary validation (API-008; a rejected edit leaves the table
  // unchanged): coverage (the precomputed covered-rectangle bounds —
  // the same test as covers()) and the height domain, in one branch
  // sequence (both failures are InvalidArgument, so the order is not
  // observable):
  if (tileX < covMinX_ || tileX >= covMaxX_ ||
      tileY < covMinY_ || tileY >= covMaxY_ ||
      height < -kIsoDepthMaxStepHeight ||
      height > kIsoDepthMaxStepHeight) {
    return Status(ErrorCode::InvalidArgument);
  }
  // The edited cell (flat index: row-major over the covered
  // rectangle, tileX fastest; the raw pointer cache — the preamble's
  // performance note):
  const std::size_t i = static_cast<std::size_t>(tileY - covMinY_) *
                            static_cast<std::size_t>(covWidth_) +
                        static_cast<std::size_t>(tileX - covMinX_);
  CellRecord& rec = cellBase_[i];
  rec.height = height;
  // The new key (packKey's exact remaining terms, inlined, no clamp,
  // no backend arithmetic). Both inputs are boundary-validated
  // (height above; qBase at creation — the quantizer's in-domain
  // range |qBase| <= 16 * 65534 = 1048544), so
  // |qBase - 16 * height| <= 1048544 + 16 * 2047 = 1081280 < 2^21:
  // the isoDepthKey clamp is provably inactive in the validated
  // domain, int32 arithmetic cannot overflow (|16 * height| <=
  // 32752, CPP-004), and d + 2^21 stays in the 22-bit field.
  // h is integral, so 16 * h needs no rounding. Bit-identity to
  // isoDepthKey is pinned by the rebuild-vs-incremental property
  // test (including the boundary heights ±2047):
  rec.key = layerField_ | static_cast<std::uint32_t>(
                              static_cast<std::uint64_t>(
                                  rec.qBase -
                                  height * kIsoDepthQuantScale) +
                              kIsoDepthFineBias);
  // The remaining affected cells: the documented neighborhood of the
  // edited cell, minus the cell itself. For the M2-ISO-01 formula the
  // radius is 0 — the branch is never taken (the preamble contract). A
  // future formula with radius R recomputes each covered neighbor from
  // its OWN stored height (uncovered neighbors have no cell); that
  // path is cold (a terrain formula change), so it may use the helpers.
  if (kIsoDepthTableUpdateRadius > 0) {
    for (std::int32_t dy = -kIsoDepthTableUpdateRadius;
         dy <= kIsoDepthTableUpdateRadius; ++dy) {
      for (std::int32_t dx = -kIsoDepthTableUpdateRadius;
           dx <= kIsoDepthTableUpdateRadius; ++dx) {
        if (dx == 0 && dy == 0) continue;
        const std::int32_t ax = tileX + dx;
        const std::int32_t ay = tileY + dy;
        if (!covers(ax, ay)) continue;
        CellRecord& n = cellBase_[cellIndex(ax, ay)];
        n.key = packKey(n.qBase, n.height);
      }
    }
  }
  return Status{};
}

template <typename Backend>
Status IsoDepthKeyTable<Backend>::ensureChunk(std::int32_t tileX,
                                              std::int32_t tileY) noexcept {
  const std::int32_t cx = detail::chunkCoord(tileX, chunkShift_);
  const std::int32_t cy = detail::chunkCoord(tileY, chunkShift_);
  if (covers(tileX, tileY)) return Status{};  // already covered (idempotent)
  // The new chunk's extreme tile centers must stay in the key domain
  // (a future setTile on them must remain representable):
  const std::int32_t cMinX = cx * options_.chunkTiles;
  const std::int32_t cMaxX = cMinX + options_.chunkTiles - 1;
  const std::int32_t cMinY = cy * options_.chunkTiles;
  const std::int32_t cMaxY = cMinY + options_.chunkTiles - 1;
  if (cMinX < kIsoDepthTableMinTileCoord ||
      cMaxX > kIsoDepthTableMaxTileCoord ||
      cMinY < kIsoDepthTableMinTileCoord ||
      cMaxY > kIsoDepthTableMaxTileCoord) {
    return Status(ErrorCode::InvalidArgument);
  }
  // The covered region grows to the chunk-aligned rectangle containing
  // both the old rectangle and the requested chunk:
  const std::int32_t newMinX = (cMinX < covMinX_) ? cMinX : covMinX_;
  const std::int32_t newMinY = (cMinY < covMinY_) ? cMinY : covMinY_;
  const std::int32_t newMaxX =
      (cMinX + options_.chunkTiles > covMaxX_)
          ? cMinX + options_.chunkTiles
          : covMaxX_;
  const std::int32_t newMaxY =
      (cMinY + options_.chunkTiles > covMaxY_)
          ? cMinY + options_.chunkTiles
          : covMaxY_;
  const std::int32_t newWidth = newMaxX - newMinX;
  const std::size_t newChunkCount =
      static_cast<std::size_t>(newWidth / options_.chunkTiles) *
      static_cast<std::size_t>((newMaxY - newMinY) / options_.chunkTiles);
  if (newChunkCount > static_cast<std::size_t>(maxChunks_)) {
    LAIGE_LOG_WARN("render", "iso_depth_table_growth_cap",
                   "iso depth key table growth cap reached",
                   laige::log::field("requested_chunk_x", cx),
                   laige::log::field("requested_chunk_y", cy),
                   laige::log::field("chunk_count",
                                     static_cast<std::uint32_t>(
                                         chunkCount())),
                   laige::log::field("max_chunks",
                                     static_cast<std::uint32_t>(maxChunks_)));
    return Status(ErrorCode::BudgetExhausted);
  }
  // Grow the flat storage to the new rectangle (setup path only — a
  // streamed-region load): ONE allocation; each old record MOVES to
  // its new position (POD moves), each new cell is created flat-
  // ground. Bounded by maxChunks (checked above) and logged (one
  // Debug event per new chunk — logChunkCreated below).
  const std::size_t newCellCount =
      static_cast<std::size_t>(newWidth) *
      static_cast<std::size_t>(newMaxY - newMinY);
  auto grown = std::make_unique<CellRecord[]>(newCellCount);
  for (std::int32_t ty = newMinY; ty < newMaxY; ++ty) {
    for (std::int32_t tx = newMinX; tx < newMaxX; ++tx) {
      CellRecord& rec =
          grown[flatIndex(tx, ty, newMinX, newMinY, newWidth)];
      const bool inOld = tx >= covMinX_ && tx < covMaxX_ &&
                         ty >= covMinY_ && ty < covMaxY_;
      if (inOld) {
        rec = cellBase_[cellIndex(tx, ty)];
      } else {
        fillFlatCell(rec, tx, ty);
      }
    }
  }
  // One Debug event per NEW chunk (the old chunks were logged at
  // creation):
  const std::int32_t newBaseCX = newMinX / options_.chunkTiles;
  const std::int32_t newBaseCY = newMinY / options_.chunkTiles;
  const std::int32_t newCXCount = newWidth / options_.chunkTiles;
  const std::int32_t newCYCount =
      (newMaxY - newMinY) / options_.chunkTiles;
  for (std::int32_t cyy = 0; cyy < newCYCount; ++cyy) {
    for (std::int32_t cxx = 0; cxx < newCXCount; ++cxx) {
      const std::int32_t chX = newBaseCX + cxx;
      const std::int32_t chY = newBaseCY + cyy;
      const bool inOldChunk =
          chX * options_.chunkTiles >= covMinX_ &&
          chX * options_.chunkTiles < covMaxX_ &&
          chY * options_.chunkTiles >= covMinY_ &&
          chY * options_.chunkTiles < covMaxY_;
      if (!inOldChunk) logChunkCreated(chX, chY);
    }
  }
  cells_ = std::move(grown);
  cellBase_ = cells_.get();
  covMinX_ = newMinX;
  covMinY_ = newMinY;
  covMaxX_ = newMaxX;
  covMaxY_ = newMaxY;
  covWidth_ = newWidth;
  cellCount_ = newCellCount;
  return Status{};
}

template <typename Backend>
std::uint32_t
IsoDepthKeyTable<Backend>::keyAt(std::int32_t tileX, std::int32_t tileY)
    const noexcept {
  // Precondition: covers(tileX, tileY) — the read path is check-free by
  // contract (the preamble's misuse warnings; the isoDepthKey
  // total-within-domain pattern).
  return cellBase_[cellIndex(tileX, tileY)].key;
}

template <typename Backend>
std::int32_t IsoDepthKeyTable<Backend>::tileHeightAt(std::int32_t tileX,
                                                     std::int32_t tileY)
    const noexcept {
  return cellBase_[cellIndex(tileX, tileY)].height;
}

template <typename Backend>
bool IsoDepthKeyTable<Backend>::covers(std::int32_t tileX, std::int32_t tileY)
    const noexcept {
  return tileX >= covMinX_ && tileX < covMaxX_ &&
         tileY >= covMinY_ && tileY < covMaxY_;
}

}  // namespace laige::render
