// laige-render isometric camera implementation (M2-CAM-02).
//
// The contract is the preamble of include/laige/render/iso_camera.h:
// the preset is validated at the config boundary (the custom shear
// must pass isoShearSupported — the M2-ISO-01 checker; M2-CAM-02 owns
// the scene-shear validation), the grid snap runs CONTINUOUSLY (create,
// setPosition, after every update's follow step), the snap candidates
// are validated against the INFLATED look-at margin so the "on the
// grid" invariant is total, and the zoom levels are the documented
// dyadic ladder. The matrix build is O(1) over the preset axes; the
// snap arithmetic is float-only (no integer conversion, total for
// every finite input).

#include "laige/render/iso_camera.h"

#include <cmath>

#include <glm/geometric.hpp>  // cross, length

#include "laige/logging.h"
#include "laige/render/iso_depth_key.h"  // isoShearSupported (M2-ISO-01)

namespace laige::render {

namespace {

// The M2-CAM-01 look-at margin (the camera.h contract), restated as a
// free function: the SUFFICIENT condition that keeps the matrices.h
// lookAt precondition true for the effective eye (position + shake)
// under any bounded shake of radius `bound` — the eye can never reach
// the look-at point and the view basis never degenerates. NaN inputs
// classify as failing (every ordered comparison against NaN is false).
bool marginOk(Vec3 position, Vec3 target, Vec3 up, float bound) noexcept {
  const Vec3 d = target - position;
  const float dist = glm::length(d);
  if (dist <= bound) return false;  // also rejects dist == 0 / NaN
  const Vec3 f = d / dist;
  return glm::length(glm::cross(f, up)) > kLookAtNearParallel;
}

// The finite3 helper (the camera.cpp precedent).
bool finite3(Vec3 v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The nearest grid multiple of v (ties AWAY FROM ZERO), float-only —
// no integer conversion, so the function is total for every finite v
// and finite g > 0: q = v/g is finite or inf (never NaN for finite
// v, g > 0 — a NaN v is rejected upstream), and floor/ceil of inf are
// defined. The snapped value is non-finite ONLY when |v|/g exceeds the
// float range — the callers reject that (snap_not_representable).
float snapCoordLocal(float v, float g) noexcept {
  const float q = v / g;
  const float n = (q > 0.0f) ? std::floorf(q + 0.5f) : std::ceilf(q - 0.5f);
  return g * n;
}

// True when v is already an exact grid multiple (the alignment check of
// the bounds rectangle — the header preamble's grid-aligned-bounds
// contract): v snaps to itself exactly.
bool gridAlignedLocal(float v, float g) noexcept { return snapCoordLocal(v, g) == v; }

// The preset's axes from the M2-GL-03 builders (single source of truth
// for the preset constants — the ADR 0005 tables): the isoMatrix
// element layout (matrices.h) is m[0][0] = dx.x, m[1][0] = dy.x,
// m[0][1] = dx.y, m[1][1] = dy.y, m[2][1] = zUnit.
IsoAxes axesOf(const Mat4& m) noexcept {
  return IsoAxes{Vec2{m[0][0], m[0][1]}, Vec2{m[1][0], m[1][1]}, m[2][1]};
}

}  // namespace

float IsoCamera::snapCoord(float v, float gridSize) noexcept {
  return snapCoordLocal(v, gridSize);
}

float IsoCamera::snapZoomLevel(float z, float zoomMin, float zoomMax) noexcept {
  const float c = (z < zoomMin) ? zoomMin : (z > zoomMax ? zoomMax : z);
  // The dyadic ladder from zoomMin: lo, 2*lo, 4*lo, ... — top is the
  // largest level <= zoomMax. Exact float arithmetic: the loop stores
  // only products that passed `<= zoomMax` (hence finite), and the
  // failing product is compared, never stored.
  float top = zoomMin;
  while (std::isfinite(top * 2.0f) && top * 2.0f <= zoomMax) top *= 2.0f;
  if (c >= top) return top;
  // Descend to the enclosing interval: level > c and level/2 < c.
  float level = top;
  while (level * 0.5f >= c) level *= 0.5f;
  // Nearest level in log2 space: the geometric midpoint of the pair is
  // lower*sqrt(2); an exact float tie goes to the HIGHER zoom.
  const float lower = level * 0.5f;
  return (c < lower * kIsoSnapSqrtTwo) ? lower : level;
}

float IsoCamera::marginBound() const noexcept {
  return camera_.maxShakeOffset() +
         (snapEnabled_ ? gridSize_ * kIsoSnapMarginPerCell : 0.0f);
}

Mat4 IsoCamera::matrix() const noexcept {
  if (!valid_) return Mat4(1.0f);
  const float inv = 1.0f / camera_.zoom();  // zoom is in [zoomMin, zoomMax], > 0
  const Vec3 e = camera_.effectivePosition();
  Mat4 m{};
  m[0][0] = axes_.dx.x * inv;
  m[1][0] = axes_.dy.x * inv;
  m[0][1] = axes_.dx.y * inv;
  m[1][1] = axes_.dy.y * inv;
  m[2][1] = axes_.zUnit * inv;
  // The camera center e projects to NDC (0, 0, 0): the translation is
  // -axes(e) scaled by the zoom (the header preamble's matrix section).
  float tx = -(axes_.dx.x * e.x + axes_.dy.x * e.y) * inv;
  float ty = -(axes_.dx.y * e.x + axes_.dy.y * e.y) * inv;
  if (tx == 0.0f) tx = 0.0f;  // normalize -0.0f (the isoMatrix golden form)
  if (ty == 0.0f) ty = 0.0f;
  m[3][0] = tx;
  m[3][1] = ty;
  m[3][3] = 1.0f;
  return m;
}

laige::Result<IsoCamera, laige::ErrorCode>
IsoCamera::create(IsoCameraOptions o) noexcept {
  // First failure wins (the Camera::create precedent): the M2-CAM-01
  // options first (its own camera/options_invalid warn), then the
  // preset, the snap, and the cross-options invariants — one rate-
  // limited iso_camera/options_invalid warn per failed create.
  const laige::Result<Camera, laige::ErrorCode> camR = Camera::create(o.camera);
  if (camR.isError()) {
    return laige::Result<IsoCamera, laige::ErrorCode>::failure(
        *camR.errorIfError());
  }

  auto reject = [&](const char* option, float value) {
    LAIGE_LOG_WARN("iso_camera", "options_invalid",
                   "IsoCamera::create rejected the options",
                   laige::log::field("option", option),
                   laige::log::field("value", value));
    return laige::Result<IsoCamera, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  };

  const IsoPresetKind kind = o.preset.kind;
  if (kind != IsoPresetKind::Dimetric2To1 &&
      kind != IsoPresetKind::TrueIso3060 &&
      kind != IsoPresetKind::CustomShear) {
    return reject("preset_kind", static_cast<float>(kind));
  }
  if (kind != IsoPresetKind::CustomShear &&
      (!std::isfinite(o.preset.scale) || o.preset.scale <= 0.0f)) {
    return reject("preset_scale", o.preset.scale);
  }
  if (kind == IsoPresetKind::CustomShear && !isoShearSupported(o.preset.axes)) {
    // M2-CAM-02 owns the scene-shear validation (the M2-ISO-01 checker):
    // an unsupported shear would break the engine-owned depth order
    // (RENDER-003) — rejected at the config boundary (API-008).
    return reject("preset_shear", o.preset.axes.zUnit);
  }
  if (!std::isfinite(o.snap.gridSize) || o.snap.gridSize <= 0.0f ||
      o.snap.gridSize < kIsoSnapMinGridSize) {
    return reject("grid_size", o.snap.gridSize);
  }
  const float g = o.snap.gridSize;
  if (o.snap.enabled && o.camera.boundsEnabled) {
    // The grid-aligned-bounds contract (the header preamble): the snap
    // of a point in a grid-aligned rectangle stays inside it, so both
    // invariants hold exactly when the rectangle is grid-aligned.
    const CameraBounds& b = o.camera.bounds;
    if (!gridAlignedLocal(b.min.x, g) || !gridAlignedLocal(b.min.y, g) ||
        !gridAlignedLocal(b.max.x, g) || !gridAlignedLocal(b.max.y, g)) {
      return reject("bounds_grid_alignment", g);
    }
  }
  // The snap-margin contract (the header preamble): the inflated
  // look-at margin, so no snap can ever break the M2-CAM-01 margin.
  Camera cam = *camR.valueIfOk();
  const float need = o.snap.enabled
                         ? o.camera.maxShakeOffset + g * kIsoSnapMarginPerCell
                         : o.camera.maxShakeOffset;
  if (!marginOk(cam.position(), cam.target(), cam.up(), need)) {
    return reject("snap_margin", need);
  }

  if (o.snap.enabled) {
    // Snap the initial position (the create is a snap point).
    Vec3 p = cam.position();
    p.x = snapCoordLocal(p.x, g);
    p.y = snapCoordLocal(p.y, g);
    if (p.x != cam.position().x || p.y != cam.position().y) {
      if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
        LAIGE_LOG_WARN("iso_camera", "snap_not_representable",
                       "create: the snapped initial position is not "
                       "representable for this grid size",
                       laige::log::field("grid_size", g));
        return laige::Result<IsoCamera, laige::ErrorCode>::failure(
            laige::ErrorCode::InvalidArgument);
      }
      // The snap-margin contract makes this commit pass; the defensive
      // rejection (impossible) keeps the state valid if it ever fires.
      if (!cam.setPosition(p).ok()) {
        LAIGE_LOG_WARN("iso_camera", "lookat_margin_violated",
                       "create: the snapped initial position would break "
                       "the look-at margin (impossible under the snap-"
                       "margin contract)",
                       laige::log::field("min_distance", need));
        return laige::Result<IsoCamera, laige::ErrorCode>::failure(
            laige::ErrorCode::InvalidArgument);
      }
    }
  }

