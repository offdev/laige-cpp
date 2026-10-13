// laige-golden (M2-GOLD-01): the committed golden scene fixtures.
//
// The first two fixtures of the golden infrastructure, both rendered
// offscreen at the canonical golden frame (kGoldenWidth x
// kGoldenHeight = 1280 x 720, top-origin / y-down) with a FIXED
// camera and FIXED scene content — no PRNG at all (the "fixed seed" of
// the M2-GOLD-01 scope is trivially satisfied: every value below is a
// constant; the scene-shape invariants are pinned by the
// GoldenFixtures* unit suite, ctest entry `golden_compare`):
//
//   sprite_iso_1000  the M2-SPRITE-02 "1000-sprite scene" (the
//                    SpriteDrawSmoke fixture): the 2:1 dimetric lattice
//                    of the world cells (x, y) in -16..15 at preset
//                    scale k = 0.125, the three probe cells declared
//                    LAST, the three (atlas, material, blend) groups,
//                    the clear color (0, 0, 255, 255). At 1280 x 720 a
//                    1x1 world tile projects to 320 x 90 px, so the
//                    32x32 lattice's diamond is far larger than the
//                    frame: every pixel of the frame lies inside a
//                    lattice cell (the clear color shows nowhere), the
//                    visible content is the alpha checkerboard cells
//                    plus a diagonal band of the additive checkerboard
//                    cells across the bottom, and the solid (atlas 1)
//                    rows fall below the frame.
//   ui_smoke         the M2-UI-02 golden UI scene (the UiRenderSmoke
//                    fixture): the five widgets (panel, button, text,
//                    image, slider) at their fixed screen-px positions,
//                    the 4x4 UI atlas + the committed test font's 1024x
//                    1024 glyph atlas (white-on-alpha), the identity
//                    screen camera (uiScreenMatrix), the clear color
//                    (0, 0, 0, 255). The widgets occupy the top-left
//                    128x128 region of the frame (screen px, y down);
//                    the clear color fills the rest.
//
// The scene CONTENT is the smoke fixtures' content unchanged (the same
// items, widgets, atlases, camera matrices — only the frame size and,
// for the UI scene, the tree's screen rect grow to the canonical
// frame; the widgets' absolute positions are unchanged). The smoke
// tests keep their own 128x128 CPU-reference checks; this header is
// the committed-reference path on top (TEST-008).
//
// This header is test-only infrastructure: it is never included from
// src/ (the include-graph lint covers src/** only). The builders are
// GL-free (pure engine data + one IsoCamera matrix build) — the GL
// work is the tool's job.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "laige/render/font.h"
#include "laige/render/iso_camera.h"
#include "laige/render/iso_depth_key.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/text_items.h"
#include "laige/render/ui_render.h"
#include "laige/render/ui_widgets.h"
#include "laige/result.h"
#include "laige/sim_math.h"

