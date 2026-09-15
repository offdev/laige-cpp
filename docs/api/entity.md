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
| `destroy(e)` | Destroy one live entity; if it is in an archetype its row is detached first (M1-ECS-03); bumps the slot generation; returns the slot to the free list. Stale/invalid → debug: **assert** (S-9); release: `InvalidArgument` + warn-once | O(1) for a component-less entity; O(tail × row-stride) when it has components — no allocation |
| `check(e)` | Access validation — the check every entity access performs (M1-ECS-03's component access builds on it). Stale/invalid → `InvalidArgument` + warn-once in **every build**; live → ok | O(1), no allocation |
| `isValid(e)` | Generation-checked liveness; no side effects | O(1) |
| `capacity()` / `entityCount()` | Declared budget / live count (the G-R3 numerator) | O(1) |
| `stats()` | `EntityStats` accounting snapshot (G-R3 and M1-PROF-01 feed) | O(1), no allocation |
| `beginFrame()` (M1-ECS-06) | Mark the frame start: resets the per-frame churn counters and the once-per-frame guardrail warn flags (G-R3/G-R4); no log, no return value. Driven once per frame by the owning loop (M1-LOOP-01); read the just-completed frame's counters with `guardrailStats()` before the next call | O(1), no allocation |
| `guardrailStats()` (M1-ECS-06) | `GuardrailStats` guardrail snapshot: the G-R3 level/warn counts, the G-R4 per-frame churn + its budget, the warn counters (M1-PROF-01 feed) | O(1), no allocation, no side effects |
| `clear()` | Destroy every live entity (shutdown path, CONC-006); each live row is detached (M1-ECS-03); every handle goes stale; capacity unchanged; world immediately reusable. Returns `Status`: under a live `each` iteration, `InvalidArgument` when a matched archetype holds live rows (query.md "Iteration legality") | O(capacity) scan + detaches, no allocation, idempotent |

Move-only (O(1) pointer swap — the archetype tables move with it, so
a moved world keeps its component data); a moved-from world is a
valid empty world (capacity 0: every `create()` fails, every handle
invalid). Not copyable.

`World::Options` (M1-DET-01) carries the determinism settings:

| Field | Default | Contract |
|---|---|---|
| `capacity` | 0 | The scene budget (G-R3); `> Entity::kMaxEntities` → `InvalidArgument`. |
| `churnPerFrameBudget` | `kDefaultChurnPerFrameBudget` | The G-R4 per-frame component-churn budget. |
| `seed` | 0 | The master PRNG seed; part of replay identity (ADR 0002). Each system registered in deterministic mode gets a substream derived from it (`Prng::deriveSubstream(seed, systemId)`). |
| `deterministic` | true | When true, `registerSystem` creates each system's PRNG substream (`SystemContext.rng` is non-null) and the G-R8 static_assert applies. When false, no substreams are created (`SystemContext.rng == nullptr`) — the documented escape hatch for non-deterministic prototypes. |

The engine (`engine.md`) forwards `EngineConfig.seed` and
`EngineConfig.determinism.enabled` to these fields.

M1-ECS-03 adds the component layer on the same slot tables:
`world.has<T>(e)`, `world.get<T>(e)`, `world.addComponent<T>(e, v)`,
`world.removeComponent<T>(e)`, `world.archetypeCount()`,
`world.archetypeStats()` — see
[archetype.md](archetype.md) (the stale-handle contract above is
inherited by all four component ops). M1-ECS-04 adds the query/
iteration API `world.each<T1, T2, ...>(fn, Read/Write tags...)` on
the same rows — see [query.md](query.md).

## Errors (FR-12.1, CORE-008)

| Operation | Failure | Code |
|---|---|---|
| `World::create(Options)` | `capacity > Entity::kMaxEntities` (16-bit id space) | `ErrorCode::InvalidArgument` (2) |
| `create()` | declared scene budget exhausted | `ErrorCode::BudgetExhausted` (4) |
| `destroy(e)` / `check(e)` | stale, cleared, or out-of-range handle | `ErrorCode::InvalidArgument` (2) |
| iteration-legality violations (write during read-iteration, structural mutation / `destroy` / `clear` touching a matched archetype, nested `each`) — M1-ECS-04 | the mutation is skipped, never applied; the iteration continues | `ErrorCode::InvalidArgument` (2) from the mutating call (debug: assert instead) — [query.md](query.md) |

Failures are `Result`/`Status` values — never exceptions, never
silent. The world logs its stale-handle degradations through the
logging facade under the stable subsystem name `ecs` (events
`stale_entity_access` / `stale_entity_destroy`); the facade's
per-(subsystem, event, severity) rate limit implements the
"warn-once" semantics (LOG-004: the first event emits, repeats are
counted and summarized).

## Guardrails (G-R3, G-R4) (M1-ECS-06)

The engine-enforced guardrails of PRD §9.3 for the entity storage.
Both are structured `Warn` events under the subsystem `ecs`, with
counters pulled by the profiler (M1-PROF-01) through
`guardrailStats()`. Both are O(1) integer bookkeeping on the hot
path — no allocation (PERF-003); the warn paths are cold (a budget
being crossed) and their fields construct only when the event is
enabled (LOG-003).

### G-R3 — entity-count thresholds

`create()` warns exactly when the live count **reaches** 25% / 50%
/ 100% of the declared scene budget — integer thresholds
`capacity * pct / 100`:

| Level | Event | Fires when (capacity 2000 example) |
|---|---|---|
| 25% | `ecs/entity_budget_25` | `entityCount == 500` |
| 50% | `ecs/entity_budget_50` | `entityCount == 1000` |
| 100% | `ecs/entity_budget_100` | `entityCount == 2000` (the last successful `create()`; the next one fails with `BudgetExhausted`) |

- A level whose threshold computes to **0 never fires** (the live
  count is 0 only before the first `create()`): e.g. capacity 2
  warns only at 50% (1) and 100% (2); capacity 1 warns only at 100%.
- **Once per level per frame**: dipping below a threshold and
  re-crossing it within the same frame does not re-warn. Frame
  boundaries are driven by `beginFrame()`; if the world is never
  frame-driven, the guardrail degrades to warn-once-per-lifetime
  (documented, never silent). The facade's rate limit (LOG-004)
  additionally collapses cross-frame repeats within its window.
- The warn carries the fields `entity_count`, `capacity`, `level`
  (25/50/100).

### G-R4 — per-frame component churn

The per-frame **churn** counts, per frame (`beginFrame()` to
`beginFrame()`):

- every successful `addComponent<T>` — including in-place
  overwrites (the `ArchetypeStats::totalAdds` semantics); and
- every `removeComponent<T>` that actually detaches a row.

No-op removes (the entity lacks the component), `destroy()`/`clear()`
detaches, and rejected calls (`BudgetExhausted`, invalid handle) are
**not** counted. When the per-frame total **strictly exceeds**
`World::Options::churnPerFrameBudget`, one `ecs/churn_per_frame`
warn fires for that frame (the warn carries the fields
`frame_churn`, `churn_budget`).

- **Budget:** `Options::churnPerFrameBudget`, default
  `laige::kDefaultChurnPerFrameBudget = 256` — at the M1 reference
  scene (10k entities, PRD §8.1) that is ~2.6% of the scene per
  frame: steady-state gameplay stays far below it, and a sustained
  breach indicates unbatched spawn/despawn churn on the hot path.
  Churn-heavy scenes raise it through typed configuration (API-006);
  **0 disables** the guardrail (documented).

### Message grammar (NFR-13.3) and the debug advice field

Every guardrail warn's **message text** is the 5-field NFR-13.3
error-grammar line, identical in every build (machine-parseable,
stable):

```text
{code} | {what} | {why} | {fix} | {doc_anchor}
```

e.g.

```text
entity_budget_100 | live entities reached 100% of the declared scene budget | the scene budget is full; the next create() fails with BudgetExhausted | destroy entities before spawning more, or raise the scene budget through typed configuration | docs/api/entity.md#guardrails
```

Per PRD §9.3 ("warn (debug: with advice)"), **debug builds** add the
advice as an extra structured `advice` FIELD — never as message
text, so the 5-field grammar stays build-stable. The G-R4 advice is
the PRD's: *move the churn to a spawn/despawn system* (the same
advice the iteration-legality rejection points to, query.md).

