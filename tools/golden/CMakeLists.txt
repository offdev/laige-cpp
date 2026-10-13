# laige-golden (M2-GOLD-01): the golden image test infrastructure.
#
# The tool renders the committed golden scene fixtures offscreen at
# the canonical 1280x720 frame (the M2-SPRITE-02 sprite scene + the
# M2-UI-02 UI scene — tests/golden/golden_fixtures.h), captures the
# frame (GlContext::readPixels, one glReadPixels, rows flipped to the
# top-origin / y-down convention), and compares it against the
# committed reference PNG (tools/golden/references) with the documented
# tolerance (max channel diff 2, 0.001% pixel budget — explicit in the
# entry's flags, never hidden; docs/testing.md). `--update` re-renders
# and rewrites the references — the human-approved regeneration path
# (the golden-update workflow; never run under ctest).
#
# Gated with the test suite like tools/scene-iso: a library-only build
# does not need it.

add_executable(laige-golden laige-golden.cpp)
laige_apply_engine_policy(laige-golden)
target_link_libraries(laige-golden PRIVATE laige-render laige-stb)
# The vendor PNG headers' DECLARATIONS (the implementations live in
# laige-stb — the render tests' deps/glad include precedent) and the
# shared golden headers (the fixtures + the comparison core) plus the
# house env-var helper.
target_include_directories(laige-golden PRIVATE
  ${CMAKE_SOURCE_DIR}/deps/stb
  ${CMAKE_SOURCE_DIR}/tests/golden
  ${CMAKE_SOURCE_DIR}/tests/support)

# The step's Verify command in executable form (roadmap: "ctest -R
# golden" — the unanchored regex also selects the GL-free
# `golden_compare` entry in tests/golden, intended). The tolerance
# flags are the documented M2-GOLD-01 tolerance, passed explicitly
# (TEST-008: tolerances are config, not a hidden constant). The
# `--references` path is the committed reference directory. Exit 77 =
# no usable OpenGL 3.3 environment (the documented environment
# contract, the GTEST_SKIP analog for a plain executable — the
# macOS runners have no usable GL, so the entry skips there).
add_test(NAME golden
         COMMAND laige-golden
                 --references ${CMAKE_SOURCE_DIR}/tools/golden/references
                 --max-channel-diff 2
                 --pixel-budget-pct 0.001)
set_tests_properties(golden PROPERTIES
  TIMEOUT 300
  SKIP_RETURN_CODE 77
  ENVIRONMENT "LAIGE_TEST_FONT_PATH=${CMAKE_SOURCE_DIR}/tests/laige-render/assets/vera.ttf")

if(LAIGE_TSAN)
  # The tool's GL teardown ordering under TSan (the render_timing
  # precedent: owner-thread state, single-threaded GL work — the entry
  # gates the engine's GL teardown). The ENVIRONMENT replaces the
  # entry's, so the font path is restated here.
  set_tests_properties(golden PROPERTIES ENVIRONMENT
    "TSAN_OPTIONS=halt_on_error=1;LAIGE_TEST_FONT_PATH=${CMAKE_SOURCE_DIR}/tests/laige-render/assets/vera.ttf")
endif()

if(LAIGE_ASAN)
  # The tool creates the same headless context the GL smokes do: the
  # libglvnd EGL vendor state is process-lifetime by design and LSan
  # reports it at exit (the laige-render ASan block's rationale).
  # detect_leaks=0 for this entry; in-run ASan/UBSan stays fully
  # active. The ENVIRONMENT replaces the entry's, so the font path is
  # restated here.
  set_tests_properties(golden PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0:log_path=${CMAKE_SOURCE_DIR}/asan-reports/asan-golden;LAIGE_TEST_FONT_PATH=${CMAKE_SOURCE_DIR}/tests/laige-render/assets/vera.ttf")
endif()
