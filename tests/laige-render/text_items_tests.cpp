// laige-render text items tests (M2-TEXT-02): the step's Verify command
// is `ctest -R text_items`.
//
// The contract pinned against: the StringTable (PRD §10.4 — intern,
// dedup, the bounded pool, the stopped state, the move semantics, the
// capacity failure + one rate-limited warn), the width measurement
// (the hand-computed goldens on the committed test font, the empty /
// all-space behavior, the failure preconditions), the exact wrap model
// (word wrap at max width, the character split, the oversized
// single-character line, the collapsed spaces, the line count / line
// step / alignment offsets), the declaration goldens (the per-glyph
// quad pos / scale / uv / tint / depthOverride fields against the
// M2-TEXT-01 oracle + hand-computed ink rects, the group count, the
// in-group declaration order, the failure paths incl. the
// nothing-declared domain check, the batcher overflow policy), the
// 1 000-frame zero-allocation declare loop (PERF-003), and the offscreen
// GL smoke (the white-on-alpha RGBA8 upload through the M2-SPRITE-02
// submit stage: the draw-call count + exact pixel checks against the
// atlas bytes).
//
// The StringTable* suites need no GL and no font asset. The TextMeasure*
// / TextLayout* / TextDeclare* / TextZeroAlloc* suites read the
// committed test font (tests/laige-render/assets/vera.ttf — the
// LAIGE_TEST_FONT_PATH env var, CTest ENVIRONMENT) but call no GL. The
// TextSmoke* suite needs a usable OpenGL 3.3 environment (it
// GTEST_SKIPs where absent — the documented environment contract, not
// an engine failure). No budget gate: the step's roadmap scope has no
// standalone budgets.json entry (the per-frame text cost is part of the
// composite 50k render-CPU budget, M2-PERF-01).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/render/gl_context.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_renderer.h"
#include "laige/render/text_items.h"
#include "laige/result.h"
#include "laige_test_seed.h"

