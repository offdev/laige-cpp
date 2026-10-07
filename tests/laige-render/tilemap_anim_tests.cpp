// laige-render tilemap animation + parallax tile layer tests
// (M2-TILE-02): the data-driven per-tile frame cycle (the documented
// tick rate), the tilemap's declare path under a parallax Tilemap-
// source layer (the M2-PAR-01 hook — the worldOffset translation +
// the tilemap's own depth layer), and the declared quad goldens — in
// laige/render/tilemap.h.
//
// Pure data + batcher bookkeeping — no GL context, no GL environment
// needed: every suite runs in every local tree and in CI. The UV
// goldens are HAND-COMPUTED from the tight-sheet formula (M2-SPRITE-
// 03: frame (col, row) of a fw x fh frame grid normalized into the
// tight sheet); the frame sequence is hand-computed from the
// documented rate (frame = ticks / frameTicks mod frameCount); the
// parallax offsets are hand-computed from the pinned M2-PAR-01
// formula (factor * (p - center) + offset) at dyadic values; the
// depth-key goldens are hand-computed from the M2-ISO-01 formula
// ((l + 512) << 22 | (d + 2^21), d = 16 * (x + y) - 16 * h at the
// translated centers), and the independent oracle is the M2-ISO-01
// function itself on a world point (the tilemap test pattern — the
// test builds its own points, never reusing the tilemap's code). The
// zero-allocation window covers the per-tick advance + per-frame
// declare loop (FR-2.2 "no per-frame allocation" — the sanitizer
// trees run the same workload shapes leak-free instead,
// methodology §4).
//
// No budget gate: the step's roadmap scope has no standalone
// budgets.json entry (the per-frame declare cost is part of the
// composite 50k render-CPU budget — M2-PERF-01).

#include "laige/render/iso_depth_key.h"
#include "laige/render/parallax.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/tilemap.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#if !defined(_MSC_VER)
#include <dlfcn.h>  // dladdr (the alloc-site module/symbol, POSIX)
#endif
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
using laige::render::ParallaxLayerDef;
using laige::render::ParallaxLayers;
using laige::render::ParallaxSource;
using laige::render::SpriteBatcher;
using laige::render::SpriteItem;
using laige::render::SpriteUvRect;
using laige::render::TileAnimationDef;
using laige::render::TileData;
using laige::render::TileMap;
using laige::render::kParallaxDepthLayerBackground;
using laige::sim::Fp32Pinned;
using laige::sim::Fpx16_16;

// ---------------------------------------------------------------------------
// The independent oracle: the M2-ISO-01 function on a (possibly
// parallax-translated) world point — the tilemap's declared keys must
// equal it (the dyadic points are in the exactness zone of both
// backends — the bit-identity contract). The test converts the float
// point into the backend's Vec2 itself (never reusing the tilemap's
// conversion).
// ---------------------------------------------------------------------------

template <typename Backend>
laige::sim::SimMath<Backend>::Vec2 backendVec2(float x, float y) {
  using M = laige::sim::SimMath<Backend>;
  if constexpr (std::is_same_v<Backend, Fp32Pinned>) {
    return typename M::Vec2{x, y};
  } else {
    return typename M::Vec2{fpx16_16::fromFloat(x), fpx16_16::fromFloat(y)};
  }
}

template <typename Backend>
std::uint32_t oracleKeyAt(float x, float y, std::int32_t height,
                          std::int32_t layer) {
  return laige::render::isoDepthKey<Backend>(backendVec2<Backend>(x, y),
                                             height, layer);
}

// The hand-computed 4-frame UV goldens (the 2 x 2 grid of 16 x 16
// frames, tight sheet 32 x 32 — the M2-SPRITE-03 formula, frame 0 at
// the top-left):
//   frame 0: (0, 0, 0.5, 0.5)
//   frame 1: (0.5, 0, 1, 0.5)
//   frame 2: (0, 0.5, 0.5, 1)
//   frame 3: (0.5, 0.5, 1, 1)
void expectUv(const SpriteUvRect& uv, float u0, float v0, float u1, float v1,
              const std::string& ctx) {
  EXPECT_EQ(uv.u0, u0) << ctx;
  EXPECT_EQ(uv.v0, v0) << ctx;
  EXPECT_EQ(uv.u1, u1) << ctx;
  EXPECT_EQ(uv.v1, v1) << ctx;
}

// A 4-frame animation on a 2 x 2 grid of 8 x 8 frames (the validation
// / cycle scene's def — the tight sheet is 16 x 16):
TileAnimationDef fourFrames() {
  TileAnimationDef def;
  def.frameCount = 4;
  def.frameTicks = 2;
  def.layout.frameWidth = 8;
  def.layout.frameHeight = 8;
  def.layout.columns = 2;
  def.layout.rows = 2;
  return def;
}

// ---------------------------------------------------------------------------
// Rejection pin (the tilemap_tests pattern)
// ---------------------------------------------------------------------------

void expectRejected(const laige::Status& st, laige::ErrorCode code) {
  EXPECT_TRUE(st.isError()) << "expected failure, got ok";
  if (st.isError()) {
    EXPECT_EQ(st.error(), code);
  }
}

// ---------------------------------------------------------------------------
// Log capture (the tilemap_tests MemorySink pattern)
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
// limiting OFF (the tilemap_tests pattern).
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

