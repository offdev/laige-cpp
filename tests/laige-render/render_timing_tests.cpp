// laige-render tests (M2-PROF-01): CPU/GPU render timing (RENDER-005) —
// the per-pass CPU phase times from the render-thread timers (batch,
// sort, submit), the opt-in per-pass GPU time via the GL TIME_ELAPSED
// query object (one frame late; "n/a" when unavailable — never a fake
// number), the implicit upload-sync stall detection (one rate-limited
// Warn per stalled frame), and the profiler feed of the render timing.
//
// Suite map (the `render_timing` CTest entry selects exactly these):
//   RenderTimingCreate   the stallThresholdUs create validation (first
//                        failure wins, one Warn) + the stopped state
//                        reads zero — no GL required (the validation
//                        precedes the context check);
//   RenderTimingCpu      the batch/sort/submit CPU phase times on the
//                        known small scene (10 sprites, 2 atlases, 2
//                        blends -> 3 groups): the per-frame values are
//                        non-negative, the since-construction totals
//                        accumulate EXACTLY, the empty frame records
//                        nothing, and the profiler feed of the render
//                        values accumulates — requires a usable
//                        OpenGL 3.3 environment (GTEST_SKIPs on an
//                        environment failure);
//   RenderTimingGpu      the opt-in GL TIME_ELAPSED pass time: off by
//                        default (n/a, no GL work), on: the
//                        one-frame-late semantics (the previous pass's
//                        value is read on the next submit) pinned under
//                        glFinish-paced frames — GL required;
//   RenderTimingStall   the implicit upload-sync stall detection: a
//                        4096-sprite frame under a 1 us threshold
//                        stalls (one Warn per stalled frame, the pinned
//                        fields), and a small frame under the default
//                        threshold does not — GL required;
//   RenderTimingSlowShader the roadmap's integration check: with a
//                        deliberately slow shader (a 16384-iteration
//                        dependent sin loop), the GPU-side pass time
//                        EXCEEDS the CPU-side submit time on a
//                        hardware-GL runner (GL required; SKIPS on a
//                        software renderer — there the CPU is the GPU
//                        and the driver is synchronous, so the
//                        observation is undemonstrable).
//
// The scene setup pattern (makeScene): the caller OWNS the context,
// the renderer, and the batcher — the renderer stores a NON-OWNING
// pointer to the context (the header's Ownership section), so the
// context must never move (the render_counters_tests pattern). An
// engine-side setup failure aborts (the environment check has
// already GTEST_SKIPped).

#include <glad/gl.h>

#include <array>
#include <chrono>
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
#include "laige/sim/profiler.h"

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
using laige::render::SpriteBatchTiming;
using laige::render::SpriteBatcher;
using laige::render::SpriteDrawStats;
using laige::render::SpriteDrawTotals;
using laige::render::SpriteItem;
using laige::render::SpriteRenderer;

// The offscreen render target (the sprite_draw scene size).
constexpr std::int32_t kWidth = 128;
constexpr std::int32_t kHeight = 128;
// The per-instance upload (the M2-SPRITE-02 instance layout, 13 floats).
constexpr std::uint64_t kInstanceBytes = 52u;

// The test phase clock (steady_clock ns since epoch — the same clock
// the engine's timing fields use).
std::int64_t nowNs() {
  return std::chrono::steady_clock::now().time_since_epoch().count();
}

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

// One declared sprite at (x, y) (the engine-owned depth key).
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

// The known small scene (10 sprites, 2 atlases, 2 blends -> 3 groups,
// the render_counters_tests scene).
std::vector<SpriteItem> sceneItems() {
  std::vector<SpriteItem> items;
  items.reserve(10);
  items.push_back(makeSprite(0, 0, 0, BlendMode::Alpha));
  items.push_back(makeSprite(1, 0, 0, BlendMode::Alpha));
  items.push_back(makeSprite(0, 1, 0, BlendMode::Alpha));
  items.push_back(makeSprite(1, 1, 0, BlendMode::Alpha));
  items.push_back(makeSprite(2, 0, 0, BlendMode::Additive));
  items.push_back(makeSprite(3, 0, 0, BlendMode::Additive));
  items.push_back(makeSprite(2, 1, 0, BlendMode::Additive));
  items.push_back(makeSprite(0, 2, 1, BlendMode::Alpha));
  items.push_back(makeSprite(1, 2, 1, BlendMode::Alpha));
  items.push_back(makeSprite(0, 3, 1, BlendMode::Alpha));
  return items;
}

