# laige-bench (M0-CORE-08, extended by M1-BENCH-01): the canonical
# benchmark command (docs/getting-started/building.md):
#
#   ./build/bin/laige-bench --suite=<name> [--runs=N] [--warmup=N]
#                           [--budget=<budget-name>]... [--budgets=<path>]
#                           [--math=<fixed_point_16_16|float_pinned_32>]
#                           [--report=<path>]
#
# Runs a suite through the budget harness (laige::Histogram + TimeIt +
# budgetCheck) and prints the AGENTS 12 report. Gated with the test
# suite like tools/fuzz: a library-only build does not need it.
#
# Suites: `synthetic` (the M0 harness stand-in — laige-core only) and
# `sim-tick` (the M1-BENCH-01 PRD 8.1 workload — drives the laige-sim
# World/systems/GameLoop: 10k entities, 2k dynamic bodies, 60 Hz,
# BenchMove + BenchHash systems, both SimMath backends).

add_executable(laige-bench laige-bench.cpp)
laige_apply_engine_policy(laige-bench)
# The sim-tick suite carries deterministic sim math (the BenchMove
# systems run on the SimMath op surface) — the ADR 0002 pinned flag
# set applies to the TU exactly as to every sim target.
laige_apply_simmath_policy(laige-bench)
target_link_libraries(laige-bench PRIVATE laige-core laige-sim)

# The report's "build" context field records the build type (AGENTS 12);
# CMake stamps it so the binary does not have to guess.
target_compile_definitions(laige-bench PRIVATE
  LAIGE_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# Smoke test: the Verify clause of M0-CORE-08 in executable form — a
# synthetic workload produces percentiles (the stats line must carry
# the sample count) and the tool exits 0. Timing values are never
# asserted (machine-dependent); the output shape is the contract.
add_test(NAME laige_bench_smoke COMMAND laige-bench --suite=synthetic
                                          --runs=50 --warmup=10)
set_tests_properties(laige_bench_smoke
  PROPERTIES PASS_REGULAR_EXPRESSION "stats: n=50")

# The M1-BENCH-01 CI gate: the sim-tick workload checked against BOTH
# sim_tick budgets (the avg AND the p99 — the repeatable --budget form)
# on BOTH SimMath backends, in the canonical 3000-tick / 1000-warmup
# shape (docs/benchmarks/baselines/m1-sim-tick.md). A budget FAIL exits
# 2 and fails the test (PRD 8.1: a budget regression fails CI). Excluded
# from the sanitizer trees: instrumentation inflates the tick's absolute
# cost (the m1-profiler-cost baseline precedent — a sanitizer measurement
# would measure the instrumentation, not the sim).
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN)
  add_test(NAME laige_bench_sim_tick_fpx16
           COMMAND laige-bench --suite=sim-tick --math=fixed_point_16_16
                                    --runs=3000 --warmup=1000
                                    --budget=sim_tick_avg
                                    --budget=sim_tick_p99)
  add_test(NAME laige_bench_sim_tick_fp32
           COMMAND laige-bench --suite=sim-tick --math=float_pinned_32
                                    --runs=3000 --warmup=1000
                                    --budget=sim_tick_avg
                                    --budget=sim_tick_p99)
  # The repo-root budgets.json (the laige-core test-suite precedent —
  # the ctest working directory is the build tree, not the source root).
  set_tests_properties(laige_bench_sim_tick_fpx16
                       laige_bench_sim_tick_fp32
    PROPERTIES ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")
endif()

if(LAIGE_TSAN)
  # Same first-report-fatal policy as the other test entries (NFR-8.2).
  set_tests_properties(laige_bench_smoke
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
