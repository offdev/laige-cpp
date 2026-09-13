// laige-sim entity handle and world entity storage (M1-ECS-01).
//
// PRD §9.1 S-1: the safe API uses 32-bit entity handles, never raw
// pointers to engine-owned data. This header ships the two ECS
// foundation types:
//
//   Entity   The 32-bit handle: a 16-bit entity id plus a 16-bit
//            generation (FR-1.2, CPP-007). A handle is valid while the
//            slot is live and the slot's current generation equals the
//            handle's; a generation bump on release invalidates every
//            stale handle to that slot.
//
//   World    The entity storage a handle indexes into: create/destroy
//            over a budgeted, accounted, pool-backed slot table with
//            no per-operation heap (PERF-002/003, S-2; M0-CORE-05
//            Pool precedent). M1-ECS-02 adds the component type
//            registry (registerComponent<T>, component.h); M1-ECS-03
//            adds the archetype SoA component storage on top of this
//            slot table (archetype.h: has/get/addComponent/
//            removeComponent + the per-slot archetype record).
//
// ---------------------------------------------------------------------------
// The handle contract (FR-1.2, CPP-007)
// ---------------------------------------------------------------------------
//
// Layout (32 bits total, static-asserted below):
//   [ id: 16 bits | generation: 16 bits ]
//
//   id          slot index, 0 .. kMaxEntityId (65535).
//   generation  1 .. 65535. Generation 0 is reserved: the default
//               Entity{} is never valid.
//
// A handle is valid in the world that produced it while:
//
//   id < world.capacity() && generation != 0 &&
//   the slot is live && slot generation == handle generation.
//
// destroy()/clear() bump the slot's generation, so every stale handle
// to that slot fails isValid() and can never pass again — except after
// 2^16 releases of that one slot (defined unsigned wrap; the documented
// one case the 16-bit scheme does not rule out, and effectively
// unreachable at the PRD §12.1 zone capacities — the M0-CORE-05 2^32
// case at the 16-bit bound).
//
// A handle is meaningful only in the world that produced it:
// id+generation pairs from different worlds are not comparable (an id
// live at the same generation in both worlds passes isValid() in both
// — the same cross-pool caveat as PoolHandle, M0-CORE-05).
//
// ---------------------------------------------------------------------------
// Stale-handle behavior (FR-12.3, S-9, CORE-008: never silent)
// ---------------------------------------------------------------------------
//
// Queries degrade safely in every build; USES of a stale handle fail
// loudly in debug and degrade in release:
//
//   isValid(e)  bool, every build, no side effects.
//   check(e)    Status: ok when live; stale/out-of-range ->
//               ErrorCode::InvalidArgument + one rate-limited
//               structured warn (LOG-004), every build. check() is
//               the access validation that M1-ECS-03's component
//               access builds on.
//   destroy(e)  stale/out-of-range -> debug: assert (a loud
//               use-after-free crash, S-9); release:
//               ErrorCode::InvalidArgument + warn-once (FR-12.3).
//
// ---------------------------------------------------------------------------
// Budget and allocation (G-R3, PERF-002/003, S-2)
// ---------------------------------------------------------------------------
//
// The entity capacity is the declared scene budget (G-R3), fixed at
// construction via World::Options (API-006): raising it is a typed
// configuration change, never a runtime behavior. 0 is legal (every
// create() fails). create() beyond the budget returns
// ErrorCode::BudgetExhausted — the world never grows silently (S-2,
// G-R1). The G-R3 warn thresholds (25%/50%/100%) land with M1-ECS-06.
//
// World::create(Options) performs the storage's only backing
// allocations (a setup path, never a hot path). Every create()/
// destroy()/check()/isValid()/clear() is O(1) and allocates nothing
// (the free list is a pre-allocated LIFO stack). The standing
// zero-allocation check lands with M1-ALLOC-01; until then ASan + the
// stats() accounting is the check (M1 milestone rules).
//
// ---------------------------------------------------------------------------
// Ownership, threading, determinism
// ---------------------------------------------------------------------------
//
// A World is move-only (O(1) pointer swap); a moved-from world is a
// valid empty world (capacity 0: every create() fails, every handle
// invalid). It has exactly one owner thread and is NOT thread-safe
// (CONC-001; PRD §10.2: simulation is single-threaded).
//
// Determinism (ARCH-010): slot assignment is pure integer bookkeeping
// over the LIFO free list — no floating point, no randomness, no
// platform intrinsics. The same create/destroy sequence produces
// bit-identical handle sequences on every platform, so handles are
// safe to replicate and are replay state from M1 on. (The one
// generation-wrap case above is defined unsigned wrap, hence
// bit-identical too.)
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - A handle held past destroy()/clear() is use-after-free. Check
//     isValid() (or read the Status from check()) — never assume.
//   - A handle from one world used in another is meaningless (above).
//   - BudgetExhausted from create() is a declared budget being
//     exceeded — surface it (log + refuse the spawn, S-2), never "fix"
//     it by growing the world at runtime.
//   - World is move-only: keep one owner; a moved-from world is empty.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"

