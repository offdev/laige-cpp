# laige-sim tests (M1-ECS-01/02/03/04/05/06/07 + M1-SYS-01/02/03
# + M1-LOOP-01/02):
# entity handle + World entity storage, component type registry,
# archetype SoA storage, query API + iteration legality,
# deterministic iteration order, the ECS guardrails (G-R3/G-R4), the
# ECS stress + memory accounting suite, the system registry (plain
# registered functions, declared budgets + component I/O), the system
# scheduler (execution order, depends_on, the declared-I/O pre-run
# validation), per-system timing + budget enforcement (the rolling
# window, the G-R5 warn/error events, the profiler feed), and the
# fixed-timestep game loop core (the accumulator, the tick-rate
# validation, the bounded catch-up + tick_dropped overload behavior),
# and the presentation snapshot + interpolation state (the per-tick
# prev/curr refresh, the anchored clamped alpha, sample_position).
#
# One executable per module (tests/README.md; docs/testing.md is the
# source of truth): laige-sim_tests links the module under test plus
# gtest_main. The unfiltered entry runs the whole module; the `entity`,
# `component_registry`, `archetype`, `query`, `iter_order`,
# `ecs_guardrails`, `ecs_stress`, `system_registry`, `scheduler`,
# `system_timing`, `game_loop`, and `presentation` entries are the
# M1-ECS-01, M1-ECS-02, M1-ECS-03, M1-ECS-04, M1-ECS-05, M1-ECS-06,
# M1-ECS-07, M1-SYS-01, M1-SYS-02, M1-SYS-03, M1-LOOP-01, and
# M1-LOOP-02 Verify commands (`ctest -R entity`, `ctest -R
# component_registry`, `ctest -R archetype`, `ctest -R query`,
# `ctest -R iter_order`, `ctest -R ecs_guardrails`, `ctest -R
# ecs_stress`, `ctest -R system_registry`, `ctest -R scheduler`,
# `ctest -R system_timing`, `ctest -R game_loop`, `ctest -R
# presentation`), selecting exactly the suites below from the shared
# executable.

set(LAIGE_SIM_TEST_SOURCES entity_tests.cpp component_registry_tests.cpp
                           archetype_tests.cpp query_tests.cpp
                           iter_order_tests.cpp ecs_guardrails_tests.cpp
                           ecs_stress_tests.cpp
                           system_registry_tests.cpp
                           scheduler_tests.cpp
                           system_timing_tests.cpp
                           game_loop_tests.cpp
                           presentation_tests.cpp)
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

# M1-ECS-05: deterministic iteration order. The step's Verify command is
# `ctest -R iter_order`; this entry selects exactly the IterOrder*
# suites from the shared laige-sim_tests executable (the machine-
# greppable iter-order scenario lines land in the ctest output,
# docs/testing.md §4).
add_test(NAME iter_order
         COMMAND laige-sim_tests
         --gtest_filter=IterOrder.*)

# M1-ECS-06: ECS guardrails (G-R3 entity-count thresholds, G-R4
# per-frame component churn). The step's Verify command is
# `ctest -R ecs_guardrails`; this entry selects exactly the
# EcsGuardrails suites from the shared laige-sim_tests executable.
add_test(NAME ecs_guardrails
         COMMAND laige-sim_tests
         --gtest_filter=EcsGuardrails.*)

# M1-ECS-07: ECS stress + memory accounting. The step's Verify command
# is `ctest -R ecs_stress` (required green under ASan: the leak-free
# build-asan run of this window is the no-leak check); this entry
# selects exactly the EcsStress suite from the shared laige-sim_tests
# executable (the machine-greppable window/iteration/memory lines land
# in the ctest output).
add_test(NAME ecs_stress
         COMMAND laige-sim_tests
         --gtest_filter=EcsStress.*)

# M1-SYS-01: system registry. The step's Verify command is
# `ctest -R system_registry`; this entry selects exactly the
# SystemRegistry suites from the shared laige-sim_tests executable.
add_test(NAME system_registry
         COMMAND laige-sim_tests
         --gtest_filter=SystemRegistry.*)

# M1-SYS-02: system scheduler. The step's Verify command is
# `ctest -R scheduler`; this entry selects exactly the SystemScheduler
# suites from the shared laige-sim_tests executable (the machine-
# greppable scheduler-order / scheduler-zeroalloc lines land in the
# ctest output).
add_test(NAME scheduler
         COMMAND laige-sim_tests
         --gtest_filter=SystemScheduler.*)

# M1-SYS-03: per-system timing + budget enforcement (PRD §9.3 G-R5).
# The step's Verify command is `ctest -R system_timing`; this entry
# selects exactly the SystemTiming suites from the shared
# laige-sim_tests executable (the machine-greppable
# system-timing-window / system-timing-zeroalloc lines land in the
# ctest output).
add_test(NAME system_timing
         COMMAND laige-sim_tests
         --gtest_filter=SystemTiming.*)

# M1-LOOP-01: fixed-timestep game loop core (FR-1.1, ARCH-002).
# The step's Verify command is `ctest -R game_loop`; this entry
# selects exactly the GameLoop suites from the shared laige-sim_tests
# executable (the machine-greppable game-loop drops /
# game-loop-zeroalloc lines land in the ctest output).
add_test(NAME game_loop
         COMMAND laige-sim_tests
         --gtest_filter=GameLoop.*)

# M1-LOOP-02: per-tick presentation snapshot + interpolation state
# (ARCH-009: presentation is never authoritative). The step's Verify
# command is `ctest -R presentation`; this entry selects exactly the
# Presentation suites from the shared laige-sim_tests executable (the
# machine-greppable presentation linear / presentation-zeroalloc
# lines land in the ctest output).
add_test(NAME presentation
         COMMAND laige-sim_tests
         --gtest_filter=Presentation.*)

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2),
  # so ctest fails loudly on any TSan report.
  set_tests_properties(laige-sim_tests entity component_registry archetype
    query iter_order ecs_guardrails ecs_stress system_registry scheduler
    system_timing game_loop presentation PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
