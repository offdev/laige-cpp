# Text items — the UI-pass text half of FR-2.8 (`laige::render` text_items)

The text-item half of FR-2.8 — "Text rendering: ... bitmap (P0), SDF
(P1)" (M2-TEXT-02/03): `TextItem` declares a block of text into the
sprite batcher as **batched glyph quads in the UI pass** (screen
space, y down). The text bytes come from a **string table** (interned
u32 code points — PRD §10.4: no per-frame `std::string` in the path),
the glyph metrics come from the [bitmap font atlas](font.md)
(M2-TEXT-01) or the [SDF glyph atlas](font_sdf.md) (M2-TEXT-03,
selected per item by `TextItem.mode` — the documented scene-config
key), and each glyph with ink becomes one `SpriteItem` in the batcher
(M2-SPRITE-02). The text renders as ordinary instanced sprite work —
one draw call per (atlas, material, blend) group (RENDER-001), no
separate text pipeline. Both glyph paths share the vendored
rasterizer (ADR 0009) and the identical M2-TEXT-01 metrics: the
layout, the wrap, and `measureText` are the same on both paths (the
width match is exact — tolerance 0, below).

Public header:
`src/laige-render/include/laige/render/text_items.h` (header-only —
no new `.cpp`, no new GL calls; the GL side is the existing
`SpriteRenderer::submit`). Unit suite: `ctest -R text_items`
(`tests/laige-render/text_items_tests.cpp`): the StringTable
contract, the width-measurement goldens (measured against the
committed test font — see
`tests/laige-render/assets/README.md`), the exact wrap model, the
per-glyph quad goldens, the group + order contract, the failure
paths, the 1 000-frame zero-allocation declare loop, and the
offscreen GL smoke (the white-on-alpha upload through the M2-SPRITE-02
submit stage — needs a usable OpenGL 3.3 environment and `GTEST_SKIP`s
where absent). The SDF mode's suites (the width match, the padded-
cell quads, the offscreen SDF render) live in `ctest -R font_sdf`
(font_sdf.md).

## The API

```cpp
// The interned string handle (a small value; never a pointer).
struct StringRef { std::uint32_t id{0}; };

// The scene's string table (PRD §10.4). Move-only; the stopped state
// is the default constructor (create failed / moved-from).
class StringTable {
 public:
  struct Options {
    std::uint32_t maxStrings{kStringTableDefaultStrings};      // 4096
    std::uint32_t maxCodePoints{kStringTableDefaultCodePoints}; // 16384
  };
  [[nodiscard]] static laige::Result<StringTable, laige::ErrorCode>
  create(Options options) noexcept;
  // The interning (the setup path, never per frame): returns the
  // handle of the FIRST interned string with the same bytes (the
  // identity property). BudgetExhausted (+ one rate-limited Warn
  // `string_table/table_full`) when a pool bound is exceeded.
  [[nodiscard]] laige::Result<StringRef, laige::ErrorCode>
  intern(std::span<const std::uint32_t> codes) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool isValid(StringRef ref) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> text(StringRef ref) const noexcept;
  [[nodiscard]] std::uint32_t count() const noexcept;
  [[nodiscard]] std::uint32_t storedCodePoints() const noexcept;
  [[nodiscard]] std::uint32_t stringCapacity() const noexcept;
  [[nodiscard]] std::uint32_t codePointCapacity() const noexcept;
};

enum class TextAlignment : std::uint8_t { Left, Center, Right };

// The glyph path (M2-TEXT-03 — the documented scene-config key).
enum class FontRenderMode : std::uint8_t { Bitmap = 0, Sdf = 1 };

// One text block for the UI pass (screen pixels, y down).
struct TextItem {
  StringRef text{};                  // the string-table handle
  const GlyphAtlas* font{};          // non-owning (scene-owned), Bitmap
  FontRenderMode mode{FontRenderMode::Bitmap}; // the glyph path
  const SdfGlyphAtlas* sdfFont{};    // non-owning (scene-owned), Sdf
  float x{};                         // the block's anchor x (by alignment)
  float y{};                         // the FIRST line's baseline
  std::uint32_t scale{1};            // integer scale, [1, 16]
  SpriteTint color{};                // default white (M2-SPRITE-02 tint)
  TextAlignment alignment{TextAlignment::Left};
  std::int32_t maxWidth{0};          // 0 = no wrap; else [1, 2^20]
  std::uint32_t depthKey{};          // the hand-tuned UI z (G-R11)
  std::uint32_t atlasId{};           // the scene's bind id (per mode)
  std::uint32_t materialId{};        // 0 = the default material
  BlendMode blend{BlendMode::Alpha}; // the text convention
};

// The no-wrap ink-block width in screen px (O(glyphs)). One overload
// per glyph path; the width is the SAME on both (the identical
// M2-TEXT-01 metrics — the width match, tolerance 0).
[[nodiscard]] laige::Result<std::int64_t> measureText(
    const GlyphAtlas& font, const StringTable& table, StringRef text,
    std::uint32_t scale) noexcept;
[[nodiscard]] laige::Result<std::int64_t> measureText(
    const SdfGlyphAtlas& font, const StringTable& table, StringRef text,
    std::uint32_t scale) noexcept;

// The declaration (O(glyphs) quads into the batcher; the frame
// protocol is the batcher's — see the section below). The item's mode
// selects the font (Bitmap: `font`, Sdf: `sdfFont`).
[[nodiscard]] laige::Status declareText(
    SpriteBatcher& batcher, const StringTable& table, TextItem item) noexcept;

// The 8-bit alpha atlas -> white-on-alpha RGBA8 upload bytes
// (the M2-SPRITE-02 `bindAtlas` input; the setup path).
[[nodiscard]] laige::Status expandGlyphAtlasRgba8(
    const GlyphAtlas& atlas, std::span<std::uint8_t> rgba) noexcept;

// The 8-bit SDF field -> white-on-alpha RGBA8 upload bytes
// (the M2-SPRITE-02 `bindSdfAtlas` input; the setup path —
// M2-TEXT-03).
[[nodiscard]] laige::Status expandSdfGlyphAtlasRgba8(
    const SdfGlyphAtlas& atlas, std::span<std::uint8_t> rgba) noexcept;
```

