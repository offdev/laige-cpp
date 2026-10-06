// laige-render tilemap tests (M2-TILE-01): the chunked tile grid data,
// the auto-depth wiring of the M2-ISO-02 depth key table, and the
// static tile-quad batch path into the sprite batcher, in
// laige/render/tilemap.h.
//
// Pure data + batcher bookkeeping — no GL context, no GL environment
// needed: every suite runs in every local tree and in CI. The goldens
// are HAND-COMPUTED from the documented formula (tile centers
// (gx + 0.5, gy + 0.5), the M2-ISO-01 key); the independent oracle is
// the M2-ISO-01 function on a tile center (the test builds its own
// centers — the iso_depth_table_tests pattern); the property test
// pins "rebuild-from-scratch == incremental result" through the
// tilemap; the zero-allocation window covers the per-frame declare
// loop (FR-2.2 "no per-frame allocation" — the sanitizer trees run
// the same workload shapes leak-free instead, methodology §4).
//
// No budget gate: the step's roadmap scope has no standalone
// budgets.json entry (the per-frame declare cost is part of the
// composite 50k render-CPU budget — M2-PERF-01).

#include "laige/render/iso_depth_key.h"
#include "laige/render/iso_depth_table.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/tilemap.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/sim_math.h"

namespace {

using laige::fpx16_16;
using laige::render::BlendMode;
using laige::render::SpriteBatcher;
using laige::render::TileData;
using laige::render::TileMap;
using laige::sim::Fp32Pinned;
using laige::sim::Fpx16_16;

// ---------------------------------------------------------------------------
// The independent oracle: the M2-ISO-01 function on a tile center —
// the tilemap's auto-depth keys must equal it cell by cell (bit-
// identity contract). The test builds its own centers (dyadic, exact
// on both backends) — never reusing the table's code.
// ---------------------------------------------------------------------------

template <typename Backend>
laige::sim::SimMath<Backend>::Vec2 tileCenter(std::int32_t gx,
                                              std::int32_t gy) {
  if constexpr (std::is_same_v<Backend, Fp32Pinned>) {
    return laige::sim::SimMathFp32::Vec2{static_cast<float>(gx) + 0.5f,
                                         static_cast<float>(gy) + 0.5f};
  } else {
    return laige::sim::SimMathFpx16::Vec2{
        fpx16_16::fromFloat(static_cast<float>(gx) + 0.5f),
        fpx16_16::fromFloat(static_cast<float>(gy) + 0.5f)};
  }
}

template <typename Backend>
std::uint32_t oracleKey(std::int32_t gx, std::int32_t gy, std::int32_t height,
                        std::int32_t layer = 0) {
  return laige::render::isoDepthKey<Backend>(tileCenter<Backend>(gx, gy),
                                             height, layer);
}

// ---------------------------------------------------------------------------
// Log capture (the render_thread_tests MemorySink pattern)
// ---------------------------------------------------------------------------

class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
  };

  void emit(const laige::log::LogRecord& record) override {
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

// Installs a fresh capture sink (heap-owned by the logger) with rate
// limiting OFF (the iso_depth_table_tests pattern).
MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

// The hand-computed golden scene (the roadmap's 4 x 4 chunk):
//
//   heights (row-major, tileX fastest; row = ty):
//   ty=0:  0 0 1 2
//   ty=1:  0 1 2 2
//   ty=2:  1 2 2 3
//   ty=3:  2 2 3 3
//
//   textures:
//   ty=0:  1 1 2 2
//   ty=1:  1 2 2 1
//   ty=2:  2 1 2 1
//   ty=3:  1 1 2 2
//
// Hand-computed keys (the layer-0 key = 0x80000000 | (d + 2^21), where
// d = 16 * (tx + ty + 1) - 16 * h — the centers (tx + 0.5, ty + 0.5)
// make the ground sum tx + ty + 1 exact, so q = 16 * (tx + ty + 1)):
//   idx  (tx, ty)  h   d    key
//   0   (0, 0)    0   16   0x80200010
//   1   (1, 0)    0   32   0x80200020
//   2   (2, 0)    1   32   0x80200020
//   3   (3, 0)    2   32   0x80200020
//   4   (0, 1)    0   32   0x80200020
//   5   (1, 1)    1   32   0x80200020
//   6   (2, 1)    2   32   0x80200020
//   7   (3, 1)    2   48   0x80200030
//   8   (0, 2)    1   32   0x80200020
//   9   (1, 2)    2   32   0x80200020
//   10  (2, 2)    2   48   0x80200030
//   11  (3, 2)    3   48   0x80200030
//   12  (0, 3)    2   32   0x80200020
//   13  (1, 3)    2   48   0x80200030
//   14  (2, 3)    3   48   0x80200030
//   15  (3, 3)    3   64   0x80200040
constexpr std::uint32_t kGoldenKeys[16] = {
    0x80200010, 0x80200020, 0x80200020, 0x80200020,
    0x80200020, 0x80200020, 0x80200020, 0x80200030,
    0x80200020, 0x80200020, 0x80200030, 0x80200030,
    0x80200020, 0x80200030, 0x80200030, 0x80200040};
constexpr std::int32_t kGoldenHeights[16] = {
    0, 0, 1, 2, 0, 1, 2, 2, 1, 2, 2, 3, 2, 2, 3, 3};
constexpr std::uint32_t kGoldenTextures[16] = {
    1, 1, 2, 2, 1, 2, 2, 1, 2, 1, 2, 1, 1, 1, 2, 2};

// The golden scene's 16 tiles as a TileData span (row-major, tileX
// fastest; animation id = the index).
std::vector<TileData> goldenTiles() {
  std::vector<TileData> tiles(16);
  for (std::size_t i = 0; i < 16; ++i) {
    tiles[i].textureId = kGoldenTextures[i];
    tiles[i].height = kGoldenHeights[i];
    tiles[i].animationId = static_cast<std::uint32_t>(i);
  }
  return tiles;
}

// The hand-computed expected instance sequences after the batcher's
// (key, declaration-position) stable sort: the golden keys above put
// the tiles into four screen rows (d = 16, 32, 48, 64); within a row
// the declaration order (row-major, tileX fastest) is the stable tie-
// break. The sorted sequence is {0, 1, 2, 3, 4, 5, 6, 8, 9, 12, 7,
// 10, 11, 13, 14, 15}; restricting it to each texture: atlas 1's
// instances (in sorted order): {0, 1, 4, 9, 12, 7, 11, 13}; atlas
// 2's: {2, 3, 5, 6, 8, 10, 14, 15}.
constexpr std::uint32_t kGroupOneInstances[8] = {0, 1, 4, 9, 12, 7, 11, 13};
constexpr std::uint32_t kGroupTwoInstances[8] = {2, 3, 5, 6, 8, 10, 14, 15};

laige::Status expectRejected(laige::Status s, laige::ErrorCode want) {
  EXPECT_TRUE(s.isError());
  EXPECT_EQ(s.error(), want);
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// TileMapCreate — the grid options and the flat/empty contract
// ---------------------------------------------------------------------------

template <typename Backend>
void defaultGrid() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;  // covered: the chunk-aligned 16 x 16 superset
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  // Introspection:
  EXPECT_EQ(m.originTileX(), 0);
  EXPECT_EQ(m.originTileY(), 0);
  EXPECT_EQ(m.widthTiles(), 4);
  EXPECT_EQ(m.heightTiles(), 4);
  EXPECT_EQ(m.layer(), 0);
  EXPECT_EQ(m.chunkTiles(), 16);
  EXPECT_EQ(m.tileCount(), 16u);
  // The requested grid (not the table's superset margin):
  EXPECT_TRUE(m.covers(0, 0));
  EXPECT_TRUE(m.covers(3, 3));
  EXPECT_FALSE(m.covers(4, 0));
  EXPECT_FALSE(m.covers(0, 4));
  EXPECT_FALSE(m.covers(-1, 0));
  EXPECT_FALSE(m.covers(0, -1));
  // Flat/empty init: every tile is {textureId 0, height 0, animation
  // id 0}; the flat-ground key (hand computation: center (0.5, 0.5),
  // q = 16, d = 16):
  const TileData t = m.tileAt(0, 0);
  EXPECT_EQ(t.textureId, 0u);
  EXPECT_EQ(t.height, 0);
  EXPECT_EQ(t.animationId, 0u);
  EXPECT_EQ(m.tileHeightAt(0, 0), 0);
  EXPECT_EQ(m.depthKeyAt(0, 0), 0x80200010u);
  EXPECT_EQ(m.depthKeyAt(0, 0), oracleKey<Backend>(0, 0, 0));
}

TEST(TileMapCreate, DefaultGrid) {
  defaultGrid<Fpx16_16>();
  defaultGrid<Fp32Pinned>();
}

TEST(TileMapCreate, NonZeroOrigin) {
  TileMap<Fpx16_16>::Options o;
  o.originTileX = 5;
  o.originTileY = -3;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  const TileMap<Fpx16_16>& m = *r.valueIfOk();
  // The requested grid [5, 9) x [-3, 1):
  EXPECT_TRUE(m.covers(5, -3));
  EXPECT_TRUE(m.covers(8, -3));
  EXPECT_TRUE(m.covers(8, 0));
  EXPECT_FALSE(m.covers(9, -3));
  EXPECT_FALSE(m.covers(5, -4));
  EXPECT_FALSE(m.covers(5, 1));
}

TEST(TileMapCreate, RejectsInvalidOptions) {
  auto expectRejected = [](laige::Result<TileMap<Fpx16_16>> r,
                           laige::ErrorCode want) {
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error(), want);
  };
  auto make = [](auto mutate) {
    TileMap<Fpx16_16>::Options o;
    o.widthTiles = 4;
    o.heightTiles = 4;
    mutate(o);
    return TileMap<Fpx16_16>::create(o);
  };
  expectRejected(
      make([](auto& o) { o.widthTiles = 0; }),
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.heightTiles = 0; }),
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.chunkTiles = 0; }),
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.chunkTiles = 3; }),  // not a power of two
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.maxChunks = 0; }),
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.layer = 512; }),     // above the layer domain
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.layer = -513; }),    // below the layer domain
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.originTileX = -32768; }),  // below the tile domain
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.originTileX = 32767; }),   // above the tile domain
      laige::ErrorCode::InvalidArgument);
  expectRejected(
      make([](auto& o) { o.widthTiles = 32767; o.originTileX = 1; }),
      laige::ErrorCode::InvalidArgument);  // max tile past the domain
  // The grid exceeds the chunk cap (17 x 17 needs 2 chunks at chunk 16
  // — a create-time misconfiguration, InvalidArgument; the runtime
  // growth cap is the BudgetExhausted, ensureChunk):
  expectRejected(
      make([](auto& o) { o.widthTiles = 17; o.heightTiles = 17; o.maxChunks = 1; }),
      laige::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// TileMapData — the per-tile data writes, the height store, the load
// ---------------------------------------------------------------------------

TEST(TileMapData, SetTileReadWrite) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // The edit: texture 7, height 3, animation 9:
  ASSERT_TRUE(m.setTile(2, 1, 7, 3, 9).ok());
  const TileData t = m.tileAt(2, 1);
  EXPECT_EQ(t.textureId, 7u);
  EXPECT_EQ(t.height, 3);
  EXPECT_EQ(t.animationId, 9u);
  EXPECT_EQ(m.tileHeightAt(2, 1), 3);
  // The auto-depth key: hand computation — center (2.5, 1.5), sum 4,
  // q = 64, d = 64 - 48 = 16:
  EXPECT_EQ(m.depthKeyAt(2, 1), 0x80200010u);
  EXPECT_EQ(m.depthKeyAt(2, 1), oracleKey<Fpx16_16>(2, 1, 3));
  // Every other tile is still flat:
  EXPECT_EQ(m.tileAt(0, 0), TileData{});
  EXPECT_EQ(m.tileHeightAt(0, 0), 0);
}

