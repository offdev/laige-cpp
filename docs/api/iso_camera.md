# Isometric camera (`laige::render::IsoCamera`)

The isometric camera (M2-CAM-02; FR-2.4, FR-2.5, PRD §10.1; AGENTS
ARCH-009, API-006/008, CORE-005, PERF-003, DOC-004). Public header:
`src/laige-render/include/laige/render/iso_camera.h`; implementation:
`src/laige-render/iso_camera.cpp`. Unit suite: `ctest -R iso_camera`
(`tests/laige-render/iso_camera_tests.cpp`) — pure value math, no GL
environment required: it runs in every local tree and in CI, with
hand-computed goldens for the preset matrices, the snap invariants,
and the zoom-level set.

The `IsoCamera` owns a **M2-CAM-01 `Camera` by value** and adds two
things on top of it: the **isometric preset** (the ADR 0005 config-only
pattern) and the **grid-snap camera mode**. The camera core's contract
(position, follow, bounds, shake, zoom clamp, the look-at margin,
`update()`) is unchanged — see [Camera](camera.md); the `IsoCamera`
delegates to it and wraps its mutations in the snap-aware invariants
below. The preset's matrix constants come from the M2-GL-03 builders
(`isoDimetric2To1`, `isoTrueIso3060`, `isoMatrix`) — no preset has its
own matrix code.

## The presets (ADR 0005; the single config value)

The preset is **one config value** (`IsoPreset`: a kind plus, for the
built-in presets, an NDC `scale`; for `CustomShear`, an `IsoAxes`):

| Kind | Meaning | Axes come from |
|---|---|---|
| `Dimetric2To1` | 2:1 dimetric — the **engine default** (ADR 0005) | `isoDimetric2To1(scale)` |
| `TrueIso3060` | true 30°/60° isometric | `isoTrueIso3060(scale)` |
| `CustomShear` | arbitrary shear (advanced) | the config's `IsoAxes` |

A scene that changes its iso look changes only this config value —
never its simulation, depth-key, picking, or asset code (the ADR 0005
config-only pattern). Both built-in presets are selectable per scene.

**The custom-shear validation (M2-CAM-02's scene-shear gate):** the
`CustomShear` axes MUST pass the M2-ISO-01 `isoShearSupported()`
checker (both ground axes project downward with equal slopes equal to
`zUnit`; the ground map is invertible). The engine owns the isometric
render depth (FR-2.2, G-R11) and a shear that does not sort back-to-front
would break RENDER-003's deterministic order, so unsupported shears are
**rejected at the config boundary** (`preset_shear` warn). The raw
`isoMatrix()` builder remains available for advanced use: an unsupported
but *invertible* shear still transforms through a hand-stored
`ProjectionView` (the M2-PROJ-01 contract) — it is simply not a scene
preset.

## The camera matrix (world → NDC)

`matrix()` returns the **combined world → NDC affine matrix** of the
camera's current state — the single matrix the batcher (M2-SPRITE-02
onward) multiplies by. With the preset's axes `(dx, dy, zUnit)`, the
eye `e = effectivePosition() = position + shakeOffset`, and `Z = zoom()`:

```
M = [ dx.x/Z   dy.x/Z   0        -(dx.x*e.x + dy.x*e.y)/Z ]
    [ dx.y/Z   dy.y/Z   zUnit/Z  -(dx.y*e.x + dy.y*e.y)/Z ]
    [ 0        0        0        0                        ]
    [ 0        0        0        1                        ]
```

- At `Z = 1` and `e = 0` (no shake), `M` equals the preset's
  `isoMatrix()` exactly (the −0.0 normalizations are pinned by the
  tests).
- The camera's ground point `e` projects to NDC `(0, 0, 0)` — the
  screen center — at every zoom and position: the iso "camera center"
  is the screen-center ground point (an affine oblique projection has
  no view/projection split — the matrices.h contract).
- NDC-z is **0 for every world point**: the 2.5D depth is
  engine-owned (the M2-ISO-01 depth keys, PRD §4), never derived from
  the projection.
- The shake's **z component does not enter `M`**: the iso projection
  has no vertical viewpoint — a screen shake uses the shake's x/y,
  which the projection maps to screen space.

