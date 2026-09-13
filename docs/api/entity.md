# Entity handle and world entity storage (`laige::Entity`, `laige::World`)

The ECS foundation (M1-ECS-01; PRD §9.1 S-1, FR-1.2, AGENTS CPP-007,
PERF-002/003, G-R3). Public header:
`src/laige-sim/include/laige/sim/entity.h`; implementation:
`src/laige-sim/entity.cpp`. Unit suite: `ctest -R entity`
(`tests/laige-sim/entity_tests.cpp`).

The safe API uses **32-bit entity handles, never raw pointers** (S-1):
use-after-free is detectable through the handle scheme, and — from
M1-ECS-03 on — structurally detectable through the generation check.

## The 32-bit handle (FR-1.2, CPP-007)

```text
Entity (exactly 32 bits, static-asserted):
  [ id: 16 bits | generation: 16 bits ]
```

- **id** — the slot index, `0 .. 65535` (`Entity::kMaxEntityId`).
- **generation** — `1 .. 65535`. Generation `0` is reserved: the
  default `Entity{}` is never valid.
- A handle is valid in the world that produced it while the slot is
  live **and** the slot's current generation equals the handle's.
  `destroy()`/`clear()` bump the slot's generation, so every stale
  handle to that slot fails `isValid()` and can never pass again.

**Generation wrap (documented limit).** After `2^16` (= 65536)
releases of one slot, the 16-bit generation wraps through the
reserved 0 and re-enters at generation 1 — colliding with the slot's
first incarnation, whose stale handle can no longer be distinguished.
That is the one case the scheme does not rule out: it requires a
single slot to be recycled 65536 times in a process's lifetime,
which is beyond the PRD §12.1 zone capacities (200–2000 entities) and
the M1 workloads (10k entities, 10k frames of churn). The M0-CORE-05
`Pool<T>` precedent documents the identical case at its 2^32 bound.
The wrap is defined unsigned wraparound (CPP-004), so handle
sequences stay bit-identical across platforms (ARCH-010).

**Cross-world caveat.** A handle is meaningful only in the world that
produced it: an id+generation pair live at the same generation in two
worlds passes `isValid()` in both (the same cross-pool caveat as
`PoolHandle`, M0-CORE-05).

## The `World` API

