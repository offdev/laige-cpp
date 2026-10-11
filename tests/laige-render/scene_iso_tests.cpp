// laige-render isometric reference scene tests (M2-SCENE-01).
//
// The M2-SCENE-01 scope under test: the deterministic builder (the
// ARCH-010 contract — same seed, same bytes), the versioned binary
// format (the round trip + the untrusted-input validation, SCALE-004 /
// TEST-005), and the engine-side world (load + one declare frame per
// backend — the PRD §8.1 workload at reference size: 50k sprites,
// stepped terrain, 3 parallax layers, particles, UI; the 14-group
// draw-call property under the 30-draw-call budget) and the zero-
// allocation declare loop (PERF-003).
//
// GL policy: none of these tests create a GL context (the batcher is
// the CPU stage — the M2-SPRITE-01 precedent); they run in every P0
// tree, including the sanitizer trees. The 50k frames are the §8.1
// workload shape at the CPU stage; M2-PERF-01 measures the composite
// frame timing.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/render/iso_reference_scene.h"
#include "laige/render/ui_render.h"
#include "laige/result.h"
#include "laige/sim_math.h"

using laige::ErrorCode;
using laige::render::BlendMode;
using laige::render::IsoRefSceneVariant;
using laige::render::IsoReferenceScene;
using laige::render::IsoReferenceSceneWorld;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using laige::render::SpriteItem;
using laige::render::UiDeclareOptions;
using laige::render::Vec2;
using laige::render::buildIsoReferenceScene;
using laige::render::declareIsoReferenceScene;
using laige::render::declareUi;
using laige::render::kIsoRefSceneBgAtlas;
using laige::render::kIsoRefSceneEmitterCount;
using laige::render::kIsoRefSceneFgAtlas;
using laige::render::kIsoRefSceneFullGridTiles;
using laige::render::kIsoRefSceneFullSprites;
using laige::render::kIsoRefSceneFormatVersion;
using laige::render::kIsoRefSceneGroundAtlas;
using laige::render::kIsoRefSceneMidAtlas;
using laige::render::kIsoRefSceneParticleAtlas;
using laige::render::kIsoRefSceneSmallSprites;
using laige::render::kIsoRefSceneSpriteAtlasBase;
using laige::render::kIsoRefSceneSpriteSheetFrames;
using laige::render::kIsoRefSceneUiWidgetAtlas;
using laige::render::loadIsoReferenceScene;
using laige::render::readIsoReferenceScene;
using laige::render::serializeIsoReferenceScene;
using laige::sim::Fp32Pinned;
using laige::sim::Fpx16_16;

