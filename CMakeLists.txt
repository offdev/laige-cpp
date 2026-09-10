# ============================================================================
# Laige — Legendary AI Game Engine
#
# Root CMake project. Rules implemented here (see AGENTS.md and PRD.md):
#   - NFR-8.8  CMake >= 3.22; single configure; no network access to build
#   - NFR-8.9  static + shared library builds (LAIGE_BUILD_SHARED)
#   - NFR-8.10 C++20; -Wall -Werror on the engine; no exceptions/RTTI
#
# Roadmap: M0-REPO-01 created the skeleton; M0-BUILD-01 adds the
# laige-core target, the sanitizer/test options, and the canonical
# command set (docs/getting-started/building.md is the source of truth).
# Every engine target is passed through laige_apply_engine_policy().
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

# Test registration (unit tests ship with every implementation step).
enable_testing()

# ---------------------------------------------------------------------------
# Options (M0-BUILD-01). Canonical command forms that reference these flags
# are documented in docs/getting-started/building.md (source of truth).
# ---------------------------------------------------------------------------
# NFR-8.9: engine libraries build static by default; ON switches engine
# targets to shared libraries.
option(LAIGE_BUILD_SHARED
  "Build engine libraries as shared libraries (OFF: static)" OFF)

# Test suite under tests/ (default ON). Every M0 implementation step lands
# its unit tests in the same change; this option lets CI do a library-only
# build when a step needs one.
option(LAIGE_BUILD_TESTS "Build the test suite (tests/)" ON)

# Sanitizer builds (NFR-8.2). Mutually exclusive: a build tree is either
# instrumented with ASan+UBSan or with TSan, never both.
option(LAIGE_ASAN
  "Instrument the whole tree with AddressSanitizer + UBSan" OFF)
option(LAIGE_TSAN
  "Instrument the whole tree with ThreadSanitizer" OFF)

# Reserved for the optional scripting module (M4). No effect in M0; declared
# now so later canonical commands can name it without a breaking option
# introduction. (CORE-004: no wiring added before it is needed.)
option(LAIGE_SCRIPT
  "Reserved: scripting module support (no effect in M0)" OFF)

if(LAIGE_ASAN AND LAIGE_TSAN)
  message(FATAL_ERROR
    "LAIGE_ASAN and LAIGE_TSAN are mutually exclusive; configure two "
    "separate build trees (canonical: build-asan and build-tsan).")
endif()

# Sanitizer instrumentation (NFR-8.2) is applied to the whole tree — engine
# libraries, tests, and future tools — so a report anywhere fails the
# build loudly (CORE-008: no silent failure). Flags are set before any
# target is defined so every target in this tree inherits them.
if(LAIGE_ASAN)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC 2022 (PRD §6 P0): ASan is full; UBSan is the supported subset.
    set(_laige_sanitizer_flags "/fsanitize=address")
  else()
    # UBSan reports are made fatal (-fno-sanitize-recover=all); frame
    # pointers are kept so ASan stack traces stay usable.
    set(_laige_sanitizer_flags
        "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_laige_sanitizer_flags}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_laige_sanitizer_flags}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_laige_sanitizer_flags}")
elseif(LAIGE_TSAN)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_laige_sanitizer_flags "/fsanitize=thread")
  else()
    set(_laige_sanitizer_flags "-fsanitize=thread")
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_laige_sanitizer_flags}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_laige_sanitizer_flags}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_laige_sanitizer_flags}")
endif()

# ---------------------------------------------------------------------------
# Engine target policy (NFR-8.10, AGENTS CORE-010)
# ---------------------------------------------------------------------------
# Every engine target MUST be passed through laige_apply_engine_policy():
# C++20, warnings-as-errors, exceptions and RTTI disabled. The MSVC options
# are the documented equivalent of
# -Wall -Werror -fno-exceptions -fno-rtti: /EHs- /EHc- clear C++ exception
# handling (Microsoft /EH reference), /GR- disables RTTI.
#
# The warning/language flags are PRIVATE: they govern this target's own
# translation units and must not leak into consumers (CPP-010) — a game
# linking the engine keeps its own compiler policy. The C++20 *requirement*
# does propagate (INTERFACE compile feature): engine headers are C++20, so
# consumers must build at least C++20.
function(laige_apply_engine_policy target)
  # PUBLIC: this target compiles as C++20 and consumers are required to be
  # C++20 as well (engine headers are C++20, NFR-8.10).
  target_compile_features(${target} PUBLIC cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(${target} PRIVATE
      -Wall -Werror -fno-exceptions -fno-rtti)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${target} PRIVATE
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
# One directory per module. Only laige-core is populated (M0-BUILD-01);
# later milestones add their targets in place.
add_subdirectory(src/laige-core)

# ---------------------------------------------------------------------------
# Tests (gated by LAIGE_BUILD_TESTS; conventions in tests/README.md)
# ---------------------------------------------------------------------------
if(LAIGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
