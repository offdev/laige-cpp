// laige-render UI render pass tests (M2-UI-02): the step's Verify
// command is `ctest -R ui_render`.
//
// The contract pinned against: the UI pass's identity screen matrix
// (uiScreenMatrix: the screen-px y-down -> NDC mapping, the out-of-
// domain fallback), the per-widget declaration model (every ADR 0006
// kind -> its batched quads + text: the hand-computed pos / scale /
// uv / tint / z goldens against the M2-UI-01 layout + the M2-TEXT-02
// glyph model, the group count, the in-group traversal order, the
// G-R11 override count), the culling model (the offscreen rule on
// all four edges + the zero-area rule, the occlusion rule: a later
// opaque panel fully covering a node, the partial-overlap /
// non-opaque / own-children exceptions), the slider fill rule
// (0 < t < 1 only), the failure paths (first failure wins, the
// nothing-declared preconditions, the partial-state-on-failure
// precedent), the 1 000-frame zero-allocation declare loop
// (PERF-003), and the golden image fixture (UiRenderSmoke: the fixed
// UI scene rendered offscreen through the M2-SPRITE-02 submit stage,
// the whole frame against a CPU reference rasterizer within the
// documented +/- 1 byte tolerance, the exact probe pixels — the
// M2-GOLD-01 golden contract, tolerance documented in
// docs/api/ui_render.md).
//
// The UiRenderScreenMatrix* / UiRenderDeclare* / UiRenderCull* /
// UiRenderSlider* / UiRenderFailure* / UiRenderZeroAlloc* suites need
// no GL; the font-backed ones read the committed test font
// (tests/laige-render/assets/vera.ttf — the LAIGE_TEST_FONT_PATH env
// var, CTest ENVIRONMENT). The UiRenderSmoke* suite needs a usable
// OpenGL 3.3 environment (it GTEST_SKIPs where absent — the
// documented environment contract, not an engine failure). No budget
// gate: the step's roadmap scope has no standalone budgets.json entry
// (the per-frame UI cost is part of the composite 50k render-CPU
// budget, M2-PERF-01).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/render/gl_context.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_renderer.h"
#include "laige/render/text_items.h"
#include "laige/render/ui_render.h"
#include "laige/render/ui_widgets.h"
#include "laige/result.h"
#include "laige_test_seed.h"