// The 4096-sprite single-group scene of the stall tests (one atlas,
// one blend: one instanced draw, 4096 * 52 B per upload).
std::vector<SpriteItem> largeSceneItems() {
  std::vector<SpriteItem> items;
  items.reserve(4096);
  for (std::uint32_t i = 0; i < 4096; ++i) {
    items.push_back(makeSprite(static_cast<std::int32_t>(i % 64),
                               static_cast<std::int32_t>(i / 64),
                               /*atlasId=*/0, BlendMode::Alpha));
  }
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

// The scene setup on caller-owned objects (the file preamble).
void makeScene(const GlContext& ctx, SpriteRenderer& renderer,
               SpriteBatcher& batcher, Mat4& matrix,
               const SpriteRenderer::Options& ro, std::uint32_t maxSprites) {
  auto r = SpriteRenderer::create(ctx, ro);
  if (r.isError()) {
    ADD_FAILURE() << "SpriteRenderer::create failed: "
                  << laige::errorText(r.error());
    abort();
  }
  renderer = std::move(r).takeValue();
  SpriteBatcher::Options bo;
  bo.maxSprites = maxSprites;
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
Result<GlContext> tryContext() { return GlContext::createHeadless(kWidth, kHeight); }

// ------------------------------------------------------------------------
// Log capture (the render_counters_tests MemorySink pattern — rate
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

// The slow shader of the RenderTimingSlowShader fixture (RENDER-005):
// a 16384-iteration DEPENDENT sin loop per fragment — the GPU does the
// work asynchronously (the CPU-side draw call just queues it).
constexpr const char* kPassVertex =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
constexpr const char* kSlowFragment =
    "#version 330 core\n"
    "out vec4 outColor;\n"
    "void main() {\n"
    "  float v = 1.0;\n"
    "  for (int i = 0; i < 16384; ++i) {\n"
    "    v = sin(v * 0.01);\n"
    "  }\n"
    "  outColor = vec4(v, v * 0.5, 0.0, 1.0);\n"
    "}\n";

}  // namespace

// ------------------------------------------------------------------------
// RenderTimingCreate — the stallThresholdUs create validation + the
// stopped state (no GL required: the options are validated before the
// context check, the stopped state makes no GL calls).
// ------------------------------------------------------------------------

TEST(RenderTimingCreate, StallThresholdValidation) {
  GlContext stopped;  // the stopped state — no GL environment needed
  MemorySink* sink = installCaptureSink();

  SpriteRenderer::Options o;
  o.maxInstances = 16;
  o.maxAtlases = 2;
  o.stallThresholdUs = 0;  // < 1
  Result<SpriteRenderer> r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 1u);
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "option"),
            "stallThresholdUs");
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "value"), "0");

  // The documented default is valid (the domain's lower bound is 1;
  // the default is 2000 us) — the create reaches the context check, so
  // the stopped context fails GlUnavailable, not InvalidArgument.
  o.stallThresholdUs = SpriteRenderer::kSpriteRendererDefaultStallUs;
  r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::GlUnavailable);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 1u);

  // First failure wins: maxInstances AND stallThresholdUs invalid ->
  // one warn, the FIRST option (maxInstances precedes stallThresholdUs).
  o.maxInstances = 0;
  o.stallThresholdUs = 0;
  r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 2u);
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "option"),
            "maxInstances");
}