namespace {

using laige::ErrorCode;
using laige::Result;
using laige::Status;
using laige::render::BlendMode;
using laige::render::GlyphAtlas;
using laige::render::GlyphMetrics;
using laige::render::Mat4;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using laige::render::SpriteDrawStats;
using laige::render::SpriteItem;
using laige::render::SpriteTint;
using laige::render::SpriteRenderer;
using laige::render::StringRef;
using laige::render::StringTable;
using laige::render::TextAlignment;
using laige::render::TextItem;
using laige::render::declareText;
using laige::render::expandGlyphAtlasRgba8;
using laige::render::measureText;
using laige::render::kTextMaxScale;

// The committed test font (tests/laige-render/assets/vera.ttf — the
// LAIGE_TEST_FONT_PATH env var, CTest ENVIRONMENT). The cross-platform
// env read + open (tests/support/laige_test_seed.h; fopen_s on MSVC —
// C4996; a read-only asset needs no _SH_DENYNO).
std::vector<std::uint8_t> loadFont() {
  const std::string path = laige::testing::ReadEnvVar("LAIGE_TEST_FONT_PATH");
  if (path.empty()) {
    ADD_FAILURE() << "LAIGE_TEST_FONT_PATH is not set (CTest ENVIRONMENT "
                     "contract — see tests/laige-render/CMakeLists.txt)";
    return {};
  }
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

// The default atlas (the M2-TEXT-01 defaults: 1024x1024, fontSize 16,
// range 32..255). Aborts on failure (the asset is committed — a
// failure is a broken test environment, not engine behavior).
GlyphAtlas makeAtlas() {
  auto r = GlyphAtlas::create(loadFont(), GlyphAtlas::Options{});
  if (!r.ok()) {
    std::fprintf(stderr, "makeAtlas: create failed\n");
    std::abort();
  }
  return std::move(r).takeValue();
}

StringTable makeTable(std::uint32_t maxStrings, std::uint32_t maxCodePoints) {
  auto r = StringTable::create(
      StringTable::Options{maxStrings, maxCodePoints});
  if (!r.ok()) {
    std::fprintf(stderr, "makeTable: create failed\n");
    std::abort();
  }
  return std::move(r).takeValue();
}

// Interns an ASCII literal as u32 code points (the header's model:
// one u32 per code point; ASCII is its own code point value).
StringRef internText(StringTable& table, const char* s) {
  std::vector<std::uint32_t> codes;
  codes.reserve(std::strlen(s));
  for (const char* p = s; *p != '\0'; ++p) {
    codes.push_back(static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(*p)));
  }
  auto r = table.intern(codes);
  if (!r.ok()) {
    std::fprintf(stderr, "internText: intern failed\n");
    std::abort();
  }
  return r.value();
}

// The frame's declared quads in batcher order (the single-group
// scenes of these tests: the in-group order IS the declaration
// order — the batcher's stable sort, equal keys).
void declaredQuads(SpriteBatcher& batcher, std::vector<SpriteItem>& out) {
  out.clear();
  const auto batches = batcher.batches();
  EXPECT_EQ(1u, batches.size());
  if (batches.empty()) return;
  for (std::uint32_t slot : batches.front().instances) {
    out.push_back(batcher.at(slot));
  }
}

// The quad's ink rect (screen px, y down) from the centered-quad model
// (the M2-SPRITE-02 contract): pos = ink center, scale = ink size.
struct InkRect {
  float left{};
  float top{};
  float width{};
  float height{};
};

InkRect inkOf(const SpriteItem& q) {
  return InkRect{q.pos.x - q.scale.x * 0.5f, q.pos.y - q.scale.y * 0.5f,
                 q.scale.x, q.scale.y};
}

// Log capture (the sprite_batcher_tests MemorySink pattern — rate
// limiting OFF so the tests assert per-event counts).
class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
  };
  void emit(const laige::log::LogRecord& record) override {
    entries.push_back(
        Entry{record.severity, std::string(record.subsystem),
              std::string(record.event)});
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

std::size_t countEvent(const MemorySink* sink, const char* subsystem,
                       const char* event) {
  std::size_t n = 0;
  for (const auto& e : sink->entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// StringTable — the PRD §10.4 string storage (no GL, no font asset)
// ---------------------------------------------------------------------------

TEST(TextStringTable, CreateValidation) {
  // First failure wins, InvalidArgument, no log (the create-validation
  // precedent).
  auto r = StringTable::create(StringTable::Options{0, 100});
  EXPECT_TRUE(r.isError());
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  r = StringTable::create(StringTable::Options{100, 0});
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  r = StringTable::create(
      StringTable::Options{laige::render::kStringTableMaxStrings + 1, 100});
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  r = StringTable::create(
      StringTable::Options{100, laige::render::kStringTableMaxCodePoints + 1});
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  // The defaults + the documented introspection.
  auto ok = StringTable::create(StringTable::Options{});
  ASSERT_TRUE(ok.ok());
  const StringTable table = std::move(ok).takeValue();
  EXPECT_TRUE(table.valid());
  EXPECT_EQ(0u, table.count());
  EXPECT_EQ(0u, table.storedCodePoints());
  EXPECT_EQ(laige::render::kStringTableDefaultStrings,
            table.stringCapacity());
  EXPECT_EQ(laige::render::kStringTableDefaultCodePoints,
            table.codePointCapacity());
}

TEST(TextStringTable, InternAndDedup) {
  StringTable table = makeTable(8, 100);
  StringRef a = internText(table, "hello");
  StringRef b = internText(table, "hello");
  StringRef c = internText(table, "world");
  StringRef e = internText(table, "");
  ASSERT_EQ(a.id, b.id);          // dedup: same bytes -> same handle
  EXPECT_NE(a.id, c.id);
  EXPECT_NE(a.id, e.id);
  EXPECT_EQ(3u, table.count());
  EXPECT_EQ(10u, table.storedCodePoints());  // 5 + 5 + 0 (dedup: one hello)
  ASSERT_TRUE(table.isValid(a));
  EXPECT_EQ(5u, static_cast<std::uint32_t>(table.text(a).size()));
  EXPECT_EQ(5u, static_cast<std::uint32_t>(table.text(b).size()));
  EXPECT_EQ(5u, static_cast<std::uint32_t>(table.text(c).size()));
  EXPECT_EQ(0u, static_cast<std::uint32_t>(table.text(e).size()));
  // The span round trip (the exact code points).
  const std::uint32_t expect[5] = {'h', 'e', 'l', 'l', 'o'};
  EXPECT_EQ(0, std::memcmp(table.text(a).data(), expect, sizeof(expect)));
  // An invalid handle reads empty + is not valid (the total read).
  EXPECT_FALSE(table.isValid(StringRef{99}));
  EXPECT_EQ(0u, static_cast<std::uint32_t>(table.text(StringRef{99}).size()));
}

TEST(TextStringTable, CapacityBounds) {
  MemorySink* sink = installCaptureSink();
  // The string-count bound (one rate-limited warn per failure, the
  // LOG-004 facade — rate limiting is OFF in the capture sink, so
  // exactly one per call).
  {
    StringTable table = makeTable(2, 100);
    ASSERT_TRUE(internText(table, "aa").id == 0);
    ASSERT_TRUE(internText(table, "bb").id == 1);
    std::vector<std::uint32_t> c3{'c', 'c'};
    auto r = table.intern(c3);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::BudgetExhausted, r.error());
    EXPECT_EQ(1u, countEvent(sink, "string_table", "table_full"));
    EXPECT_EQ(2u, table.count());  // the rejected intern stored nothing
  }
  // The code-point pool bound (a string that exactly fits is OK; the
  // next one fails on the pool, not the count).
  {
    StringTable table = makeTable(8, 5);
    ASSERT_TRUE(internText(table, "abc").id == 0);
    ASSERT_TRUE(internText(table, "de").id == 1);  // stored: 3 + 2 = 5
    std::vector<std::uint32_t> c3{'x', 'y', 'z'};
    auto r = table.intern(c3);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::BudgetExhausted, r.error());
    EXPECT_EQ(2u, countEvent(sink, "string_table", "table_full"));
    EXPECT_EQ(2u, table.count());
    EXPECT_EQ(5u, table.storedCodePoints());
  }
}

TEST(TextStringTable, StoppedState) {
  StringTable stopped;
  EXPECT_FALSE(stopped.valid());
  std::vector<std::uint32_t> c{'a'};
  auto r = stopped.intern(c);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  EXPECT_EQ(0u, stopped.count());
  EXPECT_FALSE(stopped.isValid(StringRef{0}));
  EXPECT_EQ(0u, static_cast<std::uint32_t>(stopped.text(StringRef{0}).size()));
  // No log for the stopped-state failure (the no-log precondition).
  MemorySink* sink = installCaptureSink();
  (void)stopped.intern(c);
  EXPECT_EQ(0u, countEvent(sink, "string_table", "table_full"));
}

TEST(TextStringTable, MoveSemantics) {
  auto r = StringTable::create(StringTable::Options{4, 16});
  ASSERT_TRUE(r.ok());
  StringTable a = std::move(r).takeValue();
  StringRef ref = internText(a, "move");
  StringTable b = std::move(a);
  EXPECT_FALSE(a.valid());          // moved-from: stopped
  EXPECT_TRUE(b.valid());
  ASSERT_TRUE(b.isValid(ref));
  EXPECT_EQ(4u, static_cast<std::uint32_t>(b.text(ref).size()));
}

// ---------------------------------------------------------------------------
// measureText — the width goldens (the M2-TEXT-01 measured metrics at
// 16 px: space adv 4, M adv 12, a adv 8, W adv 14, . adv 4)
// ---------------------------------------------------------------------------

TEST(TextMeasure, WidthGoldens) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable(16, 256);
  struct Case {
    const char* text;
    std::uint32_t scale;
    std::int64_t width;
  };
  const Case cases[] = {
      {"a", 1, 8},
      {"M", 1, 12},
      {"Ma", 1, 20},
      {"Ma.", 1, 24},
      {"a b", 1, 21},    // 'b' advances 9 (measured, not assumed)
      {"a  b", 1, 21},   // consecutive spaces collapse to one advance
      {"aa aa", 1, 36},
      {"W a", 1, 26},
      {"a", 2, 16},
      {"Ma", 2, 40},
      {"Ma.", 4, 96},
  };
  for (const Case& c : cases) {
    StringRef ref = internText(table, c.text);
    auto r = measureText(font, table, ref, c.scale);
    ASSERT_TRUE(r.ok()) << c.text;
    EXPECT_EQ(c.width, r.value()) << c.text << " @ scale " << c.scale;
  }
}

