// laige-sim archetype SoA storage suite (M1-ECS-03).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - archetype = ordered component set, SoA columns (one T[] per
//     component per archetype), entity -> archetype map
//   - add/remove component: pool-backed moves between archetypes with
//     no per-operation heap allocation (the churn test proves it:
//     zero ArchetypeStats reservation delta + zero process-wide
//     allocations over the churn window)
//   - get<T> is O(1) (archetype lookup + column index); stale handles
//     degrade per the M1-ECS-01 contract (nullptr + warn-once)
//   - 10k entities x add/remove churn: zero pool overflow and constant
//     per-op cost (measured, not assumed — CORE-001; the machine-
//     greppable stats line lands in the ctest output)
//   - memory layout is contiguous per column (property test) and the
//     slot-ordered row scheme survives moves (M1-ECS-05 pins the full
//     convergence property; the scheme is exercised here)
//
// Runs as CTest `archetype` (the step's Verify command:
// `ctest -R archetype`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the suites below.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/sim/entity.h"

#include "laige_test_seed.h"

#if defined(LAIGE_ALLOC_COUNTER)
#include "logging_alloc_counter.h"
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "archetype_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "archetype_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "archetype_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define ARCHETYPE_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define ARCHETYPE_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if ARCHETYPE_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "archetype_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test component types (global scope on purpose)
//
// LAIGE_COMPONENT specializes laige::detail::ComponentTraits, which
// the C++ standard requires to be declared in the primary template's
// enclosing namespace — so the marks cannot sit in an anonymous
// namespace.
// ---------------------------------------------------------------------------

// 8-byte, 4-byte-aligned: the basic packed-column case.
struct ArchPos {
  std::int32_t x;
  std::int32_t y;
};
LAIGE_COMPONENT(ArchPos);

// 8-byte-aligned: the SoA column alignment case (archetype.h: column
// blocks are aligned to kArchetypeColumnAlignment).
struct ArchVel {
  std::int64_t v;
};
LAIGE_COMPONENT(ArchVel);

// The toggled component in the churn test.
struct ArchFlag {
  std::int32_t f;
};
LAIGE_COMPONENT(ArchFlag);

// A type marked but never registered in the test worlds: the
// unregistered-type error path.
struct ArchUnreg {
  std::int32_t v;
};
LAIGE_COMPONENT(ArchUnreg);

// A family of distinct types for the budget tests.
template <int N>
struct ArchBulk {
  std::int32_t v;
};

namespace laige::detail {
template <int N>
struct ComponentTraits<ArchBulk<N>> {
  static constexpr bool isComponent = true;
};
}  // namespace laige::detail

namespace {

// The PRNG substream id for this file (docs/testing.md §4, M0-TEST-01).
inline constexpr std::uint32_t kArchetypeTestsSubstreamId = 1003;

// One world, taken out of its Result (Result::value() is const;
// takeValue() && moves the storage out — the documented
// ownership-transfer path, result.h).
laige::World makeWorld(std::uint32_t capacity) {
  auto w = laige::World::create(laige::World::Options{capacity});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << capacity
                  << ") failed: " << laige::errorName(w.error());
    abort();
  }
  return std::move(w).takeValue();
}

// Register ArchBulk<Lo> .. ArchBulk<Hi-1>; false on the first failure.
// Compile-time recursion over the non-type parameter (test setup
// code, not a hot path).
template <int Lo, int Hi>
bool registerRange(laige::World& world) {
  if constexpr (Lo < Hi) {
    auto r = world.registerComponent<ArchBulk<Lo>>();
    if (!r.ok()) return false;
    return registerRange<Lo + 1, Hi>(world);
  }
  return true;
}

// Add ArchBulk<i> to entities[i] for i in [Lo, Hi) — compile-time
// recursion over the non-type parameter (a runtime loop cannot form
// the template argument). Test setup code, not a hot path.
template <int Lo, int Hi>
bool addRange(laige::World& world, const std::vector<laige::Entity>& entities) {
  if constexpr (Lo < Hi) {
    auto r = world.addComponent<ArchBulk<Lo>>(entities[Lo], ArchBulk<Lo>{Lo});
    if (!r.ok()) return false;
    return addRange<Lo + 1, Hi>(world, entities);
  }
  return true;
}

// Add ArchBulk<i> to one entity for i in [Lo, Hi).
template <int Lo, int Hi>
bool addBulkRange(laige::World& world, laige::Entity entity) {
  if constexpr (Lo < Hi) {
    auto r = world.addComponent<ArchBulk<Lo>>(entity, ArchBulk<Lo>{Lo});
    if (!r.ok()) return false;
    return addBulkRange<Lo + 1, Hi>(world, entity);
  }
  return true;
}

// The aligned address of a pointer, for the alignment property checks.
inline std::uintptr_t addr(const void* p) {
  return reinterpret_cast<std::uintptr_t>(p);
}

// A test-only Sink that records every emitted event (the logging
// facade is a process singleton; the logging test owns its window and
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

// ---------------------------------------------------------------------------
// Archetype basics: sets, SoA columns, membership
// ---------------------------------------------------------------------------

