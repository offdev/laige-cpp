// laige-sim system scheduler suite (M1-SYS-02).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - ordering: the computed execution order is the stable
//     topological sort of the registration order plus the declared
//     depends_on edges (Kahn, min-id tie-break); forward
//     dependencies reorder, backward ones keep the registration
//     order; no dependencies = exactly the registration order
//   - running: runSystems executes the systems strictly one at a
//     time in schedule order (state flows from an earlier writer to a
//     later reader)
//   - pre-run validation (first failure wins): unknown dependency
//     name (system/dep_missing), dependency cycle
//     (system/dependency_cycle, one concrete cycle reported), two
//     systems writing the same component (system/double_writer);
//     a declared read ordered before a declared write WARNs without
//     failing (system/read_before_write)
//   - registration validation of the depends_on spec (the def-level
//     form): empty token, trailing comma, duplicate name, more than
//     kMaxSystemDependencies (system/dep_spec_invalid); whitespace
//     trimming is legal
//   - schedule lifetime: a schedule is valid for the registry it was
//     computed with (system/schedule_stale); a hand-built malformed
//     schedule is rejected (system/schedule_invalid)
//   - the empty world schedules and runs empty (moved-from world
//     included); the LAIGE_SYSTEM macro carries the optional
//     depends_on names (spec stringization)
//   - determinism (ARCH-010): the order is identical across two
//     worlds with the same registrations; a known-answer scenario
//     pins the exact order hash (machine-greppable line)
//   - no heap allocation at scheduling or per tick (setup path +
//     hot path; PERF-003 — the operator-new counter on the
//     non-sanitizer trees, the M1-ECS-03/07 pattern; the sanitizer
//     trees prove it leak-free)
//
// Runs as CTest `scheduler` (the step's Verify command:
// `ctest -R scheduler`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the suites below.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// NFR-8.10 policy self-checks (compile-time; a violation fails the
// build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "scheduler_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "scheduler_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "scheduler_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define SCHEDULER_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define SCHEDULER_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if SCHEDULER_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "scheduler_tests must be built as C++20 "
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

struct SchPos {
  std::int32_t x;
  std::int32_t y;
};
LAIGE_COMPONENT(SchPos);

struct SchVel {
  std::int64_t vx;
};
LAIGE_COMPONENT(SchVel);

struct SchTag {
  std::int32_t v;
};
LAIGE_COMPONENT(SchTag);

// ---------------------------------------------------------------------------
// The plain systems (FR-1.3: plain functions, no class, no
// inheritance). The LAIGE_SYSTEM macro declares each function and
// builds its def (`Name##Def`) directly above the definition. The
// file-scope counters and the run log are test plumbing.
// ---------------------------------------------------------------------------

namespace {

// The run log: which systems executed, in order (token per system).
// A fixed array — the zero-alloc window must not see an allocation
// here.
constexpr std::uint32_t kRunLogCapacity = 64;
std::uint32_t runLog[kRunLogCapacity];
std::uint32_t runLogCount = 0;

void clearRunLog() {
  runLogCount = 0;
}
void markRun(std::uint32_t token) {
  if (runLogCount < kRunLogCapacity) {
    runLog[runLogCount++] = token;
  }
}

std::uint64_t schReadVelSum = 0;
std::uint64_t schReadPosSum = 0;

}  // namespace

// The per-tick write path: the query's Write reference (query.h
// "Iteration legality").
LAIGE_SYSTEM(SchWriteVel, 1)
void SchWriteVel(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  markRun(2);
  static_cast<void>(ctx.each<SchVel>(
      [](laige::Entity e, SchVel& v) {
        static_cast<void>(e);
        v.vx = 0x1234;  // the sentinel the tests read back
      },
      laige::Write{}));
}

