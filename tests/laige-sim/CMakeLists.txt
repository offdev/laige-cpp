# laige-sim tests (M1-ECS-01): entity handle + World entity storage.
#
# One executable per module (tests/README.md; docs/testing.md is the
# source of truth): laige-sim_tests links the module under test plus
# gtest_main. The unfiltered entry runs the whole module; the
# `entity` entry is M1-ECS-01's Verify command (`ctest -R entity`),
# selecting exactly the suites below from the shared executable.

set(LAIGE_SIM_TEST_SOURCES entity_tests.cpp)

add_executable(laige-sim_tests ${LAIGE_SIM_TEST_SOURCES})
laige_apply_engine_policy(laige-sim_tests)
# M1: the sim test executable carries the same pinned SimMath flag set
# as laige-sim (ADR 0002, PRD §10.3 — the sim test TUs join the policy
# list with the module itself).
laige_apply_simmath_policy(laige-sim_tests)

# Links the module under test plus the dev-only test framework;
# gtest_main provides main().
target_link_libraries(laige-sim_tests PRIVATE gtest_main laige-sim)

add_test(NAME laige-sim_tests COMMAND laige-sim_tests)

# M1-ECS-01: entity handle + World entity storage. The step's Verify
# command is `ctest -R entity`; this entry selects exactly the entity
# suites from the shared laige-sim_tests executable (the unfiltered
# entry above still runs them as part of the full module suite).
# The gtest filter is one UNQUOTED argument (see
# tests/laige-core/CMakeLists.txt for the rationale).
add_test(NAME entity
         COMMAND laige-sim_tests
         --gtest_filter=EntityHandle.*:WorldBasics.*:WorldCapacity.*:WorldLogging.*:WorldStale.*:WorldStats.*)

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2),
  # so ctest fails loudly on any TSan report.
  set_tests_properties(laige-sim_tests entity
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
