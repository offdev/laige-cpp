# laige-core tests (M0-BUILD-01; GTest-ified by M0-DEP-01): build smoke test.
#
# Verifies that the laige-core library links in the selected variant
# (static by default, shared with LAIGE_BUILD_SHARED=ON; NFR-8.9) and that
# the NFR-8.10 language policy actually reached this target. Running this
# suite also exercises the GoogleTest plumbing end to end (configure →
# build → ctest) — the M0-DEP-01 verify path.

add_executable(laige-core_tests laige-core_tests.cpp)
laige_apply_engine_policy(laige-core_tests)

# Links the library under test (the link itself is the static/shared check)
# plus the dev-only test framework; gtest_main provides main().
target_link_libraries(laige-core_tests PRIVATE gtest_main laige-core)

# Stamps two facts the smoke test cross-checks at compile time (NFR-8.9,
# CORE-008): LAIGE_CORE_IS_SHARED is set only when laige-core is actually
# built shared; LAIGE_EXPECT_SHARED is what the build tree selected. A
# mismatch between them fails the build loudly.
if(LAIGE_BUILD_SHARED)
  target_compile_definitions(laige-core_tests PRIVATE
    LAIGE_CORE_IS_SHARED=1 LAIGE_EXPECT_SHARED=1)
else()
  target_compile_definitions(laige-core_tests PRIVATE LAIGE_EXPECT_SHARED=0)
endif()

add_test(NAME laige-core_tests COMMAND laige-core_tests)

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2),
  # so ctest fails loudly on any TSan report.
  set_tests_properties(laige-core_tests
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()

# M0-CI-02 Verify scratch (INTENTIONALLY FAILING; see sanitizer-scratch.cpp).
# The deliberate OOB read must turn the linux-asan CI lane red (UBSan bounds
# report, fatal via -fno-sanitize-recover=all) while the linux-tsan lane
# stays green. Delete this block and the .cpp file after the CI cycle.
add_executable(sanitizer-scratch sanitizer-scratch.cpp)
laige_apply_engine_policy(sanitizer-scratch)
target_link_libraries(sanitizer-scratch PRIVATE gtest_main laige-core)
add_test(NAME sanitizer-scratch COMMAND sanitizer-scratch)

if(LAIGE_TSAN)
  # Same fatal-report wiring as the main suite: should stay green for this
  # scratch (an OOB read is not a data race), but any race would fail hard.
  set_tests_properties(sanitizer-scratch
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
