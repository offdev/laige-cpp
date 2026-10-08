// laige-render bitmap font atlas tests (M2-TEXT-01): the configured
// font + glyph range rasterized ONCE at scene set-up into a fixed-size
// 8-bit alpha atlas + per-glyph metrics (advance, bearing, line height)
// — in laige/render/font.h.
//
// Pure engine data (no GL context, no GL environment needed): every
// suite runs in every local tree and in CI. The metric goldens are
// MEASURED against the committed test font (tests/laige-render/assets/
// vera.ttf — Bitstream Vera Sans, OFL 1.1, see the assets/ README) at
// fontSize 16: the font is a committed in-tree asset (P0 runners differ
// in their font packages, NFR-8.8 — no system font is trusted), so the
// values are stable for the life of the asset. The determinism contract
// is IN-PROCESS (same font bytes + options → identical atlas bytes;
// presentation-only float, ARCH-009 — the cross-platform bit-identity
// scope of the M2-PROJ-01/ISO-03 transforms), printed as the
// machine-greppable `font-atlas-determinism` line (docs/testing.md §4).
//
// No budget gate: the step's roadmap scope has no standalone
// budgets.json entry (the atlas is built once at scene set-up — the
// per-frame text cost lands with M2-TEXT-02, part of the composite 50k
// render-CPU budget, M2-PERF-01).
//
// The font bytes come from the LAIGE_TEST_FONT_PATH env var (set by
// the `font` and unfiltered `laige-render_tests` CTest entries — see
// tests/laige-render/CMakeLists.txt).

#include "laige/render/font.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/logging.h"

namespace {

using laige::render::GlyphAtlas;
using laige::render::GlyphMetrics;
using laige::render::kFontEmptyGlyphMetrics;
using laige::render::kFontFallbackCode;
using laige::ErrorCode;

// ---------------------------------------------------------------------------
// The committed test font (tests/laige-render/assets/vera.ttf — the
// LAIGE_TEST_FONT_PATH env var, CTest ENVIRONMENT).
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> loadFont() {
  const char* path = std::getenv("LAIGE_TEST_FONT_PATH");
  if (path == nullptr) {
    ADD_FAILURE() << "LAIGE_TEST_FONT_PATH is not set (CTest ENVIRONMENT "
                     "contract — see tests/laige-render/CMakeLists.txt)";
    return {};
  }
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    ADD_FAILURE() << "Cannot open the test font: " << path;
    return {};
  }
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

GlyphAtlas makeDefaultAtlas() {
  std::vector<std::uint8_t> font = loadFont();
  auto r = GlyphAtlas::create(font, GlyphAtlas::Options{});
  if (!r.ok()) {
    std::fprintf(stderr, "makeDefaultAtlas: create failed\n");
    std::abort();
  }
  return std::move(r).takeValue();
}

std::vector<std::uint8_t> makeDefaultBytes() {
  std::vector<std::uint8_t> font = loadFont();
  auto r = GlyphAtlas::create(font, GlyphAtlas::Options{});
  if (!r.ok()) {
    std::fprintf(stderr, "makeDefaultBytes: create failed\n");
    std::abort();
  }
  const GlyphAtlas atlas = std::move(r).takeValue();
  return std::vector<std::uint8_t>(atlas.atlas().begin(), atlas.atlas().end());
}

// FNV-1a 64 over a byte span (the machine-greppable fingerprint
// convention, docs/testing.md §4 — the particlesFnv1a64 pattern).
std::uint64_t atlasFnv1a64(const std::uint8_t* bytes, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis (FNV-1a spec)
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= 0x10000001b3ull;  // FNV prime (FNV-1a spec)
  }
  return h;
}

// ---------------------------------------------------------------------------
// Log capture (the tilemap_tests MemorySink pattern)
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

}  // namespace

// ---------------------------------------------------------------------------
// FontCreate — the create validation matrix (first failure wins), the
// untrusted-font boundary (SCALE-004), the stopped state, the capacity
// guard, and the move semantics
// ---------------------------------------------------------------------------

