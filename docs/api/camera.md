# Camera (`laige::render::Camera`)

The 3D camera core (M2-CAM-01; FR-2.4, PRD §10.1; AGENTS ARCH-009,
API-006/008, CORE-005, PERF-003, DOC-004). Public header:
`src/laige-render/include/laige/render/camera.h`; implementation:
`src/laige-render/camera.cpp`. Unit suite: `ctest -R camera`
(`tests/laige-render/camera_tests.cpp`) — pure value math, no GL
environment required: it runs in every local tree and in CI, with
hand-computed goldens for the exact-zoom-clamp, the shake decay to
zero, and the follow sequence.

The camera is the **presentation-side** view of the world (ARCH-009):
it reads nothing from and writes nothing to sim state, and its state
is never part of replay state or the simulation state hash. It builds
on the M2-GL-03 matrix builders (`lookAt`, `ortho`, `perspective`);
the isometric camera presets and the grid-snap mode land on top of it
in M2-CAM-02.

## The conventions (pinned)

The world/NDC conventions are the `matrices.h` contract (right-handed
world, `+z` up, OpenGL NDC, the camera looks along its own `−z` axis;
the canonical home is `docs/concepts/coordinates.md`). Camera-specific
conventions:

- **Zoom** is a **magnification factor** `Z` in `[zoomMin, zoomMax]` —
  the visible extent is divided by `Z` (`Z = 2` shows half the extent,
  i.e. zoomed in):
  - `Ortho`: the view box is `halfWidth/Z × halfHeight/Z` world units.
  - `Perspective`: `fov_eff = 2·atan(tan(fovY/2)/Z)`; at `Z = 1` the
    round-trip through `tan`/`atan` keeps `fov_eff` within 1–2 ulp of
    `fovY` (documented; pinned by the tests).
- **The rectangular bounds** constrain the camera *position's*
  `(x, y)` ground-plane coordinates to a closed rectangle (`min ≤ max`
  per component); the camera's `z` (its height) is unconstrained. The
  base position is clamped at creation, on every `setPosition`, and
  after every follow step — so `position()` is always inside the
  rectangle. A bounded shake may push the *effective* render position
  (`effectivePosition()`) at most `maxShakeOffset` past the edge
  (documented; the tests pin the excursion bound).
- **The look-at margin:** `|target − position| > maxShakeOffset` and
  `up` not near-parallel (within `1e-5` sine) to the view direction.
  This is the *sufficient* condition that keeps the `lookAt`
  precondition true for the effective eye (`position + shakeOffset`)
  under any bounded shake — the eye can never reach the look-at point
  and the view basis never degenerates.
- **Determinism:** given the same input sequence (the same `create`
  options and the same setter/`update` calls in the same order), the
  same build on the same platform produces **bit-identical** camera
  state: every operation is a fixed sequence of float ops (no RNG, no
  clock). The camera is render-side float, not SimMath — the pinned-
  math contract (ADR 0002) does not apply, and no cross-platform
  bit-exactness is promised.

## The API

| Member | Returns | Contract / failure behavior |
|---|---|---|
| `Camera::create(options)` | `Result<Camera, ErrorCode>` | Validates the options (below); clamps the initial position into the rectangle when enabled. `InvalidArgument` + one rate-limited `camera/options_invalid` warn (first failing option wins). |
| `valid()` | `bool` | False in the stopped state (no valid configuration). |
| `position()` / `target()` / `up()` | `Vec3` | The camera state (world units). |
| `shakeOffset()` / `effectivePosition()` | `Vec3` | The bounded shake offset; `position + shakeOffset` (the eye). |
| `zoom()` | `float` | Always in `[zoomMin, zoomMax]`. |
| `following()` / `projectionKind()` | `bool` / `CameraProjection` | State accessors. |
| `setPosition(p)` | `Status` | Clamps `(x, y)` into the rectangle (never an error). `InvalidArgument` + warn on non-finite input or a look-at-margin violation (state unchanged). |
| `setTarget(t)` | `Status` | The mirror of `setPosition` (no rectangle clamp — the look-at is not constrained). |
| `setFollowTarget(t)` | `Status` | Enables the smooth follow. `InvalidArgument` + warn on non-finite input. |
| `stopFollowing()` | `void` | Disables the follow (no-op when stopped). |
| `setZoom(z)` | `Status` | **Clamps** into `[zoomMin, zoomMax]` — exact at the bounds (below min → exactly min, above max → exactly max); clamping is documented behavior, not an error. `InvalidArgument` + warn on non-finite input. |
| `applyShake(impulse)` | `Status` | `offset = clamp(offset + impulse, ±maxShakeOffset)` per component (bounded). `InvalidArgument` + warn on non-finite input. |
| `update()` | `void` | The per-frame step (below); no-op in the stopped state. |
| `view()` / `projection()` / `viewProjection()` | `Mat4` | The M2-GL-03 builders over the current state (the shake moves the eye). The identity matrix in the stopped state. |