namespace {

// The test's fixed seed (the determinism contract's reference value —
// the ctest CLI gate uses the same one).
constexpr std::uint64_t kTestSeed = 424242u;

// The scene's deep equality (the POD value types are compared field
// by field — the wire layout is the comparison contract).
bool scenesEqual(const IsoReferenceScene& a, const IsoReferenceScene& b) {
  if (a.version != b.version || a.variant != b.variant || a.seed != b.seed ||
      a.originTileX != b.originTileX || a.originTileY != b.originTileY ||
      a.widthTiles != b.widthTiles || a.heightTiles != b.heightTiles ||
      a.plateauTiles != b.plateauTiles || a.stepHeight != b.stepHeight ||
      a.maxSteps != b.maxSteps || a.tileCount != b.tileCount ||
      a.spriteCount != b.spriteCount || a.emitterCount != b.emitterCount ||
      a.uiWidgetCount != b.uiWidgetCount || a.parallaxCount != b.parallaxCount) {
    return false;
  }
  if (a.tileHeights != b.tileHeights) return false;
  if (a.sprites.size() != b.sprites.size()) return false;
  for (std::size_t i = 0; i < a.sprites.size(); ++i) {
    const auto& s = a.sprites[i];
    const auto& t = b.sprites[i];
    if (s.x != t.x || s.y != t.y || s.stepHeight != t.stepHeight ||
        s.atlasId != t.atlasId || s.blend != t.blend || s.frame != t.frame ||
        s.reserved != t.reserved) {
      return false;
    }
  }
  for (std::size_t i = 0; i < a.parallaxCount; ++i) {
    const auto& l = a.parallax[i];
    const auto& m = b.parallax[i];
    if (l.factor != m.factor || l.center.x != m.center.x ||
        l.center.y != m.center.y || l.offset.x != m.offset.x ||
        l.offset.y != m.offset.y || l.size.x != m.size.x ||
        l.size.y != m.size.y || l.uv.u0 != m.uv.u0 || l.uv.v0 != m.uv.v0 ||
        l.uv.u1 != m.uv.u1 || l.uv.v1 != m.uv.v1 || l.atlasId != m.atlasId ||
        l.materialId != m.materialId || l.blend != m.blend ||
        l.scrollMode != m.scrollMode || l.enabled != m.enabled ||
        l.reserved != m.reserved || l.depthLayer != m.depthLayer ||
        l.scrollSpeed.x != m.scrollSpeed.x ||
        l.scrollSpeed.y != m.scrollSpeed.y) {
      return false;
    }
  }
  for (std::size_t i = 0; i < a.emitterCount; ++i) {
    const auto& e = a.emitters[i];
    const auto& f = b.emitters[i];
    if (e.originX != f.originX || e.originY != f.originY || e.depth != f.depth ||
        e.velMinX != f.velMinX || e.velMinY != f.velMinY ||
        e.velMaxX != f.velMaxX || e.velMaxY != f.velMaxY ||
        e.lifeMin != f.lifeMin || e.lifeMax != f.lifeMax ||
        e.sizeMin != f.sizeMin || e.sizeMax != f.sizeMax ||
        std::memcmp(e.tint, f.tint, 4) != 0 || e.fadeTicks != f.fadeTicks ||
        e.continuousRate != f.continuousRate) {
      return false;
    }
  }
  for (std::size_t i = 0; i < a.uiWidgetCount; ++i) {
    const auto& w = a.uiWidgets[i];
    const auto& v = b.uiWidgets[i];
    if (w.kind != v.kind || w.opaque != v.opaque || w.x != v.x || w.y != v.y ||
        w.width != v.width || w.height != v.height || w.atlasId != v.atlasId ||
        w.label != v.label) {
      return false;
    }
  }
  return true;
}

// The world pass's frame expectations for one declare after `ticks`
// particle updates and the FIRST parallax advance (the auto layers'
// both wrap coordinates are > 0 after one advance -> 4 quads each;
// the manual mid layer -> 1 quad; the parallax.h wrap contract).
constexpr std::uint32_t kFirstFrameParallaxQuads = 4u + 1u + 4u;

}  // namespace

// ---------------------------------------------------------------------------
// The builder (ARCH-010: same seed, same scene, every build/platform)
// ---------------------------------------------------------------------------

TEST(IsoReferenceSceneBuilder, SameSeedByteIdenticalBothVariants) {
  for (IsoRefSceneVariant variant : {IsoRefSceneVariant::Small,
                                     IsoRefSceneVariant::Full}) {
    const std::vector<std::uint8_t> a =
        serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, variant));
    const std::vector<std::uint8_t> b =
        serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, variant));
    EXPECT_EQ(a, b);
    // A third build (the substream sequence is a pure function of the
    // seed — re-deriving the streams reproduces every draw).
    const std::vector<std::uint8_t> c =
        serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, variant));
    EXPECT_EQ(a, c);
  }
}

TEST(IsoReferenceSceneBuilder, DifferentSeedsDifferBothVariants) {
  for (IsoRefSceneVariant variant : {IsoRefSceneVariant::Small,
                                     IsoRefSceneVariant::Full}) {
    const std::vector<std::uint8_t> a =
        serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, variant));
    const std::vector<std::uint8_t> b =
        serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed + 1u, variant));
    EXPECT_NE(a, b);
  }
}

TEST(IsoReferenceSceneBuilder, ShapeConstantsFull) {
  const IsoReferenceScene scene = buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Full);
  EXPECT_EQ(scene.version, kIsoRefSceneFormatVersion);
  EXPECT_EQ(scene.variant, IsoRefSceneVariant::Full);
  EXPECT_EQ(scene.seed, kTestSeed);
  EXPECT_EQ(scene.widthTiles, kIsoRefSceneFullGridTiles);
  EXPECT_EQ(scene.heightTiles, kIsoRefSceneFullGridTiles);
  EXPECT_EQ(scene.plateauTiles, 16);
  EXPECT_EQ(scene.stepHeight, 2);
  EXPECT_EQ(scene.maxSteps, 24);
  EXPECT_EQ(scene.tileCount, 65536u);
  EXPECT_EQ(scene.spriteCount, kIsoRefSceneFullSprites);
  EXPECT_EQ(scene.emitterCount, kIsoRefSceneEmitterCount);
  EXPECT_EQ(scene.uiWidgetCount, 2u);
  EXPECT_EQ(scene.parallaxCount, 3u);
  EXPECT_EQ(scene.tileHeights.size(), scene.tileCount);
  EXPECT_EQ(scene.sprites.size(), scene.spriteCount);
}

