# laige-fuzz — deterministic bounded fuzz runner (M0-CORE-07, minimal
# form; M0-TEST-01 extends it: CI lane semantics, nightly long runs,
# seed-handling documentation).
#
# Gated with LAIGE_BUILD_TESTS (root CMakeLists): a dev tool that links
# the engine library — a library-only build does not need it.

add_executable(laige-fuzz laige-fuzz.cpp)
laige_apply_engine_policy(laige-fuzz)
target_link_libraries(laige-fuzz PRIVATE laige-core)

# Bounded fuzz in every commit (PRD §14: "every commit (bounded), nightly
# (long)"): 1000 deterministic inputs to the json_parse target (NFR-8.7,
# ADR 0003). In an ASan tree this entry is the M0-CORE-07 Verify gate —
# the run is instrumented, so any crash/UB fails ctest loudly.
add_test(NAME fuzz_json_parse COMMAND laige-fuzz json_parse --runs=1000)
set_tests_properties(fuzz_json_parse PROPERTIES TIMEOUT 120)
if(LAIGE_TSAN)
  # Same fatal-race policy as the module test entries (NFR-8.2).
  set_tests_properties(fuzz_json_parse PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