#include "laige/sim/archetype.h"
#include "laige/sim/component.h"

namespace laige {

// The 32-bit entity handle (FR-1.2): a 16-bit slot id plus a 16-bit
// generation (CPP-007). See the header preamble for the full handle
// contract.
struct Entity {
  std::uint16_t id{};          // slot index (0 .. kMaxEntityId)
  std::uint16_t generation{};  // 0 is reserved: never a live handle

  static constexpr std::uint32_t kMaxEntityId = 0xFFFFu;  // 65535
  static constexpr std::uint32_t kMaxEntities = 0x10000u;  // 65536 slots
};

// The handle is exactly 32 bits (FR-1.2).
static_assert(sizeof(Entity) == 4,
              "Entity must be a 32-bit handle (FR-1.2)");

// Handle comparison compares the (id, generation) pair.
inline bool operator==(Entity a, Entity b) noexcept {
  return a.id == b.id && a.generation == b.generation;
}
inline bool operator!=(Entity a, Entity b) noexcept {
  return !(a == b);
}

// One world's entity accounting snapshot (FR-11.1/FR-11.4, G-R3 feed;
// mirrors the M0-CORE-05 PoolStats shape). A plain value the M1
// profiler (M1-PROF-01) and the G-R3 guardrail (M1-ECS-06) pull:
//
//   capacity      the declared scene budget (World::Options::capacity)
//   inUse         live entities right now
//   peakInUse     high-water mark of inUse since construction
//   totalCreated  successful create() calls since construction (churn)
//   bytesCapacity backing-store bytes (per-slot bookkeeping)
//   bytesInUse    bookkeeping bytes occupied by the live slots
struct EntityStats {
  std::uint32_t capacity{};
  std::uint32_t inUse{};
  std::uint32_t peakInUse{};
  std::uint64_t totalCreated{};
  std::size_t bytesCapacity{};
  std::size_t bytesInUse{};
};

// The entity storage behind laige::Entity handles (M1-ECS-01).
//
// See the header preamble for the handle, stale-handle, budget,
// allocation, ownership, threading, and determinism contracts.
class World {
 public:
  // The declared scene budget (G-R3), fixed at construction (API-006).
  // 0 is legal: every create() fails. Values above
  // Entity::kMaxEntities are rejected at construction — the 16-bit id
  // space cannot address them (API-008: the invalid state stays
  // unrepresentable).
  struct Options {
    std::uint32_t capacity{};
  };

  // Construction (setup path: the storage's only backing allocations).
  // capacity > Entity::kMaxEntities -> ErrorCode::InvalidArgument (a
  // handle-space configuration error; the world is not created).
  [[nodiscard]] static Result<World, ErrorCode> create(Options options) noexcept;

  // Create one entity. O(1), no allocation. Beyond the budget:
  // ErrorCode::BudgetExhausted (the world never grows silently, S-2).
  // Slot assignment is LIFO recycling — deterministic (see preamble).
  [[nodiscard]] Result<Entity, ErrorCode> create() noexcept;

  // Destroy one live entity and return its slot to the free list.
  // O(1) for a component-less entity; when the entity is in an
  // archetype, its row is detached first — O(tail rows * row-stride)
  // bytes moved, still no allocation (M1-ECS-03; archetype.h). The
  // slot's generation is bumped, so every stale handle to it fails
  // isValid() (CPP-007). Stale/invalid handle: debug -> assert (S-9);
  // release -> ErrorCode::InvalidArgument + one rate-limited warn
  // (FR-12.3: never silent).
  [[nodiscard]] Status destroy(Entity entity) noexcept;

  // Access validation — the check every entity access performs
  // (M1-ECS-03's component access builds on this). O(1), no
  // allocation. Stale/invalid handle: ErrorCode::InvalidArgument + one
  // rate-limited warn in every build (queries degrade safely, never
  // silent); live: an ok Status.
  [[nodiscard]] Status check(Entity entity) const noexcept;

