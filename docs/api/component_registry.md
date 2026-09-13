# Component type registry (`ComponentTypeId`, `LAIGE_COMPONENT`, `World::registerComponent<T>`)

The M1 component registry (M1-ECS-02; PRD §9.1 S-8, FR-1.2, AGENTS
CPP-003/007, ARCH-010, PERF-003/006). Public header:
`src/laige-sim/include/laige/sim/component.h`; the `World` methods are
declared in `src/laige-sim/include/laige/sim/entity.h` and implemented
in `src/laige-sim/entity.cpp` / `entity.h` (the template). Unit suite:
`ctest -R component_registry` (`tests/laige-sim/component_registry_tests.cpp`).

Components are identified by **compile-time trait types** (FR-1.2): a
type becomes a component by writing `LAIGE_COMPONENT(T)` once next to
its definition, and a component exists in a world once
`world.registerComponent<T>()` succeeds. Built-in and user-defined
components use the same path — a user-defined struct is a legal
component (S-8 data-carrier case, PRD Appendix B shape).

M1 components are **data-only**. The replication and inspector traits
(the full `LAIGE_COMPONENT(T, .replicate(...), .inspector(...))`
surface) extend the macro in M4; the M1 trait carries no behavior.

## The id contract (FR-1.2, ARCH-010)

`ComponentTypeId` is a 32-bit value; `0` is reserved
(`laige::kInvalidComponentTypeId` is never assigned).

- Ids are assigned **in registration order, densely from 1**: the first
  successful `registerComponent<T>()` on a world gets id 1, the second
  gets id 2, and so on.
- The cap is `laige::kMaxComponentTypes` (**256** types per world) —
  a named engine-level constant (CORE-005). A game's component
  vocabulary is orders of magnitude smaller than its entity count; a
  project that outgrows the cap raises the constant through an ADR,
  not per world.
- **Determinism (ARCH-010):** assignment is pure integer bookkeeping —
  no addresses, hashes, or platform state enter the id. Two worlds,
  two process runs, or two builds that register the same types in the
  same order produce bit-identical id sequences. Component ids are
  therefore replay state from M1 on and safe to replicate
  (M1-DET-01/02). The ordering contract is documented, not implied:
  a *different* registration order produces *different* ids — games
  register their component set once at world setup, in one documented
  place (M1-HEAD-01 wires the loop around it).
- **Per-world ids:** a `ComponentTypeId` is meaningful only in the
  world that assigned it (the same cross-world caveat as `Entity`
  handles). Never compare ids across worlds.

`operator<` orders ids by registration order; M1-ECS-03's
per-archetype component sets and M1-ECS-05's deterministic iteration
order by it.

## The `LAIGE_COMPONENT` macro

```cpp
struct Health {
  std::int32_t current = 100;
  std::int32_t max = 100;
};
LAIGE_COMPONENT(Health);   // once, at namespace scope, next to the type
```

- Compile-time only: it specializes the component trait for `T`. It
  adds no runtime state by itself; the runtime registration (id +
  size/alignment) happens in `World::registerComponent<T>()`.
- Components must be **trivially copyable** data carriers (S-8):
  `registerComponent<T>()` `static_assert`s this, so a non-trivial
  member (a string, a destructor, a vtable) is a compile error with an
  actionable message before it can break the M1-ECS-03 SoA layout.
- Marking a type twice is a compile error (duplicate specialization).
- The macro must be written at namespace scope: the C++ standard
  requires a full specialization to be declared in the primary
  template's enclosing namespace.

## The `World` API

| Operation | Behavior | Complexity / allocation |
|---|---|---|
| `registerComponent<T>()` | Setup-phase registration: assigns the next id and records `sizeof(T)`/`alignof(T)`. See the Errors table below | O(n) in the registered types; no allocation |
| `componentCount()` | Types registered so far (`0 .. kMaxComponentTypes`) | O(1) |
| `componentInfo(id)` | The recorded `ComponentInfo{size, alignment}` for a registered id | O(1), no allocation |

`registerComponent<T>()` is a **setup-phase operation** (world
construction, before the loop; API-004: mutation in an explicit
phase). It is not a per-tick operation and never runs on the sim hot
path. The M1-ECS-03 SoA storage reads the recorded size/alignment;
per-entity component data arrives with that step.

### Type identity without RTTI (NFR-8.10, PERF-006)

Duplicate detection and the type → record lookup key on each type's
compile-time identity token: the address of
`detail::ComponentTypeKey<T>::kMarker` (one `inline static` per
template specialization; distinct types have distinct addresses,
[basic.stc]). The registry table therefore uses no `std::type_info`
(RTTI is disabled, NFR-8.10) and no hash map (unordered containers are
banned from sim paths, PERF-006). The duplicate scan is linear over
the registered records — a setup-path cost, never a hot path.

## Errors (FR-12.1, CORE-008)

