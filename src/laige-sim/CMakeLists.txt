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
# lifecycle. M1-ECS-04 adds query.cpp: the iteration-legality guard
# behind World::each (the query template itself is header-defined in
# entity.h, next to the component access templates). M1-ECS-06 adds
# guardrails.cpp: the G-R3 entity-count thresholds and the G-R4
# per-frame churn budget (the check hooks live in entity.cpp's
# create() and entity.h's add/remove templates). M1-SYS-01 adds
# systems.cpp: the non-template World methods of the system registry
# (systemCount, system + the SystemInfo queries); the registration
# template (registerSystem) is header-defined in entity.h like
# registerComponent (the M1-ECS-02 pattern). M1-SYS-02 adds the
# system scheduler to the same systems.cpp (scheduleSystems,
# runSystems + the depends_on spec parse — no new source file; the
# public types and contract live in include/laige/sim/system.h).
# M1-SYS-03 adds system_timing.cpp: the per-system timing + budget
# enforcement (the World::systemTimingStats/systemTimingWindow
# queries and the World::checkSystemBudget hook that runSystems
# calls per system per tick; the public types and contract live in
# include/laige/sim/system.h). M1-LOOP-01 adds game_loop.cpp: the
# fixed-timestep accumulator loop (GameLoop::create/frame/runOneTick
# — the exact-ticks due computation, the bounded catch-up run loop,
# the tick_dropped overload warn; the public types and contract live
# in include/laige/sim/game_loop.h). M1-LOOP-02 adds
# include/laige/sim/presentation.h (header-only — a class template,
# the M1-ECS-02 pattern): the first built-in component (Position2D,
# one template instantiation per SimMath backend, ADR 0002) and the
# PresentationSnapshot (per-tick prev/curr presentation state, the
# exact-integer clamped alpha, sample_position — no new .cpp; the
# GameLoop gains the Options::onTick per-completed-tick hook and
# startReferenceNs() in game_loop.h/.cpp to drive it).
# startReferenceNs() in game_loop.h/.cpp to drive it). M1-HEAD-01
# adds engine.cpp: the headless engine run (Engine::create/
# run_headless/shutdown — config -> world -> systems -> loop, the
# presentation onTick wiring this step owns, and the ordered CONC-006
# shutdown; the public types and contract live in
# include/laige/sim/engine.h). M1-CFG-01 adds config.cpp +
# include/laige/sim/config.h: the final versioned declarative config
# schema (EngineConfig — moved from engine.h — plus the version gate,
# the budgets/camera/asset_roots blocks, parseEngineConfig,
# loadGameConfig, the EngineConfigOverride merge, and the
# debug-only ConfigHotReloader — the provisional JSON config surface
# in engine.cpp is folded into it).
# M1-DET-02 adds replay.cpp: the replay recording (ReplayRecorder —
# the versioned log format, the atomic temp+rename writer, the
# parseReplay/loadReplay readers, and the replay-identity hashes; the
# public types and contract live in include/laige/sim/replay.h) plus
# the Engine::startReplayRecording wiring in engine.cpp (the per-tick
# empty-frame write, the mid-run failure stop, the finalization in
# run_headless, the abandonment in shutdown).
# M1-DET-03 adds state_hash.cpp: the deterministic state hash
# (World::stateHash — the per-tick replay hash line, a pure function of
# the live state; the public contract lives in include/laige/sim/
# entity.h) and the replay EXECUTION half in replay.cpp
# (replayIdentityDiff + runReplay; the public types and contract live
# in include/laige/sim/replay.h). M1-DET-05 adds replay_diff.cpp: the
# replay DIFF (diffReplays — the identity-checked lock-step tick walk
# that aligns two replays on World::componentStateHash (state_hash.cpp,
# the canonical steps 1-4 shared with stateHash) and reports the first
# divergent tick plus the bounded state diff (World::stateDiff —
# entity.h); the public types and contract live in include/laige/sim/
# replay_diff.h). M1-PROF-01 adds profiler.cpp: the always-on profiler
# counters (Profiler — the tick/frame rolling windows, the render/
# network counter fields, the snapshot, the text/JSON report format +
# write; the public types and contract live in
# include/laige/sim/profiler.h) plus the GameLoop Options::profiler
# per-tick timing hook and the Engine's frame timing + per-run report
# finalization (game_loop.cpp, engine.cpp).
set(LAIGE_SIM_SOURCES entity.cpp archetype.cpp query.cpp guardrails.cpp
                     systems.cpp system_timing.cpp game_loop.cpp
                     engine.cpp config.cpp replay.cpp state_hash.cpp
                     replay_diff.cpp profiler.cpp)

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
