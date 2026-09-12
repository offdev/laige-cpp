# Test-infrastructure checks (M0-TEST-01).
#
# The seed-handling convention for randomized tests (docs/testing.md §4)
# lives in tests/support/laige_test_seed.h; this suite exercises it:
# known-answer FNV-1a hashes of a fixed draw count under the default seed
# and a LAIGE_TEST_SEED override, the one-repo-wide default seed's
# identity with laige-fuzz's, the seed parser, the loud-failure path of an
# invalid LAIGE_TEST_SEED, and substream isolation.
# The committed KATs make the suite a replay fixture — identical on every
# build, compiler, and CI run (M0-TEST-01 Verify clause) — and the
# `test-seed-check` line printed to stdout is the artifact-log identity
# check across two CI runs.
#
# Layout convention (docs/testing.md §1): tests/<module> mirrors
# src/<module> and executables are named <module>_tests; this directory is
# the non-module home of the test-infrastructure checks (the tooling
# checks live in tests/tools, tests/api, and tests/detcheck).

add_executable(test_infra_tests seeded_random_tests.cpp)
target_include_directories(test_infra_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/support)
laige_apply_engine_policy(test_infra_tests)
target_link_libraries(test_infra_tests PRIVATE gtest_main laige-core)

# Step Verify command: `ctest -R test_infra` selects exactly the
# SeededRandom suite. The gtest filter is one UNQUOTED argument (CTest
# passes quoted arguments through with the literal quote characters —
# see the note in tests/laige-core/CMakeLists.txt).
add_test(NAME test_infra COMMAND test_infra_tests --gtest_filter=SeededRandom.*)
