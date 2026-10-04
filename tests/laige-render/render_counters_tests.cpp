// laige-render tests (M2-SPRITE-04): render observability + the
// draw-call budget (G-R2, PRD §9.3) — the M2-SPRITE-04 profiler
// fields (program changes, upload volume, render-target use, the
// texture-memory VRAM estimate, the G-R2 draw-call-cap flag) and the
// per-pass draw-call cap (configurable, documented default — exceeding
// it WARNS + flags the frame, it never drops the frame).
//
// Suite map (the `render_counters` CTest entry selects exactly these):
//   RenderCountersCreate  the create validation of the new option
//                         (first failure wins, one Warn) + the
//                         stopped state reads zero — no GL required
//                         (the validation precedes the context check);
//   RenderCountersScene   the roadmap's known small scene (10 sprites,
//                         2 atlases, 2 blends -> 3 groups): the
//                         per-frame counters match EXACTLY (frame 1 +
//                         frame 2, the cross-frame state persistence),
//                         the since-construction totals exact, the
//                         empty frame counts nothing, and the opt-in
//                         PRIMITIVES_GENERATED feed — requires a
//                         usable OpenGL 3.3 environment (GTEST_SKIPs
//                         on an environment failure, the documented
//                         contract);
//   RenderCountersCap     the G-R2 per-pass draw-call cap: the warn
//                         fires at the configured count (with the
//                         pinned fields), the frame is still drawn
//                         (observation, never an execution gate), the
//                         flag + total track the exceedances, and a
//                         frame AT the cap does not warn;
//   RenderCountersMemory  the texture-memory VRAM estimate gauge:
//                         the bound-atlases w*h*4 sum, exact through
//                         binds and re-bind replacements.
//
// The scene setup pattern (makeScene): the caller OWNS the context,
// the renderer, and the batcher — the renderer stores a NON-OWNING
// pointer to the context (the header's Ownership section), so the
// context must never move (a moved-from context is the stopped
// state: makeCurrent fails InvalidArgument). The existing
// SpriteDraw tests use the same single-scope pattern; an engine-side
// setup failure aborts (the environment check has already GTEST_SKIPped).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "laige/render/gl_context.h"
#include "laige/render/iso_camera.h"
#include "laige/render/iso_depth_key.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/sprite_renderer.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim_math.h"

