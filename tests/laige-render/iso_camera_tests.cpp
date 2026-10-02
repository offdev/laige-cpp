// laige-render isometric camera tests (M2-CAM-02): the iso presets +
// the grid-snap camera mode in laige/render/iso_camera.h.
//
// Pure value math — no GL context, no GL environment needed: every
// suite runs in every local tree and in CI. The goldens are
// hand-computed from the documented formulas in the header preamble
// (the M2-GL-03 preset builders, the snap arithmetic, the dyadic
// zoom ladder). The randomized sequences draw from laige::Prng
// (docs/testing.md §4 — the kIsoCameraStream substream).

#include "laige/render/iso_camera.h"
#include "laige/render/matrices.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glm/matrix.hpp>  // Mat4 element access
#include <glm/vec4.hpp>    // the world -> NDC point multiply

#include <gtest/gtest.h>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige_test_seed.h"

namespace {

using laige::ErrorCode;
using laige::render::Camera;
using laige::render::CameraOptions;
using laige::render::GridSnapOptions;
using laige::render::IsoAxes;
using laige::render::IsoCamera;
using laige::render::IsoCameraOptions;
using laige::render::IsoPreset;
using laige::render::IsoPresetKind;
using laige::render::Mat4;
using laige::render::Vec2;
using laige::render::Vec3;

// The randomized suite's substream id (docs/testing.md §4 — a stable
// named constant, CORE-005).
constexpr std::uint32_t kIsoCameraStream = 0x49534F43;  // "ISOC"

// ---------------------------------------------------------------------------
// Log capture (the camera_tests.cpp MemorySink pattern — rate limiting
// OFF so the tests assert per-event counts, not the facade's LOG-004
// window).
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

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Creates an IsoCamera from options; on a rejected config it fails the
// test and hands back the stopped state (the camera_tests.cpp pattern).
IsoCamera makeIso(IsoCameraOptions o) {
  auto r = IsoCamera::create(o);
  if (r.isError()) {
    ADD_FAILURE() << "IsoCamera::create rejected the options";
    return IsoCamera{};
  }
  return std::move(r).takeValue();
}

// The "on the grid" oracle (the header preamble's invariant): v is a
// grid multiple iff the nearest integer count round-trips exactly.
// Test domain only: |v| <= ~50, g >= 0.5 (the llround argument is far
// inside the long long range).
bool onGrid(float v, float g) {
  const long long n = std::llround(v / g);
  return g * static_cast<float>(n) == v;
}

// The 16 elements of a Mat4 in column-major order (m[c][r], matrices.h).
std::array<float, 16> elementsOf(const Mat4& m) {
  std::array<float, 16> a{};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) a[c * 4 + r] = m[c][r];
  }
  return a;
}

void expectMatrix(const Mat4& actual, std::array<float, 16> expected,
                  const char* what) {
  const std::array<float, 16> a = elementsOf(actual);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(a[i], expected[i]) << what << " element " << i;
  }
}

// The supported custom shear of the tests: A = -dx.y = -dy.y = zUnit = 2,
// det(dx, dy) = 3*(-2) - (-2)*(-1) = -8 != 0 — passes isoShearSupported.
IsoAxes supportedShear() {
  return IsoAxes{Vec2{3.0f, -2.0f}, Vec2{-1.0f, -2.0f}, 2.0f};
}

// A uniform float in [lo, hi) (24-bit resolution, the Prng contract).
float uniformIn(laige::Prng& rng, float lo, float hi) {
  return lo + rng.next_float01() * (hi - lo);
}

// ---------------------------------------------------------------------------
// The presets (the ADR 0005 config-only pattern)
// ---------------------------------------------------------------------------

TEST(IsoCameraCreate, DefaultsAreTwoToOneDimetric) {
  const IsoCamera cam = makeIso(IsoCameraOptions{});
  ASSERT_TRUE(cam.valid());
  // ADR 0005: the engine default is 2:1 dimetric.
  EXPECT_EQ(cam.preset().kind, IsoPresetKind::Dimetric2To1);
  EXPECT_EQ(cam.preset().scale, 1.0f);
  EXPECT_FALSE(cam.gridSnapEnabled());
  EXPECT_EQ(cam.gridSize(), 1.0f);
  // The default state (zoom 1, position (0,0,0), no shake): the matrix
  // is exactly the M2-GL-03 2:1 builder at scale 1.
  expectMatrix(cam.matrix(), elementsOf(laige::render::isoDimetric2To1(1.0f)),
               "default matrix vs isoDimetric2To1(1)");
}

