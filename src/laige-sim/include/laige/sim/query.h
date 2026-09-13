// laige-sim query API + iteration legality (M1-ECS-04).
//
// FR-1.2/FR-1.3 (declared access, iteration legality; AGENTS CORE-008):
// this header ships the public types of the per-tick iteration API:
//
//   Access       The declared per-component access of a query:
//                Read or Write (a value type; M1-SYS-01's system I/O
//                declarations reuse it).
//   Read / Write The per-component access tags — the access_flags of
//                World::each. The tag's TYPE carries the access, so the
//                callback's reference constness is a compile-time
//                property (API-008: the invalid state is
//                unrepresentable — a Read component comes out `const`
//                and cannot be written through without a cast).
//
//   World::each<T1, T2, ...>(fn, Read/Write tags...)
//                Declared in entity.h (the World home) and defined in
//                the same header (a template): iterates every entity
//                having ALL the listed components (superset match: an
//                archetype matches when every listed type is in its
//                set; extra components do not exclude it).
//
// The PRD Appendix B sketch calls this `ctx.each<...>()`; in M1 the
// query lives on the World that owns the storage, and M1-SYS-01's
// SystemContext delegates to it (one world, one owner thread). The
// sketch's argument order (access flags first, callable last) is not
// the final syntax: C++ cannot deduce a parameter pack that is not
// last, so the tags follow the callable — one tag per listed
// component, in template order, still declared per component.
//
// ---------------------------------------------------------------------------
// Query semantics
// ---------------------------------------------------------------------------
//
//   world.each<ArchPos, ArchVel>(fn, Read{}, Write{})
//
//   - `fn` is invoked once per matching entity, as
//     `fn(Entity, const ArchPos&, ArchVel&)`: one reference per listed
//     component, in template order — a `const T&` where the access is
//     Read, a `T&` where it is Write. The Entity handle is the
//     generation-checked live handle of the row's slot.
//   - Superset match: an entity with {ArchPos, ArchVel, ArchFlag} is
//     visited by `each<ArchPos, ArchVel>`; an entity with {ArchPos}
//     only is not.
//   - `each<>` (no components) visits every LIVE entity — including
//     component-less ones — in ascending slot-id order. It iterates
//     no archetype rows, so no component storage is under an
//     iterator: structural mutations stay legal under it (see below).
//   - A listed component that is not registered in this world matches
//     nothing: the iteration runs zero times and returns an ok Status
//     (a pure query, like has<T>() reading false).
//   - `each` returns a Status: ok on every completed iteration (a
//     zero-match iteration is ok), `ErrorCode::InvalidArgument` when
//     a nested iteration is rejected (below).
//
// ---------------------------------------------------------------------------
// Iteration order (M1-ECS-05 documents the contract)
// ---------------------------------------------------------------------------
//
//   Archetypes in ascending archetype-id order (= creation order,
//   the order component sets are first seen), and within an
//   archetype in ascending entity slot-id order (the slot-ordered row
//   scheme, archetype.h "Row order"). No unordered container is
//   touched: the archetype scan is the bounded fixed table in id
//   order, the guard sets are membership-only bit sets (never
//   iterated), and the row walk is the packed slot column. The order
//   is a pure function of the world state (which archetypes exist
//   and which slots are live in each), never of the operation
//   history — M1-ECS-05 pins the convergence property over it.
//
// ---------------------------------------------------------------------------
// Iteration legality (FR-1.3: the engine enforces it, FR-12.3)
// ---------------------------------------------------------------------------
//
// At most ONE iteration is active per world at a time (M1 is
// single-threaded; the guard is world state on the owner thread).
// While `each` is active, the world records:
//
//   - the MATCHED set: the archetypes the query visits (complete
//     before the first callback runs — the archetype scan finishes
//     before any callback), and
//   - the READ set: the queried components declared Read.
//
// A World-API mutation is then checked against the guard:
//
//   LEGAL
//     - an in-place create-or-update overwrite (addComponent<T> on an
//       entity that already has T) of a component declared Write, or
//       of a component the query does not list at all (its column is
//       not under an iterator; no row moves);
//     - a structural change (addComponent/removeComponent that moves
//       the entity between archetypes, destroy, clear) whose SOURCE
//       and TARGET archetypes are both outside the matched set;
//     - create() — it touches no archetype rows (a new slot, no
//       components);
//     - the callback writing through its Write references — the
//       intended per-tick mutation path (no World call, no row move).
//
//   ILLEGAL (the mutation is REJECTED — skipped, never applied):
//     - an in-place write to a queried component declared Read
//       ("write during a read iteration");
//     - a structural change whose source OR target archetype is in
//       the matched set ("add/remove during iteration of the
//       affected archetype" — the tail of that archetype would shift
//       under the iterator);
//     - clear() while any matched archetype still holds live rows;
//     - a nested each() while an iteration is active.
//
//   The rejected mutation's row moves nowhere, so the running
//   iteration's view stays valid and the iteration CONTINUES over the
//   unmutated storage (the roadmap's "skip-with-log").
//
// Build behavior of a violation (the same split as the stale-handle
// uses, M1-ECS-01 S-9; FR-12.3, CORE-008: never silent):
//
//   debug builds   assert() fires in the mutating call (a loud
//                  SIGABRT with an actionable message);
//   release builds the mutation returns `ErrorCode::InvalidArgument`,
//                  one rate-limited structured Warn is logged (LOG-004),
//                  and the mutation is skipped.
//
// The guard is NOT a check on raw pointers: writing through a
// `get<T>(e)` pointer during a read iteration bypasses the guard —
// the access tags are the contract, and systems write through the
// query's Write references, not through get<T> (misuse warning
// below). get<T>/has<T>/check/isValid stay pure reads and are never
// rejected by the guard.
//
// ---------------------------------------------------------------------------
// Allocation and complexity (PERF-003, PERF-007)
// ---------------------------------------------------------------------------
//
//   each<T1..TN>      archetype scan O(kMaxArchetypes × N) bounded
//                     passes (N = listed components, ≤ 32) + one row
//                     visit per matching entity. No heap allocation
//                     anywhere in the query path: the iteration state
//                     is stack-scoped (two fixed id arrays of ≤ 32
//                     entries, one 256-entry match list, two 256-bit
//                     guard sets on the World) and the guard is
//                     rebuilt per call from fixed-size state (no pool
//                     needed — the state is one level deep, one
//                     iteration at a time). No logging on the success
//                     path (LOG-003), no lock (single owner thread).
//   guard check        a few loads per World-API mutation: the active
//                     flag plus up to two 256-bit membership tests
//                     (4 × 64-bit words). Negligible on the hot path.
//
// The standing zero-allocation assertion lands with M1-ALLOC-01;
// until then the QueryZeroAlloc suite (test-only operator-new
// counter, non-sanitizer trees) plus the ASan run is the check.
//
// ---------------------------------------------------------------------------
// Errors and logging (FR-12.1, CORE-008, LOG-001/002/004)
// ---------------------------------------------------------------------------
//
//   nested each() while an iteration is active
//                              -> InvalidArgument + warn
//                                 (ecs/iteration_nested); assert in
//                                 debug
//   in-place write to a Read-declared queried component
//                              -> InvalidArgument + warn
//                                 (ecs/iteration_write_during_read);
//                                 assert in debug
//   structural mutation (add/remove/destroy) touching an iterated
//                              archetype
//                              -> InvalidArgument + warn
//                                 (ecs/iteration_mutation); assert in
//                                 debug
//   clear() with live rows in an iterated archetype
//                              -> InvalidArgument + warn
//                                 (ecs/iteration_clear); assert in
//                                 debug
//
// Repeats of each event are rate-limited per (subsystem, event,
// severity) with the suppressed-count summary (LOG-004).
//
// ---------------------------------------------------------------------------
// Threading and determinism
// ---------------------------------------------------------------------------
//
// Single owner thread (CONC-001); the guard is World state mutated in
// an explicit phase (the each() call) — no concurrent access (M1 is
// single-threaded, PRD §10.2).
//
// Determinism (ARCH-010): the visit order is pure integer order over
// the fixed tables (archetype id, slot id) — no floating point, no
// randomness, no addresses. Two worlds that reach the same state
// through different operation histories visit identically (M1-ECS-05
// pins it); the guard sets carry only membership (never an iteration
// order), so they add no non-determinism.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - One access tag per listed component, in template order:
//     `each<T1, T2>(fn, Read, Write)` pairs Read with T1 and Write
//     with T2. The count is checked at compile time; a mismatched tag
//     type or count is a compile error, not a runtime surprise.
//   - A Read reference is const: writing through it (a cast) defeats
//     both the type system and the legality guard. Declare Write for
//     a component a system mutates.
//   - Never write through `get<T>(e)` during a read iteration: the
//     guard cannot see pointer writes. The query's Write reference is
//     the mutation path.
//   - A rejected mutation in release is a Status, not a crash: check
//     it (CORE-008). The usual cause is entity lifecycle inside a
//     per-tick system — move create/destroy/addComponent-set-changes
//     to a spawn/despawn system (the G-R4 advice, M1-ECS-06).
//   - The empty query `each<>` sees a changing live set if the
//     callback destroys entities: a destroyed entity is simply not
//     visited when the scan reaches its slot (documented despawn-sweep
//     semantics, not an error).
//   - Holding a query reference past the callback (or across another
//     mutation of that entity) is dangling: the row can move. Copy
//     out what must survive (PERF-005).

