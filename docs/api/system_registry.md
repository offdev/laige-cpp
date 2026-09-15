# System registry (`World::registerSystem`, `LAIGE_SYSTEM`)

The M1 system framework (M1-SYS-01; PRD §9.1 S-8, FR-1.3, AGENTS
API-006, PERF-003): plain, registered functions with declared time
budgets and declared component I/O. Public header:
`src/laige-sim/include/laige/sim/system.h` (`SystemId`, `SystemDef`,
`SystemFn`, `SystemContext`, `Io<T, Access>`, `SystemInfo`, the
`SystemSchedule` and `kMaxSystemDependencies` of the M1-SYS-02
scheduler, the `LAIGE_SYSTEM` macro, the `detail::SystemRecord`/trait
types, the full contract) plus the `World::registerSystem`/`system`/
`systemCount` members in
`src/laige-sim/include/laige/sim/entity.h`; implementation:
`src/laige-sim/systems.cpp` (the non-template World methods and the
`SystemInfo` queries) + the header-defined `registerSystem` template
(entity.h). Unit suite: `ctest -R system_registry`
(`tests/laige-sim/system_registry_tests.cpp`). The scheduler built on
top of the registry is documented in [scheduler.md](scheduler.md).

A game's setup path registers components and systems once, in one
documented place:

```cpp
// The plain system function (FR-1.3: no class, no inheritance). The
// LAIGE_SYSTEM macro declares it and builds its def.
LAIGE_SYSTEM(Movement, 1)
void Movement(laige::World& world, laige::SystemContext& ctx) {
  world.each<SimVel>(
      [](laige::Entity e, SimVel& v) { /* integrate */ }, laige::Write{});
}

// World setup (before the loop):
auto r = world.registerSystem(Movement_Def,
    laige::Io<SimVel, laige::Access::Write>{},
    laige::Io<SimPos, laige::Access::Read>{});
// r is a Result<SystemId, ErrorCode>; check it (no exceptions).
```

## The system shape (FR-1.3)

A system is a **plain free function** with the signature
`SystemFn = void (*)(World&, SystemContext&)` — no class, no
inheritance, no state object. The function plus its `SystemDef`
(the `Movement_Def` variable the macro builds) IS the system:

- `world` — the world the system runs on (one world, one owner
  thread; PRD §10.2: simulation is single-threaded).
- `ctx` — that tick's `SystemContext`: a non-owning view of the same
  world whose `each<T1, T2, ...>(fn, Read/Write tags...)` delegates
  to `World::each` (the PRD Appendix B sketch's `ctx.each<...>()`;
  identical semantics, visit order, and iteration-legality behavior —
  [query.md](query.md)). The context is built per system per tick by
  the scheduler (M1-SYS-02, [scheduler.md](scheduler.md)); never
  store it across ticks.
- `ctx.rng` — the system's own PRNG substream (M1-DET-01): a
  `laige::Prng*` derived from the world's seed and this system's
  registration id (`Prng::deriveSubstream(seed, id)`; id 0 is the
  master, never assigned to a system). Non-null when the world was
  created in deterministic mode (the default), null when it was not
  (the documented escape hatch). The stream is advanced in place as
  the system draws — its state is part of the replay state, so a
  draw belongs at a fixed position in the system's run (e.g. before
  its iteration). Drawing is optional: a system that never touches
  `ctx.rng` costs nothing. See
  [concepts/determinism.md](../concepts/determinism.md) for the
  substream contract and [api/prng.md](prng.md) for the taps.
- Systems are deterministic when the engine runs in deterministic
  mode (M1-DET-01) and must stay within their declared budget
  (M1-SYS-03 measures per-system time and enforces the budget —
  [system_timing.md](system_timing.md)).

`LAIGE_SYSTEM(Name, budget_ms, Dep..., ...)` (namespace scope,
directly above the function) expands to the function declaration plus

```cpp
inline const laige::SystemDef Name##_Def = laige::SystemDef{
    #Name, &Name, laige::fpx16_16::fromFloat(budget_ms), #__VA_ARGS__};
```

so `Name` is both the C++ function name and the system's
registration name (stringified), and the def variable is `Name##Def`.
`budget_ms` is a numeric literal in milliseconds (1, 0.5, …); the
conversion to the exact `fpx16_16` happens once, at program start
(setup path, never a hot path). The budget is enforced per tick by
the per-system timing (M1-SYS-03 —
[system_timing.md](system_timing.md)). The macro and the function
definition live in the same translation unit. The optional trailing
`Dep...` names are the **depends_on** spec (M1-SYS-02): the
registration names of the systems `Name` must run after, stringified
verbatim into the def's `dependsOn` field — see
[scheduler.md](scheduler.md) for the format and the ordering
semantics.

## SystemIds and registration (component.h id contract)

`World::registerSystem(def, Io<...>...)` is a setup-phase operation
(before the loop), like `registerComponent<T>`:

- **Ids** — `SystemId`s are assigned in registration order, densely
  from 1, per world (0 is reserved and never assigned).
  `World::systemCount()` is the high water mark;
  `World::system(id)` returns the `SystemInfo` snapshot for a valid
  id, `ErrorCode::InvalidArgument` otherwise (a pure query, like
  `componentInfo`).
