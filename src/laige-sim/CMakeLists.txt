# laige-sim — game simulation module (PRD §10.1).
#
# Dependency rule (PRD §10.1, NFR-8.11): laige-sim sits above
# laige-core in the module stack and may include only laige-core's
# PUBLIC headers — enforced by tools/laige-include-lint (R2) and kept
# visible here through the single PUBLIC link below. Consumers of
# laige-sim see exactly its PUBLIC include directory plus laige-core's
# (required: laige-sim's public headers include laige-core headers,
# CPP-010).
#
# M1-ECS-01 (roadmap/M1-heartbeat.md): laige-sim is the first module
# beyond laige-core. This step lands the entity handle and the World
# entity storage (include/laige/sim/entity.h, entity.cpp); the game
# loop, systems, and the rest of the ECS land in the remaining M1-ECS /
# M1-SYS / M1-LOOP steps.

# M1-ECS-03 (roadmap/M1-heartbeat.md): archetype.cpp adds the archetype
# SoA storage helpers (find/create/grow/attach/detach + the type-key
# index); entity.cpp keeps the handle/slot storage and the World
# lifecycle.
set(LAIGE_SIM_SOURCES entity.cpp archetype.cpp)

if(LAIGE_BUILD_SHARED)
  add_library(laige-sim SHARED ${LAIGE_SIM_SOURCES})
else()
  add_library(laige-sim STATIC ${LAIGE_SIM_SOURCES})
endif()

laige_apply_engine_policy(laige-sim)

# M1: every sim module carries the SimMath pinned-math policy
# (ADR 0002, PRD §10.3; see laige_apply_simmath_policy in the root
# CMakeLists.txt — "from M1 on, every sim module joins the list").
laige_apply_simmath_policy(laige-sim)

# laige-core is laige-sim's only internal dependency (PRD §10.1:
# arrows only downward). PUBLIC: the public headers include
# laige-core headers.
target_link_libraries(laige-sim PUBLIC laige-core)

# Public header root. Only this directory is exposed to consumers
# (CPP-010: no transitive leakage).
target_include_directories(laige-sim PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
