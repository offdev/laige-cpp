// laige-render matrix utilities (M2-GL-03): the pure camera/projection
// matrix builders documented in include/laige/render/matrices.h.
//
// All builders fill a zero-initialized column-major Mat4 (m[c][r] = row
// r of column c) element by element; the formulas in the header are the
// contract, and the unit tests (tests/laige-render, CTest entry
// `matrices`) pin them against hand-computed golden values.

#include "laige/render/matrices.h"

#include <cassert>
#include <cmath>

#include <glm/geometric.hpp>  // dot, cross, length, normalize
#include <glm/matrix.hpp>     // Mat4 operator*

namespace laige::render {

namespace {

// Tolerance for the orthonormality precondition checks in planeOrtho():
// the bases in practice are exact axis constants (1,0,0)/(0,1,0); the
// tolerance only keeps float assembly of a rotated base from failing a
// debug assert by an ulp.
constexpr float kOrthoTolerance = 1e-5f;

}  // namespace

Mat4 ortho(float left, float right, float bottom, float top, float zNear,
           float zFar) noexcept {
  assert(std::isfinite(left) && std::isfinite(right) && std::isfinite(bottom) &&
         std::isfinite(top) && std::isfinite(zNear) && std::isfinite(zFar) &&
         "ortho: all arguments must be finite");
  assert(left < right && "ortho: requires left < right");
  assert(bottom < top && "ortho: requires bottom < top");
  assert(zNear < zFar && "ortho: requires zNear < zFar");

  Mat4 m{};
  m[0][0] = 2.0f / (right - left);
  m[1][1] = 2.0f / (top - bottom);
  m[2][2] = -2.0f / (zFar - zNear);
  // The translation lives in column 3 (m[3][r]).
  m[3][0] = -(right + left) / (right - left);
  m[3][1] = -(top + bottom) / (top - bottom);
  m[3][2] = -(zFar + zNear) / (zFar - zNear);
  m[3][3] = 1.0f;
  return m;
}

Mat4 perspective(float fovY, float aspect, float zNear, float zFar) noexcept {
  assert(std::isfinite(fovY) && std::isfinite(aspect) && std::isfinite(zNear) &&
         std::isfinite(zFar) && "perspective: all arguments must be finite");
  assert(fovY > 0.0f && fovY < 3.14159265358979323846f &&
         "perspective: requires 0 < fovY < pi");
  assert(aspect > 0.0f && "perspective: requires aspect > 0");
  assert(zNear > 0.0f && zNear < zFar && "perspective: requires 0 < zNear < zFar");

  const float t = 1.0f / std::tan(fovY * 0.5f);
  Mat4 m{};
  m[0][0] = t / aspect;
  m[1][1] = t;
  m[2][2] = -(zFar + zNear) / (zFar - zNear);
  m[3][2] = -2.0f * zFar * zNear / (zFar - zNear);
  // w = -z_cam: the homogeneous divisor of the perspective divide
  // (negative camera z, i.e. in front of the camera, is a positive w).
  m[2][3] = -1.0f;
  m[3][3] = 0.0f;
  return m;
}

Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) noexcept {
  assert(std::isfinite(eye.x) && std::isfinite(eye.y) && std::isfinite(eye.z) &&
         std::isfinite(center.x) && std::isfinite(center.y) &&
         std::isfinite(center.z) && std::isfinite(up.x) && std::isfinite(up.y) &&
         std::isfinite(up.z) && "lookAt: all arguments must be finite");
  const Vec3 toCenter = center - eye;
  assert(glm::dot(toCenter, toCenter) > 0.0f &&
         "lookAt: requires eye != center");

  const Vec3 f = glm::normalize(toCenter);
  const Vec3 fCrossUp = glm::cross(f, up);
  assert(glm::dot(fCrossUp, fCrossUp) > 0.0f &&
         "lookAt: requires up not parallel to (center - eye)");
  const Vec3 s = glm::normalize(fCrossUp);
  const Vec3 u = glm::cross(s, f);

  // The camera axes are the matrix ROWS (column-major m[c][r] stores
  // row r of column c): M * (p, 1) = (s*(p-eye), u*(p-eye), -f*(p-eye)).
  Mat4 m{};
  m[0][0] = s.x;
  m[1][0] = s.y;
  m[2][0] = s.z;
  m[0][1] = u.x;
  m[1][1] = u.y;
  m[2][1] = u.z;
  m[0][2] = -f.x;
  m[1][2] = -f.y;
  m[2][2] = -f.z;
  // The translation lives in column 3 (m[3][r]).
  m[3][0] = -glm::dot(s, eye);
  m[3][1] = -glm::dot(u, eye);
  m[3][2] = glm::dot(f, eye);
  m[3][3] = 1.0f;
  return m;
}

