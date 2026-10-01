// laige-render 3D camera core tests (M2-CAM-01): the presentation-side
// camera in laige/render/camera.h.
//
// Pure value math — no GL context, no GL environment needed: every
// suite runs in every local tree and in CI. The goldens are
// hand-computed / recomputed from the documented formulas in the
// header: the zoom clamp is exact at the bounds, the shake decay is
// exact IEEE halving to zero (the documented 150-update bound for a
// 1-world-unit offset), and the follow is the exact per-update lerp
// sequence (determinism given fixed input).

#include "laige/render/camera.h"
#include "laige/render/matrices.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/matrix.hpp>  // Mat4 operator* (the viewProjection check)
#include <glm/vec4.hpp>    // the world -> NDC point multiply

#include <gtest/gtest.h>

#include "laige/errors.h"
#include "laige/logging.h"

namespace {

using laige::ErrorCode;
using laige::render::Camera;
using laige::render::CameraBounds;
using laige::render::CameraOptions;
using laige::render::CameraProjection;
using laige::render::Mat4;
using laige::render::Vec2;
using laige::render::Vec3;

// ---------------------------------------------------------------------------
// Log capture (the iso_depth_table_tests MemorySink pattern — rate
// limiting OFF so the tests assert per-event counts, not the facade's
// LOG-004 window).
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

// Creates a camera from options; on a rejected config it fails the test
// and hands back the stopped state (a valid Camera{}).
Camera makeCamera(CameraOptions o = CameraOptions{}) {
  auto r = Camera::create(std::move(o));
  EXPECT_TRUE(r.ok());
  if (r.isError()) return Camera{};
  return *r.valueIfOk();
}

// CHECKs all 16 elements of a against b (column-major m[c][r]).
void expectMatrix(const Mat4& a, const Mat4& b, const std::string& what) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_FLOAT_EQ(a[c][r], b[c][r]) << what << ": m[" << c << "][" << r
                                        << "]";
    }
  }
}

Mat4 identity() { return Mat4(1.0f); }

// CHECKs a Vec3 component by component with exact bit equality.
void expectVec3(const Vec3& a, const Vec3& b, const std::string& what) {
  EXPECT_FLOAT_EQ(a.x, b.x) << what << ": x";
  EXPECT_FLOAT_EQ(a.y, b.y) << what << ": y";
  EXPECT_FLOAT_EQ(a.z, b.z) << what << ": z";
}

}  // namespace

// ---------------------------------------------------------------------------
// Camera::create — validation (the header preamble's failure section)
// ---------------------------------------------------------------------------

TEST(CameraCreate, ValidDefaults) {
  MemorySink* sink = installCaptureSink();
  auto r = Camera::create(CameraOptions{});
  ASSERT_TRUE(r.ok());
  const Camera& cam = *r.valueIfOk();
  EXPECT_TRUE(cam.valid());
  expectVec3(cam.position(), Vec3{0.0f, 0.0f, 0.0f}, "default position");
  expectVec3(cam.target(), Vec3{0.0f, 0.0f, -1.0f}, "default target");
  expectVec3(cam.up(), Vec3{0.0f, 1.0f, 0.0f}, "default up");
  EXPECT_EQ(cam.projectionKind(), CameraProjection::Ortho);
  EXPECT_FLOAT_EQ(cam.zoom(), 1.0f);
  EXPECT_FALSE(cam.following());
  expectVec3(cam.shakeOffset(), Vec3{0.0f, 0.0f, 0.0f}, "default shake");
  // The healthy create path logs nothing (LOG-003: no warn on success).
  EXPECT_EQ(countEvents(*sink, "camera", "options_invalid"), 0u);
}

