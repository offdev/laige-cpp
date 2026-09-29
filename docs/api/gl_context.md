# GL context (`laige::render::GlContext`)

The OpenGL 3.3 core context contract (M2-GL-01; PRD §6, §11; ADR 0007;
AGENTS ARCH-003, API-008, NFR-8.6). Public header:
`src/laige-render/include/laige/render/gl_context.h`; implementation:
`src/laige-render/gl_context.cpp`. Unit suites: `ctest -R gl_context`
(`tests/laige-render/gl_context_tests.cpp`) — `GlContextGate` and
`GlContextArgs` run without any GL environment; `GlContextSmoke` needs a
usable OpenGL 3.3 environment (always present on the P0 CI runners). On
success the smoke prints one machine-greppable line in the ctest output
(the `docs/testing.md` machine-line convention):

```text
gl-smoke: kind=headless size=32x32 gl=<major>.<minor> core=<0|1> max_texture_size=<n> clear_readback=ok
```

The engine requires **OpenGL 3.3 core profile** and never falls back:
a driver that offers an older version or the compatibility profile
yields `GlVersionUnsupported`, and a driver that offers no usable
context at all yields `GlUnavailable` (error registry:
`docs/api/errors.md#gl-unavailable`,
`docs/api/errors.md#gl-version-unsupported`). There is no silent
fallback profile, no silent version downgrade (PRD §6).

## The API

| Operation | Behavior | Complexity / allocation |
|---|---|---|
| `GlContext()` | A stopped context: `valid()` is false, every operation returns `InvalidArgument` | O(1), no allocation |
| `GlContext::createWindowed(w, h, title)` (static) | A GLFW window of `w x h` pixels titled `title` plus a 3.3 core context on it; the creating thread owns the context. `w`/`h` outside `[1, kMaxContextDimension]` (16384) or a null/empty `title` → `InvalidArgument`; no windowing backend / failed context / unloadable GL library → `GlUnavailable`; not-3.3-core driver → `GlVersionUnsupported` | one-time setup: window + context + GLAD load + capability query + (one log line) |
| `GlContext::createHeadless(w, h)` (static) | A display-less offscreen context of `w x h` pixels (mechanism per OS below); the render target is an offscreen RGBA8 FBO owned by the context. Same argument validation; `GlUnavailable` additionally covers the EGL surfaceless platform being unavailable (Linux) and the FBO failing its completeness check | one-time setup: context + FBO (2 GPU allocations) + GLAD load + capability query |
| `valid()` | False when stopped (default-constructed or moved-from) | O(1) |
| `version()` | The version the driver realized (`GlVersion{major, minor}`), guaranteed ≥ 3.3 core when valid. Precondition: valid() | O(1) |
| `capabilities()` | The creation-time snapshot: version, core-profile flag, `maxTextureSize` (GL 3.3 core guarantees ≥ 4096), and the driver's `vendor`/`renderer` strings (clamped to 127 chars — driver text is untrusted, LOG-005). Precondition: valid() | O(1) |
| `width()` / `height()` | The render-target size (the window, or the FBO). Precondition: valid() | O(1) |
| `makeCurrent()` | Bind the context to the calling thread (one thread current at a time, CONC-001). The M2-GL-02 render thread calls this on takeover; calling it on the already-current thread is a no-op success. Failure: `GlUnavailable` (one structured Error event, `gl/context_make_current_failed`) | O(1), no allocation |
| `clear(r, g, b, a)` | Clear the render target (the FBO on headless contexts, the window frame buffer on windowed ones) to an RGBA color; components are clamped to `[0, 1]` before the RGBA8 conversion (1.0 → 255, 0.25 → 64, 0.75 → 191 — the conversion is exact for these values). Precondition: valid() **and** the context is current on the calling thread — otherwise `InvalidArgument` (a precondition violation, not an engine failure: no log, no GL work) | O(1), no allocation; one `glBindFramebuffer` per call (see Performance) |
| `readPixel(x, y, rgba[4])` | Read one RGBA8 pixel; `(0, 0)` is bottom-left (the GL convention). Precondition: valid(), the context current, and `(x, y)` within `[0, w) x [0, h)` — otherwise `InvalidArgument` | O(1), no allocation (one `glReadPixels` of 4 bytes) |

Move-only: `GlContext` is move-constructible/assignable; the source is
stopped by the move (its native window/context — and, headless, its FBO —
are released when the destination dies). No copies (CPP-006; the native
GL objects have exactly one owner).

**Ownership and lifetime.** A `GlContext` owns its native context and its
render target end to end: `createWindowed`/`createHeadless` produce a
fully constructed object or a `Status`; the destructor (or a move)
destroys the window/context and — headless — the FBO and its texture.
GPU-side state (textures, framebuffers) dies with the context: there is
no separate teardown step. The context is created on the creating
thread and current on it by construction; any other thread must call
`makeCurrent()` before use (CONC-001, one thread current at a time).

