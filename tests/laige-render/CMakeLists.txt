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
# (IsoDepthKeyShears) — pure math: no GL environment needed; and the
# per-scene-chunk depth key table (M2-ISO-02) — the precomputed,
# incrementally-updated tile-grid -> depth-key map (IsoDepthTable*:
# option validation + the flat-ground contract, the hand-computed
# golden scene, the single-tile-edit / rejected-edit / zero-allocation
# update contract, the bounded + logged chunk growth, the rebuild ==
# incremental property, and the PRD §8.1 10k-dirty-cell budget gate)
# — pure data: no GL environment needed; and the 3D camera core
# (M2-CAM-01) — the presentation-side Camera (camera_tests.cpp's
# Camera* suites: the create validation and initial clamp, the zoom
# clamp exact at the bounds, the rectangular bounds under adversarial
# follow/shake input, the smooth follow's deterministic exact sequence,
# the shake's bounded decay to exactly zero within the documented
# tick count, and the matrix builds against the M2-GL-03 builders) —
# pure value math: no GL environment needed.
#
# One executable per module (tests/README.md): laige-render_tests links
# the module under test plus gtest_main. The unfiltered entry runs the
# whole module; the `gl_context` entry is the M2-GL-01 Verify command
# (`ctest -R gl_context`), the `render_thread` entry is the M2-GL-02
# Verify command (`ctest -R render_thread`), the `matrices` entry is
# the M2-GL-03 Verify command (`ctest -R matrices`), the
# `iso_depth_key` entry is the M2-ISO-01 Verify command
# (`ctest -R iso_depth_key`), the `iso_depth_table` entry is the
# M2-ISO-02 Verify command (`ctest -R iso_depth_table`), and the
# `camera` entry is the M2-CAM-01 Verify command (`ctest -R camera`),
# each selecting exactly its suites from the shared executable.
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
  iso_depth_key_tests.cpp iso_depth_table_tests.cpp camera_tests.cpp)
laige_apply_engine_policy(laige-render_tests)
target_link_libraries(laige-render_tests PRIVATE gtest_main laige-render)
# The randomized suites draw through the test-only seed helper
# (docs/testing.md §4; tests/support/laige_test_seed.h).
target_include_directories(laige-render_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/support)

add_test(NAME laige-render_tests COMMAND laige-render_tests)
# The unfiltered entry also runs the M2-ISO-02 budget suite (in the
# gated builds it checks the entry), so it carries the same budgets
# path:
set_tests_properties(laige-render_tests PROPERTIES
  ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")

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

# M2-ISO-02: the step's Verify command is `ctest -R iso_depth_table`.
# Pure data (no GL calls) — runs in every local tree and CI. The
# LAIGE_BUDGETS_PATH env var points the budget suite at the repo
# budgets.json (the test falls back to the working directory when the
# var is unset — the budget_harness_tests.cpp convention). The budget
# suite runs ONLY on the non-instrumented trees (methodology §4: the
# budget gates measure the canonical Debug tree — the sanitizer
# runtimes inflate absolute cost and own operator new); it
# GTEST_SKIPs elsewhere. The absolute 0.2 ms target is reference-
# platform-scoped (methodology §5: the shared macOS/Windows runners
# are slower and noisier than the ubuntu-24.04 reference — the
# m1-sim-tick scoping), so the gate is compiled only on Linux in the
# non-instrumented trees (the LAIGE_ISO_DEPTH_BUDGET define marks
# them); other platforms still run the budget WORKLOAD (ungated —
# the suite's other suites cover the same shapes, and the workload
# itself runs leak-free under ASan/TSan):
add_test(NAME iso_depth_table
         COMMAND laige-render_tests --gtest_filter=IsoDepthTable*)
set_tests_properties(iso_depth_table PROPERTIES
  ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")

# M2-CAM-01: the step's Verify command is `ctest -R camera`. Pure value
# math (no GL calls) — runs in every local tree and in CI.
add_test(NAME camera
         COMMAND laige-render_tests --gtest_filter=Camera*)

if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(laige-render_tests PRIVATE LAIGE_ISO_DEPTH_BUDGET=1)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "")
    target_compile_definitions(laige-render_tests PRIVATE
      LAIGE_ISO_DEPTH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
  endif()
endif()

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2).
  # The M2-GL-02 `render_thread` entry is the step's TSan gate (the
  # lock-free handoff's race-freedom, the 3000-frame no-deadlock run);
  # the pure-math `matrices`, `iso_depth_key`, and `iso_depth_table`
  # entries carry the option for uniformity (they have no shared state,
  # but the flag is harmless). The test-level ENVIRONMENT replaces the
  # job-level one, so the budgets path is restated here.
  set_tests_properties(laige-render_tests gl_context render_thread matrices
    iso_depth_key iso_depth_table camera PROPERTIES
    ENVIRONMENT
    "TSAN_OPTIONS=halt_on_error=1;LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")
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
# slow headless runner generous headroom. The pure-math `matrices`,
# `iso_depth_key`, and `camera` entries are small (thousands of O(1)
# computations, comparisons, and O(1) camera updates at most) — 60s is
# generous headroom.
set_tests_properties(laige-render_tests gl_context PROPERTIES TIMEOUT 180)
set_tests_properties(render_thread PROPERTIES TIMEOUT 300)
set_tests_properties(matrices PROPERTIES TIMEOUT 60)
set_tests_properties(iso_depth_key PROPERTIES TIMEOUT 60)
set_tests_properties(camera PROPERTIES TIMEOUT 60)
# The `iso_depth_table` entry includes the budget gate: 2 backends x
# (100 warm-up + 3000 measured) iterations of 10 000 setTile calls —
# a few seconds on the canonical Debug tree; 120s is generous headroom
# for a slow CI runner.
set_tests_properties(iso_depth_table PROPERTIES TIMEOUT 120)
