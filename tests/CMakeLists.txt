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
  set(_laige_gtest_flags /EHs- /EHc- /GR-)
endif()
foreach(_t IN ITEMS gtest gtest_main)
  target_compile_options(${_t} PRIVATE ${_laige_gtest_flags})
endforeach()

add_subdirectory(laige-core)
