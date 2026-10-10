// laige-render SDF glyph atlas (M2-TEXT-03): the SDF half of FR-2.8 —
// "Text rendering ... SDF (P1)".
//
//   FR-2.8: "Text rendering: ... bitmap font atlas (P0), SDF (P1), ..."
// This step ships the P1 SDF path on top of the P0 bitmap path
// (font.h, M2-TEXT-01): the caller's TTF/OTF font bytes + a configured
// glyph range + a distance-field radius are converted ONCE, at scene
// set-up, into a fixed-size 8-bit SIGNED DISTANCE FIELD glyph atlas +
// per-glyph metrics (the SAME metrics as the bitmap path — advance,
// bearing, line height). There is NO runtime re-rasterization: after
// `create`, `glyph(code)` is a pure table lookup. The text-item /
// layout pass (text_items.h, M2-TEXT-02) selects the SDF path through
// `TextItem.mode` (the documented scene-config key); the SDF fragment
// shader mode lives in the M2-SPRITE-02 `SpriteRenderer` (the
// `bindSdfAtlas` bind + the shader's SDF branch).
//
//   SdfGlyphAtlas         The configured SDF glyph atlas (create + lookup)
//   kSdf*                 The named constants (CORE-005)
//
// ---------------------------------------------------------------------------
// Why SDF (the P1 motivation, documented)
// ---------------------------------------------------------------------------
//
// The bitmap path (M2-TEXT-01) rasterizes each glyph once at the
// configured pixel size and scales it as a texture: at integer scales
// the glyph stays aligned but its edges are the 1× rasterization's
// (a 16 px "a" at 4× is a 16 px edge stretched 4×). The SDF path
// stores, per texel, the SIGNED DISTANCE to the glyph's 0.5-coverage
// contour in PIXELS AT 1x. The fragment shader (the `SpriteRenderer`'
// SDF branch) thresholds the field with a smoothstep whose anti-aliasing
// band tracks the field's SCREEN-SPACE DERIVATIVE (GLSL
// `dFdx`/`dFdy` — the standard SDF anti-aliasing, the "screen-space
// derivative" rule): the AA band is exactly ONE SCREEN PIXEL WIDE AT ANY
// ZOOM, so text stays crisp at any zoom (the P1 motivation of FR-2.8)
// and the rendered contour matches the 0.5-coverage contour of the 1×
// rasterization, not a stretched copy of it.
//
// ---------------------------------------------------------------------------
// The model (documented units)
// ---------------------------------------------------------------------------
//
// All metric values are PIXELS AT 1x — exactly the M2-TEXT-01 convention
// (the atlas's configured `fontSize`, the stbtt pixel-height
// convention). The SDF metric is the M2-TEXT-01 `GlyphMetrics`
// UNCHANGED (advance, bearingX, bearingY, width, height are the ink's
// metrics); the SDF-specific values are:
//
//   - `radius()` — the distance-field EXTENT in px at 1x (the
//     `Options::sdfRadius`): distances are stored exactly for at most
//     this many px on each side of the contour, and SATURATE beyond
//     it (the atlas cell's corners hold the saturated value — the
//     documented saturation);
//   - the padded cell: each ink glyph's cell is its ink box GROWN by
//     the radius on all four sides (a `w × h` ink occupies a
//     `(w + 2r) × (h + 2r)` cell — the distance field needs the
//     surrounding space to carry the outside distances);
//   - `GlyphMetrics.atlasX/atlasY` — the PADDED CELL's top-left texel
//     (NOT the ink's — the ink sits at `(atlasX + r, atlasY + r)`).
//
// `lineHeight()` is the line height in px at 1x: `fontSize()` (the
// stbtt pixel-height convention — the M2-TEXT-01 formula).
//
// ---------------------------------------------------------------------------
// `SdfGlyphAtlas::create(font, options)`
// ---------------------------------------------------------------------------
//
//   1. Validates `options` (the documented order below, first failure
//      wins — all `InvalidArgument`, no log);
//   2. Validates the FONT BYTES at the untrusted-input boundary
//      (SCALE-004): a non-font / truncated / corrupt file →
//      `MalformedInput` + one structured Error `font/font_invalid`
//      (the M2-TEXT-01 untrusted-font contract, verbatim);
//   3. Rasterizes every codepoint in `[glyphFirst, glyphLast]` into the
//      fixed `atlasWidth × atlasHeight` 8-bit SDF atlas with the
//      deterministic shelf packing below (a glyph whose PADDED cell
//      does not fit → `BudgetExhausted` + one structured Error
//      `font/atlas_full` — the atlas is fixed-size by contract; grow
//      the atlas, not the font);
//   4. On success: one Info `font/sdf_atlas_created` (the set-up path
//      is low-volume; the lookup path logs nothing).
//
// The font bytes are a NON-OWNING span (the M2-TEXT-01 contract: the
// caller owns them; they are read only during `create`).
//
// ---------------------------------------------------------------------------
// The atlas layout (deterministic shelf packing, padded cells)
// ---------------------------------------------------------------------------
//
// The atlas is `atlasWidth × atlasHeight` texels of 8-bit SDF values
// (128 = the 0.5-coverage contour — the header's field section).
// Codepoints are laid out in ASCENDING order, one shelf row at a time:
// the cursor starts at (0, 0); a glyph's PADDED cell is placed at the
// cursor; the cursor advances right by the padded cell's width; when
// the cell would not fit on the row it wraps to the next row (the
// cursor's y advances by the row's max padded height). A cell is the
// padded box `(ink + 2·radius) × (ink + 2·radius)` (no inter-cell
// margin — the field saturates at the cell's corners, so no glyph can
// bleed into a neighbor; the M2-SPRITE-02 `GL_NEAREST` "texel =
// floor(u·W)" contract applies). Empty glyphs (zero-width/height ink —
// e.g. the space) occupy NO cell space. The packing is a pure function
// of (font bytes, options): same input → bit-identical atlas bytes
// (the determinism test).
//
// The glyph's UV rect for the `SpriteRenderer` upload is the PADDED
// CELL's rect: `(atlasX/W, atlasY/H, (atlasX + w + 2r)/W,
// (atlasY + h + 2r)/H)` (the M2-SPRITE-03 v-axis convention: v = 0 =
// first uploaded texel row) — the text pass (text_items.h) declares
// the glyph quad over the padded cell, so the field's outside region
// is what the fragment's smoothstep sees at the glyph's screen edges.
//
// ---------------------------------------------------------------------------
// The field (byte value, units, threshold edge behavior)
// ---------------------------------------------------------------------------
//
// Each texel stores an 8-bit value mapping a SIGNED DISTANCE d (px at
// 1x, the M2-TEXT-01 0.5-coverage contour) to a byte:
//
//   byte = clamp(128 + round(127 · d / r), 0, 255)
//
// with d POSITIVE INSIDE the glyph, NEGATIVE OUTSIDE (the Felice &
// Nienhuis sign convention, the paper's §3):
//
//   - 128 = the exact contour (d = 0);
//   - inside the glyph: byte ≥ 128, monotonically rising with distance
//     from the contour;
//   - outside the glyph: byte ≤ 128, monotonically falling;
//   - saturation: the field is EXACT for |d| ≤ r (byte 255/1 at exactly
//     |d| = r — the documented distance accuracy) and clamps to the
//     0/255 extremes beyond it.
//
// Distances are measured CENTER-TO-CENTER in pixels (a pixel is the
// unit cell at its center, the 8SDD convention): the 128 contour
// therefore crosses between texels, one texel deep in from the 0.5-
// coverage pixels of the 1× rasterization (the standard SDF-from-
// raster behavior — the rendered glyph is the same glyph as the
// bitmap path's, anti-aliased by the field instead of by the 1×
// raster).
//
// THE THRESHOLD EDGE BEHAVIOR (documented, the fragment shader's
// contract): the `SpriteRenderer`'s SDF branch renders a texel with
//
//   alpha = smoothstep(t − aa, t + aa, byte / 255)
//
// where `t` is the atlas's threshold (the `bindSdfAtlas` argument,
// default 0.5 = the 0.5-coverage contour = byte 128) and `aa` is the
// screen-space derivative band (the shader's `dFdx`/`dFdy` of the
// field, floored at one quantization step, 1/255). So: a texel within
// the band (at 1×, aa ≈ 1/(2r), so the band spans ±127/r bytes around
// 128 — about one distance-pixel either side of the contour) renders
// anti-aliased (alpha strictly between 0 and 1); a texel more than
// one distance-px INSIDE the glyph (byte ≥ 128 + 127/r) renders exactly
// opaque; a texel more than one distance-px OUTSIDE (byte ≤ 128 −
// 127/r) renders exactly transparent. Raising `t` renders the glyph
// thinner (the contour retreats into the ink), lowering it renders it
// fatter — the documented threshold knob (the field carries up to r px
// on each side of 128, so the knob is usable while the contour stays
// inside the padded cell).
//
// ---------------------------------------------------------------------------
// The distance-field algorithm (CORE-014)
// ---------------------------------------------------------------------------
//
// The field is the 8-neighborhood signed distance transform of the
// ink bitmap's 0.5-coverage pixels: the Felice & Nienhuis 8SDD
// algorithm — L. Felice and M. Nienhuis, "Simple and Efficient
// Cartesian Signed Distance Transform", IEEE TPAMI 36(3), 2014,
// §III-B (the two-pass, 8-neighborhood, Euclidean-distance dynamic
// program). Per glyph cell: one pass computes the Euclidean distance
// from every pixel to the nearest INSIDE pixel, one pass to the
// nearest OUTSIDE pixel (the two passes of the paper's algorithm —
// forward raster scan then reverse, 8 neighbors each, orthogonal
// neighbors cost 1, diagonal √2 — exact for the 8-neighborhood graph
// in 2D, O(8·W·H) per pass, no auxiliary structures beyond the cell's
// two float grids). The signed field is then `+dOut` on inside
// pixels, `−dIn` on outside pixels, quantized by the formula above.
// Determinism: a fixed sequence of float min/add/round operations —
// bit-identical for the same input on every build (the M2-TEXT-01
// determinism scope, presentation-only float, ARCH-009).
//
// ---------------------------------------------------------------------------
// Missing glyphs (the documented fallback — the M2-TEXT-01 contract)
// ---------------------------------------------------------------------------
//
// `glyph(code)` is a TOTAL function — it never crashes and, for a live
// atlas, never returns null:
//
// - `code` in `[glyphFirst, glyphLast]` → that codepoint's slot. If
//   the font has no glyph for the codepoint, the slot holds the font's
//   `.notdef` glyph (stbtt maps unknown codepoints to glyph 0) —
//   deterministic per font;
// - `code` OUTSIDE the configured range → the FALLBACK glyph: the
//   U+0020 (space) slot if `kFontFallbackCode` lies in the configured
//   range (the default Latin-1 range does), else
//   `kFontEmptyGlyphMetrics` (a zero metric — the advance applies,
//   nothing is drawn).
//
// ---------------------------------------------------------------------------
// Ownership, threading (CORE-009, CONC-001)
// ---------------------------------------------------------------------------
//
// `SdfGlyphAtlas` owns its atlas bytes and slot table (move-only, the
// `GlyphAtlas` precedent); one owner (the scene set-up thread); the
// render phase reads it (the text pass declares the quads, the
// `SpriteRenderer`'s `bindSdfAtlas` uploads the field).
// Presentation-only (ARCH-009): the atlas is never part of the sim
// state hash or replay state. No GL calls anywhere in this header.
//
// Canonical narrative: docs/api/font_sdf.md (the full contract).
// The text feature's place in the 2.5D model:
// docs/concepts/coordinates.md (§4.12 — the text row, shipped).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "laige/errors.h"
#include "laige/render/font.h"
#include "laige/result.h"

