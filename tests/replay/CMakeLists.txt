# laige-replay CTest suite (M1-DET-03).
#
# The tool is tested end to end against a recorded run of the engine's
# built-in scenario (`laige-run --headless ... --replay` — the M1-DET-02
# wiring): the record -> replay -> --expect pipeline, the hash line
# contract on stdout (tick 0 first; 16 lowercase hex; one line per tick
# — a redirect captures a clean baseline file), cross-process
# determinism (two replays, byte-identical streams), the first-
# divergence report on a perturbed baseline, the stream-length and
# malformed-baseline behavior, and the identity-mismatch rejection
# (ADR 0002: a mismatch is a rejected replay, never a silent
# divergence).
#
# Each test is a generated `cmake -P` check script
# (expect-replay-result.cmake.in, the tests/api + tests/detcheck
# pattern) that asserts the exit code AND the required stdout/stderr
# fragments — CTest inverts PASS_REGULAR_EXPRESSION with WILL_FAIL, so
# the content must be checked in the script. The fixture configs live
# in fixtures/; a run of 32 ticks at 60 Hz is ~0.5 s of wall clock per
# recorded run.

set(LAIGE_REPLAY_TEST_ENV
    "LAIGE_REPLAY=$<TARGET_FILE:laige-replay>"
    "LAIGE_RUN=$<TARGET_FILE:laige-run>"
    "LAIGE_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay_smoke.json"
    "LAIGE_WRONGSEED=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay_smoke_wrongseed.json"
    "LAIGE_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}")

# laige_replay_test(name [SETUP s] [CMD c] [SECOND_CMD c] [WRITE_OUT p]
#                   [DERIVE mode] [DERIVED_PATH p] [EXPECT_EXIT n]
#                   [EXPECT_LINES n] [EXPECT_IDENTICAL bool]
#                   [PERTURB_TICK n] [CHECK_OUT regex]* [CHECK_ERR regex]*)
# Builds one generated check script; CMD/SECOND_CMD may reference
# <REPLAY> <RUN> <FIXTURE> <DERIVED baseline> <OUT> (expanded at run
# time from the environment).
function(laige_replay_test name)
  set(oneValueArgs SETUP CMD SECOND_CMD WRITE_OUT DERIVE DERIVED_PATH
                   EXPECT_EXIT EXPECT_LINES EXPECT_IDENTICAL
                   PERTURB_TICK)
  set(multiValueArgs CHECK_OUT CHECK_ERR)
  cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})
  # The template's @VAR@ names (configure_file @ONLY scope).
  set(TEST_NAME "${name}")
  set(SETUP_CMD "${ARG_SETUP}")
  set(CMD "${ARG_CMD}")
  set(SECOND_CMD "${ARG_SECOND_CMD}")
  set(WRITE_OUT "${ARG_WRITE_OUT}")
  set(DERIVE "${ARG_DERIVE}")
  set(DERIVED_PATH "${ARG_DERIVED_PATH}")
  set(PERTURB_TICK "${ARG_PERTURB_TICK}")
  set(EXPECT_EXIT "${ARG_EXPECT_EXIT}")
  set(EXPECT_LINES "${ARG_EXPECT_LINES}")
  set(EXPECT_IDENTICAL "${ARG_EXPECT_IDENTICAL}")
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
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/expect-replay-result.cmake.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake" @ONLY)
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -P
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake")
  set_tests_properties(${name} PROPERTIES
                       ENVIRONMENT "${LAIGE_REPLAY_TEST_ENV}"
                       TIMEOUT 300)
  if(LAIGE_TSAN)
    # The first race report is fatal (NFR-8.2 — the repo convention).
    set_tests_properties(${name} PROPERTIES
                         ENVIRONMENT "${LAIGE_REPLAY_TEST_ENV};TSAN_OPTIONS=halt_on_error=1")
  endif()
endfunction()

# The shared setup: record a 32-tick headless run (the M1-DET-02
# recorder wiring on laige-run; the engine's built-in registrations).
# The recorded tick count is PLATFORM-STABLE: laige-run drives a
# bounded run with frame budget 1, so the run lands exactly on 32
# under any cadence (a late frame drops its extra due tick — the
# EngineRun.BoundedRunCompletesExactly precedent — it does not
# overshoot; with the engine's default catch-up budget the macOS CI
# runners measured 33 ticks for --ticks 32).
set(RECORD_SETUP
    "<RUN> --headless <FIXTURE> --ticks 32 --replay <OUT>/replay_smoke.log")
# The shared replay command (stdout: exactly the 33 hash lines).
set(REPLAY_CMD
    "<REPLAY> --log <OUT>/replay_smoke.log --config <FIXTURE>")