  // Generation-checked liveness (CPP-007). O(1), no side effects.
  [[nodiscard]] bool isValid(Entity entity) const noexcept;

  // The declared scene budget (World::Options::capacity).
  [[nodiscard]] std::uint32_t capacity() const noexcept;

  // The live entity count right now (the G-R3 numerator; M1-ECS-06
  // turns the inUse/capacity ratio into the 25%/50%/100% warns).
  [[nodiscard]] std::uint32_t entityCount() const noexcept;

  // Entity accounting snapshot for the profiler (M1-PROF-01) and the
  // G-R3 guardrail (M1-ECS-06). O(1), no allocation.
  [[nodiscard]] EntityStats stats() const noexcept;

  // -----------------------------------------------------------------
  // Component registry (M1-ECS-02; full contract in component.h)
  // -----------------------------------------------------------------

  // Register component type T with this world (setup phase, before
  // the loop). Assigns the next ComponentTypeId — dense, in
  // registration order, from 1 — and records sizeof(T)/alignof(T)
  // for the M1-ECS-03 SoA layout. O(n) in the registered types; no
  // allocation. The same path serves built-in and user-defined
  // components (S-8 data-carrier case).
  //
  //   T not marked with LAIGE_COMPONENT -> compile error (static_assert)
  //   T not trivially copyable          -> compile error (static_assert)
  //   T already registered in this world
  //                                     -> ErrorCode::InvalidArgument
  //                                        (+ one rate-limited warn,
  //                                         ecs/component_duplicate)
  //   more than kMaxComponentTypes      -> ErrorCode::BudgetExhausted
  //   moved-from world (no registry)    -> ErrorCode::InvalidArgument
  template <typename T>
  [[nodiscard]] Result<ComponentTypeId, ErrorCode> registerComponent() noexcept;

  // The number of component types registered so far
  // (0 .. kMaxComponentTypes). O(1), no side effects.
  [[nodiscard]] std::uint32_t componentCount() const noexcept;

  // The size/alignment recorded for the type assigned `id` (the
  // M1-ECS-03 SoA layout reads these). O(1), no allocation. `id`
  // invalid or not registered in this world ->
  // ErrorCode::InvalidArgument.
  [[nodiscard]] Result<ComponentInfo, ErrorCode> componentInfo(ComponentTypeId id) const noexcept;

  // -------------------------------------------------------------
  // Archetype SoA component storage (M1-ECS-03; full contract in
  // archetype.h)
  // -------------------------------------------------------------

  // True when `entity` is live and has a component of type T. O(1),
  // no allocation, no side effects (a pure query, like isValid: a
  // stale handle is simply "no", no warn). T must be a Laige
  // component (LAIGE_COMPONENT); an unregistered T reads as false.
  template <typename T>
  [[nodiscard]] bool has(Entity entity) const noexcept;

  // The entity's component of type T, or nullptr: stale/out-of-range
  // handle (after the rate-limited warn-once of check(), every build),
  // T not registered in this world, or the entity lacks T (a normal
  // negative query, no warn). O(1) in the entity count; no allocation.
  // The pointer is valid until the next mutation of that entity's
  // components (an add/remove that moves it shifts the column) or of
  // the world — copy the value out if you must keep it (PERF-005).
  template <typename T>
  [[nodiscard]] T* get(Entity entity) noexcept;

  // Give `entity` a component of type T: create-or-update. When the
  // entity already has T, `value` overwrites it in place (the
  // archetype does not change). Otherwise the entity moves to the
  // archetype of its component set plus T — a pool-backed move over
  // pre-reserved columns: O((tail rows) * row-stride) bytes moved,
  // no heap allocation in steady state (growth events are bounded,
  // accounted, and logged — archetype.h "Reserve policy").
  //
  //   stale/invalid handle       -> InvalidArgument + warn-once
  //   T not registered (this world)
  //                              -> InvalidArgument + warn
  //   entity at kMaxArchetypeComponents
  //                              -> BudgetExhausted + warn
  //   no free archetype slot     -> BudgetExhausted + warn
  template <typename T>
  [[nodiscard]] Status addComponent(Entity entity, const T& value) noexcept;