| Operation | Behavior | Complexity / allocation |
|---|---|---|
| `World::create(Options)` (static) | Construction (setup path): the storage's only backing allocations. `capacity > Entity::kMaxEntities` → `InvalidArgument` (the world is not created) | setup; allocates 3 arrays of `capacity` slots |
| `create()` | Create one entity. LIFO slot recycling (deterministic). Beyond the budget → `BudgetExhausted` | O(1), no allocation |
| `destroy(e)` | Destroy one live entity; bumps the slot generation; returns the slot to the free list. Stale/invalid → debug: **assert** (S-9); release: `InvalidArgument` + warn-once | O(1), no allocation |
| `check(e)` | Access validation — the check every entity access performs (M1-ECS-03's component access builds on it). Stale/invalid → `InvalidArgument` + warn-once in **every build**; live → ok | O(1), no allocation |
| `isValid(e)` | Generation-checked liveness; no side effects | O(1) |
| `capacity()` / `entityCount()` | Declared budget / live count (the G-R3 numerator) | O(1) |
| `stats()` | `EntityStats` accounting snapshot (G-R3 and M1-PROF-01 feed) | O(1), no allocation |
| `clear()` | Destroy every live entity (shutdown path, CONC-006); every handle goes stale; capacity unchanged; world immediately reusable | O(capacity) scan, no allocation, idempotent |

Move-only (O(1) pointer swap); a moved-from world is a valid empty
world (capacity 0: every `create()` fails, every handle invalid).
Not copyable.

## Errors (FR-12.1, CORE-008)

| Operation | Failure | Code |
|---|---|---|
| `World::create(Options)` | `capacity > Entity::kMaxEntities` (16-bit id space) | `ErrorCode::InvalidArgument` (2) |
| `create()` | declared scene budget exhausted | `ErrorCode::BudgetExhausted` (4) |
| `destroy(e)` / `check(e)` | stale, cleared, or out-of-range handle | `ErrorCode::InvalidArgument` (2) |

Failures are `Result`/`Status` values — never exceptions, never
silent. The world logs its stale-handle degradations through the
logging facade under the stable subsystem name `ecs` (events
`stale_entity_access` / `stale_entity_destroy`); the facade's
per-(subsystem, event, severity) rate limit implements the
"warn-once" semantics (LOG-004: the first event emits, repeats are
counted and summarized).

## Stale-handle behavior matrix (FR-12.3, S-9)

| Operation | Debug build | Release build |
|---|---|---|
| `isValid(e)` | `false` | `false` |
| `check(e)` | ok / `InvalidArgument` + warn-once (the query degrades safely) | ok / `InvalidArgument` + warn-once |
| `destroy(e)` on stale | **assert** (SIGABRT — a loud use-after-free crash) | `InvalidArgument` + warn-once |

Component access (M1-ECS-03) inherits the `destroy` row: uses of a
stale handle assert in debug and degrade in release.

## Performance (DOC-004)

- **Hot path:** `create()`/`destroy()`/`check()`/`isValid()` are O(1)
  integer bookkeeping — one LIFO stack pop/push plus generation and
  alive-flag reads. **No allocation** on any operation after
  construction (PERF-003); the free list is a pre-allocated
  `uint16` stack (PERF-004: contiguous, compact, no pointers).
- **Memory per slot:** 5 B bookkeeping (2 B generation + 1 B alive
  flag + 2 B free-list entry). `EntityStats` reports
  `capacity × 5` / `inUse × 5` bytes (M1-ECS-03 adds the per-entity
  record to this number).
- **Zero-alloc enforcement:** the standing assertion lands with
  M1-ALLOC-01; until then the step is verified by ASan + the
  `stats()` accounting (M1 milestone rules).
- **Traps:**
  - Holding a handle past `destroy()`/`clear()` and using it is
    use-after-free — the debug build crashes on it (S-9); in release
    read the `Status` and surface it.
  - `BudgetExhausted` is a declared budget being exceeded — log it
    and refuse the spawn (S-2); never grow the world at runtime
    (G-R1).
  - `clear()` is O(capacity) — a shutdown-phase operation, not a
    per-tick one.

## Threading and determinism

- **Single owner thread** (CONC-001); `World` is not thread-safe
  (PRD §10.2: simulation is single-threaded in M1).
- **Determinism (ARCH-010):** slot assignment is pure integer
  bookkeeping — no floating point, no randomness, no platform
  intrinsics. The same create/destroy sequence produces
  bit-identical `Entity` handle sequences on every platform, so
  handles are replay state from M1 on.

## Usage (performant pattern)

```cpp
// Setup (once, at world construction — the only allocation).
auto w = laige::World::create(laige::World::Options{2000});  // scene budget (G-R3)
if (w.isError()) { /* capacity above the 16-bit id space: configuration bug */ }
laige::World& world = std::move(w).takeValue();

// Hot path (per tick): no allocation.
auto r = world.create();
if (r.isError()) {
  // BudgetExhausted: the declared budget is full — log under your
  // subsystem name and refuse the spawn (S-2). Never grow the world.
} else {
  spawnThing(r.value());  // keep the Entity handle (32 bits)
}

// Later, when the entity dies:
world.destroy(handle);  // every other copy of `handle` is now stale

// Before accessing stored state (M1-ECS-03 and on):
if (!world.isValid(handle)) { /* stale — drop it, log if unexpected */ }
```

## Misuse warnings

- A handle held past `destroy()`/`clear()` is use-after-free: check
  `isValid()` or read the `Status` — never assume.
- A handle from one world used in another is meaningless (cross-world
  caveat above).
- `BudgetExhausted` is not a transient error to retry-loop on: it is
  the declared budget being exceeded — surface it and degrade
  (G-R1/G-R3).
- `World` is move-only with one owner thread; copying is deleted and
  sharing across threads requires an explicit engine boundary
  (CONC-002), which M1 defines per subsystem.

## Roadmap context

- **M1-ECS-01 (this step):** the handle + entity storage above.
- **M1-ECS-02:** the component registry (`ComponentTypeId`) —
  components get a stable compile-time id.
- **M1-ECS-03:** archetype SoA component storage on top of the same
  slot table; `world.get<T>(e)` is built on `World::check(e)` and
  inherits the stale-handle contract.
- **M1-ECS-05:** deterministic iteration (archetype order, entity id
  order — PRD §10.3).
- **M1-ECS-06:** the G-R3 warn thresholds (25%/50%/100% of the
  declared budget) pull `stats()`.
