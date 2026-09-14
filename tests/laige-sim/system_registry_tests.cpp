// laige-sim system registry suite (M1-SYS-01).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - registration: the def is stored by value (name/run/budget),
//     SystemIds are dense from 1 in registration order, the I/O pack
//     resolves into the disjoint read/write sets (Io<T, Access> tags)
//   - validation errors: null/empty name, null run, budget <= 0
//     (the budget must be explicit), unregistered I/O component,
//     duplicate I/O component (any access combination)
//   - duplicate detection: duplicate system names are an error, not a
//     no-op (FR-12.3: never silent)
//   - the engine-level system budget (kMaxSystems) is honored with
//     BudgetExhausted
//   - the LAIGE_SYSTEM macro: the plain function + the def variable
//     (`Name##Def`), no class, no inheritance (FR-1.3)
//   - SystemContext delegates World::each (the query.h contract)
//   - id stability across two worlds with the same registration order
//     (ARCH-010; system.h preamble)
//   - registry lifetime: move, clear(), moved-from world
//   - no heap allocation at registration (setup path; the
//     operator-new counter on the non-sanitizer trees, the
//     M1-ECS-03 pattern; the sanitizer trees prove it leak-free)
//
// Runs as CTest `system_registry` (the step's Verify command:
// `ctest -R system_registry`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the suites below.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/sim/entity.h"
#include "laige/sim/system.h"

#if defined(LAIGE_ALLOC_COUNTER)
#include "logging_alloc_counter.h"
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "system_registry_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "system_registry_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "system_registry_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define SYSTEM_REGISTRY_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define SYSTEM_REGISTRY_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if SYSTEM_REGISTRY_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "system_registry_tests must be built as C++20 "
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

struct SysTestPos {
  std::int32_t x;
  std::int32_t y;
};
LAIGE_COMPONENT(SysTestPos);

struct SysTestVel {
  std::int64_t vx;
};
LAIGE_COMPONENT(SysTestVel);

struct SysTestHealth {
  std::int32_t current;
  std::int32_t max;
};
LAIGE_COMPONENT(SysTestHealth);

// ---------------------------------------------------------------------------
// The plain systems (FR-1.3: plain functions, no class, no
// inheritance). The LAIGE_SYSTEM macro declares each function and
// builds its def (`Name##Def`) directly above the definition.
// ---------------------------------------------------------------------------

// The per-tick write path: the query's Write reference (query.h
// "Iteration legality").
LAIGE_SYSTEM(SysMoveVel, 1)
void SysMoveVel(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx.each<SysTestVel>(
      [](laige::Entity e, SysTestVel& v) {
        static_cast<void>(e);
        v.vx = 0x1234;  // the sentinel the tests read back
      },
      laige::Write{}));
}

// The per-tick read path: counts the health carriers through the
// context's delegated each (the file-scope counter is test plumbing).
namespace {
std::uint32_t sysReadHealthCount = 0;
}  // namespace

LAIGE_SYSTEM(SysReadHealth, 2)
void SysReadHealth(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  sysReadHealthCount = 0;
  static_cast<void>(ctx.each<SysTestHealth>(
      [](laige::Entity e, const SysTestHealth& h) {
        static_cast<void>(e);
        static_cast<void>(h);
        ++sysReadHealthCount;
      },
      laige::Read{}));
}

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

// The manual SystemDef shape (the LAIGE_SYSTEM macro builds the same
// struct); used for the validation-error cases.
laige::SystemDef makeDef(const char* name, laige::SystemFn run,
                         laige::fpx16_16 budgetMs) {
  return laige::SystemDef{name, run, budgetMs};
}

