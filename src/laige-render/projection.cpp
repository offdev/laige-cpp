// laige-render projection modes + screen<->world transforms
// implementation (M2-PROJ-01).
//
// The contract is the preamble of include/laige/render/projection.h:
// worldToScreen is one homogeneous matrix multiply (the perspective
// divide is w = 1 exactly for the affine builders, so one code path
// covers every mode); screenToWorldRay branches per mode on the
// documented preimage geometry; screenToWorld intersects it with the
// plane and returns the documented Result failures (parallel plane;
// behind the camera). No allocation, no logging, no GL calls on any
// path.

#include "laige/render/projection.h"

#include <cassert>
#include <cmath>

#include <glm/geometric.hpp>  // cross, dot, normalize
#include <glm/matrix.hpp>     // Mat4 * vec4, inverse

// glm::vec4 is used directly (not a public matrices.h alias — the
// public API never names it; the Vec2/Vec3/Mat4 aliases are the only
// GLM types the module's public headers expose).

namespace laige::render {

namespace {

// The cross product of the matrix's two screen rows: r1 = the screen-x
// row, r2 = the screen-y row, each taken as a world 3D vector from the
// matrix's 3x3 part (m[c][r] is row r of column c, matrices.h). For the
// affine builders this is exactly the preimage line's direction
// (unnormalized): both screen rows vanish on the line, so the line's
// direction is orthogonal to both rows.
Vec3 screenRowCross(const Mat4& m) noexcept {
  const Vec3 r1{m[0][0], m[1][0], m[2][0]};
  const Vec3 r2{m[0][1], m[1][1], m[2][1]};
  return glm::cross(r1, r2);
}

}  // namespace

Vec3 ProjectionView::worldToScreen(Vec2 p2d, float depth) const noexcept {
  // One homogeneous multiply. The affine builders (isoMatrix,
  // planeOrtho via ortho) have m[3][3] = 1 and m[3][0..2] = 0, so w = 1
  // EXACTLY and the divide is a no-op there; for free_cinematic w is
  // the perspective divisor (w = -z_cam, matrices.h) and the divide is
  // the full NDC projection.
  const glm::vec4 h = matrix * glm::vec4(p2d.x, p2d.y, depth, 1.0f);
  return Vec3(h.x / h.w, h.y / h.w, h.z / h.w);
}

WorldRay ProjectionView::screenToWorldRay(Vec2 ndc) const noexcept {
  assert(std::isfinite(ndc.x) && std::isfinite(ndc.y) &&
         "screenToWorldRay: finite NDC input is required");
  const float sx = ndc.x;
  const float sy = ndc.y;
  WorldRay ray{};
  switch (mode) {
    case ProjectionMode::Iso: {
      // The invertible 2x2 ground map (the isoMatrix precondition):
      // screen = A·(x, y) + t with A = [[a, b], [c, d]] the rows of the
      // matrix's ground part (column-major m[c][r]) and t the
      // translation column. The origin is on the ground plane (z = 0) —
      // the mode's reference plane (the header preamble).
      const float a = matrix[0][0];
      const float b = matrix[1][0];
      const float c = matrix[0][1];
      const float d = matrix[1][1];
      const float det = a * d - b * c;
      assert(det != 0.0f &&
             "screenToWorldRay: the iso ground map must be invertible "
             "(the isoMatrix precondition)");
      const float fx = sx - matrix[3][0];
      const float fy = sy - matrix[3][1];
      ray.origin = Vec3((fx * d - fy * b) / det, (a * fy - fx * c) / det,
                        0.0f);
      ray.direction = glm::normalize(screenRowCross(matrix));
      break;
    }
    case ProjectionMode::TopDown: {
      // The X/Y ground plane: screen x = world x (matrix row 0), screen
      // y = world y (matrix row 1); the preimage line runs along world
      // z. The origin sits on the reference plane through planeCenter
      // (perpendicular to the view — the header preamble).
      const float xScale = matrix[0][0];
      const float yScale = matrix[1][1];
      assert(xScale != 0.0f && yScale != 0.0f &&
             "screenToWorldRay: the top-down view scales must be nonzero");
      ray.origin = Vec3((sx - matrix[3][0]) / xScale,
                        (sy - matrix[3][1]) / yScale, planeCenter.z);
      ray.direction = glm::normalize(screenRowCross(matrix));
      break;
    }
    case ProjectionMode::SideView: {
      // Screen x = world x (matrix row 0), screen y = world height +z
      // (matrix row 2); the preimage line runs along world y. The
      // origin sits on the reference plane through planeCenter.
      const float xScale = matrix[0][0];
      const float zScale = matrix[2][1];
      assert(xScale != 0.0f && zScale != 0.0f &&
             "screenToWorldRay: the side-view view scales must be nonzero");
      ray.origin = Vec3((sx - matrix[3][0]) / xScale, planeCenter.y,
                        (sy - matrix[3][1]) / zScale);
      ray.direction = glm::normalize(screenRowCross(matrix));
      break;
    }
    case ProjectionMode::FreeCinematic: {
      // The preimage of an NDC point through the world->NDC matrix:
      // invert the two NDC depth points (z = -1 near, z = +1 far),
      // homogeneous-divide each (w = -z_cam, matrices.h), and build the
      // ray from the near point toward the far point — away from the
      // camera for screen points in front of it (always, for finite
      // window points). The origin is on the NDC near plane.
      const Mat4 inv = glm::inverse(matrix);
      const glm::vec4 nearH = inv * glm::vec4(sx, sy, -1.0f, 1.0f);
      const glm::vec4 farH = inv * glm::vec4(sx, sy, 1.0f, 1.0f);
      assert(std::isfinite(nearH.w) && std::isfinite(farH.w) &&
             nearH.w != 0.0f && farH.w != 0.0f &&
             "screenToWorldRay: the NDC point's preimage must not be "
             "degenerate (w != 0 — the point must not sit on the "
             "camera)");
      const Vec3 nearP(nearH.x / nearH.w, nearH.y / nearH.w,
                       nearH.z / nearH.w);
      const Vec3 farP(farH.x / farH.w, farH.y / farH.w, farH.z / farH.w);
      ray.origin = nearP;
      ray.direction = glm::normalize(farP - nearP);
      break;
    }
  }
  return ray;
}

laige::Result<Vec3, laige::ErrorCode>
ProjectionView::screenToWorld(Vec2 ndc, Plane plane) const noexcept {
  const WorldRay ray = screenToWorldRay(ndc);
  // The plane is parallel to the ray when dot(direction, normal) is 0;
  // the eps covers float rounding of an exactly-parallel dot and
  // rejects the numerically ill-conditioned near-parallel planes
  // (the kProjectionParallelEps preamble).
  const float denom = glm::dot(ray.direction, plane.normal);
  if (std::fabs(denom) <= kProjectionParallelEps) {
    return laige::Result<Vec3, laige::ErrorCode>::failure(
        ErrorCode::InvalidArgument);
  }
  const float t = (plane.d - glm::dot(plane.normal, ray.origin)) / denom;
  if (!std::isfinite(t)) {
    return laige::Result<Vec3, laige::ErrorCode>::failure(
        ErrorCode::InvalidArgument);
  }
  // FreeCinematic only: the preimage is a ray from the camera — the
  // intersection must lie in front of it (t >= 0). The affine modes use
  // the full preimage line (t ∈ ℝ — the header preamble: every
  // elevation on the line projects to the same screen point, so a
  // picking plane must reach it from either side of the origin).
  if (mode == ProjectionMode::FreeCinematic && t < 0.0f) {
    return laige::Result<Vec3, laige::ErrorCode>::failure(
        ErrorCode::InvalidArgument);
  }
  return laige::Result<Vec3, laige::ErrorCode>::success(
      ray.origin + ray.direction * t);
}

}  // namespace laige::render
