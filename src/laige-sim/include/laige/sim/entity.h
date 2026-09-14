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
//            removeComponent + the per-slot archetype record);
//            M1-ECS-04 adds the query API and the iteration-legality
//            guard (query.h: World::each<T1, T2, ...>(Read/Write
//            tags..., fn) + the World-API mutation checks); M1-SYS-01
//            adds the system registry (system.h: SystemDef,
//            SystemContext, the LAIGE_SYSTEM macro,
//            World::registerSystem/system/systemCount); M1-SYS-02
//            adds the system scheduler (system.h: SystemSchedule,
//            the depends_on spec, World::scheduleSystems/
//            runSystems — execution order, depends_on, and the
//            pre-run I/O validation).
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
// G-R1). The G-R3 warn thresholds (25%/50%/100%) and the G-R4
// per-frame churn budget are enforced by M1-ECS-06 (see the
// "Guardrails" section below; guardrails.cpp).
//
// World::create(Options) performs the storage's only backing
// allocations (a setup path, never a hot path). Every create()/
// destroy()/check()/isValid()/clear() is O(1) and allocates nothing
// (the free list is a pre-allocated LIFO stack). The standing
// zero-allocation check lands with M1-ALLOC-01; until then ASan + the
// stats() accounting is the check (M1 milestone rules).
//
// ---------------------------------------------------------------------------
// Guardrails (G-R3, G-R4; M1-ECS-06; PRD §9.3)
// ---------------------------------------------------------------------------
//
// G-R3 (entity count): create() emits one structured warn exactly
// when the live count REACHES 25%/50%/100% of the declared scene
// budget — integer thresholds capacity * pct / 100 (a level whose
// threshold computes to 0 never fires: the live count is 0 only
// before the first create). A level warns at most ONCE PER FRAME:
// dipping below and re-crossing within the same frame does not
// re-warn. Frame boundaries are driven by beginFrame(); without one
// the guardrail degrades to warn-once-per-lifetime (documented,
// never silent). Events: ecs/entity_budget_{25,50,100}.
//
// G-R4 (per-frame component churn): the successful addComponent
// calls (including in-place overwrites — the same counting as
// ArchetypeStats::totalAdds) plus the removeComponent calls that
// actually detach a row (no-op removes, destroy/clear detaches, and
// the BudgetExhausted/invalid rejects are not counted) are counted
// per frame. When the per-frame total STRICTLY EXCEEDS
// Options::churnPerFrameBudget, one ecs/churn_per_frame warn fires
// per frame. Budget 0 disables the guardrail.
//
// Both guardrails are O(1) integer bookkeeping on the hot path (no
// allocation — the warn paths are cold: fields construct only when
// the event is enabled, LOG-003). Their counters are pulled by the
// profiler (M1-PROF-01) through guardrailStats() (a plain value, no
// allocation, no side effects). Message text follows the NFR-13.3
// 5-field error grammar ({code} | {what} | {why} | {fix} |
// {doc_anchor}) and is build-stable; per PRD §9.3 ("warn (debug:
// with advice)"), debug builds additionally carry an `advice`
// structured FIELD — never message text.
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
#include <utility>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"

#include "laige/sim/archetype.h"
#include "laige/sim/component.h"
#include "laige/sim/query.h"
#include "laige/sim/system.h"

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

// The default G-R4 per-frame component-churn budget (CORE-005). At
// the M1 reference scene (10k entities, PRD §8.1) 256 lifecycle
// ops per frame is ~2.6% of the scene — steady-state gameplay stays
// far below it; a sustained breach indicates unbatched spawn/despawn
// churn on the hot path (the guardrail's advice). Overridable per
// world (World::Options::churnPerFrameBudget); scenes with a
// legitimately churning lifecycle raise it through typed
// configuration, and 0 disables the guardrail.
inline constexpr std::uint32_t kDefaultChurnPerFrameBudget = 256;

// M1-ECS-06 (G-R3, G-R4) guardrail snapshot. A plain value the M1
// profiler (M1-PROF-01) pulls each frame (World::guardrailStats());
// mirrors the EntityStats/ArchetypeStats snapshot shape:
//
//   capacity             the declared scene budget (G-R3 denominator)
//   entityCount          live entities right now (G-R3 numerator)
//   entityBudgetLevel    0, 25, 50, or 100 — the highest percentage of
//                        the budget the PEAK live count reached since
//                        construction (0 = never reached 25%)
//   entityBudgetWarns    per-level warn counts since construction
//                        (index 0 = 25%, 1 = 50%, 2 = 100%)
//   frameChurn           component adds + removes since the last
//                        beginFrame() (the G-R4 numerator)
//   churnPerFrameBudget  the configured G-R4 budget (0 = disabled)
//   churnWarns           churn warnings issued since construction
struct GuardrailStats {
  std::uint32_t capacity{};
  std::uint32_t entityCount{};
  std::uint32_t entityBudgetLevel{};
  std::uint32_t entityBudgetWarns[3]{};
  std::uint64_t frameChurn{};
  std::uint32_t churnPerFrameBudget{};
  std::uint32_t churnWarns{};
};

