# laige-run (M1-HEAD-01): the headless engine run binary.
#
# Canonical command (docs/getting-started/building.md):
#
#   ./build/bin/laige-run --headless <config.json> [--ticks N]
#                         [--replay <log>]
#
# Loads the declarative config (the provisional M1-HEAD-01 JSON
# surface), builds the laige::Engine, and runs the headless loop for
# the requested ticks (the server form: 0/omitted). The windowed mode
# is M2. Gated with the test suite like the other tools: a
# library-only build does not need it.
#
# Headless invariants (ARCH-003, NFR-8.11): the include-graph lint
# guarantees this target's include closure (laige-sim + laige-core)
# touches no GL/window/audio/input API.

add_executable(laige-run laige-run.cpp)
laige_apply_engine_policy(laige-run)
# laige-sim is the only internal dependency (PRD §10.1: arrows only
# downward); laige-core's public headers come through it (CPP-010).
target_link_libraries(laige-run PRIVATE laige-sim)

# Smoke test — the M1-HEAD-01 Verify clause in executable form: a
# bounded 1000-tick headless run (60 Hz, the committed fixture config)
# exits 0 and prints the one-line summary. This entry runs in EVERY
# P0 OS job's ctest (the roadmap clause "exits 0 in CI on all P0
# OSes"). The run is wall-clock paced (~16.7 s at 60 Hz) — the
# timeout carries the margin for loaded runners; a dropped tick
# (overload) still exits 0 (the GameLoop's bounded catch-up).
add_test(NAME laige_run_smoke
         COMMAND laige-run --headless
                 ${CMAKE_SOURCE_DIR}/tests/laige-sim/fixtures/headless_smoke.json
                 --ticks 1000)
set_tests_properties(laige_run_smoke PROPERTIES TIMEOUT 300
                                PASS_REGULAR_EXPRESSION "status=ok")

# Budget-report smoke (M1-PROF-02, FR-11.2): a bounded 30-tick run
# with the opt-in budget report on (the AGENTS §12 field report to
# stdout) and the CI gate on (--fail-on-budget: a non-zero exit —
# 3 — when the report is overall=FAIL). The fixture config declares
# no systems (the 0-system smoke config — the report's system section
# is empty, over_budget: none) and the repo's budgets.json targets
# (3 ms tick mean / 5 ms tick p99 / 0 per-frame sim allocs) pass the
# ~microsecond ticks and the steady-state zero-alloc sim, so
# overall=PASS is expected: the test passes iff the report prints
# overall=PASS AND the run exits 0 (--fail-on-budget maps a FAIL
# report to exit 3, which ctest fails). The budgets path is passed
# EXPLICITLY (the ctest CWD is the build tree — the "budgets.json"
# working-directory fallback would not resolve there).
add_test(NAME laige_run_budget
         COMMAND laige-run --headless
                 ${CMAKE_SOURCE_DIR}/tests/laige-sim/fixtures/headless_smoke.json
                 --ticks 30 --budget-report 4
                 --budgets ${CMAKE_SOURCE_DIR}/budgets.json
                 --fail-on-budget)
set_tests_properties(laige_run_budget PROPERTIES TIMEOUT 120
                                PASS_REGULAR_EXPRESSION "overall=PASS")

if(LAIGE_TSAN)
  # Same first-report-fatal policy as the other tool smoke tests
  # (NFR-8.2).
  set_tests_properties(laige_run_smoke laige_run_budget
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