// ---------------------------------------------------------------------------
// The 2 x 2 parallax scene (the golden / protocol suites): the
// tilemap (layer -2, the bg tile layer — terrain (0,0)=0, (1,0)=1,
// (0,1)=0, (1,1)=2, all texture 5) + the registry + the Tilemap-
// source layer (factor 0.25, center (4, 4), offset (1, 2), the bg
// preset). TileMap is non-default-constructible (move-only, the
// M2-TILE-01 pattern), so each test builds its scene inline.
// ---------------------------------------------------------------------------

template <typename Backend>
ParallaxLayerDef bgTilemapLayerDef() {
  ParallaxLayerDef def;
  def.id = 0;  // the bg preset
  def.source = ParallaxSource::Tilemap;
  def.factor = 0.25f;
  def.center = laige::render::Vec2{4.0f, 4.0f};
  def.offset = laige::render::Vec2{1.0f, 2.0f};
  def.tilemapId = 7;
  def.depthLayer = kParallaxDepthLayerBackground;  // == the tilemap's layer
  return def;
}

template <typename Backend>
laige::Result<TileMap<Backend>> bgTilemap() {
  typename TileMap<Backend>::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  o.layer = kParallaxDepthLayerBackground;  // the bg tile layer
  return TileMap<Backend>::create(o);
}

// ---------------------------------------------------------------------------
// TileMapAnimSet — the setAnimation validation (first failure wins,
// the slot unchanged on rejection)
// ---------------------------------------------------------------------------

template <typename Backend>
void setValidation() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  ASSERT_EQ(m.maxAnimations(), 8u);  // the default

  const TileAnimationDef good = fourFrames();
  // The id domain (0 = the static sentinel; 1..maxAnimations):
  expectRejected(m.setAnimation(0, good), laige::ErrorCode::InvalidArgument);
  expectRejected(m.setAnimation(9, good), laige::ErrorCode::InvalidArgument);
  // The frame-count domain [1, kTileAnimMaxFrames]:
  TileAnimationDef zeroFrames = good;
  zeroFrames.frameCount = 0;
  expectRejected(m.setAnimation(1, zeroFrames),
                 laige::ErrorCode::InvalidArgument);
  TileAnimationDef tooManyFrames = good;
  tooManyFrames.frameCount = 65;
  expectRejected(m.setAnimation(1, tooManyFrames),
                 laige::ErrorCode::InvalidArgument);
  // 64 frames on an 8 x 8 sheet — the accepted boundary:
  TileAnimationDef maxFrames = good;
  maxFrames.frameCount = 64;
  maxFrames.layout.columns = 8;
  maxFrames.layout.rows = 8;
  ASSERT_TRUE(m.setAnimation(2, maxFrames).ok());
  // The tick-rate domain (>= 1):
  TileAnimationDef noTicks = good;
  noTicks.frameTicks = 0;
  expectRejected(m.setAnimation(1, noTicks),
                 laige::ErrorCode::InvalidArgument);
  // The sheet extents (all four > 0):
  TileAnimationDef zeroFw = good;
  zeroFw.layout.frameWidth = 0;
  expectRejected(m.setAnimation(1, zeroFw), laige::ErrorCode::InvalidArgument);
  TileAnimationDef zeroFh = good;
  zeroFh.layout.frameHeight = 0;
  expectRejected(m.setAnimation(1, zeroFh), laige::ErrorCode::InvalidArgument);
  TileAnimationDef zeroCols = good;
  zeroCols.layout.columns = 0;
  expectRejected(m.setAnimation(1, zeroCols), laige::ErrorCode::InvalidArgument);
  TileAnimationDef zeroRows = good;
  zeroRows.layout.rows = 0;
  expectRejected(m.setAnimation(1, zeroRows), laige::ErrorCode::InvalidArgument);
  // frameCount beyond the sheet's frame count (2 x 2 = 4 frames):
  TileAnimationDef beyondSheet = good;
  beyondSheet.frameCount = 5;
  expectRejected(m.setAnimation(1, beyondSheet),
                 laige::ErrorCode::InvalidArgument);
  // The tight sheet beyond the float-exact domain (the adversarial
  // layout — the u64 overflow guard: sheetW = 2 + 200000000 > 2^24):
  TileAnimationDef wide;
  wide.frameCount = 1;
  wide.frameTicks = 1;
  wide.layout.frameWidth = 200000000;
  wide.layout.frameHeight = 1;
  wide.layout.columns = 1;
  wide.layout.rows = 1;
  wide.layout.sheetBorder = 1;
  expectRejected(m.setAnimation(3, wide), laige::ErrorCode::InvalidArgument);
  // A rejected set leaves the slot unchanged:
  ASSERT_TRUE(m.setAnimation(5, good).ok());
  m.advanceAnimations();  // tick 1 < frameTicks 2 — frame stays 0
  expectRejected(m.setAnimation(5, noTicks), laige::ErrorCode::InvalidArgument);
  ASSERT_TRUE(m.hasAnimation(5));
  const TileAnimationDef& d5 = m.animationAt(5);
  EXPECT_EQ(d5.frameCount, good.frameCount);
  EXPECT_EQ(d5.frameTicks, good.frameTicks);
  EXPECT_EQ(d5.layout.frameWidth, good.layout.frameWidth);
  EXPECT_EQ(d5.layout.frameHeight, good.layout.frameHeight);
  EXPECT_EQ(d5.layout.columns, good.layout.columns);
  EXPECT_EQ(d5.layout.rows, good.layout.rows);
  EXPECT_EQ(d5.layout.frameSpacing, good.layout.frameSpacing);
  EXPECT_EQ(d5.layout.sheetBorder, good.layout.sheetBorder);
  EXPECT_EQ(m.animationFrame(5), 0u);
  // Unset slots (and the sentinel):
  EXPECT_FALSE(m.hasAnimation(0));
  EXPECT_FALSE(m.hasAnimation(1));
  // The boundary id:
  ASSERT_TRUE(m.setAnimation(m.maxAnimations(), good).ok());
  ASSERT_TRUE(m.hasAnimation(m.maxAnimations()));
  // A replace RESETS the phase (frame 0, tick 0):
  ASSERT_TRUE(m.setAnimation(5, good).ok());
  m.advanceAnimations();
  m.advanceAnimations();  // 2 ticks / 2 ticks per frame = frame 1
  EXPECT_EQ(m.animationFrame(5), 1u);
  ASSERT_TRUE(m.setAnimation(5, good).ok());
  EXPECT_EQ(m.animationFrame(5), 0u);
}

