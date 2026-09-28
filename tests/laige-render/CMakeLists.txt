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

# The offscreen smoke creates a real (software) context + FBO; give a
# slow headless runner generous headroom.
set_tests_properties(laige-render_tests gl_context PROPERTIES TIMEOUT 180)
