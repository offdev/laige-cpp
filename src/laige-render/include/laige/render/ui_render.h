// laige-render UI render pass (M2-UI-02): the draw half of FR-2.8 —
// the screen-space UI pass that turns the M2-UI-01 retained widget
// tree into batched quads + text for the M2-SPRITE-02 submit stage.
//
// FR-2.8: "Text rendering ...; immediate-mode-style UI widget tree
// (panel, button, text, image, list, slider, input) with a retained
// mode for menus; UI is a separate render pass, screen-space." This
// header is the pass's DECLARE half (S-5: declaration, not draw):
// `declareUi` walks the tree's laid-out rects in the deterministic
// preorder (the M2-UI-01 traversal / draw order, RENDER-003), culls
// the invisible widgets (the header's culling section), and declares
// one batched quad per visible widget (plus the glyph quads of the
// text widgets and the pass-generated button labels / input content
// through the M2-TEXT-02 `declareText` path). The GL half is the
// M2-SPRITE-02 `SpriteRenderer::submit` with the UI pass's identity
// screen matrix (`uiScreenMatrix` below — screen px, y down -> NDC;
// nothing world-space leaks in, RENDER-006). One texture per atlas:
// the scene binds its UI atlas and its glyph atlas(es) through the
// existing `bindAtlas` / `bindSdfAtlas` (M2-SPRITE-02); the pass adds
// no texture machinery of its own.
//
//   UiDeclareOptions    The pass's scene configuration (the default
//                      UI font + its texture id + its integer scale)
//   uiScreenMatrix      The UI pass's identity screen matrix (the
//                      submit stage's uWorldToNdc)
//   declareUi           The O(n) tree -> batched-quads pass (the
//                      culling included)
//
// ---------------------------------------------------------------------------
// The model (documented units)
// ---------------------------------------------------------------------------
//
// All values are SCREEN PIXELS, screen space, y DOWN — the UI pass's
// coordinate system (the M2-UI-01 tree's, the M2-TEXT-02 items'). The
// SCREEN RECT is the tree's root rect (`UiTree::Options`
// screenWidth/Height, default 1280 x 720 — the M2-GOLD-01 reference
// resolution): the root is the layout-only screen container, and the
// pass reads the rect from the laid-out root (valid at create — the
// root's rect is initialized to the screen rect, not zero).
//
// Every widget quad is the M2-SPRITE-02 centered quad:
// `pos = (rectX + rectW/2, rectY + rectH/2)`, `scale = (rectW, rectH)`
// — the quad's ±0.5·scale spans exactly the widget's laid-out rect
// (one presentation-only float conversion per field, ARCH-009; the
// rect is exact int32, the M2-UI-01 layout contract).
//
// The UI pass's ORDER is the tree's traversal order (preorder,
// children in insertion order — the M2-UI-01 draw-order contract,
// RENDER-003): the pass hands the batcher a MONOTONE per-node UI z —
// the index of the node among the nodes that declare, in traversal
// order (a culled node, a list, a non-opaque panel, and an input
// without a default font take no z — a Text widget with empty
// content still takes one: its declareText succeeds, with no quads).
// Every declared quad carries `depthKey = the node's z` and
// `depthOverride = true` (the G-R11 escape hatch: the UI pass is
// screen-space, there is no isoDepthKey in this pass — the manual UI
// z is the documented order value, counted + warned at build by the
// batcher, the sprite_batcher.h G-R11 contract). Within one node the
// declaration order is the draw order (the art quad before its label
// glyphs, the slider track before its fill — the batcher's stable
// sort keeps it for the equal-key instances).
//
// Every widget quad (and the pass-generated text) uses materialId 0
// and `BlendMode::Alpha` (the M2 UI convention — per-widget blend /
// material is not part of the M2-UI-01 node model; the additive
// widget list is M4). A node's `atlasId` is its UI atlas's texture id
// (the scene's `bindAtlas` id); the pass-generated text uses
// `UiDeclareOptions::fontAtlasId` (the scene's `bindAtlas` id of the
// default font).
//
// ---------------------------------------------------------------------------
// The per-widget rendering model (the seven ADR 0006 kinds)
// ---------------------------------------------------------------------------
//
//   Panel   OPAQUE only: one background quad over the whole rect (the
//           node's `image` UV rect, `atlasId`, `tint`). A non-opaque
//           panel declares nothing (its children still render — they
//           come LATER in traversal order, so they draw on top).
//   Button  one ART quad over the whole rect (the node's `image` UV
//           rect, `atlasId`, `tint`) + the LABEL (when the pass has a
//           default font): a M2-TEXT-02 `TextItem` built by the pass
//           on the stack — the node's `label` handle, CENTER-aligned
//           on the rect's center x, the first line's baseline
//           vertically centered (`rectY + (rectH + lineStep)/2`, the
//           exact int64 line step `lineHeight() · fontScale`), the
//           node's `tint` as the text color, `maxWidth` 0 (no wrap —
//           a button label is one line), the pass's z, the
//           `fontAtlasId`.
//   Text    the node's `TextItem` verbatim, EXCEPT the pass overwrites
//           `item.depthKey` with the widget's z (the item's hand-typed
//           z is the free-standing text's order value; in the tree
//           the traversal order owns the z — the item's font /
//           alignment / wrap / scale / color / atlas / material /
//           blend are the game's). The widget's rect is used for the
//           CULLING only: the glyphs are not clipped (the scene sizes
//           the rect from `measureText` — the M2-UI-01 panel-sizing
//           read; no clipping quads in M2).
//   Image   one quad over the whole rect (the node's `image` UV rect,
//           `atlasId`, `tint`).
//   List    NOTHING (a container with no M2 background option — the
//           scene gives it a background with a nested opaque panel,
//           the M2-UI-01 stacking convention: the panel is the list's
//           FIRST child, so it draws first, behind the rows).
//   Slider  a TRACK quad over the whole rect (the node's `image` UV
//           rect, `atlasId`, `tint`) + a FILL quad spanning
//           `[rectX, rectX + rectW · t) x [rectY, rectY + rectH)` at
//           the full height, `t = (value − min) / (max − min)`
//           (presentation float, the node's `sliderValue`; min < max
//           is the addChild contract). The fill is declared only for
//           `0 < t < 1` (t = 0 is a zero-width quad — degenerate GL
//           work; t = 1 double-blends the identical track pixels —
//           the alpha blend is not idempotent). The fill samples the
//           node's `image` rect at the full UV span (a solid-color UI
//           atlas region — the scene convention — makes the stretch
//           invisible).
//   Input   the CONTENT (when the pass has a default font): a
//           M2-TEXT-02 `TextItem` built by the pass — the node's
//           `text` handle, LEFT-aligned at `rectX`, the baseline
//           vertically centered as for the button label, the WHITE
//           color (the node has no text-color field in M2),
//           `maxWidth` 0. No background quad (the node has no art
//           field in M2 — the scene wraps an input in an opaque
//           panel for a visible field).
//
// A widget that declares nothing (culled, a non-opaque panel, a
// list, an input without a default font, a text widget with empty
// content that still consumes its z) leaves the rest of the frame
// untouched: the pass is a pure declaration walk, the batcher owns
// the frame state.
//
// ---------------------------------------------------------------------------
// Culling (the documented simple model)
// ---------------------------------------------------------------------------
//
// A widget is CULLED (declares nothing) when:
//
//   1. OFFSCREEN: its laid-out rect has zero area (rectW or rectH
//      is 0 — a fresh / zero-sized widget renders nothing, the
//      M2-UI-01 contract), or it lies ENTIRELY outside the screen
//      rect: `rectX >= screenW`, `rectY >= screenH`, `rectX + rectW
//      <= 0`, or `rectY + rectH <= 0` (half-open edges — the
//      iso-picking boundary rule; a widget touching the screen's
//      right or bottom edge with its last visible column/row is
//      drawn). The rect coordinates are saturated int32
//      (±(2^21 − 1), the M2-UI-01 domain); the half-open right /
//      bottom edges are exact int32 sums (a coordinate ±(2^21 − 1)
//      plus an extent ≤ 2^21 − 1 reaches at most 2^22 − 2, well
//      inside int32).
//
//   2. OCCLUDED: a widget is FULLY COVERED by an opaque panel drawn
//      after it (LATER in traversal order — on top, the painter's
//      order): an opaque panel P culles a node N iff P is later
//      than N in the traversal and P's laid-out rect contains N's
//      (all four half-open edges: `P.x <= N.x`, `P.y <= N.y`,
//      `P.x + P.w >= N.x + N.w`, `P.y + P.h >= N.y + N.h`). This is
//      the ADR 0006 occlusion model: the opaque panel that occludes
//      the pointer to a widget occludes its render too (the
//      M2-UI-01 hit test's "opaque panel in front" clause). An
//      opaque panel NEVER culls its own children (they are later in
//      the traversal — they draw on top of the background); a
//      non-opaque panel never culls anything; a partial overlap
//      culls nothing (M2 has no clipping quads — the visible part
//      of the covered widget still draws).
//
// The offscreen test is O(1) per node. The occlusion test re-walks
// the traversal from the node's successor to the end, stopping at
// the FIRST later opaque panel that contains it (the early stop —
// the common case when a modal panel is present); worst case O(n)
// per node, O(n²) per frame (a menu-scale UI — a few hundred
// widgets, a few opaque panels — measures far below the frame
// budget; the M2-SCENE-01 reference scene's UI is a single panel).
// No allocation: the walk re-uses the tree's structural links.
// ponytail: O(n²) ceiling on a deep/large UI; upgrade if a large
// scene measures hot: collect the frame's opaque panels into a
// create-sized scratch (one u32 + rect per panel, ≤ maxWidgets) and
// test each node only against the panels after it — O(n·P).

