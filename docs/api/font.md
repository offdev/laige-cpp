# Bitmap font atlas (`laige::render::GlyphAtlas`)

The bitmap half of FR-2.8 — "Text rendering: ... bitmap (P0)"
(M2-TEXT-01): the caller's TTF/OTF font bytes + a configured glyph range
are rasterized **once, at scene set-up**, into a fixed-size 8-bit alpha
glyph atlas + per-glyph metrics (advance, bearing, line height). There
is **no runtime re-rasterization**: after `create`, `glyph(code)` is a
pure O(1) table lookup. The text widget / layout pass lands in
M2-TEXT-02; the SDF path (P1) lands in M2-TEXT-03 on top of the same
vendored rasterizer (ADR 0009).

Public header:
`src/laige-render/include/laige/render/font.h`
(implementation: `src/laige-render/font.cpp`; the vendor boundary is
the `laige-stb` target — ADR 0009). Unit suite: `ctest -R font`
(`tests/laige-render/font_tests.cpp`) — pure engine data, no GL
environment required: it runs in every local tree and in CI, with the
validation matrix, the untrusted-font boundary, the capacity guard,
the metric goldens (measured against the committed test font — see
`tests/laige-render/assets/README.md`), the deterministic-atlas
contract, the missing-glyph fallback, and the zero-allocation lookup
window.

## The API

```cpp
// The per-glyph metrics + atlas cell origin (px at 1x).
struct GlyphMetrics {
  std::int32_t advance{0};    // pen advance, px at 1x
  std::int32_t bearingX{0};   // bitmap left edge from the pen (right +)
  std::int32_t bearingY{0};   // bitmap top edge above the baseline (up +)
  std::int32_t width{0};      // bitmap ink width, texels (0 = empty)
  std::int32_t height{0};     // bitmap ink height, texels (0 = empty)
  std::uint32_t atlasX{0};    // cell top-left, texels in the atlas
  std::uint32_t atlasY{0};
};

// The configured bitmap glyph atlas (move-only).
class GlyphAtlas {
 public:
  struct Options {
    std::uint32_t atlasWidth{kFontDefaultAtlasWidth};    // 1024
    std::uint32_t atlasHeight{kFontDefaultAtlasHeight};  // 1024
    std::uint32_t fontSize{kFontDefaultFontSize};        // 16 px at 1x
    std::uint32_t glyphFirst{kFontDefaultGlyphFirst};    // 32 (U+0020)
    std::uint32_t glyphLast{kFontDefaultGlyphLast};      // 255 (Latin-1)
  };

  // Set-up path: validate, parse, rasterize (the only allocations).
  [[nodiscard]] static Result<GlyphAtlas, ErrorCode> create(
      std::span<const std::uint8_t> font, Options options) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint32_t atlasWidth() const noexcept;
  [[nodiscard]] std::uint32_t atlasHeight() const noexcept;
  [[nodiscard]] std::uint32_t fontSize() const noexcept;
  [[nodiscard]] std::uint32_t lineHeight() const noexcept;  // == fontSize()
  [[nodiscard]] std::uint32_t glyphFirst() const noexcept;
  [[nodiscard]] std::uint32_t glyphLast() const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> atlas() const noexcept;
  [[nodiscard]] const GlyphMetrics* glyph(std::uint32_t code) const noexcept;
};
```

Constants (`CORE-005`): `kFontMaxAtlasDimension` (4096),
`kFontMaxFontSize` (1024), `kFontMaxGlyphCode` (0x10FFFF),
`kFontMaxGlyphCount` (8192 — the range-size bound),
`kFontDefaultAtlasWidth/Height` (1024), `kFontDefaultFontSize` (16),
`kFontDefaultGlyphFirst/Last` (32..255, the Latin-1 set),
`kFontFallbackCode` (32 = U+0020 SPACE), `kFontEmptyGlyphMetrics` (the
zero metric).

## Documented units

All metric values are **pixels at 1x** — at the atlas's configured
`fontSize` (the stbtt pixel-height convention: the font's ascent +
descent scaled to exactly that height). To render at an integer scale
s, multiply every metric by s (exact for integer s); the atlas bitmap
is drawn at s× its texel size (the M2-SPRITE-02 sprite `scale`).