TEST(TileMapAnimSet, Validation) {
  setValidation<Fpx16_16>();
  setValidation<Fp32Pinned>();
}

TEST(TileMapAnimSet, NoLogsOnHappyPath) {
  MemorySink* sink = installCaptureSink();
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // The create's table events (iso_depth_table_created, the chunk
  // creation) are captured; the happy animation/declare paths log
  // nothing (the Status is the failure channel — LOG-002, and the
  // per-tick advance is a sim-phase hot path — LOG-003):
  sink->entries.clear();
  ASSERT_TRUE(m.setAnimation(1, fourFrames()).ok());
  ASSERT_TRUE(m.setTile(0, 0, 1, 0, 1).ok());
  m.advanceAnimations();
  SpriteBatcher::Options bo;
  bo.maxSprites = 4;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_EQ(sink->entries.size(), 0u)
      << "happy setAnimation/advance/declare path logged "
      << sink->entries.size() << " events (first: "
      << (sink->entries.empty() ? "n/a" : sink->entries[0].event) << ")";
  restoreLogger();
}

// ---------------------------------------------------------------------------
// TileMapAnimCycle — the frame cycle at the documented rate
// ---------------------------------------------------------------------------

template <typename Backend>
void cycleRate() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 1;
  o.heightTiles = 1;
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  // Two independent animations (the phase is per animation, not per
  // tile):
  ASSERT_TRUE(m.setAnimation(1, fourFrames()).ok());  // 4 frames / 2 ticks
  TileAnimationDef three;
  three.frameCount = 3;
  three.frameTicks = 1;
  three.layout.frameWidth = 8;
  three.layout.frameHeight = 8;
  three.layout.columns = 3;
  three.layout.rows = 1;
  ASSERT_TRUE(m.setAnimation(2, three).ok());  // 3 frames / 1 tick
  // The documented rate: frame(ticks) = (ticks / frameTicks) mod
  // frameCount — hand-computed over 11 ticks:
  //   anim 1 (4 / 2): 0 0 1 1 2 2 3 3 0 0 1
  //   anim 2 (3 / 1): 0 1 2 0 1 2 0 1 2 0 1
  for (std::uint32_t n = 0; n <= 10; ++n) {
    if (n > 0) m.advanceAnimations();
    EXPECT_EQ(m.animationFrame(1), (n / 2) % 4) << "tick " << n;
    EXPECT_EQ(m.animationFrame(2), n % 3) << "tick " << n;
  }
  // The unset slot: the advance is a no-op (no state, no crash):
  m.advanceAnimations();
  EXPECT_FALSE(m.hasAnimation(3));
}