Constants: `kStringTableMaxStrings` / `kStringTableMaxCodePoints`
(both `2^24`), `kStringTableDefaultStrings` (4096),
`kStringTableDefaultCodePoints` (16384), `kTextMaxScale` (16),
`kTextMaxLineWidthPx` (`2^20` px — a 20-bit screen dimension),
`kTextSpaceCode` (U+0020, the wrap separator).

## The string table (PRD §10.4)

`StringTable` stores the game's text as **u32 code points** (one per
code point — the `glyph(code)` domain, M2-TEXT-01; UTF-8/UTF-16
sources convert to this once, at string load, never per frame).
`create` makes exactly two allocations (a header table + a code-point
pool, sized by the budgets); `intern` is **idempotent** — the same
bytes return the first interned string's handle (the identity
property the layout pass relies on: shared strings share one pool
range). The pools are bounded: a string count over `maxStrings` or a
pool over `maxCodePoints` is `BudgetExhausted` with one rate-limited
Warn `string_table/table_full` (fields `limit` = `"strings"` /
`"code_points"`, `capacity`). A stopped table (default-constructed,
moved-from) rejects `intern` with `InvalidArgument`, no log, and
`text` reads empty — the handle is invalid.

The text is a **UNICODE code-point sequence**; the font's range
(default Latin-1, M2-TEXT-01) decides what renders — out-of-range
codes fall back to the space glyph (M2-TEXT-01's documented
contract), never to a re-rasterization.

## The model (documented units)

All layout values are **SCREEN PIXELS, screen space, y DOWN** (the UI
pass — the M2-UI-02 identity screen matrix
([`ui_render.md`](ui_render.md)) maps them to NDC; nothing
world-space leaks in, RENDER-006). `TextItem.scale` is an **INTEGER**
scale factor in [1, `kTextMaxScale`] (the M2-TEXT-01 "integer scale"
convention): every metric is multiplied by the scale — exact, no
fractional-pixel rasterization — so every layout coordinate is an
exact integer before the one presentation-only float conversion to
the `SpriteItem` fields (ARCH-009).