TEST(IsoReferenceSceneBuilder, ShapeConstantsSmall) {
  const IsoReferenceScene scene = buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small);
  EXPECT_EQ(scene.variant, IsoRefSceneVariant::Small);
  EXPECT_EQ(scene.widthTiles, 32);
  EXPECT_EQ(scene.heightTiles, 32);
  EXPECT_EQ(scene.plateauTiles, 8);
  EXPECT_EQ(scene.tileCount, 1024u);
  EXPECT_EQ(scene.spriteCount, kIsoRefSceneSmallSprites);
  EXPECT_EQ(scene.sprites.size(), scene.spriteCount);
}

TEST(IsoReferenceSceneBuilder, SpriteInvariantsSmall) {
  const IsoReferenceScene scene =
      buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small);
  constexpr int kGrid = 32;
  constexpr int kMaxHeight = (24 - 1) * 2;  // (maxSteps-1) x stepHeight
  for (const auto& s : scene.sprites) {
    // The 2^-16-resolution position (exact float, the ADR 0002 domain).
    EXPECT_GE(s.x, 0.0f);
    EXPECT_LT(s.x, static_cast<float>(kGrid));
    EXPECT_GE(s.y, 0.0f);
    EXPECT_LT(s.y, static_cast<float>(kGrid));
    // The sprite stands on its tile: floor(x) = rawX >> 16 exactly
    // (the position's raw domain), and its step height is that tile's.
    const std::uint32_t tx = static_cast<std::uint32_t>(s.x);
    const std::uint32_t ty = static_cast<std::uint32_t>(s.y);
    EXPECT_EQ(s.stepHeight,
              scene.tileHeights[ty * static_cast<std::uint32_t>(kGrid) + tx]);
    // The plan section's domains.
    EXPECT_GE(s.atlasId, kIsoRefSceneSpriteAtlasBase);
    EXPECT_LT(s.atlasId, kIsoRefSceneSpriteAtlasBase + 4u);
    EXPECT_LE(s.blend, 1u);
    EXPECT_LT(s.frame, kIsoRefSceneSpriteSheetFrames);
    EXPECT_GE(s.stepHeight, 0);
    EXPECT_LE(s.stepHeight, kMaxHeight);
    EXPECT_EQ(s.stepHeight % 2, 0);  // the step model: even heights
  }
}

TEST(IsoReferenceSceneBuilder, TerrainInvariantsSmall) {
  const IsoReferenceScene scene =
      buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small);
  constexpr int kGrid = 32;
  constexpr int kPlateau = 8;
  constexpr int kMaxHeight = (24 - 1) * 2;
  // The step model: even heights in [0, kMaxHeight].
  for (const std::int32_t h : scene.tileHeights) {
    EXPECT_GE(h, 0);
    EXPECT_LE(h, kMaxHeight);
    EXPECT_EQ(h % 2, 0);
  }
  // The plateau uniformity (one height per plateau, row-major).
  const std::uint32_t perAxis = static_cast<std::uint32_t>(kGrid / kPlateau);
  for (std::uint32_t py = 0; py < perAxis; ++py) {
    for (std::uint32_t px = 0; px < perAxis; ++px) {
      const std::int32_t plateauHeight = scene.tileHeights[
          (py * kPlateau) * kGrid + (px * kPlateau)];
      for (std::uint32_t ty = py * kPlateau; ty < (py + 1) * kPlateau; ++ty) {
        for (std::uint32_t tx = px * kPlateau; tx < (px + 1) * kPlateau; ++tx) {
          EXPECT_EQ(scene.tileHeights[ty * kGrid + tx], plateauHeight);
        }
      }
    }
  }
  // The worst-case property: the terrain is stepped, not flat (a
  // uniform plateau would make every sprite's depth key tie — the
  // reference scene must exercise the sort's tie-breaking; 256
  // PRNG-drawn plateaus over 24 steps differ with certainty).
  bool distinct = false;
  for (std::uint32_t i = 1; i < scene.tileHeights.size() && !distinct; ++i) {
    if (scene.tileHeights[i] != scene.tileHeights[0]) distinct = true;
  }
  EXPECT_TRUE(distinct);
}