TEST(FontCreate, OptionsValidation) {
  std::vector<std::uint8_t> font = loadFont();
  MemorySink* sink = installCaptureSink();
  struct Case {
    GlyphAtlas::Options options;
    const char* name;
  };
  const std::vector<Case> cases = {
      {GlyphAtlas::Options{.atlasWidth = 0}, "atlasWidth zero"},
      {GlyphAtlas::Options{.atlasWidth = laige::render::kFontMaxAtlasDimension + 1},
       "atlasWidth above the domain"},
      {GlyphAtlas::Options{.atlasHeight = 0}, "atlasHeight zero"},
      {GlyphAtlas::Options{.atlasHeight = laige::render::kFontMaxAtlasDimension + 1},
       "atlasHeight above the domain"},
      {GlyphAtlas::Options{.fontSize = 0}, "fontSize zero"},
      {GlyphAtlas::Options{.fontSize = laige::render::kFontMaxFontSize + 1},
       "fontSize above the domain"},
      {GlyphAtlas::Options{.glyphFirst = 100, .glyphLast = 99},
       "glyphFirst above glyphLast"},
      {GlyphAtlas::Options{.glyphLast = laige::render::kFontMaxGlyphCode + 1},
       "glyphLast above the Unicode ceiling"},
      {GlyphAtlas::Options{.glyphFirst = 0, .glyphLast = laige::render::kFontMaxGlyphCount},
       "glyph range above the size bound"},
  };
  for (const Case& c : cases) {
    auto r = GlyphAtlas::create(font, c.options);
    EXPECT_TRUE(r.isError()) << c.name;
    if (r.isError()) {
      EXPECT_EQ(r.error(), ErrorCode::InvalidArgument) << c.name;
    }
  }
  EXPECT_EQ(sink->entries.size(), 0u);  // no log — the create precedent
  restoreLogger();
}

TEST(FontCreate, EmptyOrTruncatedFont) {
  MemorySink* sink = installCaptureSink();
  const std::vector<std::uint8_t> empty;
  auto r = GlyphAtlas::create(empty, GlyphAtlas::Options{});
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::MalformedInput);
  }
  // A buffer shorter than the header + table directory guard (12
  // bytes) — rejected before any stb read (SCALE-004).
  const std::vector<std::uint8_t> shortBuf(10, 0xAB);
  r = GlyphAtlas::create(shortBuf, GlyphAtlas::Options{});
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::MalformedInput);
  }
  // A valid font cut inside the table directory: the guard rejects it
  // (the directory is longer than the buffer) — never read past.
  std::vector<std::uint8_t> font = loadFont();
  auto rCut = GlyphAtlas::create(
      std::span<const std::uint8_t>(font.data(), 16),
      GlyphAtlas::Options{});
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

// Patches the hhea table's ascent/descent (big-endian i16 at offsets
// 4/6 within the table — the table the pinned stb's
// stbtt_ScaleForPixelHeight reads) to make the font's vertical metrics
// degenerate (the SCALE-004 guard's test input).
bool patchHheaMetrics(std::vector<std::uint8_t>& font, std::int16_t ascent,
                      std::int16_t descent) {
  if (font.size() < 12) {
    return false;
  }
  const std::uint32_t numTables =
      (static_cast<std::uint32_t>(font[4]) << 8) |
      static_cast<std::uint32_t>(font[5]);
  for (std::uint32_t i = 0; i < numTables; ++i) {
    const std::size_t e = 12 + 16 * i;
    if (e + 16 > font.size()) {
      return false;
    }
    if (font[e] == 'h' && font[e + 1] == 'h' && font[e + 2] == 'e' &&
        font[e + 3] == 'a') {
      const std::uint32_t off =
          (static_cast<std::uint32_t>(font[e + 8]) << 24) |
          (static_cast<std::uint32_t>(font[e + 9]) << 16) |
          (static_cast<std::uint32_t>(font[e + 10]) << 8) |
          static_cast<std::uint32_t>(font[e + 11]);
      if (off + 8 > font.size()) {
        return false;
      }
      const std::uint16_t a = static_cast<std::uint16_t>(ascent);
      font[off + 4] = static_cast<std::uint8_t>(a >> 8);
      font[off + 5] = static_cast<std::uint8_t>(a & 0xFF);
      const std::uint16_t d = static_cast<std::uint16_t>(descent);
      font[off + 6] = static_cast<std::uint8_t>(d >> 8);
      font[off + 7] = static_cast<std::uint8_t>(d & 0xFF);
      return true;
    }
  }
  return false;
}

