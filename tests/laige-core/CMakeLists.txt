# laige-core tests (M0-BUILD-01): build smoke test.
#
# Verifies that the laige-core library links in the selected variant
# (static by default, shared with LAIGE_BUILD_SHARED=ON; NFR-8.9) and that
# the NFR-8.10 language policy actually reached this target. GTest-based
# unit suites arrive with each M0-CORE-xx step (GoogleTest is vendored in
# M0-DEP-01).

add_executable(laige-core_tests laige-core_tests.cpp)
laige_apply_engine_policy(laige-core_tests)

# Links the library under test; the link itself is the static/shared check.
target_link_libraries(laige-core_tests PRIVATE laige-core)

# Tells the smoke test which variant it is verifying.
if(LAIGE_BUILD_SHARED)
  target_compile_definitions(laige-core_tests PRIVATE LAIGE_CORE_IS_SHARED=1)
endif()

add_test(NAME laige-core_tests COMMAND laige-core_tests)

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2),
  # so ctest fails loudly on any TSan report.
  set_tests_properties(laige-core_tests
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
