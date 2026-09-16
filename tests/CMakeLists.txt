# Test suite root (added by the root CMakeLists only when
# LAIGE_BUILD_TESTS=ON, default ON).
#
# Convention (tests/README.md): one directory per engine module, mirroring
# src/; test executables are named <module>_tests. GoogleTest is a dev-only
# dependency (PRD §11) vendored in deps/googletest and locked in
# deps.lock; it is linked into test executables only — never into engine
# libraries (M0-DEP-01, DEP-004).

# --- GoogleTest (dev-only, vendored; M0-DEP-01) ----------------------------
# Set before add_subdirectory so GTest's own option() calls keep our values:
#   BUILD_GMOCK OFF — gmock is not needed yet (CORE-004); add it when a
#                     mock-based suite actually requires it.
#   INSTALL_GTEST OFF — the dependency is vendored in-tree, never installed
#                       (NFR-8.8: no install-time surprises, no network).
set(BUILD_GMOCK OFF)
set(INSTALL_GTEST OFF)
add_subdirectory(${CMAKE_SOURCE_DIR}/deps/googletest
                ${CMAKE_BINARY_DIR}/deps/googletest)

# Language consistency with the engine test translation units. GTest is
# pinned third-party code (DEP-005), so it does not take the engine's
# -Wall -Werror policy (it was verified warning-clean under that policy
# with GCC 16.2.1 and Clang 22.1.8 in M0-DEP-01; MSVC is verified in the
# M0-CI-01 job). The no-exceptions/no-rtti flags MUST match the flags of
# every test TU, because GTEST_HAS_EXCEPTIONS_ / GTEST_HAS_RTTI_ are
# detected per translation unit: a mismatch between the gtest library and
# a test TU would be an ODR violation (CPP-004). C++20 comes from the root
# project setting (GTest requires C++17+); the whole-tree sanitizer flags
# come from the root CMAKE_CXX_FLAGS (NFR-8.2).
# Applied per target (CMake 4.x rejects the multi-target form of
# target_compile_options).
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  set(_laige_gtest_flags -fno-exceptions -fno-rtti)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # /GR- only. GTest's own MSVC flags enable exceptions in its targets
  # (-EHsc -D_HAS_EXCEPTIONS=1, cmake/internal_utils.cmake); appending
  # /EHs- /EHc- after them leaves both on the command line (cl D9025) and
  # keeps the MS STL in exception mode although the compiler does not
  # unwind (warning C4530) — an inconsistent exception model between the
  # gtest library and the engine test TUs. Instead, the exception block of
  # GTest's flags is rewritten to GTest's documented no-exception set
  # (/EHs-c- -D_HAS_EXCEPTIONS=0, the same flags GTest's own
  # gtest_no_exception targets use), so one consistent model applies
  # everywhere and no conflicting flags appear on the command line.
  # Engine test TUs receive the matching _HAS_EXCEPTIONS=0 from
  # laige_apply_engine_policy() (the MS STL gates its own try/catch on
  # that macro, not on /EHs-), so the STL code paths agree on both
  # sides of the gtest boundary as well.
  set(_laige_gtest_flags /GR-)
  foreach(_t IN ITEMS gtest gtest_main)
    get_target_property(_laige_gtest_compile_flags ${_t} COMPILE_FLAGS)
    string(REPLACE "-EHsc -D_HAS_EXCEPTIONS=1"
                  "/EHs-c- -D_HAS_EXCEPTIONS=0"
                  _laige_gtest_compile_flags
                  "${_laige_gtest_compile_flags}")
    set_target_properties(${_t} PROPERTIES
      COMPILE_FLAGS "${_laige_gtest_compile_flags}")
  endforeach()
endif()
foreach(_t IN ITEMS gtest gtest_main)
  target_compile_options(${_t} PRIVATE ${_laige_gtest_flags})
endforeach()

add_subdirectory(laige-core)

# laige-sim tests (M1-ECS-01): the entity-handle and World suites
# (tests/laige-sim; CTest entry `entity`).
add_subdirectory(laige-sim)

# Test-infrastructure checks (M0-TEST-01): the seed-handling convention
# for randomized tests (tests/support/laige_test_seed.h) is exercised by
# tests/testing (suite `SeededRandom`, CTest entry `test_infra`).
add_subdirectory(testing)

# Tooling checks (M0-CI-03): the include-graph lint runs against fixture
# trees and the real repository tree (tests/tools).
add_subdirectory(tools)

# Public API manifest checks (M0-TOOL-01): the scanner runs against fixture
# trees and the real repository tree (tests/api).
add_subdirectory(api)

# Determinism checker checks (M0-TOOL-02): the tool runs the synthetic
# two-run scenario (built-in + fixture scenario binaries) in
# tests/detcheck.
add_subdirectory(detcheck)

# Replay runner checks (M1-DET-03): laige-replay replays a recorded
# log and compares the per-tick state hashes (tests/replay).
add_subdirectory(replay)
