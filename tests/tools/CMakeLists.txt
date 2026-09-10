# include-graph lint tests (M0-CI-03)
#
# tools/laige-include-lint is a plain-Python (stdlib-only) CI script, so it
# has no C++ suite of its own. These CTest tests exercise it against
# synthetic fixture trees (generated here at configure time) and against
# the real repository tree:
#
#   include-lint-clean         clean module graph           → exit 0
#   include-lint-violations    one file per rule R1–R4      → exit 1
#   include-lint-dep-budget    11 vendored deps             → exit 2
#   include-lint-unknown-module  src/laige-audio present    → exit 2
#   include-lint-real-tree     the live repository          → exit 0
#
# Each test is a generated `cmake -P` check script (see below) asserting
# the exit code and the required output fragments — not CTest regex
# properties (WILL_FAIL inverts them; see the function comment below).
#
# The real-tree test is the same check the `include-lint` CI job runs
# (ci-pull.yml / ci.yml), so a broken include lands red in every P0 job
# as well as in the dedicated job. The fixtures mirror the rules exactly
# as documented in the script header; adding a rule there requires a
# matching fixture here (CORE-007).

find_program(LAIGE_LINT_PYTHON NAMES python3 python
  DOC "Python 3 interpreter for tools/laige-include-lint")
if(NOT LAIGE_LINT_PYTHON)
  message(FATAL_ERROR
    "include-lint tests need a Python 3 interpreter (python3 or python) "
    "on PATH; every P0 runner image provides one (PRD §6).")
endif()

set(FX "${CMAKE_BINARY_DIR}/lint-fixtures")
file(REMOVE_RECURSE "${FX}")

