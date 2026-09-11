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
# command set (docs/getting-started/building.md is the source of truth);
# M0-DEP-01 adds the vendored-dependency lock check (deps.lock).
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

# MSVC: CMake's Windows platform module adds /EHsc to CMAKE_CXX_FLAGS by
# default (CMP0117 NEW, CMake >= 3.20). The engine's no-exception policy
# (NFR-8.10) supplies /EHs- /EHc- per target via
# laige_apply_engine_policy(); leaving the platform /EHsc in place makes
# the two collide on every engine target's command line (cl D9025). Strip
# the platform default so each target's policy flags are the single source
# of truth for the exception model.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  string(REGEX REPLACE "([/-])EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

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
# Vendored dependency lock (M0-DEP-01; PRD §11, NFR-8.6, DEP-005)
# ---------------------------------------------------------------------------
# deps.lock pins every vendored dependency under deps/ (name, version,
# source URL + commit, tree SHA-256, license, justification). This check
# re-hashes the vendored trees on every configure and fails loudly on any
# drift, missing tree, or unlisted vendored directory (CORE-008). It runs
# for every build tree, including library-only builds.
include(cmake/laige-deps-lock.cmake)
laige_deps_verify_lock()

# ---------------------------------------------------------------------------
# Engine target policy (NFR-8.10, AGENTS CORE-010)
# ---------------------------------------------------------------------------
# Every engine target MUST be passed through laige_apply_engine_policy():
# C++20, warnings-as-errors, exceptions and RTTI disabled. The MSVC options
# are the documented equivalent of
# -Wall -Werror -fno-exceptions -fno-rtti: /EHs- /EHc- clear C++ exception
# handling (Microsoft /EH reference), /GR- disables RTTI, and
# _HAS_EXCEPTIONS=0 switches the MS STL to its no-exception code paths:
# the STL gates its own try/catch (iosfwd _TRY_IO_BEGIN/_CATCH_IO_,
# yvals.h _TRY_BEGIN/_CATCH) on the _HAS_EXCEPTIONS macro, not on the /EH
# flags — with /EHs- alone it would still emit catch blocks that the
# compiler cannot unwind (cl C4530, fatal under /WX).
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
    # See the function header: the MS STL needs _HAS_EXCEPTIONS=0 to take
    # its no-exception code paths (same define GTest's own no-exception
    # targets use; deps/googletest/cmake/internal_utils.cmake).
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  else()
    message(FATAL_ERROR
      "laige: no engine compiler policy for '${CMAKE_CXX_COMPILER_ID}'; "
      "supported: GNU, Clang/AppleClang, MSVC 2022 (PRD §6).")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# SimMath pinned-math policy (M0-CORE-03; ADR 0002, PRD §10.3)
# ---------------------------------------------------------------------------
# Every target that carries deterministic sim math MUST be passed through
# laige_apply_simmath_policy(). In M0 that is laige-core (the SimMath
# home) and its tests; from M1 on, every sim module (laige-sim, and
# authoritative-sim code in laige-net/laige-server) joins the list.
#
# The flags implement ADR 0002's pinned set for the fp32_pinned backend
# (full rationale in src/laige-core/include/laige/sim_math.h):
#
#   GCC / Clang / AppleClang:
#     -ffp-contract=off      a*b+c is never fused into a single-rounding
#                            FMA (protects SimMath::lerp / ::length's
#                            documented two-rounding)
#     -fno-associative-math  sums are never reassociated (already the
#                            default; passed explicitly so the pinned set
#                            is visible on every compile line)
#   MSVC 2022:
#     /fp:precise            the documented precision model; MSVC does not
#                            FMA-contract C expressions and never
#                            reassociates at this setting (passed
#                            explicitly — it is the default — so the
#                            pinned set is visible on every compile line)
#
#   Banned in sim translation units, all compilers: -ffast-math /
#   -funsafe-math-optimizations / /fp:fast, floating-point intrinsics,
#   rounding-mode changes, and FP exception modes (re-audited at every
#   toolchain upgrade, ADR 0002).
function(laige_apply_simmath_policy target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(${target} PRIVATE
      -ffp-contract=off -fno-associative-math)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${target} PRIVATE /fp:precise)
  else()
    message(FATAL_ERROR
      "laige: no simmath compiler policy for '${CMAKE_CXX_COMPILER_ID}'; "
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
  # Dev tools that need the built engine library (gated with tests: a
  # library-only build does not need them).
  add_subdirectory(tools/fuzz)
  add_subdirectory(tools/bench)  # M0-CORE-08: laige-bench
endif()
