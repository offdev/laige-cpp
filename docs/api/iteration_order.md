# Deterministic iteration order (`World::each` visit contract)

The deterministic iteration contract (M1-ECS-05; PRD §10.3, ARCH-010,
AGENTS PERF-006, CORE-001). This document pins the *visit order* of
the query API — the order in which `World::each<T1, T2, ...>(fn,
tags...)` invokes `fn` — and proves, by property test, that the order
is a **pure function of the world state**, never of the operation
history that produced it. The API itself is [query.md](query.md);
the storage whose rows are visited is
[archetype.md](archetype.md).

Deterministic replays (M1-DET-02/04) and bit-exact state hashes
(M1-DET-03) build directly on this contract: two runs that reach the
same state must visit the same rows in the same order, on every
platform, or the replay and hash machinery have nothing to pin.

## The contract

`World::each<T1, ..., TN>` (superset match; unregistered types match
nothing — [query.md](query.md)) visits its matches in exactly this
order:

1. **Archetypes in ascending archetype id.** The id is assigned
   dense from 1, in the order a component set is **first seen** by a
   component mutation (`addComponent`/`removeComponent` moving an
   entity into a set not yet stored) — i.e. creation order, *not*
   the component-type registration order. A set seen first at tick 0
   has a smaller id than a set seen first at tick 1, regardless of
   how many entities ever carry it. (Archetypes are never destroyed;
   an empty archetype keeps its id and simply contributes no rows.)
2. **Entities within an archetype in ascending entity slot id** —
   the dense-id order of the row storage (below).
3. **The empty query** (`each<>`) iterates no archetype rows: it
   visits every live entity (component-less ones included) in
   ascending slot id, with no archetype grouping.

Concretely, for a query `Q`, the visit sequence is:

```text
for each archetype a in ascending id, with Q ⊆ set(a):
    for each live entity e in a, in ascending slot id:
        fn(e, <component references in template order>)
```

The order depends only on: which slots are live, which archetype each
live slot is in, and the archetype id assignment. Nothing else — no
creation order of the entities, no operation history, no per-process
addresses, no randomness.

## The dense-id order scheme (row order under moves)

Rows of every archetype are kept in **ascending slot-id order**
(archetype.h, invariant I2): `slotCol_a[r]` is strictly ascending in
`r`. The scheme survives every move the storage supports:

- **insertion** (an entity entering the archetype by
  `addComponent`, or by `removeComponent` into a smaller set): the
  row is binary-searched into the ascending slot column and the tail
  is `memmove`d — the entity keeps its slot id and lands exactly where
  the ascending order requires;
- **removal** (an entity leaving by `removeComponent` to a smaller
  set, by `destroy`, or by `clear`): the row is shifted out and the
  tail moves left — every remaining row keeps its slot id and its
  relative order;
- **in-place `addComponent`** (the component is already present): no
  row movement at all.

Because a row's position is always the rank of its slot id among the
archetype's live slots, the row layout after any sequence of
moves is the same layout the final state alone determines. Two
histories that converge on the same live slots, archetype
assignment, and archetype id assignment therefore produce
bit-identical visit sequences — the convergence property pinned by
the `iter_order` suite below.

## No unordered containers in the iteration path