// The per-tick read path: sums the velocities through the context's
// delegated each (reset per run — a stale read is observable).
LAIGE_SYSTEM(SchReadVel, 1)
void SchReadVel(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  markRun(3);
  schReadVelSum = 0;
  static_cast<void>(ctx.each<SchVel>(
      [](laige::Entity e, const SchVel& v) {
        static_cast<void>(e);
        schReadVelSum += static_cast<std::uint64_t>(v.vx);
      },
      laige::Read{}));
}

// A second writer of SchTag: paired with SchWriteTag it forms the
// double-writer conflict (one writer per component, M1-SYS-02).
LAIGE_SYSTEM(SchWriteTag, 1)
void SchWriteTag(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  markRun(4);
  static_cast<void>(ctx.each<SchTag>(
      [](laige::Entity e, SchTag& t) {
        static_cast<void>(e);
        t.v = 1;
      },
      laige::Write{}));
}

LAIGE_SYSTEM(SchWriteTag2, 1)
void SchWriteTag2(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  markRun(5);
  static_cast<void>(ctx.each<SchTag>(
      [](laige::Entity e, SchTag& t) {
        static_cast<void>(e);
        t.v = 2;
      },
      laige::Write{}));
}

// The Pos read path (the zero-alloc window exercises a second
// component through the same declared-I/O pattern).
LAIGE_SYSTEM(SchReadPos, 1)
void SchReadPos(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  markRun(6);
  schReadPosSum = 0;
  static_cast<void>(ctx.each<SchPos>(
      [](laige::Entity e, const SchPos& p) {
        static_cast<void>(e);
        schReadPosSum += static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x));
      },
      laige::Read{}));
}

// The no-op system (an ordering marker that declares no I/O).
LAIGE_SYSTEM(SchNoop, 1)
void SchNoop(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
  markRun(1);
}

// The macro with the optional depends_on names (M1-SYS-02): the
// trailing names are stringized verbatim into the def's spec —
// SchNoop before SchDepOnNoop, and SchNoop + SchWriteVel before
// SchDepOnTwo (one comma-separated list, as written).
LAIGE_SYSTEM(SchDepOnNoop, 1, SchNoop)
void SchDepOnNoop(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
  markRun(7);
}

LAIGE_SYSTEM(SchDepOnTwo, 1, SchNoop, SchWriteVel)
void SchDepOnTwo(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
  markRun(8);
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
// struct); used for the ordering/validation cases where the spec must
// be exact (whitespace, malformed tokens).
laige::SystemDef makeDef(const char* name, laige::SystemFn run,
                         laige::fpx16_16 budgetMs,
                         const char* dependsOn = nullptr) {
  return laige::SystemDef{name, run, budgetMs, dependsOn};
}

// Plain helper functions (anonymous namespace: their addresses are
// taken directly, no macro involved).
void fnNoop(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}
void fnNoop2(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

// Distinct registration names for the bulk tests (static storage: the
// names must outlive the registration — system.h).
inline char* bulkName(std::uint32_t i) {
  static char names[laige::kMaxSystems + 1][16];
  std::snprintf(names[i], sizeof(names[i]), "Dep%03u", i);
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

void restoreConsoleSink() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger init with the default console sink failed";
    abort();
  }
}

// FNV-1a 64-bit, big-endian byte order per u64 (the docs/testing.md §4
// KAT convention): the machine-greppable scenario hash.
std::uint64_t fnv1a64(const std::uint64_t* values, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis (FNV-1a spec)
  for (std::size_t i = 0; i < n; ++i) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (values[i] >> shift) & 0xFFull;
      h *= 0x100000001b3ull;  // FNV prime (FNV-1a spec)
    }
  }
  return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// The LAIGE_SYSTEM macro with depends_on (M1-SYS-02)
// ---------------------------------------------------------------------------