namespace laige::render {

// Named constants (CORE-005).
//
// The distance-field radius domain: [1, kSdfMaxRadius] px at 1x. The
// radius is the field's ACCURATE extent (the header's field section)
// and the cell padding: 16 px covers the bitmap path's integer-scale
// domain (kTextMaxScale = 16: at 16× zoom one screen px is 1/16 of an
// atlas px, far inside the 16 px field) — and a 16 px pad on a
// 16 px-font ink cell (≤ ~24 px) keeps the padded cell ≤ ~56 px,
// comfortably inside the 4096² atlas ceiling.
inline constexpr std::uint32_t kSdfDefaultRadius = 8;
inline constexpr std::uint32_t kSdfMaxRadius = 16;
// The 0.5-coverage cut of the 1× rasterization (the header's field
// section: a texel with coverage > 127 is INSIDE, ≤ 127 is OUTSIDE —
// the standard alpha threshold; the 128 byte value is the exact
// contour).
inline constexpr std::uint8_t kSdfInsideCut = 127;

// The configured SDF glyph atlas (M2-TEXT-03): the signed
// distance-field half of FR-2.8, built ONCE at scene set-up from the
// same font bytes + glyph range as the M2-TEXT-01 bitmap atlas.
class SdfGlyphAtlas {
 public:
  // The create options (API-006): the M2-TEXT-01 options + the
  // distance-field radius (the header's model section).
  struct Options {
    std::uint32_t atlasWidth{kFontDefaultAtlasWidth};
    std::uint32_t atlasHeight{kFontDefaultAtlasHeight};
    std::uint32_t fontSize{kFontDefaultFontSize};
    std::uint32_t glyphFirst{kFontDefaultGlyphFirst};
    std::uint32_t glyphLast{kFontDefaultGlyphLast};
    std::uint32_t sdfRadius{kSdfDefaultRadius};
  };

