// laige-sim ECS guardrails suite (M1-ECS-06): the G-R3 entity-count
// thresholds and the G-R4 per-frame component-churn budget (PRD
// §9.3; roadmap/M1-heartbeat.md step M1-ECS-06).
//
// Step Verify scope:
//   - the entity-count guardrail warns exactly at 25%/50%/100% of the
//     declared scene budget, at most once per level per frame (no
//     duplicates), with a structured log + counter
//   - the per-frame component-churn counter warns when a frame's
//     adds+removes strictly exceed the configured budget, at most
//     once per frame; the "move to a spawn/despawn system" advice is
//     present in debug builds
//   - the warn messages follow the NFR-13.3 5-field error grammar
//     ({code} | {what} | {why} | {fix} | {doc_anchor})
//   - both guardrails are exposed to the profiler (M1-PROF-01)
//     through guardrailStats()
//   - the guardrail hot path allocates nothing below the thresholds
//     (the M1 zero-allocation property; M1-ALLOC-01's standing check
//     lands later)
//
// Runs as CTest `ecs_guardrails` (the step's Verify command:
// `ctest -R ecs_guardrails`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the EcsGuardrails
// suite below.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/sim/entity.h"

#if defined(LAIGE_ALLOC_COUNTER)
#include "logging_alloc_counter.h"
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "ecs_guardrails_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "ecs_guardrails_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "ecs_guardrails_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define GUARDRAILS_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define GUARDRAILS_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if GUARDRAILS_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "ecs_guardrails_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

// The two component types the churn scenarios move (S-8 data
// carriers, trivially copyable).
struct GuardPos {
  std::uint32_t x{};
  std::uint32_t y{};
};
LAIGE_COMPONENT(GuardPos)

struct GuardVel {
  std::uint32_t v{};
};
LAIGE_COMPONENT(GuardVel)

namespace {

// One world with a declared scene budget and a G-R4 churn budget,
// taken out of its Result (Result::value() is const; takeValue() &&
// moves the storage out — the documented ownership-transfer path).
laige::World makeWorld(std::uint32_t capacity, std::uint32_t churnBudget) {
  auto w = laige::World::create(
      laige::World::Options{capacity, churnBudget});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << capacity << ", " << churnBudget
                  << ") failed: " << laige::errorName(w.error());
    abort();
  }
  return std::move(w).takeValue();
}

// A test-only Sink that records every Warn-or-above event (the
// guardrail asserts count warn events; the Debug-level
// ecs/archetype_created events of the component scenarios are
// irrelevant here). The logging facade is a process singleton; each
// test owns its window and restores the default console sink at the
// end.
class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
  };

  void emit(const laige::log::LogRecord& record) override {
    if (record.severity < laige::log::Severity::Warn) return;
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    for (const auto& f : record.fields) {
      e.fields.emplace_back(std::string(f.name), f.value);
    }
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

// Installs a fresh capture sink with rate limiting OFF: the tests
// assert the SOURCE-level once-per-frame dedup (the guardrail's own
// flag), not the facade's LOG-004 window. The caller restores the
// default sink after the scenario (restoreLogger).
MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

std::size_t countEvents(const MemorySink& sink, std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == "ecs" && e.event == event) ++n;
  }
  return n;
}

bool hasField(const MemorySink::Entry& e, std::string_view key,
              std::string_view value) {
  for (const auto& [k, v] : e.fields) {
    if (k == key && v == value) return true;
  }
  return false;
}

bool hasField(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return true;
  }
  return false;
}

// Only used by the debug-only advice assertions below (NDEBUG builds
// never reference it — wrapping the definition keeps -Werror happy).
#ifndef NDEBUG
std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// G-R3: the entity-count thresholds (25%/50%/100% of the scene budget)
// ---------------------------------------------------------------------------

