// laige-render tests (M2-SPRITE-02): the GPU instanced draw submits +
// the minimal GLSL 3.30 sprite shader (the frame pipeline's submit
// stage, laige/render/sprite_renderer.h).
//
// Suite map (the `sprite_draw` CTest entry selects exactly these):
//   SpriteDrawCreate  create validation (the options, first failure
//                     wins, one Warn), the stopped-context failure
//                     (clean GlUnavailable — no GL required: the
//                     validation precedes the context check), and the
//                     stopped-state behavior;
//   SpriteDrawState   the submit precondition contract on a live
//                     context (requires a usable OpenGL 3.3
//                     environment — always present on the P0 CI
//                     runners, where it must pass; on a local machine
//                     without a GL driver the suite GTEST_SKIPs with
//                     the clean Status reason — the documented
//                     environment contract, not an engine failure):
//                     the built-frame gate (an unbuilt window is never
//                     drawn as an empty frame, CORE-008), the
//                     unbound-atlas rejection, the empty frame, the
//                     instance-budget BudgetExhausted + rate-limited
//                     Warn, the bindAtlas argument domain, and the
//                     rebind-replaces semantics;
//   SpriteDrawSmoke   the roadmap's "1000-sprite scene" offscreen
//                     render: a 128x128 headless FBO, the 2:1 dimetric
//                     preset, 1000 sprites in 3 (atlas, material,
//                     blend) groups, one instanced draw per group
//                     (the dispatch count — the engine's own
//                     drawCalls counter — == the group count,
//                     cross-checked against the opt-in GL
//                     PRIMITIVES_GENERATED count == 2 x instances),
//                     and the WHOLE frame compared pixel-by-pixel
//                     against a CPU reference rasterizer (the
//                     non-empty, correct frame — the M2-GOLD-01
//                     fixture lands on top of this);
//   SpriteDrawPipeline  the integration path through the M2-GL-02
//                     RenderThread: the batch stage (batcher build) +
//                     the submit stage (renderer submit) + the
//                     onStart/onStop GL handoff, 100 headless frames of
//                     the same scene, the since-construction totals
//                     exact, and the last frame's pixel still correct
//                     after the render thread released the context
//                     (TSan race-freedom under the tsan tree).

#include <algorithm>
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

#include "laige/render/frame_pipeline.h"
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
using laige::render::RenderThread;
using laige::render::RenderThreadOptions;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using laige::render::SpriteDrawStats;
using laige::render::SpriteDrawTotals;
using laige::render::SpriteItem;
using laige::render::SpriteRenderer;
using laige::render::Vec2;

// ------------------------------------------------------------------------
// The test scene (the roadmap's "1000-sprite scene").
//
// The 2:1 dimetric lattice of the world cells (x, y) in -16..15 —
// lattice index i = (y + 16) * 32 + (x + 16), i in 0..999 — MINUS the
// three probe cells, PLUS three probes declared LAST (the (key,
// declaration) total order puts them on top of the equal-key lattice
// cells). At preset scale k = 0.125 (zoom 1) a 1x1 world tile projects
// to a 4k x 2k NDC rhombus = 32 x 16 px at 128x128; the tiles tile the
// whole viewport exactly (a lattice tiling — the pixel centers never
// land on a tile edge, so no half-coverage ambiguity).
//
// Groups (the batcher's ascending (atlas, material, blend) order):
//   (0, 0, Alpha):    i < 600 minus the probes in range + P0  = 598
//   (0, 0, Additive): 600 <= i < 800 + P1                     = 201
//   (1, 0, Alpha):    i >= 800 + P2                           = 201
// so: 3 groups, 1000 instances, 2 texture binds, 3 blend changes
// (Alpha -> Additive -> Alpha, the first set counted), 2000
// generated primitives (2 per instance of the 4-vertex strip).
//
// The probes (exact-color pixels — the reference-independent checks):
//   P0 at (0, 0):  atlas 0, Alpha,    uv = texel (0, 0)  -> (255,128,0)
//   P1 at (0, 1):  atlas 0, Additive, uv = texel (2, 0)  -> (255,128,0)
//                                  + the clear (0,0,255) -> (255,128,255)
//   P2 at (0, -1): atlas 1, Alpha,    uv = texel (0, 0)  -> (33,66,222)
constexpr std::int32_t kWidth = 128;
constexpr std::int32_t kHeight = 128;
constexpr float kPresetScale = 0.125f;
// The lattice indices of the probe cells (the scene builder's holes).
constexpr std::uint32_t kProbe0Index = 528;  // (0, 0)
constexpr std::uint32_t kProbe1Index = 560;  // (0, 1)
constexpr std::uint32_t kProbe2Index = 496;  // (0, -1)

// The clear color: a distinctive blue (bytes 0, 0, 255 — the GL
// 3.3 §8.3 exact conversion: 1.0 -> 255, 0.0 -> 0).
constexpr float kClearR = 0.0f;
constexpr float kClearG = 0.0f;
constexpr float kClearB = 1.0f;
constexpr float kClearA = 1.0f;

// Atlas 0: a 4x4 checkerboard (row 0 = v = 0, the bottom — the GL
// upload convention, no flip). Even (i + j): (255, 128, 0); odd:
// (0, 128, 255). All alpha 255.
std::array<std::uint8_t, 64> atlas0Bytes() {
  std::array<std::uint8_t, 64> b{};
  for (std::uint32_t j = 0; j < 4; ++j) {
    for (std::uint32_t i = 0; i < 4; ++i) {
      const std::array<std::uint8_t, 4> col =
          ((i + j) % 2 == 0) ? std::array<std::uint8_t, 4>{255, 128, 0, 255}
                             : std::array<std::uint8_t, 4>{0, 128, 255, 255};
      std::memcpy(&b[(j * 4 + i) * 4], col.data(), 4);
    }
  }
  return b;
}