TEST(FontCreate, DegenerateVerticalMetrics) {
  MemorySink* sink = installCaptureSink();
  std::vector<std::uint8_t> font = loadFont();
  // ascent == descent → the pixel-height scale is inf → MalformedInput.
  ASSERT_TRUE(patchHheaMetrics(font, 1000, 1000));
  auto r = GlyphAtlas::create(font, GlyphAtlas::Options{});
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::MalformedInput);
  }
  // descent > ascent → the scale is negative → MalformedInput.
  ASSERT_TRUE(patchHheaMetrics(font, 1000, 2000));
  r = GlyphAtlas::create(font, GlyphAtlas::Options{});
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::MalformedInput);
  }
  const std::size_t errors = static_cast<std::size_t>(std::count_if(
      sink->entries.begin(), sink->entries.end(),
      [](const MemorySink::Entry& e) {
        return e.severity == laige::log::Severity::Error &&
               e.subsystem == "font" && e.event == "font_invalid";
      }));
  EXPECT_EQ(errors, 2u);
  restoreLogger();
}

TEST(FontCreate, NotAFont) {
  MemorySink* sink = installCaptureSink();
  const std::vector<std::uint8_t> garbage(64, 0xFF);
  auto r = GlyphAtlas::create(garbage, GlyphAtlas::Options{});
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::MalformedInput);
  }
  EXPECT_EQ(sink->entries.size(), 1u);
  if (!sink->entries.empty()) {
    EXPECT_EQ(sink->entries.front().subsystem, "font");
    EXPECT_EQ(sink->entries.front().event, "font_invalid");
    EXPECT_EQ(sink->entries.front().severity, laige::log::Severity::Error);
  }
  restoreLogger();
}

TEST(FontCreate, StoppedState) {
  GlyphAtlas atlas;
  EXPECT_FALSE(atlas.valid());
  EXPECT_EQ(atlas.glyph('A'), nullptr);
  EXPECT_TRUE(atlas.atlas().empty());
  EXPECT_EQ(atlas.atlasWidth(), 0u);
  EXPECT_EQ(atlas.atlasHeight(), 0u);
  EXPECT_EQ(atlas.fontSize(), 0u);
  EXPECT_EQ(atlas.lineHeight(), 0u);
  EXPECT_EQ(atlas.glyphFirst(), 0u);
  EXPECT_EQ(atlas.glyphLast(), 0u);
}

TEST(FontCreate, CapacityFull) {
  std::vector<std::uint8_t> font = loadFont();
  MemorySink* sink = installCaptureSink();
  // 8 x 8 cannot hold a 16 px glyph (its ink is taller than the atlas).
  GlyphAtlas::Options small;
  small.atlasWidth = 8;
  small.atlasHeight = 8;
  auto r = GlyphAtlas::create(font, small);
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::BudgetExhausted);
  }
  const std::size_t errors = static_cast<std::size_t>(std::count_if(
      sink->entries.begin(), sink->entries.end(),
      [](const MemorySink::Entry& e) {
        return e.severity == laige::log::Severity::Error &&
               e.subsystem == "font" && e.event == "atlas_full";
      }));
  EXPECT_EQ(errors, 1u);
  restoreLogger();
}

TEST(FontCreate, SuccessIntrospection) {
  MemorySink* sink = installCaptureSink();
  const GlyphAtlas atlas = makeDefaultAtlas();
  EXPECT_TRUE(atlas.valid());
  EXPECT_EQ(atlas.atlasWidth(), laige::render::kFontDefaultAtlasWidth);
  EXPECT_EQ(atlas.atlasHeight(), laige::render::kFontDefaultAtlasHeight);
  EXPECT_EQ(atlas.fontSize(), laige::render::kFontDefaultFontSize);
  EXPECT_EQ(atlas.glyphFirst(), laige::render::kFontDefaultGlyphFirst);
  EXPECT_EQ(atlas.glyphLast(), laige::render::kFontDefaultGlyphLast);
  EXPECT_EQ(atlas.atlas().size(),
            static_cast<std::size_t>(laige::render::kFontDefaultAtlasWidth) *
                static_cast<std::size_t>(laige::render::kFontDefaultAtlasHeight));
  // Exactly one Info on the set-up path (LOG-002; the lookup path logs
  // nothing).
  const std::size_t infos = static_cast<std::size_t>(std::count_if(
      sink->entries.begin(), sink->entries.end(),
      [](const MemorySink::Entry& e) {
        return e.severity == laige::log::Severity::Info &&
               e.subsystem == "font" && e.event == "atlas_created";
      }));
  EXPECT_EQ(infos, 1u);
  restoreLogger();
}