// The header continues below the includes.

#pragma once

#include <cstdint>

#include "laige/errors.h"
#include "laige/render/font.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/text_items.h"
#include "laige/render/ui_widgets.h"
#include "laige/result.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// The pass's scene configuration
// ---------------------------------------------------------------------------

// The UI pass's scene configuration (API-006): the DEFAULT UI FONT
// (non-owning — the scene's GlyphAtlas, the M2-TEXT-01/03 atlas; it
// must outlive every frame that declares through it, the
// TextItem.font lifetime contract), its texture id (the scene's
// `bindAtlas` id for that atlas — the pass-generated text's
// `atlasId`), and its integer scale (the M2-TEXT-01 convention,
// domain [1, kTextMaxScale] — validated at `declareUi`). A NULL
// `defaultFont` turns off the pass-generated text: button labels and
// input content declare nothing (the art / art-less widgets still
// render — the scene composes a Text widget for its labels when it
// wants them without a default font).
struct UiDeclareOptions {
  // The default UI font (non-owning). null = no pass-generated text.
  const GlyphAtlas* defaultFont{};
  // The default font's texture id (the scene's `bindAtlas` id —
  // read when `defaultFont != nullptr`).
  std::uint32_t fontAtlasId{0};
  // The integer scale of the pass-generated text. Domain
  // [1, kTextMaxScale]; 0 or > kTextMaxScale with a font fails
  // `declareUi` (InvalidArgument, nothing declared).
  std::uint32_t fontScale{1};
};

