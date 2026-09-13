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
//            Pool precedent). M1-ECS-03 adds the per-entity component
//            storage on top of this slot table.
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
#include <memory>

#include "laige/errors.h"
#include "laige/result.h"

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
  // O(1), no allocation. The slot's generation is bumped, so every
  // stale handle to it fails isValid() (CPP-007). Stale/invalid
  // handle: debug -> assert (S-9); release -> ErrorCode::InvalidArgument
  // + one rate-limited warn (FR-12.3: never silent).
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

  // Destroy every live entity (shutdown path, CONC-006). Every handle
  // becomes stale; the capacity is unchanged and the world is
  // immediately reusable. O(capacity) scan, no allocation, idempotent.
  // (No per-slot element data exists yet; M1-ECS-03 adds the
  // per-entity record that clear() will then destroy.)
  void clear() noexcept;

  // Move is an O(1) pointer swap; the source becomes a valid empty
  // world (capacity 0: every create() fails, every handle invalid).
  World(World&& other) noexcept;
  World& operator=(World&& other) noexcept;
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Destroys nothing per element yet (no per-slot element data);
  // releases the backing storage. Idempotent with clear().
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

  std::uint32_t capacity_{0};
  std::unique_ptr<std::uint16_t[]> generations_;
  std::unique_ptr<std::uint8_t[]> alive_;
  std::unique_ptr<std::uint16_t[]> freeStack_;
  std::uint32_t freeCount_{0};
  std::uint32_t inUse_{0};
  std::uint32_t peakInUse_{0};
  std::uint64_t totalCreated_{0};
};

}  // namespace laige