TEST(IsoCameraCreate, PresetMatricesMatchTheGlBuilders) {
  // Dimetric2To1 at scale 2.
  {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::Dimetric2To1;
    o.preset.scale = 2.0f;
    const IsoCamera cam = makeIso(o);
    expectMatrix(cam.matrix(), elementsOf(laige::render::isoDimetric2To1(2.0f)),
                 "dimetric2to1 matrix");
  }
  // TrueIso3060 at scale 2.
  {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::TrueIso3060;
    o.preset.scale = 2.0f;
    const IsoCamera cam = makeIso(o);
    expectMatrix(cam.matrix(), elementsOf(laige::render::isoTrueIso3060(2.0f)),
                 "true-iso matrix");
  }
  // CustomShear (the supported axes above).
  {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::CustomShear;
    o.preset.axes = supportedShear();
    const IsoCamera cam = makeIso(o);
    expectMatrix(cam.matrix(),
                 elementsOf(laige::render::isoMatrix(supportedShear())),
                 "custom-shear matrix");
  }
}

// The zoom and the camera center (the header preamble's matrix section):
// hand-computed goldens for the 2:1 dimetric preset at scale 1.
TEST(IsoCameraCreate, ZoomAndCameraCenter) {
  // Zoom 2, position (0,0,0): the preset's axes scaled by 1/zoom.
  {
    IsoCameraOptions o;
    o.camera.zoom = 2.0f;
    const IsoCamera cam = makeIso(o);
    // Column-major m[c][r] (matrices.h): rows of the matrix are
    // [dx.x/2, dy.x/2, 0, 0] = [1, -1, 0, 0] and
    // [dx.y/2, dy.y/2, zUnit/2, 0] = [-0.5, -0.5, 0.5, 0] — so
    // m[2][1] (column 2, row 1) carries the 1/zoom height scale.
    expectMatrix(
        cam.matrix(),
        {1.0f, -0.5f, 0.0f, 0.0f,   // column 0
         -1.0f, -0.5f, 0.0f, 0.0f,  // column 1
         0.0f, 0.5f, 0.0f, 0.0f,    // column 2
         0.0f, 0.0f, 0.0f, 1.0f},   // column 3
        "zoom-2 matrix");
  }
  // Camera center at (2, 3): translation tx = -(2*2 - 2*3) = 2,
  // ty = -(-1*2 - 1*3) = 5 (dimetric scale 1 axes).
  {
    IsoCameraOptions o;
    o.camera.position = Vec3{2.0f, 3.0f, 0.0f};
    const IsoCamera cam = makeIso(o);
    const Mat4 m = cam.matrix();
    // The camera's ground point projects to NDC (0, 0, 0) — the screen
    // center.
    const glm::vec4 h = m * glm::vec4(2.0f, 3.0f, 0.0f, 1.0f);
    EXPECT_FLOAT_EQ(h.x, 0.0f);
    EXPECT_FLOAT_EQ(h.y, 0.0f);
    EXPECT_FLOAT_EQ(h.z, 0.0f);
    // A hand-computed world point: (5, 7, 1) ->
    // NDC_x = 2*(5-2) - 2*(7-3) = -2, NDC_y = -(5-2) - (7-3) + 1 = -6.
    const glm::vec4 h2 = m * glm::vec4(5.0f, 7.0f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(h2.x, -2.0f);
    EXPECT_FLOAT_EQ(h2.y, -6.0f);
    EXPECT_FLOAT_EQ(h2.z, 0.0f);
  }
  // The shake moves the view center (the effective eye's ground x/y):
  // a +x shake of 0.25 shifts the translation by -2*0.25 (dx.x * s).
  {
    IsoCameraOptions o;
    IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.applyShake(Vec3{0.25f, 0.0f, 0.0f}).ok());
    const Mat4 m = cam.matrix();
    EXPECT_FLOAT_EQ(m[3][0], -0.5f);
    EXPECT_FLOAT_EQ(m[3][1], 0.25f);
    // The shake's z component does not enter the iso matrix.
    ASSERT_TRUE(cam.applyShake(Vec3{0.0f, 0.0f, 0.25f}).ok());
    const Mat4 m2 = cam.matrix();
    EXPECT_FLOAT_EQ(m2[3][0], -0.5f);
    EXPECT_FLOAT_EQ(m2[3][1], 0.25f);
  }
}