TEST(TextMeasure, EmptyAndAllSpace) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable(8, 64);
  StringRef empty = internText(table, "");
  StringRef spaces = internText(table, "   ");
  StringRef padded = internText(table, "  a  ");
  auto r = measureText(font, table, empty, 1);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(0, r.value());
  r = measureText(font, table, spaces, 1);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(0, r.value());
  r = measureText(font, table, padded, 1);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(8, r.value());  // leading/trailing spaces carry no width
}

TEST(TextMeasure, FailurePreconditions) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable(8, 64);
  StringRef ref = internText(table, "a");
  GlyphAtlas stopped;
  // A stopped/missing font.
  auto r = measureText(stopped, table, ref, 1);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  r = measureText(font, table, ref, 0);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  r = measureText(font, table, ref, kTextMaxScale + 1);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  // A handle the table does not own.
  r = measureText(font, table, StringRef{99}, 1);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
}

// ---------------------------------------------------------------------------
// declareText — the layout (wrap model, alignment, line step)
// ---------------------------------------------------------------------------

TEST(TextLayout, LineCountAndStep) {
  GlyphAtlas font = makeAtlas();
  const GlyphMetrics& mM = *font.glyph('M');
  const GlyphMetrics& ma = *font.glyph('a');
  ASSERT_EQ(12, mM.advance);
  ASSERT_EQ(8, ma.advance);
  StringTable table = makeTable(16, 256);
  struct Case {
    const char* text;
    std::int32_t maxWidth;
    std::uint32_t lines;
    std::uint32_t glyphsPerLine[8];
  };
  // Hand-computed from the measured advances (M 12, a 8, b 9, space 4):
  const Case cases[] = {
      {"aa aa", 0, 1, {4}},        // no wrap
      {"aa aa", 36, 1, {4}},       // 16 + 4 + 16 = 36 fits exactly
      {"aa aa", 20, 2, {2, 2}},    // 16 + 4 + 16 = 36 > 20
      {"MMMM", 24, 2, {2, 2}},     // 48 > 24: split MM | MM
      {"MMMM", 12, 4, {1, 1, 1, 1}},  // each M (12) its own line
      {"MMMM", 11, 4, {1, 1, 1, 1}},  // oversized: lines exceed maxWidth
      {"a b c", 16, 3, {1, 1, 1}},    // 8 + 4 + 9 = 21 > 16 per word pair
      {"a b c", 21, 2, {2, 1}},       // "a b" = 8 + 4 + 9 = 21 fits; "c" next
      {"a b ", 16, 2, {1, 1}},        // the trailing space owns no line
      {"a  b", 21, 1, {2}},           // collapsed spaces: 8 + 4 + 9 = 21
  };
  for (const Case& c : cases) {
    StringRef ref = internText(table, c.text);
    auto bc = SpriteBatcher::create({256});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    TextItem item;
    item.text = ref;
    item.font = &font;
    item.x = 0.0f;
    item.y = 20.0f;
    item.scale = 1;
    item.maxWidth = c.maxWidth;
    auto r = declareText(batcher, table, item);
    ASSERT_TRUE(r.ok()) << c.text << " maxWidth " << c.maxWidth;
    ASSERT_TRUE(batcher.build().ok());
    std::vector<SpriteItem> quads;
  declaredQuads(batcher, quads);
    // The line count: the distinct baselines (the exact integer
    // 20 + 16 * lineIndex at scale 1, one presentation-only float).
    // The glyph of a quad: its cell's (u0, v0) — the shelf packing
    // gives every ink glyph a distinct cell (M2-TEXT-01).
    auto codeOf = [&](const SpriteItem& q) {
      for (std::uint32_t code = 32; code <= 255; ++code) {
        const GlyphMetrics* m = font.glyph(code);
        if (static_cast<float>(m->atlasX) / 1024.0f == q.uv.u0 &&
            static_cast<float>(m->atlasY) / 1024.0f == q.uv.v0) {
          return code;
        }
      }
      ADD_FAILURE() << "no atlas cell matches the quad (u0, v0)";
      return 0u;
    };
    std::vector<std::uint32_t> perLine;
    for (const SpriteItem& q : quads) {
      // The baseline = ink top + bearingY (the ink bottom is NOT the
      // baseline for ascenders/descenders).
      const float inkTop = q.pos.y - q.scale.y * 0.5f;
      const float baseline =
          inkTop + static_cast<float>(font.glyph(codeOf(q))->bearingY);
      if (perLine.empty()) {
        perLine.push_back(1);
      } else if (std::fabs(baseline -
                          (20.0f + 16.0f *
                           static_cast<float>(perLine.size() - 1))) <
                 0.001f) {
        perLine.back() += 1;
      } else {
        perLine.push_back(1);
      }
      // The baseline is the exact integer of its line (line step 16).
      const float expectedBaseline =
          20.0f + 16.0f * static_cast<float>(perLine.size() - 1);
      EXPECT_FLOAT_EQ(expectedBaseline, baseline) << c.text;
    }
    ASSERT_EQ(c.lines, perLine.size()) << c.text << " maxWidth "
                                       << c.maxWidth;
    for (std::uint32_t i = 0; i < c.lines; ++i) {
      EXPECT_EQ(c.glyphsPerLine[i], perLine[i]) << c.text << " line " << i;
    }
  }
}

