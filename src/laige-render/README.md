# laige-render

Module per PRD §10.1: GL context, 2.5D batcher, isometric depth keys,
parallax, tiles, particles, text/UI, camera, passes.

Status: M2 in progress. M2-GL-01 landed the GL infrastructure —
`laige::render::GlContext`, the OpenGL 3.3 core context contract:
windowed creation (GLFW) and headless offscreen creation (EGL surfaceless
on Linux; a never-shown GLFW window on Windows/macOS), the 3.3 core
version gate (`checkGlVersion` — no fallback profile), the creation-time
capability snapshot (`GlCapabilities`), and the offscreen RGBA8 FBO as
the headless render target. The two new error codes (`GlUnavailable`,
`GlVersionUnsupported`) live in the `laige-core` registry. Public header
`include/laige/render/gl_context.h`, implementation `gl_context.cpp`;
API contract in [docs/api/gl_context.md](../docs/api/gl_context.md),
vendoring decision in
[ADR 0007](../docs/decisions/0007-glfw-glad-vendoring.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `gl_context`;
the GL-free `GlContextGate`/`GlContextArgs` suites run in every local
tree, the `GlContextSmoke` suite needs a usable GL and is verified on
the P0 CI runners). The GLFW/Glad targets compile the vendored C code
(`laige-glad` C target; GLFW via `add_subdirectory`); the engine target
stays C++-only behind the pimpl.