TEST(RenderTimingCreate, StoppedStateReadsZero) {
  SpriteRenderer r;
  EXPECT_FALSE(r.valid());
  const SpriteDrawStats s = r.frameStats();
  EXPECT_DOUBLE_EQ(s.submitMs, 0.0);
  EXPECT_EQ(s.uploadWallUs, 0u);
  EXPECT_FALSE(s.uploadStalled);
  EXPECT_EQ(s.gpuTimeUs, 0u);
  EXPECT_FALSE(s.gpuTimeValid);
  const SpriteDrawTotals t = r.totals();
  EXPECT_DOUBLE_EQ(t.submitMs, 0.0);
  EXPECT_EQ(t.uploadStallUs, 0u);
  EXPECT_EQ(t.uploadStallFrames, 0u);
  EXPECT_EQ(t.gpuTimeUs, 0u);
  EXPECT_EQ(t.gpuTimeFrames, 0u);

  // The batcher's stopped state: the timing fields read zero (a
  // stopped batcher's empty-frame build records no timing).
  SpriteBatcher b;
  const SpriteBatchTiming bt = b.frameTiming();
  EXPECT_DOUBLE_EQ(bt.batchMs, 0.0);
  EXPECT_DOUBLE_EQ(bt.sortMs, 0.0);
  const auto tt = b.timingTotals();
  EXPECT_EQ(tt.frames, 0u);
  EXPECT_DOUBLE_EQ(tt.batchMs, 0.0);
  EXPECT_DOUBLE_EQ(tt.sortMs, 0.0);
}

// ------------------------------------------------------------------------
// RenderTimingCpu — the batch/sort/submit CPU phase times on the known
// small scene (GL required — GTEST_SKIPs on an environment failure).
// ------------------------------------------------------------------------

TEST(RenderTimingCpu, SmallScenePhaseTimes) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  const SpriteRenderer::Options ro{};  // defaults: gpuTiming off
  makeScene(ctx, renderer, batcher, matrix, ro, /*maxSprites=*/16);

  const std::vector<SpriteItem> items = sceneItems();
  // Frame 1.
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteBatchTiming bt1 = batcher.frameTiming();
  const SpriteDrawStats s1 = renderer.frameStats();
  EXPECT_GE(bt1.batchMs, 0.0);
  EXPECT_GE(bt1.sortMs, 0.0);
  EXPECT_GT(s1.submitMs, 0.0);  // the submit's GL work takes time
  EXPECT_GE(s1.uploadWallUs, 0u);
  EXPECT_FALSE(s1.uploadStalled);  // the default 2000 us threshold

  // Frame 2 (the since-construction totals accumulate EXACTLY).
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteBatchTiming bt2 = batcher.frameTiming();
  const SpriteDrawStats s2 = renderer.frameStats();
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.frames, 2u);
  EXPECT_DOUBLE_EQ(t.submitMs, s1.submitMs + s2.submitMs);
  EXPECT_EQ(t.uploadStallUs, 0u);
  EXPECT_EQ(t.uploadStallFrames, 0u);
  EXPECT_EQ(t.gpuTimeUs, 0u);  // the gpuTiming option is off (n/a)
  EXPECT_EQ(t.gpuTimeFrames, 0u);
  const auto tt = batcher.timingTotals();
  EXPECT_EQ(tt.frames, 2u);
  EXPECT_DOUBLE_EQ(tt.batchMs, bt1.batchMs + bt2.batchMs);
  EXPECT_DOUBLE_EQ(tt.sortMs, bt1.sortMs + bt2.sortMs);

  // The profiler feed of the render values (the engine wiring, M2).
  laige::Profiler prof(laige::Profiler::Options{});
  prof.addRenderBatchMs(bt1.batchMs);
  prof.addRenderSortMs(bt1.sortMs);
  prof.addRenderSubmitMs(s1.submitMs);
  prof.setRenderTextureMemory(renderer.textureMemoryBytes());
  prof.addRenderBatchMs(bt2.batchMs);
  prof.addRenderSortMs(bt2.sortMs);
  prof.addRenderSubmitMs(s2.submitMs);
  prof.setRenderTextureMemory(renderer.textureMemoryBytes());
  const laige::ProfilerStats ps = prof.snapshot();
  EXPECT_DOUBLE_EQ(ps.renderBatchMs, bt1.batchMs + bt2.batchMs);
  EXPECT_DOUBLE_EQ(ps.renderSortMs, bt1.sortMs + bt2.sortMs);
  EXPECT_DOUBLE_EQ(ps.renderSubmitMs, s1.submitMs + s2.submitMs);
  EXPECT_EQ(ps.renderGpuFrames, 0u);  // no GPU measurement completed (n/a)
  EXPECT_EQ(ps.renderTextureMemoryBytes, renderer.textureMemoryBytes());

  // The built empty frame: the frame stats record nothing (all new
  // fields zero), the renderer's timing totals are unchanged (only
  // the frame count moves), and the batcher's build still records its
  // (trivial) phase times.
  declareFrame(batcher, {});
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s3 = renderer.frameStats();
  EXPECT_DOUBLE_EQ(s3.submitMs, 0.0);
  EXPECT_EQ(s3.uploadWallUs, 0u);
  EXPECT_FALSE(s3.uploadStalled);
  EXPECT_EQ(s3.gpuTimeUs, 0u);
  EXPECT_FALSE(s3.gpuTimeValid);
  const SpriteDrawTotals t3 = renderer.totals();
  EXPECT_EQ(t3.frames, 3u);
  EXPECT_DOUBLE_EQ(t3.submitMs, s1.submitMs + s2.submitMs);
  EXPECT_EQ(batcher.timingTotals().frames, 3u);
}

