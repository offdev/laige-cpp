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
# Suites: `synthetic` (the M0 harness stand-in — laige-core only),
# `sim-tick` (the M1-BENCH-01 PRD 8.1 workload — drives the laige-sim
# World/systems/GameLoop: 10k entities, 2k dynamic bodies, 60 Hz,
# BenchMove + BenchHash systems, both SimMath backends),
# `render-50k` (the M2-PERF-01 PRD 8.1 "50k visible sprites" workload —
# the FULL M2-SCENE-01 reference scene rendered offscreen through the
# batcher + submit stages, both SimMath backends), and `ac4` /
# `render-sort-10k` (the M2-AC-01 AC-4.x acceptance workload — 10k
# overlapping sprites sorted + drawn at 60 FPS, both SimMath backends).

add_executable(laige-bench laige-bench.cpp)
laige_apply_engine_policy(laige-bench)
# The sim-tick, render-50k, and ac4 suites carry deterministic sim
# math (the BenchMove systems, the reference scene's fixed-point depth
# keys, and the ac4 workload's per-backend isoDepthKey runs on the
# SimMath op surface) — the ADR 0002 pinned flag set applies to the TU
# exactly as to every sim target.
laige_apply_simmath_policy(laige-bench)
target_link_libraries(laige-bench PRIVATE laige-core laige-sim laige-render)

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
#
# Platform scope (roadmap M1-BENCH-01: "the CI reference machine,
# ubuntu-24.04, is the gate"; docs/benchmarks/methodology.md §5): the
# absolute PRD §8.1 targets are calibrated on the CI reference machine —
# the P0 Linux jobs, both of which run on ubuntu-24.04. The shared
# GitHub macOS/Windows runners are slower and noisier than the reference
# machine, and their tail latency is dominated by runner preemption
# (a macos-15 merge-lane run measured p99 6.00 ms / max 136.92 ms on the
# 10k sim-tick workload against the reference max of 0.81 ms, with the
# run's final_hash bit-identical to the baseline — no engine regression),
# so the absolute gate would flap there without tracking regressions.
# The other P0 platforms keep the full ctest suite (determinism,
# zero-alloc, fuzz, and API checks all still run); only the absolute
# sim-tick budget gate is reference-platform-scoped — and the ungated
# sim-tick smoke below still exercises the workload on every platform.
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
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

# The M2-PERF-01 CI gate: the FULL M2-SCENE-01 reference scene (the
# PRD 8.1 "50k visible sprites" workload) rendered offscreen at the
# M2-GOLD-01 reference resolution, both SimMath backends, in the
# canonical 3000-run / 1000-warmup shape, checked against BOTH PRD
# 8.1 budgets: the frame's draw-call count (max <= 30) and the
# batch-stage CPU (mean <= 2.0 ms) — the suite's zero-allocation
# assertion (M1-ALLOC-01 extended to the batcher) aborts the run on
# the first allocating frame (exit 1, CORE-008). Platform scope as the
# sim-tick gate: the absolute targets are calibrated on the CI
# reference machine (methodology §5 — the P0 Linux jobs, ubuntu-
# 24.04); the entry is Linux-only because the suite needs an
# offscreen GL 3.3 context (the CI's Mesa surfaceless EGL + llvmpipe
# stack, M2-GL-01) and there is no all-platform smoke: the macOS
# runners have no usable GL (the GL smokes' documented SKIP) and the
# workload's 4000 x 116k-quad renders would not fit the other lanes'
# time budgets. Excluded from the sanitizer trees: instrumentation
# inflates the absolute cost (the m1-profiler-cost precedent). The
# 1800 s timeout bounds 4000 offscreen frames of the ~116k-item frame
# with wide margin (shared-runner variance — the M2-GL-02 3000-frame
# entry's 600 s shape is a small scene; this one is the §8.1 worst
# case, 116k quads per frame on software GL).
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME laige_bench_render_50k_fpx16
           COMMAND laige-bench --suite=render-50k --math=fixed_point_16_16
                                 --runs=3000 --warmup=1000
                                 --budget=sprites_50k_draw_calls
                                 --budget=sprites_50k_cpu)
  add_test(NAME laige_bench_render_50k_fp32
           COMMAND laige-bench --suite=render-50k --math=float_pinned_32
                                 --runs=3000 --warmup=1000
                                 --budget=sprites_50k_draw_calls
                                 --budget=sprites_50k_cpu)
  set_tests_properties(laige_bench_render_50k_fpx16
                       laige_bench_render_50k_fp32
    PROPERTIES ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json"
               TIMEOUT 1800)