TEST(TileMapAnimCycle, DocumentedRate) {
  cycleRate<Fpx16_16>();
  cycleRate<Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// TileMapAnimDeclare — the animated/static frame fields on the
// declared quads (the UV goldens, the frameIndex, the groups)
// ---------------------------------------------------------------------------

template <typename Backend>
void frameUvGoldens() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  // The scene (row-major): (0,0) static texture 1, (1,0) animated
  // texture 2, (0,1) static texture 1, (1,1) animated texture 3
  // (flat ground — the auto-depth keys are the M2-TILE-01 goldens):
  ASSERT_TRUE(m.setTile(0, 0, 1, 0, 0).ok());
  ASSERT_TRUE(m.setTile(1, 0, 2, 0, 1).ok());
  ASSERT_TRUE(m.setTile(0, 1, 1, 0, 0).ok());
  ASSERT_TRUE(m.setTile(1, 1, 3, 0, 1).ok());
  TileAnimationDef anim;
  anim.frameCount = 4;
  anim.frameTicks = 2;
  anim.layout.frameWidth = 16;
  anim.layout.frameHeight = 16;
  anim.layout.columns = 2;
  anim.layout.rows = 2;  // the tight sheet is 32 x 32
  ASSERT_TRUE(m.setAnimation(1, anim).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 8;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(batcher.frameCount(), 4u);
  // Frame 0 (tick 0 — the fresh phase). Hand-computed goldens (the
  // layer-0 key = 0x80000000 | (d + 2^21), d = 16 * (tx + ty + 1)):
  //   slot 0 (0,0): pos (0.5, 0.5), key 0x80200010, STATIC:
  //                  uv (0, 0, 1, 1), frameIndex 0, atlas 1
  //   slot 1 (1,0): pos (1.5, 0.5), key 0x80200020, ANIMATED:
  //                  uv frame 0 (0, 0, 0.5, 0.5), frameIndex 0, atlas 2
  //   slot 2 (0,1): pos (0.5, 1.5), key 0x80200020, STATIC:
  //                  uv (0, 0, 1, 1), frameIndex 0, atlas 1
  //   slot 3 (1,1): pos (1.5, 1.5), key 0x80200030, ANIMATED:
  //                  uv frame 0 (0, 0, 0.5, 0.5), frameIndex 0, atlas 3
  struct Expect {
    float x, y;
    std::uint32_t key;
    bool animated;
    std::uint32_t atlas;
  };
  constexpr Expect kExp[4] = {
      {0.5f, 0.5f, 0x80200010u, false, 1u},
      {1.5f, 0.5f, 0x80200020u, true, 2u},
      {0.5f, 1.5f, 0x80200020u, false, 1u},
      {1.5f, 1.5f, 0x80200030u, true, 3u}};
  for (int i = 0; i < 4; ++i) {
    const SpriteItem* item = batcher.get(static_cast<std::uint32_t>(i));
    ASSERT_NE(item, nullptr) << "tile " << i;
    EXPECT_EQ(item->pos.x, kExp[i].x) << "tile " << i;
    EXPECT_EQ(item->pos.y, kExp[i].y) << "tile " << i;
    EXPECT_EQ(item->depthKey, kExp[i].key) << "tile " << i;
    EXPECT_EQ(item->depthKey,
              oracleKeyAt<Backend>(kExp[i].x, kExp[i].y, 0, 0))
        << "tile " << i;
    EXPECT_FALSE(item->depthOverride) << "tile " << i;
    EXPECT_EQ(item->atlasId, kExp[i].atlas) << "tile " << i;
    EXPECT_EQ(item->frameIndex, 0u) << "tile " << i;
    if (kExp[i].animated) {
      expectUv(item->uv, 0.0f, 0.0f, 0.5f, 0.5f, "tile " + std::to_string(i));
    } else {
      expectUv(item->uv, 0.0f, 0.0f, 1.0f, 1.0f, "tile " + std::to_string(i));
    }
  }
  // The (atlas, material, blend) groups: 3 (atlas 1/2/3), ascending;
  // the atlas-1 group's instances in sorted key order [0, 2]:
  ASSERT_EQ(batcher.batchCount(), 3u);
  const auto& batches = batcher.batches();
  EXPECT_EQ(batches[0].atlasId, 1u);
  EXPECT_EQ(batches[0].instances.size(), 2u);
  EXPECT_EQ(batches[0].instances[0], 0u);
  EXPECT_EQ(batches[0].instances[1], 2u);
  EXPECT_EQ(batches[1].atlasId, 2u);
  EXPECT_EQ(batches[1].instances.size(), 1u);
  EXPECT_EQ(batches[1].instances[0], 1u);
  EXPECT_EQ(batches[2].atlasId, 3u);
  EXPECT_EQ(batches[2].instances.size(), 1u);
  EXPECT_EQ(batches[2].instances[0], 3u);
}

TEST(TileMapAnimDeclare, FrameUvGoldens) {
  frameUvGoldens<Fpx16_16>();
  frameUvGoldens<Fp32Pinned>();
}

template <typename Backend>
void frameWrapAndDeterminism() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  ASSERT_TRUE(m.setTile(0, 0, 1, 0, 0).ok());
  ASSERT_TRUE(m.setTile(1, 0, 2, 0, 1).ok());
  ASSERT_TRUE(m.setTile(0, 1, 1, 0, 0).ok());
  ASSERT_TRUE(m.setTile(1, 1, 3, 0, 1).ok());
  TileAnimationDef anim;
  anim.frameCount = 4;
  anim.frameTicks = 2;
  anim.layout.frameWidth = 16;
  anim.layout.frameHeight = 16;
  anim.layout.columns = 2;
  anim.layout.rows = 2;
  ASSERT_TRUE(m.setAnimation(1, anim).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 8;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  // One declare: the animated tile (slot 1) — the pointer; the null
  // checks live in the test (ASSERTs are void-returning, not allowed
  // in a value-returning lambda):
  auto declareAnimated = [&]() -> const SpriteItem* {
    batcher.beginFrame();
    if (!m.declareTo(batcher, typename Map::DeclareOptions{}).ok()) {
      return nullptr;
    }
    if (!batcher.build().ok()) return nullptr;
    return batcher.get(1);
  };
  // The 4-frame / 2-tick cycle: the frame steps every 2 ticks and
  // WRAPS at frameCount (tick 0 → frame 0, tick 2 → frame 1, tick 4
  // → frame 2, tick 6 → frame 3, tick 8 → frame 0, tick 10 → frame
  // 1). The UV goldens are per frame (the tight-sheet 2 x 2 grid of
  // 16 x 16 frames — the M2-SPRITE-03 formula):
  struct Expect {
    float u0, v0, u1, v1;
  };
  constexpr Expect kFrameUv[4] = {
      {0.0f, 0.0f, 0.5f, 0.5f},
      {0.5f, 0.0f, 1.0f, 0.5f},
      {0.0f, 0.5f, 0.5f, 1.0f},
      {0.5f, 0.5f, 1.0f, 1.0f}};
  const SpriteItem* p0 = declareAnimated();  // tick 0: frame 0
  ASSERT_NE(p0, nullptr);
  EXPECT_EQ(p0->frameIndex, 0u);
  expectUv(p0->uv, 0.0f, 0.0f, 0.5f, 0.5f, "tick 0");
  for (int step = 1; step <= 5; ++step) {
    m.advanceAnimations();
    m.advanceAnimations();  // one frame (2 ticks)
    const SpriteItem* p = declareAnimated();
    ASSERT_NE(p, nullptr) << "step " << step;
    const std::uint32_t frame = static_cast<std::uint32_t>(step % 4);
    EXPECT_EQ(p->frameIndex, frame) << "step " << step;
    expectUv(p->uv, kFrameUv[frame].u0, kFrameUv[frame].v0,
             kFrameUv[frame].u1, kFrameUv[frame].v1, "step " + std::to_string(step));
  }
  // Cross-frame determinism: the same animation state declares
  // BIT-IDENTICAL items (same data + same frame state → same batch):
  auto snapshot = [&]() {
    batcher.beginFrame();
    if (!m.declareTo(batcher, typename Map::DeclareOptions{}).ok()) {
      return std::vector<SpriteItem>{};
    }
    if (!batcher.build().ok()) return std::vector<SpriteItem>{};
    std::vector<SpriteItem> items(4);
    for (std::uint32_t i = 0; i < 4; ++i) {
      const SpriteItem* it = batcher.get(i);
      if (it == nullptr) return std::vector<SpriteItem>{};
      items[i] = *it;
    }
    return items;
  };
  const std::vector<SpriteItem> a = snapshot();
  ASSERT_EQ(a.size(), 4u);
  const std::vector<SpriteItem> b = snapshot();
  ASSERT_EQ(b.size(), 4u);
  for (std::uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(a[i].pos.x, b[i].pos.x) << "tile " << i;
    EXPECT_EQ(a[i].pos.y, b[i].pos.y) << "tile " << i;
    EXPECT_EQ(a[i].depthKey, b[i].depthKey) << "tile " << i;
    // SpriteUvRect carries no operator== (the M2-SPRITE-01 value
    // type) — the fields:
    EXPECT_EQ(a[i].uv.u0, b[i].uv.u0) << "tile " << i;
    EXPECT_EQ(a[i].uv.v0, b[i].uv.v0) << "tile " << i;
    EXPECT_EQ(a[i].uv.u1, b[i].uv.u1) << "tile " << i;
    EXPECT_EQ(a[i].uv.v1, b[i].uv.v1) << "tile " << i;
    EXPECT_EQ(a[i].frameIndex, b[i].frameIndex) << "tile " << i;
    EXPECT_EQ(a[i].atlasId, b[i].atlasId) << "tile " << i;
  }
}

TEST(TileMapAnimDeclare, FrameWrapAndDeterminism) {
  frameWrapAndDeterminism<Fpx16_16>();
  frameWrapAndDeterminism<Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// TileMapAnimParallax — the parallax tile layer: the worldOffset
// translation goldens at given camera positions (the M2-PAR-01 hook)
// ---------------------------------------------------------------------------

// The scene setup (each test builds it inline — TileMap is
// move-only, no default constructor, the M2-TILE-01 pattern): the
// bgTilemap() tilemap + the terrain + the registry/layer + the
// 8-slot batcher.
template <typename Backend>
bool bgTerrain(TileMap<Backend>& m) {
  return m.setTile(0, 0, 5, 0, 0).ok() && m.setTile(1, 0, 5, 1, 0).ok() &&
         m.setTile(0, 1, 5, 0, 0).ok() && m.setTile(1, 1, 5, 2, 0).ok();
}

template <typename Backend>
ParallaxLayers<Backend> bgLayers() {
  typename ParallaxLayers<Backend>::Options lo;
  auto rl = ParallaxLayers<Backend>::create(lo);
  if (!rl.ok()) {
    ADD_FAILURE() << "parallax registry create failed";
    return ParallaxLayers<Backend>();  // the stopped state
  }
  ParallaxLayers<Backend> layers = std::move(rl).takeValue();
  if (!layers.setLayer(bgTilemapLayerDef<Backend>()).ok()) {
    ADD_FAILURE() << "bg layer set failed";
    return ParallaxLayers<Backend>();  // the stopped state
  }
  return layers;
}

template <typename Backend>
void goldenOffsets() {
  using Map = TileMap<Backend>;
  auto r = bgTilemap<Backend>();
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  ASSERT_TRUE(bgTerrain(m));
  ParallaxLayers<Backend> layers = bgLayers<Backend>();
  SpriteBatcher::Options bo;
  bo.maxSprites = 8;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  SpriteBatcher batcher = std::move(rb).takeValue();
  // The ground probe (layer 0) — the layer-dominance check:
  auto declareFrame = [&](laige::render::Vec2 cameraPos) {
    batcher.beginFrame();
    SpriteItem ground;
    ground.pos = laige::render::Vec2{0.5f, 0.5f};
    ground.depthKey = oracleKeyAt<Backend>(0.5f, 0.5f, 0, 0);
    ground.atlasId = 99;
    ASSERT_TRUE(batcher.add(ground).ok());
    ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}, layers, 0,
                            cameraPos).ok());
    ASSERT_TRUE(batcher.build().ok());
  };
  // Camera (10, 6): offset = 0.25 * (10 - 4, 6 - 4) + (1, 2)
  //                 = 0.25 * (6, 2) + (1, 2) = (2.5, 2.5)
  declareFrame(laige::render::Vec2{10.0f, 6.0f});
  // Hand-computed keys (l = -2: (510 << 22) | (d + 2^21); d =
  // 16 * (x + y) - 16 * h at the TRANSLATED centers; the 2^21 bias
  // adds into bit 21, as in the ground-layer golden 0x80200010):
  //   (0,0) → (3, 3): d = 96 - 0   = 96  → 0x7FA00060
  //   (1,0) → (4, 3): d = 112 - 16  = 96  → 0x7FA00060
  //   (0,1) → (3, 4): d = 112 - 0   = 112 → 0x7FA00070
  //   (1,1) → (4, 4): d = 128 - 32  = 96  → 0x7FA00060
  struct Expect {
    float x, y;
    std::uint32_t key;
    std::int32_t h;
  };
  constexpr Expect kExp[4] = {
      {3.0f, 3.0f, 0x7FA00060u, 0},
      {4.0f, 3.0f, 0x7FA00060u, 1},
      {3.0f, 4.0f, 0x7FA00070u, 0},
      {4.0f, 4.0f, 0x7FA00060u, 2}};
  ASSERT_EQ(batcher.frameCount(), 5u);
  for (int i = 0; i < 4; ++i) {
    const SpriteItem* item = batcher.get(1u + static_cast<std::uint32_t>(i));
    ASSERT_NE(item, nullptr) << "tile " << i;
    EXPECT_EQ(item->pos.x, kExp[i].x) << "tile " << i;
    EXPECT_EQ(item->pos.y, kExp[i].y) << "tile " << i;
    EXPECT_EQ(item->depthKey, kExp[i].key) << "tile " << i;
    EXPECT_EQ(item->depthKey,
              oracleKeyAt<Backend>(kExp[i].x, kExp[i].y, kExp[i].h, -2))
        << "tile " << i;
    EXPECT_EQ(item->atlasId, 5u) << "tile " << i;
    EXPECT_FALSE(item->depthOverride) << "tile " << i;
  }
  // The ground probe (layer 0) sorts AFTER every bg tile — the layer
  // field dominates the key (the documented background-first order):
  const SpriteItem* g = batcher.get(0);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->depthKey, 0x80200010u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_LT(batcher.get(1u + static_cast<std::uint32_t>(i))->depthKey,
              g->depthKey)
        << "tile " << i;
  }
}