namespace laige::testing::golden {

// The canonical golden frame (the M2-GOLD-01 scope's "fixed
// resolution"; the M2-SCENE-01 reference screen).
constexpr std::uint32_t kGoldenWidth = 1280;
constexpr std::uint32_t kGoldenHeight = 720;

// ---------------------------------------------------------------------------
// The sprite fixture (the M2-SPRITE-02 scene).
//
// The 2:1 dimetric lattice of the world cells (x, y) in -16..15 —
// lattice index i = (y + 16) * 32 + (x + 16), i in 0..999 — MINUS the
// three probe cells, PLUS three probes declared LAST (the (key,
// declaration) total order puts them on top of the equal-key lattice
// cells). Groups (the batcher's ascending (atlas, material, blend)
// order):
//   (0, 0, Alpha):    i < 600 minus the probes in range + P0  = 598
//   (0, 0, Additive): 600 <= i < 800 + P1                     = 201
//   (1, 0, Alpha):    i >= 800 + P2                           = 201
// so: 3 groups, 1000 instances, 2 texture binds, 3 blend changes.
//
// The probes (exact-color pixels of their quads' interiors):
//   P0 at (0, 0):  atlas 0, Alpha,    uv = texel (0, 0)
//   P1 at (0, 1):  atlas 0, Additive, uv = texel (2, 0) + the clear
//   P2 at (0, -1): atlas 1, Alpha,    uv = texel (0, 0)
// ---------------------------------------------------------------------------

constexpr std::uint32_t kSpriteProbe0Index = 528;  // (0, 0)
constexpr std::uint32_t kSpriteProbe1Index = 560;  // (0, 1)
constexpr std::uint32_t kSpriteProbe2Index = 496;  // (0, -1)
constexpr float kSpritePresetScale = 0.125f;  // ADR 0005 preset at zoom 1
constexpr std::uint32_t kSpriteAtlas0Id = 0;
constexpr std::uint32_t kSpriteAtlas1Id = 1;

// Atlas 0: a 4x4 checkerboard (row 0 = v = 0, the bottom — the GL
// upload convention, no flip). Even (i + j): (255, 128, 0); odd:
// (0, 128, 255). All alpha 255.
std::array<std::uint8_t, 64> spriteAtlas0Bytes() {
  std::array<std::uint8_t, 64> b{};
  for (std::uint32_t j = 0; j < 4; ++j) {
    for (std::uint32_t i = 0; i < 4; ++i) {
      const std::array<std::uint8_t, 4> col =
          ((i + j) % 2 == 0) ? std::array<std::uint8_t, 4>{255, 128, 0, 255}
                             : std::array<std::uint8_t, 4>{0, 128, 255, 255};
      std::memcpy(&b[(j * 4 + i) * 4], col.data(), 4);
    }
  }
  return b;
}

// Atlas 1: a 4x4 solid (33, 66, 222, 255).
std::array<std::uint8_t, 64> spriteAtlas1Bytes() {
  std::array<std::uint8_t, 64> b{};
  for (std::size_t p = 0; p < 16; ++p) {
    b[p * 4 + 0] = 33;
    b[p * 4 + 1] = 66;
    b[p * 4 + 2] = 222;
    b[p * 4 + 3] = 255;
  }
  return b;
}

// The M2-SPRITE-02 scene in declaration order (the batcher's entity-id
// order — the stable tie-break's carrier). Deterministic; 1000 items.
std::vector<laige::render::SpriteItem> buildSpriteSceneItems() {
  using laige::render::BlendMode;
  using laige::render::SpriteItem;
  using laige::render::isoDepthKey;
  using laige::sim::Fp32Pinned;
  using laige::sim::SimMath;

  auto latticeKey = [](std::int32_t x, std::int32_t y) {
    return isoDepthKey<Fp32Pinned>(SimMath<Fp32Pinned>::Vec2{
                                       static_cast<float>(x),
                                       static_cast<float>(y)},
                                   /*stepHeight=*/0, /*layer=*/0);
  };

  std::vector<SpriteItem> items;
  items.reserve(1000);
  for (std::uint32_t i = 0; i < 1000; ++i) {
    if (i == kSpriteProbe0Index || i == kSpriteProbe1Index ||
        i == kSpriteProbe2Index) {
      continue;  // the probe cells (the probes below take them)
    }
    const std::int32_t x = static_cast<std::int32_t>(i % 32) - 16;
    const std::int32_t y = static_cast<std::int32_t>(i / 32) - 16;
    SpriteItem it;
    it.pos = {static_cast<float>(x), static_cast<float>(y)};
    it.depthKey = latticeKey(x, y);
    it.uv = {0.0f, 0.0f, 1.0f, 1.0f};
    it.scale = {1.0f, 1.0f};
    it.rotation = 0.0f;
    it.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    it.atlasId = (i >= 800) ? kSpriteAtlas1Id : kSpriteAtlas0Id;
    it.materialId = 0;
    it.blend = (i >= 600 && i < 800) ? BlendMode::Additive
                                     : BlendMode::Alpha;
    items.push_back(it);
  }
  // The probes (declared LAST — the (key, declaration) total order
  // puts them on top of the equal-key lattice cells).
  auto probe = [&](std::int32_t x, std::int32_t y, float u0, float v0,
                   float u1, float v1, std::uint32_t atlasId,
                   BlendMode blend) {
    SpriteItem it;
    it.pos = {static_cast<float>(x), static_cast<float>(y)};
    it.depthKey = latticeKey(x, y);
    it.uv = {u0, v0, u1, v1};
    it.scale = {1.0f, 1.0f};
    it.rotation = 0.0f;
    it.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    it.atlasId = atlasId;
    it.materialId = 0;
    it.blend = blend;
    items.push_back(it);
  };
  probe(0, 0, 0.0f, 0.0f, 0.25f, 0.25f, kSpriteAtlas0Id,
        laige::render::BlendMode::Alpha);
  probe(0, 1, 0.5f, 0.0f, 0.75f, 0.25f, kSpriteAtlas0Id,
        laige::render::BlendMode::Additive);
  probe(0, -1, 0.0f, 0.0f, 0.25f, 0.25f, kSpriteAtlas1Id,
        laige::render::BlendMode::Alpha);
  return items;  // 997 lattice + 3 probes = 1000
}

// The frame's world -> NDC matrix: the ADR 0005 default preset at the
// fixture scale, the default camera state (position (0, 0, 0), zoom 1,
// no shake) — matrix() == isoDimetric2To1(k).
laige::Result<laige::render::Mat4> spriteSceneMatrix() {
  laige::render::IsoCameraOptions o;
  o.preset = laige::render::IsoPreset{
      laige::render::IsoPresetKind::Dimetric2To1, kSpritePresetScale};
  const auto r = laige::render::IsoCamera::create(o);
  if (r.isError()) {
    return laige::Result<laige::render::Mat4>::failure(r.error());
  }
  return laige::Result<laige::render::Mat4>(r.value().matrix());
}

// The complete sprite fixture (GL-free: the items, the atlases, the
// clear color, the matrix).
struct SpriteScene {
  std::vector<laige::render::SpriteItem> items;  // 1000, declaration order
  laige::render::Mat4 matrix;
  std::array<std::uint8_t, 64> atlas0;  // 4x4 checkerboard
  std::array<std::uint8_t, 64> atlas1;  // 4x4 solid
  float clearR{0.0f};
  float clearG{0.0f};
  float clearB{1.0f};
  float clearA{1.0f};
};

laige::Result<SpriteScene> buildSpriteScene() {
  const auto m = spriteSceneMatrix();
  if (m.isError()) {
    return laige::Result<SpriteScene>::failure(m.error());
  }
  SpriteScene s;
  s.items = buildSpriteSceneItems();
  s.matrix = m.value();
  s.atlas0 = spriteAtlas0Bytes();
  s.atlas1 = spriteAtlas1Bytes();
  return laige::Result<SpriteScene>(std::move(s));
}

// ---------------------------------------------------------------------------
// The UI fixture (the M2-UI-02 golden scene).
//
// The fixed scene (the hand-computed layout — the same widgets and
// absolute screen-px positions as the 128x128 UiRenderSmoke fixture;
// the tree's screen rect is the canonical frame, so the widgets sit in
// its top-left region):
//
//   P  opaque Panel  (10, 10, 40, 30)  uv texel (0,0) red    atlas 7
//   B  Button        (16, 16, 20, 14)  uv texel (1,0) green  label "M"
//   T  Text          (70, 12, 20, 20)  "a" @ (80, 20) left   atlas 9
//   I  Image         (80, 40, 16, 16)  uv texel (2,0) blue
//   S  Slider        (10, 50, 40, 8)   [0, 4] value 2       uv texel (3,0)
//
// 7 quads after declareUi: P z0, B-art z1, B-label M z1, T-a z2, I z3,
// S track + fill z4. Two (atlas, material, blend) groups:
// (7, 0, Alpha) 5 instances, (9, 0, Alpha) 2.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kUiAtlasId = 7;  // the 4x4 UI atlas texture id
constexpr std::uint32_t kFontAtlasId = 9;  // the glyph atlas texture id

// The 4x4 UI atlas (solid colors, all opaque — the golden's known
// texture: row-major RGBA8, 256 bytes). Row 0: red, green, blue,
// white. Row 1: yellow, cyan, magenta, gray. Row 2: dark slate (x4).
// Row 3: dark gray (x4).
const std::array<std::uint8_t, 64>& uiAtlasRgba() {
  static const std::array<std::uint8_t, 64> rgba = {
      255, 0, 0, 255,    0, 255, 0, 255,   0, 0, 255, 255,
      255, 255, 255, 255, 255, 255, 0, 255, 0, 255, 255, 255,
      255, 0, 255, 255,  128, 128, 128, 255,
      10, 20, 30, 255,   10, 20, 30, 255,  10, 20, 30, 255,
      10, 20, 30, 255,   64, 64, 64, 255,  64, 64, 64, 255,
      64, 64, 64, 255,   64, 64, 64, 255};
  return rgba;
}

// The complete UI fixture (GL-free: the font atlas + its white-on-alpha
// upload bytes, the string table, the laid-out tree, the clear color,
// the identity screen matrix). Move-only (the tree).
struct UiScene {
  // The committed test font, defaults (behind a unique_ptr so its
  // address is stable across UiScene moves: the Text widgets' items
  // hold `const GlyphAtlas*` into it — the M2-TEXT-02 scene-font
  // lifetime contract — and a moved member would dangle those).
  std::unique_ptr<laige::render::GlyphAtlas> font;
  std::vector<std::uint8_t> fontRgba;  // 1024x1024x4 white-on-alpha
  laige::render::StringTable table;  // the "M" / "a" strings
  laige::render::UiTree tree;  // the canonical-frame screen rect
  std::array<std::uint8_t, 64> uiAtlas{};  // the 4x4 UI atlas
  float clearR{0.0f};
  float clearG{0.0f};
  float clearB{0.0f};
  float clearA{1.0f};
  laige::render::Mat4 matrix;  // uiScreenMatrix(1280, 720)
  std::uint32_t uiAtlasId{kUiAtlasId};
  std::uint32_t fontAtlasId{kFontAtlasId};

