// laige-render matrix utilities tests (M2-GL-03): the camera/projection
// builders in laige/render/matrices.h.
//
// Pure math — no GL context, no GL environment needed: every suite runs
// in every local tree and in CI. The goldens are HAND-COMPUTED from the
// documented formulas in the header (the roadmap's "hand-computed
// golden values": the iso matrix maps known grid points to the expected
// screen positions). Float tolerance is 1e-6 (the hand values are
// double; float has ~7 significant digits and the test values stay
// within ~10 in magnitude).

#include "laige/render/matrices.h"

#include <cmath>
#include <string>

#include <gtest/gtest.h>

namespace {

using laige::render::IsoAxes;
using laige::render::Mat4;
using laige::render::Vec2;
using laige::render::Vec3;

// Golden tolerance: the expected values are exact doubles; the builders
// produce float (GLM's default precision). 1e-6 is ~0.1 ulp at |v| = 1
// and safe at the magnitudes used here (<= ~10).
constexpr float kEps = 1e-6f;
// The 1/sqrt(3) constant of the true 30°/60° preset (double).
constexpr double kInvSqrt3D = 0.57735026918962576451;

// Applies a column-major 4x4 matrix to the homogeneous point p (w = 1)
// and perspective-divides (w = 1 for every affine matrix this header
// produces; kept general for future projective builders).
Vec3 applyMatrix(const Mat4& m, Vec3 p) {
  const float x = m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0];
  const float y = m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1];
  const float z = m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2];
  const float w = m[0][3] * p.x + m[1][3] * p.y + m[2][3] * p.z + m[3][3];
  return Vec3{x / w, y / w, z / w};
}

// CHECKs that m maps p to the expected NDC point (with a context label).
void expectMapped(const Mat4& m, Vec3 p, Vec3 expected, const std::string& what) {
  const Vec3 q = applyMatrix(m, p);
  EXPECT_NEAR(q.x, expected.x, kEps) << what << ": point " << p.x << ", " << p.y
                                     << ", " << p.z;
  EXPECT_NEAR(q.y, expected.y, kEps) << what << ": point " << p.x << ", " << p.y
                                     << ", " << p.z;
  EXPECT_NEAR(q.z, expected.z, kEps) << what << ": point " << p.x << ", " << p.y
                                     << ", " << p.z;
}

// CHECKs all 16 elements of m against a hand-computed table given in
// row-major (display) order e[r][c] — the GLM storage is column-major
// (m[c][r] is row r of column c).
void expectMatrix(const Mat4& m, const float e[4][4], const std::string& what) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_NEAR(m[c][r], e[r][c], kEps)
          << what << ": m[" << c << "][" << r << "]";
    }
  }
}

// CHECKs two matrices element by element.
void expectMatricesEqual(const Mat4& a, const Mat4& b, const std::string& what) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_NEAR(a[c][r], b[c][r], kEps)
          << what << ": m[" << c << "][" << r << "]";
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// ortho()
// ---------------------------------------------------------------------------

TEST(MatricesOrtho, UnitBoxIsZFlip) {
  // The unit NDC box with the unit depth slab (camera z in [-1, 1]):
  // the camera looks along -z, so NDC-z = -z_cam — a pure z flip.
  const Mat4 m = laige::render::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
  const float e[4][4] = {{1, 0, 0, 0},
                        {0, 1, 0, 0},
                        {0, 0, -1, 0},
                        {0, 0, 0, 1}};
  expectMatrix(m, e, "ortho unit box");
  expectMapped(m, {0.5f, -0.25f, 1.0f}, {0.5f, -0.25f, -1.0f}, "near edge");
  expectMapped(m, {-0.5f, 0.25f, -1.0f}, {-0.5f, 0.25f, 1.0f}, "far edge");
}

TEST(MatricesOrtho, CornerMapping) {
  // Box [-2, 2] x [-1, 3] with camera-space z in [-10, 0] (near plane at
  // z_cam = -zNear = 0, far plane at z_cam = -zFar = -10) -> NDC cube.
  const Mat4 m = laige::render::ortho(-2.0f, 2.0f, -1.0f, 3.0f, 0.0f, 10.0f);
  expectMapped(m, {-2.0f, -1.0f, 0.0f}, {-1.0f, -1.0f, -1.0f}, "near bottom-left");
  expectMapped(m, {2.0f, 3.0f, -10.0f}, {1.0f, 1.0f, 1.0f}, "far top-right");
  expectMapped(m, {2.0f, -1.0f, -10.0f}, {1.0f, -1.0f, 1.0f}, "far bottom-right");
  expectMapped(m, {-2.0f, 3.0f, 0.0f}, {-1.0f, 1.0f, -1.0f}, "near top-left");
  expectMapped(m, {0.0f, 1.0f, -5.0f}, {0.0f, 0.0f, 0.0f}, "slab center");
}