TEST(ArchetypeBasics, AddCreatesSingleComponentArchetype) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();

  EXPECT_EQ(world.archetypeCount(), 0u);  // no components yet
  EXPECT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());

  EXPECT_EQ(world.archetypeCount(), 1u);
  EXPECT_TRUE(world.has<ArchPos>(entity));
  const ArchPos* p = world.get<ArchPos>(entity);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->x, 1);
  EXPECT_EQ(p->y, 2);
}

TEST(ArchetypeBasics, SecondComponentCreatesNewArchetype) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();

  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());
  EXPECT_EQ(world.archetypeCount(), 1u);
  ASSERT_TRUE(world.addComponent<ArchVel>(entity, ArchVel{9}).ok());
  // A new component set {Pos, Vel}: a second archetype — the first
  // stays alive (archetypes are never destroyed in M1).
  EXPECT_EQ(world.archetypeCount(), 2u);

  const ArchPos* p = world.get<ArchPos>(entity);
  const ArchVel* v = world.get<ArchVel>(entity);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(p->x, 1);
  EXPECT_EQ(v->v, 9);

  // Removing one component returns the entity to the other set —
  // the {Pos} archetype already exists (no third archetype).
  ASSERT_TRUE(world.removeComponent<ArchVel>(entity).ok());
  EXPECT_EQ(world.archetypeCount(), 2u);
  EXPECT_TRUE(world.has<ArchPos>(entity));
  EXPECT_FALSE(world.has<ArchVel>(entity));
  EXPECT_EQ(world.get<ArchPos>(entity)->x, 1);
}

TEST(ArchetypeBasics, SameSetConvergesToSameArchetype) {
  // Order-independence of the signature: two entities acquire the same
  // set in opposite orders and share one archetype, with the rows in
  // ascending slot order (slots: e1 = 3, e2 = 2 in a capacity-4 world).
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());

  auto e1 = world.create();
  auto e2 = world.create();
  ASSERT_TRUE(e1.ok());
  ASSERT_TRUE(e2.ok());

  ASSERT_TRUE(world.addComponent<ArchPos>(e1.value(), ArchPos{1, 0}).ok());
  ASSERT_TRUE(world.addComponent<ArchVel>(e1.value(), ArchVel{1}).ok());
  ASSERT_TRUE(world.addComponent<ArchVel>(e2.value(), ArchVel{2}).ok());
  ASSERT_TRUE(world.addComponent<ArchPos>(e2.value(), ArchPos{2, 0}).ok());

  // {Pos}, {Vel}, {Pos,Vel} — three archetypes, one per set seen.
  EXPECT_EQ(world.archetypeCount(), 3u);
  const ArchPos* p1 = world.get<ArchPos>(e1.value());
  const ArchPos* p2 = world.get<ArchPos>(e2.value());
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  // Slot-ordered rows: e2 (slot 2) sits below e1 (slot 3), packed
  // exactly one element apart (the contiguity property).
  EXPECT_EQ(addr(p1) - addr(p2), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  EXPECT_EQ(p1->x, 1);
  EXPECT_EQ(p2->x, 2);
}

TEST(ArchetypeBasics, RemoveAllLeavesEntityAlive) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();

  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());
  ASSERT_TRUE(world.addComponent<ArchVel>(entity, ArchVel{9}).ok());
  ASSERT_TRUE(world.removeComponent<ArchVel>(entity).ok());
  ASSERT_TRUE(world.removeComponent<ArchPos>(entity).ok());

  // The entity is alive and component-less; the archetypes it visited
  // stay (M1: sets are never destroyed).
  EXPECT_TRUE(world.isValid(entity));
  EXPECT_FALSE(world.has<ArchPos>(entity));
  EXPECT_FALSE(world.has<ArchVel>(entity));
  EXPECT_EQ(world.get<ArchPos>(entity), nullptr);
  EXPECT_EQ(world.entityCount(), 1u);
  EXPECT_EQ(world.archetypeCount(), 2u);
  EXPECT_EQ(world.archetypeStats().rowsLive, 0u);
}

TEST(ArchetypeBasics, ReaddOverwritesInPlace) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();

  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());
  const ArchPos* first = world.get<ArchPos>(entity);
  ASSERT_NE(first, nullptr);

  // Create-or-update: the value is replaced in place — same row,
  // same archetype (no move).
  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{3, 4}).ok());
  const ArchPos* second = world.get<ArchPos>(entity);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(second->x, 3);
  EXPECT_EQ(second->y, 4);
  EXPECT_EQ(world.archetypeCount(), 1u);
}

TEST(ArchetypeBasics, RemoveMissingIsNoop) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();

  // Removing a component the entity does not have is a no-op ok.
  EXPECT_TRUE(world.removeComponent<ArchPos>(entity).ok());
  EXPECT_FALSE(world.has<ArchPos>(entity));
  EXPECT_EQ(world.archetypeCount(), 0u);

  // ... including when the entity has other components.
  ASSERT_TRUE(world.addComponent<ArchVel>(entity, ArchVel{5}).ok());
  EXPECT_TRUE(world.removeComponent<ArchPos>(entity).ok());
  EXPECT_TRUE(world.has<ArchVel>(entity));
  EXPECT_EQ(world.get<ArchVel>(entity)->v, 5);
  EXPECT_EQ(world.archetypeCount(), 1u);
}