// ---------------------------------------------------------------------------
// The format (the round trip + the untrusted-input validation)
// ---------------------------------------------------------------------------

TEST(IsoReferenceSceneFormat, RoundTripBothVariants) {
  for (IsoRefSceneVariant variant : {IsoRefSceneVariant::Small,
                                     IsoRefSceneVariant::Full}) {
    const IsoReferenceScene scene = buildIsoReferenceScene(kTestSeed, variant);
    const std::vector<std::uint8_t> bytes = serializeIsoReferenceScene(scene);
    const auto parsed =
        readIsoReferenceScene(std::span<const std::uint8_t>(bytes));
    ASSERT_TRUE(parsed.ok()) << "variant " << static_cast<int>(variant);
    const IsoReferenceScene& back = *parsed.valueIfOk();
    EXPECT_TRUE(scenesEqual(scene, back));
  }
}

TEST(IsoReferenceSceneFormat, BadVersionRejected) {
  std::vector<std::uint8_t> bytes =
      serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small));
  bytes[0] = static_cast<std::uint8_t>(kIsoRefSceneFormatVersion + 1u);
  const auto parsed =
      readIsoReferenceScene(std::span<const std::uint8_t>(bytes));
  ASSERT_TRUE(parsed.isError());
  EXPECT_EQ(parsed.error(), ErrorCode::MalformedInput);
}

TEST(IsoReferenceSceneFormat, TruncatedRejected) {
  std::vector<std::uint8_t> bytes =
      serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small));
  bytes.pop_back();  // a truncated last widget byte
  const auto parsed =
      readIsoReferenceScene(std::span<const std::uint8_t>(bytes));
  ASSERT_TRUE(parsed.isError());
  EXPECT_EQ(parsed.error(), ErrorCode::MalformedInput);
}

TEST(IsoReferenceSceneFormat, TrailingBytesRejected) {
  std::vector<std::uint8_t> bytes =
      serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small));
  bytes.push_back(0u);  // the exact-length contract: extra bytes are not this scene
  const auto parsed =
      readIsoReferenceScene(std::span<const std::uint8_t>(bytes));
  ASSERT_TRUE(parsed.isError());
  EXPECT_EQ(parsed.error(), ErrorCode::MalformedInput);
}

TEST(IsoReferenceSceneFormat, CorruptedSpriteCountRejected) {
  std::vector<std::uint8_t> bytes =
      serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small));
  // The spriteCount field (offset 48, little-endian) beyond the 50k cap.
  bytes[48] = 0xFFu;
  bytes[49] = 0xFFu;
  bytes[50] = 0xFFu;
  bytes[51] = 0xFFu;
  const auto parsed =
      readIsoReferenceScene(std::span<const std::uint8_t>(bytes));
  ASSERT_TRUE(parsed.isError());
  EXPECT_EQ(parsed.error(), ErrorCode::MalformedInput);
}

TEST(IsoReferenceSceneFormat, CorruptedHeightRejected) {
  std::vector<std::uint8_t> bytes =
      serializeIsoReferenceScene(buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small));
  // The first tile height (offset 68, little-endian) beyond the M2-ISO-01
  // |z| domain.
  bytes[68] = 0x7Fu;
  bytes[69] = 0x7Fu;
  bytes[70] = 0xFFu;
  bytes[71] = 0xFFu;
  const auto parsed =
      readIsoReferenceScene(std::span<const std::uint8_t>(bytes));
  ASSERT_TRUE(parsed.isError());
  EXPECT_EQ(parsed.error(), ErrorCode::MalformedInput);
}

TEST(IsoReferenceSceneFormat, EmptyInputRejected) {
  const auto parsed = readIsoReferenceScene(std::span<const std::uint8_t>{});
  ASSERT_TRUE(parsed.isError());
  EXPECT_EQ(parsed.error(), ErrorCode::MalformedInput);
}

// ---------------------------------------------------------------------------
// The world (load + one declare frame — both SimMath backends)
// ---------------------------------------------------------------------------