TEST(MatricesOrtho, ElementGoldens) {
  // Hand-computed from the header formula for (-2, 2, -1, 3, 0, 10)
  // (translation in column 3):
  //   m[0][0] = 2/4 = 0.5, m[1][1] = 2/4 = 0.5, m[2][2] = -2/10 = -0.2
  //   m[3][0] = 0/4 = 0,    m[3][1] = -(3-1)/4 = -0.5
  //   m[3][2] = -(10+0)/10 = -1
  const Mat4 m = laige::render::ortho(-2.0f, 2.0f, -1.0f, 3.0f, 0.0f, 10.0f);
  const float e[4][4] = {{0.5f, 0.0f, 0.0f, 0.0f},
                        {0.0f, 0.5f, 0.0f, -0.5f},
                        {0.0f, 0.0f, -0.2f, -1.0f},
                        {0.0f, 0.0f, 0.0f, 1.0f}};
  expectMatrix(m, e, "ortho elements");
}

// ---------------------------------------------------------------------------
// perspective()
// ---------------------------------------------------------------------------

TEST(MatricesPerspective, ElementGoldens) {
  // fovY = 2*atan(1) = pi/2 -> t = 1/tan(pi/4) = 1; aspect 1; near 1; far 10:
  //   m[0][0] = 1, m[1][1] = 1, m[2][2] = -11/9, m[2][3] = -20/9,
  //   m[3][2] = 1 (w = -z_cam), m[3][3] = 0.
  const float fovY = 2.0f * std::atan(1.0f);
  const Mat4 m = laige::render::perspective(fovY, 1.0f, 1.0f, 10.0f);
  const float e[4][4] = {{1.0f, 0.0f, 0.0f, 0.0f},
                        {0.0f, 1.0f, 0.0f, 0.0f},
                        {0.0f, 0.0f, -11.0f / 9.0f, -20.0f / 9.0f},
                        {0.0f, 0.0f, -1.0f, 0.0f}};
  expectMatrix(m, e, "perspective elements");
}

