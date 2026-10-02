// laige-render isometric grid picking (M2-ISO-03): the engine-owned,
// safe screen -> ground-plane -> grid-cell transform.
//
// FR-2.11 (roadmap/M2-rendering-2.5d.md, M2-ISO-03 scope): the
// isometric picking is O(1), exact at all supported zoom levels, and
// returns an integer grid cell — the engine owns the inverse (S-5,
// G-R11: game code never writes its own screen->grid / z-ordering
// math). M2-PROJ-01: the screen<->world transforms are the base this
// step lands on (projection.h: "the isometric grid picking lands on
// top of this in M2-ISO-03"). PRD §4: click-to-select / click-to-
// move is a first-class isometric need.
//
//   IsoGridConfig    the pick grid: the cell size in world units
//   IsoGridPick      the pick result: the grid cell + the ground point
//   screenToGrid()   the O(1) inverse: NDC screen -> ground -> cell,
//                    for the M2-CAM-02 IsoCamera or an Iso
//                    ProjectionView (the M2-PROJ-01 base)
//
// ---------------------------------------------------------------------------
// The inverse (O(1), FR-2.11)
// ---------------------------------------------------------------------------
//
// The frame's render matrix M (the IsoCamera::matrix() of the camera's
// CURRENT state — the M2-CAM-02 matrix section — or an Iso
// ProjectionView's matrix) maps a ground-plane point (x, y, 0) to
// NDC:
//
//   ndc.x = a*x + b*y + tx        a = m[0][0], b = m[1][0], tx = m[3][0]
//   ndc.y = c*x + d*y + ty        c = m[0][1], d = m[1][1], ty = m[3][1]
//
// (column-major m[c][r]; row 2 is 0 and row 3 = (0, 0, 0, 1) for every
// iso matrix — the matrices.h contract). The inverse is the 2x2 solve
// — one pick is a few flops: no per-pick 4x4 inverse, no GLM calls,
// no allocation:
//
//   det = a*d - b*c                          (the ground map's det)
//   w.x = (d*(ndc.x - tx) - b*(ndc.y - ty)) / det
//   w.y = (a*(ndc.y - ty) - c*(ndc.x - tx)) / det
//
// det != 0 is the isoMatrix / IsoCamera precondition (the ground map
// is invertible — matrices.h). The solve is the exact inverse of the
// STORED float matrix: the pick resolves to the cell of the
// projection the frame actually renders with — never a different,
// "ideal" matrix.
//
// The grid cell (the documented BOUNDARY RULE, FR-2.11): cell (gx, gy)
// is the half-open square
//
//   [gx*g, (gx+1)*g) x [gy*g, (gy+1)*g)
//
// i.e. gx = floor(w.x / g), gy = floor(w.y / g). A point exactly on a
// cell's lower or left boundary belongs to THAT cell; a point exactly
// on its upper or right boundary belongs to the cell beyond it (the
// floor convention); the corner of four cells belongs to the cell to
// its upper right. The grid is anchored at the world origin: the tile
// map's tile (gx, gy) (M2-TILE-01) is exactly this cell at g = 1, with
// center (gx + 0.5, gy + 0.5) — the grid the M2-CAM-02 grid-snap
// camera locks to.
//
// EXACT AT ALL SUPPORTED ZOOM LEVELS: the inverse is a fixed sequence
// of float ops with no zoom-dependent branch or table — zoom enters
// only through M's entries (the M2-CAM-02 matrix build), so the pick
// of a stored screen point resolves to that point's exact cell at
// every zoom the camera supports (the continuous [zoomMin, zoomMax]
// range without snap, the dyadic ladder with snap — both are
// "supported zoom" here). The goldens pin 4 zoom levels by hand.
//
// Precision (the documented BOUNDARY ZONE): the computed ground point
// w-hat satisfies
//
//   |w-hat - w| <= kIsoPickOpFactor * kIsoPickUlp * kappa * (|e| + |w|)
//
// (world units, per axis), where w is the exact ground point of the
// exact inverse, e is the camera's ground point
// (effectivePosition() = position + shakeOffset), and kappa is the
// infinity-norm condition number of the UNSCALED ground 2x2 [[dx.x,
// dy.x], [dx.y, dy.y]] (the built-in presets: kappa = 4.5 for 2:1
// dimetric, ~2.73 for true 30/60 — scale-invariant, so it is a
// property of the preset, not of the zoom). kIsoPickOpFactor = 16
// covers the 10 rounding ops of the fixed sequence (2x2 solve + cell
// divisions) with margin over the backward-error bound 4*kappa*u*
// (|w| + |w - e|). Consequence (the boundary guarantee): the pick
// returns the exact cell of the exact inverse UNLESS the exact ground
// point sits within that bound of a cell boundary; inside that zone
// either of the two adjacent cells may be returned (the float
// rounding decides — deterministic per build, never a random flip
// between frames of the same stored matrix). The zone is ~6e-4 world
// units at scene scale (|e|, |w| <= 64.5, a built-in preset) and
// kIsoPickDomainBoundaryEps world units in the documented domain
// worst case (kappa <= kIsoPickMaxCondition = 64 — 14x the built-ins
// — and |e| = |w| = 32767). A custom shear with kappa > 64 still
// picks (the inverse is total over every invertible shear) but its
// zone scales linearly in kappa.
//
// ---------------------------------------------------------------------------
// Total function, domain, and saturation (CORE-008)
// ---------------------------------------------------------------------------
//
// screenToGrid is TOTAL: it returns a pick for EVERY input — no error
// path, no per-call assert (a per-click pick is called from input
// handling with untrusted data; a failed click must not be an engine
// failure the game handles — the isoDepthKey total-function
// precedent):
//
//   - non-finite screen components propagate through the fixed float
//     sequence and the ground point saturates at the documented world
//     domain (kIsoDepthMaxWorldUnits = 32767, iso_depth_key.h):
//     +inf -> +bound, -inf and NaN -> -bound (IEEE: NaN compares
//     false — the isoDepthKey saturation convention);
//   - the result cell is then within |gx|, |gy| <= 32767/g — inside
//     int32 for every valid g (g >= kIsoPickMinCellSize:
//     32767 / 1e-4 = 3.3e8 < 2^31 - 1).
//
// The grid config is a config value validated by the game at
// construction (the preset-scale precedent): finite, > 0, and
// >= kIsoPickMinCellSize is a precondition (the matrices.h house
// convention: a violation is a programmer error — debug assert,
// release undefined behavior).
//
// A STOPPED (invalid) IsoCamera picks with the identity matrix (the
// M2-CAM-02 stopped-state contract: matrix() is the identity):
// cell = floor(screen / g) — degenerate but total; the game checks
// valid() before relying on the pick.
//
// ---------------------------------------------------------------------------
// Safe API (S-5, G-R11)
// ---------------------------------------------------------------------------
//
// This is the engine-owned screen->grid path: game code passes a
// screen point (NDC — the documented pixel <-> NDC conversion of
// projection.h is the input boundary, RENDER-006) and gets the grid
// cell; it never inverts the iso matrix itself. The result's `ground`
// is the computed ground-plane point (world units) for proximity
// queries and diagnostics; the cell is the pick.
//
// The grid PLANE is the ground plane (z = 0): a click on raised
// terrain picks the cell of its ground projection (the standard
// isometric click-to-select semantics). The tile-height-aware pick
// (pick the standing cell of the clicked tile) lands with the tile
// map (M2-TILE-01/02), which consumes this inverse.
//
// ---------------------------------------------------------------------------
// Determinism, threading, performance (PERF-002/003, DOC-004)
// ---------------------------------------------------------------------------
//
// Pure function of (screen, the camera's current matrix, grid):
// deterministic per build — a fixed sequence of float ops (render-
// side float, NOT SimMath — the pinned-math contract of ADR 0002
// does not apply; no RNG, no clock). Presentation-only (ARCH-009):
// reads no sim state, writes nothing; never part of replay state or
// the simulation state hash.
//
// O(1): 10 rounding ops + 2 exact floorfs + the camera matrix build
// (O(1) itself — the pick is a per-click / per-query call in input
// handling, NEVER per sprite or per frame). No allocation, no
// logging, no GL calls — disabled cost is zero. Budget: the
// `iso_picking` entry of budgets.json (one pick, mean <= 0.01 ms,
// PRD §8.1) — the gated budget suite
// (tests/laige-render/iso_picking_tests.cpp) records the baseline in
// docs/benchmarks/baselines/m2-iso-picking.md.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - The screen is NDC (projection.h), not window pixels: convert
//     with the documented one-liner first (RENDER-006 — the window is
//     a platform detail).
//   - The camera's matrix() is rebuilt on the pick (O(1)): it always
//     reflects the camera's CURRENT state — do not cache the matrix
//     across camera changes and pick with the stale one.
//   - Do not pick through a stopped camera (check valid() first) or a
//     non-Iso ProjectionView (the overload's precondition).
//   - The cell is on the GROUND plane (z = 0): for raised terrain the
//     cell is the ground projection — the tile map step documents the
//     height-aware variant.

