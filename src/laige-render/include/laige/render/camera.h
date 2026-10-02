// laige-render 3D camera core (M2-CAM-01): the presentation-side camera.
//
// FR-2.4 (roadmap/M2-rendering-2.5d.md, M2-CAM-01 scope): a full 3D
// camera — position, look-at, ortho or perspective (FOV), zoom with
// clamping (min/max per scene config), rectangular bounds constraint,
// smooth follow (target + lerp factor), shake (bounded, decaying,
// deterministic given input). The matrices come from the M2-GL-03
// builders (lookAt / ortho / perspective). The isometric camera
// presets and the grid-snap mode (M2-CAM-02) live in
// laige/render/iso_camera.h, on top of this camera: the IsoCamera
// owns a Camera by value and adds the preset matrix build, the grid
// position snap, and the zoom-level snapping.
//
//   CameraProjection  Ortho | Perspective (vertical FOV)
//   CameraBounds      The rectangular position constraint (ground plane)
//   CameraOptions     The scene-level camera configuration (API-006)
//   Camera            Camera state + the per-frame update + matrix builds
//
// ---------------------------------------------------------------------------
// Presentation-only (ARCH-009)
// ---------------------------------------------------------------------------
//
// The camera reads nothing from and writes nothing to sim state: the
// follow target is supplied by game code from the presentation state
// (the read-only boundary, M1-LOOP-02), and the camera never touches
// the world. Its state is presentation state — never part of replay
// state or the simulation state hash (ARCH-009). Determinism scope:
// given the same input sequence (the same create options and the same
// setter/update calls in the same order), the same build on the same
// platform produces bit-identical camera state — update() is a fixed
// sequence of float ops (render-side float, NOT SimMath: the pinned-
// math contract of PRD §10.3 does not apply; no RNG, no clock).
//
// ---------------------------------------------------------------------------
// Conventions (pinned — the matrices.h contract; the canonical home is
// docs/concepts/coordinates.md)
// ---------------------------------------------------------------------------
//
// World space: right-handed; (x, y) is the ground plane, +z up. NDC
// (OpenGL): x right, y up, z in [-1, +1], the camera looks along its
// own -z axis.
//
// Zoom: a MAGNIFICATION factor Z in [zoomMin, zoomMax] — the visible
// extent is divided by Z (Z = 2 shows half the extent; zoomed in):
//
//   Ortho:       halfWidth/Z, halfHeight/Z (world units)
//   Perspective: fov_eff = 2*atan(tan(fovY/2)/Z)
//
// Z = 1 is the configured extent. The round-trip through tan/atan at
// Z = 1 keeps fov_eff within 1-2 ulp of fovY (documented; the tests
// pin it).
//
// The rectangular bounds constrain the camera POSITION's (x, y)
// ground-plane coordinates only (z is unconstrained — the camera's
// height is free). A bounded shake may push the effective render
// position at most maxShakeOffset past the rectangle's edge
// (documented); the base position always stays inside.
//
// The look-at margin: |target - position| > maxShakeOffset and up not
// near-parallel (within kLookAtNearParallel sine) to the view
// direction. This is the SUFFICIENT condition that keeps the
// matrices.h lookAt precondition true for the effective eye
// (position + shakeOffset) under any bounded shake — the eye can
// never reach the look-at point and the view basis never degenerates.
//
// ---------------------------------------------------------------------------
// The per-frame update contract (one update() per presentation frame)
// ---------------------------------------------------------------------------
//
// The caller paces update() once per presentation frame (the frame
// clock's cadence, M2-GL-02). It runs, in this order:
//
//   1. follow step (while following):
//        delta = (followTarget - target) * followLerp
//        target += delta;  position += delta
//      A rigid translation: the eye -> look-at vector (the view
//      direction and the eye -> look-at distance) stays constant within
//      float rounding, and the look-at point converges to the follow
//      target — the camera slides until the followed point sits at
//      the look-at point (the screen center of the view). followLerp
//      in (0, 1] is PER UPDATE (the documented lerp factor); frame-
//      rate independence is the caller's concern.
//   2. bounds clamp (when enabled): position.xy clamped into the
//      rectangle (step 1 can carry it out; the clamp lands it back).
//   3. shake decay: shakeOffset *= shakeDecay (per component).
//
// ---------------------------------------------------------------------------
// The shake (bounded, decaying, deterministic given input)
// ---------------------------------------------------------------------------
//
// The shake is a bounded camera-position OFFSET added to the base
// position when the view matrix is built (the eye = position +
// shakeOffset):
//
//   applyShake(impulse):  offset = clamp(offset + impulse,
//                                       -max, +max)   per component
//                         (max = maxShakeOffset — the offset never
//                          exceeds it: bounded)
//   update():             offset *= shakeDecay
//
// With the default shakeDecay = 0.5 the offset is exactly halved per
// update; in IEEE binary32, x * 2^-n <= 2^-150 rounds to 0, so the
// offset reaches exactly 0.0 within
//
//   150 + ceil(log2(maxShakeOffset)) updates
//
// (fewer when the bound is < 1 world unit) — i.e. within 150 updates
// for any bound <= 1: the documented decay bound (the tests pin it
// exactly, including the last nonzero denormal).
//
// ---------------------------------------------------------------------------
// Validation and failure behavior
// ---------------------------------------------------------------------------
//
// Camera::create(options) validates the options (first failure wins;
// one rate-limited camera/options_invalid warn) and returns
// Result<Camera, ErrorCode> — InvalidArgument on a bad config
// (API-008: validated at the boundary, the failure is actionable).
// The initial position is CLAMPED into the rectangle when the bounds
// are enabled, so the invariant "position() is inside the rectangle"
// holds from creation.
//
// A failed create is the STOPPED state (the FrameClock failed-create
// precedent): valid() is false, the matrix builders return the
// identity matrix, and the mutators return InvalidArgument with no
// log (the RenderThread stopped-state precedent).
//
// The runtime setters validate their input at the boundary (one
// rate-limited warn each, InvalidArgument on failure):
//
//   - non-finite input (camera/non_finite_input);
//   - setPosition / setTarget must keep the look-at margin (above)
//     (camera/lookat_margin_violated);
//   - setZoom CLAMPS into [zoomMin, zoomMax] — clamping is the
//     documented behavior, not an error: a request below min yields
//     exactly min, above max exactly max (exact at the bounds).
//
// ---------------------------------------------------------------------------
// Ownership / lifetime / threading
// ---------------------------------------------------------------------------
//
// A single-owner value object owned by the game's render set-up /
// presentation phase (the frame pipeline's phases, M2-GL-02). It
// carries no resources: copy and move are plain value copies (no
// ownership transfer semantics — unlike GlContext/RenderThread).
// Not thread-safe (no internal synchronization, CONC-001); the frame
// pipeline reads the built matrices in its render set-up stage.
//
// ---------------------------------------------------------------------------
// Performance (PERF-003, DOC-004)
// ---------------------------------------------------------------------------
//
// No heap storage at all: every operation — create, every setter,
// update(), the matrix builds — allocates nothing on every path, so
// the zero-per-frame-allocation property is structural. update() is
// O(1) float arithmetic (a few dozen ops); view() is lookAt (two
// normalizations + one cross); projection() is a dozen flops
// (perspective adds tan + atan). No locks, no GL calls, and no logging
// on the healthy path (rejection paths log one rate-limited warn).
// Build the matrices once per frame change (set-up phase); the per-
// sprite world -> NDC multiply (M2-SPRITE-02) consumes the stored
// viewProjection — never rebuild it per sprite.
//
// Misuse warnings:
//   - pace update() once per presentation frame: the lerp and decay
//     factors are per-update, not per-second;
//   - rebuild viewProjection() only when the camera changed;
//   - a look-at margin near maxShakeOffset is a degenerate config: a
//     worst-case shake can put the effective eye on the look-at
//     point (the lookAt precondition — the debug assert fires).