// ---------------------------------------------------------------------------
// The create validation (first failure wins, one warn per failure)
// ---------------------------------------------------------------------------

TEST(IsoCameraCreate, RejectsUnknownPresetKind) {
  MemorySink* sink = installCaptureSink();
  IsoCameraOptions o;
  o.preset.kind = static_cast<IsoPresetKind>(7);
  auto r = IsoCamera::create(o);
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(countEvents(*sink, "iso_camera", "options_invalid"), 1u);
  const MemorySink::Entry* e = firstEvent(*sink, "options_invalid");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "option"), "preset_kind");
}

TEST(IsoCameraCreate, RejectsBadBuiltInScale) {
  MemorySink* sink = installCaptureSink();
  for (float scale : {0.0f, -1.0f, std::nanf("")}) {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::Dimetric2To1;
    o.preset.scale = scale;
    auto r = IsoCamera::create(o);
    EXPECT_TRUE(r.isError());
    if (r.isError()) {
      EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
    }
  }
  EXPECT_EQ(countEvents(*sink, "iso_camera", "options_invalid"), 3u);
  const MemorySink::Entry* e = firstEvent(*sink, "options_invalid");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "option"), "preset_scale");
}

// M2-CAM-02 owns the scene-shear validation: the custom shear must pass
// the M2-ISO-01 isoShearSupported checker.
TEST(IsoCameraCreate, RejectsUnsupportedCustomShear) {
  MemorySink* sink = installCaptureSink();
  // (a) the ground axes' downward slopes differ (A != B).
  {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::CustomShear;
    o.preset.axes = IsoAxes{Vec2{2.0f, -1.0f}, Vec2{-2.0f, -0.5f}, 1.0f};
    auto r = IsoCamera::create(o);
    EXPECT_TRUE(r.isError());
  }
  // (b) the height unit differs from the downward slope (A != C).
  {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::CustomShear;
    o.preset.axes = IsoAxes{Vec2{2.0f, -1.0f}, Vec2{-2.0f, -1.0f}, 2.0f};
    auto r = IsoCamera::create(o);
    EXPECT_TRUE(r.isError());
  }
  // (c) the ground map is degenerate (det == 0) — isoShearSupported.
  {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::CustomShear;
    o.preset.axes = IsoAxes{Vec2{2.0f, -1.0f}, Vec2{4.0f, -2.0f}, 1.0f};
    auto r = IsoCamera::create(o);
    EXPECT_TRUE(r.isError());
  }
  EXPECT_EQ(countEvents(*sink, "iso_camera", "options_invalid"), 3u);
  const MemorySink::Entry* e = firstEvent(*sink, "options_invalid");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "option"), "preset_shear");
}

TEST(IsoCameraCreate, RejectsBadGridSize) {
  MemorySink* sink = installCaptureSink();
  for (float g : {0.0f, -1.0f, std::nanf(""), 1e-9f}) {
    IsoCameraOptions o;
    o.snap.enabled = true;
    o.snap.gridSize = g;
    auto r = IsoCamera::create(o);
    EXPECT_TRUE(r.isError());
    if (r.isError()) {
      EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
    }
  }
  EXPECT_EQ(countEvents(*sink, "iso_camera", "options_invalid"), 4u);
  const MemorySink::Entry* e = firstEvent(*sink, "options_invalid");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "option"), "grid_size");
}