TEST(TileMapAnimParallax, GoldenOffsets) {
  goldenOffsets<Fpx16_16>();
  goldenOffsets<Fp32Pinned>();
}

template <typename Backend>
void offsetFollowsCamera() {
  using Map = TileMap<Backend>;
  auto r = bgTilemap<Backend>();
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  ASSERT_TRUE(bgTerrain(m));
  ParallaxLayers<Backend> layers = bgLayers<Backend>();
  SpriteBatcher::Options bo;
  bo.maxSprites = 8;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  SpriteBatcher batcher = std::move(rb).takeValue();
  // Camera (4, 4) = the layer's center: offset (1, 2) — tile (0,0)
  // at (1.5, 2.5):
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}, layers, 0,
                          laige::render::Vec2{4.0f, 4.0f}).ok());
  ASSERT_TRUE(batcher.build().ok());
  const SpriteItem* t00 = batcher.get(0);
  ASSERT_NE(t00, nullptr);
  EXPECT_EQ(t00->pos.x, 1.5f);
  EXPECT_EQ(t00->pos.y, 2.5f);
  // d = 16 * 4 - 0 = 64 → (510 << 22) | (2^21 + 64) = 0x7FA00040:
  EXPECT_EQ(t00->depthKey, 0x7FA00040u);
  EXPECT_EQ(t00->depthKey, oracleKeyAt<Backend>(1.5f, 2.5f, 0, -2));
  // Camera (14, 8): offset = 0.25 * (10, 4) + (1, 2) = (3.5, 3) —
  // tile (0,0) at (4, 3.5):
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}, layers, 0,
                          laige::render::Vec2{14.0f, 8.0f}).ok());
  ASSERT_TRUE(batcher.build().ok());
  const SpriteItem* t01 = batcher.get(0);
  ASSERT_NE(t01, nullptr);
  EXPECT_EQ(t01->pos.x, 4.0f);
  EXPECT_EQ(t01->pos.y, 3.5f);
  // d = 16 * 7.5 - 0 = 120 → (510 << 22) | (2^21 + 120) = 0x7FA00078:
  EXPECT_EQ(t01->depthKey, 0x7FA00078u);
  EXPECT_EQ(t01->depthKey, oracleKeyAt<Backend>(4.0f, 3.5f, 0, -2));
}