// Atlas 1: a 4x4 solid (33, 66, 222, 255).
std::array<std::uint8_t, 64> atlas1Bytes() {
  std::array<std::uint8_t, 64> b{};
  for (std::size_t p = 0; p < 16; ++p) {
    b[p * 4 + 0] = 33;
    b[p * 4 + 1] = 66;
    b[p * 4 + 2] = 222;
    b[p * 4 + 3] = 255;
  }
  return b;
}

std::span<const std::uint8_t> bytesOf(const std::array<std::uint8_t, 64>& a) {
  return std::span<const std::uint8_t>(a.data(), a.size());
}

// The texel color of the test atlases (the reference rasterizer's
// texture fetch — the GL_NEAREST, no-mipmap, CLAMP_TO_EDGE contract:
// the floor of the uv in 4-px texel space).
std::array<std::uint8_t, 4> atlasTexel(std::uint32_t atlasId,
                                       std::uint32_t tx, std::uint32_t ty) {
  tx = std::min<std::uint32_t>(tx, 3);
  ty = std::min<std::uint32_t>(ty, 3);
  if (atlasId == 1) {
    return {33, 66, 222, 255};
  }
  return ((tx + ty) % 2 == 0) ? std::array<std::uint8_t, 4>{255, 128, 0, 255}
                              : std::array<std::uint8_t, 4>{0, 128, 255, 255};
}

// One declared probe sprite (the exact-color pixel of its quad's
// interior — the scene comment).
SpriteItem makeProbe(std::int32_t x, std::int32_t y, float u0, float v0,
                     float u1, float v1, std::uint32_t atlasId,
                     BlendMode blend) {
  SpriteItem it;
  it.pos = {static_cast<float>(x), static_cast<float>(y)};
  it.depthKey = laige::render::isoDepthKey<laige::sim::Fp32Pinned>(
      laige::sim::SimMath<laige::sim::Fp32Pinned>::Vec2{
          static_cast<float>(x), static_cast<float>(y)},
      /*stepHeight=*/0, /*layer=*/0);
  it.uv = {u0, v0, u1, v1};
  it.scale = {1.0f, 1.0f};
  it.rotation = 0.0f;
  it.tint = {1.0f, 1.0f, 1.0f, 1.0f};
  it.atlasId = atlasId;
  it.materialId = 0;
  it.blend = blend;
  return it;
}

// The 1000-sprite scene in declaration order (the batcher's entity-id
// order — the stable tie-break's carrier). Deterministic; built once.
std::vector<SpriteItem> buildSceneItems() {
  std::vector<SpriteItem> items;
  items.reserve(1000);
  for (std::uint32_t i = 0; i < 1000; ++i) {
    if (i == kProbe0Index || i == kProbe1Index || i == kProbe2Index) {
      continue;  // the probe cells (the probes below take them)
    }
    const std::int32_t x = static_cast<std::int32_t>(i % 32) - 16;
    const std::int32_t y = static_cast<std::int32_t>(i / 32) - 16;
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
    it.atlasId = (i >= 800) ? 1 : 0;
    it.materialId = 0;
    it.blend = (i >= 600 && i < 800) ? BlendMode::Additive
                                     : BlendMode::Alpha;
    items.push_back(it);
  }
  // The probes (declared LAST — the (key, declaration) total order
  // puts them on top of the equal-key lattice cells).
  items.push_back(
      makeProbe(0, 0, 0.0f, 0.0f, 0.25f, 0.25f, 0, BlendMode::Alpha));
  items.push_back(
      makeProbe(0, 1, 0.5f, 0.0f, 0.75f, 0.25f, 0, BlendMode::Additive));
  items.push_back(
      makeProbe(0, -1, 0.0f, 0.0f, 0.25f, 0.25f, 1, BlendMode::Alpha));
  return items;  // 997 lattice + 3 probes = 1000
}

// The frame's world -> NDC matrix: the ADR 0005 default preset at the
// test scale, the default camera state (position (0, 0, 0), zoom 1,
// no shake) — matrix() == isoDimetric2To1(k).
Mat4 sceneMatrix() {
  IsoCameraOptions o;
  o.preset = IsoPreset{laige::render::IsoPresetKind::Dimetric2To1,
                       kPresetScale};
  const Result<IsoCamera> r = IsoCamera::create(o);
  if (r.isError()) {
    ADD_FAILURE() << "IsoCamera::create failed: "
                  << laige::errorText(r.error());
    abort();
  }
  return r.value().matrix();
}

// A live offscreen context (the documented environment contract — the
// calling TEST body GTEST_SKIPs on error, not on an engine failure).
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

