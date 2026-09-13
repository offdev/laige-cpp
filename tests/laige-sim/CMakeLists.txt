# laige-sim tests (M1-ECS-01/02/03/04): entity handle + World entity
# storage, component type registry, archetype SoA storage, query API +
# iteration legality.
#
# One executable per module (tests/README.md; docs/testing.md is the
# source of truth): laige-sim_tests links the module under test plus
# gtest_main. The unfiltered entry runs the whole module; the `entity`,
# `component_registry`, `archetype`, and `query` entries are the
# M1-ECS-01, M1-ECS-02, M1-ECS-03, and M1-ECS-04 Verify commands
# (`ctest -R entity`, `ctest -R component_registry`, `ctest -R archetype`,
# `ctest -R query`), selecting exactly the suites below from the shared
# executable.

set(LAIGE_SIM_TEST_SOURCES entity_tests.cpp component_registry_tests.cpp
                           archetype_tests.cpp query_tests.cpp)
# M1-ECS-03: the test-only allocation counter overrides the global
# operator new/new[]; the sanitizer runtimes define their own
# new/delete (strong symbols in the Clang/GCC TSan runtime archives,
# interposed by ASan), so the counter is excluded from the sanitizer
# trees. There, the step's zero-allocation property is covered by the
# leak-free sanitizer run of the same churn loop plus the
# ArchetypeStats reservation-delta assertion (the same fallback
# pattern as tests/laige-core, M0-CORE-02/05).
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN)
  list(APPEND LAIGE_SIM_TEST_SOURCES logging_alloc_counter.cpp)
endif()

add_executable(laige-sim_tests ${LAIGE_SIM_TEST_SOURCES})
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN)
  target_compile_definitions(laige-sim_tests PRIVATE LAIGE_ALLOC_COUNTER=1)
endif()
laige_apply_engine_policy(laige-sim_tests)
# M1: the sim test executable carries the same pinned SimMath flag set
# as laige-sim (ADR 0002, PRD §10.3 — the sim test TUs join the policy
# list with the module itself).
laige_apply_simmath_policy(laige-sim_tests)

# The M1-ECS-03 churn order draws from the shared test seed helper
# (docs/testing.md §4, M0-TEST-01: seeded laige::Prng substreams).
target_include_directories(laige-sim_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/support)

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

# M1-ECS-02: component type registry. The step's Verify command is
# `ctest -R component_registry`; this entry selects exactly the
# ComponentTypeId/ComponentRegistry suites from the shared
# laige-sim_tests executable.
add_test(NAME component_registry
         COMMAND laige-sim_tests
         --gtest_filter=ComponentTypeId.*:ComponentRegistry.*)

# M1-ECS-03: archetype SoA storage. The step's Verify command is
# `ctest -R archetype`; this entry selects exactly the Archetype*
# suites from the shared laige-sim_tests executable (the churn test's
# machine-greppable stats line lands in the ctest output).
add_test(NAME archetype
         COMMAND laige-sim_tests
         --gtest_filter=Archetype*)

# M1-ECS-04: query API + iteration legality. The step's Verify command
# is `ctest -R query`; this entry selects exactly the Query* suites
# from the shared laige-sim_tests executable (the 10k-entity
# zero-alloc window's machine-greppable stats lines land in the ctest
# output).
add_test(NAME query
         COMMAND laige-sim_tests
         --gtest_filter=Query*)

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2),
  # so ctest fails loudly on any TSan report.
  set_tests_properties(laige-sim_tests entity component_registry archetype
    query PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