TEST(MatricesPerspective, NearFarCorners) {
  const float fovY = 2.0f * std::atan(1.0f);
  const Mat4 m = laige::render::perspective(fovY, 1.0f, 1.0f, 10.0f);
  // Frustum sides touch the NDC cube edges at every depth: the
  // near-plane corner (t*n, t*n, -n) = (1, 1, -1) maps to (1, 1, -1)
  // and the far-plane corner (t*f, t*f, -f) = (10, 10, -10) to (1, 1, 1).
  expectMapped(m, {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, "near corner");
  expectMapped(m, {10.0f, 10.0f, -10.0f}, {1.0f, 1.0f, 1.0f}, "far corner");
  expectMapped(m, {-10.0f, -10.0f, -10.0f}, {-1.0f, -1.0f, 1.0f}, "far corner neg");
  expectMapped(m, {0.0f, 0.0f, -10.0f}, {0.0f, 0.0f, 1.0f}, "far plane center");
  expectMapped(m, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, "near plane center");
}

TEST(MatricesPerspective, Aspect) {
  // 16:9 at fovY = pi/2: m[0][0] = t/aspect = 9/16, m[1][1] = t = 1.
  const float fovY = 2.0f * std::atan(1.0f);
  const Mat4 m = laige::render::perspective(fovY, 16.0f / 9.0f, 1.0f, 10.0f);
  const float e[4][4] = {{9.0f / 16.0f, 0.0f, 0.0f, 0.0f},
                        {0.0f, 1.0f, 0.0f, 0.0f},
                        {0.0f, 0.0f, -11.0f / 9.0f, -20.0f / 9.0f},
                        {0.0f, 0.0f, -1.0f, 0.0f}};
  expectMatrix(m, e, "perspective 16:9 elements");
}

// ---------------------------------------------------------------------------
// lookAt()
// ---------------------------------------------------------------------------

TEST(MatricesLookAt, PureTranslation) {
  // eye (0,0,5) looking at the origin, up +y: f = (0,0,-1), s = (1,0,0),
  // u = (0,1,0); rows (s, u, -f) with translation (0, -5, -5): a pure
  // -5 z translation.
  const Mat4 m = laige::render::lookAt({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f},
                                       {0.0f, 1.0f, 0.0f});
  const float e[4][4] = {{1, 0, 0, 0},
                        {0, 1, 0, 0},
                        {0, 0, 1, -5},
                        {0, 0, 0, 1}};
  expectMatrix(m, e, "lookAt axis-aligned elements");
  expectMapped(m, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -5.0f}, "target is 5 in front");
  expectMapped(m, {1.0f, 2.0f, 3.0f}, {1.0f, 2.0f, -2.0f}, "arbitrary point");
  expectMapped(m, {0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, "eye maps to origin");
}

TEST(MatricesLookAt, Rotated90) {
  // eye (5,0,0) looking at the origin, up +y — hand-computed:
  //   f = (-1,0,0), s = normalize(cross(f,up)) = (0,0,-1), u = (0,1,0).
  //   rows (s, u, -f) with translation (0, 0, -5).
  const Mat4 m = laige::render::lookAt({5.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
                                       {0.0f, 1.0f, 0.0f});
  const float e[4][4] = {{0, 0, -1, 0},
                        {0, 1, 0, 0},
                        {1, 0, 0, -5},
                        {0, 0, 0, 1}};
  expectMatrix(m, e, "lookAt rotated elements");
  expectMapped(m, {5.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, "eye maps to origin");
  expectMapped(m, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -5.0f}, "target 5 in front");
  expectMapped(m, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, -5.0f}, "up stays up");
}

TEST(MatricesLookAt, Oblique) {
  // eye (0,0,5) looking at (0,10,-5) — a 45° downward view, up +y —
  // hand-computed:
  //   f = (0, 1/√2, -1/√2), s = normalize(cross(f,up)) = (1,0,0),
  //   u = cross(s,f) = (0, 1/√2, 1/√2).
  //   rows (s, u, -f) with translation (0, -5/√2, -5/√2).
  const float r = 1.0f / std::sqrt(2.0f);  // = 1/√2
  const float c = r * 5.0f;                // = 5/√2
  const Mat4 m = laige::render::lookAt({0.0f, 0.0f, 5.0f}, {0.0f, 10.0f, -5.0f},
                                       {0.0f, 1.0f, 0.0f});
  const float e[4][4] = {{1.0f, 0.0f, 0.0f, 0.0f},
                        {0.0f, r, r, -c},
                        {0.0f, -r, r, -c},
                        {0.0f, 0.0f, 0.0f, 1.0f}};
  expectMatrix(m, e, "lookAt oblique elements");
  // The target lands straight ahead on the camera's -z axis,
  // |eye-center| = 10/√2 in front.
  expectMapped(m, {0.0f, 10.0f, -5.0f}, {0.0f, 0.0f, -20.0f * r}, "target ahead");
  expectMapped(m, {0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, "eye maps to origin");
}

// ---------------------------------------------------------------------------
// planeOrtho()
// ---------------------------------------------------------------------------

TEST(MatricesPlaneOrtho, TopDown) {
  // top_down mode (M2-PROJ-01): plane z = 0 through (10,20,0), right +x,
  // up +y, n = (0,0,1); halfWidth 10, halfHeight 5, slab [1, 11].
  const Mat4 m = laige::render::planeOrtho({10.0f, 20.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                                           {0.0f, 1.0f, 0.0f}, 10.0f, 5.0f, 1.0f,
                                           11.0f);
  // Hand-computed VP = P * V:
  //   P rows: (0.1,0,0,0), (0,0.2,0,0), (0,0,-0.2,-1.2), (0,0,0,1)
  //   V rows: (1,0,0,-10), (0,1,0,-20), (0,0,1,-1), (0,0,0,1)
  const float e[4][4] = {{0.1f, 0.0f, 0.0f, -1.0f},
                        {0.0f, 0.2f, 0.0f, -4.0f},
                        {0.0f, 0.0f, -0.2f, -1.0f},
                        {0.0f, 0.0f, 0.0f, 1.0f}};
  expectMatrix(m, e, "planeOrtho top-down VP");
  // The plane center is on the near plane; the half-extents hit the
  // NDC edges; depth runs from the plane (z = -1) to the viewer.
  expectMapped(m, {10.0f, 20.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, "plane center");
  expectMapped(m, {20.0f, 20.0f, 0.0f}, {1.0f, 0.0f, -1.0f}, "right edge");
  expectMapped(m, {10.0f, 15.0f, 0.0f}, {0.0f, -1.0f, -1.0f}, "bottom edge");
  expectMapped(m, {10.0f, 20.0f, -5.0f}, {0.0f, 0.0f, 0.0f}, "mid-depth slab");
  // 9 units behind the camera -> beyond the near plane (clipped).
  expectMapped(m, {10.0f, 20.0f, 10.0f}, {0.0f, 0.0f, -3.0f}, "behind camera");
}

TEST(MatricesPlaneOrtho, SideView) {
  // side_view mode (M2-PROJ-01): plane y = 0 through the origin, right +x,
  // up = world +z (screen y = height), n = (0,-1,0); slab [1, 11].
  const Mat4 m = laige::render::planeOrtho({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                                           {0.0f, 0.0f, 1.0f}, 10.0f, 5.0f, 1.0f,
                                           11.0f);
  // Hand-computed VP = P * V:
  //   V rows: (1,0,0,0), (0,0,1,0), (0,-1,0,-1), (0,0,0,1)
  const float e[4][4] = {{0.1f, 0.0f, 0.0f, 0.0f},
                        {0.0f, 0.0f, 0.2f, 0.0f},
                        {0.0f, 0.2f, 0.0f, -1.0f},
                        {0.0f, 0.0f, 0.0f, 1.0f}};
  expectMatrix(m, e, "planeOrtho side-view VP");
  expectMapped(m, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, "plane center");
  expectMapped(m, {5.0f, 3.0f, 2.0f}, {0.5f, 0.4f, -0.4f}, "in-slab point");
  expectMapped(m, {0.0f, 10.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, "far plane");
  // 1 unit past the far plane -> outside the NDC cube (clipped).
  expectMapped(m, {0.0f, 11.0f, 0.0f}, {0.0f, 0.0f, 1.2f}, "past far plane");
  // The camera sits at (0,-1,0): its own position is behind the near
  // plane (z_ndc = -1.2), and 10 units behind the camera at (0,-11,0).
  expectMapped(m, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.2f}, "camera position");
  expectMapped(m, {0.0f, -11.0f, 0.0f}, {0.0f, 0.0f, -3.2f}, "far behind");
}

TEST(MatricesPlaneOrtho, PlaneLandsOnNearPlane) {
  // Every point of the plane through `center` (spanned by right/up)
  // lands exactly on the NDC near plane (z = -1), for several offsets.
  const Vec3 center{3.0f, -4.0f, 2.0f};
  const Vec3 right{0.0f, 1.0f, 0.0f};
  const Vec3 up{0.0f, 0.0f, 1.0f};
  const Mat4 m = laige::render::planeOrtho(center, right, up, 8.0f, 4.0f, 0.0f,
                                           20.0f);
  const float offs[8][2] = {{0, 0}, {3, -2}, {-5, 1}, {1, 7},
                           {-6, -3}, {0.5, -0.25}, {4, 0}, {0, -4}};
  for (const auto& o : offs) {
    const Vec3 p{center.x + right.x * o[0] + up.x * o[1],
                 center.y + right.y * o[0] + up.y * o[1],
                 center.z + right.z * o[0] + up.z * o[1]};
    expectMapped(m, p, {o[0] / 8.0f, o[1] / 4.0f, -1.0f}, "plane point");
  }
}

// ---------------------------------------------------------------------------
// isoMatrix() / isoDimetric2To1() / isoTrueIso3060()
// ---------------------------------------------------------------------------

TEST(MatricesIso, Dimetric2To1Elements) {
  // k = 1: row 0 = (2, -2, 0, 0); row 1 = (-1, -1, 1, 0); row 2 = 0;
  // row 3 = (0, 0, 0, 1) — the ADR 0005 (±2, 1)·k deltas, y up.
  const Mat4 m = laige::render::isoDimetric2To1(1.0f);
  const float e[4][4] = {{2.0f, -2.0f, 0.0f, 0.0f},
                        {-1.0f, -1.0f, 1.0f, 0.0f},
                        {0.0f, 0.0f, 0.0f, 0.0f},
                        {0.0f, 0.0f, 0.0f, 1.0f}};
  expectMatrix(m, e, "2:1 dimetric k=1 elements");
}

TEST(MatricesIso, Dimetric2To1GridPoints) {
  // The roadmap golden: known grid points -> expected screen (NDC)
  // positions, hand-computed from sx = (x-y)·2k, sy = -(x+y)·k + z·k.
  const Mat4 m = laige::render::isoDimetric2To1(1.0f);
  expectMapped(m, {1.0f, 0.0f, 0.0f}, {2.0f, -1.0f, 0.0f}, "+x step");
  expectMapped(m, {0.0f, 1.0f, 0.0f}, {-2.0f, -1.0f, 0.0f}, "+y step");
  expectMapped(m, {1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, "tile top at z=1");
  expectMapped(m, {2.0f, 3.0f, 0.0f}, {-2.0f, -5.0f, 0.0f}, "arbitrary tile");
  expectMapped(m, {0.0f, 0.0f, 5.0f}, {0.0f, 5.0f, 0.0f}, "pure height");
  // At k = 1/2 the deltas are (±1, 1/2) (the ADR's foreshortened form).
  const Mat4 half = laige::render::isoDimetric2To1(0.5f);
  expectMapped(half, {1.0f, 0.0f, 0.0f}, {1.0f, -0.5f, 0.0f}, "k=1/2 +x step");
  expectMapped(half, {1.0f, 1.0f, 1.0f}, {0.0f, -0.5f, 0.0f}, "k=1/2 tile top");
}

TEST(MatricesIso, Dimetric2To1VerticalIsHalfTile) {
  // ADR 0005: the vertical is squashed to 1/2 — one world height unit
  // projects to k screen units while the tile's screen height is 2k.
  const float k = 0.75f;
  const Mat4 m = laige::render::isoDimetric2To1(k);
  const Vec3 top = applyMatrix(m, {1.0f, 1.0f, 0.0f});   // tile far corner
  const Vec3 base = applyMatrix(m, {0.0f, 0.0f, 0.0f});   // tile near corner
  const float tileHeight = base.y - top.y;               // 2k (y up)
  const Vec3 unit = applyMatrix(m, {0.0f, 0.0f, 1.0f});   // one height unit
  EXPECT_NEAR(tileHeight, 2.0f * k, kEps) << "tile screen height";
  EXPECT_NEAR(unit.y, k, kEps) << "height unit";
  EXPECT_NEAR(unit.y / tileHeight, 0.5f, kEps) << "vertical squash";
}

TEST(MatricesIso, TrueIso3060Elements) {
  // s = 1: row 0 = (1, -1, 0, 0); row 1 = (-1/√3, -1/√3, 1/√3, 0);
  // row 2 = 0; row 3 = (0, 0, 0, 1).
  const float r = static_cast<float>(kInvSqrt3D);
  const Mat4 m = laige::render::isoTrueIso3060(1.0f);
  const float e[4][4] = {{1.0f, -1.0f, 0.0f, 0.0f},
                        {-r, -r, r, 0.0f},
                        {0.0f, 0.0f, 0.0f, 0.0f},
                        {0.0f, 0.0f, 0.0f, 1.0f}};
  expectMatrix(m, e, "true 30°/60° s=1 elements");
}

TEST(MatricesIso, TrueIso3060GridPoints) {
  // Hand-computed from sx = (x-y)·s, sy = -(x+y)·s/√3 + z·s/√3.
  const Mat4 m = laige::render::isoTrueIso3060(1.0f);
  const float r = static_cast<float>(kInvSqrt3D);
  expectMapped(m, {1.0f, 0.0f, 0.0f}, {1.0f, -r, 0.0f}, "+x step");
  expectMapped(m, {0.0f, 1.0f, 0.0f}, {-1.0f, -r, 0.0f}, "+y step");
  expectMapped(m, {1.0f, 1.0f, 0.0f}, {0.0f, -2.0f * r, 0.0f}, "tile far corner");
  expectMapped(m, {0.0f, 0.0f, 1.0f}, {0.0f, r, 0.0f}, "pure height");
}

TEST(MatricesIso, PresetsAreCustomShearCases) {
  // The presets must equal isoMatrix() with the ADR 0005 axis deltas —
  // M2-CAM-02 selects the preset and builds through one mechanism.
  const float r = static_cast<float>(kInvSqrt3D);
  const float scales[3] = {0.5f, 1.0f, 2.0f};
  for (float k : scales) {
    const Mat4 d = laige::render::isoDimetric2To1(k);
    const Mat4 dgen = laige::render::isoMatrix(IsoAxes{
        Vec2{2.0f * k, -k}, Vec2{-2.0f * k, -k}, k});
    expectMatricesEqual(d, dgen, "dimetric == custom shear");
    const Mat4 t = laige::render::isoTrueIso3060(k);
    const Mat4 tgen = laige::render::isoMatrix(IsoAxes{
        Vec2{k, -k * r}, Vec2{-k, -k * r}, k * r});
    expectMatricesEqual(t, tgen, "true iso == custom shear");
  }
}

TEST(MatricesIso, CustomShearMapping) {
  const Mat4 m = laige::render::isoMatrix(IsoAxes{
      Vec2{1.0f, -0.5f}, Vec2{-1.0f, -0.5f}, 0.5f});
  expectMapped(m, {1.0f, 0.0f, 0.0f}, {1.0f, -0.5f, 0.0f}, "+x step");
  expectMapped(m, {0.0f, 1.0f, 0.0f}, {-1.0f, -0.5f, 0.0f}, "+y step");
  expectMapped(m, {0.0f, 0.0f, 2.0f}, {0.0f, 1.0f, 0.0f}, "height");
}

TEST(MatricesIso, NdcZIsAlwaysZero) {
  // The 2.5D depth is engine-owned (FR-2.2 depth keys): the iso
  // projection's NDC z row is zero for every point.
  const Mat4 d = laige::render::isoDimetric2To1(1.0f);
  const Mat4 t = laige::render::isoTrueIso3060(1.0f);
  const Vec3 pts[6] = {{0, 0, 0}, {1, 0, 1}, {-2, 3, -4}, {0.5, 0.25, 7},
                      {9, -9, 0.125}, {0.1, 0.2, 0.3}};
  for (const Vec3& p : pts) {
    EXPECT_EQ(applyMatrix(d, p).z, 0.0f) << "dimetric z for " << p.x << ", " << p.y;
    EXPECT_EQ(applyMatrix(t, p).z, 0.0f) << "true-iso z for " << p.x << ", " << p.y;
  }
}

TEST(MatricesIso, GroundRoundTripInverse) {
  // Property: the ground-plane affine map (z = 0) is invertible — the
  // M2-ISO-03 picking inverse rests on this. For
  //   [sx]   [dx.x  dy.x] [x]
  //   [sy] = [dx.y  dy.y] [y]
  // the analytic inverse is x = (d*sx - b*sy)/det,
  //                     y = (-c*sx + a*sy)/det, det = a*d - b*c.
  const Vec3 grid[12] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                        {2, 3, 0}, {-1, 2, 0}, {0.5, -0.25, 0}, {3.75, -1.5, 0},
                        {-4.25, 2.5, 0}, {0.1, 0.9, 0}, {7, -7, 0},
                        {0.333, 0.667, 0}};
  struct Preset {
    const char* name;
    Mat4 m;
  };
  const Preset presets[2] = {
      {"dimetric", laige::render::isoDimetric2To1(1.0f)},
      {"true-iso", laige::render::isoTrueIso3060(1.0f)},
  };
  for (const Preset& preset : presets) {
    const float a = preset.m[0][0];
    const float b = preset.m[1][0];
    const float c = preset.m[0][1];
    const float d = preset.m[1][1];
    const float det = a * d - b * c;
    ASSERT_GT(std::abs(det), 1e-6f) << preset.name << ": ground map degenerate";
    for (const Vec3& p : grid) {
      const Vec3 q = applyMatrix(preset.m, p);
      const float x = (d * q.x - b * q.y) / det;
      const float y = (-c * q.x + a * q.y) / det;
      EXPECT_NEAR(x, p.x, 1e-4f) << preset.name << ": round-trip x for " << p.x
                                 << ", " << p.y;
      EXPECT_NEAR(y, p.y, 1e-4f) << preset.name << ": round-trip y for " << p.x
                                 << ", " << p.y;
    }
  }
}

TEST(MatricesIso, ADR0005ScreenDeltas) {
  // ADR 0005's table, machine-checked: 2:1 ground deltas (±2, 1)·k at
  // 26.565° (arctan 1/2) to screen horizontal; true-iso deltas at 30°.
  const Mat4 d = laige::render::isoDimetric2To1(1.0f);
  const Vec3 dx = applyMatrix(d, {1.0f, 0.0f, 0.0f});
  EXPECT_NEAR(std::atan2(dx.y, dx.x), -std::atan(0.5f), 1e-5f)
      << "2:1 ground-axis angle";
  const Mat4 t = laige::render::isoTrueIso3060(1.0f);
  const Vec3 tx = applyMatrix(t, {1.0f, 0.0f, 0.0f});
  EXPECT_NEAR(std::atan2(tx.y, tx.x), -std::atan(1.0f / std::sqrt(3.0f)), 1e-5f)
      << "true-iso ground-axis angle";
}