Mat4 planeOrtho(Vec3 center, Vec3 right, Vec3 up, float halfWidth,
                float halfHeight, float zNear, float zFar) noexcept {
  assert(std::isfinite(center.x) && std::isfinite(center.y) &&
         std::isfinite(center.z) && std::isfinite(right.x) &&
         std::isfinite(right.y) && std::isfinite(right.z) &&
         std::isfinite(up.x) && std::isfinite(up.y) && std::isfinite(up.z) &&
         std::isfinite(halfWidth) && std::isfinite(halfHeight) &&
         std::isfinite(zNear) && std::isfinite(zFar) &&
         "planeOrtho: all arguments must be finite");
  assert(std::abs(glm::length(right) - 1.0f) <= kOrthoTolerance &&
         "planeOrtho: requires |right| == 1");
  assert(std::abs(glm::length(up) - 1.0f) <= kOrthoTolerance &&
         "planeOrtho: requires |up| == 1");
  assert(std::abs(glm::dot(right, up)) <= kOrthoTolerance &&
         "planeOrtho: requires right perpendicular to up");
  assert(halfWidth > 0.0f && halfHeight > 0.0f &&
         "planeOrtho: requires halfWidth > 0 and halfHeight > 0");
  assert(zNear < zFar && "planeOrtho: requires zNear < zFar");

  const Vec3 n = glm::normalize(glm::cross(right, up));  // toward the viewer

  // View: eye = center + n*zNear, looking along -n. The camera axes are
  // the matrix ROWS (m[c][r] stores row r of column c): M*(p,1) =
  // (right*(p-center), up*(p-center), n*(p-center) - zNear).
  Mat4 view{};
  view[0][0] = right.x;
  view[1][0] = right.y;
  view[2][0] = right.z;
  view[0][1] = up.x;
  view[1][1] = up.y;
  view[2][1] = up.z;
  view[0][2] = n.x;
  view[1][2] = n.y;
  view[2][2] = n.z;
  // The translation lives in column 3 (view[3][r]).
  view[3][0] = -glm::dot(right, center);
  view[3][1] = -glm::dot(up, center);
  view[3][2] = -glm::dot(n, center) - zNear;
  view[3][3] = 1.0f;

  const Mat4 proj = ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, zNear,
                          zFar);
  return proj * view;
}

Mat4 isoMatrix(IsoAxes axes) noexcept {
  assert(std::isfinite(axes.dx.x) && std::isfinite(axes.dx.y) &&
         std::isfinite(axes.dy.x) && std::isfinite(axes.dy.y) &&
         std::isfinite(axes.zUnit) && "isoMatrix: all arguments must be finite");
  // [[maybe_unused]]: in Release (-DNDEBUG) the assert below is compiled
  // out and det has no other use (the invertibility contract is a debug
  // assert per docs/api/matrices.md — pre-existing Release -Werror break,
  // surfaced by the M2-ISO-01 six-tree validation, 2026-09-30).
  [[maybe_unused]] const float det =
      axes.dx.x * axes.dy.y - axes.dx.y * axes.dy.x;
  assert(det != 0.0f &&
         "isoMatrix: requires the ground-plane map to be invertible "
         "(det(dx, dy) != 0 — the M2-ISO-03 picking inverse)");

  Mat4 m{};
  m[0][0] = axes.dx.x;
  m[1][0] = axes.dy.x;
  m[2][0] = 0.0f;
  m[3][0] = 0.0f;
  m[0][1] = axes.dx.y;
  m[1][1] = axes.dy.y;
  m[2][1] = axes.zUnit;
  m[3][1] = 0.0f;
  // row 2 is zero: the 2.5D depth is engine-owned (FR-2.2 depth keys),
  // never derived from the projection (PRD §4).
  m[3][3] = 1.0f;
  return m;
}

Mat4 isoDimetric2To1(float scale) noexcept {
  assert(std::isfinite(scale) && scale > 0.0f &&
         "isoDimetric2To1: requires scale > 0, finite");
  return isoMatrix(IsoAxes{
      Vec2{2.0f * scale, -scale},   // +x step: down-right (±2, 1)·scale
      Vec2{-2.0f * scale, -scale},  // +y step: down-left
      scale,                        // vertical squashed to 1/2 the tile height
  });
}

Mat4 isoTrueIso3060(float scale) noexcept {
  assert(std::isfinite(scale) && scale > 0.0f &&
         "isoTrueIso3060: requires scale > 0, finite");
  // Orthographic axonometric at 45° azimuth and elevation
  // arcsin(1/sqrt(3)): all three axes equally foreshortened. The
  // ground-axis screen delta per unit step is (±1, ∓1/√3)·scale — the
  // 1/√3 is the inverse of the ground-axis foreshortening (the
  // horizontal axis sits 30° to screen horizontal, hence "30°/60°").
  const float invSqrt3 = 1.0f / std::sqrt(3.0f);
  return isoMatrix(IsoAxes{
      Vec2{scale, -scale * invSqrt3},  // +x step
      Vec2{-scale, -scale * invSqrt3},  // +y step
      scale * invSqrt3,                 // vertical, equally foreshortened
  });
}

}  // namespace laige::render
