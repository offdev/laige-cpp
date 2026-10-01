# Projection modes + screen↔world transforms (`laige::render::ProjectionView`)

The per-scene/view projection modes (FR-2.5) and the screen↔world
transform API they define (FR-2.11 — the picking base; the isometric
grid picking lands on top in M2-ISO-03). Public header:
`src/laige-render/include/laige/render/projection.h`; implementation:
`src/laige-render/projection.cpp`. Unit suite: `ctest -R projection`
(`tests/laige-render/projection_tests.cpp`) — pure value math over the
M2-GL-03 / M2-CAM-01 matrices, no GL environment required: it runs in
every local tree and in CI, with hand-computed goldens per mode and
10k random round trips per mode against the documented precision.

The view **wraps** the M2-GL-03 matrix builders (the iso family,
`planeOrtho`) and the M2-CAM-01 camera (`viewProjection()` for
free_cinematic): the game builds the mode's matrix with the documented
builder and stores it in the view. The canonical narrative home for the
world/NDC conventions is
[`docs/concepts/coordinates.md`](../concepts/coordinates.md); the header
preamble carries the machine-checked contract.

## The projection modes (FR-2.5)

`ProjectionMode` — one value per scene/view; **`Iso` is the engine
default** (ADR 0005, PRD v0.2):

| Mode | Matrix builder (M2-GL-03 / M2-CAM-01) | Screen mapping | Preimage of a screen point |
|---|---|---|---|
| `Iso` | `isoMatrix` / `isoDimetric2To1` / `isoTrueIso3060` (the ADR 0005 presets) | affine oblique; NDC-z = 0 | the **full line** `origin + t·direction, t ∈ ℝ`, origin on the ground plane (z = 0) |
| `SideView` | `planeOrtho(center, (1,0,0), (0,0,1), …)` | screen x = world x, screen y = world **height** (+z) | the line along world **y**; origin on the reference plane through `planeCenter` |
| `TopDown` | `planeOrtho(center, (1,0,0), (0,1,0), …)` | the X/Y ground plane | the line along world **z**; origin on the reference plane through `planeCenter` |
| `FreeCinematic` | the M2-CAM-01 camera's `viewProjection()` (ortho or perspective) | full 3D camera | a **true ray**: origin on the NDC near plane, direction away from the camera (t ≥ 0) |

The ray `direction` is always `normalize(r1 × r2)` — the cross product
of the matrix's screen-x / screen-y rows (world 3D) — for the affine
modes, and the normalized near→far direction for free_cinematic. It is
normalized (unit length) in every mode.

## The API

| Member | Returns | Contract / failure behavior |
|---|---|---|
| `ProjectionView{}` | value | default state: `mode = Iso` (the engine default), identity matrix, origin reference plane. A plain value — no ownership, nothing to release. |
| `view.mode` / `view.matrix` / `view.planeCenter` | `ProjectionMode` / `Mat4` / `Vec3` | the view's state. `matrix` must be the mode's documented builder output; `planeCenter` is used by `SideView`/`TopDown` only (the same value passed to `planeOrtho`). |
| `view.worldToScreen(p2d, depth)` | `Vec3` (NDC) | the 2.5D world point (ground plane `p2d` + elevation `depth`, world units; render-side float from the presentation snapshot, M1-LOOP-02) → NDC (x, y, z). Iso: NDC-z = 0 exactly. Plane modes: NDC-z is the depth in the slab (the reference plane is NDC z = -1). No failure path — a pure transform. |
| `view.screenToWorldRay(ndc)` | `WorldRay {origin, direction}` | the mode's preimage ray of the NDC screen point (geometry above). No failure path — finite NDC input is a precondition (a programmer error, the matrices.h house convention). |
| `view.screenToWorld(ndc, plane)` | `Result<Vec3, ErrorCode>` | the preimage line/ray ∩ the plane `n·p = d`. `InvalidArgument` when (a) the plane is (numerically) parallel to the ray (`|dot(direction, n)| <= kProjectionParallelEps`) or (b) free_cinematic only: the intersection is behind the NDC near plane (t < 0 — clipped, unpickable: points between the camera and the near plane are never rendered). A failed pick is a recoverable game condition, not an engine error. |

Named constants (CORE-005): `kProjectionRoundTripTolerance` (1e-3
world units — the documented round-trip bound) and
`kProjectionParallelEps` (1e-6 — the numerical-parallelity threshold).

### The round trip (the documented precision)

For a mode and a world point `p` on a plane the preimage crosses,

```
view.screenToWorld(view.worldToScreen(p2d(p), p.z), that plane) == p
```

within `kProjectionRoundTripTolerance` world units for `|p| <= 32`
(one matrix multiply plus the per-mode inverse in float — a handful of
ulp at |p| ~ 32). The canonical planes: `Iso` / `TopDown`: any
`z = const` plane; `SideView`: any `y = const` plane; `FreeCinematic`:
any plane the camera ray crosses within the visible volume
(`z_cam ∈ [-zFar, -zNear]`). The suite pins 10k random points per mode
(seed per docs/testing.md §4).

### NDC ↔ window pixels (the game-facing boundary, RENDER-006)

The transforms take and return **NDC** — the module's documented screen
space (matrices.h). The window size is a render/platform detail (the
`GlContext`), not part of this pure API; the affine bridge is the
game's:

```
px = (ndc.x * 0.5 + 0.5) * width
py = (0.5 - ndc.y * 0.5) * height      (window y is down; NDC y is up)
ndc.x = px / width * 2 - 1
ndc.y = 1 - py / height * 2
```

## Ownership, threading, and phases