TEST(TileMapData, RejectedEditsLeaveNoState) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // A valid base edit:
  ASSERT_TRUE(m.setTile(1, 1, 5, 1, 0).ok());
  const TileData base = m.tileAt(1, 1);
  const std::uint32_t baseKey = m.depthKeyAt(1, 1);
  // Outside the REQUESTED grid (the superset-margin cells are table
  // cells, not tiles) — rejected, no state change:
  expectRejected(m.setTile(4, 0, 1, 1, 0),
                 laige::ErrorCode::InvalidArgument);
  expectRejected(m.setTile(0, 4, 1, 1, 0),
                 laige::ErrorCode::InvalidArgument);
  expectRejected(m.setTile(-1, 0, 1, 1, 0),
                 laige::ErrorCode::InvalidArgument);
  expectRejected(m.setTile(0, -1, 1, 1, 0),
                 laige::ErrorCode::InvalidArgument);
  // Out-of-domain heights (above/below the key domain):
  expectRejected(m.setTile(1, 1, 5, 2048, 0),
                 laige::ErrorCode::InvalidArgument);
  expectRejected(m.setTile(1, 1, 5, -2048, 0),
                 laige::ErrorCode::InvalidArgument);
  // The base edit is intact (the data AND the table):
  EXPECT_EQ(m.tileAt(1, 1), base);
  EXPECT_EQ(m.depthKeyAt(1, 1), baseKey);
  // Boundary heights inside the domain are accepted:
  ASSERT_TRUE(m.setTile(1, 1, 5, 2047, 0).ok());
  ASSERT_TRUE(m.setTile(1, 1, 5, -2047, 0).ok());
  ASSERT_TRUE(m.setTile(1, 1, 5, 1, 0).ok());  // back to the base height
  EXPECT_EQ(m.tileAt(1, 1), base);
  EXPECT_EQ(m.depthKeyAt(1, 1), baseKey);
}