- `y` is always the **FIRST LINE'S BASELINE** (screen px, y down).
- `x` is, by `alignment`, the text block's **LEFT edge** (`Left`),
  **CENTER x** (`Center`), or **RIGHT edge** (`Right`). Each line is
  aligned independently against the anchor (the standard paragraph
  model — every line's own width is centered / right-justified, not
  the longest line's).
- **Line step:** `lineHeight() * scale` px (M2-TEXT-01's line height —
  `lineHeight() == fontSize()` by the stbtt pixel-height convention).

## The wrap model (exact, hand-testable)

With `maxWidth == 0` the text is one line (no wrap). Otherwise the
layout walks the codes greedily:

- **Line width = ink-block width**: the sum of the glyphs' scaled
  advances, plus ONE collapsed space advance per inter-word gap
  (consecutive spaces count once). Leading and trailing spaces carry
  no width.
- **Word wrap:** a following word is appended when
  `lineW + space + wordW ≤ maxWidth`; otherwise it breaks to the next
  line.
- **Character split:** a single word WIDER than `maxWidth` is split
  greedily into the longest prefixes that fit. A single character
  wider than `maxWidth` takes its own line (the line is wider than
  `maxWidth` — the documented oversized behavior; nothing is ever
  dropped or clipped).
- The walk is a TOTAL function on a live font: empty and
  all-whitespace texts lay out zero lines (declare nothing).

## The glyph quads (the M2-SPRITE-02 quad model)

One `SpriteItem` per **glyph with ink** (`width > 0 && height > 0`
after the M2-TEXT-01 fallback — a space declares no quad):

- `pos` = the ink rect's center: left = lineX0 + pen + `bearingX·scale`,
  top = baseline − `bearingY·scale`, size = `width·scale × height·scale`
  (the batcher's centered-quad model: corner (−0.5,−0.5) → uv u0/v0).
- `uv` = the glyph's atlas cell over the atlas:
  (`atlasX/W, atlasY/H, (atlasX+w)/W, (atlasY+h)/H`) — the M2-TEXT-01
  oracle rect. The atlas's row 0 is its TOP (the shelf packing), and
  the quad's top corner maps to v0, so the glyph draws upright with no
  flip (the M2-SPRITE-02 v-axis convention).
- `scale` = the ink size; `rotation` = 0; `frameIndex` = 0; `tint` =
  the item's color (the white-on-alpha upload makes the drawn color
  `color × glyph alpha`); `atlasId` / `materialId` / `blend` are the
  item's fields (the group key, FR-2.1 — one draw call per group).
- `depthOverride = true` on every text quad: the UI pass is
  screen-space, so the hand-tuned `depthKey` IS the documented order
  value (the G-R11 escape hatch — counted + warned at build, the
  sprite_batcher.h contract).

**The SDF glyph path (M2-TEXT-03):** the same quad model over the
glyph's **padded cell** instead of the ink rect — the ink rect grown
by the atlas's distance-field radius `r` on all four sides (the SDF
field needs the surrounding space to carry the outside distances; the
fragment's smoothstep is what produces the sharp screen-space edge).
Per glyph: the quad's size is `(w + 2r)·scale × (h + 2r)·scale`, its
left is `lineX0 + pen + (bearingX − r)·scale`, its top is
`baseline − (bearingY + r)·scale`, and its uv is the padded cell's
rect `(atlasX/W, atlasY/H, (atlasX + w + 2r)/W, (atlasY + h + 2r)/H)`
(the `GlyphMetrics.atlasX/atlasY` are already the padded cell's
origin on the SDF path — font_sdf.md). The **layout is unchanged**:
the pen still advances by the ink's `advance`·scale, so the wrap, the
line layout, and `measureText` return the SAME width on both paths —
the documented width-match contract is **tolerance 0** (both paths
read the identical M2-TEXT-01 metrics), not "within a tolerance".
The SDF atlas's texture must be bound through
`SpriteRenderer::bindSdfAtlas` (the per-atlas shader mode travels
with the texture bind — sprite_renderer.md), and the item's
`atlasId` must name it.

## The frame protocol (the batcher's)

`declareText` appends the item's quads to the batcher's open frame
window (the `beginFrame`/`add`/`build` protocol — M2-SPRITE-01). The
item's `mode` selects the font up front (Bitmap reads `font`, Sdf
reads `sdfFont`; a null or stopped one is `InvalidArgument`, no log —
the failure section): the walk that follows is one shared code path
for both (the metrics are the M2-TEXT-01 metrics on either atlas).
Before the first quad is appended, a **pre-check walk** lays out every
line: any line wider than `kTextMaxLineWidthPx` (the screen domain
guard) fails the call `InvalidArgument` with **nothing declared**
(the window is untouched). After that, the emit walk re-walks the same
lines and appends one quad per ink glyph; a batcher failure (stopped
→ `BudgetExhausted`, closed window → `InvalidArgument`, overflow →
the batcher's drop-oldest + Warn, PERF-008) ends the call — already
declared quads stay in the frame (the `declareParticles` precedent).

`measureText(font, table, ref, scale)` is the no-wrap width query
(O(glyphs), zero alloc): the ink-block width of one line at the scale.
It is the layout half's cheap probe (UI hit-testing, menu sizing).
The SDF overload takes the same width (the width match, tolerance 0).

## The white-on-alpha upload (M2-SPRITE-02)

`expandGlyphAtlasRgba8(atlas, rgba)` fills a caller-owned `W·H·4` byte
buffer with `(255, 255, 255, alpha)` for every texel — the glyph atlas
as a **white-on-alpha RGBA8** texture. Call it once per font per
scene (the setup path), then `SpriteRenderer::bindAtlas(id, W, H,
rgba)` (M2-SPRITE-02). The shader's `texture(uAtlas, uv) * tint` then
draws the glyph at `color × alpha` (the alpha blend, the text
convention). Wrong-size span or a stopped atlas → `InvalidArgument`,
nothing written.

`expandSdfGlyphAtlasRgba8(sdfAtlas, rgba)` (M2-TEXT-03) does the same
for the SDF field: every texel becomes `(255, 255, 255, field)`,
uploaded through `SpriteRenderer::bindSdfAtlas(id, W, H, rgba,
threshold)`. The shader's SDF branch then thresholds the field's
alpha channel (the screen-space derivative AA — sprite_renderer.md),
and the tint colors the drawn glyph exactly as the bitmap path does.

## Ownership, threading (CORE-009, CONC-001)

- `StringTable` owns its two pools (move-only; the moved-from table is
  stopped — `valid()` false). The scene set-up thread creates +
  interns; the render phase reads it (`text`, `isValid` — read-only,
  no mutation after set-up).
- `TextItem.font` (Bitmap) and `TextItem.sdfFont` (Sdf) are
  **non-owning pointers** to the scene's glyph atlas (M2-TEXT-01 /
  M2-TEXT-03) — the mode's atlas must outlive the frame.