// ---------------------------------------------------------------------------
// The UI pass's identity screen matrix (RENDER-006)
// ---------------------------------------------------------------------------

// The UI pass's screen matrix: maps screen px (y DOWN) to NDC,
//   x_ndc = 2·x/width − 1,   y_ndc = 1 − 2·y/height
// — the submit stage's `uWorldToNdc` for the UI pass (the identity
// CAMERA of FR-2.8: the UI renders screen-space, nothing
// world-space leaks in, RENDER-006). The frame buffer / FBO the
// matrix addresses MUST be the same width/height (the tree's screen
// rect — `UiTree::Options`, the M2-GOLD-01 reference resolution when
// it matters).
//
// Column-major Mat4 (the house convention): the linear part in
// columns 0..1, the translation in column 3. Total function: a
// dimension outside [1, kUiMaxDimPx] is unrepresentable — the
// identity matrix is returned (the caller passes the tree's screen
// rect, which is in domain by construction). O(1), no allocation.
[[nodiscard]] inline Mat4 uiScreenMatrix(std::int32_t width,
                                         std::int32_t height) noexcept {
  Mat4 m(1.0f);  // the identity (the out-of-domain fallback)
  if (width >= 1 && width <= static_cast<std::int32_t>(kUiMaxDimPx) &&
      height >= 1 && height <= static_cast<std::int32_t>(kUiMaxDimPx)) {
    m[0].x = 2.0f / static_cast<float>(width);
    m[1].y = -2.0f / static_cast<float>(height);
    m[3].x = -1.0f;
    m[3].y = 1.0f;
  }
  return m;
}