TEST(EcsGuardrails, EntityBudgetWarnsExactlyAtDocumentedPercentages) {
  MemorySink* sink = installCaptureSink();
  // capacity 8: the levels are 2, 4, and 8 live entities.
  laige::World world = makeWorld(8, laige::kDefaultChurnPerFrameBudget);
  world.beginFrame();

  // Below 25% (1 of 8 = 12.5%): no warn yet.
  ASSERT_TRUE(world.create().ok());
  EXPECT_EQ(sink->entries.size(), 0u);

  // Exactly 25% (2 of 8): one warn, no duplicates.
  ASSERT_TRUE(world.create().ok());
  EXPECT_EQ(sink->entries.size(), 1u);
  const auto& e25 = sink->entries[0];
  EXPECT_EQ(e25.severity, laige::log::Severity::Warn);
  EXPECT_EQ(e25.subsystem, "ecs");
  EXPECT_EQ(e25.event, "entity_budget_25");
  EXPECT_TRUE(hasField(e25, "entity_count", "2"));
  EXPECT_TRUE(hasField(e25, "capacity", "8"));
  EXPECT_TRUE(hasField(e25, "level", "25"));
#ifdef NDEBUG
  EXPECT_FALSE(hasField(e25, "advice"));  // the advice is debug-only
#else
  EXPECT_TRUE(hasField(e25, "advice"));
#endif

  // Exactly 50% (4 of 8).
  ASSERT_TRUE(world.create().ok());
  ASSERT_TRUE(world.create().ok());
  EXPECT_EQ(sink->entries.size(), 2u);
  EXPECT_EQ(sink->entries[1].event, "entity_budget_50");
  EXPECT_TRUE(hasField(sink->entries[1], "entity_count", "4"));

  // Exactly 100% (8 of 8): the last successful create.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(world.create().ok());
  }
  EXPECT_EQ(sink->entries.size(), 3u);
  EXPECT_EQ(sink->entries[2].event, "entity_budget_100");
  EXPECT_TRUE(hasField(sink->entries[2], "entity_count", "8"));

  // Over the budget: create fails; the 100% warn is not repeated.
  auto over = world.create();
  EXPECT_FALSE(over.ok());
  EXPECT_EQ(over.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(sink->entries.size(), 3u);

  // The guardrail counters (M1-PROF-01 feed).
  const auto s = world.guardrailStats();
  EXPECT_EQ(s.capacity, 8u);
  EXPECT_EQ(s.entityCount, 8u);
  EXPECT_EQ(s.entityBudgetLevel, 100u);
  EXPECT_EQ(s.entityBudgetWarns[0], 1u);
  EXPECT_EQ(s.entityBudgetWarns[1], 1u);
  EXPECT_EQ(s.entityBudgetWarns[2], 1u);
  EXPECT_EQ(s.frameChurn, 0u);
  EXPECT_EQ(s.churnPerFrameBudget, laige::kDefaultChurnPerFrameBudget);
  EXPECT_EQ(s.churnWarns, 0u);

  restoreLogger();
}

TEST(EcsGuardrails, EntityBudgetWarnsAtMostOncePerLevelPerFrame) {
  MemorySink* sink = installCaptureSink();
  // capacity 4: the 25% threshold is 1 live entity.
  laige::World world = makeWorld(4, laige::kDefaultChurnPerFrameBudget);
  world.beginFrame();

  // Frame 1: cross 25% — the warn fires.
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  EXPECT_EQ(countEvents(*sink, "entity_budget_25"), 1u);

  // Same frame: dip below the threshold, cross it again — NO second
  // warn (the roadmap's "no duplicates: warn once per level per
  // frame").
  ASSERT_TRUE(world.destroy(e).ok());
  auto r2 = world.create();
  ASSERT_TRUE(r2.ok());
  e = r2.value();
  EXPECT_EQ(countEvents(*sink, "entity_budget_25"), 1u);

  // Frame 2: dip below, cross again — the new frame's warn fires.
  world.beginFrame();
  ASSERT_TRUE(world.destroy(e).ok());
  auto r3 = world.create();
  ASSERT_TRUE(r3.ok());
  EXPECT_EQ(countEvents(*sink, "entity_budget_25"), 2u);

  EXPECT_EQ(world.guardrailStats().entityBudgetWarns[0], 2u);
  restoreLogger();
}