TEST(CameraCreate, RejectsInvalidOptions) {
  MemorySink* sink = installCaptureSink();
  const float kPi = 3.14159265358979323846f;

  struct Case {
    const char* name;
    CameraOptions options;
  };
  std::vector<Case> cases;
  auto add = [&](const char* name, CameraOptions o) {
    cases.push_back(Case{name, std::move(o)});
  };

  CameraOptions o;
  o.aspect = 0.0f;
  add("aspect_zero", o);
  o.aspect = std::nanf("");
  add("aspect_nan", o);

  o.aspect = 1.0f;
  o.zNear = 5.0f;
  o.zFar = 1.0f;
  add("depth_slab_inverted", o);

  o.zNear = 1.0f;
  o.zFar = 100.0f;
  o.halfWidth = 0.0f;
  add("half_width_zero", o);
  o.halfWidth = 10.0f;
  o.halfHeight = -1.0f;
  add("half_height_negative", o);

  o.halfHeight = 5.0f;
  o.projection = CameraProjection::Perspective;
  o.fovY = 0.0f;
  add("fov_zero", o);
  o.fovY = kPi;  // the strict upper bound: 0 < fovY < pi
  add("fov_pi", o);
  o.fovY = 4.0f;  // > pi
  add("fov_above_pi", o);

  o.projection = CameraProjection::Ortho;
  o.fovY = 1.0f;
  o.zoomMin = 0.0f;
  add("zoom_min_zero", o);

  o.zoomMin = 0.1f;
  o.zoomMax = 0.05f;
  add("zoom_max_below_min", o);

  o.zoomMax = 16.0f;
  o.zoom = std::nanf("");
  add("zoom_nan", o);

  o.zoom = 1.0f;
  o.followLerp = 0.0f;
  add("follow_lerp_zero", o);
  o.followLerp = 1.0000001f;
  add("follow_lerp_above_one", o);

  o.followLerp = 0.2f;
  o.shakeDecay = 1.0f;
  add("shake_decay_one", o);
  o.shakeDecay = -0.1f;
  add("shake_decay_negative", o);

  o.shakeDecay = 0.5f;
  o.maxShakeOffset = -0.25f;
  add("max_shake_offset_negative", o);

  o.maxShakeOffset = 0.25f;
  o.position = Vec3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
  add("position_infinite", o);

  o.position = Vec3{0.0f, 0.0f, 0.0f};
  o.target = Vec3{0.0f, 0.0f, 0.0f};  // distance 0: no look-at margin
  add("eye_at_target", o);

  o.target = Vec3{0.25f, 0.0f, 0.0f};  // distance == maxShakeOffset (not >)
  add("margin_at_shake_bound", o);

  o.target = Vec3{0.0f, 1.0f, 0.0f};  // up parallel to the view direction
  add("up_parallel", o);

  o.target = Vec3{0.0f, 0.0f, -1.0f};
  o.boundsEnabled = true;
  o.bounds = CameraBounds{Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}};
  add("bounds_min_above_max", o);

  o.boundsEnabled = false;
  o.up = Vec3{0.0f, 0.0f, 0.0f};
  add("up_zero", o);

  ASSERT_FALSE(cases.empty());
  for (const Case& c : cases) {
    auto r = Camera::create(c.options);
    EXPECT_TRUE(r.isError()) << c.name;
    if (r.isError()) {
      EXPECT_EQ(r.error(), ErrorCode::InvalidArgument) << c.name;
    }
  }
  // One warn per failed create (rate limiting is off in the capture
  // sink), each naming the first failing option (LOG-001/LOG-002).
  EXPECT_EQ(countEvents(*sink, "camera", "options_invalid"), cases.size());
  const MemorySink::Entry* first = firstEvent(*sink, "options_invalid");
  ASSERT_NE(first, nullptr);
  // The first case fails on the aspect; the warn names the stable
  // option (not the test's case label).
  EXPECT_EQ(fieldOf(*first, "option"), "aspect");
}

TEST(CameraCreate, InitialPositionClampedToRect) {
  CameraOptions o;
  o.boundsEnabled = true;
  o.bounds = CameraBounds{Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}};
  o.position = Vec3{5.0f, -2.0f, 0.5f};
  Camera cam = makeCamera(o);
  // (x, y) land on the rectangle; z is unconstrained (the camera's
  // height is free).
  expectVec3(cam.position(), Vec3{1.0f, 0.0f, 0.5f}, "clamped position");
}

TEST(CameraCreate, InitialZoomClamped) {
  CameraOptions o;
  o.zoom = 100.0f;
  Camera high = makeCamera(o);
  EXPECT_FLOAT_EQ(high.zoom(), o.zoomMax);
  o.zoom = 0.01f;
  Camera low = makeCamera(o);
  EXPECT_FLOAT_EQ(low.zoom(), o.zoomMin);
}

