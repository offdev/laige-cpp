// laige-render projection modes + screen<->world transforms (M2-PROJ-01).
//
// FR-2.5 (projection modes: isometric primary, side-view, top-down/
// oblique, free-cinematic — selectable per scene/view), FR-2.11 (the
// screen<->world transform base of world picking; the isometric grid
// picking lands on top of this in M2-ISO-03), PRD §4 (the projection
// list; isometric is the first-class default, ADR 0005).
//
//   ProjectionMode    iso | side_view | top_down | free_cinematic
//                     (iso is the engine default)
//   Plane             a plane n·p = d (screenToWorld's target)
//   WorldRay          a world-space preimage ray (origin + normalized
//                     direction)
//   ProjectionView    the per-scene/view projection: mode + world->NDC
//                     matrix (+ the plane camera's center for the plane
//                     modes) + the transforms
//
// The view wraps the M2-GL-03 matrix builders (the iso family,
// planeOrtho) and the M2-CAM-01 camera (free_cinematic): the game builds
// the mode's matrix with the documented builder and stores it in the
// view — once per frame change, in the render set-up phase (the
// camera.h budget convention).
//
// ---------------------------------------------------------------------------
// The projection modes (FR-2.5; the per-mode preimage geometry)
// ---------------------------------------------------------------------------
//
// Each mode pairs a matrix builder with the geometry of the preimage of
// a screen point (the header preamble's transforms invert that matrix):
//
//   Iso            The affine oblique iso matrix (M2-GL-03's
//                  isoMatrix / isoDimetric2To1 / isoTrueIso3060 — the
//                  ADR 0005 presets). NDC-z is 0 for every point. The
//                  preimage of a screen point is the FULL LINE
//
//                      origin + t·direction,   t ∈ ℝ,
//
//                  with the origin on the GROUND PLANE (z = 0) and
//                  direction = normalize(r1 × r2) (r1, r2 = the matrix's
//                  screen-x / screen-y rows, world 3D). Every elevation
//                  on the line projects to the same screen point — a
//                  picking plane z = d always meets it exactly once.
//                  Depth sorting uses the M2-ISO-01 depth key: the shear
//                  must pass isoShearSupported() (M2-CAM-02 validates
//                  scene shears); an invertible shear that does not
//                  still transforms (the key order is simply not
//                  guaranteed — the M2-ISO-01 contract).
//
//   SideView       The plane camera planeOrtho(center, right = (1,0,0),
//                  up = (0,0,1)): screen x = world x, screen y = world
//                  HEIGHT (+z). The preimage line runs along world y;
//                  its origin sits on the REFERENCE PLANE through
//                  planeCenter perpendicular to the view direction (the
//                  plane camera's NDC near plane — planeOrtho's contract:
//                  the plane through center lands on NDC z = -1).
//
//   TopDown        The plane camera planeOrtho(center, right = (1,0,0),
//                  up = (0,1,0)): the X/Y ground plane; screen x = world
//                  x, screen y = world y. The preimage line runs along
//                  world z; the origin sits on the reference plane
//                  through planeCenter (typically the ground, z = 0).
//
//   FreeCinematic  A full 3D camera (M2-CAM-01; the viewProjection() of
//                  the camera — ortho or perspective, zoom applied).
//                  The preimage is a true ray from the camera: the
//                  origin on the NDC near plane, the direction away from
//                  the camera (t ≥ 0 for screen points in front of the
//                  camera — always, for finite window points). The
//                  t >= 0 half covers the VISIBLE VOLUME (z_cam in
//                  [-zFar, -zNear]): points between the camera and the
//                  near plane are clipped (never rendered) and cannot
//                  be picked — screenToWorld's t >= 0 constraint
//                  enforces exactly that.
//
// ---------------------------------------------------------------------------
// The transforms (pure, deterministic, O(1))
// ---------------------------------------------------------------------------
//
//   worldToScreen(p2d, depth)   the 2.5D world point (ground plane
//     (p2d.x, p2d.y) + elevation depth, world units; the presentation
//     snapshot's interpolated position, M1-LOOP-02) -> the NDC point
//     (x, y, z). Iso: NDC-z = 0. The plane modes: NDC-z is the depth
//     within the [zNear, zFar] slab (the reference plane is NDC z = -1).
//     FreeCinematic: standard camera NDC-z (the perspective divide,
//     w = -z_cam, matrices.h).
//
//   screenToWorldRay(ndc)      NDC (x, y) -> the mode's preimage ray
//     (WorldRay; the geometry above).
//
//   screenToWorld(ndc, plane)  the preimage line/ray ∩ the plane n·p = d
//     -> Result<Vec3>: the world point on success; InvalidArgument when
//     (a) the plane is (numerically) PARALLEL to the ray —
//     |dot(direction, n)| <= kProjectionParallelEps — or (b)
//     FreeCinematic only: the intersection is OUTSIDE the visible
//     volume, behind the NDC near plane (t < 0 — clipped, unpickable).
//     The affine modes use the full preimage line (t ∈ ℝ): no sign
//     restriction — a point at any elevation on the line projects to
//     the same screen point, and picking planes (ground z = 0, height
//     z = d) must be able to reach every elevation.
//
// ROUND-TRIP (the documented precision): for a mode and a world point p
// on a plane the preimage crosses,
//
//     screenToWorld(worldToScreen(p), that plane) == p
//
// within kProjectionRoundTripTolerance world units for |p| <= 32: the
// round trip is one matrix multiply plus the per-mode inverse (a 2x2
// solve, a 4x4 inverse + two divides, or the two-point construction) in
// float — the error is a handful of ulp at |p| ~ 32. The tests pin 10k
// random points per mode. The canonical planes: Iso / TopDown: any
// z = const plane; SideView: any y = const plane; FreeCinematic: any
// plane the camera ray crosses within the visible volume (z_cam in
// [-zFar, -zNear]).
//
// NDC <-> window pixels (the game's input/output side — the window
// size is a render/platform detail, NOT part of this pure API; the
// documented boundary per RENDER-006):
//
//     px = (ndc.x * 0.5 + 0.5) * width
//     py = (0.5 - ndc.y * 0.5) * height     (window y is down; NDC y is up)
//     ndc.x = px / width * 2 - 1
//     ndc.y = 1 - py / height * 2
//
// ---------------------------------------------------------------------------
// Preconditions and failure behavior
// ---------------------------------------------------------------------------
//
// The matrix must be the output of the mode's documented builder with
// finite entries (a validated Camera for free_cinematic — the Camera
// owns its own validation). The per-call divisors are exactly the
// builders' preconditions (the iso ground map invertible; the plane
// scales nonzero; the cinematic preimage non-degenerate, w != 0): they
// are asserted in debug; in release a violation is undefined behavior —
// the matrices.h house convention (precondition violations are
// programmer errors, not recoverable engine failures). Finite NDC input
// is a precondition of the screen->world transforms, likewise.
//
// The failure paths that DO exist are runtime geometric (screenToWorld:
// a parallel plane, an intersection behind the camera) — returned as
// Result errors, never asserted (API-008, CORE-008: a failed pick is a
// recoverable condition the game handles).
//
// ---------------------------------------------------------------------------
// Presentation-only (ARCH-009), determinism, performance
// ---------------------------------------------------------------------------
//
// The transforms read nothing from and write nothing to sim state: the
// input is render-side float (the presentation snapshot's interpolated
// position — the sim-state -> presentation conversion happens at the
// M1-LOOP-02 boundary, not here). Projection state is never part of
// replay state or the simulation state hash (ARCH-009).
//
// Determinism scope (the camera.h contract): given the same view and
// the same call sequence, the same build on the same platform produces
// bit-identical results — every operation is a fixed sequence of float
// ops (render-side float, NOT SimMath: the pinned-math contract of
// ADR 0002 does not apply; no RNG, no clock). No cross-platform
// bit-exactness is promised.
//
// Performance (PERF-002/003, DOC-004): every operation is O(1) float
// arithmetic — one matrix multiply, a 2x2 solve, one 4x4 inverse (only
// free_cinematic) — with no allocation, no locks, no GL calls, no
// logging on any path. The transforms are per-pick / per-query work
// (input handling, M2-ISO-03 onward), NOT per-sprite work: the batcher's
// per-sprite world -> NDC multiply (M2-SPRITE-02) consumes the stored
// matrix directly — never call worldToScreen per sprite (it exists for
// game-facing queries, and the batcher path must not pay its cost).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - Build the matrix with the mode's documented builder (above) and
//     keep it consistent with the mode field: the per-mode extraction
//     (which rows invert, where the reference plane is) assumes the
//     documented structure.
//   - Set planeCenter for side_view / top_down to the SAME value passed
//     to planeOrtho: the ray origin (and thus the picking) is defined
//     relative to that reference plane.
//   - Rebuild the matrix when the view changes (camera update, zoom,
//     preset switch) — once per frame change, set-up phase.
//   - Do not pass sim (SimMath) coordinates to the transforms: the
//     input is render-side float from the presentation snapshot
//     (ARCH-009).
//   - Do not derive render ordering from these transforms (PRD §4,
//     G-R11): ordering is the M2-ISO-01 depth key, world-space by
//     contract.
//   - screenToWorld is a QUERY, not a per-frame call: one per pick /
//     per UI hover, never in a per-entity loop.