- **Determinism (ARCH-010)** — id assignment and the name-uniqueness
  check are pure integer/string bookkeeping: no addresses, hashes,
  or platform state enter the id. Two worlds, two process runs, or
  two builds that register the same systems in the same order
  produce bit-identical id sequences, so SystemIds are replay state
  from M1 on (M1-DET-01/02). Ids are per-world: never compare them
  across worlds (the `Entity`/`ComponentTypeId` cross-world caveat).
- **Engine budget** — `kMaxSystems` (256) is the engine-level cap on
  systems per world (CORE-005, the `kMaxComponentTypes` precedent).
  Registering a 257th system is `BudgetExhausted`, never silent.
- **Lifetime** — the def is **copied by value** into the world's
  fixed record table at registration, so a def on the stack is safe;
  the record table is the world's setup-path allocation (like the
  component registry). The registry travels with the world on move
  and survives `clear()` (a system is not per-entity data).

## Declared component I/O (FR-1.3)

The declared I/O is part of the **registration**, not the def:
component ids are per-world runtime values (component.h) and cannot
be baked into a compile-time def. One `Io<T, Access>{}` tag value per
declared component:

- `T` must be a Laige component (`LAIGE_COMPONENT`) **registered in
  this world**; a non-component `T` is a compile error
  (static_assert in `registerSystem`).
- A component appears **at most once** per system, in any access
  combination (`Read+Write` of the same component is ambiguous —
  rejected). The declaration is a set, not a multiset.
- The I/O is resolved at registration into the disjoint read/write
  id sets of the world's record and read back through
  `SystemInfo::declaresRead`/`declaresWrite` (O(1), no allocation).
  The documented I/O list order is ascending `ComponentTypeId` (a
  pure function of the sets).
- M1-SYS-02 consumes these sets: two systems writing the same
  component type in one tick is rejected at scheduling time
  (FR-12.3), and a declared read ordered before a declared write of
  the same component is warned ([scheduler.md](scheduler.md)).

## Registration validation (FR-12.1, CORE-008)

The order is normative — the first failure wins. Every failure is one
rate-limited structured warn (subsystem `system`, LOG-004) plus a
`Status`; no exceptions (NFR-8.10).

| Condition | Result | Event |
|---|---|---|
| moved-from world (no registry) | `InvalidArgument` | — (pure failure) |
| `def.name` null or empty | `InvalidArgument` + warn | `system/name_invalid` |
| `def.run` null | `InvalidArgument` + warn | `system/run_invalid` |
| `def.budgetMs` ≤ 0 | `InvalidArgument` + warn | `system/budget_invalid` |
| malformed `depends_on` spec (empty token, duplicate name, more than `kMaxSystemDependencies`) | `InvalidArgument` + warn | `system/dep_spec_invalid` |
| duplicate name in this world | `InvalidArgument` + warn | `system/duplicate` |
| `Io<T>`: `T` not a Laige component | compile error | — |
| `Io<T>`: `T` not determinism-safe (G-R8, M1-DET-01) | compile error (a `static_assert` in `registerSystem`) | — |
| `Io<T>`: `T` not registered (this world) | `InvalidArgument` + warn | `system/io_unregistered` |
| same component declared twice (any access) | `InvalidArgument` + warn | `system/io_duplicate` |
| more than `kMaxSystems` systems | `BudgetExhausted` + warn | `system/budget_exhausted` |

The `budget_raw` log field is the rejected budget in Q16.16 raw
units (value = raw / 2^16 ms, ADR 0002); `existing_system_id` /
`component_id` identify the conflicting registrations.

The G-R8 `static_assert` (M1-DET-01) is a compile-time check over the
system's declared I/O: every `Io<T, Access>` component must be
determinism-safe storage — integers, enums, `fpx16_16`, `float`, a
`SimMath<B>::Vec2/Vec3`, or a user struct marked
`LAIGE_DETERMINISM_SAFE(T, Members...)` (a `double` member is never
legal; no SimMath backend uses it). The check fails with an actionable
message naming the fix and pointing at
[api/determinism.md](determinism.md); it runs at the call site, before
any runtime validation. See
[concepts/determinism.md](../concepts/determinism.md) for the
two-layer enforcement (this trait is the compile-time half; the
`determinism-lint` CI job is the source half).

## Performance (DOC-004)

- **Registration (setup path):** O(n) in the number of registered
  systems (the duplicate-name scan); the def copy and the I/O sets
  are in-place writes into the fixed record table — **no allocation
  at registration** (asserted by the `system_registry` suite's
  zero-alloc window on the non-sanitizer trees; the sanitizer trees
  prove it leak-free).
- **Per tick:** the registry is read-only during the loop — the
  scheduler (M1-SYS-02) reads `SystemInfo` O(1) per system; no
  allocation, no logging by default (LOG-003).
- **`World::system` / `systemCount`:** O(1), no allocation.
- **`SystemContext::each`:** the `World::each` cost — the
  O(kMaxArchetypes · N) archetype scan plus one visit per matching
  entity; no allocation (query.md).
- **Misuse:** registering near the `kMaxSystems` bound makes the
  duplicate scan O(n²) across setup — a game that outgrows 256
  systems raises the constant through an ADR, not a hot path.

## Threading and failure (CONC-001, API-004)

Registration mutates the registry in an explicit setup phase on the
world's single owner thread (mutation phase, like
`registerComponent<T>`); systems run on the sim thread (PRD §10.2).
All failures are `Result`/`Status` values — the engine core and
public API use no exceptions (FR-12.1, NFR-8.10).
