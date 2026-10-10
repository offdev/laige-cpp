// laige-render SDF glyph atlas tests (M2-TEXT-03): the SDF half of
// FR-2.8 — the configured font + glyph range + distance-field radius
// converted ONCE at scene set-up into a fixed-size 8-bit signed
// distance-field glyph atlas + per-glyph metrics (the M2-TEXT-01
// metrics unchanged), in laige/render/font_sdf.h.
//
// The FontSdfCreate/FontSdfMetrics/FontSdfDeterminism/
// FontSdfThreshold/FontSdfFallback/FontSdfExpand/FontSdfZeroAlloc
// suites are pure engine data (no GL context, no GL environment
// needed): every one runs in every local tree and in CI. The
// FontSdfSmoke suite is the offscreen GL render (the SDF field
// uploaded through `bindSdfAtlas`, one frame, pixel reads) and
// GTEST_SKIPs where no usable OpenGL 3.3 environment — the documented
// environment contract. The field goldens are MEASURED against the
// committed test font (tests/laige-render/assets/vera.ttf — Bitstream
// Vera Sans, OFL 1.1, see the assets/ README) at fontSize 16 and the
// default radius 8: the font is a committed in-tree asset (P0 runners
// differ in their font packages, NFR-8.8 — no system font is trusted).
//
// The determinism contract is IN-PROCESS (same font bytes + options →
// identical atlas bytes; presentation-only float, ARCH-009 — the
// scope of the M2-TEXT-01 font-atlas determinism test), printed as the
// machine-greppable `font-sdf-determinism` line (docs/testing.md §4).
// The width-match contract (the roadmap Verify property) is tested
// EXACT (tolerance 0 — both paths read the identical M2-TEXT-01
// metrics).
//
// No budget gate: the step's roadmap scope has no standalone
// budgets.json entry (the SDF atlas is built once at scene set-up —
// the per-frame text cost is part of the composite 50k render-CPU
// budget, M2-PERF-01).
//
// The font bytes come from the LAIGE_TEST_FONT_PATH env var (set by
// the `font_sdf`, unfiltered `laige-render_tests`, and sanitizer-block
// CTest entries — see tests/laige-render/CMakeLists.txt).

#include "laige/render/font.h"
#include "laige/render/font_sdf.h"
#include "laige/render/gl_context.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_renderer.h"
#include "laige/render/text_items.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige_test_seed.h"

namespace {

using laige::render::GlyphAtlas;
using laige::render::GlyphMetrics;
using laige::render::SdfGlyphAtlas;
using laige::render::kSdfDefaultRadius;
using laige::ErrorCode;

// ---------------------------------------------------------------------------
// The committed test font (tests/laige-render/assets/vera.ttf — the
// LAIGE_TEST_FONT_PATH env var, CTest ENVIRONMENT).
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> loadFont() {
  // The cross-platform env read (tests/support/laige_test_seed.h — MSVC
  // deprecates plain getenv under /WX, C4996).
  const std::string path = laige::testing::ReadEnvVar("LAIGE_TEST_FONT_PATH");
  if (path.empty()) {
    ADD_FAILURE() << "LAIGE_TEST_FONT_PATH is not set (CTest ENVIRONMENT "
                     "contract — see tests/laige-render/CMakeLists.txt)";
    return {};
  }
  // The cross-platform open (fopen_s on MSVC — C4996).
  std::FILE* f = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&f, path.c_str(), "rb") != 0) {
    ADD_FAILURE() << "Cannot open the test font: " << path;
    return {};
  }
#else
  f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    ADD_FAILURE() << "Cannot open the test font: " << path;
    return {};
  }
#endif
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (got != bytes.size()) {
    ADD_FAILURE() << "Short read of the test font: " << path;
    return {};
  }
  return bytes;
}

GlyphAtlas makeBitmapFont() {
  std::vector<std::uint8_t> font = loadFont();
  auto r = GlyphAtlas::create(font, GlyphAtlas::Options{});
  if (!r.ok()) {
    std::fprintf(stderr, "makeBitmapFont: create failed\n");
    std::abort();
  }
  return std::move(r).takeValue();
}

SdfGlyphAtlas makeSdfFont(std::uint32_t radius) {
  std::vector<std::uint8_t> font = loadFont();
  SdfGlyphAtlas::Options options;
  options.sdfRadius = radius;
  auto r = SdfGlyphAtlas::create(font, options);
  if (!r.ok()) {
    std::fprintf(stderr, "makeSdfFont: create failed\n");
    std::abort();
  }
  return std::move(r).takeValue();
}

std::vector<std::uint8_t> makeSdfBytes() {
  const SdfGlyphAtlas atlas = makeSdfFont(kSdfDefaultRadius);
  return std::vector<std::uint8_t>(atlas.atlas().begin(), atlas.atlas().end());
}

// FNV-1a 64 over a byte span (the machine-greppable fingerprint
// convention, docs/testing.md §4 — the font_tests atlasFnv1a64
// pattern).
std::uint64_t atlasFnv1a64(const std::uint8_t* bytes, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis (FNV-1a spec)
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= 0x10000001b3ull;  // FNV prime (FNV-1a spec)
  }
  return h;
}

// ---------------------------------------------------------------------------
// Log capture (the font_tests MemorySink pattern)
// ---------------------------------------------------------------------------

