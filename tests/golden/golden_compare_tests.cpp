// laige-golden tests (M2-GOLD-01): the GL-free half of the golden
// image test infrastructure.
//
// The contract pinned against: the frame comparison core
// (golden_compare.h — the documented tolerance model: the per-pixel
// max-channel test, the pixel budget, the frame pass/fail decision,
// the bounded failing-pixel samples — TEST-008's explicit-tolerance
// requirement), and the committed golden scene fixtures
// (golden_fixtures.h — the M2-SPRITE-02 and M2-UI-02 scenes' shape
// invariants at the canonical 1280x720 frame: the item/quad counts,
// the (atlas, material, blend) groups, the fixed camera matrices, the
// clear colors — the fixture drift a golden failure must never
// swallow silently).
//
// The suite map (the `golden_compare` CTest entry selects exactly
// these; no GL anywhere — the offscreen render + PNG comparison is
// the tool's job, ctest entry `golden`):
//   GoldenCompare    the comparison core (the tolerance model + the
//                    budget boundary + the sample bounds);
//   GoldenFixtures   the scene shape invariants (both fixtures).
//
// The GoldenFixtures UI half reads the committed test font
// (tests/laige-render/assets/vera.ttf — the LAIGE_TEST_FONT_PATH env
// var, CTest ENVIRONMENT), like the font-backed UI suites.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "golden_compare.h"
#include "golden_fixtures.h"
#include "laige/errors.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_batcher.h"
#include "laige/result.h"
#include "laige_test_seed.h"

namespace {

using laige::render::BlendMode;
using laige::render::Mat4;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using laige::render::SpriteItem;
using laige::testing::golden::Comparison;
using laige::testing::golden::Tolerance;
using laige::testing::golden::kGoldenHeight;
using laige::testing::golden::kGoldenWidth;
using laige::testing::golden::buildSpriteScene;
using laige::testing::golden::buildUiScene;
using laige::testing::golden::compareFrames;
using laige::testing::golden::uiAtlasRgba;

// The test font (the house loadFont pattern, ui_render_tests.cpp).
std::vector<std::uint8_t> loadFont() {
  const std::string path = laige::testing::ReadEnvVar("LAIGE_TEST_FONT_PATH");
  if (path.empty()) {
    ADD_FAILURE() << "LAIGE_TEST_FONT_PATH is not set (CTest ENVIRONMENT "
                     "contract — see tests/golden/CMakeLists.txt)";
    return {};
  }
  std::FILE* f = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&f, path.c_str(), "rb") != 0) {
    ADD_FAILURE() << "Cannot open the test font: " << path;
    return {};
  }
#else
  f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    ADD_FAILURE() << "Cannot open the test font: " << path;
    return {};
  }
#endif
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (got != bytes.size()) {
    ADD_FAILURE() << "Short read of the test font: " << path;
    return {};
  }
  return bytes;
}

// A flat RGBA8 frame of `w x h` pixels (top-origin / y-down), all
// pixels the same color.
std::vector<std::uint8_t> frame(std::uint32_t w, std::uint32_t h,
                                std::uint8_t r, std::uint8_t g,
                                std::uint8_t b, std::uint8_t a) {
  std::vector<std::uint8_t> f(static_cast<std::size_t>(w) *
                                  static_cast<std::size_t>(h) * 4u);
  for (std::size_t p = 0; p < f.size() / 4u; ++p) {
    f[p * 4 + 0] = r;
    f[p * 4 + 1] = g;
    f[p * 4 + 2] = b;
    f[p * 4 + 3] = a;
  }
  return f;
}

void setPixel(std::vector<std::uint8_t>& f, std::uint32_t w, std::uint32_t x,
              std::uint32_t y, std::uint8_t r, std::uint8_t g,
              std::uint8_t b, std::uint8_t a) {
  const std::size_t p =
      (static_cast<std::size_t>(y) * w + x) * 4u;
  f[p + 0] = r;
  f[p + 1] = g;
  f[p + 2] = b;
  f[p + 3] = a;
}

}  // namespace

// ---------------------------------------------------------------------------
// GoldenCompare — the comparison core (GL-free)
// ---------------------------------------------------------------------------

TEST(GoldenCompare, DocumentedDefaults) {
  const Tolerance t{};
  EXPECT_EQ(2, t.maxChannelDiff);
  EXPECT_DOUBLE_EQ(0.001, t.pixelBudgetPct);
}

TEST(GoldenCompare, IdenticalFramesPass) {
  const auto e = frame(8, 8, 10, 20, 30, 40);
  const auto c = compareFrames(e, e, 8, 8, Tolerance{});
  EXPECT_EQ(64u, c.totalPixels);
  EXPECT_EQ(0u, c.failingPixels);
  EXPECT_EQ(0, c.maxObservedDiff);
  EXPECT_TRUE(c.pass());
}