// M1-ECS-04 query helpers (detail: engine implementation, excluded
// from the public API scan). Declared before World: the member
// templates of the World class reference them by qualified name at
// the point of definition.
namespace detail {

// The row reference of the I-th queried component (the query's
// per-row hand-off, World::each). `row` is the dense row index,
// `cols` the per-component column indices of the matched archetype
// (resolved once per archetype by each()). The declared access tag
// decides the value category — a `T&` for Write (the intended
// mutation path), a `const T&` for Read (a write through it requires
// a cast: a bug the compiler rejects, API-008). No allocation: one
// pointer arithmetic.
//
// The component TYPE and ACCESS packs arrive as single tuple types
// (std::tuple<Ts...>, std::tuple<Acc...>): an explicit template
// argument list cannot partition between two consecutive packs
// (GCC gives the first pack zero arguments), so no pack may sit in
// the template parameter list here. The reference type is one
// conditional alias (a single return statement: an if-constexpr pair
// of returns would force a consistent decltype(auto) deduction from
// both branches, and T& vs const T& never is).
template <std::size_t I, typename Row, typename TList, typename AList>
constexpr decltype(auto) rowRef(const Row& row, const std::uint32_t* cols,
                               const ArchetypeRecord& arch) {
  using T = std::tuple_element_t<I, TList>;
  using A = std::tuple_element_t<I, AList>;
  static_assert(std::is_trivially_copyable_v<T>,
                "Laige components must be trivially copyable data "
                "carriers (S-8) — the SoA columns memcpy them");
  static_assert(alignof(T) <= kArchetypeColumnAlignment,
                "Laige components must have alignof(T) <= 32 "
                "(kArchetypeColumnAlignment): the SoA column blocks are "
                "aligned to 32 bytes");
  using Ref = std::conditional_t<std::is_same_v<A, Write>, T&, const T&>;
  const ArchetypeColumn& column = arch.columns[cols[I]];
  T* p = reinterpret_cast<T*>(
      column.base + static_cast<std::size_t>(row) * column.size);
  return static_cast<Ref>(*p);
}

// Record a queried component's id in the guard's read set when its
// access tag is Read (the tags are static-checked to be Read/Write by
// World::each, so "not Write" is "Read"). id 0 (unregistered) is a
// no-op — such a component matches nothing.
template <std::size_t I, typename... Acc>
void setQueryReadBit(IdSet256& readSet, std::uint32_t id) {
  using A = std::tuple_element_t<I, std::tuple<Acc...>>;
  if constexpr (!std::is_same_v<A, Write>) {
    readSet.set(id);
  }
}

// True when every tag in the pack is a Read/Write access tag
// (World::each's compile-time tag check; an empty pack is true).
template <typename... As>
inline constexpr bool isAccessTags() {
  bool ok = true;
  ((ok = ok && (std::is_same_v<As, Read> || std::is_same_v<As, Write>)), ...);
  return ok;
}

}  // namespace detail

// The entity storage behind laige::Entity handles (M1-ECS-01).
//
// See the header preamble for the handle, stale-handle, budget,
// allocation, ownership, threading, and determinism contracts.
class World {
 public:
  // The declared scene budget (G-R3) and the G-R4 per-frame churn
  // budget, fixed at construction (API-006).
  struct Options {
    // The declared scene budget (G-R3). 0 is legal: every create()
    // fails. Values above Entity::kMaxEntities are rejected at
    // construction — the 16-bit id space cannot address them
    // (API-008: the invalid state stays unrepresentable).
    std::uint32_t capacity{};
    // The G-R4 per-frame component-churn budget: the number of
    // component add/remove ops per frame (beginFrame() to
    // beginFrame()) above which the world warns
    // (ecs/churn_per_frame). Strictly-greater semantics; 0 disables
    // the guardrail. Default: kDefaultChurnPerFrameBudget.
    std::uint32_t churnPerFrameBudget{kDefaultChurnPerFrameBudget};
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

  // ---------------------------------------------------------------
  // ECS guardrails (M1-ECS-06: G-R3, G-R4; full contract in the
  // header preamble "Guardrails" and docs/api/entity.md)
  // ---------------------------------------------------------------

  // Mark the start of a frame (G-R3/G-R4): resets the per-frame
  // component-churn counters and the once-per-frame entity-budget
  // warn flags. O(1), no allocation, no log. The owning loop drives
  // it once per frame (M1-LOOP-01); before the loop exists, the game
  // or tests drive it manually. Never driven, the guardrails
  // degrade to warn-once-per-lifetime (documented, never silent).
  // Reading the per-frame counters: guardrailStats() before the next
  // beginFrame() returns the just-completed frame's values.
  void beginFrame() noexcept;

  // The guardrail accounting snapshot for the profiler (M1-PROF-01):
  // the G-R3 level/warn counts, the G-R4 per-frame churn and its
  // budget, and the warn counters (GuardrailStats). O(1), no
  // allocation, no side effects.
  [[nodiscard]] GuardrailStats guardrailStats() const noexcept;

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

