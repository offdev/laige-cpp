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

if(LAIGE_BUILD_SHARED)
  add_library(laige-core SHARED version.cpp errors.cpp logging.cpp)
else()
  add_library(laige-core STATIC version.cpp errors.cpp logging.cpp)
endif()

laige_apply_engine_policy(laige-core)

# Public header root. Only this directory is exposed to consumers
# (CPP-010: no transitive leakage).
target_include_directories(laige-core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
