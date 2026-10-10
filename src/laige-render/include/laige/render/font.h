// laige-render bitmap font atlas (M2-TEXT-01): the bitmap half of FR-2.8
// — "Text rendering ... bitmap (P0); SDF (P1)".
//
//   FR-2.8: "Text rendering: ... bitmap font atlas (P0), SDF (P1), ..."
// This step ships the P0 bitmap path: the caller's TTF/OTF font bytes +
// a configured glyph range are rasterized ONCE, at scene set-up, into a
// fixed-size 8-bit alpha glyph atlas + per-glyph metrics (advance,
// bearing, line height). There is NO runtime re-rasterization: after
// `create`, `glyph(code)` is a pure table lookup. The SDF path (P1)
// lands in M2-TEXT-03 on top of the same vendored rasterizer (ADR
// 0009); the text-item / layout pass ships in text_items.h
// (M2-TEXT-02).
//
//   GlyphAtlas          The configured glyph atlas (create + lookup)
//   GlyphMetrics        Per-glyph metrics + atlas cell origin
//   kFont*              The named constants (CORE-005)
//
// ---------------------------------------------------------------------------
// The model (documented units)
// ---------------------------------------------------------------------------
//
// All metric values are PIXELS AT 1x — i.e. at the atlas's configured
// `fontSize` (the font's pixel height, the stbtt pixel-height
// convention: the font's ascent + descent scaled to exactly that
// height). To render at an integer scale s, multiply every metric by
// s (exact for integer s); the atlas bitmap itself is then drawn at
// s× its texel size (the text_items.h/M2-SPRITE-02 sprite `scale`).
//
// `GlyphAtlas::create(font, options)`:
//
//   1. Validates `options` (the documented order below, first failure
//      wins — all `InvalidArgument`, no log);
//   2. Validates the FONT BYTES at the untrusted-input boundary
//      (SCALE-004): a non-font / truncated / corrupt file →
//      `MalformedInput` + one structured Error `font/font_invalid`
//      (CORE-008 — never a crash, never a silent fallback);
//   3. Rasterizes every codepoint in `[glyphFirst, glyphLast]` into the
//      fixed `atlasWidth × atlasHeight` 8-bit alpha atlas with the
//      deterministic shelf packing below (a glyph that does not fit →
//      `BudgetExhausted` + one structured Error `font/atlas_full` —
//      the atlas is fixed-size by contract; grow the atlas, not the
//      font);
//   4. On success: one Info `font/atlas_created` (the set-up path is
//      low-volume; the lookup path logs nothing).
//
// The font bytes are a NON-OWNING span: the caller owns them (the game's
// asset pipeline — the M3 asset system); they are read only during
// `create` and may be freed immediately after (the atlas owns a copy of
// the bitmap, not the font).
//
// ---------------------------------------------------------------------------
// The atlas layout (deterministic shelf packing)
// ---------------------------------------------------------------------------
//
// The atlas is `atlasWidth × atlasHeight` texels of 8-bit alpha (0 =
// transparent). Codepoints are laid out in ASCENDING order, one shelf
// row at a time: the cursor starts at (0, 0); a glyph's cell is placed
// at the cursor; the cursor advances right by the cell's width; when
// the cell would not fit on the row it wraps to the next row (the
// cursor's y advances by the row's max cell height). A cell is EXACTLY
// the glyph's ink bitmap (no padding — stbtt's rasterization is
// clipped to the cell, so no glyph can bleed into a neighbor; the
// M2-SPRITE-02 `GL_NEAREST` "texel = floor(u·W)" contract applies).
// Empty glyphs (zero-width/height ink — e.g. the space) occupy NO cell
// space. The packing is a pure function of (font bytes, options):
// same input → bit-identical atlas bytes (the determinism test).
//
// `GlyphMetrics.atlasX/atlasY` is the cell's top-left texel; the glyph's
// UV rect for the M2-SPRITE-02 upload is
// `(atlasX/W, atlasY/H, (atlasX+width)/W, (atlasY+height)/H)` (the
// M2-SPRITE-03 v-axis convention: v = 0 = first uploaded texel row).
//
// ---------------------------------------------------------------------------
// The metrics (px at 1x)
// ---------------------------------------------------------------------------
//
//   advance   the pen advance after drawing the glyph (positive =
//             right; a negative advance is stored as-is — RTL/quirk
//             fonts are out of scope, but the field is total).
//   bearingX  the horizontal offset of the bitmap's LEFT edge from the
//             pen (positive = right of the pen; negative = the ink
//             overhangs the pen left — the cell is placed at
//             penX + bearingX).
//   bearingY  the vertical offset of the bitmap's TOP edge ABOVE the
//             baseline (positive = above the baseline).
//   width/height  the bitmap's ink size in texels (0 × 0 = the empty
//             glyph — nothing is drawn, the advance still applies).
//
// `lineHeight()` is the line height in px at 1x: by the stbtt
// pixel-height convention the font's ascent + descent is scaled to
// EXACTLY the configured `fontSize`, so `lineHeight() == fontSize()`
// (the formula is documented, not a separate measurement).
//
// ---------------------------------------------------------------------------
// Missing glyphs (the documented fallback)
// ---------------------------------------------------------------------------
//
// `glyph(code)` is a TOTAL function — it never crashes and, for a live
// atlas, never returns null:
//
// - `code` in `[glyphFirst, glyphLast]` → that codepoint's slot. If
//   the font has no glyph for the codepoint, the slot holds the font's
//   `.notdef` glyph (stbtt maps unknown codepoints to glyph 0) —
//   deterministic per font.
// - `code` OUTSIDE the configured range → the FALLBACK glyph: the
//   U+0020 (space) slot if `kFontFallbackCode` lies in the configured
//   range (the default Latin-1 range does), else `kFontEmptyGlyphMetrics`
//   (a zero metric — the advance applies, nothing is drawn).
//
// The fallback is documented, never silent: a game that wants a visible
// "missing character" box configures a glyph range that includes its
// box glyph (the `.notdef` of its font) — the engine does not draw
// extra glyphs.
//
// ---------------------------------------------------------------------------
// Ownership, threading (CORE-009, CONC-001)
// ---------------------------------------------------------------------------
//
// `GlyphAtlas` owns its atlas bytes and slot table (move-only, the
// `SpriteBatcher` precedent); one owner (the scene set-up thread); the
// render phase reads it (the M2-GL-02 cull/batch stage uploads the
// atlas + declares text quads — text_items.h, M2-TEXT-02).
// Presentation-only (ARCH-009):
// the atlas is never part of the sim state hash or replay state. No GL
// calls anywhere in this header.
//
// Canonical narrative: docs/api/font.md (the full contract).
// The text feature's place in the 2.5D model:
// docs/concepts/coordinates.md (§4.12 — the text row, shipped).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "laige/errors.h"
#include "laige/result.h"