namespace {

using laige::ErrorCode;
using laige::Result;
using laige::Status;
using laige::render::BlendMode;
using laige::render::GlContext;
using laige::render::IsoCamera;
using laige::render::IsoCameraOptions;
using laige::render::IsoPreset;
using laige::render::Mat4;
using laige::render::SpriteBatcher;
using laige::render::SpriteDrawStats;
using laige::render::SpriteDrawTotals;
using laige::render::SpriteItem;
using laige::render::SpriteRenderer;

// The offscreen render target (the sprite_draw scene size — the
// render-target-use expectation is kWidth * kHeight * 4).
constexpr std::int32_t kWidth = 128;
constexpr std::int32_t kHeight = 128;
constexpr std::uint64_t kRenderTargetBytes =
    static_cast<std::uint64_t>(kWidth) * kHeight * 4u;
// The per-instance upload (the M2-SPRITE-02 instance layout, 13 floats).
constexpr std::uint64_t kInstanceBytes = 52u;

// A 4x4 solid RGBA8 atlas (all alpha 255).
std::array<std::uint8_t, 64> solidAtlas4(std::uint8_t r, std::uint8_t g,
                                         std::uint8_t b) {
  std::array<std::uint8_t, 64> a{};
  for (std::size_t p = 0; p < 16; ++p) {
    a[p * 4 + 0] = r;
    a[p * 4 + 1] = g;
    a[p * 4 + 2] = b;
    a[p * 4 + 3] = 255;
  }
  return a;
}

template <std::size_t N>
std::span<const std::uint8_t> bytesOf(const std::array<std::uint8_t, N>& a) {
  return std::span<const std::uint8_t>(a.data(), a.size());
}

// One declared sprite at (x, y) (the engine-owned depth key — the
// G-R11 contract: never hand-rolled, never screen space).
SpriteItem makeSprite(std::int32_t x, std::int32_t y, std::uint32_t atlasId,
                      BlendMode blend) {
  SpriteItem it;
  it.pos = {static_cast<float>(x), static_cast<float>(y)};
  it.depthKey = laige::render::isoDepthKey<laige::sim::Fp32Pinned>(
      laige::sim::SimMath<laige::sim::Fp32Pinned>::Vec2{
          static_cast<float>(x), static_cast<float>(y)},
      /*stepHeight=*/0, /*layer=*/0);
  it.uv = {0.0f, 0.0f, 1.0f, 1.0f};
  it.scale = {1.0f, 1.0f};
  it.rotation = 0.0f;
  it.tint = {1.0f, 1.0f, 1.0f, 1.0f};
  it.atlasId = atlasId;
  it.materialId = 0;
  it.blend = blend;
  return it;
}

// The roadmap's known small scene: 10 sprites, 2 atlases, 2 blends —
// 3 (atlas, material, blend) groups in the batcher's ascending
// (atlas, material, blend) order:
//   (0, 0, Alpha):    4 sprites at (0,0),(1,0),(0,1),(1,1)
//   (0, 0, Additive): 3 sprites at (2,0),(3,0),(2,1)
//   (1, 0, Alpha):    3 sprites at (0,2),(1,2),(0,3)
std::vector<SpriteItem> sceneItems() {
  std::vector<SpriteItem> items;
  items.reserve(10);
  // Group (0, 0, Alpha): 4 sprites.
  items.push_back(makeSprite(0, 0, 0, BlendMode::Alpha));
  items.push_back(makeSprite(1, 0, 0, BlendMode::Alpha));
  items.push_back(makeSprite(0, 1, 0, BlendMode::Alpha));
  items.push_back(makeSprite(1, 1, 0, BlendMode::Alpha));
  // Group (0, 0, Additive): 3 sprites.
  items.push_back(makeSprite(2, 0, 0, BlendMode::Additive));
  items.push_back(makeSprite(3, 0, 0, BlendMode::Additive));
  items.push_back(makeSprite(2, 1, 0, BlendMode::Additive));
  // Group (1, 0, Alpha): 3 sprites.
  items.push_back(makeSprite(0, 2, 1, BlendMode::Alpha));
  items.push_back(makeSprite(1, 2, 1, BlendMode::Alpha));
  items.push_back(makeSprite(0, 3, 1, BlendMode::Alpha));
  return items;
}

// The 2-group scene of the cap tests: 4 sprites —
//   (0, 0, Alpha):    2 sprites at (0,0),(1,0)
//   (0, 0, Additive): 2 sprites at (0,1),(1,1)
std::vector<SpriteItem> twoGroupItems() {
  std::vector<SpriteItem> items;
  items.reserve(4);
  items.push_back(makeSprite(0, 0, 0, BlendMode::Alpha));
  items.push_back(makeSprite(1, 0, 0, BlendMode::Alpha));
  items.push_back(makeSprite(0, 1, 0, BlendMode::Additive));
  items.push_back(makeSprite(1, 1, 0, BlendMode::Additive));
  return items;
}

// Declare the frame (the batch stage protocol) and build it.
void declareFrame(SpriteBatcher& batcher,
                  const std::vector<SpriteItem>& items) {
  batcher.beginFrame();
  for (const SpriteItem& it : items) {
    const auto slot = batcher.add(it);
    if (slot.isError()) {
      ADD_FAILURE() << "batcher.add failed: " << laige::errorText(slot.error());
      abort();
    }
  }
  if (batcher.build().isError()) {
    ADD_FAILURE() << "batcher.build failed";
    abort();
  }
}

// The scene setup on caller-owned objects (the file preamble: the
// context must never move — the renderer stores a non-owning pointer
// to it). Any engine-side failure aborts (the caller GTEST_SKIPped on
// an environment failure).
void makeScene(const GlContext& ctx, SpriteRenderer& renderer,
               SpriteBatcher& batcher, Mat4& matrix, std::uint32_t maxDrawCalls,
               bool primitiveQuery) {
  SpriteRenderer::Options o;
  o.maxInstances = 64;
  o.maxAtlases = 2;
  o.maxDrawCalls = maxDrawCalls;
  o.primitiveQuery = primitiveQuery;
  auto r = SpriteRenderer::create(ctx, o);
  if (r.isError()) {
    ADD_FAILURE() << "SpriteRenderer::create failed: "
                  << laige::errorText(r.error());
    abort();
  }
  renderer = std::move(r).takeValue();
  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto br = SpriteBatcher::create(bo);
  if (br.isError()) {
    ADD_FAILURE() << "SpriteBatcher::create failed: "
                  << laige::errorText(br.error());
    abort();
  }
  batcher = std::move(br).takeValue();
  const std::array<std::uint8_t, 64> a0 = solidAtlas4(255, 128, 0);
  const std::array<std::uint8_t, 64> a1 = solidAtlas4(33, 66, 222);
  const Status bind0 = renderer.bindAtlas(0, 4, 4, bytesOf(a0));
  if (bind0.isError()) {
    ADD_FAILURE() << "bindAtlas(0) failed: " << laige::errorText(bind0.error());
    abort();
  }
  const Status bind1 = renderer.bindAtlas(1, 4, 4, bytesOf(a1));
  if (bind1.isError()) {
    ADD_FAILURE() << "bindAtlas(1) failed: " << laige::errorText(bind1.error());
    abort();
  }
  IsoCameraOptions co;
  co.preset = IsoPreset{laige::render::IsoPresetKind::Dimetric2To1, 0.125f};
  const Result<IsoCamera> c = IsoCamera::create(co);
  if (c.isError()) {
    ADD_FAILURE() << "IsoCamera::create failed: " << laige::errorText(c.error());
    abort();
  }
  matrix = c.value().matrix();
}

// A live offscreen context (the documented environment contract —
// the calling TEST body GTEST_SKIPs on an environment failure, never
// on an engine failure).
Result<GlContext> tryContext() {
  return GlContext::createHeadless(kWidth, kHeight);
}

// ------------------------------------------------------------------------
// Log capture (the sprite_batcher_tests MemorySink pattern — rate
// limiting OFF so the tests assert per-event counts, not the facade's
// LOG-004 window).
// ------------------------------------------------------------------------
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

std::size_t countEvents(const MemorySink& sink, std::string_view subsystem,
                        std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

const MemorySink::Entry* lastEvent(const MemorySink& sink,
                                  std::string_view event) {
  const MemorySink::Entry* last = nullptr;
  for (const auto& e : sink.entries) {
    if (e.event == event) last = &e;
  }
  return last;
}

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

// ------------------------------------------------------------------------
// The small scene's per-frame expectations (the file preamble's scene).
// The last-atlas / last-blend state PERSISTS across frames (the
// M2-SPRITE-02 state model): frame 1 ends on atlas 1 / blend Alpha,
// so frame 2 sets 2 texture binds (atlas 1 -> 0 -> 1) + 2 blend
// changes (Alpha -> Additive -> Alpha), while frame 1 set 2 binds +
// 3 changes.
// ------------------------------------------------------------------------
struct FrameExpectations {
  std::uint32_t drawCalls;
  std::uint32_t textureBinds;
  std::uint32_t blendChanges;
  std::uint32_t programChanges;
  std::uint32_t instances;
  std::uint64_t uploadBytes;
  std::uint64_t renderTargetBytes;
};

const FrameExpectations kFrame1 = {
    /*drawCalls=*/3, /*textureBinds=*/2, /*blendChanges=*/3,
    /*programChanges=*/1, /*instances=*/10,
    /*uploadBytes=*/10 * kInstanceBytes, /*renderTargetBytes=*/kRenderTargetBytes};
const FrameExpectations kFrame2 = {
    /*drawCalls=*/3, /*textureBinds=*/2, /*blendChanges=*/2,
    /*programChanges=*/1, /*instances=*/10,
    /*uploadBytes=*/10 * kInstanceBytes, /*renderTargetBytes=*/kRenderTargetBytes};

void checkFrame(const SpriteRenderer& renderer, const char* frame,
                const FrameExpectations& e) {
  const SpriteDrawStats s = renderer.frameStats();
  EXPECT_EQ(s.drawCalls, e.drawCalls) << frame;
  EXPECT_EQ(s.textureBinds, e.textureBinds) << frame;
  EXPECT_EQ(s.blendChanges, e.blendChanges) << frame;
  EXPECT_EQ(s.programChanges, e.programChanges) << frame;
  EXPECT_EQ(s.instances, e.instances) << frame;
  EXPECT_EQ(s.uploadBytes, e.uploadBytes) << frame;
  EXPECT_EQ(s.renderTargetBytes, e.renderTargetBytes) << frame;
  EXPECT_FALSE(s.drawCallCapExceeded) << frame;
}

}  // namespace

// ------------------------------------------------------------------------
// RenderCountersCreate — the create validation of the new option +
// the stopped state (no GL required: the options are validated before
// the context check, the stopped state makes no GL calls).
// ------------------------------------------------------------------------

TEST(RenderCountersCreate, MaxDrawCallsValidation) {
  GlContext stopped;  // the stopped state — no GL environment needed
  MemorySink* sink = installCaptureSink();

  SpriteRenderer::Options o;
  o.maxInstances = 16;
  o.maxAtlases = 2;
  o.maxDrawCalls = 0;  // < 1
  Result<SpriteRenderer> r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 1u);
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "option"),
            "maxDrawCalls");
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "value"), "0");

  // The documented default is valid (the domain's lower bound is 1;
  // the default is 64) — the create reaches the context check, so the
  // stopped context fails GlUnavailable, not InvalidArgument.
  o.maxDrawCalls = SpriteRenderer::kSpriteRendererDefaultDrawCalls;
  r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::GlUnavailable);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 1u);

  // First failure wins: maxInstances AND maxDrawCalls invalid ->
  // one warn, the FIRST option (maxInstances precedes maxDrawCalls).
  o.maxInstances = 0;
  o.maxDrawCalls = 0;
  r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 2u);
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "option"),
            "maxInstances");
}

