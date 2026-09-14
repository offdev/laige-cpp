// laige-sim ECS guardrails (M1-ECS-06): the G-R3 entity-count
// thresholds and the G-R4 per-frame component-churn budget (PRD
// §9.3; FR-12.3, S-9).
//
// Implementation of the guardrail methods declared in
// include/laige/sim/entity.h — see that header (the preamble section
// "Guardrails") and docs/api/entity.md for the full contract:
//
//   G-R3  create() emits one structured warn exactly when the live
//         count reaches 25%/50%/100% of the declared scene budget
//         (integer thresholds capacity * pct / 100), at most once per
//         level per frame.
//   G-R4  the per-frame component add/remove count emits one
//         structured warn when it strictly exceeds
//         Options::churnPerFrameBudget (0 disables the guardrail),
//         at most once per frame.
//
// Message text follows the NFR-13.3 5-field error grammar
// ({code} | {what} | {why} | {fix} | {doc_anchor}) and is
// build-stable: machine-parseable output must not depend on the build
// type. Per PRD §9.3 ("warn (debug: with advice)"), the advice is
// carried as an extra structured FIELD in debug builds only — never
// as message text.
//
// Hot-path cost: three threshold comparisons per create(), one counter
// increment plus one comparison per counted add/remove — pure integer
// bookkeeping, no allocation (PERF-003). The warn paths are cold (a
// budget being crossed); their Field values construct only when the
// event is enabled (LOG-003).

#include "laige/sim/entity.h"

#include <cstdint>

#include "laige/logging.h"