// The grid-aligned-bounds contract: snap + enabled bounds require every
// rectangle corner to be an exact multiple of the grid size.
TEST(IsoCameraCreate, RejectsNonAlignedBounds) {
  MemorySink* sink = installCaptureSink();
  {
    IsoCameraOptions o;
    o.snap.enabled = true;
    o.snap.gridSize = 1.0f;
    o.camera.boundsEnabled = true;
    o.camera.bounds = laige::render::CameraBounds{
        Vec2{0.5f, 0.0f}, Vec2{10.0f, 10.0f}};
    auto r = IsoCamera::create(o);
    EXPECT_TRUE(r.isError());
  }
  // The same rectangle is aligned at grid size 0.5.
  {
    IsoCameraOptions o;
    o.snap.enabled = true;
    o.snap.gridSize = 0.5f;
    o.camera.boundsEnabled = true;
    o.camera.bounds = laige::render::CameraBounds{
        Vec2{0.5f, 0.0f}, Vec2{10.0f, 10.0f}};
    const IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
  }
  EXPECT_EQ(countEvents(*sink, "iso_camera", "options_invalid"), 1u);
  const MemorySink::Entry* e = firstEvent(*sink, "options_invalid");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "option"), "bounds_grid_alignment");
}

// The snap-margin contract: the create must satisfy the INFLATED look-at
// margin (maxShakeOffset + g*sqrt(2)/2) in snap mode.
TEST(IsoCameraCreate, RejectsTightSnapMargin) {
  MemorySink* sink = installCaptureSink();
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 1.0f;  // need > 0.25 + 0.7071 = 0.9571
  o.camera.position = Vec3{0.0f, 0.0f, 0.0f};
  o.camera.target = Vec3{0.0f, 0.0f, -0.9f};  // distance 0.9 < 0.9571
  auto r = IsoCamera::create(o);
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(countEvents(*sink, "iso_camera", "options_invalid"), 1u);
  const MemorySink::Entry* e = firstEvent(*sink, "options_invalid");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "option"), "snap_margin");
}

// A base M2-CAM-01 camera failure short-circuits with the base event
// (no iso_camera event — the base owns it).
TEST(IsoCameraCreate, BaseCameraFailureShortCircuits) {
  MemorySink* sink = installCaptureSink();
  IsoCameraOptions o;
  o.camera.aspect = 0.0f;
  auto r = IsoCamera::create(o);
  EXPECT_TRUE(r.isError());
  if (r.isError()) {
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(countEvents(*sink, "camera", "options_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "iso_camera", "options_invalid"), 0u);
}

// ---------------------------------------------------------------------------
// The grid snap (the roadmap unit test: "in grid snap mode the camera
// always lands on grid coordinates for any input")
// ---------------------------------------------------------------------------

// The create snaps the initial position into the grid.
TEST(IsoCameraGridSnap, CreateSnapsInitialPosition) {
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 1.0f;
  o.camera.position = Vec3{0.3f, 0.8f, 0.0f};
  const IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  EXPECT_FLOAT_EQ(cam.camera().position().x, 0.0f);  // 0.3 -> 0
  EXPECT_FLOAT_EQ(cam.camera().position().y, 1.0f);  // 0.8 -> 1
  EXPECT_FLOAT_EQ(cam.camera().position().z, 0.0f);  // z is never snapped
}

// The snapCoord oracle: nearest multiple, ties away from zero.
TEST(IsoCameraGridSnap, SnapCoordOracle) {
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(2.3f, 2.0f), 2.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(2.7f, 2.0f), 2.0f);  // closer to 2
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(3.0f, 2.0f), 4.0f);  // tie: away
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(-3.0f, 2.0f), -4.0f);  // tie: away
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(2.5f, 2.0f), 2.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(1.5f, 2.0f), 2.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(-1.5f, 2.0f), -2.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(0.0f, 2.0f), 0.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapCoord(1.23f, 0.5f), 1.0f);
  // Idempotent: the snap of a snapped value is the value.
  for (float v : {-7.7f, -0.3f, 0.0f, 2.3f, 3.0f, 9.9f}) {
    const float s = IsoCamera::snapCoord(v, 2.0f);
    EXPECT_FLOAT_EQ(IsoCamera::snapCoord(s, 2.0f), s) << "v = " << v;
  }
}

