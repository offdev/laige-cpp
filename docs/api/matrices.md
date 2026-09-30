# Matrix utilities (`laige::render` matrix builders)

The rendering-side camera and projection matrix builders (M2-GL-03;
PRD §11, §4, §10.3; AGENTS ARCH-009/ARCH-006, CORE-005, PERF-003,
CPP-008; ADR 0008 for the GLM vendoring, ADR 0005 for the isometric
preset tables). Public header:
`src/laige-render/include/laige/render/matrices.h`; implementation:
`src/laige-render/matrices.cpp`. Unit suite: `ctest -R matrices`
(`tests/laige-render/matrices_tests.cpp`) — pure math, no GL
environment required: it runs in every local tree and in CI, and the
goldens are hand-computed from the documented formulas.

These are **stateless pure functions**: each returns a matrix that is a
function only of its arguments. They create no state, read no state,
make no GL calls, allocate nothing, and log nothing. The camera objects
that arrive with M2-CAM-01/02 own the *build cadence* (build once per
scene or camera change, in the set-up phase); these builders do not.

## The conventions (pinned)

- **World space:** right-handed; `(x, y)` is the ground plane, `+z` is
  height/up (the `SimMath` 2.5D convention — the simulation is
  axis-aligned 2D plus a depth/height value, PRD §4).
- **NDC (OpenGL):** x right, y up, z ∈ [−1, +1] with the **near plane
  at z = −1** and the **far plane at z = +1**. A camera looks along its
  own **−z** axis (camera space: x right, y up, −z forward); the near
  plane sits at camera z = −zNear, the far plane at camera z = −zFar.
- **Storage:** column-major GLM `Mat4` (`m[c][r]` is row r of column
  c). The affine translation lives in **column 3**. View matrices map
  world → camera space; projection matrices map camera → NDC. `lookAt`
  and the projection builders are meant to be composed (`M = P * V`);
  `planeOrtho` and the iso builders return the **combined** world → NDC
  affine matrix in one step (affine oblique projections have no
  meaningful view/projection split).
- **"Screen positions" are NDC values.** The mapping from NDC to
  window pixels is M2-PROJ-01's job (the viewport transform); these
  builders stop at NDC.

The canonical home for the engine's coordinate conventions is
`docs/concepts/coordinates.md` (created with M2-ISO-01); the header
preamble carries the pinned copy.

## The API

| Builder | Returns | Precondition set (debug-asserted; release UB) |
|---|---|---|
| `ortho(left, right, bottom, top, zNear, zFar)` | camera → NDC | `left < right`, `bottom < top`, `zNear < zFar`, all finite |
| `perspective(fovY, aspect, zNear, zFar)` | camera → NDC | `0 < fovY < π`, `aspect > 0`, `0 < zNear < zFar`, all finite |
| `lookAt(eye, center, up)` | world → camera | `eye != center`, `up` not parallel to `center − eye`, all finite |
| `planeOrtho(center, right, up, halfWidth, halfHeight, zNear, zFar)` | world → NDC (combined P·V) | `right`/`up` orthonormal (unit + perpendicular, tolerance 1e-5), `halfWidth > 0`, `halfHeight > 0`, `zNear < zFar`, all finite |
| `isoMatrix(IsoAxes)` | world → NDC (combined affine) | all finite, ground 2×2 invertible (`det ≠ 0`) |
| `isoDimetric2To1(scale)` | world → NDC (2:1 dimetric, ADR 0005 default) | `scale > 0`, finite |
| `isoTrueIso3060(scale)` | world → NDC (true 30°/60°) | `scale > 0`, finite |

Every builder's exact element formulas are documented in the header
next to the declaration (CORE-005: no unexplained constants — the
`1/√3` constants in the true-iso preset are the inverse of the
ground-axis foreshortening of a unit diagonal step, documented at the
site). A precondition violation is a **programmer error** (misconfigured
camera data), not a recoverable engine failure: debug builds assert,
release builds treat the input as undefined — the engine's
Result/Status convention for precondition violations (cf. `SimMath::
clamp`, `laige/sim_math.h`). The builders themselves return no errors
and log nothing (there is no failure to report: the inputs *are* the
camera configuration).

### `planeOrtho` — the 2D-plane camera

The top_down and side_view projection modes (M2-PROJ-01): an
orthographic camera looking straight at the plane spanned by `right`
and `up` through `center`. The plane normal is
`n = normalize(cross(right, up))` (the toward-the-viewer side); the
camera sits at `center + n*zNear` and looks along `−n`; the plane
through `center` lands exactly on the NDC near plane (z = −1); the
visible depth slab is `[zNear, zFar]` measured from the plane toward
the viewer. `M2-PROJ-01` instantiates it:

- top_down — `center = (ox, oy, 0)`, `right = (1,0,0)`, `up = (0,1,0)`;
- side_view — `center = (ox, oy, oz)`, `right = (1,0,0)`, `up = (0,0,1)`
  (screen y = world height).