// Plain helper functions (anonymous namespace: their addresses are
// taken directly, no macro involved).
void fnA(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}
void fnB(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

// Distinct registration names for the bulk tests (static storage: the
// names must outlive the registration — system.h).
inline char* bulkName(std::uint32_t i) {
  static char names[laige::kMaxSystems + 1][16];
  std::snprintf(names[i], sizeof(names[i]), "Sys%03u", i);
  return names[i];
}

// A test-only Sink that records every emitted event (the logging
// facade is a process singleton; the tests that use it restore the
// default console sink at the end — the ComponentRegistry pattern).
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

// Find one field of a recorded event (test plumbing).
const char* fieldValue(const MemorySink::Entry& entry,
                       const char* key) {
  for (const auto& [k, v] : entry.fields) {
    if (k == key) return v.c_str();
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// The id type (FR-1.3; the component.h precedent)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, IdTypeBasics) {
  EXPECT_EQ(laige::kInvalidSystemId.value, 0u);
  EXPECT_NE(laige::SystemId{1}, laige::kInvalidSystemId);
  EXPECT_EQ(laige::SystemId{1}, laige::SystemId{1});
  EXPECT_NE(laige::SystemId{1}, laige::SystemId{2});
  EXPECT_FALSE(laige::SystemId{1} != laige::SystemId{1});
}

// ---------------------------------------------------------------------------
// The LAIGE_SYSTEM macro (FR-1.3: plain registered functions)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, MacroBuildsDefAndFunction) {
  // The macro built SysMoveVel_Def from the plain function: the name
  // is the stringified Name, the run pointer is the function, the
  // budget is the declared value (exact fpx16_16 — 1 ms).
  EXPECT_STREQ(SysMoveVel_Def.name, "SysMoveVel");
  EXPECT_EQ(SysMoveVel_Def.run, &SysMoveVel);
  EXPECT_EQ(SysMoveVel_Def.budgetMs, laige::fpx16_16::fromInt32(1));
  // The second macro use (2 ms, a different function):
  EXPECT_STREQ(SysReadHealth_Def.name, "SysReadHealth");
  EXPECT_EQ(SysReadHealth_Def.run, &SysReadHealth);
  EXPECT_EQ(SysReadHealth_Def.budgetMs, laige::fpx16_16::fromInt32(2));
}

// ---------------------------------------------------------------------------
// Registration: ids, the def value copy, the I/O sets
// ---------------------------------------------------------------------------

TEST(SystemRegistry, IdsAssignedInRegistrationOrder) {
  laige::World world = makeWorld(0);
  auto a = world.registerSystem(SysMoveVel_Def);
  ASSERT_TRUE(a.ok());
  auto b = world.registerSystem(SysReadHealth_Def);
  ASSERT_TRUE(b.ok());
  // Dense from 1, in the order of the successful calls:
  EXPECT_EQ(a.value().value, 1u);
  EXPECT_EQ(b.value().value, 2u);
  EXPECT_EQ(world.systemCount(), 2u);
  // 0 is never assigned:
  EXPECT_NE(a.value(), laige::kInvalidSystemId);
}

TEST(SystemRegistry, DefStoredByValue) {
  laige::World world = makeWorld(0);
  // A stack-scoped def: registerSystem must copy it (system.h: the
  // user's def may be a stack variable).
  const laige::SystemDef def =
      makeDef("StackDef", &fnA, laige::fpx16_16{1 << 15});  // 0.5 ms
  auto r = world.registerSystem(def);
  ASSERT_TRUE(r.ok());
  auto info = world.system(r.value());
  ASSERT_TRUE(info.ok());
  EXPECT_STREQ(info.value().def.name, "StackDef");
  EXPECT_EQ(info.value().def.run, &fnA);
  EXPECT_EQ(info.value().def.budgetMs, laige::fpx16_16{1 << 15});
  EXPECT_EQ(info.value().id, r.value());
}

TEST(SystemRegistry, IoDeclaredAndQueryable) {
  laige::World world = makeWorld(1);
  auto pos = world.registerComponent<SysTestPos>();
  auto vel = world.registerComponent<SysTestVel>();
  ASSERT_TRUE(pos.ok() && vel.ok());
  auto r = world.registerSystem(SysMoveVel_Def,
                                laige::Io<SysTestVel, laige::Access::Write>{},
                                laige::Io<SysTestPos, laige::Access::Read>{});
  ASSERT_TRUE(r.ok());
  auto info = world.system(r.value());
  ASSERT_TRUE(info.ok());
  // The declared I/O, as stored (the disjoint read/write sets):
  EXPECT_TRUE(info.value().declaresWrite(vel.value()));
  EXPECT_TRUE(info.value().declaresRead(pos.value()));
  EXPECT_FALSE(info.value().declaresRead(vel.value()));
  EXPECT_FALSE(info.value().declaresWrite(pos.value()));
  // A registered component the system does not declare:
  auto health = world.registerComponent<SysTestHealth>();
  ASSERT_TRUE(health.ok());
  EXPECT_FALSE(info.value().declaresRead(health.value()));
  EXPECT_FALSE(info.value().declaresWrite(health.value()));
}

TEST(SystemRegistry, ZeroIoPackLegal) {
  // A system that touches no components declares no I/O: the empty
  // pack is legal (a pure query system).
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world.registerComponent<SysTestVel>().ok());
  auto r = world.registerSystem(SysMoveVel_Def);
  ASSERT_TRUE(r.ok());
  auto info = world.system(r.value());
  ASSERT_TRUE(info.ok());
  EXPECT_FALSE(info.value().declaresRead(laige::ComponentTypeId{1}));
  EXPECT_FALSE(info.value().declaresWrite(laige::ComponentTypeId{1}));
}