Build `matrix()` in the render **set-up phase, once per frame change**
(camera update, zoom, position, preset switch) — the M2-CAM-01
per-frame convention; never per sprite.

## The grid-snap camera mode

**The documented choice:** the engine **snaps continuously** — the
camera position's `(x, y)` ground coordinates are quantized to the
grid on `create`, on every `setPosition`, and after every follow step
of `update()`. The standard isometric grid-locked camera feel: the
camera never rests off the grid, even mid-follow (it tracks moving
targets in grid steps). The alternative (snap-on-release) was
considered and rejected for the standard feel: a release-snap camera
slides off-grid between releases, which is not the isometric look.

**The position snap** (per ground axis):

```
snapCoord(v, g) = g * round-half-away(v / g)
```

float-only arithmetic (no integer conversion — total for every finite
`v` and `g > 0`), nearest grid multiple, **ties away from zero** (the
`iso_depth_key.h` rounding convention), deterministic per build. The
`z` coordinate is **never snapped**: the camera height is free (the
M2-CAM-01 bounds contract).

**The snap invariants** (all machine-checked by the `iso_camera` CTest
entry):

- **On the grid:** in snap mode, `position().x` and `position().y` are
  grid multiples (`snapCoord(p, g) == p`) after the create, after every
  mutation, and after every `update()` — for any input inside the
  documented world domain (`|x|, |y| ≤ 32767` world units,
  [coordinates](../concepts/coordinates.md) §1). Outside it the snap
  result can be non-representable: the frame's snap is rejected with a
  `snap_not_representable` warn — the state stays valid, off-grid for
  that frame only.
- **Grid-aligned bounds:** the snap of a point inside a grid-aligned
  rectangle stays inside it (the nearest grid multiple of a value in
  `[m, M]` with `m, M` multiples of `g` is itself in `[m, M]`), so
  grid-snap mode **requires** the camera bounds rectangle to be
  grid-aligned (every corner an exact multiple of `gridSize`) whenever
  both are enabled — validated at create (`bounds_grid_alignment`).
  Without it no clamp/snap order can keep both invariants.
- **Snap margin:** the snap moves the position by at most `g/2` per
  ground axis (Euclidean distance ≤ `g·√2/2`), so snap mode validates
  the **inflated** look-at margin at create and on every
  position/target mutation:

  ```
  |target − position| > maxShakeOffset + g·√2/2
  ```

  Under this bound no snap can ever break the M2-CAM-01 look-at
  margin — the "on the grid" invariant is **total** inside the world
  domain: there is no input for which a snap is silently skipped.
  (A mutation whose *snapped* candidate would break the margin is
  still rejected, state unchanged — the documented failure path for
  adversarial inputs.)

**The zoom levels (the documented grid-aligned set):**

```
L = { zoomMin · 2^n : n ≥ 0, zoomMin · 2^n ≤ zoomMax }
```

the **dyadic ladder** anchored at `zoomMin`. Every level is a power of
two away from `zoomMin`, so the grid-to-screen scale at any level is a
power of two times the level-0 scale: moving between levels can never
leave the grid off the pixel alignment it had at an aligned level
(halving/doubling the scale preserves pixel alignment; an arbitrary
zoom factor cannot). The final pixel alignment also depends on the
window size — the game's concern (RENDER-006: the window is a platform
detail, not part of this pure API); the ladder is the
window-size-independent part.

`snapZoomLevel(z, zoomMin, zoomMax)`: `z` is clamped into
`[zoomMin, zoomMax]` (below/above clamp to the bounds — documented,
not an error); the result is the **nearest level in log2 space** — the
comparison is against the geometric midpoint `level·√2` of the
enclosing pair, and an **exact float tie goes to the higher zoom**.
The result is always an exact member of `L` (idempotent: a level snaps
to itself — the tests pin the set exactly).

## The API