TEST(EcsGuardrails, EntityBudgetDegenerateThresholds) {
  // capacity 2: the 25% threshold is 0 (never fires); 50% is 1; 100%
  // is 2.
  MemorySink* sink = installCaptureSink();
  laige::World world = makeWorld(2, laige::kDefaultChurnPerFrameBudget);
  world.beginFrame();
  ASSERT_TRUE(world.create().ok());
  ASSERT_TRUE(world.create().ok());
  auto over = world.create();
  EXPECT_FALSE(over.ok());
  EXPECT_EQ(countEvents(*sink, "entity_budget_25"), 0u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_50"), 1u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_100"), 1u);
  restoreLogger();

  // capacity 1: only the 100% level (threshold 1) can fire.
  sink = installCaptureSink();
  world = makeWorld(1, laige::kDefaultChurnPerFrameBudget);
  world.beginFrame();
  ASSERT_TRUE(world.create().ok());
  EXPECT_EQ(countEvents(*sink, "entity_budget_100"), 1u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_25"), 0u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_50"), 0u);
  restoreLogger();

  // capacity 0: every create fails; nothing can fire.
  sink = installCaptureSink();
  world = makeWorld(0, laige::kDefaultChurnPerFrameBudget);
  world.beginFrame();
  auto r0 = world.create();
  EXPECT_FALSE(r0.ok());
  EXPECT_EQ(sink->entries.size(), 0u);
  restoreLogger();
}

TEST(EcsGuardrails, EntityBudgetFiresWithoutFrameBookkeeping) {
  // No beginFrame() call: setup-phase creates still trigger the warns.
  // The frame flags only DEDUPLICATE within a frame; they never gate
  // the first crossing (a world driven without frame boundaries
  // degrades to warn-once-per-lifetime — documented, never silent).
  MemorySink* sink = installCaptureSink();
  laige::World world = makeWorld(8, laige::kDefaultChurnPerFrameBudget);
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(world.create().ok());
  }
  EXPECT_EQ(countEvents(*sink, "entity_budget_25"), 1u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_50"), 1u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_100"), 1u);
  restoreLogger();
}

// ---------------------------------------------------------------------------
// G-R4: the per-frame component-churn budget
// ---------------------------------------------------------------------------

TEST(EcsGuardrails, ChurnWarnsExactlyWhenBudgetExceeded) {
  MemorySink* sink = installCaptureSink();
  // Budget 4: churn of exactly 4 does NOT warn (strictly-greater);
  // 5 does.
  laige::World world = makeWorld(16, 4);
  ASSERT_TRUE(world.registerComponent<GuardPos>().ok());
  ASSERT_TRUE(world.registerComponent<GuardVel>().ok());
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  world.beginFrame();

  auto addPos = [&] {
    return world.addComponent<GuardPos>(e, GuardPos{1, 2});
  };
  auto removePos = [&] { return world.removeComponent<GuardPos>(e); };

  // Four ops: churn 4 == budget — no warn yet.
  ASSERT_TRUE(addPos().ok());
  ASSERT_TRUE(removePos().ok());
  ASSERT_TRUE(addPos().ok());
  ASSERT_TRUE(removePos().ok());
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 0u);
  EXPECT_EQ(world.guardrailStats().frameChurn, 4u);

  // The fifth op: churn 5 > 4 — the warn fires, once.
  ASSERT_TRUE(addPos().ok());
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 1u);
  const auto& cw = sink->entries[0];
  EXPECT_EQ(cw.severity, laige::log::Severity::Warn);
  EXPECT_EQ(cw.subsystem, "ecs");
  EXPECT_EQ(cw.event, "churn_per_frame");
  EXPECT_TRUE(hasField(cw, "frame_churn", "5"));
  EXPECT_TRUE(hasField(cw, "churn_budget", "4"));
#ifdef NDEBUG
  EXPECT_FALSE(hasField(cw, "advice"));  // the advice is debug-only
#else
  // The PRD §9.3 G-R4 advice text (debug builds).
  EXPECT_TRUE(hasField(cw, "advice"));
  EXPECT_NE(fieldOf(cw, "advice").find("spawn/despawn system"),
            std::string::npos)
      << "advice: " << fieldOf(cw, "advice");
#endif

  // More churn in the same frame: no second warn.
  ASSERT_TRUE(removePos().ok());
  ASSERT_TRUE(addPos().ok());
  ASSERT_TRUE(removePos().ok());
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 1u);

  const auto s = world.guardrailStats();
  EXPECT_EQ(s.frameChurn, 8u);
  EXPECT_EQ(s.churnWarns, 1u);
  restoreLogger();
}