| Operation | Failure | Code |
|---|---|---|
| `registerComponent<T>()` | `T` not marked with `LAIGE_COMPONENT` | compile error (`static_assert`, actionable message) |
| `registerComponent<T>()` | `T` not trivially copyable | compile error (`static_assert`, actionable message) |
| `registerComponent<T>()` | `T` already registered in this world | `ErrorCode::InvalidArgument` (2) + one rate-limited warn |
| `registerComponent<T>()` | `kMaxComponentTypes` reached (a new type) | `ErrorCode::BudgetExhausted` (4) |
| `registerComponent<T>()` | moved-from world (no registry) | `ErrorCode::InvalidArgument` (2) |
| `componentInfo(id)` | reserved/unregistered id | `ErrorCode::InvalidArgument` (2) |

Duplicate registration is an **error, not a no-op** (FR-12.3: never
silent). The world logs it through the logging facade under the stable
subsystem name `ecs`, event `component_duplicate`, with the
already-registered `component_id` plus the recorded `size` and
`alignment` fields (LOG-002); the facade's rate limit implements the
warn-once semantics (LOG-004).

## Performance (DOC-004)

- **Setup path only:** `registerComponent<T>()` is one linear scan
  over ≤ 256 records plus one 16-byte record write. No allocation, no
  I/O, no synchronization (PERF-003, PERF-007 documents the O(n) cost;
  n is bounded by `kMaxComponentTypes`).
- **Registry memory:** 256 × 16 B = 4 KiB per world (fixed at world
  construction, the setup-path allocation next to the entity tables).
  This is type metadata only; per-entity component bytes are the
  M1-ECS-03 SoA columns' job.
- **Zero-alloc enforcement:** the standing assertion lands with
  M1-ALLOC-01; registration is not a hot path, so the step is verified
  by ASan + the no-allocation implementation (M1 milestone rules).
- **Traps:**
  - Do not register in a per-tick system: mutation in the sim phase
    invalidates iteration (M1-ECS-04's legality contract) and breaks
    the replay identity (the registration order is replay state).
  - `BudgetExhausted` from registration means the engine-level type
    budget is full: surface it and review the component vocabulary
    (raise the constant via ADR) — never add per-world capacity.
  - `componentInfo(id)` with an id from another world is
    meaningless (per-world ids above).

## Threading, ownership, determinism

- `World` has one owner thread (CONC-001; PRD §10.2: simulation is
  single-threaded in M1); registration happens on that thread, in the
  setup phase. `World` is move-only; the registry moves with it, and a
  moved-from world has no registry (`registerComponent<T>()` returns
  `InvalidArgument`; the same "valid empty world" contract as entity
  creation).
- `clear()` destroys live entities only; the type registry is setup
  state and survives (M1-ECS-03's `clear()` will destroy per-entity
  component data, not the type records).
- **Determinism (ARCH-010):** see the id contract — bit-identical id
  sequences across runs/builds for a fixed registration order.

## Usage (performant pattern)

```cpp
// One documented registration site per world setup (M1-HEAD-01).
struct PlayerPos { int32_t x; int32_t y; };   // built-in or user type
LAIGE_COMPONENT(PlayerPos);

auto w = laige::World::create(laige::World::Options{2000});
laige::World& world = std::move(w).takeValue();

// Setup phase: register every component type the game uses, once.
auto pos = world.registerComponent<PlayerPos>();
if (pos.isError()) {
  // InvalidArgument  -> duplicate (or moved-from world): fix the
  //                      registration list (FR-12.3).
  // BudgetExhausted  -> engine-level type budget full: ADR path.
}
// `pos.value()` is the stable ComponentTypeId for this world —
// M1-SYS-01 declares system I/O by these ids, and M1-DET-02's replay
// header hashes the component schema built from them.

// Later (M1-ECS-03): world.add_component<PlayerPos>(e, {...}) and
// world.get<PlayerPos>(e) resolve T -> id -> SoA column through this
// registry; O(1), no allocation.
```

## Misuse warnings

- Registering the same type twice on one world is an error, not a
  no-op: read the `Result` (FR-12.3).
- Do not compare `ComponentTypeId` values across worlds (per-world
  ids).
- A moved-from world has no registry: `registerComponent<T>()` on one
  fails with `InvalidArgument`.
- Mark each type once with `LAIGE_COMPONENT`; a second mark is a
  compile error.
- Component types must be trivially copyable (S-8 data carriers); the
  `static_assert` in `registerComponent<T>()` is the guard.

## Roadmap context

- **M1-ECS-01 (done):** the entity handle and world entity storage
  this registry hangs off — see [entity.md](entity.md).
- **M1-ECS-02 (this step):** the component registry above.
- **M1-ECS-03:** archetype SoA storage consumes the recorded
  size/alignment (`world.add_component<T>`/`world.get<T>`, O(1) column
  lookup).
- **M1-ECS-05:** deterministic iteration orders the component sets by
  registration order (`operator<`).
- **M1-SYS-01:** system I/O declarations reference these ids.
- **M1-DET-02:** the replay header's component-schema hash is computed
  from the registered (id, size, alignment) triples in id order.
- **M4:** the macro gains the replication/inspector trait surface
  (PRD Appendix B).
