// laige-render parallax layer tests (M2-PAR-01): the named background /
// midground / foreground layer model of FR-2.3 — the parallax factor,
// the world-space offset formula, the UV scroll (auto/manual) with the
// exact wrap at the texture boundary, and the batch path into the
// sprite batcher — in laige/render/parallax.h.
//
// Pure data + batcher bookkeeping — no GL context, no GL environment
// needed: every suite runs in every local tree and in CI. The offset
// goldens are HAND-COMPUTED from the pinned formula (factor * (p -
// center) + offset) at dyadic values (bit-exact in float, and in the
// dyadic exactness zone of both SimMath backends); the depth-key
// goldens are hand-computed from the M2-ISO-01 formula ((l + 512)
// << 22 | (d + 2^21), d = 16 * (cx + cy) at the dyadic centers), and
// the independent oracle is the M2-ISO-01 function itself (the
// tilemap test pattern). The zero-allocation window covers the
// per-frame declare loop (FR-2.2 "no per-frame allocation" — the
// sanitizer trees run the same workload shapes leak-free instead,
// methodology §4).
//
// No budget gate: the step's roadmap scope has no standalone
// budgets.json entry (the per-frame declare cost is part of the
// composite 50k render-CPU budget — M2-PERF-01).

#include "laige/render/iso_depth_key.h"
#include "laige/render/parallax.h"
#include "laige/render/sprite_batcher.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#if !defined(_MSC_VER)
#include <dlfcn.h>  // dladdr (the alloc-site module/symbol, POSIX)
#endif
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/sim_math.h"