  // Take the component of type T from `entity` (a no-op ok Status when
  // the entity lacks T or has no components). Otherwise the entity
  // moves to the archetype of its component set minus T — same cost
  // and allocation contract as addComponent. Stale/invalid handle or
  // unregistered T -> InvalidArgument (+ warn).
  template <typename T>
  [[nodiscard]] Status removeComponent(Entity entity) noexcept;

  // The number of distinct component sets seen by this world so far
  // (0 .. kMaxArchetypes; archetypes are never destroyed in M1).
  // O(1), no side effects.
  [[nodiscard]] std::uint32_t archetypeCount() const noexcept;

  // Archetype storage accounting snapshot (ArchetypeStats): the
  // profiler (M1-PROF-01) and the zero-overflow/zero-allocation checks
  // read this. O(kMaxArchetypes), no allocation.
  [[nodiscard]] ArchetypeStats archetypeStats() const noexcept;

  // Destroy every live entity (shutdown path, CONC-006). Every handle
  // becomes stale; the capacity is unchanged and the world is
  // immediately reusable. O(capacity + detached rows * row-stride),
  // no allocation, idempotent. M1-ECS-03: each live entity is
  // detached from its archetype first (the per-entity component data
  // is released with its row); the archetypes themselves — and the
  // component type registry — survive.
  void clear() noexcept;

  // Move is an O(1) pointer swap; the source becomes a valid empty
  // world (capacity 0: every create() fails, every handle invalid).
  World(World&& other) noexcept;
  World& operator=(World&& other) noexcept;
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Detaches every live entity's component rows (clear()) and
  // releases the backing storage (per-slot tables, archetype table
  // with its column blocks, type-key index). Idempotent with clear().
  ~World() noexcept;

 private:
  // The factory (World::create) is the only constructor path: a World
  // with an out-of-range capacity cannot be built (API-008).
  World();

  // Bump one slot's generation on release (defined unsigned wrap,
  // CPP-004). Generation 0 is reserved, so a wrap through 0 is
  // skipped: the slot re-enters at generation 1 — the documented one
  // case the 16-bit scheme does not rule out (see the preamble).
  void bumpGeneration(std::uint16_t slot) noexcept;

  // -------------------------------------------------------------
  // Archetype SoA storage helpers (M1-ECS-03; defined in
  // archetype.cpp)
  // -------------------------------------------------------------

  // This world's ComponentTypeId for a component type identity key
  // (&ComponentTypeKey<T>::kMarker); 0 when the type is not registered
  // in this world. O(1) expected (open-addressing probe, never
  // iterated); no allocation.
  std::uint32_t componentIdOfKey(const void* key) const noexcept;

  // Insert a freshly registered (key, id) into the type-key index
  // (setup path only, called from registerComponent).
  void noteComponentKey(const void* key, std::uint32_t id) noexcept;

  // The archetype whose signature is the sorted, 0-terminated
  // component-id array `sig` of length `count`; nullptr when no such
  // archetype exists yet. O(kMaxArchetypes) bounded scan (fingerprint
  // short-circuit + lexicographic verify); no allocation.
  detail::ArchetypeRecord* findArchetype(const std::uint32_t* sig,
                                         std::uint16_t count) noexcept;

  // Create the archetype for a new sorted signature. Preconditions
  // (checked by the caller): count <= kMaxArchetypeComponents and
  // archetypeCount_ < kMaxArchetypes. Reserves the initial columns
  // (the only allocation; accounted in ArchetypeStats). Returns the
  // record (always non-null under the preconditions).
  detail::ArchetypeRecord* createArchetype(const std::uint32_t* sig,
                                           std::uint16_t count) noexcept;

  // The column index of `componentId` in `arch` (its sorted sig);
  // kInvalidColumnIndex when the archetype lacks the component.
  // O(log kMaxArchetypeComponents); no allocation.
  std::uint32_t columnIndexOf(const detail::ArchetypeRecord& arch,
                              std::uint32_t componentId) const noexcept;

  // Grow `arch`'s row capacity to min(capacity_, 2 * rowCapacity) —
  // one bounded reservation per column (the reserve policy,
  // archetype.h). Returns false only on the unreachable
  // at-world-capacity edge. Accounted + logged (ecs/archetype_grow).
  bool growArchetype(detail::ArchetypeRecord& arch,
                     std::uint32_t archetypeId) noexcept;