TEST(RenderCountersCreate, StoppedStateReadsZero) {
  SpriteRenderer r;
  EXPECT_FALSE(r.valid());
  EXPECT_EQ(r.maxInstances(), 0u);
  EXPECT_EQ(r.maxAtlases(), 0u);
  EXPECT_EQ(r.maxDrawCalls(), 0u);
  EXPECT_EQ(r.textureMemoryBytes(), 0u);
  const SpriteDrawStats s = r.frameStats();
  EXPECT_EQ(s.drawCalls, 0u);
  EXPECT_EQ(s.textureBinds, 0u);
  EXPECT_EQ(s.blendChanges, 0u);
  EXPECT_EQ(s.programChanges, 0u);
  EXPECT_EQ(s.instances, 0u);
  EXPECT_EQ(s.primitives, 0u);
  EXPECT_EQ(s.uploadBytes, 0u);
  EXPECT_EQ(s.renderTargetBytes, 0u);
  EXPECT_FALSE(s.drawCallCapExceeded);
  const SpriteDrawTotals t = r.totals();
  EXPECT_EQ(t.frames, 0u);
  EXPECT_EQ(t.drawCalls, 0u);
  EXPECT_EQ(t.textureBinds, 0u);
  EXPECT_EQ(t.blendChanges, 0u);
  EXPECT_EQ(t.programChanges, 0u);
  EXPECT_EQ(t.instances, 0u);
  EXPECT_EQ(t.primitives, 0u);
  EXPECT_EQ(t.uploadBytes, 0u);
  EXPECT_EQ(t.renderTargetBytes, 0u);
  EXPECT_EQ(t.capExceededFrames, 0u);
}