TEST(FontCreate, MoveSemantics) {
  auto r = GlyphAtlas::create(loadFont(), GlyphAtlas::Options{});
  ASSERT_TRUE(r.ok());
  GlyphAtlas live = std::move(r).takeValue();
  EXPECT_TRUE(live.valid());
  GlyphAtlas stopped;
  stopped = std::move(live);
  EXPECT_TRUE(stopped.valid());
  EXPECT_FALSE(live.valid());  // moved-from is the stopped state
  EXPECT_EQ(live.glyph('A'), nullptr);
  EXPECT_NE(stopped.glyph('A'), nullptr);  // the moved-into atlas is intact
  GlyphAtlas third = std::move(stopped);
  EXPECT_TRUE(third.valid());
  EXPECT_FALSE(stopped.valid());
  EXPECT_EQ(stopped.glyph('A'), nullptr);
  EXPECT_NE(third.glyph('A'), nullptr);
}

// ---------------------------------------------------------------------------
// FontMetrics — the documented units (px at 1x) pinned against the
// committed test font (Vera Sans, fontSize 16)
// ---------------------------------------------------------------------------

TEST(FontMetrics, AsciiGoldens) {
  const GlyphAtlas atlas = makeDefaultAtlas();
  // Space (U+0020): no ink, a positive advance (the fallback glyph).
  const GlyphMetrics* space = atlas.glyph(' ');
  ASSERT_NE(space, nullptr);
  EXPECT_EQ(space->advance, 4);
  EXPECT_EQ(space->width, 0);
  EXPECT_EQ(space->height, 0);
  EXPECT_EQ(space->bearingX, 0);
  EXPECT_EQ(space->bearingY, 0);
  // 'M' (U+004D): a wide uppercase, full height of the caps line.
  const GlyphMetrics* m = atlas.glyph('M');
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->advance, 12);
  EXPECT_EQ(m->width, 10);
  EXPECT_EQ(m->height, 11);
  EXPECT_EQ(m->bearingX, 1);
  EXPECT_EQ(m->bearingY, 11);
  // 'a' (U+0061): a lowercase with a left overhang-free box.
  const GlyphMetrics* a = atlas.glyph('a');
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->advance, 8);
  EXPECT_EQ(a->width, 8);
  EXPECT_EQ(a->height, 9);
  EXPECT_EQ(a->bearingX, 0);
  EXPECT_EQ(a->bearingY, 8);
  // 'W' (U+0057): the widest Latin-1 uppercase.
  const GlyphMetrics* w = atlas.glyph('W');
  ASSERT_NE(w, nullptr);
  EXPECT_EQ(w->advance, 14);
  EXPECT_EQ(w->width, 14);
  EXPECT_EQ(w->height, 11);
  EXPECT_EQ(w->bearingX, 0);
  EXPECT_EQ(w->bearingY, 11);
  // '.' (U+002E): a small ink with a left bearing (the overhang-free
  // dot sits one texel right of the pen).
  const GlyphMetrics* dot = atlas.glyph('.');
  ASSERT_NE(dot, nullptr);
  EXPECT_EQ(dot->advance, 4);
  EXPECT_EQ(dot->width, 2);
  EXPECT_EQ(dot->height, 2);
  EXPECT_EQ(dot->bearingX, 1);
  EXPECT_EQ(dot->bearingY, 2);
}

TEST(FontMetrics, DescenderAndAdvance) {
  const GlyphAtlas atlas = makeDefaultAtlas();
  // 'g' (U+0067): a descender — the ink bottom lies BELOW the baseline,
  // i.e. height > bearingY (the documented relation), while the top
  // still sits above it.
  const GlyphMetrics* g = atlas.glyph('g');
  ASSERT_NE(g, nullptr);
  EXPECT_GT(g->height, g->bearingY);
  EXPECT_GT(g->bearingY, 0);
  // Every A..z Latin-1 glyph of the test font advances the pen
  // (positive advance — the documented metric domain for this set).
  for (std::uint32_t code = 'A'; code <= 'z'; ++code) {
    const GlyphMetrics* m = atlas.glyph(code);
    ASSERT_NE(m, nullptr);
    EXPECT_GT(m->advance, 0) << "code " << code;
  }
}

TEST(FontMetrics, LineHeightIsPixelHeight) {
  const GlyphAtlas atlas = makeDefaultAtlas();
  // The stbtt pixel-height convention: the font's ascent + descent is
  // scaled to exactly the configured pixel height (font.h preamble).
  EXPECT_EQ(atlas.lineHeight(), atlas.fontSize());
  EXPECT_EQ(atlas.lineHeight(), laige::render::kFontDefaultFontSize);
}