namespace {

using laige::fpx16_16;
using laige::render::BlendMode;
using laige::render::ParallaxLayerDef;
using laige::render::ParallaxScrollMode;
using laige::render::ParallaxSource;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using laige::render::SpriteItem;
using laige::render::SpriteUvRect;
using laige::render::kParallaxDepthLayerBackground;
using laige::render::kParallaxDepthLayerForeground;
using laige::render::kParallaxDepthLayerMidground;
using laige::render::kParallaxLayerCustomBase;
using laige::sim::Fp32Pinned;
using laige::sim::Fpx16_16;

// ---------------------------------------------------------------------------
// The independent oracle: the M2-ISO-01 function on the quad's world
// center — the parallax keys must equal it (the dyadic centers are in
// the exactness zone of both backends — the bit-identity contract).
// The test converts the float point into the backend's Vec2 itself
// (never reusing the registry's conversion).
// ---------------------------------------------------------------------------

template <typename Backend>
laige::sim::SimMath<Backend>::Vec2 backendVec2(float x, float y) {
  using M = laige::sim::SimMath<Backend>;
  if constexpr (std::is_same_v<Backend, Fp32Pinned>) {
    return typename M::Vec2{x, y};
  } else {
    return typename M::Vec2{fpx16_16::fromFloat(x), fpx16_16::fromFloat(y)};
  }
}

template <typename Backend>
std::uint32_t oracleKey(float x, float y, std::int32_t layer) {
  return laige::render::isoDepthKey<Backend>(backendVec2<Backend>(x, y), 0,
                                            layer);
}

// ---------------------------------------------------------------------------
// Log capture (the camera_tests MemorySink pattern)
// ---------------------------------------------------------------------------

class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
  };

  void emit(const laige::log::LogRecord& record) override {
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    for (const auto& f : record.fields) {
      e.fields.emplace_back(std::string(f.name), f.value);
    }
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

// Installs a fresh capture sink (heap-owned by the logger) with rate
// limiting OFF (the iso_depth_table_tests pattern).
MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

std::size_t countEvents(const MemorySink& sink, std::string_view subsystem,
                        std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

laige::Status expectRejected(laige::Status s, laige::ErrorCode want) {
  EXPECT_TRUE(s.isError());
  EXPECT_EQ(s.error(), want);
  return s;
}

// SpriteUvRect has no operator== (the M2-SPRITE-01 value type):
// the field-wise comparison (the sprite_frames_tests pattern).
void expectUvRect(const char* what, const SpriteUvRect& uv, float u0,
                  float v0, float u1, float v1) {
  EXPECT_EQ(uv.u0, u0) << what;
  EXPECT_EQ(uv.v0, v0) << what;
  EXPECT_EQ(uv.u1, u1) << what;
  EXPECT_EQ(uv.v1, v1) << what;
}

// The valid `bg` preset layer definition (the template for the
// validation matrix): the roadmap's named bg layer.
ParallaxLayerDef bgDef(std::uint32_t id) {
  ParallaxLayerDef d;
  d.id = id;
  d.source = ParallaxSource::Image;
  d.factor = 0.25f;
  d.center = {4.0f, 4.0f};
  d.offset = {1.0f, 2.0f};
  d.size = {2.0f, 2.0f};
  d.uv = {0.0f, 0.0f, 1.0f, 1.0f};
  d.atlasId = 0;
  d.materialId = 0;
  d.blend = BlendMode::Alpha;
  d.depthLayer = kParallaxDepthLayerBackground;
  d.scrollMode = ParallaxScrollMode::Manual;
  d.scrollSpeed = {0.0f, 0.0f};
  d.enabled = true;
  return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// ParallaxCreate — the registry options and the stopped state
// ---------------------------------------------------------------------------

TEST(ParallaxCreate, CreateValidation) {
  using Layers = laige::render::ParallaxLayers<Fp32Pinned>;
  typename Layers::Options o;
  // The domain bounds (no log — the M2-SPRITE-01 create precedent):
  o.maxLayers = 0;
  auto r = Layers::create(o);
  EXPECT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  o.maxLayers = laige::render::kParallaxLayersMaxLayers + 1;
  r = Layers::create(o);
  EXPECT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  // The full domain:
  o.maxLayers = laige::render::kParallaxLayersMaxLayers;
  r = Layers::create(o);
  ASSERT_TRUE(r.ok());
  auto maxed = std::move(r).takeValue();
  EXPECT_TRUE(maxed.valid());
  EXPECT_EQ(maxed.maxLayers(), laige::render::kParallaxLayersMaxLayers);
  EXPECT_EQ(maxed.layerCount(), 0u);
  EXPECT_FALSE(maxed.has(0));
  // The default (stopped) state: every operation InvalidArgument, no
  // crash, no log (the RenderThread stopped-state precedent).
  Layers stopped;
  EXPECT_FALSE(stopped.valid());
  EXPECT_EQ(stopped.maxLayers(), 0u);
  expectRejected(stopped.setLayer(bgDef(0)),
                 laige::ErrorCode::InvalidArgument);
  expectRejected(stopped.setUvOffset(0, {0.5f, 0.0f}),
                 laige::ErrorCode::InvalidArgument);
  SpriteBatcher batcher;
  expectRejected(stopped.declareTo(batcher, {1.0f, 1.0f}),
                 laige::ErrorCode::InvalidArgument);
  stopped.advanceScrolls();  // no-op (no crash)
}

// ---------------------------------------------------------------------------
// ParallaxLayer — the setLayer validation (first failure wins), the
// warn's fields, the state-unchanged-on-rejection, and the replace
// semantics
// ---------------------------------------------------------------------------

template <typename Backend>
void setValidation() {
  using Layers = laige::render::ParallaxLayers<Backend>;
  typename Layers::Options o;
  o.maxLayers = 4;
  auto r = Layers::create(o);
  ASSERT_TRUE(r.ok());
  auto layers = std::move(r).takeValue();

  // The happy path: accepted, introspection, no log.
  auto sink = installCaptureSink();
  ParallaxLayerDef d = bgDef(0);
  EXPECT_TRUE(layers.setLayer(d).ok());
  EXPECT_EQ(sink->entries.size(), 0u) << "the happy path logs nothing";
  EXPECT_TRUE(layers.has(0));
  EXPECT_EQ(layers.layerCount(), 1u);
  EXPECT_TRUE(layers.layerAt(0) == d);
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.0f, 0.0f}));
  restoreLogger();

  // The validation matrix: one capture sink per case, one
  // parallax/layer_invalid warn with the failing field PINNED.
  auto runCase = [&](const ParallaxLayerDef& bad, const char* field) {
    auto s = installCaptureSink();
    expectRejected(layers.setLayer(bad),
                   laige::ErrorCode::InvalidArgument);
    EXPECT_EQ(countEvents(*s, "parallax", "layer_invalid"), 1u)
        << "one warn for the rejection";
    ASSERT_EQ(s->entries.size(), 1u);
    EXPECT_EQ(fieldOf(s->entries.front(), "layer"),
              std::to_string(bad.id));
    EXPECT_EQ(fieldOf(s->entries.front(), "field"), field);
    restoreLogger();
  };
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.id = 4;  // == maxLayers
    runCase(bad, "id");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.factor = -0.1f;
    runCase(bad, "factor");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.factor = 1.1f;
    runCase(bad, "factor");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.factor = std::nanf("");
    runCase(bad, "factor");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.center = {std::nanf(""), 1.0f};
    runCase(bad, "center");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.offset = {1.0f, std::numeric_limits<float>::infinity()};
    runCase(bad, "offset");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.scrollSpeed = {std::nanf(""), 0.0f};
    runCase(bad, "scroll_speed");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.size = {0.0f, 1.0f};
    runCase(bad, "size");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.size = {2.0f, -1.0f};
    runCase(bad, "size");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.size = {std::nanf(""), 2.0f};
    runCase(bad, "size");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.uv = {0.5f, 0.0f, 0.25f, 1.0f};  // u0 > u1
    runCase(bad, "uv");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.uv = {0.0f, 0.0f, 1.1f, 1.0f};  // u1 > 1
    runCase(bad, "uv");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.uv = {0.0f, -0.1f, 1.0f, 1.0f};  // v0 < 0
    runCase(bad, "uv");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.depthLayer = -513;  // below the M2-ISO-01 domain
    runCase(bad, "depth_layer");
  }
  {
    ParallaxLayerDef bad = bgDef(0);
    bad.depthLayer = 512;  // above the M2-ISO-01 domain
    runCase(bad, "depth_layer");
  }

  // The domain EDGES are accepted (the full M2-ISO-01 layer domain,
  // the factor bounds, and the factor extremes):
  {
    ParallaxLayerDef ok = bgDef(1);
    ok.depthLayer = -512;
    EXPECT_TRUE(layers.setLayer(ok).ok());
  }
  {
    ParallaxLayerDef ok = bgDef(2);
    ok.depthLayer = 511;
    EXPECT_TRUE(layers.setLayer(ok).ok());
  }
  {
    ParallaxLayerDef ok = bgDef(3);
    ok.factor = 0.0f;
    EXPECT_TRUE(layers.setLayer(ok).ok());
    ok.factor = 1.0f;
    EXPECT_TRUE(layers.setLayer(ok).ok());
  }
  // The Tilemap source: the geometry is the tilemap's (M2-TILE-02) —
  // a degenerate size/uv is IGNORED, not rejected:
  {
    ParallaxLayerDef ok = bgDef(0);
    ok.source = ParallaxSource::Tilemap;
    ok.tilemapId = 7;
    ok.size = {0.0f, 0.0f};
    ok.uv = {0.5f, 0.0f, 0.2f, 1.0f};  // invalid as an Image
    EXPECT_TRUE(layers.setLayer(ok).ok());
  }

  // A rejection leaves the slot UNCHANGED (validate-before-write):
  auto sink2 = installCaptureSink();
  ParallaxLayerDef one = bgDef(1);
  one.offset = {9.0f, 9.0f};
  EXPECT_TRUE(layers.setLayer(one).ok());
  ParallaxLayerDef bad = bgDef(1);
  bad.factor = 7.0f;
  expectRejected(layers.setLayer(bad),
                 laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(layers.layerAt(1).offset, (glm::vec2{9.0f, 9.0f}));
  // id 0 was replaced by the Tilemap case above (still set); id 2 and
  // 3 are set from the domain-edge cases: the rejected ids were never
  // written, and the layerCount counts SET slots only:
  EXPECT_TRUE(layers.has(0));
  EXPECT_TRUE(layers.has(2));
  EXPECT_TRUE(layers.has(3));
  EXPECT_EQ(layers.layerCount(), 4u);  // ids 0, 1, 2, 3 all set
  EXPECT_EQ(countEvents(*sink2, "parallax", "layer_invalid"), 1u);
  restoreLogger();

  // The replace semantics: the last definition wins, and the scroll
  // offset RESETS (the def carries no scroll state):
  EXPECT_TRUE(layers.setUvOffset(1, {0.5f, 0.0f}).ok());
  ParallaxLayerDef again = bgDef(1);
  again.factor = 0.75f;
  EXPECT_TRUE(layers.setLayer(again).ok());
  EXPECT_EQ(layers.layerAt(1).factor, 0.75f);
  EXPECT_EQ(layers.uvOffsetAt(1), (glm::vec2{0.0f, 0.0f}));
}

