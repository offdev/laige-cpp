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

M2-SORT-01 landed the deterministic depth sort — the stable,
deterministic, pre-allocated sorter for the frame's 32-bit isometric
depth keys (FR-2.2 "no per-frame allocation"; RENDER-003; S-5/G-R11:
the engine owns the render ordering). Header-only public header
`include/laige/render/depth_sort.h`: `DepthSort` (`create(capacity)`
— one flat 16 B/slot allocation at scene set-up — and `sort(keys)` —
O(4n + 4·256) per frame, zero allocation, no logging, no GL). The
algorithm is 4 × 8-bit LSD radix (stable bucket) passes (one stable
256-bucket counting sort per 8-bit digit, LSB first — Knuth TAOCP
Vol. 3 §7.2.1); the stable tie-break (RENDER-003) is the sort's
stability plus the batcher's deterministic entity-id insertion order
(the (key, entity id) total order of `iso_depth_key.h`, without the
sorter ever seeing the ids). The sorted order is read back as
`sortedKeys()`/`sortedIndices()` (parallel spans: the i-th key in
back-to-front order + its original input position) — payload-
agnostic, the M2-SPRITE-01 batcher maps indices to its sprite pool.
Overflow is `BudgetExhausted` with the sorter unchanged; the default
state is the empty (capacity-0) stopped sorter. Presentation-only,
bit-identical deterministic per platform/build. The PRD §8.1
`depth_sort_10k` budget (10k keys sorted mean ≤ 1.0 ms — 6% of the
60 FPS frame budget, half of the 2 ms 50k render-CPU budget) is gated
by the `depth_sort` ctest entry (measured 0.225852 ms — the seventh
baseline,
[docs/benchmarks/baselines/m2-depth-sort.md](../docs/benchmarks/baselines/m2-depth-sort.md)).
API contract in
[docs/api/depth_sort.md](../docs/api/depth_sort.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `depth_sort`
— pure integer math, no GL environment required).

M2-SPRITE-01 landed the sprite item + batcher — the engine-owned
"declare, don't draw" declaration window + batch builder (S-5: scene
content is declared, the engine batches; FR-2.1: one draw call per
(atlas, material, blend) group per frame). Header-only public header
`include/laige/render/sprite_batcher.h`: `SpriteItem` (the declared
sprite — world position, the M2-ISO-01 depth key, UV sub-rect, the
animation frame index (M2-SPRITE-03), rotation, scale, tint, blend,
atlas/material refs, the G-R11 depth-override flag) in a budgeted,
accounted `ArenaPool` (`SpriteBatcher`, `create(Options{maxSprites})`
— ~136 B per capacity slot, 6.8 MB at the 50k stress budget); the
frame protocol
`beginFrame()` → `add(item) × n` → `build()`. `build()` sorts the
frame's depth keys with the M2-SORT-01 `DepthSort`, groups into
(atlas, material, blend) batches in deterministic order (ascending
(atlas, material, blend) — RENDER-003), and scatters each group's
instances in global back-to-front order (the sorted order restricted
to the group — the (key, entity id) total order per group, the
entity-id insertion order FR-1.2 carries). The overflow policy is
bounded drop-oldest + warn (PERF-008, S-2); the per-sprite depth
override is counted + warned per frame (G-R11, "prefer tile height");
every per-frame path allocates nothing (PERF-003 — the
zero-allocation proof). The submit stage (M2-SPRITE-02) reads
`batches()` — one instanced draw call per group. No standalone
`budgets.json` entry: the sort cost is the `depth_sort_10k` budget and
the composite 50k render-CPU budget is measured with the submit stage
(M2-PERF-01). API contract in
[docs/api/sprite_batcher.md](../docs/api/sprite_batcher.md), tests
under [tests/laige-render](../tests/laige-render) (CTest entry
`batcher` — pure integer bookkeeping, no GL environment required).