TEST(ArchetypeBasics, FreshEntityIsInNoArchetype) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  EXPECT_FALSE(world.has<ArchPos>(e.value()));
  EXPECT_EQ(world.get<ArchPos>(e.value()), nullptr);
  EXPECT_EQ(world.archetypeCount(), 0u);
  EXPECT_EQ(world.archetypeStats().rowsLive, 0u);
}

// ---------------------------------------------------------------------------
// Access: O(1) reads and the stale-handle contract (M1-ECS-01)
// ---------------------------------------------------------------------------

TEST(ArchetypeAccess, GetStaleReturnsNull) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();
  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());
  ASSERT_TRUE(world.destroy(entity).ok());

  // Stale use degrades in every build: nullptr (the warn-once of
  // check() is asserted by the entity suite, M1-ECS-01).
  EXPECT_EQ(world.get<ArchPos>(entity), nullptr);
}

TEST(ArchetypeAccess, HasStaleIsFalse) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();
  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());
  ASSERT_TRUE(world.destroy(entity).ok());

  // has is a pure query (like isValid): no warn, no side effect — a
  // stale handle simply reads as "no".
  EXPECT_FALSE(world.has<ArchPos>(entity));
}

TEST(ArchetypeAccess, AddStaleReturnsInvalidArgument) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();
  ASSERT_TRUE(world.destroy(entity).ok());

  auto r = world.addComponent<ArchPos>(entity, ArchPos{1, 2});
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
}

TEST(ArchetypeAccess, RemoveStaleReturnsInvalidArgument) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();
  ASSERT_TRUE(world.destroy(entity).ok());

  auto r = world.removeComponent<ArchPos>(entity);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
}

TEST(ArchetypeAccess, UnregisteredTypeIsAnError) {
  laige::World world = makeWorld(4);
  // ArchUnreg is LAIGE_COMPONENT-marked but never registered here: the
  // per-world registry has no id for it.
  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();

  auto add = world.addComponent<ArchUnreg>(entity, ArchUnreg{7});
  EXPECT_FALSE(add.ok());
  EXPECT_EQ(add.error(), laige::ErrorCode::InvalidArgument);
  auto remove = world.removeComponent<ArchUnreg>(entity);
  EXPECT_FALSE(remove.ok());
  EXPECT_EQ(remove.error(), laige::ErrorCode::InvalidArgument);
  // The entity is untouched.
  EXPECT_FALSE(world.has<ArchUnreg>(entity));
  EXPECT_EQ(world.archetypeCount(), 0u);
}

// ---------------------------------------------------------------------------
// Layout: SoA contiguity, alignment, slot-ordered rows
// ---------------------------------------------------------------------------

TEST(ArchetypeLayout, ColumnsAreContiguousAndAligned) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());

  // Four entities, slots 7, 6, 5, 4 (LIFO free list): slot order is
  // e3 < e2 < e1 < e0.
  std::vector<laige::Entity> entities;
  for (int i = 0; i < 4; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    entities.push_back(e.value());
    ASSERT_TRUE(world.addComponent<ArchPos>(entities.back(),
                                            ArchPos{i, 100}).ok());
    ASSERT_TRUE(world.addComponent<ArchVel>(entities.back(),
                                            ArchVel{static_cast<std::int64_t>(i * 1000)})
                    .ok());
  }

  const ArchPos* pos[4];
  const ArchVel* vel[4];
  for (int i = 0; i < 4; ++i) {
    pos[i] = world.get<ArchPos>(entities[i]);
    vel[i] = world.get<ArchVel>(entities[i]);
    ASSERT_NE(pos[i], nullptr);
    ASSERT_NE(vel[i], nullptr);
  }
  // Slot-ordered rows: the smallest slot (e3) is row 0, packed
  // contiguously — the memory-layout property (one element per row).
  EXPECT_LT(addr(pos[3]), addr(pos[2]));
  EXPECT_LT(addr(pos[2]), addr(pos[1]));
  EXPECT_LT(addr(pos[1]), addr(pos[0]));
  EXPECT_EQ(addr(pos[2]) - addr(pos[3]), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  EXPECT_EQ(addr(pos[1]) - addr(pos[2]), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  EXPECT_EQ(addr(pos[0]) - addr(pos[1]), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  EXPECT_EQ(addr(vel[2]) - addr(vel[3]), static_cast<std::uintptr_t>(sizeof(ArchVel)));
  // The values follow their slots (each row's data matches its entity).
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(pos[i]->x, i);
    EXPECT_EQ(vel[i]->v, static_cast<std::int64_t>(i) * 1000);
  }
  // Column alignment: every row address is aligned to the component's
  // alignment (kArchetypeColumnAlignment covers it — archetype.h).
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(addr(pos[i]) % alignof(ArchPos), 0u);
    EXPECT_EQ(addr(vel[i]) % alignof(ArchVel), 0u);
  }
}

