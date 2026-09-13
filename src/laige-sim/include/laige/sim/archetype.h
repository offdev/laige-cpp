// laige-sim archetype SoA component storage (M1-ECS-03).
//
// PRD §9.1 S-1/S-2 and FR-1.2: the ECS core is archetype/SoA storage —
// dense entity iteration, O(1) component access, component add/remove
// through pool-backed (never per-operation heap) moves. This header
// ships the storage's public types and constants; the World methods
// that use them (has/get/addComponent/removeComponent/archetypeStats)
// are declared in entity.h and defined there (the M1-ECS-02 pattern:
// public types in the subsystem header, World methods in its home).
//
// ---------------------------------------------------------------------------
// Storage layout
// ---------------------------------------------------------------------------
//
//   Archetype = an ordered set of component types: the component type
//               ids sorted ascending (ids are registration-order dense
//               from 1, so this is the canonical, unique order for the
//               set). Every entity belongs to exactly one archetype —
//               the one matching its component set — or to none (an
//               entity with no components is in no archetype).
//
//   SoA columns: each archetype carries one contiguous column per
//               component — `rowCapacity` packed rows of `sizeof(T)`
//               bytes aligned to alignof(T) — plus a slot column
//               (`rowCapacity` entity slot ids). Row i of every column
//               is one entity's data; rows are packed with no padding
//               between elements (PERF-004: contiguous, compact).
//
//   Row order (the dense-id-order scheme, pinned by M1-ECS-05): the
//               rows of every archetype are kept in ascending entity
//               slot-id order. The row layout is therefore a pure
//               function of the world state (which slots are live and
//               which archetype each is in) — never of the operation
//               history that produced it. Worlds that converge on the
//               same state iterate identically, including after
//               component moves (the property test M1-ECS-05 pins).
//
//   entity -> archetype map: the per-slot record — `archetypeOf[slot]`
//               (0 = no archetype) and `rowOf[slot]` (the dense row) —
//               directly indexed by the 16-bit slot id. No hash table
//               is needed: a slot's archetype is one table load.
//               (M1-ECS-05's "one internal hash structure" allowance
//               is unused by design: direct indexing is strictly
//               stronger — no hash, nothing unordered to iterate.)
//
// Storage invariants (maintained by attachSlot/removeRow; the test
// suite pins their observable consequences, and ASan covers the
// memory side):
//   I1  archetypeOf_[s] == 0  <=>  the slot's entity has no
//       components; otherwise it names exactly one live archetype.
//   I2  For every live row r of archetype a: slotCol_a[r] is a live
//       slot, archetypeOf_[slotCol_a[r]] == a, and rowOf_[slotCol_a[r]]
//       == r. The slot column is strictly ascending, so the row
//       layout is the ascending slot-id order (the dense-id-order
//       scheme above).
//   I3  The component columns of every live row hold valid component
//       values (written by the moving operation before the row
//       becomes visible).
//   I4  archetype.size == the number of live slots with
//       archetypeOf_ == the archetype's id; rowsLive across all
//       archetypes == the number of entities with components.
//
// ---------------------------------------------------------------------------
// Budgets and the reserve policy (CORE-005, PERF-003)
// ---------------------------------------------------------------------------
//
//   kMaxArchetypes              256  distinct component sets per world.
//                               A game with more distinct component
//                               combinations exceeds the M1 bound —
//                               raise it through an ADR, not a knob.
//   kMaxArchetypeComponents     32   components per entity (per
//                               archetype). A 33rd addComponent fails
//                               with BudgetExhausted.
//   kInitialArchetypeRows       16   rows reserved when an archetype is
//                               created (or the world capacity, when
//                               smaller).
//
// Reserve policy: an archetype's columns are reserved for `rowCapacity`
// rows, and rowCapacity only ever grows, in bounded chunks: when a row
// must be attached to a full archetype, that archetype reserves exactly
// double its rows (capped at the world's entity capacity) in one
// reservation per column and moves its rows into the new blocks. The
// number of growth events over a world's lifetime is therefore bounded
// per archetype by log2(worldCapacity / kInitialArchetypeRows) + 1, and
// the total reserved rows across all archetypes stays within
// 2 × live rows + kMaxArchetypes × kInitialArchetypeRows (each live
// entity holds exactly one row). Growth is a bounded, accounted, logged
// event — never a per-operation allocation: steady-state add/remove
// moves only ever memcpy within already-reserved blocks (the M1
// zero-allocation property; M1-ALLOC-01's assertion hooks these counters
// later, and ASan + archetypeStats() is the check until then).
//
// ---------------------------------------------------------------------------
// Allocation, complexity, determinism
// ---------------------------------------------------------------------------
//
// Construction (World::create) performs the storage's backing
// allocations: the per-slot record arrays (archetypeOf_, rowOf_), the
// archetype table (kMaxArchetypes records), and the type-key index
// (kComponentKeyIndexSize slots). First creation of an archetype and
// each growth event reserve column blocks (the only other allocations,
// both bounded and accounted — see the reserve policy).
//
//   has<T>(e)           O(1) entity-count: a few loads plus the
//                       type-key lookup (O(1) open-addressing, never
//                       iterated) and the column binary search
//                       (O(log kMaxArchetypeComponents)). No warn, no
//                       side effects (a pure query, like isValid).
//   get<T>(e)           O(1) as has<T> plus one dereference. Stale
//                       handle: nullptr after the rate-limited
//                       warn-once of check() (every build).
//   addComponent<T>(e)  O(1) in the entity count for the bookkeeping,
//                       O((tail rows) × row-stride) bytes moved when the
//                       entity changes archetype (the slot-ordered
//                       insertion shifts the tail right in every
//                       column); no allocation in steady state (growth
//                       events as above).
//   removeComponent<T>  as addComponent, shifting the tail left.
//
// All bookkeeping is pure integer arithmetic over pre-reserved blocks
// (PERF-004/005: contiguous, compact, no pointer chasing beyond one
// level). Determinism (ARCH-010): the same operation sequence produces
// bit-identical archetype tables, row layouts, and counter values on
// every platform — no floating point, no randomness, no addresses, no
// unordered iteration anywhere in the storage.
//
// ---------------------------------------------------------------------------
// Errors (FR-12.1, CORE-008 — never silent)
// ---------------------------------------------------------------------------
//
//   stale/invalid handle in add/remove/get
//                              -> ErrorCode::InvalidArgument + one
//                                 rate-limited warn (ecs/stale_entity_
//                                 access, via check(); every build)
//   T not registered in this world
//                              -> ErrorCode::InvalidArgument + warn
//                                 (ecs/component_unregistered)
//   the entity would exceed kMaxArchetypeComponents
//                              -> ErrorCode::BudgetExhausted + warn
//                                 (ecs/component_limit)
//   more than kMaxArchetypes distinct sets
//                              -> ErrorCode::BudgetExhausted + warn
//                                 (ecs/archetype_budget)
//   removeComponent<T> when the entity lacks T
//                              -> ok Status, no-op (a normal state, not
//                                 an error — despawn cleanup)
//   addComponent<T> when the entity has T
//                              -> ok Status, the value is overwritten
//                                 in place (create-or-update; the
//                                 archetype does not change)
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - get<T>(e) returns nullptr for three different reasons (stale
//     handle, T unregistered, entity lacks T): use has<T>() to branch,
//     and isValid(e) to tell "stale" from "lacks" when it matters.
//   - addComponent/removeComponent are per-entity mutations (the spawn/
//     despawn path). Sustained per-frame churn is what the G-R4
//     guardrail (M1-ECS-06) warns about — batch entity lifecycle in a
//     spawn/despawn system.
//   - An archetype is never destroyed once created (M1 keeps them
//     alive; empty archetypes stay until clear()/destruction of the
//     world). The kMaxArchetypes budget therefore counts sets ever
//     seen, not sets currently occupied.
//   - A component's alignof must be <= kArchetypeColumnAlignment
//     (32) — the column blocks are aligned to 32 bytes (the P0 targets'
//     maximum fundamental alignment). registerComponent<T>()
//     static-asserts this (M1-ECS-03's SoA layout bound).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace laige {

// The engine-level cap on distinct component sets (archetypes) per
// world (CORE-005: a named engine constant, the kMaxComponentTypes
// precedent — a game's component-set vocabulary is orders of magnitude
// smaller than its entity count; raising it is an ADR, not a knob).
inline constexpr std::uint32_t kMaxArchetypes = 256;

// The engine-level cap on components per entity (per archetype)
// (CORE-005). A 33rd distinct component on one entity fails
// addComponent with BudgetExhausted.
inline constexpr std::uint32_t kMaxArchetypeComponents = 32;

// Rows reserved when an archetype is created (or the world's entity
// capacity, when smaller). Growth doubles from here (see the header
// preamble, "Reserve policy").
inline constexpr std::uint32_t kInitialArchetypeRows = 16;

// Column blocks are aligned to this many bytes: the P0 targets'
// maximum fundamental alignment (x86-64/arm64), so every trivially
// copyable component type's alignof fits (registerComponent static-
// asserts alignof(T) <= kArchetypeColumnAlignment).
inline constexpr std::uint32_t kArchetypeColumnAlignment = 32;
// Over-allocation that keeps the aligned base inside the raw block.
inline constexpr std::size_t kArchetypeColumnPad =
    kArchetypeColumnAlignment - 1;

// The "column absent" sentinel for columnIndexOf results (API-008:
// call sites never spell raw 0xFFFFFFFF).
inline constexpr std::uint32_t kInvalidColumnIndex = 0xFFFFFFFFu;

// The "row absent" sentinel for attachSlot failure results. Rows are
// 0-based and row 0 is a valid row, so 0 cannot be the failure value
// (API-008: call sites never spell raw 0xFFFFFFFF).
inline constexpr std::uint32_t kInvalidRowIndex = 0xFFFFFFFFu;

// One archetype's storage accounting snapshot (PRD §10.4, FR-11.4,
// G-R4 feed; mirrors the M0-CORE-05 PoolStats shape). A plain value
// the M1 profiler (M1-PROF-01) and the churn/overflow checks pull:
//
//   archetypeCount        archetypes created so far (0 .. kMaxArchetypes)
//   rowsLive              entities in an archetype right now
//   rowsReserved          rows reserved across all archetypes
//   bytesReserved         reserved row bytes (slot column + component
//                         columns; alignment padding not counted)
//   totalAdds             successful addComponent calls (including
//                         in-place overwrites; destroy/clear detaches
//                         are not counted)
//   totalRemoves          successful removeComponent calls that
//                         detached a row (no-op removes not counted;
//                         destroy/clear detaches not counted)
//   totalArchetypeGrowth  growth events (bounded per the reserve
//                         policy)
//   totalReservations     column block reservations since construction
//                         (archetype creation + growth) — the pool
//                         accounting the zero-allocation property reads
struct ArchetypeStats {
  std::uint32_t archetypeCount{};
  std::uint32_t rowsLive{};
  std::uint32_t rowsReserved{};
  std::uint64_t bytesReserved{};
  std::uint64_t totalAdds{};
  std::uint64_t totalRemoves{};
  std::uint64_t totalArchetypeGrowth{};
  std::uint64_t totalReservations{};
};

namespace detail {

// One SoA column of an archetype: a packed array of `rowCapacity` rows
// of `size` bytes each, aligned to kArchetypeColumnAlignment. The raw
// block is over-allocated by kArchetypeColumnPad bytes so the aligned
// `base` always fits inside it (the raw block owns the storage; `base`
// is a non-owning view). Row i sits at `base + i * size`, packed with
// no inter-element padding (PERF-004).
struct ArchetypeColumn {
  std::unique_ptr<std::byte[]> block;
  std::byte* base;
  std::uint32_t size;  // bytes per row (the component's sizeof(T))
};

// One archetype: the ordered component set (signature) plus the SoA
// columns. The World stores kMaxArchetypes of these, indexed by
// (archetype id - 1); an id is assigned when the set is first seen.
// `sig` is 0-terminated; ids are registration-order dense from 1, so
// 0 doubles as the terminator (no separate count is needed for the
// signature scan — sigCount is kept for O(1) reads).
struct ArchetypeRecord {
  std::uint32_t sig[kMaxArchetypeComponents];  // sorted component ids
  std::uint16_t sigCount{};                    // number of sig entries
  std::uint32_t fingerprint{};  // FNV-1a 32-bit over sig (short-circuit)
  std::uint32_t rowCapacity{};  // reserved rows (the reserve policy)
  std::uint32_t size{};         // live rows (== entities in this archetype)
  std::unique_ptr<std::uint16_t[]> slotCol;  // row -> entity slot id (ascending)
  std::unique_ptr<ArchetypeColumn[]> columns;  // sigCount columns, in sig order
};

// One slot of the per-world component type-key index: the open-addressing
// table that maps a component type's identity token
// (&ComponentTypeKey<T>::kMarker) to this world's ComponentTypeId. See
// the World::componentIdOfKey contract in entity.h for the lookup
// semantics (deterministic splitmix64 hash of the address value + linear
// probing; correctness rests on key equality only — the table is never
// iterated, so its per-process layout is not observable).
struct ComponentKeySlot {
  const void* key{};  // nullptr marks an empty slot
  std::uint32_t id{};  // this world's ComponentTypeId
};

// The type-key index size: a power of two keeping the load factor at or
// below 1/2 for kMaxComponentTypes entries (CORE-005).
inline constexpr std::uint32_t kComponentKeyIndexSize = 512;

}  // namespace detail

}  // namespace laige