TEST(SystemScheduler, MacroCarriesDependsOnSpec) {
  // No trailing names: the no-deps spec — nullptr or "" (both are the
  // documented forms; SystemDef::dependsOn, parseDepSpec). The exact
  // representation is a preprocessor property of an EMPTY variadic
  // pack: "" where #__VA_ARGS__ stringizes it (GCC/Clang,
  // [cpp.stringize]), nullptr where the pack stays empty and the 4th
  // initializer value-initializes (MSVC 2022). The engine treats both
  // alike, so the test pins the contract, not the representation
  // (platform-dependent assertions do not travel across P0 platforms).
  ASSERT_TRUE(SchNoop_Def.dependsOn == nullptr ||
              SchNoop_Def.dependsOn[0] == '\0');
  // One trailing name, stringified verbatim:
  EXPECT_STREQ(SchDepOnNoop_Def.dependsOn, "SchNoop");
  // Two trailing names: the comma-separated list as written.
  EXPECT_STREQ(SchDepOnTwo_Def.dependsOn, "SchNoop, SchWriteVel");
}

// ---------------------------------------------------------------------------
// The computed execution order (the stable topological sort)
// ---------------------------------------------------------------------------

TEST(SystemScheduler, RegistrationOrderIsTheExecutionOrder) {
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world.registerSystem(SchNoop_Def).ok());
  ASSERT_TRUE(world.registerSystem(SchWriteVel_Def).ok());
  ASSERT_TRUE(world.registerSystem(SchReadVel_Def).ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.systemCount, 3u);
  EXPECT_EQ(sched.order[0], 1u);
  EXPECT_EQ(sched.order[1], 2u);
  EXPECT_EQ(sched.order[2], 3u);
}

TEST(SystemScheduler, BackwardDependencyKeepsRegistrationOrder) {
  // B depends on A: the registration order already satisfies the edge.
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("OrderA", &fnNoop,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(
                      makeDef("OrderB", &fnNoop2,
                             laige::fpx16_16::fromInt32(1), "OrderA"))
                  .ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.order[0], 1u);
  EXPECT_EQ(sched.order[1], 2u);
}