namespace detail {

// The offscreen cull (the header's culling section, rule 1): true
// when the node's laid-out rect has zero area or lies entirely
// outside the screen rect (half-open edges — the iso-picking
// boundary rule). The rect sums are exact int32 (the header's
// culling section: the saturation domain bounds the edge to
// 2^22 − 2).
[[nodiscard]] inline bool uiRectOffscreen(const UiNode& n,
                                          std::int32_t screenW,
                                          std::int32_t screenH) noexcept {
  if (n.rectW <= 0 || n.rectH <= 0) return true;
  return n.rectX >= screenW || n.rectY >= screenH ||
         n.rectX + n.rectW <= 0 || n.rectY + n.rectH <= 0;
}

// The occlusion containment (the header's culling section, rule 2):
// true when the (opaque panel's) rect `p` fully contains the node's
// rect `n` (all four half-open edges — exact int32, the same domain
// argument as uiRectOffscreen).
[[nodiscard]] inline bool uiRectCovers(const UiNode& p,
                                       const UiNode& n) noexcept {
  return p.rectX <= n.rectX && p.rectY <= n.rectY &&
         p.rectX + p.rectW >= n.rectX + n.rectW &&
         p.rectY + p.rectH >= n.rectY + n.rectH;
}

// The traversal-order walk cursor (the M2-UI-01 preorder — parent
// before children, children in insertion order, the draw order):
// the current node + one step to the successor (the first child,
// else the next sibling, else the nearest ancestor with a next
// sibling; the end at the traversal's end). Iterative (no
// recursion — a 65k-deep chain must not overflow the stack), O(1)
// AMORTIZED per step (each parent climb exits one subtree; a full
// traversal climbs each edge at most once). The cursor holds no
// tree state of its own (it re-walks the structural links — the
// culling's allocation-free contract). Note: the Pool API has no
// const element read (pools.h), so the tree is taken non-const
// (the M2-UI-01 house note — a logically const walk).
struct UiPreorderCursor {
  UiTree* tree;  // non-owning (the cursor outlives nothing)
  UiWidgetId cur{};

