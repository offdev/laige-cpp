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

if(LAIGE_TSAN)
  # Same first-report-fatal policy as the other tool smoke tests
  # (NFR-8.2).
  set_tests_properties(laige_run_smoke
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
