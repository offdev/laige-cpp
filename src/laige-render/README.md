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
producer, one consumer, the seqlock argument, the backpressure drop
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

The sprite batcher, draw submits, and sim→render wiring land in the
remaining M2 steps (M2-SPRITE-01/02 on).