TEST(GoldenCompare, ExactMaxDiffPasses) {
  // A pixel at exactly the tolerance's max channel difference passes
  // (the test is inclusive: diff > maxChannelDiff fails).
  auto e = frame(8, 8, 100, 100, 100, 255);
  auto a = e;
  setPixel(a, 8, 3, 4, 102, 100, 100, 255);  // max diff = 2
  const auto c = compareFrames(e, a, 8, 8, Tolerance{});
  EXPECT_EQ(0u, c.failingPixels);
  EXPECT_EQ(2, c.maxObservedDiff);
  EXPECT_TRUE(c.pass());
}

TEST(GoldenCompare, OverMaxDiffFails) {
  auto e = frame(8, 8, 100, 100, 100, 255);
  auto a = e;
  setPixel(a, 8, 3, 4, 103, 100, 100, 255);  // max diff = 3
  const auto c = compareFrames(e, a, 8, 8, Tolerance{});
  // 8x8 = 64 px; 0.001% -> floor(0.00064) = 0 tolerated.
  EXPECT_EQ(0u, c.budget);
  EXPECT_EQ(1u, c.failingPixels);
  EXPECT_EQ(3, c.maxObservedDiff);
  ASSERT_EQ(1u, c.samples.size());
  EXPECT_EQ(3, c.samples[0].x);
  EXPECT_EQ(4, c.samples[0].y);
  EXPECT_EQ(100, c.samples[0].expected[0]);
  EXPECT_EQ(103, c.samples[0].actual[0]);
  EXPECT_EQ(3, c.samples[0].maxDiff);
  EXPECT_FALSE(c.pass());
}

TEST(GoldenCompare, MaxOverChannelsNotSum) {
  // A pixel whose four channels differ by (1, 1, 1, 4) fails on the
  // MAX channel (4 > 2), not the sum.
  auto e = frame(8, 8, 100, 100, 100, 100);
  auto a = e;
  setPixel(a, 8, 0, 0, 101, 101, 101, 104);
  const auto c = compareFrames(e, a, 8, 8, Tolerance{});
  EXPECT_EQ(1u, c.failingPixels);
  EXPECT_EQ(4, c.maxObservedDiff);
  EXPECT_FALSE(c.pass());
}

TEST(GoldenCompare, BudgetBoundary) {
  // 200x200 = 40 000 px; a 0.01% budget is floor(4.0) = 4 pixels.
  const Tolerance t{2, 0.01};
  auto e = frame(200, 200, 0, 0, 0, 255);
  auto a = e;
  for (std::uint32_t x = 0; x < 4; ++x) {
    setPixel(a, 200, x, 0, 10, 0, 0, 255);  // 4 failing pixels
  }
  const auto c4 = compareFrames(e, a, 200, 200, t);
  EXPECT_EQ(4u, c4.budget);
  EXPECT_EQ(4u, c4.failingPixels);
  EXPECT_TRUE(c4.pass());
  setPixel(a, 200, 4, 0, 10, 0, 0, 255);  // one more
  const auto c5 = compareFrames(e, a, 200, 200, t);
  EXPECT_EQ(5u, c5.failingPixels);
  EXPECT_FALSE(c5.pass());
}

TEST(GoldenCompare, SamplesBounded) {
  // 100 failing pixels, 10 sampled (the first, in scan order).
  const Tolerance t{0, 0.001};
  auto e = frame(10, 10, 0, 0, 0, 255);
  auto a = e;
  for (std::uint32_t y = 0; y < 10; ++y) {
    for (std::uint32_t x = 0; x < 10; ++x) {
      setPixel(a, 10, x, y, 5, 0, 0, 255);
    }
  }
  const auto c = compareFrames(e, a, 10, 10, t, 10);
  EXPECT_EQ(100u, c.failingPixels);
  EXPECT_EQ(10u, c.samples.size());
  EXPECT_EQ(0, c.samples[0].x);
  EXPECT_EQ(0, c.samples[0].y);
  EXPECT_EQ(9, c.samples[9].x);
  EXPECT_EQ(0, c.samples[9].y);
  EXPECT_FALSE(c.pass());
}

// ---------------------------------------------------------------------------
// GoldenFixtures — the committed scene shape invariants (GL-free)
// ---------------------------------------------------------------------------

TEST(GoldenFixtures, CanonicalFrame) {
  EXPECT_EQ(1280u, kGoldenWidth);
  EXPECT_EQ(720u, kGoldenHeight);
}

