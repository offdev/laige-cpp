# Laige documentation

Documentation index and navigation (DOC-001). The engine is at **M1**
(heartbeat): `laige-core` holds the M0 foundations, and `laige-sim`
has started (M1-ECS-01: the entity handle and world entity storage;
M1-ECS-02: the component type registry; M1-ECS-03: archetype SoA
component storage; M1-ECS-04: the query API + iteration legality;
M1-ECS-05: the deterministic iteration contract; M1-ECS-06: the
ECS guardrails G-R3/G-R4; M1-ECS-07: the ECS stress + memory
accounting suite; M1-SYS-01: the system registry; M1-SYS-02: the
system scheduler; M1-SYS-03: the per-system timing + budget
enforcement; M1-LOOP-01: the fixed-timestep game loop core;
M1-LOOP-02: the per-tick presentation snapshot + interpolation
state; M1-HEAD-01: the headless engine run — `Engine`
(config → world → systems → loop) and the `laige-run` binary;
M1-CFG-01: the declarative game config — the version 1
`config.json` schema (tick rate, budgets, camera defaults, asset
roots, determinism block), `loadGameConfig`, the programmatic
override merge, and the debug-only hot reload of non-simulation
keys (`laige/sim/config.h`);
M1-DET-01: deterministic mode — the SimMath-only sim guarantee
(the G-R8 compile-time trait + the CI source scan), the per-system
PRNG substreams, and the `seed`/`determinism` config keys;
M1-DET-02: replay recording — the versioned replay log format
(replay identity per ADR 0002), the `ReplayRecorder` (atomic
temp+rename, size-bounded), `Engine::startReplayRecording`, and
`laige-run --replay`; M1-DET-03: replay execution —
`World::stateHash` (the deterministic state hash), `runReplay`
(the identity-checked re-run and its per-tick hash stream), and the
`laige-replay` runner; M1-SAMPLE-01: `hello.laige` — the headless
template game (samples/hello: one component, one system, one entity,
the per-tick `World::stateHash` stream on stdout, record → replay
through the scenario's own binary, the PRD §9.4 line budget —
[samples/hello/README.md](../../samples/hello/README.md));
M1-DET-04: bit-exactness CI — the hello scenario's committed per-tick
hash baselines (both SimMath backends), the `hello --expect` baseline
check in every P0 OS job's ctest, the merge `detcheck` job's
two-configuration pairs (g++ vs clang++, Debug+ASan vs Release), and
the [determinism report](benchmarks/determinism-matrix.md)).
Every section of the AGENTS §13 `docs/` tree exists; each entry below
links what is written and the "not yet written" section marks what is
still to land.

## Getting started

- [Building Laige](getting-started/building.md) — the source of truth
  for the canonical build commands, build trees, options, compiler
  policy (NFR-8.10), sanitizer builds (NFR-8.2), and the current M0
  status. Tool commands: `laige-run`, `laige-replay`, `laige-fuzz`, `laige-bench`,
  `laige-detcheck`, the `laige-api` manifest target, and the
  include-graph lint.
- [hello.laige](../../samples/hello/README.md) — the headless template
  game (M1-SAMPLE-01): the canonical smallest-complete Laige game —
  build it with the repository, run it headless, record and replay it,
  and learn every component/system/tick mark a Laige game uses.

## Concepts

- [Concepts index](concepts/README.md) — architecture, coordinates
  (ARCH-008), lifecycle, threading, and determinism scope.
  [Determinism](concepts/determinism.md) is written (M1-DET-01: the
  same-build scope, the two-layer G-R8 enforcement, the exception
  policy, the PRNG substreams); the other topics name their planned
  document and interim home.

## API contracts (per public header)