TEST(ArchetypeLayout, SlotOrderDeterminesRowOrder) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  auto eA = world.create();  // slot 7
  auto eB = world.create();  // slot 6
  ASSERT_TRUE(eA.ok());
  ASSERT_TRUE(eB.ok());
  ASSERT_TRUE(world.addComponent<ArchPos>(eA.value(), ArchPos{1, 0}).ok());
  ASSERT_TRUE(world.addComponent<ArchPos>(eB.value(), ArchPos{2, 0}).ok());
  const ArchPos* bBefore = world.get<ArchPos>(eB.value());  // row 0
  const ArchPos* aBefore = world.get<ArchPos>(eA.value());  // row 1
  ASSERT_NE(bBefore, nullptr);
  ASSERT_NE(aBefore, nullptr);
  EXPECT_EQ(addr(aBefore) - addr(bBefore),
            static_cast<std::uintptr_t>(sizeof(ArchPos)));

  // eC (slot 5) is inserted at the FRONT (smallest slot): every
  // existing row shifts down exactly one row — each entity lands on
  // the row (and address) the row before it vacated, packed.
  auto eC = world.create();
  ASSERT_TRUE(eC.ok());
  ASSERT_TRUE(world.addComponent<ArchPos>(eC.value(), ArchPos{3, 0}).ok());

  const ArchPos* a = world.get<ArchPos>(eA.value());
  const ArchPos* b = world.get<ArchPos>(eB.value());
  const ArchPos* c = world.get<ArchPos>(eC.value());
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c, bBefore);  // the head slot takes the old head row
  EXPECT_EQ(b, aBefore);  // each shifted row took the row above's slot
  EXPECT_EQ(addr(a) - addr(aBefore),
            static_cast<std::uintptr_t>(sizeof(ArchPos)));
  EXPECT_LT(addr(c), addr(b));
  EXPECT_LT(addr(b), addr(a));
  EXPECT_EQ(addr(b) - addr(c), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  EXPECT_EQ(addr(a) - addr(b), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  // The shifted rows kept their data.
  EXPECT_EQ(b->x, 2);
  EXPECT_EQ(c->x, 3);
}

// ---------------------------------------------------------------------------
// Moves: data preservation and the dense-id-order scheme
// ---------------------------------------------------------------------------

TEST(ArchetypeMoves, AddPreservesSharedData) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());
  ASSERT_TRUE(world.registerComponent<ArchFlag>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();
  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());
  ASSERT_TRUE(world.addComponent<ArchVel>(entity, ArchVel{9}).ok());

  ASSERT_TRUE(world.addComponent<ArchFlag>(entity, ArchFlag{5}).ok());

  // The move {Pos,Vel} -> {Pos,Vel,Flag} preserved the shared data.
  EXPECT_EQ(world.get<ArchPos>(entity)->x, 1);
  EXPECT_EQ(world.get<ArchPos>(entity)->y, 2);
  EXPECT_EQ(world.get<ArchVel>(entity)->v, 9);
  EXPECT_EQ(world.get<ArchFlag>(entity)->f, 5);
  EXPECT_EQ(world.archetypeCount(), 3u);
}

TEST(ArchetypeMoves, RemovePreservesRemainingData) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());
  ASSERT_TRUE(world.registerComponent<ArchFlag>().ok());

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();
  ASSERT_TRUE(world.addComponent<ArchPos>(entity, ArchPos{1, 2}).ok());
  ASSERT_TRUE(world.addComponent<ArchVel>(entity, ArchVel{9}).ok());
  ASSERT_TRUE(world.addComponent<ArchFlag>(entity, ArchFlag{5}).ok());

  ASSERT_TRUE(world.removeComponent<ArchFlag>(entity).ok());

  EXPECT_EQ(world.get<ArchPos>(entity)->x, 1);
  EXPECT_EQ(world.get<ArchVel>(entity)->v, 9);
  EXPECT_EQ(world.get<ArchFlag>(entity), nullptr);
  EXPECT_EQ(world.archetypeCount(), 3u);  // {Pos,Vel,Flag} stays alive
}

