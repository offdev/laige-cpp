// laige-sim component type registry suite (M1-ECS-02).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - duplicate registration is an error (InvalidArgument + one
//     rate-limited structured warn, ecs/component_duplicate)
//   - type ids stable across two runs with the same registration
//     order (two worlds stand in for the two runs: the id assignment
//     is pure integer bookkeeping — ARCH-010, component.h preamble)
//   - user-defined structs are legal components (S-8 data-carrier
//     case) and register through the same path as built-ins
//   - size/alignment are recorded for the M1-ECS-03 SoA layout
//   - the engine-level component budget (kMaxComponentTypes) is
//     honored with BudgetExhausted
//   - registry state moves with the World; clear() leaves it untouched
//
// Runs as CTest `component_registry` (the step's Verify command:
// `ctest -R component_registry`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the suites below.

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
#include "laige/sim/component.h"
#include "laige/sim/entity.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "component_registry_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "component_registry_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "component_registry_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define COMPONENT_REGISTRY_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define COMPONENT_REGISTRY_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if COMPONENT_REGISTRY_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "component_registry_tests must be built as C++20 "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test component types (global scope on purpose)
//
// LAIGE_COMPONENT specializes laige::detail::ComponentTraits, which
// the C++ standard requires to be declared in the primary template's
// enclosing namespace — so the marks cannot sit in an anonymous
// namespace.
// ---------------------------------------------------------------------------

// Engine-style built-in test components (the macro path).
struct SimTestPos {
  std::int32_t x;
  std::int32_t y;
};
LAIGE_COMPONENT(SimTestPos);

struct SimTestVel {
  std::int64_t vx;  // 8-byte alignment: the SoA column alignment case
};
LAIGE_COMPONENT(SimTestVel);

// A user-defined component (S-8 data-carrier case, PRD Appendix B
// shape): a plain struct marked with the same macro — the same
// registration path, no built-in privilege.
struct SimPlayerHealth {
  std::int32_t current;
  std::int32_t max;
};
LAIGE_COMPONENT(SimPlayerHealth);

// A family of distinct types for the kMaxComponentTypes budget test.
// The partial specialization marks the whole family at once (the
// macro is per-type sugar over the same trait; see component.h).
template <int N>
struct SimBulkComp {
  std::int32_t v;
};

namespace laige::detail {
template <int N>
struct ComponentTraits<SimBulkComp<N>> {
  static constexpr bool isComponent = true;
};
}  // namespace laige::detail