const MemorySink::Entry* firstEvent(const MemorySink& sink,
                                   std::string_view event) {
  for (const auto& e : sink.entries) {
    if (e.event == event) return &e;
  }
  return nullptr;
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
// The CPU reference rasterizer (the smoke test's oracle): for one
// pixel, walk the BUILT frame in the exact draw order (the batcher's
// published group order, the in-group order), test the pixel-center
// NDC against each sprite's rotated NDC quad, and accumulate the
// per-group blend in double (the GPU's float32 differs by <= 1 byte
// after the RGBA8 conversion — the smoke test's ±1 tolerance; the
// probe pixels are rounding-exact and asserted exactly).
// ------------------------------------------------------------------------
struct RefQuad {
  double corners[8];  // 4 x (x, y), CYCLIC (the interior test's edges)
  double cx{0.0};
  double cy{0.0};
  double c{1.0};
  double s{0.0};
  double u0{0.0};
  double v0{0.0};
  double u1{0.0};
  double v1{0.0};
  double sx{1.0};
  double sy{1.0};
  BlendMode blend{BlendMode::Alpha};
  std::uint32_t atlasId{0};
  // The matrix's 2x2 linear part (the UV recovery's inverse):
  //   NDC.x = a00 * x + a10 * y + a30
  //   NDC.y = a01 * x + a11 * y + a31
  double a00{1.0};
  double a10{0.0};
  double a01{0.0};
  double a11{1.0};
  // The quad's bounding box (the fast reject).
  double minX{0.0};
  double maxX{0.0};
  double minY{0.0};
  double maxY{0.0};
};

// Build the reference quad from one item + the world -> NDC matrix
// (the shader's exact math, in double: the items' floats and the
// matrix's floats convert to double exactly). The matrix is
// column-major: the point (x, y, 0, 1) maps to
//   px = m[0][0]*x + m[1][0]*y + m[3][0]
//   py = m[0][1]*x + m[1][1]*y + m[3][1]
// (the z row is the iso zero row — the NDC-z always-0 contract).
RefQuad makeRefQuad(const SpriteItem& it, const Mat4& m) {
  const double a00 = m[0][0], a10 = m[1][0], a30 = m[3][0];
  const double a01 = m[0][1], a11 = m[1][1], a31 = m[3][1];
  const double c = std::cos(static_cast<double>(it.rotation));
  const double s = std::sin(static_cast<double>(it.rotation));
  RefQuad q;
  q.c = c;
  q.s = s;
  q.cx = a00 * it.pos.x + a10 * it.pos.y + a30;
  q.cy = a01 * it.pos.x + a11 * it.pos.y + a31;
  q.u0 = it.uv.u0;
  q.v0 = it.uv.v0;
  q.u1 = it.uv.u1;
  q.v1 = it.uv.v1;
  q.sx = it.scale.x;
  q.sy = it.scale.y;
  q.blend = it.blend;
  q.atlasId = it.atlasId;
  q.a00 = a00;
  q.a10 = a10;
  q.a01 = a01;
  q.a11 = a11;
  q.minX = 1e300;
  q.maxX = -1e300;
  q.minY = 1e300;
  q.maxY = -1e300;
  // CYCLIC order (the refInside's edges walk the perimeter — the GPU's
  // TRIANGLE_STRIP VBO uses the zig-zag order instead, the same set).
  constexpr double kCorners[8] = {-0.5, -0.5, 0.5, -0.5, 0.5, 0.5, -0.5,
                                  0.5};
  for (std::size_t k = 0; k < 4; ++k) {
    const double wx = it.pos.x + kCorners[k * 2] * it.scale.x;
    const double wy = it.pos.y + kCorners[k * 2 + 1] * it.scale.y;
    const double px = a00 * wx + a10 * wy + a30;
    const double py = a01 * wx + a11 * wy + a31;
    const double ox = px - q.cx;
    const double oy = py - q.cy;
    q.corners[k * 2] = q.cx + c * ox - s * oy;
    q.corners[k * 2 + 1] = q.cy + s * ox + c * oy;
    q.minX = std::min(q.minX, q.corners[k * 2]);
    q.maxX = std::max(q.maxX, q.corners[k * 2]);
    q.minY = std::min(q.minY, q.corners[k * 2 + 1]);
    q.maxY = std::max(q.maxY, q.corners[k * 2 + 1]);
  }
  return q;
}

// Strict interior test (the pixel centers of this scene never land on
// a quad edge — the scene comment; a boundary result would be
// ambiguous to the rasterizer and is excluded by construction).
bool refInside(const RefQuad& q, double px, double py) {
  if (px < q.minX || px > q.maxX || py < q.minY || py > q.maxY) {
    return false;
  }
  bool allPos = true;
  bool allNeg = true;
  for (std::size_t k = 0; k < 4; ++k) {
    const double x0 = q.corners[k * 2];
    const double y0 = q.corners[k * 2 + 1];
    const double x1 = q.corners[((k + 1) % 4) * 2];
    const double y1 = q.corners[((k + 1) % 4) * 2 + 1];
    const double cross = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    if (cross <= 0.0) allPos = false;
    if (cross >= 0.0) allNeg = false;
  }
  return allPos || allNeg;
}

// The reference color of one pixel (the clear color accumulated
// through the draw order's blends, per the shader + the per-group
// BlendMode contract).
std::array<std::uint8_t, 4> refPixel(int px, int py, std::int32_t width,
                                     std::int32_t height,
                                     const std::vector<RefQuad>& quads,
                                     double clearR, double clearG,
                                     double clearB, double clearA) {
  // The pixel-center NDC (readPixel's (0, 0) is the BOTTOM-LEFT — the
  // GL convention: py grows upward, NDC y grows upward: py = 0 maps
  // to ny near -1).
  const double nx = 2.0 * (px + 0.5) / width - 1.0;
  const double ny = 2.0 * (py + 0.5) / height - 1.0;
  double r = clearR, g = clearG, b = clearB, a = clearA;
  for (const RefQuad& q : quads) {
    if (!refInside(q, nx, ny)) continue;
    // The per-vertex UV at the pixel: the quad is an affine image of
    // the unit square — the local (lx, ly) in [-0.5, 0.5]^2 maps to
    // (u0, v0)..(u1, v1) (the rotation is inverted by R^-1).
    const double dx = nx - q.cx;
    const double dy = ny - q.cy;
    // Undo the screen-space rotation: the unrotated projected offset
    // off = R^-1 * (pixel - center).
    const double lx = q.c * dx + q.s * dy;
    const double ly = -q.s * dx + q.c * dy;
    // Undo the projection's linear part (the shader's world =
    // aPos + aCorner * aScale maps through the 2x2): the local offset
    // (corner.x * sx, corner.y * sy) = M^-1 * off.
    const double det = q.a00 * q.a11 - q.a10 * q.a01;
    const double lxw = (q.a11 * lx - q.a10 * ly) / det;  // corner.x * sx
    const double lyw = (-q.a01 * lx + q.a00 * ly) / det;  // corner.y * sy
    // The shader's UV: u = u0 + (corner.x + 0.5) * (u1 - u0).
    const double wx = lxw / q.sx + 0.5;
    const double wy = lyw / q.sy + 0.5;
    const double u = q.u0 + (q.u1 - q.u0) * wx;
    const double v = q.v0 + (q.v1 - q.v0) * wy;
    const std::uint32_t tx = static_cast<std::uint32_t>(
        std::max(0.0, std::min(3.999999, u * 4.0)));
    const std::uint32_t ty = static_cast<std::uint32_t>(
        std::max(0.0, std::min(3.999999, v * 4.0)));
    const std::array<std::uint8_t, 4> t = atlasTexel(q.atlasId, tx, ty);
    const double sr = t[0] / 255.0;  // the scene's tint is (1, 1, 1, 1)
    const double sg = t[1] / 255.0;
    const double sb = t[2] / 255.0;
    const double sa = t[3] / 255.0;
    if (q.blend == BlendMode::Alpha) {
      r = sr * sa + r * (1.0 - sa);
      g = sg * sa + g * (1.0 - sa);
      b = sb * sa + b * (1.0 - sa);
      a = sa + a * (1.0 - sa);
    } else {
      r = sr + r;
      g = sg + g;
      b = sb + b;
      a = sa + a;
    }
    r = std::min(1.0, std::max(0.0, r));
    g = std::min(1.0, std::max(0.0, g));
    b = std::min(1.0, std::max(0.0, b));
    a = std::min(1.0, std::max(0.0, a));
  }
  return {static_cast<std::uint8_t>(std::min(255.0, r * 255.0 + 0.5)),
          static_cast<std::uint8_t>(std::min(255.0, g * 255.0 + 0.5)),
          static_cast<std::uint8_t>(std::min(255.0, b * 255.0 + 0.5)),
          static_cast<std::uint8_t>(std::min(255.0, a * 255.0 + 0.5))};
}

// The frame's quads in the exact draw order (the batcher's published
// group order, the in-group order — the submit stage's draw order).
std::vector<RefQuad> frameQuads(SpriteBatcher& batcher, const Mat4& m) {
  std::vector<RefQuad> quads;
  quads.reserve(batcher.frameCount());
  for (const SpriteBatch& b : batcher.batches()) {
    for (const std::uint32_t slot : b.instances) {
      quads.push_back(makeRefQuad(batcher.at(slot), m));
    }
  }
  return quads;
}

}  // namespace