TEST(ArchetypeMoves, ConvergedWorldsShareLayout) {
  // Two worlds, same final state (both entities in {Pos,Vel}, same
  // slot ids) reached through different operation interleavings —
  // the row layouts must agree (the dense-id-order scheme; M1-ECS-05
  // runs the full property test on this scheme).
  // World A: e1 gets {Pos} first, e2 gets {Vel} first.
  laige::World wa = makeWorld(4);
  ASSERT_TRUE(wa.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(wa.registerComponent<ArchVel>().ok());
  auto ea1 = wa.create();
  auto ea2 = wa.create();
  ASSERT_TRUE(ea1.ok());
  ASSERT_TRUE(ea2.ok());
  ASSERT_TRUE(wa.addComponent<ArchPos>(ea1.value(), ArchPos{1, 0}).ok());
  ASSERT_TRUE(wa.addComponent<ArchVel>(ea2.value(), ArchVel{2}).ok());
  ASSERT_TRUE(wa.addComponent<ArchVel>(ea1.value(), ArchVel{1}).ok());
  ASSERT_TRUE(wa.addComponent<ArchPos>(ea2.value(), ArchPos{2, 0}).ok());

  // World B: e2 gets {Vel} first, e1 gets {Vel} first — same final
  // state, different interleaving.
  laige::World wb = makeWorld(4);
  ASSERT_TRUE(wb.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(wb.registerComponent<ArchVel>().ok());
  auto eb1 = wb.create();
  auto eb2 = wb.create();
  ASSERT_TRUE(eb1.ok());
  ASSERT_TRUE(eb2.ok());
  ASSERT_TRUE(wb.addComponent<ArchVel>(eb2.value(), ArchVel{2}).ok());
  ASSERT_TRUE(wb.addComponent<ArchVel>(eb1.value(), ArchVel{1}).ok());
  ASSERT_TRUE(wb.addComponent<ArchPos>(eb2.value(), ArchPos{2, 0}).ok());
  ASSERT_TRUE(wb.addComponent<ArchPos>(eb1.value(), ArchPos{1, 0}).ok());

  // The sets VISITED differ (A saw {Pos} on the way; B never did), so
  // the archetype counts differ — but both final {Pos,Vel} layouts
  // must agree.
  EXPECT_EQ(wa.archetypeCount(), 3u);  // {Pos}, {Pos,Vel}, {Vel}
  EXPECT_EQ(wb.archetypeCount(), 2u);  // {Vel}, {Pos,Vel}
  EXPECT_EQ(wa.archetypeStats().rowsLive, wb.archetypeStats().rowsLive);
  // Same relative layout: e2 (slot 2) below e1 (slot 3), packed.
  const ArchPos* pa1 = wa.get<ArchPos>(ea1.value());
  const ArchPos* pa2 = wa.get<ArchPos>(ea2.value());
  const ArchPos* pb1 = wb.get<ArchPos>(eb1.value());
  const ArchPos* pb2 = wb.get<ArchPos>(eb2.value());
  ASSERT_NE(pa1, nullptr);
  ASSERT_NE(pa2, nullptr);
  ASSERT_NE(pb1, nullptr);
  ASSERT_NE(pb2, nullptr);
  EXPECT_EQ(addr(pa1) - addr(pa2), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  EXPECT_EQ(addr(pb1) - addr(pb2), static_cast<std::uintptr_t>(sizeof(ArchPos)));
  const ArchVel* va1 = wa.get<ArchVel>(ea1.value());
  const ArchVel* vb2 = wb.get<ArchVel>(eb2.value());
  ASSERT_NE(va1, nullptr);
  ASSERT_NE(vb2, nullptr);
  EXPECT_EQ(va1->v, 1);
  EXPECT_EQ(vb2->v, 2);
}

// ---------------------------------------------------------------------------
// Budgets: the engine-level caps (archetype.h)
// ---------------------------------------------------------------------------

TEST(ArchetypeBudget, ArchetypeCountLimitHonored) {
  // kMaxArchetypes = 256 distinct sets: 256 single-component archetypes
  // plus one two-component set would be the 257th — BudgetExhausted.
  laige::World world = makeWorld(300);
  ASSERT_TRUE((registerRange<0, 256>(world)));

  constexpr int kArchetypes = 256;
  std::vector<laige::Entity> entities;
  entities.reserve(kArchetypes + 1);
  for (int i = 0; i < kArchetypes + 1; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    entities.push_back(e.value());
  }
  ASSERT_TRUE((addRange<0, 256>(world, entities)));
  EXPECT_EQ(world.archetypeCount(), static_cast<std::uint32_t>(laige::kMaxArchetypes));

  // The extra entity joins the existing {ArchBulk<0>} archetype (ok),
  // then asks for the new set {ArchBulk<0>, ArchBulk<1>} — no room.
  ASSERT_TRUE(world.addComponent<ArchBulk<0>>(entities[kArchetypes],
                                              ArchBulk<0>{kArchetypes}).ok());
  auto overflow = world.addComponent<ArchBulk<1>>(entities[kArchetypes],
                                                  ArchBulk<1>{kArchetypes});
  EXPECT_FALSE(overflow.ok());
  EXPECT_EQ(overflow.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(world.archetypeCount(), static_cast<std::uint32_t>(laige::kMaxArchetypes));
  // The entity keeps what it was given.
  EXPECT_TRUE(world.has<ArchBulk<0>>(entities[kArchetypes]));
  EXPECT_FALSE(world.has<ArchBulk<1>>(entities[kArchetypes]));
}

TEST(ArchetypeBudget, ComponentLimitHonored) {
  // kMaxArchetypeComponents = 32: a 33rd distinct component on one
  // entity fails with BudgetExhausted.
  laige::World world = makeWorld(40);
  ASSERT_TRUE((registerRange<0, 33>(world)));

  auto e = world.create();
  ASSERT_TRUE(e.ok());
  const laige::Entity entity = e.value();
  ASSERT_TRUE((addBulkRange<0, 32>(world, entity)));
  auto overflow = world.addComponent<ArchBulk<32>>(entity, ArchBulk<32>{32});
  EXPECT_FALSE(overflow.ok());
  EXPECT_EQ(overflow.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_TRUE(world.has<ArchBulk<31>>(entity));
  EXPECT_FALSE(world.has<ArchBulk<32>>(entity));
}

TEST(ArchetypeBudget, GrowthCapsAtWorldCapacity) {
  // Reserve policy: an archetype grows 16 -> min(capacity, 32) = 18 for
  // a capacity-18 world — the reserve never exceeds the world budget.
  laige::World world = makeWorld(18);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());

  for (int i = 0; i < 18; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(world.addComponent<ArchPos>(e.value(), ArchPos{i, 0}).ok());
  }
  const laige::ArchetypeStats s = world.archetypeStats();
  EXPECT_EQ(s.rowsLive, 18u);
  EXPECT_EQ(s.rowsReserved, 18u);  // capped at 18, not doubled to 32
  EXPECT_EQ(s.totalArchetypeGrowth, 1u);
  // 2 initial reservations (slot column + Pos column) + 2 grown.
  EXPECT_EQ(s.totalReservations, 4u);
}

// ---------------------------------------------------------------------------
// Accounting: ArchetypeStats (the profiler / G-R4 feed)
// ---------------------------------------------------------------------------

TEST(ArchetypeStats, StatsTrackRowsAndChurn) {
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());

  std::vector<laige::Entity> entities;
  for (int i = 0; i < 4; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    entities.push_back(e.value());
  }

  laige::ArchetypeStats s0 = world.archetypeStats();
  EXPECT_EQ(s0.archetypeCount, 0u);
  EXPECT_EQ(s0.rowsLive, 0u);
  EXPECT_EQ(s0.rowsReserved, 0u);
  EXPECT_EQ(s0.totalAdds, 0u);
  EXPECT_EQ(s0.totalRemoves, 0u);

  for (std::size_t i = 0; i < entities.size(); ++i) {
    ASSERT_TRUE(world.addComponent<ArchPos>(
                    entities[i], ArchPos{static_cast<std::int32_t>(i), 0})
                    .ok());
  }
  laige::ArchetypeStats s1 = world.archetypeStats();
  EXPECT_EQ(s1.archetypeCount, 1u);
  EXPECT_EQ(s1.rowsLive, 4u);
  // Initial reserve is min(kInitialArchetypeRows, capacity) = 8 for a
  // capacity-8 world (the small-world edge of the reserve policy).
  EXPECT_EQ(s1.rowsReserved, 8u);
  // 8 rows x (2 B slot column + 8 B Pos) = 80 reserved row bytes.
  EXPECT_EQ(s1.bytesReserved, 80u);
  EXPECT_EQ(s1.totalAdds, 4u);
  EXPECT_EQ(s1.totalReservations, 2u);  // slot column + 1 component column

  for (const auto& entity : entities) {
    ASSERT_TRUE(world.addComponent<ArchVel>(entity, ArchVel{0}).ok());
  }
  laige::ArchetypeStats s2 = world.archetypeStats();
  EXPECT_EQ(s2.archetypeCount, 2u);
  EXPECT_EQ(s2.rowsLive, 4u);
  // 8 rows x 10 B ({Pos}) + 8 rows x 18 B ({Pos,Vel}).
  EXPECT_EQ(s2.bytesReserved, 8 * 10u + 8 * 18u);
  EXPECT_EQ(s2.totalAdds, 8u);

  for (const auto& entity : entities) {
    ASSERT_TRUE(world.removeComponent<ArchVel>(entity).ok());
    ASSERT_TRUE(world.removeComponent<ArchPos>(entity).ok());
  }
  laige::ArchetypeStats s3 = world.archetypeStats();
  EXPECT_EQ(s3.rowsLive, 0u);
  // The archetypes stay alive (and their reserves stay accounted).
  EXPECT_EQ(s3.archetypeCount, 2u);
  EXPECT_EQ(s3.bytesReserved, 8 * 10u + 8 * 18u);
  EXPECT_EQ(s3.totalRemoves, 8u);
}

// ---------------------------------------------------------------------------
// Budget-exhaustion logging: warn-once (LOG-004) through the facade
// ---------------------------------------------------------------------------

TEST(ArchetypeLogging, ArchetypeBudgetWarnsOnce) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();

  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);  // the burst stays in-window
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());
  // Only Warn and above from ecs: the 256 archetype_created Info
  // events of the setup phase are suppressed so the burst is isolated
  // (the Info path itself is exercised by the M0 logging suite).
  laige::log::Logger::instance().setSubsystemLevel("ecs", laige::log::Level::Warn);

  laige::World world = makeWorld(300);
  ASSERT_TRUE((registerRange<0, 256>(world)));

  constexpr int kArchetypes = 256;
  std::vector<laige::Entity> entities;
  entities.reserve(kArchetypes + 3);
  for (int i = 0; i < kArchetypes + 3; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    entities.push_back(e.value());
  }
  ASSERT_TRUE((addRange<0, 256>(world, entities)));

  // Three attempts to create the 257th archetype within the window:
  // the first emits the warn, the other two are suppressed and counted
  // (LOG-004: the rate_limited summary carries the count).
  for (int i = kArchetypes; i < kArchetypes + 3; ++i) {
    ASSERT_TRUE(world.addComponent<ArchBulk<0>>(entities[i], ArchBulk<0>{i}).ok());
    auto overflow = world.addComponent<ArchBulk<1>>(entities[i],
                                                    ArchBulk<1>{i});
    EXPECT_FALSE(overflow.ok());
    if (overflow.isError()) {
      EXPECT_EQ(overflow.error(), laige::ErrorCode::BudgetExhausted);
    }
  }

  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "ecs");
  EXPECT_EQ(sinkPtr->entries[0].event, "archetype_budget");
  bool foundCount = false;
  for (const auto& [key, value] : sinkPtr->entries[0].fields) {
    if (key == "archetype_count" && value == "256") foundCount = true;
  }
  EXPECT_TRUE(foundCount);

  // Controlled shutdown drains the pending rate-limit summary
  // (CONC-006/LOG-007/LOG-004).
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(sinkPtr->entries.size(), 2u);
  EXPECT_EQ(sinkPtr->entries[1].event, laige::log::kRateLimitedEvent);
  bool foundSuppressed = false;
  for (const auto& [key, value] : sinkPtr->entries[1].fields) {
    if (key == "suppressed" && value == "2") foundSuppressed = true;
  }
  EXPECT_TRUE(foundSuppressed);

  // Restore the default console sink for the remaining tests.
  laige::log::LoggerOptions defaults;
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(defaults)).ok());
}