TEST(TileMapData, RebuildSceneLoad) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  const std::vector<TileData> tiles = goldenTiles();
  ASSERT_TRUE(m.rebuild(tiles).ok());
  // Every tile's data and key (the hand-computed goldens + oracle):
  for (std::int32_t ty = 0; ty < 4; ++ty) {
    for (std::int32_t tx = 0; tx < 4; ++tx) {
      const std::size_t i = static_cast<std::size_t>(ty) * 4 + tx;
      const TileData t = m.tileAt(tx, ty);
      EXPECT_EQ(t.textureId, kGoldenTextures[i]) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(t.height, kGoldenHeights[i]) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(t.animationId, i) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(m.tileHeightAt(tx, ty), kGoldenHeights[i]) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(m.depthKeyAt(tx, ty), kGoldenKeys[i]) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(m.depthKeyAt(tx, ty), oracleKey<Fpx16_16>(tx, ty, kGoldenHeights[i]))
          << "tile (" << tx << ", " << ty << ")";
    }
  }
}

TEST(TileMapData, RebuildValidation) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // A valid base edit (the state a rejected load must not touch):
  ASSERT_TRUE(m.setTile(0, 0, 4, 2, 1).ok());
  const TileData base = m.tileAt(0, 0);
  const std::uint32_t baseKey = m.depthKeyAt(0, 0);
  // Wrong span sizes (the requested grid has 16 tiles):
  std::vector<TileData> shortSpan(15);
  expectRejected(m.rebuild(shortSpan), laige::ErrorCode::InvalidArgument);
  std::vector<TileData> longSpan(17);
  expectRejected(m.rebuild(longSpan), laige::ErrorCode::InvalidArgument);
  // An out-of-domain height anywhere in the span (tile 7 = 2048):
  std::vector<TileData> badHeight = goldenTiles();
  badHeight[7].height = 2048;
  expectRejected(m.rebuild(badHeight), laige::ErrorCode::InvalidArgument);
  std::vector<TileData> badHeightLow = goldenTiles();
  badHeightLow[7].height = -2048;
  expectRejected(m.rebuild(badHeightLow), laige::ErrorCode::InvalidArgument);
  // The base edit is intact after all the rejections:
  EXPECT_EQ(m.tileAt(0, 0), base);
  EXPECT_EQ(m.depthKeyAt(0, 0), baseKey);
}