// ---------------------------------------------------------------------------
// Zoom — clamped exactly at the bounds; the projection scales
// ---------------------------------------------------------------------------

TEST(CameraZoom, ClampExactAtBounds) {
  CameraOptions o;
  o.zoomMin = 0.5f;
  o.zoomMax = 4.0f;
  Camera cam = makeCamera(o);
  EXPECT_TRUE(cam.setZoom(0.0f).ok());
  EXPECT_FLOAT_EQ(cam.zoom(), 0.5f);  // below min -> exactly min
  EXPECT_TRUE(cam.setZoom(1e30f).ok());
  EXPECT_FLOAT_EQ(cam.zoom(), 4.0f);  // above max -> exactly max
  EXPECT_TRUE(cam.setZoom(0.5f).ok());
  EXPECT_FLOAT_EQ(cam.zoom(), 0.5f);  // at min
  EXPECT_TRUE(cam.setZoom(4.0f).ok());
  EXPECT_FLOAT_EQ(cam.zoom(), 4.0f);  // at max
  EXPECT_TRUE(cam.setZoom(2.0f).ok());
  EXPECT_FLOAT_EQ(cam.zoom(), 2.0f);  // in range: unchanged
}

TEST(CameraZoom, RejectsNonFinite) {
  Camera cam = makeCamera();
  EXPECT_FALSE(cam.setZoom(std::nanf("")).ok());
  EXPECT_FALSE(cam.setZoom(std::numeric_limits<float>::infinity()).ok());
  EXPECT_FLOAT_EQ(cam.zoom(), 1.0f);  // unchanged on rejection
}

TEST(CameraZoom, OrthoProjectionScalesWithZoom) {
  CameraOptions o;
  o.halfWidth = 10.0f;
  o.halfHeight = 5.0f;
  o.zNear = 1.0f;
  o.zFar = 100.0f;
  Camera cam = makeCamera(o);
  EXPECT_TRUE(cam.setZoom(2.0f).ok());
  // 10/2 = 5 and 5/2 = 2.5 are exact dyadics: the zoomed projection is
  // the M2-GL-03 ortho of the halved box (bit-exact).
  expectMatrix(cam.projection(),
               laige::render::ortho(-5.0f, 5.0f, -2.5f, 2.5f, 1.0f, 100.0f),
               "zoomed ortho projection");
}

// ---------------------------------------------------------------------------
// The rectangular bounds — the camera stays inside under adversarial
// input (the roadmap's adversarial-input contract)
// ---------------------------------------------------------------------------

TEST(CameraBounds, KeepsInsideRectUnderAdversarialFollow) {
  CameraOptions o;
  o.boundsEnabled = true;
  o.bounds = CameraBounds{Vec2{-1.0f, -3.0f}, Vec2{2.0f, 1.0f}};
  o.followLerp = 0.5f;
  Camera cam = makeCamera(o);
  // A follow target far outside the rectangle: the follow step drags
  // the camera out every frame and the bounds clamp lands it back.
  ASSERT_TRUE(cam.setFollowTarget(Vec3{1000.0f, 1000.0f, 5.0f}).ok());
  for (int i = 0; i < 2000; ++i) {
    cam.update();
    const Vec3 p = cam.position();
    EXPECT_GE(p.x, -1.0f) << "frame " << i;
    EXPECT_LE(p.x, 2.0f) << "frame " << i;
    EXPECT_GE(p.y, -3.0f) << "frame " << i;
    EXPECT_LE(p.y, 1.0f) << "frame " << i;
  }
  // Converged to the rectangle's corner (the clamp returns the exact
  // bounds); z is unconstrained and slides: the look-at converges to
  // the target's z (5) and the camera keeps its initial height offset
  // (p0.z - t0.z = 1) — so z converges to 6.
  const Vec3 p = cam.position();
  EXPECT_FLOAT_EQ(p.x, 2.0f);
  EXPECT_FLOAT_EQ(p.y, 1.0f);
  EXPECT_NEAR(p.z, 6.0f, 1e-4f);
}

