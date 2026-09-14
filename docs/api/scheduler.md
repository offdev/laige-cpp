# System scheduler (`World::scheduleSystems`, `World::runSystems`)

The M1 system framework's scheduler (M1-SYS-02; PRD §9.1 S-8, FR-1.3,
PRD §10.2, AGENTS API-004, PERF-003): turns the system registry (the
registration order, the declared `depends_on` edges, and the declared
component I/O) into the per-tick execution order, validates it
once, and runs the systems in that order. Public header:
`src/laige-sim/include/laige/sim/system.h` (`SystemSchedule`, the
`depends_on` spec, the full contract) plus the `World::
scheduleSystems`/`runSystems` members in
`src/laige-sim/include/laige/sim/entity.h`; implementation:
`src/laige-sim/systems.cpp`. Unit suite: `ctest -R scheduler`
(`tests/laige-sim/scheduler_tests.cpp`).

The game's setup path registers systems once, schedules once, and then
the loop runs the schedule every tick (M1-LOOP-01 owns the loop):

```cpp
// World setup (before the loop):
for (the systems) { world.registerSystem(def, Io<...>...); }
SystemSchedule schedule;
Status s = world.scheduleSystems(schedule);   // check it (no exceptions)

for (tick) {
  world.beginFrame();
  world.runSystems(schedule);
}
```

## Execution order

- **Base order** — the registration order (ascending `SystemId`).
  Without `depends_on`, the computed order is exactly the registration
  order.
- **`depends_on` edges** — a system lists the registration names of
  the systems it must run after. Forward edges are legal (the
  dependency may be registered later: the spec is resolved against the
  world at scheduling time, not at registration).
- **The stable topological sort** — the order is computed with Kahn's
  algorithm with a min-id tie-break: repeatedly place the smallest
  unrun `SystemId` whose dependencies are all placed. A system only
  moves LATER, behind its dependencies — it never moves earlier than
  registration order would place it. The order is a pure function of
  the registration order and the declared edges (ARCH-010), so two
  runs (two builds, two processes) produce bit-identical schedules.
- **A barrier is registration position, not a dependency list** — to
  run one system after all others, register it last. The direct
  `depends_on` list is a small hand-written declaration, bounded at
  `kMaxSystemDependencies` (16).

## The depends_on spec

The spec is the def's `dependsOn` string (`SystemDef::dependsOn`,
`const char*`). The `LAIGE_SYSTEM(Name, budget_ms, Dep..., ...)` macro
builds it by stringizing the trailing names verbatim:

```cpp
LAIGE_SYSTEM(Health, 1, Spawner)   // spec: "Spawner"
LAIGE_SYSTEM(Damage, 1, Spawner, Movement)  // spec: "Spawner, Movement"
LAIGE_SYSTEM(CleanUp, 1)           // spec: "" (no dependencies)
```

Manual `SystemDef` construction takes the raw string (or nullptr):

```cpp
laige::SystemDef def{"Damage", &Damage, laige::fpx16_16::fromInt32(1),
                    "Spawner, Movement"};
```

Parse rules (checked at registration, resolved at scheduling):

- comma-separated registration names; each token is trimmed of ASCII
  whitespace (space, tab, CR, LF);
- no empty tokens (a trailing or doubled comma is a typo → an error);
- no duplicate names (the dependency list is a set, not a multiset —
  the declared-I/O precedent, [system_registry.md](system_registry.md));
- at most `kMaxSystemDependencies` (16) direct dependencies;
- `nullptr` or `""` means no dependencies.

## Pre-run validation (`World::scheduleSystems`)

`scheduleSystems` is a setup-phase operation (after all
registrations, before the loop); a pure read of the registry (the
method is `const`). Validation order — **first failure wins**, every
failure is one rate-limited structured warn (subsystem `system`,
LOG-004) plus a `Status` (FR-12.3: never silent):

| # | Condition | Event | Result |
|---|-----------|-------|--------|
| 1 | A dependency name that is not registered in this world (first in ascending (system id, spec position) order) | `system/dep_missing` | `InvalidArgument` |
| 2 | A dependency cycle: Kahn's leaves systems with unsatisfied dependencies (one concrete cycle is reported — the deterministic walk from the smallest remaining id, following each system's first spec-listed dependency that is still remaining) | `system/dependency_cycle` | `InvalidArgument` |
| 3 | Two systems both declaring `Write` of the same component type in one tick (first conflict in ascending component-id, then ascending writer-id order; order-independent: the last write would silently win) | `system/double_writer` | `InvalidArgument` |
| 4 | (WARN ONLY — scheduling succeeds) a declared read that the computed order places BEFORE a declared write of the same component: the reader observes the previous tick's value, not this tick's write (each (reader, writer, component) triple warns once, in ascending component-id, reader-id, writer-id order) | `system/read_before_write` | ok + Warn |

On success `out` is fully populated and **nothing is logged**
(LOG-003: the success path has no diagnostics).

The event fields (LOG-001 stable keys, LOG-002 actionable):