template <typename Backend>
void rebuildEqualsIncremental() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 8;
  o.heightTiles = 8;
  // The "rebuild from scratch == incremental" property through the
  // tilemap: one tilemap loaded with rebuild, one built up tile by
  // tile with setTile — the same grid, the same keys (the M2-ISO-02
  // property; the heights are a deterministic function of position —
  // no PRNG substream needed):
  auto rA = Map::create(o);
  ASSERT_TRUE(rA.ok());
  auto a = std::move(rA).takeValue();
  auto rB = Map::create(o);
  ASSERT_TRUE(rB.ok());
  auto b = std::move(rB).takeValue();
  std::vector<TileData> tiles(64);
  for (std::int32_t ty = 0; ty < 8; ++ty) {
    for (std::int32_t tx = 0; tx < 8; ++tx) {
      const std::size_t i = static_cast<std::size_t>(ty) * 8 + tx;
      tiles[i].textureId = 10 + static_cast<std::uint32_t>(tx + ty);
      tiles[i].height = (3 * tx + 5 * ty) % 7;
      tiles[i].animationId = 100 + static_cast<std::uint32_t>(tx * 8 + ty);
      ASSERT_TRUE(b.setTile(tx, ty, tiles[i].textureId, tiles[i].height,
                            tiles[i].animationId).ok());
    }
  }
  ASSERT_TRUE(a.rebuild(tiles).ok());
  for (std::int32_t ty = 0; ty < 8; ++ty) {
    for (std::int32_t tx = 0; tx < 8; ++tx) {
      const std::size_t i = static_cast<std::size_t>(ty) * 8 + tx;
      EXPECT_EQ(a.tileAt(tx, ty), b.tileAt(tx, ty)) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(a.tileHeightAt(tx, ty), tiles[i].height) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(a.depthKeyAt(tx, ty), b.depthKeyAt(tx, ty)) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(a.depthKeyAt(tx, ty), oracleKey<Backend>(tx, ty, tiles[i].height))
          << "tile (" << tx << ", " << ty << ")";
    }
  }
}