// ------------------------------------------------------------------------
// RenderCountersScene — the known small scene (10 sprites, 2 atlases,
// 2 blends) with EXACT per-frame + since-construction counters (GL
// required — GTEST_SKIPs on an environment failure).
// ------------------------------------------------------------------------

TEST(RenderCountersScene, TenSpritesExactCounters) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  makeScene(ctx, renderer, batcher, matrix,
            SpriteRenderer::kSpriteRendererDefaultDrawCalls,
            /*primitiveQuery=*/false);

  const std::vector<SpriteItem> items = sceneItems();
  // Frame 1 (the fresh-state frame: 3 blend functions + 2 binds).
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  checkFrame(renderer, "frame 1", kFrame1);
  EXPECT_EQ(renderer.frameStats().primitives, 0u);  // the query is off
  EXPECT_EQ(renderer.maxDrawCalls(),
            SpriteRenderer::kSpriteRendererDefaultDrawCalls);

  // Frame 2 (the state persists: 2 binds + 2 blend changes).
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  checkFrame(renderer, "frame 2", kFrame2);

  // The since-construction totals (successful submits only).
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.frames, 2u);
  EXPECT_EQ(t.drawCalls,
            static_cast<std::uint64_t>(kFrame1.drawCalls + kFrame2.drawCalls));
  EXPECT_EQ(t.textureBinds,
            static_cast<std::uint64_t>(kFrame1.textureBinds + kFrame2.textureBinds));
  EXPECT_EQ(t.blendChanges,
            static_cast<std::uint64_t>(kFrame1.blendChanges + kFrame2.blendChanges));
  EXPECT_EQ(t.programChanges,
            static_cast<std::uint64_t>(kFrame1.programChanges + kFrame2.programChanges));
  EXPECT_EQ(t.instances,
            static_cast<std::uint64_t>(kFrame1.instances + kFrame2.instances));
  EXPECT_EQ(t.primitives, 0u);
  EXPECT_EQ(t.uploadBytes,
            static_cast<std::uint64_t>(kFrame1.uploadBytes + kFrame2.uploadBytes));
  EXPECT_EQ(t.renderTargetBytes,
            static_cast<std::uint64_t>(kFrame1.renderTargetBytes +
                                       kFrame2.renderTargetBytes));
  EXPECT_EQ(t.capExceededFrames, 0u);
}