TEST(SystemScheduler, ForwardDependencyReorders) {
  // A depends on C (registered LAST): the stable sort places C (and
  // the independent B) before A — A only moves later, behind its
  // dependency.
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("ReorderA", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "ReorderC"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("ReorderB", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("ReorderC", &fnNoop,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.order[0], 2u);  // B (the smallest available)
  EXPECT_EQ(sched.order[1], 3u);  // C
  EXPECT_EQ(sched.order[2], 1u);  // A, behind C
}

TEST(SystemScheduler, DependencyChainReorders) {
  // Registered Z -> Y -> X, but Z depends on Y and Y depends on X:
  // the chain forces X, Y, Z.
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("ChainZ", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "ChainY"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("ChainY", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1),
                                          "ChainX"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("ChainX", &fnNoop,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.order[0], 3u);  // X
  EXPECT_EQ(sched.order[1], 2u);  // Y
  EXPECT_EQ(sched.order[2], 1u);  // Z
}

TEST(SystemScheduler, DiamondDependencyOrdersDependeeLast) {
  // D is registered first but depends on B and C: D runs last. The
  // spec list order ("DiamondC, DiamondB" — reversed) must not
  // matter: the dependency set is orderless, the tie-break is the min
  // id.
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("DiamondD", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "DiamondC, DiamondB"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("DiamondB", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("DiamondC", &fnNoop,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.order[0], 2u);  // B
  EXPECT_EQ(sched.order[1], 3u);  // C
  EXPECT_EQ(sched.order[2], 1u);  // D, behind both
}

TEST(SystemScheduler, MacroForwardDependencyReorders) {
  // The macro-declared dependency (SchDepOnNoop lists SchNoop) is a
  // forward dependency: registered first, it must run after SchNoop.
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world.registerSystem(SchDepOnNoop_Def).ok());
  ASSERT_TRUE(world.registerSystem(SchNoop_Def).ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.order[0], 2u);  // SchNoop (the dependency)
  EXPECT_EQ(sched.order[1], 1u);  // SchDepOnNoop
  // And it actually runs in that order:
  clearRunLog();
  ASSERT_TRUE(world.runSystems(sched).ok());
  ASSERT_EQ(runLogCount, 2u);
  EXPECT_EQ(runLog[0], 1u);  // SchNoop's token
  EXPECT_EQ(runLog[1], 7u);  // SchDepOnNoop's token
}

// ---------------------------------------------------------------------------
// Running: strictly one system at a time, in schedule order
// ---------------------------------------------------------------------------

TEST(SystemScheduler, RunExecutesInTheScheduledOrder) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<SchVel>().ok());
  auto e = world.create();
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(world.addComponent<SchVel>(e.value(), SchVel{0}).ok());
  // SchNoop declares no I/O (it is an ordering marker only): no
  // declared read, so no read_before_write warn can fire here.
  ASSERT_TRUE(world.registerSystem(SchNoop_Def).ok());
  ASSERT_TRUE(world
                  .registerSystem(SchWriteVel_Def,
                                  laige::Io<SchVel, laige::Access::Write>{})
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(SchReadVel_Def,
                                  laige::Io<SchVel, laige::Access::Read>{})
                  .ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  clearRunLog();
  ASSERT_TRUE(world.runSystems(sched).ok());
  ASSERT_EQ(runLogCount, 3u);
  EXPECT_EQ(runLog[0], 1u);  // SchNoop
  EXPECT_EQ(runLog[1], 2u);  // SchWriteVel
  EXPECT_EQ(runLog[2], 3u);  // SchReadVel
  // The writer ran before the reader: the reader saw the fresh value.
  EXPECT_EQ(schReadVelSum, 0x1234u);
}

// ---------------------------------------------------------------------------
// The declared-I/O pre-run validation
// ---------------------------------------------------------------------------

TEST(SystemScheduler, ReaderBeforeWriterWarnsButSchedules) {
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<SchVel>().ok());
  auto e = world.create();
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(world.addComponent<SchVel>(e.value(), SchVel{0x9999}).ok());
  // The reader is registered FIRST: it will run before the writer.
  ASSERT_TRUE(world
                  .registerSystem(SchReadVel_Def,
                                  laige::Io<SchVel, laige::Access::Read>{})
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(SchWriteVel_Def,
                                  laige::Io<SchVel, laige::Access::Write>{})
                  .ok());
  // The sink is installed AFTER the setup events (the G-R3 entity
  // budget and archetype_created infos are world setup, not scheduler
  // events): it sees only the scheduler's own diagnostics.
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());
  // A warn, not an error: the schedule succeeds.
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.order[0], 1u);
  EXPECT_EQ(sched.order[1], 2u);
  // The one warn identifies the reader, the writer, the component:
  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "system");
  EXPECT_EQ(sinkPtr->entries[0].event, "read_before_write");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "reader"), "SchReadVel");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "writer"), "SchWriteVel");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "component_id"), "1");
  // And the consequence is observable: the reader ran first and read
  // the previous tick's value (0x9999), not the write (0x1234).
  clearRunLog();
  ASSERT_TRUE(world.runSystems(sched).ok());
  ASSERT_EQ(runLogCount, 2u);
  EXPECT_EQ(runLog[0], 3u);  // SchReadVel
  EXPECT_EQ(runLog[1], 2u);  // SchWriteVel
  EXPECT_EQ(schReadVelSum, 0x9999u);  // the stale value

  laige::log::Logger::instance().shutdown();
  restoreConsoleSink();
}