TEST(TileMapData, RebuildEqualsIncremental) {
  rebuildEqualsIncremental<Fpx16_16>();
  rebuildEqualsIncremental<Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// TileMapAutoDepth — the height -> depth-table wiring (M2-ISO-02 path)
// ---------------------------------------------------------------------------

template <typename Backend>
void heightChangeUpdatesOnlyDocumentedCell() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 8;
  o.heightTiles = 8;
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // A deterministic base terrain (the update-test pattern):
  std::vector<TileData> tiles(64);
  for (std::int32_t ty = 0; ty < 8; ++ty) {
    for (std::int32_t tx = 0; tx < 8; ++tx) {
      const std::size_t i = static_cast<std::size_t>(ty) * 8 + tx;
      tiles[i].textureId = 1;
      tiles[i].height = (3 * tx + 5 * ty) % 4;
    }
  }
  ASSERT_TRUE(m.rebuild(tiles).ok());
  std::vector<std::uint32_t> before(64);
  for (std::int32_t ty = 0; ty < 8; ++ty) {
    for (std::int32_t tx = 0; tx < 8; ++tx) {
      before[static_cast<std::size_t>(ty) * 8 + tx] = m.depthKeyAt(tx, ty);
    }
  }
  // A single tile-height edit changes ONLY the documented cell: the
  // radius-0 neighborhood is the edited cell itself (the M2-ISO-02
  // contract through the tilemap). Hand computation: cell (3, 4):
  // center (3.5, 4.5), sum 8, q = 128, h = 2 -> d = 96 -> key
  // 0x80200060:
  ASSERT_TRUE(m.setTile(3, 4, 9, 2, 0).ok());
  EXPECT_EQ(m.depthKeyAt(3, 4), 0x80200060u);
  EXPECT_EQ(m.depthKeyAt(3, 4), oracleKey<Backend>(3, 4, 2));
  EXPECT_EQ(m.tileHeightAt(3, 4), 2);
  EXPECT_EQ(m.tileAt(3, 4).textureId, 9u);
  for (std::size_t i = 0; i < 64; ++i) {
    const std::int32_t tx = static_cast<std::int32_t>(i % 8);
    const std::int32_t ty = static_cast<std::int32_t>(i / 8);
    if (tx == 3 && ty == 4) {
      EXPECT_NE(m.depthKeyAt(tx, ty), before[i]);  // the edited cell changed
    } else {
      EXPECT_EQ(m.depthKeyAt(tx, ty), before[i])
          << "cell (" << tx << ", " << ty << ")";
    }
  }
  // Last write wins: setting the cell back to its base height
  // ((9 + 20) % 4 = 1) restores the original key:
  ASSERT_TRUE(m.setTile(3, 4, 9, 1, 0).ok());
  EXPECT_EQ(m.depthKeyAt(3, 4), before[4 * 8 + 3]);
}