namespace laige {

namespace {

// The stable subsystem name for ECS events (LOG-001; entity.cpp).
inline constexpr const char* kEcsSubsystem = "ecs";

// The G-R3 levels in ascending rank order (CORE-005: the single source
// for the thresholds, the event names, the level field, and the
// GuardrailStats index).
inline constexpr std::uint32_t kEntityBudgetLevels[3] = {25u, 50u, 100u};

// One stable event name per level (LOG-001: distinct per level so the
// facade's per-(subsystem, event, severity) rate limit cannot merge
// two levels' warns into one window).
inline constexpr const char* kEntityBudgetEvent[3] = {
    "entity_budget_25", "entity_budget_50", "entity_budget_100"};

// NFR-13.3 5-field grammar, identical in every build (machine-
// parseable, stable): {code} | {what} | {why} | {fix} | {doc_anchor}.
inline constexpr const char* kEntityBudgetMessage[3] = {
    "entity_budget_25 | live entities reached 25% of the declared scene "
    "budget | the scene budget is filling up; 75% of the budget remains "
    "| reduce simultaneous entity count or raise the scene budget through "
    "typed configuration (World::Options::capacity) | "
    "docs/api/entity.md#guardrails",
    "entity_budget_50 | live entities reached 50% of the declared scene "
    "budget | the scene budget is half full; create() starts failing "
    "with BudgetExhausted at 100% | profile the entity composition "
    "(M1-PROF-01) and reduce simultaneous entity count or raise the "
    "scene budget through typed configuration | "
    "docs/api/entity.md#guardrails",
    "entity_budget_100 | live entities reached 100% of the declared "
    "scene budget | the scene budget is full; the next create() fails "
    "with BudgetExhausted | destroy entities before spawning more, or "
    "raise the scene budget through typed configuration | "
    "docs/api/entity.md#guardrails",
};

#ifndef NDEBUG
// The PRD §9.3 debug-only advice (G-R3: "warn (debug: with advice)").
// A structured field, never message text: the 5-field grammar stays
// build-stable (NFR-13.3).
inline constexpr const char* kEntityBudgetAdvice[3] = {
    "headroom remains: no action needed yet — monitor the entity "
    "composition in the profiler (M1-PROF-01) before the 50% level",
    "profile the entity composition (M1-PROF-01 counters) before "
    "raising the budget — identify which systems hold the most live "
    "entities",
    "spawn paths now fail with BudgetExhausted — move spawning into a "
    "budgeted spawn/despawn system that reclaims entities before it "
    "spawns",
};
#endif

inline constexpr const char* kChurnEvent = "churn_per_frame";
inline constexpr const char* kChurnMessage =
    "churn_per_frame | component add/remove churn exceeded the per-frame "
    "budget | per-frame entity lifecycle churn is unbatched and spikes "
    "tick time | reduce per-frame add/remove churn (batch lifecycle "
    "work) | docs/api/entity.md#guardrails";

#ifndef NDEBUG
// The PRD §9.3 G-R4 advice text, debug-only (see the entity-budget
// note: a field, never message text).
inline constexpr const char* kChurnAdvice =
    "move the churn to a spawn/despawn system — batch entity lifecycle "
    "(create/destroy/addComponent/removeComponent) in one bounded "
    "per-frame system (PRD §9.3 G-R4)";
#endif

}  // namespace

void World::initEntityBudgetThresholds() noexcept {
  // capacity <= Entity::kMaxEntities (65536) and the levels are <=
  // 100, so capacity * level cannot overflow a uint32
  // (65536 * 100 = 6,553,600 < 2^32).
  for (std::uint32_t i = 0; i < 3; ++i) {
    entityThreshold_[i] = capacity_ * kEntityBudgetLevels[i] / 100u;
  }
}

void World::beginFrame() noexcept {
  // The per-frame guardrail state: the G-R4 churn counters restart at
  // 0 and the once-per-frame warn flags clear, so the next frame can
  // re-warn a still-broken level (and only a NEW crossing within that
  // frame warns — see checkEntityBudget).
  frameAdds_ = 0;
  frameRemoves_ = 0;
  for (std::uint32_t i = 0; i < 3; ++i) {
    entityBudgetWarnedThisFrame_[i] = false;
  }
  churnWarnedThisFrame_ = false;
}

GuardrailStats World::guardrailStats() const noexcept {
  // The highest budget percentage the PEAK live count reached: the
  // thresholds are monotone in the level, so a linear scan is exact
  // (a 0 threshold never matches: the peak is 0 only on an empty
  // world, and 0 >= 0 would be a false "reached").
  std::uint32_t level = 0;
  for (std::uint32_t i = 0; i < 3; ++i) {
    if (entityThreshold_[i] != 0 && peakInUse_ >= entityThreshold_[i]) {
      level = kEntityBudgetLevels[i];
    }
  }
  return GuardrailStats{
      capacity_,
      inUse_,
      level,
      {entityBudgetWarns_[0], entityBudgetWarns_[1], entityBudgetWarns_[2]},
      frameAdds_ + frameRemoves_,
      churnPerFrameBudget_,
      churnWarns_};
}

void World::checkEntityBudget() noexcept {
  // create() bumps inUse_ by exactly 1, so a level's threshold is
  // crossed exactly at inUse_ == threshold (no other value can be a
  // first reach). A 0 threshold never fires: the live count is 0 only
  // before the first create.
  for (std::uint32_t i = 0; i < 3; ++i) {
    if (entityThreshold_[i] != 0 && inUse_ == entityThreshold_[i] &&
        !entityBudgetWarnedThisFrame_[i]) {
      // Once per level per frame (the roadmap's "no duplicates"): the
      // flag suppresses a same-frame down-cross + up-cross; beginFrame
      // clears it for the next frame.
      entityBudgetWarnedThisFrame_[i] = true;
      ++entityBudgetWarns_[i];
#ifndef NDEBUG
      LAIGE_LOG_WARN(kEcsSubsystem, kEntityBudgetEvent[i],
                     kEntityBudgetMessage[i],
                     laige::log::field("entity_count", inUse_),
                     laige::log::field("capacity", capacity_),
                     laige::log::field("level", kEntityBudgetLevels[i]),
                     laige::log::field("advice", kEntityBudgetAdvice[i]));
#else
      LAIGE_LOG_WARN(kEcsSubsystem, kEntityBudgetEvent[i],
                     kEntityBudgetMessage[i],
                     laige::log::field("entity_count", inUse_),
                     laige::log::field("capacity", capacity_),
                     laige::log::field("level", kEntityBudgetLevels[i]));
#endif
    }
  }
}

void World::checkChurnBudget() noexcept {
  // 0 disables the guardrail (World::Options); the flag makes the warn
  // fire at most once per frame even while the churn stays above the
  // budget. Strictly-greater: churn == budget is legal, churn >
  // budget is the breach (PRD §9.3: "> threshold/tick").
  if (churnPerFrameBudget_ == 0 || churnWarnedThisFrame_) return;
  if (frameAdds_ + frameRemoves_ <= churnPerFrameBudget_) return;
  churnWarnedThisFrame_ = true;
  ++churnWarns_;
#ifndef NDEBUG
  LAIGE_LOG_WARN(kEcsSubsystem, kChurnEvent, kChurnMessage,
                 laige::log::field("frame_churn",
                                   frameAdds_ + frameRemoves_),
                 laige::log::field("churn_budget", churnPerFrameBudget_),
                 laige::log::field("advice", kChurnAdvice));
#else
  LAIGE_LOG_WARN(kEcsSubsystem, kChurnEvent, kChurnMessage,
                 laige::log::field("frame_churn",
                                   frameAdds_ + frameRemoves_),
                 laige::log::field("churn_budget", churnPerFrameBudget_));
#endif
}

}  // namespace laige
