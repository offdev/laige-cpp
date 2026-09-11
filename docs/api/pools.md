# Memory pools (`laige::ArenaPool<T>`, `laige::Pool<T>`)

Budgeted, accounted, engine-owned memory pools (M0-CORE-05; PRD §10.4
memory model, §9.1 S-2, AGENTS PERF-003, CPP-002/007). Public header:
`src/laige-core/include/laige/pools.h` (header-only). Unit suite:
`ctest -R pools` (`tests/laige-core/pools_tests.cpp`).

The safe API has **no per-frame heap** (S-2): engine data lives in these
pools. A safe-API call that would allocate is either pooled internally or
refuses with `ErrorCode::BudgetExhausted` — a pool never grows silently
(G-R1).

## The two kinds

| | `ArenaPool<T>` | `Pool<T>` |
|---|---|---|
| Lifetime | Arena-scoped: all elements live until `reset()` (the per-frame release) | Element-scoped: created/destroyed individually, in any order |
| Addressing | Slot index (`std::uint32_t`), valid until the next `reset()` | Stable handle = index + generation (`PoolHandle`, CPP-007) |
| Recycle | None in-frame (bump cursor; `reset()` rewinds) | LIFO free list; freed slots recycle immediately |
| Release | `reset()` — O(inUse), idempotent | `destroy(handle)` — O(1); `clear()` — O(capacity) |
| Use for | Per-frame scratch (contacts, temp vectors, frame-local buffers) | Long-lived / cross-frame objects (entities, components, particles, audio sources, packets) |

## Common contracts

**Budget (S-6, PERF-008, SCALE-003).** The element capacity is fixed at
construction via `Options{capacity}` and is a *declared budget*: raising
it is a typed configuration change (API-006), never runtime behavior. A
`create()` beyond the budget returns `ErrorCode::BudgetExhausted` (no
growth, no crash, no silent degradation). A budget of 0 is legal: every
`create()` fails.

**Allocation (PERF-002, PERF-003).** Construction performs the pool's
only backing allocation (a setup path, never a hot path). Every later
`create()`/`destroy()`/`reset()`/`clear()` is O(1) (LIFO free list for
`Pool`), allocates nothing, does no I/O, and takes no locks. The only
real work of a `create()` is the element's own constructor (placement
new). T must not throw from its constructor (NFR-8.10: the engine builds
with `-fno-exceptions`).

**Accounting (PRD §10.4, FR-11.4, G-R4).** `stats()` returns a
`PoolStats` value — element counts (`capacity`, `inUse`, `peakInUse`),
the `totalCreated` churn counter, and backing-store byte counts. The
pool *publishes*; the M1 profiler (FR-11.4 memory inspector) *pulls* and
aggregates — there is no registration or callback (CORE-004: the sink
interface lands with the profiler, not before it). `peakInUse` is the
high-water mark since construction (FR-11.4 peak tracking);
`totalCreated` counts churn, not liveness (G-R4: a create immediately
destroyed still counts).

**Errors (FR-12.1, CORE-008).**

| Operation | Failure | Code |
|---|---|---|
| `create()` | budget exhausted | `ErrorCode::BudgetExhausted` (4) |
| `Pool::destroy()` | invalid or stale handle | `ErrorCode::InvalidArgument` (2) |

Failures are `Result`/`Status` values — never exceptions, never silent.
The pool itself does not log: a failing `create()` stays a branch on the
cold path, and the owning system logs the failure under its own
subsystem name (LOG-002, G-R1 "pool overflow → logged degradation").

**Ownership (CPP-002, CONC-001).** A pool owns its elements and its
backing store. It is never copyable; it is movable (O(1) pointer swap)
and its destructor destroys every live element — no leak, ASan-verified
by the suite. A moved-from pool is a valid *empty* pool (capacity 0:
every `create()` fails, every handle invalid). Move assignment first
destroys the elements the destination currently owns, then takes over
the source's storage; the stats follow the storage (the moved-from pool
is zeroed).

**Threading.** A pool has exactly **one owner thread** — it is NOT
thread-safe (CONC-001). Use it from one owner, typically one system's
update phase. Sharing across threads requires an explicit engine
synchronization boundary (CONC-002), which M1 defines per subsystem;
`stats()` is a plain value read that is safe once the owning phase has
ended.

**Determinism (ARCH-010, PRD §10.3).** Pool allocation is pure integer
bookkeeping — no floating point, no platform intrinsics, no randomness.
The same create/destroy sequence produces bit-identical handle
sequences on every platform, so handles are safe to replicate and are
replay state from M1 on. (The generation-wrap case below is defined
unsigned wrap, hence bit-identical too.)

## `laige::ArenaPool<T>`

```cpp
laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{128});
auto s = pool.create(args...);   // Result<uint32_t, ErrorCode>; slot, valid until reset()
Tracked& e = pool.at(s.value()); // asserts live (debug); undefined in release if stale
Tracked* p = pool.get(3);        // nullptr if slot >= inUse
pool.reset();                    // per-frame release: destroy all, rewind cursor
```

- `create(Args...)` — `Result<std::uint32_t, ErrorCode>`; the slot index,
  or `BudgetExhausted`. O(1), no allocation.
- `reset()` — destroy every live element, rewind the cursor. O(inUse),
  idempotent. `peakInUse`/`totalCreated` survive the reset
  (since-construction counters; a per-frame profiler diffs them).
- `at(slot)` — `T&`; asserts the slot is live (debug: loud crash;
  release: undefined — the engine Result convention).
- `get(slot)` — `T*`; null-safe read (nullptr when the slot is not
  live).
- `isValid(slot)` — true while the slot is live.
- `capacity()`, `inUse()`, `stats()` — O(1), no allocation.

