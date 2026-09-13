# Archetype SoA component storage (`World::get<T>`, `World::addComponent<T>`, ...)

Archetype SoA storage (M1-ECS-03; PRD §9.1, AGENTS PERF-003/004,
CORE-001, G-R4). Public header:
`src/laige-sim/include/laige/sim/archetype.h` (constants, `ArchetypeStats`,
layout contract) plus the `World` member templates in
`src/laige-sim/include/laige/sim/entity.h`; implementation:
`src/laige-sim/archetype.cpp` (+ the slot tables in `entity.cpp`).
Unit suite: `ctest -R archetype` (`tests/laige-sim/archetype_tests.cpp`),
including the 10k-entity churn test whose machine-greppable stats line
lands in the ctest output on every run (CORE-001).

An **archetype** is a *set of component types* an entity can carry,
stored as **Structure-of-Arrays**: one contiguous `T[]` column per
component type, one row per entity in the set. The entity→archetype
map is two dense per-slot tables (`archetypeOf_`, `rowOf_`), so
`get<T>(e)` is **O(1)**: slot → archetype → column index → row, no
hash, no search (the roadmap's "archetype lookup + column index").

## Storage layout

```text
per archetype (256 max, created on first sighting of a component set):
  sig          the component ids, strictly ascending (dense 1-based)
  fingerprint  FNV-1a 32-bit over sig (short-circuit only; a collision
               is harmless — the lexicographic verify decides)
  slotCol[r]   the entity slot id of row r — rows are kept in
               ascending slot-id order (a pure function of the world
               state; M1-ECS-05 pins the convergence property)
  columns[c]   one byte[] block per component: rowCapacity * size bytes
               (over-allocated 31 B), base aligned to 32 bytes
               (kArchetypeColumnAlignment — covers every
               trivially-copyable alignof on the P0 targets)

per entity slot (entity.h): archetypeOf_[slot] (0 = no archetype) +
rowOf_[slot] — the dual representation; the two always agree
(invariant I2, maintained inside attachSlot/removeRow).
```

Rows are packed (no gaps): the tail is memmoved on every
insert/remove, so contiguity per column is a stored-data property,
not an assumption — the suite pins it (the `ArchetypeLayout`
property tests check element spacing, 32-byte alignment, and the
slot-ordered addresses).

## The `World` component API

| Operation | Behavior | Complexity / allocation |
|---|---|---|
| `world.has<T>(e)` | Membership: is `T` in `e`'s set? Stale handle → `false` (pure query, no warn); unregistered `T` → `false` | O(1), no allocation |
| `world.get<T>(e)` | `T*` into the column row, or `nullptr`: stale handle (warn-once via `check`), unregistered `T`, or `e` lacks `T` (silent — the two silent cases are documented, never an error) | O(1), no allocation |
| `world.addComponent<T>(e, v)` | **Create-or-update**: `T` present → overwrite in place (no move); absent → move `e` to the set `current ∪ {T}`, copying the shared components into the new row. Stale handle → `InvalidArgument` + warn-once; unregistered `T` → `InvalidArgument` + warn; set would exceed 32 components → `BudgetExhausted` + warn | O(tail × row-stride) bytes moved (tail = rows at/above the insertion point); **no heap allocation** — growth is a pre-reserved, accounted, logged reserve (below) |
| `world.removeComponent<T>(e)` | Remove `T`: no-op ok if absent; else move `e` to `current ∪ {T} \ {T}`, copying the remaining components into the new row. Same error rows as add | as add |
| `world.archetypeCount()` | Distinct component sets created so far (archetypes are never destroyed; empty sets stay) | O(1) |
| `world.archetypeStats()` | `ArchetypeStats` snapshot: `archetypeCount`, `rowsLive`, `rowsReserved`, `bytesReserved`, `totalAdds`, `totalRemoves`, `totalArchetypeGrowth`, `totalReservations` (the M1-PROF-01 / G-R4 feed) | O(256 × 32) cold pass, no allocation |

The `World::destroy(e)` / `World::clear()` cost note now includes the
row detach: an entity with components leaves its archetype first —
O(tail × row-stride) bytes moved, still no allocation (entity.md).

## Reserve policy (no per-op allocation)

Each column block is reserved, not sized, to the rows it holds:

- **Initial:** `min(kInitialArchetypeRows = 16, world capacity)` rows,
  at archetype creation.
- **Growth:** ×2, capped at the world capacity — one bounded
  `unique_ptr` reallocation per column per growth, **accounted** in
  `totalReservations` / `totalArchetypeGrowth` and logged
  (`ecs/archetype_grow`). Bounded by `log2(capacity / 16) + 1` growth
  events per archetype (≤ 10 for a 10k world).
- **Steady state:** an add/remove that does not hit a full archetype
  allocates nothing — the churn test proves it: zero reservation
  delta over the window **and** zero process-wide allocations
  (test-only `operator new` counter, non-sanitizer trees; the
  sanitizer trees prove the same property with a leak-free run of the
  same loop — M1-ALLOC-01 lands the standing assertion).

## Budgets

| Constant | Value | Meaning | Error when exceeded |
|---|---|---|---|
| `kMaxArchetypes` | 256 | distinct component sets per world | `BudgetExhausted` + warn `ecs/archetype_budget` |
| `kMaxArchetypeComponents` | 32 | components on one entity (one row) | `BudgetExhausted` + warn `ecs/component_limit` |
| `kMaxComponentTypes` | 256 | registered types per world (M1-ECS-02) | `BudgetExhausted` |

The 256/32 bounds are M1 engine-level caps (documented here and in
archetype.h); raising either is an ADR (the zone workloads — PRD
§12.1: 200–2000 entities, small component palettes — are far inside
them).

## Logging (LOG-001/002/004)

Stable subsystem `ecs`, stable events:

| Event | Severity | When |
|---|---|---|
| `archetype_created` | Info | a component set is seen for the first time (`archetype_id`, `components`) |
| `archetype_grow` | Info | a column reserve doubles (`archetype_id`, `rows`, `components`) |
| `archetype_budget` | Warn | 257th distinct set (rate-limited, `archetype_count`) |
| `component_limit` | Warn | 33rd component on one entity (rate-limited) |
| `component_unregistered` | Warn | add/remove of a type not registered in this world (rate-limited) |
| `stale_entity_access` | Warn | component op on a stale handle (rate-limited; inherited from M1-ECS-01) |

Repeats are rate-limited per (subsystem, event, severity) with the
suppressed-count summary on shutdown (LOG-004) — the suite pins the
warn-once + `rate_limited` drain.

## Performance (DOC-004)

- **Hot path:** `has<T>` / `get<T>` are O(1) pointer arithmetic
  (slot table → record → column binary search ≤ 32 → row). **No
  allocation, no lock, no I/O, no logging** on any success path
  (PERF-003; LOG-003).
- **Move cost:** add/remove between archetypes memmove the tail —
  O(tail × row-stride) bytes plus the O(tail) `rowOf_` re-sync.
  This is the documented cost of the dense packed rows (PERF-004:
  contiguous over pointer-per-entity); it is a *spawn/despawn-time*
  cost, not a per-tick one (iteration — the per-tick path — lands in
  M1-ECS-04/05 over the same columns).
- **Measured baseline (CORE-001; g++ 16.2.1, 2026-09, single-threaded
  headless):** 10k entities × 20k add/remove ops (seeded random
  order, full cost range), 3-component working set:

  | Build | p50 | p99 | p99/p50 |
  |---|---|---|---|
  | Debug (`-O0`) | 0.123 ms | 0.243 ms | 1.98 |
  | Release (`-O2`) | 0.0021 ms | 0.0040 ms | 1.94 |

  The suite asserts the flatness (`p99 < 3 × p50`), the zero
  reservation delta, and the zero-allocation window, and prints the
  machine-greppable line (`archetype-churn <stats>`) to the ctest
  output on every run — the M1 baseline record for the G-R4 feed.
  Numbers are machine-dependent; the *shape* (flat, no spike, no
  allocation) is the tested property.
- **Memory per entity (live, with components):** one row per archetype
  column — `Σ component sizes` bytes (8 B for a pos+vel pair) plus the
  2 B slot column slot; per-slot bookkeeping is 11 B (entity.md).
  Reserved (not live) bytes are accounted in `bytesReserved`.
- **Traps:**
  - The move cost is proportional to the row *above* the insertion
    point: adding a component to a high-slot entity in a large
    archetype is the expensive case (the churn window spans exactly
    this range). Batch spawn/despawn when possible (API-002); the
    per-tick iteration never moves rows.
  - Archetypes are never destroyed: a workload that churns through
    many component *sets* fills the 256-set budget even though few
    entities are live — keep the per-world component-set count
    bounded by design (game palettes are small; the warn names it).
  - `get<T>` returns a raw pointer valid until the entity's next
    add/remove/destroy (the row can move) — never hold it across a
    mutating op.

## Threading and determinism

- **Single owner thread** (CONC-001); not thread-safe (M1 is
  single-threaded simulation — PRD §10.2).
- **Determinism (ARCH-010):** the row order is a pure function of the
  world state (ascending slot id); the type→id lookup is a
  deterministic splitmix64 hash with linear probing that is *only
  ever queried* (never iterated), and the archetype scan is a
  lexicographic verify — no platform intrinsics, no floating point.
  The same operation sequence produces bit-identical layouts and
  address sequences on every platform; two worlds that reach the
  same state through different interleavings share the layout
  (the suite's convergence test).

## Usage (performant pattern)

```cpp
// Setup (once): register every component type the world will carry.
ASSERT(world.registerComponent<PlayerPos>().ok());
ASSERT(world.registerComponent<PlayerVel>().ok());

auto e = world.create();
if (e.isError()) { /* BudgetExhausted: refuse the spawn (S-2) */ }

// Spawn: create-or-update (an existing component is overwritten, not duplicated).
world.addComponent<PlayerPos>(e.value(), PlayerPos{1, 2});
world.addComponent<PlayerVel>(e.value(), PlayerVel{9});

// Read (per tick, via the M1-ECS-04 query API once it lands):
const PlayerPos* p = world.get<PlayerPos>(e.value());  // O(1), nullptr on absence
if (p != nullptr) { /* use p — valid until the entity's next mutation */ }

// Despawn:
world.removeComponent<PlayerVel>(e.value());  // back to {PlayerPos}
world.removeComponent<PlayerPos>(e.value());  // component-less, still alive
```

## Misuse warnings

- `get<T>` returning `nullptr` is **not always** an error: unregistered
  `T` and absent `T` are silent by contract (a stale handle is the
  one case that warns). Branch on it; don't log every miss.
- Holding a `get<T>` pointer across `addComponent`/`removeComponent`
  on the same entity is dangling (the row moves). Re-fetch.
- `addComponent` is create-or-update: passing a new value for a
  present component *replaces* it — no duplication, no error.
- Adding components one-by-one through many intermediate sets walks
  through (and leaves alive) intermediate archetypes; for a spawn
  with N new components, N−1 moves are the cost.
- The 256-set / 32-component caps are hard engine bounds: a `Warn`
  `ecs/archetype_budget` / `ecs/component_limit` means the game's
  component design outgrew M1 — fix the design or ADR the bound.

## Roadmap context

- **M1-ECS-01 (done):** the entity handle and slot storage —
  [entity.md](entity.md).
- **M1-ECS-02 (done):** the component registry that supplies the
  ids and sizes this storage consumes —
  [component_registry.md](component_registry.md).
- **M1-ECS-03 (this step):** the storage above.
- **M1-ECS-04:** the query/iteration API over these columns (no
  iteration-legality state until M1-ECS-05).
- **M1-ECS-05:** deterministic iteration over the stored row order
  (the slot-ordered scheme above is what it iterates).
- **M1-ECS-06:** the G-R3 warn thresholds read the same slot tables.
- **M1-PROF-01 / G-R4:** `archetypeStats()` feeds the profiler.
- **M1-ALLOC-01:** the standing zero-allocation assertion over the
  churn property this step measured.
