// laige-sim World iteration-legality guard (M1-ECS-04).
//
// Implementation of the guard helpers declared in
// include/laige/sim/entity.h — see include/laige/sim/query.h for the
// full contract (query semantics, the legality rules, the guard
// lifecycle, the error table, the no-allocation guarantee).
//
// Behavior of a violation (FR-12.3, CORE-008, S-9): the assert fires in
// debug builds (a loud SIGABRT with an actionable message); release
// builds log one rate-limited structured Warn (LOG-004) and return
// ErrorCode::InvalidArgument, and the caller skips the mutation —
// never applied, never silent.

#include "laige/sim/entity.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "laige/logging.h"

namespace laige {

namespace {

// The stable subsystem name for ECS events (LOG-001).
inline constexpr const char* kEcsSubsystem = "ecs";

}  // namespace

Status World::guardIterationStart() noexcept {
  if (iterationActive_) {
    assert(!iterationActive_ &&
           "World::each: a nested iteration started while another "
           "iteration is active — one active iteration per world "
           "(M1-ECS-04, query.h); end the outer iteration before "
           "starting another");
    LAIGE_LOG_WARN(kEcsSubsystem, "iteration_nested",
                   "A World::each iteration started while another "
                   "iteration is active; the nested iteration was "
                   "rejected");
    return ErrorCode::InvalidArgument;
  }
  return Status{};
}

Status World::guardInplaceWrite(std::uint32_t componentId,
                                std::uint32_t archIdx,
                                Entity entity) noexcept {
  if (iterationActive_ && iterationArchetypes_.contains(archIdx) &&
      iterationReadComponents_.contains(componentId)) {
    assert(false &&
           "World::addComponent: an in-place write to a component the "
           "active World::each iteration declared Read (a write during "
           "a read iteration — M1-ECS-04, query.h); declare Write "
           "access for the component in the query, or perform the "
           "update outside the iteration");
    LAIGE_LOG_WARN(kEcsSubsystem, "iteration_write_during_read",
                   "An in-place component write was rejected: the "
                   "component is declared Read for the active "
                   "World::each iteration; the mutation was skipped",
                   laige::log::field("entity_id", entity.id),
                   laige::log::field("generation", entity.generation),
                   laige::log::field("component_id", componentId),
                   laige::log::field("archetype_id", archIdx));
    return ErrorCode::InvalidArgument;
  }
  return Status{};
}

Status World::guardStructural(const char* op, std::uint32_t sourceArch,
                              std::uint32_t targetArch,
                              Entity entity) noexcept {
  // A structural change (an archetype move, a destroy, or a clear —
  // the caller names the operation in `op`) is illegal when its
  // source OR target archetype is in the active iteration's matched
  // set: the tail of an iterated archetype would shift under the
  // iterator (M1-ECS-04, query.h). 0 = "no archetype" and is never a
  // member of the matched set.
  if (iterationActive_ &&
      (iterationArchetypes_.contains(sourceArch) ||
       iterationArchetypes_.contains(targetArch))) {
    assert(false &&
           "World component mutation ("
           "addComponent/removeComponent/destroy): a structural "
           "change — an archetype move, or a destroy — while the "
           "affected archetype is being iterated by World::each "
           "(M1-ECS-04, query.h); perform entity lifecycle in a "
           "spawn/despawn system, or end the iteration first");
    LAIGE_LOG_WARN(kEcsSubsystem, "iteration_mutation",
                   "A structural component mutation was rejected while "
                   "the affected archetype is being iterated; the "
                   "mutation was skipped",
                   laige::log::field("op", op),
                   laige::log::field("entity_id", entity.id),
                   laige::log::field("generation", entity.generation),
                   laige::log::field("source_archetype", sourceArch),
                   laige::log::field("target_archetype", targetArch));
    return ErrorCode::InvalidArgument;
  }
  return Status{};
}

Status World::guardClear() noexcept {
  if (!iterationActive_) return Status{};
  // clear() detaches every live row; it is illegal exactly when a
  // matched archetype of the active iteration still holds live rows
  // (an O(kMaxArchetypes) bounded pass over the fixed table — clear()
  // is a shutdown-path operation, never a hot path).
  for (std::uint32_t i = 0; i < archetypeCount_; ++i) {
    if (iterationArchetypes_.contains(i + 1) && archetypes_[i].size > 0) {
      assert(false &&
             "World::clear: clearing entities of an archetype currently "
             "being iterated by World::each (M1-ECS-04, query.h); end "
             "the iteration before clearing the world");
      LAIGE_LOG_WARN(kEcsSubsystem, "iteration_clear",
                     "clear() was rejected: live entities of an "
                     "archetype being iterated by World::each exist; "
                     "the clear was skipped",
                     laige::log::field("archetype_id", i + 1),
                     laige::log::field("rows", archetypes_[i].size));
      return ErrorCode::InvalidArgument;
    }
  }
  return Status{};
}

}  // namespace laige