TEST(TileMapAutoDepth, HeightChangeUpdatesOnlyDocumentedCell) {
  heightChangeUpdatesOnlyDocumentedCell<Fpx16_16>();
  heightChangeUpdatesOnlyDocumentedCell<Fp32Pinned>();
}

TEST(TileMapAutoDepth, CrossBackendKeyAgreement) {
  // The tile centers are dyadic and inside the exactness zone, so both
  // backends' keys are bit-equal on the same grid:
  std::vector<std::uint32_t> fpx16, fp32;
  for (std::int32_t ty = 0; ty < 4; ++ty) {
    for (std::int32_t tx = 0; tx < 4; ++tx) {
      const std::size_t i = static_cast<std::size_t>(ty) * 4 + tx;
      fpx16.push_back(oracleKey<Fpx16_16>(tx, ty, kGoldenHeights[i]));
      fp32.push_back(oracleKey<Fp32Pinned>(tx, ty, kGoldenHeights[i]));
    }
  }
  ASSERT_EQ(fpx16.size(), fp32.size());
  for (std::size_t i = 0; i < fpx16.size(); ++i) {
    EXPECT_EQ(fpx16[i], fp32[i]) << "cell " << i;
  }
}

// ---------------------------------------------------------------------------
// TileMapDeclareGolden — the tile quad positions + depth goldens
// (the roadmap's 4 x 4 chunk)
// ---------------------------------------------------------------------------

template <typename Backend>
void quadPositionsAndDepth() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  const std::vector<TileData> tiles = goldenTiles();
  ASSERT_TRUE(m.rebuild(tiles).ok());
  // The batcher: exactly the frame's 16 tiles (the frame budget is
  // tight on purpose — no overflow):
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(batcher.frameCount(), 16u);
  EXPECT_EQ(batcher.overrideCount(), 0u);  // no depthOverride (auto-depth)
  // Every declared quad: the hand-computed position + depth goldens
  // (the declaration order is row-major, tileX fastest — slot ==
  // declaration index on this first frame):
  for (std::int32_t ty = 0; ty < 4; ++ty) {
    for (std::int32_t tx = 0; tx < 4; ++tx) {
      const std::size_t i = static_cast<std::size_t>(ty) * 4 + tx;
      const laige::render::SpriteItem* item = batcher.get(
          static_cast<std::uint32_t>(i));
      ASSERT_NE(item, nullptr) << "tile (" << tx << ", " << ty << ")";
      // The tile's CENTER (the exact dyadic float):
      EXPECT_EQ(item->pos.x, static_cast<float>(tx) + 0.5f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->pos.y, static_cast<float>(ty) + 0.5f) << "tile (" << tx << ", " << ty << ")";
      // The AUTO-DEPTH key (the table's — the hand-computed golden,
      // the oracle, and the tilemap's read path all agree):
      EXPECT_EQ(item->depthKey, kGoldenKeys[i]) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->depthKey, oracleKey<Backend>(tx, ty, kGoldenHeights[i])) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->depthKey, m.depthKeyAt(tx, ty)) << "tile (" << tx << ", " << ty << ")";
      EXPECT_FALSE(item->depthOverride) << "tile (" << tx << ", " << ty << ")";
      // The fixed-frame quad (the default DeclareOptions):
      EXPECT_EQ(item->uv.u0, 0.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->uv.v0, 0.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->uv.u1, 1.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->uv.v1, 1.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->frameIndex, 0u) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->rotation, 0.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->scale.x, 1.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->scale.y, 1.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->tint.r, 1.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->tint.g, 1.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->tint.b, 1.0f) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->tint.a, 1.0f) << "tile (" << tx << ", " << ty << ")";
      // The tile's texture + the group-key fields:
      EXPECT_EQ(item->atlasId, kGoldenTextures[i]) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->materialId, 0u) << "tile (" << tx << ", " << ty << ")";
      EXPECT_EQ(item->blend, BlendMode::Alpha) << "tile (" << tx << ", " << ty << ")";
    }
  }
}