TEST(CameraBounds, SetPositionClamped) {
  CameraOptions o;
  o.boundsEnabled = true;
  o.bounds = CameraBounds{Vec2{-1.0f, -3.0f}, Vec2{2.0f, 1.0f}};
  Camera cam = makeCamera(o);
  EXPECT_TRUE(cam.setPosition(Vec3{100.0f, 100.0f, 0.0f}).ok());
  expectVec3(cam.position(), Vec3{2.0f, 1.0f, 0.0f}, "clamped set");
  // Inside the rect: unchanged.
  EXPECT_TRUE(cam.setPosition(Vec3{0.0f, 0.0f, 0.25f}).ok());
  expectVec3(cam.position(), Vec3{0.0f, 0.0f, 0.25f}, "unclamped set");
}

TEST(CameraBounds, ShakeExcursionBoundedPastTheEdge) {
  CameraOptions o;
  o.boundsEnabled = true;
  o.bounds = CameraBounds{Vec2{-1.0f, -3.0f}, Vec2{2.0f, 1.0f}};
  o.maxShakeOffset = 0.25f;
  Camera cam = makeCamera(o);
  ASSERT_TRUE(cam.setPosition(Vec3{2.0f, 1.0f, 0.0f}).ok());
  ASSERT_TRUE(cam.applyShake(Vec3{1.0f, 1.0f, 1.0f}).ok());
  const Vec3 base = cam.position();
  const Vec3 eff = cam.effectivePosition();
  // The base position stays inside the rectangle ...
  EXPECT_GE(base.x, -1.0f);
  EXPECT_LE(base.x, 2.0f);
  EXPECT_GE(base.y, -3.0f);
  EXPECT_LE(base.y, 1.0f);
  // ... and the effective position may exceed the edge by at most the
  // documented bound (the offset is clamped to maxShakeOffset).
  expectVec3(cam.shakeOffset(), Vec3{0.25f, 0.25f, 0.25f}, "clamped shake");
  EXPECT_LE(eff.x - base.x, 0.25f);
  EXPECT_LE(eff.y - base.y, 0.25f);
  EXPECT_FLOAT_EQ(eff.x, 2.25f);
  EXPECT_FLOAT_EQ(eff.y, 1.25f);
}

// ---------------------------------------------------------------------------
// Smooth follow — deterministic given fixed input (the roadmap's
// determinism contract)
// ---------------------------------------------------------------------------

namespace {

// The follow scenario: p0/t0 one unit apart along -z, the follow
// target 40/25 units away, lerp 0.25 (an exact dyadic).
constexpr Vec3 kFollowP0{0.0f, 5.0f, 10.0f};
constexpr Vec3 kFollowT0{0.0f, 5.0f, 9.0f};
constexpr Vec3 kFollowTarget{40.0f, 30.0f, 9.0f};
constexpr float kFollowLerp = 0.25f;

}  // namespace

TEST(CameraFollow, DeterministicExactSequence) {
  CameraOptions o;
  o.position = kFollowP0;
  o.target = kFollowT0;
  o.followLerp = kFollowLerp;
  Camera cam = makeCamera(o);
  ASSERT_TRUE(cam.setFollowTarget(kFollowTarget).ok());
  ASSERT_TRUE(cam.following());

  // The documented per-update step, recomputed independently in the
  // same float order: delta = (F - target) * lerp; both points move by
  // the same delta. Bit-exact against the camera's state.
  Vec3 p = kFollowP0, t = kFollowT0;
  for (int i = 0; i < 10; ++i) {
    const Vec3 delta = (kFollowTarget - t) * kFollowLerp;
    t += delta;
    p += delta;
    cam.update();
    expectVec3(cam.position(), p, "follow position");
    expectVec3(cam.target(), t, "follow target");
  }

  // Determinism: a second camera with the same input sequence ends in
  // bit-identical state.
  Camera other = makeCamera(o);
  ASSERT_TRUE(other.setFollowTarget(kFollowTarget).ok());
  for (int i = 0; i < 10; ++i) other.update();
  expectVec3(other.position(), cam.position(), "determinism position");
  expectVec3(other.target(), cam.target(), "determinism target");
  EXPECT_FLOAT_EQ(other.zoom(), cam.zoom());
}

