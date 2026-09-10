# ============================================================================
# Laige — Legendary AI Game Engine
#
# Root CMake project. Rules implemented here (see AGENTS.md and PRD.md):
#   - NFR-8.8  CMake >= 3.22; single configure; no network access to build
#   - NFR-8.9  static + shared library builds (LAIGE_BUILD_SHARED)
#   - NFR-8.10 C++20; -Wall -Werror on the engine; no exceptions/RTTI
#
# Roadmap M0-REPO-01: this is the skeleton. The first engine target
# (laige-core) lands in M0-BUILD-01 and must call
# laige_apply_engine_policy() for every engine target.
# ============================================================================

cmake_minimum_required(VERSION 3.22)

project(laige
  DESCRIPTION "Laige — Legendary AI Game Engine"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Engine-wide language policy (NFR-8.10)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Both the static (default) and shared (LAIGE_BUILD_SHARED) library variants
# are position-independent so they link identically into games and tools
# (NFR-8.9).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Canonical commands always pass -DCMAKE_BUILD_TYPE explicitly
# (roadmap/README.md §1); default to Debug when nothing is given.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Predictable artifact locations; canonical commands reference build/bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY  ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY  ${CMAKE_BINARY_DIR}/lib)

# Test registration; test targets land in M0-BUILD-01 / M0-DEP-01.
enable_testing()

# ---------------------------------------------------------------------------
# Options (placeholders; wired into targets by later M0 steps)
# ---------------------------------------------------------------------------
# NFR-8.9: engine libraries build static by default; ON switches engine
# targets to shared libraries.
option(LAIGE_BUILD_SHARED
  "Build engine libraries as shared libraries (OFF: static)" OFF)

# ---------------------------------------------------------------------------
# Engine target policy (NFR-8.10, AGENTS CORE-010)
# ---------------------------------------------------------------------------
# Every engine target MUST be passed through laige_apply_engine_policy():
# C++20, warnings-as-errors, exceptions and RTTI disabled. The MSVC options
# are the documented equivalent of
# -Wall -Werror -fno-exceptions -fno-rtti: /EHs- /EHc- clear C++ exception
# handling (Microsoft /EH reference), /GR- disables RTTI.
function(laige_apply_engine_policy target)
  target_compile_features(${target} REQUIRED cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(${target} PUBLIC
      -Wall -Werror -fno-exceptions -fno-rtti)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${target} PUBLIC
      /W4 /WX /EHs- /EHc- /GR-)
  else()
    message(FATAL_ERROR
      "laige: no engine compiler policy for '${CMAKE_CXX_COMPILER_ID}'; "
      "supported: GNU, Clang/AppleClang, MSVC 2022 (PRD §6).")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Modules (PRD §10.1 module map)
# ---------------------------------------------------------------------------
# One directory per module. Only laige-core is populated now (M0-REPO-01);
# M0-BUILD-01 adds the laige-core library target in place.
add_subdirectory(src/laige-core)