  // Converts the configured glyph range of `font` into the fixed-size
  // 8-bit SDF atlas (set-up path — the only allocations: the atlas
  // bytes + the slot table + the per-glyph distance grids, freed on
  // return). Validation order, first failure wins:
  //   1. atlasWidth/atlasHeight in [1, kFontMaxAtlasDimension],
  //      fontSize in [1, kFontMaxFontSize],
  //      glyphFirst ≤ glyphLast ≤ kFontMaxGlyphCode,
  //      (glyphLast − glyphFirst + 1) ≤ kFontMaxGlyphCount,
  //      sdfRadius in [1, kSdfMaxRadius]
  //      (InvalidArgument, no log — the M2-TEXT-01 create precedent);
  //   2. the font bytes: a non-font / truncated / corrupt file →
  //      MalformedInput + one Error `font/font_invalid` (the
  //      M2-TEXT-01 untrusted-font contract);
  //   3. a glyph whose PADDED cell does not fit the fixed atlas →
  //      BudgetExhausted + one Error `font/atlas_full` (fields: the
  //      codepoint, its padded cell size, the atlas size).
  // A failed create returns the error; no atlas is produced.
  [[nodiscard]] static laige::Result<SdfGlyphAtlas, laige::ErrorCode>
  create(std::span<const std::uint8_t> font, Options options) noexcept;