#pragma once

#include "laige/render/matrices.h"  // Vec2/Vec3/Mat4 + the matrix builders
#include "laige/result.h"           // Result/Status (the screenToWorld error)

namespace laige::render {

// The per-scene/view projection mode (FR-2.5). iso is the engine
// default (ADR 0005, PRD v0.2) — the default member of ProjectionView.
enum class ProjectionMode {
  Iso,           // the affine oblique isometric (primary projection)
  SideView,      // the plane camera; screen y = world height (+z)
  TopDown,       // the plane camera; the X/Y ground plane
  FreeCinematic, // the full 3D camera (M2-CAM-01 viewProjection)
};

// A plane in world space, normal·p = d (screenToWorld's intersection
// target). `normal` is any nonzero vector — need not be normalized: the
// intersection is scale-invariant.
struct Plane {
  Vec3 normal{0.0f, 0.0f, 1.0f};
  float d{0.0f};
};

// The world-space preimage ray of a screen point: the point
// origin + t·direction (t ≥ 0; the affine modes use the full line
// through origin along ±direction — the header preamble). `direction`
// is normalized (unit length).
struct WorldRay {
  Vec3 origin{0.0f, 0.0f, 0.0f};
  Vec3 direction{0.0f, 0.0f, 0.0f};
};

// The documented round-trip precision (the header preamble): the bound
// on |screenToWorld(worldToScreen(p)) - p| in world units for |p| <= 32
// (a single-pass float round trip; the tests pin it per mode).
inline constexpr float kProjectionRoundTripTolerance = 1e-3f;

// The |dot(direction, normal)| threshold below which screenToWorld
// treats the plane as parallel to the ray: it covers float rounding of
// an exactly-parallel dot (a cancellation of unit-vector products can
// round to ~1e-7) and rejects the numerically ill-conditioned planes
// more parallel than that (the intersection point is then far beyond
// the representable scene extent).
inline constexpr float kProjectionParallelEps = 1e-6f;

// The per-scene/view projection state (FR-2.5, FR-2.11): the mode
// selects the preimage geometry and the matrix — the output of the
// mode's documented builder (the header preamble) — is the world->NDC
// map. A plain value object (the CameraOptions pattern): the game sets
// the fields, rebuilds the matrix when the view changes, and passes the
// view (or its matrix) to the pickers and the batcher. No state is
// hidden, nothing to own or release; copy is a plain value copy.
struct ProjectionView {
  // The projection mode (default: Iso — the engine default, ADR 0005).
  ProjectionMode mode{ProjectionMode::Iso};
  // The world->NDC matrix: the output of the mode's documented builder
  // (the header preamble; the default identity is the empty state).
  Mat4 matrix{Mat4(1.0f)};
  // side_view / top_down only: the plane camera's center (the SAME
  // value passed to planeOrtho) — the reference plane through it
  // perpendicular to the view direction is where the ray origin sits.
  // Ignored by Iso (the reference is the ground plane z = 0) and by
  // FreeCinematic (the reference is the NDC near plane).
  Vec3 planeCenter{0.0f, 0.0f, 0.0f};

  // The NDC of the 2.5D world point (the ground plane (p2d.x, p2d.y) +
  // elevation depth). Pure (no mutation); no allocation, no logging.
  // @budget O(1): one 4x4 matrix multiply (+ one divide for
  // free_cinematic; w = 1 exactly for the affine modes).
  [[nodiscard]] Vec3 worldToScreen(Vec2 p2d, float depth) const noexcept;

  // The world preimage ray of the NDC screen point (the header
  // preamble's per-mode geometry). Pure; no allocation, no logging.
  // @budget O(1): a 2x2 solve (the affine modes) or one 4x4 inverse +
  // two divides (free_cinematic).
  [[nodiscard]] WorldRay screenToWorldRay(Vec2 ndc) const noexcept;

  // The preimage line/ray ∩ the plane (the header preamble): the world
  // point on success; InvalidArgument when the plane is parallel to
  // the ray or (free_cinematic) the intersection is behind the camera.
  // Pure; no allocation, no logging.
  // @budget O(1): one screenToWorldRay + two dot products + one divide.
  [[nodiscard]] laige::Result<Vec3, laige::ErrorCode>
  screenToWorld(Vec2 ndc, Plane plane) const noexcept;
};

}  // namespace laige::render