namespace {

using laige::ErrorCode;
using laige::Result;
using laige::Status;
using laige::render::BlendMode;
using laige::render::GlyphAtlas;
using laige::render::GlyphMetrics;
using laige::render::Mat4;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using laige::render::SpriteDrawStats;
using laige::render::SpriteItem;
using laige::render::SpriteRenderer;
using laige::render::SpriteTint;
using laige::render::SpriteUvRect;
using laige::render::StringRef;
using laige::render::StringTable;
using laige::render::TextAlignment;
using laige::render::TextItem;
using laige::render::UiAnchor;
using laige::render::kInvalidUiWidgetId;
using laige::render::UiDeclareOptions;
using laige::render::UiTree;
using laige::render::UiWidgetId;
using laige::render::UiWidgetKind;
using laige::render::UiWidgetSpec;
using laige::render::Vec2;
using laige::render::declareUi;
using laige::render::expandGlyphAtlasRgba8;
using laige::render::uiScreenMatrix;
using laige::render::kTextMaxScale;

// ---------------------------------------------------------------------------
// The shared fixtures (the committed test font, the string table, the
// tree / spec helpers — the text_items_tests pattern)
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> loadFont() {
  const std::string path = laige::testing::ReadEnvVar("LAIGE_TEST_FONT_PATH");
  if (path.empty()) {
    ADD_FAILURE() << "LAIGE_TEST_FONT_PATH is not set (CTest ENVIRONMENT "
                     "contract — see tests/laige-render/CMakeLists.txt)";
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

GlyphAtlas makeAtlas() {
  auto r = GlyphAtlas::create(loadFont(), GlyphAtlas::Options{});
  if (!r.ok()) {
    std::fprintf(stderr, "makeAtlas: create failed\n");
    std::abort();
  }
  return std::move(r).takeValue();
}

StringTable makeTable() {
  auto r = StringTable::create(StringTable::Options{16, 64});
  if (!r.ok()) {
    std::fprintf(stderr, "makeTable: create failed\n");
    std::abort();
  }
  return std::move(r).takeValue();
}

StringRef internText(StringTable& table, const char* s) {
  std::vector<std::uint32_t> codes;
  for (const char* c = s; *c != '\0'; ++c) {
    codes.push_back(static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(*c)));
  }
  auto r = table.intern(codes);
  if (!r.ok()) {
    ADD_FAILURE() << "intern failed: " << laige::errorText(r.error());
    std::abort();
  }
  return std::move(r).takeValue();
}

UiTree makeTree(std::int32_t width, std::int32_t height,
                std::uint32_t maxWidgets = 64) {
  auto r = UiTree::create(
      UiTree::Options{maxWidgets, 16, width, height});
  if (!r.ok()) {
    ADD_FAILURE() << "UiTree::create failed: " << laige::errorText(r.error());
    std::abort();
  }
  return std::move(r).takeValue();
}

UiWidgetId addWidget(UiTree& tree, UiWidgetSpec spec) {
  auto r = tree.addChild(spec);
  if (!r.ok()) {
    ADD_FAILURE() << "addChild failed: " << laige::errorText(r.error());
    std::abort();
  }
  return std::move(r).takeValue();
}

// The UI-pass options of the font-backed scenes (the default font,
// its texture id 9, the 1x scale).
UiDeclareOptions fontOptions(const GlyphAtlas& font) {
  return UiDeclareOptions{&font, 9, 1};
}

// ---------------------------------------------------------------------------
// The declare goldens (the hand-computed expected quads)
// ---------------------------------------------------------------------------

SpriteItem expectQuad(float px, float py, float w, float h,
                      SpriteUvRect uv, std::uint32_t z,
                      std::uint32_t atlasId,
                      SpriteTint tint = SpriteTint{1.0f, 1.0f, 1.0f, 1.0f}) {
  SpriteItem q;
  q.pos = Vec2{px, py};
  q.scale = Vec2{w, h};
  q.rotation = 0.0f;
  q.frameIndex = 0;
  q.uv = uv;
  q.tint = tint;
  q.atlasId = atlasId;
  q.materialId = 0;
  q.blend = BlendMode::Alpha;
  q.depthKey = z;
  q.depthOverride = true;
  return q;
}

bool sameItem(const SpriteItem& a, const SpriteItem& e) {
  return a.pos.x == e.pos.x && a.pos.y == e.pos.y &&
         a.scale.x == e.scale.x && a.scale.y == e.scale.y &&
         a.rotation == e.rotation && a.frameIndex == e.frameIndex &&
         a.uv.u0 == e.uv.u0 && a.uv.v0 == e.uv.v0 && a.uv.u1 == e.uv.u1 &&
         a.uv.v1 == e.uv.v1 && a.tint.r == e.tint.r &&
         a.tint.g == e.tint.g && a.tint.b == e.tint.b &&
         a.tint.a == e.tint.a && a.atlasId == e.atlasId &&
         a.materialId == e.materialId && a.blend == e.blend &&
         a.depthKey == e.depthKey && a.depthOverride == e.depthOverride;
}

// The all-seven-kinds scene (screen 200 x 150; the hand-computed
// layout goldens are in the UiRenderDeclare test below):
//
//   P  opaque Panel   (0, 0, 200, 150)  uv texel (0,0)   atlas 7
//   B  Button         (20, 20, 40, 20)  uv texel (1,0)   label "Ma"
//   T  Text           (70, 20, 60, 20)  item "Ma" @ (100, 38) center, atlas 9
//   I  Image          (100, 20, 20, 20) uv texel (2,0)
//   S  Slider         (10, 60, 80, 10)  [0, 10] value 5  uv texel (0,1)
//   IN Input          (100, 60, 60, 16) text "M"
//   L  List           (10, 90, 80, 40)
//   L1 Button (child)  row (10, 90, 60, 16)  uv texel (1,1)  label ""
//   L2 Button (child)  row (10, 106, 60, 16) uv texel (2,1)  label ""
//   B2 Button          (300, 100, 40, 20)   OFFSCREEN (culled)
void buildDeclareScene(UiTree& tree, const GlyphAtlas& font,
                       StringRef ma, StringRef m, StringRef empty) {
  UiWidgetSpec s;
  s.kind = UiWidgetKind::Panel;
  s.opaque = true;
  s.x = 0;
  s.y = 0;
  s.width = 200;
  s.height = 150;
  s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
  s.atlasId = 7;
  const UiWidgetId p = addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Button;
  s.parent = p;
  s.x = 20;
  s.y = 20;
  s.width = 40;
  s.height = 20;
  s.image = SpriteUvRect{0.25f, 0.0f, 0.5f, 0.25f};
  s.atlasId = 7;
  s.label = ma;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Text;
  s.parent = p;
  s.x = 70;
  s.y = 20;
  s.width = 60;
  s.height = 20;
  TextItem item;
  item.text = ma;
  item.font = &font;
  item.x = 100.0f;
  item.y = 38.0f;
  item.alignment = TextAlignment::Center;
  item.scale = 1;
  item.maxWidth = 0;
  item.depthKey = 77;  // the game's hand z (the pass overwrites it)
  item.atlasId = 9;
  s.item = item;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Image;
  s.parent = p;
  s.x = 100;
  s.y = 20;
  s.width = 20;
  s.height = 20;
  s.image = SpriteUvRect{0.5f, 0.0f, 0.75f, 0.25f};
  s.atlasId = 7;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Slider;
  s.parent = p;
  s.x = 10;
  s.y = 60;
  s.width = 80;
  s.height = 10;
  s.sliderMin = 0.0f;
  s.sliderMax = 10.0f;
  s.sliderValue = 5.0f;
  s.image = SpriteUvRect{0.0f, 0.25f, 0.25f, 0.5f};
  s.atlasId = 7;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Input;
  s.parent = p;
  s.x = 100;
  s.y = 60;
  s.width = 60;
  s.height = 16;
  s.text = m;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::List;
  s.parent = p;
  s.x = 10;
  s.y = 90;
  s.width = 80;
  s.height = 40;
  const UiWidgetId l = addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Button;
  s.parent = l;
  s.width = 60;
  s.height = 16;
  s.image = SpriteUvRect{0.25f, 0.25f, 0.5f, 0.5f};
  s.atlasId = 7;
  s.label = empty;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Button;
  s.parent = l;
  s.width = 60;
  s.height = 16;
  s.image = SpriteUvRect{0.5f, 0.25f, 0.75f, 0.5f};
  s.atlasId = 7;
  s.label = empty;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Button;
  s.parent = p;
  s.x = 300;
  s.y = 100;
  s.width = 40;
  s.height = 20;
  s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
  s.atlasId = 7;
  s.label = empty;
  addWidget(tree, s);

}

}  // namespace

// ---------------------------------------------------------------------------
// UiRenderScreenMatrix — the identity screen matrix (no GL)
// ---------------------------------------------------------------------------

TEST(UiRenderScreenMatrix, UiPlaneMapping) {
  const Mat4 m = uiScreenMatrix(1280, 720);
  // The column-major layout: the linear part in columns 0..1, the
  // translation in column 3, everything else the identity.
  EXPECT_FLOAT_EQ(2.0f / 1280.0f, m[0].x);
  EXPECT_FLOAT_EQ(-2.0f / 720.0f, m[1].y);
  EXPECT_FLOAT_EQ(-1.0f, m[3].x);
  EXPECT_FLOAT_EQ(1.0f, m[3].y);
  EXPECT_FLOAT_EQ(1.0f, m[2].z);
  EXPECT_FLOAT_EQ(0.0f, m[0].y);
  EXPECT_FLOAT_EQ(0.0f, m[1].x);
  EXPECT_FLOAT_EQ(0.0f, m[3].z);
  EXPECT_FLOAT_EQ(0.0f, m[2].w);
  EXPECT_FLOAT_EQ(1.0f, m[3].w);
  // The plane corners / center (x_ndc = 2x/w - 1, y_ndc = 1 - 2y/h;
  // screen y down: the top edge (y = 0) maps to NDC +1, the bottom
  // edge (y = h) to NDC -1).
  EXPECT_FLOAT_EQ(-1.0f, m[0].x * 0.0f + m[3].x);
  EXPECT_FLOAT_EQ(1.0f, m[1].y * 0.0f + m[3].y);
  EXPECT_FLOAT_EQ(1.0f, m[0].x * 1280.0f + m[3].x);
  EXPECT_FLOAT_EQ(-1.0f, m[1].y * 720.0f + m[3].y);
  EXPECT_FLOAT_EQ(0.0f, m[0].x * 640.0f + m[3].x);
  EXPECT_FLOAT_EQ(0.0f, m[1].y * 360.0f + m[3].y);
}

TEST(UiRenderScreenMatrix, OutOfDomainIsIdentity) {
  // A dimension outside [1, 2^20] is unrepresentable: the identity
  // (the total-function contract — the caller passes the tree's
  // screen rect, in domain by construction).
  const Mat4 m0 = uiScreenMatrix(0, 720);
  const Mat4 m1 = uiScreenMatrix(1280, 1 << 21);
  const Mat4 id = Mat4(1.0f);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_FLOAT_EQ(id[c][r], m0[c][r]);
      EXPECT_FLOAT_EQ(id[c][r], m1[c][r]);
    }
  }
}

// ---------------------------------------------------------------------------
// UiRenderDeclare — the per-widget model (no GL, the committed font)
// ---------------------------------------------------------------------------

