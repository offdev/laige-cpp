# Query API + iteration legality (`World::each<T1, T2, ...>`)

The M1 query/iteration API over the archetype SoA columns
(M1-ECS-04; PRD §9.1 S-3/S-4, §10.3, FR-12.3, AGENTS PERF-003/004/006,
API-004). Public headers: `src/laige-sim/include/laige/sim/query.h`
(`Access`, the `Read`/`Write` access tags, the `detail::IdSet256`
guard sets, the full contract) plus the `World::each` member template
in `src/laige-sim/include/laige/sim/entity.h`; implementation:
`src/laige-sim/query.cpp` (the iteration-guard helpers) + the
header-defined `each` body. Unit suite: `ctest -R query`
(`tests/laige-sim/query_tests.cpp`), including the 10k-entity
zero-allocation window whose machine-greppable stats lines land in
the ctest output on every run (CORE-001).

The PRD Appendix B sketch calls this `ctx.each<Tag<Player>, Health>()`;
in M1 the query lives on the `World` that owns the storage, and
M1-SYS-01's `SystemContext` delegates to it (one world, one owner
thread). The sketch's argument order (access flags first, callable
last) is not the final syntax: C++ cannot deduce a parameter pack
that is not last, so the access tags follow the callable — one tag
per listed component, in template order, still declared per component.

## Query semantics

```cpp
world.each<ArchPos, ArchVel>(fn, laige::Read{}, laige::Write{});
```

- **Superset match:** an archetype matches when *every* listed
  component type is in its set; extra components do not exclude an
  entity (an entity with `{Pos, Vel, Flag}` is visited by
  `each<Pos, Vel>`).
- **Reference hand-off:** `fn` is invoked once per matching entity as
  `fn(Entity, R1, ..., RN)` — one reference per listed component, in
  template order: a `const T&` where the tag is `Read`, a `T&` where
  it is `Write` (compile-time; a write through a `Read` reference
  requires a cast — a bug the compiler rejects, API-008). The
  `Entity` handle is the generation-checked live handle of the row's
  slot.
- **Tag discipline:** exactly one `Read`/`Write` tag per listed
  component, in template order; a mismatched type or count is a
  compile error (static_asserts in `each`), not a runtime surprise.
- **Empty query:** `world.each<>(fn)` visits every live entity (the
  component-less ones included) in ascending slot-id order, with no
  component references. It iterates no archetype rows, so its guard
  matches no archetype and structural mutations remain legal under
  it.
- **Unregistered type:** a listed component not registered in this
  world matches nothing — the iteration runs zero times and returns
  ok (a pure query, like `has<T>` reading `false`). No archetype is
  created for the query.

## Visit order

Ascending archetype id (creation order), then ascending slot id
within the archetype (the slot-ordered rows of archetype.h — invariant
I2). The empty query visits ascending slot id directly. M1-ECS-05
documents this as the deterministic iteration contract; the suite
pins the exact sequence.

## Iteration legality (API-004, FR-12.3)

A live `each` iteration carries a **guard** (two membership-only
256-bit sets, `detail::IdSet256`, stack-owned): the matched-archetype
set (computed before the first callback, so complete for the whole
iteration) and the read-component set (the queried components
declared `Read`). Mutations are checked *before* any side effect —
a rejected mutation is **skipped, never applied**, and the
iteration continues over the unmutated storage.

| Mutation inside a live `each` | Legal when |
|---|---|
| in-place `addComponent<T>` (create-or-update overwrite) | `T` is declared `Write` by the query, or `T` is not listed at all |
| structural `addComponent` / `removeComponent` (archetype move) | both the source AND the target archetype are outside the matched set |
| `destroy(e)` | `e`'s archetype (0 if component-less) is outside the matched set |
| `clear()` | no matched archetype holds live rows |
| `create()` | always (touches no rows) |
| write through a `Write` reference; in-place overwrite of a `Write`-declared component | always (the intended mutation path) |
| a nested `each` | never |

The guard is live from the first callback to the last; after
`each` returns, the same structural mutation is legal again (the
suite pins both sides).

**Debug builds** assert (SIGABRT) on every violation — a loud
misuse crash, in the stale-handle precedent (CPP-012, S-9).
**Release builds** return `ErrorCode::InvalidArgument` from the
mutating call, log one rate-limited warn, and skip the mutation
(FR-12.3 degradation).

## Errors (FR-12.1, CORE-008)

| Violation | Release return | Event (subsystem `ecs`) |
|---|---|---|
| in-place write of a `Read`-declared queried component | `InvalidArgument` (2) from the mutating call | `iteration_write_during_read` |
| structural move / `destroy` touching a matched archetype | `InvalidArgument` (2) from the mutating call | `iteration_mutation` |
| `clear()` with live rows in a matched archetype | `InvalidArgument` (2) from `clear` | `iteration_clear` |
| a nested `each` | `InvalidArgument` (2) from the nested `each` | `iteration_nested` |

No new `ErrorCode` values: the registry is stable and additive-only,
and all iteration-legality failures reuse `InvalidArgument` (the
stale-handle precedent). Every event carries the correlating fields:
`entity_id`, `generation`, plus `component_id` (write case) and
`archetype_id` / `source_archetype` / `target_archetype` / `rows`
(structural cases) — LOG-002.

Repeats are rate-limited per (subsystem, event, severity) with the
suppressed-count summary on shutdown (LOG-004); the suite pins
warn-once + the `rate_limited` drain (release builds — in debug the
assert fires before the log).

