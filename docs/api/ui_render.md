# UI render pass — the screen-space draw half of FR-2.8 (`laige::render` ui_render)

`laige/render/ui_render.h` (M2-UI-02) is the **declare half** (S-5) of
the FR-2.8 UI work: it turns the M2-UI-01 retained widget tree
(`laige/render/ui_widgets.h`) into batched quads + text for the
M2-SPRITE-02 submit stage (`laige/render/sprite_renderer.h`). The GL
half is the existing `SpriteRenderer::submit` with the UI pass's
identity screen matrix; this header adds no GL calls of its own.

FR-2.8: "Text rendering …; immediate-mode-style UI widget tree
(panel, button, text, image, list, slider, input) with a retained
mode for menus; **UI is a separate render pass, screen-space**." This
pass is that separate pass: it runs after the world pass, on its own
`SpriteBatcher`, with an identity camera — nothing world-space leaks
in (RENDER-006). One texture per atlas: the scene binds its UI atlas
and its glyph atlas(es) through the existing `bindAtlas` /
`bindSdfAtlas`; the pass adds no texture machinery.

## The API

| Symbol | Kind | Purpose |
| --- | --- | --- |
| `UiDeclareOptions` | struct | The pass's scene configuration: the default UI font (non-owning `const GlyphAtlas*`), its `bindAtlas` id (`fontAtlasId`), its integer scale (`fontScale`, domain [1, `kTextMaxScale`]). A null `defaultFont` turns off the pass-generated text (button labels and input content declare nothing; the art still renders). |
| `uiScreenMatrix(width, height)` | `Mat4` | The UI pass's identity screen matrix (screen px, y down → NDC): `x_ndc = 2·x/width − 1`, `y_ndc = 1 − 2·y/height`. Column-major; the linear part in columns 0..1, the translation in column 3. Out-of-domain dimension (outside [1, `kUiMaxDimPx`]) → the identity matrix (the caller passes the tree's screen rect, which is in domain by construction). |
| `declareUi(batcher, table, tree, options)` | `Status` | The O(n) tree → batched-quads pass (culling included). See the frame protocol below. |

`declareUi` requires the tree to be laid out (`layout()` first — a
node added or re-placed since the last layout has a zero rect and is
culled as offscreen) and the batcher to be in an open frame window
(`beginFrame` before, `build` after — the M2-SPRITE-01 frame
protocol).

## The per-widget rendering model (the seven ADR 0006 kinds)