// ------------------------------------------------------------------------
// RenderTimingGpu — the opt-in GL TIME_ELAPSED pass time (GL required).
// ------------------------------------------------------------------------

TEST(RenderTimingGpu, OffByDefaultIsNA) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  const SpriteRenderer::Options ro{};  // gpuTiming off (the default)
  makeScene(ctx, renderer, batcher, matrix, ro, /*maxSprites=*/16);

  const std::vector<SpriteItem> items = sceneItems();
  for (int f = 0; f < 2; ++f) {
    declareFrame(batcher, items);
    EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
    const SpriteDrawStats s = renderer.frameStats();
    EXPECT_FALSE(s.gpuTimeValid) << "frame " << (f + 1);  // n/a, no GL work
    EXPECT_EQ(s.gpuTimeUs, 0u) << "frame " << (f + 1);
  }
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.gpuTimeFrames, 0u);  // no completed measurement
  EXPECT_EQ(t.gpuTimeUs, 0u);
}

TEST(RenderTimingGpu, OneFrameLateMeasurements) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  SpriteRenderer renderer;
  SpriteBatcher batcher;
  Mat4 matrix{};
  SpriteRenderer::Options ro{};
  ro.gpuTiming = true;
  makeScene(ctx, renderer, batcher, matrix, ro, /*maxSprites=*/16);

  const std::vector<SpriteItem> items = sceneItems();
  // The frame-pipeline pattern: the render target is cleared before
  // the first query-enabled FBO draw (the llvmpipe lazy pipe
  // initialization race — the RenderCountersScene pattern; the clear
  // touches no sprite-pass state).
  EXPECT_TRUE(ctx.clear(0, 0, 0, 0).ok());

  // Frame 1: its measurement is BEGUN (nothing to read yet — the
  // first frame is always n/a).
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s1 = renderer.frameStats();
  EXPECT_FALSE(s1.gpuTimeValid);
  EXPECT_EQ(s1.gpuTimeUs, 0u);

  // glFinish: the GPU is idle, so frame 1's measurement is complete —
  // frame 2's submit reads it (the one-frame-late semantics) and
  // begins frame 2's measurement.
  glFinish();
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s2 = renderer.frameStats();
  EXPECT_TRUE(s2.gpuTimeValid);
  EXPECT_GT(s2.gpuTimeUs, 0u);  // the pass's GL work took GPU time
  const std::uint64_t m1 = s2.gpuTimeUs;

  // Same for frame 2 -> frame 3.
  glFinish();
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s3 = renderer.frameStats();
  EXPECT_TRUE(s3.gpuTimeValid);
  EXPECT_GT(s3.gpuTimeUs, 0u);
  const std::uint64_t m2 = s3.gpuTimeUs;

  // The since-construction totals: the two COMPLETED measurements
  // (frame 1's value read on frame 2, frame 2's on frame 3).
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.gpuTimeFrames, 2u);
  EXPECT_EQ(t.gpuTimeUs, m1 + m2);
}