TEST(ParallaxLayer, SetValidationFpx16) { setValidation<Fpx16_16>(); }
TEST(ParallaxLayer, SetValidationFp32) { setValidation<Fp32Pinned>(); }

// ---------------------------------------------------------------------------
// ParallaxOffset — the EXACT offset formula: factor * (p - center) +
// offset (the roadmap's pinned contract; the dyadic values are
// bit-exact)
// ---------------------------------------------------------------------------

TEST(ParallaxOffset, Formula) {
  using Layers = laige::render::ParallaxLayers<Fp32Pinned>;
  auto r = Layers::create({});  // 8 slots
  ASSERT_TRUE(r.ok());
  auto layers = std::move(r).takeValue();
  // id 0: factor 0 — FIXED IN WORLD SPACE at `offset` (maximum
  // parallax).
  auto d0 = bgDef(0);
  d0.factor = 0.0f;
  d0.center = {3.0f, -7.0f};
  d0.offset = {2.0f, 5.0f};
  ASSERT_TRUE(layers.setLayer(d0).ok());
  // id 1: factor 1 — FIXED ON SCREEN (moves 1:1 with the camera).
  auto d1 = d0;
  d1.id = 1;
  d1.factor = 1.0f;
  ASSERT_TRUE(layers.setLayer(d1).ok());
  // id 2: factor 0.5 (dyadic).
  auto d2 = d0;
  d2.id = 2;
  d2.factor = 0.5f;
  d2.center = {2.0f, -4.0f};
  d2.offset = {1.0f, 3.0f};
  ASSERT_TRUE(layers.setLayer(d2).ok());
  // id 3: factor 0.25, the world-origin reference.
  auto d3 = d0;
  d3.id = 3;
  d3.factor = 0.25f;
  d3.center = {0.0f, 0.0f};
  d3.offset = {0.5f, -0.25f};
  ASSERT_TRUE(layers.setLayer(d3).ok());
  // id 4: factor 0.3 (NON-dyadic — the linearity check), the
  // world-origin reference (center/offset zeroed — the d0 template
  // carries its own).
  auto d4 = d0;
  d4.id = 4;
  d4.factor = 0.3f;
  d4.center = {0.0f, 0.0f};
  d4.offset = {0.0f, 0.0f};
  ASSERT_TRUE(layers.setLayer(d4).ok());

  // factor 0: the offset for EVERY camera position (exact):
  for (const glm::vec2 p : {glm::vec2{0.0f, 0.0f}, glm::vec2{10.0f, 6.0f},
                            glm::vec2{-4.0f, 4.0f}}) {
    EXPECT_EQ(layers.worldOffsetAt(0, p), (glm::vec2{2.0f, 5.0f}));
  }
  // factor 1: p - center + offset (exact, dyadic):
  //   (10, 6) - (3, -7) + (2, 5) = (9, 18); p = center -> (2, 5).
  EXPECT_EQ(layers.worldOffsetAt(1, {10.0f, 6.0f}), (glm::vec2{9.0f, 18.0f}));
  EXPECT_EQ(layers.worldOffsetAt(1, {3.0f, -7.0f}), (glm::vec2{2.0f, 5.0f}));
  // factor 0.5:
  //   p (10, 2):   0.5 * (8, 6)   + (1, 3) = (5, 6)
  //   p (-6, 8):   0.5 * (-8, 12) + (1, 3) = (-3, 9)
  //   p = center:  (1, 3)
  EXPECT_EQ(layers.worldOffsetAt(2, {10.0f, 2.0f}), (glm::vec2{5.0f, 6.0f}));
  EXPECT_EQ(layers.worldOffsetAt(2, {-6.0f, 8.0f}), (glm::vec2{-3.0f, 9.0f}));
  EXPECT_EQ(layers.worldOffsetAt(2, {2.0f, -4.0f}), (glm::vec2{1.0f, 3.0f}));
  // factor 0.25: p (4, -8): 0.25 * (4, -8) + (0.5, -0.25) = (1.5, -2.25)
  EXPECT_EQ(layers.worldOffsetAt(3, {4.0f, -8.0f}), (glm::vec2{1.5f, -2.25f}));
  // factor 0.3 (non-dyadic — the LINEARITY of the formula, 1e-4
  // float tolerance): offset(p2) - offset(p1) == factor * (p2 - p1),
  // and offset(p1) == factor * p1:
  const glm::vec2 p1{0.1f, 2.0f};
  const glm::vec2 p2{11.3f, 4.2f};
  const glm::vec2 o1 = layers.worldOffsetAt(4, p1);
  const glm::vec2 o2 = layers.worldOffsetAt(4, p2);
  EXPECT_NEAR(o2.x - o1.x, 0.3f * (p2.x - p1.x), 1.0e-4f);
  EXPECT_NEAR(o2.y - o1.y, 0.3f * (p2.y - p1.y), 1.0e-4f);
  EXPECT_NEAR(o1.x, 0.3f * p1.x, 1.0e-4f);
  EXPECT_NEAR(o1.y, 0.3f * p1.y, 1.0e-4f);
}

// ---------------------------------------------------------------------------
// ParallaxScroll — the UV scroll (auto/manual) + the EXACT wrap at the
// texture boundary
// ---------------------------------------------------------------------------

