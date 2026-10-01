// laige-render projection modes + screen<->world transform tests
// (M2-PROJ-01): the ProjectionView transforms in
// laige/render/projection.h.
//
// Pure value math — no GL context, no GL environment needed: every
// suite runs in every local tree and in CI. The goldens are
// HAND-COMPUTED from the documented matrix formulas (the mode builders
// of matrices.h / camera.h); the round-trip tests pin the documented
// precision (kProjectionRoundTripTolerance) on 10k random points per
// mode (TestPrng substreams — docs/testing.md §4).

#include "laige/render/camera.h"
#include "laige/render/projection.h"

#include <cmath>

#include <gtest/gtest.h>

#include "laige_test_seed.h"

namespace {

using laige::ErrorCode;
using laige::render::IsoAxes;
using laige::render::Mat4;
using laige::render::Plane;
using laige::render::ProjectionMode;
using laige::render::ProjectionView;
using laige::render::Vec2;
using laige::render::Vec3;
using laige::render::WorldRay;

// Golden tolerance: the expected values are exact doubles; the
// transforms produce float. 1e-6 is ~0.1 ulp at |v| = 1 and safe at
// the magnitudes used here (<= ~100).
constexpr float kEps = 1e-6f;
// The round-trip tolerance is the header's documented constant — the
// tests assert against the shipped value, not a local copy.
constexpr float kRoundTrip = laige::render::kProjectionRoundTripTolerance;

// Test substream ids (docs/testing.md §4 — one per randomized suite so
// streams never share a position; stable named constants, CORE-005).
constexpr std::uint32_t kIsoRoundTripStream = 2101;
constexpr std::uint32_t kSideRoundTripStream = 2102;
constexpr std::uint32_t kTopRoundTripStream = 2103;
constexpr std::uint32_t kCineOrthoRoundTripStream = 2104;
constexpr std::uint32_t kCinePerspRoundTripStream = 2105;

// A uniform float in [lo, hi) (24-bit resolution, the Prng contract).
float uniformIn(laige::Prng& rng, float lo, float hi) {
  return lo + rng.next_float01() * (hi - lo);
}

// --- the mode views (the documented builder per mode) -------------------

// The 2:1 dimetric iso view at scale 1 (the engine default, ADR 0005):
// screen_x = 2x - 2y, screen_y = -x - y + z, screen_z = 0.
ProjectionView isoView() {
  ProjectionView v;
  v.mode = ProjectionMode::Iso;
  v.matrix = laige::render::isoDimetric2To1(1.0f);
  return v;
}

// The true 30°/60° iso view at scale 1: screen_x = x - y, screen_y =
// -(x + y)/√3 + z/√3.
ProjectionView trueIsoView() {
  ProjectionView v;
  v.mode = ProjectionMode::Iso;
  v.matrix = laige::render::isoTrueIso3060(1.0f);
  return v;
}

// An invertible CUSTOM shear that is NOT depth-key-supported (the
// transforms must work for any invertible shear — the isoDepthKey
// order is simply not guaranteed for it, M2-ISO-01's contract):
// screen_x = 1.5x - y, screen_y = -x - 0.5y + z.
ProjectionView customShearView() {
  ProjectionView v;
  v.mode = ProjectionMode::Iso;
  v.matrix = laige::render::isoMatrix(IsoAxes{
      Vec2{1.5f, -1.0f}, Vec2{-1.0f, -0.5f}, 1.0f});
  return v;
}

// The side_view plane camera: center (5, 7, 2), right (1,0,0),
// up (0,0,1) — screen x = world x, screen y = world height; the
// reference plane is y = 7 (through planeCenter, perpendicular to the
// view). halfWidth 10, halfHeight 5, slab [1, 100].
ProjectionView sideView() {
  ProjectionView v;
  v.mode = ProjectionMode::SideView;
  v.planeCenter = Vec3{5.0f, 7.0f, 2.0f};
  v.matrix = laige::render::planeOrtho(Vec3{5.0f, 7.0f, 2.0f},
                                       Vec3{1.0f, 0.0f, 0.0f},
                                       Vec3{0.0f, 0.0f, 1.0f}, 10.0f, 5.0f,
                                       1.0f, 100.0f);
  return v;
}

// The top_down plane camera: center (5, 7, 0), right (1,0,0),
// up (0,1,0) — the X/Y ground plane; the reference plane is z = 0.
ProjectionView topView() {
  ProjectionView v;
  v.mode = ProjectionMode::TopDown;
  v.planeCenter = Vec3{5.0f, 7.0f, 0.0f};
  v.matrix = laige::render::planeOrtho(Vec3{5.0f, 7.0f, 0.0f},
                                       Vec3{1.0f, 0.0f, 0.0f},
                                       Vec3{0.0f, 1.0f, 0.0f}, 10.0f, 5.0f,
                                       1.0f, 100.0f);
  return v;
}

// The free_cinematic ortho camera: position (0,0,10), look-at
// (0,0,0), up (0,1,0); ortho box 20 x 10 world units, slab [1, 100].
ProjectionView cineOrthoView() {
  laige::render::CameraOptions o;
  o.position = Vec3{0.0f, 0.0f, 10.0f};
  o.target = Vec3{0.0f, 0.0f, 0.0f};
  o.up = Vec3{0.0f, 1.0f, 0.0f};
  o.projection = laige::render::CameraProjection::Ortho;
  o.halfWidth = 10.0f;
  o.halfHeight = 5.0f;
  o.zNear = 1.0f;
  o.zFar = 100.0f;
  auto cam = laige::render::Camera::create(o);
  EXPECT_TRUE(cam.ok());
  ProjectionView v;
  v.mode = ProjectionMode::FreeCinematic;
  v.matrix = cam.value().viewProjection();
  return v;
}

// The free_cinematic perspective camera: position (0,0,20), look-at
// (0,0,0), fovY 60°, aspect 16:9, slab [1, 100].
ProjectionView cinePerspView() {
  laige::render::CameraOptions o;
  o.position = Vec3{0.0f, 0.0f, 20.0f};
  o.target = Vec3{0.0f, 0.0f, 0.0f};
  o.up = Vec3{0.0f, 1.0f, 0.0f};
  o.projection = laige::render::CameraProjection::Perspective;
  o.aspect = 16.0f / 9.0f;
  o.fovY = 60.0f * laige::render::kCameraDegreesToRadians;
  o.zNear = 1.0f;
  o.zFar = 100.0f;
  auto cam = laige::render::Camera::create(o);
  EXPECT_TRUE(cam.ok());
  ProjectionView v;
  v.mode = ProjectionMode::FreeCinematic;
  v.matrix = cam.value().viewProjection();
  return v;
}

// CHECKs that view.worldToScreen(p2d, depth) is the NDC point e.
void expectScreen(const ProjectionView& v, Vec2 p2d, float depth, Vec3 e) {
  const Vec3 s = v.worldToScreen(p2d, depth);
  EXPECT_NEAR(s.x, e.x, kEps) << "ndc x";
  EXPECT_NEAR(s.y, e.y, kEps) << "ndc y";
  EXPECT_NEAR(s.z, e.z, kEps) << "ndc z";
}

// CHECKs the round-trip: screenToWorld(worldToScreen(p), plane) == p
// within the documented tolerance (the component-wise error bound).
void expectRoundTrip(const ProjectionView& v, Vec3 p, Plane plane,
                     const char* what) {
  const Vec2 p2d{p.x, p.y};
  const Vec3 s = v.worldToScreen(p2d, p.z);
  auto hit = v.screenToWorld(s, plane);
  if (!hit.ok()) {
    ADD_FAILURE() << what << ": screenToWorld failed the round trip for "
                  << "(" << p.x << ", " << p.y << ", " << p.z << ")";
    return;
  }
  const Vec3 q = *hit.valueIfOk();
  EXPECT_NEAR(q.x, p.x, kRoundTrip) << what << ": x";
  EXPECT_NEAR(q.y, p.y, kRoundTrip) << what << ": y";
  EXPECT_NEAR(q.z, p.z, kRoundTrip) << what << ": z";
}

// CHECKs that a WorldRay has the given origin and (normalized)
// direction.
void expectRay(const WorldRay& r, Vec3 origin, Vec3 dir) {
  EXPECT_NEAR(r.origin.x, origin.x, kEps) << "origin x";
  EXPECT_NEAR(r.origin.y, origin.y, kEps) << "origin y";
  EXPECT_NEAR(r.origin.z, origin.z, kEps) << "origin z";
  EXPECT_NEAR(r.direction.x, dir.x, kEps) << "dir x";
  EXPECT_NEAR(r.direction.y, dir.y, kEps) << "dir y";
  EXPECT_NEAR(r.direction.z, dir.z, kEps) << "dir z";
}

}  // namespace