// ---------------------------------------------------------------------------
// SystemContext: the delegated World::each (query.h contract)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, ContextDelegatesEach) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<SysTestVel>().ok());
  ASSERT_TRUE(world.registerComponent<SysTestHealth>().ok());
  auto e1 = world.create();
  auto e2 = world.create();
  ASSERT_TRUE(e1.ok() && e2.ok());
  ASSERT_TRUE(world.addComponent<SysTestVel>(e1.value(), SysTestVel{0}).ok());
  ASSERT_TRUE(world
                  .addComponent<SysTestHealth>(e2.value(),
                                               SysTestHealth{1, 10})
                  .ok());
  laige::SystemContext ctx{world};
  // The plain function IS the system: calling it directly is what
  // the M1-SYS-02 scheduler will do. The write path lands the
  // sentinel on exactly the SysTestVel carrier.
  static_cast<void>(SysMoveVel(world, ctx));
  const SysTestVel* v1 = world.get<SysTestVel>(e1.value());
  ASSERT_NE(v1, nullptr);
  EXPECT_EQ(v1->vx, 0x1234);
  EXPECT_EQ(world.get<SysTestVel>(e2.value()), nullptr);
  // The read path counts exactly the SysTestHealth carrier (the
  // const reference: no write is possible through it, API-008).
  static_cast<void>(SysReadHealth(world, ctx));
  EXPECT_EQ(sysReadHealthCount, 1u);
}

// ---------------------------------------------------------------------------
// Validation errors (system.h "Registration"; the first failure wins)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, NullNameRejected) {
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(makeDef(nullptr, &fnA,
                                        laige::fpx16_16::fromInt32(1)));
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemRegistry, EmptyNameRejected) {
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(makeDef("", &fnA,
                                        laige::fpx16_16::fromInt32(1)));
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemRegistry, NullRunRejected) {
  laige::World world = makeWorld(0);
  auto r =
      world.registerSystem(makeDef("NoRun", nullptr,
                                   laige::fpx16_16::fromInt32(1)));
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemRegistry, ZeroBudgetRejected) {
  // The budget must be explicit and strictly positive (FR-1.3): 0 is
  // not "unset" — it is an undeclared budget (API-008).
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(
      makeDef("NoBudget", &fnA, laige::fpx16_16{}));
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemRegistry, NegativeBudgetRejected) {
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(
      makeDef("NegBudget", &fnA, laige::fpx16_16::fromInt32(-1)));
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemRegistry, FractionalBudgetAccepted) {
  // 0.5 ms = 32768 raw Q16.16 units (fpx16_16: exact, no rounding):
  // a positive sub-millisecond budget is legal and stored exactly.
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(
      makeDef("HalfMs", &fnA, laige::fpx16_16{1 << 15}));
  ASSERT_TRUE(r.ok());
  auto info = world.system(r.value());
  ASSERT_TRUE(info.ok());
  EXPECT_EQ(info.value().def.budgetMs, laige::fpx16_16{1 << 15});
}