**Slot lifetime:** a slot is valid until the next `reset()` — never
carry it across frames, and there is no per-element release in an arena
(`reset()` is the only release; that is the arena's contract).

## `laige::Pool<T>`

```cpp
laige::Pool<Entity> pool(laige::Pool<Entity>::Options{4096});
auto h = pool.create(args...);        // Result<PoolHandle, ErrorCode>
Entity& e = pool.at(h.value());        // asserts live (debug); undefined in release if stale
Entity* p = pool.get(h.value());       // nullptr when stale
Status r = pool.destroy(h.value());    // InvalidArgument when stale/invalid
pool.clear();                          // destroy all; every handle becomes stale
```

- `create(Args...)` — `Result<PoolHandle, ErrorCode>`; O(1), no
  allocation.
- `destroy(handle)` — `Status`; O(1). Bumps the slot's generation (the
  stale-handle check), returns the slot to the LIFO free list.
- `at(handle)` — `T&`; asserts the handle is live (debug: loud crash;
  release: undefined).
- `get(handle)` — `T*`; null-safe read (nullptr when stale).
- `isValid(handle)` — generation-checked liveness (CPP-007).
- `clear()` — destroy every live element; every handle becomes stale;
  the capacity is unchanged and the pool is immediately reusable.
  O(capacity). (Reset-per-frame workloads should use `ArenaPool<T>` —
  its `reset()` is O(inUse) and has no free-list bookkeeping.)
- `capacity()`, `inUse()`, `stats()` — O(1), no allocation.

### The handle contract (CPP-007)

`PoolHandle{index, generation}` is a 64-bit pair: a 32-bit slot index
and a 32-bit generation. A handle is valid while the slot is live and
its current generation matches the handle's:

- a slot starts at generation 1 (generation 0 is reserved — the default
  `PoolHandle{}` is never valid);
- `destroy()`/`clear()` bump the slot's generation, so every stale
  handle to that slot fails `isValid()` and can never pass again —
  except after 2^32 frees of that one slot (defined unsigned wrap,
  effectively unreachable; the one case the scheme does not rule out,
  documented rather than assertable);
- a handle is meaningful only in the pool that produced it:
  index+generation pairs from different pools are not comparable.

**Stale-handle detection is the debug guarantee (S-9):** `at()` asserts
in debug builds (the suite proves the assert fires, in a forked child);
in every build, `isValid()` queries and `get()` reads null-safely.
Release behavior of `at()` on a stale handle is undefined, per the
engine Result convention (result.h).

## `PoolStats`

```cpp
struct PoolStats {
  std::uint32_t capacity{};     // element budget (Options::capacity)
  std::uint32_t inUse{};        // live elements right now
  std::uint32_t peakInUse{};    // high-water mark of inUse since construction
  std::uint64_t totalCreated{}; // successful create() calls since construction
  std::size_t bytesCapacity{};  // backing store bytes (element slots + bookkeeping)
  std::size_t bytesInUse{};     // element bytes occupied by live elements
};
```

`bytesCapacity` is the whole backing store: for `ArenaPool<T>` the
element block (`capacity * stride`); for `Pool<T>` the element block plus
per-slot bookkeeping (4 B generation + 1 B alive flag + 4 B free-list
entry). `stride` is the aligned element size —
`sizeof(ElementSlot<T>)` = the size of `T` rounded up to its alignment
(1 byte for an empty T). `bytesInUse` is the element footprint of the
live elements only.

## Performance

- **Complexity:** every hot-path operation is O(1) except
  `Pool::clear()` (O(capacity)) and `ArenaPool::reset()` (O(inUse)) —
  both are per-frame releases, budgeted by construction (PERF-008: the
  work is bounded by the declared budget, never an arbitrary backlog).
- **Allocations:** none after construction (PERF-003). No std::function,
  no virtual dispatch, no hash maps, no locks (PERF-006).
- **Data layout:** one contiguous aligned block per pool (PERF-004);
  elements are placed densely; the LIFO free list keeps recycling
  cache-local.
- **Common traps:**
  - treating a `PoolHandle` as stable after `destroy()`/`clear()`
    (use-after-free — check `isValid()`, or read via `get()`);
  - carrying an `ArenaPool` slot across `reset()`;
  - holding two pools' handles in the same container (cross-pool
    comparison is meaningless);
  - expecting a pool to grow when `BudgetExhausted` arrives (it will
    never — raise the budget through configuration instead).

## Performant example (per-frame contacts + cross-frame entities)

```cpp
// Per-frame scratch: one arena, reset at the end of the frame.
laige::ArenaPool<Contact> contacts(laige::ArenaPool<Contact>::Options{512});
for (auto const& pair : pairsThisFrame) {
  auto r = contacts.create(pair.a, pair.b);
  if (r.isError()) { /* log under your subsystem name (G-R1); drop the pair */ }
  resolve(contacts.at(r.value()));
}
// ... at frame end:
contacts.reset();

// Cross-frame: stable handles, generation-checked.
laige::Pool<Entity> entities(laige::Pool<Entity>::Options{8192});
auto h = entities.create(spawnArgs...);
if (h.isError()) { /* BudgetExhausted: log, refuse the spawn (S-2) */ }
// later, when the entity dies:
entities.destroy(h.value());   // h is stale from now on
```

## Misuse warnings

- A stale `PoolHandle` through `at()` is a debug assert and release UB —
  exactly the use-after-free the handle scheme exists to make
  detectable (S-9).
- `ArenaPool<T>` slots are frame-local; cross-frame data belongs in
  `Pool<T>`.
- A pool's `BudgetExhausted` is a *declared budget being exceeded* —
  surface it (log + refuse), never "fix" it by letting the pool grow.