class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
  };

  void emit(const laige::log::LogRecord& record) override {
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    std::abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

// A small interned string (the text_items_tests makeTable/internText
// pattern).
laige::render::StringRef internText(laige::render::StringTable& table,
                                    const char* s) {
  std::vector<std::uint32_t> codes;
  for (const char* p = s; *p != '\0'; ++p) {
    codes.push_back(static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(*p)));
  }
  auto r = table.intern(codes);
  if (!r.ok()) {
    ADD_FAILURE() << "intern failed";
  }
  return std::move(r).takeValue();
}

}  // namespace

// ---------------------------------------------------------------------------
// FontSdfCreate — the create validation matrix (first failure wins),
// the untrusted-font boundary (SCALE-004), the stopped state, the
// capacity guard (the PADDED cell), the success log, and the move
// semantics
// ---------------------------------------------------------------------------

TEST(FontSdfCreate, OptionsValidation) {
  std::vector<std::uint8_t> font = loadFont();
  MemorySink* sink = installCaptureSink();
  struct Case {
    SdfGlyphAtlas::Options options;
    const char* name;
  };
  const std::vector<Case> cases = {
      {SdfGlyphAtlas::Options{.atlasWidth = 0}, "atlasWidth zero"},
      {SdfGlyphAtlas::Options{.atlasWidth = laige::render::kFontMaxAtlasDimension + 1},
       "atlasWidth above the domain"},
      {SdfGlyphAtlas::Options{.atlasHeight = 0}, "atlasHeight zero"},
      {SdfGlyphAtlas::Options{.atlasHeight = laige::render::kFontMaxAtlasDimension + 1},
       "atlasHeight above the domain"},
      {SdfGlyphAtlas::Options{.fontSize = 0}, "fontSize zero"},
      {SdfGlyphAtlas::Options{.fontSize = laige::render::kFontMaxFontSize + 1},
       "fontSize above the domain"},
      {SdfGlyphAtlas::Options{.glyphFirst = 100, .glyphLast = 99},
       "glyphFirst above glyphLast"},
      {SdfGlyphAtlas::Options{.glyphLast = laige::render::kFontMaxGlyphCode + 1},
       "glyphLast above the Unicode ceiling"},
      {SdfGlyphAtlas::Options{.glyphFirst = 0, .glyphLast = laige::render::kFontMaxGlyphCount},
       "glyph range above the size bound"},
      {SdfGlyphAtlas::Options{.sdfRadius = 0}, "radius below the domain"},
      {SdfGlyphAtlas::Options{.sdfRadius = laige::render::kSdfMaxRadius + 1},
       "radius above the domain"},
  };
  for (const Case& c : cases) {
    auto r = SdfGlyphAtlas::create(font, c.options);
    EXPECT_TRUE(r.isError()) << c.name;
    if (r.isError()) {
      EXPECT_EQ(r.error(), ErrorCode::InvalidArgument) << c.name;
    }
  }
  EXPECT_EQ(sink->entries.size(), 0u);  // no log — the create precedent
  restoreLogger();
}

TEST(FontSdfCreate, EmptyOrTruncatedFont) {
  MemorySink* sink = installCaptureSink();
  const std::vector<std::uint8_t> empty;
  auto r = SdfGlyphAtlas::create(empty, SdfGlyphAtlas::Options{});
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::MalformedInput);
  }
  // A buffer shorter than the header + table directory guard (12
  // bytes) — rejected before any stb read (SCALE-004).
  const std::vector<std::uint8_t> shortBuf(10, 0xAB);
  r = SdfGlyphAtlas::create(shortBuf, SdfGlyphAtlas::Options{});
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::MalformedInput);
  }
  // A valid font cut inside the table directory: the guard rejects it
  // (the directory is longer than the buffer) — never read past.
  std::vector<std::uint8_t> font = loadFont();
  auto rCut = SdfGlyphAtlas::create(
      std::span<const std::uint8_t>(font.data(), 16),
      SdfGlyphAtlas::Options{});
  EXPECT_TRUE(rCut.isError());
  if (rCut.isError()) {
    EXPECT_EQ(rCut.error(), ErrorCode::MalformedInput);
  }
  // Exactly one structured Error per rejection (LOG-002).
  const std::size_t errors = static_cast<std::size_t>(std::count_if(
      sink->entries.begin(), sink->entries.end(),
      [](const MemorySink::Entry& e) {
        return e.severity == laige::log::Severity::Error &&
               e.subsystem == "font" && e.event == "font_invalid";
      }));
  EXPECT_EQ(errors, 3u);
  restoreLogger();
}