TEST(GoldenFixtures, SpriteSceneShape) {
  auto r = buildSpriteScene();
  ASSERT_TRUE(r.ok());
  const auto scene = std::move(r).takeValue();
  // 1000 items in declaration order (997 lattice + 3 probes LAST).
  ASSERT_EQ(1000u, scene.items.size());
  EXPECT_EQ(16u, scene.atlas0.size() / 4u);  // the 4x4 atlas0
  EXPECT_EQ(16u, scene.atlas1.size() / 4u);  // the 4x4 atlas1
  // The groups, in the batcher's ascending (atlas, material, blend)
  // order: (0, 0, Alpha) 598, (0, 0, Additive) 201, (1, 0, Alpha) 201.
  std::uint32_t gAlpha0 = 0, gAdd0 = 0, gAlpha1 = 0;
  for (const SpriteItem& it : scene.items) {
    if (it.atlasId == 0 && it.blend == BlendMode::Alpha) {
      ++gAlpha0;
    } else if (it.atlasId == 0 && it.blend == BlendMode::Additive) {
      ++gAdd0;
    } else if (it.atlasId == 1 && it.blend == BlendMode::Alpha) {
      ++gAlpha1;
    } else {
      ADD_FAILURE() << "unexpected (atlas, blend) pair";
    }
  }
  EXPECT_EQ(598u, gAlpha0);
  EXPECT_EQ(201u, gAdd0);
  EXPECT_EQ(201u, gAlpha1);
  // The lattice cell (x, y) = (-16, -16) is declared first (i = 0).
  EXPECT_EQ(-16.0f, scene.items.front().pos.x);
  EXPECT_EQ(-16.0f, scene.items.front().pos.y);
  // The probes are declared LAST, at their exact cells.
  const SpriteItem p0 = scene.items[997];
  const SpriteItem p1 = scene.items[998];
  const SpriteItem p2 = scene.items[999];
  EXPECT_EQ(0.0f, p0.pos.x);
  EXPECT_EQ(0.0f, p0.pos.y);
  EXPECT_EQ(0.0f, p1.pos.x);
  EXPECT_EQ(1.0f, p1.pos.y);
  EXPECT_EQ(0.0f, p2.pos.x);
  EXPECT_EQ(-1.0f, p2.pos.y);
  EXPECT_EQ(BlendMode::Additive, p1.blend);
  EXPECT_EQ(1u, p2.atlasId);
  // The fixed camera: the ADR 0005 preset at k = 0.125, bit-exact
  // against the M2-GL-03 builder.
  EXPECT_EQ(laige::render::isoDimetric2To1(laige::testing::golden::
                                               kSpritePresetScale),
            scene.matrix);
  // The clear color (bytes 0, 0, 255).
  EXPECT_FLOAT_EQ(0.0f, scene.clearR);
  EXPECT_FLOAT_EQ(0.0f, scene.clearG);
  EXPECT_FLOAT_EQ(1.0f, scene.clearB);
  EXPECT_FLOAT_EQ(1.0f, scene.clearA);
}

TEST(GoldenFixtures, UiSceneShape) {
  const auto font = loadFont();
  ASSERT_FALSE(font.empty());
  auto r = buildUiScene(font);
  ASSERT_TRUE(r.ok());
  auto scene = std::move(r).takeValue();
  // The 7 quads and the 2 groups, declared GL-free through the real
  // UI pass (the M2-UI-02 declare contract — no GL needed).
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(
      laige::render::declareUi(batcher, scene.table, scene.tree,
                               scene.declareOptions())
          .ok());
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_EQ(7u, batcher.frameCount());
  EXPECT_EQ(2u, batcher.batchCount());
  // The group order: (7, 0, Alpha) 5 instances, then (9, 0, Alpha) 2.
  const auto batches = batcher.batches();
  ASSERT_EQ(2u, batches.size());
  EXPECT_EQ(7u, batches[0].atlasId);
  EXPECT_EQ(5u, batches[0].instances.size());
  EXPECT_EQ(9u, batches[1].atlasId);
  EXPECT_EQ(2u, batches[1].instances.size());
  EXPECT_EQ(BlendMode::Alpha, batches[0].blend);
  EXPECT_EQ(BlendMode::Alpha, batches[1].blend);
  // The first quad is the panel: centered at (30, 25), 40x30, uv texel
  // (0, 0), atlas 7 (the layout is the smoke fixture's absolute
  // positions — unchanged by the larger screen rect).
  const SpriteItem panel = batcher.at(batches[0].instances[0]);
  EXPECT_EQ(30.0f, panel.pos.x);
  EXPECT_EQ(25.0f, panel.pos.y);
  EXPECT_EQ(40.0f, panel.scale.x);
  EXPECT_EQ(30.0f, panel.scale.y);
  EXPECT_EQ(0.0f, panel.uv.u0);
  EXPECT_EQ(0.25f, panel.uv.u1);
  EXPECT_EQ(7u, panel.atlasId);
  // The fixed camera: the identity screen matrix at the canonical
  // frame, bit-exact against the M2-UI-02 builder.
  EXPECT_EQ(laige::render::uiScreenMatrix(
                static_cast<std::int32_t>(kGoldenWidth),
                static_cast<std::int32_t>(kGoldenHeight)),
            scene.matrix);
  // The clear color (bytes 0, 0, 0).
  EXPECT_FLOAT_EQ(0.0f, scene.clearR);
  EXPECT_FLOAT_EQ(0.0f, scene.clearG);
  EXPECT_FLOAT_EQ(0.0f, scene.clearB);
  EXPECT_FLOAT_EQ(1.0f, scene.clearA);
  // The atlas uploads: the 4x4 UI atlas (256 bytes) and the 1024x1024
  // white-on-alpha font atlas.
  EXPECT_EQ(64u, uiAtlasRgba().size());
  EXPECT_EQ(1024u * 1024u * 4u, scene.fontRgba.size());
}
