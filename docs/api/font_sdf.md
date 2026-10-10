# SDF glyph atlas (`laige::render::SdfGlyphAtlas`)

The SDF half of FR-2.8 — "Text rendering: ... SDF (P1)"
(M2-TEXT-03): the caller's TTF/OTF font bytes + a configured glyph
range + a distance-field radius are converted **once, at scene
set-up**, into a fixed-size 8-bit **signed distance-field** glyph
atlas + per-glyph metrics (the same metrics as the bitmap path —
advance, bearing, line height). There is **no runtime
re-rasterization**: after `create`, `glyph(code)` is a pure O(1)
table lookup. The text-item / layout pass
([`text_items.md`](text_items.md)) selects the SDF path through
`TextItem.mode` (the documented scene-config key); the SDF fragment
shader mode lives in the M2-SPRITE-02 `SpriteRenderer`
([`sprite_renderer.md`](sprite_renderer.md)) — the `bindSdfAtlas`
bind + the shader's SDF branch.

Public header:
`src/laige-render/include/laige/render/font_sdf.h`
(implementation: `src/laige-render/font_sdf.cpp`; the vendor
boundary is the `laige-stb` target — ADR 0009, the same vendored
rasterizer as the bitmap path). Unit suite: `ctest -R font_sdf`
(`tests/laige-render/font_sdf_tests.cpp`) — pure engine data, no GL
environment required (the offscreen GL smoke GTEST_SKIPs where
absent — the documented environment contract): it runs in every
local tree and in CI, with the validation matrix, the untrusted-
font boundary, the capacity guard, the metrics parity with the
bitmap path, the exact width match, the deterministic-atlas
contract, the threshold edge behavior (against a brute-force
distance oracle), the missing-glyph fallback, the upload bytes,
and the zero-allocation lookup window.

## Why SDF (the P1 motivation, documented)

The bitmap path (M2-TEXT-01) rasterizes each glyph once at the
configured pixel size and scales it as a texture: at integer scales
the glyph stays aligned, but its edges are the 1x rasterization's
(a 16 px "a" at 4x is a 16 px edge stretched 4x). The SDF path
stores, per texel, the **signed distance to the glyph's
0.5-coverage contour in pixels at 1x**. The fragment shader's SDF
branch thresholds the field with a smoothstep whose anti-aliasing
band tracks the field's **screen-space derivative** (GLSL
`dFdx`/`dFdy` — the standard SDF anti-aliasing, the "screen-space
derivative" rule): the AA band is **exactly one screen pixel wide
at any zoom**, so text stays crisp at any zoom (the P1 motivation
of FR-2.8) and the rendered contour matches the 0.5-coverage
contour of the 1x rasterization, not a stretched copy of it.

