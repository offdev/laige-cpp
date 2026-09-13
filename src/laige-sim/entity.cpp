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
// (1 B) + free-list entry (2 B). M1-ECS-03 adds the per-entity record
// to this number.
inline constexpr std::size_t kBytesPerSlot = 5;

}  // namespace

World::World() = default;

World::~World() noexcept { clear(); }

World::World(World&& other) noexcept
    : capacity_(other.capacity_),
      generations_(std::move(other.generations_)),
      alive_(std::move(other.alive_)),
      freeStack_(std::move(other.freeStack_)),
      freeCount_(other.freeCount_), inUse_(other.inUse_),
      peakInUse_(other.peakInUse_), totalCreated_(other.totalCreated_) {
  other.capacity_ = 0;
  other.freeCount_ = 0;
  other.inUse_ = 0;
  other.peakInUse_ = 0;
  other.totalCreated_ = 0;
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
  other.capacity_ = 0;
  other.freeCount_ = 0;
  other.inUse_ = 0;
  other.peakInUse_ = 0;
  other.totalCreated_ = 0;
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
  if (w.capacity_ > 0) {
    // Backing allocations for the whole storage (setup path,
    // PERF-002): the per-slot generation table, the per-slot alive
    // flag, and the LIFO free-list stack (pre-filled 0..capacity-1).
    w.generations_ = std::make_unique<std::uint16_t[]>(w.capacity_);
    w.alive_ = std::make_unique<std::uint8_t[]>(w.capacity_);
    w.freeStack_ = std::make_unique<std::uint16_t[]>(w.capacity_);
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
  alive_[entity.id] = 0;
  bumpGeneration(entity.id);
  freeStack_[freeCount_++] = entity.id;
  --inUse_;
  return Status{};
}

void World::clear() noexcept {
  // No per-slot element data yet (M1-ECS-03 adds the per-entity
  // record): clear is slot bookkeeping only.
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    if (alive_[i] != 0) {
      alive_[i] = 0;
      bumpGeneration(static_cast<std::uint16_t>(i));
      freeStack_[freeCount_++] = static_cast<std::uint16_t>(i);
    }
  }
  inUse_ = 0;
}

std::uint32_t World::capacity() const noexcept { return capacity_; }

std::uint32_t World::entityCount() const noexcept { return inUse_; }

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
