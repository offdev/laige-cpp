# laige-core — engine foundation module (PRD §10.1).
#
# Dependency rule (PRD §10.1): laige-core depends on nothing internal, so
# this target links no other engine target and exposes no transitive
# includes or linked dependencies (CPP-010). Consumers of laige-core see
# exactly its PUBLIC include directory and nothing else.
#
# M0-BUILD-01: the library target. Static by default; LAIGE_BUILD_SHARED=ON
# builds it shared (NFR-8.9). The target currently carries the
# version/build-identifier code (version.cpp) plus the first functional
# engine code (M0-CORE-01: Result/Status + error registry, errors.cpp)
# and the structured logging facade (M0-CORE-02, logging.cpp); further
# functional code lands in the remaining M0-CORE-xx steps. The link
# smoke test in tests/laige-core/ verifies the library in both variants.

# M1-ALLOC-01: the G-R1 zero-allocation watch (the counting backend for
# the debug per-tick sim assertion, docs/api/alloc_watch.md).
# alloc_watch.cpp carries the strong global operator new/new[]
# overrides that count heap allocations into the armed window; the
# LAIGE_ALLOC_WATCH definition marks every tree where the counting
# backend is compiled in (it is PUBLIC so every consumer — the sim
# module, the tools, the test executables — sees the same state). The
# TU is excluded from the sanitizer trees: the sanitizer runtimes
# define their own new/delete, so the overrides cannot be linked
# there. There, the zero-allocation property is covered by the
# leak-free sanitizer run of the same loop plus the pool
# reservation-delta assertion (the established fallback pattern —
# tests/laige-core, M0-CORE-02/05; tests/laige-sim, M1-ECS-03/07).
if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN)
  set(LAIGE_CORE_SOURCES version.cpp errors.cpp logging.cpp
       sim_math.cpp sim_math_fixed.cpp json.cpp budget_harness.cpp
       alloc_watch.cpp)
else()
  set(LAIGE_CORE_SOURCES version.cpp errors.cpp logging.cpp
       sim_math.cpp sim_math_fixed.cpp json.cpp budget_harness.cpp)
endif()

if(LAIGE_BUILD_SHARED)
  add_library(laige-core SHARED ${LAIGE_CORE_SOURCES})
else()
  add_library(laige-core STATIC ${LAIGE_CORE_SOURCES})
endif()

if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN)
  target_compile_definitions(laige-core PUBLIC LAIGE_ALLOC_WATCH=1)
endif()

laige_apply_engine_policy(laige-core)

# M0-CORE-03: laige-core carries the SimMath deterministic-math interface
# (include/laige/sim_math.h); pin its IEEE float semantics (ADR 0002,
# PRD §10.3). Every later sim module passes through this same policy.
laige_apply_simmath_policy(laige-core)

# Public header root. Only this directory is exposed to consumers
# (CPP-010: no transitive leakage).
target_include_directories(laige-core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