// Any input sequence (setPosition, setZoom, applyShake, follow, update)
// leaves the position on the grid, in snap mode, no bounds.
TEST(IsoCameraGridSnap, AlwaysOnGridForAnyInput) {
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 2.0f;  // snap-margin need: 0.25 + 2*sqrt(2)/2 = 1.6642
  o.camera.target = Vec3{0.0f, 0.0f, -5.0f};  // distance 5 > 1.6642
  IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  ASSERT_TRUE(onGrid(cam.camera().position().x, 2.0f));

  laige::Prng rng = laige::testing::TestPrng(kIsoCameraStream);
  for (int i = 0; i < 2000; ++i) {
    const std::uint64_t op = rng.next_u64() % 5u;
    if (op == 0) {
      const float x = uniformIn(rng, -50.0f, 50.0f);
      const float y = uniformIn(rng, -50.0f, 50.0f);
      const float z = uniformIn(rng, -1.0f, 1.0f);
      if (cam.setPosition(Vec3{x, y, z}).ok()) {
        const Vec3 p = cam.camera().position();
        EXPECT_TRUE(onGrid(p.x, 2.0f)) << "x off-grid at step " << i << ": "
                                       << p.x;
        EXPECT_TRUE(onGrid(p.y, 2.0f)) << "y off-grid at step " << i << ": "
                                       << p.y;
      }
    } else if (op == 1) {
      const float z = uniformIn(rng, 0.25f, 4.0f);
      ASSERT_TRUE(cam.setZoom(z).ok());
    } else if (op == 2) {
      const Vec3 s{uniformIn(rng, -0.1f, 0.1f), uniformIn(rng, -0.1f, 0.1f),
                   uniformIn(rng, -0.1f, 0.1f)};
      ASSERT_TRUE(cam.applyShake(s).ok());
    } else if (op == 3) {
      if (i % 40 == 0) {
        const Vec3 f{uniformIn(rng, -30.0f, 30.0f), uniformIn(rng, -30.0f, 30.0f),
                     0.0f};
        ASSERT_TRUE(cam.setFollowTarget(f).ok());
      } else if (i % 40 == 20) {
        cam.stopFollowing();
      }
    }
    cam.update();
    const Vec3 p = cam.camera().position();
    EXPECT_TRUE(onGrid(p.x, 2.0f)) << "x off-grid after update " << i << ": "
                                   << p.x;
    EXPECT_TRUE(onGrid(p.y, 2.0f)) << "y off-grid after update " << i << ": "
                                   << p.y;
  }
}

// Follow + aligned bounds: 2000 frames of tracking a far-away target —
// the position is on the grid AND inside the rectangle every frame
// (the grid-aligned-bounds contract: the snap of a point in an aligned
// rectangle stays inside it).
TEST(IsoCameraGridSnap, FollowStaysOnGridInsideAlignedBounds) {
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 2.0f;
  o.camera.boundsEnabled = true;
  o.camera.bounds = laige::render::CameraBounds{
      Vec2{-10.0f, -12.0f}, Vec2{10.0f, 12.0f}};  // every corner a 2-multiple
  o.camera.position = Vec3{3.0f, 4.0f, 0.0f};
  IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  ASSERT_TRUE(onGrid(cam.camera().position().x, 2.0f));
  ASSERT_TRUE(onGrid(cam.camera().position().y, 2.0f));

  ASSERT_TRUE(cam.setFollowTarget(Vec3{500.0f, 300.0f, 0.0f}).ok());
  for (int i = 0; i < 2000; ++i) {
    cam.update();
    const Vec3 p = cam.camera().position();
    EXPECT_TRUE(onGrid(p.x, 2.0f)) << "x off-grid at frame " << i << ": "
                                   << p.x;
    EXPECT_TRUE(onGrid(p.y, 2.0f)) << "y off-grid at frame " << i << ": "
                                   << p.y;
    EXPECT_GE(p.x, -10.0f) << "x out of bounds at frame " << i;
    EXPECT_LE(p.x, 10.0f) << "x out of bounds at frame " << i;
    EXPECT_GE(p.y, -12.0f) << "y out of bounds at frame " << i;
    EXPECT_LE(p.y, 12.0f) << "y out of bounds at frame " << i;
  }
}