- [Entity handle and world entity storage](api/entity.md) —
  `laige::Entity` (32-bit id+generation handle) and `laige::World`
  entity storage, including the M1-ECS-06 guardrails: the G-R3
  entity-count thresholds (`beginFrame()`, `guardrailStats()`,
  `ecs/entity_budget_{25,50,100}`) and the G-R4 per-frame churn
  budget (`ecs/churn_per_frame`) (M1-ECS-01/06; `laige-sim`).
- [Component type registry](api/component_registry.md) —
  `ComponentTypeId`, `LAIGE_COMPONENT`, `World::registerComponent<T>`
  (M1-ECS-02; `laige-sim`).
- [Archetype SoA component storage](api/archetype.md) — archetypes as
  ordered component sets with per-column SoA storage,
  `World::get<T>`/`addComponent<T>`/`removeComponent<T>`, the reserve
  policy, and the 10k-entity churn baseline (M1-ECS-03; `laige-sim`).
- [Query API + iteration legality](api/query.md) —
  `World::each<T1, T2, ...>(fn, Read/Write tags...)` over the archetype
  rows: superset match, per-component access, the stack-scoped
  iteration-legality guard, and the 10k-entity zero-allocation window
  (M1-ECS-04; `laige-sim`).
- [Deterministic iteration order](api/iteration_order.md) — the
  `World::each` visit contract: archetypes in first-seen (creation)
  order, entities in ascending slot id, the dense-id-order scheme
  under moves, no unordered containers in the iteration path, and the
  convergence property test (M1-ECS-05; `laige-sim`).
- [System registry](api/system_registry.md) — plain registered
  functions (`LAIGE_SYSTEM` + `SystemDef`) with declared time budgets
  (fpx16_16 ms) and declared component I/O (`Io<T, Access>`),
  `World::registerSystem`/`system`/`systemCount`, and
  `SystemContext`'s delegated `each` (M1-SYS-01; `laige-sim`).
- [System scheduler](api/scheduler.md) —
  `World::scheduleSystems`/`runSystems`: the execution order (the
  registration order plus the declared `depends_on` edges), the
  pre-run validation (unknown dependency, cycle, double writer,
  read-before-write warn), and the per-tick system phase
  (M1-SYS-02; `laige-sim`).
- [Per-system timing and budget enforcement](api/system_timing.md) —
  the per-tick rolling time windows, the G-R5 budget enforcement
  (`system/budget_overrun` warn, `system/budget_critical` error), and
  the `World::systemTimingStats`/`systemTimingWindow` profiler feed
  (M1-SYS-03; `laige-sim`).
- [Fixed-timestep game loop core](api/game_loop.md) — the `GameLoop`
  accumulator loop: integer ticks at a validated 20–120 Hz rate
  (default 60), the exact due computation, the bounded catch-up with
  the `loop/tick_dropped` overload warn (drop, never silent), and the
  `GameLoopStats` profiler feed (M1-LOOP-01; `laige-sim`).
- [Presentation snapshot and interpolation state](api/presentation.md)
  — `Position2D` (the first built-in component) and
  `PresentationSnapshot`: the per-tick `prev`/`curr` capture, the
  exact-integer anchored alpha (clamped to [0, 1], never
  extrapolates), and `sample_position` (M1-LOOP-02; `laige-sim`).
- [The always-on profiler](api/profiler.md) — the FR-11.1
  always-on counters (tick/frame time windows, entity counts, sim
  alloc count, draw calls / texture binds / net bytes), the cold
  snapshot + text/JSON reports, the `GameLoop` per-tick timing hook,
  the engine's per-run report (`laige-run --prof-out`), and the
  measured ≤1% enabled cost (M1-PROF-01; `laige-sim`).
- [The frame graph / budget report](api/frame_budget.md) — the
  FR-11.2 per-frame budget report: every declared budget (system
  time, total tick time, allocation count) measured vs declared with
  a pass/flag, the over-budget systems list, the fixed
  `FrameBudgetRecorder` ring, the engine's opt-in cached per-run
  report (`laige-run --budget-report`, `--fail-on-budget`), and the
  zero-allocation record path (M1-PROF-02; `laige-sim`).
 - [Headless engine run](api/engine.md) — `laige::Engine`
  (config → world → systems → loop): `run_headless(maxTicks)` the
  bounded + server run forms, the ordered idempotent CONC-006
  shutdown, the backend selection at init, and the `laige-run`
  CLI (M1-HEAD-01; `laige-sim` + `tools/run`).