// ---------------------------------------------------------------------------
// The defaults and the pure-function contract
// ---------------------------------------------------------------------------

TEST(ProjectionDefaults, ModeAndMatrixDefaults) {
  // iso is the engine default (ADR 0005): the default-constructed view
  // is the Iso mode with the identity matrix (the empty state) and the
  // origin reference plane.
  const ProjectionView v{};
  EXPECT_EQ(v.mode, ProjectionMode::Iso);
  // Extra parentheses: the macro preprocessor would otherwise count the
  // brace-init commas as macro arguments.
  EXPECT_EQ((v.planeCenter), (Vec3{0.0f, 0.0f, 0.0f}));
  const Mat4 eye = Mat4(1.0f);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_EQ(v.matrix[c][r], eye[c][r]) << "m[" << c << "][" << r << "]";
    }
  }
  // The identity view maps world points to themselves (w = 1).
  expectScreen(v, Vec2{3.5f, -2.25f}, 7.5f, Vec3{3.5f, -2.25f, 7.5f});
}

TEST(ProjectionDefaults, PureAndDeterministic) {
  const ProjectionView v = isoView();
  // No mutation: the view is bit-identical after the transform calls.
  const Vec3 s1 = v.worldToScreen(Vec2{1.5f, -2.0f}, 3.0f);
  const WorldRay r1 = v.screenToWorldRay(Vec2{0.25f, -0.125f});
  (void)v.screenToWorld(Vec2{0.25f, -0.125f}, Plane{Vec3{0.0f, 0.0f, 1.0f},
                                                     3.0f});
  EXPECT_EQ(v.mode, ProjectionMode::Iso);
  EXPECT_EQ((v.planeCenter), (Vec3{0.0f, 0.0f, 0.0f}));
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_EQ(v.matrix[c][r], isoView().matrix[c][r]);
    }
  }
  // Deterministic: repeated calls are bit-identical (no RNG, no clock,
  // fixed float-op sequence — the camera.h determinism contract).
  EXPECT_EQ(v.worldToScreen(Vec2{1.5f, -2.0f}, 3.0f), s1);
  const WorldRay r2 = v.screenToWorldRay(Vec2{0.25f, -0.125f});
  EXPECT_EQ(r1.origin, r2.origin);
  EXPECT_EQ(r1.direction, r2.direction);
}