// ---------------------------------------------------------------------------
// The churn step (CORE-001: measured, not assumed) — 10k entities x
// add/remove, zero pool overflow, zero allocations, flat per-op cost
// ---------------------------------------------------------------------------

TEST(ArchetypeChurn, TenKEntitiesAddRemoveChurnZeroAllocAndFlatCost) {
  constexpr std::uint32_t kEntities = 10000;
  laige::World world = makeWorld(kEntities);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());
  ASSERT_TRUE(world.registerComponent<ArchFlag>().ok());

  std::vector<laige::Entity> entities(kEntities);
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    entities[i] = e.value();
  }
  // Warm-up (setup phase; growth events are allowed and accounted):
  // both working archetypes {Pos,Vel} and {Pos,Vel,Flag} are grown to
  // the full working size before the measured window.
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    ASSERT_TRUE(world.addComponent<ArchPos>(entities[i], ArchPos{0, 0}).ok());
    ASSERT_TRUE(world.addComponent<ArchVel>(entities[i], ArchVel{0}).ok());
  }
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    ASSERT_TRUE(world.addComponent<ArchFlag>(entities[i], ArchFlag{1}).ok());
  }
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    ASSERT_TRUE(world.removeComponent<ArchFlag>(entities[i]).ok());
  }

  const laige::ArchetypeStats before = world.archetypeStats();

  // The measured window: every entity toggles Flag once — kEntities
  // adds + kEntities removes = 2 * kEntities ops, in a seeded random
  // order (docs/testing.md §4: deterministic per seed) so the slot-
  // ordered insertion positions span the full cost range.
  laige::Prng rng = laige::testing::TestPrng(kArchetypeTestsSubstreamId);
  std::vector<std::uint32_t> order(kEntities);
  std::iota(order.begin(), order.end(), std::uint32_t{0});
  for (std::uint32_t i = kEntities; i > 1; --i) {
    const std::uint32_t j = rng.next_range(0, i);  // [0, i)
    std::swap(order[i - 1], order[j]);
  }

  laige::Histogram hist(laige::Histogram::Options{kEntities * 2});
  laige::TimeIt timer;