TEST(CameraFollow, ConvergesToFollowTarget) {
  CameraOptions o;
  o.position = kFollowP0;
  o.target = kFollowT0;
  o.followLerp = kFollowLerp;
  Camera cam = makeCamera(o);
  ASSERT_TRUE(cam.setFollowTarget(kFollowTarget).ok());
  for (int i = 0; i < 1000; ++i) cam.update();
  // The look-at point converges to the follow target (the delta is
  // exact and underflows to zero long before 1000 updates)...
  expectVec3(cam.target(), kFollowTarget, "converged look-at");
  // ... and the camera sits one eye->look-at distance behind it, the
  // rigid-translation offset preserved within float rounding.
  const Vec3 p = cam.position();
  EXPECT_NEAR(p.x, 40.0f, 1e-5f);
  EXPECT_NEAR(p.y, 30.0f, 1e-5f);
  EXPECT_NEAR(p.z, 10.0f, 1e-5f);
}

TEST(CameraFollow, StopFollowingFreezes) {
  CameraOptions o;
  o.position = kFollowP0;
  o.target = kFollowT0;
  o.followLerp = kFollowLerp;
  Camera cam = makeCamera(o);
  ASSERT_TRUE(cam.setFollowTarget(kFollowTarget).ok());
  for (int i = 0; i < 5; ++i) cam.update();
  cam.stopFollowing();
  EXPECT_FALSE(cam.following());
  const Vec3 p = cam.position();
  const Vec3 t = cam.target();
  for (int i = 0; i < 5; ++i) cam.update();
  expectVec3(cam.position(), p, "frozen position");
  expectVec3(cam.target(), t, "frozen target");
}

// ---------------------------------------------------------------------------
// Shake — bounded, decaying to exactly zero within the documented tick
// count, deterministic given input
// ---------------------------------------------------------------------------

TEST(CameraShake, DecaysToZeroWithinDocumentedTicks) {
  CameraOptions o;
  o.maxShakeOffset = 1.0f;
  // Distance 2 > 1: keeps the look-at margin (|t - p| > maxShakeOffset).
  o.target = Vec3{0.0f, 0.0f, -2.0f};
  o.shakeDecay = 0.5f;  // the documented default: exact halving per update
  Camera cam = makeCamera(o);
  ASSERT_TRUE(cam.applyShake(Vec3{1.0f, 0.5f, -1.0f}).ok());
  expectVec3(cam.shakeOffset(), Vec3{1.0f, 0.5f, -1.0f}, "initial offset");

  // The documented bound: with decay 0.5 and a bound <= 1 world unit,
  // the offset reaches exactly 0.0 within 150 updates. At update 148
  // the x offset is still the exact dyadic 2^-148; at 149 it is the
  // last nonzero denormal 2^-149; at 150 the next halving, 2^-150,
  // rounds to exactly zero (IEEE binary32).
  for (int i = 0; i < 148; ++i) cam.update();
  EXPECT_FLOAT_EQ(cam.shakeOffset().x, std::ldexp(1.0f, -148));
  cam.update();
  EXPECT_FLOAT_EQ(cam.shakeOffset().x, std::ldexp(1.0f, -149));
  cam.update();  // the 150th
  expectVec3(cam.shakeOffset(), Vec3{0.0f, 0.0f, 0.0f}, "exact zero");
  // Still zero on further updates.
  cam.update();
  expectVec3(cam.shakeOffset(), Vec3{0.0f, 0.0f, 0.0f}, "stays zero");
}

TEST(CameraShake, BoundedByMaxOffset) {
  CameraOptions o;
  o.maxShakeOffset = 1.0f;
  // Distance 2 > 1: keeps the look-at margin (|t - p| > maxShakeOffset).
  o.target = Vec3{0.0f, 0.0f, -2.0f};
  o.shakeDecay = 0.5f;
  Camera cam = makeCamera(o);
  // An impulse far beyond the bound clamps to the bound exactly.
  ASSERT_TRUE(cam.applyShake(Vec3{100.0f, 100.0f, 100.0f}).ok());
  expectVec3(cam.shakeOffset(), Vec3{1.0f, 1.0f, 1.0f}, "clamped impulse");
  // The bound holds after accumulation in every direction.
  ASSERT_TRUE(cam.applyShake(Vec3{100.0f, -100.0f, 100.0f}).ok());
  expectVec3(cam.shakeOffset(), Vec3{1.0f, -1.0f, 1.0f}, "clamped sum");
}

