// laige-render 3D camera core implementation (M2-CAM-01).
//
// The contract is the preamble of include/laige/render/camera.h: the
// per-frame update runs follow step -> bounds clamp -> shake decay in
// that order; the shake is a bounded per-component offset decaying by
// shakeDecay per update; zoom divides the visible extent (ortho:
// half-extents, perspective: tan(fov/2)); the matrix builds delegate to
// the M2-GL-03 builders. Validation follows the FrameClock::create
// precedent (first failure wins, one rate-limited warn); the stopped
// state follows the RenderThread precedent (identity matrices,
// InvalidArgument mutators, no log).

#include "laige/render/camera.h"

#include <cmath>

#include <glm/geometric.hpp>  // cross, length
#include <glm/matrix.hpp>     // Mat4 operator*

#include "laige/logging.h"

namespace laige::render {

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool finite3(Vec3 v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float clampf(float x, float lo, float hi) noexcept {
  if (x > hi) return hi;
  if (x < lo) return lo;
  return x;
}

// The look-at margin check (the header preamble): the SUFFICIENT
// condition that keeps the matrices.h lookAt precondition true for the
// effective eye (eye + shake) under any bounded shake of radius
// maxShakeOffset — the eye can never reach the look-at point (the
// distance is strictly larger than the worst-case shake) and up never
// degenerates the view basis (its sine against the view direction stays
// above kLookAtNearParallel). NaN inputs classify as failing (every
// comparison against NaN is false / ordered comparisons false).
bool lookAtMargin(Vec3 eye, Vec3 center, Vec3 up, float maxShakeOffset) noexcept {
  const Vec3 d = center - eye;
  const float dist = glm::length(d);
  if (dist <= maxShakeOffset) return false;  // also rejects dist == 0/NaN
  const Vec3 f = d / dist;
  return glm::length(glm::cross(f, up)) > kLookAtNearParallel;
}

// The option validation (the header preamble): the first failing option
// wins. Returns the stable option name (LOG-001) or nullptr when valid;
// `outValue` carries a representative value for the warn's field.
const char* validateOptions(const CameraOptions& o, float& outValue) noexcept {
  if (o.projection != CameraProjection::Ortho &&
      o.projection != CameraProjection::Perspective) {
    outValue = static_cast<float>(o.projection);
    return "projection";
  }
  if (!std::isfinite(o.aspect) || o.aspect <= 0.0f) {
    outValue = o.aspect;
    return "aspect";
  }
  if (!std::isfinite(o.zNear) || !std::isfinite(o.zFar) || o.zNear >= o.zFar) {
    outValue = o.zNear;
    return "depth_slab";
  }
  if (o.projection == CameraProjection::Ortho) {
    if (!std::isfinite(o.halfWidth) || o.halfWidth <= 0.0f) {
      outValue = o.halfWidth;
      return "half_width";
    }
    if (!std::isfinite(o.halfHeight) || o.halfHeight <= 0.0f) {
      outValue = o.halfHeight;
      return "half_height";
    }
  } else {
    if (!std::isfinite(o.fovY) || o.fovY <= 0.0f || o.fovY >= kPi) {
      outValue = o.fovY;
      return "fov_y";
    }
  }
  if (!std::isfinite(o.zoomMin) || o.zoomMin <= 0.0f) {
    outValue = o.zoomMin;
    return "zoom_min";
  }
  if (!std::isfinite(o.zoomMax) || o.zoomMax < o.zoomMin) {
    outValue = o.zoomMax;
    return "zoom_max";
  }
  if (!std::isfinite(o.zoom)) {
    outValue = o.zoom;
    return "zoom";
  }
  if (!std::isfinite(o.followLerp) || o.followLerp <= 0.0f ||
      o.followLerp > 1.0f) {
    outValue = o.followLerp;
    return "follow_lerp";
  }
  if (!std::isfinite(o.shakeDecay) || o.shakeDecay < 0.0f ||
      o.shakeDecay >= 1.0f) {
    outValue = o.shakeDecay;
    return "shake_decay";
  }
  if (!std::isfinite(o.maxShakeOffset) || o.maxShakeOffset < 0.0f) {
    outValue = o.maxShakeOffset;
    return "max_shake_offset";
  }
  if (!finite3(o.position)) {
    outValue = o.position.x;
    return "position";
  }
  if (!finite3(o.target)) {
    outValue = o.target.x;
    return "target";
  }
  if (!finite3(o.up) || glm::length(o.up) == 0.0f) {
    outValue = o.up.x;
    return "up";
  }
  if (!lookAtMargin(o.position, o.target, o.up, o.maxShakeOffset)) {
    outValue = 0.0f;
    return "lookat_margin";
  }
  if (o.boundsEnabled) {
    const bool rectOk = std::isfinite(o.bounds.min.x) &&
                        std::isfinite(o.bounds.min.y) &&
                        std::isfinite(o.bounds.max.x) &&
                        std::isfinite(o.bounds.max.y) &&
                        o.bounds.min.x <= o.bounds.max.x &&
                        o.bounds.min.y <= o.bounds.max.y;
    if (!rectOk) {
      outValue = o.bounds.min.x;
      return "bounds";
    }
  }
  return nullptr;
}

}  // namespace

Result<Camera, ErrorCode> Camera::create(CameraOptions options) noexcept {
  float badValue = 0.0f;
  if (const char* bad = validateOptions(options, badValue); bad != nullptr) {
    LAIGE_LOG_WARN("camera", "options_invalid",
                   "the camera options failed validation",
                   laige::log::field("option", bad),
                   laige::log::field("value", badValue));
    return Result<Camera, ErrorCode>::failure(ErrorCode::InvalidArgument);
  }
  Camera cam;
  cam.valid_ = true;
  cam.projection_ = options.projection;
  cam.aspect_ = options.aspect;
  cam.halfWidth_ = options.halfWidth;
  cam.halfHeight_ = options.halfHeight;
  cam.fovY_ = options.fovY;
  cam.zNear_ = options.zNear;
  cam.zFar_ = options.zFar;
  cam.zoom_ = clampf(options.zoom, options.zoomMin, options.zoomMax);
  cam.zoomMin_ = options.zoomMin;
  cam.zoomMax_ = options.zoomMax;
  cam.boundsEnabled_ = options.boundsEnabled;
  cam.bounds_ = options.bounds;
  cam.followLerp_ = options.followLerp;
  cam.shakeDecay_ = options.shakeDecay;
  cam.maxShakeOffset_ = options.maxShakeOffset;
  cam.up_ = options.up;
  cam.target_ = options.target;
  // The documented initial clamp: a config position outside the
  // rectangle starts on the rectangle (the "position() is inside the
  // rectangle" invariant holds from creation).
  Vec3 p = options.position;
  if (options.boundsEnabled) {
    p.x = clampf(p.x, options.bounds.min.x, options.bounds.max.x);
    p.y = clampf(p.y, options.bounds.min.y, options.bounds.max.y);
  }
  cam.position_ = p;
  return Result<Camera, ErrorCode>::success(std::move(cam));
}

Status Camera::setPosition(Vec3 p) noexcept {
  if (!valid_) return Status(ErrorCode::InvalidArgument);
  if (!finite3(p)) {
    LAIGE_LOG_WARN("camera", "non_finite_input",
                   "the camera position setter got a non-finite value",
                   laige::log::field("input", "position"));
    return Status(ErrorCode::InvalidArgument);
  }
  if (!lookAtMargin(p, target_, up_, maxShakeOffset_)) {
    LAIGE_LOG_WARN("camera", "lookat_margin_violated",
                   "the new position violates the camera's look-at margin",
                   laige::log::field("min_distance", maxShakeOffset_));
    return Status(ErrorCode::InvalidArgument);
  }
  if (boundsEnabled_) {
    p.x = clampf(p.x, bounds_.min.x, bounds_.max.x);
    p.y = clampf(p.y, bounds_.min.y, bounds_.max.y);
  }
  position_ = p;
  return Status();
}

Status Camera::setTarget(Vec3 t) noexcept {
  if (!valid_) return Status(ErrorCode::InvalidArgument);
  if (!finite3(t)) {
    LAIGE_LOG_WARN("camera", "non_finite_input",
                   "the camera look-at setter got a non-finite value",
                   laige::log::field("input", "target"));
    return Status(ErrorCode::InvalidArgument);
  }
  if (!lookAtMargin(position_, t, up_, maxShakeOffset_)) {
    LAIGE_LOG_WARN("camera", "lookat_margin_violated",
                   "the new look-at point violates the camera's look-at margin",
                   laige::log::field("min_distance", maxShakeOffset_));
    return Status(ErrorCode::InvalidArgument);
  }
  target_ = t;
  return Status();
}

Status Camera::setFollowTarget(Vec3 t) noexcept {
  if (!valid_) return Status(ErrorCode::InvalidArgument);
  if (!finite3(t)) {
    LAIGE_LOG_WARN("camera", "non_finite_input",
                   "the camera follow-target setter got a non-finite value",
                   laige::log::field("input", "follow_target"));
    return Status(ErrorCode::InvalidArgument);
  }
  following_ = true;
  followTarget_ = t;
  return Status();
}

void Camera::stopFollowing() noexcept {
  if (valid_) following_ = false;
}

Status Camera::setZoom(float z) noexcept {
  if (!valid_) return Status(ErrorCode::InvalidArgument);
  if (!std::isfinite(z)) {
    LAIGE_LOG_WARN("camera", "non_finite_input",
                   "the camera zoom setter got a non-finite value",
                   laige::log::field("input", "zoom"));
    return Status(ErrorCode::InvalidArgument);
  }
  zoom_ = clampf(z, zoomMin_, zoomMax_);
  return Status();
}

Status Camera::applyShake(Vec3 impulse) noexcept {
  if (!valid_) return Status(ErrorCode::InvalidArgument);
  if (!finite3(impulse)) {
    LAIGE_LOG_WARN("camera", "non_finite_input",
                   "the camera shake impulse got a non-finite value",
                   laige::log::field("input", "shake"));
    return Status(ErrorCode::InvalidArgument);
  }
  shakeOffset_.x =
      clampf(shakeOffset_.x + impulse.x, -maxShakeOffset_, maxShakeOffset_);
  shakeOffset_.y =
      clampf(shakeOffset_.y + impulse.y, -maxShakeOffset_, maxShakeOffset_);
  shakeOffset_.z =
      clampf(shakeOffset_.z + impulse.z, -maxShakeOffset_, maxShakeOffset_);
  return Status();
}

void Camera::update() noexcept {
  if (!valid_) return;
  if (following_) {
    // The rigid-translation follow step (the header preamble): the same
    // delta moves eye and look-at, so the view direction and the eye ->
    // look-at distance stay constant within float rounding while the
    // look-at point converges to the follow target.
    const Vec3 delta = (followTarget_ - target_) * followLerp_;
    target_ += delta;
    position_ += delta;
  }
  if (boundsEnabled_) {
    position_.x = clampf(position_.x, bounds_.min.x, bounds_.max.x);
    position_.y = clampf(position_.y, bounds_.min.y, bounds_.max.y);
  }
  shakeOffset_ *= shakeDecay_;
}

Mat4 Camera::view() const noexcept {
  if (!valid_) return Mat4(1.0f);
  return lookAt(effectivePosition(), target_, up_);
}

Mat4 Camera::projection() const noexcept {
  if (!valid_) return Mat4(1.0f);
  if (projection_ == CameraProjection::Ortho) {
    const float hw = halfWidth_ / zoom_;
    const float hh = halfHeight_ / zoom_;
    return ortho(-hw, hw, -hh, hh, zNear_, zFar_);
  }
  // The zoom semantics (the header preamble): divide the extent by the
  // zoom factor — fov_eff = 2*atan(tan(fovY/2)/Z). At Z = 1 the
  // round-trip through tan/atan keeps fov_eff within 1-2 ulp of fovY.
  const float fov = 2.0f * std::atan(std::tan(fovY_ * 0.5f) / zoom_);
  return perspective(fov, aspect_, zNear_, zFar_);
}

Mat4 Camera::viewProjection() const noexcept {
  return projection() * view();
}

}  // namespace laige::render