// ---------------------------------------------------------------------------
// Iso mode (the primary projection; 2:1 dimetric default, ADR 0005)
// ---------------------------------------------------------------------------

TEST(ProjectionIsoGoldens, WorldToScreen) {
  // Hand-computed from screen_x = 2x - 2y, screen_y = -x - y + z,
  // screen_z = 0 (isoDimetric2To1(1): dx = (2,-1), dy = (-2,-1),
  // zUnit = 1).
  const ProjectionView v = isoView();
  expectScreen(v, Vec2{1.0f, 0.0f}, 0.0f, Vec3{2.0f, -1.0f, 0.0f});
  expectScreen(v, Vec2{0.0f, 1.0f}, 0.0f, Vec3{-2.0f, -1.0f, 0.0f});
  expectScreen(v, Vec2{0.0f, 0.0f}, 3.0f, Vec3{0.0f, 3.0f, 0.0f});
  expectScreen(v, Vec2{2.0f, 3.0f}, 1.0f, Vec3{-2.0f, -4.0f, 0.0f});
  // NDC-z is EXACTLY 0 for every point (the iso builders' contract:
  // the 2.5D depth is engine-owned, never derived from the projection).
  EXPECT_EQ(v.worldToScreen(Vec2{1.5f, -2.25f}, 4.0f).z, 0.0f);
}