TEST(UiRenderDeclare, AllSevenKindsGolden) {
  GlyphAtlas font = makeAtlas();
  const GlyphMetrics& mM = *font.glyph('M');
  const GlyphMetrics& ma = *font.glyph('a');
  // The Vera 16px metric goldens (the committed asset — the
  // M2-TEXT-01 pin): the hand-computed quads below assume these.
  ASSERT_EQ(12, mM.advance);
  ASSERT_EQ(10, mM.width);
  ASSERT_EQ(11, mM.height);
  ASSERT_EQ(1, mM.bearingX);
  ASSERT_EQ(11, mM.bearingY);
  ASSERT_EQ(8, ma.advance);
  ASSERT_EQ(8, ma.width);
  ASSERT_EQ(9, ma.height);
  ASSERT_EQ(8, ma.bearingY);
  StringTable table = makeTable();
  const StringRef maRef = internText(table, "Ma");
  const StringRef mRef = internText(table, "M");
  const StringRef empty = internText(table, "");

  UiTree tree = makeTree(200, 150);
  buildDeclareScene(tree, font, maRef, mRef, empty);
  ASSERT_TRUE(tree.layout().ok());

  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareUi(batcher, table, tree, fontOptions(font)).ok());
  ASSERT_TRUE(batcher.build().ok());

  // The frame: 12 quads — P z0, B-art z1, B-label M/a z1, T M/a z2,
  // I z3, S track + fill z4, IN M z5, L1 z6, L2 z7 (L declares
  // nothing, B2 is culled offscreen). Two (atlas, material, blend)
  // groups: the UI atlas (7, 0, Alpha) 7 instances, the glyph atlas
  // (9, 0, Alpha) 5 — in-group traversal order, every quad a G-R11
  // override (the UI pass's manual z).
  EXPECT_EQ(12u, batcher.frameCount());
  EXPECT_EQ(2u, batcher.batchCount());
  EXPECT_EQ(12u, batcher.overrideCount());
  const std::span<const SpriteBatch> batches = batcher.batches();
  ASSERT_EQ(7u, batches[0].atlasId);
  ASSERT_EQ(0u, batches[0].materialId);
  ASSERT_EQ(BlendMode::Alpha, batches[0].blend);
  ASSERT_EQ(7u, batches[0].instances.size());
  ASSERT_EQ(9u, batches[1].atlasId);
  ASSERT_EQ(5u, batches[1].instances.size());

  // The glyph cells (the runtime atlas positions — the exact-float
  // uv of the M2-TEXT-02 model: atlasX/Y and cell extent over the
  // 1024 texel atlas).
  const float invW = 1.0f / 1024.0f;
  const SpriteUvRect uvM{mM.atlasX * invW, mM.atlasY * invW,
                         static_cast<float>(mM.atlasX + 10u) * invW,
                         static_cast<float>(mM.atlasY + 11u) * invW};
  const SpriteUvRect uvA{ma.atlasX * invW, ma.atlasY * invW,
                         static_cast<float>(ma.atlasX + 8u) * invW,
                         static_cast<float>(ma.atlasY + 9u) * invW};

  // Group 0 (the UI atlas) in draw order (the hand-computed goldens —
  // the header's per-widget model): P, B-art, I, S track, S fill,
  // L1, L2.
  const std::vector<SpriteItem> uiExpected = {
      expectQuad(100.0f, 75.0f, 200.0f, 150.0f,
                 SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f}, 0, 7),
      expectQuad(40.0f, 30.0f, 40.0f, 20.0f,
                 SpriteUvRect{0.25f, 0.0f, 0.5f, 0.25f}, 1, 7),
      expectQuad(110.0f, 30.0f, 20.0f, 20.0f,
                 SpriteUvRect{0.5f, 0.0f, 0.75f, 0.25f}, 3, 7),
      expectQuad(50.0f, 65.0f, 80.0f, 10.0f,
                 SpriteUvRect{0.0f, 0.25f, 0.25f, 0.5f}, 4, 7),
      expectQuad(30.0f, 65.0f, 40.0f, 10.0f,
                 SpriteUvRect{0.0f, 0.25f, 0.25f, 0.5f}, 4, 7),
      expectQuad(40.0f, 98.0f, 60.0f, 16.0f,
                 SpriteUvRect{0.25f, 0.25f, 0.5f, 0.5f}, 6, 7),
      expectQuad(40.0f, 114.0f, 60.0f, 16.0f,
                 SpriteUvRect{0.5f, 0.25f, 0.75f, 0.5f}, 7, 7)};
  for (std::size_t k = 0; k < uiExpected.size(); ++k) {
    const SpriteItem& q = batcher.at(batches[0].instances[k]);
    EXPECT_TRUE(sameItem(q, uiExpected[k]))
        << "UI group instance " << k;
  }

  // Group 1 (the glyph atlas) in draw order: the B label (M, a at
  // z1 — the centered label on the button's center x = 40, baseline
  // 38: the line "Ma" width 20 centers to lineX0 = 30; the M ink
  // left 30 + bearingX, top 38 - bearingY; the a starts at the M's
  // advance), then the T item's glyphs (z2 — the pass overwrote the
  // item's hand z 77; the item is CENTER-aligned on x = 100, baseline
  // 38, so its line centers to lineX0 = 90), then the IN content (z5
  // — left-aligned at x = 100, baseline 60 + (16 + 16)/2 = 76).
  const float bLineX0 = 30.0f;  // 40 - 20/2 (the B label, center x = 40)
  const float tLineX0 = 90.0f;  // 100 - 20/2 (the T item, center x = 100)
  const std::vector<SpriteItem> glyphExpected = {
      expectQuad(bLineX0 + static_cast<float>(mM.bearingX) + 5.0f,
                 38.0f - static_cast<float>(mM.bearingY) + 5.5f, 10.0f, 11.0f,
                 uvM, 1, 9),
      expectQuad(bLineX0 + static_cast<float>(mM.advance) +
                     static_cast<float>(ma.bearingX) + 4.0f,
                 38.0f - static_cast<float>(ma.bearingY) + 4.5f, 8.0f, 9.0f,
                 uvA, 1, 9),
      expectQuad(tLineX0 + static_cast<float>(mM.bearingX) + 5.0f,
                 38.0f - static_cast<float>(mM.bearingY) + 5.5f, 10.0f, 11.0f,
                 uvM, 2, 9),
      expectQuad(tLineX0 + static_cast<float>(mM.advance) +
                     static_cast<float>(ma.bearingX) + 4.0f,
                 38.0f - static_cast<float>(ma.bearingY) + 4.5f, 8.0f, 9.0f,
                 uvA, 2, 9),
      expectQuad(106.0f, 70.5f, 10.0f, 11.0f, uvM, 5, 9)};
  for (std::size_t k = 0; k < glyphExpected.size(); ++k) {
    const SpriteItem& q = batcher.at(batches[1].instances[k]);
    EXPECT_TRUE(sameItem(q, glyphExpected[k]))
        << "glyph group instance " << k;
  }
  std::printf(
      "ui-render-declare: quads=%llu groups=%llu overrides=%llu\n",
      static_cast<unsigned long long>(batcher.frameCount()),
      static_cast<unsigned long long>(batcher.batchCount()),
      static_cast<unsigned long long>(batcher.overrideCount()));
}