// The grid snap locks the camera position, not the height: the z
// coordinate passes through unsnapped.
TEST(IsoCameraGridSnap, ZIsNeverSnapped) {
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 1.0f;
  IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  ASSERT_TRUE(cam.setPosition(Vec3{0.3f, 0.8f, 1.23f}).ok());
  EXPECT_FLOAT_EQ(cam.camera().position().z, 1.23f);  // z never snapped
  // Follow a target with a non-grid z: the M2-CAM-01 rigid-translation
  // follow step preserves the (target - position) offset, so the
  // position converges to F - (T0 - P0) with z = 0.5 - (-1 - 1.23) =
  // 2.73 — an OFF-GRID height, which the snap leaves untouched (the
  // grid locks x/y only).
  ASSERT_TRUE(cam.setFollowTarget(Vec3{0.0f, 0.0f, 0.5f}).ok());
  for (int i = 0; i < 300; ++i) cam.update();
  EXPECT_NEAR(cam.camera().position().z, 2.73f, 1e-3f);
  EXPECT_FALSE(onGrid(cam.camera().position().z, 1.0f));  // the height is free
  EXPECT_TRUE(onGrid(cam.camera().position().x, 1.0f));
  EXPECT_TRUE(onGrid(cam.camera().position().y, 1.0f));
}

// A mutation whose SNAPPED candidate would break the (inflated) look-at
// margin is rejected, state unchanged (the documented failure path).
TEST(IsoCameraGridSnap, RejectsSnappedMarginViolation) {
  MemorySink* sink = installCaptureSink();
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 1.0f;
  o.camera.target = Vec3{0.0f, 0.0f, -2.0f};  // distance 2 > 0.9571
  IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  // The snapped candidate (0, 0, -2) sits ON the target: distance 0.
  EXPECT_FALSE(cam.setPosition(Vec3{0.2f, 0.2f, -2.0f}).ok());
  // State unchanged (the pre-snap position, still on the grid).
  EXPECT_FLOAT_EQ(cam.camera().position().x, 0.0f);
  EXPECT_FLOAT_EQ(cam.camera().position().y, 0.0f);
  EXPECT_FLOAT_EQ(cam.camera().position().z, 0.0f);
  EXPECT_EQ(countEvents(*sink, "iso_camera", "lookat_margin_violated"), 1u);
}

// setTarget under snap: the INFLATED margin applies (the next frame's
// snap can move the position by g*sqrt(2)/2); the same target is
// accepted without snap.
TEST(IsoCameraGridSnap, SetTargetUsesInflatedMargin) {
  MemorySink* sink = installCaptureSink();
  {
    IsoCameraOptions o;
    o.snap.enabled = true;
    o.snap.gridSize = 1.0f;  // inflated bound 0.9571
    IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());  // create margin: distance 1 > 0.9571
    // 0.5 < 0.9571: rejected under snap...
    EXPECT_FALSE(cam.setTarget(Vec3{0.0f, 0.0f, -0.5f}).ok());
  }
  {
    IsoCameraOptions o;  // no snap: the M2-CAM-01 strict bound (0.25)
    IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
    // ...but accepted without snap (0.5 > 0.25).
    EXPECT_TRUE(cam.setTarget(Vec3{0.0f, 0.0f, -0.5f}).ok());
  }
  EXPECT_EQ(countEvents(*sink, "iso_camera", "lookat_margin_violated"), 1u);
}

// Non-finite inputs are rejected (InvalidArgument + warn, state
// unchanged) on every snap-aware mutation path.
TEST(IsoCameraGridSnap, RejectsNonFiniteInputs) {
  MemorySink* sink = installCaptureSink();
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 1.0f;
  IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  EXPECT_FALSE(cam.setPosition(Vec3{std::nanf(""), 0.0f, 0.0f}).ok());
  EXPECT_FALSE(cam.setTarget(Vec3{0.0f, 0.0f, std::nanf("")}).ok());
  EXPECT_FALSE(cam.setZoom(std::nanf("")).ok());
  EXPECT_EQ(countEvents(*sink, "iso_camera", "non_finite_input"), 3u);
  EXPECT_EQ(cam.camera().zoom(), 1.0f);  // unchanged
}

// ---------------------------------------------------------------------------
// The zoom levels (the roadmap unit test: "zoom levels exactly match the
// documented set")
// ---------------------------------------------------------------------------

