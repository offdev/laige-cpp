# hello.laige CTest suite (M1-SAMPLE-01, extended by M1-DET-04).
#
# The template game is tested end to end against the scenario contract:
# the no-arg 300-tick headless run (exactly what the CI detcheck job
# executes, from the repository root), the record -> replay pipeline
# (a recorded run and its replay are byte-identical on stdout — the
# determinism proof behind M1-DET-04), the hash-line contract on stdout
# (tick 0 first; 16 lowercase hex; one line per completed tick), the
# usage/IO/identity failure paths (exit 2, actionable stderr), and the
# PRD §9.4 game-code line budget (< 100 lines of code in hello.cpp —
# the NFR-13.5 comment lines do not count).
#
# M1-DET-04: the committed per-tick hash baselines (both SimMath
# backends — samples/hello/baselines/) are asserted here in EVERY P0
# job's ctest: each job's own build of `hello`/`hello-fp32` must
# reproduce the reference build's (canonical Debug g++) stream
# bit-exactly (`hello --expect`, the laige-replay --expect contract —
# 0 match, 1 first divergence, 2 baseline read/contract error). The
# failure fixtures (derived perturbed/truncated/malformed baselines)
# pin the exit-1/exit-2 paths. The cross-compiler / sanitizer /
# Release pairs run in the CI `detcheck` job (docs/benchmarks/
# determinism-matrix.md).
#
# Each test is a generated `cmake -P` check script
# (expect-sample-result.cmake.in, the tests/replay + tests/detcheck
# pattern) that asserts the exit code AND the required stdout/stderr
# fragments — CTest inverts PASS_REGULAR_EXPRESSION with WILL_FAIL, so
# the content must be checked in the script. One run of 300 ticks is
# unpaced (no wall-clock delay — the template drives the tick loop
# directly), so every test is fast under any cadence.

set(LAIGE_SAMPLE_TEST_ENV
    "SAMPLE_HELLO=$<TARGET_FILE:hello>;"
    "SAMPLE_HELLO_FP32=$<TARGET_FILE:hello-fp32>;"
    "SAMPLE_CONFIG=${CMAKE_SOURCE_DIR}/samples/hello/config.json;"
    "SAMPLE_BASELINE_FPX=${CMAKE_SOURCE_DIR}/samples/hello/baselines/fixed_point_16_16/hash_stream.txt;"
    "SAMPLE_BASELINE_FP32=${CMAKE_SOURCE_DIR}/samples/hello/baselines/float_pinned_32/hash_stream.txt;"
    "SAMPLE_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}")

# The test environment every entry shares (TSan-aware, built ONCE so
# the per-test SAMPLE_DERIVED override appends to the right base —
# CTest's ENVIRONMENT property is REPLACED by a later
# set_tests_properties call).
set(LAIGE_SAMPLE_ENV "${LAIGE_SAMPLE_TEST_ENV}")
if(LAIGE_TSAN)
  # The first race report is fatal (NFR-8.2 — the repo convention).
  set(LAIGE_SAMPLE_ENV "TSAN_OPTIONS=halt_on_error=1;${LAIGE_SAMPLE_TEST_ENV}")
endif()

