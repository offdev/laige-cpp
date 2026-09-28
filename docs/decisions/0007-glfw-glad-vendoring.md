# ADR 0007 — Vendoring GLFW 3.5.1 and the GLAD 2.0.8-generated GL 3.3 loader

- **Status:** Accepted
- **Date:** 2026-09-28
- **Decider:** Roadmap step M2-GL-01
- **Refs:** PRD §11 (dependency table, windowing/GL row), §6 (OpenGL 3.3 core
  requirement), §8.3 (NFR-8.6, NFR-8.8), AGENTS DEP-003/DEP-004/DEP-005,
  roadmap M2-GL-01, ADR 0004 (the vendoring machinery this reuses)

## Context

M2-GL-01 lands the engine's GL infrastructure: a windowed and a headless
OpenGL 3.3 **core** context, the version gate, the capability query, and an
offscreen render target for CI. Two capabilities are needed:

1. **Windowing + context creation** on all P0 OSes (Windows 10/11 x64,
   Linux x64, macOS arm64+Intel) — creating a 3.3 core context with the
   right hints, hidden-window support, and clean error reporting.
2. **OpenGL function access** without the platform GL header
   mess: one loader that resolves every GL 3.3 core entry point on all
   P0 OSes (WGL/GLX/EGL load paths) so the engine compiles against a
   single, version-checked API surface.

PRD §11 pre-approves **GLFW** (windowing) and **GLAD** (GL loader) for
exactly these roles, so no PRD revision is required — only the DEP-003
documentation and the DEP-005 pinning, which this ADR and `deps.lock`
provide. NFR-8.8 forbids network access at build time, so both are
vendored in-tree and verified by the `deps.lock` tree hash on every
configure (ADR 0004 machinery).

## Decision

- Vendor **GLFW 3.5.1** (upstream tag commit
  `d9d6f0f1f967807ffade6598ea9a631ebaf37a56`) under `deps/glfw/` — the
  complete tagged tree (159 files, zlib license file included). Built as a
  static C library via its own CMake project with every dev-facing output
  off (`GLFW_BUILD_DOCS/EXAMPLES/TESTS/INSTALL = OFF`); it takes no engine
  language policy (pinned third-party code, the ADR 0004 treatment) and is
  not sanitizer-instrumented (the root CMake sets the sanitizer flags on
  `CMAKE_CXX_FLAGS` only — the ASan/TSan lanes cover the engine boundary,
  not GLFW internals).
- Vendor the **GLAD 2.0.8-generated output** under `deps/glad/`:
  `include/glad/gl.h`, `include/KHR/khrplatform.h`, `src/gl.c` — generated
  by `gladv2 --api gl:core=3.3 --reproducible c --loader` (glad2 v2.0.8,
  upstream repo commit `73db193f853e2ee079bf3ca8a64aa2eaf6459043`).
  What is vendored is the
  **generated output, not a source checkout**: regeneration from the same
  command is byte-identical (`--reproducible`), which is what the tree
  hash pins. The loader self-loads the platform GL library
  (`gladLoaderLoadGL()`: `opengl32.dll` / `libGL.so.1` /
   `OpenGL.framework`), so the engine has no GL header or link dependency
  of its own.
- `gl.c` compiles in its own C target (`laige-glad`, `src/laige-render/
  CMakeLists.txt`), PRIVATE to `laige-render`; GLFW links PRIVATE as
  well. The public header (`laige/render/gl_context.h`) names no vendor
  type (CPP-010, DEP-004 — the include edge is machine-checked by
  `tools/laige-include-lint`, R3).
- Pin both in `deps.lock` (tree SHA-256, source URL + commit, license,
  justification); the root `CMakeLists.txt` runs `laige_deps_verify_lock()`
  on every configure and fails loudly on any mismatch (CORE-008, DEP-005).
- The root `project()` enables `C` from M2-GL-01 on (the GLFW subproject
  and the `laige-glad` target are C targets at the root scope); engine
  targets remain C++-only and keep the engine policy verbatim.

## DEP-003 justification

- **Capability needed:** (a) a maintained, cross-P0 windowing/context
  library that creates exactly the 3.3 core context the PRD mandates, with
  hidden-window support (needed for Windows/macOS headless rendering) and
  no GPL component (the engine is MIT, NFR-1.2); (b) a GL function loader
  pinned to exactly GL 3.3 core so the engine's API surface cannot silently
  drift to a newer/older GL or to extension-only functions.