TEST(TileMapAnimParallax, OffsetFollowsCamera) {
  offsetFollowsCamera<Fpx16_16>();
  offsetFollowsCamera<Fp32Pinned>();
}

TEST(TileMapAnimParallax, Protocol) {
  using Map = TileMap<Fpx16_16>;
  auto r = bgTilemap<Fpx16_16>();
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  ASSERT_TRUE(bgTerrain(m));
  ParallaxLayers<Fpx16_16> layers = bgLayers<Fpx16_16>();
  SpriteBatcher::Options bo;
  bo.maxSprites = 8;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  SpriteBatcher batcher = std::move(rb).takeValue();
  const laige::render::Vec2 p{10.0f, 6.0f};
  const typename Map::DeclareOptions opts{};
  // A built frame's window is closed — rejected, nothing declared:
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, opts, layers, 0, p).ok());
  ASSERT_TRUE(batcher.build().ok());
  expectRejected(m.declareTo(batcher, opts, layers, 0, p),
                 laige::ErrorCode::InvalidArgument);
  batcher.beginFrame();
  EXPECT_EQ(batcher.frameCount(), 0u);
  // An unset layer id:
  expectRejected(m.declareTo(batcher, opts, layers, 1, p),
                 laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(batcher.frameCount(), 0u);
  // A non-Tilemap-source layer (the Image def on id 1):
  laige::render::ParallaxLayerDef img;
  img.id = 1;
  img.factor = 0.5f;
  img.size = laige::render::Vec2{8.0f, 8.0f};
  img.atlasId = 50;
  ASSERT_TRUE(layers.setLayer(img).ok());
  expectRejected(m.declareTo(batcher, opts, layers, 1, p),
                 laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(batcher.frameCount(), 0u);
  // A DISABLED Tilemap layer: nothing declared (OK, no log — the
  // layer's documented skip):
  laige::render::ParallaxLayerDef off = bgTilemapLayerDef<Fpx16_16>();
  off.enabled = false;
  ASSERT_TRUE(layers.setLayer(off).ok());
  ASSERT_TRUE(m.declareTo(batcher, opts, layers, 0, p).ok());
  EXPECT_EQ(batcher.frameCount(), 0u);
}

template <typename Backend>
void animatedUnderParallax() {
  using Map = TileMap<Backend>;
  auto r = bgTilemap<Backend>();
  ASSERT_TRUE(r.ok());
  Map m = std::move(r).takeValue();
  ASSERT_TRUE(bgTerrain(m));
  ParallaxLayers<Backend> layers = bgLayers<Backend>();
  SpriteBatcher::Options bo;
  bo.maxSprites = 8;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  SpriteBatcher batcher = std::move(rb).takeValue();
  // The (0,0) tile cycles from animation 1 (4 frames / 2 ticks, the
  // 16 x 16 frame grid — the tight sheet 32 x 32):
  ASSERT_TRUE(m.setTile(0, 0, 5, 0, 1).ok());
  TileAnimationDef anim;
  anim.frameCount = 4;
  anim.frameTicks = 2;
  anim.layout.frameWidth = 16;
  anim.layout.frameHeight = 16;
  anim.layout.columns = 2;
  anim.layout.rows = 2;
  ASSERT_TRUE(m.setAnimation(1, anim).ok());
  m.advanceAnimations();
  m.advanceAnimations();  // tick 2: frame 1
  // Camera (10, 6): offset (2.5, 2.5) — tile (0,0) at (3, 3):
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}, layers, 0,
                          laige::render::Vec2{10.0f, 6.0f}).ok());
  ASSERT_TRUE(batcher.build().ok());
  const SpriteItem* t00 = batcher.get(0);
  ASSERT_NE(t00, nullptr);
  EXPECT_EQ(t00->pos.x, 3.0f);
  EXPECT_EQ(t00->pos.y, 3.0f);
  EXPECT_EQ(t00->depthKey, 0x7FA00060u);
  EXPECT_EQ(t00->depthKey, oracleKeyAt<Backend>(3.0f, 3.0f, 0, -2));
  // The ANIMATED frame under the parallax translation: frame 1's UV
  // (the precomputed rect), frameIndex 1:
  expectUv(t00->uv, 0.5f, 0.0f, 1.0f, 0.5f, "animated tile (0,0)");
  EXPECT_EQ(t00->frameIndex, 1u);
  EXPECT_EQ(t00->atlasId, 5u);
  // The static sibling (1,0): the fixed frame (0, 0, 1, 1),
  // frameIndex 0:
  const SpriteItem* t10 = batcher.get(1);
  ASSERT_NE(t10, nullptr);
  expectUv(t10->uv, 0.0f, 0.0f, 1.0f, 1.0f, "static tile (1,0)");
  EXPECT_EQ(t10->frameIndex, 0u);
}