TEST(ProjectionIsoGoldens, TrueIsoAndCustomShear) {
  // True 30°/60° (isoTrueIso3060(1)): dx = (1, -1/√3), dy = (-1, -1/√3),
  // zUnit = 1/√3.
  const ProjectionView v = trueIsoView();
  const double invSqrt3 = 0.57735026918962576451;
  expectScreen(v, Vec2{1.0f, 0.0f}, 0.0f,
               Vec3{1.0f, static_cast<float>(-invSqrt3), 0.0f});
  expectScreen(v, Vec2{0.0f, 0.0f}, 1.0f,
               Vec3{0.0f, static_cast<float>(invSqrt3), 0.0f});

  // A custom shear that is invertible but NOT depth-key-supported:
  // the transforms still work (M2-ISO-01: it renders; the key order is
  // simply not guaranteed). screen_x = 1.5x - y, screen_y = -x - 0.5y + z.
  const ProjectionView c = customShearView();
  expectScreen(c, Vec2{1.0f, 0.0f}, 0.0f, Vec3{1.5f, -1.0f, 0.0f});
  expectScreen(c, Vec2{2.0f, 2.0f}, 1.0f, Vec3{1.0f, -2.0f, 0.0f});
}

TEST(ProjectionIsoRay, OriginOnGroundPlaneAndDirection) {
  const ProjectionView v = isoView();
  // The direction: cross of the screen rows (2, -2, 0) x (-1, -1, 1) =
  // (-2, -2, -4) -> (-1, -1, -2)/√6 (downward — away from the overhead
  // viewer; the preimage line runs along (1, 1, 2)).
  const double len6 = std::sqrt(6.0);
  const Vec3 dir{static_cast<float>(-1.0 / len6),
                 static_cast<float>(-1.0 / len6),
                 static_cast<float>(-2.0 / len6)};
  // The screen center is the world origin (the 2x2 inverse of (0, 0));
  // the direction is screen-point-independent (the preimage is a
  // parallel family of lines for the affine modes).
  const WorldRay r0 = v.screenToWorldRay(Vec2{0.0f, 0.0f});
  expectRay(r0, Vec3{0.0f, 0.0f, 0.0f}, dir);
  // The screen point of world (1, 0, 0) is (2, -1): the origin of its
  // preimage line sits on the ground plane exactly at (1, 0, 0).
  const WorldRay r1 = v.screenToWorldRay(Vec2{2.0f, -1.0f});
  expectRay(r1, Vec3{1.0f, 0.0f, 0.0f}, dir);
  // A third screen point: same direction, a different origin.
  const WorldRay r2 = v.screenToWorldRay(Vec2{0.5f, 0.25f});
  EXPECT_NEAR(r2.direction.x, r0.direction.x, kEps);
  EXPECT_NEAR(r2.direction.y, r0.direction.y, kEps);
  EXPECT_NEAR(r2.direction.z, r0.direction.z, kEps);
}

TEST(ProjectionIsoRoundTrip, FixedPoints) {
  const ProjectionView v = isoView();
  // The round-trip plane is z = p.z (the mode's canonical plane — any
  // z = const plane crosses the preimage line exactly once).
  struct P {
    float x, y, z;
  };
  const P pts[] = {{1.0f, 0.0f, 0.0f},     {0.0f, 1.0f, 0.0f},
                   {0.0f, 0.0f, 3.0f},     {2.0f, 3.0f, 1.0f},
                   {-5.5f, 2.25f, 0.0f},   {3.75f, -1.5f, 7.0f},
                   {-32.0f, 32.0f, 32.0f}, {32.0f, -32.0f, 0.0f}};
  for (const P& p : pts) {
    expectRoundTrip(v, Vec3{p.x, p.y, p.z},
                    Plane{Vec3{0.0f, 0.0f, 1.0f}, p.z}, "iso fixed");
  }
}

TEST(ProjectionIsoRoundTrip, TenThousandRandomPoints) {
  const ProjectionView v = isoView();
  laige::Prng rng = laige::testing::TestPrng(kIsoRoundTripStream);
  for (int i = 0; i < 10000; ++i) {
    const Vec3 p{uniformIn(rng, -32.0f, 32.0f), uniformIn(rng, -32.0f, 32.0f),
                 uniformIn(rng, 0.0f, 32.0f)};
    expectRoundTrip(v, p, Plane{Vec3{0.0f, 0.0f, 1.0f}, p.z},
                    "iso random");
  }
}

// ---------------------------------------------------------------------------
// side_view (screen y = world height)
// ---------------------------------------------------------------------------