#if defined(LAIGE_ALLOC_COUNTER)
  // The churn window starts here: the shuffle and the histogram setup
  // allocated above, so the reset lands between setup and the ops.
  laige::test::resetAllocCounter();
#endif
  std::uint64_t failures = 0;
  for (std::uint32_t idx : order) {
    timer.reset();
    if (!world.addComponent<ArchFlag>(entities[idx],
                                      ArchFlag{static_cast<std::int32_t>(idx)})
             .ok()) {
      ++failures;
    }
    hist.record(timer.elapsedMs());
    timer.reset();
    if (!world.removeComponent<ArchFlag>(entities[idx]).ok()) {
      ++failures;
    }
    hist.record(timer.elapsedMs());
  }
  const laige::ArchetypeStats after = world.archetypeStats();

  // Zero pool overflow: every op succeeded (no BudgetExhausted, no
  // growth failure) and the window reserved nothing new — the churn
  // moved only between pre-reserved columns.
  EXPECT_EQ(failures, 0u);
  EXPECT_EQ(after.totalReservations, before.totalReservations);
  EXPECT_EQ(after.totalArchetypeGrowth, before.totalArchetypeGrowth);
  EXPECT_EQ(after.totalAdds, before.totalAdds + kEntities);
  EXPECT_EQ(after.totalRemoves, before.totalRemoves + kEntities);
  // All entities are back in {Pos,Vel}.
  EXPECT_EQ(after.rowsLive, kEntities);