M2-SPRITE-02 landed the GPU instanced draw submits — the frame
pipeline's SUBMIT stage: the minimal GLSL 3.30 sprite shader (per
instance: world position + world-unit scale, UV sub-rect, screen-space
rotation, multiplicative tint; one atlas texture) and ONE
`glDrawArraysInstanced` per (atlas, material, blend) group per frame
(FR-2.1, RENDER-001). `SpriteRenderer` (`include/laige/render/sprite_renderer.h`,
`sprite_renderer.cpp`): `create(const GlContext&, Options{maxInstances,
maxAtlases, primitiveQuery})` (one shader program, one quad VBO, one
per-frame instance buffer sized `maxInstances * 52` B, one VAO — no
per-frame allocation, PERF-003), `bindAtlas(id, w, h, rgba)` (the
set-up/asset path — one GL texture per atlas, `GL_NEAREST`,
`GL_CLAMP_TO_EDGE`), and `submit(batcher, worldToNdc)` (the per-frame
draw: frame validation — the batcher's `frameBuilt()`, the instance
budget, every group's atlas bound — the one `glBufferSubData` upload,
then the per-group texture bind / blend function / instanced draw; a
failed submit draws nothing, counts nothing, and leaves no stale
sprite-pass state). The observable counters (`SpriteDrawStats` /
`SpriteDrawTotals`: draw calls == group count, texture binds, blend
changes, instances, the opt-in `PRIMITIVES_GENERATED` primitives —
extended in M2-SPRITE-04 with the profiler's per-frame fields: program
changes, upload volume, render-target use, the texture-memory VRAM
estimate, and the G-R2 draw-call-cap flag) feed the M2-PROF-01
profiler. `GlContext::frameBuffer()` exposes the
offscreen FBO the pass binds per frame (the surfaceless default frame
buffer is not a valid draw target; the pass also sets the viewport).
The offscreen 1 000-sprite render is verified against a CPU reference
rasterizer (±1-byte per channel + rounding-exact probes — the GPU
float32 vs the reference double; the rotation's cos/sin is
driver-float, pinned by M2-GOLD-01). API contract in
[docs/api/sprite_renderer.md](../docs/api/sprite_renderer.md), tests
under [tests/laige-render](../tests/laige-render) (CTest entry
`sprite_draw` — a usable OpenGL 3.3 environment required: Mesa
software GL on the CI offscreen path; the suites self-
`GTEST_SKIP` on an environment failure). No standalone `budgets.json`
entry: the composite 50k render-CPU budget is measured with this stage
(M2-PERF-01).

M2-SPRITE-03 landed the atlas UV frame animation hook — the
data-driven half of FR-2.1's "atlas UV animation (sheet frames)": the
atlas sheet frame LAYOUT (`SpriteFrameLayout`: frame size, row/col,
the inter-frame spacing + the sheet-edge margin, all in texels — the
documented sheet model: row-major, frame 0 at the top-left, tight
sheet `2·border + cols·fw + (cols−1)·spacing`) and the pure
frame-index → UV sub-rect computation (`spriteFrameUv(frameIndex,
layout, atlasWidth, atlasHeight)` — O(1), zero allocation,
float-exact at atlas dimensions ≤ 2^24, the documented `u1 > u0`,
`v1 > v0` invariant; out-of-range frame → `InvalidArgument`, never
wrap). `SpriteItem` gained the animation frame index (`frameIndex` —
the caller sets it, the batcher carries it through untouched; the
caller also sets `uv` to the frame's UV rect — M3 animation will
drive the frame advance on top of this same layout). Header-only
`include/laige/render/sprite_frames.h`; API contract in
[docs/api/sprite_frames.md](../docs/api/sprite_frames.md), tests
under [tests/laige-render](../tests/laige-render) (CTest entry
`sprite_frames` — pure float/integer math, no GL environment
required). No standalone `budgets.json` entry: the per-frame
conversion cost is part of the composite 50k render-CPU budget,
measured with M2-PERF-01.

