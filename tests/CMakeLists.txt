# Test suite root (added by the root CMakeLists only when
# LAIGE_BUILD_TESTS=ON, default ON).
#
# Convention (tests/README.md): one directory per engine module, mirroring
# src/; test executables are named <module>_tests. GoogleTest is a dev-only
# dependency vendored in M0-DEP-01 and is never linked into engine
# libraries; until then tests are plain C++ executables registered with
# CTest (no third-party dependency).

add_subdirectory(laige-core)