#if defined(LAIGE_ALLOC_COUNTER)
  // The roadmap's Verify property: the churn window allocates zero
  // heap — only the pre-reserved column blocks are touched. (The
  // sanitizer trees prove the same property with a leak-free run of
  // this loop plus the reservation delta above.)
  EXPECT_EQ(laige::test::allocCounter(), 0u);
#endif

  // Constant per-op cost (CORE-001: measured, not assumed): the
  // distribution over the full cost range is flat — p99 within 3x
  // the median (no spike beyond the documented O(tail * row-stride)
  // move cost, no growth event, no hidden allocation).
  const laige::HistogramStats st = hist.stats();
  ASSERT_EQ(st.n, kEntities * 2);
  EXPECT_TRUE(std::isfinite(st.mean));
  EXPECT_TRUE(std::isfinite(st.p50));
  EXPECT_TRUE(std::isfinite(st.p99));
  EXPECT_GT(st.p50, 0.0);
  EXPECT_LT(st.p99, st.p50 * 3.0);
  // Machine-greppable stats line for the M1 baseline record (CORE-001
  // / AGENTS §12: the measured per-op cost, on every ctest run).
  std::printf("archetype-churn %s\n", laige::formatStatsLine(st).c_str());
  std::fflush(stdout);

  // Spot-check the final state through the public API.
  EXPECT_TRUE(world.has<ArchPos>(entities[0]));
  EXPECT_TRUE(world.has<ArchVel>(entities[0]));
  EXPECT_FALSE(world.has<ArchFlag>(entities[0]));
}

// ---------------------------------------------------------------------------
// World lifetime: moves and clear over the archetype storage
// ---------------------------------------------------------------------------

TEST(ArchetypeLifetime, MovedWorldKeepsComponents) {
  laige::World a = makeWorld(4);
  ASSERT_TRUE(a.registerComponent<ArchPos>().ok());

  auto e1 = a.create();
  auto e2 = a.create();
  ASSERT_TRUE(e1.ok());
  ASSERT_TRUE(e2.ok());
  ASSERT_TRUE(a.addComponent<ArchPos>(e1.value(), ArchPos{1, 2}).ok());
  ASSERT_TRUE(a.addComponent<ArchPos>(e2.value(), ArchPos{3, 4}).ok());

  laige::World b = std::move(a);

  // The storage moved with the world (O(1) pointer swap of the tables).
  EXPECT_EQ(b.entityCount(), 2u);
  EXPECT_EQ(b.archetypeCount(), 1u);
  EXPECT_TRUE(b.has<ArchPos>(e1.value()));
  EXPECT_EQ(b.get<ArchPos>(e1.value())->x, 1);
  EXPECT_EQ(b.get<ArchPos>(e2.value())->x, 3);
  EXPECT_EQ(b.archetypeStats().rowsLive, 2u);

  // The moved-from world is a valid empty world: no live entities, no
  // archetypes, every handle invalid, every create fails (capacity 0).
  EXPECT_EQ(a.entityCount(), 0u);
  EXPECT_EQ(a.archetypeCount(), 0u);
  EXPECT_FALSE(a.isValid(e1.value()));
  EXPECT_FALSE(a.has<ArchPos>(e1.value()));
  auto e3 = a.create();
  EXPECT_FALSE(e3.ok());
  EXPECT_EQ(e3.error(), laige::ErrorCode::BudgetExhausted);
}

TEST(ArchetypeLifetime, ClearDetachesAllRows) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<ArchPos>().ok());
  ASSERT_TRUE(world.registerComponent<ArchVel>().ok());

  std::vector<laige::Entity> entities;
  for (int i = 0; i < 3; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    entities.push_back(e.value());
    ASSERT_TRUE(world.addComponent<ArchPos>(entities.back(),
                                            ArchPos{i, 0}).ok());
    ASSERT_TRUE(world.addComponent<ArchVel>(entities.back(),
                                            ArchVel{i}).ok());
  }
  EXPECT_EQ(world.archetypeStats().rowsLive, 3u);

  world.clear();

  // Every handle is stale and every row is released; the archetypes
  // and the type registry survive (setup state).
  EXPECT_EQ(world.entityCount(), 0u);
  for (const auto& entity : entities) {
    EXPECT_FALSE(world.isValid(entity));
  }
  const laige::ArchetypeStats s = world.archetypeStats();
  EXPECT_EQ(s.rowsLive, 0u);
  // Both visited sets stay alive ({Pos} was visited on the way to
  // {Pos,Vel}).
  EXPECT_EQ(s.archetypeCount, 2u);
  EXPECT_EQ(world.componentCount(), 2u);

  // The world is immediately reusable: a new entity re-enters the
  // surviving archetype.
  auto e4 = world.create();
  ASSERT_TRUE(e4.ok());
  ASSERT_TRUE(world.addComponent<ArchPos>(e4.value(), ArchPos{9, 9}).ok());
  EXPECT_TRUE(world.has<ArchPos>(e4.value()));
  EXPECT_EQ(world.get<ArchPos>(e4.value())->x, 9);
  EXPECT_EQ(world.archetypeStats().rowsLive, 1u);
}