## Performance (DOC-004)

- **Hot path:** the archetype scan is O(kMaxArchetypes × N) with
  N ≤ 32 listed components (N `columnIndexOf` probes per archetype,
  each a ≤ 32-entry lexicographic scan); visits are one cache-line
  stride per row. **No heap allocation**: the iteration state is
  stack-scoped and bounded — `ids[N]`, `cols[N]`,
  `matchedIds[kMaxArchetypes]`, and two 256-bit guard sets (PERF-003,
  PERF-006: no `std::function`, no hash map, no lock in the loop —
  `fn` is a template parameter, inlined).
- **Zero-alloc evidence (CORE-001):** `QueryZeroAlloc` runs 10k
  entities × `{Pos, Vel}` through a Read/Write pass (a legal in-place
  write per visit) and a Read/Read pass in a reset allocation-counter
  window (non-sanitizer trees): zero process-wide heap allocations,
  zero reservation delta (pool-steady), and prints the machine-
  greppable `query-iteration <stats>` lines to the ctest output.
  The sanitizer trees cover the same loop leak-free; M1-ALLOC-01
  lands the standing assertion.
- **Measured baseline (g++ 16.2.1, 2026-09, single-threaded
  headless, Debug tree):** 10k visits × 2 passes ≈ 0.88 ms total
  (≈ 0.044 µs/visit, `-O0`) — the M1-BENCH-01 tick baseline input;
  numbers are machine-dependent, the *shape* (flat, no spike, no
  allocation) is the tested property.
- **Traps:**
  - The visit order is not sorted by component values and is not
    the entity-creation order — the archetype-id/slot-id scheme is
    the contract (M1-ECS-05 documents it; deterministic replay is
    what M1-ECS-05 builds on).
  - A query over many archetypes pays the O(256 × N) scan even when
    few rows match — M1 keeps N small by design; the per-tick
    system pattern is few components, many entities (API-002).
  - The callback runs synchronously on the owner thread with the
    guard live: the legal-mutation table above is the whole
    reentrancy contract (API-005) — everything else asserts in
    debug.

## Threading and determinism

- **Single owner thread** (CONC-001); `each` is not thread-safe
  (M1 is single-threaded simulation — PRD §10.2).
- **Determinism (ARCH-010):** the visit order is a pure function of
  the world state (archetype creation order × slot order); the guard
  sets are membership-only and never iterated; no floating point, no
  platform intrinsics. The same operation sequence produces
  bit-identical visit sequences on every platform.

## Usage (performant pattern)

```cpp
// Setup (once): register the component types (M1-ECS-02).
ASSERT(world.registerComponent<ArchPos>().ok());
ASSERT(world.registerComponent<ArchVel>().ok());

// Per tick: iterate every {ArchPos, ArchVel} entity and update.
// Read/Read — a pure read pass (the guard rejects any write).
world.each<ArchPos, ArchVel>(
    [&](laige::Entity e, const ArchPos& p, const ArchVel& v) {
      /* read p, v — no mutation allowed while this runs */
    },
    laige::Read{}, laige::Read{});

// Per tick: integrate (Write-declared component: the reference store
// and an in-place addComponent overwrite are the intended paths).
world.each<ArchPos, ArchVel>(
    [&](laige::Entity e, const ArchPos& p, ArchVel& v) {
      v.v += static_cast<std::int64_t>(p.x);  // legal: Write tag
    },
    laige::Read{}, laige::Write{});

// Empty query: every live entity, ascending slot order.
world.each<>([](laige::Entity e) { /* e.g. despawn sweep bookkeeping */ });
```

## Misuse warnings

- Writing through a `Read` reference compiles only with a cast —
  that cast is a bug the debug guard will assert on (and the release
  guard will skip) if it reaches the storage through a World call;
  declare the component `Write` instead.
- Moving, destroying, or clearing an entity of a matched archetype
  from inside the callback invalidates the iteration the engine is
  mid-way through: it is rejected by design (assert/skip) — defer
  such mutations to an explicit despawn phase (API-004).
- The nested `each` is rejected because the outer guard's state would
  be clobbered by the inner scan; flatten the work into one query.
- `each` returns `Status`: the empty/valid iteration returns ok —
  the error path is the guard's rejection of a nested iteration only
  (a callback's own mutating call surfaces its own `Status`).
- Holding references from a callback past the callback's end is
  dangling once any mutation moves rows — copy out what you need
  (the row storage is the same SoA columns `get<T>` exposes).

## Roadmap context

- **M1-ECS-01/02/03 (done):** the entity handle, the component
  registry, and the archetype SoA columns this API iterates —
  [entity.md](entity.md),
  [component_registry.md](component_registry.md),
  [archetype.md](archetype.md).
- **M1-ECS-04 (this step):** the query API + iteration legality
  above.
- **M1-ECS-05:** the deterministic iteration contract over the visit
  order pinned by this step (replay/hash tests at the promised scope,
  ARCH-010).
- **M1-SYS-01/02:** the system loop and `SystemContext` — the
  `ctx.each` of the PRD sketch delegates to `World::each` (one
  world, one owner thread).
- **M1-ALLOC-01:** the standing zero-allocation assertion over the
  window this step measured.
- **M1-BENCH-01:** the tick budget uses the measured visit cost above
  as the iteration baseline.