TEST(ProjectionSideViewGoldens, WorldToScreen) {
  // Hand-computed from the planeOrtho(center (5,7,2), right (1,0,0),
  // up (0,0,1), halfWidth 10, halfHeight 5, slab [1, 100]) structure:
  // ndc_x = (x - 5)/10, ndc_y = (z - 2)/5, ndc_z = (-2·((y - 7) - 1) -
  // 101)/99 (the camera looks along +y; n = (0,-1,0), toward the viewer
  // at y = 6).
  const ProjectionView v = sideView();
  expectScreen(v, Vec2{5.0f, 7.0f}, 2.0f, Vec3{0.0f, 0.0f, -1.0f});
  expectScreen(v, Vec2{15.0f, 7.0f}, 2.0f, Vec3{1.0f, 0.0f, -1.0f});
  expectScreen(v, Vec2{5.0f, 7.0f}, 7.0f, Vec3{0.0f, 1.0f, -1.0f});
  expectScreen(v, Vec2{5.0f, 17.0f}, 2.0f,
               Vec3{0.0f, 0.0f, static_cast<float>(-79.0 / 99.0)});
}

TEST(ProjectionSideViewGoldens, Ray) {
  const ProjectionView v = sideView();
  // The screen center's preimage line runs along world y through the
  // center (on the reference plane y = 7); direction (0, -1, 0) = the
  // cross of the screen rows ((1/10, 0, 0) × (0, 0, 2/99)).
  expectRay(v.screenToWorldRay(Vec2{0.0f, 0.0f}), Vec3{5.0f, 7.0f, 2.0f},
            Vec3{0.0f, -1.0f, 0.0f});
  // The screen point of world (15, 7, 2) is (1, 0): its line's origin
  // is (15, 7, 2).
  expectRay(v.screenToWorldRay(Vec2{1.0f, 0.0f}), Vec3{15.0f, 7.0f, 2.0f},
            Vec3{0.0f, -1.0f, 0.0f});
}

TEST(ProjectionSideViewRoundTrip, TenThousandRandomPoints) {
  // The canonical plane is y = p.y (any y = const plane crosses the
  // preimage line exactly once) — the round trip holds for ANY depth,
  // not just the reference plane.
  const ProjectionView v = sideView();
  laige::Prng rng = laige::testing::TestPrng(kSideRoundTripStream);
  for (int i = 0; i < 10000; ++i) {
    const Vec3 p{uniformIn(rng, -32.0f, 32.0f), uniformIn(rng, 0.0f, 100.0f),
                 uniformIn(rng, -10.0f, 10.0f)};
    expectRoundTrip(v, p, Plane{Vec3{0.0f, 1.0f, 0.0f}, p.y},
                    "side random");
  }
}

// ---------------------------------------------------------------------------
// top_down (the X/Y ground plane)
// ---------------------------------------------------------------------------

TEST(ProjectionTopDownGoldens, WorldToScreen) {
  // Hand-computed from the planeOrtho(center (5,7,0), right (1,0,0),
  // up (0,1,0), halfWidth 10, halfHeight 5, slab [1, 100]) structure:
  // ndc_x = (x - 5)/10, ndc_y = (y - 7)/5, ndc_z = (-2·(z - 1) -
  // 101)/99 (the camera looks along -z; n = (0,0,1), toward the viewer
  // above).
  const ProjectionView v = topView();
  expectScreen(v, Vec2{5.0f, 7.0f}, 0.0f, Vec3{0.0f, 0.0f, -1.0f});
  // The screen edges sit at halfWidth / halfHeight from the center:
  // x + 10 and y + 5 map to NDC 1.
  expectScreen(v, Vec2{15.0f, 7.0f}, 0.0f, Vec3{1.0f, 0.0f, -1.0f});
  expectScreen(v, Vec2{5.0f, 12.0f}, 0.0f, Vec3{0.0f, 1.0f, -1.0f});
  expectScreen(v, Vec2{5.0f, 7.0f}, 49.0f,
               Vec3{0.0f, 0.0f, static_cast<float>(-197.0 / 99.0)});
}

TEST(ProjectionTopDownGoldens, Ray) {
  const ProjectionView v = topView();
  // The screen center's preimage line runs along world z through the
  // center (on the reference plane z = 0); direction (0, 0, 1) = the
  // cross of the screen rows ((1/10, 0, 0) × (0, 1/5, 0)).
  expectRay(v.screenToWorldRay(Vec2{0.0f, 0.0f}), Vec3{5.0f, 7.0f, 0.0f},
            Vec3{0.0f, 0.0f, 1.0f});
  // The screen point of world (15, 7, 0) is (1, 0).
  expectRay(v.screenToWorldRay(Vec2{1.0f, 0.0f}), Vec3{15.0f, 7.0f, 0.0f},
            Vec3{0.0f, 0.0f, 1.0f});
}