// ------------------------------------------------------------------------
// RenderTimingStall — the implicit upload-sync stall detection (GL
// required).
// ------------------------------------------------------------------------

TEST(RenderTimingStall, LargeFrameUploadStalls) {
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
  SpriteRenderer::Options ro{};
  ro.maxInstances = 8192;
  ro.stallThresholdUs = 1;  // an artificial 1 us threshold
  makeScene(ctx, renderer, batcher, matrix, ro, /*maxSprites=*/8192);

  // The 4096-sprite frame: 4096 * 52 B = 212 992 B per upload. The
  // upload's CPU wall time exceeds the 1 us threshold on every P0
  // runner (the CPU-side copy of 212 KB alone is tens of us), so both
  // frames stall — one Warn each (the capture sink has rate limiting
  // OFF) + the pinned fields.
  const std::vector<SpriteItem> items = largeSceneItems();
  const std::uint64_t uploadBytes = 4096u * kInstanceBytes;

  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s1 = renderer.frameStats();
  EXPECT_TRUE(s1.uploadStalled);
  EXPECT_GT(s1.uploadWallUs, ro.stallThresholdUs);
  EXPECT_EQ(s1.uploadBytes, uploadBytes);

  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s2 = renderer.frameStats();
  EXPECT_TRUE(s2.uploadStalled);
  EXPECT_GT(s2.uploadWallUs, ro.stallThresholdUs);
  EXPECT_EQ(s2.uploadBytes, uploadBytes);

  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "submit_stall"), 2u);
  const MemorySink::Entry* e = lastEvent(*sink, "submit_stall");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->severity, laige::log::Severity::Warn);
  EXPECT_EQ(fieldOf(*e, "stall_us"), std::to_string(s2.uploadWallUs));
  EXPECT_EQ(fieldOf(*e, "upload_bytes"), std::to_string(uploadBytes));

  // The since-construction stall totals (successful submits only).
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.uploadStallFrames, 2u);
  EXPECT_EQ(t.uploadStallUs, s1.uploadWallUs + s2.uploadWallUs);
}

TEST(RenderTimingStall, SmallFrameDoesNotStall) {
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
  const SpriteRenderer::Options ro{};  // the default 2000 us threshold
  makeScene(ctx, renderer, batcher, matrix, ro, /*maxSprites=*/16);

  // The 10-sprite frame: a 520 B upload — the wall time is far below
  // the documented default threshold on every P0 runner.
  const std::vector<SpriteItem> items = sceneItems();
  declareFrame(batcher, items);
  EXPECT_TRUE(renderer.submit(batcher, matrix).ok());
  const SpriteDrawStats s = renderer.frameStats();
  EXPECT_EQ(s.uploadBytes, 10u * kInstanceBytes);
  EXPECT_LT(s.uploadWallUs, ro.stallThresholdUs);
  EXPECT_FALSE(s.uploadStalled);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "submit_stall"), 0u);
  EXPECT_EQ(renderer.totals().uploadStallFrames, 0u);
  EXPECT_EQ(renderer.totals().uploadStallUs, 0u);
}

// ------------------------------------------------------------------------
// RenderTimingSlowShader — the roadmap's integration check (GL
// required): the deliberately slow shader's GPU-side pass time EXCEEDS
// its CPU-side submit time on a hardware-GL runner.
// ------------------------------------------------------------------------

