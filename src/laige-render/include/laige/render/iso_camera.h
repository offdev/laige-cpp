// laige-render isometric camera: the iso presets + the grid-snap camera
// mode (M2-CAM-02), on top of the M2-CAM-01 Camera.
//
// FR-2.4 (roadmap/M2-rendering-2.5d.md, M2-CAM-02 scope): isometric
// presets — 2:1 dimetric (default per D-ISO / ADR 0005), true 30°/60°,
// custom shear — selected by a single config value, the matrices from
// the M2-GL-03 builders; and the grid-snap camera mode: the camera
// position is quantized to grid coordinates (the standard isometric
// game feel) and the zoom is clamped to the documented grid-aligned
// level set. The M2-CAM-01 Camera (position, follow, bounds, shake,
// zoom clamp) is owned by value inside the IsoCamera; the preset and
// the snap config sit on top of it.
//
//   IsoPresetKind     dimetric_2_1 (default) | true_iso_30_60 | custom_shear
//   IsoPreset         the single preset config value: kind + (scale | axes)
//   GridSnapOptions   the grid-snap mode: enabled + the grid size
//   IsoCameraOptions  the camera (M2-CAM-01) + the preset + the snap
//   IsoCamera         the isometric camera: state, mutations, matrix()
//
// ---------------------------------------------------------------------------
// The isometric presets (ADR 0005; FR-2.4 / FR-2.5)
// ---------------------------------------------------------------------------
//
// The preset is ONE config value (the ADR 0005 config-only pattern — a
// scene that changes its iso look changes only its config, never its
// simulation, depth-key, picking, or asset code). No preset has its own
// matrix code: the axes come from the M2-GL-03 builders (single source
// of truth for the preset constants — the ADR 0005 tables):
//
//   Dimetric2To1: axes = the isoDimetric2To1(scale) matrix's axes
//   TrueIso3060:  axes = the isoTrueIso3060(scale) matrix's axes
//   CustomShear:  axes = the config's IsoAxes, which MUST pass
//                 isoShearSupported() (M2-ISO-01) — the engine owns the
//                 isometric render depth (FR-2.2, G-R11) and a shear
//                 that does not sort back-to-front would break
//                 RENDER-003's deterministic order, so unsupported
//                 shears are REJECTED at the config boundary (API-008).
//                 (The raw isoMatrix() builder remains available for
//                 advanced use: an unsupported but invertible shear
//                 still TRANSFORMS through a hand-stored ProjectionView
//                 — the M2-PROJ-01 contract — it is simply not a scene
//                 preset.)
//
// `scale` (Dimetric2To1 / TrueIso3060) is the preset's NDC scale
// (matrices.h: at zoom 1 the 2:1 tile is a 4*scale by 2*scale NDC
// rhombus) — finite and > 0.
//
// ---------------------------------------------------------------------------
// The camera matrix (the world -> NDC build, FR-2.5 / RENDER-006)
// ---------------------------------------------------------------------------
//
// matrix() returns the COMBINED world -> NDC affine matrix of the
// camera's current state (position, zoom, shake) — the single matrix
// the batcher (M2-SPRITE-02 onward) multiplies by. It is the preset's
// isoMatrix() with the zoom and the camera center applied:
//
//   e = effectivePosition() = position + shakeOffset   (the eye; the
//       shake moves the view center — the screen shake of FR-2.4)
//   Z = zoom()                                             (M2-CAM-01)
//   M = [ dx.x/Z   dy.x/Z   0        -(dx.x*e.x + dy.x*e.y)/Z ]
//       [ dx.y/Z   dy.y/Z   zUnit/Z  -(dx.y*e.x + dy.y*e.y)/Z ]
//       [ 0        0        0        0                    ]
//       [ 0        0        0        1                    ]
//
// At Z = 1 and e = (0, 0, 0) (no shake) M equals the preset's
// isoMatrix() exactly (the -0.0f normalizations pinned by the tests).
// The camera's ground point e projects to NDC (0, 0, 0) — the screen
// center — at every zoom and every position (the iso "camera center"
// is the screen-center ground point; an affine oblique projection has
// no separate view/projection split — the matrices.h contract). NDC-z
// is 0 for every world point: the 2.5D depth is engine-owned (the
// M2-ISO-01 depth keys, PRD §4), never derived from the projection.
// The shake's z component does not enter M: the iso projection has no
// vertical viewpoint (a screen shake uses the shake's x/y, which the
// projection maps to screen space).
//
// The matrix is built in the render SET-UP phase, once per frame
// change (camera update, zoom, position, preset switch) — the M2-CAM-01
// per-frame convention; never per sprite (the batcher multiplies the
// stored matrix).
//
// ---------------------------------------------------------------------------
// The grid-snap camera mode (FR-2.4)
// ---------------------------------------------------------------------------
//
// The DOCUMENTED CHOICE (the roadmap leaves snap-on-release vs
// continuous to this step): the engine SNAPS CONTINUOUSLY — the
// camera position's (x, y) ground coordinates are quantized to the
// grid on create, on every setPosition, and after every follow step of
// update(). The standard isometric grid-locked camera feel: the
// camera never rests off the grid, even mid-follow (it tracks moving
// targets in grid steps).
//
// The position snap (one per ground axis):
//
//   snapCoord(v, g) = g * round-half-away(v / g)
//
// float-only arithmetic (no integer conversion — total for every
// finite v and g > 0), nearest grid multiple, TIES AWAY FROM ZERO
// (the iso_depth_key.h rounding convention), deterministic per build.
// The z coordinate is NEVER snapped: the camera height is free (the
// M2-CAM-01 bounds contract).
//
// The snap invariants (all machine-checked by the tests/laige-render
// suite, CTest entry `iso_camera`):
//
//   - On the grid: in snap mode, position().x and position().y are
//     grid multiples (snapCoord(p, g) == p) after the create, after
//     every mutation, and after every update() — for ANY input inside
//     the documented world domain (|x|, |y| <= 32767 world units,
//     docs/concepts/coordinates.md §1; outside it the snap result can
//     be non-representable and the frame's snap is rejected with a
//     warn — the state stays valid, off-grid for that frame only).
//   - Grid-aligned bounds: the snap of a point inside a grid-ALIGNED
//     rectangle stays inside it (the nearest grid multiple of a value
//     in [m, M] with m, M multiples of g is itself in [m, M]), so grid-
//     snap mode REQUIRES the camera bounds rectangle to be grid-
//     aligned (every corner an exact multiple of g) whenever both are
//     enabled — validated at create (bounds_grid_alignment). Without
//     it no clamp/snap order can keep both invariants.
//   - Snap margin: the snap moves the position by at most g/2 per
//     ground axis (Euclidean distance <= g*sqrt(2)/2), so snap mode
//     validates the INFLATED look-at margin at the create and on every
//     position/target mutation:
//
//         |target - position| > maxShakeOffset + g*sqrt(2)/2
//
//     under which no snap can ever break the M2-CAM-01 look-at margin
//     (the view() precondition) — the "on the grid" invariant is TOTAL
//     inside the world domain: there is no input for which a snap is
//     silently skipped. (A mutation whose SNAPPED candidate would
//     break the margin is still rejected, state unchanged — the
//     documented failure path for adversarial inputs.)
//
// The ZOOM levels (the documented grid-aligned set, FR-2.4's "zoom
// clamping"):
//
//   L = { zoomMin * 2^n : n in Z>=0, zoomMin * 2^n <= zoomMax }
//
// the DYADIC LADDER anchored at zoomMin. Every level is a power of two
// away from zoomMin, so the grid-to-screen scale at any level is a
// power of two times the level-0 scale: moving between levels can
// never leave the grid off the pixel alignment it had at an aligned
// level (halving/doubling the scale preserves pixel alignment; an
// arbitrary zoom factor cannot). The final pixel alignment also
// depends on the window size — the game's concern (RENDER-006: the
// window is a platform detail, not part of this pure API); the ladder
// is the window-size-independent part.
//
// snapZoomLevel(z, zoomMin, zoomMax): z clamped into [zoomMin, zoomMax]
// (below/above clamp to the bounds — documented, not an error); the
// result is the NEAREST level in log2 space — the comparison is
// against the geometric midpoint level*sqrt(2) of the enclosing pair,
// and an EXACT float tie goes to the HIGHER zoom. The result is always
// an exact member of L (idempotent: snapZoomLevel of a level is that
// level — the tests pin the set exactly).
//
// ---------------------------------------------------------------------------
// Validation, failure, and stopped state
// ---------------------------------------------------------------------------
//
// create(options): first failure wins, one rate-limited warn per failed
// create (LOG-004), subsystem `iso_camera`, event `options_invalid`,
// the failing option in the stable `option` field:
//
//   1. the M2-CAM-01 camera options — validated by Camera::create
//      (its own `camera/options_invalid` warn; its failure short-circuits)
//   2. `preset_kind`    — kind not one of the three presets
//   3. `preset_scale`   — Dimetric2To1/TrueIso3060 scale not finite > 0
//   4. `preset_shear`   — CustomShear axes fail isoShearSupported()
//   5. `grid_size`      — not finite, <= 0, or < kIsoSnapMinGridSize
//   6. `bounds_grid_alignment` — snap + enabled bounds, a corner not
//      an exact multiple of gridSize
//   7. `snap_margin`    — snap enabled and the inflated margin fails
//
// The mutations: `InvalidArgument` with a state-unchanged contract on
// non-finite input (`non_finite_input` warn, `input` field) and on a
// snapped candidate that violates the (inflated) look-at margin
// (`lookat_margin_violated` warn — the M2-CAM-01 event name) or that
// snaps to non-finite coordinates (`snap_not_representable` warn).
// setZoom in snap mode snaps to the ladder (no error path beyond
// non-finite input); out of snap mode it is the M2-CAM-01 clamp.
//
// The stopped state (the failed-create / default form, the Camera
// precedent): valid() false, matrix() the identity matrix, the
// mutators InvalidArgument with no log, update() a no-op.
//
// ---------------------------------------------------------------------------
// Presentation-only (ARCH-009) and determinism
// ---------------------------------------------------------------------------
//
// The IsoCamera reads nothing from and writes nothing to sim state:
// the follow target is supplied by game code from the presentation
// state (M1-LOOP-02's read-only boundary), and the camera never touches
// the world. Its state is presentation state — never part of replay
// state or the simulation state hash (ARCH-009). AC-4.2: nothing in
// this header enters the simulation (the include-graph lint R2).
//
// Determinism scope: given the same input sequence (the same create
// options and the same setter/update calls in the same order), the
// same build on the same platform produces bit-identical camera state
// — every operation is a fixed sequence of float ops (render-side
// float, NOT SimMath — the pinned-math contract of PRD §10.3 does not
// apply; no RNG, no clock).
//
// ---------------------------------------------------------------------------
// Performance (RENDER-002/003, PERF-003; DOC-004)
// ---------------------------------------------------------------------------
//
// Every operation is O(1) float arithmetic (the snapZoomLevel ladder
// walk is O(log2(zoomMax/zoomMin)) doublings — bounded, ~7 for the
// default [0.1, 16] range; setZoom is an input event, not per-frame).
// No allocation on any path (a plain value object — structurally
// zero-heap, the M2-CAM-01 pattern). No GL calls, no logging on the
// healthy paths (the update/matrix builds log nothing); disabled
// logging costs one atomic load (the facade gate). Budget: the
// matrix build is per-frame-change work in the set-up phase (the
// M2-CAM-01 convention); the snap in update() adds two divisions, two
// floor/ceil, and two multiplies per frame in snap mode.
//
// ---------------------------------------------------------------------------
// Ownership and threading
// ---------------------------------------------------------------------------
//
// A plain value object (the CameraOptions pattern, CONC-001): copy is
// a plain value copy; the owner is the render set-up phase (the frame
// pipeline's phases serialize access — do NOT share the camera across
// threads). No internal synchronization, no resources, nothing to
// release.