  // -------------------------------------------------------------
  // Query API + iteration legality (M1-ECS-04; full contract in
  // query.h)
  // -------------------------------------------------------------

  // Iterate every entity having ALL of T1..TN (superset match: extra
  // components do not exclude an entity), invoking
  // `fn(Entity, R1, ..., RN)` — one reference per listed component,
  // in template order: a `const T&` where the access tag is Read, a
  // `T&` where it is Write. The access tags follow `fn`, one
  // Read/Write tag per listed component, in the same order (checked
  // at compile time — they come after the callable because a pack of
  // parameters must be the last parameters to be deducible);
  // `each<>` (no components, no tags) visits every live entity in
  // ascending slot-id order with no component references.
  //
  //   Visit order      ascending archetype id (creation order), then
  //                    ascending slot id per archetype (M1-ECS-05
  //                    documents the contract)
  //   Unregistered T   the query matches nothing (ok Status, zero
  //                    visits — a pure query, like has<T>)
  //   Nested each()    while an iteration is active: assert in debug,
  //                    ErrorCode::InvalidArgument + one rate-limited
  //                    warn in release (the nested iteration is
  //                    rejected; query.h "Iteration legality")
  //   Allocation       none — the iteration state is stack-scoped
  //                    (query.h)
  //   Complexity       O(kMaxArchetypes * N) scan + one visit per
  //                    matching entity (PERF-007)
  // The access-tag VALUES are unnamed: only their TYPES are used (the
  // static_asserts and template arguments in the definition) — a named
  // pack would be an unreferenced parameter (MSVC C4100, fatal under
  // /WX; NFR-8.10).
  template <typename... Ts, typename... Acc, typename F>
  [[nodiscard]] Status each(F&& fn, Acc...) noexcept;

  // -------------------------------------------------------------
  // System registry (M1-SYS-01; full contract in system.h)
  // -------------------------------------------------------------

  // Register the system described by `def` in this world, declaring
  // its component I/O as the Io<...> pack (zero entries = a system
  // that touches no components). Setup phase (world construction,
  // before the loop), like registerComponent<T>: O(n) in the number
  // of registered systems, no allocation (the def is copied into the
  // fixed kMaxSystems record table; the I/O sets are written in
  // place).
  //
  //   moved-from world (no registry)   -> InvalidArgument
  //   def.name null or empty            -> InvalidArgument + warn
  //                                        (system/name_invalid)
  //   def.run nullptr                   -> InvalidArgument + warn
  //                                        (system/run_invalid)
  //   def.budgetMs <= 0                 -> InvalidArgument + warn
  //                                        (system/budget_invalid) —
  //                                        the budget must be explicit
  //   malformed depends_on spec (empty
  //   token, duplicate name, more than
  //   kMaxSystemDependencies)          -> InvalidArgument + warn
  //                                        (system/dep_spec_invalid)
  //   duplicate name in this world      -> InvalidArgument + warn
  //                                        (system/duplicate)
  //   Io<T> T not a Laige component     -> compile error (static_assert)
  //   Io<T> T not registered (this world)
  //                                   -> InvalidArgument + warn
  //                                        (system/io_unregistered)
  //   the same component declared twice by one system (any access
  //   combination)                     -> InvalidArgument + warn
  //                                        (system/io_duplicate)
  //   more than kMaxSystems            -> BudgetExhausted + warn
  //                                        (system/budget_exhausted)
  //
  // Returns the new SystemId (dense, from 1, in registration order —
  // the component.h id contract).
  template <typename... Ios>
  [[nodiscard]] Result<SystemId, ErrorCode> registerSystem(const SystemDef& def, Ios...) noexcept;

  // The number of systems registered so far (0 .. kMaxSystems). O(1),
  // no side effects.
  [[nodiscard]] std::uint32_t systemCount() const noexcept;

  // The registered system's record under `id` (SystemInfo: the def
  // value copy plus the declared I/O membership queries). O(1), no
  // allocation. `id` invalid (0 or above systemCount()) or a
  // moved-from world -> ErrorCode::InvalidArgument (a pure query,
  // like componentInfo).
  [[nodiscard]] Result<SystemInfo, ErrorCode> system(SystemId id) const noexcept;

  // -------------------------------------------------------------
  // System scheduler (M1-SYS-02; full contract in system.h,
  // "Scheduler")
  // -------------------------------------------------------------

  // Compute and validate this world's execution order into `out`
  // (SystemSchedule). Setup phase (after all registrations, before
  // the loop); a pure read of the registry (const). The order is the
  // stable topological sort of the registration order plus the
  // declared depends_on edges (system.h). Validation order (first
  // failure wins): unknown dependency name (system/dep_missing),
  // dependency cycle (system/dependency_cycle), two systems writing
  // the same component (system/double_writer) — each InvalidArgument
  // + one rate-limited warn; a declared read ordered before a
  // declared write of the same component WARNs without failing
  // (system/read_before_write). Success: `out` fully populated,
  // nothing logged (LOG-003). Setup path: O(n·d·n + c·n²) in the
  // system count n (≤ kMaxSystems), direct dependencies d (≤
  // kMaxSystemDependencies), and component count c (≤
  // kMaxComponentTypes); no allocation.
  [[nodiscard]] Status scheduleSystems(SystemSchedule& out) const noexcept;

