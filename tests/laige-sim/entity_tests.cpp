// laige-sim entity handle + World entity storage suite (M1-ECS-01).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - reuse after destroy bumps generation (CPP-007)
//   - stale access is detected: queries degrade safely in every build
//     (isValid false, check -> InvalidArgument + warn-once); using a
//     stale handle asserts in debug (forked SIGABRT child) and returns
//     Status in release (FR-12.3, S-9)
//   - capacity limit honored: create() beyond the declared scene
//     budget (G-R3) returns BudgetExhausted; an over-capacity world
//     construction returns InvalidArgument (API-008)
//   - the documented 16-bit generation wrap is pinned, not assumed
//   - accounting (stats()) feeds the G-R3 guardrail / profiler
//
// Runs as CTest `entity` (the step's Verify command: `ctest -R entity`,
// green under ASan per the step's Verify clause): a filtered view of
// the shared laige-sim_tests executable, selecting exactly the suites
// below.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/sim/entity.h"

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "entity_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "entity_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "entity_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define ENTITY_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define ENTITY_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if ENTITY_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "entity_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

namespace {

// One world, taken out of its Result so the test holds a mutable
// lvalue (Result::value() is const; takeValue() && moves the storage
// out — the documented ownership-transfer path).
laige::World makeWorld(std::uint32_t capacity) {
  auto w = laige::World::create(laige::World::Options{capacity});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << capacity
                  << ") failed: " << laige::errorName(w.error());
    abort();
  }
  return std::move(w).takeValue();
}

}  // namespace

// ---------------------------------------------------------------------------
// The 32-bit handle (FR-1.2, CPP-007)
// ---------------------------------------------------------------------------

TEST(EntityHandle, HandleIs32Bits) {
  static_assert(sizeof(laige::Entity) == 4,
                "Entity must be a 32-bit handle (FR-1.2)");
  const laige::Entity e{};
  EXPECT_EQ(e.id, 0u);
  EXPECT_EQ(e.generation, 0u);
}

TEST(EntityHandle, EqualityComparesThePair) {
  const laige::Entity a{1, 2};
  const laige::Entity b{1, 2};
  const laige::Entity c{1, 3};
  const laige::Entity d{2, 2};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(a, d);
  EXPECT_NE(laige::Entity{}, a);
}

// ---------------------------------------------------------------------------
// World entity storage: create/destroy, LIFO recycling, clear, move
// ---------------------------------------------------------------------------

TEST(WorldBasics, CreateAssignsLifoSlotsAndCounts) {
  // LIFO free-list recycling: the free stack is pre-filled
  // 0..capacity-1, so slots are handed out from the top (capacity-1
  // down) — deterministic for a given operation sequence (ARCH-010;
  // see the header).
  laige::World world = makeWorld(6);
  laige::Entity a, b, c;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    a = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    b = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    c = r.value();
  }
  EXPECT_EQ(a.id, 5u);
  EXPECT_EQ(b.id, 4u);
  EXPECT_EQ(c.id, 3u);
  EXPECT_EQ(a.generation, 1u);
  EXPECT_EQ(b.generation, 1u);
  EXPECT_EQ(c.generation, 1u);
  EXPECT_EQ(world.entityCount(), 3u);
  EXPECT_TRUE(world.isValid(a));
  EXPECT_TRUE(world.isValid(b));
  EXPECT_TRUE(world.isValid(c));
}

TEST(WorldBasics, GenerationBumpsOnReuseAfterDestroy) {
  // The roadmap's named property: reusing a destroyed entity's slot
  // bumps its generation (CPP-007), so the stale handle can never pass
  // isValid() again (short of the documented 2^16 wrap — see
  // GenerationWrapDocumentedCollision).
  laige::World world = makeWorld(1);
  auto r1 = world.create();
  ASSERT_TRUE(r1.ok());
  const laige::Entity stale = r1.value();
  ASSERT_TRUE(world.destroy(stale).ok());
  auto r2 = world.create();
  ASSERT_TRUE(r2.ok());
  const laige::Entity live = r2.value();
  EXPECT_EQ(live.id, stale.id);
  EXPECT_EQ(live.generation, stale.generation + 1);
  EXPECT_FALSE(world.isValid(stale));
  EXPECT_TRUE(world.isValid(live));
}

TEST(WorldBasics, LifoRecyclingOrder) {
  // The most recently freed slot is recycled first (LIFO); untouched
  // entities keep their slots and generations.
  laige::World world = makeWorld(6);
  laige::Entity a, b, c;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    a = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    b = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    c = r.value();
  }
  ASSERT_TRUE(world.destroy(b).ok());
  auto r4 = world.create();
  ASSERT_TRUE(r4.ok());
  const laige::Entity d = r4.value();
  EXPECT_EQ(d.id, b.id);
  EXPECT_EQ(d.generation, b.generation + 1);
  EXPECT_TRUE(world.isValid(a));
  EXPECT_TRUE(world.isValid(c));
}