#pragma once

#include <glm/glm.hpp>

#include "laige/errors.h"
#include "laige/render/camera.h"
#include "laige/render/matrices.h"
#include "laige/result.h"

namespace laige::render {

// The sqrt(2) factor of the grid-snap margin bound (g*sqrt(2)/2: the
// snap's worst-case Euclidean movement, g/2 per ground axis) and of
// the zoom-level midpoint (level*sqrt(2), the geometric mean of two
// adjacent dyadic levels). The float literal closest to sqrt(2).
inline constexpr float kIsoSnapSqrtTwo = 1.4142135623730951f;
// The snap margin per grid cell (CORE-005): the worst-case distance the
// snap moves the position, in world units per unit of gridSize.
inline constexpr float kIsoSnapMarginPerCell = kIsoSnapSqrtTwo * 0.5f;
// The minimum grid size (world units): below it the snap of a position
// at the documented world-domain edge (32767 units) can exceed the
// float-integer representable range of the snap arithmetic (CORE-005).
inline constexpr float kIsoSnapMinGridSize = 1e-6f;

// The isometric preset selector (ADR 0005; the single config value of
// FR-2.4 / FR-2.5). The engine default is Dimetric2To1 (ADR 0005).
enum class IsoPresetKind {
  Dimetric2To1,  // 2:1 dimetric — the template default (ADR 0005)
  TrueIso3060,   // true 30°/60° isometric
  CustomShear,   // arbitrary shear: the axes must pass isoShearSupported()
};

// The single preset config value (API-006: one struct, no positional
// booleans). For Dimetric2To1 / TrueIso3060 only `scale` is read
// (the axes come from the M2-GL-03 builders at creation); for
// CustomShear only `axes` is read.
struct IsoPreset {
  IsoPresetKind kind{IsoPresetKind::Dimetric2To1};  // ADR 0005 default
  float scale{1.0f};  // the built-in presets' NDC scale (finite, > 0)
  IsoAxes axes{};  // CustomShear only (the matrices.h IsoAxes)
};

// The grid-snap camera mode (FR-2.4). Off by default — the plain
// M2-CAM-01 camera behavior (free position, clamped continuous zoom).
struct GridSnapOptions {
  bool enabled{false};
  // World units per grid cell (finite, > 0, >= kIsoSnapMinGridSize).
  float gridSize{1.0f};
};

// The scene-level isometric camera configuration (API-006): the
// M2-CAM-01 camera options + the preset + the snap mode. A plain
// value — no ownership, nothing to release.
struct IsoCameraOptions {
  CameraOptions camera{};
  IsoPreset preset{};
  GridSnapOptions snap{};
};

// The isometric camera (FR-2.4): the M2-CAM-01 Camera (position,
// follow, bounds, shake, zoom) + the preset matrix build + the grid-
// snap mode. The header preamble is the full contract.
class IsoCamera {
 public:
  // The stopped state (the failed-create / default form): valid()
  // false, matrix() the identity, the mutators InvalidArgument with
  // no log, update() a no-op.
  IsoCamera() noexcept = default;
  // Value semantics: plain copies (no resources to own).
  IsoCamera(const IsoCamera&) = default;
  IsoCamera& operator=(const IsoCamera&) = default;