TEST(RenderCountersScene, EmptyFrameCountsNothing) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  makeScene(ctx, renderer, batcher, matrix,
            SpriteRenderer::kSpriteRendererDefaultDrawCalls,
            /*primitiveQuery=*/false);

  // A built empty frame: counted as a frame, NOTHING else (no GL
  // state is touched — the early return precedes the pass setup).
  declareFrame(batcher, {});
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats st = renderer.frameStats();
  EXPECT_EQ(st.drawCalls, 0u);
  EXPECT_EQ(st.textureBinds, 0u);
  EXPECT_EQ(st.blendChanges, 0u);
  EXPECT_EQ(st.programChanges, 0u);
  EXPECT_EQ(st.instances, 0u);
  EXPECT_EQ(st.primitives, 0u);
  EXPECT_EQ(st.uploadBytes, 0u);
  EXPECT_EQ(st.renderTargetBytes, 0u);
  EXPECT_FALSE(st.drawCallCapExceeded);
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.frames, 1u);
  EXPECT_EQ(t.drawCalls, 0u);
  EXPECT_EQ(t.uploadBytes, 0u);
  EXPECT_EQ(t.renderTargetBytes, 0u);
  EXPECT_EQ(t.capExceededFrames, 0u);
}

TEST(RenderCountersScene, PrimitiveQueryFeed) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  makeScene(ctx, renderer, batcher, matrix,
            SpriteRenderer::kSpriteRendererDefaultDrawCalls,
            /*primitiveQuery=*/true);

  // The opt-in PRIMITIVES_GENERATED cross-check: 2 per instance of
  // the 4-vertex strip (20 for the 10-sprite scene).
  const std::vector<SpriteItem> items = sceneItems();
  declareFrame(batcher, items);
  // The frame-pipeline pattern: the render target is cleared before
  // the draw (the real frame loop always clears first). Required
  // here for a second reason — a query-enabled submit as the FIRST
  // FBO operation leaves the Mesa llvmpipe worker's lazy pipe
  // initialization in a state that races the context's teardown
  // under TSan (a driver-internal data race, both accesses inside
  // libgallium — verified on CI's Mesa 25.2.8 and this machine's
  // Mesa 26.2.3). A clear before the draw (the
  // ThousandSpriteFrame pattern) leaves the teardown clean. The
  // clear touches no sprite-pass state — the counters below are
  // unaffected.
  EXPECT_TRUE(ctx.clear(0, 0, 0, 0).ok());
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s1 = renderer.frameStats();
  EXPECT_EQ(s1.primitives, 20u);
  EXPECT_EQ(s1.drawCalls, 3u);
  EXPECT_EQ(s1.instances, 10u);
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.primitives, 20u);
  EXPECT_EQ(t.frames, 1u);
}