// ------------------------------------------------------------------------
// SpriteDrawCreate — the create validation + the stopped state (no GL
// required: the options are validated before the context check, the
// stopped state makes no GL calls).
// ------------------------------------------------------------------------

TEST(SpriteDrawCreate, OptionsValidation) {
  GlContext stopped;  // the stopped state — no GL environment needed
  MemorySink* sink = installCaptureSink();

  SpriteRenderer::Options o;
  o.maxInstances = 0;  // < 1
  o.maxAtlases = 1;
  Result<SpriteRenderer> r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 1);
  EXPECT_EQ(fieldOf(*firstEvent(*sink, "options_invalid"), "option"),
            "maxInstances");

  o.maxInstances = 1;
  o.maxAtlases = 0;  // < 1 (the second field — first failure now here)
  r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "option"),
            "maxAtlases");

  o.maxInstances = 1;
  o.maxAtlases = SpriteRenderer::kSpriteRendererMaxAtlases + 1;
  r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "option"),
            "maxAtlases");

  // First failure wins: BOTH invalid -> one warn, the first option.
  o.maxInstances = 0;
  o.maxAtlases = 0;
  r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "options_invalid"), 4);
  EXPECT_EQ(fieldOf(*lastEvent(*sink, "options_invalid"), "option"),
            "maxInstances");
}

TEST(SpriteDrawCreate, StoppedContext) {
  GlContext stopped;
  MemorySink* sink = installCaptureSink();
  SpriteRenderer::Options o;
  o.maxInstances = 16;
  o.maxAtlases = 4;
  Result<SpriteRenderer> r = SpriteRenderer::create(stopped, o);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::GlUnavailable);
  // No GL failure log: the context check precedes any GL work.
  EXPECT_EQ(countEvents(*sink, "sprite_renderer",
                        "resource_creation_failed"), 0);
}