template <typename Backend>
void loadAndBatchVariant(IsoRefSceneVariant variant) {
  const IsoReferenceScene scene = buildIsoReferenceScene(kTestSeed, variant);
  auto worldResult = loadIsoReferenceScene<Backend>(scene);
  ASSERT_TRUE(worldResult.ok())
      << "load failed for variant " << static_cast<int>(variant);
  auto world = std::move(worldResult).takeValue();
  ASSERT_TRUE(world.valid());
  // The scene round-trips into the world (the owned copy).
  EXPECT_EQ(world.scene().seed, scene.seed);
  EXPECT_EQ(world.scene().spriteCount, scene.spriteCount);

  // The particle lifecycle: the emitters run 10 sim ticks (the
  // M2-PART-01 once-per-tick contract), then the live count is the
  // frame's particle contribution (deterministic — the system's Prng
  // is seeded from the scene's seed).
  for (int t = 0; t < 10; ++t) {
    world.particles().update();
  }
  const std::uint32_t live =
      static_cast<std::uint32_t>(world.particles().liveParticles().size());

  // The first-frame parallax quads (the wrap contract): bg auto ->
  // 4, mid manual -> 1, fg auto -> 4.
  const std::uint32_t expectedFrame = scene.spriteCount + scene.tileCount +
      kFirstFrameParallaxQuads + live;

  auto batcherResult =
      SpriteBatcher::create(SpriteBatcher::Options{expectedFrame});
  ASSERT_TRUE(batcherResult.ok());
  auto batcher = std::move(batcherResult).takeValue();
  batcher.beginFrame();
  const auto st = declareIsoReferenceScene<Backend>(
      world, batcher, Vec2{12.5f, 7.5f});
  ASSERT_TRUE(st.ok());
  ASSERT_TRUE(batcher.build().ok());

  EXPECT_EQ(batcher.frameCount(), expectedFrame);
  EXPECT_EQ(batcher.droppedTotal(), 0u);  // the capacity was exact
  EXPECT_EQ(batcher.overrideCount(), 0u);  // no depth overrides in the scene

  // The group expectations, derived from the scene's own data (the
  // header's plan section: bg/mid/ground/sprites/particles/fg).
  std::array<std::array<std::uint32_t, 2>, 4> spriteGroups{};
  for (const auto& s : scene.sprites) {
    spriteGroups[s.atlasId - kIsoRefSceneSpriteAtlasBase][s.blend]++;
  }
  std::uint32_t expectedBatches = 1u /* ground */ + 3u /* parallax */ +
                                  1u /* particles */;
  for (const auto& row : spriteGroups) {
    for (const std::uint32_t g : row) {
      if (g != 0u) ++expectedBatches;
    }
  }
  EXPECT_EQ(batcher.batchCount(), expectedBatches);
  // The PRD §8.1 draw-call budget (50k sprites, 3 parallax, UI <= 30).
  EXPECT_LE(batcher.batchCount(), 30u);

  // The per-batch instance counts + the in-group depth-key monotonicity
  // (M2-SORT-01: the batcher's back-to-front order).
  for (const SpriteBatch& batch : batcher.batches()) {
    std::uint32_t expected = 0u;
    if (batch.atlasId == kIsoRefSceneBgAtlas) {
      expected = 4u;
    } else if (batch.atlasId == kIsoRefSceneMidAtlas) {
      expected = 1u;
    } else if (batch.atlasId == kIsoRefSceneFgAtlas) {
      expected = 4u;
    } else if (batch.atlasId == kIsoRefSceneParticleAtlas) {
      expected = live;
    } else if (batch.atlasId == kIsoRefSceneGroundAtlas) {
      expected = scene.tileCount;
    } else {
      expected = spriteGroups[batch.atlasId - kIsoRefSceneSpriteAtlasBase][
          static_cast<std::uint32_t>(batch.blend)];
    }
    EXPECT_EQ(batch.instances.size(), expected);
    std::uint32_t prev = 0u;
    bool first = true;
    for (const std::uint32_t slot : batch.instances) {
      const SpriteItem& item = batcher.at(slot);
      if (!first) {
        EXPECT_LE(prev, item.depthKey);
      }
      prev = item.depthKey;
      first = false;
    }
  }
}

TEST(IsoReferenceSceneLoadAndBatch, SmallFpx16) {
  loadAndBatchVariant<Fpx16_16>(IsoRefSceneVariant::Small);
}
TEST(IsoReferenceSceneLoadAndBatch, SmallFp32) {
  loadAndBatchVariant<Fp32Pinned>(IsoRefSceneVariant::Small);
}
TEST(IsoReferenceSceneLoadAndBatch, FullFpx16) {
  loadAndBatchVariant<Fpx16_16>(IsoRefSceneVariant::Full);
}
TEST(IsoReferenceSceneLoadAndBatch, FullFp32) {
  loadAndBatchVariant<Fp32Pinned>(IsoRefSceneVariant::Full);
}