TEST(ProjectionTopDownRoundTrip, TenThousandRandomPoints) {
  // The canonical plane is z = p.z (any z = const plane crosses the
  // preimage line exactly once).
  const ProjectionView v = topView();
  laige::Prng rng = laige::testing::TestPrng(kTopRoundTripStream);
  for (int i = 0; i < 10000; ++i) {
    const Vec3 p{uniformIn(rng, -32.0f, 32.0f), uniformIn(rng, -32.0f, 32.0f),
                 uniformIn(rng, 0.0f, 100.0f)};
    expectRoundTrip(v, p, Plane{Vec3{0.0f, 0.0f, 1.0f}, p.z},
                    "top random");
  }
}

// ---------------------------------------------------------------------------
// free_cinematic (the full 3D camera; the preimage is a true ray)
// ---------------------------------------------------------------------------

TEST(ProjectionCinematicOrtho, WorldToScreenAndRay) {
  // The ortho camera at (0,0,10) looking at the origin (z_cam = z - 10):
  // ndc_x = x/10, ndc_y = y/5, ndc_z = (-2·z_cam - 101)/99. For z = 0:
  // z_cam = -10 -> ndc_z = (20 - 101)/99 = -81/99.
  const ProjectionView v = cineOrthoView();
  expectScreen(v, Vec2{0.0f, 0.0f}, 0.0f,
               Vec3{0.0f, 0.0f, static_cast<float>(-81.0 / 99.0)});
  expectScreen(v, Vec2{10.0f, 0.0f}, 0.0f,
               Vec3{1.0f, 0.0f, static_cast<float>(-81.0 / 99.0)});
  expectScreen(v, Vec2{0.0f, 5.0f}, 0.0f,
               Vec3{0.0f, 1.0f, static_cast<float>(-81.0 / 99.0)});
  // The near plane (1 unit in front of the camera) is NDC z = -1.
  expectScreen(v, Vec2{0.0f, 0.0f}, 9.0f, Vec3{0.0f, 0.0f, -1.0f});

  // The screen center's preimage ray: origin on the NDC near plane
  // (0, 0, 9), direction away from the camera (down).
  expectRay(v.screenToWorldRay(Vec2{0.0f, 0.0f}), Vec3{0.0f, 0.0f, 9.0f},
            Vec3{0.0f, 0.0f, -1.0f});
}

TEST(ProjectionCinematicOrtho, TenThousandRandomPoints) {
  // The cinematic preimage ray starts on the NDC near plane (1 unit in
  // front of the camera, z = 9) and the t >= 0 half covers the VISIBLE
  // volume (z_cam <= -zNear): points between the camera and the near
  // plane are clipped — no pick can land there (documented semantics).
  // The round trip also holds OUTSIDE the frustum (the ray extends
  // past the NDC box — the screen point is then outside [-1, 1], which
  // the transforms do not require).
  const ProjectionView v = cineOrthoView();
  laige::Prng rng = laige::testing::TestPrng(kCineOrthoRoundTripStream);
  for (int i = 0; i < 10000; ++i) {
    const Vec3 p{uniformIn(rng, -40.0f, 40.0f), uniformIn(rng, -40.0f, 40.0f),
                 uniformIn(rng, -50.0f, 8.9f)};
    expectRoundTrip(v, p, Plane{Vec3{0.0f, 0.0f, 1.0f}, p.z},
                    "cine-ortho random");
  }
}