  // Validates the options (the header preamble's failure section) and
  // snaps the initial position into the grid when the snap is enabled.
  // One rate-limited iso_camera/options_invalid warn per failed create
  // (first failure wins; the camera's own failures use the M2-CAM-01
  // `camera/options_invalid` event).
  // @budget O(1); no allocation (one warn string on failure).
  [[nodiscard]] static laige::Result<IsoCamera, laige::ErrorCode>
  create(IsoCameraOptions options) noexcept;

  // True when the camera holds a validated configuration.
  [[nodiscard]] bool valid() const noexcept { return valid_; }

  // The owned M2-CAM-01 camera (const — all mutation goes through the
  // snap-aware IsoCamera mutators below): position/target/up, follow,
  // shake, zoom, bounds.
  [[nodiscard]] const Camera& camera() const noexcept { return camera_; }
  // The selected preset (the config value as validated at creation).
  [[nodiscard]] const IsoPreset& preset() const noexcept { return preset_; }
  // The grid-snap state (false: the plain M2-CAM-01 behavior).
  [[nodiscard]] bool gridSnapEnabled() const noexcept { return snapEnabled_; }
  [[nodiscard]] float gridSize() const noexcept { return gridSize_; }

  // -----------------------------------------------------------------
  // Matrix build (the set-up phase; O(1), no allocation, no GL)
  // -----------------------------------------------------------------