### `GuardrailStats` (the profiler feed)

| Field | Meaning |
|---|---|
| `capacity` | the declared scene budget (G-R3 denominator) |
| `entityCount` | live entities right now (G-R3 numerator) |
| `entityBudgetLevel` | 0/25/50/100 — the highest percentage the **peak** live count reached since construction |
| `entityBudgetWarns[3]` | per-level warn counts (index 0 = 25%, 1 = 50%, 2 = 100%) |
| `frameChurn` | adds + removes since the last `beginFrame()` |
| `churnPerFrameBudget` | the configured G-R4 budget (0 = disabled) |
| `churnWarns` | churn warnings issued since construction |

`beginFrame()` resets `frameChurn` (and the warn flags); the warn
counters and `entityBudgetLevel` are since-construction.

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
- **Memory per slot:** 11 B bookkeeping (2 B generation + 1 B alive
  flag + 2 B free-list entry + 2 B archetype slot + 4 B row index —
  M1-ECS-03). `EntityStats` reports `capacity × 11` / `inUse × 11`
  bytes; the archetype column blocks are accounted separately in
  `ArchetypeStats` (archetype.md).
- **Guardrail cost (M1-ECS-06):** `create()` adds three threshold
  comparisons; each counted add/remove adds one counter increment
  plus one comparison; `beginFrame()` is a handful of stores. Pure
  integer bookkeeping, no allocation — a zero-allocation test pins
  the below-threshold window (`ecs_guardrails` suite,
  `GuardrailChecksAllocateNothingBelowTheThresholds`).
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

// Per frame (the game loop, M1-LOOP-01): restart the guardrail windows.
world.beginFrame();  // G-R3/G-R4 (M1-ECS-06): reset per-frame churn + warn flags
// ...at frame end, before the next beginFrame():
//   const auto g = world.guardrailStats();  // the profiler feed (M1-PROF-01)

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
- **M1-ECS-02 (done):** the component registry —
  `ComponentTypeId`, `LAIGE_COMPONENT`, `World::registerComponent<T>`;
  see [component_registry.md](component_registry.md).
- **M1-ECS-03 (done):** archetype SoA component storage on top of
  the same slot table; `world.get<T>(e)` is built on
  `World::check(e)` and inherits the stale-handle contract — see
  [archetype.md](archetype.md).
- **M1-ECS-04 (done):** the query API + iteration legality —
  `World::each<T1, T2, ...>(fn, Read/Write tags...)` iterates the
  archetype rows with a stack-scoped guard (no hidden allocations);
  see [query.md](query.md).
- **M1-ECS-05 (done):** deterministic iteration (archetype order,
  entity id order — PRD §10.3) over the visit order M1-ECS-04 pins —
  see [iteration_order.md](iteration_order.md).
- **M1-ECS-06 (done):** the G-R3 entity-count thresholds and the
  G-R4 per-frame churn guardrail — `beginFrame()`,
  `guardrailStats()`, the `ecs/entity_budget_{25,50,100}` and
  `ecs/churn_per_frame` warns (the "Guardrails" section above);
  suite `ctest -R ecs_guardrails`.