| Member | Returns | Contract / failure behavior |
|---|---|---|
| `IsoCamera::create(options)` | `Result<IsoCamera, ErrorCode>` | Validates (below); snaps the initial position into the grid in snap mode. `InvalidArgument` + one rate-limited `iso_camera/options_invalid` warn (first failing option wins). |
| `valid()` | `bool` | False in the stopped state. |
| `camera()` | `const Camera&` | The owned M2-CAM-01 camera — const: all mutation goes through the snap-aware `IsoCamera` mutators. |
| `preset()` | `const IsoPreset&` | The preset config as validated at creation. |
| `gridSnapEnabled()` / `gridSize()` | `bool` / `float` | The snap mode state. |
| `matrix()` | `Mat4` | The combined world → NDC matrix of the current state (the section above). Identity in the stopped state. |
| `setPosition(p)` | `Status` | Clamps `(x, y)` into the bounds rectangle (when enabled), then snaps to the grid (snap mode); the candidate is validated against the (inflated) look-at margin before commit — state unchanged on rejection. `z` is never snapped. |
| `setTarget(t)` | `Status` | The look-at point is free (the grid locks the position, not the look-at). Validated against the (inflated) look-at margin; state unchanged on rejection. |
| `setFollowTarget(t)` | `Status` | Enables the smooth follow (the M2-CAM-01 step; in snap mode the follow runs in grid steps — the position is snapped after every follow step). |
| `stopFollowing()` | `void` | Disables the follow. |
| `setZoom(z)` | `Status` | Snap mode: snapped to the dyadic ladder `L` (below/above clamp to the bounds). No snap: the M2-CAM-01 clamp. `InvalidArgument` + warn on non-finite input. |
| `applyShake(impulse)` | `Status` | The M2-CAM-01 bounded shake; the shake moves the view center (the matrix translation); its z component does not enter the iso matrix. |
| `update()` | `void` | The M2-CAM-01 update (follow step → bounds clamp → shake decay) + the grid snap of the position (snap mode only). No-op in the stopped state. |
| `snapCoord(v, g)` | `float` | Static, pure: the nearest grid multiple of `v` (the section above). |
| `snapZoomLevel(z, lo, hi)` | `float` | Static, pure: the snapped zoom level (the section above). |

**Option validation** (first failure wins; one `iso_camera/options_invalid`
warn per failed create, the failing option in the `option` field):

1. the M2-CAM-01 camera options — validated by `Camera::create` (its
   own `camera/options_invalid` warn; its failure short-circuits);
2. `preset_kind` — the kind is not one of the three presets;
3. `preset_scale` — the built-in scale is not finite `> 0`;
4. `preset_shear` — the `CustomShear` axes fail `isoShearSupported()`;
5. `grid_size` — not finite, `≤ 0`, or `< kIsoSnapMinGridSize` (`1e-6`);
6. `bounds_grid_alignment` — snap + enabled bounds, a corner not an
   exact multiple of `gridSize`;
7. `snap_margin` — snap enabled and the inflated margin fails.

## Ownership, lifetime, threading