- `declareText` mutates only the batcher (the frame window) and reads
  the table + atlas. One owner thread per batcher, as always
  (M2-SPRITE-01).
- Presentation-only (ARCH-009): text is never sim state, never in the
  sim hash or replay. No GL calls anywhere in this API (the GL is the
  M2-SPRITE-02 submit stage).

## Performance (DOC-004)

- `StringTable::create` (set-up, once per scene): 2 allocations
  (`maxStrings · 8 B` headers + `maxCodePoints · 4 B` pool — 64 KB at
  the defaults). `intern` (set-up, per unique string): O(count · len)
  memcmp dedup scan + O(len) copy — the setup path, never per frame.
  Zero alloc after the pool is full (the bounded-failure path).
- `measureText` / `declareText` (render declare path, per text item
  per frame): **O(glyphs)**, zero allocation, zero logging, zero GL —
  integer sums over the string's codes + one quad append per ink
  glyph. The zero-alloc test: 1 000 frames of a wrapped 2x-scale
  declare = 0 heap blocks.
- No `budgets.json` entry: the per-frame text cost is part of the
  composite 50k render-CPU budget, measured with M2-PERF-01.

**Common trap:** interning or measuring per frame. `intern` and the
dedup scan are set-up work (PRD §10.4) — the frame path is the handle
plus the O(glyphs) walk. A per-frame `std::string` in a UI item is
exactly the allocation this module exists to remove.

## Determinism (ARCH-010)