TEST(CameraShake, DeterministicFixedSequence) {
  CameraOptions o;
  o.maxShakeOffset = 1.0f;
  // Distance 2 > 1: keeps the look-at margin (|t - p| > maxShakeOffset).
  o.target = Vec3{0.0f, 0.0f, -2.0f};
  o.shakeDecay = 0.5f;
  Camera cam = makeCamera(o);
  ASSERT_TRUE(cam.applyShake(Vec3{0.5f, 0.0f, 0.0f}).ok());
  cam.update();  // 0.5 * 0.5 = 0.25
  ASSERT_TRUE(cam.applyShake(Vec3{0.25f, 0.0f, 0.0f}).ok());  // 0.25 + 0.25
  cam.update();  // 0.5 * 0.5 = 0.25
  cam.update();  // 0.25 * 0.5 = 0.125
  expectVec3(cam.shakeOffset(), Vec3{0.125f, 0.0f, 0.0f}, "fixed sequence");
}

TEST(CameraShake, ZeroDecayKillsInOneUpdate) {
  CameraOptions o;
  o.maxShakeOffset = 1.0f;
  // Distance 2 > 1: keeps the look-at margin (|t - p| > maxShakeOffset).
  o.target = Vec3{0.0f, 0.0f, -2.0f};
  o.shakeDecay = 0.0f;
  Camera cam = makeCamera(o);
  ASSERT_TRUE(cam.applyShake(Vec3{0.5f, 0.0f, 0.0f}).ok());
  cam.update();
  expectVec3(cam.shakeOffset(), Vec3{0.0f, 0.0f, 0.0f}, "killed in one");
}

TEST(CameraShake, RejectsNonFinite) {
  Camera cam = makeCamera();
  EXPECT_FALSE(cam.applyShake(Vec3{std::nanf(""), 0.0f, 0.0f}).ok());
  expectVec3(cam.shakeOffset(), Vec3{0.0f, 0.0f, 0.0f}, "unchanged");
}

// ---------------------------------------------------------------------------
// The matrix builds (delegation to the M2-GL-03 builders)
// ---------------------------------------------------------------------------

TEST(CameraMatrices, ViewIsLookAtOfEffectivePosition) {
  Camera cam = makeCamera();  // position (0,0,0), target (0,0,-1), up (0,1,0)
  expectMatrix(cam.view(),
               laige::render::lookAt(Vec3{0.0f, 0.0f, 0.0f},
                                     Vec3{0.0f, 0.0f, -1.0f},
                                     Vec3{0.0f, 1.0f, 0.0f}),
               "base view");
  // The shake moves the eye (0 + 0.1 and 0 + 0.25 are exact dyadics).
  ASSERT_TRUE(cam.applyShake(Vec3{0.1f, 0.25f, 0.0f}).ok());
  expectMatrix(cam.view(),
               laige::render::lookAt(Vec3{0.1f, 0.25f, 0.0f},
                                     Vec3{0.0f, 0.0f, -1.0f},
                                     Vec3{0.0f, 1.0f, 0.0f}),
               "shaken view");
}

TEST(CameraMatrices, ViewProjectionCombines) {
  CameraOptions o;
  o.position = Vec3{0.0f, 0.0f, 5.0f};
  o.target = Vec3{0.0f, 0.0f, 0.0f};
  Camera cam = makeCamera(o);
  const Mat4 vp = cam.viewProjection();
  // projection() * view() applied to a world point (the per-sprite
  // world -> NDC multiply, M2-SPRITE-02) — the same GLM composition.
  const Vec3 p(1.0f, 2.0f, 3.0f);
  const glm::vec4 viaCombined = vp * glm::vec4{p, 1.0f};
  const glm::vec4 viaSeparate =
      cam.projection() * cam.view() * glm::vec4{p, 1.0f};
  expectVec3(Vec3(viaCombined.x, viaCombined.y, viaCombined.z),
             Vec3(viaSeparate.x, viaSeparate.y, viaSeparate.z),
             "combined vs separate");
}