TEST(ProjectionCinematicPerspective, RayAndRoundTrip) {
  // The perspective camera at (0,0,20) looking at the origin, fovY
  // 60°, aspect 16:9: the screen center's preimage ray starts on the
  // near plane (0, 0, 19) and points along -z.
  const ProjectionView v = cinePerspView();
  expectRay(v.screenToWorldRay(Vec2{0.0f, 0.0f}), Vec3{0.0f, 0.0f, 19.0f},
            Vec3{0.0f, 0.0f, -1.0f});

  // The round trip for random points in the visible volume (z_cam <=
  // -zNear: z <= 18.9 — points between the camera and the near plane
  // are clipped, no pick can land there) plus a far outside-the-frustum
  // point (the ray still extends).
  laige::Prng rng = laige::testing::TestPrng(kCinePerspRoundTripStream);
  for (int i = 0; i < 10000; ++i) {
    const Vec3 p{uniformIn(rng, -30.0f, 30.0f), uniformIn(rng, -30.0f, 30.0f),
                 uniformIn(rng, -80.0f, 18.9f)};
    expectRoundTrip(v, p, Plane{Vec3{0.0f, 0.0f, 1.0f}, p.z},
                    "cine-persp random");
  }
  expectRoundTrip(v, Vec3{100.0f, 0.0f, 0.0f},
                  Plane{Vec3{0.0f, 0.0f, 1.0f}, 0.0f}, "cine-persp far");
}

// ---------------------------------------------------------------------------
// The documented failure behavior (screenToWorld Result errors)
// ---------------------------------------------------------------------------

TEST(ProjectionErrors, ParallelPlane) {
  // The affine preimage lines: a plane parallel to the line is
  // rejected (the intersection does not exist).
  const ProjectionView iso = isoView();
  // The iso line runs along (1, 1, 2); the plane x - y = 3 is parallel
  // to it (dot((1,1,2), (1,-1,0)) = 0).
  auto e1 = iso.screenToWorld(Vec2{0.25f, -0.125f},
                              Plane{Vec3{1.0f, -1.0f, 0.0f}, 3.0f});
  EXPECT_TRUE(e1.isError());
  EXPECT_EQ(*e1.errorIfError(), ErrorCode::InvalidArgument);

  const ProjectionView side = sideView();
  // The side line runs along y; the plane z = 5 is parallel to it.
  auto e2 = side.screenToWorld(Vec2{0.0f, 0.0f},
                               Plane{Vec3{0.0f, 0.0f, 1.0f}, 5.0f});
  EXPECT_TRUE(e2.isError());
  EXPECT_EQ(*e2.errorIfError(), ErrorCode::InvalidArgument);

  const ProjectionView top = topView();
  // The top line runs along z; the plane y = 5 is parallel to it.
  auto e3 = top.screenToWorld(Vec2{0.0f, 0.0f},
                              Plane{Vec3{0.0f, 1.0f, 0.0f}, 5.0f});
  EXPECT_TRUE(e3.isError());
  EXPECT_EQ(*e3.errorIfError(), ErrorCode::InvalidArgument);
}

TEST(ProjectionErrors, DegeneratePlane) {
  // A zero normal is parallel to everything (denom = 0): rejected.
  const ProjectionView iso = isoView();
  auto e = iso.screenToWorld(Vec2{0.25f, -0.125f},
                             Plane{Vec3{0.0f, 0.0f, 0.0f}, 0.0f});
  EXPECT_TRUE(e.isError());
  EXPECT_EQ(*e.errorIfError(), ErrorCode::InvalidArgument);
}

TEST(ProjectionErrors, BehindCamera) {
  // FreeCinematic: the intersection must lie in FRONT of the camera
  // (t >= 0). The plane z = 20 sits above/behind the ortho camera at
  // z = 10 (the ray goes from z = 9 downward): t = -11 < 0 -> error.
  const ProjectionView v = cineOrthoView();
  auto e = v.screenToWorld(Vec2{0.0f, 0.0f},
                           Plane{Vec3{0.0f, 0.0f, 1.0f}, 20.0f});
  EXPECT_TRUE(e.isError());
  EXPECT_EQ(*e.errorIfError(), ErrorCode::InvalidArgument);

  // The boundary t = 0 is allowed: the plane through the ray origin
  // (the NDC near plane, z = 9) hits exactly at the origin.
  auto ok = v.screenToWorld(Vec2{0.0f, 0.0f},
                            Plane{Vec3{0.0f, 0.0f, 1.0f}, 9.0f});
  ASSERT_TRUE(ok.ok());
  const Vec3 hit = *ok.valueIfOk();
  EXPECT_NEAR(hit.x, 0.0f, kEps) << "hit x";
  EXPECT_NEAR(hit.y, 0.0f, kEps) << "hit y";
  EXPECT_NEAR(hit.z, 9.0f, kEps) << "hit z";
}