  // Shift `arch`'s tail left by one row at `row` (the slot column plus
  // every live column), decrement size, and re-sync the rowOf_ records
  // of the shifted rows (invariant I2 — the slot column and the rowOf_
  // table must agree). The caller clears archetypeOf_/rowOf_ for the
  // detached slot when it leaves the archetypes. O((size - row) *
  // row-stride) bytes moved; no allocation.
  void removeRow(detail::ArchetypeRecord& arch, std::uint32_t row) noexcept;

  // Attach `slot` to `arch` at its slot-ordered position: shift the
  // tail right, write the slot column, set archetypeOf_/rowOf_ (the
  // new row's record plus the shifted tail — invariant I2), grow first
  // if the archetype is full. Returns the new row; kInvalidRowIndex
  // only when growth failed (unreachable while a live entity exists —
  // row 0 is a valid row, hence the sentinel). The component columns
  // of the new row are zero until the caller writes them.
  // O((size - row) * row-stride) bytes moved; no allocation in steady
  // state.
  std::uint32_t attachSlot(std::uint32_t slot,
                           detail::ArchetypeRecord& arch) noexcept;

  std::uint32_t capacity_{0};
  std::unique_ptr<std::uint16_t[]> generations_;
  std::unique_ptr<std::uint8_t[]> alive_;
  std::unique_ptr<std::uint16_t[]> freeStack_;
  std::uint32_t freeCount_{0};
  std::uint32_t inUse_{0};
  std::uint32_t peakInUse_{0};
  std::uint64_t totalCreated_{0};
  // Component registry (M1-ECS-02): dense records indexed by
  // (id - 1) — ids are dense by construction. Setup-only state:
  // clear() does not touch it (M1-ECS-03's clear destroys per-entity
  // component data, not the type registry).
  std::unique_ptr<detail::ComponentRecord[]> components_;
  std::uint32_t componentCount_{0};
  // Per-slot entity record (M1-ECS-03): the archetype the slot's entity
  // is in (0 = no archetype: the entity has no components) and the
  // entity's dense row within that archetype (the slot-ordered row —
  // see archetype.h "Row order"). Both are per-slot table loads: the
  // entity -> archetype map is direct indexing, no hash.
  std::unique_ptr<std::uint16_t[]> archetypeOf_;
  std::unique_ptr<std::uint32_t[]> rowOf_;
  // Archetype table (M1-ECS-03): the fixed engine budget
  // (kMaxArchetypes), records indexed by (archetype id - 1); an
  // archetype is created when its component set is first seen.
  // clear() empties the archetypes' rows but keeps the archetypes
  // themselves (M1: sets are never destroyed — archetype.h).
  std::unique_ptr<detail::ArchetypeRecord[]> archetypes_;
  std::uint32_t archetypeCount_{0};
  // Component type-key index (M1-ECS-03): the open-addressing table
  // (kComponentKeyIndexSize slots) mapping a component type identity
  // key to this world's ComponentTypeId — the O(1) type -> id
  // resolution behind has/get/addComponent/removeComponent.
  std::unique_ptr<detail::ComponentKeySlot[]> keyIndex_;
  // Archetype storage counters (ArchetypeStats feed; G-R4/M1-PROF-01).
  std::uint64_t totalAdds_{0};
  std::uint64_t totalRemoves_{0};
  std::uint64_t totalArchetypeGrowth_{0};
  std::uint64_t totalReservations_{0};
};

// Component registration (M1-ECS-02). Header-defined: it is a template,
// so it must be visible to every translation unit that registers a
// component. See component.h for the id contract, the type-identity
// mechanism, and the failure table.
template <typename T>
Result<ComponentTypeId, ErrorCode> World::registerComponent() noexcept {
  static_assert(detail::ComponentTraits<T>::isComponent,
                "T is not a Laige component type: write LAIGE_COMPONENT(T) "
                "once at namespace scope next to the type definition "
                "(FR-1.2, S-8)");
  static_assert(std::is_trivially_copyable_v<T>,
                "Laige components must be trivially copyable data "
                "carriers (S-8): a non-trivial member (a string, a "
                "destructor, a vtable) is not supported by the "
                "M1-ECS-03 SoA storage");
  static_assert(alignof(T) <= kArchetypeColumnAlignment,
                "Laige components must have alignof(T) <= 32 "
                "(kArchetypeColumnAlignment): the M1-ECS-03 SoA column "
                "blocks are aligned to 32 bytes (the P0 targets' "
                "maximum fundamental alignment); reduce the alignment "
                "or ADR the bound");
  constexpr const void* key = &detail::ComponentTypeKey<T>::kMarker;
  if (components_ == nullptr) {
    // Moved-from world: no registry (see the preamble, entity.h).
    return ErrorCode::InvalidArgument;
  }
  for (std::uint32_t i = 0; i < componentCount_; ++i) {
    if (components_[i].typeKey == key) {
      // Duplicate registration is an error (M1-ECS-02 scope); the
      // facade rate-limits the warn per event (LOG-004).
      LAIGE_LOG_WARN("ecs", "component_duplicate",
                     "Component type is already registered in this world; "
                     "duplicate registration is an error",
                     laige::log::field("component_id", i + 1u),
                     laige::log::field("size", components_[i].size),
                     laige::log::field("alignment", components_[i].alignment));
      return ErrorCode::InvalidArgument;
    }
  }
  if (componentCount_ >= kMaxComponentTypes) {
    // The engine-level component budget (component.h preamble).
    return ErrorCode::BudgetExhausted;
  }
  detail::ComponentRecord& rec = components_[componentCount_];
  rec.typeKey = key;
  rec.size = static_cast<std::uint32_t>(sizeof(T));
  rec.alignment = static_cast<std::uint32_t>(alignof(T));
  const ComponentTypeId id{componentCount_ + 1};
  noteComponentKey(key, id.value);  // M1-ECS-03: the type -> id lookup
  ++componentCount_;
  return id;  // ids are dense, from 1
}

// ---------------------------------------------------------------------------
// Archetype SoA component access (M1-ECS-03; full contract in
// archetype.h). Header-defined like registerComponent: templates must
// be visible to every translation unit that touches a component.
// ---------------------------------------------------------------------------

// The row-copy helper lives in detail (excluded from the public API
// scan, tools/api) — an anonymous namespace is not scanner-parseable.
namespace detail {

// Copy `size` bytes from `src` to `dst` (components are trivially
// copyable — the registerComponent static_assert — so memcpy is the
// well-defined copy). Cold-branch helper to keep the templates short.
inline void copyRow(std::byte* dst, const std::byte* src, std::uint32_t size) {
  std::memcpy(dst, src, static_cast<std::size_t>(size));
}

}  // namespace detail

template <typename T>
bool World::has(Entity entity) const noexcept {
  static_assert(detail::ComponentTraits<T>::isComponent,
                "T is not a Laige component type: write LAIGE_COMPONENT(T) "
                "once at namespace scope next to the type definition "
                "(FR-1.2, S-8)");
  if (!isValid(entity)) return false;
  const std::uint32_t id =
      componentIdOfKey(&detail::ComponentTypeKey<T>::kMarker);
  if (id == 0) return false;  // T not registered in this world
  const std::uint32_t archIdx = archetypeOf_[entity.id];
  if (archIdx == 0) return false;  // the entity has no components
  return columnIndexOf(archetypes_[archIdx - 1], id) != kInvalidColumnIndex;
}

template <typename T>
T* World::get(Entity entity) noexcept {
  static_assert(detail::ComponentTraits<T>::isComponent,
                "T is not a Laige component type: write LAIGE_COMPONENT(T) "
                "once at namespace scope next to the type definition "
                "(FR-1.2, S-8)");
  if (!isValid(entity)) {
    static_cast<void>(check(entity));  // rate-limited warn-once, every build
    return nullptr;
  }
  const std::uint32_t id =
      componentIdOfKey(&detail::ComponentTypeKey<T>::kMarker);
  if (id == 0) return nullptr;  // T not registered in this world
  const std::uint32_t archIdx = archetypeOf_[entity.id];
  if (archIdx == 0) return nullptr;  // the entity has no components
  detail::ArchetypeRecord& arch = archetypes_[archIdx - 1];
  const std::uint32_t col = columnIndexOf(arch, id);
  if (col == kInvalidColumnIndex) return nullptr;  // entity lacks T
  const detail::ArchetypeColumn& column = arch.columns[col];
  return reinterpret_cast<T*>(column.base +
                              static_cast<std::size_t>(rowOf_[entity.id]) *
                              column.size);
}

template <typename T>
Status World::addComponent(Entity entity, const T& value) noexcept {
  static_assert(detail::ComponentTraits<T>::isComponent,
                "T is not a Laige component type: write LAIGE_COMPONENT(T) "
                "once at namespace scope next to the type definition "
                "(FR-1.2, S-8)");
  static_assert(std::is_trivially_copyable_v<T>,
                "Laige components must be trivially copyable data "
                "carriers (S-8) — the SoA columns memcpy them");
  if (!isValid(entity)) {
    static_cast<void>(check(entity));  // rate-limited warn-once, every build
    return ErrorCode::InvalidArgument;
  }
  const std::uint32_t id =
      componentIdOfKey(&detail::ComponentTypeKey<T>::kMarker);
  if (id == 0) {
    LAIGE_LOG_WARN("ecs", "component_unregistered",
                   "addComponent called for a type not registered in this "
                   "world; register it at world setup",
                   laige::log::field("entity_id", entity.id),
                   laige::log::field("generation", entity.generation));
    return ErrorCode::InvalidArgument;
  }
  const std::uint32_t curIdx = archetypeOf_[entity.id];
  const std::uint32_t curRow = rowOf_[entity.id];
  if (curIdx != 0) {
    const std::uint32_t curCol =
        columnIndexOf(archetypes_[curIdx - 1], id);
    if (curCol != kInvalidColumnIndex) {
      // Create-or-update: the entity already has T — overwrite in
      // place (no archetype change, documented).
      const detail::ArchetypeColumn& column =
          archetypes_[curIdx - 1].columns[curCol];
      detail::copyRow(column.base + static_cast<std::size_t>(curRow) * column.size,
              reinterpret_cast<const std::byte*>(&value), sizeof(T));
      ++totalAdds_;
      return Status{};
    }
  }
  // Build the target signature (the entity's set plus T, sorted).
  std::uint32_t targetSig[kMaxArchetypeComponents + 1];
  std::uint16_t targetCount = 0;
  if (curIdx == 0) {
    targetSig[0] = id;
    targetCount = 1;
  } else {
    detail::ArchetypeRecord& cur = archetypes_[curIdx - 1];
    if (cur.sigCount >= kMaxArchetypeComponents) {
      LAIGE_LOG_WARN("ecs", "component_limit",
                     "Entity already carries kMaxArchetypeComponents "
                     "components; the set is full (M1 bound — ADR to raise)",
                     laige::log::field("entity_id", entity.id),
                     laige::log::field("archetype_id", curIdx),
                     laige::log::field("components", cur.sigCount));
      return ErrorCode::BudgetExhausted;
    }
    // Merge-insert `id` into the sorted cur.sig (id is absent: the
    // in-place case above caught its presence).
    const std::uint16_t n = cur.sigCount;
    for (std::uint16_t r = 0; r < n; ++r) {
      if (cur.sig[r] < id) {
        targetSig[targetCount++] = cur.sig[r];
      } else {
        targetSig[targetCount++] = id;
        for (std::uint16_t k = r; k < n; ++k) {
          targetSig[targetCount++] = cur.sig[k];
        }
        break;
      }
    }
    if (targetCount == n) targetSig[targetCount++] = id;  // id is last
  }
  // Find the target archetype, creating it when the set is new.
  detail::ArchetypeRecord* target = findArchetype(targetSig, targetCount);
  if (target == nullptr) {
    if (archetypeCount_ >= kMaxArchetypes) {
      LAIGE_LOG_WARN("ecs", "archetype_budget",
                     "The world has kMaxArchetypes distinct component "
                     "sets; a new set cannot be created (M1 bound — "
                     "ADR to raise)",
                     laige::log::field("entity_id", entity.id),
                     laige::log::field("archetype_count", archetypeCount_));
      return ErrorCode::BudgetExhausted;
    }
    target = createArchetype(targetSig, targetCount);
  }
  // Attach the slot to the target (slot-ordered; grows the target
  // first when it is full), write the new row, then release the old
  // row. attachSlot owns archetypeOf_/rowOf_ updates; the old row is
  // removed explicitly from cur (the target differs from cur: the
  // signatures differ by T).
  const std::uint32_t newRow = attachSlot(entity.id, *target);
  if (newRow == kInvalidRowIndex) {
    // Unreachable while a live entity exists (the reserve policy caps
    // growth at the world capacity, and one live entity outside the
    // full archetype always exists — the one being added).
    LAIGE_LOG_WARN("ecs", "archetype_budget",
                   "Archetype row reserve failed (at world capacity — "
                   "should be unreachable)",
                   laige::log::field("entity_id", entity.id));
    return ErrorCode::BudgetExhausted;
  }
  for (std::uint16_t i = 0; i < target->sigCount; ++i) {
    const std::uint32_t cid = target->sig[i];
    detail::ArchetypeColumn& column = target->columns[i];
    if (cid == id) {
      detail::copyRow(column.base + static_cast<std::size_t>(newRow) * column.size,
              reinterpret_cast<const std::byte*>(&value), sizeof(T));
    } else if (curIdx != 0) {
      const detail::ArchetypeRecord& cur = archetypes_[curIdx - 1];
      const std::uint32_t srcCol = columnIndexOf(cur, cid);
      const detail::ArchetypeColumn& src = cur.columns[srcCol];
      detail::copyRow(column.base + static_cast<std::size_t>(newRow) * column.size,
              src.base + static_cast<std::size_t>(curRow) * src.size,
              src.size);
    }
  }
  if (curIdx != 0) removeRow(archetypes_[curIdx - 1], curRow);
  ++totalAdds_;
  return Status{};
}

template <typename T>
Status World::removeComponent(Entity entity) noexcept {
  static_assert(detail::ComponentTraits<T>::isComponent,
                "T is not a Laige component type: write LAIGE_COMPONENT(T) "
                "once at namespace scope next to the type definition "
                "(FR-1.2, S-8)");
  if (!isValid(entity)) {
    static_cast<void>(check(entity));  // rate-limited warn-once, every build
    return ErrorCode::InvalidArgument;
  }
  const std::uint32_t id =
      componentIdOfKey(&detail::ComponentTypeKey<T>::kMarker);
  if (id == 0) {
    LAIGE_LOG_WARN("ecs", "component_unregistered",
                   "removeComponent called for a type not registered in "
                   "this world; register it at world setup",
                   laige::log::field("entity_id", entity.id),
                   laige::log::field("generation", entity.generation));
    return ErrorCode::InvalidArgument;
  }
  const std::uint32_t curIdx = archetypeOf_[entity.id];
  if (curIdx == 0) return Status{};  // no components: a no-op
  detail::ArchetypeRecord& cur = archetypes_[curIdx - 1];
  const std::uint32_t curCol = columnIndexOf(cur, id);
  if (curCol == kInvalidColumnIndex) return Status{};  // lacks T: a no-op
  const std::uint32_t curRow = rowOf_[entity.id];
  if (cur.sigCount == 1) {
    // The entity's last component: it leaves the archetypes entirely.
    removeRow(cur, curRow);
    archetypeOf_[entity.id] = 0;
    rowOf_[entity.id] = 0;
    ++totalRemoves_;
    return Status{};
  }
  // Build the target signature (the entity's set minus T, still
  // sorted — cur.sig is sorted and unique).
  std::uint32_t targetSig[kMaxArchetypeComponents];
  std::uint16_t targetCount = 0;
  for (std::uint16_t r = 0; r < cur.sigCount; ++r) {
    if (cur.sig[r] != id) targetSig[targetCount++] = cur.sig[r];
  }
  detail::ArchetypeRecord* target = findArchetype(targetSig, targetCount);
  if (target == nullptr) {
    if (archetypeCount_ >= kMaxArchetypes) {
      LAIGE_LOG_WARN("ecs", "archetype_budget",
                     "The world has kMaxArchetypes distinct component "
                     "sets; a new set cannot be created (M1 bound — "
                     "ADR to raise)",
                     laige::log::field("entity_id", entity.id),
                     laige::log::field("archetype_count", archetypeCount_));
      return ErrorCode::BudgetExhausted;
    }
    target = createArchetype(targetSig, targetCount);
  }
  const std::uint32_t newRow = attachSlot(entity.id, *target);
  if (newRow == kInvalidRowIndex) {
    LAIGE_LOG_WARN("ecs", "archetype_budget",
                   "Archetype row reserve failed (at world capacity — "
                   "should be unreachable)",
                   laige::log::field("entity_id", entity.id));
    return ErrorCode::BudgetExhausted;
  }
  for (std::uint16_t i = 0; i < target->sigCount; ++i) {
    const std::uint32_t cid = target->sig[i];
    detail::ArchetypeColumn& column = target->columns[i];
    const std::uint32_t srcCol = columnIndexOf(cur, cid);  // cid != id
    const detail::ArchetypeColumn& src = cur.columns[srcCol];
    detail::copyRow(column.base + static_cast<std::size_t>(newRow) * column.size,
            src.base + static_cast<std::size_t>(curRow) * src.size,
            src.size);
  }
  removeRow(cur, curRow);
  ++totalRemoves_;
  return Status{};
}

}  // namespace laige