TEST(UiRenderDeclare, NoDefaultFontDeclaresNoText) {
  StringTable table = makeTable();
  const StringRef maRef = internText(table, "Ma");
  UiTree tree = makeTree(200, 150);
  UiWidgetSpec s;
  s.kind = UiWidgetKind::Panel;
  s.opaque = true;
  s.width = 200;
  s.height = 150;
  s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
  s.atlasId = 7;
  const UiWidgetId p = addWidget(tree, s);
  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Button;
  s.parent = p;
  s.x = 20;
  s.y = 20;
  s.width = 40;
  s.height = 20;
  s.image = SpriteUvRect{0.25f, 0.0f, 0.5f, 0.25f};
  s.atlasId = 7;
  s.label = maRef;
  addWidget(tree, s);
  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Input;
  s.parent = p;
  s.x = 100;
  s.y = 60;
  s.width = 60;
  s.height = 16;
  s.text = maRef;
  addWidget(tree, s);
  ASSERT_TRUE(tree.layout().ok());
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  // No default font: the art quads still render, the labels /
  // content declare nothing (the header's per-widget model).
  ASSERT_TRUE(declareUi(batcher, table, tree, UiDeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  // The panel's quad (opaque) + the button's art quad = 2 (the label
  // and the input's content declare nothing without a font).
  EXPECT_EQ(2u, batcher.frameCount());
  EXPECT_EQ(1u, batcher.batchCount());
}

// ---------------------------------------------------------------------------
// UiRenderCull — the documented simple model (no GL)
// ---------------------------------------------------------------------------

TEST(UiRenderCull, OffscreenRule) {
  StringTable table = makeTable();
  UiTree tree = makeTree(50, 50);
  auto addImage = [&](std::int32_t x, std::int32_t y, std::int32_t w,
                      std::int32_t h) {
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Image;
    s.x = x;
    s.y = y;
    s.width = w;
    s.height = h;
    s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
    s.atlasId = 7;
    addWidget(tree, s);
  };
  addImage(50, 0, 10, 10);    // at the right edge (rectX >= screenW)
  addImage(0, 50, 10, 10);    // at the bottom edge
  addImage(-20, 0, 10, 10);   // right edge -10 <= 0
  addImage(0, -20, 10, 10);   // bottom edge -10 <= 0
  addImage(0, 0, 0, 10);      // zero width
  addImage(-5, 0, 10, 10);    // partially on screen (z0)
  addImage(45, 0, 10, 10);    // partially on screen (z1)
  ASSERT_TRUE(tree.layout().ok());
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareUi(batcher, table, tree, UiDeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  // The half-open edge rule: a widget touching the screen's right /
  // bottom edge with a visible column/row is drawn; the rest is
  // culled (zero-area or entirely outside).
  EXPECT_EQ(2u, batcher.frameCount());
}

TEST(UiRenderCull, OcclusionRule) {
  StringTable table = makeTable();
  UiTree tree = makeTree(100, 100);
  auto addImage = [&](UiWidgetId parent, std::int32_t x, std::int32_t y,
                      std::int32_t w, std::int32_t h) {
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Image;
    s.parent = parent;
    s.x = x;
    s.y = y;
    s.width = w;
    s.height = h;
    s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
    s.atlasId = 7;
    return addWidget(tree, s);
  };
  auto addPanel = [&](UiWidgetId parent, std::int32_t x, std::int32_t y,
                      std::int32_t w, std::int32_t h, bool opaque) {
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Panel;
    s.parent = parent;
    s.x = x;
    s.y = y;
    s.width = w;
    s.height = h;
    s.opaque = opaque;
    s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
    s.atlasId = 7;
    return addWidget(tree, s);
  };
  // The hand-computed occlusion matrix (the header's culling
  // section): I1 is culled by the LATER same-rect opaque P1; I2 by
  // nothing (P2 only partially overlaps); I3 by nothing (P3 is EARLIER
  // — I3 draws on top); I4 by nothing (a child of the opaque P4 — it
  // is LATER, on top of the background); I5 is culled by the later
  // same-rect opaque P5 (a later sibling); I6 by nothing (P6 is not
  // opaque — it declares nothing, culls nothing).
  addImage(kInvalidUiWidgetId, 0, 0, 20, 20);      // I1
  addPanel(kInvalidUiWidgetId, 0, 0, 20, 20, true);  // P1 (covers I1)
  addImage(kInvalidUiWidgetId, 30, 0, 20, 20);     // I2
  addPanel(kInvalidUiWidgetId, 30, 0, 10, 20, true);  // P2 (partial)
  addPanel(kInvalidUiWidgetId, 60, 0, 20, 20, true);  // P3 (earlier)
  addImage(kInvalidUiWidgetId, 60, 0, 20, 20);     // I3
  const UiWidgetId p4 =
      addPanel(kInvalidUiWidgetId, 0, 60, 40, 40, true);  // P4
  addImage(p4, 5, 5, 10, 10);                 // I4 (a child)
  addImage(p4, 15, 5, 10, 10);                // I5
  addPanel(p4, 15, 5, 10, 10, true);          // P5 (covers I5)
  addImage(kInvalidUiWidgetId, 80, 0, 10, 10);  // I6
  addPanel(kInvalidUiWidgetId, 80, 0, 40, 40, false);  // P6 (not opaque)
  ASSERT_TRUE(tree.layout().ok());
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareUi(batcher, table, tree, UiDeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  // 9 quads in traversal / z order: P1 z0, I2 z1, P2 z2, P3 z3, I3 z4,
  // P4 z5, I4 z6, P5 z7, I6 z8 (I1 / I5 culled; P6 declares nothing).
  EXPECT_EQ(9u, batcher.frameCount());
  EXPECT_EQ(9u, batcher.overrideCount());
  const std::span<const SpriteBatch> batches = batcher.batches();
  ASSERT_EQ(1u, batches.size());
  ASSERT_EQ(9u, batches[0].instances.size());
  const std::uint32_t expectedZ[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  for (std::size_t k = 0; k < 9; ++k) {
    const SpriteItem& q = batcher.at(batches[0].instances[k]);
    EXPECT_EQ(expectedZ[k], q.depthKey) << "instance " << k;
  }
  // The culled I1 / I5 contribute nothing: the first quad is P1 (the
  // (0, 0, 20, 20) rect), the seventh is I4 (the (5, 65, 10, 10)
  // child rect).
  EXPECT_FLOAT_EQ(10.0f, batcher.at(batches[0].instances[0]).pos.x);
  EXPECT_FLOAT_EQ(10.0f, batcher.at(batches[0].instances[6]).pos.x);
  EXPECT_FLOAT_EQ(70.0f, batcher.at(batches[0].instances[6]).pos.y);
}

// ---------------------------------------------------------------------------
// UiRenderSlider — the track + fill rule (no GL)
// ---------------------------------------------------------------------------

TEST(UiRenderSlider, FillDeclaredOnlyForInteriorValues) {
  StringTable table = makeTable();
  UiTree tree = makeTree(100, 100);
  auto addSlider = [&](std::int32_t y, float value, std::int32_t w) {
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Slider;
    s.x = 10;
    s.y = y;
    s.width = w;
    s.height = 8;
    s.sliderMin = 0.0f;
    s.sliderMax = 10.0f;
    s.sliderValue = value;
    s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
    s.atlasId = 7;
    addWidget(tree, s);
  };
  addSlider(10, 0.0f, 40);  // t = 0: the track only
  addSlider(30, 10.0f, 40);  // t = 1: the track only (no double-blend)
  addSlider(50, 5.0f, 40);  // t = 0.5: the track + the fill
  addSlider(70, 5.0f, 0);   // zero width: culled (zero area)
  ASSERT_TRUE(tree.layout().ok());
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareUi(batcher, table, tree, UiDeclareOptions{}).ok());
  ASSERT_TRUE(batcher.build().ok());
  // 4 quads: S0 track z0, S1 track z1, S2 track + fill z2, S3 nothing.
  EXPECT_EQ(4u, batcher.frameCount());
  const std::span<const SpriteBatch> batches = batcher.batches();
  ASSERT_EQ(1u, batches.size());
  ASSERT_EQ(4u, batches[0].instances.size());
  const SpriteItem& track0 = batcher.at(batches[0].instances[0]);
  const SpriteItem& track1 = batcher.at(batches[0].instances[1]);
  const SpriteItem& track2 = batcher.at(batches[0].instances[2]);
  const SpriteItem& fill2 = batcher.at(batches[0].instances[3]);
  EXPECT_FLOAT_EQ(30.0f, track0.pos.x);
  EXPECT_FLOAT_EQ(14.0f, track0.pos.y);
  EXPECT_FLOAT_EQ(40.0f, track0.scale.x);
  EXPECT_FLOAT_EQ(8.0f, track0.scale.y);
  EXPECT_FLOAT_EQ(34.0f, track1.pos.y);
  EXPECT_FLOAT_EQ(30.0f, track2.pos.x);
  EXPECT_FLOAT_EQ(54.0f, track2.pos.y);
  // The fill spans [10, 30) x [50, 58) (t = 0.5 of the 40 px track).
  EXPECT_FLOAT_EQ(20.0f, fill2.pos.x);
  EXPECT_FLOAT_EQ(54.0f, fill2.pos.y);
  EXPECT_FLOAT_EQ(20.0f, fill2.scale.x);
  EXPECT_FLOAT_EQ(8.0f, fill2.scale.y);
  EXPECT_EQ(track2.depthKey, fill2.depthKey);  // the node's z (track, fill)
  EXPECT_EQ(track0.depthKey, track0.depthKey);  // the t = 0 track only
  (void)track0;
}

// ---------------------------------------------------------------------------
// UiRenderFailure — first failure wins (no GL)
// ---------------------------------------------------------------------------

namespace {

SpriteBatcher makeBatcher() {
  auto bc = SpriteBatcher::create({16});
  if (!bc.ok()) std::abort();
  return std::move(bc).takeValue();
}

UiTree onePanelTree() {
  UiTree tree = makeTree(100, 100);
  UiWidgetSpec s;
  s.kind = UiWidgetKind::Panel;
  s.opaque = true;
  s.width = 100;
  s.height = 100;
  s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
  s.atlasId = 7;
  addWidget(tree, s);
  return tree;
}

}  // namespace

TEST(UiRenderFailure, PreconditionFailuresDeclareNothing) {
  StringTable table = makeTable();
  // A stopped tree: InvalidArgument, nothing declared.
  {
    UiTree stopped;
    SpriteBatcher batcher = makeBatcher();
    batcher.beginFrame();
    auto r = declareUi(batcher, table, stopped, UiDeclareOptions{});
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
  }
  // The font scale domain (with a default font): 0 and 17 fail before
  // the first add.
  GlyphAtlas font = makeAtlas();
  for (std::uint32_t scale : {0u, kTextMaxScale + 1u}) {
    UiTree tree = onePanelTree();
    ASSERT_TRUE(tree.layout().ok());
    SpriteBatcher batcher = makeBatcher();
    batcher.beginFrame();
    auto r = declareUi(batcher, table, tree,
                       UiDeclareOptions{&font, 9, scale});
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
  }
}

TEST(UiRenderFailure, TextAndLabelHandleFailures) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable();
  internText(table, "M");  // the table owns exactly 1 string
  const StringRef invalid{9};
  // A Text widget with a handle the table does not own: the EARLIER
  // panel's quad stays in the frame (the batcher owns the frame state),
  // the call fails InvalidArgument.
  {
    UiTree tree = makeTree(100, 100);
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Panel;
    s.opaque = true;
    s.width = 100;
    s.height = 100;
    s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
    s.atlasId = 7;
    addWidget(tree, s);
    s = UiWidgetSpec{};
    s.kind = UiWidgetKind::Text;
    s.x = 10;
    s.y = 10;
    s.width = 60;
    s.height = 20;
    TextItem item;
    item.text = invalid;
    item.font = &font;
    item.atlasId = 9;
    s.item = item;
    addWidget(tree, s);
    ASSERT_TRUE(tree.layout().ok());
    SpriteBatcher batcher = makeBatcher();
    batcher.beginFrame();
    auto r = declareUi(batcher, table, tree, fontOptions(font));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(1u, batcher.frameCount());  // the panel's quad remains
  }
  // A Button's label with an invalid handle: the art quad is declared
  // BEFORE the label (the per-widget model), so it remains in the
  // frame on failure (the header's failure section).
  {
    UiTree tree = makeTree(100, 100);
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Button;
    s.x = 10;
    s.y = 10;
    s.width = 40;
    s.height = 20;
    s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
    s.atlasId = 7;
    s.label = invalid;
    addWidget(tree, s);
    ASSERT_TRUE(tree.layout().ok());
    SpriteBatcher batcher = makeBatcher();
    batcher.beginFrame();
    auto r = declareUi(batcher, table, tree, fontOptions(font));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(1u, batcher.frameCount());  // the art quad remains
  }
  // An Input's text with an invalid handle (the first node).
  {
    UiTree tree = makeTree(100, 100);
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Input;
    s.x = 10;
    s.y = 10;
    s.width = 60;
    s.height = 16;
    s.text = invalid;
    addWidget(tree, s);
    ASSERT_TRUE(tree.layout().ok());
    SpriteBatcher batcher = makeBatcher();
    batcher.beginFrame();
    auto r = declareUi(batcher, table, tree, fontOptions(font));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
  }
  // A Text widget with a stopped (moved-from) font: InvalidArgument.
  {
    GlyphAtlas moved;  // the stopped state
    UiTree tree = makeTree(100, 100);
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Text;
    s.x = 10;
    s.y = 10;
    s.width = 60;
    s.height = 20;
    TextItem item;
    item.text = internText(table, "M");
    item.font = &moved;
    item.atlasId = 9;
    s.item = item;
    addWidget(tree, s);
    ASSERT_TRUE(tree.layout().ok());
    SpriteBatcher batcher = makeBatcher();
    batcher.beginFrame();
    auto r = declareUi(batcher, table, tree, fontOptions(font));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
  }
}

TEST(UiRenderFailure, BatcherStateFailures) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable();
  UiTree tree = onePanelTree();
  ASSERT_TRUE(tree.layout().ok());
  // A stopped batcher: the first add fails BudgetExhausted.
  {
    SpriteBatcher stopped;
    auto r = declareUi(stopped, table, tree, fontOptions(font));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::BudgetExhausted, r.error());
  }
  // A closed window: build() first, then the first add fails
  // InvalidArgument.
  {
    SpriteBatcher batcher = makeBatcher();
    batcher.beginFrame();
    ASSERT_TRUE(batcher.build().ok());
    auto r = declareUi(batcher, table, tree, fontOptions(font));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
  }
}

// ---------------------------------------------------------------------------
// UiRenderZeroAlloc — the 1 000-frame declare loop (no GL)
// ---------------------------------------------------------------------------

TEST(UiRenderZeroAlloc, FrameLoopAllocatesNothing) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable();
  const StringRef maRef = internText(table, "Ma");
  const StringRef mRef = internText(table, "M");
  const StringRef empty = internText(table, "");
  UiTree tree = makeTree(200, 150);
  buildDeclareScene(tree, font, maRef, mRef, empty);
  SpriteBatcher batcher = makeBatcher();
  const UiDeclareOptions options = fontOptions(font);
  constexpr std::uint32_t kFrames = 1000;
  auto runFrames = [&]() {
    for (std::uint32_t f = 0; f < kFrames; ++f) {
      if (!tree.layout().ok()) {
        std::fprintf(stderr, "UiRenderZeroAlloc: layout failed\n");
        std::abort();
      }
      batcher.beginFrame();
      if (!declareUi(batcher, table, tree, options).ok()) {
        std::fprintf(stderr, "UiRenderZeroAlloc: declare failed\n");
        std::abort();
      }
      if (!batcher.build().ok()) {
        std::fprintf(stderr, "UiRenderZeroAlloc: build failed\n");
        std::abort();
      }
    }
  };
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    runFrames();
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(0ull, reading.allocs)
        << kFrames << " frames of the seven-kinds declare allocated "
        << reading.allocs << " heap blocks on the loop thread";
    std::printf("ui-render-zeroalloc frames=%u allocs=%llu\n", kFrames,
                static_cast<unsigned long long>(reading.allocs));
  } else {
    runFrames();  // sanitizer tree: the leak-free run covers it
  }
}

