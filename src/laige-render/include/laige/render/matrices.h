// laige-render matrix utilities (M2-GL-03): the rendering-side camera
// and projection matrix builders.
//
// PRD §11: GLM is the math foundation of the *rendering* side (cameras,
// projection matrices — incl. isometric camera matrices); the simulation
// keeps its own fixed-point math (PRD §10.3, SimMath). This header is the
// engine's documented, convention-pinned layer over GLM: every matrix is
// a pure, allocation-free function of its arguments. The include-graph
// lint (tools/laige-include-lint, R3: deps/glm is owned by laige-render)
// is what keeps GLM out of the sim modules — sim code never sees these
// types.
//
//   Vec2 / Vec3 / Mat4    The GLM value types this API uses (aliases)
//   IsoAxes               The custom-shear preset's axis screen-deltas
//   ortho()               Orthographic projection (camera -> NDC)
//   perspective()         Perspective projection (camera -> NDC)
//   lookAt()              View matrix (world -> camera space)
//   planeOrtho()          2D-plane camera: the combined view+projection
//                         of an orthographic camera looking straight at
//                         a plane (top_down / side_view modes)
//   isoMatrix()           The general affine oblique isometric matrix
//                         (the "custom shear" preset)
//   isoDimetric2To1()     2:1 dimetric preset (ADR 0005 — the default)
//   isoTrueIso3060()      True 30°/60° isometric preset
//
// ---------------------------------------------------------------------------
// Conventions (pinned here; the canonical home is
// docs/concepts/coordinates.md, created with M2-ISO-01)
// ---------------------------------------------------------------------------
//
// World space: right-handed; (x, y) is the ground plane, +z is
// height/up (the sim_math.h 2.5D convention — the simulation is
// axis-aligned 2D plus a depth/height value, PRD §4).
//
// NDC (OpenGL): x right, y UP, z in [-1, +1] with the near plane at
// z = -1 and the far plane at z = +1. A camera looks along its own -z
// axis (camera space: x right, y up, -z forward).
//
// All matrices are column-major GLM matrices (Mat4 m stores columns
// m[0..3], each a Vec4; element m[c][r] is row r of column c). View
// matrices map world -> camera space; projection matrices map camera
// -> NDC; the iso matrices and planeOrtho() return the COMBINED
// world -> NDC affine matrix in one step (affine oblique projections
// have no separate meaningful view/projection split).
//
// Presentation-only (ARCH-009): these matrices never read or write sim
// state; they are built once per scene/camera update and used to map
// presentation positions to the screen.
//
// ---------------------------------------------------------------------------
// The isometric presets (ADR 0005; FR-2.5)
// ---------------------------------------------------------------------------
//
// All three presets are affine oblique projections of the form
//
//   screen_x = dx.x*x + dy.x*y
//   screen_y = dx.y*x + dy.y*y + zUnit*z
//   screen_z = 0
//
// with both ground axes projecting DOWNWARD on screen (y down in the
// ADR's pixel convention, i.e. negative NDC-y here) and a positive
// (x + y) screen-y contribution, so the axis-aligned depth key
// f(x + y, height, layer) (FR-2.2; M2-ISO-01) sorts back-to-front for
// every preset and the O(1) picking inverse (FR-2.11; M2-ISO-03)
// exists whenever the ground 2x2 is invertible. NDC-z is 0 for every
// point: the 2.5D depth is engine-owned (FR-2.2 depth keys), never
// derived from the projection (PRD §4).
//
//   Preset           dx            dy            zUnit   geometry
//   ---------------  -------------  -------------  ------  ----------------------
//   2:1 dimetric     ( 2k, -k)      (-2k, -k)        k     affine, 45° azimuth,
//   (default)                                                     vertical squashed to
//                                                               1/2 (ADR 0005);
//                                                               per-step delta
//                                                               (±2, 1)·k — integer
//                                                               at k = 1
//   true 30°/60°     ( s, -s/√3)    (-s, -s/√3)  s/√3    true orthographic
//                                                               axonometric: 45°
//                                                               azimuth, elevation
//                                                               arcsin(1/√3) ≈
//                                                               35.264°; ground
//                                                               axes at 30° to
//                                                               horizontal; all
//                                                               three axes
//                                                               equally
//                                                               foreshortened
//   custom shear     user-supplied  user-supplied  user-      any invertible
//                                                            supplied          oblique view of
//                                                                              the ground plane
//
// The 2:1 tile at scale k is a 4k-wide by 2k-tall rhombus (the 2:1
// pixel-art ratio) and one world height unit projects to k screen
// units (half the tile height — the ADR 0005 "vertical squashed to ½").
// The true-iso tile is 2s wide by 2s/√3 tall (the √3:1 "30°/60°" ratio).
//
// ---------------------------------------------------------------------------
// Preconditions and failure behavior
// ---------------------------------------------------------------------------
//
// Every builder documents its precondition set; a precondition violation
// is a PROGRAMMER error (misconfigured camera data), not a recoverable
// engine failure: debug builds assert, and release builds treat the
// input as undefined (the engine's Result/Status convention for
// precondition violations — cf. SimMath::clamp in laige/sim_math.h).
// The functions themselves are total over finite inputs that meet their
// preconditions; none returns an error, none logs, none allocates.
//
// ---------------------------------------------------------------------------
// Performance (PERF-001/003)
// ---------------------------------------------------------------------------
//
// Every builder is O(1) arithmetic (a few dozen flops), no allocation,
// no locks, no GL calls, no logging; callable from any thread at any
// phase (pure). They are set-up-phase work (built once per scene or
// camera change — the M2-CAM-01/02 camera objects own that cadence);
// they are never called per sprite.