- [Declarative game config](api/config.md) — the version 1
  `config.json` schema (the required `version` key, tick rate,
  budgets, camera defaults, asset roots, the determinism block),
  `EngineConfig`, `loadGameConfig`, the `EngineConfigOverride`
  merge, and the debug-only `ConfigHotReloader` (M1-CFG-01;
  `laige-sim`).
- [Determinism-safe storage](api/determinism.md) — the G-R8
  compile-time trait: `SimMathBackend`, `DeterminismConfig`,
  `detail::IsDeterminismSafe<T>`, and
  `LAIGE_DETERMINISM_SAFE(Type, MemberTypes...)` (M1-DET-01;
  `laige-sim`).
- [Replay recording and execution](api/replay.md) — the versioned
  replay log format (replay identity: seed, tick rate, component
  schema hash, math backend id, config hash — ADR 0002), the
  `ReplayRecorder` (atomic temp+rename, size-bounded), the
  `parseReplay`/`loadReplay` readers, the identity hashes, the
  engine/CLI wiring (M1-DET-02), and the execution half:
  `World::stateHash`, `replayIdentityDiff`/`runReplay`, and the
  `laige-replay` runner (M1-DET-03; `laige-sim`).
- [Result / Status / error codes](api/errors.md) — `laige::Result<T,E>`,
  `laige::Status`, the stable `ErrorCode` registry (M0-CORE-01).
- [Structured logging](api/logging.md) — the `laige::log` facade, sinks,
  rate limiting, crash handling (M0-CORE-02; AGENTS §14).
- [SimMath deterministic math](api/sim_math.md) — the op surface plus the
  `fp32_pinned` and `fpx16_16` backends, NaN/Inf policy, pinned-math
  flags (M0-CORE-03/04; ADR 0002).
- [Memory pools](api/pools.md) — `ArenaPool<T>` and `Pool<T>` with
  generation-checked handles and `PoolStats` accounting (M0-CORE-05).
- [Allocation watch](api/alloc_watch.md) — the process-wide heap
  allocation counter behind the G-R1 zero-sim-loop-allocation
  guardrail: the armed-window model, the per-tick debug assertion
  (game_loop.md), the release fallback, and the sanitizer scope
  (M1-ALLOC-01).
- [Bounded JSON](api/json.md) — `laige::JsonValue`, `parseJson`,
  `serializeJson`, `JsonOptions` bounds (M0-CORE-07; ADR 0003).
- [Budget harness](api/budget_harness.md) — `Histogram`, `TimeIt`,
  `budgetCheck`, the AGENTS §12 report format, and the `budgets.json`
  schema (M0-CORE-08).
- [PRNG](api/prng.md) — `laige::Prng`: xorshift128+ with splitmix64
  seeding, substreams, period, and the determinism contract
  (M0-CORE-06).
- [Determinism checker](api/detcheck.md) — the `laige-detcheck` tool and
  the scenario hash-line contract (`<tick> <hash>` lines, two build
  configurations) (M0-TOOL-02; activated on the hello scenario by
  M1-DET-04 — the baseline comparison and the CI matrix).

## Guides

- [Guides index](guides/README.md) — task-oriented usage and
  optimization guides. **None yet**: they land with their milestones
  (first headless project and determinism/replay in M1, profiling in
  M1, rendering in M2, MMO server setup in M6/M7).

## Debugging

- [Debugging index](debugging/README.md) — what is usable today
  (structured logging, the budget harness, `laige-detcheck`, sanitizer
  builds, test seeds). The in-engine debug mode (AGENTS §15) lands with
  the profiling work (M1-PROF-01, M2-PROF-01).