# 1) record -> replay: exit 0, the stdout contract (33 lines; each
#    '<tick> <hash>' with tick == line index and 16 hex digits — the
#    template's structural validation), the summary line on stderr.
laige_replay_test(replay_smoke
  SETUP "${RECORD_SETUP}"
  CMD "${REPLAY_CMD}"
  EXPECT_EXIT 0
  EXPECT_LINES 33
  CHECK_ERR "frames=32 hash_lines=33 result=ok")

# 2) cross-process determinism: two replays of the same log are
#    byte-identical on stdout (FR-11.3).
laige_replay_test(replay_deterministic
  SETUP "${RECORD_SETUP}"
  CMD "${REPLAY_CMD}"
  SECOND_CMD "${REPLAY_CMD}"
  EXPECT_EXIT 0
  EXPECT_IDENTICAL true
  CHECK_ERR "result=ok")

# 3) --expect against the captured stream: exit 0, the streams match.
laige_replay_test(replay_expect_match
  SETUP "${RECORD_SETUP}"
  CMD "${REPLAY_CMD}"
  WRITE_OUT "<OUT>/replay_smoke.baseline"
  SECOND_CMD "<REPLAY> --log <OUT>/replay_smoke.log --config <FIXTURE> --expect <OUT>/replay_smoke.baseline"
  EXPECT_EXIT 0
  EXPECT_IDENTICAL true
  CHECK_ERR "result=ok")

# 4) --expect against a PERTURBED baseline (tick 5's hash flipped):
#    exit 1 with the first-divergence report at tick 5 — the step's
#    Verify criterion (a perturbed baseline fails at the right tick).
laige_replay_test(replay_expect_perturbed
  SETUP "${RECORD_SETUP}"
  CMD "${REPLAY_CMD}"
  WRITE_OUT "<OUT>/replay_smoke.baseline"
  DERIVE perturb
  DERIVED_PATH "<OUT>/replay_smoke.baseline.derived"
  PERTURB_TICK 5
  SECOND_CMD "<REPLAY> --log <OUT>/replay_smoke.log --config <FIXTURE> --expect <OUT>/replay_smoke.baseline.derived"
  EXPECT_EXIT 1
  CHECK_ERR "hash mismatch at tick 5" "first divergence")

# 5) --expect against a TRUNCATED baseline (32 lines for 33): exit 1
#    with the stream-length report (the first missing/extra tick).
laige_replay_test(replay_expect_truncated
  SETUP "${RECORD_SETUP}"
  CMD "${REPLAY_CMD}"
  WRITE_OUT "<OUT>/replay_smoke.baseline"
  DERIVE truncate
  DERIVED_PATH "<OUT>/replay_smoke.baseline.derived"
  SECOND_CMD "<REPLAY> --log <OUT>/replay_smoke.log --config <FIXTURE> --expect <OUT>/replay_smoke.baseline.derived"
  EXPECT_EXIT 1
  CHECK_ERR "baseline stream length mismatch" "first missing/extra at tick 32")

# 6) --expect against a MALFORMED baseline (tick 2's hash non-hex):
#    exit 2 with the actionable line report.
laige_replay_test(replay_expect_malformed
  SETUP "${RECORD_SETUP}"
  CMD "${REPLAY_CMD}"
  WRITE_OUT "<OUT>/replay_smoke.baseline"
  DERIVE corrupt
  DERIVED_PATH "<OUT>/replay_smoke.baseline.derived"
  SECOND_CMD "<REPLAY> --log <OUT>/replay_smoke.log --config <FIXTURE> --expect <OUT>/replay_smoke.baseline.derived"
  EXPECT_EXIT 2
  CHECK_ERR "malformed baseline line 2")

# 7) the replay identity mismatch (ADR 0002): a config recorded under a
#    different seed is a REJECTED replay — exit 2, the report names the
#    differing fields (seed, config_hash).
laige_replay_test(replay_identity_mismatch
  SETUP "${RECORD_SETUP}"
  CMD "<REPLAY> --log <OUT>/replay_smoke.log --config <WRONGSEED>"
  EXPECT_EXIT 2
  CHECK_ERR "replay identity mismatch" "seed +DIFFERS" "config_hash +DIFFERS")

# 8) usage error: --config missing -> exit 2.
laige_replay_test(replay_usage_error
  CMD "<REPLAY> --log <OUT>/replay_smoke.log"
  EXPECT_EXIT 2
  CHECK_ERR "are required")

# 9) missing log file -> exit 2 (the IoError text on stderr).
laige_replay_test(replay_log_missing
  CMD "<REPLAY> --log <OUT>/does_not_exist.log --config <FIXTURE>"
  EXPECT_EXIT 2
  CHECK_ERR "laige-replay: log:")