// ---------------------------------------------------------------------------
// The UI pass (the scene's 2 widgets on the reference screen)
// ---------------------------------------------------------------------------

template <typename Backend>
void uiPassVariant() {
  const IsoReferenceScene scene =
      buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small);
  auto worldResult = loadIsoReferenceScene<Backend>(scene);
  ASSERT_TRUE(worldResult.ok());
  auto world = std::move(worldResult).takeValue();
  ASSERT_TRUE(world.valid());

  // The tree: the implicit root + the scene's 2 widgets.
  EXPECT_EQ(world.ui().widgetCount(), 3u);
  ASSERT_TRUE(world.ui().layout().ok());

  // The pass WITHOUT a default font: the widget quads render (panel
  // + button), the button's label text does not (the ui_render.h
  // contract — a null default font skips the text pass, no error).
  auto batcherResult = SpriteBatcher::create(SpriteBatcher::Options{64u});
  ASSERT_TRUE(batcherResult.ok());
  auto batcher = std::move(batcherResult).takeValue();
  batcher.beginFrame();
  UiDeclareOptions opts{};  // defaultFont = null
  const auto st = declareUi(batcher, world.strings(), world.ui(), opts);
  ASSERT_TRUE(st.ok());
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_EQ(batcher.frameCount(), 2u);  // the panel quad + the button quad
  // Both quads share (atlas 100, material 0, Alpha) -> ONE batch.
  EXPECT_EQ(batcher.batchCount(), 1u);
  // The two depth overrides (the M2-UI-02 UI-plane keys).
  EXPECT_EQ(batcher.overrideCount(), 2u);
}

TEST(IsoReferenceSceneUiPass, SmallFpx16) { uiPassVariant<Fpx16_16>(); }
TEST(IsoReferenceSceneUiPass, SmallFp32) { uiPassVariant<Fp32Pinned>(); }

// ---------------------------------------------------------------------------
// The zero-allocation declare loop (PERF-003 — the §8.1 workload's
// per-frame cost at the CPU stage)
// ---------------------------------------------------------------------------

template <typename Backend>
void declareLoopAllocatesNothing() {
  const IsoReferenceScene scene =
      buildIsoReferenceScene(kTestSeed, IsoRefSceneVariant::Small);
  auto worldResult = loadIsoReferenceScene<Backend>(scene);
  ASSERT_TRUE(worldResult.ok());
  auto world = std::move(worldResult).takeValue();
  ASSERT_TRUE(world.valid());
  for (int t = 0; t < 10; ++t) {
    world.particles().update();
  }
  const std::uint32_t live =
      static_cast<std::uint32_t>(world.particles().liveParticles().size());
  const std::uint32_t capacity = scene.spriteCount + scene.tileCount +
      kFirstFrameParallaxQuads + live;
  auto batcherResult =
      SpriteBatcher::create(SpriteBatcher::Options{capacity});
  ASSERT_TRUE(batcherResult.ok());
  auto batcher = std::move(batcherResult).takeValue();

  // The 1000-frame loop (the M2-SPRITE-01 zero-alloc test's shape): a
  // moving camera (the parallax auto layers scroll every frame — the
  // declared quads stay within the split budget, the wrap contract),
  // one declare + one build per frame. The sanitizer trees no-op the
  // watch (their instrumentation is the check); the clean trees
  // assert zero heap allocations in the window.
  laige::allocWatchArm();
  for (int frame = 0; frame < 1000; ++frame) {
    batcher.beginFrame();
    const auto st = declareIsoReferenceScene<Backend>(
        world, batcher, Vec2{128.0f, 96.0f});
    ASSERT_TRUE(st.ok());
    ASSERT_TRUE(batcher.build().ok());
  }
  const laige::AllocWatchReading reading = laige::allocWatchRead();
  if (laige::allocWatchLive()) {
    EXPECT_EQ(reading.allocs, 0u);
  }
}

TEST(IsoReferenceSceneZeroAlloc, SmallFpx16) {
  declareLoopAllocatesNothing<Fpx16_16>();
}
TEST(IsoReferenceSceneZeroAlloc, SmallFp32) {
  declareLoopAllocatesNothing<Fp32Pinned>();
}