#pragma once

#include "laige/render/matrices.h"  // Vec2/Vec3/Mat4 + the matrix builders
#include "laige/result.h"

namespace laige::render {

// Radians per degree (the fovY default and the perspective validation;
// the engine takes radians everywhere — matrices.h).
inline constexpr float kCameraDegreesToRadians =
    3.14159265358979323846f / 180.0f;
// The sine below which `up` is treated as parallel to the view
// direction (the lookAt precondition's numerical margin — CORE-005).
inline constexpr float kLookAtNearParallel = 1e-5f;

// The camera's projection kind (API-008: a named type, not a boolean).
enum class CameraProjection {
  // Orthographic: the view's half-extents are halfWidth/halfHeight in
  // world units (zoom divides them).
  Ortho,
  // Perspective: the vertical field of view fovY (radians); zoom
  // divides the extent (the header preamble's zoom semantics).
  Perspective,
};

// The rectangular camera-position constraint: a closed rectangle in
// the ground plane (x, y world units; the camera's z is unconstrained).
// min <= max per component (validated in Camera::create).
struct CameraBounds {
  Vec2 min{0.0f, 0.0f};
  Vec2 max{0.0f, 0.0f};
};

// The camera's scene-level configuration (API-006: an option
// structure, not positional booleans). Validated by Camera::create —
// see the header preamble for the full contract.
struct CameraOptions {
  // The camera's initial position (world units). Clamped into the
  // rectangle at creation when the bounds are enabled.
  Vec3 position{0.0f, 0.0f, 0.0f};
  // The initial look-at point (the screen center of the view). Must
  // keep the look-at margin vs `position` and `up` (the preamble).
  Vec3 target{0.0f, 0.0f, -1.0f};
  // The camera up vector (normalized internally by lookAt). Must not
  // be near-parallel to (target - position).
  Vec3 up{0.0f, 1.0f, 0.0f};
  // The projection kind (Ortho — the 2.5D engine's default view).
  CameraProjection projection{CameraProjection::Ortho};
  // Screen aspect (width / height), > 0 (used by Perspective only).
  float aspect{16.0f / 9.0f};
  // Ortho only: the view's half-extents in world units at zoom 1.
  float halfWidth{10.0f};
  float halfHeight{5.0f};
  // Perspective only: the vertical field of view (radians);
  // 0 < fovY < pi.
  float fovY{60.0f * kCameraDegreesToRadians};
  // The depth slab (both kinds): the near plane sits at camera z =
  // -zNear, the far plane at camera z = -zFar (matrices.h); zNear <
  // zFar, and zNear may be <= 0 for orthographic slabs.
  float zNear{1.0f};
  float zFar{100.0f};
  // Zoom: the initial magnification (clamped into [zoomMin, zoomMax]
  // at creation); the clamp bounds are per scene config (FR-2.4).
  float zoom{1.0f};
  float zoomMin{0.1f};
  float zoomMax{16.0f};
  // The rectangular bounds constraint (the header preamble).
  bool boundsEnabled{false};
  CameraBounds bounds{CameraBounds{
      Vec2{-1000.0f, -1000.0f}, Vec2{1000.0f, 1000.0f}}};
  // Smooth follow: the per-update lerp factor in (0, 1] (the preamble
  //'s follow step).
  float followLerp{0.2f};
  // Shake: the per-update decay factor in [0, 1) (0 kills the shake in
  // one update) and the per-component offset bound in world units.
  float shakeDecay{0.5f};
  float maxShakeOffset{0.25f};
};

// The 3D camera core (FR-2.4). A presentation-only value object
// (ARCH-009) — the header preamble is the full contract.
class Camera {
 public:
  // The stopped state (the failed-create / moved-from-nothing form):
  // valid() is false, the matrix builders return the identity matrix,
  // the mutators return InvalidArgument with no log.
  Camera() noexcept = default;
  // Value semantics: plain copies (no resources to own — unlike
  // GlContext/RenderThread).
  Camera(const Camera&) = default;
  Camera& operator=(const Camera&) = default;