  // The combined world -> NDC affine matrix of the camera's current
  // state (the header preamble's matrix section) — the frame's render
  // matrix (M2-SPRITE-02 consumes it). The identity on a stopped
  // camera. Build it once per frame change, never per sprite.
  // @budget O(1); no allocation.
  [[nodiscard]] Mat4 matrix() const noexcept;

  // -----------------------------------------------------------------
  // Mutation (owner thread; the snap-aware forms)
  // -----------------------------------------------------------------

  // Sets the camera position. (x, y) are clamped into the bounds
  // rectangle (when enabled), then snapped to the grid (when the snap
  // is enabled) — the candidate is validated against the (inflated,
  // snap mode) look-at margin before it is committed: state is
  // unchanged on rejection (InvalidArgument + warn, the header
  // preamble's failure section). z is never snapped.
  // @budget O(1); no allocation.
  [[nodiscard]] laige::Status setPosition(Vec3 position) noexcept;
  // Sets the look-at point (free — the grid locks the camera position,
  // not the look-at point). Validated against the (inflated, snap
  // mode) look-at margin; state unchanged on rejection.
  [[nodiscard]] laige::Status setTarget(Vec3 target) noexcept;

  // Sets the follow target and enables the smooth follow (the M2-CAM-01
  // follow step; update() applies it — in snap mode the follow runs in
  // grid steps: the position is snapped after every follow step).
  // Rejects non-finite input (InvalidArgument + warn).
  [[nodiscard]] laige::Status setFollowTarget(Vec3 target) noexcept;
  // Disables the follow (the camera stops sliding on the next update).
  void stopFollowing() noexcept;