TEST(SystemScheduler, WriterBeforeReaderIsClean) {
  // The opposite order (writer first) is the healthy pattern: no warn.
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<SchVel>().ok());
  auto e = world.create();
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(world.addComponent<SchVel>(e.value(), SchVel{0}).ok());
  ASSERT_TRUE(world
                  .registerSystem(SchWriteVel_Def,
                                  laige::Io<SchVel, laige::Access::Write>{})
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(SchReadVel_Def,
                                  laige::Io<SchVel, laige::Access::Read>{})
                  .ok());
  // The sink is installed AFTER the setup events (the G-R3 entity
  // budget and archetype_created infos are world setup, not scheduler
  // events): it must stay EMPTY — the success path logs nothing.
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  // Nothing logged on the success path (LOG-003) — and no
  // read_before_write (the reader is ordered after the writer).
  EXPECT_EQ(sinkPtr->entries.size(), 0u);
  clearRunLog();
  ASSERT_TRUE(world.runSystems(sched).ok());
  EXPECT_EQ(schReadVelSum, 0x1234u);  // the fresh value

  laige::log::Logger::instance().shutdown();
  restoreConsoleSink();
}

TEST(SystemScheduler, DoubleWriterRejectedWarnsOnceThenRateLimited) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);  // the burst stays in-window
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<SchTag>().ok());
  ASSERT_TRUE(world
                  .registerSystem(SchWriteTag_Def,
                                  laige::Io<SchTag, laige::Access::Write>{})
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(SchWriteTag2_Def,
                                  laige::Io<SchTag, laige::Access::Write>{})
                  .ok());
  // Two writers of the same component in one tick: a scheduling error
  // (FR-12.3: fail loudly, never silent).
  laige::SystemSchedule sched;
  for (int i = 0; i < 3; ++i) {
    auto s = world.scheduleSystems(sched);
    EXPECT_FALSE(s.ok());
    if (s.isError()) {
      EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
    }
  }
  // The first failure emits the warn with the identifying fields
  // (LOG-002); the repeats are rate-limited (LOG-004) and surface as
  // the rate_limited summary on shutdown.
  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "system");
  EXPECT_EQ(sinkPtr->entries[0].event, "double_writer");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "component_id"), "1");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "first_writer"),
               "SchWriteTag");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "second_writer"),
               "SchWriteTag2");
  laige::log::Logger::instance().shutdown();
  EXPECT_EQ(sinkPtr->entries.size(), 2u);
  EXPECT_EQ(sinkPtr->entries[1].event, laige::log::kRateLimitedEvent);
  EXPECT_STREQ(fieldValue(sinkPtr->entries[1], "suppressed"), "2");
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The depends_on graph validation (missing names, cycles)
// ---------------------------------------------------------------------------

TEST(SystemScheduler, MissingDependencyRejected) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("GhostHunt", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "GhostSystem"))
                  .ok());
  laige::SystemSchedule sched;
  auto s = world.scheduleSystems(sched);
  EXPECT_FALSE(s.ok());
  if (s.isError()) {
    EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  }
  // The warn names the system and the missing name (LOG-002):
  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].event, "dep_missing");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "system"), "GhostHunt");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "missing_dep"),
               "GhostSystem");
  laige::log::Logger::instance().shutdown();
  restoreConsoleSink();
}

TEST(SystemScheduler, DependencyCycleRejected) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("CycA", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "CycB"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("CycB", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1),
                                          "CycA"))
                  .ok());
  laige::SystemSchedule sched;
  auto s = world.scheduleSystems(sched);
  EXPECT_FALSE(s.ok());
  if (s.isError()) {
    EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  }
  // The warn reports one concrete cycle (walk order from the smallest
  // remaining id): CycA -> CycB -> CycA.
  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].event, "dependency_cycle");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "cycle"), "CycA,CycB");
  laige::log::Logger::instance().shutdown();
  restoreConsoleSink();
}

TEST(SystemScheduler, SelfDependencyIsACycle) {
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("CycSelf", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "CycSelf"))
                  .ok());
  laige::SystemSchedule sched;
  auto s = world.scheduleSystems(sched);
  EXPECT_FALSE(s.ok());
  if (s.isError()) {
    EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  }
}