TEST(SystemRegistry, DuplicateNameIsAnError) {
  // The roadmap's named property: duplicate system names are an
  // error, not a no-op (FR-12.3: never silent).
  laige::World world = makeWorld(0);
  auto first = world.registerSystem(
      makeDef("Dup", &fnA, laige::fpx16_16::fromInt32(1)));
  ASSERT_TRUE(first.ok());
  // A different function, the same registration name:
  auto second = world.registerSystem(
      makeDef("Dup", &fnB, laige::fpx16_16::fromInt32(2)));
  EXPECT_FALSE(second.ok());
  EXPECT_EQ(second.error(), laige::ErrorCode::InvalidArgument);
  // The registry is unchanged: same count, the first def intact:
  EXPECT_EQ(world.systemCount(), 1u);
  auto info = world.system(first.value());
  ASSERT_TRUE(info.ok());
  EXPECT_STREQ(info.value().def.name, "Dup");
  EXPECT_EQ(info.value().def.run, &fnA);
  EXPECT_EQ(info.value().def.budgetMs, laige::fpx16_16::fromInt32(1));
}

TEST(SystemRegistry, IoUnregisteredComponentIsAnError) {
  // SysTestVel is not registered in this world: the I/O entry cannot
  // be resolved (component.h id contract: per-world ids).
  laige::World world = makeWorld(1);
  auto r = world.registerSystem(
      SysMoveVel_Def, laige::Io<SysTestVel, laige::Access::Write>{});
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemRegistry, IoDuplicateComponentIsAnError) {
  laige::World world = makeWorld(1);
  ASSERT_TRUE(world.registerComponent<SysTestVel>().ok());
  // Read + Write of the same component: ambiguous — the I/O is a set,
  // not a multiset (system.h; M1-SYS-02 reads the disjoint sets).
  auto rw = world.registerSystem(SysMoveVel_Def,
                                 laige::Io<SysTestVel, laige::Access::Read>{},
                                 laige::Io<SysTestVel, laige::Access::Write>{});
  EXPECT_FALSE(rw.ok());
  EXPECT_EQ(rw.error(), laige::ErrorCode::InvalidArgument);
  // Read + Read: also a duplicate:
  auto rr = world.registerSystem(SysReadHealth_Def,
                                 laige::Io<SysTestVel, laige::Access::Read>{},
                                 laige::Io<SysTestVel, laige::Access::Read>{});
  EXPECT_FALSE(rr.ok());
  EXPECT_EQ(rr.error(), laige::ErrorCode::InvalidArgument);
  // The failed registrations change nothing:
  EXPECT_EQ(world.systemCount(), 0u);
  // The same component with a DIFFERENT one is fine:
  ASSERT_TRUE(world.registerComponent<SysTestPos>().ok());
  auto ok = world.registerSystem(SysMoveVel_Def,
                                 laige::Io<SysTestVel, laige::Access::Write>{},
                                 laige::Io<SysTestPos, laige::Access::Read>{});
  EXPECT_TRUE(ok.ok());
}

// ---------------------------------------------------------------------------
// The engine-level system budget (kMaxSystems)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, EngineBudgetHonored) {
  // kMaxSystems is the engine-level system budget (system.h):
  // reaching it turns further registration into BudgetExhausted (the
  // world never grows silently, S-2/G-R1).
  laige::World world = makeWorld(0);
  for (std::uint32_t i = 0; i < laige::kMaxSystems; ++i) {
    auto r = world.registerSystem(makeDef(bulkName(i), &fnA,
                                          laige::fpx16_16::fromInt32(1)));
    ASSERT_TRUE(r.ok());
  }
  EXPECT_EQ(world.systemCount(), laige::kMaxSystems);
  // A distinct 257th system: this is the budget path, not the
  // duplicate path:
  auto over = world.registerSystem(makeDef(bulkName(laige::kMaxSystems),
                                           &fnB,
                                           laige::fpx16_16::fromInt32(1)));
  EXPECT_FALSE(over.ok());
  EXPECT_EQ(over.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(world.systemCount(), laige::kMaxSystems);
}

// ---------------------------------------------------------------------------
// system() id validation (the componentInfo precedent)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, SystemQueryValidatesTheId) {
  laige::World world = makeWorld(0);
  // The reserved id:
  auto invalid = world.system(laige::kInvalidSystemId);
  EXPECT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error(), laige::ErrorCode::InvalidArgument);
  // Nothing registered yet:
  auto empty = world.system(laige::SystemId{1});
  EXPECT_FALSE(empty.ok());
  EXPECT_EQ(empty.error(), laige::ErrorCode::InvalidArgument);
  // Register one; id 2 is still unregistered:
  auto first = world.registerSystem(SysMoveVel_Def);
  ASSERT_TRUE(first.ok());
  auto above = world.system(laige::SystemId{first.value().value + 1});
  EXPECT_FALSE(above.ok());
  EXPECT_EQ(above.error(), laige::ErrorCode::InvalidArgument);
  // The registered id resolves:
  auto ok = world.system(first.value());
  EXPECT_TRUE(ok.ok());
  EXPECT_STREQ(ok.value().def.name, "SysMoveVel");
}