// ------------------------------------------------------------------------
// RenderCountersCap — the G-R2 per-pass draw-call cap (GL required).
// ------------------------------------------------------------------------

TEST(RenderCountersCap, WarnAtConfiguredCount) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  MemorySink* sink = installCaptureSink();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  makeScene(ctx, renderer, batcher, matrix,
            /*maxDrawCalls=*/2, /*primitiveQuery=*/false);
  EXPECT_EQ(renderer.maxDrawCalls(), 2u);

  const std::vector<SpriteItem> items = sceneItems();  // 3 groups > cap 2
  // Frame 1: the cap is exceeded — the frame is STILL drawn (the
  // observation, never an execution gate), one Warn with the pinned
  // fields, the flag + total track it.
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  EXPECT_TRUE(renderer.frameStats().drawCallCapExceeded);
  EXPECT_EQ(renderer.frameStats().drawCalls, 3u);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "draw_call_cap"), 1u);
  const MemorySink::Entry* e = lastEvent(*sink, "draw_call_cap");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->severity, laige::log::Severity::Warn);
  EXPECT_EQ(fieldOf(*e, "capacity"), "2");
  EXPECT_EQ(fieldOf(*e, "draw_calls"), "3");
  EXPECT_EQ(renderer.totals().capExceededFrames, 1u);

  // Frame 2: the exceedance repeats — a second Warn (the capture
  // sink has rate limiting OFF) + the total.
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  EXPECT_TRUE(renderer.frameStats().drawCallCapExceeded);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "draw_call_cap"), 2u);
  EXPECT_EQ(renderer.totals().capExceededFrames, 2u);

  // A frame AT OR BELOW the cap: no flag, no new Warn. The 2-group
  // scene (4 sprites) draws 2 draw calls == the cap (strictly not
  // above). The last-atlas / last-blend state persists from the
  // 3-group frames (last atlas 1 / blend Alpha): this frame's state
  // = 1 bind (atlas 1 -> 0) + 1 blend change (the first group's Alpha
  // is unchanged, the second group sets Additive).
  const std::vector<SpriteItem> small = twoGroupItems();
  declareFrame(batcher, small);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  EXPECT_FALSE(renderer.frameStats().drawCallCapExceeded);
  EXPECT_EQ(renderer.frameStats().drawCalls, 2u);
  EXPECT_EQ(renderer.frameStats().instances, 4u);
  EXPECT_EQ(renderer.frameStats().textureBinds, 1u);
  EXPECT_EQ(renderer.frameStats().blendChanges, 1u);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "draw_call_cap"), 2u);
  EXPECT_EQ(renderer.totals().capExceededFrames, 2u);
}

