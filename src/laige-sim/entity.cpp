// laige-sim World entity storage (M1-ECS-01).
//
// Implementation of the World declared in include/laige/sim/entity.h
// — see that header for the full contract (handle layout,
// stale-handle behavior, budget/allocation, ownership, threading,
// determinism).

#include "laige/sim/entity.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

#include "laige/logging.h"

namespace laige {

namespace {

// The stable subsystem name for ECS events (LOG-001).
inline constexpr const char* kEcsSubsystem = "ecs";

// One slot's bookkeeping footprint: generation (2 B) + alive flag
// (1 B) + free-list entry (2 B) + the M1-ECS-03 per-entity record
// (archetypeOf_ 2 B + rowOf_ 4 B). The archetype table and the SoA
// column blocks are not per-slot bytes — they are accounted separately
// in ArchetypeStats (archetype.h).
inline constexpr std::size_t kBytesPerSlot = 11;

}  // namespace

World::World() = default;

World::~World() noexcept { clear(); }

World::World(World&& other) noexcept
    : capacity_(other.capacity_),
      generations_(std::move(other.generations_)),
      alive_(std::move(other.alive_)),
      freeStack_(std::move(other.freeStack_)),
      freeCount_(other.freeCount_), inUse_(other.inUse_),
      peakInUse_(other.peakInUse_), totalCreated_(other.totalCreated_),
      components_(std::move(other.components_)),
      componentCount_(other.componentCount_),
      archetypeOf_(std::move(other.archetypeOf_)),
      rowOf_(std::move(other.rowOf_)),
      archetypes_(std::move(other.archetypes_)),
      archetypeCount_(other.archetypeCount_),
      keyIndex_(std::move(other.keyIndex_)),
      totalAdds_(other.totalAdds_), totalRemoves_(other.totalRemoves_),
      totalArchetypeGrowth_(other.totalArchetypeGrowth_),
      totalReservations_(other.totalReservations_) {
  other.capacity_ = 0;
  other.freeCount_ = 0;
  other.inUse_ = 0;
  other.peakInUse_ = 0;
  other.totalCreated_ = 0;
  other.componentCount_ = 0;
  other.archetypeCount_ = 0;
  other.totalAdds_ = 0;
  other.totalRemoves_ = 0;
  other.totalArchetypeGrowth_ = 0;
  other.totalReservations_ = 0;
}

World& World::operator=(World&& other) noexcept {
  if (this == &other) return *this;
  clear();  // release what this currently owns
  capacity_ = other.capacity_;
  generations_ = std::move(other.generations_);
  alive_ = std::move(other.alive_);
  freeStack_ = std::move(other.freeStack_);
  freeCount_ = other.freeCount_;
  inUse_ = other.inUse_;
  peakInUse_ = other.peakInUse_;
  totalCreated_ = other.totalCreated_;
  components_ = std::move(other.components_);
  componentCount_ = other.componentCount_;
  archetypeOf_ = std::move(other.archetypeOf_);
  rowOf_ = std::move(other.rowOf_);
  archetypes_ = std::move(other.archetypes_);
  archetypeCount_ = other.archetypeCount_;
  keyIndex_ = std::move(other.keyIndex_);
  totalAdds_ = other.totalAdds_;
  totalRemoves_ = other.totalRemoves_;
  totalArchetypeGrowth_ = other.totalArchetypeGrowth_;
  totalReservations_ = other.totalReservations_;
  other.capacity_ = 0;
  other.freeCount_ = 0;
  other.inUse_ = 0;
  other.peakInUse_ = 0;
  other.totalCreated_ = 0;
  other.componentCount_ = 0;
  other.archetypeCount_ = 0;
  other.totalAdds_ = 0;
  other.totalRemoves_ = 0;
  other.totalArchetypeGrowth_ = 0;
  other.totalReservations_ = 0;
  return *this;
}

Result<World, ErrorCode> World::create(Options options) noexcept {
  // The 16-bit id space addresses 65536 slots; a larger declared
  // budget is a configuration error (API-008: fail loudly at setup).
  if (options.capacity > Entity::kMaxEntities) {
    return ErrorCode::InvalidArgument;
  }
  World w;
  w.capacity_ = options.capacity;
  // Component registry table (M1-ECS-02): the fixed engine-level
  // budget (kMaxComponentTypes), a setup-path allocation like the
  // entity tables below.
  w.components_ = std::make_unique<detail::ComponentRecord[]>(kMaxComponentTypes);
  // Archetype storage (M1-ECS-03): the fixed archetype table
  // (kMaxArchetypes records, value-initialized) and the type-key
  // index (kComponentKeyIndexSize slots) — setup-path allocations,
  // allocated even for a zero-capacity world so a moved-from /
  // zero-capacity world stays a valid empty world with working
  // registry behavior.
  w.archetypes_ = std::make_unique<detail::ArchetypeRecord[]>(kMaxArchetypes);
  w.keyIndex_ =
      std::make_unique<detail::ComponentKeySlot[]>(detail::kComponentKeyIndexSize);
  if (w.capacity_ > 0) {
    // Backing allocations for the whole storage (setup path,
    // PERF-002): the per-slot generation table, the per-slot alive
    // flag, the LIFO free-list stack (pre-filled 0..capacity-1), and
    // the M1-ECS-03 per-slot entity record (archetype membership +
    // dense row; value-initialized: archetypeOf_ 0 = no archetype).
    w.generations_ = std::make_unique<std::uint16_t[]>(w.capacity_);
    w.alive_ = std::make_unique<std::uint8_t[]>(w.capacity_);
    w.freeStack_ = std::make_unique<std::uint16_t[]>(w.capacity_);
    w.archetypeOf_ = std::make_unique<std::uint16_t[]>(w.capacity_);
    w.rowOf_ = std::make_unique<std::uint32_t[]>(w.capacity_);
    for (std::uint32_t i = 0; i < w.capacity_; ++i) {
      w.generations_[i] = 1;  // generation 0 is reserved
      w.freeStack_[i] = static_cast<std::uint16_t>(i);
      ++w.freeCount_;
    }
  }
  return std::move(w);
}

Result<Entity, ErrorCode> World::create() noexcept {
  if (freeCount_ == 0) return ErrorCode::BudgetExhausted;
  const std::uint16_t slot = freeStack_[--freeCount_];
  alive_[slot] = 1;
  // A new entity carries no components: it is in no archetype.
  // (Cleared-slot leftovers are overwritten here — a recycled slot
  // always re-enters clean.)
  archetypeOf_[slot] = 0;
  rowOf_[slot] = 0;
  ++inUse_;
  if (inUse_ > peakInUse_) peakInUse_ = inUse_;
  ++totalCreated_;
  return Entity{slot, generations_[slot]};
}

bool World::isValid(Entity entity) const noexcept {
  return entity.id < capacity_ && entity.generation != 0 &&
         alive_[entity.id] != 0 &&
         generations_[entity.id] == entity.generation;
}

Status World::check(Entity entity) const noexcept {
  if (!isValid(entity)) {
    LAIGE_LOG_WARN(kEcsSubsystem, "stale_entity_access",
                   "Stale or out-of-range entity handle used for access",
                   laige::log::field("entity_id", entity.id),
                   laige::log::field("generation", entity.generation));
    return ErrorCode::InvalidArgument;
  }
  return Status{};
}

Status World::destroy(Entity entity) noexcept {
  // A stale/invalid handle here is a caller bug (S-9): debug builds
  // fail loudly; release builds degrade to a Status + warn-once
  // (FR-12.3, CORE-008: never silent).
  assert(isValid(entity) &&
         "World::destroy: stale or out-of-range entity handle "
         "(destroyed, cleared, or never created) — "
         "see docs/api/entity.md");
  if (!isValid(entity)) {
    LAIGE_LOG_WARN(kEcsSubsystem, "stale_entity_destroy",
                   "Stale or out-of-range entity handle used for destroy",
                   laige::log::field("entity_id", entity.id),
                   laige::log::field("generation", entity.generation));
    return ErrorCode::InvalidArgument;
  }
  // M1-ECS-03: release the entity's component row first — its slot
  // leaves the archetype (the row-stride move cost is documented in
  // archetype.h; no allocation).
  const std::uint32_t archIdx = archetypeOf_[entity.id];
  if (archIdx != 0) {
    removeRow(archetypes_[archIdx - 1], rowOf_[entity.id]);
    archetypeOf_[entity.id] = 0;
    rowOf_[entity.id] = 0;
  }
  alive_[entity.id] = 0;
  bumpGeneration(entity.id);
  freeStack_[freeCount_++] = entity.id;
  --inUse_;
  return Status{};
}

void World::clear() noexcept {
  // M1-ECS-03: every live entity is detached from its archetype first
  // (its component row is released with its slot); the archetypes
  // themselves and the component type registry survive (setup state).
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    if (alive_[i] != 0) {
      const std::uint32_t archIdx = archetypeOf_[i];
      if (archIdx != 0) {
        removeRow(archetypes_[archIdx - 1], rowOf_[i]);
      }
      archetypeOf_[i] = 0;
      rowOf_[i] = 0;
      alive_[i] = 0;
      bumpGeneration(static_cast<std::uint16_t>(i));
      freeStack_[freeCount_++] = static_cast<std::uint16_t>(i);
    }
  }
  inUse_ = 0;
}

std::uint32_t World::capacity() const noexcept { return capacity_; }

std::uint32_t World::entityCount() const noexcept { return inUse_; }

std::uint32_t World::componentCount() const noexcept {
  return componentCount_;
}

Result<ComponentInfo, ErrorCode> World::componentInfo(ComponentTypeId id) const noexcept {
  // Ids are dense from 1, so a valid registered id is exactly the
  // range [1, componentCount_] (M1-ECS-02; component.h contract).
  if (id.value == 0 || id.value > componentCount_ || components_ == nullptr) {
    return ErrorCode::InvalidArgument;
  }
  const detail::ComponentRecord& rec = components_[id.value - 1];
  return ComponentInfo{rec.size, rec.alignment};
}

EntityStats World::stats() const noexcept {
  return EntityStats{
      capacity_,  inUse_,         peakInUse_,   totalCreated_,
      static_cast<std::size_t>(capacity_) * kBytesPerSlot,
      static_cast<std::size_t>(inUse_) * kBytesPerSlot};
}

void World::bumpGeneration(std::uint16_t slot) noexcept {
  ++generations_[slot];  // defined unsigned wrap (CPP-004)
  if (generations_[slot] == 0) ++generations_[slot];  // skip reserved 0
}

}  // namespace laige