TEST(TextLayout, AlignmentAnchors) {
  GlyphAtlas font = makeAtlas();
  const GlyphMetrics& mM = *font.glyph('M');
  const GlyphMetrics& ma = *font.glyph('a');
  ASSERT_EQ(1, mM.bearingX);
  ASSERT_EQ(12, mM.advance);
  ASSERT_EQ(8, ma.advance);
  StringTable table = makeTable(8, 64);
  StringRef ref = internText(table, "Ma");  // width 20
  struct Case {
    TextAlignment alignment;
    float x;
    float expectedMInkLeft;
  };
  const Case cases[] = {
      {TextAlignment::Left, 50.0f, 50.0f + 1.0f},     // anchor = left edge
      {TextAlignment::Center, 50.0f, 50.0f - 10.0f + 1.0f},  // 50 - 20/2
      {TextAlignment::Right, 50.0f, 50.0f - 20.0f + 1.0f},   // 50 - 20
  };
  for (const Case& c : cases) {
    auto bc = SpriteBatcher::create({16});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    TextItem item;
    item.text = ref;
    item.font = &font;
    item.x = c.x;
    item.y = 20.0f;
    item.scale = 1;
    item.alignment = c.alignment;
    auto r = declareText(batcher, table, item);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(batcher.build().ok());
    std::vector<SpriteItem> quads;
  declaredQuads(batcher, quads);
    ASSERT_EQ(2u, quads.size());
    EXPECT_FLOAT_EQ(c.expectedMInkLeft, inkOf(quads[0]).left);
  }
}

TEST(TextLayout, ScaleDoublesEverything) {
  GlyphAtlas font = makeAtlas();
  const GlyphMetrics& mM = *font.glyph('M');
  ASSERT_EQ(10, mM.width);
  ASSERT_EQ(11, mM.height);
  ASSERT_EQ(1, mM.bearingX);
  ASSERT_EQ(11, mM.bearingY);
  StringTable table = makeTable(8, 64);
  StringRef ref = internText(table, "M");
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  TextItem item;
  item.text = ref;
  item.font = &font;
  item.x = 10.0f;
  item.y = 30.0f;
  item.scale = 2;
  auto r = declareText(batcher, table, item);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(batcher.build().ok());
  std::vector<SpriteItem> quads;
  declaredQuads(batcher, quads);
  ASSERT_EQ(1u, quads.size());
  const InkRect ink = inkOf(quads[0]);
  // The 2x ink rect: left 10 + 1*2 = 12, top 30 - 11*2 = 8, 20 x 22.
  EXPECT_FLOAT_EQ(12.0f, ink.left);
  EXPECT_FLOAT_EQ(8.0f, ink.top);
  EXPECT_FLOAT_EQ(20.0f, ink.width);
  EXPECT_FLOAT_EQ(22.0f, ink.height);
}

TEST(TextLayout, EmptyTextDeclaresNothing) {
  StringTable table = makeTable(8, 64);
  StringRef empty = internText(table, "");
  StringRef spaces = internText(table, "  ");
  GlyphAtlas font = makeAtlas();
  for (StringRef ref : {empty, spaces}) {
    auto bc = SpriteBatcher::create({16});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    TextItem item;
    item.text = ref;
    item.font = &font;
    item.x = 0.0f;
    item.y = 20.0f;
    auto r = declareText(batcher, table, item);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(0u, batcher.frameCount());
    ASSERT_TRUE(batcher.build().ok());
    EXPECT_EQ(0u, batcher.batches().size());
  }
}

// ---------------------------------------------------------------------------
// declareText — the per-glyph quad goldens (the M2-TEXT-01 oracle + the
// hand-computed ink rects) + the group / order contract
// ---------------------------------------------------------------------------