The layout is a pure function of (string, font metrics, anchor,
scale, alignment, maxWidth) — exact integer arithmetic (the scaled
advances are integers; the only float is the final presentation
conversion). Same string + same atlas → same quads, on every
platform. The wrap model, the alignment offsets, and the quad fields
are all hand-computed goldens in the unit suite.

## Example (performant path)

```cpp
// Scene set-up (once): intern the UI strings, build the atlas, upload
// it (M2-SPRITE-02), and keep the StringTable for the frame pipeline.
laige::render::StringTable strings =
    laige::render::StringTable::create({});  // the defaults
std::vector<std::uint32_t> hello{'H', 'e', 'l', 'l', 'o'};
auto r = strings.intern(hello);  // the handle, reused every frame
auto atlas = laige::render::GlyphAtlas::create(fontBytes,
                                               laige::render::GlyphAtlas::Options{});
std::vector<std::uint8_t> rgba;
rgba.resize(1024u * 1024u * 4u);
laige::render::expandGlyphAtlasRgba8(atlas, rgba);
renderer.bindAtlas(/*id*/, 1024, 1024, rgba);

// Per frame (the UI declaration pass): one TextItem per visible block.
laige::render::TextItem item;
item.text = r.value();
item.font = &atlas;
item.x = 8.0f;  item.y = 24.0f;      // the first line's baseline
item.scale = 1;
item.color = {1.0f, 1.0f, 1.0f, 1.0f};
item.maxWidth = 220;                  // wrap at 220 px
item.atlasId = /*the scene's atlas id*/;
laige::render::declareText(batcher, strings, item);  // then build()

// The SDF path (M2-TEXT-03): the same item, mode Sdf, the SDF atlas
// (bound through bindSdfAtlas), the same width.
auto sdfAtlas = laige::render::SdfGlyphAtlas::create(fontBytes,
                                                     laige::render::SdfGlyphAtlas::Options{});
std::vector<std::uint8_t> sdfRgba;
sdfRgba.resize(1024u * 1024u * 4u);
laige::render::expandSdfGlyphAtlasRgba8(sdfAtlas, sdfRgba);
renderer.bindSdfAtlas(/*id2*/, 1024, 1024, sdfRgba, 0.5f);  // the contour
item.mode = laige::render::FontRenderMode::Sdf;
item.sdfFont = &sdfAtlas;
item.atlasId = /*id2*/;
laige::render::declareText(batcher, strings, item);  // same width
```

## Misuse warnings

- Do not intern per frame (set-up path — PRD §10.4).
- Do not free the font atlas before the frame's `submit` (the
  `TextItem.font` span is non-owning).
- Do not expect `maxWidth` to clip: an oversized single character
  takes its own wider line (the documented behavior).
- Do not reuse a `StringRef` from a different table (handles are
  table-local; the invalid-handle read is empty, by contract).
- Do not set `scale` outside [1, 16] or `maxWidth` outside 0 /
  [1, `kTextMaxLineWidthPx`] (InvalidArgument, nothing declared).
- Do not leave an SDF-mode item's `sdfFont` null (or its Bitmap
  `font` null): the mode selects the font, a null or stopped one is
  InvalidArgument, nothing declared.
- Do not bind the SDF field with `bindAtlas` (the per-atlas shader
  mode travels with the texture bind — the field must go through
  `bindSdfAtlas`, and the item's `atlasId` must name that bind).

## Related

- [`font.md`](font.md) — the bitmap glyph atlas (M2-TEXT-01).
- [`font_sdf.md`](font_sdf.md) — the SDF glyph atlas (M2-TEXT-03):
  the field model, the padded cells, the threshold edge behavior,
  the width match.
- [`sprite_batcher.md`](sprite_batcher.md) — the batcher the text
  quads are declared into.
- [`sprite_renderer.md`](sprite_renderer.md) — the submit stage the
  quads render through (the white-on-alpha blend, M2-SPRITE-02).
- [`particle_render.md`](particle_render.md) — the sibling
  render-phase declaration pass (the same batcher pattern).
- [`ui_render.md`](ui_render.md) — the UI render pass (M2-UI-02)
  that declares the Text widgets' items through this path and the
  identity screen matrix the quads draw with.
- `docs/concepts/coordinates.md` §4.12 + §5 — the text coordinate
  row (screen space, y down).