**Failure behavior (NFR-008 / CORE-008).** Every creation failure is a
returned `Status` plus exactly one structured Error event,
`gl/context_creation_failed`, carrying `kind` (windowed/headless), a
machine-stable `reason` (`glfw_init`, `window_create`, `egl_load`,
`egl_surfaceless`, `egl_init`, `egl_context`, `egl_make_current`,
`gl_library_load`, `version_unsupported`, `fbo_incomplete`), and the
registry line for the returned code. Success logs one Info event,
`gl/context_created` (kind, version, size — driver strings are kept out
of the log, LOG-005).

**Threading and phase.** Context creation is a setup-phase operation
(one per process in the engine's design — M2-GL-02 runs exactly one
render thread against it). `makeCurrent`/`clear`/`readPixel` may be
called from the thread that owns the context at the moment of the call;
the GL functions they wrap are not reentrant across threads (that is the
GL contract `makeCurrent` enforces). No engine locks are held across
GL calls (CONC-003).

## Headless mechanism (the exact per-OS contract)

`createHeadless` renders with no display. The mechanism is fixed per
OS so that CI can run the GL smoke on every P0 runner:

- **Linux (the P0 CI path): an EGL surfaceless context.** The engine
  loads `libEGL.so.1` at runtime (`dlopen` — the engine has **no
  build-time or header-time EGL dependency**; a missing library is a
  clean `GlUnavailable`, not a build failure) and resolves the small
  EGL 1.5 function set by name (`eglGetPlatformDisplay`,
  `eglInitialize`, `eglBindAPI`, `eglCreateContext`, `eglMakeCurrent`,
  `eglGetCurrentContext`, `eglDestroyContext`, and `eglGetProcAddress`
  — plus, **optional**, `eglDestroyDisplay`: the libglvnd dispatcher
  that P0 Ubuntu exposes as `libEGL.so.1` does not export it, so its
  absence is not a failure; when absent the display's resources are
  released at process termination, which matches the engine's
  one-display-per-process design (the display's lifetime is the
  process's). Consequence for the ASan CI lane: that vendor state is
  live at process exit by design, so the two render test entries run
  with `detect_leaks=0` there (`tests/laige-render/CMakeLists.txt`;
  in-run ASan/UBSan error detection stays fully active — the smoke
  still performs its GL work under the sanitizer). `eglGetProcAddress`
  is required: both the libglvnd
  dispatcher and Mesa's vendor library export it (verified against the
  P0 distro's symbol tables), and the headless GL load resolves the GL
  API through it (see "GL function access"). `eglBindAPI(EGL_OPENGL_API)`
  is called **before** `eglInitialize` (the spec's API-selection
  order) and its result is checked: the spec guarantees no failure for
  a valid API enum, so a failure means a broken EGL stack (an ES-only
  or mismatched dispatcher) — a clean `GlUnavailable`
  (`reason=egl_context`), never a silent fallback. The display is
  created for **`EGL_PLATFORM_SURFACELESS_MESA` (0x31DD,
  `EGL_MESA_platform_surfaceless`)** — no display server, no window,
  no surface — and the context is created with no config and no
  surface: `eglCreateContext(display, EGL_NO_CONFIG, NULL,
  {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
  EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
  EGL_NONE})`, then `eglMakeCurrent(display, NULL, NULL, context)`.
  The render target is the offscreen FBO. This is the mechanism the
  Linux CI runner uses (Mesa provides the surfaceless platform).
- **Windows / macOS: a never-shown GLFW window.**
  `glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE)` + the 3.3 core hints +
  `glfwCreateWindow(w, h, "laige (offscreen)", NULL, NULL)` +
  `glfwMakeContextCurrent`. GLFW owns the platform windowing; the
  window is never presented and the render target is the offscreen
  FBO.
- **Windowed (all P0 OSes):** a normal GLFW window with the same 3.3
  core hints; the render target is the window's frame buffer. On Linux
  the GLFW build carries the **X11 backend only** (`GLFW_BUILD_WAYLAND=
  OFF`, ADR 0007): Wayland sessions are reached through XWayland, and
  GLFW 3.5 loads the X11 libraries at runtime (`dlopen` — no X11 link
  dependency). A Linux host with no X server (the P0 CI runners) yields
  a clean `GlUnavailable` from `createWindowed`; the CI GL smoke there
  runs the headless path. The no-display detection is a fast-fail probe
  (`DISPLAY` unset → `GlUnavailable`/`glfw_init` before GLFW is
  initialized): GLFW 3.5's X11 init failure path leaks process-global
  Xlib state (upstream), which the engine's fatal-on-any-leak ASan CI
  lane would otherwise turn into a failed job.

In every case the FBO (headless) is an RGBA8 texture
(`glTexImage2D(GL_RGBA8)`) attached to a single framebuffer, created
once per context, and completeness-checked (`glCheckFramebufferStatus`
must be `GL_FRAMEBUFFER_COMPLETE` or creation fails with
`GlUnavailable`).

## GL function access

The engine calls GL through the GLAD 2.0.8-generated loader
(`glad_glX` symbols; ADR 0007). `gl_context.cpp` is the only engine TU
that includes `<glad/gl.h>` or `<GLFW/glfw3.h>` — public headers name
no vendor type (CPP-010, DEP-004; include-lint R3). The GL entry
points are resolved per backend:

- **GLFW backends** (windowed on all P0 OSes; headless on
  Windows/macOS): GLAD's built-in loader (`gladLoaderLoadGL()`) —
  correct for the native interface of each platform (WGL / GLX /
  Cocoa).
- **EGL backend** (headless on Linux): GLAD's built-in loader is
  GLX-flavored on Linux (it dlopens `libGL.so.1` and resolves through
  `glXGetProcAddressARB`), which cannot serve an EGL surfaceless
  context — so the engine loads GLAD with its own userptr loader
  (`gladLoadGLUserPtr`) backed by the context's `eglGetProcAddress`,
  the EGL 1.5 mechanism for resolving a context's GL API. The context
  is current on the creating thread before the load (GLAD's version
  detection requires it).

GLAD's `gl:core=3.3` generation deliberately contains no GL 3.2 "NP"
(meta-context) functions such as `glGetCurrentContext`; the
"current-on-this-thread" check in `clear`/`readPixel` therefore uses
the platform layer directly (`glfwGetCurrentContext()` for the GLFW
path, `eglGetCurrentContext()` for the EGL path).

## Performance

**Setup path (once).** `createWindowed`/`createHeadless`: O(1) engine
work plus the platform's context creation; headless adds two GPU
allocations (FBO texture + framebuffer) sized `w x h x 4` bytes —
bounded by `kMaxContextDimension^2 x 4` (≈ 1 GiB at 16384×16384, a
setup-path budget; the engine's actual targets are scene-sized,
typically ≤ 2 GiB of frame storage across all targets per the M2
budgets). One Info log line on success, one Error line on failure
(LOG-003: both off the hot path).

**Per-frame path.** `clear` is O(1), no allocation, no CPU/GPU
synchronization (the GL call queues to the GPU; nothing blocks,
RENDER-005/PERF-002). `readPixel` is the one deliberately synchronous
call: a single-pixel `glReadPixels` forces a pipeline flush — it is a
test/smoke primitive, not a per-frame operation (TEST-008). The
per-frame draw path (M2-SPRITE-02) binds its FBO once per frame instead
of per call; `clear`/`readPixel` here bind per call because they are
the M2-GL-01 frame primitives, not the batched submit path.

**No hot-path logging, no hot-path allocation.** `version()`/
`capabilities()`/`width()`/`height()`/`valid()` are pure reads of the
creation-time snapshot — O(1), no allocation, no GL call (PERF-003,
PERF-007: O(1) documented).

**Misuse warnings.**

- Calling `clear`/`readPixel` before `makeCurrent` (on a thread that
  does not own the context) returns `InvalidArgument` — it is a
  precondition query, not an error: no log, no GL work.
- `clear`/`readPixel` on a stopped context (default-constructed or
  moved-from) return `InvalidArgument` for the same reason.
- Out-of-bounds `readPixel` returns `InvalidArgument`; the bounds are
  the render-target size, checked before any GL call.
- Two `GlContext`s in one process are supported by the API (each owns
  its own context) but the engine's design uses exactly one
  (M2-GL-02); multiple contexts multiply GPU memory and driver
  context-switch cost.

## Performant example

```cpp
// Setup (once, e.g. in the render module's start):
auto r = laige::render::GlContext::createHeadless(2560, 1440);
if (r.isError()) {
  // r.error() is GlUnavailable or GlVersionUnsupported — report via the
  // registry line (laige::errorText) and the gl/context_creation_failed
  // event already logged; there is no fallback to attempt.
  return;
}
laige::render::GlContext gl = std::move(r).takeValue();

// Per frame (the render thread, after makeCurrent on takeover):
gl.clear(0.0f, 0.0f, 0.0f, 1.0f);          // O(1), queued, no sync
// ... M2-SPRITE-02 batched draw submits ...
// gl.readPixel is NOT a per-frame operation (it flushes the pipeline).
```
