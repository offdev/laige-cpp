# Laige documentation

Documentation index and navigation (DOC-001). The engine is at **M1**
(heartbeat): `laige-core` holds the M0 foundations, and `laige-sim`
has started (M1-ECS-01: the entity handle and world entity storage;
M1-ECS-02: the component type registry; M1-ECS-03: archetype SoA
component storage; M1-ECS-04: the query API + iteration legality;
M1-ECS-05: the deterministic iteration contract; M1-ECS-06: the
ECS guardrails G-R3/G-R4; M1-ECS-07: the ECS stress + memory
accounting suite; M1-SYS-01: the system registry).
Every section of the AGENTS §13 `docs/` tree exists; each entry below
links what is written and the "not yet written" section marks what is
still to land.

## Getting started

- [Building Laige](getting-started/building.md) — the source of truth
  for the canonical build commands, build trees, options, compiler
  policy (NFR-8.10), sanitizer builds (NFR-8.2), and the current M0
  status. Tool commands: `laige-fuzz`, `laige-bench`, `laige-detcheck`,
  the `laige-api` manifest target, and the include-graph lint.

## Concepts

- [Concepts index](concepts/README.md) — architecture, coordinates
  (ARCH-008), lifecycle, threading, and determinism scope. **Not yet
  written** (M0 is foundations only); the index names each planned
  document and its interim home today (the `Vec2`/`Vec3` comments in
  `src/laige-core/include/laige/sim_math.h`, ADR 0002, the per-API
  contracts).

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
- [Result / Status / error codes](api/errors.md) — `laige::Result<T,E>`,
  `laige::Status`, the stable `ErrorCode` registry (M0-CORE-01).
- [Structured logging](api/logging.md) — the `laige::log` facade, sinks,
  rate limiting, crash handling (M0-CORE-02; AGENTS §14).
- [SimMath deterministic math](api/sim_math.md) — the op surface plus the
  `fp32_pinned` and `fpx16_16` backends, NaN/Inf policy, pinned-math
  flags (M0-CORE-03/04; ADR 0002).
- [Memory pools](api/pools.md) — `ArenaPool<T>` and `Pool<T>` with
  generation-checked handles and `PoolStats` accounting (M0-CORE-05).
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
  configurations) (M0-TOOL-02).

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
  reports. **Empty so far** (all `budgets.json` entries have
  `measured: 0`); the first, `m0-synthetic.md`, lands with M0-EXIT-01.

## Architecture decisions (ADRs)

- [ADR index](decisions/README.md) — 0001 (name and license), 0002
  (deterministic math), 0003 (config JSON), 0004 (GoogleTest
  vendoring).

## Compatibility

- [Compatibility](compatibility/README.md) — P0 platforms and
  compilers (PRD §6; the CI matrix), the current machine-readable
  formats (`budgets.json`, `laige-api.json`, `deps.lock`), and the
  migration-guide status (none yet — pre-1.0, no breaking changes).

## Testing

- [Testing conventions](testing.md) — test layout (module dirs mirror
  `src/`, `<module>_tests` executables), the `regress_<short-id>`
  regression-test convention, `laige-fuzz` target registration and CI
  lane semantics, and the seed-handling convention for randomized tests
  (M0-TEST-01).

## Not yet written (honest status)

- `concepts/` — the architecture, coordinates, lifecycle, threading, and
  determinism concept documents (the [index](concepts/README.md) names
  each and its interim home).
- `guides/` — task-oriented usage (first game, profiling, determinism)
  — see the [index](guides/README.md).
- `debugging/` — the in-engine debug mode (AGENTS §15; profiling
  foundations land in M1, render observability in M2).
- `benchmarks/baselines/` — no measured baselines yet; the first lands
  with M0-EXIT-01.
- `compatibility/` — no persistent data formats or migration guides yet
  (they land with M1 replay and M6/M7 networking).
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
  [system_timing.md](api/system_timing.md).)

## Related

- [Roadmap index](../roadmap/README.md) — the M0/M1/… step plan,
  progress board, and change log;
  [M0 foundations](../roadmap/M0-foundations.md) is the current
  milestone.
- `AGENTS.md` — the engineering contract this documentation implements.
- `PRD.md` — the product requirements.