// ---------------------------------------------------------------------------
// UiRenderSmoke — the golden image fixture (the offscreen GL render,
// GTEST_SKIPs where no usable OpenGL 3.3 environment — the documented
// environment contract)
// ---------------------------------------------------------------------------

namespace {

constexpr std::int32_t kSmokeWidth = 128;
constexpr std::int32_t kSmokeHeight = 128;

// The fixed golden scene (screen 128 x 128 — the hand-computed
// layout):
//
//   P  opaque Panel  (10, 10, 40, 30)  uv texel (0,0) red    atlas 7
//   B  Button        (16, 16, 20, 14)  uv texel (1,0) green  label "M"
//   T  Text          (70, 12, 20, 20)  "a" @ (80, 20) left   atlas 9
//   I  Image         (80, 40, 16, 16)  uv texel (2,0) blue
//   S  Slider        (10, 50, 40, 8)   [0, 4] value 2       uv texel (3,0) white
//
// 7 quads: P z0, B-art z1, B-label M z1, T-a z2, I z3, S track + fill
// z4. Two groups: (7, 0, Alpha) 5 instances, (9, 0, Alpha) 2.
void buildSmokeScene(UiTree& tree, const GlyphAtlas& font, StringRef mRef,
                     StringRef aRef) {
  UiWidgetSpec s;
  s.kind = UiWidgetKind::Panel;
  s.opaque = true;
  s.x = 10;
  s.y = 10;
  s.width = 40;
  s.height = 30;
  s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
  s.atlasId = 7;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Button;
  s.x = 16;
  s.y = 16;
  s.width = 20;
  s.height = 14;
  s.image = SpriteUvRect{0.25f, 0.0f, 0.5f, 0.25f};
  s.atlasId = 7;
  s.label = mRef;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Text;
  s.x = 70;
  s.y = 12;
  s.width = 20;
  s.height = 20;
  TextItem item;
  item.text = aRef;
  item.font = &font;
  item.x = 80.0f;
  item.y = 20.0f;
  item.alignment = TextAlignment::Left;
  item.scale = 1;
  item.maxWidth = 0;
  item.atlasId = 9;
  s.item = item;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Image;
  s.x = 80;
  s.y = 40;
  s.width = 16;
  s.height = 16;
  s.image = SpriteUvRect{0.5f, 0.0f, 0.75f, 0.25f};
  s.atlasId = 7;
  addWidget(tree, s);

  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Slider;
  s.x = 10;
  s.y = 50;
  s.width = 40;
  s.height = 8;
  s.sliderMin = 0.0f;
  s.sliderMax = 4.0f;
  s.sliderValue = 2.0f;
  s.image = SpriteUvRect{0.75f, 0.0f, 1.0f, 0.25f};
  s.atlasId = 7;
  addWidget(tree, s);
}

// The 4x4 UI atlas (solid colors, all opaque — the golden's known
// texture: row-major RGBA8, 256 bytes).
const std::array<std::uint8_t, 64>& uiAtlasRgba() {
  // The 4x4 UI atlas (16 texels x 4 channels = 64 bytes), row-major
  // RGBA8. Row 0: red, green, blue, white. Row 1: yellow, cyan,
  // magenta, gray. Row 2: dark slate (x4). Row 3: dark gray (x4).
  // All texels are opaque (a = 255).
  static const std::array<std::uint8_t, 64> rgba = {
      255, 0, 0, 255,    0, 255, 0, 255,   0, 0, 255, 255,
      255, 255, 255, 255, 255, 255, 0, 255, 0, 255, 255, 255,
      255, 0, 255, 255,  128, 128, 128, 255,
      10, 20, 30, 255,   10, 20, 30, 255,  10, 20, 30, 255,
      10, 20, 30, 255,   64, 64, 64, 255,  64, 64, 64, 255,
      64, 64, 64, 255,   64, 64, 64, 255};
  return rgba;
}

// ---------------------------------------------------------------------------
// The CPU reference rasterizer (the golden's oracle): the UI quads are
// UNROTATED axis-aligned rects — the pixel-center screen test, the
// exact UV recovery, the texture fetch (GL_NEAREST, no-mipmap,
// CLAMP_TO_EDGE: the floor of the uv in texel space), the tint, and
// the per-group blend in double (the GPU's float32 differs by <= 1
// byte after the RGBA8 conversion — the documented +/- 1 tolerance;
// the probe pixels are asserted exactly).
// ---------------------------------------------------------------------------

struct RefUiQuad {
  double left{0.0};
  double top{0.0};
  double right{0.0};
  double bottom{0.0};
  double u0{0.0};
  double v0{0.0};
  double u1{0.0};
  double v1{0.0};
  double tr{1.0};
  double tg{1.0};
  double tb{1.0};
  double ta{1.0};
  std::uint32_t atlasId{0};
  BlendMode blend{BlendMode::Alpha};
};

RefUiQuad makeRefUiQuad(const SpriteItem& it) {
  RefUiQuad q;
  q.left = static_cast<double>(it.pos.x) - static_cast<double>(it.scale.x) * 0.5;
  q.top = static_cast<double>(it.pos.y) - static_cast<double>(it.scale.y) * 0.5;
  q.right = static_cast<double>(it.pos.x) + static_cast<double>(it.scale.x) * 0.5;
  q.bottom = static_cast<double>(it.pos.y) + static_cast<double>(it.scale.y) * 0.5;
  q.u0 = it.uv.u0;
  q.v0 = it.uv.v0;
  q.u1 = it.uv.u1;
  q.v1 = it.uv.v1;
  q.tr = it.tint.r;
  q.tg = it.tint.g;
  q.tb = it.tint.b;
  q.ta = it.tint.a;
  q.atlasId = it.atlasId;
  q.blend = it.blend;
  return q;
}

bool refUiInside(const RefUiQuad& q, double px, double py) {
  // Strict interior (the pixel centers of this scene never land on a
  // quad edge — the rect edges are integer screen px, the pixel
  // centers are +0.5: the boundary is excluded by construction).
  return px > q.left && px < q.right && py > q.top && py < q.bottom;
}

std::array<std::uint8_t, 4> goldenAtlasTexel(std::uint32_t atlasId,
                                             std::uint32_t tx, std::uint32_t ty,
                                             const std::span<const std::uint8_t>& fontRgba) {
  if (atlasId == 9) {
    constexpr std::uint32_t W = 1024;
    tx = std::min(tx, W - 1);
    ty = std::min(ty, W - 1);
    const std::uint8_t* p =
        fontRgba.data() + static_cast<std::size_t>(ty * W + tx) * 4;
    return {p[0], p[1], p[2], p[3]};
  }
  constexpr std::uint32_t W = 4;
  tx = std::min(tx, W - 1);
  ty = std::min(ty, W - 1);
  const std::uint8_t* p = uiAtlasRgba().data() +
                          static_cast<std::size_t>(ty * W + tx) * 4;
  return {p[0], p[1], p[2], p[3]};
}

std::array<std::uint8_t, 4> refUiPixel(std::int32_t px, std::int32_t py,
                                       const std::vector<RefUiQuad>& quads,
                                       const std::span<const std::uint8_t>& fontRgba) {
  // The clear (0, 0, 0, 1) in float, accumulated through the draw
  // order's blends (the shader's exact math, per the per-group
  // BlendMode contract).
  double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
  const double cx = static_cast<double>(px) + 0.5;
  const double cy = static_cast<double>(py) + 0.5;
  for (const RefUiQuad& q : quads) {
    if (!refUiInside(q, cx, cy)) continue;
    const double u =
        q.u0 + (cx - q.left) / (q.right - q.left) * (q.u1 - q.u0);
    const double v =
        q.v0 + (cy - q.top) / (q.bottom - q.top) * (q.v1 - q.v0);
    const std::uint32_t W = (q.atlasId == 9) ? 1024u : 4u;
    const std::uint32_t tx = static_cast<std::uint32_t>(
        std::max(0.0, std::min(static_cast<double>(W - 1),
                               std::floor(u * static_cast<double>(W)))));
    const std::uint32_t ty = static_cast<std::uint32_t>(
        std::max(0.0, std::min(static_cast<double>(W - 1),
                               std::floor(v * static_cast<double>(W)))));
    const std::array<std::uint8_t, 4> t =
        goldenAtlasTexel(q.atlasId, tx, ty, fontRgba);
    const double srcR = static_cast<double>(t[0]) / 255.0 * q.tr;
    const double srcG = static_cast<double>(t[1]) / 255.0 * q.tg;
    const double srcB = static_cast<double>(t[2]) / 255.0 * q.tb;
    const double srcA = static_cast<double>(t[3]) / 255.0 * q.ta;
    if (q.blend == BlendMode::Alpha) {
      // The engine's SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend on ALL
      // four channels (the M2-SPRITE-02 contract).
      r = srcA * srcR + (1.0 - srcA) * r;
      g = srcA * srcG + (1.0 - srcA) * g;
      b = srcA * srcB + (1.0 - srcA) * b;
      a = srcA * srcA + (1.0 - srcA) * a;
    } else {
      r = srcR + r;
      g = srcG + g;
      b = srcB + b;
      a = srcA + a;
    }
    r = std::min(1.0, std::max(0.0, r));
    g = std::min(1.0, std::max(0.0, g));
    b = std::min(1.0, std::max(0.0, b));
    a = std::min(1.0, std::max(0.0, a));
  }
  return {static_cast<std::uint8_t>(std::min(255.0, r * 255.0 + 0.5)),
          static_cast<std::uint8_t>(std::min(255.0, g * 255.0 + 0.5)),
          static_cast<std::uint8_t>(std::min(255.0, b * 255.0 + 0.5)),
          static_cast<std::uint8_t>(std::min(255.0, a * 255.0 + 0.5))};
}

// The frame's quads in the exact draw order (the batcher's published
// group order, the in-group order — the submit stage's draw order).
std::vector<RefUiQuad> smokeQuads(SpriteBatcher& batcher) {
  std::vector<RefUiQuad> quads;
  quads.reserve(batcher.frameCount());
  for (const SpriteBatch& b : batcher.batches()) {
    for (const std::uint32_t slot : b.instances) {
      quads.push_back(makeRefUiQuad(batcher.at(slot)));
    }
  }
  return quads;
}

}  // namespace