The trade is set-up cost and texture memory: the field carries the
outside distances in the padded cell around each glyph (below),
and the shader branch is a per-fragment smoothstep (the SDF atlas
is the render-phase's, not a sim-state, cost — ARCH-009).

## The API

```cpp
// The configured SDF glyph atlas (move-only).
class SdfGlyphAtlas {
 public:
  struct Options {
    std::uint32_t atlasWidth{kFontDefaultAtlasWidth};    // 1024
    std::uint32_t atlasHeight{kFontDefaultAtlasHeight};  // 1024
    std::uint32_t fontSize{kFontDefaultFontSize};        // 16 px at 1x
    std::uint32_t glyphFirst{kFontDefaultGlyphFirst};    // 32 (U+0020)
    std::uint32_t glyphLast{kFontDefaultGlyphLast};      // 255 (Latin-1)
    std::uint32_t sdfRadius{kSdfDefaultRadius};          // 8 px at 1x
  };

  // Set-up path: validate, parse, rasterize, distance transform.
  [[nodiscard]] static Result<SdfGlyphAtlas, ErrorCode> create(
      std::span<const std::uint8_t> font, Options options) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint32_t atlasWidth() const noexcept;
  [[nodiscard]] std::uint32_t atlasHeight() const noexcept;
  [[nodiscard]] std::uint32_t fontSize() const noexcept;
  [[nodiscard]] std::uint32_t lineHeight() const noexcept;  // == fontSize()
  [[nodiscard]] std::uint32_t glyphFirst() const noexcept;
  [[nodiscard]] std::uint32_t glyphLast() const noexcept;
  [[nodiscard]] std::uint32_t radius() const noexcept;      // 0 when stopped
  [[nodiscard]] std::span<const std::uint8_t> atlas() const noexcept;
  [[nodiscard]] const GlyphMetrics* glyph(std::uint32_t code) const noexcept;
};
```

Constants (`CORE-005`): `kSdfDefaultRadius` (8 px at 1x),
`kSdfMaxRadius` (16 px at 1x — the radius domain ceiling),
`kSdfInsideCut` (127 — the 0.5-coverage cut of the 1x rasterization:
a texel with alpha > 127 is inside, <= 127 is outside). The
atlas/font constants are the M2-TEXT-01 ones (`kFontMaxAtlasDimension`
4096, `kFontMaxFontSize` 1024, `kFontMaxGlyphCode` 0x10FFFF,
`kFontMaxGlyphCount` 8192, `kFontFallbackCode` 32,
`kFontEmptyGlyphMetrics` — see [`font.md`](font.md)).

## The model (documented units)

All metric values are **pixels at 1x** — exactly the M2-TEXT-01
convention. The SDF metric is the M2-TEXT-01 `GlyphMetrics`
**unchanged** (advance, bearingX, bearingY, width, height are the
ink's metrics), with two SDF-specific meanings:

- `radius()` — the distance-field **extent** in px at 1x (the
  `Options::sdfRadius`): distances are stored exactly for at most
  this many px on each side of the contour, and **saturate** beyond
  it (the atlas cell's corners hold the saturated value — the
  documented saturation). The radius domain is `[1, 16]` px: 16 px
  covers the bitmap path's integer-scale domain (`kTextMaxScale`
  16 — at 16x zoom one screen px is 1/16 of an atlas px, far inside
  the 16 px field), and a 16 px pad on a 16 px-font ink cell keeps
  the padded cell <= ~56 px, comfortably inside the 4096^2 atlas
  ceiling.
- the **padded cell**: each ink glyph's cell is its ink box grown by
  the radius on all four sides (a `w x h` ink occupies a
  `(w + 2r) x (h + 2r)` cell — the distance field needs the
  surrounding space to carry the outside distances);
- `GlyphMetrics.atlasX/atlasY` — the **padded cell's** top-left
  texel (not the ink's: the ink sits at `(atlasX + r, atlasY + r)`).

`lineHeight()` is the line height in px at 1x: **`lineHeight() ==
fontSize()`** exactly (the M2-TEXT-01 formula).

## The atlas layout (deterministic shelf packing, padded cells)

The atlas is `atlasWidth x atlasHeight` texels of 8-bit SDF values
(128 = the 0.5-coverage contour — the field section below).
Codepoints are laid out in **ascending order**, one shelf row at a
time: the cursor starts at (0, 0); a glyph's **padded cell** is
placed at the cursor; the cursor advances right by the padded
cell's width; when the cell would not fit on the row it wraps to
the next row (the cursor's y advances by the row's max padded
height). A cell is the padded box `(ink + 2*r) x (ink + 2*r)` (no
inter-cell margin — the field saturates at the cell's corners, so
no glyph can bleed into a neighbor; the M2-SPRITE-02 `GL_NEAREST`
"texel = floor(u*W)" contract applies). Empty glyphs (zero-
width/height ink — e.g. the space) occupy **no cell space**. The
packing is a pure function of (font bytes, options): same input to
**bit-identical atlas bytes** (the determinism test).

The glyph's UV rect for the `SpriteRenderer` upload is the **padded
cell's** rect: `(atlasX/W, atlasY/H, (atlasX + w + 2r)/W,
(atlasY + h + 2r)/H)` (the M2-SPRITE-03 v-axis convention: v = 0 =
first uploaded texel row). The text pass declares the glyph quad
over the padded cell, so the field's outside region is what the
fragment's smoothstep sees at the glyph's screen edges.

## The field (byte value, units, threshold edge behavior)

Each texel stores an 8-bit value mapping a **signed distance d**
(px at 1x, to the glyph's 0.5-coverage contour) to a byte:

```text
byte = clamp(128 + round(127 * d / r), 0, 255)
```

with d **positive inside** the glyph, **negative outside** (the
Felice & Nienhuis sign convention, the paper's section III-B):

- 128 = the exact contour (d = 0);
- inside the glyph: byte >= 128, monotonically rising with distance
  from the contour;
- outside the glyph: byte <= 128, monotonically falling;
- saturation: the field is **exact for |d| <= r** (byte 255/1 at
  exactly |d| = r — the documented distance accuracy) and clamps to
  the 0/255 extremes beyond it.

Distances are measured **center-to-center in pixels** (a pixel is
the unit cell at its center, the 8SDD convention): the 128
contour therefore crosses between texels, one texel deep in from
the 0.5-coverage pixels of the 1x rasterization (the standard
SDF-from-raster behavior — the rendered glyph is the same glyph as
the bitmap path's, anti-aliased by the field instead of by the 1x
raster).

**The threshold edge behavior** (documented; the fragment
shader's contract). The `SpriteRenderer`'s SDF branch renders a
texel with

```text
alpha = smoothstep(t - aa, t + aa, byte / 255)
```

where `t` is the atlas's threshold (the `bindSdfAtlas` argument,
default 0.5 = the 0.5-coverage contour = byte 128) and `aa` is the
screen-space derivative band (the shader's `dFdx`/`dFdy` of the
field, floored at one quantization step, 1/255). So:

- a texel **within the band** (at 1x, aa ~ 1/(2r), so the band
  spans ~127/r bytes either side of 128 — about one distance-pixel
  either side of the contour) renders anti-aliased (alpha strictly
  between 0 and 1);
- a texel **more than one distance-px inside** the glyph (byte >=
  128 + 127/r) renders **exactly opaque**;
- a texel **more than one distance-px outside** (byte <= 128 -
  127/r) renders **exactly transparent**.

Raising `t` renders the glyph thinner (the contour retreats into
the ink); lowering it renders it fatter. The knob is usable while
the contour stays inside the padded cell: the field carries up to
r px on each side of 128.

**Width match with the bitmap path** (the documented contract,
tolerance **0**): the SDF path's layout reads the identical
M2-TEXT-01 metrics (advance, bearing, ink size), so
`measureText` returns the **same width** for the same text on
both paths — exactly, not within a tolerance. The quads differ
only in size (the padded cell vs the ink rect) and UV rect
(padded cell vs ink cell); the pen walk, the line layout, and the
wrap are unchanged.

## The distance-field algorithm (CORE-014)

The field is the **8-neighborhood signed distance transform** of
the ink bitmap's 0.5-coverage pixels: the **Felice & Nienhuis 8SDD
algorithm** — L. Felice and M. Nienhuis, "Simple and Efficient
Cartesian Signed Distance Transform", IEEE TPAMI 36(3), 2014,
section III-B (the two-pass, 8-neighborhood, Euclidean-distance
dynamic program). Per glyph cell: one pass computes the Euclidean
distance from every pixel to the nearest INSIDE pixel, one pass to
the nearest OUTSIDE pixel (the paper's two passes — forward raster
scan then reverse, 8 neighbors each, orthogonal neighbors cost 1,
diagonal sqrt(2) — exact for the 8-neighborhood graph in 2D,
O(8*W*H) per pass, no auxiliary structures beyond the cell's two
float grids). The signed field is then `+dOut` on inside pixels,
`-dIn` on outside pixels, quantized by the formula above.
Determinism: a fixed sequence of float min/add/round operations —
**bit-identical for the same input on every build** (the
M2-TEXT-01 determinism scope, presentation-only float, ARCH-009).

## Missing glyphs (the documented fallback)

`glyph(code)` is a **total function** — it never crashes and, for
a live atlas, never returns null (the M2-TEXT-01 contract,
verbatim):

- `code` in `[glyphFirst, glyphLast]` to that codepoint's slot. A
  codepoint the font does not carry maps to the font's `.notdef`
  glyph (stbtt maps unknown codepoints to glyph 0) —
  deterministic per font.
- `code` **outside** the range to the **fallback glyph**: the
  U+0020 space slot if `kFontFallbackCode` lies in the configured
  range (the default does), else `kFontEmptyGlyphMetrics` (a zero
  metric — the advance applies, nothing is drawn).
- The stopped atlas to `nullptr` (check `valid()` first).

## Failure behavior (CORE-008)

`create` validates in this order (first failure wins):

1. **Options** (no log — the M2-TEXT-01 create precedent):
   `atlasWidth`/`atlasHeight` in `[1, 4096]`, `fontSize` in
   `[1, 1024]`, `glyphFirst <= glyphLast <= 0x10FFFF`,
   `(glyphLast - glyphFirst + 1) <= 8192`, `sdfRadius` in
   `[1, 16]` to `InvalidArgument`.
2. **Font bytes** (the untrusted-input boundary, SCALE-004): the
   header + full table directory must be present (the engine-side
   guard — stb's directory scan is not bounds-checked; the buf-
   based table parsing is), the required tables must parse, and
   the pixel-height scale must be finite, positive, and <= 2^15
   to `MalformedInput` + one structured Error `font/font_invalid`
   (field: `bytes`) — the M2-TEXT-01 untrusted-font contract,
   verbatim. A truncated, non-font, or degenerate-metric file is
   an error, never a crash and never a silent fallback.
3. **Capacity**: a glyph whose **padded cell** does not fit the
   fixed atlas (wider/taller than the atlas, or the shelf is
   full) to `BudgetExhausted` + one structured Error
   `font/atlas_full` (fields: `code`, `width`, `height`,
   `atlas_width`, `atlas_height`). The atlas is fixed-size by
   contract: grow the atlas, not the font.
4. Success to one Info `font/sdf_atlas_created` (fields:
   `atlas_width`, `atlas_height`, `font_size`, `sdf_radius`,
   `glyphs` — the set-up path is low-volume; the lookup path logs
   nothing).

A failed create returns the error; no atlas is produced.

## The upload (the `bindSdfAtlas` input)

`expandSdfGlyphAtlasRgba8` (text_items.h, M2-TEXT-03) expands the
8-bit field into white-on-alpha RGBA8 bytes — every texel becomes
`(255, 255, 255, field)` — for the M2-SPRITE-02 `bindSdfAtlas`
upload (one call per font per scene load, before the one-time
texture upload — RENDER-004). The shader's SDF branch then
thresholds the field's alpha channel (the field section above);
the multiplicative tint colors the drawn glyph exactly as the
bitmap path's tint does.

## Ownership, threading (CORE-009, CONC-001)

`SdfGlyphAtlas` owns its atlas bytes + slot table (move-only, the
`GlyphAtlas` precedent). One owner (the scene set-up thread); the
render phase reads it (the text pass declares the quads, the
`SpriteRenderer`'s `bindSdfAtlas` uploads the field). The font
bytes are a **non-owning span**: the caller owns them (the game's
asset pipeline); they are read only during `create` and may be
freed immediately after. Presentation-only (ARCH-009): the atlas
is never part of the sim state hash or replay state. No GL calls
anywhere in this header.

## Performance (DOC-004)

- `create` (set-up path, once per scene): O(glyphs x
  glyph-area) rasterization + O(8 * cell-area) per-cell distance
  transform — 224 glyphs at the default Latin-1 range is a few ms
  (the bitmap path's cost plus two float grids per cell, freed on
  return). The two persistent allocations are the atlas bytes
  (1 MiB at the default 1024^2) + the slot table (28 B per
  codepoint — 6.3 KB for Latin-1); the per-glyph bitmap buffer
  (<= 16 777 216 bytes, capacity-bounded) and the two per-cell
  float grids are transient.
- `glyph(code)` (render read path, per text quad per frame):
  **O(1), no allocation, no logging, no GL** — a bounds check + a
  pointer return (the zero-allocation test: 1000 mixed lookups = 0
  heap blocks). The returned pointer is valid until the atlas is
  moved/destroyed (the slot table is never reallocated after
  create).
- The SDF fragment branch (per-fragment smoothstep on the SDF
  quads only — per-atlas state, the `bindSdfAtlas` argument; the
  bitmap quads take the existing branch): the per-frame text cost
  is part of the composite 50k render-CPU budget (M2-PERF-01).
- No `budgets.json` entry: the atlas is built once at set-up, not
  per frame (the budgets.json count stays 17 — the
  `BudgetHarnessTable` pin asserts the repo entry count).

**Common traps:** building the atlas per frame (the FR-2.8 "no
runtime re-rasterization" contract); binding the SDF field through
`bindAtlas` (the bitmap shader branch would draw the field's raw
alpha — the per-atlas mode travels with the texture bind, so the
SDF field goes through `bindSdfAtlas`); using a radius smaller
than the zoom the field must support (a 16 px zoom needs the 16 px
field — `kSdfMaxRadius`).

## Example (performant path)

```cpp
// Scene set-up (once): read the font file (the asset pipeline owns
// the bytes), build the SDF atlas, upload it (expandSdfGlyphAtlasRgba8,
// then bindSdfAtlas — the per-atlas shader mode), and keep the
// SdfGlyphAtlas for the frame pipeline's declaration stage.
std::vector<std::uint8_t> fontBytes = readAsset("fonts/vera.ttf");
laige::render::SdfGlyphAtlas::Options opts;  // the defaults + the radius
auto sdf = laige::render::SdfGlyphAtlas::create(fontBytes, opts);
if (sdf.isError()) { /* handle: the font asset is invalid */ }
fontBytes.clear();  // safe: the atlas owns a copy of the field
// Per frame (M2-TEXT-02/03): a TextItem with mode = Sdf and sdfFont =
// &sdf declares through declareText (text_items.h) — the quads cover
// the padded cells, the pen advances by the unchanged advances, and
// measureText matches the bitmap path exactly.
```

## Misuse warnings

- Do not free the font bytes before `create` returns (the span is
  non-owning, read during creation).
- Do not `create` per frame — build the atlas once at scene set-up.
- Do not bind the SDF field with `bindAtlas`: the per-atlas shader
  mode travels with the texture bind; the field must go through
  `bindSdfAtlas` (the SDF branch thresholds it).
- Do not assume `glyph(code)` covers a codepoint: the range is
  configured (default Latin-1); outside-range codes fall back to
  the space glyph (the documented contract), they are never
  rasterized on demand.
- Do not reuse the pointer from `glyph()` after the atlas is moved
  (the slot table storage moves with the atlas).
- Do not pick a radius below the zoom the field must support: the
  field is exact only within `radius` px of the contour
  (`kSdfMaxRadius` 16 covers the `kTextMaxScale` 16 domain).

## Related

- [`font.md`](font.md) — the bitmap path (M2-TEXT-01) this step
  builds on: the same vendor boundary, the same metrics, the same
  untrusted-font contract.
- [`text_items.md`](text_items.md) — the text-item / layout pass
  (M2-TEXT-02/03) that consumes both atlases: `TextItem.mode`
  selects the path, the width match is exact.
- [`sprite_renderer.md`](sprite_renderer.md) — the M2-SPRITE-02
  submit stage: `bindSdfAtlas` (the per-atlas SDF mode) and the
  fragment shader's SDF branch (the screen-space derivative AA).
- [`sprite_batcher.md`](sprite_batcher.md) — the batcher the text
  quads are declared into.
- ADR 0009 — the stb_truetype vendoring (the dependency both font
  paths share).
