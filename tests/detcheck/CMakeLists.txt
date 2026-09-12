# laige-detcheck CTest suite (M0-TOOL-02)
#
# The tool is tested with a synthetic two-run scenario (the step's
# Verify clause): the built-in `synthetic` scenario self-check, plus the
# real cross-binary mode against fixture scenario binaries — one source
# (detcheck-fixture-scenario.cpp) compiled in five variants, the clean
# copy being configuration A and the perturbed copy configuration B:
#
#   detcheck-synthetic              built-in --scenario=synthetic         exit 0
#   detcheck-synthetic-perturbed    built-in, perturbed run-b             exit 1
#   detcheck-bin-identical          fixture A vs fixture A                exit 0
#   detcheck-bin-identical-args     both, with pass-through --ticks=16    exit 0
#   detcheck-bin-diverged           fixture A vs perturbed copy           exit 1
#   detcheck-bin-malformed          fixture A vs bad-output copy          exit 2
#   detcheck-scenario-failure       fixture that exits 3 at tick 5        exit 2
#   detcheck-stream-mismatch        16-tick fixture vs 32-tick fixture    exit 1
#   detcheck-unknown-scenario       --scenario=bogus                      exit 2
#
# Each test is a generated `cmake -P` check script
# (expect-detcheck-result.cmake.in) that asserts the exit code AND the
# required output fragments — the same pattern as tests/api (CTest
# inverts PASS_REGULAR_EXPRESSION when WILL_FAIL is set, and a crash
# exits non-zero like a correct failure, so content must be checked).
# The real-tree scenario comparison (M1-DET-04) is not exercised here:
# it is wired in the CI `detcheck` job and activates with M1-SAMPLE-01.

# --- The fixture scenario (the synthetic two-run scenario) -----------------
# One source, five compiled variants (one flag each) — see the source
# header for what each variant does. No engine link: the fixture tests
# the contract plumbing, not the engine math.
add_executable(detcheck-fixture-scenario detcheck-fixture-scenario.cpp)
laige_apply_engine_policy(detcheck-fixture-scenario)

add_executable(detcheck-fixture-scenario-perturbed detcheck-fixture-scenario.cpp)
laige_apply_engine_policy(detcheck-fixture-scenario-perturbed)
target_compile_definitions(detcheck-fixture-scenario-perturbed
  PRIVATE DETCHK_FIXTURE_PERTURB_TICK=7)

add_executable(detcheck-fixture-scenario-bad detcheck-fixture-scenario.cpp)
laige_apply_engine_policy(detcheck-fixture-scenario-bad)
target_compile_definitions(detcheck-fixture-scenario-bad
  PRIVATE DETCHK_FIXTURE_BAD_OUTPUT=1)

add_executable(detcheck-fixture-scenario-fail detcheck-fixture-scenario.cpp)
laige_apply_engine_policy(detcheck-fixture-scenario-fail)
target_compile_definitions(detcheck-fixture-scenario-fail
  PRIVATE DETCHK_FIXTURE_FAIL_TICK=5 DETCHK_FIXTURE_FAIL_EXIT=3)

add_executable(detcheck-fixture-scenario-short detcheck-fixture-scenario.cpp)
laige_apply_engine_policy(detcheck-fixture-scenario-short)
target_compile_definitions(detcheck-fixture-scenario-short
  PRIVATE DETCHK_FIXTURE_TICKS=16)

# The test environment shared by every entry: the tool and all five
# fixture paths (the template reads them via $ENV{}; $<TARGET_FILE>
# resolves per configuration under multi-config generators). Built with
# string(APPEND) so the result is ONE string: a multi-argument set()
# would double the ';' separators (verified on CMake 4.4.3).
set(LAIGE_DETCHECK_TEST_ENV
    "LAIGE_DETCHECK=$<TARGET_FILE:laige-detcheck>;")
string(APPEND LAIGE_DETCHECK_TEST_ENV
       "LAIGE_DETCHECK_FIX_A=$<TARGET_FILE:detcheck-fixture-scenario>;")
string(APPEND LAIGE_DETCHECK_TEST_ENV
       "LAIGE_DETCHECK_FIX_PERTURBED=$<TARGET_FILE:detcheck-fixture-scenario-perturbed>;")
string(APPEND LAIGE_DETCHECK_TEST_ENV
       "LAIGE_DETCHECK_FIX_BAD=$<TARGET_FILE:detcheck-fixture-scenario-bad>;")
string(APPEND LAIGE_DETCHECK_TEST_ENV
       "LAIGE_DETCHECK_FIX_FAIL=$<TARGET_FILE:detcheck-fixture-scenario-fail>;")
string(APPEND LAIGE_DETCHECK_TEST_ENV
       "LAIGE_DETCHECK_FIX_SHORT=$<TARGET_FILE:detcheck-fixture-scenario-short>")
# M0-TEST-01 Windows CI diagnosis: tag every check-script detcheck
# invocation. The tool's env dump logs it, so a per-test diag file reveals
# whether its writer was the check script's own invocation. (The cmake
# versions some CI runners provide give execute_process no environment
# options, so the tag rides in the test's ENVIRONMENT property and is
# inherited by the spawned tool.)
string(APPEND LAIGE_DETCHECK_TEST_ENV
       ";LAIGE_DETCHECK_CALLER=check-script")