TEST(CameraMatrices, PerspectiveProjectionFormula) {
  CameraOptions o;
  o.projection = CameraProjection::Perspective;
  o.fovY = 60.0f * laige::render::kCameraDegreesToRadians;
  o.aspect = 1.5f;
  o.zNear = 1.0f;
  o.zFar = 10.0f;
  Camera cam = makeCamera(o);
  // Zoom 2: the documented fov_eff = 2*atan(tan(fovY/2)/Z) — the same
  // float expression as the engine's, so bit-exact.
  ASSERT_TRUE(cam.setZoom(2.0f).ok());
  expectMatrix(cam.projection(),
               laige::render::perspective(
                   2.0f * std::atan(std::tan(o.fovY * 0.5f) / 2.0f), 1.5f, 1.0f,
                   10.0f),
               "zoomed perspective");
  // Zoom 1: the round-trip through tan/atan stays within 1-2 ulp.
  ASSERT_TRUE(cam.setZoom(1.0f).ok());
  const Mat4 atOne = cam.projection();
  const Mat4 direct = laige::render::perspective(o.fovY, 1.5f, 1.0f, 10.0f);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_NEAR(atOne[c][r], direct[c][r], 1e-6f)
          << "zoom-1 m[" << c << "][" << r << "]";
    }
  }
}

TEST(CameraMatrices, StoppedCameraIsIdentity) {
  Camera cam;  // the stopped state (no valid configuration)
  EXPECT_FALSE(cam.valid());
  expectMatrix(cam.view(), identity(), "stopped view");
  expectMatrix(cam.projection(), identity(), "stopped projection");
  expectMatrix(cam.viewProjection(), identity(), "stopped viewProjection");
  EXPECT_FALSE(cam.setZoom(2.0f).ok());
  EXPECT_FALSE(cam.setPosition(Vec3{1.0f, 0.0f, 0.0f}).ok());
  EXPECT_FALSE(cam.setTarget(Vec3{0.0f, 0.0f, -1.0f}).ok());
  EXPECT_FALSE(cam.setFollowTarget(Vec3{1.0f, 0.0f, 0.0f}).ok());
  EXPECT_FALSE(cam.applyShake(Vec3{1.0f, 0.0f, 0.0f}).ok());
  cam.update();  // no-op on a stopped camera
  EXPECT_FALSE(cam.valid());
}

// ---------------------------------------------------------------------------
// The setter contracts (boundary validation; the stopped-state and
// margin precedents)
// ---------------------------------------------------------------------------

TEST(CameraSetters, MarginViolationRejected) {
  MemorySink* sink = installCaptureSink();
  Camera cam = makeCamera();  // target (0,0,-1), maxShakeOffset 0.25
  // |target - p| = 0.2 <= 0.25: the look-at margin is violated.
  EXPECT_FALSE(cam.setPosition(Vec3{-0.2f, 0.0f, -1.0f}).ok());
  expectVec3(cam.position(), Vec3{0.0f, 0.0f, 0.0f}, "unchanged position");
  // |p - target| = 0.2: mirrored.
  EXPECT_FALSE(cam.setTarget(Vec3{0.2f, 0.0f, 0.0f}).ok());
  expectVec3(cam.target(), Vec3{0.0f, 0.0f, -1.0f}, "unchanged target");
  // One warn per rejection (rate limiting off), naming the margin.
  EXPECT_EQ(countEvents(*sink, "camera", "lookat_margin_violated"), 2u);
  const MemorySink::Entry* e = firstEvent(*sink, "lookat_margin_violated");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "min_distance"), "0.25");
}

TEST(CameraSetters, UpParallelRejected) {
  Camera cam = makeCamera();  // up (0,1,0), distance 1 > 0.25 margin
  EXPECT_FALSE(cam.setTarget(Vec3{0.0f, 1.0f, 0.0f}).ok());
  expectVec3(cam.target(), Vec3{0.0f, 0.0f, -1.0f}, "unchanged target");
}

TEST(CameraSetters, RejectsNonFinite) {
  MemorySink* sink = installCaptureSink();
  Camera cam = makeCamera();
  EXPECT_FALSE(cam.setPosition(Vec3{std::nanf(""), 0.0f, 0.0f}).ok());
  EXPECT_FALSE(cam.setTarget(Vec3{0.0f, 0.0f, std::nanf("")}).ok());
  EXPECT_FALSE(cam.setFollowTarget(Vec3{0.0f, 0.0f, std::nanf("")}).ok());
  EXPECT_EQ(countEvents(*sink, "camera", "non_finite_input"), 3u);
  const MemorySink::Entry* e = firstEvent(*sink, "non_finite_input");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(fieldOf(*e, "input"), "position");
}