TEST(EcsGuardrails, ChurnCounterResetsAtFrameStart) {
  MemorySink* sink = installCaptureSink();
  laige::World world = makeWorld(16, 4);
  ASSERT_TRUE(world.registerComponent<GuardPos>().ok());
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }

  // Frame 1: 10 ops (5 add/remove pairs) — one warn at op 5
  // (churn 5 > budget 4).
  world.beginFrame();
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(
        world.addComponent<GuardPos>(
            e, GuardPos{static_cast<std::uint32_t>(i), 0})
            .ok());
    ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  }
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 1u);
  EXPECT_EQ(world.guardrailStats().frameChurn, 10u);

  // Frame 2: the counter starts at 0 again; the warn can fire a
  // second time in the new frame.
  world.beginFrame();
  EXPECT_EQ(world.guardrailStats().frameChurn, 0u);
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{0, 9}).ok());
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 1u);  // churn 1: no warn
  ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{1, 9}).ok());
  ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{2, 9}).ok());
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 2u);  // churn 5: warn

  EXPECT_EQ(world.guardrailStats().churnWarns, 2u);
  restoreLogger();
}

TEST(EcsGuardrails, ChurnNoOpRemoveIsNotCounted) {
  MemorySink* sink = installCaptureSink();
  laige::World world = makeWorld(16, 2);
  ASSERT_TRUE(world.registerComponent<GuardPos>().ok());
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  world.beginFrame();

  // A no-op remove (the entity lacks the component / has no
  // components) is an ok Status and does NOT count toward the churn
  // (the ArchetypeStats::totalRemoves semantics).
  EXPECT_TRUE(world.removeComponent<GuardPos>(e).ok());
  EXPECT_EQ(world.guardrailStats().frameChurn, 0u);
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 0u);

  // churn 1 (add), churn 2 (remove == budget: still no warn),
  // churn 3 (add > budget: the warn).
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{1, 1}).ok());
  ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 0u);
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{1, 1}).ok());
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 1u);
  restoreLogger();
}

TEST(EcsGuardrails, ChurnBudgetZeroDisablesTheGuardrail) {
  MemorySink* sink = installCaptureSink();
  laige::World world = makeWorld(16, 0);
  ASSERT_TRUE(world.registerComponent<GuardPos>().ok());
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  world.beginFrame();
  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE(
        world.addComponent<GuardPos>(
            e, GuardPos{static_cast<std::uint32_t>(i), 0})
            .ok());
    ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  }
  EXPECT_EQ(sink->entries.size(), 0u);
  EXPECT_EQ(world.guardrailStats().frameChurn, 60u);
  EXPECT_EQ(world.guardrailStats().churnWarns, 0u);
  restoreLogger();
}

// ---------------------------------------------------------------------------
// The NFR-13.3 message grammar + the profiler feed
// ---------------------------------------------------------------------------

TEST(EcsGuardrails, WarnMessagesFollowTheErrorGrammar) {
  // NFR-13.3: every engine error follows
  // {code} | {what} | {why} | {fix} | {doc_anchor} — the guardrail
  // warns carry the same 5-field line in their message text
  // (identical in every build; debug adds the advice FIELD, not
  // message text).
  MemorySink* sink = installCaptureSink();
  // capacity 8: creating 4 entities crosses 25% (2) and 50% (4);
  // budget 2: 3 component ops breach the churn.
  laige::World world = makeWorld(8, 2);
  ASSERT_TRUE(world.registerComponent<GuardPos>().ok());
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  world.beginFrame();
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(world.create().ok());
  }
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{1, 1}).ok());
  ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{1, 1}).ok());

  EXPECT_EQ(countEvents(*sink, "entity_budget_25"), 1u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_50"), 1u);
  EXPECT_EQ(countEvents(*sink, "churn_per_frame"), 1u);
  ASSERT_EQ(sink->entries.size(), 3u);

  for (const auto& entry : sink->entries) {
    // Split the message on " | ": exactly 5 fields, none empty.
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
      const std::size_t pos = entry.message.find(" | ", start);
      if (pos == std::string::npos) {
        fields.push_back(entry.message.substr(start));
        break;
      }
      fields.push_back(entry.message.substr(start, pos - start));
      start = pos + 3;
    }
    ASSERT_EQ(fields.size(), 5u) << "message: " << entry.message;
    for (const auto& f : fields) {
      EXPECT_FALSE(f.empty()) << "message: " << entry.message;
    }
    // {code} names the event; {doc_anchor} points at the entity docs.
    EXPECT_EQ(fields[0], entry.event);
    EXPECT_EQ(fields[4], "docs/api/entity.md#guardrails");
  }
  restoreLogger();
}