TEST(TileMapDeclareGolden, QuadPositionsAndDepth) {
  quadPositionsAndDepth<Fpx16_16>();
  quadPositionsAndDepth<Fp32Pinned>();
}

// The hand-computed grouping + determinism of the golden scene: two
// distinct texture ids -> exactly two (atlas, material, blend) groups
// (one draw call each, FR-2.1), ascending atlas order, the in-group
// instance order = the global (key, declaration-position) order
// RESTRICTED to the group (the M2-SORT-01 stable sort; RENDER-003).
template <typename Backend>
void groupingAndDeterminism() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  const std::vector<TileData> tiles = goldenTiles();
  ASSERT_TRUE(m.rebuild(tiles).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  auto checkFrame = [&batcher]() {
    ASSERT_EQ(batcher.batchCount(), 2u);
    auto batches = batcher.batches();
    // Ascending (atlas, material, blend) group order:
    EXPECT_EQ(batches[0].atlasId, 1u);
    EXPECT_EQ(batches[0].materialId, 0u);
    EXPECT_EQ(batches[0].blend, BlendMode::Alpha);
    EXPECT_EQ(batches[1].atlasId, 2u);
    // The hand-computed in-group instance sequences:
    ASSERT_EQ(batches[0].instances.size(), 8u);
    for (std::size_t i = 0; i < 8; ++i) {
      EXPECT_EQ(batches[0].instances[i], kGroupOneInstances[i]) << "instance " << i;
    }
    ASSERT_EQ(batches[1].instances.size(), 8u);
    for (std::size_t i = 0; i < 8; ++i) {
      EXPECT_EQ(batches[1].instances[i], kGroupTwoInstances[i]) << "instance " << i;
    }
  };
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  checkFrame();
  // Determinism: the second frame (same tile data) is bit-identical:
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  checkFrame();
  std::printf("tilemap-golden: frames=2 groups=2 instances=16 status=ok\n");
}

TEST(TileMapDeclareGolden, GroupingAndDeterminism) {
  groupingAndDeterminism<Fpx16_16>();
  groupingAndDeterminism<Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// TileMapDeclare — the frame protocol, the failure paths, the options
// ---------------------------------------------------------------------------

TEST(TileMapDeclare, WindowProtocol) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  // A built frame's window is closed: the declare is rejected and
  // NOTHING is declared:
  expectRejected(m.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{}),
                 laige::ErrorCode::InvalidArgument);
  batcher.beginFrame();
  EXPECT_EQ(batcher.frameCount(), 0u);
}

TEST(TileMapDeclare, StoppedBatcher) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  SpriteBatcher batcher;  // the default (stopped) batcher: capacity 0
  expectRejected(m.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{}),
                 laige::ErrorCode::BudgetExhausted);
  // The tilemap is unchanged (nothing was declared):
  EXPECT_EQ(m.tileAt(0, 0), TileData{});
}

