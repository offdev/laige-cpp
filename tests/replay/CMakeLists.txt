# laige-replay CTest suite (M1-DET-03 + M1-DET-05).
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
# The diff mode (M1-DET-05, `laige-replay --diff`): the diff tests
# record two scenario logs (the template's SETUP/SETUP2 steps) and
# assert the structured stdout report grammar + the exit codes
# (0 identical, 1 divergent/length-divergent, 2 usage/identity/IO) —
# replay_diff_identical (two 16-tick recordings, same fixture),
# replay_diff_length (16 vs 10 ticks), replay_diff_identity (a
# budget-128 config against a budget-64 log), and replay_diff_usage
# (--diff with one log path).
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
    "LAIGE_DIFFBUDGET=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay_smoke_budget128.json"
    "LAIGE_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}")

# laige_replay_test(name [SETUP s] [SETUP2 s] [CMD c] [SECOND_CMD c]
#                   [WRITE_OUT p] [DERIVE mode] [DERIVED_PATH p]
#                   [EXPECT_EXIT n] [EXPECT_LINES n] [EXPECT_IDENTICAL
#                   bool] [PERTURB_TICK n] [CHECK_OUT regex]*
#                   [CHECK_ERR regex]*)
# Builds one generated check script; CMD/SECOND_CMD may reference
# <REPLAY> <RUN> <FIXTURE> <DIFFBUDGET> <DERIVED baseline> <OUT>
# (expanded at run time from the environment).
function(laige_replay_test name)
  set(oneValueArgs SETUP SETUP2 CMD SECOND_CMD WRITE_OUT DERIVE
                   DERIVED_PATH EXPECT_EXIT EXPECT_LINES
                   EXPECT_IDENTICAL PERTURB_TICK)
  set(multiValueArgs CHECK_OUT CHECK_ERR)
  cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})
  # The template's @VAR@ names (configure_file @ONLY scope).
  set(TEST_NAME "${name}")
  set(SETUP_CMD "${ARG_SETUP}")
  set(SETUP2_CMD "${ARG_SETUP2}")
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

# ---------------------------------------------------------------------------
# The diff mode (M1-DET-05): `laige-replay --diff <logA> <logB>`.
# Each test records its scenario log(s) via the template's SETUP/SETUP2
# steps (laige-run --headless ... --replay, the engine's built-in
# registrations) and asserts the structured stdout report grammar +
# the exit codes (0 identical, 1 divergent/length, 2 identity/usage/IO).
# ---------------------------------------------------------------------------

# The shared 16-tick recording (the diff fixture runs).
set(DIFF_RECORD_16
    "<RUN> --headless <FIXTURE> --ticks 16 --replay <OUT>/diff_16.log")
set(DIFF_RECORD_10
    "<RUN> --headless <FIXTURE> --ticks 10 --replay <OUT>/diff_10.log")

# 10) identical: two 16-tick recordings of the SAME fixture (same
#     seed 42) -> the component state is bit-identical over all 16
#     ticks: exit 0, result=identical, zero differing items, and the
#     full state (PRNG substreams) also matches (same seed).
laige_replay_test(replay_diff_identical
  SETUP "${DIFF_RECORD_16}"
  SETUP2 "<RUN> --headless <FIXTURE> --ticks 16 --replay <OUT>/diff_16_b.log"
  CMD "<REPLAY> --diff <OUT>/diff_16.log <OUT>/diff_16_b.log --config <FIXTURE>"
  EXPECT_EXIT 0
  CHECK_OUT "replay_diff result=identical" "frames_a=16" "frames_b=16"
            "first_divergent_tick=0" "full_state_divergent=false"
            "differing_items=0" "reported_items=0"
  CHECK_ERR "replays identical over 17 ticks")

# 11) length divergence: a 16-tick log vs a 10-tick log (same fixture):
#     the shared ticks match, but the frame counts differ -> exit 1,
#     result=length_divergence, first tick only one replay has is 11
#     (0-based: the 11th completed tick), zero differing items.
laige_replay_test(replay_diff_length
  SETUP "${DIFF_RECORD_16}"
  SETUP2 "${DIFF_RECORD_10}"
  CMD "<REPLAY> --diff <OUT>/diff_16.log <OUT>/diff_10.log --config <FIXTURE>"
  EXPECT_EXIT 1
  CHECK_OUT "replay_diff result=length_divergence" "frames_a=16"
            "frames_b=10" "first_divergent_tick=11"
            "length_divergence=true" "differing_items=0"
  CHECK_ERR "length divergence: replay A has 16 frames, replay B has 10")

# 12) identity mismatch (ADR 0002): a 16-tick log recorded under the
#     budget-64 fixture, diffed against ITSELF under the budget-128
#     config (same seed 42): the config_hash differs -> the diff is
#     REJECTED (never a silent divergence) — exit 2, the report names
#     the config_hash field.
laige_replay_test(replay_diff_identity
  SETUP "${DIFF_RECORD_16}"
  CMD "<REPLAY> --diff <OUT>/diff_16.log <OUT>/diff_16.log --config <DIFFBUDGET>"
  EXPECT_EXIT 2
  CHECK_ERR "replay identity mismatch" "config_hash" "DIFFERS")

# 13) usage error: --diff with a single log path (no second) -> exit 2.
laige_replay_test(replay_diff_usage
  CMD "<REPLAY> --diff <OUT>/diff_16.log"
  EXPECT_EXIT 2
  CHECK_ERR "--diff needs two log paths")
