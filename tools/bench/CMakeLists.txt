# laige-bench (M0-CORE-08): the canonical benchmark command
# (docs/getting-started/building.md):
#
#   ./build/bin/laige-bench --suite=<name> [--runs=N] [--warmup=N]
#                           [--budget=<budget-name>] [--budgets=<path>]
#                           [--report=<path>]
#
# Runs a synthetic suite through the budget harness (laige::Histogram +
# TimeIt + budgetCheck) and prints the AGENTS 12 report. Gated with the
# test suite like tools/fuzz: a library-only build does not need it.

add_executable(laige-bench laige-bench.cpp)
laige_apply_engine_policy(laige-bench)
target_link_libraries(laige-bench PRIVATE laige-core)

# The report's "build" context field records the build type (AGENTS 12);
# CMake stamps it so the binary does not have to guess.
target_compile_definitions(laige-bench PRIVATE
  LAIGE_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# Smoke test: the Verify clause of M0-CORE-08 in executable form — a
# synthetic workload produces percentiles (the stats line must carry the
# sample count) and the tool exits 0. Timing values are never asserted
# (machine-dependent); the output shape is the contract.
add_test(NAME laige_bench_smoke COMMAND laige-bench --suite=synthetic
                                         --runs=50 --warmup=10)
set_tests_properties(laige_bench_smoke
  PROPERTIES PASS_REGULAR_EXPRESSION "stats: n=50")

if(LAIGE_TSAN)
  # Same first-report-fatal policy as the other test entries (NFR-8.2).
  set_tests_properties(laige_bench_smoke
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