  // The stopped state (default / moved-from): `valid()` false,
  // `glyph()` returns null, `atlas()` empty, introspection zero — the
  // `GlyphAtlas` stopped-state precedent. Nothing owned.
  SdfGlyphAtlas() noexcept = default;

  SdfGlyphAtlas(const SdfGlyphAtlas&) = delete;
  SdfGlyphAtlas& operator=(const SdfGlyphAtlas&) = delete;
  SdfGlyphAtlas(SdfGlyphAtlas&&) noexcept = default;
  SdfGlyphAtlas& operator=(SdfGlyphAtlas&&) noexcept = default;
  ~SdfGlyphAtlas() noexcept = default;

  // True iff the atlas is live (create succeeded).
  [[nodiscard]] bool valid() const noexcept { return !atlas_.empty(); }

  // The atlas dimensions (texels; zero in the stopped state).
  [[nodiscard]] std::uint32_t atlasWidth() const noexcept { return atlasWidth_; }
  [[nodiscard]] std::uint32_t atlasHeight() const noexcept {
    return atlasHeight_;
  }

  // The configured pixel height, px at 1x.
  [[nodiscard]] std::uint32_t fontSize() const noexcept { return fontSize_; }

  // The line height, px at 1x: `fontSize()` (the stbtt pixel-height
  // convention — the M2-TEXT-01 formula).
  [[nodiscard]] std::uint32_t lineHeight() const noexcept { return fontSize_; }

  // The configured glyph range (inclusive).
  [[nodiscard]] std::uint32_t glyphFirst() const noexcept { return glyphFirst_; }
  [[nodiscard]] std::uint32_t glyphLast() const noexcept { return glyphLast_; }

  // The distance-field radius, px at 1x (the header's model section;
  // 0 in the stopped state).
  [[nodiscard]] std::uint32_t radius() const noexcept { return radius_; }

  // The atlas bytes: 8-bit SDF values, row-major,
  // `atlasWidth() × atlasHeight()` texels (128 = the 0.5-coverage
  // contour — the header's field section; empty in the stopped state).
  // The render read path: the text pass (text_items.h) uploads this as
  // a white-on-alpha RGBA8 atlas (the M2-SPRITE-02 `bindSdfAtlas`
  // contract). O(1); never writes.
  [[nodiscard]] std::span<const std::uint8_t> atlas() const noexcept {
    return atlas_;
  }

  // The metrics for `code` (the header's fallback section: a total
  // function — null only in the stopped state; the in-range slot, else
  // the documented fallback). O(1); no allocation, no logging, no GL.
  // The M2-TEXT-01 `GlyphMetrics` UNCHANGED (advance, bearingX,
  // bearingY, width, height = the ink's metrics; `atlasX`/`atlasY` =
  // the PADDED CELL's origin — the header's model section). The
  // returned pointer is valid until this atlas is moved/destroyed
  // (the slot table is member storage, never reallocated after
  // create).
  [[nodiscard]] const GlyphMetrics* glyph(std::uint32_t code) const noexcept;

 private:
  std::vector<std::uint8_t> atlas_;  // atlasWidth_ × atlasHeight_
  std::vector<GlyphMetrics> slots_;  // one per codepoint in [glyphFirst_, glyphLast_]
  std::uint32_t atlasWidth_{0};
  std::uint32_t atlasHeight_{0};
  std::uint32_t fontSize_{0};
  std::uint32_t glyphFirst_{0};
  std::uint32_t glyphLast_{0};
  std::uint32_t radius_{0};
};

}  // namespace laige::render