TEST(FontSdfCreate, PaddedCellCapacity) {
  // The M "M" ink at 16 px is 10x11; with the default radius 8 its
  // PADDED cell is 26x27. An 8x8 atlas cannot hold it — the capacity
  // guard fires before any bitmap allocation (BudgetExhausted + one
  // structured Error).
  std::vector<std::uint8_t> font = loadFont();
  MemorySink* sink = installCaptureSink();
  SdfGlyphAtlas::Options small;
  small.atlasWidth = 8;
  small.atlasHeight = 8;
  auto r = SdfGlyphAtlas::create(font, small);
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::BudgetExhausted);
  }
  // A 32x32 atlas holds the padded M (26x27) but the 1024^2 default
  // range still overflows it shelf-wise: the packing fills the 32x32
  // and the later glyphs exhaust the atlas (BudgetExhausted either
  // way — the atlas is fixed-size by contract).
  SdfGlyphAtlas::Options tiny = small;
  tiny.atlasWidth = 32;
  tiny.atlasHeight = 32;
  auto r2 = SdfGlyphAtlas::create(font, tiny);
  EXPECT_TRUE(r2.isError());
  if (r2.isError()) {
    EXPECT_EQ(r2.error(), ErrorCode::BudgetExhausted);
  }
  const std::size_t errors = static_cast<std::size_t>(std::count_if(
      sink->entries.begin(), sink->entries.end(),
      [](const MemorySink::Entry& e) {
        return e.severity == laige::log::Severity::Error &&
               e.subsystem == "font" && e.event == "atlas_full";
      }));
  EXPECT_EQ(errors, 2u);
  restoreLogger();
}

TEST(FontSdfCreate, Success) {
  MemorySink* sink = installCaptureSink();
  SdfGlyphAtlas atlas = makeSdfFont(kSdfDefaultRadius);
  EXPECT_TRUE(atlas.valid());
  EXPECT_EQ(atlas.atlasWidth(), laige::render::kFontDefaultAtlasWidth);
  EXPECT_EQ(atlas.atlasHeight(), laige::render::kFontDefaultAtlasHeight);
  EXPECT_EQ(atlas.fontSize(), laige::render::kFontDefaultFontSize);
  EXPECT_EQ(atlas.lineHeight(), atlas.fontSize());  // the pixel-height rule
  EXPECT_EQ(atlas.glyphFirst(), laige::render::kFontDefaultGlyphFirst);
  EXPECT_EQ(atlas.glyphLast(), laige::render::kFontDefaultGlyphLast);
  EXPECT_EQ(atlas.radius(), kSdfDefaultRadius);
  EXPECT_EQ(atlas.atlas().size(),
            static_cast<std::size_t>(atlas.atlasWidth()) *
                static_cast<std::size_t>(atlas.atlasHeight()));
  // One structured Info per successful build (the set-up path).
  const std::size_t infos = static_cast<std::size_t>(std::count_if(
      sink->entries.begin(), sink->entries.end(),
      [](const MemorySink::Entry& e) {
        return e.severity == laige::log::Severity::Info &&
               e.subsystem == "font" && e.event == "sdf_atlas_created";
      }));
  EXPECT_EQ(infos, 1u);
  restoreLogger();
}

TEST(FontSdfCreate, StoppedAndMove) {
  SdfGlyphAtlas stopped;
  EXPECT_FALSE(stopped.valid());
  EXPECT_EQ(stopped.glyph(65), nullptr);
  EXPECT_EQ(stopped.atlas().size(), 0u);
  EXPECT_EQ(stopped.atlasWidth(), 0u);
  EXPECT_EQ(stopped.radius(), 0u);
  // The move (the GlyphAtlas precedent): the target is live, the
  // source stopped.
  SdfGlyphAtlas source = makeSdfFont(kSdfDefaultRadius);
  ASSERT_TRUE(source.valid());
  SdfGlyphAtlas moved = std::move(source);
  EXPECT_FALSE(source.valid());
  EXPECT_EQ(source.glyph(65), nullptr);
  EXPECT_TRUE(moved.valid());
  EXPECT_EQ(moved.atlas().size(),
            static_cast<std::size_t>(moved.atlasWidth()) *
                static_cast<std::size_t>(moved.atlasHeight()));
}

// ---------------------------------------------------------------------------
// FontSdfMetrics — the metrics parity with the bitmap path (the
// layout must be identical: same advance/bearing/ink, the same
// measureText widths) and the padded-cell packing invariants
// ---------------------------------------------------------------------------

TEST(FontSdfMetrics, ParityWithBitmap) {
  const GlyphAtlas bitmap = makeBitmapFont();
  const SdfGlyphAtlas sdf = makeSdfFont(kSdfDefaultRadius);
  EXPECT_EQ(sdf.fontSize(), bitmap.fontSize());
  EXPECT_EQ(sdf.glyphFirst(), bitmap.glyphFirst());
  EXPECT_EQ(sdf.glyphLast(), bitmap.glyphLast());
  const std::uint32_t W = sdf.atlasWidth();
  const std::uint32_t H = sdf.atlasHeight();
  for (std::uint32_t code = bitmap.glyphFirst(); code <= bitmap.glyphLast();
       ++code) {
    const GlyphMetrics* b = bitmap.glyph(code);
    const GlyphMetrics* s = sdf.glyph(code);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(s, nullptr);
    // The M2-TEXT-01 metrics are UNCHANGED (advance, bearing, ink
    // size — the layout contract).
    EXPECT_EQ(s->advance, b->advance) << "code " << code;
    EXPECT_EQ(s->bearingX, b->bearingX) << "code " << code;
    EXPECT_EQ(s->bearingY, b->bearingY) << "code " << code;
    EXPECT_EQ(s->width, b->width) << "code " << code;
    EXPECT_EQ(s->height, b->height) << "code " << code;
    // The padded cell fits the atlas (the packing invariant).
    if (s->width > 0 && s->height > 0) {
      const std::uint32_t cellW =
          static_cast<std::uint32_t>(s->width) + 2u * sdf.radius();
      const std::uint32_t cellH =
          static_cast<std::uint32_t>(s->height) + 2u * sdf.radius();
      EXPECT_LE(s->atlasX + cellW, W) << "code " << code;
      EXPECT_LE(s->atlasY + cellH, H) << "code " << code;
    }
  }
}