TEST(TextDeclare, QuadGoldens) {
  GlyphAtlas font = makeAtlas();
  const GlyphMetrics& mM = *font.glyph('M');
  const GlyphMetrics& ma = *font.glyph('a');
  // The pinned M2-TEXT-01 goldens at 16 px (vera.ttf):
  ASSERT_EQ(12, mM.advance);
  ASSERT_EQ(10, mM.width);
  ASSERT_EQ(11, mM.height);
  ASSERT_EQ(1, mM.bearingX);
  ASSERT_EQ(11, mM.bearingY);
  ASSERT_EQ(8, ma.advance);
  ASSERT_EQ(8, ma.width);
  ASSERT_EQ(9, ma.height);
  ASSERT_EQ(8, ma.bearingY);
  StringTable table = makeTable(8, 64);
  StringRef ref = internText(table, "Ma");
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  TextItem item;
  item.text = ref;
  item.font = &font;
  item.x = 10.0f;
  item.y = 20.0f;
  item.scale = 2;
  item.color = SpriteTint{0.5f, 0.25f, 1.0f, 1.0f};
  item.depthKey = 123;
  item.atlasId = 7;
  auto r = declareText(batcher, table, item);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(2u, batcher.frameCount());
  ASSERT_TRUE(batcher.build().ok());
  std::vector<SpriteItem> quads;
  declaredQuads(batcher, quads);
  ASSERT_EQ(2u, quads.size());
  const SpriteItem& qM = quads[0];
  const SpriteItem& qa = quads[1];
  // The M quad (hand-computed): ink left 10 + 1*2 = 12, top 20 - 11*2 =
  // -2, 20 x 22; the uv is the cell rect over the 1024^2 atlas.
  EXPECT_FLOAT_EQ(12.0f + 10.0f, qM.pos.x);  // inkLeft + w/2
  EXPECT_FLOAT_EQ(-2.0f + 11.0f, qM.pos.y);  // inkTop + h/2
  EXPECT_FLOAT_EQ(20.0f, qM.scale.x);
  EXPECT_FLOAT_EQ(22.0f, qM.scale.y);
  const float invW = 1.0f / 1024.0f;
  EXPECT_FLOAT_EQ(static_cast<float>(mM.atlasX) * invW, qM.uv.u0);
  EXPECT_FLOAT_EQ(static_cast<float>(mM.atlasY) * invW, qM.uv.v0);
  EXPECT_FLOAT_EQ(static_cast<float>(mM.atlasX + 10u) * invW, qM.uv.u1);
  EXPECT_FLOAT_EQ(static_cast<float>(mM.atlasY + 11u) * invW, qM.uv.v1);
  // The a quad: the pen advanced by M's advance (12 * 2) from x = 10.
  EXPECT_FLOAT_EQ(10.0f + 24.0f + 2.0f * ma.bearingX + 8.0f, qa.pos.x);
  EXPECT_FLOAT_EQ(20.0f - 16.0f + 9.0f, qa.pos.y);
  EXPECT_FLOAT_EQ(16.0f, qa.scale.x);
  EXPECT_FLOAT_EQ(18.0f, qa.scale.y);
  EXPECT_FLOAT_EQ(static_cast<float>(ma.atlasX) * invW, qa.uv.u0);
  EXPECT_FLOAT_EQ(static_cast<float>(ma.atlasY) * invW, qa.uv.v0);
  EXPECT_FLOAT_EQ(static_cast<float>(ma.atlasX + 8u) * invW, qa.uv.u1);
  EXPECT_FLOAT_EQ(static_cast<float>(ma.atlasY + 9u) * invW, qa.uv.v1);
  // The shared declaration fields (the header's glyph-quad section).
  for (const SpriteItem* q : {&qM, &qa}) {
    EXPECT_EQ(123u, q->depthKey);
    EXPECT_TRUE(q->depthOverride);  // the G-R11 UI-z escape hatch
    EXPECT_EQ(0u, q->frameIndex);
    EXPECT_FLOAT_EQ(0.0f, q->rotation);
    EXPECT_FLOAT_EQ(0.5f, q->tint.r);
    EXPECT_FLOAT_EQ(0.25f, q->tint.g);
    EXPECT_FLOAT_EQ(1.0f, q->tint.b);
    EXPECT_FLOAT_EQ(1.0f, q->tint.a);
    EXPECT_EQ(7u, q->atlasId);
    EXPECT_EQ(0u, q->materialId);
    EXPECT_EQ(BlendMode::Alpha, q->blend);  // the text convention
  }
}

TEST(TextDeclare, GroupsAndOrder) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable(16, 256);
  StringRef dot = internText(table, "Ma.");
  StringRef second = internText(table, "ab");
  auto bc = SpriteBatcher::create({64});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  TextItem a;
  a.text = dot;
  a.font = &font;
  a.x = 0.0f;
  a.y = 0.0f;
  a.atlasId = 7;
  TextItem b;
  b.text = second;
  b.font = &font;
  b.x = 0.0f;
  b.y = 32.0f;
  b.atlasId = 3;
  ASSERT_TRUE(declareText(batcher, table, a).ok());
  ASSERT_TRUE(declareText(batcher, table, b).ok());
  ASSERT_EQ(5u, batcher.frameCount());  // 3 + 2 glyphs
  ASSERT_TRUE(batcher.build().ok());
  const auto batches = batcher.batches();
  // One (atlas, material, blend) group per atlas id — ascending
  // (RENDER-001/003): atlas 3 first, then 7 (the FR-2.1 group key).
  ASSERT_EQ(2u, batches.size());
  EXPECT_EQ(3u, batches[0].atlasId);
  EXPECT_EQ(2u, batches[0].instances.size());
  EXPECT_EQ(7u, batches[1].atlasId);
  EXPECT_EQ(3u, batches[1].instances.size());
  // The in-group order is the declaration order (equal keys, the
  // batcher's stable sort — the M, a, . sequence on line 0).
  const GlyphMetrics& mM = *font.glyph('M');
  const GlyphMetrics& mDot = *font.glyph('.');
  const SpriteItem& first = batcher.at(batches[1].instances[0]);
  const SpriteItem& last = batcher.at(batches[1].instances[2]);
  EXPECT_FLOAT_EQ(static_cast<float>(mM.atlasX) / 1024.0f, first.uv.u0);
  EXPECT_FLOAT_EQ(static_cast<float>(mDot.atlasX) / 1024.0f, last.uv.u0);
}