M2-GL-02 landed the frame pipeline — `laige::render::RenderThread`
(the render thread + the lock-free single-slot frame handoff: one
producer, one consumer, the atomic-slot handoff argument, the backpressure drop
(PERF-008), the exact accounting, the ordered idempotent shutdown
(CONC-006)) and `laige::render::FrameClock` (the vsync-paced frame
deadline grid + the `render_time` provider for
presentation/interpolation, M1-LOOP-02). Public header
`include/laige/render/frame_pipeline.h`, implementation
`frame_pipeline.cpp`; API contract in
[docs/api/frame_pipeline.md](../docs/api/frame_pipeline.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `render_thread`;
the GL-free `FrameClock`/`RenderThreadHandoff` suites run in every local
tree, the `RenderThreadOffscreen` suite needs a usable GL and is
verified on the P0 CI runners). No render→sim module edge was added:
the handoff carries an opaque `frameData` pointer (the sim state read
arrives with the sprite stages, M2-SPRITE-02; CORE-004). `GlContext`
gained `refreshRateHz()` (the display refresh rate for the frame
clock's vsync pace; 0 = headless or unavailable).

M2-GL-03 landed the camera/matrix utilities — the pure matrix builders in
namespace `laige::render` (public header `include/laige/render/
matrices.h`, implementation `matrices.cpp`): `ortho`, `perspective`,
`lookAt`, the 2D-plane camera `planeOrtho` (the top_down/side_view modes),
and the isometric family — `isoMatrix` (the general affine oblique form,
i.e. the "custom shear" preset), `isoDimetric2To1` (the ADR 0005 default)
and `isoTrueIso3060`. All are stateless, allocation-free, GL-free pure
functions (no context, no state, callable from any thread — the
set-up-phase camera objects M2-CAM-01/02 own the build cadence); NDC
conventions (right-handed world, +z up, OpenGL NDC z ∈ [−1, +1]) are
pinned in the header and the ADR-0005 preset tables are machine-checked
by the tests. GLM (the PRD §11 "math foundation for the rendering side")
is vendored at `deps/glm` and exposed through this module's public API
(the value types `Vec2`/`Vec3`/`Mat4` are GLM aliases — a deliberate
contrast to the GLFW/GLAD pimpl; the include edge is module-owned and
`tools/laige-include-lint` R3 keeps GLM out of every other module,
notably the sim). API contract in
[docs/api/matrices.md](../docs/api/matrices.md), vendoring decision in
[ADR 0008](../docs/decisions/0008-glm-vendoring.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `matrices` —
pure math, runs in every local tree and CI).

M2-ISO-01 landed the isometric depth key — the engine-owned 32-bit
sortable depth key for isometric render ordering (FR-2.2, PRD §4,
RENDER-003; the PRD §10.1 module map places "isometric depth keys" in
this module). Public header
`include/laige/render/iso_depth_key.h` (header-only — a template over
the SimMath backends, the `PresentationSnapshot` pattern):
`isoDepthKey<Backend>(pos, stepHeight, layer)` (the formula, bit layout,
domain, and shear contract live in the header preamble — the key is
`v = (x + y) − z` quantized at 1/16 world units, packed with the layer:
unsigned key order = back-to-front; the monotone ties-away rounding
never inverts the painter's order), `isoDepthKeyParts` (the exact
inverse — the diagnostics view), `isoDepthOrderLess` (the explicit
stable total render order `(key, entity id)` — the stable sort's
insertion order is the batcher's deterministic entity-id order, FR-1.2),
and `isoShearSupported` (the back-to-front contract:
`−dx.y == −dy.y == zUnit > 0` exactly — both built-in presets pass; a
custom shear must pass to use isometric depth sorting). The key is
world-space by contract (PRD §4: never screen space),
presentation-only (ARCH-009), O(1), allocation-free, no GL, no
per-call asserts (total; out-of-domain input saturates). The canonical
coordinate/depth/ordering document (ARCH-008) is
[docs/concepts/coordinates.md](../docs/concepts/coordinates.md) (created
with this step); API contract in
[docs/api/iso_depth_key.md](../docs/api/iso_depth_key.md); tests under
[tests/laige-render](../tests/laige-render) (CTest entry `iso_depth_key`
— pure math, runs in every local tree and CI: hand-computed golden keys
for a small stepped-terrain scene on both SimMath backends, the 10k
random scene's back-to-front property against the M2-GL-03 iso matrices,
the `(key, entity id)` total order and determinism, the domain/
saturation contract, and the supported-shear checker).

M2-ISO-02 landed the per-scene-chunk depth key table — the
precomputed, incrementally-updated tile-grid → depth-key map (FR-2.2:
"precomputed at scene build and incrementally updated on tile/height
changes", PRD §8.1 "10k dirty cells ≤ 0.2 ms"; the PRD §10.1 module map
places "isometric depth keys" in this module). Public header
`include/laige/render/iso_depth_table.h` (header-only — a template over
the SimMath backends, the `PresentationSnapshot` pattern):
`IsoDepthKeyTable<Backend>::create(options)` (validates the options,
pre-sizes one flat storage for the covered — chunk-aligned — region,
flat-ground init; setup path), `rebuild(heights)` (the from-scratch
load: every key through the full M2-ISO-01 function, so
`rebuild(final grid) == any edit sequence reaching the same grid`),
`setTile(gx, gy, h)` (the O(1) zero-allocation incremental update — the
edited cell plus its documented neighborhood, radius 0 for the M2-ISO-01
formula), `ensureChunk(gx, gy)` (bounded + logged growth, up to
`maxChunks` — `BudgetExhausted` beyond, one Debug event per created
chunk, one rate-limited Warn on the cap), `keyAt`/`tileHeightAt`/
`covers` (the render read path). The table is world-space,
presentation-only (ARCH-009), sim-phase writes / render-phase reads (the
frame pipeline's phase ordering, M2-GL-02), and its cells agree with
`isoDepthKey` bit-for-bit (the preamble derivation; the tests pin it).
The canonical narrative is
[docs/concepts/coordinates.md](../docs/concepts/coordinates.md) §4.5
(created with this step); API contract in
[docs/api/iso_depth_table.md](../docs/api/iso_depth_table.md); the PRD
§8.1 budget is recorded in
[docs/benchmarks/baselines/m2-iso-depth-table.md](../docs/benchmarks/baselines/m2-iso-depth-table.md)
(`budgets.json` entry `iso_depthkey_rebuild` — gated on the reference
platform, non-instrumented trees — methodology §4/§5; the sanitizer
trees run the same workload ungated). Tests under
[tests/laige-render](../tests/laige-render) (CTest entry
`iso_depth_table` — pure data, no GL environment required: option
validation, hand-computed golden keys for a 4×4 stepped scene on both
backends with the cross-backend equality, the single-tile-edit contract
(only the documented cell changes, last-write-wins, rejected edits
unchanged, zero-allocation window), the bounded + logged growth, the
rebuild-from-scratch == incremental property (64×64, 2k seeded edits),
and the 10k-dirty-cell budget workload).

M2-CAM-01 landed the 3D camera core — `laige::render::Camera`
(public header `include/laige/render/camera.h`, implementation
`camera.cpp`): the presentation-side camera of FR-2.4 — position,
look-at, ortho or perspective (FOV), zoom with clamping exact at the
bounds (min/max per scene config), the rectangular bounds constraint
(the camera position is clamped into the rectangle at creation, on
every `setPosition`, and after every follow step), smooth follow (the
per-update lerp factor — a rigid translation that converges the
look-at point to the follow target), and the bounded decaying shake
(offset clamped to `maxShakeOffset`, decaying to exactly `0.0` within
the documented 150-update bound at the default decay of 0.5). The
matrices delegate to the M2-GL-03 builders (`lookAt`/`ortho`/
`perspective`); the isometric presets and the grid-snap mode live on
top of the camera in the M2-CAM-02 isometric camera (below).
ARCH-009: the camera never touches
sim state (the follow target comes from the presentation state) and
its state is never part of replay state. A pure value object — no
heap storage, zero allocation on every operation, single owner (the
render set-up phase), no internal synchronization. API contract in
[docs/api/camera.md](../docs/api/camera.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `camera` —
pure value math, no GL environment required: the create validation
and initial clamp, the zoom clamp exact at the bounds, the bounds
under adversarial follow/shake input, the follow's deterministic exact
sequence, the shake's decay to exact zero at the documented tick
count, and the matrix builds against the M2-GL-03 builders).

M2-CAM-02 landed the isometric camera — `laige::render::IsoCamera`
(public header `include/laige/render/iso_camera.h`, implementation
`iso_camera.cpp`): the isometric camera of FR-2.4 on top of the
M2-CAM-01 camera (owned by value). The preset is a single config
value (the ADR 0005 config-only pattern) — 2:1 dimetric (the engine
default), true 30°/60°, or a custom shear validated against the
M2-ISO-01 `isoShearSupported()` checker at the config boundary
(unsupported shears are rejected: they would break the engine-owned
depth order, RENDER-003) — with the matrix constants from the M2-GL-03
builders (no preset has its own matrix code). `matrix()` is the
combined world→NDC affine matrix of the current state (the camera
center projects to the NDC origin at every zoom/position; NDC-z is 0
— depth is engine-owned, PRD §4). The grid-snap camera mode (the
documented continuous choice): the camera position's `(x, y)` are
quantized to the grid on create, on every `setPosition`, and after
every follow step (the camera never rests off the grid, mid-follow
included), with the documented dyadic zoom-level ladder
(`L = {zoomMin·2ⁿ ≤ zoomMax}`, nearest level in log2 space, exact
float tie to the higher zoom) — both invariants total inside the
documented world domain by the inflated look-at margin
(`maxShakeOffset + g·√2/2`); grid-snap + bounds require the bounds
rectangle to be grid-aligned (validated at create). ARCH-009:
presentation-only, never in sim/replay; determinism is bit-identical
per build/platform. A pure value object — no heap storage, zero
allocation on every operation. API contract in
[docs/api/iso_camera.md](../docs/api/iso_camera.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `iso_camera`
— pure value math, no GL environment required: the preset matrices
against the M2-GL-03 builders, the create validation, the on-the-grid
invariant under 2k adversarial mutation/follow inputs, the zoom-level
set exactly, and the bit-identical determinism replay).

M2-ISO-03 landed the isometric grid picking — the engine-owned, safe
screen → ground-plane → grid-cell transform (FR-2.11, PRD §4
click-to-select/move; S-5/G-R11: game code never inverts the iso
matrix itself). Header-only public header `include/laige/render/
iso_picking.h`: `IsoGridConfig` (the pick grid: `cellSize` world units
per cell), `IsoGridPick` (the cell indices + the computed ground
point), and `screenToGrid(screen, camera, grid)` — the O(1) inverse of
the M2-CAM-02 camera matrix (a 2×2 solve on the ground rows; no
per-pick 4×4 inverse, no allocation) with a `ProjectionView` overload
for hand-stored iso matrices (the M2-PROJ-01 base). The grid cells are
half-open (`[gx·g, (gx+1)·g)²`, the documented boundary rule); the
pick is exact at all supported zoom levels (a fixed float sequence —
zoom enters only through the matrix) with a documented precision zone
(`16·2⁻²⁴·κ·(|e| + |w|)` — ~6e-4 world units at scene scale); a
total function (non-finite screen saturates at ±32767; a stopped
camera picks with the identity matrix). Presentation-only,
deterministic per build, zero allocation. The PRD §8.1 `iso_picking`
budget (one pick mean ≤ 0.01 ms) is gated by the `iso_picking` ctest
entry (measured 0.000136262 ms — the sixth baseline,
[docs/benchmarks/baselines/m2-iso-picking.md](../docs/benchmarks/baselines/m2-iso-picking.md)).
API contract in
[docs/api/iso_picking.md](../docs/api/iso_picking.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `iso_picking`
— pure float math, no GL environment required).

The sprite batcher, draw submits, and sim→render wiring land in the
remaining M2 steps (M2-SPRITE-01/02 on).