- `advance` — the pen advance after the glyph (positive = right; a
  negative value is stored as-is — RTL/quirk fonts are out of scope,
  the field is total).
- `bearingX` — the bitmap's LEFT edge from the pen (right positive;
  negative = the ink overhangs the pen left). Place the cell at
  `penX + bearingX`.
- `bearingY` — the bitmap's TOP edge ABOVE the baseline (up positive).
  The ink bottom is at `bearingY − height` below the baseline (a
  descender when `height > bearingY`).
- `width`/`height` — the ink size in texels (`0 × 0` = the empty
  glyph — nothing is drawn, the advance still applies).
- `lineHeight()` — the line height in px at 1x: **`lineHeight() ==
  fontSize()`** exactly (the documented formula, not a separate
  measurement).

## The atlas layout (deterministic shelf packing)

The atlas is `atlasWidth × atlasHeight` texels of 8-bit alpha.
Codepoints are laid out in **ascending order**, one shelf row at a
time: the cursor starts at (0, 0); a glyph's cell is placed at the
cursor; the cursor advances right by the cell's width; when the cell
would not fit on the row it wraps to the next row (y advances by the
row's max cell height). A cell is **exactly the glyph's ink bitmap**
(no padding — the rasterization is clipped to the cell, so no glyph
can bleed into a neighbor; the M2-SPRITE-02 `GL_NEAREST`
"texel = floor(u·W)" contract applies). Empty glyphs occupy no cell
space. The packing is a pure function of (font bytes, options):
same input → **bit-identical atlas bytes** (the determinism test).

`GlyphMetrics.atlasX/atlasY` is the cell's top-left texel; the glyph's
UV rect for the M2-SPRITE-02 upload is
`(atlasX/W, atlasY/H, (atlasX+width)/W, (atlasY+height)/H)`
(the M2-SPRITE-03 v-axis convention: v = 0 = first uploaded texel row).

The atlas bytes are 8-bit alpha; the M2-TEXT-02 pass uploads them as a
white-on-alpha RGBA8 atlas through the M2-SPRITE-02 `bindAtlas`
path.

## Missing glyphs (the documented fallback)

`glyph(code)` is a **total function** — it never crashes and, for a
live atlas, never returns null:

- `code` in `[glyphFirst, glyphLast]` → that codepoint's slot. A
  codepoint the font does not carry maps to the font's `.notdef`
  glyph (the stbtt unknown-codepoint → glyph 0 contract) —
  deterministic per font.
- `code` OUTSIDE the range → the **fallback glyph**: the U+0020 space
  slot if `kFontFallbackCode` lies in the configured range (the
  default does), else `kFontEmptyGlyphMetrics` (the zero metric).
- The stopped atlas → `nullptr` (check `valid()` first).

The fallback is documented, never silent: a game that wants a visible
"missing character" box configures a glyph range that includes its box
glyph — the engine does not draw extra glyphs.

## Failure behavior (CORE-008)

`create` validates in this order (first failure wins):

1. **Options** (no log — the M2-SPRITE-01 create precedent):
   `atlasWidth`/`atlasHeight` in `[1, 4096]`, `fontSize` in
   `[1, 1024]`, `glyphFirst ≤ glyphLast ≤ 0x10FFFF`,
   `(glyphLast − glyphFirst + 1) ≤ 8192` → `InvalidArgument`.
2. **Font bytes** (the untrusted-input boundary, SCALE-004): the
   header + full table directory must be present (the engine-side
   guard — stb's directory scan is not bounds-checked; the buf-based
   table parsing is), the required tables must parse, and the
   pixel-height scale (px per font unit) must be finite, positive, and
   ≤ 2^15 (the int-domain ceiling — the reachable max is 1024; a font
   with 0 or inverted vertical metrics makes it inf or negative) →
   `MalformedInput` + one
   structured Error `font/font_invalid` (field: `bytes`). A
   truncated, non-font, or degenerate-metric file is an error, never
   a crash and never a silent fallback.
3. **Capacity**: a glyph whose ink cell does not fit the fixed atlas
   (wider/taller than the atlas, or the shelf is full) →
   `BudgetExhausted` + one structured Error `font/atlas_full`
   (fields: `code`, `width`, `height`, `atlas_width`, `atlas_height`).
   The atlas is fixed-size by contract: grow the atlas, not the font.
4. Success → one Info `font/atlas_created` (the set-up path is
   low-volume; the lookup path logs nothing).

A failed create returns the error; no atlas is produced.

## Ownership, threading (CORE-009, CONC-001)

`GlyphAtlas` owns its atlas bytes + slot table (move-only, the
`SpriteBatcher` precedent). One owner (the scene set-up thread); the
render phase reads it (the M2-GL-02 cull/batch stage uploads the atlas
+ declares text quads — M2-TEXT-02). The font bytes are a
**non-owning span**: the caller owns them (the game's asset pipeline —
the M3 asset system); they are read only during `create` and may be
freed immediately after. Presentation-only (ARCH-009): the atlas is
never part of the sim state hash or replay state. No GL calls
anywhere in this API.

## Performance (DOC-004)

- `create` (set-up path, once per scene): O(glyphs × glyph-area)
  rasterization — 224 glyphs × 16 px ≈ a few ms for the default Latin-1
  range; the two allocations are the atlas bytes (1 MiB at the default
  1024²) + the slot table (28 B per codepoint — 6.3 KB for Latin-1).
  The per-glyph `stbtt` bitmap buffer is a transient ≤ 16 777 216
  bytes (atlasWidth × atlasHeight, the capacity-bounded guard — never
  an int overflow, never an unbounded malloc).
- `glyph(code)` (render read path, per text quad per frame): **O(1),
  no allocation, no logging, no GL** — a bounds check + a pointer
  return (the zero-allocation test: 1000 mixed lookups = 0 heap
  blocks). The returned pointer is valid until the atlas is
  moved/destroyed (the slot table is never reallocated after create).
- No `budgets.json` entry: the atlas is built once at set-up, not per
  frame — the per-frame text cost lands with M2-TEXT-02 (part of the
  composite 50k render-CPU budget, M2-PERF-01).

**Common trap:** rasterizing in a per-frame code path. The atlas is a
set-up artifact — `create` per frame re-parses and re-rasterizes the
font (the FR-2.8 "no runtime re-rasterization" contract exists for
this reason).

## Example (performant path)

```cpp
// Scene set-up (once): read the font file (the asset pipeline owns
// the bytes), build the atlas, upload it (M2-TEXT-02), and keep the
// GlyphAtlas for the frame pipeline's declaration stage.
std::vector<std::uint8_t> fontBytes = readAsset("fonts/vera.ttf");
auto atlas = laige::render::GlyphAtlas::create(fontBytes,
                                               laige::render::GlyphAtlas::Options{});
if (atlas.isError()) { /* handle: the font asset is invalid */ }
fontBytes.clear();  // safe: the atlas owns a copy of the bitmap
// Per frame (M2-TEXT-02): one quad per drawn glyph —
// metrics = atlas.glyph(code); quad uv = (atlasX/W, atlasY/H,
// (atlasX+width)/W, (atlasY+height)/H); advance pen by advance * scale.
```

## Misuse warnings

- Do not free the font bytes before `create` returns (the span is
  non-owning, read during creation).
- Do not `create` per frame — build the atlas once at scene set-up.
- Do not assume `glyph(code)` covers a codepoint: the range is
  configured (default Latin-1); outside-range codes fall back to the
  space glyph (the documented contract), they are never rasterized on
  demand.
- Do not reuse the pointer from `glyph()` after the atlas is moved
  (the slot table storage moves with the atlas).

## Related

- [`sprite_batcher.md`](sprite_batcher.md) — the batcher the M2-TEXT-02
  text quads are declared into.
- [`sprite_frames.md`](sprite_frames.md) — the atlas UV conventions
  (the v-axis, the float-exact domain).
- [`particle_render.md`](particle_render.md) — the sibling render-phase
  declaration pass (the pattern M2-TEXT-02 follows).
- ADR 0009 — the stb_truetype vendoring (the dependency this step adds).
