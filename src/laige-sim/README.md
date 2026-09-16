# laige-sim

Module per PRD §10.1: game loop, physics, input state, animation state,
pathing/steering, AI hooks.

Status: M1 in progress. M1-ECS-01 landed the ECS foundation — the
32-bit `laige::Entity` handle and the `laige::World` entity storage
(`include/laige/sim/entity.h`, `entity.cpp`; API contract in
[docs/api/entity.md](../docs/api/entity.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `entity`).
M1-ECS-02 landed the component type registry — `ComponentTypeId`, the
`LAIGE_COMPONENT` macro, and `World::registerComponent<T>()`
(`include/laige/sim/component.h`; API contract in
[docs/api/component_registry.md](../docs/api/component_registry.md),
tests under [tests/laige-sim](../tests/laige-sim), CTest entry
`component_registry`). M1-ECS-03 landed the archetype SoA component
storage — archetypes as ordered component sets with per-column SoA
arrays, `World::get<T>`/`addComponent<T>`/`removeComponent<T>`, the
bounded reserve policy, and the 10k-entity zero-alloc churn baseline
(`include/laige/sim/archetype.h`, `archetype.cpp`; API contract in
[docs/api/archetype.md](../docs/api/archetype.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `archetype`).
M1-ECS-04 landed the query API + iteration legality —
`World::each<T1, T2, ...>(fn, Read/Write tags...)` over the archetype
rows with per-component access (superset match), the stack-scoped
iteration-legality guard (assert in debug, `Status` + skip-with-log in
release), and the 10k-entity zero-allocation iteration window
(`include/laige/sim/query.h`, `query.cpp`; API contract in
[docs/api/query.md](../docs/api/query.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `query`).
M1-ECS-05 landed the deterministic iteration contract — the
`World::each` visit order (archetypes in first-seen/creation order,
entities in ascending slot id), the dense-id-order scheme under
component moves, the no-unordered-containers rule for the iteration
path, and the convergent-worlds property test (API contract in
[docs/api/iteration_order.md](../docs/api/iteration_order.md), tests
under [tests/laige-sim](../tests/laige-sim), CTest entry `iter_order`).
M1-ECS-06 landed the ECS guardrails — the G-R3 entity-count
thresholds and the G-R4 per-frame churn budget (`guardrails.cpp`;
CTest entry `ecs_guardrails`). M1-ECS-07 landed the ECS stress +
memory accounting suite (CTest entry `ecs_stress`). M1-SYS-01 landed
the system registry — the plain registered functions
(`LAIGE_SYSTEM` + `SystemDef`), declared time budgets (fpx16_16 ms)
and declared component I/O (`Io<T, Access>`),
`World::registerSystem`/`system`/`systemCount`, and
`SystemContext`'s delegated `each` (`include/laige/sim/system.h`,
`systems.cpp`; API contract in
[docs/api/system_registry.md](../docs/api/system_registry.md), tests
under [tests/laige-sim](../tests/laige-sim), CTest entry
`system_registry`). M1-SYS-02 landed the system scheduler — the
execution order (the registration order plus the declared
`depends_on` edges, the stable topological sort), the pre-run
validation (unknown dependency, dependency cycle, double writer,
read-before-write warn), `SystemSchedule`, and
`World::scheduleSystems`/`runSystems` (`include/laige/sim/system.h`,
`systems.cpp`; API contract in
[docs/api/scheduler.md](../docs/api/scheduler.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `scheduler`).
M1-SYS-03 landed the per-system timing + budget enforcement — the
per-tick rolling time windows (`kSystemTimingWindowSamples`), the
G-R5 budget enforcement (the `system/budget_overrun` warn and
`system/budget_critical` error events,
`kBudgetCriticalMultiplier`), and the
`World::systemTimingStats`/`systemTimingWindow` profiler feed
(`include/laige/sim/system.h`, `system_timing.cpp` + the per-system
measurement in `runSystems` (`systems.cpp`); API contract in
[docs/api/system_timing.md](../docs/api/system_timing.md), tests
under [tests/laige-sim](../tests/laige-sim), CTest entry
`system_timing`). M1-LOOP-01 landed the fixed-timestep game loop
core — the `GameLoop` accumulator loop (integer ticks at a validated
20–120 Hz rate, the exact due computation, the bounded catch-up with
the `loop/tick_dropped` overload warn, the `GameLoopStats` profiler
feed; `include/laige/sim/game_loop.h`, `game_loop.cpp`; API contract
in [docs/api/game_loop.md](../docs/api/game_loop.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `game_loop`).
M1-LOOP-02 landed the per-tick presentation snapshot +
interpolation state — `Position2D` (the first built-in component,
both SimMath backends), `PresentationSnapshot` (the per-completed-
tick `prev`/`curr` capture, the exact-integer anchored alpha clamped
to [0, 1] — never extrapolates — and `sample_position`, new
entities snap to `curr`), the `GameLoop`'s `onTick` hook +
`startReferenceNs` (the M1-HEAD-01 wiring shape), header-only (the
class-template pattern, ADR 0002) (`include/laige/sim/presentation.h`
+ the `game_loop.h`/`game_loop.cpp` hook; API contract in
[docs/api/presentation.md](../docs/api/presentation.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `presentation`).
M1-HEAD-01 landed the headless engine run — `Engine`
(config → world → systems → loop: `EngineConfig` + the provisional
`parseEngineConfig` JSON surface, `run_headless(maxTicks)` the
bounded + server run forms, the ordered idempotent CONC-006
shutdown, the presentation snapshot wiring on the configured SimMath
backend) plus the `laige-run` binary (`--headless`/`--ticks`/
`--replay` stub) and the `laige_run_smoke` CI entry
(`include/laige/sim/engine.h`, `engine.cpp`, `tools/run`; API
contract in [docs/api/engine.md](../docs/api/engine.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `engine`).
M1-DET-01 landed deterministic mode — the SimMath-only sim
guarantee (the G-R8 compile-time trait
`include/laige/sim/determinism.h`: `SimMathBackend`,
`DeterminismConfig`, `detail::IsDeterminismSafe<T>`,
`LAIGE_DETERMINISM_SAFE`, enforced by a `static_assert` in
`World::registerSystem`), the per-system PRNG substreams
(`World::Options.seed`/`deterministic`, `SystemContext.rng`), the
`seed`/`determinism` keys on the provisional config surface + the
backend selection at engine init (built-in component + snapshot), and
the sim-source determinism scan (`tools/laige-determinism-lint`, the
`determinism-lint` CI job) with same-line
`LAIGE-DETERM-EXCEPTION` markers as the documented false-positive
policy; the scope statement is
[docs/concepts/determinism.md](../docs/concepts/determinism.md), the
trait API in
[docs/api/determinism.md](../docs/api/determinism.md); tests under
[tests/laige-sim](../tests/laige-sim) (CTest entries
`determinism_mode` + `trait_compile_*`; lint fixtures in
[tests/tools](../tests/tools)).
M1-DET-02 landed replay recording — the versioned replay log
format (header: format version + the ADR 0002 replay identity —
seed, tick rate, component schema hash, math backend id, config
hash — + per-tick frames as opaque byte blobs + the trailer's
FNV-1a fileHash; SCALE-005), the `ReplayRecorder` (opt-in, atomic
temp+rename, size-bounded), the `parseReplay`/`loadReplay` readers,
the `componentSchemaHash`/`configHash`/`makeReplayIdentity`
identity hashes (`include/laige/sim/replay.h`, `replay.cpp`), the
engine wiring (`Engine::startReplayRecording`: one zero-length
frame per completed tick, a mid-run failure stops the run, the
ordered shutdown abandons an unfinished recording) and the
`laige-run --replay` flag (debug builds only; it was the
M1-HEAD-01 stub); replay execution (`world.state_hash`, the
`laige-replay` runner) is M1-DET-03; API contract in
[docs/api/replay.md](../docs/api/replay.md), tests under
[tests/laige-sim](../tests/laige-sim) (CTest entries
`replay_record` + `fuzz_replay_parse`).
The profiler, PRNG state introspection
(M1-DET-03), the detcheck matrix (M1-DET-04), and the remaining M1
steps land next; physics, input, and animation in M3.