TEST(TileMapDeclare, CustomOptions) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // One texture everywhere (one group):
  std::vector<TileData> tiles(16);
  for (auto& t : tiles) t.textureId = 3;
  ASSERT_TRUE(m.rebuild(tiles).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  TileMap<Fpx16_16>::DeclareOptions dopts;
  dopts.materialId = 7;
  dopts.blend = BlendMode::Additive;
  dopts.uv = laige::render::SpriteUvRect{0.25f, 0.25f, 0.75f, 0.75f};
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, dopts).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(batcher.frameCount(), 16u);
  ASSERT_EQ(batcher.batchCount(), 1u);
  const auto batch = batcher.batches()[0];
  EXPECT_EQ(batch.atlasId, 3u);
  EXPECT_EQ(batch.materialId, 7u);
  EXPECT_EQ(batch.blend, BlendMode::Additive);
  // Every item carries the options:
  for (std::uint32_t slot = 0; slot < 16; ++slot) {
    const laige::render::SpriteItem* item = batcher.get(slot);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->atlasId, 3u);
    EXPECT_EQ(item->materialId, 7u);
    EXPECT_EQ(item->blend, BlendMode::Additive);
    EXPECT_EQ(item->uv.u0, 0.25f);
    EXPECT_EQ(item->uv.v0, 0.25f);
    EXPECT_EQ(item->uv.u1, 0.75f);
    EXPECT_EQ(item->uv.v1, 0.75f);
  }
}

TEST(TileMapDeclare, NoLogsOnHappyPath) {
  MemorySink* sink = installCaptureSink();
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // The create's table events (iso_depth_table_created, the chunk
  // creation) are captured; the happy edit + declare paths log
  // nothing (the Status is the failure channel — LOG-002, and the
  // update paths are budget-critical — LOG-003):
  sink->entries.clear();
  for (std::int32_t ty = 0; ty < 4; ++ty) {
    for (std::int32_t tx = 0; tx < 4; ++tx) {
      ASSERT_TRUE(m.setTile(tx, ty, 1, (tx + ty) % 3, 0).ok());
    }
  }
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_EQ(sink->entries.size(), 0u)
      << "happy edit/declare path logged " << sink->entries.size()
      << " events (first: "
      << (sink->entries.empty() ? "n/a" : sink->entries[0].event) << ")";
  restoreLogger();
}

// ---------------------------------------------------------------------------
// TileMapZeroAlloc — the per-frame declare loop allocates nothing
// (FR-2.2)
// ---------------------------------------------------------------------------

template <typename Backend>
void declareLoopAllocatesNothing() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 16;
  o.heightTiles = 16;  // 256 tiles per frame (the chunk size)
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  std::vector<TileData> tiles(256);
  for (std::size_t i = 0; i < 256; ++i) {
    tiles[i].textureId = 1 + static_cast<std::uint32_t>(i % 4);
    tiles[i].height = static_cast<std::int32_t>((i * 3) % 5);
  }
  ASSERT_TRUE(m.rebuild(tiles).ok());  // setup (before the window)
  SpriteBatcher::Options bo;
  bo.maxSprites = 256;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  // Zero-allocation proof (where the watch is live — the non-
  // sanitizer trees; the sanitizer runtimes own operator new):
  // 1000 frames of the declare loop allocate nothing (the batcher and
  // the sorter storage are pre-allocated):
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    for (std::int32_t frame = 0; frame < 1000; ++frame) {
      batcher.beginFrame();
      auto s = m.declareTo(batcher, typename Map::DeclareOptions{});
      if (!s.ok()) return;
      auto b = batcher.build();
      if (!b.ok()) return;
    }
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "1000 frames of beginFrame/declareTo/build allocated "
        << reading.allocs << " heap blocks (first site: "
        << (void*)reading.firstSite << ")";
  }
}

TEST(TileMapZeroAlloc, DeclareLoopAllocatesNothing) {
  declareLoopAllocatesNothing<Fpx16_16>();
  declareLoopAllocatesNothing<Fp32Pinned>();
}
