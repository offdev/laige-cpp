# Isometric grid picking (`laige::render::screenToGrid`)

The engine-owned, safe screen → ground-plane → grid-cell transform
(M2-ISO-03; FR-2.11, PRD §4 click-to-select/move; AGENTS ARCH-008/
009, API-001/008, CORE-005/008, PERF-003, DOC-004, S-5/G-R11). Public
header: `src/laige-render/include/laige/render/iso_picking.h`
(header-only — the inverse is a fixed sequence of float ops; there is
no implementation file). Unit suite: `ctest -R iso_picking`
(`tests/laige-render/iso_picking_tests.cpp`) — pure float math, no GL
environment required: it runs in every local tree and in CI, with
hand-computed golden cells at 4 zoom levels, the 10k-cell × 4-zoom
round-trip property, the boundary/precision pinning, the total-
function saturation contract, and the PRD §8.1 one-pick budget gate.

The picking is the M2-PROJ-01 screen↔world base applied to the
isometric grid: it inverts the frame's **stored** world → NDC matrix
(the M2-CAM-02 `IsoCamera::matrix()` or an Iso `ProjectionView`
matrix) on the ground plane (z = 0) and maps the ground point to its
grid cell. Game code never inverts the iso matrix itself (S-5, G-R11:
the engine owns the picking math, just as it owns the depth keys —
PRD §4).

## The API

```cpp
struct IsoGridConfig { float cellSize{1.0f}; };  // world units per cell

struct IsoGridPick {
  std::int32_t cellX{};  // the cell's x index (floor convention)
  std::int32_t cellY{};  // the cell's y index (floor convention)
  Vec2 ground{};         // the (saturated) ground-plane world point
};

// The NDC screen point -> the ground plane (z = 0) -> the grid cell.
[[nodiscard]] IsoGridPick screenToGrid(Vec2 screen,
                                       const IsoCamera& camera,
                                       const IsoGridConfig& grid);
// The same inverse over a hand-stored iso matrix (advanced use).
// Precondition: view.mode == ProjectionMode::Iso.
[[nodiscard]] IsoGridPick screenToGrid(Vec2 screen,
                                       const ProjectionView& view,
                                       const IsoGridConfig& grid);
```

- **`screen`** is the screen point in **NDC** (the projection.h
  convention — NDC x right, y up, `[-1, 1]²`). The window pixel → NDC
  conversion is the game's input boundary (RENDER-006: the window is a
  platform detail): `ndc = (px / W * 2 - 1, 1 - py / H * 2)`.
- **`camera`** is the M2-CAM-02 `IsoCamera` in its CURRENT state
  (position, zoom, shake — the camera's `matrix()` is rebuilt on the
  pick, O(1), so the pick always uses the current frame matrix).