# laige_sample_test(name [CMD c] [SECOND_CMD c] [EXPECT_EXIT n]
#                   [EXPECT_LINES n] [EXPECT_IDENTICAL bool]
#                   [DERIVE mode] [PERTURB_TICK n]
#                   [CHECK_OUT regex]* [CHECK_ERR regex]*)
# Builds one generated check script; CMD/SECOND_CMD may reference
# <HELLO> <HELLO_FP32> <CONFIG> <BASELINE_FPX> <BASELINE_FP32>
# <DERIVED> <OUT> (expanded at run time from the environment). DERIVE
# ("hash-perturb" | "truncate" | "malformed") builds a derived copy of
# the committed baseline at $ENV{SAMPLE_DERIVED} (set per test below)
# before the run — the `hello --expect` failure fixtures.
function(laige_sample_test name)
  set(oneValueArgs CMD SECOND_CMD
                    EXPECT_EXIT EXPECT_LINES EXPECT_IDENTICAL
                    DERIVE PERTURB_TICK)
  set(multiValueArgs CHECK_OUT CHECK_ERR)
  cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})
  # The template's @VAR@ names (configure_file @ONLY scope).
  set(TEST_NAME "${name}")
  set(CMD "${ARG_CMD}")
  set(SECOND_CMD "${ARG_SECOND_CMD}")
  set(EXPECT_EXIT "${ARG_EXPECT_EXIT}")
  set(EXPECT_LINES "${ARG_EXPECT_LINES}")
  set(EXPECT_IDENTICAL "${ARG_EXPECT_IDENTICAL}")
  set(DERIVE "${ARG_DERIVE}")
  set(PERTURB_TICK "${ARG_PERTURB_TICK}")
  # The committed baseline the derive step builds its fixtures from
  # (a configure-time path — the fpx baseline: the derived copies
  # exercise `hello`'s comparison, whose stream is backend-agnostic).
  set(DERIVED_SOURCE
      "${CMAKE_SOURCE_DIR}/samples/hello/baselines/fixed_point_16_16/hash_stream.txt")
  set(_checks "")
  foreach(_frag IN LISTS ARG_CHECK_OUT)
    string(APPEND _checks "
if(NOT _check_out MATCHES \"${_frag}\")
  set(_problems \"${_problems}; stdout missing fragment '${_frag}'\")
endif()
")
  endforeach()
  foreach(_frag IN LISTS ARG_CHECK_ERR)
    string(APPEND _checks "
if(NOT _check_err MATCHES \"${_frag}\")
  set(_problems \"${_problems}; stderr missing fragment '${_frag}'\")
endif()
")
  endforeach()
  set(CHECKS "${_checks}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/expect-sample-result.cmake.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake" @ONLY)
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -P
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake")
  set_tests_properties(${name} PROPERTIES
                       ENVIRONMENT "${LAIGE_SAMPLE_ENV}"
                       TIMEOUT 300)
endfunction()

# 1) The CI scenario: a no-arg run from any CWD (the canonical config is
#    embedded — no CWD-relative file access). Exit 0, exactly 301 hash
#    lines on stdout (tick 0 = initial state + 300 completed ticks), the
#    one-line summary on stderr. This is the exact invocation of the CI
#    detcheck job's "Real scenario (M1-SAMPLE-01)" step.
laige_sample_test(hello_scenario
  CMD "<HELLO>"
  EXPECT_EXIT 0
  EXPECT_LINES 301
  CHECK_ERR "hello headless ticks=300 status=ok")

# 2) The record -> replay pipeline: record a 300-tick run (--replay),
#    then replay the log (--log): exit 0, 301 hash lines, and the
#    replayed stdout byte-identical to the recorded run's stdout
#    (determinism, S-7 — the bit-identity M1-DET-04 promotes to a
#    two-configuration detcheck comparison). --replay is a debug-build
#    feature (the engine's startReplayRecording policy, the #if
#    defined(NDEBUG) gate in hello.cpp), so the test is registered only
#    for debug builds (CI's P0 jobs are Debug) and pinned to the Debug
#    configuration in multi-config trees.
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  laige_sample_test(hello_replay
    CMD "<HELLO> --replay <OUT>/hello_scenario.log"
    SECOND_CMD "<HELLO> --log <OUT>/hello_scenario.log"
    EXPECT_EXIT 0
    EXPECT_LINES 301
    EXPECT_IDENTICAL true
    CHECK_ERR "hello replay ticks=300 status=ok")
  set_tests_properties(hello_replay PROPERTIES CONFIGURATIONS Debug)
endif()

# 3) --log on a missing log: exit 2, the replay error on stderr.
laige_sample_test(hello_replay_missing
  CMD "<HELLO> --log <OUT>/does_not_exist.log"
  EXPECT_EXIT 2
  CHECK_ERR "hello: replay:")

# 4) --log on a corrupt log (the committed garbage fixture): exit 2,
#    the replay error on stderr (a malformed log is a loud load failure,
#    never a partial replay — CORE-008).
laige_sample_test(hello_replay_malformed
  CMD "<HELLO> --log ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/garbage.log"
  EXPECT_EXIT 2
  CHECK_ERR "hello: replay:")

# 5) The modes are exclusive: exit 2.
laige_sample_test(hello_mode_exclusive
  CMD "<HELLO> --replay <OUT>/a.log --log <OUT>/b.log"
  EXPECT_EXIT 2
  CHECK_ERR "exclusive")

# 6) Unknown option: exit 2, the actionable line on stderr.
laige_sample_test(hello_usage_unknown
  CMD "<HELLO> --bogus value"
  EXPECT_EXIT 2
  CHECK_ERR "unknown option")

# 7) A dangling option (missing its value): exit 2.
laige_sample_test(hello_usage_missing_value
  CMD "<HELLO> --replay"
  EXPECT_EXIT 2
  CHECK_ERR "missing its value")

# 8) config.json is a valid engine config: laige-run (the engine's CLI,
#    the M1-HEAD-01 declarative surface) consumes it cleanly — a bounded
#    16-tick headless run exits 0. This keeps the declarative record in
#    sync with the embedded config's format (the values themselves are
#    documented identical in samples/hello/README.md).
add_test(NAME hello_config_valid
         COMMAND laige-run --headless
                 ${CMAKE_SOURCE_DIR}/samples/hello/config.json
                 --ticks 16)
set_tests_properties(hello_config_valid PROPERTIES TIMEOUT 300
                                ENVIRONMENT "${LAIGE_SAMPLE_ENV}"
                                PASS_REGULAR_EXPRESSION "status=ok")

# 9) The game-code line budget (PRD §9.4): < 100 lines of code in
#    hello.cpp (non-comment, non-blank lines — the NFR-13.5 comment
#    lines do not count). CMake-core script, no compiler involved.
add_test(NAME hello_line_budget
         COMMAND ${CMAKE_COMMAND} -P
               "${CMAKE_CURRENT_SOURCE_DIR}/check-line-budget.cmake")
set_tests_properties(hello_line_budget PROPERTIES
                     ENVIRONMENT "SAMPLE_HELLO_SRC=${CMAKE_SOURCE_DIR}/samples/hello/hello.cpp")

# --- M1-DET-04: the baseline matrix (every P0 job, both backends) -------
# The reference-build baselines (canonical Debug g++ — the
# samples/hello/baselines/ README) must be reproduced bit-exactly by
# THIS tree's build of the scenario, on every P0 OS job (and the
# sanitizer lanes' ctest as well). These are the CI form of the
# "run the scenario headless and assert per-tick identity against the
# baseline" half of M1-DET-04 (the `laige-replay --expect` contract on
# the scenario's own stream — the scenario binary carries it because a
# game log cannot be replayed by laige-replay; docs/api/detcheck.md).

# 10) The fixed_point_16_16 baseline: this build's `hello` vs the
#     committed stream. Exit 0, 301 lines, the summary unchanged.
laige_sample_test(hello_baseline_fpx
  CMD "<HELLO> --expect <BASELINE_FPX>"
  EXPECT_EXIT 0
  EXPECT_LINES 301
  CHECK_ERR "hello headless ticks=300 status=ok")

# 11) The float_pinned_32 baseline: this build's `hello-fp32` vs the
#     committed stream (the same matrix, the second backend — ADR 0002).
laige_sample_test(hello_baseline_fp32
  CMD "<HELLO_FP32> --expect <BASELINE_FP32>"
  EXPECT_EXIT 0
  EXPECT_LINES 301
  CHECK_ERR "hello headless ticks=300 status=ok")

# 12) The first-divergence path: one flipped hash in a derived copy of
#     the baseline -> exit 1, the laige-replay --expect report (the
#     tick-150 line, "first divergence").
laige_sample_test(hello_baseline_mismatch
  CMD "<HELLO> --expect <DERIVED>"
  DERIVE "hash-perturb" PERTURB_TICK 150
  EXPECT_EXIT 1
  CHECK_ERR "hash mismatch at tick 150" "first divergence")
set_tests_properties(hello_baseline_mismatch PROPERTIES
                     ENVIRONMENT "${LAIGE_SAMPLE_ENV};SAMPLE_DERIVED=${CMAKE_CURRENT_BINARY_DIR}/baseline_mismatch.txt")

# 13) The stream-length path: a derived baseline truncated to 160 lines
#     -> exit 1, the length-mismatch report (first missing at tick 160).
laige_sample_test(hello_baseline_truncated
  CMD "<HELLO> --expect <DERIVED>"
  DERIVE "truncate" PERTURB_TICK 160
  EXPECT_EXIT 1
  CHECK_ERR "length mismatch" "first missing/extra at tick 160")
set_tests_properties(hello_baseline_truncated PROPERTIES
                     ENVIRONMENT "${LAIGE_SAMPLE_ENV};SAMPLE_DERIVED=${CMAKE_CURRENT_BINARY_DIR}/baseline_truncated.txt")

# 14) The baseline-contract path: a malformed derived baseline (a hash
#     with the wrong digit count) -> exit 2 BEFORE any tick is run
#     (the loud CORE-008 load failure, never a silent mismatch).
laige_sample_test(hello_baseline_malformed
  CMD "<HELLO> --expect <DERIVED>"
  DERIVE "malformed" PERTURB_TICK 7
  EXPECT_EXIT 2
  CHECK_ERR "malformed baseline line 7")
set_tests_properties(hello_baseline_malformed PROPERTIES
                     ENVIRONMENT "${LAIGE_SAMPLE_ENV};SAMPLE_DERIVED=${CMAKE_CURRENT_BINARY_DIR}/baseline_malformed.txt")

# 15) The missing-baseline path: a baseline path that does not exist ->
#     exit 2, the actionable read error.
laige_sample_test(hello_baseline_missing
  CMD "<HELLO> --expect <OUT>/does_not_exist_baseline.txt"
  EXPECT_EXIT 2
  CHECK_ERR "hello: baseline:")