### The isometric presets (ADR 0005)

All three presets are affine oblique projections with NDC z = 0 for
every point — the 2.5D depth is **engine-owned** (the FR-2.2 depth
keys, M2-ISO-01), never derived from the projection (PRD §4):

| Preset | `dx` (per +x step) | `dy` (per +y step) | `zUnit` (per +z step) | Geometry |
|---|---|---|---|---|
| 2:1 dimetric (default) | `(2k, −k)` | `(−2k, −k)` | `k` | Affine, 45° azimuth; per-step delta (±2, 1)·k — integer at k = 1; the tile is a 4k × 2k rhombus; the vertical squashed to ½ the tile height |
| true 30°/60° | `(s, −s/√3)` | `(−s, −s/√3)` | `s/√3` | True orthographic axonometric: 45° azimuth, elevation arcsin(1/√3) ≈ 35.264°; ground axes at 30° to horizontal; all three axes equally foreshortened; tile ratio √3:1 |
| custom shear | user `IsoAxes` | user `IsoAxes` | user | Any invertible oblique view of the ground plane (`isoMatrix`) |

Both ground axes project **downward** (negative NDC-y) and `+z`
projects upward, so the axis-aligned depth key `f(x + y, height,
layer)` (FR-2.2; M2-ISO-01) sorts back-to-front for every preset, and
the O(1) picking inverse (FR-2.11; M2-ISO-03) exists whenever the
ground 2×2 is invertible — both properties are unit-tested. M2-CAM-02
selects the preset by config and builds through one mechanism: the
preset id maps to `IsoAxes`/scale, then `isoMatrix` (the preset
builders are thin named constructors over it — equality is tested).

## Ownership, lifetime, threading

- **Ownership:** none. The builders return by value; there is nothing
  to own, release, or invalidate.
- **Threading / phase:** pure functions — callable from any thread at
  any phase (sim tick, render set-up, main thread); no shared state, no
  locks, no GL context required. The M2-CAM-01/02 camera objects own
  the build cadence (set-up phase, once per scene/camera change); a
  per-sprite call is a misuse (see below).
- **GLM exposure:** `Vec2`/`Vec3`/`Mat4` are GLM value types exposed
  through this public API by design (ADR 0008) — consumers get GLM
  transitively via the module's public include interface and never
  `#include <glm/...>` themselves (include-graph lint R3).

## Performance (PERF-001/003, DOC-004)

- Every builder is **O(1)** — a few dozen flops (`perspective` adds one
  `tan`; `lookAt`/`planeOrtho` add two normalizations and one cross;
  `isoTrueIso3060` adds one `sqrt`).
- **Zero allocation, zero locks, zero logging, zero GL calls.** Nothing
  on any path scales with scene size.
- Set-up phase work: build the camera matrices once per scene or
  camera change; store them; the per-frame render path only *uses* the
  stored matrices (the M2-PROJ-01 `world_to_screen` multiplies a
  stored matrix by each presentation position — that multiply is the
  hot path and is O(1) per point, no allocation).
- Common trap: rebuilding `lookAt`/`planeOrtho` per sprite per frame.
  They are configuration functions, not per-entity functions.

## Performant example

```cpp
// Set-up phase (once per scene / camera change) — the M2-CAM-01/02
// camera objects own this cadence.
auto vp = laige::render::planeOrtho(   // top_down mode
    laige::render::Vec3{origin.x, origin.y, 0.0f},
    laige::render::Vec3{1.0f, 0.0f, 0.0f},
    laige::render::Vec3{0.0f, 1.0f, 0.0f},
    /*halfWidth=*/8.0f * zoom, /*halfHeight=*/4.0f * zoom,
    /*zNear=*/1.0f, /*zFar=*/21.0f);

// Render phase (per presentation position, O(1), no allocation):
const laige::render::Vec3 p{tile.x, tile.y, 0.0f};
const laige::render::Vec4 clip = vp * laige::render::Vec4{p, 1.0f};
// clip.xy is the NDC position (M2-PROJ-01 maps NDC -> pixels).
```

## Misuse warnings

- **Do not call a builder per sprite per frame.** They are set-up-phase
  configuration functions; rebuild only when the camera/scene changes.
- **Do not use the iso matrices for depth.** Their NDC z is 0 by
  design (PRD §4); depth ordering comes from the engine's FR-2.2 depth
  keys, not from the projection.
- **Do not feed a degenerate iso camera** (parallel ground axes,
  `det = 0`): the picking inverse (M2-ISO-03) does not exist and the
  debug assert fires.
- **Do not mix conventions.** The builders assume the pinned
  conventions above (right-handed world, +z up, OpenGL NDC, camera
  looks along −z). A left-handed or +z-forward input is a precondition
  violation, not a supported configuration.