#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace laige::render {

// The GLM value types of the rendering-side math API (PRD §11). GLM is
// pinned in deps.lock (ADR 0008) and exposed through this header only —
// the include-graph lint (R3) forbids every other module from including
// it, so the sim modules' SimMath (PRD §10.3) stays the sim-side math.
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Mat4 = glm::mat4;

// The custom-shear isometric preset (ADR 0005: "the two axis
// screen-deltas"): the NDC (y up) delta of one world +x ground step
// (dx), one world +y ground step (dy), and one world +z (height) step
// (zUnit). isoMatrix() builds the matrix; the preset is invertible for
// picking (M2-ISO-03) iff det(dx.x*dy.y - dx.y*dy.x) != 0.
struct IsoAxes {
  Vec2 dx{0.0f, 0.0f};
  Vec2 dy{0.0f, 0.0f};
  float zUnit{0.0f};
};

// Orthographic projection (camera space -> NDC).
//
// Maps the box [left, right] x [bottom, top] with camera-space z in
// [-zFar, -zNear] (the camera looks along -z, so the near plane sits at
// z_cam = -zNear and the far plane at z_cam = -zFar) to the NDC cube
// [-1, 1]^3: (left, bottom, -zNear) -> (-1, -1, -1) and
// (right, top, -zFar) -> (1, 1, 1). Column-major elements (m[c][r]):
//
//   m[0][0] =  2 / (right - left)        m[1][1] =  2 / (top - bottom)
//   m[2][2] = -2 / (zFar - zNear)
//   m[3][0] = -(right + left) / (right - left)
//   m[3][1] = -(top + bottom) / (top - bottom)
//   m[3][2] = -(zFar + zNear) / (zFar - zNear)
//   m[3][3] = 1            (all other elements 0)
//
// Precondition: left < right, bottom < top, zNear < zFar, all finite.
// (zNear may be <= 0 for orthographic slabs; unlike perspective() there
// is no focus point.)
// @budget O(1); no allocation; a dozen flops.
[[nodiscard]] Mat4 ortho(float left, float right, float bottom, float top,
                         float zNear, float zFar) noexcept;

// Perspective projection (camera space -> NDC), vertical field of view.
//
// Maps the frustum (fovY about the -z axis, aspect = width/height) to
// NDC with the near plane at z = -1 and the far plane at z = +1.
// Column-major elements (m[c][r]):
//
//   t = 1 / tan(fovY / 2)
//   m[0][0] = t / aspect
//   m[1][1] = t
//   m[2][2] = -(zFar + zNear) / (zFar - zNear)
//   m[3][2] = -2*zFar*zNear / (zFar - zNear)
//   m[2][3] = -1           (all other elements 0; w = -z_cam)
//
// NDC = (M * (p, 1)) / w with w = -z_cam (the perspective divide). A
// point on the near plane's corner (aspect*t*zNear, t*zNear, -zNear)
// maps to NDC (1, 1, -1); the far plane's corner (aspect*t*zFar,
// t*zFar, -zFar) maps to (1, 1, 1) — the frustum sides touch the NDC
// cube edges at every depth.
// Precondition: 0 < fovY < pi, aspect > 0, 0 < zNear < zFar, all finite.
// @budget O(1); no allocation; a dozen flops + one tan.
[[nodiscard]] Mat4 perspective(float fovY, float aspect, float zNear,
                               float zFar) noexcept;