TEST(FontSdfMetrics, WidthMatchesBitmap) {
  // The roadmap Verify property: the SDF text width matches the
  // bitmap path within the documented tolerance — 0, exactly: both
  // paths read the identical M2-TEXT-01 metrics.
  const GlyphAtlas bitmap = makeBitmapFont();
  const SdfGlyphAtlas sdf = makeSdfFont(kSdfDefaultRadius);
  auto rc = laige::render::StringTable::create({8, 64});
  ASSERT_TRUE(rc.ok());
  laige::render::StringTable table = std::move(rc).takeValue();
  struct Case {
    const char* text;
    std::uint32_t scale;
  };
  const std::vector<Case> cases = {
      {"Ma", 1},
      {"Hello World", 1},
      {"Hello World", 2},
      {"W", 4},
      {"", 1},
      {"   ", 1},
  };
  for (const Case& c : cases) {
    const laige::render::StringRef ref = internText(table, c.text);
    auto rb = laige::render::measureText(bitmap, table, ref, c.scale);
    auto rs = laige::render::measureText(sdf, table, ref, c.scale);
    ASSERT_TRUE(rb.ok());
    ASSERT_TRUE(rs.ok());
    // takeValue() is rvalue-only (the house Result contract): the
    // valueIfOk read.
    EXPECT_EQ(*rb.valueIfOk(), *rs.valueIfOk())
        << c.text << " @ " << c.scale;
  }
}

// ---------------------------------------------------------------------------
// FontSdfDeterminism — same font bytes + options → identical atlas
// bytes (the roadmap Verify property; presentation-only float,
// ARCH-009, so the scope is in-process / same-build)
// ---------------------------------------------------------------------------

TEST(FontSdfDeterminism, IdenticalBytes) {
  const std::vector<std::uint8_t> b1 = makeSdfBytes();
  const std::vector<std::uint8_t> b2 = makeSdfBytes();
  EXPECT_EQ(b1.size(), b2.size());
  EXPECT_TRUE(std::equal(b1.begin(), b1.end(), b2.begin()));
  const SdfGlyphAtlas a1 = makeSdfFont(kSdfDefaultRadius);
  const SdfGlyphAtlas a2 = makeSdfFont(kSdfDefaultRadius);
  for (std::uint32_t code = a1.glyphFirst(); code <= a1.glyphLast(); ++code) {
    const GlyphMetrics* m1 = a1.glyph(code);
    const GlyphMetrics* m2 = a2.glyph(code);
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    EXPECT_EQ(*m1, *m2) << "code " << code;  // GlyphMetrics is a value
  }
  // The machine-greppable fingerprint (docs/testing.md §4 — no pinned
  // value: the determinism contract is in-process, the hash records
  // the build's output for run-to-run comparison).
  std::printf("font-sdf-determinism font=vera.ttf size=%u glyphs=%u "
              "atlas=%ux%u radius=%u fnv1a=0x%016llx\n",
              static_cast<unsigned>(a1.fontSize()),
              static_cast<unsigned>(a1.glyphLast() - a1.glyphFirst() + 1),
              static_cast<unsigned>(a1.atlasWidth()),
              static_cast<unsigned>(a1.atlasHeight()),
              static_cast<unsigned>(a1.radius()),
              static_cast<unsigned long long>(
                  atlasFnv1a64(b1.data(), b1.size())));
}

TEST(FontSdfDeterminism, RadiusDiverges) {
  const SdfGlyphAtlas a1 = makeSdfFont(kSdfDefaultRadius);
  const SdfGlyphAtlas a2 = makeSdfFont(4);
  // A different radius is a different field (the padded cells are a
  // different size — the packing AND the field differ).
  EXPECT_NE(atlasFnv1a64(a1.atlas().data(), a1.atlas().size()),
            atlasFnv1a64(a2.atlas().data(), a2.atlas().size()));
}

// ---------------------------------------------------------------------------
// FontSdfThreshold — the documented threshold edge behavior: the
// 128 boundary (inside >= 128, outside <= 128), the saturation at the
// cell corners, and the exact field value against a brute-force
// oracle (the distance transform's correctness on the test font)
// ---------------------------------------------------------------------------

