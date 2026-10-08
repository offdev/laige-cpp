// laige-render bitmap font atlas implementation (M2-TEXT-01).
//
// The engine boundary of the vendored stb_truetype rasterizer (ADR
// 0009): this file uses the vendor header's DECLARATIONS only (the
// implementation is compiled once in laige-stb, plain compiler policy),
// and the public header (font.h) names no stb type (CPP-010, DEP-004).

#include "laige/render/font.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "laige/logging.h"
#include "stb_truetype.h"  // declarations only (impl: laige-stb target)

namespace laige::render {
namespace {

// The TrueType/OpenType header + table-directory guard (the untrusted-
// input boundary, SCALE-004): stb reads the font header and scans the
// table directory WITHOUT bounds checks (its buf-based table parsing is
// bounds-checked, the directory scan is not — verified against the
// pinned header), so the engine requires the header (12 bytes) and the
// FULL table directory to be present before any stb call. A truncated
// or non-font file is rejected here, never read past.
// Returns false (not a parsable font header) when the guard fails.
bool fontDirectoryPresent(std::span<const std::uint8_t> font) noexcept {
  constexpr std::size_t kHeaderBytes = 12;  // 4 version + 4 count + 4 offset
  if (font.size() < kHeaderBytes) {
    return false;
  }
  const std::uint32_t numTables =
      (static_cast<std::uint32_t>(font[4]) << 8) |
      static_cast<std::uint32_t>(font[5]);
  return kHeaderBytes + 16 * numTables <= font.size();
}

// One rasterized glyph bitmap. Owns the stbtt-malloc'd buffer — the
// C-library allocation is encapsulated here (CPP-005).
struct GlyphBitmap {
  std::uint8_t* data = nullptr;
  std::int32_t width = 0;
  std::int32_t height = 0;
  ~GlyphBitmap() {
    if (data != nullptr) {
      std::free(data);
    }
  }
};

}  // namespace

laige::Result<GlyphAtlas, laige::ErrorCode> GlyphAtlas::create(
    std::span<const std::uint8_t> font, Options options) noexcept {
  // --- 1. Options validation (the documented order, first failure wins;
  // no log — the M2-SPRITE-01 create precedent) -------------------------
  if (options.atlasWidth < 1 || options.atlasWidth > kFontMaxAtlasDimension ||
      options.atlasHeight < 1 || options.atlasHeight > kFontMaxAtlasDimension ||
      options.fontSize < 1 || options.fontSize > kFontMaxFontSize ||
      options.glyphFirst > options.glyphLast ||
      options.glyphLast > kFontMaxGlyphCode ||
      options.glyphLast - options.glyphFirst + 1 > kFontMaxGlyphCount) {
    return laige::Result<GlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }

  // --- 2. The font bytes (untrusted input, SCALE-004) -------------------
  if (!fontDirectoryPresent(font)) {
    LAIGE_LOG_ERROR("font", "font_invalid",
                    "The configured font file is not a parsable TTF/OTF "
                    "font (truncated or non-font bytes)",
                    laige::log::field("bytes", font.size()));
    return laige::Result<GlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::MalformedInput);
  }
  stbtt_fontinfo stbFont;
  // The directory guard above makes the scan reads in-bounds; the offset
  // for a plain font is 0 (a TTC's first font is what index 0 names).
  const int offset = stbtt_GetFontOffsetForIndex(font.data(), 0);
  if (offset < 0 || !stbtt_InitFont(&stbFont, font.data(), offset)) {
    LAIGE_LOG_ERROR("font", "font_invalid",
                    "The configured font file failed TTF/OTF parsing "
                    "(missing required tables or corrupt table data)",
                    laige::log::field("bytes", font.size()));
    return laige::Result<GlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::MalformedInput);
  }

  // --- 3. The fixed-size atlas + per-codepoint slots ---------------------
  GlyphAtlas atlas;
  atlas.atlasWidth_ = options.atlasWidth;
  atlas.atlasHeight_ = options.atlasHeight;
  atlas.fontSize_ = options.fontSize;
  atlas.glyphFirst_ = options.glyphFirst;
  atlas.glyphLast_ = options.glyphLast;
  atlas.atlas_.assign(
      static_cast<std::size_t>(options.atlasWidth) *
          static_cast<std::size_t>(options.atlasHeight),
      0);
  const std::size_t slotCount =
      static_cast<std::size_t>(options.glyphLast - options.glyphFirst + 1);
  atlas.slots_.reserve(slotCount);  // the set-up path's only allocations

  const float scale =
      stbtt_ScaleForPixelHeight(&stbFont, static_cast<float>(options.fontSize));
  // The degenerate-metric guard (SCALE-004): the pixel-height scale
  // (px per font unit = fontSize / (hhea ascent − hhea descent)) must
  // be finite and in the int domain, so stbtt's float→int box rounding
  // and the lround'd advance (advanceUnits ≤ 65535 × scale, box ≤
  // 65534 × scale — both < 2^31 when scale ≤ 2^15) stay in their int
  // domains. The reachable max is kFontMaxFontSize / 1 = 1024 (the
  // 2^15 bound is the explicit int-domain ceiling); scale ≤ 0 or inf
  // means a font with zero/inverted vertical metrics — malformed.
  constexpr float kMaxPixelScale = 32768.0f;  // 2^15, the int-domain bound
  if (!std::isfinite(scale) || scale <= 0.0f || scale > kMaxPixelScale) {
    LAIGE_LOG_ERROR("font", "font_invalid",
                    "The font's vertical metrics are degenerate (the "
                    "pixel-height scale is not a finite value in the "
                    "documented domain)",
                    laige::log::field("bytes", font.size()));
    return laige::Result<GlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::MalformedInput);
  }

  // The deterministic shelf packing (font.h preamble): ascending
  // codepoint order, one shelf row at a time, cells exactly the ink
  // bitmaps.
  std::uint32_t cursorX = 0;
  std::uint32_t cursorY = 0;
  std::uint32_t rowHeight = 0;
  for (std::uint32_t code = options.glyphFirst; code <= options.glyphLast;
       ++code) {
    const int glyphIndex = stbtt_FindGlyphIndex(&stbFont, static_cast<int>(code));
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBox(&stbFont, glyphIndex, scale, scale, &x0, &y0, &x1,
                            &y1);
    const std::int32_t w = static_cast<std::int32_t>(x1 - x0);
    const std::int32_t h = static_cast<std::int32_t>(y1 - y0);

    // The capacity check BEFORE the bitmap allocation: the cell must fit
    // the fixed atlas (a single cell wider/taller than the atlas can
    // never pack), and it bounds stb's w * h allocation to
    // atlasWidth * atlasHeight <= 2^24 bytes (no int overflow, no
    // unbounded malloc — the malicious-font guard, SCALE-004).
    if (w > 0 && (w > static_cast<std::int32_t>(options.atlasWidth) ||
                  h > static_cast<std::int32_t>(options.atlasHeight))) {
      LAIGE_LOG_ERROR("font", "atlas_full",
                      "A glyph's bitmap does not fit the fixed glyph atlas "
                      "(grow the atlas or shrink the font size)",
                      laige::log::field("code", code),
                      laige::log::field("width", w),
                      laige::log::field("height", h),
                      laige::log::field("atlas_width", options.atlasWidth),
                      laige::log::field("atlas_height", options.atlasHeight));
      return laige::Result<GlyphAtlas, laige::ErrorCode>::failure(
          laige::ErrorCode::BudgetExhausted);
    }

    // The pen advance (unscaled font units; the LSB is not a metric
    // here — the bearing comes from the ink box's x0, the bitmap's true
    // left edge).
    int advanceUnits = 0;
    stbtt_GetGlyphHMetrics(&stbFont, glyphIndex, &advanceUnits, nullptr);

    GlyphMetrics metrics;
    metrics.bearingX = x0;  // the bitmap's left edge from the pen (right +)
    metrics.bearingY = -y0;  // the bitmap's top edge above the baseline (up +)
    metrics.width = w;
    metrics.height = h;
    metrics.advance =
        static_cast<std::int32_t>(std::lround(advanceUnits * scale));

    if (w > 0) {
      // The shelf packing: wrap the row when the cell does not fit.
      if (cursorX + static_cast<std::uint32_t>(w) > options.atlasWidth) {
        cursorX = 0;
        cursorY += rowHeight;
        rowHeight = 0;
      }
      if (cursorY + static_cast<std::uint32_t>(h) > options.atlasHeight) {
        LAIGE_LOG_ERROR("font", "atlas_full",
                        "The glyph atlas is full (the configured glyph range "
                        "does not fit the fixed atlas; grow the atlas or "
                        "shrink the font size)",
                        laige::log::field("code", code),
                        laige::log::field("atlas_width", options.atlasWidth),
                        laige::log::field("atlas_height", options.atlasHeight));
        return laige::Result<GlyphAtlas, laige::ErrorCode>::failure(
            laige::ErrorCode::BudgetExhausted);
      }
    }
    // The cell origin is the cursor AFTER any row wrap (a wrapped cell
    // starts the new row, not at the stale end of the old one).
    metrics.atlasX = cursorX;
    metrics.atlasY = cursorY;

    if (w > 0) {
      if (h > 0) {
        GlyphBitmap bitmap;
        bitmap.width = w;
        bitmap.height = h;
        bitmap.data = stbtt_GetGlyphBitmap(&stbFont, scale, scale, glyphIndex,
                                           &bitmap.width, &bitmap.height,
                                           nullptr, nullptr);
        if (bitmap.data == nullptr) {
          // Unreachable for a validated (w, h): STBTT_malloc failure is
          // the only null path, and w * h <= 2^24 bytes.
          LAIGE_LOG_ERROR("font", "rasterize_failed",
                          "Glyph bitmap rasterization failed (allocation "
                          "failure in the font rasterizer)",
                          laige::log::field("code", code));
          return laige::Result<GlyphAtlas, laige::ErrorCode>::failure(
              laige::ErrorCode::MalformedInput);
        }
        // The verbatim copy (8-bit alpha rows, the stbtt row-major layout).
        for (std::int32_t row = 0; row < h; ++row) {
          std::uint8_t* dst =
              atlas.atlas_.data() +
              static_cast<std::size_t>(cursorY + static_cast<std::uint32_t>(row)) *
                  static_cast<std::size_t>(options.atlasWidth) +
              cursorX;
          std::copy_n(bitmap.data + static_cast<std::size_t>(row) * w, w, dst);
        }
        cursorX += static_cast<std::uint32_t>(w);
        if (h > static_cast<std::int32_t>(rowHeight)) {
          rowHeight = static_cast<std::uint32_t>(h);
        }
      }
    }
    atlas.slots_.push_back(metrics);
  }

  LAIGE_LOG_INFO("font", "atlas_created",
                 "The bitmap glyph atlas was built at scene set-up",
                 laige::log::field("atlas_width", options.atlasWidth),
                 laige::log::field("atlas_height", options.atlasHeight),
                 laige::log::field("font_size", options.fontSize),
                 laige::log::field("glyphs", slotCount));
  return laige::Result<GlyphAtlas, laige::ErrorCode>::success(std::move(atlas));
}

const GlyphMetrics* GlyphAtlas::glyph(std::uint32_t code) const noexcept {
  if (!valid()) {
    return nullptr;
  }
  if (code >= glyphFirst_ && code <= glyphLast_) {
    return &slots_[static_cast<std::size_t>(code - glyphFirst_)];
  }
  // The documented fallback (font.h preamble): the U+0020 slot when the
  // configured range includes it, else the zero metric.
  if (kFontFallbackCode >= glyphFirst_ && kFontFallbackCode <= glyphLast_) {
    return &slots_[static_cast<std::size_t>(kFontFallbackCode - glyphFirst_)];
  }
  return &kFontEmptyGlyphMetrics;
}

}  // namespace laige::render