// View matrix (world -> camera space) of a camera at `eye` looking at
// `center` with world up vector `up` (the classic look-at).
//
// With f = normalize(center - eye), s = normalize(cross(f, up)),
// u = cross(s, f) (right-handed: s x u = -f, the camera looks along
// -f), the matrix's rows are:
//
//   row 0 = s,  row 1 = u,  row 2 = -f
//   translation column = (-s*eye, -u*eye, f*eye, 1)
//
// so a world point p maps to camera space as (s, u, -f) dot (p - eye).
// Precondition: eye != center, `up` not parallel to (center - eye),
// all finite.
// @budget O(1); no allocation; two normalizations + one cross.
[[nodiscard]] Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) noexcept;

// 2D-plane camera: the COMBINED view+projection matrix of an
// orthographic camera looking straight at the plane spanned by `right`
// and `up` through `center` (the top_down and side_view projection
// modes, M2-PROJ-01).
//
// The plane normal is n = normalize(cross(right, up)) — the
// toward-the-viewer side. The camera sits at center + n*zNear and looks
// along -n; world `right` maps to NDC +x and world `up` to NDC +y.
// The plane through `center` lands exactly on the NDC near plane
// (z = -1), and the visible depth slab is [zNear, zFar] measured from
// the plane toward the viewer.
//
// Returns ortho(-halfWidth, +halfWidth, -halfHeight, +halfHeight,
// zNear, zFar) * view, where the view matrix's columns are
// (right, up, n, (-right*center, -up*center, -n*center - zNear, 1)).
//
// Precondition: `right` and `up` orthonormal (unit and perpendicular),
// halfWidth > 0, halfHeight > 0, zNear < zFar, all finite.
// Examples (M2-PROJ-01): top_down — center = (ox, oy, 0), right =
// (1,0,0), up = (0,1,0); side_view — center = (ox, oy, oz), right =
// (1,0,0), up = (0,0,1) (screen y = world height).
// @budget O(1); no allocation; one mat4 multiply.
[[nodiscard]] Mat4 planeOrtho(Vec3 center, Vec3 right, Vec3 up,
                              float halfWidth, float halfHeight,
                              float zNear, float zFar) noexcept;

// The general affine oblique isometric matrix (world -> NDC) — the
// "custom shear" preset (ADR 0005). See the header preamble for the
// screen-space form:
//
//   screen_x = axes.dx.x*x + axes.dy.x*y
//   screen_y = axes.dx.y*x + axes.dy.y*y + axes.zUnit*z
//   screen_z = 0
//
// Row-major form (column-major elements m[c][r]):
//
//   row 0 = (axes.dx.x, axes.dy.x, 0, 0)
//   row 1 = (axes.dx.y, axes.dy.y, axes.zUnit, 0)
//   row 2 = (0, 0, 0, 0)     — the 2.5D depth is engine-owned (FR-2.2
//                              depth keys), never from the projection
//   row 3 = (0, 0, 0, 1)
//
// Precondition: all finite, and det(axes.dx.x*axes.dy.y -
// axes.dx.y*axes.dy.x) != 0 (the ground-plane map must be invertible —
// the M2-ISO-03 picking inverse).
// @budget O(1); no allocation.
[[nodiscard]] Mat4 isoMatrix(IsoAxes axes) noexcept;

// The 2:1 dimetric preset (ADR 0005 — the engine default): affine,
// 45° azimuth, the two ground axes equally foreshortened, the vertical
// squashed to 1/2. Per-unit ground step screen delta (±2, 1)·scale
// (NDC, y up); one world tile maps to a 4*scale-wide by 2*scale-tall
// rhombus; one world height unit maps to `scale` screen units (half
// the tile height).
//
// Equals isoMatrix({dx = (2k, -k), dy = (-2k, -k), zUnit = k}) with
// k = `scale`.
// Precondition: scale > 0, finite.
// @budget O(1); no allocation.
[[nodiscard]] Mat4 isoDimetric2To1(float scale) noexcept;

// The true 30°/60° isometric preset: a true orthographic axonometric
// projection at 45° azimuth and elevation arcsin(1/√3) ≈ 35.264° —
// all three axes equally foreshortened, the ground axes at 30° to
// screen horizontal (hence "30°/60°": the tile's edges sit at 30° and
// 60° to horizontal), the tile a √3:1 rhombus.
//
// Equals isoMatrix({dx = (s, -s/√3), dy = (-s, -s/√3), zUnit = s/√3})
// with s = `scale` (1/√3 = 1/sqrt(3), the inverse of the ground-axis
// foreshortening of a unit diagonal step).
// Precondition: scale > 0, finite.
// @budget O(1); no allocation; one sqrt.
[[nodiscard]] Mat4 isoTrueIso3060(float scale) noexcept;

}  // namespace laige::render