## Benchmarks

- [Benchmarks index](benchmarks/README.md) — methodology, baselines,
  results, and the regression policy.
- [Benchmark methodology](benchmarks/methodology.md) — the AGENTS §12
  report fields, the `budgets.json` field mapping, the baseline-file
  convention, and the PRD §8.1 regression policy (M0-DOC-01).
- [Baselines](benchmarks/baselines/README.md) — recorded baseline
  reports (`m0-synthetic.md`, M0-EXIT-01, and `m1-ecs-stress.md`,
  M1-ECS-07; every `budgets.json` entry still has `measured: 0`).
- [Determinism matrix](benchmarks/determinism-matrix.md) — the
  M1-DET-04 determinism report: the committed per-tick hash baselines
  of the hello scenario (both SimMath backends), the CI matrix (every
  P0 OS job's baseline check + the merge detcheck job's cross-compiler
  / sanitizer / Release pairs), and the ARCH-010 scope statement.

## Architecture decisions (ADRs)

- [ADR index](decisions/README.md) — 0001 (name and license), 0002
  (deterministic math), 0003 (config JSON), 0004 (GoogleTest
  vendoring), 0005 (iso default: 2:1 dimetric).

## Compatibility

- [Compatibility](compatibility/README.md) — P0 platforms and
  compilers (PRD §6; the CI matrix), the current machine-readable
  formats (`budgets.json`, `laige-api.json`, `deps.lock`, and the
  version 1 replay log format, M1-DET-02), and the migration-guide
  status (none yet — pre-1.0, no breaking changes).

## Testing

- [Testing conventions](testing.md) — test layout (module dirs mirror
  `src/`, `<module>_tests` executables), the `regress_<short-id>`
  regression-test convention, `laige-fuzz` target registration and CI
  lane semantics, and the seed-handling convention for randomized tests
  (M0-TEST-01).

## Not yet written (honest status)

- `concepts/` — the architecture, coordinates, lifecycle, and
  threading concept documents (the [index](concepts/README.md) names
  each and its interim home). [Determinism](concepts/determinism.md)
  is written (M1-DET-01).
- `guides/` — task-oriented usage (first game, profiling, determinism)
  — see the [index](guides/README.md).
- `debugging/` — the in-engine debug mode (AGENTS §15; profiling
  foundations land in M1, render observability in M2).
- `benchmarks/baselines/` — no measured baselines yet; the first lands
  with M0-EXIT-01.
- `compatibility/` — no **migration guides** yet (they land with the
  first breaking public-API change); the version 1 replay log format
  has landed with M1-DET-02 (see [compatibility/README.md](compatibility/README.md)).
- Per-module API docs for the remaining M1+ modules (`laige-render`,
  `laige-assets`, `laige-net`, `laige-server`, `laige-script`,
  `laige-editor`) — they land with their modules. (`laige-sim` has
  one per shipped piece:
  [entity.md](api/entity.md),
  [component_registry.md](api/component_registry.md),
  [archetype.md](api/archetype.md),
  [query.md](api/query.md),
  [iteration_order.md](api/iteration_order.md),
  [system_registry.md](api/system_registry.md),
  [scheduler.md](api/scheduler.md),
  [system_timing.md](api/system_timing.md),
  [game_loop.md](api/game_loop.md),
  [presentation.md](api/presentation.md),
  [profiler.md](api/profiler.md),
  [engine.md](api/engine.md),
  [config.md](api/config.md),
  [determinism.md](api/determinism.md),
  [replay.md](api/replay.md).)

## Related

- [Roadmap index](../roadmap/README.md) — the M0/M1/… step plan,
  progress board, and change log;
  [M0 foundations](../roadmap/M0-foundations.md) is the current
  milestone.
- `AGENTS.md` — the engineering contract this documentation implements.
- `PRD.md` — the product requirements.