// ---------------------------------------------------------------------------
// Determinism of the id assignment (ARCH-010; system.h contract)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, SystemIdsStableAcrossWorldsWithSameOrder) {
  // The id assignment is pure integer bookkeeping (system.h preamble,
  // ARCH-010): two worlds that register the same systems in the same
  // order produce bit-identical id sequences (the property also
  // holds across process runs and builds).
  laige::World w1 = makeWorld(0);
  laige::World w2 = makeWorld(0);
  ASSERT_TRUE(w1.registerComponent<SysTestVel>().ok());
  ASSERT_TRUE(w2.registerComponent<SysTestVel>().ok());
  ASSERT_TRUE(w1.registerComponent<SysTestHealth>().ok());
  ASSERT_TRUE(w2.registerComponent<SysTestHealth>().ok());
  auto a1 = w1.registerSystem(SysMoveVel_Def,
                              laige::Io<SysTestVel, laige::Access::Write>{});
  auto b1 = w1.registerSystem(SysReadHealth_Def,
                              laige::Io<SysTestHealth, laige::Access::Read>{});
  auto a2 = w2.registerSystem(SysMoveVel_Def,
                              laige::Io<SysTestVel, laige::Access::Write>{});
  auto b2 = w2.registerSystem(SysReadHealth_Def,
                              laige::Io<SysTestHealth, laige::Access::Read>{});
  ASSERT_TRUE(a1.ok() && b1.ok() && a2.ok() && b2.ok());
  EXPECT_EQ(a1.value(), a2.value());
  EXPECT_EQ(b1.value(), b2.value());
  EXPECT_EQ(a1.value().value, 1u);
  EXPECT_EQ(b1.value().value, 2u);
}

TEST(SystemRegistry, RegistrationOrderDeterminesIds) {
  // The documented contract (system.h): ids follow registration order,
  // so a different order produces different ids. Games register their
  // systems once at world setup, in one documented place (M1-HEAD-01
  // wires the loop around it).
  laige::World w1 = makeWorld(0);
  laige::World w2 = makeWorld(0);
  auto a1 = w1.registerSystem(SysMoveVel_Def);   // id 1 in w1
  auto b1 = w1.registerSystem(SysReadHealth_Def);  // id 2 in w1
  auto a2 = w2.registerSystem(SysReadHealth_Def);  // id 1 in w2
  auto b2 = w2.registerSystem(SysMoveVel_Def);     // id 2 in w2
  ASSERT_TRUE(a1.ok() && b1.ok() && a2.ok() && b2.ok());
  EXPECT_EQ(a1.value().value, 1u);
  EXPECT_EQ(b1.value().value, 2u);
  EXPECT_EQ(a2.value().value, 1u);
  EXPECT_EQ(b2.value().value, 2u);
  // The same def in different worlds/orders: different ids —
  // SystemIds are per-world and never compared across worlds.
  EXPECT_NE(a1.value(), b2.value());
  EXPECT_NE(b1.value(), a2.value());
}

// ---------------------------------------------------------------------------
// Registry lifetime: move, clear, moved-from world
// ---------------------------------------------------------------------------

TEST(SystemRegistry, MovedWorldCarriesRegistry) {
  laige::World w1 = makeWorld(2);
  ASSERT_TRUE(w1.registerComponent<SysTestVel>().ok());
  auto a = w1.registerSystem(SysMoveVel_Def,
                             laige::Io<SysTestVel, laige::Access::Write>{});
  ASSERT_TRUE(a.ok());
  laige::World w2 = std::move(w1);
  EXPECT_EQ(w2.systemCount(), 1u);
  auto info = w2.system(a.value());
  ASSERT_TRUE(info.ok());
  EXPECT_TRUE(info.value().declaresWrite(laige::ComponentTypeId{1}));
  // The moved-from world has no registry (valid empty world):
  EXPECT_EQ(w1.systemCount(), 0u);
  auto r = w1.registerSystem(SysReadHealth_Def);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  auto q = w1.system(laige::SystemId{1});
  EXPECT_FALSE(q.ok());
  EXPECT_EQ(q.error(), laige::ErrorCode::InvalidArgument);
}