- **Ownership:** a plain value object (the `CameraOptions` pattern,
  CONC-001): copy is a plain value copy; the owner is the render
  set-up phase (the frame pipeline's phases serialize access). No
  internal synchronization, no resources, nothing to release.
- **Presentation-only (ARCH-009):** the camera never touches sim state;
  the follow target is supplied by game code from the presentation
  state (M1-LOOP-02's read-only boundary). The camera state is
  excluded from replay state and the simulation state hash. Nothing in
  this module enters the simulation (the include-graph lint R2).
- **Determinism:** the same input sequence produces **bit-identical**
  camera state on the same build/platform (a fixed sequence of float
  ops — render-side float, not SimMath; no RNG, no clock). The
  pinned-math contract (ADR 0002) does not apply; no cross-platform
  bit-exactness is promised.

## Logging

All events go through the `laige::log` facade (AGENTS §14), subsystem
`iso_camera`:

| Event | Severity | Fields | When |
|---|---|---|---|
| `options_invalid` | Warn (rate-limited) | `option` (stable name), `value` | a failed `IsoCamera::create` (first failing option wins; a base camera failure uses the M2-CAM-01 `camera/options_invalid` event instead) |
| `non_finite_input` | Warn (rate-limited) | `input` (`position`/`target`/`zoom`) | a snap-aware setter got a non-finite value |
| `lookat_margin_violated` | Warn (rate-limited) | `min_distance` | a mutation's candidate would break the (inflated) look-at margin |
| `snap_not_representable` | Warn (rate-limited) | `grid_size` | a snap result is not representable (outside the documented world domain for this grid size) — the frame's snap is skipped, state stays valid |

The healthy path (create success, `update`, the matrix builds) logs
nothing (LOG-003).

## Performance

The camera is a **pure value object with no heap storage**: every
operation — `create`, every setter, `update()`, the matrix build —
allocates nothing on every path (the zero-per-frame-allocation property
is structural, the M2-CAM-01 pattern). Costs:

- `matrix()`: O(1) — ~15 flops over the preset axes (stored at create).
- `update()`: the M2-CAM-01 cost (a few dozen ops) + in snap mode two
  divisions, two `floor`/`ceil`, and two multiplies (the position snap).
- `setZoom()` (snap mode): O(log₂(zoomMax/zoomMin)) ladder doublings —
  bounded, ~7 for the default `[0.1, 16]` range; `setZoom` is an input
  event, not per-frame work.
- Budget guidance: build `matrix()` **once per frame change** (set-up
  phase); the per-sprite world→NDC multiply (M2-SPRITE-02) consumes the
  stored matrix. Never rebuild it per sprite, and never call it per
  tick (the camera is presentation state — ARCH-002).

## A performant example

```cpp
// Scene setup (once): the iso camera is presentation state — it lives
// in the game's render set-up phase, not in the simulation.
laige::render::IsoCameraOptions opts;
opts.camera.halfWidth = 16.0f;        // the ortho view extent (M2-CAM-01)
opts.camera.halfHeight = 9.0f;
opts.camera.boundsEnabled = true;
opts.camera.bounds = laige::render::CameraBounds{  // grid-aligned corners
    laige::render::Vec2{0.0f, 0.0f}, laige::render::Vec2{64.0f, 64.0f}};
opts.preset.kind = laige::render::IsoPresetKind::Dimetric2To1;  // ADR 0005 default
opts.preset.scale = 1.0f;
opts.snap.enabled = true;             // the grid-snap mode
opts.snap.gridSize = 1.0f;            // world units per tile
auto cam = laige::render::IsoCamera::create(opts);  // Result: check .ok()

// Each presentation frame (the frame clock's cadence, M2-GL-02):
cam.setFollowTarget(playerWorldPos);  // from the presentation state
cam.setZoom(zoomLevel);               // snapped to the dyadic ladder
cam.update();                         // follow (in grid steps) + clamp + decay + snap
auto m = cam.matrix();                // one build per frame change
// The batcher's per-sprite world -> NDC multiply consumes `m`.
```

## Misuse warnings

- **Pace `update()` once per presentation frame.** The follow lerp and
  shake decay are per-update (the M2-CAM-01 contract) — calling it
  twice per frame doubles the follow speed and the decay rate.
- **Align the bounds to the grid.** In snap mode a non-aligned bounds
  rectangle is rejected at create: the snap of a point in an aligned
  rectangle stays inside it, and no clamp/snap order can keep both
  invariants for a non-aligned one.
- **Keep the look-at margin comfortably above `maxShakeOffset +
  g·√2/2` in snap mode** (validated at create and on mutations): a
  margin near the bound means a snap or a worst-case shake can put the
  effective eye on the look-at point — the `view()` precondition.
- **Do not mutate the owned camera directly.** `camera()` is const —
  the snap-aware mutators are the only mutation path (the invariants
  would otherwise be bypassable).
- **Custom shears that do not sort back-to-front are rejected.** If
  you need an invertible-but-unsupported shear transform, use the raw
  `isoMatrix()` + `ProjectionView` path (M2-PROJ-01) — it is not a
  scene preset and the engine's depth order does not apply to it.

## Related

- [Camera](camera.md) — the M2-CAM-01 camera core the `IsoCamera`
  owns (position, follow, bounds, shake, zoom).
- [Matrix utilities](matrices.md) — the M2-GL-03 preset builders
  (`isoDimetric2To1`, `isoTrueIso3060`, `isoMatrix`) and the NDC
  conventions.
- [Isometric depth keys](iso_depth_key.md) — the engine-owned depth
  order the preset validation protects (`isoShearSupported`).
- [Projection](projection.md) — the per-scene projection modes and the
  raw-matrix path for advanced transforms.
- [Coordinates](../concepts/coordinates.md) — the world/NDC/depth
  conventions (ARCH-008).