M2-SPRITE-04 landed render observability + the draw-call budget —
the M2-SPRITE-04 profiler fields on top of the M2-SPRITE-02 counters
(G-R2, PRD §9.3): `SpriteDrawStats` gained `programChanges`,
`uploadBytes` (the frame's instance upload, n × 52 B),
`renderTargetBytes` (the render-target size drawn, w × h × 4), and the
G-R2 `drawCallCapExceeded` flag; `SpriteDrawTotals` gained the
`programChanges`, `uploadBytes`, `renderTargetBytes`, and
`capExceededFrames` sums. `SpriteRenderer::Options` gained the
configurable per-pass draw-call cap (`maxDrawCalls`, documented
default `kSpriteRendererDefaultDrawCalls` = 64 — 2× the PRD §8.1
worst-case reference-scene budget of 30 draw calls; domain
`[1, kSpriteRendererMaxInstances]`); an over-cap frame fires one
rate-limited Warn `draw_call_cap` (fields `capacity`, `draw_calls`),
sets the flag, and is STILL drawn — observation, never an execution
gate (the G-R5 precedent). The new `textureMemoryBytes()` gauge
reports the bound atlases' `w × h * 4` sum (the texture-memory VRAM
estimate — updated at each `bindAtlas`, the replacement subtracts the
old upload). API contract in
[docs/api/sprite_renderer.md](../docs/api/sprite_renderer.md) (the
"Render observability + the draw-call cap" section), tests under
[tests/laige-render](../tests/laige-render) (CTest entry
`render_counters` — `RenderCountersCreate` is GL-free, the scene/cap/
memory suites require a usable OpenGL 3.3 environment and self-
`GTEST_SKIP` on an environment failure). No standalone `budgets.json`
entry: the counters are O(1) bookkeeping and the composite 50k
render-CPU budget is measured with this stage (M2-PERF-01).