- **Alternatives considered:**
  - *Home-grown WGL/GLX/EGL + X11/Cocoa/Win32 windowing* — rejected per
    DEP-002: mature, security-adjacent platform-API work is not ours to
    reinvent; it would also triple the platform code the P0 matrix must
    keep green.
  - *SDL3 (windowing)* — viable but broader than needed (audio, events)
    and PRD §11 already lists GLFW; adding a second windowing stack's
    dependency cost buys nothing M2 uses.
  - *GLES + ANGLE (cross-platform via the Windows ANGLE runtime)* —
    rejected: ANGLE's driver support on the Linux P0 runner is not part of
    the pinned CI image, and GLES drops the desktop-GL surface the 2.5D
    renderer is specified against (PRD §6 names OpenGL 3.3 core).
  - *Direct GL headers (`GL/gl.h` + platform WGL/GLX)* — rejected: no
    single 3.3-only header surface exists per OS, extension availability
    becomes platform-conditional, and the engine loses the loader's
    single entry point for "which GL did we actually get".
  - *GLM / other loader alternatives* — GLAD is the PRD §11 choice and its
    generated output is a deterministic artifact (reproducible
    regeneration), which makes vendoring it a hash-pinned file set
    rather than a dependency on a code-generation tool at build time.
- **Transitive dependencies:** GLFW: none beyond the system windowing
  stacks it wraps (X11/Wayland on Linux, Win32 on Windows, Cocoa on macOS —
  system-provided on every P0 image). GLAD: none (one C file, no
  dependencies; the loader `dlopen`s the system GL driver library at
  runtime). Build impact: GLFW adds one static C library (≈160 files,
  seconds of build time); GLAD adds one C object.
- **Platforms / health / license:** GLFW — maintained by the GLFW project
  (15+ years, CMake-native, the de-facto standard C windowing library);
  zlib license (permissive, MIT-compatible). GLAD — the glad2 generator
  (actively maintained, the standard GL loader generator); the generated
  C is licensed `WTFPL OR CC0-1.0 AND Apache-2.0` (the header carries the
  combined license text); all permissive, MIT-compatible.
- **Security:** both parse no untrusted input in our usage; GLFW is
  security-maintained (public CVE history is handled by upstream releases —
  the pin is re-audited on every upgrade, DEP-005).
- **Upgrade/removal strategy:** re-vendor the new tag / regenerate with the
  same glad2 command, recompute the tree hashes, update `deps.lock`,
  re-run CI (DEP-005). Neither crosses a public API boundary (both sit
  behind the `GlContext` pimpl), so upgrades cannot break game consumers.

## Evidence

- Vendored trees verified: `deps.lock` hash check passes at configure
  time ("laige-deps: deps.lock OK — 3 vendored dependency(ies) verified");
  tampering any vendored file makes the configure fail with the expected
  vs actual hash (repro: modify one file under `deps/glfw/` and
  reconfigure in a scratch tree).
- Warning-clean under the engine policy in the **GCC 16.2.1** and
  **Clang 22.1.8** Debug trees (2026-09-28); GLFW/gl.c compile under the
  plain C policy with zero diagnostics. MSVC 2022 and AppleClang
  verification is the CI P0 matrix (GPU-free configure/build lanes).
- The GL smoke test (`tests/laige-render`, CTest entry `gl_context`)
  creates a headless offscreen context and round-trips a clear color
  through an RGBA8 FBO; it is green on the P0 CI runners and
  `GTEST_SKIP`s with the clean `Status` reason on a machine without a
  usable GL (documented environment contract, `docs/api/gl_context.md`).
  Per the sandbox rule, no GPU workload runs locally — the smoke's local
  status is "skips cleanly", verified by the GL-free suites
  (`GlContextGate`, `GlContextArgs`), which pass in every local tree.

## Consequences

- `laige-render` (and its `laige-glad` C target) become the only engine
  targets that touch GL; the GLFW/GLAD surface stays behind the
  `GlContext` pimpl, so M2-GL-02+ renderer code never sees vendor types.
- Headless CI rendering is now the documented mechanism: **Linux CI
  renders through an EGL surfaceless context** (Mesa's
  `EGL_MESA_platform_surfaceless`; `libEGL.so.1` loaded at runtime — no
  build-time EGL dependency); **Windows/macOS headless uses a never-shown
  GLFW window** (`GLFW_VISIBLE = false`). Details: `docs/api/
  gl_context.md` §Headless mechanism.
- The root project is `LANGUAGES C CXX` from M2-GL-01 on; engine targets
  remain C++-only (the C sources live in the vendored targets).
- GL function access is via `glad_glX` names (the generated loader's
  symbols); `gl_context.cpp` is the only engine TU that includes
  `<glad/gl.h>` or `<GLFW/glfw3.h>`.

## Review conditions

- Upgrade GLFW/GLAD only via a DEP-005 change (new tag or regeneration →
  new tree hash → lock update → CI green); no public API impact is
  possible.
- Revisit the Windows/macOS hidden-window mechanism if a P0 image gains a
  supported headless EGL surfaceless path — the Linux mechanism is
  already the surfaceless one, and the engine's `createHeadless` API does
  not change either way.
- If the engine ever needs GL > 3.3 (it currently must not — PRD §6),
  regenerate GLAD with the new version and bump the gate in
  `laige::render::checkGlVersion` in one change (both are pinned by this
  ADR's review).