  // Validates the options (the header preamble's failure section) and
  // clamps the initial position into the rectangle when the bounds
  // are enabled. One rate-limited camera/options_invalid warn per
  // failed create (first failing option wins).
  // @budget O(1); no allocation.
  [[nodiscard]] static laige::Result<Camera, laige::ErrorCode>
  create(CameraOptions options) noexcept;

  // True when the camera holds a validated configuration.
  [[nodiscard]] bool valid() const noexcept { return valid_; }

  // The camera state (world units).
  [[nodiscard]] Vec3 position() const noexcept { return position_; }
  [[nodiscard]] Vec3 target() const noexcept { return target_; }
  [[nodiscard]] Vec3 up() const noexcept { return up_; }
  // The current bounded shake offset (zero when none).
  [[nodiscard]] Vec3 shakeOffset() const noexcept { return shakeOffset_; }
  // The effective render position: position + shakeOffset (the eye the
  // view matrix is built from).
  [[nodiscard]] Vec3 effectivePosition() const noexcept {
    return position_ + shakeOffset_;
  }
  // The current zoom (always in [zoomMin, zoomMax]).
  [[nodiscard]] float zoom() const noexcept { return zoom_; }
  // True while a follow target is set (stopFollowing clears it).
  [[nodiscard]] bool following() const noexcept { return following_; }
  [[nodiscard]] CameraProjection projectionKind() const noexcept {
    return projection_;
  }
  // The zoom clamp bounds (the preamble's zoom contract: zoom() is
  // always in [zoomMin, zoomMax]). M2-CAM-02's grid-snap zoom levels
  // are anchored at zoomMin.
  [[nodiscard]] float zoomMin() const noexcept { return zoomMin_; }
  [[nodiscard]] float zoomMax() const noexcept { return zoomMax_; }
  // The rectangular-bounds state (the preamble's bounds contract):
  // whether the rectangle is enabled and the rectangle itself (closed,
  // min <= max per component). The camera's z is unconstrained.
  [[nodiscard]] bool boundsEnabled() const noexcept { return boundsEnabled_; }
  [[nodiscard]] CameraBounds bounds() const noexcept { return bounds_; }
  // The shake offset bound (world units per component) — the look-at
  // margin's bound term (the preamble's margin contract).
  [[nodiscard]] float maxShakeOffset() const noexcept { return maxShakeOffset_; }