TEST(SpriteDrawCreate, StoppedState) {
  SpriteRenderer r;
  EXPECT_FALSE(r.valid());
  EXPECT_EQ(r.maxInstances(), 0);
  EXPECT_EQ(r.maxAtlases(), 0);
  const SpriteDrawStats s = r.frameStats();
  EXPECT_EQ(s.drawCalls, 0);
  EXPECT_EQ(s.textureBinds, 0);
  EXPECT_EQ(s.blendChanges, 0);
  EXPECT_EQ(s.instances, 0);
  EXPECT_EQ(s.primitives, 0);
  const SpriteDrawTotals t = r.totals();
  EXPECT_EQ(t.frames, 0);
  EXPECT_EQ(t.drawCalls, 0);
  EXPECT_EQ(t.instances, 0);
  EXPECT_EQ(t.primitives, 0);

  // The stopped state makes no GL calls: submit/bindAtlas fail with
  // InvalidArgument (the first check — before the batcher is read).
  SpriteBatcher::Options bo;
  bo.maxSprites = 4;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher b = std::move(br).takeValue();
  b.beginFrame();
  EXPECT_TRUE(b.add(SpriteItem{}).ok());
  EXPECT_TRUE(b.build().ok());
  const Mat4 m = Mat4(1.0f);  // the identity — the stopped check precedes its use
  EXPECT_EQ(r.submit(b, m).error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(r.bindAtlas(0, 4, 4, std::span<const std::uint8_t>{}).error(),
            ErrorCode::InvalidArgument);
}

TEST(SpriteDrawCreate, MoveKeepsStopped) {
  SpriteRenderer a;
  SpriteRenderer b = std::move(a);
  EXPECT_FALSE(a.valid());
  EXPECT_FALSE(b.valid());
}

// ------------------------------------------------------------------------
// SpriteDrawState — the submit precondition contract (GL required —
// GTEST_SKIPs on a host without the environment, the documented
// contract).
// ------------------------------------------------------------------------

namespace {

// A small live setup: the context (the scene size) + a 4-atlas,
// 64-instance renderer.
struct StateSetup {
  GlContext ctx;
  SpriteRenderer renderer;
  std::uint32_t maxTextureSize{0};
};

StateSetup makeStateSetup(Result<GlContext> cr) {
  StateSetup s;
  s.ctx = std::move(cr).takeValue();  // the caller GTEST_SKIPped on error
  s.maxTextureSize = s.ctx.capabilities().maxTextureSize;
  SpriteRenderer::Options o;
  o.maxInstances = 64;
  o.maxAtlases = 4;
  auto r = SpriteRenderer::create(s.ctx, o);
  if (r.isError()) {
    ADD_FAILURE() << "SpriteRenderer::create failed: "
                  << laige::errorText(r.error());
    abort();
  }
  s.renderer = std::move(r).takeValue();
  return s;
}

// A 4x4 solid atlas (the state tests' texture content).
std::array<std::uint8_t, 64> solidAtlas(std::uint8_t r, std::uint8_t g,
                                        std::uint8_t b) {
  std::array<std::uint8_t, 64> d{};
  for (std::size_t p = 0; p < 16; ++p) {
    d[p * 4 + 0] = r;
    d[p * 4 + 1] = g;
    d[p * 4 + 2] = b;
    d[p * 4 + 3] = 255;
  }
  return d;
}

}  // namespace

TEST(SpriteDrawState, BuiltFrameGate) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  StateSetup s = makeStateSetup(std::move(cr));
  const std::array<std::uint8_t, 64> atlas = solidAtlas(10, 20, 30);
  ASSERT_TRUE(s.renderer.bindAtlas(0, 4, 4, bytesOf(atlas)).ok());

  SpriteBatcher::Options bo;
  bo.maxSprites = 16;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher batcher = std::move(br).takeValue();
  const Mat4 m = sceneMatrix();

  SpriteItem it;
  it.pos = {0.0f, 0.0f};
  it.uv = {0, 0, 1, 1};

  // An open window with declared items: NEVER drawn as an empty frame
  // (CORE-008) — InvalidArgument, nothing drawn, nothing counted.
  batcher.beginFrame();
  const auto add1 = batcher.add(it);
  ASSERT_TRUE(add1.ok());
  EXPECT_EQ(add1.value(), 0u);
  EXPECT_FALSE(batcher.frameBuilt());
  const Status unbuilt = s.renderer.submit(batcher, m);
  ASSERT_TRUE(unbuilt.isError());
  EXPECT_EQ(unbuilt.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(s.renderer.frameStats().drawCalls, 0);
  EXPECT_EQ(s.renderer.totals().frames, 0u);

  // The built frame submits: one draw (one group), one instance.
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_TRUE(batcher.frameBuilt());
  ASSERT_TRUE(s.renderer.submit(batcher, m).ok());
  EXPECT_EQ(s.renderer.frameStats().drawCalls, 1u);
  EXPECT_EQ(s.renderer.frameStats().instances, 1u);
  EXPECT_EQ(s.renderer.totals().frames, 1u);

  // The next window re-opens: an unbuilt window is rejected again
  // (the previous frame's batches are gone — frameBuilt() is false).
  batcher.beginFrame();
  const auto add2 = batcher.add(it);
  ASSERT_TRUE(add2.ok());
  EXPECT_FALSE(batcher.frameBuilt());
  const Status reopened = s.renderer.submit(batcher, m);
  ASSERT_TRUE(reopened.isError());
  EXPECT_EQ(reopened.error(), ErrorCode::InvalidArgument);
  // The failed submit zeroes the per-frame counters (the last
  // successful frame's stats are not served for this frame).
  EXPECT_EQ(s.renderer.frameStats().drawCalls, 0);
  EXPECT_EQ(s.renderer.totals().frames, 1u);
}

TEST(SpriteDrawState, UnboundAtlasRejected) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  StateSetup s = makeStateSetup(std::move(cr));
  const std::array<std::uint8_t, 64> atlas = solidAtlas(10, 20, 30);
  ASSERT_TRUE(s.renderer.bindAtlas(0, 4, 4, bytesOf(atlas)).ok());
  // Atlas 1 is NEVER bound.

  SpriteBatcher::Options bo;
  bo.maxSprites = 8;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher batcher = std::move(br).takeValue();
  const Mat4 m = sceneMatrix();

  batcher.beginFrame();
  SpriteItem it;
  it.atlasId = 1;  // the unbound atlas
  ASSERT_TRUE(batcher.add(it).ok());
  ASSERT_TRUE(batcher.build().ok());

  MemorySink* sink = installCaptureSink();
  const Status st = s.renderer.submit(batcher, m);
  ASSERT_TRUE(st.isError());
  EXPECT_EQ(st.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "atlas_unbound"), 1);
  // The frame is not counted: the totals are unchanged, the per-frame
  // counters zero.
  EXPECT_EQ(s.renderer.totals().frames, 0u);
  EXPECT_EQ(s.renderer.frameStats().drawCalls, 0);
  EXPECT_EQ(s.renderer.frameStats().instances, 0);
}

TEST(SpriteDrawState, EmptyFrame) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  StateSetup s = makeStateSetup(std::move(cr));
  SpriteBatcher::Options bo;
  bo.maxSprites = 4;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher batcher = std::move(br).takeValue();
  const Mat4 m = sceneMatrix();

  batcher.beginFrame();  // no items
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_TRUE(batcher.frameBuilt());
  ASSERT_TRUE(s.renderer.submit(batcher, m).ok());

  const SpriteDrawStats fs = s.renderer.frameStats();
  EXPECT_EQ(fs.drawCalls, 0);
  EXPECT_EQ(fs.instances, 0);
  EXPECT_EQ(fs.primitives, 0);
  EXPECT_EQ(s.renderer.totals().frames, 1u);
  EXPECT_EQ(s.renderer.totals().drawCalls, 0);
}