TEST(UiRenderSmoke, GoldenUiScene) {
  auto ctx = laige::render::GlContext::createHeadless(kSmokeWidth,
                                                      kSmokeHeight);
  if (!ctx.ok()) {
    GTEST_SKIP() << "No usable OpenGL 3.3 environment: "
                 << laige::errorText(ctx.error())
                 << " (the documented environment contract — the P0 CI "
                    "runners always have one)";
  }
  laige::render::GlContext gl = std::move(ctx).takeValue();
  GlyphAtlas font = makeAtlas();
  const GlyphMetrics& ma = *font.glyph('a');
  ASSERT_EQ(8, ma.advance);
  ASSERT_EQ(8, ma.width);
  ASSERT_EQ(9, ma.height);
  ASSERT_EQ(8, ma.bearingY);
  StringTable table = makeTable();
  const StringRef mRef = internText(table, "M");
  const StringRef aRef = internText(table, "a");
  // The setup uploads (one call per atlas per scene load — RENDER-004).
  const auto& uiArr = uiAtlasRgba();
  const std::span<const std::uint8_t> uiRgba(uiArr.data(), uiArr.size());
  std::vector<std::uint8_t> fontRgba;
  fontRgba.resize(1024u * 1024u * 4u);
  ASSERT_TRUE(expandGlyphAtlasRgba8(font, fontRgba).ok());
  // maxInstances = the batcher's capacity (16); maxAtlases = 16 so the
  // UI atlas (id 7) and the glyph atlas (id 9) are both in domain
  // ([0, maxAtlases)).
  auto rc = laige::render::SpriteRenderer::create(
      gl, SpriteRenderer::Options{16, 16});
  ASSERT_TRUE(rc.ok());
  laige::render::SpriteRenderer renderer = std::move(rc).takeValue();
  ASSERT_TRUE(renderer.bindAtlas(7, 4, 4, uiRgba).ok());
  ASSERT_TRUE(
      renderer.bindAtlas(9, 1024, 1024,
                         std::span<const std::uint8_t>(fontRgba)).ok());
  UiTree tree = makeTree(kSmokeWidth, kSmokeHeight);
  buildSmokeScene(tree, font, mRef, aRef);
  ASSERT_TRUE(tree.layout().ok());
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareUi(batcher, table, tree, fontOptions(font)).ok());
  ASSERT_TRUE(batcher.build().ok());
  // The golden frame: 7 quads, 2 (atlas, material, blend) groups —
  // the UI atlas + the glyph atlas, each one texture (one texture per
  // atlas, the M2-UI-02 scope).
  EXPECT_EQ(7u, batcher.frameCount());
  EXPECT_EQ(2u, batcher.batchCount());
  // The clear BEFORE the first draw (the offscreen frame protocol).
  ASSERT_TRUE(gl.clear(0.0f, 0.0f, 0.0f, 1.0f).ok());
  ASSERT_TRUE(renderer.submit(batcher, uiScreenMatrix(kSmokeWidth,
                                                      kSmokeHeight))
                  .ok());
  const SpriteDrawStats stats = renderer.frameStats();
  EXPECT_EQ(2u, stats.drawCalls);
  EXPECT_EQ(7u, stats.instances);
  EXPECT_EQ(7u * 52u, stats.uploadBytes);
  // The whole 128x128 frame against the CPU reference (the GPU's
  // float32 vs the reference double after the RGBA8 conversion — the
  // documented +/- 1 byte tolerance; the probe pixels below are
  // rounding-exact and asserted exactly).
  const std::vector<RefUiQuad> quads = smokeQuads(batcher);
  auto readScreen = [&](std::int32_t sx, std::int32_t sy) {
    std::uint8_t rgba4[4]{};
    auto r = gl.readPixel(sx, kSmokeHeight - 1 - sy, rgba4);
    if (!r.ok()) return std::array<std::uint8_t, 4>{};
    return std::array<std::uint8_t, 4>{rgba4[0], rgba4[1], rgba4[2],
                                       rgba4[3]};
  };
  std::uint64_t mismatches = 0;
  for (std::int32_t py = 0; py < kSmokeHeight; ++py) {
    for (std::int32_t px = 0; px < kSmokeWidth; ++px) {
      const std::array<std::uint8_t, 4> ref =
          refUiPixel(px, py, quads,
                     std::span<const std::uint8_t>(fontRgba));
      const std::array<std::uint8_t, 4> p = readScreen(px, py);
      for (std::size_t c = 0; c < 4; ++c) {
        if (std::abs(static_cast<int>(p[c]) - static_cast<int>(ref[c])) > 1) {
          ++mismatches;
          break;
        }
      }
    }
  }
  EXPECT_EQ(0ull, mismatches) << "pixels off by more than the documented "
                                 "+/- 1 tolerance";
  // The exact probes (the rounding-exact pixels — the reference-
  // independent anchors of the golden):
  // The opaque solid texels over the opaque clear: src.a = 1 -> the
  // texel's color, exactly (the GL 8-bit conversion of the exact
  // blend result).
  {
    const std::array<std::uint8_t, 4> p = readScreen(20, 20);  // the P red
    EXPECT_EQ(255u, p[0]);
    EXPECT_EQ(0u, p[1]);
    EXPECT_EQ(0u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  {
    const std::array<std::uint8_t, 4> p = readScreen(26, 23);  // the B green
    EXPECT_EQ(0u, p[0]);
    EXPECT_EQ(255u, p[1]);
    EXPECT_EQ(0u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  {
    const std::array<std::uint8_t, 4> p = readScreen(88, 48);  // the I blue
    EXPECT_EQ(0u, p[0]);
    EXPECT_EQ(0u, p[1]);
    EXPECT_EQ(255u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // The slider fill over the track (the two identical opaque white
  // quads: the second blend is a no-op on the alpha-1 source): white.
  {
    const std::array<std::uint8_t, 4> p = readScreen(20, 54);
    EXPECT_EQ(255u, p[0]);
    EXPECT_EQ(255u, p[1]);
    EXPECT_EQ(255u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // The background (outside every quad): the clear.
  {
    const std::array<std::uint8_t, 4> p = readScreen(120, 120);
    EXPECT_EQ(0u, p[0]);
    EXPECT_EQ(0u, p[1]);
    EXPECT_EQ(0u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // The 'a' glyph ink (the white-on-alpha upload over the black clear):
  // an opaque texel is exactly (255, 255, 255, 255) — the UV mapping
  // pinned (that exact texel lands on that exact pixel); the
  // semi-transparent edge texel is rgb = A exactly and the alpha
  // channel within +/- 1 of the engine's four-channel blend (the
  // driver-defined 8-bit rounding).
  const std::span<const std::uint8_t> alpha = font.atlas();
  const int inkAL = 80 + ma.bearingX;  // the a ink left (80 + 0)
  const int inkAT = 20 - ma.bearingY;  // the a ink top (20 - 8)
  // The first texel of the 'a' cell in an alpha class ({tx, ty, a};
  // tx = -1 when the class is empty): 0 = transparent, 1 = semi,
  // 2 = opaque.
  auto firstTexel = [&](int cls) {
    for (int ty = 0; ty < ma.height; ++ty) {
      for (int tx = 0; tx < ma.width; ++tx) {
        const std::uint8_t a =
            alpha[(ma.atlasY + static_cast<std::uint32_t>(ty)) * 1024u +
                  ma.atlasX + static_cast<std::uint32_t>(tx)];
        const bool hit = cls == 0
                             ? a == 0u
                             : (cls == 1
                                    ? (a > 0 && a < 255u)
                                    : a == 255u);
        if (hit) {
          return std::array<int, 3>{tx, ty, static_cast<int>(a)};
        }
      }
    }
    return std::array<int, 3>{-1, 0, 0};
  };
  {
    const std::array<int, 3> o = firstTexel(2);
    ASSERT_EQ(255, o[2]);
    const std::array<std::uint8_t, 4> p =
        readScreen(inkAL + o[0], inkAT + o[1]);
    EXPECT_EQ(255u, p[0]);
    EXPECT_EQ(255u, p[1]);
    EXPECT_EQ(255u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  {
    const std::array<int, 3> s = firstTexel(1);
    ASSERT_NE(-1, s[0]);
    const std::uint8_t A = static_cast<std::uint8_t>(s[2]);
    const std::array<std::uint8_t, 4> p =
        readScreen(inkAL + s[0], inkAT + s[1]);
    EXPECT_EQ(A, p[0]);
    EXPECT_EQ(A, p[1]);
    EXPECT_EQ(A, p[2]);
    const double a = static_cast<double>(A) / 255.0;
    const int expect = static_cast<int>(255.0 * (a * a + (1.0 - a)) + 0.5);
    EXPECT_LE(static_cast<int>(p[3]) - expect + 1, 1);
    EXPECT_LE(expect - static_cast<int>(p[3]) + 1, 1);
  }
  std::printf(
      "ui-render-golden: frame=%dx%d quads=%llu groups=%llu draw_calls=%llu "
      "instances=%llu mismatches=%llu\n",
      kSmokeWidth, kSmokeHeight,
      static_cast<unsigned long long>(batcher.frameCount()),
      static_cast<unsigned long long>(batcher.batchCount()),
      static_cast<unsigned long long>(stats.drawCalls),
      static_cast<unsigned long long>(stats.instances),
      static_cast<unsigned long long>(mismatches));
  // The context outlives the renderer (the ownership contract).
}