TEST(ParallaxScroll, Wrap) {
  using Layers = laige::render::ParallaxLayers<Fp32Pinned>;
  auto r = Layers::create({});  // 8 slots
  ASSERT_TRUE(r.ok());
  auto layers = std::move(r).takeValue();
  // id 0: AUTO, speed (0.25, 0).
  auto d0 = bgDef(0);
  d0.scrollMode = ParallaxScrollMode::Auto;
  d0.scrollSpeed = {0.25f, 0.0f};
  ASSERT_TRUE(layers.setLayer(d0).ok());
  // id 1: AUTO, speed (0.5, -0.25) (a negative axis).
  auto d1 = d0;
  d1.id = 1;
  d1.scrollSpeed = {0.5f, -0.25f};
  ASSERT_TRUE(layers.setLayer(d1).ok());
  // id 2: MANUAL.
  auto d2 = d0;
  d2.id = 2;
  d2.scrollMode = ParallaxScrollMode::Manual;
  ASSERT_TRUE(layers.setLayer(d2).ok());

  // Auto id 0: 3 advances of 0.25 -> (0.75, 0) EXACT; the 4th crosses
  // the texture boundary: 1.0 wraps to EXACTLY 0.0.
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.0f, 0.0f}));
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.25f, 0.0f}));
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.5f, 0.0f}));
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.75f, 0.0f}));
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.0f, 0.0f}));  // 1.0 -> 0.0
  // Auto id 1: (0.5, -0.25) advances:
  //   1: (0.5, 0.75)   2: (0.0, 0.5)   3: (0.5, 0.25) — all exact
  //   (the negative axis wraps the other way: -0.25 -> 0.75).
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(1), (glm::vec2{0.5f, 0.75f}));
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(1), (glm::vec2{0.0f, 0.5f}));
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(1), (glm::vec2{0.5f, 0.25f}));
  // Manual id 2: advanceScrolls leaves it UNTOUCHED.
  EXPECT_EQ(layers.uvOffsetAt(2), (glm::vec2{0.0f, 0.0f}));

  // Manual: setUvOffset WRAPS any finite value to [0, 1)^2 (exact at
  // the dyadic boundaries):
  EXPECT_TRUE(layers.setUvOffset(2, {1.5f, -0.5f}).ok());
  EXPECT_EQ(layers.uvOffsetAt(2), (glm::vec2{0.5f, 0.5f}));
  EXPECT_TRUE(layers.setUvOffset(2, {1.0f, 0.25f}).ok());
  EXPECT_EQ(layers.uvOffsetAt(2), (glm::vec2{0.0f, 0.25f}));  // 1.0 -> 0.0
  EXPECT_TRUE(layers.setUvOffset(2, {-1.0f, 2.0f}).ok());
  EXPECT_EQ(layers.uvOffsetAt(2), (glm::vec2{0.0f, 0.0f}));
  // A manual set on an AUTO layer composes with the per-frame advance
  // (0.75 + 0.25 -> exactly 0.0):
  EXPECT_TRUE(layers.setUvOffset(0, {0.75f, 0.0f}).ok());
  layers.advanceScrolls();
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.0f, 0.0f}));

  // Non-finite: rejected (InvalidArgument + one rate-limited warn),
  // state unchanged:
  auto sink = installCaptureSink();
  expectRejected(layers.setUvOffset(2, {std::nanf(""), 0.0f}),
                 laige::ErrorCode::InvalidArgument);
  expectRejected(
      layers.setUvOffset(2, {1.0f, std::numeric_limits<float>::infinity()}),
      laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "parallax", "uv_offset_invalid"), 2u);
  EXPECT_EQ(fieldOf(sink->entries.front(), "layer"), "2");
  EXPECT_EQ(layers.uvOffsetAt(2), (glm::vec2{0.0f, 0.0f}));
  // Unknown layer id: InvalidArgument, NO log (precondition):
  auto sink2 = installCaptureSink();
  expectRejected(layers.setUvOffset(3, {0.5f, 0.5f}),
                 laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(sink2->entries.size(), 0u);
  restoreLogger();

  // The offset PERSISTS across frames (it is state, not a per-frame
  // reset): advance, run a full frame, the offset is unchanged.
  EXPECT_TRUE(layers.setUvOffset(0, {0.25f, 0.0f}).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(layers.declareTo(batcher, {1.0f, 1.0f}).ok());
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_EQ(layers.uvOffsetAt(0), (glm::vec2{0.25f, 0.0f}));
}

// ---------------------------------------------------------------------------
// ParallaxDeclare — the batch path: the golden scene (4 layers + a
// ground sprite), the hand-computed golden keys (both backends — the
// dyadic exactness zone), the group (draw) order, the layer-dominance
// orderings, and the cross-frame determinism
// ---------------------------------------------------------------------------

template <typename Backend>
void declareGolden() {
  using Layers = laige::render::ParallaxLayers<Backend>;
  auto r = Layers::create({});  // 8 slots
  ASSERT_TRUE(r.ok());
  auto layers = std::move(r).takeValue();
  // bg: factor 0.25, center (4, 4), offset (1, 2), size 2 x 2.
  ASSERT_TRUE(layers.setLayer(bgDef(0)).ok());
  // mid: factor 0.5, offset (0.5, 0.5), size 1 x 1.
  auto dMid = bgDef(1);
  dMid.factor = 0.5f;
  dMid.center = {0.0f, 0.0f};
  dMid.offset = {0.5f, 0.5f};
  dMid.size = {1.0f, 1.0f};
  dMid.atlasId = 1;
  dMid.depthLayer = kParallaxDepthLayerMidground;
  ASSERT_TRUE(layers.setLayer(dMid).ok());
  // fg: factor 1.0 (screen-fixed), offset (-1, 1), size 0.5 x 0.5.
  auto dFg = bgDef(2);
  dFg.factor = 1.0f;
  dFg.center = {0.0f, 0.0f};
  dFg.offset = {-1.0f, 1.0f};
  dFg.size = {0.5f, 0.5f};
  dFg.atlasId = 2;
  dFg.depthLayer = kParallaxDepthLayerForeground;
  ASSERT_TRUE(layers.setLayer(dFg).ok());
  // custom-fg: factor 0 (fixed in world space), offset (-10, -10),
  // depth layer +1 — v = -19, LOWER than the bg's v = 7, yet its
  // layer (+1) must still sort AFTER the bg (layer dominates).
  auto dCustom = bgDef(kParallaxLayerCustomBase);
  dCustom.factor = 0.0f;
  dCustom.center = {0.0f, 0.0f};
  dCustom.offset = {-10.0f, -10.0f};
  dCustom.size = {1.0f, 1.0f};
  dCustom.atlasId = 3;
  dCustom.depthLayer = kParallaxDepthLayerForeground;
  ASSERT_TRUE(layers.setLayer(dCustom).ok());

  const glm::vec2 p{10.0f, 6.0f};  // the camera's ground position
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();

  auto declareFrame = [&]() {
    batcher.beginFrame();
    ASSERT_TRUE(layers.declareTo(batcher, p).ok());
    // The ground sprite (the world content, layer 0, atlas 4 — the
    // scene's atlas-id convention: the parallax layers' atlas ids
    // ordered bg < ground < fg, "The render order").
    SpriteItem ground{};
    ground.pos = {0.5f, 0.5f};
    ground.scale = {1.0f, 1.0f};
    ground.uv = {0.0f, 0.0f, 1.0f, 1.0f};
    ground.atlasId = 4;
    ground.materialId = 0;
    ground.blend = BlendMode::Alpha;
    ground.depthOverride = false;
    ground.depthKey = oracleKey<Backend>(0.5f, 0.5f, 0);
    auto rAdd = batcher.add(ground);
    ASSERT_TRUE(rAdd.ok());
    ASSERT_TRUE(batcher.build().ok());
  };
  declareFrame();
  ASSERT_EQ(batcher.frameCount(), 5u);  // 4 layer quads + the ground

  // The group (draw) order: ascending (atlas, material, blend) —
  // bg, mid, fg, custom-fg, ground (background FIRST).
  ASSERT_EQ(batcher.batchCount(), 5u);
  const auto batches = batcher.batches();
  const std::uint32_t wantAtlas[5] = {0, 1, 2, 3, 4};
  for (std::size_t g = 0; g < 5; ++g) {
    EXPECT_EQ(batches[g].atlasId, wantAtlas[g]);
    EXPECT_EQ(batches[g].instances.size(), 1u);
  }
  // One instance per group: pin the EXACT quad fields (all uvOffsets
  // (0, 0) — one quad per layer, the full texture).
  struct QuadGolden {
    float cx, cy, sx, sy;
    std::uint32_t key;  // hand-computed (both backends — dyadic)
    std::int32_t layer;
  };
  const std::array<QuadGolden, 5> goldens = {{
      // bg:    offset 0.25 * (6, 2) + (1, 2) = (2.5, 2.5); center
      //        (3.5, 3.5), v = 7, d = 112; l = -2 ->
      //        (510 << 22) | (112 + 2^21) = 0x7F800000 | 0x200070
      {3.5f, 3.5f, 2.0f, 2.0f, 0x7FA00070u, kParallaxDepthLayerBackground},
      // mid:   offset 0.5 * (10, 6) + (0.5, 0.5) = (5.5, 3.5); center
      //        (6, 4), v = 10, d = 160; l = -1 ->
      //        (511 << 22) | (160 + 2^21)
      {6.0f, 4.0f, 1.0f, 1.0f, 0x7FE000A0u, kParallaxDepthLayerMidground},
      // fg:    offset (10, 6) + (-1, 1) = (9, 7); center (9.25, 7.25),
      //        v = 16.5, d = 264; l = +1 ->
      //        (513 << 22) | (264 + 2^21)
      {9.25f, 7.25f, 0.5f, 0.5f, 0x80600108u, kParallaxDepthLayerForeground},
      // custom-fg: offset (-10, -10); center (-9.5, -9.5), v = -19,
      //        d = -304; l = +1 ->
      //        (513 << 22) | (-304 + 2^21)
      {-9.5f, -9.5f, 1.0f, 1.0f, 0x805FFED0u, kParallaxDepthLayerForeground},
      // ground: center (0.5, 0.5), v = 1, d = 16; l = 0 ->
      //        (512 << 22) | (16 + 2^21)
      {0.5f, 0.5f, 1.0f, 1.0f, 0x80200010u, 0},
  }};
  std::uint32_t keys[5] = {0, 0, 0, 0, 0};
  for (std::size_t g = 0; g < 5; ++g) {
    const SpriteItem& item = batcher.at(batches[g].instances[0]);
    const QuadGolden& q = goldens[g];
    EXPECT_EQ(item.pos, (glm::vec2{q.cx, q.cy}));
    EXPECT_EQ(item.scale, (glm::vec2{q.sx, q.sy}));
    EXPECT_EQ(item.rotation, 0.0f);
    expectUvRect("golden quad uv", item.uv, 0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_EQ(item.atlasId, wantAtlas[g]);
    EXPECT_FALSE(item.depthOverride);
    // The golden key (hand-computed — the M2-ISO-01 formula) and the
    // independent oracle (the M2-ISO-01 function on the same center):
    EXPECT_EQ(item.depthKey, q.key);
    EXPECT_EQ(item.depthKey, oracleKey<Backend>(q.cx, q.cy, q.layer));
    // The key's layer field (the background-first contract):
    EXPECT_EQ(laige::render::isoDepthKeyParts(item.depthKey).layer, q.layer);
    keys[g] = item.depthKey;
  }
  // The LAYER DOMINANCE (the engine-guaranteed within-group order,
  // independent of v — the bg's v = 7 is ABOVE the ground's v = 1 and
  // the custom-fg's v = -19, yet its layer sorts them apart):
  EXPECT_LT(keys[0], keys[4]);  // bg    < ground  (l -2 < 0)
  EXPECT_LT(keys[1], keys[4]);  // mid   < ground  (l -1 < 0)
  EXPECT_GT(keys[3], keys[4]);  // custom> ground  (l +1 > 0, v -19 < 1)
  EXPECT_GT(keys[3], keys[0]);  // custom> bg      (l +1 > -2, v -19 < 7)
  EXPECT_LT(keys[0], keys[1]);  // bg    < mid     (l -2 < -1)
  // Machine-greppable summary:
  std::printf(
      "parallax-golden: quads=5 keys bg=0x%08x mid=0x%08x fg=0x%08x "
      "custom=0x%08x ground=0x%08x order=bg<mid<ground<custom<fg (layer "
      "dominance)\n",
      keys[0], keys[1], keys[2], keys[3], keys[4]);

  // Cross-frame DETERMINISM: the same registry state + the same
  // camera position -> bit-identical declarations (every field of
  // every quad).
  const std::array<SpriteItem, 5> first = {{
      batcher.at(batches[0].instances[0]), batcher.at(batches[1].instances[0]),
      batcher.at(batches[2].instances[0]), batcher.at(batches[3].instances[0]),
      batcher.at(batches[4].instances[0])}};
  declareFrame();
  ASSERT_EQ(batcher.frameCount(), 5u);
  const auto second = batcher.batches();
  for (std::size_t g = 0; g < 5; ++g) {
    const SpriteItem& a = first[g];
    const SpriteItem& b = batcher.at(second[g].instances[0]);
    EXPECT_EQ(a.pos, b.pos);
    EXPECT_EQ(a.scale, b.scale);
    expectUvRect("determinism uv", b.uv, a.uv.u0, a.uv.v0, a.uv.u1, a.uv.v1);
    EXPECT_EQ(a.depthKey, b.depthKey);
    EXPECT_EQ(a.atlasId, b.atlasId);
  }
}

TEST(ParallaxDeclare, GoldenFpx16) { declareGolden<Fpx16_16>(); }
TEST(ParallaxDeclare, GoldenFp32) { declareGolden<Fp32Pinned>(); }

// ---------------------------------------------------------------------------
// ParallaxDeclare (ScrolledSplit) — the 2 x 2 wrap split: the exact
// world rects + the exact atlas UV rects at a scrolled offset (the
// wrap is exact at every texture boundary), the degenerate cases,
// and the atlas sub-rect mapping
// ---------------------------------------------------------------------------

template <typename Backend>
void scrolledSplit() {
  using Layers = laige::render::ParallaxLayers<Backend>;
  auto r = Layers::create({});  // 8 slots
  ASSERT_TRUE(r.ok());
  auto layers = std::move(r).takeValue();
  // id 0: factor 0, offset (0, 0) — fixed at the origin — size 2 x 2,
  // the full texture.
  auto d0 = bgDef(0);
  d0.factor = 0.0f;
  d0.offset = {0.0f, 0.0f};
  ASSERT_TRUE(layers.setLayer(d0).ok());
  // id 1: the atlas SUB-RECT layer (uv (0.25, 0, 0.75, 0.5)).
  auto d1 = bgDef(1);
  d1.factor = 0.0f;
  d1.atlasId = 1;
  d1.uv = {0.25f, 0.0f, 0.75f, 0.5f};
  ASSERT_TRUE(layers.setLayer(d1).ok());
  // id 2: the OFFSET layer (offset (3, -1), size 2 x 1).
  auto d2 = bgDef(2);
  d2.factor = 0.0f;
  d2.offset = {3.0f, -1.0f};
  d2.size = {2.0f, 1.0f};
  d2.atlasId = 2;
  ASSERT_TRUE(layers.setLayer(d2).ok());

  SpriteBatcher::Options bo;
  bo.maxSprites = 64;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  const glm::vec2 p{0.0f, 0.0f};  // the layers are factor 0: p is moot

  // The fully-scrolled layer: uvOffset (0.5, 0.25) -> the FULL 2 x 2
  // split (4 quads; the world areas sum to the rectangle's area and
  // the uv rects tile the texture exactly).
  ASSERT_TRUE(layers.setUvOffset(0, {0.5f, 0.25f}).ok());
  batcher.beginFrame();
  ASSERT_TRUE(layers.declareTo(batcher, p).ok());
  ASSERT_TRUE(batcher.build().ok());
  // 4 (scrolled layer 0) + 1 (layer 1, un-scrolled) + 1 (layer 2).
  ASSERT_EQ(batcher.frameCount(), 6u);
  // Layer 0's four quads (the wrap lines at x = 1, y = 1.5), pinned
  // by the slot order (the frame's declaration order: q00, q10, q01,
  // q11 — layers 1 and 2 are un-scrolled here, 4 and 1 quads):
  struct SplitGolden {
    float cx, cy, sx, sy;
    SpriteUvRect uv;  // the full-texture layer: base uv == atlas uv
  };
  const std::array<SplitGolden, 4> split = {{
      {0.5f, 0.75f, 1.0f, 1.5f, {0.5f, 0.25f, 1.0f, 1.0f}},  // q00
      {1.5f, 0.75f, 1.0f, 1.5f, {0.0f, 0.25f, 0.5f, 1.0f}},  // q10
      {0.5f, 1.75f, 1.0f, 0.5f, {0.5f, 0.0f, 1.0f, 0.25f}},  // q01
      {1.5f, 1.75f, 1.0f, 0.5f, {0.0f, 0.0f, 0.5f, 0.25f}},  // q11
  }};
  for (std::uint32_t q = 0; q < 4; ++q) {
    const SpriteItem& item = batcher.at(q);
    const SplitGolden& g = split[q];
    EXPECT_EQ(item.pos, (glm::vec2{g.cx, g.cy}));
    EXPECT_EQ(item.scale, (glm::vec2{g.sx, g.sy}));
    expectUvRect("split quad uv", item.uv, g.uv.u0, g.uv.v0, g.uv.u1,
                 g.uv.v1);
    EXPECT_EQ(item.atlasId, 0u);
  }
  // The wrap is EXACT: the world areas sum to the rectangle's area:
  float worldArea = 0.0f;
  for (std::uint32_t q = 0; q < 4; ++q) {
    const SpriteItem& item = batcher.at(q);
    worldArea += item.scale.x * item.scale.y;
  }
  EXPECT_EQ(worldArea, 2.0f * 2.0f);
  // The sorted order (group atlas 0): q00 (d = 20) first, then the
  // EQUAL-key pair q10/q01 (d = 36 — the stable tie-break is the
  // declaration order: q10 before q01), then q11 (d = 52):
  const auto batches = batcher.batches();
  const SpriteBatch* group0 = nullptr;
  for (const auto& b : batches) {
    if (b.atlasId == 0) {
      group0 = &b;
      break;
    }
  }
  ASSERT_NE(group0, nullptr);
  ASSERT_EQ(group0->instances.size(), 4u);
  EXPECT_EQ(batcher.at(group0->instances[0]).pos, (glm::vec2{0.5f, 0.75f}));  // q00
  EXPECT_EQ(batcher.at(group0->instances[1]).pos, (glm::vec2{1.5f, 0.75f}));  // q10
  EXPECT_EQ(batcher.at(group0->instances[2]).pos, (glm::vec2{0.5f, 1.75f}));  // q01
  EXPECT_EQ(batcher.at(group0->instances[3]).pos, (glm::vec2{1.5f, 1.75f}));  // q11
  EXPECT_EQ(batcher.at(group0->instances[0]).depthKey,
            oracleKey<Backend>(0.5f, 0.75f, kParallaxDepthLayerBackground));
  EXPECT_EQ(batcher.at(group0->instances[1]).depthKey,
            oracleKey<Backend>(1.5f, 0.75f, kParallaxDepthLayerBackground));
  EXPECT_EQ(batcher.at(group0->instances[2]).depthKey,
            oracleKey<Backend>(0.5f, 1.75f, kParallaxDepthLayerBackground));
  EXPECT_EQ(batcher.at(group0->instances[3]).depthKey,
            oracleKey<Backend>(1.5f, 1.75f, kParallaxDepthLayerBackground));

  // The half-scrolled layer: uvOffset (0, 0.25) -> exactly 2 quads
  // (the zero-width quads are skipped):
  batcher.beginFrame();
  ASSERT_TRUE(layers.setUvOffset(0, {0.0f, 0.25f}).ok());
  ASSERT_TRUE(layers.declareTo(batcher, p).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(batcher.frameCount(), 4u);  // 2 + 1 + 1
  const SpriteItem& halfA = batcher.at(0);
  const SpriteItem& halfB = batcher.at(1);
  EXPECT_EQ(halfA.pos, (glm::vec2{1.0f, 0.75f}));  // q00: [0,2) x [0,1.5)
  EXPECT_EQ(halfA.scale, (glm::vec2{2.0f, 1.5f}));
  expectUvRect("half-scrolled q00 uv", halfA.uv, 0.0f, 0.25f, 1.0f, 1.0f);
  EXPECT_EQ(halfB.pos, (glm::vec2{1.0f, 1.75f}));  // q01: [0,2) x [1.5,2)
  EXPECT_EQ(halfB.scale, (glm::vec2{2.0f, 0.5f}));
  expectUvRect("half-scrolled q01 uv", halfB.uv, 0.0f, 0.0f, 1.0f, 0.25f);

  // The un-scrolled layer: uvOffset (0, 0) -> exactly ONE quad (the
  // full rectangle, the full texture):
  batcher.beginFrame();
  ASSERT_TRUE(layers.setUvOffset(0, {0.0f, 0.0f}).ok());
  ASSERT_TRUE(layers.declareTo(batcher, p).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(batcher.frameCount(), 3u);  // 1 + 1 + 1
  const SpriteItem& full = batcher.at(0);
  EXPECT_EQ(full.pos, (glm::vec2{1.0f, 1.0f}));
  EXPECT_EQ(full.scale, (glm::vec2{2.0f, 2.0f}));
  expectUvRect("un-scrolled uv", full.uv, 0.0f, 0.0f, 1.0f, 1.0f);

  // The atlas sub-rect mapping (layer 1, uvOffset (0.5, 0.25), the
  // sub-rect (0.25, 0, 0.75, 0.5)): layer 1's four quads are slots
  // 1..4 (declared after layer 0's single quad):
  batcher.beginFrame();
  ASSERT_TRUE(layers.setUvOffset(1, {0.5f, 0.25f}).ok());
  ASSERT_TRUE(layers.declareTo(batcher, p).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(batcher.frameCount(), 6u);  // 1 + 4 + 1 (layer 0 un-scrolled)
  const std::array<SpriteUvRect, 4> subSplit = {{
      // q00: (0.25 + 0.5*0.5, 0 + 0.25*0.5, 0.75, 0.5)
      {0.5f, 0.125f, 0.75f, 0.5f},
      // q10: (0.25, 0 + 0.25*0.5, 0.25 + 0.5*0.5, 0.5)
      {0.25f, 0.125f, 0.5f, 0.5f},
      // q01: (0.5, 0, 0.75, 0 + 0.25*0.5)
      {0.5f, 0.0f, 0.75f, 0.125f},
      // q11: (0.25, 0, 0.5, 0.125)
      {0.25f, 0.0f, 0.5f, 0.125f},
  }};
  for (std::uint32_t q = 0; q < 4; ++q) {
    expectUvRect("sub-rect split uv", batcher.at(1 + q).uv, subSplit[q].u0,
                 subSplit[q].v0, subSplit[q].u1, subSplit[q].v1);
    EXPECT_EQ(batcher.at(1 + q).atlasId, 1u);
  }

  // The offset layer (layer 2, uvOffset (0, 0)): one quad at the
  // offset world rectangle:
  const SpriteItem& off = batcher.at(5);
  EXPECT_EQ(off.pos, (glm::vec2{4.0f, -0.5f}));  // center of [3,5) x [-1,0)
  EXPECT_EQ(off.scale, (glm::vec2{2.0f, 1.0f}));
  expectUvRect("offset layer uv", off.uv, 0.0f, 0.0f, 1.0f, 1.0f);
  EXPECT_EQ(off.depthKey, oracleKey<Backend>(4.0f, -0.5f,
                                             kParallaxDepthLayerBackground));
}

TEST(ParallaxDeclare, ScrolledSplitFpx16) { scrolledSplit<Fpx16_16>(); }
TEST(ParallaxDeclare, ScrolledSplitFp32) { scrolledSplit<Fp32Pinned>(); }

// ---------------------------------------------------------------------------
// ParallaxDeclare (Protocol) — the window/stopped/disabled/tilemap
// failure paths + the no-log behavior
// ---------------------------------------------------------------------------

template <typename Backend>
void declareProtocol() {
  using Layers = laige::render::ParallaxLayers<Backend>;
  auto r = Layers::create({});
  ASSERT_TRUE(r.ok());
  auto layers = std::move(r).takeValue();
  // A disabled layer + a Tilemap-source layer (the M2-TILE-02 hook).
  auto dOff = bgDef(1);
  dOff.enabled = false;
  ASSERT_TRUE(layers.setLayer(dOff).ok());
  auto dTile = bgDef(2);
  dTile.source = ParallaxSource::Tilemap;
  dTile.tilemapId = 9;
  ASSERT_TRUE(layers.setLayer(dTile).ok());
  ASSERT_TRUE(layers.setLayer(bgDef(0)).ok());

  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();

  // The disabled + tilemap layers declare NOTHING (no log):
  auto sink = installCaptureSink();
  batcher.beginFrame();
  EXPECT_TRUE(layers.declareTo(batcher, {0.0f, 0.0f}).ok());
  EXPECT_EQ(batcher.frameCount(), 1u);  // only layer 0
  EXPECT_EQ(sink->entries.size(), 0u) << "the skipped layers log nothing";
  // A built frame: the window is closed — nothing declared.
  EXPECT_TRUE(batcher.build().ok());
  expectRejected(layers.declareTo(batcher, {0.0f, 0.0f}),
                 laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(batcher.frameCount(), 1u);
  // The stopped batcher: the first add fails (BudgetExhausted),
  // nothing declared.
  SpriteBatcher stopped;  // the default (stopped) batcher: capacity 0
  expectRejected(layers.declareTo(stopped, {0.0f, 0.0f}),
                 laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(stopped.frameCount(), 0u);
  restoreLogger();

  // The stopped registry: InvalidArgument.
  Layers stoppedLayers;
  expectRejected(stoppedLayers.declareTo(batcher, {0.0f, 0.0f}),
                 laige::ErrorCode::InvalidArgument);
}

TEST(ParallaxDeclare, ProtocolFpx16) { declareProtocol<Fpx16_16>(); }
TEST(ParallaxDeclare, ProtocolFp32) { declareProtocol<Fp32Pinned>(); }

// ---------------------------------------------------------------------------
// ParallaxZeroAlloc — the per-frame declare loop allocates nothing
// ---------------------------------------------------------------------------

// The first offending site, resolved to its module + symbol when the
// platform provides dladdr (POSIX — the tilemap test's path): the
// actionable context for the failure. Named distinctly from the
// tilemap test's same-role global helper (one TU per test file, one
// shared executable).
#if !defined(_MSC_VER)
std::string describeParallaxAllocSite(const void* site) {
  if (site == nullptr) {
    return "<null site>";
  }
  Dl_info info;
  if (dladdr(const_cast<void*>(const_cast<void* const&>(site)), &info) != 0) {
    std::string out;
    out += (info.dli_fname != nullptr) ? info.dli_fname : "<module>";
    out += " @ ";
    out += (info.dli_sname != nullptr) ? info.dli_sname : "<symbol>";
    out += " (addr ";
    out += std::to_string(reinterpret_cast<std::uintptr_t>(site));
    out += ")";
    return out;
  }
  return "<unresolved site " +
         std::to_string(reinterpret_cast<std::uintptr_t>(site)) + ">";
}
#else
std::string describeParallaxAllocSite(const void* site) {
  return "<unresolved site " +
         std::to_string(reinterpret_cast<std::uintptr_t>(site)) + ">";
}
#endif

template <typename Backend>
void declareLoopAllocatesNothing() {
  using Layers = laige::render::ParallaxLayers<Backend>;
  auto r = Layers::create({});  // 8 slots
  ASSERT_TRUE(r.ok());
  auto layers = std::move(r).takeValue();
  // One AUTO layer (the full 2 x 2 split per frame — 4 quads) + two
  // manual layers (1 quad each) + two sprites: 8 items per frame.
  auto d0 = bgDef(0);
  d0.scrollMode = ParallaxScrollMode::Auto;
  d0.scrollSpeed = {0.25f, 0.25f};  // crosses the wrap boundary often
  ASSERT_TRUE(layers.setLayer(d0).ok());
  auto d1 = bgDef(1);
  d1.atlasId = 1;
  ASSERT_TRUE(layers.setLayer(d1).ok());
  auto d2 = bgDef(2);
  d2.atlasId = 2;
  ASSERT_TRUE(layers.setLayer(d2).ok());
  SpriteBatcher::Options bo;
  bo.maxSprites = 64;
  auto rb = SpriteBatcher::create(bo);
  ASSERT_TRUE(rb.ok());
  auto batcher = std::move(rb).takeValue();
  auto runFrames = [&]() {
    for (std::int32_t frame = 0; frame < 1000; ++frame) {
      layers.advanceScrolls();
      batcher.beginFrame();
      ASSERT_TRUE(layers.declareTo(batcher, {1.0f, -1.0f}).ok())
          << "frame " << frame;
      SpriteItem s{};
      s.pos = {0.5f, 0.5f};
      s.scale = {1.0f, 1.0f};
      s.uv = {0.0f, 0.0f, 1.0f, 1.0f};
      s.atlasId = 3;
      s.depthKey = oracleKey<Backend>(0.5f, 0.5f, 0);
      ASSERT_TRUE(batcher.add(s).ok()) << "frame " << frame;
      ASSERT_TRUE(batcher.build().ok()) << "frame " << frame;
    }
  };
  // Zero-allocation proof (where the watch is live — the non-
  // sanitizer trees; the sanitizer runtimes own operator new):
  // 1000 frames of the declare loop allocate nothing (the registry's
  // slot table and the batcher's frame storage are pre-allocated —
  // FR-2.2 "no per-frame allocation").
  //
  // The window's owner is THIS thread: the watch counts owner-thread
  // allocations only (the attribution contract, alloc_watch.h).
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    runFrames();
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "1000 frames of advanceScrolls/beginFrame/declareTo/build "
        << "allocated " << reading.allocs
        << " heap blocks on the loop thread (first site: "
        << describeParallaxAllocSite(reading.firstSite) << ")";
  }
}

TEST(ParallaxZeroAlloc, DeclareLoopAllocatesNothing) {
  declareLoopAllocatesNothing<Fpx16_16>();
  declareLoopAllocatesNothing<Fp32Pinned>();
}