# --- fixture: clean module graph (expect exit 0) --------------------------
file(MAKE_DIRECTORY "${FX}/clean/src/laige-core/include/laige/core")
file(MAKE_DIRECTORY "${FX}/clean/src/laige-sim/include/laige/sim")
file(WRITE "${FX}/clean/deps.lock" "{
  \"version\": 1,
  \"entries\": []
}")
file(WRITE "${FX}/clean/src/laige-core/include/laige/core/a.h" "#pragma once
")
file(WRITE "${FX}/clean/src/laige-core/core.cpp" "#include <cstdint>

#include \"laige/core/a.h\"
")
file(WRITE "${FX}/clean/src/laige-sim/include/laige/sim/s.h" "#pragma once
")
file(WRITE "${FX}/clean/src/laige-sim/detail.h" "#pragma once
")
file(WRITE "${FX}/clean/src/laige-sim/sim.cpp" "#include <laige/core/a.h>

#include \"detail.h\"

#include \"laige/sim/s.h\"
")

# --- fixture: one violation per rule R1–R4 (expect exit 1) -----------------
file(MAKE_DIRECTORY "${FX}/violations/src/laige-core/include/laige/core")
file(MAKE_DIRECTORY "${FX}/violations/src/laige-sim/include/laige/sim")
file(MAKE_DIRECTORY "${FX}/violations/src/laige-render/include/laige/render")
file(MAKE_DIRECTORY "${FX}/violations/deps/gteststub/include/gteststub")
file(MAKE_DIRECTORY "${FX}/violations/tests")
file(WRITE "${FX}/violations/deps.lock" "{
  \"version\": 1,
  \"entries\": [
    {
      \"name\": \"gteststub\",
      \"version\": \"0.1\",
      \"path\": \"deps/gteststub\",
      \"owner\": \"tests\",
      \"source_url\": \"https://example.invalid/gteststub\",
      \"source_commit\": \"0000000000000000000000000000000000000000\",
      \"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\",
      \"license\": \"MIT\",
      \"justification\": \"fixture: dev-only dep owned by tests/\"
    }
  ]
}")
file(WRITE "${FX}/violations/deps/gteststub/include/gteststub/g.h" "#pragma once
")
file(WRITE "${FX}/violations/tests/hidden.h" "#pragma once
")
file(WRITE "${FX}/violations/src/laige-core/include/laige/core/a.h" "#pragma once
")
file(WRITE "${FX}/violations/src/laige-core/priv.h" "#pragma once
")
file(WRITE "${FX}/violations/src/laige-sim/include/laige/sim/s.h" "#pragma once
")
file(WRITE "${FX}/violations/src/laige-render/include/laige/render/r.h" "#pragma once
")
# R1: laige-core includes an internal module (laige-sim).
file(WRITE "${FX}/violations/src/laige-core/r1.cpp" "#include <laige/sim/s.h>
")
# R2: laige-sim includes a module above it in the PRD §10.1 stack.
file(WRITE "${FX}/violations/src/laige-sim/r2.cpp" "#include <laige/render/r.h>
")
# R3: laige-core includes a vendored dep it does not own (owner: tests).
file(WRITE "${FX}/violations/src/laige-core/r3.cpp"
  "#include \"../../deps/gteststub/include/gteststub/g.h\"
")
# R4: an include that resolves outside src/** and deps/** (tests/).
file(WRITE "${FX}/violations/src/laige-sim/r4.cpp"
  "#include \"../../tests/hidden.h\"
")
# R2 (public boundary): laige-sim reaches laige-core's private code.
file(WRITE "${FX}/violations/src/laige-sim/r5.cpp"
  "#include \"../laige-core/priv.h\"
")
# R3 (angle-bracket): a vendored header path included from a non-owner.
file(WRITE "${FX}/violations/src/laige-sim/r6.cpp" "#include <gteststub/g.h>
")

# --- fixture: 11 vendored deps exceed the PRD §11 budget (expect exit 2) ---
set(_budget_entries "")
foreach(_n IN ITEMS 00 01 02 03 04 05 06 07 08 09 10)
  if(NOT _budget_entries STREQUAL "")
    string(APPEND _budget_entries ",")
  endif()
  string(APPEND _budget_entries "
    {
      \"name\": \"dep${_n}\",
      \"version\": \"0.1\",
      \"path\": \"deps/dep${_n}\",
      \"owner\": \"tests\",
      \"source_url\": \"https://example.invalid/dep\",
      \"source_commit\": \"0000000000000000000000000000000000000000\",
      \"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\",
      \"license\": \"MIT\",
      \"justification\": \"fixture: over-budget entry\"
    }")
  # Unique header name per dep: identical names across dep trees would be
  # an ambiguous vendored include path (a separate structural failure the
  # lint checks, and not what this fixture is meant to exercise).
  file(MAKE_DIRECTORY "${FX}/budget/deps/dep${_n}")
  file(WRITE "${FX}/budget/deps/dep${_n}/dep${_n}.h" "#pragma once
")
endforeach()
file(WRITE "${FX}/budget/deps.lock" "{
  \"version\": 1,
  \"entries\": [${_budget_entries}
  ]
}")
file(MAKE_DIRECTORY "${FX}/budget/src/laige-core/include/laige/core")
file(WRITE "${FX}/budget/src/laige-core/include/laige/core/a.h" "#pragma once
")
file(WRITE "${FX}/budget/src/laige-core/core.cpp" "#include <cstdint>
")

# --- fixture: src/ subdirectory that is not a PRD §10.1 module (exit 2) ----
file(MAKE_DIRECTORY "${FX}/unknown-module/src/laige-audio")
file(WRITE "${FX}/unknown-module/deps.lock" "{
  \"version\": 1,
  \"entries\": []
}")
file(WRITE "${FX}/unknown-module/src/laige-audio/audio.cpp" "#include <cstdint>
")

# --- the tests -------------------------------------------------------------
# Each test runs a generated `cmake -P` check script
# (expect-lint-result.cmake.in) that asserts the lint's exit code AND the
# required output fragments. CTest properties cannot express this safely:
# CTest inverts PASS_REGULAR_EXPRESSION when WILL_FAIL is set (verified on
# CMake 4.4.3), and an interpreter crash exits 1 — the same code as a
# correct lint failure — so content must be checked, not just the code.
function(laige_add_lint_test name expect_exit root)
  set(_checks "")
  foreach(_needle IN LISTS ARGN)
    string(REPLACE "\\" "\\\\" _esc "${_needle}")
    string(REPLACE "\"" "\\\"" _esc "${_esc}")
    string(APPEND _checks
      "if(NOT _text MATCHES \"${_esc}\")\n"
      "  string(APPEND _problems \"output missing '<${_esc}'>; \")\n"
      "endif()\n")
  endforeach()
  set(PYTHON "${LAIGE_LINT_PYTHON}")
  set(LINT "${CMAKE_SOURCE_DIR}/tools/laige-include-lint")
  set(ROOT "${root}")
  set(EXPECT_EXIT "${expect_exit}")
  set(CHECKS "${_checks}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/expect-lint-result.cmake.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake" @ONLY)
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -P
           "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake")
endfunction()

# (name, expected exit, tree, required output fragments...)
laige_add_lint_test(include-lint-clean 0 "${FX}/clean"
  "laige-include-lint: OK")
laige_add_lint_test(include-lint-violations 1 "${FX}/violations"
  "6 include violation" "R1:" "R2:" "R3:" "R4:")
laige_add_lint_test(include-lint-dep-budget 2 "${FX}/budget"
  "count: 11" "exceed the PRD" "budget of 10")
laige_add_lint_test(include-lint-unknown-module 2 "${FX}/unknown-module"
  "src/laige-audio: not a PRD")
laige_add_lint_test(include-lint-real-tree 0 "${CMAKE_SOURCE_DIR}"
  "laige-include-lint: OK")