All values are screen pixels, y down (the M2-UI-01 tree's and the
M2-TEXT-02 items' coordinate system). Every widget quad is the
M2-SPRITE-02 centered quad: `pos = (rectX + rectW/2, rectY + rectH/2)`,
`scale = (rectW, rectH)` — the quad's ±0.5·scale spans exactly the
widget's laid-out rect. Every quad (and the pass-generated text) uses
`materialId 0` and `BlendMode::Alpha`; a node's `atlasId` is its UI
atlas's texture id, the pass-generated text uses
`UiDeclareOptions::fontAtlasId`.

| Kind | What it declares |
| --- | --- |
| Panel | OPAQUE only: one background quad over the whole rect (the node's `image` UV rect, `atlasId`, `tint`). A non-opaque panel declares nothing (its children still render — they come later in traversal order, so they draw on top). |
| Button | One ART quad over the whole rect + the LABEL (with a default font): a stack-built M2-TEXT-02 `TextItem` — the node's `label` handle, CENTER-aligned on the rect's center x, the first line's baseline vertically centered (`rectY + (rectH + lineStep)/2`, exact int64, `lineStep = lineHeight() · fontScale`), the node's `tint` as the text color, `maxWidth` 0 (one line), the pass's z, the `fontAtlasId`. |
| Text | The node's `TextItem` verbatim, **except** the pass overwrites `item.depthKey` with the widget's z (in the tree the traversal order owns the z). The widget's rect is used for CULLING only: the glyphs are not clipped (the scene sizes the rect from `measureText`; no clipping quads in M2). |
| Image | One quad over the whole rect. |
| List | NOTHING (a container with no M2 background option — the scene gives it a background with a nested opaque panel as the list's first child, the M2-UI-01 stacking convention). |
| Slider | A TRACK quad over the whole rect + a FILL quad spanning `[rectX, rectX + rectW·t) × [rectY, rectY + rectH)` at full height, `t = (value − min)/(max − min)`. The fill is declared only for `0 < t < 1` (t = 0 is a zero-width quad; t = 1 double-blends the identical track pixels — the alpha blend is not idempotent). The fill samples the node's `image` rect at the full UV span. |
| Input | The CONTENT (with a default font): a stack-built `TextItem` — the node's `text` handle, LEFT-aligned at `rectX`, the baseline vertically centered as for the button label, WHITE color (no text-color field in M2), `maxWidth` 0. No background quad in M2 (the scene wraps an input in an opaque panel for a visible field). |

A widget that declares nothing (culled, a non-opaque panel, a list,
an input without a default font, empty content) leaves the rest of
the frame untouched: the pass is a pure declaration walk, the batcher
owns the frame state.

## The order (the monotone UI z, G-R11)

The pass's order is the tree's traversal order (preorder, children in
insertion order — the M2-UI-01 draw-order contract, RENDER-003). The
pass hands the batcher a **monotone per-node z**: the index of the
node among the nodes that declare something, in traversal order (a
culled / no-declare node consumes no z — but a Text widget with empty
content still consumes its z, and an Input without a font does not).
Every declared quad carries `depthKey = the node's z` and
`depthOverride = true` (the G-R11 escape hatch — the UI pass is
screen-space, there is no `isoDepthKey` in this pass; the manual UI z
is counted + warned at build by the batcher, the sprite_batcher.h
G-R11 contract). Within one node the declaration order is the draw
order (art before label glyphs, track before fill — the batcher's
stable sort keeps it for the equal-key instances).

## Culling (the documented simple model)

A widget is **culled** (declares nothing) when:

1. **Offscreen** — its laid-out rect has zero area (`rectW` or
   `rectH` is 0 — a fresh / zero-sized widget renders nothing), or it
   lies entirely outside the screen rect: `rectX >= screenW`,
   `rectY >= screenH`, `rectX + rectW <= 0`, or `rectY + rectH <= 0`
   (half-open edges — the iso-picking boundary rule; a widget
   touching the screen's right or bottom edge with its last visible
   column/row is drawn). O(1) per node; the rect sums are exact int32
   (the M2-UI-01 saturation domain bounds the edge to 2^22 − 2).
2. **Occluded** — it is FULLY COVERED by an opaque panel drawn after
   it (later in traversal order — on top, the painter's order): an
   opaque panel P culles a node N iff P is later than N in the
   traversal and P's laid-out rect contains N's (all four half-open
   edges: `P.x <= N.x`, `P.y <= N.y`, `P.x + P.w >= N.x + N.w`,
   `P.y + P.h >= N.y + N.h`). This is the ADR 0006 occlusion model:
   the opaque panel that occludes the pointer to a widget occludes
   its render too (the M2-UI-01 hit test's "opaque panel in front"
   clause). An opaque panel never culls its own children; a
   non-opaque panel never culls anything; a partial overlap culls
   nothing (M2 has no clipping quads — the visible part of the
   covered widget still draws).

The offscreen test is O(1) per node. The occlusion test re-walks the
traversal from the node's successor to the end, stopping at the
FIRST later opaque panel that contains it (the early stop — the
common case when a modal panel is present); worst case O(n) per
node, O(n²) per frame. No allocation: the walk re-uses the tree's
structural links.

## The frame protocol (the batcher's)

```cpp
uiTree.layout();                      // M2-UI-01 (every frame)
uiBatcher.beginFrame();              // M2-SPRITE-01 frame window
Status s = declareUi(uiBatcher, strings, uiTree, opts);
// … the world pass on its own batcher, its own submit …
uiBatcher.build();                   // the group table (RENDER-003)
renderer.submit(uiBatcher, uiScreenMatrix(screenW, screenH));
```

The UI pass is its **own** batcher (one batcher per sprite pass,
M2-SPRITE-01), sized for the frame's UI quads + glyph count. The
UI matrix and the frame buffer / FBO must match the tree's screen
rect (the M2-GOLD-01 reference resolution 1280×720 when it matters).

## Failure behavior (first failure wins)

| Condition | Error | Declared state |
| --- | --- | --- |
| Stopped tree (default / moved-from), or a missing root | `InvalidArgument` | nothing |
| `fontScale` outside [1, `kTextMaxScale`] with a default font | `InvalidArgument` | nothing (checked before the first add) |
| A Text widget's / button's label's / input's text handle not owned by the string table, or a stopped font (the item's `font` pointer) | `InvalidArgument` | the quads already declared stay in the frame (the batcher owns the frame state — the `declareParticles` / `declareText` precedent) |
| A failed batcher `add` | the batcher's error (`BudgetExhausted` on capacity, `InvalidArgument` on a closed window, …) | same — the quads already declared stay |

No logging on preconditions (the house failure contract).

## Ownership, threading (CORE-009, CONC-001)

- `declareUi` is a pure declaration pass: it reads the tree (laid
  out), the string table, and the font (non-owning `const GlyphAtlas*`
  — the scene's atlas, the `TextItem.font` lifetime contract), and
  writes to the batcher's frame. It does not mutate the tree.
- Owner-thread, render phase (the frame protocol): call it on the
  thread that owns the batcher, between `beginFrame` and `build`.
  No locks, no shared state beyond the documented frame protocol.
- One `declareUi` per frame per batcher (the S-5 declare phase); the
  submit stage draws what was declared.

## Performance (DOC-004)

- `declareUi` is **O(n + G)** CPU: one preorder pass over the n
  nodes (the offscreen test O(1) each, the occlusion test O(n) worst
  case each — O(n²) worst per frame, see Culling), plus the
  per-widget `declareText` walks for the text-bearing widgets (the
  M2-TEXT-02 O(glyphs) each). No allocation in the walk (the z, the
  cursor, and the stack-built `TextItem`s); the batcher's `add` is
  the O(1) frame append (PERF-003).
- The per-frame UI cost is part of the composite 50k render-CPU
  budget — measured with M2-PERF-01; this step has no standalone
  `budgets.json` entry (the 17-entry pin is unchanged).
- One batcher per pass (RENDER-001): the UI groups are (atlas,
  material, blend) — with the M2 conventions (one UI atlas, material
  0, alpha) a typical frame is **two** instanced draw calls (the UI
  atlas + the glyph atlas), regardless of the widget count.
- The batcher's G-R11 `depthOverride` counter is bumped once per
  UI quad; the build stage's rate-limited warn fires when it is
  nonzero (expected for the UI pass — the screen-space manual z IS
  the documented order value).

**Common traps.** Sizing the UI batcher for the world pass only
(capacity under the frame's UI quad count → `BudgetExhausted`);
calling `declareUi` before `layout()` (fresh nodes cull as
zero-area); expecting the slider fill at t = 0 or t = 1 (it is
declared only for `0 < t < 1`); expecting a background under a
non-opaque panel or an input (M2 has none — nest an opaque panel).

## Determinism (ARCH-010)

The pass is deterministic within one build: the traversal order is
the tree's (insertion order, stable), the z is a monotone function of
that order, the culling tests are pure integer comparisons on the
laid-out rects, and the float values (the quad pos/scale, the slider
t, the text layout) are the same presentation floats the M2-UI-01
layout and M2-TEXT-02 declare produce. Same build, same tree state
→ identical declared frame.

## Example (performant path)

```cpp
// Scene set-up (once): the tree, the font, the UI atlas.
UiTree ui = /* M2-UI-01 create + addChild ... */;
GlyphAtlas font = /* M2-TEXT-01 create ... */;
std::vector<std::uint8_t> uiRgba = /* the UI atlas bytes ... */;
renderer.bindAtlas(7, atlasW, atlasH, uiRgba);     // the UI atlas
// (the glyph atlas through bindAtlas / bindSdfAtlas, M2-TEXT-02/03)

SpriteBatcher uiB = /* create({uiCapacity}) ... */;
UiDeclareOptions opts;
opts.defaultFont = &font;
opts.fontAtlasId = 9;          // the glyph atlas's bindAtlas id
opts.fontScale = 1;

// Per frame (the render phase, after the world pass):
ui.layout();
uiB.beginFrame();
Status s = declareUi(uiB, strings, ui, opts);
if (!s.ok()) { /* report — CORE-008 */ }
uiB.build();
renderer.submit(uiB, uiScreenMatrix(1280, 720));
```

## Misuse warnings

- **Do not call `declareUi` without `layout()` first**: a node added
  or re-placed since the last layout has a ZERO rect and is culled
  as offscreen (the frame protocol calls `layout()` every frame —
  the M2-UI-01 contract).
- **Do not size the UI batcher for the world pass only**: the UI
  pass is its own batcher, sized for the frame's UI quads + glyph
  count.
- **Do not expect sub-pixel text**: the font scale is integer (the
  M2-TEXT-01 convention).
- **The UI matrix and the FBO must match the tree's screen rect**:
  `uiScreenMatrix(screenWidth, screenHeight)` with a frame buffer of
  the same size (the M2-GOLD-01 reference resolution 1280×720 when
  it matters).

## Related

- [UI widget tree](ui_widgets.md) — the M2-UI-01 retained tree this
  pass walks (the seven kinds, the layout, the focus, the pointer
  events).
- [Text items](text_items.md) — the M2-TEXT-02 `declareText` path the
  pass uses for the text widgets and the pass-generated labels.
- [Font](font.md) / [SDF glyph atlas](font_sdf.md) — the glyph
  atlases the pass's text draws from.
- [Sprite batcher](sprite_batcher.md) — the frame protocol, the
  (atlas, material, blend) groups, the G-R11 depth override.
- [Sprite renderer](sprite_renderer.md) — the submit stage the UI
  quads draw through (the `bindAtlas` uploads, the stats).
- [Coordinates](../concepts/coordinates.md) — the screen-space →
  NDC identity mapping (§4.14, ARCH-008).