- **Ownership:** `ProjectionView` is a plain value object (the
  `CameraOptions` pattern) — no ownership, no lifetime concerns; copy
  is a plain value copy (CONC-001). The matrix is built by the game
  from the M2-GL-03 builders / M2-CAM-01 camera.
- **Threading:** single-owner value; the frame pipeline's phases
  serialize access by construction (do not share the view across
  threads — the camera.md contract).
- **Phase:** build/refresh the view in the render **set-up phase**,
  once per frame change (camera update, zoom, preset switch) — the
  per-frame camera convention (camera.md). The transforms themselves
  are pure and phase-agnostic.
- **Presentation-only (ARCH-009):** the transforms read nothing from and
  write nothing to sim state — the input is render-side float from the
  presentation snapshot (the sim → presentation conversion happens at
  the M1-LOOP-02 boundary, not here). Projection state is never part of
  replay state or the simulation state hash.
- **AC-4.2 (sim never sees projection):** the include-graph lint rule
  R2 (arrows only downward, PRD §10.1) blocks every `laige-sim` →
  `laige-render` include — fixture-tested by the
  `include-lint-sim-to-render` CTest entry (tests/tools); the surface
  check (no `ProjectionMode` in the laige-sim/laige-net public surface)
  lands with M2-AC-01.

## Performance

- **Complexity:** every operation is **O(1)** float arithmetic —
  `worldToScreen`: one 4×4 matrix multiply (+ one divide for
  free_cinematic; w = 1 exactly for the affine modes);
  `screenToWorldRay`: a 2×2 solve (affine) or one 4×4 inverse + two
  divides (free_cinematic); `screenToWorld`: one ray + two dots + one
  divide.
- **Allocations:** none, on any path.
- **Budget:** these are **per-pick / per-query** transforms (input
  handling, M2-ISO-03 onward) — NOT per-sprite work. The batcher's
  per-sprite world→NDC multiply (M2-SPRITE-02) consumes the stored
  `view.matrix` directly — never call `worldToScreen` per sprite
  (that would move this query API into the hot path it is not
  designed for).
- **Observer effect:** pure value math — no logging, no allocation, no
  GL calls; the cost is the flops themselves (negligible: a 4×4
  inverse is ~40 float ops, called once per query).
- **Determinism scope:** given the same view and the same call
  sequence, the same build on the same platform produces
  **bit-identical** results — every operation is a fixed sequence of
  float ops (render-side float, NOT SimMath: the pinned-math contract
  of ADR 0002 does not apply; no RNG, no clock). No cross-platform
  bit-exactness is promised (the camera.md contract).

## Misuse warnings

- **Build the matrix with the mode's documented builder** and keep it
  consistent with `mode`: the per-mode extraction (which rows invert,
  where the reference plane is) assumes the documented structure
  (debug asserts pin the divisors).
- **Set `planeCenter` for `SideView`/`TopDown` to the SAME value
  passed to `planeOrtho`** — the ray origin (and thus the picking) is
  defined relative to that reference plane.
- **Rebuild the matrix when the view changes** (camera update, zoom,
  preset switch) — once per frame change, set-up phase.
- **Do not pass sim (SimMath) coordinates** to the transforms: the
  input is render-side float from the presentation snapshot (ARCH-009).
- **Do not derive render ordering from these transforms** (PRD §4,
  G-R11): ordering is the M2-ISO-01 depth key, world-space by
  contract. An invertible shear that is not depth-key-supported
  (`isoShearSupported` = false) still transforms correctly — the depth
  key order is simply not guaranteed for it (M2-CAM-02 validates scene
  shears).
- **`screenToWorld` is a query, not a per-frame call:** one per pick /
  per UI hover, never in a per-entity loop. A failed pick
  (`InvalidArgument`: parallel plane, or a behind-the-near-plane
  intersection) is the game's condition to handle (e.g. "no object
  under the cursor") — not an engine failure.

## Example (performant path)

```cpp
// Scene set-up (once), and after any camera/preset change (set-up phase):
laige::render::ProjectionView view;
view.mode = laige::render::ProjectionMode::Iso;           // the default
view.matrix = laige::render::isoDimetric2To1(1.0f);       // ADR 0005 preset

// Per-pick (input handling) — the isometric picking of FR-2.11:
laige::render::Vec2 ndc{inputNdcX, inputNdcY};             // window → NDC (above)
auto hit = view.screenToWorld(ndc,
                              laige::render::Plane{laige::render::Vec3{0, 0, 1}, 0.0f});
if (hit.ok()) { /* *hit.valueIfOk() is the world ground point — grid cell
                  from it is M2-ISO-03's screen_to_grid */ }
// else: no ground point under the cursor (the plane never meets the
// preimage line — e.g. a parallel plane) — a normal "no hit" case.
```

## Related

- [`concepts/coordinates.md`](../concepts/coordinates.md) — the
  canonical world/NDC/depth/ordering conventions (ARCH-008).
- [`matrices.md`](matrices.md) — the matrix builders this view wraps
  (M2-GL-03).
- [`camera.md`](camera.md) — the M2-CAM-01 camera the free_cinematic
  mode uses (and whose budget convention the view refresh follows).
- [`iso_depth_key.md`](iso_depth_key.md) — the render ordering these
  transforms do NOT define (world-space by contract, PRD §4).
- Roadmap: M2-PROJ-01 (this), M2-CAM-02 (iso presets + grid-snap on the
  camera), M2-ISO-03 (isometric grid picking on `screenToWorldRay`),
  M2-SPRITE-02 (the batcher consuming `view.matrix`).