TEST(TextDeclare, FailurePaths) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable(64, 8192);
  StringRef ref = internText(table, "Ma");
  TextItem item;
  item.text = ref;
  item.font = &font;
  item.x = 0.0f;
  item.y = 0.0f;
  // The stopped batcher (the first add fails — BudgetExhausted, nothing
  // declared).
  {
    SpriteBatcher stopped;
    auto r = declareText(stopped, table, item);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::BudgetExhausted, r.error());
    EXPECT_EQ(0u, stopped.frameCount());
  }
  // The closed window (after build) — InvalidArgument, nothing added.
  {
    auto bc = SpriteBatcher::create({16});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    batcher.beginFrame();
    ASSERT_TRUE(declareText(batcher, table, item).ok());
    ASSERT_TRUE(batcher.build().ok());
    auto r = declareText(batcher, table, item);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(2u, batcher.frameCount());  // the first call's quads only
  }
  // A null / stopped font (InvalidArgument, nothing declared).
  {
    auto bc = SpriteBatcher::create({16});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    item.font = nullptr;
    auto r = declareText(batcher, table, item);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
    GlyphAtlas stopped;
    item.font = &stopped;
    r = declareText(batcher, table, item);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
    item.font = &font;
  }
  // An invalid handle (the table does not own it).
  {
    auto bc = SpriteBatcher::create({16});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    item.text = StringRef{999};
    auto r = declareText(batcher, table, item);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
    item.text = ref;
  }
  // The out-of-domain fields (scale, maxWidth).
  {
    auto bc = SpriteBatcher::create({16});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    item.scale = 0;
    EXPECT_TRUE(declareText(batcher, table, item).isError());
    item.scale = kTextMaxScale + 1;
    EXPECT_TRUE(declareText(batcher, table, item).isError());
    item.scale = 1;
    item.maxWidth = -1;
    EXPECT_TRUE(declareText(batcher, table, item).isError());
    item.maxWidth = static_cast<std::int32_t>(
        laige::render::kTextMaxLineWidthPx) + 1;
    EXPECT_TRUE(declareText(batcher, table, item).isError());
    item.maxWidth = 0;
    EXPECT_EQ(0u, batcher.frameCount());  // nothing declared by any fail
  }
  // The screen domain: a no-wrap line wider than kTextMaxLineWidthPx
  // (8192 Ms at 16x: 8192 * 12 * 16 = 1 572 864 px > 2^20) declares
  // NOTHING (the pre-check runs before the first add).
  {
    // Its own table (the shared one already holds "Ma"'s 2 code points
    // and would fail the pool bound, not the screen domain).
    StringTable big = makeTable(8, 8192);
    std::vector<std::uint32_t> ms(8192, 'M');
    auto ri = big.intern(ms);
    ASSERT_TRUE(ri.ok());
    auto bc = SpriteBatcher::create({16});
    ASSERT_TRUE(bc.ok());
    SpriteBatcher batcher = std::move(bc).takeValue();
    item.text = ri.value();
    item.scale = 16;
    item.maxWidth = 0;
    auto r = declareText(batcher, big, item);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
    EXPECT_EQ(0u, batcher.frameCount());
  }
}

TEST(TextDeclare, BatcherOverflowDropsOldest) {
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable(8, 64);
  StringRef ref = internText(table, "Ma.");  // 3 ink glyphs
  auto bc = SpriteBatcher::create({2});  // the frame holds only 2
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  TextItem item;
  item.text = ref;
  item.font = &font;
  item.x = 0.0f;
  item.y = 0.0f;
  // The batcher's own overflow policy (drop oldest + warn, PERF-008)
  // bounds the frame — declareText itself succeeds.
  auto r = declareText(batcher, table, item);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(2u, batcher.frameCount());
  ASSERT_TRUE(batcher.build().ok());
  std::vector<SpriteItem> quads;
  declaredQuads(batcher, quads);
  ASSERT_EQ(2u, quads.size());
  // The oldest quad (the M) was dropped: the survivors are a, . in
  // declaration order (the M2-TEXT-01 oracle cells).
  const GlyphMetrics& ma = *font.glyph('a');
  const GlyphMetrics& mDot = *font.glyph('.');
  EXPECT_FLOAT_EQ(static_cast<float>(ma.atlasX) / 1024.0f, quads[0].uv.u0);
  EXPECT_FLOAT_EQ(static_cast<float>(mDot.atlasX) / 1024.0f,
                  quads[1].uv.u0);
}

// ---------------------------------------------------------------------------
// expandGlyphAtlasRgba8 — the white-on-alpha upload bytes
// ---------------------------------------------------------------------------

TEST(TextDeclare, ExpandRgba8) {
  GlyphAtlas font = makeAtlas();
  std::vector<std::uint8_t> rgba;
  // The wrong-size span (nothing expanded).
  rgba.assign(8, 0);
  auto r = expandGlyphAtlasRgba8(font, rgba);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(ErrorCode::InvalidArgument, r.error());
  // The exact size: 1024 x 1024 x 4.
  rgba.assign(1024u * 1024u * 4u, 0);
  ASSERT_TRUE(expandGlyphAtlasRgba8(font, rgba).ok());
  const std::span<const std::uint8_t> alpha = font.atlas();
  // Spot checks: white-on-alpha (R = G = B = 255, A = the atlas byte).
  for (std::uint32_t i : {0u, 1u, 1024u * 17u + 3u, 1024u * 1023u + 1023u}) {
    const std::uint8_t a = alpha[i];
    EXPECT_EQ(255u, rgba[i * 4 + 0]);
    EXPECT_EQ(255u, rgba[i * 4 + 1]);
    EXPECT_EQ(255u, rgba[i * 4 + 2]);
    EXPECT_EQ(a, rgba[i * 4 + 3]);
  }
  // A stopped atlas (nothing expanded).
  GlyphAtlas stopped;
  rgba.assign(8, 0);
  EXPECT_TRUE(expandGlyphAtlasRgba8(stopped, rgba).isError());
}