TEST(RenderCountersCap, NoWarnAtTheCap) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  MemorySink* sink = installCaptureSink();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  makeScene(ctx, renderer, batcher, matrix,
            /*maxDrawCalls=*/3, /*primitiveQuery=*/false);

  // The 3-group scene AT the cap of 3: the cap is an upper bound
  // (exceeding means strictly above) — no Warn, no flag, drawn.
  const std::vector<SpriteItem> items = sceneItems();
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  EXPECT_FALSE(renderer.frameStats().drawCallCapExceeded);
  EXPECT_EQ(renderer.frameStats().drawCalls, 3u);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "draw_call_cap"), 0u);
  EXPECT_EQ(renderer.totals().capExceededFrames, 0u);
}

// ------------------------------------------------------------------------
// RenderCountersMemory — the texture-memory VRAM estimate gauge (GL
// required: bindAtlas is the set-up/asset path's GL upload).
// ------------------------------------------------------------------------

TEST(RenderCountersMemory, TextureMemoryGauge) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  makeScene(ctx, renderer, batcher, matrix,
            SpriteRenderer::kSpriteRendererDefaultDrawCalls,
            /*primitiveQuery=*/false);

  // The two 4x4 atlases of the setup: 2 * (4 * 4 * 4) = 128 bytes.
  EXPECT_EQ(renderer.textureMemoryBytes(), 128u);

  // A second renderer on the same context (the atlas registry is
  // per-renderer): one 8x8 atlas (256 B) + the re-bind replacement
  // (16x16 = 1024 B replaces the 8x8 — 256 - 256 + 1024 = 1024).
  SpriteRenderer::Options o;
  o.maxInstances = 16;
  o.maxAtlases = 2;
  auto r = SpriteRenderer::create(ctx, o);
  if (r.isError()) {
    ADD_FAILURE() << "SpriteRenderer::create failed: "
                  << laige::errorText(r.error());
    abort();
  }
  SpriteRenderer big = std::move(r).takeValue();
  EXPECT_EQ(big.textureMemoryBytes(), 0u);
  const std::array<std::uint8_t, 256> a8 = [] {
    std::array<std::uint8_t, 256> a{};
    for (std::size_t p = 0; p < 64; ++p) {
      a[p * 4 + 0] = 9;
      a[p * 4 + 1] = 18;
      a[p * 4 + 2] = 27;
      a[p * 4 + 3] = 255;
    }
    return a;
  }();
  ASSERT_TRUE(big.bindAtlas(0, 8, 8, bytesOf(a8)).ok());
  EXPECT_EQ(big.textureMemoryBytes(), 256u);
  const std::array<std::uint8_t, 1024> a16 = [] {
    std::array<std::uint8_t, 1024> a{};
    for (std::size_t p = 0; p < 256; ++p) {
      a[p * 4 + 0] = 36;
      a[p * 4 + 1] = 72;
      a[p * 4 + 2] = 108;
      a[p * 4 + 3] = 255;
    }
    return a;
  }();
  ASSERT_TRUE(big.bindAtlas(0, 16, 16, bytesOf(a16)).ok());
  EXPECT_EQ(big.textureMemoryBytes(), 1024u);

  // The render-target use is per submit (the scene test pins the
  // exact 128 * 128 * 4 bytes) — nothing asserted on it here.
  (void)renderer;
  (void)batcher;
  (void)matrix;
}