endif()

# The M2-AC-01 CI gate (PRD §4 AC-4.3): the 10 000-overlapping-sprite
# workload (the 100 x 10 grid, 10 sprites stacked per cell — the
# sort's equal-key ties + the frame's overdraw) sorted + drawn at
# 60 FPS, both SimMath backends, in the canonical 3000-run /
# 1000-warmup shape, checked against the `sprites_10k_frame` budget
# (the whole frame's CPU ms, mean <= 16.7 ms); the suite's
# zero-allocation assertion (M1-ALLOC-01 extended to the batcher)
# aborts the run on the first allocating frame (exit 1, CORE-008), and
# the AC-4.4 engine-default assert (the default ProjectionView's mode
# is Iso) runs in setup. Platform scope as the render-50k gate: the
# absolute target is calibrated on the CI reference machine
# (methodology §5 — the P0 Linux jobs, ubuntu-24.04); the entry is
# Linux-only because the frame sample needs an offscreen GL 3.3
# context (the CI's Mesa surfaceless EGL + llvmpipe stack, M2-GL-01 —
# a CPU-only run would leave the frame histogram empty and fail
# NO_SAMPLES, not skip, and the macOS runners have no usable GL) and
# the workload's 4000 x 10k-quad frames would not fit the other
# lanes' time budgets. Excluded from the sanitizer trees:
# instrumentation inflates the absolute cost (the m1-profiler-cost
# precedent). The 1800 s timeout bounds 4000 offscreen frames of the
# 10k-quad frame with wide margin (the render-50k gate's shape).
# `ctest -R ac4` selects these two entries together with the
# ac4_surface* lint entries (the AC-4.x suite in one ctest selection).
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME laige_bench_ac4_fpx16
           COMMAND laige-bench --suite=ac4 --math=fixed_point_16_16
                                 --runs=3000 --warmup=1000
                                 --budget=sprites_10k_frame)
  add_test(NAME laige_bench_ac4_fp32
           COMMAND laige-bench --suite=ac4 --math=float_pinned_32
                                 --runs=3000 --warmup=1000
                                 --budget=sprites_10k_frame)
  set_tests_properties(laige_bench_ac4_fpx16
                       laige_bench_ac4_fp32
    PROPERTIES ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json"
               TIMEOUT 1800)

  # TEMPORARY DIAGNOSTIC (removed once root-caused): the ac4 run's
  # exit-time segfault backtrace (gdb, a reduced frame count — the
  # crash is in the exit path, not the measured region).
  add_test(NAME laige_bench_ac4_fpx16_gdb
           COMMAND /usr/bin/gdb -batch -ex run -ex bt
                   --args laige-bench --suite=ac4 --math=fixed_point_16_16
                         --runs=20 --warmup=5
                         --budget=sprites_10k_frame)
  set_tests_properties(laige_bench_ac4_fpx16_gdb
    PROPERTIES ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json"
               TIMEOUT 300)
endif()

# All-platform sim-tick smoke (no budget gate): the sim-tick workload
# in its canonical 4000-tick shape (the default fpx16_16 backend), run
# in EVERY P0 platform's ctest — including the platforms where the
# absolute budget gate above is reference-scoped — so the workload's
# code paths (BenchMove, BenchHash, the per-tick stateHash) stay
# exercised and deterministic on every platform. Two assertions, no
# timing: (1) the run completes and prints its determinism
# fingerprint — the fpx16_16 4000-tick final_hash, bit-identical
# across build types, compilers, and P0 platforms per ADR 0002 (the
# M1-BENCH-01 baseline's fingerprint,
# docs/benchmarks/baselines/m1-sim-tick.md; observed identical on the
# AppleClang and MSVC CI jobs); (2) every measured tick in the debug
# tree survives the engine's G-R1 per-tick zero-allocation assertion
# (an allocating tick aborts the run, M1-ALLOC-01). No `--budget`:
# the absolute targets are calibrated on the reference machine
# (methodology §5) — this entry checks correctness, not speed.
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN)
  add_test(NAME laige_bench_sim_tick_smoke
           COMMAND laige-bench --suite=sim-tick
                             --runs=3000 --warmup=1000)
  set_tests_properties(laige_bench_sim_tick_smoke
    PROPERTIES PASS_REGULAR_EXPRESSION "final_hash=0x7840644a24334cd0")
endif()

if(LAIGE_TSAN)
  # Same first-report-fatal policy as the other test entries (NFR-8.2).
  set_tests_properties(laige_bench_smoke
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