TEST(WorldBasics, ClearInvalidatesAllAndIsReusable) {
  laige::World world = makeWorld(3);
  laige::Entity e0, e1, e2;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e0 = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e1 = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e2 = r.value();
  }
  world.clear();
  EXPECT_EQ(world.entityCount(), 0u);
  EXPECT_FALSE(world.isValid(e0));
  EXPECT_FALSE(world.isValid(e1));
  EXPECT_FALSE(world.isValid(e2));
  // The capacity is unchanged and the world is immediately reusable;
  // every cleared slot's generation was bumped.
  EXPECT_EQ(world.capacity(), 3u);
  auto r = world.create();
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().id, 2u);  // LIFO: the top of the cleared stack
  EXPECT_EQ(r.value().generation, 2u);
  world.clear();  // idempotent
  EXPECT_EQ(world.entityCount(), 0u);
}

TEST(WorldBasics, MoveTransfersStorageAndEmptiesSource) {
  laige::World worldA = makeWorld(4);
  laige::Entity e1, e2;
  {
    auto r = worldA.create();
    ASSERT_TRUE(r.ok());
    e1 = r.value();
  }
  {
    auto r = worldA.create();
    ASSERT_TRUE(r.ok());
    e2 = r.value();
  }
  laige::World worldB = std::move(worldA);
  EXPECT_EQ(worldB.capacity(), 4u);
  EXPECT_EQ(worldB.entityCount(), 2u);
  EXPECT_TRUE(worldB.isValid(e1));
  EXPECT_TRUE(worldB.isValid(e2));
  EXPECT_TRUE(worldB.create().ok());
  // The moved-from world is a valid empty world (capacity 0).
  EXPECT_EQ(worldA.capacity(), 0u);
  EXPECT_EQ(worldA.entityCount(), 0u);
  EXPECT_FALSE(worldA.isValid(e1));
  EXPECT_FALSE(worldA.isValid(e2));
  auto r = worldA.create();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::BudgetExhausted);
}

TEST(WorldBasics, MoveAssignmentTakesOverAndEmptiesSource) {
  laige::World worldA = makeWorld(2);
  laige::World worldB = makeWorld(3);
  ASSERT_TRUE(worldA.create().ok());  // one live entity in A
  laige::Entity moved;
  {
    auto r = worldB.create();
    ASSERT_TRUE(r.ok());
    moved = r.value();
  }
  worldA = std::move(worldB);
  EXPECT_EQ(worldA.capacity(), 3u);
  EXPECT_EQ(worldA.entityCount(), 1u);
  EXPECT_TRUE(worldA.isValid(moved));
  EXPECT_EQ(worldB.capacity(), 0u);
  EXPECT_EQ(worldB.entityCount(), 0u);
  EXPECT_FALSE(worldB.isValid(moved));
}

// ---------------------------------------------------------------------------
// Capacity: the declared scene budget (G-R3)
// ---------------------------------------------------------------------------

TEST(WorldCapacity, CapacityLimitHonored) {
  // The roadmap's named property: the declared scene budget is the
  // hard ceiling — create() beyond it returns BudgetExhausted (the
  // world never grows silently, S-2/G-R1).
  laige::World world = makeWorld(3);
  laige::Entity e0, e1, e2;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e0 = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e1 = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e2 = r.value();
  }
  EXPECT_EQ(world.entityCount(), 3u);
  auto full = world.create();
  EXPECT_FALSE(full.ok());
  EXPECT_EQ(full.error(), laige::ErrorCode::BudgetExhausted);
  // Reclaiming a slot keeps the world within the budget.
  ASSERT_TRUE(world.destroy(e1).ok());
  auto recycled = world.create();
  EXPECT_TRUE(recycled.ok());
  EXPECT_EQ(world.entityCount(), 3u);
}

TEST(WorldCapacity, ZeroCapacityRefusesEveryCreate) {
  // A budget of 0 is legal (every create() fails) — the pool
  // precedent (M0-CORE-05).
  laige::World world = makeWorld(0);
  EXPECT_EQ(world.capacity(), 0u);
  auto r = world.create();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_FALSE(world.isValid(laige::Entity{}));
  world.clear();  // idempotent on an empty world
  EXPECT_EQ(world.entityCount(), 0u);
}

TEST(WorldCapacity, OverCapacityIsInvalidArgument) {
  // The 16-bit id space addresses 65536 slots; a declared budget above
  // that is a configuration error, rejected at construction (API-008:
  // the invalid state stays unrepresentable).
  auto over =
      laige::World::create(laige::World::Options{laige::Entity::kMaxEntities + 1});
  EXPECT_FALSE(over.ok());
  EXPECT_EQ(over.error(), laige::ErrorCode::InvalidArgument);
  // The boundary itself is legal.
  auto max =
      laige::World::create(laige::World::Options{laige::Entity::kMaxEntities});
  EXPECT_TRUE(max.ok());
  EXPECT_EQ(max.value().capacity(), laige::Entity::kMaxEntities);
}

