# laige-render — 2.5D rendering module (PRD §10.1).
#
# Dependency rule (PRD §10.1, NFR-8.11): laige-render sits above
# laige-core and laige-sim in the module stack and may include only
# their PUBLIC headers — enforced by tools/laige-include-lint (R2,
# arrows only downward). M2-GL-01 links laige-core (public headers use
# laige::Result/Status). M2-GL-02 adds the frame pipeline (render
# thread + lock-free handoff + frame clock) with NO render→sim edge:
# the handoff carries a POD FrameDescriptor plus an opaque frameData
# pointer (the sim state read — the PresentationSnapshot — happens in
# the pipeline stages, M2-SPRITE-01/02 onward; CORE-004: no
# speculative wiring).
#
# M2-GL-01 (roadmap/M2-rendering-2.5d.md, ADR 0007): the GL
# infrastructure — GLFW 3.5.1 (windowing, compiled, vendored per PRD
# §11) and the GLAD 2.0.8-generated GL 3.3 core loader (vendored
# generated output). The target lands the GlContext (windowed +
# headless offscreen, the 3.3 core gate, the capability query); the
# render thread + frame handoff land with M2-GL-02, the sprite batcher
# and draw submits with the remaining M2 steps.
#
# GLFW: added as a subdirectory with every dev-facing output disabled —
# examples/tests/docs/install are off (vendored, never installed,
# NFR-8.8). GLFW is a C library; it takes no engine language policy
# (pinned third-party code, DEP-005 — the same treatment as the
# vendored GoogleTest) but it does inherit the whole-tree sanitizer
# instrumentation for C++ objects; its own C objects are not
# instrumented (the root CMake sets the sanitizer flags on
# CMAKE_CXX_FLAGS only — the ASan/TSan lanes therefore cover the
# engine boundary, not GLFW internals).
#
# GLAD: the generated loader (deps/glad) has no CMake project, so its
# one C source compiles in its own C target (laige-glad, below). It
# keeps the plain compiler policy (pinned generated code, DEP-005 — the
# same treatment as the vendored GLFW) and its headers are PRIVATE:
# public headers never expose vendor code (CPP-010, DEP-004; the include
# edge is machine-checked by tools/laige-include-lint, R3).
set(GLFW_BUILD_DOCS OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_INSTALL OFF)
# Backend selection (ADR 0007): X11 only on Linux.
# - The Wayland backend is OFF: it requires wayland-scanner
#   (wayland-protocols) at CONFIGURE time — a system package the P0 CI
#   runners (ubuntu-24.04) do not carry — and it buys no P0 coverage:
#   GLFW's X11 backend reaches Wayland sessions through XWayland.
# - GLFW 3.5 resolves the X11 and GL libraries at RUNTIME (dlopen +
#   dlsym inside the X11/GLX backend), so only the X11 *headers* are a
#   configure-time requirement (the CI workflows install the six
#   libx*-dev packages; see .github/workflows).
set(GLFW_BUILD_X11 ON)
set(GLFW_BUILD_WAYLAND OFF)
add_subdirectory(${CMAKE_SOURCE_DIR}/deps/glfw ${CMAKE_BINARY_DIR}/deps/glfw)

add_library(laige-glad STATIC ${CMAKE_SOURCE_DIR}/deps/glad/src/gl.c)
target_include_directories(laige-glad PRIVATE
  ${CMAKE_SOURCE_DIR}/deps/glad/include)

set(LAIGE_RENDER_SOURCES gl_context.cpp frame_pipeline.cpp matrices.cpp)

if(LAIGE_BUILD_SHARED)
  add_library(laige-render SHARED ${LAIGE_RENDER_SOURCES})
else()
  add_library(laige-render STATIC ${LAIGE_RENDER_SOURCES})
endif()

laige_apply_engine_policy(laige-render)

# Public internal dependency: laige-core (the public header uses
# laige::Result/Status, CPP-010).
target_link_libraries(laige-render PUBLIC laige-core)
# GLFW and the GLAD loader are implementation detail behind a pimpl
# (the public header never names vendor types) — PRIVATE.
target_link_libraries(laige-render PRIVATE glfw laige-glad)
# GLAD's generated headers (used by gl_context.cpp, never by public
# headers), likewise PRIVATE.
target_include_directories(laige-render PRIVATE
  ${CMAKE_SOURCE_DIR}/deps/glad/include)

# GLM (M2-GL-03, ADR 0008): unlike GLFW/GLAD (opaque native resources
# behind the pimpl), GLM is a pure value-type math library and is part
# of the render-side public API (PRD §11: "math foundation for the
# rendering side" — cameras, projection matrices). laige/render/
# matrices.h includes GLM headers, so the GLM include root is PUBLIC
# (build interface): consumers get GLM transitively, pinned by
# deps.lock. The include edge stays module-owned: tools/laige-include-
# lint (R3) records deps/glm as owned by laige-render, so no other
# module (notably laige-sim, whose math stays SimMath per PRD §10.3)
# may include GLM directly.
target_include_directories(laige-render PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/deps/glm>)

# Public header root. Only this directory is exposed to consumers
# (CPP-010: no transitive leakage).
target_include_directories(laige-render PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