TEST(SystemScheduler, CycleReportedAmongIndependentSystems) {
  // A and D are not part of the cycle: A schedules first (no deps),
  // D after A. The reported cycle is the concrete B <-> C pair only —
  // the deterministic walk starts at the smallest REMAINING id (B),
  // not at A (which is already scheduled).
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("CycA", &fnNoop,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("CycB", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1),
                                          "CycC"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("CycC", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "CycB"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("CycD", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1),
                                          "CycA"))
                  .ok());
  laige::SystemSchedule sched;
  auto s = world.scheduleSystems(sched);
  EXPECT_FALSE(s.ok());
  if (s.isError()) {
    EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  }
  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].event, "dependency_cycle");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "cycle"), "CycB,CycC");
  laige::log::Logger::instance().shutdown();
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The depends_on spec validation at registration (the def-level form)
// ---------------------------------------------------------------------------

TEST(SystemScheduler, DepSpecEmptyTokenRejected) {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  laige::World world = makeWorld(0);
  auto r = world.registerSystem(makeDef("BadSpec", &fnNoop,
                                        laige::fpx16_16::fromInt32(1),
                                        "TrimA, , TrimB"));
  EXPECT_FALSE(r.ok());
  if (r.isError()) {
    EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  }
  // The world's registry is unchanged (the validation runs before the
  // record is written):
  EXPECT_EQ(world.systemCount(), 0u);
  ASSERT_EQ(sinkPtr->entries.size(), 1u);
  EXPECT_EQ(sinkPtr->entries[0].event, "dep_spec_invalid");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "name"), "BadSpec");
  EXPECT_STREQ(fieldValue(sinkPtr->entries[0], "error"), "empty_token");
  laige::log::Logger::instance().shutdown();
  restoreConsoleSink();
}