  // Sets the zoom. Snap mode: snapped to the documented dyadic ladder
  // (the header preamble's zoom section — the result is always an
  // exact member of L; below/above the bounds clamp to the bounds).
  // No snap mode: the M2-CAM-01 clamp into [zoomMin, zoomMax].
  // Rejects non-finite input (InvalidArgument + warn).
  // @budget O(1) — the ladder walk is O(log2(zoomMax/zoomMin)).
  [[nodiscard]] laige::Status setZoom(float zoom) noexcept;

  // Adds a bounded shake impulse (the M2-CAM-01 contract). The shake
  // moves the view center (the matrix's translation); its z component
  // does not enter the iso matrix (the header preamble).
  [[nodiscard]] laige::Status applyShake(Vec3 impulse) noexcept;

  // The per-frame update: the M2-CAM-01 update (follow step, bounds
  // clamp, shake decay) + the grid snap of the position (snap mode
  // only — the continuous choice, the header preamble). No-op on a
  // stopped camera. No logging on the healthy paths.
  // @budget O(1) float ops (+ two snap divisions in snap mode); no
  // allocation.
  void update() noexcept;

  // -----------------------------------------------------------------
  // The documented snap functions (pure; the test oracles)
  // -----------------------------------------------------------------

  // The nearest grid multiple of v (the header preamble's position
  // snap): float-only, nearest, ties away from zero, deterministic per
  // build. Precondition: gridSize finite, > 0. @budget O(1).
  [[nodiscard]] static float snapCoord(float v, float gridSize) noexcept;

  // The snapped zoom level of z for the [zoomMin, zoomMax] dyadic
  // ladder (the header preamble's zoom section): clamps into the
  // bounds, nearest level in log2 space, exact float tie to the higher
  // zoom; the result is always an exact member of L. Preconditions:
  // 0 < zoomMin <= zoomMax, finite. @budget O(log2(zoomMax/zoomMin)).
  [[nodiscard]] static float snapZoomLevel(float z, float zoomMin,
                                           float zoomMax) noexcept;

 private:
  // The look-at margin bound for the current configuration: the
  // M2-CAM-01 maxShakeOffset, inflated by the snap's worst-case
  // movement (g*sqrt(2)/2) in snap mode (the header preamble).
  [[nodiscard]] float marginBound() const noexcept;

  bool valid_{false};
  Camera camera_;
  IsoPreset preset_;
  IsoAxes axes_;  // the preset's axes (resolved at create, M2-GL-03)
  bool snapEnabled_{false};
  float gridSize_{1.0f};
};

}  // namespace laige::render