TEST(SpriteDrawState, InstanceCapacity) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  StateSetup s = makeStateSetup(std::move(cr));
  const std::array<std::uint8_t, 64> atlas = solidAtlas(10, 20, 30);
  ASSERT_TRUE(s.renderer.bindAtlas(0, 4, 4, bytesOf(atlas)).ok());

  // The renderer's budget is 64; declare 65 sprites.
  SpriteBatcher::Options bo;
  bo.maxSprites = 65;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher batcher = std::move(br).takeValue();
  const Mat4 m = sceneMatrix();

  batcher.beginFrame();
  for (std::uint32_t i = 0; i < 65; ++i) {
    SpriteItem it;
    it.pos = {static_cast<float>(i % 8), static_cast<float>(i / 8)};
    const auto r = batcher.add(it);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), i);
  }
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_EQ(batcher.frameCount(), 65u);

  MemorySink* sink = installCaptureSink();
  const Status st = s.renderer.submit(batcher, m);
  ASSERT_TRUE(st.isError());
  EXPECT_EQ(st.error(), ErrorCode::BudgetExhausted);
  EXPECT_EQ(countEvents(*sink, "sprite_renderer", "instance_capacity"), 1);
  const MemorySink::Entry* e = lastEvent(*sink, "instance_capacity");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "capacity"), "64");
  EXPECT_EQ(fieldOf(*e, "frame_count"), "65");
  EXPECT_EQ(s.renderer.totals().frames, 0u);
  EXPECT_EQ(s.renderer.frameStats().drawCalls, 0);
}

TEST(SpriteDrawState, BindAtlasArgs) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  StateSetup s = makeStateSetup(std::move(cr));
  const std::array<std::uint8_t, 64> atlas = solidAtlas(10, 20, 30);

  // Out-of-domain atlas id (the registry's flat table is 4 slots).
  EXPECT_EQ(s.renderer.bindAtlas(4, 4, 4, bytesOf(atlas)).error(),
            ErrorCode::InvalidArgument);
  // Wrong data size (one byte short).
  const std::span<const std::uint8_t> shortData(atlas.data(), 63);
  EXPECT_EQ(s.renderer.bindAtlas(0, 4, 4, shortData).error(),
            ErrorCode::InvalidArgument);
  // Zero size.
  EXPECT_EQ(s.renderer.bindAtlas(0, 0, 4, bytesOf(atlas)).error(),
            ErrorCode::InvalidArgument);
  // Beyond the context's texture size domain.
  EXPECT_EQ(
      s.renderer
          .bindAtlas(0, s.maxTextureSize + 1, 4, bytesOf(atlas))
          .error(),
      ErrorCode::InvalidArgument);
  // Nothing was created: a valid bind still works.
  EXPECT_TRUE(s.renderer.bindAtlas(0, 4, 4, bytesOf(atlas)).ok());
}

TEST(SpriteDrawState, RebindReplaces) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  StateSetup s = makeStateSetup(std::move(cr));
  // A 1x1 atlas (one texel): the exact-color check without UV math.
  const std::array<std::uint8_t, 4> a0 = {10, 20, 30, 255};
  const std::array<std::uint8_t, 4> a1 = {200, 210, 220, 255};
  ASSERT_TRUE(
      s.renderer.bindAtlas(0, 1, 1, std::span<const std::uint8_t>(
                                         a0.data(), a0.size()))
          .ok());
  ASSERT_TRUE(
      s.renderer.bindAtlas(0, 1, 1, std::span<const std::uint8_t>(
                                         a1.data(), a1.size()))
          .ok());  // the replace

  SpriteBatcher::Options bo;
  bo.maxSprites = 2;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher batcher = std::move(br).takeValue();
  // The camera maps (0, 0) to the NDC origin (the frame center).
  const Mat4 m = sceneMatrix();
  batcher.beginFrame();
  SpriteItem it;
  it.pos = {0.0f, 0.0f};
  it.uv = {0, 0, 1, 1};  // the single texel
  ASSERT_TRUE(batcher.add(it).ok());
  ASSERT_TRUE(batcher.build().ok());

  ASSERT_TRUE(s.ctx.clear(kClearR, kClearG, kClearB, kClearA).ok());
  ASSERT_TRUE(s.renderer.submit(batcher, m).ok());

  // The frame center: the sprite's quad interior (the 1x1 tile at
  // scale 0.125 is 32x16 px — the center pixel is well inside).
  std::uint8_t px[4] = {0, 0, 0, 0};
  ASSERT_TRUE(s.ctx.readPixel(64, 63, px).ok());

  EXPECT_EQ(px[0], 200);  // the REPLACED color — the last bind wins
  EXPECT_EQ(px[1], 210);
  EXPECT_EQ(px[2], 220);
  EXPECT_EQ(px[3], 255);
}

// ------------------------------------------------------------------------
// SpriteDrawSmoke — the roadmap's 1000-sprite offscreen render (GL
// required). One instanced draw per group (the dispatch count), the
// GL-side primitive cross-check, and the whole frame against the CPU
// reference (the non-empty, correct frame).
// ------------------------------------------------------------------------