TEST(FontSdfThreshold, InsideOutsideBoundary) {
  const GlyphAtlas bitmap = makeBitmapFont();
  const SdfGlyphAtlas sdf = makeSdfFont(kSdfDefaultRadius);
  const std::uint32_t W = sdf.atlasWidth();
  const std::uint32_t r = sdf.radius();
  std::size_t checked = 0;
  for (std::uint32_t code = bitmap.glyphFirst(); code <= bitmap.glyphLast();
       ++code) {
    const GlyphMetrics& b = *bitmap.glyph(code);
    const GlyphMetrics& s = *sdf.glyph(code);
    if (b.width <= 0 || b.height <= 0) continue;  // no cell, no field
    for (std::int32_t ty = 0; ty < b.height; ++ty) {
      for (std::int32_t tx = 0; tx < b.width; ++tx) {
        // The ink texel (tx, ty) of the bitmap cell maps to the
        // same texel at offset (r, r) of the SDF padded cell (the
        // packing invariant — font_sdf.h model section).
        const std::uint8_t alpha = bitmap.atlas()[
            (b.atlasY + static_cast<std::uint32_t>(ty)) * W +
            b.atlasX + static_cast<std::uint32_t>(tx)];
        const std::uint8_t v = sdf.atlas()[
            (s.atlasY + r + static_cast<std::uint32_t>(ty)) * W +
            s.atlasX + r + static_cast<std::uint32_t>(tx)];
        // The documented boundary (font_sdf.h field section): the
        // 0.5-coverage cut at 127 -> inside (alpha > 127) has byte
        // >= 128, outside (alpha <= 127) has byte <= 128.
        if (alpha > 127) {
          EXPECT_GE(v, 128) << "code " << code << " texel " << tx << ","
                            << ty;
        } else {
          EXPECT_LE(v, 128) << "code " << code << " texel " << tx << ","
                            << ty;
        }
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 0u);
}

TEST(FontSdfThreshold, CornerSaturation) {
  // The padded cell's corners are sqrt(2) * r from the nearest ink
  // pixel (the ink sits at offset (r, r) of the cell) — beyond the
  // radius: the field clamps to the 0 byte value exactly (the
  // documented saturation, font_sdf.h field section).
  const SdfGlyphAtlas sdf = makeSdfFont(kSdfDefaultRadius);
  for (std::uint32_t code = sdf.glyphFirst(); code <= sdf.glyphLast();
       ++code) {
    const GlyphMetrics* s = sdf.glyph(code);
    if (s == nullptr || s->width <= 0 || s->height <= 0) continue;
    const std::uint32_t W = sdf.atlasWidth();
    const std::uint32_t cellW =
        static_cast<std::uint32_t>(s->width) + 2u * sdf.radius();
    const std::uint32_t cellH =
        static_cast<std::uint32_t>(s->height) + 2u * sdf.radius();
    const std::uint8_t tl =
        sdf.atlas()[s->atlasY * W + s->atlasX];
    const std::uint8_t br = sdf.atlas()[
        (s->atlasY + cellH - 1) * W + (s->atlasX + cellW - 1)];
    EXPECT_EQ(tl, 0u) << "code " << code;
    EXPECT_EQ(br, 0u) << "code " << code;
  }
}

TEST(FontSdfThreshold, ExactValuesAgainstOracle) {
  // The distance transform's correctness: the engine's byte for a
  // deep-inside pixel of "a" and a deep-outside pixel matches a
  // brute-force exact oracle (the min Euclidean distance to the
  // opposite set, the 8SDD algorithm's contract — Felice & Nienhuis
  // 2014). The oracle runs in double over the small cell; the engine
  // in float32 — the tolerance is one quantization byte (the float
  // rounding of the distance can only matter at an lround half-
  // boundary, the documented worst case).
  const GlyphAtlas bitmap = makeBitmapFont();
  const SdfGlyphAtlas sdf = makeSdfFont(kSdfDefaultRadius);
  const std::uint32_t W = sdf.atlasWidth();
  const std::uint32_t r = sdf.radius();
  const GlyphMetrics& b = *bitmap.glyph('a');
  const GlyphMetrics& s = *sdf.glyph('a');
  ASSERT_GT(b.width, 0);
  ASSERT_GT(b.height, 0);
  // The SDF cell's origin (the SDF packing differs from the bitmap
  // packing — the cell origins diverge after the first glyph).
  ASSERT_EQ(s.width, b.width);
  ASSERT_EQ(s.height, b.height);
  // The padded cell of "a" in both atlases (the bitmap cell at the
  // bitmap origin, the SDF cell at the SDF origin + (r, r)).
  const std::uint32_t cw = static_cast<std::uint32_t>(b.width) + 2u * r;
  const std::uint32_t ch = static_cast<std::uint32_t>(b.height) + 2u * r;
  // The 0.5-coverage grid of the SDF cell (the oracle's inside set).
  std::vector<std::uint8_t> inside(static_cast<std::size_t>(cw) * ch, 0);
  for (std::uint32_t ty = 0; ty < static_cast<std::uint32_t>(b.height); ++ty) {
    for (std::uint32_t tx = 0; tx < static_cast<std::uint32_t>(b.width); ++tx) {
      const std::uint8_t alpha = bitmap.atlas()[
          (b.atlasY + ty) * W + b.atlasX + tx];
      if (alpha > 127) {
        inside[(r + ty) * cw + (r + tx)] = 1;
      }
    }
  }
  // The brute-force min distance (double) from (px, py) to the pixels
  // of `set`.
  auto minDist = [&](std::uint32_t px, std::uint32_t py, bool toInside) {
    double best = 1.0e30;
    for (std::uint32_t oy = 0; oy < ch; ++oy) {
      for (std::uint32_t ox = 0; ox < cw; ++ox) {
        const bool onSet = toInside ? inside[oy * cw + ox] != 0
                                    : inside[oy * cw + ox] == 0;
        if (!onSet) continue;
        const double dx = static_cast<double>(static_cast<int32_t>(px) -
                                              static_cast<int32_t>(ox));
        const double dy = static_cast<double>(static_cast<int32_t>(py) -
                                              static_cast<int32_t>(oy));
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
      }
    }
    return best;
  };
  // A fully-covered inside pixel (alpha 255 — the oracle's distance to
  // the outside set is then >= 1 px and the byte is >= 128 + 127/r;
  // the oracle below verifies the exact value either way).
  std::uint32_t ipx = 0;
  std::uint32_t ipy = 0;
  bool foundInside = false;
  for (std::uint32_t ty = 0; ty < static_cast<std::uint32_t>(b.height); ++ty) {
    for (std::uint32_t tx = 0; tx < static_cast<std::uint32_t>(b.width); ++tx) {
      const std::uint8_t alpha = bitmap.atlas()[
          (b.atlasY + ty) * W + b.atlasX + tx];
      if (alpha == 255) {
        ipx = r + tx;
        ipy = r + ty;
        foundInside = true;
        break;
      }
    }
    if (foundInside) break;
  }
  ASSERT_TRUE(foundInside) << "the test font's \"a\" has no fully-covered "
                              "inside pixel (the asset's contract)";
  const double dIn = minDist(ipx, ipy, false);  // to the outside set
  const int expectedIn =
      static_cast<int>(std::max(0.0, std::min(255.0,
        128.0 + std::round(127.0 * dIn / static_cast<double>(r)))));
  // The engine's byte for the cell pixel (cell coords -> atlas coords
  // through the SDF cell's origin).
  const std::uint8_t actualIn =
      sdf.atlas()[static_cast<std::size_t>(s.atlasY + ipy) * W +
                  s.atlasX + ipx];
  // One quantization byte of tolerance (the float32 distance rounding
  // can only matter at an lround half-boundary).
  EXPECT_GE(static_cast<int>(actualIn), expectedIn - 1);
  EXPECT_LE(static_cast<int>(actualIn), expectedIn + 1);
  // A deep-outside pixel (the padded cell's corner — the saturated
  // region; the oracle agrees the byte is 1).
  const double dOut = minDist(0, 0, true);  // to the inside set
  const int expectedOut =
      static_cast<int>(std::max(0.0, std::min(255.0,
        128.0 - std::round(127.0 * dOut / static_cast<double>(r)))));
  const std::uint8_t actualOut =
      sdf.atlas()[s.atlasY * W + s.atlasX];
  EXPECT_GE(static_cast<int>(actualOut), expectedOut - 1);
  EXPECT_LE(static_cast<int>(actualOut), expectedOut + 1);
  EXPECT_EQ(actualOut, 0u);  // the corner saturates (the boundary test)
}

// ---------------------------------------------------------------------------
// FontSdfFallback — the documented fallback (the M2-TEXT-01 contract,
// verbatim) and the stopped state
// ---------------------------------------------------------------------------

TEST(FontSdfFallback, OutOfRange) {
  const SdfGlyphAtlas sdf = makeSdfFont(kSdfDefaultRadius);
  // An out-of-range code falls back to the U+0020 slot (the default
  // range includes it).
  const GlyphMetrics* fallback = sdf.glyph(0x0100);
  ASSERT_NE(fallback, nullptr);
  const GlyphMetrics* space = sdf.glyph(0x20);
  ASSERT_NE(space, nullptr);
  EXPECT_EQ(fallback->advance, space->advance);
  // A range WITHOUT the fallback code reads the zero metric.
  std::vector<std::uint8_t> font = loadFont();
  SdfGlyphAtlas::Options noSpace;
  noSpace.glyphFirst = 65;  // 'A'..'Z' — no U+0020
  noSpace.glyphLast = 90;
  auto r = SdfGlyphAtlas::create(font, noSpace);
  ASSERT_TRUE(r.ok());
  const SdfGlyphAtlas narrow = std::move(r).takeValue();
  const GlyphMetrics* empty = narrow.glyph(0x20);
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(empty->advance, 0);
  EXPECT_EQ(empty->width, 0);
  // An in-range code of the narrow range is live.
  const GlyphMetrics* a = narrow.glyph('A');
  ASSERT_NE(a, nullptr);
  EXPECT_GT(a->advance, 0);
}

TEST(FontSdfFallback, Stopped) {
  SdfGlyphAtlas stopped;
  EXPECT_EQ(stopped.glyph(65), nullptr);
}

// ---------------------------------------------------------------------------
// FontSdfExpand — the white-on-alpha upload bytes (the bindSdfAtlas
// input)
// ---------------------------------------------------------------------------

TEST(FontSdfExpand, Rgba8) {
  const SdfGlyphAtlas atlas = makeSdfFont(kSdfDefaultRadius);
  const std::size_t need =
      static_cast<std::size_t>(atlas.atlasWidth()) *
      static_cast<std::size_t>(atlas.atlasHeight()) * 4u;
  std::vector<std::uint8_t> rgba(need, 0xEE);
  ASSERT_TRUE(laige::render::expandSdfGlyphAtlasRgba8(atlas,
                                                      rgba).ok());
  const std::span<const std::uint8_t> field = atlas.atlas();
  for (std::size_t i = 0; i < need / 4; ++i) {
    EXPECT_EQ(rgba[i * 4 + 0], 255u);
    EXPECT_EQ(rgba[i * 4 + 1], 255u);
    EXPECT_EQ(rgba[i * 4 + 2], 255u);
    EXPECT_EQ(rgba[i * 4 + 3], field[i]);
  }
  // The wrong-size span: InvalidArgument, nothing written.
  std::vector<std::uint8_t> shortRgba(need - 4, 0xEE);
  EXPECT_FALSE(laige::render::expandSdfGlyphAtlasRgba8(atlas, shortRgba)
                   .ok());
  // The stopped atlas: InvalidArgument.
  SdfGlyphAtlas stopped;
  EXPECT_FALSE(
      laige::render::expandSdfGlyphAtlasRgba8(stopped, rgba).ok());
}

// ---------------------------------------------------------------------------
// FontSdfZeroAlloc — the per-lookup path allocates nothing (PERF-003:
// the render read path is a pure table lookup)
// ---------------------------------------------------------------------------

TEST(FontSdfZeroAlloc, LookupZeroAlloc) {
  const SdfGlyphAtlas atlas = makeSdfFont(kSdfDefaultRadius);
  laige::allocWatchArm();
  std::int64_t sum = 0;
  for (std::uint32_t i = 0; i < 1000; ++i) {
    const GlyphMetrics* inRange = atlas.glyph(
        atlas.glyphFirst() + i % (atlas.glyphLast() - atlas.glyphFirst() + 1));
    const GlyphMetrics* fallback = atlas.glyph(0x0100 + i);
    if (inRange != nullptr) {
      sum += inRange->advance;
    }
    if (fallback != nullptr) {
      sum += fallback->advance;
    }
  }
  const laige::AllocWatchReading reading = laige::allocWatchRead();
  EXPECT_EQ(reading.allocs, 0u);
  EXPECT_GT(sum, 0);  // the lookups actually happened
}

// ---------------------------------------------------------------------------
// FontSdfSmoke — the offscreen GL render (the M2-SPRITE-02 submit
// stage with the SDF field uploaded through `bindSdfAtlas`;
// GTEST_SKIPs where no usable OpenGL 3.3 environment — the documented
// environment contract)
// ---------------------------------------------------------------------------

namespace {

constexpr std::int32_t kSmokeWidth = 64;
constexpr std::int32_t kSmokeHeight = 64;

// The UI pass's identity screen matrix (screen px, y down -> NDC):
// x_ndc = 2x/w - 1, y_ndc = 1 - 2y/h (the M2-UI-02 plane — the
// text_items_tests smokeMatrix pattern; the GLM scalar constructor
// is the identity: diagonal s, off-diagonal 0).
laige::render::Mat4 smokeMatrix() {
  // Column-major (the house Mat4 convention): the linear part in
  // columns 0..1, the translation in column 3.
  laige::render::Mat4 m(1.0f);
  m[0].x = 2.0f / static_cast<float>(kSmokeWidth);
  m[1].y = -2.0f / static_cast<float>(kSmokeHeight);
  m[3].x = -1.0f;
  m[3].y = 1.0f;
  return m;
}

}  // namespace

TEST(FontSdfSmoke, OffscreenSdfFrame) {
  auto ctx = laige::render::GlContext::createHeadless(kSmokeWidth,
                                                      kSmokeHeight);
  if (!ctx.ok()) {
    GTEST_SKIP() << "No usable OpenGL 3.3 environment: "
                 << laige::errorText(ctx.error())
                 << " (the documented environment contract — the P0 CI "
                    "runners always have one)";
  }
  laige::render::GlContext gl = std::move(ctx).takeValue();
  const SdfGlyphAtlas sdf = makeSdfFont(kSdfDefaultRadius);
  const GlyphMetrics& mM = *sdf.glyph('M');
  ASSERT_EQ(10, mM.width);
  ASSERT_EQ(11, mM.height);
  ASSERT_EQ(1, mM.bearingX);
  ASSERT_EQ(11, mM.bearingY);
  // The upload bytes (the setup path — one call per font per scene).
  std::vector<std::uint8_t> rgba;
  rgba.resize(static_cast<std::size_t>(sdf.atlasWidth()) *
                  static_cast<std::size_t>(sdf.atlasHeight()) * 4u);
  ASSERT_TRUE(laige::render::expandSdfGlyphAtlasRgba8(sdf, rgba).ok());
  auto rc = laige::render::SpriteRenderer::create(
      gl, laige::render::SpriteRenderer::Options{16, 8});
  ASSERT_TRUE(rc.ok());
  laige::render::SpriteRenderer renderer = std::move(rc).takeValue();
  // The SDF bind (threshold 0.5 = the 0.5-coverage contour — the
  // documented default).
  ASSERT_TRUE(renderer
                  .bindSdfAtlas(3, sdf.atlasWidth(), sdf.atlasHeight(), rgba,
                                0.5f)
                  .ok());
  // The invalid threshold domain: InvalidArgument, no state change.
  EXPECT_FALSE(renderer
                   .bindSdfAtlas(3, sdf.atlasWidth(), sdf.atlasHeight(), rgba,
                                 1.5f)
                   .ok());
  auto tc = laige::render::StringTable::create({8, 64});
  ASSERT_TRUE(tc.ok());
  laige::render::StringTable table = std::move(tc).takeValue();
  const laige::render::StringRef ref = internText(table, "Ma");
  auto bc = laige::render::SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  laige::render::SpriteBatcher batcher = std::move(bc).takeValue();
  laige::render::TextItem item;
  item.text = ref;
  item.mode = laige::render::FontRenderMode::Sdf;
  item.sdfFont = &sdf;
  item.x = 10.0f;
  item.y = 20.0f;  // the first line's baseline (screen y down)
  item.atlasId = 3;
  ASSERT_TRUE(laige::render::declareText(batcher, table, item).ok());
  ASSERT_TRUE(batcher.build().ok());
  // The clear BEFORE the first draw (the offscreen frame protocol —
  // the M2-SPRITE-04 TSan lesson: a query-enabled submit as the first
  // FBO op of the frame raced the Mesa teardown; the query is off
  // here, but the clear is the documented protocol either way).
  ASSERT_TRUE(gl.clear(0.0f, 0.0f, 0.0f, 1.0f).ok());
  ASSERT_TRUE(renderer.submit(batcher, smokeMatrix()).ok());
  // One (atlas, material, blend) group: one instanced draw, 2
  // instances (RENDER-001).
  const laige::render::SpriteDrawStats stats = renderer.frameStats();
  EXPECT_EQ(1u, stats.drawCalls);
  EXPECT_EQ(2u, stats.instances);
  // The pixel checks (screen y down -> GL y = H - 1 - sy; the clear
  // alpha is 1.0 -> the composite alpha is exactly 255 over the black
  // clear; the SDF branch's alpha feeds the engine's
  // SRC_ALPHA/ONE_MINUS_SRC_ALPHA blend on all four channels).
  auto readScreen = [&](std::int32_t sx, std::int32_t sy) {
    std::uint8_t rgba4[4]{};
    auto r = gl.readPixel(sx, kSmokeHeight - 1 - sy, rgba4);
    if (!r.ok()) return std::array<std::uint8_t, 4>{};
    return std::array<std::uint8_t, 4>{rgba4[0], rgba4[1], rgba4[2],
                                       rgba4[3]};
  };
  // The SDF quad covers the M's PADDED cell (the header's glyph-quad
  // section): ink left 10 + bearingX = 11, top 20 - bearingY = 9; the
  // padded cell left 11 - r = 3, top 9 - r = 1.
  const std::int32_t r = static_cast<std::int32_t>(sdf.radius());
  const int inkML = 10 + mM.bearingX;
  const int inkMT = 20 - mM.bearingY;
  const int padML = inkML - r;
  const int padMT = inkMT - r;
  // 1. The background (outside every quad): the clear shows through.
  {
    const std::array<std::uint8_t, 4> p = readScreen(1, 55);
    EXPECT_EQ(0u, p[0]);
    EXPECT_EQ(0u, p[1]);
    EXPECT_EQ(0u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // 2. A deep-inside pixel of the M: the field is at least 2 distance-
  // px inside the glyph (byte >= 128 + 2*127/r, checked against the
  // engine's own field — self-consistent) — the smooth step is
  // exactly 1 -> (255, 255, 255, 255) exactly (the documented
  // threshold edge behavior, font_sdf.md).
  {
    const std::span<const std::uint8_t> field = sdf.atlas();
    const std::uint32_t deepIn = static_cast<std::uint32_t>(
        128 + 2 * static_cast<int>(127.0 / static_cast<double>(sdf.radius())));
    bool found = false;
    int tx = 0;
    int ty = 0;
    for (int y = 0; y < mM.height && !found; ++y) {
      for (int x = 0; x < mM.width; ++x) {
        const std::uint8_t v = field[(mM.atlasY + sdf.radius() + y) *
                                         static_cast<std::uint32_t>(1024) +
                                     mM.atlasX + sdf.radius() + x];
        if (v >= deepIn) {
          tx = x;
          ty = y;
          found = true;
          break;
        }
      }
    }
    ASSERT_TRUE(found) << "the test font's \"M\" has no deep-inside texel";
    const std::array<std::uint8_t, 4> p =
        readScreen(inkML + tx, inkMT + ty);
    EXPECT_EQ(255u, p[0]);
    EXPECT_EQ(255u, p[1]);
    EXPECT_EQ(255u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // 3. A deep-outside pixel of the PADDED CELL (the field's corner —
  // saturated to the 1 byte value, more than one distance-px outside):
  // the smooth step is exactly 0 -> the clear shows through exactly.
  {
    const std::array<std::uint8_t, 4> p = readScreen(padML, padMT);
    EXPECT_EQ(0u, p[0]);
    EXPECT_EQ(0u, p[1]);
    EXPECT_EQ(0u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // 4. The screen-space anti-aliasing (the P1 motivation): the field's
  // edge is anti-aliased with a band one screen pixel wide at 1x —
  // scanning across the M's left stem at mid-height, at least one
  // pixel is strictly between clear and opaque (0 < rgb < 255).
  {
    const int midY = inkMT + mM.height / 2;
    bool sawEdge = false;
    for (int x = inkML - 2; x <= inkML + 2; ++x) {
      const std::array<std::uint8_t, 4> p = readScreen(x, midY);
      if (p[0] > 0 && p[0] < 255 && p[1] == p[0] && p[2] == p[0]) {
        sawEdge = true;
        break;
      }
    }
    EXPECT_TRUE(sawEdge) << "no anti-aliased edge pixel across the M's left "
                           "stem (the SDF band is missing)";
  }
  // The context outlives the renderer (the ownership contract).
}
