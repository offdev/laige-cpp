# laige-render tests: the GL context contract (M2-GL-01) — the pure
# 3.3 core gate (GlContextGate), the creation argument validation and
# the stopped-context state (GlContextArgs, no GL required), and the
# offscreen render smoke (GlContextSmoke: the headless offscreen FBO
# frame round trip, the move semantics, and the windowed path's clean
# failure on a headless host); and the frame pipeline (M2-GL-02) — the
# vsync-paced frame clock (FrameClock, no GL), the single-slot lock-free
# handoff (RenderThreadHandoff, no GL), and the offscreen pipeline runs
# (RenderThreadOffscreen: the 3000-frame no-deadlock integration run and
# the slowed-submit drop path); and the matrix utilities (M2-GL-03) —
# the camera/projection builders pinned against hand-computed golden
# values (Matrices*, pure math: no GL environment needed); and the
# isometric depth key (M2-ISO-01) — the engine-owned 32-bit sortable
# depth key pinned against hand-computed golden keys for a small
# stepped-terrain scene (IsoDepthKeyGolden), the 10k random scene's
# back-to-front property against the iso projection matrices
# (IsoDepthKeyProperty), the (key, entity id) total order and
# determinism (IsoDepthKeyTieBreak), the domain/saturation contract
# (IsoDepthKeyDomain), and the supported-shear checker
# (IsoDepthKeyShears) — pure math: no GL environment needed.
#
# One executable per module (tests/README.md): laige-render_tests links
# the module under test plus gtest_main. The unfiltered entry runs the
# whole module; the `gl_context` entry is the M2-GL-01 Verify command
# (`ctest -R gl_context`), the `render_thread` entry is the M2-GL-02
# Verify command (`ctest -R render_thread`), the `matrices` entry is
# the M2-GL-03 Verify command (`ctest -R matrices`), and the
# `iso_depth_key` entry is the M2-ISO-01 Verify command
# (`ctest -R iso_depth_key`), each selecting exactly its suites from
# the shared executable.
#
# Environment note: the GlContextSmoke and RenderThreadOffscreen suites
# require a usable OpenGL 3.3 environment — always present on the P0 CI
# runners (Mesa/ANGLE/Apple/Windows software or hardware drivers), where
# they must pass. On a local machine without a GL driver the suites
# GTEST_SKIP with the clean Status reason — the documented environment
# contract (docs/api/gl_context.md), not an engine failure. The
# GlContextGate/GlContextArgs/FrameClock/RenderThreadHandoff suites
# always run (they make no GL calls).

add_executable(laige-render_tests
  gl_context_tests.cpp render_thread_tests.cpp matrices_tests.cpp
  iso_depth_key_tests.cpp)
laige_apply_engine_policy(laige-render_tests)
target_link_libraries(laige-render_tests PRIVATE gtest_main laige-render)
# The randomized suites draw through the test-only seed helper
# (docs/testing.md §4; tests/support/laige_test_seed.h).
target_include_directories(laige-render_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/support)

add_test(NAME laige-render_tests COMMAND laige-render_tests)

# M2-GL-01: the step's Verify command is `ctest -R gl_context`. The
# gtest filter is one UNQUOTED argument (see
# tests/laige-core/CMakeLists.txt for the rationale).
add_test(NAME gl_context
         COMMAND laige-render_tests --gtest_filter=GlContextGate.*:GlContextArgs.*:GlContextSmoke.*)

# M2-GL-02: the step's Verify command is `ctest -R render_thread`.
# FrameClock + RenderThreadHandoff run without any GL environment;
# RenderThreadOffscreen needs the same usable OpenGL 3.3 environment as
# GlContextSmoke (GTEST_SKIPs where absent — the documented
# environment contract).
add_test(NAME render_thread
         COMMAND laige-render_tests
         --gtest_filter=FrameClock.*:RenderThreadHandoff.*:RenderThreadOffscreen.*)

# M2-GL-03: the step's Verify command is `ctest -R matrices`. Pure math
# (no GL calls) — runs in every local tree and CI.
add_test(NAME matrices
         COMMAND laige-render_tests --gtest_filter=Matrices*)

# M2-ISO-01: the step's Verify command is `ctest -R iso_depth_key`.
# Pure math (no GL calls) — runs in every local tree and CI.
add_test(NAME iso_depth_key
         COMMAND laige-render_tests --gtest_filter=IsoDepthKey*)

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2).
  # The M2-GL-02 `render_thread` entry is the step's TSan gate (the
  # lock-free handoff's race-freedom, the 3000-frame no-deadlock run);
  # the pure-math `matrices` and `iso_depth_key` entries carry the
  # option for uniformity (they have no shared state, but the flag is
  # harmless).
  set_tests_properties(laige-render_tests gl_context render_thread matrices
    iso_depth_key PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()

if(LAIGE_ASAN)
  # The headless smoke's EGL display is process-lifetime by design
  # (docs/api/gl_context.md; CORE-009): the libglvnd dispatcher does not
  # export eglDestroyDisplay, so the Mesa driver state reachable through
  # the display is still live at process exit — LSan reports it as a leak
  # (2026-09-29 CI run 36501951395: ~1.88 MB across 17,781 allocations,
  # every stack allocated through createHeadless / createFbo /
  # finishCreation / readPixel). That is vendor state, not engine
  # memory: every engine-owned allocation in this module is freed by
  # RAII before exit. It cannot be suppressed surgically — LSan's
  # `leak:` suppression lines abort ASan's own suppression parser at
  # init (mutually exclusive formats in one file, verified against
  # compiler-rt), and the leak roots are libEGL-internal state objects
  # the engine never holds a pointer to. detect_leaks=0 for the render
  # entries instead (render_thread's RenderThreadOffscreen suite creates
  # the same headless context): in-run ASan/UBSan error detection stays
  # fully active (the smoke still performs its GL work under the
  # sanitizer). The test-level ENVIRONMENT replaces the job-level
  # ASAN_OPTIONS (GitHub workflow), so mirror its options here.
  set_tests_properties(laige-render_tests gl_context render_thread PROPERTIES
    ENVIRONMENT
    "ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0:log_path=${CMAKE_SOURCE_DIR}/asan-reports/asan")
endif()

# The offscreen smoke creates a real (software) context + FBO; give a
# slow headless runner generous headroom. The pure-math `matrices` and
# `iso_depth_key` entries are small (the 10k-scene property test is a
# few thousand O(1) key computations and comparisons) — 60s is
# generous headroom.
set_tests_properties(laige-render_tests gl_context PROPERTIES TIMEOUT 180)
set_tests_properties(render_thread PROPERTIES TIMEOUT 300)
set_tests_properties(matrices PROPERTIES TIMEOUT 60)
set_tests_properties(iso_depth_key PROPERTIES TIMEOUT 60)