namespace laige::render {

// Named constants (CORE-005).
//
// The atlas dimension domain: [1, kFontMaxAtlasDimension]. 4096² = 16
// 777 216 texels (16 MiB at 8-bit alpha) — far beyond the practical
// driver maxTextureSize (16K) for a single glyph atlas.
inline constexpr std::uint32_t kFontMaxAtlasDimension = 4096;
// The configured pixel-height domain: [1, kFontMaxFontSize] px at 1x.
// 1024 px is beyond any practical UI text size; the rasterizer's float
// domain (the pixel box stays < 2^31 for every unitsPerEm) is never
// approached.
inline constexpr std::uint32_t kFontMaxFontSize = 1024;
// The glyph-range domain: codepoints up to U+10FFFF (the Unicode
// ceiling); the RANGE SIZE is bounded by kFontMaxGlyphCount (a full
// 1.1M-codepoint range would rasterize minutes of set-up work — the
// documented budget guardrail, PERF-008).
inline constexpr std::uint32_t kFontMaxGlyphCode = 0x10FFFF;
inline constexpr std::uint32_t kFontMaxGlyphCount = 8192;
// The defaults (the Latin-1 set, the FR-2.8 P0 scope: 32..126 printable
// ASCII + 160..255 the Latin-1 supplement; 127..159 C1 controls rasterize
// to the font's .notdef — harmless, deterministic).
inline constexpr std::uint32_t kFontDefaultAtlasWidth = 1024;
inline constexpr std::uint32_t kFontDefaultAtlasHeight = 1024;
inline constexpr std::uint32_t kFontDefaultFontSize = 16;
inline constexpr std::uint32_t kFontDefaultGlyphFirst = 32;  // U+0020 SPACE
inline constexpr std::uint32_t kFontDefaultGlyphLast = 255;  // U+00FF
// The documented fallback glyph (the header preamble): U+0020 SPACE.
inline constexpr std::uint32_t kFontFallbackCode = 32;

// Per-glyph metrics + the atlas cell origin (px at 1x, the header
// preamble). Plain value: no ownership, no lifetime beyond the atlas
// that stores it.
struct GlyphMetrics {
  std::int32_t advance{0};    // pen advance, px at 1x
  std::int32_t bearingX{0};   // bitmap left edge from the pen (right positive)
  std::int32_t bearingY{0};   // bitmap top edge above the baseline (up positive)
  std::int32_t width{0};      // bitmap ink width, texels (0 = empty glyph)
  std::int32_t height{0};     // bitmap ink height, texels (0 = empty glyph)
  std::uint32_t atlasX{0};    // cell top-left, texels in the atlas
  std::uint32_t atlasY{0};