// The documented set for (0.25, 2): L = {0.25, 0.5, 1, 2}. Levels are
// fixed points; below/above clamp to the bounds; the midpoint goes to
// the higher zoom (an exact float tie).
TEST(IsoCameraZoomLevels, DocumentedSetExact) {
  const float lo = 0.25f;
  const float hi = 2.0f;
  const float levels[4] = {0.25f, 0.5f, 1.0f, 2.0f};
  // Every level snaps to itself (exact).
  for (float lv : levels) {
    EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(lv, lo, hi), lv) << "level "
                                                              << lv;
  }
  // Below min -> min; above max -> max.
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(0.1f, lo, hi), 0.25f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(100.0f, lo, hi), 2.0f);
  // The midpoint of the first pair: 0.25*sqrt(2) — just below -> 0.25,
  // the exact float tie -> the HIGHER (0.5), just above -> 0.5.
  const float mid = 0.25f * laige::render::kIsoSnapSqrtTwo;
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(mid - 1e-6f, lo, hi), 0.25f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(mid, lo, hi), 0.5f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(mid + 1e-6f, lo, hi), 0.5f);
  // Midpoint of (1, 2): 1*sqrt(2) — just below -> 1, above -> 2.
  const float mid2 = laige::render::kIsoSnapSqrtTwo;
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(mid2 - 1e-6f, lo, hi), 1.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(mid2 + 1e-6f, lo, hi), 2.0f);
}

// A non-dyadic top bound: the ladder stops at the largest level <= hi.
// For (1, 10): L = {1, 2, 4, 8} — 9 and 10 snap to 8 (16 is not a level).
TEST(IsoCameraZoomLevels, NonDyadicTopBound) {
  const float lo = 1.0f;
  const float hi = 10.0f;
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(9.0f, lo, hi), 8.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(10.0f, lo, hi), 8.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(4.5f, lo, hi), 4.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(5.7f, lo, hi), 8.0f);  // > 4*sqrt2
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(1.5f, lo, hi), 2.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(0.5f, lo, hi), 1.0f);  // below
  // degenerate: hi == lo -> the single level {lo}.
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(3.0f, 3.0f, 3.0f), 3.0f);
  EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(100.0f, 3.0f, 3.0f), 3.0f);
}

// 10k random zooms: the result is always an exact member of L
// (membership + idempotence).
TEST(IsoCameraZoomLevels, RandomAlwaysOnLadder) {
  const float lo = 0.25f;
  const float hi = 2.0f;
  const float levels[4] = {0.25f, 0.5f, 1.0f, 2.0f};
  laige::Prng rng = laige::testing::TestPrng(kIsoCameraStream + 1u);
  for (int i = 0; i < 10000; ++i) {
    const float z = uniformIn(rng, 0.001f, 20.0f);
    const float s = IsoCamera::snapZoomLevel(z, lo, hi);
    bool onLadder = false;
    for (float lv : levels) {
      if (s == lv) onLadder = true;
    }
    EXPECT_TRUE(onLadder) << "z = " << z << " snapped to " << s;
    EXPECT_FLOAT_EQ(IsoCamera::snapZoomLevel(s, lo, hi), s) << "z = " << z;
  }
}

// setZoom in snap mode snaps to the ladder; without snap it is the
// M2-CAM-01 clamp.
TEST(IsoCameraZoomLevels, SetZoomIntegration) {
  {
    IsoCameraOptions o;
    o.snap.enabled = true;
    o.camera.zoomMin = 0.25f;
    o.camera.zoomMax = 2.0f;
    IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
    ASSERT_TRUE(cam.setZoom(1.2f).ok());
    EXPECT_FLOAT_EQ(cam.camera().zoom(), 1.0f);  // below 1*sqrt2
    ASSERT_TRUE(cam.setZoom(1.5f).ok());
    EXPECT_FLOAT_EQ(cam.camera().zoom(), 2.0f);  // above 1*sqrt2
    ASSERT_TRUE(cam.setZoom(0.1f).ok());
    EXPECT_FLOAT_EQ(cam.camera().zoom(), 0.25f);  // clamps to min
    ASSERT_TRUE(cam.setZoom(10.0f).ok());
    EXPECT_FLOAT_EQ(cam.camera().zoom(), 2.0f);  // clamps to max
  }
  {
    IsoCameraOptions o;  // no snap: the continuous clamp
    o.camera.zoomMin = 0.25f;
    o.camera.zoomMax = 2.0f;
    IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
    ASSERT_TRUE(cam.setZoom(1.2f).ok());
    EXPECT_FLOAT_EQ(cam.camera().zoom(), 1.2f);  // unchanged
  }
}

