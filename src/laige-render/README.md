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

The sprite batcher, draw submits, and sim→render wiring land in the
remaining M2 steps (M2-SPRITE-01/02 on).