  IsoCamera ic;
  ic.valid_ = true;
  ic.camera_ = std::move(cam);
  ic.preset_ = o.preset;
  switch (kind) {
    case IsoPresetKind::Dimetric2To1:
      ic.axes_ = axesOf(isoDimetric2To1(o.preset.scale));
      break;
    case IsoPresetKind::TrueIso3060:
      ic.axes_ = axesOf(isoTrueIso3060(o.preset.scale));
      break;
    case IsoPresetKind::CustomShear:
      ic.axes_ = o.preset.axes;
      break;
  }
  ic.snapEnabled_ = o.snap.enabled;
  ic.gridSize_ = g;
  return laige::Result<IsoCamera, laige::ErrorCode>::success(std::move(ic));
}

laige::Status IsoCamera::setPosition(Vec3 position) noexcept {
  if (!valid_) return laige::Status(laige::ErrorCode::InvalidArgument);
  if (!finite3(position)) {
    LAIGE_LOG_WARN("iso_camera", "non_finite_input",
                   "setPosition: non-finite position",
                   laige::log::field("input", "position"));
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  // The candidate: bounds clamp (M2-CAM-01), then the grid snap — the
  // candidate is validated BEFORE anything is committed (state-
  // unchanged on rejection).
  Vec3 c = position;
  if (camera_.boundsEnabled()) {
    const CameraBounds& b = camera_.bounds();
    c.x = (c.x < b.min.x) ? b.min.x : (c.x > b.max.x ? b.max.x : c.x);
    c.y = (c.y < b.min.y) ? b.min.y : (c.y > b.max.y ? b.max.y : c.y);
  }
  if (snapEnabled_) {
    c.x = snapCoordLocal(c.x, gridSize_);
    c.y = snapCoordLocal(c.y, gridSize_);
    if (!std::isfinite(c.x) || !std::isfinite(c.y)) {
      LAIGE_LOG_WARN("iso_camera", "snap_not_representable",
                     "setPosition: the snapped position is not "
                     "representable (the position is too far from the "
                     "origin for this grid size)",
                     laige::log::field("grid_size", gridSize_));
      return laige::Status(laige::ErrorCode::InvalidArgument);
    }
  }
  if (!marginOk(c, camera_.target(), camera_.up(), marginBound())) {
    LAIGE_LOG_WARN("iso_camera", "lookat_margin_violated",
                   "setPosition: the candidate position would break the "
                   "look-at margin",
                   laige::log::field("min_distance", marginBound()));
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  // Commit through the M2-CAM-01 camera: the strict margin check passes
  // by construction (the inflated bound is stricter) and the re-clamp
  // is a no-op (c is already inside the rectangle).
  return camera_.setPosition(c);
}

laige::Status IsoCamera::setTarget(Vec3 target) noexcept {
  if (!valid_) return laige::Status(laige::ErrorCode::InvalidArgument);
  if (!finite3(target)) {
    LAIGE_LOG_WARN("iso_camera", "non_finite_input",
                   "setTarget: non-finite target",
                   laige::log::field("input", "target"));
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  // The target is free (the grid locks the camera position, not the
  // look-at point) — but the next frame's snap can move the position by
  // up to g*sqrt(2)/2, so the INFLATED margin must hold (the header
  // preamble).
  if (!marginOk(camera_.position(), target, camera_.up(), marginBound())) {
    LAIGE_LOG_WARN("iso_camera", "lookat_margin_violated",
                   "setTarget: the target would break the look-at margin "
                   "under the grid snap",
                   laige::log::field("min_distance", marginBound()));
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  // Commit through the M2-CAM-01 camera: its strict margin check passes
  // by construction.
  return camera_.setTarget(target);
}

laige::Status IsoCamera::setFollowTarget(Vec3 target) noexcept {
  if (!valid_) return laige::Status(laige::ErrorCode::InvalidArgument);
  return camera_.setFollowTarget(target);
}

void IsoCamera::stopFollowing() noexcept {
  if (valid_) camera_.stopFollowing();
}

laige::Status IsoCamera::setZoom(float zoom) noexcept {
  if (!valid_) return laige::Status(laige::ErrorCode::InvalidArgument);
  if (!std::isfinite(zoom)) {
    LAIGE_LOG_WARN("iso_camera", "non_finite_input",
                   "setZoom: non-finite zoom",
                   laige::log::field("input", "zoom"));
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  if (!snapEnabled_) return camera_.setZoom(zoom);  // the M2-CAM-01 clamp
  // The documented dyadic ladder: the level is in [zoomMin, zoomMax] by
  // construction, so the base clamp is a no-op and no warn fires.
  return camera_.setZoom(snapZoomLevel(zoom, camera_.zoomMin(),
                                       camera_.zoomMax()));
}

laige::Status IsoCamera::applyShake(Vec3 impulse) noexcept {
  if (!valid_) return laige::Status(laige::ErrorCode::InvalidArgument);
  return camera_.applyShake(impulse);
}

void IsoCamera::update() noexcept {
  if (!valid_) return;
  camera_.update();  // follow step -> bounds clamp -> shake decay
  if (!snapEnabled_) return;
  const Vec3 p = camera_.position();
  const float sx = snapCoordLocal(p.x, gridSize_);
  const float sy = snapCoordLocal(p.y, gridSize_);
  if (sx == p.x && sy == p.y) return;  // already on the grid
  if (!std::isfinite(sx) || !std::isfinite(sy)) {
    // Outside the documented world domain for this grid size: keep the
    // (valid, off-grid for this frame only) position and surface it.
    LAIGE_LOG_WARN("iso_camera", "snap_not_representable",
                   "update: the snapped position is not representable "
                   "(the position is too far from the origin for this "
                   "grid size)",
                   laige::log::field("grid_size", gridSize_));
    return;
  }
  // The snap-margin contract makes this commit pass (no input can
  // break the margin through the snap); the defensive rejection keeps
  // the state valid if it ever fires.
  if (!camera_.setPosition(Vec3{sx, sy, p.z}).ok()) {
    LAIGE_LOG_WARN("iso_camera", "lookat_margin_violated",
                   "update: the snapped position would break the look-at "
                   "margin (impossible under the snap-margin contract)",
                   laige::log::field("min_distance", marginBound()));
  }
}

}  // namespace laige::render
