# laige-core tests (M0-BUILD-01; GTest-ified by M0-DEP-01): build smoke test.
#
# Verifies that the laige-core library links in the selected variant
# (static by default, shared with LAIGE_BUILD_SHARED=ON; NFR-8.9) and that
# the NFR-8.10 language policy actually reached this target. Running this
# suite also exercises the GoogleTest plumbing end to end (configure →
# build → ctest) — the M0-DEP-01 verify path.

# One executable per module (tests/README.md); the functional suites land
# in this shared executable as separate source files and are exposed as
# their own CTest entries (M0-CORE-01 adds `result_status`; later
# M0-CORE-xx steps follow the same pattern).
set(LAIGE_CORE_TEST_SOURCES laige-core_tests.cpp result_status_tests.cpp
                             logging_tests.cpp math_float_tests.cpp
                             math_fixed_tests.cpp pools_tests.cpp
                              prng_tests.cpp
                              config_json_tests.cpp
                               budget_harness_tests.cpp)
# M1-ALLOC-01: the test-only allocation counter no longer carries its
# own global operator new/new[] overrides — they moved into laige-core
# (src/laige-core/alloc_watch.cpp, the G-R1 counting backend, one
# strong definition per binary), so the test-side probes
# (logging_alloc_counter.h) and the engine's per-tick assertion share
# one counter. LAIGE_ALLOC_COUNTER marks the non-sanitizer trees where
# the backend is live (the same gate as laige-core's LAIGE_ALLOC_WATCH;
# the sanitizer runtimes own operator new/delete, so the backend is
# excluded there — the zero-allocation property is then covered by the
# leak-free sanitizer run of the same loops plus the pool
# reservation-delta assertion, the same fallback pattern).
add_executable(laige-core_tests ${LAIGE_CORE_TEST_SOURCES})
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN)
  target_compile_definitions(laige-core_tests PRIVATE LAIGE_ALLOC_COUNTER=1)
endif()
laige_apply_engine_policy(laige-core_tests)
# M0-CORE-03: the fp32_pinned SimMath backend is instantiated in this test
# executable, so it must carry the same pinned flag set as laige-core
# (ADR 0002, PRD §10.3) — the runtime FMA canaries in
# math_float_tests.cpp depend on it.
laige_apply_simmath_policy(laige-core_tests)

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

# The gtest filter is always one UNQUOTED argument. CTest passes quoted
# arguments through with the literal quote characters still attached, and
# gtest's colon-split of the filter then drops the quote-attached
# patterns — silently under-running the step's Verify scope (CORE-008).
# (Fixed in M0-CORE-03, where this defect was found under
# `ctest -R math_float`; it affected the result_status and logging
# entries inherited from M0-CORE-01/02 as well.)

# M0-CORE-01: Result<T,E>/Status + error registry. The step's Verify
# command is `ctest -R result_status`; this entry selects exactly the
# ResultStatus/Status/ErrorCodeRegistry suites from the shared
# laige-core_tests executable (the unfiltered entry above still runs them
# as part of the full module suite).
add_test(NAME result_status
         COMMAND laige-core_tests
         --gtest_filter=ResultStatus.*:Status.*:ErrorCodeRegistry.*)

# M0-CORE-02: structured logging facade. The step's Verify command is
# `ctest -R logging`; this entry selects exactly the logging suites
# from the shared laige-core_tests executable.
add_test(NAME logging
         COMMAND laige-core_tests
         --gtest_filter=LogGate.*:LogRecord.*:LogSinks.*:LogRateLimit.*:LogFatal.*:LogCrash.*:LogConcurrency.*:LogPerformance.*)

# M0-CORE-03: SimMath interface + fp32_pinned backend. The step's Verify
# command is `ctest -R math_float`; this entry selects exactly the
# SimMath* suites (SimMathBasics, SimMathNanInf, SimMathProperties,
# SimMathDispatch) from the shared laige-core_tests executable.
add_test(NAME math_float
         COMMAND laige-core_tests
         --gtest_filter=SimMathBasics.*:SimMathNanInf.*:SimMathProperties.*:SimMathDispatch.*)

# M0-CORE-04: SimMath fpx16_16 backend (default). The step's Verify
# command is `ctest -R math_fixed`; this entry selects exactly the
# FixedPoint* suites from the shared laige-core_tests executable.
add_test(NAME math_fixed
         COMMAND laige-core_tests
         --gtest_filter=FixedPointBasics.*:FixedPointRounding.*:FixedPointSaturation.*:FixedPointConversions.*:FixedPointSimMath.*:FixedPointDispatch.*:FixedPointDeterminism.*)

# M0-CORE-05: memory pools (ArenaPool<T>, Pool<T>). The step's Verify
# command is `ctest -R pools`; this entry selects exactly the pool
# suites from the shared laige-core_tests executable.
add_test(NAME pools
         COMMAND laige-core_tests
         --gtest_filter=ArenaPoolBasics.*:ArenaPoolBudget.*:PoolBasics.*:PoolBudget.*:PoolStale.*:PoolDestruction.*:PoolStats.*:PoolMove.*)

# M0-CORE-06: deterministic PRNG (xorshift128+ with splitmix64 seeding
# and per-substream derivation). The step's Verify command is
# `ctest -R prng`; this entry selects exactly the Prng* suites. The
# PrngPeriod suite carries the committed period proof (state map's
# characteristic polynomial is primitive of degree 128 => every nonzero
# state has period 2^128 - 1).
add_test(NAME prng
         COMMAND laige-core_tests
         --gtest_filter=PrngGolden.*:PrngRange.*:PrngFloat01.*:PrngSubstreams.*:PrngPeriod.*)

# M0-CORE-07: bounded JSON parser + serializer (ADR 0003). The step's
# Verify command is `ctest -R config_json`; this entry selects exactly
# the ConfigJson* suites from the shared laige-core_tests executable.
# The fuzz gate of the step is the `fuzz_json_parse` entry registered in
# tools/fuzz (runs instrumented in the ASan tree).
add_test(NAME config_json
         COMMAND laige-core_tests
         --gtest_filter=ConfigJson*)

# M0-CORE-08: budget harness (Histogram, TimeIt, budgetCheck,
# loadBudgets). The step's Verify command is `ctest -R budget_harness`;
# this entry selects exactly the BudgetHarness* suites from the shared
# laige-core_tests executable. The table-load tests read the repo-root
# budgets.json through LAIGE_BUDGETS_PATH (set for the full module
# suite too, so both entries behave identically); the "budgets.json"
# fallback only works from the source root.
add_test(NAME budget_harness
         COMMAND laige-core_tests
         --gtest_filter=BudgetHarness*)
# NB: CTest's ENVIRONMENT property is replaced (not appended) by a later
# set_tests_properties call on the same test — the LAIGE_TSAN block below
# therefore re-states LAIGE_BUDGETS_PATH alongside TSAN_OPTIONS.
set_tests_properties(laige-core_tests budget_harness
  PROPERTIES ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2),
  # so ctest fails loudly on any TSan report.
  set_tests_properties(
    laige-core_tests result_status logging math_float math_fixed pools prng
    config_json budget_harness
    # ENVIRONMENT is replaced (not appended) by a later set_tests_properties
    # call on the same test, so restate the budgets path here (see the note
    # above the first set_tests_properties). List value = one semicolon-
    # joined string (CTest's ENVIRONMENT is a ';'-separated list).
    PROPERTIES ENVIRONMENT
    "TSAN_OPTIONS=halt_on_error=1;LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")
endif()