- **`grid`** is the pick grid: `cellSize` in world units (the tile
  map's grid is `cellSize = 1`). A valid config is finite, `> 0`, and
  `>= kIsoPickMinCellSize` (1e-4) — a **config precondition** (the
  matrices.h house convention: a violation is a programmer error —
  debug assert, release undefined behavior), not a runtime error.
- **The result**: `cellX`/`cellY` are the grid cell indices; `ground`
  is the computed ground-plane world point (world units) — the
  diagnostics / proximity-query view of the pick.

## The inverse (O(1), FR-2.11)

For the frame matrix `M` (column-major `m[c][r]`; rows 0/1 are the
NDC x/y rows, row 3 the translation):

```
a = m[0][0], b = m[1][0], c = m[0][1], d = m[1][1],
tx = m[3][0], ty = m[3][1]

det = a*d - b*c                          (the ground map's determinant)
w.x = (d*(ndc.x - tx) - b*(ndc.y - ty)) / det
w.y = (a*(ndc.y - ty) - c*(ndc.x - tx)) / det
```

One pick is a few flops: no per-pick 4×4 inverse, no GLM calls, no
allocation. `det != 0` is the `isoMatrix`/`IsoCamera` precondition
(the ground map is invertible — matrices.h). The solve is the exact
inverse of the **stored float matrix**: the pick resolves to the cell
of the projection the frame actually renders with — never a different,
"ideal" matrix.

**Exact at all supported zoom levels.** The inverse is a fixed
sequence of float ops with no zoom-dependent branch or table — zoom
enters only through `M`'s entries (the M2-CAM-02 matrix build) — so
the same stored screen point resolves to the same cell at every zoom
the camera supports: the continuous `[zoomMin, zoomMax]` range
without snap, or the dyadic zoom ladder in snap mode. The goldens pin
4 zoom levels (`0.25, 1, 4, 16`) by hand for both built-in presets,
plus a camera-offset case.

## The grid cell: boundary rule and precision (FR-2.11)

**The boundary rule** (documented; pinned by
`IsoPickBoundary.HalfOpenCellsAndBoundaryZone`): cell `(gx, gy)` is
the half-open square

```
[gx*g, (gx+1)*g) x [gy*g, (gy+1)*g)      i.e.  gx = floor(w.x / g)
```

A point exactly on a cell's lower or left boundary belongs to THAT
cell; exactly on its upper or right boundary to the cell beyond it
(the floor convention); a corner to the cell to its upper right. The
grid is anchored at the world origin — the tile map's tile `(gx, gy)`
(M2-TILE-01) is exactly this cell at `g = 1`, with center
`(gx + 0.5, gy + 0.5)`. This is the same grid the M2-CAM-02 grid-snap
camera locks its position to.

**The precision (the boundary zone).** The computed ground point
`ŵ` satisfies

```
|ŵ − w| <= kIsoPickOpFactor * kIsoPickUlp * kappa * (|e| + |w|)
```

(world units, per axis), where `w` is the exact ground point of the
exact inverse, `e` is the camera's ground point
(`effectivePosition() = position + shakeOffset`), and `kappa` is the
∞-norm condition number of the UNSCALED ground 2×2 `[[dx.x, dy.x],
[dx.y, dy.y]]` (a preset property, scale-invariant: **4.5** for 2:1
dimetric, **~2.73** for true 30/60). The constants (CORE-005):
`kIsoPickUlp = 2⁻²⁴` (the per-op float rounding unit),
`kIsoPickOpFactor = 16` (the 10 rounding ops of the fixed sequence +
margin over the backward-error bound). Consequence (the boundary
guarantee):

- a pick returns the **exact cell of the exact inverse** unless the
  exact ground point lies within that bound of a cell boundary;
- inside that zone either of the two adjacent cells may be returned —
  the float rounding decides, **deterministic per build** (never a
  random flip between frames of the same stored matrix);
- the zone is **~6e-4 world units at scene scale** (`|e|, |w| <= 64.5`,
  a built-in preset — the property test's 0.5-cell margin is ~900x
  it), and `kIsoPickDomainBoundaryEps` (~4 world units) in the
  domain worst case (`kappa <= kIsoPickMaxCondition = 64` — 14x the
  built-ins — and `|e| = |w| = 32767`);
- a custom shear with `kappa > 64` still picks (the inverse is total
  over every invertible shear) but its zone scales linearly in
  `kappa`.

## Total function, domain, and saturation (CORE-008)

`screenToGrid` is **total**: it returns a pick for every input — no
error path, no per-call assert (a per-click pick is called from input
handling with untrusted data; a failed click must not be an engine
failure the game handles — the `isoDepthKey` total-function
precedent):

- non-finite screen components propagate through the fixed float
  sequence and the ground point **saturates** at the documented
  world domain (`kIsoDepthMaxWorldUnits = 32767`, iso_depth_key.h):
  `+inf → +bound`, `-inf` and `NaN → -bound` (IEEE: NaN compares
  false — the isoDepthKey saturation convention);
- the result cell is then within `|gx|, |gy| <= 32767/g` — inside
  `int32` for every valid `g` (`32767 / 1e-4 = 3.3e8 < 2³¹−1`).

A **stopped** (invalid) `IsoCamera` picks with the identity matrix
(the M2-CAM-02 stopped-state contract: `matrix()` is the identity):
`cell = floor(screen / g)` — degenerate but total; the game checks
`valid()` before relying on the pick (pinned by
`IsoPickStoppedCamera.IdentityDegenerate`).

## Ownership, lifetime, threading

- **Ownership/lifetime:** plain value types — `IsoGridConfig`,
  `IsoGridPick` are trivially copyable; nothing to own or release.
  The function is a pure query: it owns nothing, borrows the camera
  (const reference, caller-owned) and the grid config (by value).
- **Threading:** the pick is a pure function of its inputs — callable
  from any thread (input handling, the render set-up phase). It
  performs no mutation: a concurrent camera UPDATE is a data race on
  the camera's state (the camera's single-owner contract,
  docs/api/camera.md, CONC-001) — pick between camera mutations, as
  the frame pipeline phases it.
- **Presentation-only** (ARCH-009): reads no sim state, writes
  nothing; never part of replay state or the simulation state hash.
  Determinism scope: **per build** — a fixed sequence of float ops
  (render-side float, NOT SimMath — the ADR 0002 pinned-math contract
  does not apply; no RNG, no clock). Same (screen, matrix, grid) →
  bit-identical pick on the same build/platform.

## Performance (PERF-002/003, DOC-004)

- **Complexity: O(1)** — 10 rounding ops + 2 exact `floorf` + the
  camera's O(1) matrix build. One pick: no allocation, no logging,
  no GL calls (disabled cost is zero). The measured single-pick cost
  on the reference platform: **0.000136 ms mean** (the `iso_picking`
  budget, PRD §8.1 — target 0.01 ms; baseline
  [m2-iso-picking.md](../benchmarks/baselines/m2-iso-picking.md)).
- **Call site:** the pick is a **per-click / per-query** call in input
  handling — NEVER per sprite or per frame. Batch picks (e.g. a
  drag-select region) are one call per screen point; there is no
  higher-level batch form (there is no per-frame pick work to batch).
- **Blocking/IO/GPU:** none — pure float arithmetic.

## Determinism and network-authority implications

The pick is presentation-only: it never mutates authoritative state.
A pick result is a *client* input (the game decides what a clicked
cell means); replicating the pick's INPUT (the screen point / cell) —
not the authoritative consequence — is the standard
client-authority pattern (PRD §4: click-to-select/move). Because the
pick is deterministic per build for a given stored matrix, a replayed
frame + the same click reproduces the same cell on the same build
(no cross-platform promise — the float ops are render-side).

## Misuse warnings

- **The screen is NDC, not window pixels** — convert with the
  documented one-liner first (RENDER-006). Picking with pixel
  coordinates picks the wrong cell by an order of magnitude.
- **Do not pick through a stopped camera** (check `valid()`) — the
  identity-matrix degenerate pick is total but meaningless; and not
  through a non-Iso `ProjectionView` (the overload's precondition:
  the 2×2 solve assumes the iso row structure).
- **The cell is on the GROUND plane (z = 0)** — a click on raised
  terrain picks the cell of its ground projection (the standard
  isometric click-to-select semantics). The tile-height-aware pick
  (the standing cell of the clicked tile) lands with the tile map
  (M2-TILE-01/02), which consumes this inverse.
- **Config once, pick often**: build the `IsoGridConfig` with the
  scene (the grid's cell size is a scene constant); the pick itself
  is the hot input path.