  // Run the systems of `schedule` once — one sim tick's system phase
  // (the M1-LOOP-01 accumulator calls this once per tick). The
  // systems run strictly one at a time, in schedule order, on the
  // world's single owner thread (PRD §10.2); each gets a fresh
  // non-owning SystemContext. O(n) dispatch plus the systems' own
  // work; no allocation (PERF-003), no logging on the success path
  // (LOG-003).
  //
  //   schedule.systemCount != the world's systemCount
  //                                   -> InvalidArgument + warn
  //                                        (system/schedule_stale) —
  //                                        the registry changed since
  //                                        the schedule was computed
  //   order entry 0, above systemCount, or a duplicate id
  //                                   -> InvalidArgument + warn
  //                                        (system/schedule_invalid)
  //   schedule.systemCount == 0       -> ok, runs nothing
  [[nodiscard]] Status runSystems(const SystemSchedule& schedule) noexcept;

  // Destroy every live entity (shutdown path, CONC-006). Every handle
  // becomes stale; the capacity is unchanged and the world is
  // immediately reusable. O(capacity + detached rows * row-stride),
  // no allocation, idempotent. M1-ECS-03: each live entity is
  // detached from its archetype first (the per-entity component data
  // is released with its row); the archetypes themselves — and the
  // component type registry — survive. M1-ECS-04: rejected with
  // ErrorCode::InvalidArgument (+ one rate-limited warn) while an
  // iteration is active and any matched archetype still holds live
  // rows — the clear is skipped, never partial (assert in debug;
  // query.h "Iteration legality"); an ok Status otherwise.
  [[nodiscard]] Status clear() noexcept;

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

  // -------------------------------------------------------------
  // Iteration-legality guard (M1-ECS-04; defined in query.cpp, full
  // contract in query.h). Every helper asserts in debug and logs +
  // returns InvalidArgument in release when the check fails; ok
  // Status when the mutation is legal.
  // -------------------------------------------------------------

  // Reject a nested World::each while an iteration is active
  // (ecs/iteration_nested).
  [[nodiscard]] Status guardIterationStart() noexcept;

  // Reject an in-place component overwrite (the create-or-update
  // branch of addComponent) of a queried component declared Read in
  // the active iteration (ecs/iteration_write_during_read). Legal
  // when the component is declared Write, or not listed by the query.
  [[nodiscard]] Status guardInplaceWrite(std::uint32_t componentId,
                                         std::uint32_t archIdx,
                                         Entity entity) noexcept;

  // Reject a structural change — an archetype move (addComponent /
  // removeComponent), a destroy, or a clear — whose source OR target
  // archetype is in the active iteration's matched set
  // (ecs/iteration_mutation). `op` names the mutating call for the
  // actionable message; 0 = "no archetype" (a component-less entity,
  // or an entity leaving the archetypes) and is never a member.
  [[nodiscard]] Status guardStructural(const char* op,
                                       std::uint32_t sourceArch,
                                       std::uint32_t targetArch,
                                       Entity entity) noexcept;

  // Reject clear() while any matched archetype of the active
  // iteration still holds live rows (ecs/iteration_clear).
  [[nodiscard]] Status guardClear() noexcept;

  // -------------------------------------------------------------
  // M1-SYS-01 system-registry helper (defined in this header with
  // registerSystem; full contract in system.h)
  // -------------------------------------------------------------

  // Resolve one Io<T, Access> entry of registerSystem: resolve T's
  // ComponentTypeId in this world (0 when T is unregistered) and set
  // the declared bit in the matching set. The sets are mutated only
  // on Ok. `failedId` receives the resolved component id for the
  // Duplicate case (the log field; left 0 otherwise).
  template <typename Tag>
  detail::IoResolution resolveIoEntry(detail::IdSet256& read,
                                      detail::IdSet256& write,
                                      std::uint32_t* failedId) const noexcept;

  // -------------------------------------------------------------
  // M1-ECS-06 guardrail checks (G-R3/G-R4; defined in guardrails.cpp)
  // -------------------------------------------------------------

  // Compute the per-level entity-budget thresholds (25/50/100% of
  // the declared capacity; a 0 threshold never fires) — setup path,
  // called from World::create(Options).
  void initEntityBudgetThresholds() noexcept;

  // G-R3: the per-level crossing check, run at the end of every
  // successful create(). Emits the ecs/entity_budget_{25,50,100}
  // warns (at most once per level per frame).
  void checkEntityBudget() noexcept;

  // G-R4: the per-frame churn-budget check, run after every counted
  // add/remove. Emits the ecs/churn_per_frame warn (at most once per
  // frame).
  void checkChurnBudget() noexcept;