// ---------------------------------------------------------------------------
// TextZeroAlloc — the per-frame declare loop allocates nothing
// (PERF-003)
// ---------------------------------------------------------------------------

TEST(TextZeroAlloc, DeclareLoopAllocatesNothing) {
  constexpr std::uint32_t kFrames = 1000;
  GlyphAtlas font = makeAtlas();
  StringTable table = makeTable(8, 64);
  StringRef ref =
      internText(table, "The quick brown fox jumps over the lazy dog");
  auto bc = SpriteBatcher::create({512});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  TextItem item;
  item.text = ref;
  item.font = &font;
  item.x = 0.0f;
  item.y = 0.0f;
  item.scale = 2;
  item.maxWidth = 40;  // a multi-line wrapped text (the loop's shape)
  auto runFrames = [&]() {
    for (std::uint32_t f = 0; f < kFrames; ++f) {
      batcher.beginFrame();
      if (!declareText(batcher, table, item).ok()) {
        std::fprintf(stderr, "TextZeroAlloc: declare failed\n");
        std::abort();
      }
      if (!batcher.build().ok()) {
        std::fprintf(stderr, "TextZeroAlloc: build failed\n");
        std::abort();
      }
    }
  };
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    runFrames();
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(0ull, reading.allocs)
        << kFrames << " frames of a wrapped 2x-scale declare allocated "
        << reading.allocs << " heap blocks on the loop thread";
    std::printf("text-items-zeroalloc frames=%u allocs=%llu\n", kFrames,
                static_cast<unsigned long long>(reading.allocs));
  } else {
    runFrames();  // sanitizer tree: the leak-free run covers it
  }
}

// ---------------------------------------------------------------------------
// TextSmoke — the offscreen GL render (the M2-SPRITE-02 submit stage
// with the white-on-alpha glyph atlas; GTEST_SKIPs where no usable
// OpenGL 3.3 environment — the documented environment contract)
// ---------------------------------------------------------------------------

namespace {

constexpr std::int32_t kSmokeWidth = 64;
constexpr std::int32_t kSmokeHeight = 64;

// The UI pass's identity screen matrix (screen px, y down -> NDC):
// x_ndc = 2x/w - 1, y_ndc = 1 - 2y/h (the M2-UI-02 plane).
Mat4 smokeMatrix() {
  // Column-major (the house Mat4 convention): the linear part in
  // columns 0..1, the translation in column 3 (the reference
  // rasterizer's a30/a31 — sprite_draw_tests' makeRefQuad).
  Mat4 m(1.0f);  // the identity
  m[0].x = 2.0f / static_cast<float>(kSmokeWidth);
  m[1].y = -2.0f / static_cast<float>(kSmokeHeight);
  m[3].x = -1.0f;
  m[3].y = 1.0f;
  return m;
}

}  // namespace