#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>

#include "laige/render/iso_camera.h"    // IsoCamera (M2-CAM-02)
#include "laige/render/iso_depth_key.h"  // kIsoDepthMaxWorldUnits (M2-ISO-01)
#include "laige/render/projection.h"     // ProjectionView (M2-PROJ-01)

namespace laige::render {

// The float rounding unit (2^-24): the per-op relative error bound of
// one IEEE-754 binary32 op (CORE-005; the precision formula below).
inline constexpr float kIsoPickUlp = 5.9604644775390625e-8f;
// The rounding-op count of the picking inverse's fixed sequence (10
// rounding ops: the 2x2 solve + the two cell divisions) plus margin
// (CORE-005; the precision formula below).
inline constexpr float kIsoPickOpFactor = 16.0f;
// The precision contract's condition-number bound (the header
// preamble): the built-in presets are kappa = 4.5 (2:1) / ~2.73
// (30/60); a custom shear with kappa <= 64 keeps the documented
// boundary zone, and beyond it the zone scales linearly in kappa.
inline constexpr float kIsoPickMaxCondition = 64.0f;
// The domain-worst boundary ambiguity zone (the header preamble):
// kIsoPickOpFactor * kIsoPickUlp * kIsoPickMaxCondition *
// (2 * kIsoDepthMaxWorldUnits) world units (~4.000061).
inline constexpr float kIsoPickDomainBoundaryEps =
    kIsoPickOpFactor * kIsoPickUlp * kIsoPickMaxCondition *
    (2.0f * static_cast<float>(kIsoDepthMaxWorldUnits));
// The minimum grid cell size (world units): below it the saturated
// cell index 32767/g can leave int32 (CORE-005: 32767/1e-4 = 3.3e8 <
// 2^31 - 1 with 6.5x margin).
inline constexpr float kIsoPickMinCellSize = 1e-4f;

// The pick grid (FR-2.11's grid_config): the cell size in world units
// (finite, > 0, >= kIsoPickMinCellSize — a config precondition, the
// header preamble). 1.0 world unit per cell is the tile map's grid
// (M2-TILE-01).
struct IsoGridConfig {
  float cellSize{1.0f};
};

// The pick result (the safe API — S-5/G-R11): the grid cell (the
// floor convention, the header preamble) + the computed ground-plane
// point (world units; the diagnostics / proximity-query view of the
// pick). A plain value — no ownership, nothing to release.
struct IsoGridPick {
  std::int32_t cellX{};  // the cell's x index (floor(w.x / cellSize))
  std::int32_t cellY{};  // the cell's y index (floor(w.y / cellSize))
  Vec2 ground{};         // the (saturated) ground-plane world point
};

namespace detail {

// The 2x2 ground-plane inverse of the stored iso matrix (the header
// preamble's formula): the world (x, y) on z = 0 that projects to the
// NDC screen point. Non-finite inputs propagate to non-finite output
// (the caller saturates — the total-function contract). Precondition:
// the matrix is a valid iso matrix (det != 0 — the isoMatrix /
// IsoCamera contract).
[[nodiscard]] inline Vec2 groundInverse(Vec2 screen, const Mat4& m)
    noexcept {
  const float a = m[0][0], b = m[1][0], c = m[0][1], d = m[1][1];
  const float sx = screen.x - m[3][0];
  const float sy = screen.y - m[3][1];
  const float det = a * d - b * c;
  return Vec2{(d * sx - b * sy) / det, (a * sy - c * sx) / det};
}

// The domain saturation (the isoDepthKey total-function convention):
// +inf -> +bound, -inf and NaN -> -bound (IEEE: NaN compares false in
// `v > 0.0f`). Finite values pass through unchanged.
inline float saturateWorldCoord(float v) noexcept {
  if (std::isfinite(v)) return v;
  return (v > 0.0f) ? static_cast<float>(kIsoDepthMaxWorldUnits)
                    : -static_cast<float>(kIsoDepthMaxWorldUnits);
}

// The pick from a stored iso matrix: the 2x2 inverse + the domain
// saturation + the half-open cell (the header preamble). The ::floorf
// global forms (not std::floorf): some CI g++ toolchains expose the
// float versions only in the global namespace (the iso_camera.cpp
// precedent — PR #67 CI run 37006646480, linux-gcc lane).
[[nodiscard]] inline IsoGridPick pickFromMatrix(Vec2 screen, const Mat4& m,
                                               float cellSize) noexcept {
  Vec2 w = groundInverse(screen, m);
  const float wx = saturateWorldCoord(w.x);
  const float wy = saturateWorldCoord(w.y);
  return IsoGridPick{static_cast<std::int32_t>(::floorf(wx / cellSize)),
                     static_cast<std::int32_t>(::floorf(wy / cellSize)),
                     Vec2{wx, wy}};
}

}  // namespace detail

// The safe isometric pick (FR-2.11; the header preamble is the full
// contract): the NDC screen point -> the ground plane (z = 0) -> the
// grid cell. O(1) (the 2x2 solve + two divisions + the camera's O(1)
// matrix build); total for every input (the header preamble's
// saturation section); no allocation, no logging, no GL.
// @budget O(1); no allocation.
[[nodiscard]] inline IsoGridPick screenToGrid(Vec2 screen,
                                             const IsoCamera& camera,
                                             const IsoGridConfig& grid)
    noexcept {
  const float g = grid.cellSize;
  assert(std::isfinite(g) && g > 0.0f && g >= kIsoPickMinCellSize &&
         "IsoGridConfig: cellSize must be finite, > 0, and >= "
         "kIsoPickMinCellSize");
  return detail::pickFromMatrix(screen, camera.matrix(), g);
}

// The ProjectionView overload (the M2-PROJ-01 base this step lands on
// — the header preamble): the same inverse over a hand-stored iso
// matrix (advanced use: e.g. a custom shear through the raw
// isoMatrix builder, M2-CAM-02). Precondition: view.mode ==
// ProjectionMode::Iso (the per-mode structure assumption — the
// matrices.h house convention: debug assert, release undefined).
// @budget O(1); no allocation.
[[nodiscard]] inline IsoGridPick screenToGrid(Vec2 screen,
                                             const ProjectionView& view,
                                             const IsoGridConfig& grid)
    noexcept {
  assert(view.mode == ProjectionMode::Iso &&
         "screenToGrid(ProjectionView): the view must be in Iso mode");
  const float g = grid.cellSize;
  assert(std::isfinite(g) && g > 0.0f && g >= kIsoPickMinCellSize &&
         "IsoGridConfig: cellSize must be finite, > 0, and >= "
         "kIsoPickMinCellSize");
  return detail::pickFromMatrix(screen, view.matrix, g);
}

}  // namespace laige::render