  // M1-ECS-04 query helpers: compile-time recursion over the listed
  // components (N ≤ 32 — the M1 bound). Recursion, not a fold: the
  // per-index component TYPE must reach a template argument, which a
  // fold's `...` does not expand (it only expands packs in the
  // operand expression).
  // The pack comes LAST in the template parameter list: an explicit
  // argument list cannot partition unambiguously when a pack is
  // followed by non-pack parameters (GCC gives the pack zero
  // arguments), so the fixed parameters go first.
  template <std::size_t I, std::size_t N, typename... Ts>
  void fillQueryIdImpl(std::uint32_t* ids) const noexcept {
    if constexpr (I < N) {
      ids[I] = componentIdOfKey(
          &detail::ComponentTypeKey<
              std::tuple_element_t<I, std::tuple<Ts...>>>::kMarker);
      this->template fillQueryIdImpl<I + 1, N, Ts...>(ids);
    }
  }

  // Resolve the world id of every listed query component into
  // `ids[I]` (0 when T is not registered in this world).
  template <typename... Ts>
  void fillQueryIds(std::uint32_t* ids) const noexcept {
    this->template fillQueryIdImpl<0, sizeof...(Ts), Ts...>(ids);
  }

  template <std::size_t I, std::size_t N, typename... Acc>
  void fillQueryReadSetImpl(detail::IdSet256& readSet,
                            const std::uint32_t* ids) const noexcept {
    if constexpr (I < N) {
      detail::setQueryReadBit<I, Acc...>(readSet, ids[I]);
      this->template fillQueryReadSetImpl<I + 1, N, Acc...>(readSet, ids);
    }
  }

  // Record every query component declared Read into `readSet` (the
  // guard's write-during-read set); ids[I] is the resolved id (0 =
  // unregistered: IdSet256::set is a no-op for it).
  template <typename... Acc>
  void fillQueryReadSet(detail::IdSet256& readSet,
                        const std::uint32_t* ids) const noexcept {
    this->template fillQueryReadSetImpl<0, sizeof...(Acc), Acc...>(
        readSet, ids);
  }

  // The recursive per-row reference builder (M1-ECS-04): I walks the
  // query components, and the references built so far ride along as
  // the Refs pack, DEDUCED from the call arguments (an explicitly
  // filled pack cannot also grow from arguments — hence the parameter
  // order: fixed template arguments first, F and Refs deduced last).
  // Refs&... is a pack of REFERENCE parameters (the row references
  // must reach the callback without a copy — PERF-005), and the
  // recursive call forwards refs... so the accumulated references
  // grow one per level. The component and access packs ride as
  // single tuple types (std::tuple<Ts...>, std::tuple<Acc...>): an
  // explicit template argument list cannot partition between two
  // consecutive packs, so no pack may sit in this template parameter
  // list (see rowRef).
  template <std::size_t I, std::size_t N, typename TList, typename AList,
            typename F, typename... Refs>
  void visitRowRec(F& fn, const Entity& entity, std::uint32_t row,
                   const std::uint32_t* cols, detail::ArchetypeRecord& arch,
                   Refs&... refs) const noexcept {
    if constexpr (I < N) {
      this->template visitRowRec<I + 1, N, TList, AList>(
          fn, entity, row, cols, arch, refs...,
          detail::rowRef<I, std::size_t, TList, AList>(row, cols, arch));
    } else {
      fn(entity, refs...);
    }
  }