  // -----------------------------------------------------------------
  // Mutation (owner thread; validation per the header preamble)
  // -----------------------------------------------------------------

  // Sets the camera position. (x, y) are CLAMPED into the rectangle
  // when the bounds are enabled — the documented constraint, never an
  // error. Rejects (InvalidArgument + warn) non-finite input and input
  // that violates the look-at margin (|target - p| > maxShakeOffset,
  // up not near-parallel). State is unchanged on rejection.
  // @budget O(1); no allocation.
  [[nodiscard]] laige::Status setPosition(Vec3 position) noexcept;
  // Sets the look-at point (the mirror of setPosition).
  [[nodiscard]] laige::Status setTarget(Vec3 target) noexcept;

  // Sets the follow target and enables the smooth follow (the
  // preamble's follow step; update() applies it from the next call).
  // Rejects non-finite input (InvalidArgument + warn).
  [[nodiscard]] laige::Status setFollowTarget(Vec3 target) noexcept;
  // Disables the follow (the camera stops sliding on the next update).
  void stopFollowing() noexcept;

  // Sets the zoom: CLAMPED into [zoomMin, zoomMax], exact at the bounds
  // (below min yields exactly min, above max exactly max) — clamping
  // is documented behavior, not an error. Rejects non-finite input.
  [[nodiscard]] laige::Status setZoom(float zoom) noexcept;

  // Adds a bounded shake impulse: the offset becomes
  // clamp(offset + impulse, -maxShakeOffset, +maxShakeOffset) per
  // component (the header preamble's shake section). Rejects non-
  // finite input.
  [[nodiscard]] laige::Status applyShake(Vec3 impulse) noexcept;

  // The per-frame update (the preamble's 3-step order: follow step,
  // bounds clamp, shake decay). No-op on a stopped camera.
  // @budget O(1) float ops; no allocation; no logging.
  void update() noexcept;

  // -----------------------------------------------------------------
  // Matrix builds (the M2-GL-03 builders; O(1), no allocation)
  // -----------------------------------------------------------------

  // The view matrix (world -> camera space): lookAt(effectivePosition,
  // target, up) — the shake moves the eye. The identity matrix on a
  // stopped camera.
  [[nodiscard]] Mat4 view() const noexcept;
  // The projection matrix (camera -> NDC) with the current zoom applied
  // (the preamble's zoom semantics). The identity on a stopped camera.
  [[nodiscard]] Mat4 projection() const noexcept;
  // The combined world -> NDC matrix (projection * view) — the frame's
  // render matrix (M2-SPRITE-02 consumes it). The identity on a
  // stopped camera.
  [[nodiscard]] Mat4 viewProjection() const noexcept;

 private:
  bool valid_{false};
  CameraProjection projection_{CameraProjection::Ortho};
  Vec3 position_{0.0f, 0.0f, 0.0f};
  Vec3 target_{0.0f, 0.0f, -1.0f};
  Vec3 up_{0.0f, 1.0f, 0.0f};
  float aspect_{16.0f / 9.0f};
  float halfWidth_{10.0f};
  float halfHeight_{5.0f};
  float fovY_{60.0f * kCameraDegreesToRadians};
  float zNear_{1.0f};
  float zFar_{100.0f};
  float zoom_{1.0f};
  float zoomMin_{0.1f};
  float zoomMax_{16.0f};
  bool boundsEnabled_{false};
  CameraBounds bounds_;
  float followLerp_{0.2f};
  bool following_{false};
  Vec3 followTarget_{0.0f, 0.0f, 0.0f};
  float shakeDecay_{0.5f};
  float maxShakeOffset_{0.25f};
  Vec3 shakeOffset_{0.0f, 0.0f, 0.0f};
};

}  // namespace laige::render