// ---------------------------------------------------------------------------
// FontDeterminism — same font bytes + options → identical atlas bytes
// (the roadmap Verify property; presentation-only float, ARCH-009, so
// the scope is in-process / same-build)
// ---------------------------------------------------------------------------

TEST(FontDeterminism, IdenticalBytes) {
  const std::vector<std::uint8_t> b1 = makeDefaultBytes();
  const std::vector<std::uint8_t> b2 = makeDefaultBytes();
  EXPECT_EQ(b1.size(), b2.size());
  EXPECT_EQ(std::equal(b1.begin(), b1.end(), b2.begin()), true);
  const GlyphAtlas a1 = makeDefaultAtlas();
  const GlyphAtlas a2 = makeDefaultAtlas();
  for (std::uint32_t code = a1.glyphFirst(); code <= a1.glyphLast(); ++code) {
    const GlyphMetrics* m1 = a1.glyph(code);
    const GlyphMetrics* m2 = a2.glyph(code);
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    EXPECT_EQ(*m1, *m2) << "code " << code;  // GlyphMetrics is a value
  }
  // The machine-greppable fingerprint (docs/testing.md §4 — no pinned
  // value: the determinism contract is in-process, the hash records the
  // build's output for run-to-run comparison).
  std::printf("font-atlas-determinism font=vera.ttf size=%u glyphs=%u "
              "atlas=%ux%u fnv1a=0x%016llx\n",
              static_cast<unsigned>(a1.fontSize()),
              static_cast<unsigned>(a1.glyphLast() - a1.glyphFirst() + 1),
              static_cast<unsigned>(a1.atlasWidth()),
              static_cast<unsigned>(a1.atlasHeight()),
              static_cast<unsigned long long>(
                  atlasFnv1a64(b1.data(), b1.size())));
}

TEST(FontDeterminism, SizeDiverges) {
  std::vector<std::uint8_t> font = loadFont();
  auto r1 = GlyphAtlas::create(font, GlyphAtlas::Options{});
  ASSERT_TRUE(r1.ok());
  GlyphAtlas::Options big = GlyphAtlas::Options{};
  big.fontSize = 32;
  auto r2 = GlyphAtlas::create(font, big);
  ASSERT_TRUE(r2.ok());
  const GlyphAtlas a1 = std::move(r1).takeValue();
  const GlyphAtlas a2 = std::move(r2).takeValue();
  EXPECT_NE(atlasFnv1a64(a1.atlas().data(), a1.atlas().size()),
            atlasFnv1a64(a2.atlas().data(), a2.atlas().size()));
  EXPECT_NE(a1.glyph('M')->width, a2.glyph('M')->width);
}

TEST(FontDeterminism, RangeDiverges) {
  std::vector<std::uint8_t> font = loadFont();
  auto r1 = GlyphAtlas::create(font, GlyphAtlas::Options{});
  ASSERT_TRUE(r1.ok());
  GlyphAtlas::Options narrow = GlyphAtlas::Options{};
  narrow.glyphFirst = 'A';
  narrow.glyphLast = 'z';
  auto r2 = GlyphAtlas::create(font, narrow);
  ASSERT_TRUE(r2.ok());
  const GlyphAtlas a1 = std::move(r1).takeValue();
  const GlyphAtlas a2 = std::move(r2).takeValue();
  // 'A' is the FIRST glyph in the narrow range (placed at the origin),
  // but not in the default range — the packing differs, the bytes
  // differ; 'A's OWN metrics are unchanged (same font, same size).
  EXPECT_NE(a1.glyph('A')->atlasX, a2.glyph('A')->atlasX);
  EXPECT_EQ(a1.glyph('A')->width, a2.glyph('A')->width);
  EXPECT_EQ(a1.glyph('A')->bearingX, a2.glyph('A')->bearingX);
}

// ---------------------------------------------------------------------------
// FontFallback — the documented missing-glyph contract (never a crash,
// never a silent glyph)
// ---------------------------------------------------------------------------