// ---------------------------------------------------------------------------
// Stale-handle behavior (FR-12.3, S-9, CORE-008)
// ---------------------------------------------------------------------------

TEST(WorldStale, IsFalseForStaleClearedOutOrRangeAndDefault) {
  laige::World world = makeWorld(2);
  laige::Entity e0, e1;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e0 = r.value();
  }
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e1 = r.value();
  }
  // Destroyed (stale):
  ASSERT_TRUE(world.destroy(e1).ok());
  EXPECT_FALSE(world.isValid(e1));
  // Out-of-range id (beyond the declared budget):
  EXPECT_FALSE(world.isValid(laige::Entity{
      static_cast<std::uint16_t>(world.capacity()), 1}));
  // Generation 0 is reserved:
  EXPECT_FALSE(world.isValid(laige::Entity{0, 0}));
  // The default handle is never valid:
  EXPECT_FALSE(world.isValid(laige::Entity{}));
  // Cleared:
  world.clear();
  EXPECT_FALSE(world.isValid(e0));
}

TEST(WorldStale, CheckStaleReturnsInvalidArgument) {
  // check() is the safe access validation (M1-ECS-03's component
  // access builds on it): in every build — not only release — a stale
  // handle yields Status InvalidArgument + a rate-limited warn, never
  // silent (FR-12.3, CORE-008).
  laige::World world = makeWorld(2);
  auto r = world.create();
  ASSERT_TRUE(r.ok());
  const laige::Entity e = r.value();
  EXPECT_TRUE(world.check(e).ok());
  ASSERT_TRUE(world.destroy(e).ok());
  auto s = world.check(e);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  // An out-of-range handle fails the same way.
  auto s2 = world.check(laige::Entity{
      static_cast<std::uint16_t>(world.capacity()), 1});
  EXPECT_FALSE(s2.ok());
  EXPECT_EQ(s2.error(), laige::ErrorCode::InvalidArgument);
}

TEST(WorldStale, DestroyStaleReturnsInvalidArgumentInRelease) {
  // The release degradation path of destroy() (FR-12.3). Debug builds
  // assert instead (DestroyStaleAbortsInDebug below); the same
  // validation is exercised in every build via check() above.
#ifdef NDEBUG
  laige::World world = makeWorld(1);
  auto r = world.create();
  ASSERT_TRUE(r.ok());
  const laige::Entity e = r.value();
  ASSERT_TRUE(world.destroy(e).ok());
  auto s = world.destroy(e);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
#else
  GTEST_SKIP() << "destroy() on a stale handle asserts in debug builds "
                 "(S-9); the release Status path is covered by "
                 "CheckStaleReturnsInvalidArgument in every build.";
#endif
}

TEST(WorldStale, DestroyStaleAbortsInDebug) {
  // The step's Verify clause "debug stale-handle test asserts" (S-9):
  // using a stale handle through destroy() must fail loudly in debug
  // builds. Exercised in a forked child so the test process survives:
  // the child must die on SIGABRT. POSIX only (fork); the Windows jobs
  // skip with a reason (the pools_tests.cpp pattern).
#if defined(__unix__)
#  if defined(NDEBUG)
  GTEST_SKIP() << "assert-based stale detection is a debug-build property";
#  else
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    auto w = laige::World::create(laige::World::Options{1});
    if (!w.ok()) _exit(117);
    laige::World world = std::move(w).takeValue();
    auto r = world.create();
    if (!r.ok() || !world.destroy(r.value()).ok()) _exit(117);
    // Stale: the debug assert inside destroy() must fire before this
    // statement completes.
    (void)world.destroy(r.value());
    _exit(1);  // unreachable: the parent fails below without the assert
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
      << "expected the stale-handle assert to abort the child (SIGABRT)";
#  endif
#else
  GTEST_SKIP() << "fork() is not available on Windows; the stale-handle "
                 "assert is exercised on the POSIX jobs.";
#endif
}

