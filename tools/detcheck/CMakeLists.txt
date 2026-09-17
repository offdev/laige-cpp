# laige-detcheck (M0-TOOL-02): the determinism checker skeleton
# (FR-11.5; AGENTS ARCH-010, TEST-004).
#
# Canonical command (docs/getting-started/building.md):
#
#   ./build/bin/laige-detcheck --scenario=<name>
#
# Runs a named scenario in two build configurations and compares the
# per-tick state-hash streams (the scenario contract — `<tick> <hash>`
# lines, 16 lowercase hex hash digits — is documented in the header
# comment of laige-detcheck.cpp and in docs/api/detcheck.md). The
# built-in `synthetic` scenario is the M0 self-check; M1-DET-04
# activated the real two-configuration mode (--run-a/--run-b +
# --compare-combined) on M1-SAMPLE-01's hello scenario — the CI matrix
# is in docs/benchmarks/determinism-matrix.md.
#
# Gated with the test suite like tools/fuzz, tools/bench, and tools/api:
# a library-only build does not need it.
#
# The built-in synthetic workload uses only fpx16_16 (pure integer
# arithmetic) and laige::Prng — no IEEE float — so it does not need
# laige_apply_simmath_policy (that policy pins fp32_pinned IEEE semantics;
# integer arithmetic is bit-exact by the language standard, ADR 0002).

add_executable(laige-detcheck laige-detcheck.cpp)
laige_apply_engine_policy(laige-detcheck)
target_link_libraries(laige-detcheck PRIVATE laige-core)

# The report's run labels record the build type (AGENTS 12 context);
# CMake stamps it so the binary does not have to guess.
target_compile_definitions(laige-detcheck PRIVATE
  LAIGE_DETCHECK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