M2-TILE-01 landed the tilemap — `laige::render::TileMap<Backend>`
(public header `include/laige/render/tilemap.h`, header-only — a
template over the SimMath backends): the chunked tile grid of
FR-2.6 (chunks, per-tile depth/height, auto-depth). The tilemap OWNS
one `IsoDepthKeyTable<Backend>` (M2-ISO-02) — the tile's HEIGHT lives
in the table alone (one source of truth), and the remaining per-tile
data (`textureId`, `animationId` — 0 = a static tile; 1..maxAnimations
= the animation slot, M2-TILE-02) lives in one flat pre-sized array
(8 B/tile, the requested grid). The AUTO-DEPTH wiring:
`setTile(gx, gy, textureId, height, animationId)` routes the height
into the table's `setTile` (the table recomputes exactly that cell's
key — the M2-ISO-02 incremental update, radius 0), and `rebuild(tiles)`
loads the whole grid through the table's from-scratch `rebuild`
(`rebuild(final grid) == any edit sequence reaching the same grid` —
the M2-ISO-02 property, pinned through the tilemap). The batch path
(S-5): `declareTo(batcher, options)` declares the static tile quads
into the sprite batcher — one `SpriteItem` per tile (the tile's CENTER
pos, scale (1,1), the table's key (auto-depth — the game never writes
it, G-R11), the tile's texture as `atlasId`, the fixed-frame UV), in
the grid's row-major order (RENDER-003's deterministic insertion
order) — and the batcher's (atlas, material, blend) grouping renders
the tilemap in a BOUNDED number of draw calls: one per distinct
(textureId, material, blend) combination (FR-2.1, RENDER-001 — tiles
of one chunk sharing one texture and blend form one group).
ARCH-009: headless-buildable, presentation-only, sim-phase writes /
render-phase reads; the declare loop allocates NOTHING per frame
(FR-2.2 — the zero-allocation proof, the tests). Rejected operations
leave the tile data AND the table unchanged (the `Status` is the
failure channel — LOG-002). API contract in
[docs/api/tilemap.md](../docs/api/tilemap.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `tilemap` —
pure data + batcher bookkeeping, no GL environment required: the grid
options + flat/empty contract, the per-tile data writes / rejected
edits / scene load + the rebuild-from-scratch == incremental property,
the height → depth-table wiring (a height edit changes exactly the
edited cell's key), the hand-computed 4×4 chunk's quad positions +
depth goldens + the grouping and cross-frame determinism, the frame
protocol / failure paths / custom options / no-log happy path, and the
1000-frame zero-allocation declare loop). No standalone `budgets.json`
entry: the per-frame declare cost is part of the composite 50k
render-CPU budget, measured with M2-PERF-01.

M2-PAR-01 landed the parallax layers —
`laige::render::ParallaxLayers<Backend>` (public header
`include/laige/render/parallax.h`, header-only — a template over the
SimMath backends): the named bg/mid/fg model of FR-2.3. Each layer is
a WORLD-SPACE RECTANGLE (a texture, or a tilemap — the M2-TILE-02
hook, data-only in M2-PAR-01) whose content position at camera
position `p` is the EXACT formula `worldOffset(p) = factor * (p -
center) + offset` (factor 0 = fixed in world space, 1 = fixed on
screen — one source of truth). The layer's quads carry the M2-ISO-01
key's LAYER field (engine-owned, G-R11): the presets are
`kParallaxDepthLayerBackground` = -2, `kParallaxDepthLayerMidground`
= -1, `kIsoDepthGroundLayer` = 0 (the ground),
`kParallaxDepthLayerForeground` = +1 — WITHIN a shared (atlas,
material, blend) group the layer field dominates (background first,
engine-guaranteed); ACROSS groups the draw order is the batcher's
group order, so the scene's SET-UP assigns the layers' atlas ids
background-below / foreground-above the world content (the
M2-TILE-01 texture-id convention). The UV scroll (auto or manual)
carries a per-layer offset in [0, 1)² with the EXACT wrap at the
texture boundary (`wrap(x) = x - floor(x)` — 1.0 → exactly 0.0),
rendered through the 2 x 2 wrap split (a single SpriteItem carries one
UV rect — no wrap): up to four quads per layer, one draw call per
layer group (FR-2.1). ARCH-009: headless-buildable, presentation-only
(the layer state reads the camera's presentation position, never sim
state); the per-frame `advanceScrolls` + `declareTo` loop allocates
NOTHING (FR-2.2 — the zero-allocation proof, the tests). Rejected
definitions leave the slot unchanged (one rate-limited
`parallax/layer_invalid` warn with the failing field — LOG-002/004).
API contract in [docs/api/parallax.md](../docs/api/parallax.md),
tests under [tests/laige-render](../tests/laige-render) (CTest entry
`parallax` — pure data + batcher bookkeeping, no GL environment
required: the registry create/stopped state, the `setLayer`
validation matrix with the pinned warn fields, the EXACT offset
formula (hand-computed dyadic goldens), the auto/manual UV scroll with
the EXACT wrap, the hand-computed golden keys of the 4-layer + ground
scene (both backends — the dyadic exactness zone) with the group
(draw) order and the layer-dominance orderings, the 2 x 2 wrap split's
exact world/UV rects (incl. the atlas sub-rect mapping), the frame
protocol / stopped / disabled / tilemap-hook paths, cross-frame
determinism, and the 1000-frame zero-allocation declare loop). No
standalone `budgets.json` entry: the per-frame declare cost is part of
the composite 50k render-CPU budget, measured with M2-PERF-01 (the
M2-SCENE-01 reference scene has 3 parallax layers within the 50k-sprite
/ ≤30-draw-call budget).

M2-TILE-02 landed the tilemap's tile animation and parallax tile
layer — extensions to `laige::render::TileMap<Backend>` (same header,
header-only). **Tile animation** (data-driven frame cycling, FR-2.6):
an ANIMATION is a slot of the tilemap's pre-sized animation table
(ids 1..`maxAnimations`; 0 = the static sentinel) — `setAnimation(id,
def)` (setup/config path) sets the frame COUNT (`frameCount`,
[1, `kTileAnimMaxFrames` = 64]), the documented TICK RATE (`frameTicks`
— the SIMULATION ticks per frame), and the tile SHEET's frame layout
(the M2-SPRITE-03 `SpriteFrameLayout`, texels — the tight sheet). The
scene owner calls `advanceAnimations()` ONCE PER SIM TICK (ARCH-002 —
per sim tick, never per render frame): every set animation's frame
steps every `frameTicks` ticks, wrapping at `frameCount`
(`frame(ticks) = (ticks / frameTicks) mod frameCount`); all tiles of
one animation share its phase (per-tile offsets are the M3 editor's
control). The frame UVs are PRECOMPUTED at `setAnimation` (one setup
allocation); the per-frame declare path only READS them (FR-2.2 — no
per-frame allocation). The declared quad's frame fields: the STATIC
tile carries `DeclareOptions::uv` + `frameIndex` 0; the ANIMATED tile
carries its animation's CURRENT frame UV + `frameIndex`. The frame
state is presentation state (ARCH-009 — never in the sim state hash).
**The parallax tile layer** (the M2-PAR-01 Tilemap-source hook):
`declareTo(batcher, options, layers, layerId, cameraPos)` declares the
tilemap's quads UNDER the layer — every quad TRANSLATED by the
layer's `worldOffset(cameraPos)` (the M2-PAR-01 formula (1)), and its
key is the M2-ISO-01 key of the TRANSLATED center at the TILEMAP's own
`Options::layer` (the scene-setup convention: the layer's def
`depthLayer` equals it — bg/mid/fg tilemaps get -2/-1/+1). The
translated keys are computed per tile per frame (the camera-dependent
translation is not precomputable — O(tileCount), zero allocation).
Protocol (first failure wins, nothing declared, no log): a built
frame's closed window, an unset layer id, a non-Tilemap-source layer
→ `InvalidArgument`; a DISABLED layer declares NOTHING (OK). Rejected
`setAnimation`/`setTile`/`rebuild` leave the slots/data/table
unchanged; an animated tile whose slot is UNSET fails the declare.
ARCH-009: headless-buildable, presentation-only; the per-tick
`advanceAnimations` + per-frame `declareTo` loops (standalone +
parallax) allocate NOTHING (FR-2.2 — the zero-allocation proof, the
tests). API contract in
[docs/api/tilemap.md](../docs/api/tilemap.md), tests under
[tests/laige-render](../tests/laige-render) (CTest entry `tilemap_anim`
— pure data + batcher bookkeeping, no GL environment required: the
`setAnimation` validation matrix (slot domain, frame count / tick
rate / sheet domains, the tight-sheet float-exact domain with the
adversarial-layout overflow guards, the rejected-set-leaves-no-state
+ phase-reset contract, no-log happy path), the frame cycle at the
documented rate (hand-computed `frame = ticks / frameTicks mod
frameCount`), the animated/static frame fields on the declared quads
(hand-computed frame-UV goldens from the M2-SPRITE-03 tight-sheet
formula, the frameIndex, the grouping, the wrap + cross-frame
determinism), the parallax tile layer's hand-computed key goldens at
given camera positions (both backends — the dyadic exactness zone) +
the layer-dominance ordering + the protocol paths (built frame /
unset id / non-tilemap source / disabled layer) + the animated tile
under the translation, the animationId edit/load domain + the
unset-slot declare failure, and the 1000-frame advance + declare
zero-allocation loop (standalone + parallax)). No standalone
`budgets.json` entry: the per-frame declare + per-tick advance cost is
part of the composite 50k render-CPU budget, measured with M2-PERF-01.

The remaining M2 steps (text/UI, M2-PERF-01) land in later steps.