TEST(WorldStale, GenerationWrapDocumentedCollision) {
  // The documented 16-bit generation bound: after 2^16 releases of one
  // slot the generation wraps through the reserved 0 and re-enters at
  // generation 1 — colliding with the slot's first incarnation, whose
  // stale handle can no longer be distinguished. This is the one case
  // the scheme does not rule out (see the header; the M0-CORE-05
  // precedent documents the same case at the 2^32 bound). The test
  // pins the wrap behavior instead of assuming it.
  laige::World world = makeWorld(1);
  auto first = world.create();
  ASSERT_TRUE(first.ok());
  const laige::Entity firstHandle = first.value();
  EXPECT_EQ(firstHandle.generation, 1u);
  // The first release takes the generation 1 -> 2; the remaining
  // 2^16 - 2 releases walk 2 -> 0xFFFF -> (wrap through the reserved
  // 0) -> 1.
  ASSERT_TRUE(world.destroy(firstHandle).ok());
  for (std::uint32_t i = 0; i + 2 < laige::Entity::kMaxEntities; ++i) {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(world.destroy(r.value()).ok());
  }
  auto last = world.create();
  ASSERT_TRUE(last.ok());
  const laige::Entity lastHandle = last.value();
  EXPECT_EQ(lastHandle.generation, 1u);
  EXPECT_TRUE(world.isValid(lastHandle));
  // The documented collision: the first incarnation's stale handle
  // matches the wrapped generation again.
  EXPECT_TRUE(world.isValid(firstHandle));
}

// ---------------------------------------------------------------------------
// Accounting (FR-11.1/FR-11.4; G-R3 and M1-PROF-01 feed)
// ---------------------------------------------------------------------------

TEST(WorldStats, StatsTrackCountsPeakAndChurn) {
  laige::World world = makeWorld(4);
  laige::Entity e[4];
  for (int i = 0; i < 4; ++i) {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e[i] = r.value();
  }
  auto s = world.stats();
  EXPECT_EQ(s.capacity, 4u);
  EXPECT_EQ(s.inUse, 4u);
  EXPECT_EQ(s.peakInUse, 4u);
  EXPECT_EQ(s.totalCreated, 4u);
  // A create immediately destroyed still counts as churn (G-R4).
  ASSERT_TRUE(world.destroy(e[2]).ok());
  auto r = world.create();
  ASSERT_TRUE(r.ok());
  s = world.stats();
  EXPECT_EQ(s.inUse, 4u);
  EXPECT_EQ(s.peakInUse, 4u);
  EXPECT_EQ(s.totalCreated, 5u);
  ASSERT_TRUE(world.destroy(e[0]).ok());
  ASSERT_TRUE(world.destroy(e[1]).ok());
  ASSERT_TRUE(world.destroy(e[3]).ok());
  s = world.stats();
  EXPECT_EQ(s.inUse, 1u);
  EXPECT_EQ(s.peakInUse, 4u);
  EXPECT_EQ(s.totalCreated, 5u);
}

TEST(WorldStats, StatsTrackBytes) {
  laige::World world = makeWorld(4);
  auto r = world.create();
  ASSERT_TRUE(r.ok());
  const auto s = world.stats();
  // Per-slot footprint: 11 B bookkeeping (2 B generation + 1 B alive
  // flag + 2 B free-list entry + 2 B archetype slot + 4 B row index —
  // M1-ECS-03) — see the header and entity.md.
  EXPECT_EQ(s.bytesCapacity, 4u * 11u);
  EXPECT_EQ(s.bytesInUse, 1u * 11u);
}

// ---------------------------------------------------------------------------
// Stale-access logging: warn-once (LOG-004) through the facade
// ---------------------------------------------------------------------------

namespace {

// A test-only Sink that records every emitted event (the logging
// facade is a process singleton; this test owns its window and
// restores the default console sink at the end).
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

}  // namespace

TEST(WorldLogging, StaleAccessWarnsOnce) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();

  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);  // the burst stays in-window
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(1);
  auto created = world.create();
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(world.destroy(created.value()).ok());
  const laige::Entity stale = created.value();

  // Three stale accesses within the window: the first emits the warn,
  // the other two are suppressed and counted (LOG-004: the
  // rate_limited summary carries the count).
  for (int i = 0; i < 3; ++i) {
    const auto s = world.check(stale);
    EXPECT_FALSE(s.ok());
    if (s.isError()) {
      EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
    }
  }

  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "ecs");
  EXPECT_EQ(sinkPtr->entries[0].event, "stale_entity_access");

  // Controlled shutdown drains the pending rate-limit summary
  // (CONC-006/LOG-007/LOG-004).
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(sinkPtr->entries.size(), 2u);
  EXPECT_EQ(sinkPtr->entries[1].event, laige::log::kRateLimitedEvent);
  bool foundEventField = false;
  bool foundCountField = false;
  for (const auto& [key, value] : sinkPtr->entries[1].fields) {
    if (key == "event" && value == "stale_entity_access") foundEventField = true;
    if (key == "suppressed" && value == "2") foundCountField = true;
  }
  EXPECT_TRUE(foundEventField);
  EXPECT_TRUE(foundCountField);

  // Restore the default console sink for the remaining tests.
  laige::log::LoggerOptions defaults;
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(defaults)).ok());
}
