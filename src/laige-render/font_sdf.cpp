// laige-render SDF glyph atlas implementation (M2-TEXT-03).
//
// The engine boundary of the vendored stb_truetype rasterizer (ADR
// 0009) for the SDF path: this file uses the vendor header's
// DECLARATIONS only (the implementation is compiled once in laige-stb,
// plain compiler policy), and the public header (font_sdf.h) names no
// stb type (CPP-010, DEP-004).
//
// The distance field (font_sdf.h "The distance-field algorithm",
// CORE-014): the Felice & Nienhuis 8SDD two-pass, 8-neighborhood,
// Euclidean signed distance transform (IEEE TPAMI 36(3), 2014,
// §III-B) of the 1× rasterization's 0.5-coverage pixels, quantized to
// the documented 8-bit field (128 = the contour, ±r px exact).

#include "laige/render/font_sdf.h"

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
// input boundary, SCALE-004 — the M2-TEXT-01 guard, verbatim): stb
// reads the font header and scans the table directory WITHOUT bounds
// checks (its buf-based table parsing is bounds-checked, the directory
// scan is not — verified against the pinned header), so the engine
// requires the header (12 bytes) and the FULL table directory to be
// present before any stb call. A truncated or non-font file is
// rejected here, never read past.
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
// C-library allocation is encapsulated here (CPP-005 — the M2-TEXT-01
// GlyphBitmap).
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

// The 8SDD Euclidean distance transform (Felice & Nienhuis 2014,
// §III-B): the exact Euclidean distance, in pixels (center-to-center),
// from every cell pixel to the nearest pixel of the target set — 0 on
// the set itself. `inside` is the W × H 0.5-coverage grid; the target
// set is `inside` when `targetIsInside`, else its complement (the
// outside). Two passes over the 8-neighborhood graph — the forward
// (raster) pass relaxes from the upper-left half of the neighborhood,
// the backward pass from the lower-right — which the paper proves
// exact for the 8-neighborhood graph in 2D: every shortest 8-path
// reaches every pixel from one of the two scan directions.
//
// Complexity: O(8·W·H) per call (two passes, 4 neighbor candidates
// each). Fixed float operation order: min of (candidate + constant)
// over a FIXED neighbor order — deterministic, bit-identical for the
// same grid on every build (ARCH-009 presentation float).
//
// The neighbor constants: orthogonal steps cost exactly 1.0 px
// (center-to-center), diagonal steps √2 px (the house dyadic-free
// float constant, the kIsoSnapSqrtTwo precedent — no libm call in the
// pass). INF keeps the relaxation total: 1e30 + 1.42 stays finite in
// float32 (the cell's true max distance is < 2^15 px at the domain
// ceiling, so no distance ever collides with the INF band).
constexpr float kSqrt2 = 1.4142135623730951f;
constexpr float kSdtInfinity = 1.0e30f;

void signedDistanceTransform(std::span<float> d,
                             std::span<const std::uint8_t> inside,
                             std::uint32_t w, std::uint32_t h,
                             bool targetIsInside) noexcept {
  const std::size_t count = static_cast<std::size_t>(w) * h;
  // The initialization: 0 on the target set, INF elsewhere. The inside
  // grid is 1 byte/pixel (the set-up path's temporary).
  for (std::size_t i = 0; i < count; ++i) {
    const bool onSet = targetIsInside ? inside[static_cast<std::size_t>(i)] != 0
                                      : inside[static_cast<std::size_t>(i)] == 0;
    d[static_cast<std::size_t>(i)] = onSet ? 0.0f : kSdtInfinity;
  }
  // The forward pass (raster order): relax from the upper-left half of
  // the 8-neighborhood (left, upper-left, up, upper-right).
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::size_t i = static_cast<std::size_t>(y) * w + x;
      float best = d[i];
      if (x > 0) {
        best = std::min(best, d[i - 1] + 1.0f);
      }
      if (y > 0) {
        const std::size_t up = i - static_cast<std::size_t>(w);
        if (x > 0) {
          best = std::min(best, d[up - 1] + kSqrt2);
        }
        best = std::min(best, d[up] + 1.0f);
        if (x + 1 < w) {
          best = std::min(best, d[up + 1] + kSqrt2);
        }
      }
      d[i] = best;
    }
  }
  // The backward pass (reverse raster order): relax from the
  // lower-right half (right, lower-left, down, lower-right).
  for (std::uint32_t y = h; y-- > 0;) {
    for (std::uint32_t x = w; x-- > 0;) {
      std::size_t i = static_cast<std::size_t>(y) * w + x;
      float best = d[i];
      if (x + 1 < w) {
        best = std::min(best, d[i + 1] + 1.0f);
      }
      if (y + 1 < h) {
        const std::size_t down = i + static_cast<std::size_t>(w);
        if (x > 0) {
          best = std::min(best, d[down - 1] + kSqrt2);
        }
        best = std::min(best, d[down] + 1.0f);
        if (x + 1 < w) {
          best = std::min(best, d[down + 1] + kSqrt2);
        }
      }
      d[i] = best;
    }
  }
}

}  // namespace