TEST(SystemScheduler, DepSpecTrailingCommaRejected) {
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(makeDef("TrailingComma", &fnNoop,
                                        laige::fpx16_16::fromInt32(1),
                                        "TrimA,"));
  EXPECT_FALSE(r.ok());
  if (r.isError()) {
    EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemScheduler, DepSpecDuplicateRejected) {
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(makeDef("DupDep", &fnNoop,
                                        laige::fpx16_16::fromInt32(1),
                                        "TrimA, TrimA"));
  EXPECT_FALSE(r.ok());
  if (r.isError()) {
    EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemScheduler, DepSpecTooManyRejected) {
  // kMaxSystemDependencies + 1 distinct names: the bound is honored.
  std::string spec;
  for (std::uint32_t i = 0; i < laige::kMaxSystemDependencies + 1; ++i) {
    if (i != 0) spec += ", ";
    spec += bulkName(i);
  }
  laige::World world = makeWorld(0);
  auto r = world.registerSystem(makeDef("TooMany", &fnNoop,
                                        laige::fpx16_16::fromInt32(1),
                                        spec.c_str()));
  EXPECT_FALSE(r.ok());
  if (r.isError()) {
    EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(world.systemCount(), 0u);
}

TEST(SystemScheduler, DepSpecWhitespaceTrimmed) {
  // Whitespace around the names is legal (the macro stringizes the
  // list as written); the trimmed names resolve against the world.
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("TrimA", &fnNoop,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("TrimB", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(
                      makeDef("TrimC", &fnNoop,
                             laige::fpx16_16::fromInt32(1),
                             " TrimA ,  TrimB "))
                  .ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.order[0], 1u);  // TrimA
  EXPECT_EQ(sched.order[1], 2u);  // TrimB
  EXPECT_EQ(sched.order[2], 3u);  // TrimC, behind both
}

// ---------------------------------------------------------------------------
// Schedule lifetime and malformed schedules
// ---------------------------------------------------------------------------

TEST(SystemScheduler, EmptyWorldSchedulesAndRunsEmpty) {
  laige::World world = makeWorld(0);
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.systemCount, 0u);
  ASSERT_TRUE(world.runSystems(sched).ok());
  clearRunLog();
  EXPECT_EQ(runLogCount, 0u);
  // A moved-from world is a valid empty world (the registry is gone):
  laige::World moved = std::move(world);
  laige::SystemSchedule empty;
  ASSERT_TRUE(moved.scheduleSystems(empty).ok());
  EXPECT_EQ(empty.systemCount, 0u);
  ASSERT_TRUE(moved.runSystems(empty).ok());
}

TEST(SystemScheduler, StaleScheduleRejected) {
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world.registerSystem(SchNoop_Def).ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  // A system registered AFTER the schedule was computed: the old
  // schedule no longer describes the registry.
  ASSERT_TRUE(world
                  .registerSystem(makeDef("LateSystem", &fnNoop,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  auto s = world.runSystems(sched);
  EXPECT_FALSE(s.ok());
  if (s.isError()) {
    EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);
  }
  clearRunLog();
  EXPECT_EQ(runLogCount, 0u);  // nothing ran
  // The recomputed schedule is usable:
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  EXPECT_EQ(sched.systemCount, 2u);
  ASSERT_TRUE(world.runSystems(sched).ok());
  EXPECT_EQ(runLogCount, 1u);
}

TEST(SystemScheduler, InvalidScheduleRejected) {
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world.registerSystem(SchNoop_Def).ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("Second", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  // A duplicate id in the order (the count matches the registry, so
  // the failure is the order validation, not the staleness check):
  laige::SystemSchedule dup;
  dup.systemCount = 2;
  dup.order[0] = 1;
  dup.order[1] = 1;
  auto s1 = world.runSystems(dup);
  EXPECT_FALSE(s1.ok());
  if (s1.isError()) {
    EXPECT_EQ(s1.error(), laige::ErrorCode::InvalidArgument);
  }
  // An id above the count:
  laige::SystemSchedule big;
  big.systemCount = 2;
  big.order[0] = 1;
  big.order[1] = 5;
  auto s2 = world.runSystems(big);
  EXPECT_FALSE(s2.ok());
  if (s2.isError()) {
    EXPECT_EQ(s2.error(), laige::ErrorCode::InvalidArgument);
  }
  // Nothing ran:
  clearRunLog();
  EXPECT_EQ(runLogCount, 0u);
}

// ---------------------------------------------------------------------------
// Determinism (ARCH-010) and the known-answer pin
// ---------------------------------------------------------------------------

// The shared registration sequence (two worlds, same order and
// specs — the ARCH-010 comparison input).
bool buildStableWorld(laige::World& world) {
  if (!world.registerSystem(SchDepOnTwo_Def).ok()) return false;
  if (!world
           .registerSystem(makeDef("StableX", &fnNoop,
                                   laige::fpx16_16::fromInt32(1)))
           .ok()) {
    return false;
  }
  if (!world.registerSystem(SchNoop_Def).ok()) return false;
  if (!world.registerSystem(SchWriteVel_Def).ok()) return false;
  return world
             .registerSystem(makeDef("StableY", &fnNoop2,
                                     laige::fpx16_16::fromInt32(1),
                                     "StableX, SchNoop"))
             .ok();
}

TEST(SystemScheduler, ScheduleIdenticalAcrossTwoWorlds) {
  laige::World w1 = makeWorld(0);
  laige::World w2 = makeWorld(0);
  ASSERT_TRUE(buildStableWorld(w1));
  ASSERT_TRUE(buildStableWorld(w2));
  laige::SystemSchedule s1;
  laige::SystemSchedule s2;
  ASSERT_TRUE(w1.scheduleSystems(s1).ok());
  ASSERT_TRUE(w2.scheduleSystems(s2).ok());
  EXPECT_EQ(s1.systemCount, s2.systemCount);
  EXPECT_EQ(s1.systemCount, 5u);
  // Bit-identical orders (the pure-function-of-registry property):
  EXPECT_EQ(std::memcmp(s1.order, s2.order,
                        s1.systemCount * sizeof(s1.order[0])),
            0);
}

TEST(SystemScheduler, KnownAnswerScheduleOrder) {
  // The fixed scenario (the KAT; machine-greppable line on every run):
  //   1. KatA      depends on KatB (a FORWARD dependency: A is
  //                 registered before B and must run after it)
  //   2. KatB      (no dependencies)
  //   3. KatC      depends on KatB, KatA
  //   4. KatD      (no dependencies)
  //   5. KatE      depends on KatD, KatC
  // The stable sort places B, A, C, D, E (min-id tie-break: the
  // forward edge moves A behind B; nothing else reorders).
  laige::World world = makeWorld(0);
  ASSERT_TRUE(world
                  .registerSystem(makeDef("KatA", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "KatB"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("KatB", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("KatC", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "KatB, KatA"))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("KatD", &fnNoop2,
                                          laige::fpx16_16::fromInt32(1)))
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(makeDef("KatE", &fnNoop,
                                          laige::fpx16_16::fromInt32(1),
                                          "KatD, KatC"))
                  .ok());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  std::uint64_t words[5];
  for (std::uint32_t i = 0; i < 5; ++i) {
    words[i] = sched.order[i];
  }
  const std::uint64_t hash = fnv1a64(words, 5);
  std::printf("scheduler-order systems=%u fnv1a=0x%016llx\n",
              sched.systemCount, static_cast<unsigned long long>(hash));
  // The exact order (not just the hash): B, A, C, D, E.
  EXPECT_EQ(sched.order[0], 2u);
  EXPECT_EQ(sched.order[1], 1u);
  EXPECT_EQ(sched.order[2], 3u);
  EXPECT_EQ(sched.order[3], 4u);
  EXPECT_EQ(sched.order[4], 5u);
  // The pinned KAT hash (docs/testing.md §4; byte-identical across
  // trees and platforms — pure integer order, no addresses).
  EXPECT_EQ(hash, 0xaef3282f393ab332ull);
}

// ---------------------------------------------------------------------------
// Zero allocation: scheduling and the per-tick run (PERF-003)
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
TEST(SystemScheduler, SchedulingAndTicksAllocateNothing) {
  // The schedule is fixed-size stack/world state and runSystems builds
  // a trivial SystemContext per system: nothing in the window may
  // touch the heap.
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<SchVel>().ok());
  ASSERT_TRUE(world.registerComponent<SchPos>().ok());
  for (std::uint32_t i = 0; i < 4; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    ASSERT_TRUE(world.addComponent<SchVel>(e.value(), SchVel{0}).ok());
    ASSERT_TRUE(world.addComponent<SchPos>(e.value(), SchPos{1, 1}).ok());
  }
  ASSERT_TRUE(world
                  .registerSystem(SchWriteVel_Def,
                                  laige::Io<SchVel, laige::Access::Write>{})
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(SchReadVel_Def,
                                  laige::Io<SchVel, laige::Access::Read>{})
                  .ok());
  ASSERT_TRUE(world
                  .registerSystem(SchReadPos_Def,
                                  laige::Io<SchPos, laige::Access::Read>{})
                  .ok());
  laige::SystemSchedule sched;
  laige::test::resetAllocCounter();
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  for (int tick = 0; tick < 100; ++tick) {
    world.beginFrame();
    ASSERT_TRUE(world.runSystems(sched).ok());
  }
  // Every tick: the writer ran before the readers — 4 entities each
  // reading the fresh 0x1234, and x=1 for each of the 4 Pos carriers.
  EXPECT_EQ(schReadVelSum, 4u * 0x1234u);
  EXPECT_EQ(schReadPosSum, 4u);
  const std::uint64_t allocs = laige::test::allocCounter();
  std::printf("scheduler-zeroalloc ticks=100 allocs=%llu\n",
              static_cast<unsigned long long>(allocs));
  EXPECT_EQ(allocs, 0u);
}
#endif
