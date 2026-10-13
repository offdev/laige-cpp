# laige-golden tests (M2-GOLD-01): the GL-free half of the golden image
# test infrastructure.
#
# `laige-golden-tests` builds the unit suites of the comparison core
# (golden_compare.h — the documented tolerance model) and the committed
# golden scene fixtures (golden_fixtures.h — the M2-SPRITE-02 and
# M2-UI-02 scene shape invariants at the canonical 1280x720 frame). No
# GL anywhere (the offscreen render + PNG comparison is the tool's job,
# ctest entry `golden` in tools/golden); the UI fixture half reads the
# committed test font (the LAIGE_TEST_FONT_PATH env var — the font
# suites' ENVIRONMENT contract).

add_executable(laige-golden-tests golden_compare_tests.cpp)
laige_apply_engine_policy(laige-golden-tests)
target_link_libraries(laige-golden-tests PRIVATE gtest_main laige-render)
target_include_directories(laige-golden-tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tests/support)

# The step's GL-free Verify half: the comparison core + the fixture
# shape invariants (the `golden` entry in tools/golden is the GL half;
# `ctest -R golden` selects both — the unanchored regex, the documented
# tilemap/tilemap_anim precedent). The 120 s timeout bounds the UI
# fixture's one-time font rasterization (the 1024x1024 glyph atlas) on
# a slow headless runner, like the same-shaped font entries.
add_test(NAME golden_compare
         COMMAND laige-golden-tests
                 --gtest_filter=GoldenCompare*:GoldenFixtures*)
set_tests_properties(golden_compare PROPERTIES
  TIMEOUT 120
  ENVIRONMENT "LAIGE_TEST_FONT_PATH=${CMAKE_SOURCE_DIR}/tests/laige-render/assets/vera.ttf")

if(LAIGE_TSAN)
  # First data race report fatal (NFR-8.2); the ENVIRONMENT replaces
  # the entry's, so the font path is restated here.
  set_tests_properties(golden_compare PROPERTIES ENVIRONMENT
    "TSAN_OPTIONS=halt_on_error=1;LAIGE_TEST_FONT_PATH=${CMAKE_SOURCE_DIR}/tests/laige-render/assets/vera.ttf")
endif()