TEST(SpriteDrawSmoke, ThousandSpriteFrame) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();

  SpriteRenderer::Options o;
  o.maxInstances = 2048;
  o.maxAtlases = 4;
  o.primitiveQuery = true;  // the GL-side dispatch cross-check
  auto rr = SpriteRenderer::create(ctx, o);
  ASSERT_TRUE(rr.ok());
  SpriteRenderer renderer = std::move(rr).takeValue();

  const std::array<std::uint8_t, 64> atlas0 = atlas0Bytes();
  const std::array<std::uint8_t, 64> atlas1 = atlas1Bytes();
  ASSERT_TRUE(renderer.bindAtlas(0, 4, 4, bytesOf(atlas0)).ok());
  ASSERT_TRUE(renderer.bindAtlas(1, 4, 4, bytesOf(atlas1)).ok());

  const std::vector<SpriteItem> items = buildSceneItems();
  ASSERT_EQ(items.size(), 1000u);

  SpriteBatcher::Options bo;
  bo.maxSprites = 2048;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher batcher = std::move(br).takeValue();
  batcher.beginFrame();
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto r = batcher.add(items[i]);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), i);
  }
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_TRUE(batcher.frameBuilt());

  // The batcher's group structure: the scene comment's exact shape.
  ASSERT_EQ(batcher.batchCount(), 3u);
  EXPECT_EQ(batcher.frameCount(), 1000u);
  const std::span<const SpriteBatch> batches = batcher.batches();
  EXPECT_EQ(batches[0].atlasId, 0u);
  EXPECT_EQ(batches[0].blend, BlendMode::Alpha);
  EXPECT_EQ(batches[1].atlasId, 0u);
  EXPECT_EQ(batches[1].blend, BlendMode::Additive);
  EXPECT_EQ(batches[2].atlasId, 1u);
  EXPECT_EQ(batches[2].blend, BlendMode::Alpha);
  EXPECT_EQ(batches[0].instances.size() + batches[1].instances.size() +
                batches[2].instances.size(),
            1000u);

  const Mat4 m = sceneMatrix();
  ASSERT_TRUE(ctx.clear(kClearR, kClearG, kClearB, kClearA).ok());
  const Status st = renderer.submit(batcher, m);
  ASSERT_TRUE(st.ok()) << laige::errorText(st.error());

  // The dispatch count (RENDER-001): ONE instanced draw per group —
  // the engine's own bookkeeping == the group count (the roadmap's
  // "draw call count == group count" in the test log); the GL-side
  // cross-check: the generated primitives == 2 x instances (2 per
  // instance of the 4-vertex strip).
  const SpriteDrawStats fs = renderer.frameStats();
  EXPECT_EQ(fs.drawCalls, 3u);
  EXPECT_EQ(fs.instances, 1000u);
  EXPECT_EQ(fs.primitives, 2000u);
  // The state changes: 2 texture binds (atlas 0, then atlas 1), 3
  // blend changes (Alpha -> Additive -> Alpha, the first set counted).
  EXPECT_EQ(fs.textureBinds, 2u);
  EXPECT_EQ(fs.blendChanges, 3u);
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.frames, 1u);
  EXPECT_EQ(t.drawCalls, 3u);
  EXPECT_EQ(t.instances, 1000u);
  EXPECT_EQ(t.primitives, 2000u);

  // The whole frame against the CPU reference (the non-empty, correct
  // frame): the ±1-byte tolerance covers the GPU float32 vs the
  // reference double after the RGBA8 conversion.
  const std::vector<RefQuad> quads = frameQuads(batcher, m);
  std::uint64_t mismatches = 0;
  std::uint64_t nonempty = 0;
  for (std::int32_t py = 0; py < kHeight; ++py) {
    for (std::int32_t px = 0; px < kWidth; ++px) {
      std::uint8_t got[4] = {0, 0, 0, 0};
      ASSERT_TRUE(ctx.readPixel(px, py, got).ok());
      const std::array<std::uint8_t, 4> want =
          refPixel(px, py, kWidth, kHeight, quads,
                    static_cast<double>(kClearR),
                    static_cast<double>(kClearG),
                    static_cast<double>(kClearB),
                    static_cast<double>(kClearA));
      bool ok = true;
      for (std::size_t ch = 0; ch < 4; ++ch) {
        const int d = got[ch] - want[ch];
        if (d < -1 || d > 1) {
          ok = false;
          break;
        }
      }
      if (!ok) ++mismatches;
      if (got[0] != 0 || got[1] != 0 || got[2] != 255 || got[3] != 255) {
        ++nonempty;
      }
    }
  }
  // The scene comment: the tiles tile the whole viewport exactly, so
  // EVERY pixel is covered (the clear color never shows through).
  EXPECT_EQ(mismatches, 0u);
  EXPECT_EQ(nonempty, static_cast<std::uint64_t>(kWidth * kHeight));

  // The exact probes (rounding-exact — no tolerance):
  std::uint8_t px[4] = {0, 0, 0, 0};
  // P0 at (0, 0): the texel (0, 0) of the checkerboard.
  ASSERT_TRUE(ctx.readPixel(64, 63, px).ok());
  EXPECT_EQ((std::array<std::uint8_t, 4>{px[0], px[1], px[2], px[3]}),
            (std::array<std::uint8_t, 4>{255, 128, 0, 255}));
  // P1 at (0, 1): additive over the clear (0, 0, 255). The probe's
  // quad center pixel (NDC (-0.25, -0.125) -> pixel (48, 56)).
  ASSERT_TRUE(ctx.readPixel(48, 56, px).ok());
  EXPECT_EQ((std::array<std::uint8_t, 4>{px[0], px[1], px[2], px[3]}),
            (std::array<std::uint8_t, 4>{255, 128, 255, 255}));
  // P2 at (0, -1): the solid atlas 1. The probe's quad center pixel
  // (NDC (0.25, 0.125) -> pixel (80, 72)).
  ASSERT_TRUE(ctx.readPixel(80, 72, px).ok());
  EXPECT_EQ((std::array<std::uint8_t, 4>{px[0], px[1], px[2], px[3]}),
            (std::array<std::uint8_t, 4>{33, 66, 222, 255}));

  // The roadmap's log line (machine-greppable): the dispatch count ==
  // the group count, on the offscreen path.
  std::printf(
      "sprite-draw: size=128x128 groups=%u draw_calls=%u instances=%u "
      "primitives=%u texture_binds=%u blend_changes=%u nonempty=%llu "
      "reference_mismatches=%llu\n",
      static_cast<unsigned>(batcher.batchCount()), fs.drawCalls,
      fs.instances, fs.primitives, fs.textureBinds, fs.blendChanges,
      static_cast<unsigned long long>(nonempty),
      static_cast<unsigned long long>(mismatches));
}

// ------------------------------------------------------------------------
// SpriteDrawPipeline — the integration path through the M2-GL-02
// RenderThread (GL required): the batch stage (the batcher build) +
// the submit stage (the renderer submit) + the onStart/onStop GL
// handoff. 100 headless frames of the scene; the totals exact; the
// last frame's pixel correct after the render thread released the
// context.
// ------------------------------------------------------------------------