  // The UI-pass options of the scene (the default font, its texture id,
  // the 1x scale — the smoke fixture's options).
  [[nodiscard]] laige::render::UiDeclareOptions declareOptions() const {
    return laige::render::UiDeclareOptions{font.get(), fontAtlasId, 1};
  }
};

laige::Result<UiScene> buildUiScene(
    std::span<const std::uint8_t> fontBytes) {
  using laige::render::GlyphAtlas;
  using laige::render::StringRef;
  using laige::render::StringTable;
  using laige::render::TextAlignment;
  using laige::render::TextItem;
  using laige::render::UiTree;
  using laige::render::UiWidgetKind;
  using laige::render::UiWidgetSpec;
  using laige::render::SpriteUvRect;

  auto fontR = GlyphAtlas::create(fontBytes, GlyphAtlas::Options{});
  if (fontR.isError()) {
    return laige::Result<UiScene>::failure(fontR.error());
  }
  UiScene scene;
  scene.font = std::make_unique<GlyphAtlas>(std::move(fontR).takeValue());
  scene.uiAtlas = uiAtlasRgba();

  std::vector<std::uint8_t> rgba(1024u * 1024u * 4u);
  if (!laige::render::expandGlyphAtlasRgba8(*scene.font, rgba).ok()) {
    return laige::Result<UiScene>::failure(laige::ErrorCode::InvalidArgument);
  }
  scene.fontRgba = std::move(rgba);

  auto tableR = StringTable::create(StringTable::Options{16, 64});
  if (tableR.isError()) {
    return laige::Result<UiScene>::failure(tableR.error());
  }
  scene.table = std::move(tableR).takeValue();
  auto intern = [&](const char* s) -> laige::Result<StringRef> {
    std::vector<std::uint32_t> codes;
    for (const char* c = s; *c != '\0'; ++c) {
      codes.push_back(
          static_cast<std::uint32_t>(static_cast<std::uint8_t>(*c)));
    }
    return scene.table.intern(codes);
  };
  const auto mR = intern("M");
  if (mR.isError()) {
    return laige::Result<UiScene>::failure(mR.error());
  }
  const auto aR = intern("a");
  if (aR.isError()) {
    return laige::Result<UiScene>::failure(aR.error());
  }
  const StringRef mRef = mR.value();
  const StringRef aRef = aR.value();

  auto treeR = UiTree::create(UiTree::Options{
      64, 16, static_cast<std::int32_t>(kGoldenWidth),
      static_cast<std::int32_t>(kGoldenHeight)});
  if (treeR.isError()) {
    return laige::Result<UiScene>::failure(treeR.error());
  }
  scene.tree = std::move(treeR).takeValue();

  // P  opaque Panel  (10, 10, 40, 30)  uv texel (0,0) red    atlas 7
  UiWidgetSpec s;
  s.kind = UiWidgetKind::Panel;
  s.opaque = true;
  s.x = 10;
  s.y = 10;
  s.width = 40;
  s.height = 30;
  s.image = SpriteUvRect{0.0f, 0.0f, 0.25f, 0.25f};
  s.atlasId = kUiAtlasId;
  if (!scene.tree.addChild(s).ok()) {
    return laige::Result<UiScene>::failure(laige::ErrorCode::InvalidArgument);
  }

  // B  Button  (16, 16, 20, 14)  uv texel (1,0) green  label "M"
  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Button;
  s.x = 16;
  s.y = 16;
  s.width = 20;
  s.height = 14;
  s.image = SpriteUvRect{0.25f, 0.0f, 0.5f, 0.25f};
  s.atlasId = kUiAtlasId;
  s.label = mRef;
  if (!scene.tree.addChild(s).ok()) {
    return laige::Result<UiScene>::failure(laige::ErrorCode::InvalidArgument);
  }

  // T  Text  (70, 12, 20, 20)  "a" @ (80, 20) left   atlas 9
  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Text;
  s.x = 70;
  s.y = 12;
  s.width = 20;
  s.height = 20;
  TextItem item;
  item.text = aRef;
  item.font = scene.font.get();
  item.x = 80.0f;
  item.y = 20.0f;
  item.alignment = TextAlignment::Left;
  item.scale = 1;
  item.maxWidth = 0;
  item.atlasId = kFontAtlasId;
  s.item = item;
  if (!scene.tree.addChild(s).ok()) {
    return laige::Result<UiScene>::failure(laige::ErrorCode::InvalidArgument);
  }

  // I  Image  (80, 40, 16, 16)  uv texel (2,0) blue
  s = UiWidgetSpec{};
  s.kind = UiWidgetKind::Image;
  s.x = 80;
  s.y = 40;
  s.width = 16;
  s.height = 16;
  s.image = SpriteUvRect{0.5f, 0.0f, 0.75f, 0.25f};
  s.atlasId = kUiAtlasId;
  if (!scene.tree.addChild(s).ok()) {
    return laige::Result<UiScene>::failure(laige::ErrorCode::InvalidArgument);
  }

  // S  Slider  (10, 50, 40, 8)  [0, 4] value 2  uv texel (3,0) white
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
  s.atlasId = kUiAtlasId;
  if (!scene.tree.addChild(s).ok()) {
    return laige::Result<UiScene>::failure(laige::ErrorCode::InvalidArgument);
  }

  if (!scene.tree.layout().ok()) {
    return laige::Result<UiScene>::failure(laige::ErrorCode::InvalidArgument);
  }
  scene.matrix = laige::render::uiScreenMatrix(
      static_cast<std::int32_t>(kGoldenWidth),
      static_cast<std::int32_t>(kGoldenHeight));
  return laige::Result<UiScene>(std::move(scene));
}

}  // namespace laige::testing::golden