  // Visit every row of `arch` (ascending row = ascending slot order),
  // invoking `fn(entity, row references...)` — the per-archetype row
  // loop of World::each. The component and access packs arrive as
  // single tuple types (see visitRowRec): an explicit template
  // argument list cannot partition between two consecutive packs.
  template <typename TList, typename AList, typename F>
  void visitArchetype(F& fn, detail::ArchetypeRecord& arch,
                      const std::uint32_t* cols) const noexcept {
    for (std::uint32_t row = 0; row < arch.size; ++row) {
      const std::uint16_t slot = arch.slotCol[row];
      const Entity entity{slot, generations_[slot]};
      this->template visitRowRec<0, std::tuple_size_v<TList>, TList, AList>(
          fn, entity, row, cols, arch);
    }
  }

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
  // Rows moved by attachSlot/removeRow tail shifts (ArchetypeStats feed;
  // the churn test's deterministic work KAT — archetype.h).
  std::uint64_t totalRowShifts_{0};
  // M1-ECS-06 guardrails (G-R3/G-R4; guardrails.cpp). Per-frame state
  // is reset by beginFrame(); the rest is since-construction. The
  // per-level arrays are indexed by level rank (0 = 25%, 1 = 50%,
  // 2 = 100% — see the header preamble "Guardrails").
  std::uint32_t churnPerFrameBudget_{0};
  std::uint32_t entityThreshold_[3]{};  // capacity * level / 100 (0: never fires)
  bool entityBudgetWarnedThisFrame_[3]{};  // G-R3 once-per-frame flags
  bool churnWarnedThisFrame_{false};  // G-R4 once-per-frame flag
  std::uint64_t frameAdds_{0};
  std::uint64_t frameRemoves_{0};
  std::uint32_t entityBudgetWarns_[3]{};  // per-level warn counts
  std::uint32_t churnWarns_{0};
  // Iteration-legality guard (M1-ECS-04; query.h): live while a
  // World::each runs, on the world's single owner thread. The matched
  // set names the archetypes the active query visits (complete before
  // the first callback); the read set names the queried components
  // declared Read (the in-place-write check). Both are membership-only
  // fixed 256-bit sets — never iterated (PERF-006).
  bool iterationActive_{false};
  detail::IdSet256 iterationArchetypes_{};
  detail::IdSet256 iterationReadComponents_{};
  // System registry (M1-SYS-01; system.h): the fixed engine budget
  // (kMaxSystems records), indexed by (system id - 1); a dense id is
  // assigned at registration (registration order, component.h
  // precedent). Setup state: clear() does not touch it (a system is
  // not per-entity data).
  std::unique_ptr<detail::SystemRecord[]> systems_;
  std::uint32_t systemCount_{0};
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
// System registry (M1-SYS-01). Header-defined like registerComponent:
// templates must be visible to every translation unit that registers a
// system. See system.h for the full contract (the validation order
// below, the I/O resolution, and the determinism note).
// ---------------------------------------------------------------------------

template <typename Tag>
detail::IoResolution World::resolveIoEntry(detail::IdSet256& read,
                                           detail::IdSet256& write,
                                           std::uint32_t* failedId) const noexcept {
  // Tag is an Io<C, Access> (the static_asserts in registerSystem).
  using C = detail::IoComponent<Tag>::type;
  const std::uint32_t id =
      componentIdOfKey(&detail::ComponentTypeKey<C>::kMarker);
  if (id == 0) return detail::IoResolution::Unregistered;
  if (read.contains(id) || write.contains(id)) {
    // The component is already declared by this system (any access
    // combination): the I/O is a set, not a multiset (system.h).
    *failedId = id;
    return detail::IoResolution::Duplicate;
  }
  if (detail::IoComponent<Tag>::access == Access::Read) read.set(id);
  else write.set(id);
  return detail::IoResolution::Ok;
}

template <typename... Ios>
Result<SystemId, ErrorCode> World::registerSystem(const SystemDef& def, Ios...) noexcept {
  // I/O pack validation (compile time, not runtime surprises):
  // every entry must be an Io<T, Access> tag and its component type
  // must be a Laige component (FR-1.2, S-8).
  static_assert((detail::IsIoTag<Ios>::value && ...),
                "registerSystem: every I/O entry must be an "
                "Io<T, Access> tag value (laige/sim/system.h)");
  static_assert((detail::IsIoComponent<Ios>::value && ...),
                "registerSystem: every Io<T, ...> component type must "
                "be marked with LAIGE_COMPONENT(T) (FR-1.2, S-8)");
  if (systems_ == nullptr) {
    // Moved-from world: no registry (the same "valid empty world"
    // contract as the component registry, component.h).
    return ErrorCode::InvalidArgument;
  }
  // The validation order is normative (system.h preamble): the def's
  // fields first, then the name uniqueness, then the I/O entries,
  // lastly the engine budget. Every failure is one rate-limited
  // structured warn + a Status (FR-12.3: never silent; LOG-004).
  if (def.name == nullptr || def.name[0] == '\0') {
    // The name cannot be logged (it is null or empty); the message
    // names the field, and the budget identifies the def.
    LAIGE_LOG_WARN("system", "name_invalid",
                   "System has no registration name (the def's name is "
                   "null or empty)",
                   laige::log::field("budget_raw", def.budgetMs.raw));
    return ErrorCode::InvalidArgument;
  }
  if (def.run == nullptr) {
    LAIGE_LOG_WARN("system", "run_invalid",
                   "System has no run function; build the def with "
                   "LAIGE_SYSTEM or set run explicitly",
                   laige::log::field("name", def.name));
    return ErrorCode::InvalidArgument;
  }
  if (def.budgetMs.raw <= 0) {
    // The budget must be explicit and strictly positive (FR-1.3;
    // fpx16_16 is exact, so raw <= 0 is exactly "not > 0 ms").
    LAIGE_LOG_WARN("system", "budget_invalid",
                   "System time budget must be explicit and > 0 ms",
                   laige::log::field("name", def.name),
                   laige::log::field("budget_raw", def.budgetMs.raw));
    return ErrorCode::InvalidArgument;
  }
  // The depends_on spec (M1-SYS-02, system.h "Scheduler"): a
  // malformed name list is a def-level defect — a registration error,
  // like a malformed name or budget. The names themselves are
  // resolved against this world at SCHEDULING time, so forward
  // dependencies (a system registered later) are legal.
  detail::DepSpecParse parsedDep;
  const detail::DepSpecError depErr =
      detail::parseDepSpec(def.dependsOn, &parsedDep);
  if (depErr != detail::DepSpecError::Ok) {
    LAIGE_LOG_WARN("system", "dep_spec_invalid",
                   "System depends_on spec is malformed (empty token, "
                   "duplicate name, or more than "
                   "kMaxSystemDependencies dependencies); fix the "
                   "LAIGE_SYSTEM depends_on list",
                   laige::log::field("name", def.name),
                   laige::log::field("error",
                                     detail::depSpecErrorName(depErr)));
    return ErrorCode::InvalidArgument;
  }
  for (std::uint32_t i = 0; i < systemCount_; ++i) {
    if (std::strcmp(systems_[i].def.name, def.name) == 0) {
      // Duplicate name is an error (M1-SYS-01 scope); the facade
      // rate-limits the warn per event (LOG-004).
      LAIGE_LOG_WARN("system", "duplicate",
                     "System name is already registered in this world; "
                     "duplicate names are an error",
                     laige::log::field("name", def.name),
                     laige::log::field("existing_system_id", i + 1u));
      return ErrorCode::InvalidArgument;
    }
  }
  // The I/O entries, resolved against this world's component registry
  // (per-world ids, component.h). The fold short-circuits on the first
  // failure (the sets are mutated only on Ok, so a failed registration
  // changes nothing). The unnamed Ios pack is intentional: only the
  // TYPES are used — a named pack would be an unreferenced parameter
  // (MSVC C4100, fatal under /WX; NFR-8.10), the each() precedent.
  detail::IdSet256 read, write;
  std::uint32_t failedId = 0;
  detail::IoResolution io = detail::IoResolution::Ok;
  ((io = (io == detail::IoResolution::Ok
                ? this->template resolveIoEntry<Ios>(read, write, &failedId)
                : io)),
   ...);
  if (io != detail::IoResolution::Ok) {
    if (io == detail::IoResolution::Unregistered) {
      LAIGE_LOG_WARN("system", "io_unregistered",
                     "System declares I/O for a component type that is "
                     "not registered in this world; register it at world "
                     "setup",
                     laige::log::field("name", def.name));
    } else {
      LAIGE_LOG_WARN("system", "io_duplicate",
                     "System declares the same component more than once "
                     "in its I/O list (any access combination)",
                     laige::log::field("name", def.name),
                     laige::log::field("component_id", failedId));
    }
    return ErrorCode::InvalidArgument;
  }
  if (systemCount_ >= kMaxSystems) {
    // The engine-level system budget (system.h preamble).
    LAIGE_LOG_WARN("system", "budget_exhausted",
                   "The world has kMaxSystems systems; a new system "
                   "cannot be registered (M1 bound - raise it through "
                   "an ADR)",
                   laige::log::field("name", def.name),
                   laige::log::field("systems", systemCount_));
    return ErrorCode::BudgetExhausted;
  }
  detail::SystemRecord& rec = systems_[systemCount_];
  rec.def = def;  // value copy: the user's def may be stack-scoped
  rec.readComponents = read;
  rec.writeComponents = write;
  const std::uint32_t id = systemCount_ + 1;
  ++systemCount_;
  return SystemId{id};  // ids are dense, from 1
}

// The context's delegated each (M1-SYS-01). Out-of-line here (not in
// system.h) because the delegated call is checked against the
// complete World: World is only forward-declared in system.h. The
// pack is named so its VALUES can be forwarded (an unnamed pack
// cannot be forwarded — C++ has no pack of packless values).
template <typename... Ts, typename... Acc, typename F>
[[nodiscard]] Status SystemContext::each(F&& fn, Acc... acc) noexcept {
  return world.each<Ts...>(std::forward<F>(fn), std::forward<Acc>(acc)...);
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
      // place (no archetype change, documented). M1-ECS-04: the
      // iteration-legality check runs first — an in-place write to a
      // component the active query declared Read is rejected (assert
      // in debug; Status + skip-with-log in release; query.h).
      Status guard = guardInplaceWrite(id, curIdx, entity);
      if (guard.isError()) return guard;
      const detail::ArchetypeColumn& column =
          archetypes_[curIdx - 1].columns[curCol];
      detail::copyRow(column.base + static_cast<std::size_t>(curRow) * column.size,
              reinterpret_cast<const std::byte*>(&value), sizeof(T));
      ++totalAdds_;
      // M1-ECS-06 (G-R4): an in-place overwrite is a counted add
      // (the ArchetypeStats::totalAdds semantics).
      ++frameAdds_;
      checkChurnBudget();
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
  // M1-ECS-04: the iteration-legality check runs before any side
  // effect — a rejected mutation creates no archetype and logs no
  // archetype_created (the target's id is known either way: the
  // created archetype would be the next dense id).
  const std::uint32_t targetArch = target != nullptr
      ? static_cast<std::uint32_t>(
            std::distance(archetypes_.get(), target)) + 1
      : archetypeCount_ + 1;
  {
    Status guard = guardStructural("addComponent", curIdx, targetArch, entity);
    if (guard.isError()) return guard;
  }
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
  // M1-ECS-06 (G-R4): the structural add is counted (above).
  ++frameAdds_;
  checkChurnBudget();
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
    // The entity's last component: it leaves the archetypes entirely
    // (target archetype 0 = "none"). M1-ECS-04: the iteration-
    // legality check runs first (query.h).
    {
      Status guard =
          guardStructural("removeComponent", curIdx, 0, entity);
      if (guard.isError()) return guard;
    }
    removeRow(cur, curRow);
    archetypeOf_[entity.id] = 0;
    rowOf_[entity.id] = 0;
    ++totalRemoves_;
    // M1-ECS-06 (G-R4): the detached row is a counted remove.
    ++frameRemoves_;
    checkChurnBudget();
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
  // M1-ECS-04: the iteration-legality check runs before any side
  // effect (same placement and reasoning as addComponent).
  {
    const std::uint32_t targetArch = target != nullptr
        ? static_cast<std::uint32_t>(
              std::distance(archetypes_.get(), target)) + 1
        : archetypeCount_ + 1;
    Status guard = guardStructural("removeComponent", curIdx, targetArch,
                                   entity);
    if (guard.isError()) return guard;
  }
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
  // M1-ECS-06 (G-R4): the detached row is a counted remove.
  ++frameRemoves_;
  checkChurnBudget();
  return Status{};
}

// ---------------------------------------------------------------------------
// Query API (M1-ECS-04; full contract in query.h). Header-defined like
// registerComponent: a template, visible to every translation unit.
// ---------------------------------------------------------------------------

// The access-tag values are unnamed (only their types are used — see the
// declaration's note); a named pack would trigger MSVC C4100 under /WX.
template <typename... Ts, typename... Acc, typename F>
Status World::each(F&& fn, Acc...) noexcept {
  static_assert(sizeof...(Ts) <= kMaxArchetypeComponents,
                "a query lists at most kMaxArchetypeComponents (32) "
                "components: no entity can carry more (the M1 bound — "
                "ADR to raise)");
  static_assert(sizeof...(Ts) == sizeof...(Acc),
                "World::each takes exactly one access tag per listed "
                "component, in the same order: "
                "each<T1, ..., TN>(Read/Write, ..., Read/Write, fn)");
  static_assert(detail::isAccessTags<Acc...>(),
                "World::each access tags must be laige::Read or "
                "laige::Write (one per listed component)");

  // M1-ECS-04: one active iteration per world — a nested each() is
  // rejected (assert in debug; Status + warn in release; query.h).
  Status start = guardIterationStart();
  if (start.isError()) return start;

  // Resolve the queried components (world ids; 0 = unregistered —
  // such a component matches nothing) and the read set (queried
  // components declared Read — the guard's write-during-read check).
  const std::size_t n = sizeof...(Ts);
  std::uint32_t ids[n == 0 ? 1 : n];
  detail::IdSet256 readSet{};
  if constexpr (n > 0) {
    this->template fillQueryIds<Ts...>(ids);
    this->template fillQueryReadSet<Acc...>(readSet, ids);
  }

  // The matched archetypes (superset match: every queried component is
  // in the archetype's set), in ascending archetype-id order
  // (= creation order — the visit order M1-ECS-05 documents). The
  // scan finishes before the first callback, so the guard's matched
  // set is complete for the whole iteration.
  std::uint32_t matchedIds[kMaxArchetypes];
  std::uint32_t matchedCount = 0;
  detail::IdSet256 matchedArchs{};
  if constexpr (n > 0) {
    for (std::uint32_t a = 0; a < archetypeCount_; ++a) {
      const detail::ArchetypeRecord& rec = archetypes_[a];
      bool matches = true;
      for (std::size_t i = 0; i < n; ++i) {
        if (columnIndexOf(rec, ids[i]) == kInvalidColumnIndex) {
          matches = false;
          break;
        }
      }
      if (matches) {
        matchedArchs.set(a + 1);
        matchedIds[matchedCount++] = a + 1;
      }
    }
  }

  // The guard is live from the first callback to the last (the
  // synchronous single-threaded flow: no early exit, no exceptions).
  iterationActive_ = true;
  iterationArchetypes_ = matchedArchs;
  iterationReadComponents_ = readSet;

  if constexpr (n == 0) {
    // The empty query: every live entity, ascending slot-id order
    // (the dense-id order M1-ECS-05 documents). It iterates no
    // archetype rows, so the guard's matched set stays empty and
    // structural mutations remain legal under it.
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
      if (alive_[slot] != 0) {
        fn(Entity{static_cast<std::uint16_t>(slot), generations_[slot]});
      }
    }
  } else {
    // Rows of every matched archetype, ascending row = ascending
    // slot order (the slot column is strictly ascending — invariant
    // I2). The column indices are resolved once per archetype.
    for (std::uint32_t mi = 0; mi < matchedCount; ++mi) {
      detail::ArchetypeRecord& arch = archetypes_[matchedIds[mi] - 1];
      std::uint32_t cols[n];
      for (std::size_t i = 0; i < n; ++i) {
        cols[i] = columnIndexOf(arch, ids[i]);  // present: it matched
      }
      this->template visitArchetype<std::tuple<Ts...>, std::tuple<Acc...>>(
          fn, arch, cols);
    }
  }

  // The guard releases with the iteration.
  iterationActive_ = false;
  iterationArchetypes_.clear();
  iterationReadComponents_.clear();
  return Status{};
}

}  // namespace laige