namespace {

struct PipelineCtx {
  GlContext* ctx;
  SpriteBatcher* batcher;
  SpriteRenderer* renderer;
  const SpriteItem* items;
  std::uint32_t count;
  Mat4 matrix;
};

// The batch stage: the clear + the frame's declarations + the build
// (the render thread — the context is current on it, the onStart
// hook's takeover).
void pipelineBatchStage(void* p, const laige::render::FrameDescriptor&) noexcept {
  PipelineCtx* c = static_cast<PipelineCtx*>(p);
  EXPECT_TRUE(c->ctx->clear(kClearR, kClearG, kClearB, kClearA).ok());
  c->batcher->beginFrame();
  for (std::uint32_t i = 0; i < c->count; ++i) {
    // The frame budget never overflows (the scene is 1000 < 2048).
    EXPECT_TRUE(c->batcher->add(c->items[i]).ok());
  }
  EXPECT_TRUE(c->batcher->build().ok());
}

// The submit stage: the renderer's submit (the M2-SPRITE-02 stage —
// the pipeline's submit stage contract).
void pipelineSubmitStage(void* p,
                         const laige::render::FrameDescriptor&) noexcept {
  PipelineCtx* c = static_cast<PipelineCtx*>(p);
  EXPECT_TRUE(c->renderer->submit(*c->batcher, c->matrix).ok());
}

// The GL handoff (M2-GL-02): the takeover on the render thread, the
// release hand-back after the last frame.
void pipelineStart(void* p) noexcept {
  EXPECT_TRUE(static_cast<PipelineCtx*>(p)->ctx->makeCurrent().ok());
}
void pipelineStop(void* p) noexcept {
  EXPECT_TRUE(static_cast<PipelineCtx*>(p)->ctx->release().ok());
}

}  // namespace

TEST(SpriteDrawPipeline, HundredFrameOffscreen) {
  Result<GlContext> cr = tryContext();
  if (cr.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment: "
                 << laige::errorText(cr.error());
  }
  GlContext ctx = std::move(cr).takeValue();

  SpriteRenderer::Options o;
  o.maxInstances = 2048;
  o.maxAtlases = 4;
  auto rr = SpriteRenderer::create(ctx, o);
  ASSERT_TRUE(rr.ok());
  SpriteRenderer renderer = std::move(rr).takeValue();

  const std::array<std::uint8_t, 64> atlas0 = atlas0Bytes();
  const std::array<std::uint8_t, 64> atlas1 = atlas1Bytes();
  ASSERT_TRUE(renderer.bindAtlas(0, 4, 4, bytesOf(atlas0)).ok());
  ASSERT_TRUE(renderer.bindAtlas(1, 4, 4, bytesOf(atlas1)).ok());

  const std::vector<SpriteItem> items = buildSceneItems();
  ASSERT_EQ(items.size(), 1000u);

  SpriteBatcher::Options bo;
  bo.maxSprites = 2048;
  auto br = SpriteBatcher::create(bo);
  SpriteBatcher batcher = std::move(br).takeValue();

  PipelineCtx pc{&ctx, &batcher, &renderer, items.data(),
                 static_cast<std::uint32_t>(items.size()), sceneMatrix()};
  // The GL handoff (the class preamble's protocol): the test thread
  // releases the context BEFORE the render thread's onStart takes it
  // over (the P0 EGL stack rejects a live cross-thread takeover).
  ASSERT_TRUE(ctx.release().ok());
  RenderThreadOptions ro;
  ro.batchStage = &pipelineBatchStage;
  ro.submitStage = &pipelineSubmitStage;
  ro.stageContext = &pc;
  ro.onStart = &pipelineStart;
  ro.onStartContext = &pc;
  ro.onStop = &pipelineStop;
  ro.onStopContext = &pc;
  RenderThread thread(ro);

  // 100 frames (the offscreen CI path). The submit loop is PACED to
  // the render thread: wait for each frame to render before publishing
  // the next, so the single-slot handoff never backs up (0 drops) and
  // the since-construction totals below are deterministic (a tight
  // loop would outrun the software-GL render and drop 98 of 100).
  for (std::uint64_t f = 1; f <= 100; ++f) {
    laige::render::FrameDescriptor fd;
    fd.frameIndex = f;
    ASSERT_TRUE(thread.submitFrame(fd).ok());
    thread.waitIdle();
  }
  thread.shutdown();

  // The handoff accounted every frame (no drops: the single slot
  // never backed up — the submit loop is slower than the render
  // thread, the offscreen path).
  const laige::render::RenderThreadStats ts = thread.stats();
  EXPECT_EQ(ts.framesSubmitted, 100u);
  EXPECT_EQ(ts.framesRendered, 100u);
  EXPECT_EQ(ts.framesDropped, 0u);

  // The since-construction totals: exact (100 frames x the scene's
  // 3 draws / 1000 instances / 2 texture binds per frame; the blend
  // transitions carry across frames — frame 1 sets 3 (Alpha ->
  // Additive -> Alpha), each later frame sets 2: its first group
  // matches the blend the previous frame's last group left on the
  // GPU, so no redundant state change).
  const SpriteDrawTotals t = renderer.totals();
  EXPECT_EQ(t.frames, 100u);
  EXPECT_EQ(t.drawCalls, 300u);
  EXPECT_EQ(t.instances, 100000u);
  EXPECT_EQ(t.textureBinds, 200u);
  EXPECT_EQ(t.blendChanges, 3u + 99u * 2u);
  EXPECT_EQ(t.primitives, 0u);  // the query is off in this renderer

  // The last frame survives in the FBO: the render thread released
  // the context (onStop); the test thread takes it back (the
  // release-then-bind contract) and reads the P0 probe pixel.
  ASSERT_TRUE(ctx.makeCurrent().ok());
  std::uint8_t px[4] = {0, 0, 0, 0};
  ASSERT_TRUE(ctx.readPixel(64, 63, px).ok());
  EXPECT_EQ((std::array<std::uint8_t, 4>{px[0], px[1], px[2], px[3]}),
            (std::array<std::uint8_t, 4>{255, 128, 0, 255}));
}