// ---------------------------------------------------------------------------
// Determinism (same input sequence -> bit-identical state)
// ---------------------------------------------------------------------------

TEST(IsoCameraDeterminism, BitIdenticalForSameInput) {
  IsoCameraOptions o;
  o.snap.enabled = true;
  o.snap.gridSize = 0.7f;  // non-dyadic grid (the interesting case)
  o.camera.position = Vec3{1.3f, 2.7f, 0.5f};
  o.camera.target = Vec3{1.3f, 2.7f, -2.0f};  // distance 2.5 > 0.745
  IsoCamera a = makeIso(o);
  IsoCamera b = makeIso(o);
  ASSERT_TRUE(a.valid());
  ASSERT_TRUE(b.valid());

  laige::Prng rng = laige::testing::TestPrng(kIsoCameraStream + 2u);
  for (int i = 0; i < 200; ++i) {
    const std::uint64_t op = rng.next_u64() % 5u;
    if (op == 0) {
      const Vec3 p{uniformIn(rng, -20.0f, 20.0f), uniformIn(rng, -20.0f, 20.0f),
                   uniformIn(rng, -1.0f, 1.0f)};
      EXPECT_EQ(a.setPosition(p).ok(), b.setPosition(p).ok());
    } else if (op == 1) {
      const float z = uniformIn(rng, 0.25f, 4.0f);
      EXPECT_EQ(a.setZoom(z).ok(), b.setZoom(z).ok());
    } else if (op == 2) {
      const Vec3 s{uniformIn(rng, -0.1f, 0.1f), uniformIn(rng, -0.1f, 0.1f),
                   uniformIn(rng, -0.1f, 0.1f)};
      EXPECT_EQ(a.applyShake(s).ok(), b.applyShake(s).ok());
    } else if (op == 3) {
      if (i % 40 == 0) {
        const Vec3 f{uniformIn(rng, -30.0f, 30.0f), uniformIn(rng, -30.0f, 30.0f),
                     0.0f};
        EXPECT_EQ(a.setFollowTarget(f).ok(), b.setFollowTarget(f).ok());
      } else if (i % 40 == 20) {
        a.stopFollowing();
        b.stopFollowing();
      }
    }
    a.update();
    b.update();
    const Vec3 pa = a.camera().position();
    const Vec3 pb = b.camera().position();
    EXPECT_EQ(pa.x, pb.x) << "position.x diverged at step " << i;
    EXPECT_EQ(pa.y, pb.y) << "position.y diverged at step " << i;
    EXPECT_EQ(pa.z, pb.z) << "position.z diverged at step " << i;
    EXPECT_EQ(a.camera().zoom(), b.camera().zoom())
        << "zoom diverged at step " << i;
    const std::array<float, 16> ma = elementsOf(a.matrix());
    const std::array<float, 16> mb = elementsOf(b.matrix());
    EXPECT_EQ(ma, mb) << "matrix diverged at step " << i;
  }
}

// ---------------------------------------------------------------------------
// The stopped state (the Camera precedent)
// ---------------------------------------------------------------------------

TEST(IsoCameraStopped, StoppedStateIsSilent) {
  MemorySink* sink = installCaptureSink();
  IsoCamera cam;
  EXPECT_FALSE(cam.valid());
  expectMatrix(cam.matrix(), elementsOf(Mat4(1.0f)), "stopped matrix");
  EXPECT_FALSE(cam.setPosition(Vec3{1.0f, 0.0f, 0.0f}).ok());
  EXPECT_FALSE(cam.setTarget(Vec3{0.0f, 0.0f, -1.0f}).ok());
  EXPECT_FALSE(cam.setFollowTarget(Vec3{0.0f, 0.0f, 0.0f}).ok());
  EXPECT_FALSE(cam.setZoom(2.0f).ok());
  EXPECT_FALSE(cam.applyShake(Vec3{0.1f, 0.0f, 0.0f}).ok());
  cam.update();  // no-op
  cam.stopFollowing();  // no-op
  EXPECT_EQ(sink->entries.size(), 0u) << "the stopped state logs nothing";
}

}  // namespace