laige::Result<SdfGlyphAtlas, laige::ErrorCode> SdfGlyphAtlas::create(
    std::span<const std::uint8_t> font, Options options) noexcept {
  // --- 1. Options validation (the documented order, first failure
  // wins; no log — the M2-TEXT-01 create precedent) -------------------
  if (options.atlasWidth < 1 ||
      options.atlasWidth > kFontMaxAtlasDimension ||
      options.atlasHeight < 1 ||
      options.atlasHeight > kFontMaxAtlasDimension ||
      options.fontSize < 1 || options.fontSize > kFontMaxFontSize ||
      options.glyphFirst > options.glyphLast ||
      options.glyphLast > kFontMaxGlyphCode ||
      options.glyphLast - options.glyphFirst + 1 > kFontMaxGlyphCount ||
      options.sdfRadius < 1 || options.sdfRadius > kSdfMaxRadius) {
    return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }

  // --- 2. The font bytes (untrusted input, SCALE-004 — the M2-TEXT-01
  // untrusted-font contract, verbatim) --------------------------------
  if (!fontDirectoryPresent(font)) {
    LAIGE_LOG_ERROR("font", "font_invalid",
                    "The configured font file is not a parsable TTF/OTF "
                    "font (truncated or non-font bytes)",
                    laige::log::field("bytes", font.size()));
    return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::MalformedInput);
  }
  stbtt_fontinfo stbFont;
  // The directory guard above makes the scan reads in-bounds; the
  // offset for a plain font is 0 (a TTC's first font is what index 0
  // names).
  const int offset = stbtt_GetFontOffsetForIndex(font.data(), 0);
  if (offset < 0 || !stbtt_InitFont(&stbFont, font.data(), offset)) {
    LAIGE_LOG_ERROR("font", "font_invalid",
                    "The configured font file failed TTF/OTF parsing "
                    "(missing required tables or corrupt table data)",
                    laige::log::field("bytes", font.size()));
    return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::MalformedInput);
  }

  // --- 3. The fixed-size atlas + per-codepoint slots -------------------
  SdfGlyphAtlas atlas;
  atlas.atlasWidth_ = options.atlasWidth;
  atlas.atlasHeight_ = options.atlasHeight;
  atlas.fontSize_ = options.fontSize;
  atlas.glyphFirst_ = options.glyphFirst;
  atlas.glyphLast_ = options.glyphLast;
  atlas.radius_ = options.sdfRadius;
  atlas.atlas_.assign(
      static_cast<std::size_t>(options.atlasWidth) *
          static_cast<std::size_t>(options.atlasHeight),
      0);
  const std::size_t slotCount =
      static_cast<std::size_t>(options.glyphLast - options.glyphFirst + 1);
  atlas.slots_.reserve(slotCount);  // the set-up path's allocations

  const float scale =
      stbtt_ScaleForPixelHeight(&stbFont, static_cast<float>(options.fontSize));
  // The degenerate-metric guard (SCALE-004) — the M2-TEXT-01 guard,
  // verbatim (the same int-domain argument over stbtt's float→int box
  // rounding and the lround'd advance).
  constexpr float kMaxPixelScale = 32768.0f;  // 2^15, the int-domain bound
  if (!std::isfinite(scale) || scale <= 0.0f || scale > kMaxPixelScale) {
    LAIGE_LOG_ERROR("font", "font_invalid",
                    "The font's vertical metrics are degenerate (the "
                    "pixel-height scale is not a finite value in the "
                    "documented domain)",
                    laige::log::field("bytes", font.size()));
    return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::failure(
        laige::ErrorCode::MalformedInput);
  }

  // The deterministic shelf packing (font_sdf.h preamble): ascending
  // codepoint order, one shelf row at a time, cells the ink boxes GROWN
  // by the radius on all four sides (the padded cell).
  const std::int32_t radius = static_cast<std::int32_t>(options.sdfRadius);
  std::uint32_t cursorX = 0;
  std::uint32_t cursorY = 0;
  std::uint32_t rowHeight = 0;
  for (std::uint32_t code = options.glyphFirst; code <= options.glyphLast;
       ++code) {
    const int glyphIndex =
        stbtt_FindGlyphIndex(&stbFont, static_cast<int>(code));
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBox(&stbFont, glyphIndex, scale, scale, &x0, &y0, &x1,
                            &y1);
    const std::int32_t w = static_cast<std::int32_t>(x1 - x0);
    const std::int32_t h = static_cast<std::int32_t>(y1 - y0);

    // The capacity check BEFORE the bitmap allocation: the PADDED cell
    // must fit the fixed atlas (a single padded cell wider/taller than
    // the atlas can never pack), and it bounds stb's w * h allocation
    // to (atlasWidth − 2r) * (atlasHeight − 2r) <= 2^24 bytes (no int
    // overflow, no unbounded malloc — the malicious-font guard,
    // SCALE-004).
    if (w > 0 &&
        (w + 2 * radius > static_cast<std::int32_t>(options.atlasWidth) ||
         h + 2 * radius > static_cast<std::int32_t>(options.atlasHeight))) {
      LAIGE_LOG_ERROR("font", "atlas_full",
                      "A glyph's padded SDF cell does not fit the fixed "
                      "glyph atlas (grow the atlas or shrink the font "
                      "size or the SDF radius)",
                      laige::log::field("code", code),
                      laige::log::field("width", w + 2 * radius),
                      laige::log::field("height", h + 2 * radius),
                      laige::log::field("atlas_width", options.atlasWidth),
                      laige::log::field("atlas_height", options.atlasHeight));
      return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::failure(
          laige::ErrorCode::BudgetExhausted);
    }

    // The pen advance (unscaled font units; the bearing comes from the
    // ink box's x0, the bitmap's true left edge — the M2-TEXT-01
    // metrics, verbatim).
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
      // The shelf packing over the PADDED cell (the row's cursor
      // advances by the padded width, wraps on the padded width).
      const std::uint32_t paddedW =
          static_cast<std::uint32_t>(w + 2 * radius);
      const std::uint32_t paddedH =
          static_cast<std::uint32_t>(h + 2 * radius);
      if (cursorX + paddedW > options.atlasWidth) {
        cursorX = 0;
        cursorY += rowHeight;
        rowHeight = 0;
      }
      if (cursorY + paddedH > options.atlasHeight) {
        LAIGE_LOG_ERROR("font", "atlas_full",
                        "The SDF glyph atlas is full (the configured glyph "
                        "range does not fit the fixed atlas with the "
                        "distance-field padding; grow the atlas, shrink "
                        "the font size, or reduce the SDF radius)",
                        laige::log::field("code", code),
                        laige::log::field("atlas_width", options.atlasWidth),
                        laige::log::field("atlas_height", options.atlasHeight));
        return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::failure(
            laige::ErrorCode::BudgetExhausted);
      }
    }
    // The cell origin is the cursor AFTER any row wrap (the M2-TEXT-01
    // post-wrap cursor — a wrapped cell starts the new row, not at the
    // stale end of the old one). For the SDF path the origin is the
    // PADDED CELL's top-left (font_sdf.h preamble: the ink sits at
    // origin + (r, r)).
    metrics.atlasX = cursorX;
    metrics.atlasY = cursorY;

    if (w > 0 && h > 0) {
      // The ink bitmap (the 1× rasterization — the M2-TEXT-01 cell).
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
        return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::failure(
            laige::ErrorCode::MalformedInput);
      }
      // The 0.5-coverage grid of the PADDED cell (the inside set of the
      // distance transform): 1 byte/pixel, the ink at offset (r, r).
      const std::uint32_t cellW = static_cast<std::uint32_t>(w + 2 * radius);
      const std::uint32_t cellH = static_cast<std::uint32_t>(h + 2 * radius);
      const std::size_t cellCount =
          static_cast<std::size_t>(cellW) * cellH;
      std::vector<std::uint8_t> inside(cellCount, 0);
      for (std::int32_t row = 0; row < h; ++row) {
        const std::uint8_t* src =
            bitmap.data + static_cast<std::size_t>(row) * w;
        std::uint8_t* dst = inside.data() +
            static_cast<std::size_t>(static_cast<std::uint32_t>(row) +
                                     static_cast<std::uint32_t>(radius)) *
                cellW +
            static_cast<std::uint32_t>(radius);
        for (std::int32_t col = 0; col < w; ++col) {
          // The documented 0.5-coverage cut (font_sdf.h field section):
          // coverage > 127 is inside, ≤ 127 is outside.
          dst[col] = src[col] > kSdfInsideCut ? 1 : 0;
        }
      }
      // The two 8SDD passes (the header's algorithm section): the
      // distance to the nearest inside pixel (dIn — 0 on inside) and to
      // the nearest outside pixel (dOut — 0 on outside). The signed
      // field is +dOut on inside pixels, −dIn on outside pixels
      // (positive inside, the Felice & Nienhuis convention).
      std::vector<float> dIn(cellCount);
      std::vector<float> dOut(cellCount);
      signedDistanceTransform(dIn, inside, cellW, cellH, true);
      signedDistanceTransform(dOut, inside, cellW, cellH, false);
      // The quantization (font_sdf.h field section): byte =
      // clamp(128 + round(127 · d / r), 0, 255). d/r is exact float in
      // the cell's domain (|d| < 2^15 px, the domain ceiling), and
      // 127 · d/r < 2^31, so lround is exact; the clamp: byte 255/1 at
      // exactly |d| = r, 0/255 beyond (the documented saturation).
      std::uint8_t* dstRow =
          atlas.atlas_.data() +
          static_cast<std::size_t>(cursorY) *
              static_cast<std::size_t>(options.atlasWidth) +
          cursorX;
      const float invRadius = 1.0f / static_cast<float>(options.sdfRadius);
      for (std::uint32_t row = 0; row < cellH; ++row) {
        for (std::uint32_t col = 0; col < cellW; ++col) {
          const std::size_t i = static_cast<std::size_t>(row) * cellW + col;
          const float signedDist =
              inside[i] != 0 ? dOut[i] : -dIn[i];
          const long quantized =
              std::lround(128.0 + 127.0f * signedDist * invRadius);
          const int clamped = static_cast<int>(std::max(0L, std::min(255L, quantized)));
          dstRow[static_cast<std::size_t>(row) *
                     static_cast<std::size_t>(options.atlasWidth) +
                 static_cast<std::size_t>(col)] =
              static_cast<std::uint8_t>(clamped);
        }
      }
      cursorX += static_cast<std::uint32_t>(w + 2 * radius);
      if (cellH > rowHeight) {
        rowHeight = cellH;
      }
    }
    atlas.slots_.push_back(metrics);
  }

  LAIGE_LOG_INFO("font", "sdf_atlas_created",
                 "The SDF glyph atlas was built at scene set-up",
                 laige::log::field("atlas_width", options.atlasWidth),
                 laige::log::field("atlas_height", options.atlasHeight),
                 laige::log::field("font_size", options.fontSize),
                 laige::log::field("sdf_radius", options.sdfRadius),
                 laige::log::field("glyphs", slotCount));
  return laige::Result<SdfGlyphAtlas, laige::ErrorCode>::success(
      std::move(atlas));
}

const GlyphMetrics* SdfGlyphAtlas::glyph(std::uint32_t code) const noexcept {
  if (!valid()) {
    return nullptr;
  }
  if (code >= glyphFirst_ && code <= glyphLast_) {
    return &slots_[static_cast<std::size_t>(code - glyphFirst_)];
  }
  // The documented fallback (font_sdf.h preamble — the M2-TEXT-01
  // contract, verbatim): the U+0020 slot when the configured range
  // includes it, else the zero metric.
  if (kFontFallbackCode >= glyphFirst_ && kFontFallbackCode <= glyphLast_) {
    return &slots_[static_cast<std::size_t>(kFontFallbackCode - glyphFirst_)];
  }
  return &kFontEmptyGlyphMetrics;
}

}  // namespace laige::render