TEST(RenderTimingSlowShader, GpuTimeExceedsCpuTime) {
  auto cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();
  // The roadmap's observation is defined for the desktop CI runner's
  // hardware GL (RENDER-005: CPU and GPU measured independently). On a
  // software renderer (llvmpipe/swrast) the CPU IS the GPU and the
  // driver is synchronous — the draw call's wall time already contains
  // the GPU work, so the observation is undemonstrable there.
  const char* renderer = ctx.capabilities().renderer;
  const char* vendor = ctx.capabilities().vendor;
  if (std::strstr(renderer, "llvmpipe") != nullptr ||
      std::strstr(renderer, "swrast") != nullptr ||
      std::strstr(vendor, "Mesa") != nullptr) {
    GTEST_SKIP() << "software renderer (" << renderer
                 << "): the GPU>CPU observation requires a hardware-GL "
                    "runner";
  }
  ASSERT_TRUE(ctx.makeCurrent().ok());

  // The shader (engine-side failure after the environment check: the
  // context is a live 3.3 core — a compile failure here is a test bug).
  const GLenum kinds[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
  const char* sources[2] = {kPassVertex, kSlowFragment};
  GLuint shaders[2] = {0, 0};
  for (int i = 0; i < 2; ++i) {
    shaders[i] = glCreateShader(kinds[i]);
    glShaderSource(shaders[i], 1, &sources[i], nullptr);
    glCompileShader(shaders[i]);
    GLint compiled = 0;
    glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
      ADD_FAILURE() << "shader compile failed";
      abort();
    }
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, shaders[0]);
  glAttachShader(program, shaders[1]);
  glDeleteShader(shaders[0]);
  glDeleteShader(shaders[1]);
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == 0) {
    ADD_FAILURE() << "program link failed";
    abort();
  }

  // The full-screen triangle + its VAO.
  const float verts[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
  GLuint vbo = 0;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(verts)),
               verts, GL_STATIC_DRAW);
  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         nullptr);
  glEnableVertexAttribArray(0);

  // The render target (the context's offscreen FBO).
  glBindFramebuffer(GL_FRAMEBUFFER, ctx.frameBuffer());
  glViewport(0, 0, kWidth, kHeight);
  glUseProgram(program);
  // The warm-up draw: the program's first use (shader setup) is kept
  // out of the measured window.
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glFinish();

  // The measured draw: the CPU wall time of the draw call vs the
  // GPU-side TIME_ELAPSED interval of the same call.
  GLuint query = 0;
  glGenQueries(1, &query);
  if (query == 0) {
    ADD_FAILURE() << "glGenQueries failed";
    abort();
  }
  glBeginQuery(GL_TIME_ELAPSED, query);
  const std::int64_t cpuStartNs = nowNs();
  glDrawArrays(GL_TRIANGLES, 0, 3);
  const std::int64_t cpuEndNs = nowNs();
  // GL 3.3 core's one-argument form: ends the target's pending query
  // (the 2-argument form is GL 4.0+).
  glEndQuery(GL_TIME_ELAPSED);
  glFinish();  // the diagnostic sync: the result is available now
  std::uint32_t gpuUs = 0;
  GLint available = 0;
  glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
  if (available != 0) {
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &gpuUs);
  }
  const std::uint64_t cpuUs =
      static_cast<std::uint64_t>((cpuEndNs - cpuStartNs) / 1000);
  std::printf("slow-shader gpu_us=%llu cpu_us=%llu\n",
              static_cast<unsigned long long>(gpuUs),
              static_cast<unsigned long long>(cpuUs));
  // The GPU does the slow shader's work asynchronously: the
  // measurement interval exceeds the CPU-side draw call's wall time
  // (the roadmap's integration check).
  EXPECT_GT(gpuUs, cpuUs);

  // Leave no hidden GL state (the pass's program/VAO are its own).
  glDeleteQueries(1, &query);
  glDeleteVertexArrays(1, &vao);
  glDeleteBuffers(1, &vbo);
  glDeleteProgram(program);
  glUseProgram(0);
  glBindVertexArray(0);
}