# --- The tests ---------------------------------------------------------------
# Same shape as the tests/api helper: a generated cmake -P check script
# asserts the exit code and the required output fragments.
function(laige_add_detcheck_test name expect_exit cmd)
  set(_checks "")
  foreach(_needle IN LISTS ARGN)
    # Escape ECMAScript metacharacters for the generated MATCHES check.
    string(REPLACE "\\" "\\\\" _esc "${_needle}")
    string(REPLACE "\"" "\\\"" _esc "${_esc}")
    string(REPLACE "(" "\\(" _esc "${_esc}")
    string(REPLACE ")" "\\)" _esc "${_esc}")
    string(REPLACE "+" "\\+" _esc "${_esc}")
    string(REPLACE "." "\\." _esc "${_esc}")
    string(REPLACE "*" "\\*" _esc "${_esc}")
    string(REPLACE "?" "\\?" _esc "${_esc}")
    string(REPLACE "|" "\\|" _esc "${_esc}")
    string(REPLACE "^" "\\^" _esc "${_esc}")
    string(REPLACE "$" "\\$" _esc "${_esc}")
    string(REPLACE "[" "\\[" _esc "${_esc}")
    string(REPLACE "]" "\\]" _esc "${_esc}")
    string(REPLACE "{" "\\{" _esc "${_esc}")
    string(REPLACE "}" "\\}" _esc "${_esc}")
    string(APPEND _checks
      "if(NOT _text MATCHES \"${_esc}\")\n"
      "  string(APPEND _problems \"output missing '<${_esc}'>; \")\n"
      "endif()\n")
  endforeach()
  set(CMD "${cmd}")
  set(EXPECT_EXIT "${expect_exit}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/expect-detcheck-result.cmake.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake" @ONLY)
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -P
           "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake")
  # A bounded timeout: a hung scenario capture would otherwise hang ctest.
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
  # The executable paths are carried in the test environment, not in the
  # command or baked into the script: $<TARGET_FILE> resolves per
  # configuration under multi-config generators (see the template header).
  # The quoted expansion keeps the ';'-separated pairs in ONE argument.
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "${LAIGE_DETCHECK_TEST_ENV}")
  # Windows CI investigation (M0-TEST-01): the tool flushes its spawn
  # diagnostics to a per-test file; the Windows CI job cats the files
  # after ctest (ctest hides passing-test output). POSIX ignores the
  # variable.
  if(WIN32)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/diag")
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT
      "${LAIGE_DETCHECK_TEST_ENV};LAIGE_DETCHECK_DIAG_FILE=${CMAKE_CURRENT_BINARY_DIR}/diag/${name}.txt")
  endif()
endfunction()

# (name, expected exit, quoted command list, required output fragments...)
laige_add_detcheck_test(detcheck-synthetic 0
  "--scenario=synthetic"
  "detcheck scenario=synthetic result=OK ticks=256"
  "run-a: synthetic\[seed=")

laige_add_detcheck_test(detcheck-synthetic-perturbed 1
  "--scenario=synthetic-perturbed"
  "detcheck scenario=synthetic-perturbed result=DIVERGED first_diff_tick=7")

laige_add_detcheck_test(detcheck-bin-identical 0
  "--run-a=$FIX_A$ --run-b=$FIX_A$"
  "result=OK ticks=32")

laige_add_detcheck_test(detcheck-bin-identical-args 0
  "--run-a=$FIX_A$ --run-b=$FIX_A$ -- --ticks=16"
  "result=OK ticks=16")

laige_add_detcheck_test(detcheck-bin-diverged 1
  "--run-a=$FIX_A$ --run-b=$FIX_PERTURBED$"
  "result=DIVERGED first_diff_tick=7")

laige_add_detcheck_test(detcheck-bin-malformed 2
  "--run-a=$FIX_A$ --run-b=$FIX_BAD$"
  "malformed line 4")

laige_add_detcheck_test(detcheck-scenario-failure 2
  "--run-a=$FIX_FAIL$ --run-b=$FIX_A$"
  "scenario run-a: scenario process exited with code 3")

laige_add_detcheck_test(detcheck-stream-mismatch 1
  "--run-a=$FIX_SHORT$ --run-b=$FIX_A$"
  "result=DIVERGED first_diff_tick=16"
  "stream ends: 16 ticks")

laige_add_detcheck_test(detcheck-unknown-scenario 2
  "--scenario=bogus"
  "unknown scenario 'bogus'")

if(LAIGE_TSAN)
  # Same first-report-fatal policy as the other tool entries (NFR-8.2).
  # NB: CTest's ENVIRONMENT property is replaced (not appended) by a
  # later set_tests_properties call, so the fixture paths are restated.
  set_tests_properties(
    detcheck-synthetic detcheck-synthetic-perturbed detcheck-bin-identical
    detcheck-bin-identical-args detcheck-bin-diverged detcheck-bin-malformed
    detcheck-scenario-failure detcheck-stream-mismatch
    detcheck-unknown-scenario
    PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1;${LAIGE_DETCHECK_TEST_ENV}")
endif()
