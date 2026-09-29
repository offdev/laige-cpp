# laige-render tests (M2-GL-01): the GL context contract — the pure
# 3.3 core gate (GlContextGate), the creation argument validation and
# the stopped-context state (GlContextArgs, no GL required), and the
# offscreen render smoke (GlContextSmoke: the headless offscreen FBO
# frame round trip, the move semantics, and the windowed path's clean
# failure on a headless host).
#
# One executable per module (tests/README.md): laige-render_tests links
# the module under test plus gtest_main. The unfiltered entry runs the
# whole module; the `gl_context` entry is the M2-GL-01 Verify command
# (`ctest -R gl_context`), selecting exactly the GlContext* suites from
# the shared executable.
#
# Environment note: the GlContextSmoke suite requires a usable OpenGL
# 3.3 environment — always present on the P0 CI runners (Mesa/ANGLE/
# Apple/Windows software or hardware drivers), where it must pass. On a
# local machine without a GL driver the suite GTEST_SKIPs with the clean
# Status reason — the documented environment contract (docs/api/
# gl_context.md), not an engine failure. The GlContextGate and
# GlContextArgs suites always run (they make no GL calls).

add_executable(laige-render_tests gl_context_tests.cpp)
laige_apply_engine_policy(laige-render_tests)
target_link_libraries(laige-render_tests PRIVATE gtest_main laige-render)

add_test(NAME laige-render_tests COMMAND laige-render_tests)

# M2-GL-01: the step's Verify command is `ctest -R gl_context`. The
# gtest filter is one UNQUOTED argument (see
# tests/laige-core/CMakeLists.txt for the rationale).
add_test(NAME gl_context
         COMMAND laige-render_tests --gtest_filter=GlContextGate.*:GlContextArgs.*:GlContextSmoke.*)

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2).
  set_tests_properties(laige-render_tests gl_context
    PROPERTIES
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
  # the engine never holds a pointer to. detect_leaks=0 for the two
  # render entries instead: in-run ASan/UBSan error detection stays
  # fully active (the smoke still performs its GL work under the
  # sanitizer). The test-level ENVIRONMENT replaces the job-level
  # ASAN_OPTIONS (GitHub workflow), so mirror its options here.
  set_tests_properties(laige-render_tests gl_context PROPERTIES
    ENVIRONMENT
    "ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0:log_path=${CMAKE_SOURCE_DIR}/asan-reports/asan")
endif()

# The offscreen smoke creates a real (software) context + FBO; give a
# slow headless runner generous headroom.
set_tests_properties(laige-render_tests gl_context PROPERTIES TIMEOUT 180)