| Event | Fields |
|-------|--------|
| `system/dep_missing` | `system` (the system's name), `missing_dep` (the unregistered name, bounded to 64 chars), `position` (the token index in the spec) |
| `system/dependency_cycle` | `cycle` (the systems of one concrete cycle, comma-joined in walk order, bounded to 256 chars) |
| `system/double_writer` | `component_id`, `first_writer`, `second_writer` (registration names) |
| `system/read_before_write` | `reader`, `writer`, `component_id` |

## Running (`World::runSystems`)

One sim tick's system phase. The systems run **strictly one at a
time**, in schedule order, on the world's single owner thread (PRD
§10.2: simulation is single-threaded; API-004: the system phase is the
mutation phase). Each system gets a fresh `SystemContext` (a
non-owning view — never stored across ticks or systems).

- **Stale schedule** — `schedule.systemCount !=` the world's current
  `systemCount` (systems registered after the schedule was computed,
  or a schedule from another world) → `InvalidArgument` + one
  rate-limited warn `system/schedule_stale` (fields `scheduled_systems`,
  `current_systems`). Recompute the schedule after any registration
  change.
- **Malformed schedule** — an order entry that is 0, above
  `systemCount`, or a duplicate id (a hand-built schedule) →
  `InvalidArgument` + one rate-limited warn `system/schedule_invalid`
  (fields `slot`, `id`).
- **Empty schedule** — `systemCount == 0` (the empty world, or a
  moved-from world) → ok, runs nothing.
- **Iteration legality** — the M1-ECS-04 guard
  ([query.md](query.md)) applies inside every system exactly as for a
  direct `World::each`: a nested `each()` or an illegal mutation is
  rejected per system. The declared-I/O validation above is the
  cross-system complement: one writer per component (rejected at
  scheduling time), and read-before-write surfaced as a warn.
- A system's run function is `void`: per-entity `Status` results from
  its own `each()` calls are the system's to handle (check them,
  CORE-008). `runSystems` itself reports only schedule-level failures.
- **Calling `runSystems` from inside a system is misuse**: nesting
  system phases breaks the declared order contract (the
  one-writer-per-component invariant still prevents state
  corruption).

## Determinism (ARCH-010)

Scheduling is pure integer/string bookkeeping: id scans, string
comparisons over the registration names, and Kahn's with a min-id
rule. No floating point, no randomness, and no addresses enter the
order or the warning set. Two worlds that reach the same registry
(same registration order, specs, and declared I/O) produce
bit-identical schedules and identical warning sequences — the
`scheduler-order … fnv1a=0x…` known-answer line in the `scheduler`
CTest entry pins the property.

## Performance

- **`World::scheduleSystems`:** O(n·d·n + c·n²) in the system count
  `n` (≤ `kMaxSystems` = 256), direct dependencies `d` (≤
  `kMaxSystemDependencies` = 16), and component count `c` (≤
  `kMaxComponentTypes` = 256) — bounded, setup path only, called once
  per world setup. All state is fixed-size stack/world arrays; **no
  allocation** (PERF-003), no logging on the success path (LOG-003).
- **`World::runSystems`:** O(n) dispatch (a staleness check, an O(n)
  id check, and one `SystemContext` construction + one call per
  system) plus the systems' own work. **No allocation, no logging**
  on the success path — the per-tick cost is the systems' declared
  budgets (M1-SYS-03 measures them; the per-system timing + budget
  enforcement is documented in
  [system_timing.md](system_timing.md)).
- **Misuse:** scheduling a world near the `kMaxSystems` bound costs
  O(n³) worst case (~1M bounded integer ops for n = 256) — a setup
  cost, never a hot path; a game that outgrows 256 systems raises the
  constant through an ADR.

## Threading and failure (CONC-001, API-004)

`scheduleSystems` is a pure read (setup phase, owner thread);
`runSystems` is the per-tick mutation phase on the world's single
owner thread (PRD §10.2: simulation is single-threaded). All failures
are `Result`/`Status` values with one rate-limited structured warn
each (LOG-004); no exceptions (FR-12.1, NFR-8.10).

## Misuse warnings

- `depends_on` names registration names, not function addresses or
  `SystemId`s (ids are per-world runtime values; names are the stable
  identity). A dependency on a name that is never registered in this
  world fails at scheduling time — a dependency on a system registered
  in ANOTHER world is always such a failure (systems never cross
  worlds).
- A schedule is computed for the registry it was computed with:
  registering systems after `scheduleSystems()` and then `runSystems()`
  with the old schedule is rejected (`system/schedule_stale`).
  Recompute the schedule after any registration change.
- A system that reads a component written by a LATER system in the
  computed order reads the previous tick's value: the scheduler warns
  (`system/read_before_write`). If the read must see this tick's
  write, declare `depends_on` (or register the writer earlier); a
  deliberate cross-tick read is declared by not declaring the write
  at all (undeclared I/O is invisible to the check — use it
  consciously).
- A system that writes a component it did not declare is invisible to
  the double-writer check ([system_registry.md](system_registry.md)):
  declare the I/O the system really uses.