**Option validation** (first failure wins): `projection` a known
kind; `aspect > 0`; `zNear < zFar` (both finite); ortho: `halfWidth >
0`, `halfHeight > 0`; perspective: `0 < fovY < π`; `zoomMin > 0`,
`zoomMax ≥ zoomMin`; `zoom` finite (clamped at creation);
`0 < followLerp ≤ 1`; `0 ≤ shakeDecay < 1`; `maxShakeOffset ≥ 0`;
`position`/`target`/`up` finite, `up` nonzero; the look-at margin
(above); when `boundsEnabled`, `bounds.min ≤ bounds.max` per component,
all finite.

## The per-frame update

`update()` runs once per presentation frame (the caller paces it — the
frame clock's cadence, M2-GL-02), in this order:

1. **Follow step** (while following): `delta = (followTarget − target)
   · followLerp`, then `target += delta; position += delta`. A rigid
   translation: the view direction and the eye→look-at distance stay
   constant within float rounding while the look-at point converges to
   the follow target — the camera slides until the followed point sits
   at the look-at point (the screen center of the view). `followLerp`
   is **per update**, not per second: frame-rate independence is the
   caller's concern.
2. **Bounds clamp** (when enabled): `position.xy` clamped into the
   rectangle (the follow step can carry it out; the clamp lands it
   back — the adversarial-follow test exercises exactly this).
3. **Shake decay**: `shakeOffset *= shakeDecay` per component.

## The shake (bounded, decaying, deterministic)

The shake is a bounded camera-position offset added to the base
position when the view matrix is built (the eye = `position +
shakeOffset`):

- `applyShake(impulse)`: `offset = clamp(offset + impulse, ±maxShakeOffset)`
  per component — the offset never exceeds the bound.
- `update()`: `offset *= shakeDecay` per component.
- **The documented decay bound:** with the default `shakeDecay = 0.5`
  the offset is exactly halved per update; in IEEE binary32,
  `x · 2⁻ⁿ ≤ 2⁻¹⁵⁰` rounds to 0, so the offset reaches **exactly
  `0.0` within `150 + ⌈log₂(maxShakeOffset)⌉` updates** — i.e. within
  150 updates for any bound ≤ 1 world unit. The tests pin the exact
  boundary: at update 149 the 1.0 offset is still the last nonzero
  denormal (`2⁻¹⁴⁹`), at update 150 it is exactly zero.

## Ownership, lifetime, threading

- **Ownership:** a single-owner value object owned by the game's
  render set-up / presentation phase (the frame pipeline's phases,
  M2-GL-02). It carries no resources — copy and move are plain value
  copies (no ownership-transfer semantics, unlike `GlContext` /
  `RenderThread`).
- **Threading:** not thread-safe (no internal synchronization,
  CONC-001). The frame pipeline reads the built matrices in its
  render set-up stage; the game mutates the camera (follow target,
  zoom, shake) in the presentation phase — one writer, one reader
  sequence, no concurrent access.
- **Presentation-only (ARCH-009):** the camera never touches sim
  state; the follow target is supplied by game code from the
  presentation state (M1-LOOP-02). The camera state is excluded from
  replay state and the simulation state hash.

## Logging

All events go through the `laige::log` facade (AGENTS §14),
subsystem `camera`:

| Event | Severity | Fields | When |
|---|---|---|---|
| `options_invalid` | Warn (rate-limited) | `option` (stable name), `value` | a failed `Camera::create` (first failing option wins) |
| `non_finite_input` | Warn (rate-limited) | `input` (`position`/`target`/`follow_target`/`zoom`/`shake`) | a setter got a non-finite value |
| `lookat_margin_violated` | Warn (rate-limited) | `min_distance` | `setPosition`/`setTarget` would break the look-at margin |

The healthy path (`create` success, `update`, the matrix builds) logs
nothing (LOG-003: a disabled event costs one gate check; these events
are not emitted at all).

## Performance

The camera is a **pure value object with no heap storage**: every
operation — `create`, every setter, `update()`, the matrix builds —
allocates nothing on every path, so the zero-per-frame-allocation
property is structural (no measurement needed). Costs:

- `update()`: O(1) float arithmetic — a few dozen ops (one lerp and two
  adds for the follow, up to two clamps, three decays). No allocation,
  no logging, no GL calls.
- `view()`: `lookAt` — two normalizations + one cross + a matrix fill.
- `projection()`: a dozen flops; `perspective` adds `tan` + `atan`.
- `viewProjection()`: one `Mat4 × Mat4` multiply (16 multiplies + 16 adds).
- Budget guidance: build `viewProjection()` **once per frame change**
  (set-up phase); the per-sprite world→NDC multiply (M2-SPRITE-02)
  consumes the stored matrix. Never rebuild it per sprite, and never
  call it per tick (the camera is presentation state — it does not
  participate in the simulation cadence, ARCH-002).

## A performant example

```cpp
// Scene setup (once): the camera is presentation state — it lives in
// the game's render set-up phase, not in the simulation.
laige::render::CameraOptions opts;
opts.projection = laige::render::CameraProjection::Ortho;
opts.halfWidth = 16.0f;
opts.halfHeight = 9.0f;
opts.boundsEnabled = true;                 // keep the camera on the map
opts.bounds = laige::render::CameraBounds{
    laige::render::Vec2{0.0f, 0.0f}, laige::render::Vec2{64.0f, 64.0f}};
auto cam = laige::render::Camera::create(opts);   // Result: check .ok()

// Each presentation frame (the frame clock's cadence, M2-GL-02):
cam.setFollowTarget(playerScreenAnchor);  // from the presentation state
cam.setZoom(zoomTarget);                  // clamped to [zoomMin, zoomMax]
cam.update();                             // follow + bounds + shake decay
auto vp = cam.viewProjection();           // one build per frame change
// The batcher's per-sprite world -> NDC multiply consumes `vp`.
```

## Misuse warnings

- **Pace `update()` once per presentation frame.** The lerp and decay
  factors are per-update, not per-second: calling it twice per frame
  doubles the follow speed and the shake decay rate.
- **Rebuild `viewProjection()` only when the camera changed.** It is a
  set-up-phase build, not a per-sprite call.
- **Do not fight the bounds.** The clamp is the constraint: a follow
  target outside the rectangle makes the camera hug the edge (the
  look-at keeps moving — the view slides, the position does not).
- **A look-at margin near `maxShakeOffset` is a degenerate config:** a
  worst-case shake can put the effective eye on the look-at point —
  the `lookAt` precondition (the debug assert in `view()`). Keep
  `|target − position|` comfortably above the shake bound.
- **Do not share the camera across threads.** It is a single-owner
  value object (CONC-001); the frame pipeline's phases serialize
  access by construction.

## Related

- [Matrix utilities](matrices.md) — the builders the camera delegates
  to, and the NDC conventions (M2-GL-03).
- [Frame pipeline](frame_pipeline.md) — the render thread and frame
  clock that pace `update()` (M2-GL-02).
- [Isometric depth keys](iso_depth_key.md) — the render ordering the
  camera's view does not affect (world state only, PRD §4).
- [Coordinates](../concepts/coordinates.md) — the world/NDC/depth
  conventions (ARCH-008).