  [[nodiscard]] bool end() const noexcept {
    return cur == kInvalidUiWidgetId;
  }
  [[nodiscard]] UiWidgetId id() const noexcept { return cur; }
  [[nodiscard]] const UiNode* node() const noexcept {
    return tree->node(cur);
  }
  void step() noexcept {
    const UiNode* n = tree->node(cur);
    if (n == nullptr) {
      cur = kInvalidUiWidgetId;
      return;
    }
    if (tree->isValid(n->firstChild)) {
      cur = n->firstChild;
      return;
    }
    if (tree->isValid(n->nextSibling)) {
      cur = n->nextSibling;
      return;
    }
    UiWidgetId a = n->parent;
    while (tree->isValid(a)) {
      const UiNode* pa = tree->node(a);
      if (pa == nullptr) break;
      if (tree->isValid(pa->nextSibling)) {
        cur = pa->nextSibling;
        return;
      }
      a = pa->parent;
    }
    cur = kInvalidUiWidgetId;
  }
};

// The occlusion cull (the header's culling section, rule 2): true
// when a LATER node in the traversal (drawn on top) is an opaque
// panel whose rect fully contains the node's. The walk starts at
// the node's successor (one cursor step — one amortized-O(1) climb)
// and stops at the first containing opaque panel (the early stop).
[[nodiscard]] inline bool uiOccluded(UiTree& tree, UiWidgetId id,
                                    const UiNode& n) noexcept {
  UiPreorderCursor c{&tree, id};
  c.step();  // past the node itself
  while (!c.end()) {
    const UiNode* p = c.node();
    if (p != nullptr && p->kind == UiWidgetKind::Panel && p->opaque &&
        uiRectCovers(*p, n)) {
      return true;
    }
    c.step();
  }
  return false;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The UI tree -> batched-quads pass (the M2-UI-02 declare half)
// ---------------------------------------------------------------------------

// Walks the laid-out `tree` in its deterministic preorder (the
// M2-UI-01 draw order, RENDER-003), culls the invisible widgets (the
// header's culling section — offscreen + occluded by a later opaque
// panel), and declares the visible widgets' quads (and the text
// glyphs of the Text widgets and the pass-generated button labels /
// input content) into `batcher` (the header's per-widget model):
// O(n) per frame plus the culling's worst case (the header's
// culling section — menu-scale UIs are negligible), NO ALLOCATION
// (the cursor re-walks the tree's links; the pass-generated
// TextItems are stack values — PERF-003), no logging on the happy
// path (the batcher's G-R11 warn at build is the batcher's own
// cold, rate-limited path, LOG-003/004), no GL (the submit half is
// the M2-SPRITE-02 `SpriteRenderer::submit`).
//
// The batcher must be live and its frame window OPEN (the frame
// protocol, the `declareParticles` / `declareText` precedent): a
// stopped batcher fails the first add with BudgetExhausted, a
// closed window with InvalidArgument. The pass assigns the UI z
// (the header's model section): every node that declares something
// takes the next z in traversal order; its quads / glyphs carry
// `depthKey = z`, `depthOverride = true` (G-R11 — counted + warned
// at build).
//
// Fails (first failure wins, no log — the header's model section):
// a stopped tree, or a `fontScale` outside [1, kTextMaxScale] with a
// default font -> InvalidArgument BEFORE the first add (nothing
// declared); a text handle the table does not own (a Text widget's
// `item.text`, a Button's `label`, an Input's `text` — through
// `declareText`'s validation) or a stopped/missing font (a Text
// widget's mode-selected font — through `declareText`) ->
// InvalidArgument; a failed batcher add (stopped batcher ->
// BudgetExhausted, closed window -> InvalidArgument) -> the
// batcher's error. A failed call leaves the quads already declared
// BY THIS CALL in the frame (the batcher owns the frame state — the
// `declareParticles` precedent).
//
// Call site: once per frame per UI scene, in the RENDER phase (the
// M2-GL-02 cull/batch stage of the UI pass — AFTER the tree's
// `layout()` / `handlePointer()` frame protocol, BEFORE the
// batcher's `build()` and the `SpriteRenderer::submit`), NEVER in
// the simulation tick (ARCH-002). The per-frame UI cost is part of
// the composite 50k render-CPU budget (PRD §8.1 — measured with
// M2-PERF-01; no standalone budgets.json entry for this step).
//
// Misuse warnings:
//   - Do not call `declareUi` without `layout()` first: a node added
//     or re-placed since the last layout has a ZERO rect and is
//     culled as offscreen (the frame protocol calls layout() every
//     frame — the M2-UI-01 contract);
//   - Do not size the UI batcher for the world pass only: the UI
//     pass is its OWN batcher (one batcher per sprite pass,
//     M2-SPRITE-01), sized for the frame's UI quads + glyph count;
//   - Do not expect sub-pixel text: the font scale is INTEGER (the
//     M2-TEXT-01 convention);
//   - The UI matrix and the FBO must match the tree's screen rect:
//     `uiScreenMatrix(screenWidth, screenHeight)` with a frame
//     buffer of the same size (the M2-GOLD-01 reference resolution
//     1280 x 720 when it matters).
//
// Canonical narrative: docs/concepts/coordinates.md §4.14 (ARCH-008);
// API contract: docs/api/ui_render.md.
[[nodiscard]] inline laige::Status declareUi(SpriteBatcher& batcher,
                                             const StringTable& table,
                                             UiTree& tree,
                                             const UiDeclareOptions& options) noexcept {
  // The create-level preconditions (first failure wins, InvalidArgument,
  // no log, nothing declared — the header's failure section).
  if (!tree.valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  if (options.defaultFont != nullptr &&
      (options.fontScale < 1 || options.fontScale > kTextMaxScale)) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  const UiNode* root = tree.node(tree.root());
  if (root == nullptr) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  // The screen rect (the header's model section): the laid-out root.
  const std::int32_t screenW = root->rectW;
  const std::int32_t screenH = root->rectH;
  const std::int64_t lineStep =
      options.defaultFont != nullptr
          ? static_cast<std::int64_t>(options.defaultFont->lineHeight()) *
                static_cast<std::int64_t>(options.fontScale)
          : 0;

  // One widget quad over the node's whole rect (the header's model
  // section): the centered quad, the node's art fields, the UI pass
  // convention (materialId 0, Alpha), the G-R11 UI z.
  auto declareQuad = [&](const UiNode& n,
                         std::uint32_t z) -> laige::Status {
    SpriteItem q;
    q.pos = Vec2{static_cast<float>(n.rectX + n.rectW / 2),
                 static_cast<float>(n.rectY + n.rectH / 2)};
    q.scale = Vec2{static_cast<float>(n.rectW),
                   static_cast<float>(n.rectH)};
    q.rotation = 0.0f;
    q.frameIndex = 0;
    q.uv = n.image;
    q.tint = n.tint;
    q.atlasId = n.atlasId;
    q.materialId = 0;
    q.blend = BlendMode::Alpha;
    q.depthKey = z;
    // The G-R11 escape hatch: the UI pass's manual z (the
    // sprite_batcher.h contract — counted + warned at build).
    q.depthOverride = true;
    const auto r = batcher.add(std::move(q));
    if (!r.ok()) {
      return laige::Status::failure(r.error());
    }
    return laige::Status{};
  };

  // The pass-generated text (the header's per-widget model): the
  // button label (centered) and the input content (left). The handle
  // validation is declareText's (first failure wins — the header's
  // failure section).
  auto declarePassText = [&](const UiNode& n, StringRef text,
                             float x, TextAlignment alignment,
                             SpriteTint color,
                             std::uint32_t z) -> laige::Status {
    TextItem it;
    it.text = text;
    it.font = options.defaultFont;  // non-null (the caller's branch)
    it.mode = FontRenderMode::Bitmap;
    it.scale = options.fontScale;
    it.alignment = alignment;
    it.x = x;
    // The baseline, vertically centered (exact int64 — the
    // header's per-widget model).
    it.y = static_cast<float>(static_cast<std::int64_t>(n.rectY) +
                              (static_cast<std::int64_t>(n.rectH) + lineStep) /
                                  2);
    it.color = color;
    it.maxWidth = 0;
    it.depthKey = z;
    it.atlasId = options.fontAtlasId;
    it.materialId = 0;
    it.blend = BlendMode::Alpha;
    return declareText(batcher, table, it);
  };

  // The walk (the header's model section): the preorder, the cull,
  // the per-node declare, the monotone UI z.
  std::uint32_t z = 0;
  detail::UiPreorderCursor c{&tree, tree.root()};
  while (!c.end()) {
    const UiWidgetId id = c.id();
    const UiNode* n = c.node();
    c.step();
    if (n == nullptr || n == root) continue;  // the root: layout-only
    if (detail::uiRectOffscreen(*n, screenW, screenH)) continue;
    if (detail::uiOccluded(tree, id, *n)) continue;
    bool declared = false;
    laige::Status s{};
    switch (n->kind) {
      case UiWidgetKind::Panel:
        if (n->opaque) {
          s = declareQuad(*n, z);
          if (s.ok()) declared = true;
        }
        break;
      case UiWidgetKind::Button:
        s = declareQuad(*n, z);
        if (!s.ok()) break;
        declared = true;
        if (options.defaultFont != nullptr) {
          s = declarePassText(*n, n->label,
                              static_cast<float>(n->rectX + n->rectW / 2),
                              TextAlignment::Center, n->tint, z);
          if (!s.ok()) break;
        }
        break;
      case UiWidgetKind::Text:
        {
          TextItem it = n->item;
          // The tree owns the order (the header's per-widget model):
          // the pass overwrites the item's hand-typed z.
          it.depthKey = z;
          s = declareText(batcher, table, it);
          if (s.ok()) declared = true;
        }
        break;
      case UiWidgetKind::Image:
        s = declareQuad(*n, z);
        if (s.ok()) declared = true;
        break;
      case UiWidgetKind::List:
        // NOTHING (the header's per-widget model): the container has
        // no M2 background option.
        break;
      case UiWidgetKind::Slider:
        if (n->rectW > 0 && n->rectH > 0) {
          s = declareQuad(*n, z);  // the track (the whole rect)
          if (!s.ok()) break;
          declared = true;
          // The fill (the header's per-widget model): only 0 < t < 1.
          const float t =
              (n->sliderValue - n->sliderMin) / (n->sliderMax - n->sliderMin);
          if (t > 0.0f && t < 1.0f) {
            const float fillW = static_cast<float>(n->rectW) * t;
            SpriteItem f;
            f.pos = Vec2{static_cast<float>(n->rectX) + fillW * 0.5f,
                         static_cast<float>(n->rectY + n->rectH / 2)};
            f.scale = Vec2{fillW, static_cast<float>(n->rectH)};
            f.rotation = 0.0f;
            f.frameIndex = 0;
            f.uv = n->image;
            f.tint = n->tint;
            f.atlasId = n->atlasId;
            f.materialId = 0;
            f.blend = BlendMode::Alpha;
            f.depthKey = z;
            f.depthOverride = true;  // G-R11 (the track's quad)
            const auto r = batcher.add(std::move(f));
            if (!r.ok()) {
              s = laige::Status::failure(r.error());
              break;
            }
          }
        }
        break;
      case UiWidgetKind::Input:
        if (options.defaultFont != nullptr) {
          s = declarePassText(*n, n->text, static_cast<float>(n->rectX),
                              TextAlignment::Left, SpriteTint{}, z);
          if (s.ok()) declared = true;
        }
        break;
    }
    if (s.ok() && declared) ++z;
    if (!s.ok()) return s;  // first failure wins (the header's failure)
  }
  return laige::Status{};
}

}  // namespace laige::render