TEST(SystemRegistry, ClearLeavesRegistryUntouched) {
  // clear() destroys live entities (M1-ECS-01); the system registry
  // is setup state and survives (a system is not per-entity data).
  laige::World world = makeWorld(2);
  ASSERT_TRUE(world.registerComponent<SysTestVel>().ok());
  auto e = world.create();
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(world.addComponent<SysTestVel>(e.value(), SysTestVel{0}).ok());
  auto s = world.registerSystem(SysMoveVel_Def,
                                laige::Io<SysTestVel, laige::Access::Write>{});
  ASSERT_TRUE(s.ok());
  ASSERT_TRUE(world.clear().ok());
  EXPECT_EQ(world.entityCount(), 0u);
  EXPECT_EQ(world.systemCount(), 1u);
  auto info = world.system(s.value());
  ASSERT_TRUE(info.ok());
  EXPECT_TRUE(info.value().declaresWrite(laige::ComponentTypeId{1}));
}

// ---------------------------------------------------------------------------
// Zero allocation at registration (setup path; PERF-003)
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
TEST(SystemRegistry, RegistrationPerformsNoHeapAllocation) {
  // The registration path (def copy + I/O sets into the fixed table)
  // must allocate nothing: the record table is the world's setup-path
  // allocation (system.h; the component registry precedent).
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<SysTestVel>().ok());
  laige::test::resetAllocCounter();
  for (std::uint32_t i = 0; i < 32; ++i) {
    auto r = world.registerSystem(
        makeDef(bulkName(i), &fnA, laige::fpx16_16::fromInt32(1 + i % 4)),
        laige::Io<SysTestVel, laige::Access::Write>{});
    ASSERT_TRUE(r.ok());
  }
  // And a full read-back of one record:
  auto info = world.system(laige::SystemId{1});
  ASSERT_TRUE(info.ok());
  static_cast<void>(info);
  EXPECT_EQ(laige::test::allocCounter(), 0u);
}
#endif

// ---------------------------------------------------------------------------
// Structured logging (FR-12.3: never silent; LOG-004 rate limiting)
// ---------------------------------------------------------------------------

TEST(SystemRegistry, DuplicateWarnsOnceWithIdentifyingFields) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();

  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);  // the burst stays in-window
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(1);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("DupWarn", &fnA,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());

  // Three duplicate attempts within the window: the first emits the
  // warn, the other two are suppressed and counted (LOG-004: the
  // rate_limited summary carries the count).
  for (int i = 0; i < 3; ++i) {
    auto r = world.registerSystem(makeDef("DupWarn", &fnB,
                                          laige::fpx16_16::fromInt32(2)));
    EXPECT_FALSE(r.ok());
    if (r.isError()) {
      EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
    }
  }

  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "system");
  EXPECT_EQ(sinkPtr->entries[0].event, "duplicate");
  // The event identifies the name and the existing id (LOG-002):
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "name"), "DupWarn");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "existing_system_id"), "1");

  // Controlled shutdown drains the pending rate-limit summary
  // (CONC-006/LOG-007/LOG-004).
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(sinkPtr->entries.size(), 2u);
  EXPECT_EQ(sinkPtr->entries[1].event, laige::log::kRateLimitedEvent);
  EXPECT_STREQ(fieldValue(sinkPtr->entries[1], "suppressed"), "2");

  // Restore the default console sink for the remaining tests.
  laige::log::LoggerOptions defaults;
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(defaults)).ok());
}

TEST(SystemRegistry, BudgetInvalidWarnsOnceWithRawField) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();

  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(0);
  auto r = world.registerSystem(
      makeDef("NoBudget", &fnA, laige::fpx16_16{}));
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);

  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "system");
  EXPECT_EQ(sinkPtr->entries[0].event, "budget_invalid");
  // The rejected budget, in Q16.16 raw units (system.h): the value is
  // raw / 2^16 ms.
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "name"), "NoBudget");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "budget_raw"), "0");

  laige::log::Logger::instance().shutdown();
  // Restore the default console sink for the remaining tests.
  laige::log::LoggerOptions defaults;
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(defaults)).ok());
}