TEST(FontFallback, InRangeMissingGlyph) {
  const GlyphAtlas atlas = makeDefaultAtlas();
  // Two codepoints the test font does not carry (C1 control points,
  // inside the default Latin-1 range): stbtt maps unknown codepoints to
  // glyph 0 (the font's .notdef) — identical SHAPE metrics (measured
  // against the test font: an 8 x 13 box, advance 8), separate cells
  // (each codepoint owns its own atlas cell).
  const GlyphMetrics* m1 = atlas.glyph(0x0081);
  const GlyphMetrics* m2 = atlas.glyph(0x009F);
  ASSERT_NE(m1, nullptr);
  ASSERT_NE(m2, nullptr);
  EXPECT_EQ(m1->advance, 8);
  EXPECT_EQ(m1->bearingX, 0);
  EXPECT_EQ(m1->bearingY, 10);
  EXPECT_EQ(m1->width, 8);
  EXPECT_EQ(m1->height, 13);
  EXPECT_EQ(m1->advance, m2->advance);
  EXPECT_EQ(m1->bearingX, m2->bearingX);
  EXPECT_EQ(m1->bearingY, m2->bearingY);
  EXPECT_EQ(m1->width, m2->width);
  EXPECT_EQ(m1->height, m2->height);
  EXPECT_NE(m1->atlasX, m2->atlasX);
}

TEST(FontFallback, OutOfRangeIsSpace) {
  const GlyphAtlas atlas = makeDefaultAtlas();
  // A code just outside the configured range (the default range ends at
  // U+00FF) and one far outside it (an emoji): both fall back to the
  // documented U+0020 space slot.
  const GlyphMetrics* space = atlas.glyph(kFontFallbackCode);
  ASSERT_NE(space, nullptr);
  EXPECT_EQ(*atlas.glyph(0x0100), *space);
  EXPECT_EQ(*atlas.glyph(0x1F600), *space);
}

TEST(FontFallback, NoSpaceInRangeIsZeroMetric) {
  std::vector<std::uint8_t> font = loadFont();
  GlyphAtlas::Options letters;
  letters.glyphFirst = 'A';
  letters.glyphLast = 'z';
  auto r = GlyphAtlas::create(font, letters);
  ASSERT_TRUE(r.ok());
  const GlyphAtlas atlas = std::move(r).takeValue();
  // The configured range has no space: the out-of-range fallback is the
  // zero metric (the documented contract).
  EXPECT_EQ(*atlas.glyph(0x100), kFontEmptyGlyphMetrics);
  EXPECT_EQ(*atlas.glyph('A'), *atlas.glyph('A'));  // sanity: in-range
}

TEST(FontFallback, StoppedReturnsNull) {
  GlyphAtlas atlas;  // the stopped state
  EXPECT_EQ(atlas.glyph('A'), nullptr);
  EXPECT_EQ(atlas.glyph(0x1F600), nullptr);
}

// ---------------------------------------------------------------------------
// FontAtlas — the atlas invariants: the packing fits the fixed atlas,
// and the ink is present (the rasterizer actually drew)
// ---------------------------------------------------------------------------

TEST(FontAtlas, CellsFitAtlas) {
  const GlyphAtlas atlas = makeDefaultAtlas();
  const std::uint32_t W = atlas.atlasWidth();
  const std::uint32_t H = atlas.atlasHeight();
  for (std::uint32_t code = atlas.glyphFirst(); code <= atlas.glyphLast();
       ++code) {
    const GlyphMetrics* m = atlas.glyph(code);
    ASSERT_NE(m, nullptr);
    if (m->width > 0 && m->height > 0) {
      EXPECT_LE(m->atlasX + static_cast<std::uint32_t>(m->width), W)
          << "code " << code;
      EXPECT_LE(m->atlasY + static_cast<std::uint32_t>(m->height), H)
          << "code " << code;
    }
  }
}

TEST(FontAtlas, InkIsPresent) {
  const GlyphAtlas atlas = makeDefaultAtlas();
  const std::span<const std::uint8_t> a = atlas.atlas();
  const std::size_t total = a.size();
  std::size_t nonZero = 0;
  std::uint8_t maxCover = 0;
  for (std::uint8_t v : a) {
    if (v != 0) {
      ++nonZero;
    }
    if (v > maxCover) {
      maxCover = v;
    }
  }
  // The Latin-1 ink occupies a small fraction of the 1024 x 1024
  // atlas (most texels are transparent margin), and at least one texel
  // is full coverage (a solid stem at 16 px).
  EXPECT_GT(nonZero, 100u);
  EXPECT_LT(nonZero, total / 4);
  EXPECT_EQ(maxCover, 255u);
}

// ---------------------------------------------------------------------------
// FontZeroAlloc — the per-lookup path allocates nothing (PERF-003: the
// render read path is a pure table lookup)
// ---------------------------------------------------------------------------

TEST(FontZeroAlloc, LookupZeroAlloc) {
  const GlyphAtlas atlas = makeDefaultAtlas();
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