TEST(EcsGuardrails, GuardrailStatsTrackTheFrameWindow) {
  // The profiler feed (M1-PROF-01): the per-frame values are read
  // before the next beginFrame(); the warn counters are
  // since-construction.
  MemorySink* sink = installCaptureSink();
  laige::World world = makeWorld(8, 2);
  ASSERT_TRUE(world.registerComponent<GuardPos>().ok());
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  world.beginFrame();
  ASSERT_TRUE(world.create().ok());  // inUse 2: crosses 25% (threshold 2)
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{1, 1}).ok());
  ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{1, 1}).ok());
  const auto s = world.guardrailStats();
  EXPECT_EQ(s.capacity, 8u);
  EXPECT_EQ(s.entityCount, 2u);
  EXPECT_EQ(s.entityBudgetLevel, 25u);
  EXPECT_EQ(s.entityBudgetWarns[0], 1u);
  EXPECT_EQ(s.frameChurn, 3u);
  EXPECT_EQ(s.churnPerFrameBudget, 2u);
  EXPECT_EQ(s.churnWarns, 1u);

  // The next frame: the churn window restarts, the level is kept
  // (peak-based), the warn counts persist.
  world.beginFrame();
  const auto s2 = world.guardrailStats();
  EXPECT_EQ(s2.frameChurn, 0u);
  EXPECT_EQ(s2.entityBudgetLevel, 25u);
  EXPECT_EQ(s2.entityBudgetWarns[0], 1u);
  EXPECT_EQ(s2.churnWarns, 1u);
  EXPECT_EQ(sink->entries.size(), 2u);  // one entity-budget, one churn
  restoreLogger();
}

TEST(EcsGuardrails, GuardrailChecksAllocateNothingBelowTheThresholds) {
#if defined(LAIGE_ALLOC_COUNTER)
  MemorySink* sink = installCaptureSink();
  laige::World world = makeWorld(16, laige::kDefaultChurnPerFrameBudget);
  ASSERT_TRUE(world.registerComponent<GuardPos>().ok());
  laige::Entity e{};
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  // Setup (before the window): create the GuardPos archetype so the
  // window's adds are steady-state moves (no growth reservations).
  ASSERT_TRUE(world.addComponent<GuardPos>(e, GuardPos{0, 0}).ok());
  ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  // The world/sink/registry setup above allocated; the reset lands
  // between setup and the guarded window.
  laige::test::resetAllocCounter();
  world.beginFrame();
  // Stay below every threshold: 2 more creates (inUse 3 < the 25%
  // threshold of 4) and 8 component ops (churn 8 < budget 256) —
  // no warn fires, so no Field construction, so no heap.
  for (int i = 0; i < 2; ++i) {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(
        world.addComponent<GuardPos>(
            e, GuardPos{static_cast<std::uint32_t>(i), 0})
            .ok());
    ASSERT_TRUE(world.removeComponent<GuardPos>(e).ok());
  }
  EXPECT_EQ(sink->entries.size(), 0u);  // no warn fired
  // The guardrail hot path (the threshold comparisons per create, the
  // churn counters per add/remove) touches no heap: the M1
  // zero-allocation property (M1-ALLOC-01's standing assertion lands
  // later; ASan + this counter is the check until then).
  EXPECT_EQ(laige::test::allocCounter(), 0u);
  restoreLogger();
#else
  GTEST_SKIP() << "the allocation counter is excluded from the sanitizer "
                 "trees (their own new/delete interposes); the property "
                 "is covered there by the leak-free run.";
#endif
}