TEST(TextSmoke, OffscreenTextFrame) {
  auto ctx = laige::render::GlContext::createHeadless(kSmokeWidth,
                                                      kSmokeHeight);
  if (!ctx.ok()) {
    GTEST_SKIP() << "No usable OpenGL 3.3 environment: "
                 << laige::errorText(ctx.error())
                 << " (the documented environment contract — the P0 CI "
                    "runners always have one)";
  }
  laige::render::GlContext gl = std::move(ctx).takeValue();
  GlyphAtlas font = makeAtlas();
  const GlyphMetrics& mM = *font.glyph('M');
  const GlyphMetrics& ma = *font.glyph('a');
  ASSERT_EQ(10, mM.width);
  ASSERT_EQ(11, mM.height);
  ASSERT_EQ(1, mM.bearingX);
  ASSERT_EQ(11, mM.bearingY);
  ASSERT_EQ(8, ma.width);
  ASSERT_EQ(9, ma.height);
  // The upload bytes (the setup path — one call per font per scene).
  std::vector<std::uint8_t> rgba;
  rgba.resize(1024u * 1024u * 4u);
  ASSERT_TRUE(expandGlyphAtlasRgba8(font, rgba).ok());
  auto rc = laige::render::SpriteRenderer::create(
      gl, SpriteRenderer::Options{16, 8});
  ASSERT_TRUE(rc.ok());
  SpriteRenderer renderer = std::move(rc).takeValue();
  ASSERT_TRUE(
      renderer.bindAtlas(3, font.atlasWidth(), font.atlasHeight(), rgba)
          .ok());
  StringTable table = makeTable(8, 64);
  StringRef ref = internText(table, "Ma");
  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  SpriteBatcher batcher = std::move(bc).takeValue();
  TextItem item;
  item.text = ref;
  item.font = &font;
  item.x = 10.0f;
  item.y = 20.0f;  // the first line's baseline (screen y down)
  item.atlasId = 3;
  ASSERT_TRUE(declareText(batcher, table, item).ok());
  ASSERT_TRUE(batcher.build().ok());
  // The clear BEFORE the first draw (the offscreen frame protocol).
  ASSERT_TRUE(gl.clear(0.0f, 0.0f, 0.0f, 1.0f).ok());
  ASSERT_TRUE(renderer.submit(batcher, smokeMatrix()).ok());
  // One (atlas, material, blend) group: one instanced draw, 2
  // instances (RENDER-001).
  const SpriteDrawStats stats = renderer.frameStats();
  EXPECT_EQ(1u, stats.drawCalls);
  EXPECT_EQ(2u, stats.instances);
  // The pixel checks (screen y down -> GL y = H - 1 - sy; the clear
  // alpha is 1.0 -> the composite alpha is exactly 255; the white-on-
  // alpha glyph over black: rgb = the atlas alpha byte, exactly — the
  // GL 3.3 8-bit conversion of 255 * (A/255)).
  const std::span<const std::uint8_t> alpha = font.atlas();
  auto readScreen = [&](std::int32_t sx, std::int32_t sy) {
    std::uint8_t rgba4[4]{};
    auto r = gl.readPixel(sx, kSmokeHeight - 1 - sy, rgba4);
    if (!r.ok()) return std::array<std::uint8_t, 4>{};
    return std::array<std::uint8_t, 4>{rgba4[0], rgba4[1], rgba4[2],
                                       rgba4[3]};
  };
  // The ink rects (screen px, y down) — the hand-computed declaration
  // (the item at (10, 20), scale 1, left-aligned): the M ink left
  // 10 + bearingX, top 20 - bearingY; the a starts at 10 + M's
  // advance.
  const int inkML = 10 + mM.bearingX;
  const int inkMT = 20 - mM.bearingY;
  const int inkAL = 10 + mM.advance + ma.bearingX;
  const int inkAT = 20 - ma.bearingY;
  // The first texel of a cell in an alpha class ({tx, ty, a}; tx = -1
  // when the class is empty): 0 = transparent, 1 = semi, 2 = opaque.
  auto firstTexel = [&](const GlyphMetrics& g, int cls) {
    for (int ty = 0; ty < g.height; ++ty) {
      for (int tx = 0; tx < g.width; ++tx) {
        const std::uint8_t a =
            alpha[(g.atlasY + static_cast<std::uint32_t>(ty)) * 1024u +
                  g.atlasX + static_cast<std::uint32_t>(tx)];
        const bool hit = cls == 0
                             ? a == 0u
                             : (cls == 1
                                    ? (a > 0 && a < 255u)
                                    : a == 255u);
        if (hit) {
          return std::array<int, 3>{tx, ty, static_cast<int>(a)};
        }
      }
    }
    return std::array<int, 3>{-1, 0, 0};
  };
  // The background (outside both inks).
  {
    const std::array<std::uint8_t, 4> p = readScreen(5, 40);
    EXPECT_EQ(0u, p[0]);
    EXPECT_EQ(0u, p[1]);
    EXPECT_EQ(0u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // The fully opaque texels of both glyphs: the white-on-alpha upload
  // over the black clear is rgb = A = 255, and the alpha channel is
  // exactly 255 (the engine's SRC_ALPHA/ONE_MINUS_SRC_ALPHA blend with
  // a source alpha of 1). The pixel's screen position pins the UV
  // mapping (that exact texel lands on that exact pixel).
  {
    const std::array<int, 3> m = firstTexel(mM, 2);
    ASSERT_EQ(255, m[2]);
    const std::array<std::uint8_t, 4> p =
        readScreen(inkML + m[0], inkMT + m[1]);
    EXPECT_EQ(255u, p[0]);
    EXPECT_EQ(255u, p[1]);
    EXPECT_EQ(255u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  {
    const std::array<int, 3> a = firstTexel(ma, 2);
    ASSERT_EQ(255, a[2]);
    const std::array<std::uint8_t, 4> p =
        readScreen(inkAL + a[0], inkAT + a[1]);
    EXPECT_EQ(255u, p[0]);
    EXPECT_EQ(255u, p[1]);
    EXPECT_EQ(255u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // A transparent texel of the M cell: nothing drawn there -> the
  // clear shows through.
  {
    const std::array<int, 3> t = firstTexel(mM, 0);
    ASSERT_NE(-1, t[0]);
    const std::array<std::uint8_t, 4> p =
        readScreen(inkML + t[0], inkMT + t[1]);
    EXPECT_EQ(0u, p[0]);
    EXPECT_EQ(0u, p[1]);
    EXPECT_EQ(0u, p[2]);
    EXPECT_EQ(255u, p[3]);
  }
  // A semi-transparent texel of the M: the rgb is exactly the atlas
  // alpha (the white-on-alpha over black), and the alpha channel
  // follows the engine's blend on all four channels (M2-SPRITE-02:
  // SRC_ALPHA/ONE_MINUS_SRC_ALPHA) — out.a = A/255 + (1 - A/255) *
  // (1 - A/255) in float, one driver-defined 8-bit rounding step, so
  // the tolerance is +/- 1.
  {
    const std::array<int, 3> m = firstTexel(mM, 1);
    ASSERT_NE(-1, m[0]);
    const std::uint8_t A = static_cast<std::uint8_t>(m[2]);
    const std::array<std::uint8_t, 4> p =
        readScreen(inkML + m[0], inkMT + m[1]);
    EXPECT_EQ(A, p[0]);
    EXPECT_EQ(A, p[1]);
    EXPECT_EQ(A, p[2]);
    const double a = static_cast<double>(A) / 255.0;
    const int expect = static_cast<int>(255.0 * (a * a + (1.0 - a)) + 0.5);
    EXPECT_LE(static_cast<int>(p[3]) - expect + 1, 1);
    EXPECT_LE(expect - static_cast<int>(p[3]) + 1, 1);
  }
  // The context outlives the renderer (the ownership contract).
}