namespace {

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

// Register SimBulkComp<Lo> .. SimBulkComp<Hi-1>; false on the first
// failure. Compile-time recursion over the non-type parameter (test
// setup code, not a hot path).
template <int Lo, int Hi>
bool registerRange(laige::World& world) {
  if constexpr (Lo < Hi) {
    auto r = world.registerComponent<SimBulkComp<Lo>>();
    if (!r.ok()) return false;
    return registerRange<Lo + 1, Hi>(world);
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The id type (FR-1.2)
// ---------------------------------------------------------------------------

TEST(ComponentTypeId, ZeroIsReservedAndNeverAssigned) {
  EXPECT_EQ(laige::kInvalidComponentTypeId.value, 0u);
  EXPECT_NE(laige::ComponentTypeId{1}, laige::kInvalidComponentTypeId);
}

TEST(ComponentTypeId, ComparisonOperators) {
  const laige::ComponentTypeId a{1}, b{1}, c{2};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_LT(a, c);
  EXPECT_FALSE(c < a);
  EXPECT_FALSE(a < a);
}

// ---------------------------------------------------------------------------
// World::registerComponent: ids, size/alignment, errors
// ---------------------------------------------------------------------------

TEST(ComponentRegistry, IdsAssignedInRegistrationOrder) {
  laige::World world = makeWorld(8);
  auto a = world.registerComponent<SimTestPos>();
  ASSERT_TRUE(a.ok());
  auto b = world.registerComponent<SimTestVel>();
  ASSERT_TRUE(b.ok());
  auto c = world.registerComponent<SimPlayerHealth>();
  ASSERT_TRUE(c.ok());
  // Dense from 1, in the order of the successful calls:
  EXPECT_EQ(a.value().value, 1u);
  EXPECT_EQ(b.value().value, 2u);
  EXPECT_EQ(c.value().value, 3u);
  EXPECT_EQ(world.componentCount(), 3u);
  // 0 is never assigned:
  EXPECT_NE(a.value(), laige::kInvalidComponentTypeId);
}

TEST(ComponentRegistry, SizeAndAlignmentRecorded) {
  laige::World world = makeWorld(0);
  auto a = world.registerComponent<SimTestPos>();
  ASSERT_TRUE(a.ok());
  auto infoA = world.componentInfo(a.value());
  ASSERT_TRUE(infoA.ok());
  EXPECT_EQ(infoA.value().size, sizeof(SimTestPos));
  EXPECT_EQ(infoA.value().alignment, alignof(SimTestPos));

  // The 8-byte-aligned case: the SoA column layout (M1-ECS-03)
  // depends on the recorded alignment, not a hand-computed one.
  auto b = world.registerComponent<SimTestVel>();
  ASSERT_TRUE(b.ok());
  auto infoB = world.componentInfo(b.value());
  ASSERT_TRUE(infoB.ok());
  EXPECT_EQ(infoB.value().size, sizeof(SimTestVel));
  EXPECT_EQ(infoB.value().alignment, 8u);
}

TEST(ComponentRegistry, DuplicateRegistrationIsAnError) {
  // The roadmap's named property: duplicate registration is an
  // error, not a no-op (FR-12.3: never silent).
  laige::World world = makeWorld(1);
  auto first = world.registerComponent<SimTestPos>();
  ASSERT_TRUE(first.ok());
  auto second = world.registerComponent<SimTestPos>();
  EXPECT_FALSE(second.ok());
  EXPECT_EQ(second.error(), laige::ErrorCode::InvalidArgument);
  // The registry is unchanged: same count, the first id still
  // queryable with the original size/alignment.
  EXPECT_EQ(world.componentCount(), 1u);
  auto info = world.componentInfo(first.value());
  ASSERT_TRUE(info.ok());
  EXPECT_EQ(info.value().size, sizeof(SimTestPos));
}

// ---------------------------------------------------------------------------
// Determinism of the id assignment (ARCH-010; component.h contract)
// ---------------------------------------------------------------------------

TEST(ComponentRegistry, TypeIdsStableAcrossWorldsWithSameOrder) {
  // The roadmap's named property: type ids stable across two runs
  // with the same registration order. Two worlds stand in for the two
  // runs: the id assignment is pure integer bookkeeping (component.h
  // preamble, ARCH-010), so the property also holds across process
  // runs and builds.
  laige::World w1 = makeWorld(0);
  laige::World w2 = makeWorld(0);
  auto a1 = w1.registerComponent<SimTestPos>();
  auto b1 = w1.registerComponent<SimTestVel>();
  auto c1 = w1.registerComponent<SimPlayerHealth>();
  auto a2 = w2.registerComponent<SimTestPos>();
  auto b2 = w2.registerComponent<SimTestVel>();
  auto c2 = w2.registerComponent<SimPlayerHealth>();
  ASSERT_TRUE(a1.ok() && b1.ok() && c1.ok());
  ASSERT_TRUE(a2.ok() && b2.ok() && c2.ok());
  EXPECT_EQ(a1.value(), a2.value());
  EXPECT_EQ(b1.value(), b2.value());
  EXPECT_EQ(c1.value(), c2.value());
}

TEST(ComponentRegistry, RegistrationOrderDeterminesIds) {
  // The documented contract (component.h): ids follow registration
  // order, so a different order produces different ids. Replay and
  // replication rely on the order being fixed once at setup
  // (M1-HEAD-01 wires the loop around it).
  laige::World w1 = makeWorld(0);
  laige::World w2 = makeWorld(0);
  auto a1 = w1.registerComponent<SimTestPos>();  // id 1 in w1
  auto b1 = w1.registerComponent<SimTestVel>();   // id 2 in w1
  auto a2 = w2.registerComponent<SimTestVel>();   // id 1 in w2
  auto b2 = w2.registerComponent<SimTestPos>();   // id 2 in w2
  ASSERT_TRUE(a1.ok() && b1.ok() && a2.ok() && b2.ok());
  EXPECT_EQ(a1.value().value, 1u);
  EXPECT_EQ(b1.value().value, 2u);
  EXPECT_EQ(a2.value().value, 1u);
  EXPECT_EQ(b2.value().value, 2u);
  // The same type in different worlds/orders: different ids —
  // ComponentTypeIds are per-world and never compared across worlds.
  EXPECT_NE(a1.value(), b2.value());
  EXPECT_NE(b1.value(), a2.value());
}

// ---------------------------------------------------------------------------
// The engine-level component budget (kMaxComponentTypes)
// ---------------------------------------------------------------------------

TEST(ComponentRegistry, EngineBudgetHonored) {
  // kMaxComponentTypes is the engine-level component budget
  // (component.h): reaching it turns further NEW-type registration
  // into BudgetExhausted (the world never grows silently, S-2/G-R1).
  laige::World world = makeWorld(0);
  EXPECT_TRUE((registerRange<0, laige::kMaxComponentTypes>(world)));
  EXPECT_EQ(world.componentCount(), laige::kMaxComponentTypes);
  // A distinct 257th type: this is the budget path, not the
  // duplicate path.
  auto over = world.registerComponent<SimBulkComp<laige::kMaxComponentTypes>>();
  EXPECT_FALSE(over.ok());
  EXPECT_EQ(over.error(), laige::ErrorCode::BudgetExhausted);
}

// ---------------------------------------------------------------------------
// componentInfo validation
// ---------------------------------------------------------------------------

TEST(ComponentRegistry, InfoQueriesValidateTheId) {
  laige::World world = makeWorld(0);
  // The reserved id:
  auto invalid = world.componentInfo(laige::kInvalidComponentTypeId);
  EXPECT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error(), laige::ErrorCode::InvalidArgument);
  // Nothing registered yet:
  auto empty = world.componentInfo(laige::ComponentTypeId{1});
  EXPECT_FALSE(empty.ok());
  EXPECT_EQ(empty.error(), laige::ErrorCode::InvalidArgument);
  // Register one; id 2 is still unregistered:
  auto a = world.registerComponent<SimTestPos>();
  ASSERT_TRUE(a.ok());
  auto missing = world.componentInfo(laige::ComponentTypeId{2});
  EXPECT_FALSE(missing.ok());
  EXPECT_EQ(missing.error(), laige::ErrorCode::InvalidArgument);
  // The registered id resolves:
  auto ok = world.componentInfo(laige::ComponentTypeId{1});
  EXPECT_TRUE(ok.ok());
  EXPECT_EQ(ok.value().size, sizeof(SimTestPos));
}

// ---------------------------------------------------------------------------
// World lifetime: move and clear
// ---------------------------------------------------------------------------

TEST(ComponentRegistry, MovedWorldCarriesRegistry) {
  laige::World w1 = makeWorld(2);
  auto a = w1.registerComponent<SimTestPos>();
  ASSERT_TRUE(a.ok());
  auto b = w1.registerComponent<SimTestVel>();
  ASSERT_TRUE(b.ok());
  laige::World w2 = std::move(w1);
  EXPECT_EQ(w2.componentCount(), 2u);
  auto info = w2.componentInfo(a.value());
  ASSERT_TRUE(info.ok());
  EXPECT_EQ(info.value().size, sizeof(SimTestPos));
  auto info2 = w2.componentInfo(b.value());
  ASSERT_TRUE(info2.ok());
  EXPECT_EQ(info2.value().size, sizeof(SimTestVel));
  // The moved-from world has no registry (valid empty world):
  EXPECT_EQ(w1.componentCount(), 0u);
  auto r = w1.registerComponent<SimTestPos>();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
}

TEST(ComponentRegistry, ClearLeavesRegistryUntouched) {
  // clear() destroys live entities (M1-ECS-01); the type registry is
  // setup state and survives (M1-ECS-03 will destroy per-entity
  // component data, not the type records).
  laige::World world = makeWorld(2);
  auto e = world.create();
  ASSERT_TRUE(e.ok());
  auto a = world.registerComponent<SimTestPos>();
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(world.destroy(e.value()).ok());
  ASSERT_TRUE(world.clear().ok());
  EXPECT_EQ(world.entityCount(), 0u);
  EXPECT_EQ(world.componentCount(), 1u);
  auto info = world.componentInfo(a.value());
  ASSERT_TRUE(info.ok());
  EXPECT_EQ(info.value().size, sizeof(SimTestPos));
}

// ---------------------------------------------------------------------------
// Duplicate-registration logging: warn-once (LOG-004) through the facade
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

TEST(ComponentRegistry, DuplicateWarnsOnceWithIdentifyingFields) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();

  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);  // the burst stays in-window
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(1);
  ASSERT_TRUE(world.registerComponent<SimTestPos>().ok());

  // Three duplicate attempts within the window: the first emits the
  // warn, the other two are suppressed and counted (LOG-004: the
  // rate_limited summary carries the count).
  for (int i = 0; i < 3; ++i) {
    auto r = world.registerComponent<SimTestPos>();
    EXPECT_FALSE(r.ok());
    if (r.isError()) {
      EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
    }
  }

  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "ecs");
  EXPECT_EQ(sinkPtr->entries[0].event, "component_duplicate");
  // The event identifies the already-registered id (LOG-002):
  bool foundId = false;
  for (const auto& [key, value] : sinkPtr->entries[0].fields) {
    if (key == "component_id" && value == "1") foundId = true;
  }
  EXPECT_TRUE(foundId);

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