#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>

#include "laige/sim/archetype.h"

namespace laige {

// The declared per-component access of a query (FR-1.3). Read: the
// component is only read during the iteration; Write: the system
// mutates it (through the query's reference or an in-place
// addComponent<T> overwrite — both legal, see the preamble "Iteration
// legality"). M1-SYS-01's system I/O declarations reuse this value
// type.
enum class Access : std::uint8_t {
  Read = 0,
  Write = 1,
};

// The access tags of World::each — one per listed component, in
// template order (the preamble "Query semantics"). The TYPE carries
// the access, so the callback's reference constness is decided at
// compile time:
//
//   world.each<Pos, Vel>(Read{}, Write{}, fn)
//       -> fn(Entity, const Pos&, Vel&)
struct Read {
  static constexpr Access value = Access::Read;
};
struct Write {
  static constexpr Access value = Access::Write;
};

namespace detail {

// A fixed 256-bit set over 1-based ids: bit (id - 1) of word
// (id - 1) / 64. The iteration guard's state (M1-ECS-04): both guard
// sets index the 1..256 id spaces (archetype ids ≤ kMaxArchetypes,
// component ids ≤ kMaxComponentTypes — both 256). Membership tests
// ONLY — the set is never iterated, so its word order is not an
// iteration order (PERF-006: no unordered containers in sim paths;
// M1-ECS-05). Fixed 4 × 64-bit words: no allocation.
struct IdSet256 {
  std::uint64_t word[kMaxArchetypes / 64]{};

  // True when `id` (1..256) is a member. id 0 ("no archetype" /
  // unregistered) is never a member.
  bool contains(std::uint32_t id) const noexcept {
    if (id == 0) return false;
    return (word[(id - 1) / 64] & (1uLL << ((id - 1) % 64))) != 0;
  }

  // Insert `id`; id 0 is a no-op.
  void set(std::uint32_t id) noexcept {
    if (id == 0) return;
    word[(id - 1) / 64] |= 1uLL << ((id - 1) % 64);
  }

  void clear() noexcept {
    word[0] = 0;
    word[1] = 0;
    word[2] = 0;
    word[3] = 0;
  }

  bool empty() const noexcept {
    return (word[0] | word[1] | word[2] | word[3]) == 0;
  }
};

}  // namespace detail

}  // namespace laige