TEST(TileMapAnimParallax, AnimatedUnderParallax) {
  animatedUnderParallax<Fpx16_16>();
  animatedUnderParallax<Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// TileMapAnimProtocol — the animationId domain on the edit/load paths
// + the unset-slot declare failure
// ---------------------------------------------------------------------------

TEST(TileMapAnimProtocol, EditAnimationDomain) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // Out-of-range id (9 > maxAnimations 8) — rejected, no state change:
  expectRejected(m.setTile(0, 0, 1, 0, 9), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(m.tileAt(0, 0), TileData{});
  // The boundary id (8) and the sentinel (0) are accepted:
  ASSERT_TRUE(m.setTile(0, 0, 1, 0, 8).ok());
  EXPECT_EQ(m.tileAt(0, 0).animationId, 8u);
  ASSERT_TRUE(m.setTile(0, 0, 1, 0, 0).ok());
  EXPECT_EQ(m.tileAt(0, 0).animationId, 0u);
  // Rebuild: an out-of-range id ANYWHERE in the span — rejected, the
  // whole span validated before any write:
  ASSERT_TRUE(m.setTile(1, 1, 2, 3, 0).ok());
  const TileData base = m.tileAt(1, 1);
  const std::uint32_t baseKey = m.depthKeyAt(1, 1);
  std::vector<TileData> tiles(4);
  tiles[3].animationId = 9;  // tile (1, 1)
  expectRejected(m.rebuild(tiles), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(m.tileAt(1, 1), base);
  EXPECT_EQ(m.depthKeyAt(1, 1), baseKey);
  tiles[3].animationId = 8;
  ASSERT_TRUE(m.rebuild(tiles).ok());
  EXPECT_EQ(m.tileAt(1, 1), (TileData{0, 0, 8}));
}

TEST(TileMapAnimProtocol, DeclareUnsetAnimation) {
  TileMap<Fpx16_16>::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  auto r = TileMap<Fpx16_16>::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  // (0,0) is animated (slot 1) but the slot is UNSET — first failure
  // wins, and (0,0) is the first tile in row-major order: nothing
  // declared:
  ASSERT_TRUE(m.setTile(0, 0, 1, 0, 1).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 4;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  batcher.beginFrame();
  expectRejected(
      m.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{}),
      laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(batcher.frameCount(), 0u);
  // The slot's set is the scene's setup responsibility:
  ASSERT_TRUE(m.setAnimation(1, fourFrames()).ok());
  batcher.beginFrame();
  ASSERT_TRUE(m.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{}).ok());
  EXPECT_EQ(batcher.frameCount(), 4u);
}

// ---------------------------------------------------------------------------
// TileMapAnimZeroAlloc — the per-tick advance + per-frame declare loop
// (standalone + parallax) allocates nothing (FR-2.2)
// ---------------------------------------------------------------------------

// The first offending site, resolved to its module + symbol when the
// platform provides dladdr (POSIX — the macOS/Linux lanes): the
// actionable context for the failure (LOG-002, FR-12.3). Raw address
// elsewhere (the Windows lane — MSVC has no dladdr).
std::string describeTileAnimAllocSite(const void* site) {
#if defined(_MSC_VER)
  return "<raw site " +
         std::to_string(reinterpret_cast<std::uintptr_t>(site)) + ">";
#else
  Dl_info info;
  if (site != nullptr && dladdr(site, &info) != 0 &&
      info.dli_fname != nullptr) {
    std::string out = info.dli_fname;
    if (info.dli_sname != nullptr) {
      out += " +";
      out += info.dli_sname;
    }
    out += " (addr ";
    out += std::to_string(reinterpret_cast<std::uintptr_t>(site));
    out += ")";
    return out;
  }
  return "<unresolved site " +
         std::to_string(reinterpret_cast<std::uintptr_t>(site)) + ">";
#endif
}

template <typename Backend>
void advanceAndDeclareLoopAllocatesNothing() {
  using Map = TileMap<Backend>;
  typename Map::Options o;
  o.widthTiles = 16;
  o.heightTiles = 16;  // 256 tiles per frame (the chunk size)
  auto r = Map::create(o);
  ASSERT_TRUE(r.ok());
  auto m = std::move(r).takeValue();
  std::vector<TileData> tiles(256);
  for (std::size_t i = 0; i < 256; ++i) {
    const std::int32_t tx = static_cast<std::int32_t>(i % 16);
    const std::int32_t ty = static_cast<std::int32_t>(i / 16);
    tiles[i].textureId = 1;
    tiles[i].height = static_cast<std::int32_t>((i * 3) % 5);
    tiles[i].animationId = ((tx + ty) % 3 == 0) ? 1u : 0u;  // ~86 animated
  }
  ASSERT_TRUE(m.rebuild(tiles).ok());  // setup (before the window)
  ASSERT_TRUE(m.setAnimation(1, fourFrames()).ok());
  // The parallax tile layer: a second 8 x 8 tilemap (the bg layer) +
  // the Tilemap-source layer (factor 0.5, offset (2, 1), Manual):
  typename Map::Options po;
  po.widthTiles = 8;
  po.heightTiles = 8;
  po.layer = kParallaxDepthLayerBackground;
  auto rp = Map::create(po);
  ASSERT_TRUE(rp.ok());
  auto pm = std::move(rp).takeValue();
  typename ParallaxLayers<Backend>::Options lo;
  auto rl = ParallaxLayers<Backend>::create(lo);
  ASSERT_TRUE(rl.ok());
  auto layers = std::move(rl).takeValue();
  ParallaxLayerDef def;
  def.id = 0;
  def.source = ParallaxSource::Tilemap;
  def.factor = 0.5f;
  def.offset = laige::render::Vec2{2.0f, 1.0f};
  def.tilemapId = 1;
  def.depthLayer = kParallaxDepthLayerBackground;
  ASSERT_TRUE(layers.setLayer(def).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 256 + 64;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  auto runFrames = [&]() {
    for (std::int32_t frame = 0; frame < 1000; ++frame) {
      // The sim ticks (30 per frame — the advance is the sim phase):
      for (int t = 0; t < 30; ++t) {
        m.advanceAnimations();
        pm.advanceAnimations();
      }
      batcher.beginFrame();
      ASSERT_TRUE(m.declareTo(batcher, typename Map::DeclareOptions{}).ok())
          << "frame " << frame;
      ASSERT_TRUE(pm.declareTo(batcher, typename Map::DeclareOptions{}, layers,
                               0, laige::render::Vec2{10.0f, 6.0f}).ok())
          << "frame " << frame;
      ASSERT_TRUE(batcher.build().ok()) << "frame " << frame;
    }
  };
  // Zero-allocation proof (where the watch is live — the non-
  // sanitizer trees; the sanitizer runtimes own operator new):
  // 1000 frames of the advance + declare loop allocate nothing (the
  // animation UVs are precomputed at setAnimation, the slot table at
  // create — FR-2.2 "no per-frame allocation").
  //
  // The window's owner is THIS thread: the watch counts owner-thread
  // allocations only (the attribution contract, alloc_watch.h). The
  // GL/GLFW suites earlier in this binary load the macOS graphics
  // framework chain, and its background framework threads (QuartzCore
  // / SkyLight — e.g. the WindowServer datagram dispatch, observed in
  // CI) do their own heap work in parallel with our loop; that is
  // not the loop's work and is not counted here.
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    runFrames();
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "1000 frames of advanceAnimations/beginFrame/declareTo/build "
        << "allocated " << reading.allocs
        << " heap blocks on the loop thread (first site: "
        << describeTileAnimAllocSite(reading.firstSite) << ")";
  }
}

TEST(TileMapAnimZeroAlloc, LoopAllocatesNothing) {
  advanceAndDeclareLoopAllocatesNothing<Fpx16_16>();
  advanceAndDeclareLoopAllocatesNothing<Fp32Pinned>();
}

}  // namespace