PRD §10.3 bans unordered containers in sim hot paths ("hash tables
use deterministic hash + fixed iteration, or are banned in sim").
The iteration path touches exactly:

| Structure | Access in the visit path | Determinism |
|---|---|---|
| the archetype table (fixed 256 records) | scanned in ascending table (= id) order before the first callback | pure table order |
| each archetype's packed `slotCol` + component columns | one sequential walk, ascending slot order | stored-data order (the dense-id scheme) |
| the per-slot record tables (`archetypeOf_`, `rowOf_`) | direct index by the 16-bit slot id | pure index |
| the iteration-guard sets (`detail::IdSet256`) | membership tests only — **never iterated** | fixed 4 × 64-bit words |

The **entity→archetype map** the step anticipated as "the one
internal hash structure" is *not a hash at all*: it is the per-slot
direct index above (archetype.h) — one table load, no hash, nothing
unordered to iterate. That is strictly stronger than the allowance.

The **only hash structure in `laige-sim`** is the component type-key
index (component.h): an open-addressing table mapping a
compile-time type-identity token to the world's `ComponentTypeId`,
hashed by a deterministic `splitmix64` of the token's address value.
It is **never iterated** — lookup and insert only, correctness rests
on key equality — so its per-process layout (the address input is
ASLR-dependent) is not observable and contributes nothing to the
visit order. It is a setup-path structure
(`registerComponent`), not part of any tick.

The structural ban is verified here by inspection and will be
enforced in CI by the M1-DET-01 sim source scan; the observable
consequence — order purity — is what the suite pins at runtime.

## Convergence property and its test

**Property:** two worlds that converge on the same final state —
same live slots, same entity→slot (id) assignment, same archetype
assignment, same archetype id assignment, same component values —
iterate identically: every query returns the same visit sequence
(handles and component values) in both worlds.

`tests/laige-sim/iter_order_tests.cpp` (CTest `iter_order`; suites
`IterOrder.*`) pins it with a fixed PRNG seed (docs/testing.md §4 —
`TestPrng` substreams 1005-1007; override via `LAIGE_TEST_SEED`):

- **Scenario:** a 20-slot world, 16 logical entities (entity `i`
  takes slot `19-i` — the LIFO free list), dead set `i % 5 == 2`,
  three component types registered A, B, C. Every entity runs a
  component script that moves it between the archetypes
  `{A} → {A,B} → {A,B,C} → …` (adds of absent components, removes of
  present ones — rows churn between archetypes on every step); one
  live entity ends component-less.
- **Sequence A (serial):** create all 16; every entity runs its
  script in turn (the dead ones hold their rows until the final
  destroy); the dead entities are destroyed last, in index order.
- **Sequence B (interleaved):** the *same* create phase — the LIFO
  free list requires it, and it is what pins the identical
  entity→id assignment (an earlier dead-slot destroy would have
  pushed the slot back on top of the free list). The difference is
  *when* the dead destroys happen (PRNG-assigned round boundaries,
  scattered through the component phase instead of at its end), the
  component steps run **round-robin** over the live entities in
  per-round PRNG permutations, and PRNG-placed component-less
  scratch create/destroy pairs ride the round boundaries.
- **Oracle:** an independent model recomputes, per sequence, the
  final state (free-list simulation + per-entity sets/values) and
  the first-seen archetype order, and derives the expected visit
  sequences for five queries: `each<>`, `each<A>`, `each<A,B>`,
  `each<B>`, `each<C>`.
- **Assertions:** world A's and world B's observed visits equal
  each other *and* the oracle, per visit (slot, generation, and each
  queried component's value); the entity→slot assignment matches the
  oracle for every live entity; archetype 1's visit group is a
  strictly ascending-slot prefix, archetype 2's a strictly
  ascending suffix, and the boundary is *not* a global slot order
  (archetype 2 opens at slot 4, below archetype 1's top slot 19 —
  so the test distinguishes archetype order from a globally
  slot-sorted walk).

The only state the two sequences may differ in is bookkeeping the
iteration cannot observe: the churn counters, the free-list *order*
of the pushed dead slots (the slot a hypothetical *next* `create()`
would pop), and the generation counters of never-created slots a
scratch pair pops (dead slots, handle-generation bookkeeping). The
suite's machine-greppable `iter-order … fnv1a=0x…` line records the
seed, the PRNG-placed pair count, the visit counts, and the hash of
the visit sequences — byte-identical across CI runs, compiler trees,
and scenario instantiations (verified: g++/Clang/ASan/release,
default and overridden seeds).

A committed known-answer test (`IterOrder.DeterministicVisitOrderKAT`)
pins the exact visit sequences of the degenerate interleaving
(no PRNG draws), so a change to the order contract fails the KAT
before it can reach the replay machinery.

## Determinism scope (ARCH-010)

The visit order is pure integer ordering over fixed-width ids; no
floats, addresses, wall clocks, or platform intrinsics enter it. The
scope the contract promises is therefore the widest ARCH-010 allows:
**the same state visited identically on every supported platform,
architecture, and compiler** (same build). The state *hash* that
M1-DET-03 computes over the visit order inherits this scope once the
component-value encoding is pinned there.

## Performance (DOC-004)

- **Cost:** the matched-archetype scan is O(256 × N) over the fixed
  archetype table (N = the number of queried components) and finishes
  before the first callback; the visit itself is one O(1) column
  index per reference — O(1) amortized per entity, no tail work on
  the read path.
- **Allocation:** none in the query path (M1-ECS-04's 10k-entity
  window proves zero heap: the matched-id array, the column index
  array, and the guard sets are all stack-scoped fixed arrays).
- **Budget:** the measured 10k-visit baseline lives in
  [query.md](query.md#performance-doc-004) (≈ 0.044 µs/visit, g++
  16.2.1 Debug, `-O0`; the flat shape is the tested property).
- **Traps:**
  - Do not assume the visit order is creation order or a global
    slot order — archetype groups interleave in slot space by
    design (the KAT's boundary above). Systems that need a global
    order iterate `each<>` and index, or keep their own ordered
    structure (game policy, M1-SYS-01+).
  - Do not `std::sort` a captured visit list to "normalize" order
    for comparison across runs — the order *is* the contract;
    re-sorting hides a regression the replay hash exists to catch.
  - A query over many archetypes pays the O(256 × N) scan even when
    few rows match (the M1-ECS-04 trap); keep per-tick queries small.

## Roadmap context

- **M1-ECS-01/02/03/04 (done):** the entity handle, the component
  registry, the archetype rows, and the query API this contract
  governs — [entity.md](entity.md),
  [component_registry.md](component_registry.md),
  [archetype.md](archetype.md), [query.md](query.md).
- **M1-ECS-05 (this step):** the contract above + the convergence
  property test.
- **M1-DET-02/03/04:** the replay header, the state hash (computed in
  visit order), and the bit-exact CI replay build on this contract.
- **M1-SYS-01/02:** systems iterate through `SystemContext::each`
  (a delegate to `World::each`), so they inherit this order; system
  I/O declarations (M1-SYS-01) reuse the `Read`/`Write` tags.