  // Field-wise equality (the deterministic-comparison test needs it;
  // the house idiom — no synthesized ==, the SpriteUvRect precedent).
  [[nodiscard]] bool operator==(const GlyphMetrics& other) const noexcept {
    return advance == other.advance && bearingX == other.bearingX &&
           bearingY == other.bearingY && width == other.width &&
           height == other.height && atlasX == other.atlasX &&
           atlasY == other.atlasY;
  }
};

// The zero metric: the out-of-range fallback when the configured glyph
// range does not include the fallback code (the header preamble).
inline constexpr GlyphMetrics kFontEmptyGlyphMetrics{};

// The configured bitmap glyph atlas (M2-TEXT-01).
class GlyphAtlas {
 public:
  // The create options (API-006): see the header preamble for the
  // documented units.
  struct Options {
    std::uint32_t atlasWidth{kFontDefaultAtlasWidth};
    std::uint32_t atlasHeight{kFontDefaultAtlasHeight};
    std::uint32_t fontSize{kFontDefaultFontSize};
    std::uint32_t glyphFirst{kFontDefaultGlyphFirst};
    std::uint32_t glyphLast{kFontDefaultGlyphLast};
  };

  // Rasterizes the configured glyph range of `font` into the atlas
  // (set-up path — the only allocations: the atlas bytes + the slot
  // table). Validation order, first failure wins:
  //   1. atlasWidth/atlasHeight in [1, kFontMaxAtlasDimension],
  //      fontSize in [1, kFontMaxFontSize],
  //      glyphFirst ≤ glyphLast ≤ kFontMaxGlyphCode,
  //      (glyphLast − glyphFirst + 1) ≤ kFontMaxGlyphCount
  //      (InvalidArgument, no log — the M2-SPRITE-01 create precedent);
  //   2. the font bytes: a non-font / truncated / corrupt file →
  //      MalformedInput + one Error `font/font_invalid`;
  //   3. a glyph whose cell does not fit the fixed atlas →
  //      BudgetExhausted + one Error `font/atlas_full` (fields: the
  //      codepoint, its cell size, the atlas size).
  // A failed create returns the error; no atlas is produced.
  [[nodiscard]] static laige::Result<GlyphAtlas, laige::ErrorCode>
  create(std::span<const std::uint8_t> font, Options options) noexcept;

  // The stopped state (default / moved-from): `valid()` false,
  // `glyph()` returns null, `atlas()` empty, introspection zero — the
  // RenderThread stopped-state precedent. Nothing owned.
  GlyphAtlas() noexcept = default;

  GlyphAtlas(const GlyphAtlas&) = delete;
  GlyphAtlas& operator=(const GlyphAtlas&) = delete;
  GlyphAtlas(GlyphAtlas&&) noexcept = default;
  GlyphAtlas& operator=(GlyphAtlas&&) noexcept = default;
  ~GlyphAtlas() noexcept = default;

  // True iff the atlas is live (create succeeded).
  [[nodiscard]] bool valid() const noexcept { return !atlas_.empty(); }

  // The atlas dimensions (texels; zero in the stopped state).
  [[nodiscard]] std::uint32_t atlasWidth() const noexcept { return atlasWidth_; }
  [[nodiscard]] std::uint32_t atlasHeight() const noexcept { return atlasHeight_; }

  // The configured pixel height, px at 1x.
  [[nodiscard]] std::uint32_t fontSize() const noexcept { return fontSize_; }

  // The line height, px at 1x: `fontSize()` (the stbtt pixel-height
  // convention — the header preamble).
  [[nodiscard]] std::uint32_t lineHeight() const noexcept { return fontSize_; }

  // The configured glyph range (inclusive).
  [[nodiscard]] std::uint32_t glyphFirst() const noexcept { return glyphFirst_; }
  [[nodiscard]] std::uint32_t glyphLast() const noexcept { return glyphLast_; }

  // The atlas bytes: 8-bit alpha, row-major, `atlasWidth() × atlasHeight()`
  // texels (empty in the stopped state). The render read path: the
  // text_items.h (M2-TEXT-02) uploads this as a white-on-alpha RGBA8
  // atlas (the M2-SPRITE-02 `bindAtlas` contract). O(1); never writes.
  [[nodiscard]] std::span<const std::uint8_t> atlas() const noexcept {
    return atlas_;
  }

  // The metrics for `code` (the header preamble: a total function —
  // null only in the stopped state; the in-range slot, else the
  // documented fallback). O(1); no allocation, no logging, no GL.
  // The returned pointer is valid until this atlas is moved/destroyed
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
};

}  // namespace laige::render
