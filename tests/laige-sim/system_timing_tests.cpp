// laige-sim per-system timing + budget enforcement suite (M1-SYS-03).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - a synthetic slow system fires the budget_overrun WARN at the
//     documented 1× multiplier, and the budget_critical ERROR at the
//     documented 3× multiplier (the warn first, the error after, in
//     the same tick)
//   - the rolling histogram window resets correctly: samples beyond
//     kSystemTimingWindowSamples drop the OLDEST sample (count caps
//     at the capacity, totalRecorded keeps counting, and stats()
//     reflects exactly the stored window — old burn samples are
//     gone after the fast ticks roll in)
//   - healthy systems log nothing (the success path is silent,
//     LOG-003) and the cheap SystemTimingStats track the runs
//   - the events follow the NFR-13.3 5-field message grammar
//     ({code} | {what} | {why} | {fix} | {doc_anchor})
//   - the warn's rolling p99 field carries the window's p99
//   - the threshold assertions are preemption-tolerant: shared-
//     runner deschedules can only STRETCH a measured run (never
//     shorten it), so the over-budget tests branch on the measured
//     time instead of assuming the nominal margin held (the
//     2026-09-21 macOS arm64 flake stretched the 6.5 ms burn past
//     the 15 ms critical threshold)
//   - the queries validate ids (invalid id / no-systems world /
//     moved-from world)
//   - the timing state travels with the world on move
//   - no heap allocation on the healthy per-tick timing path
//     (test-only operator-new counter, non-sanitizer trees; the
//     sanitizer trees prove it leak-free)
//
// Runs as CTest `system_timing` (the step's Verify command:
// `ctest -R system_timing`): a filtered view of the shared
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
              "system_timing_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "system_timing_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "system_timing_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define SYSTEM_TIMING_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define SYSTEM_TIMING_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

static_assert(SYSTEM_TIMING_TESTS_ACTIVE_CPLUSPLUS >= 202002L,
              "system_timing_tests must be built with C++20 "
              "(NFR-8.10); see laige_apply_engine_policy().");

// LAIGE_COMPONENT specializes laige::detail::ComponentTraits, which
// must be specialized at global scope (the component_registry_tests
// pattern).
struct STTag {
  std::int32_t v{};
};
LAIGE_COMPONENT(STTag)
// M1-DET-01 (G-R8): integer-only storage (determinism.h trait).
LAIGE_DETERMINISM_SAFE(STTag, std::int32_t)

namespace {

using laige::Access;
using laige::ErrorCode;
using laige::Entity;
using laige::Io;
using laige::SystemDef;
using laige::SystemId;
using laige::SystemSchedule;
using laige::SystemTimingStats;
using laige::World;

// ---------------------------------------------------------------------------
// The synthetic slow system (the step's "synthetic slow system")
// ---------------------------------------------------------------------------

// The burn target (ms) of STBurn — test plumbing: the tests set it
// before running a tick (the sim is single-threaded, PRD §10.2). 0
// means a healthy no-burn run.
double stBurnMs = 0.0;

// The burn loop's accumulator (namespace scope: written, never read —
// the loop's work must not be eliminated, and a namespace-scope
// variable carries no unused-variable diagnostic).
volatile std::uint64_t gBurnSink = 0;

// Burn roughly `ms` milliseconds of wall time: a clock-driven loop,
// machine-independent in intent (the measured duration is ~ms plus a
// small clock-check granularity). The volatile sink defeats
// elimination.
void burnMs(double ms) {
  if (ms <= 0.0) return;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             std::chrono::steady_clock::now() - start).count() < ms) {
    // Ordinary assignment: compound assignment to a volatile is
    // deprecated in C++20 (P1152R2) and a hard error under
    // -Werror,-Wdeprecated-volatile (AppleClang on the macos-14 job).
    gBurnSink = gBurnSink + 1;
  }
}

LAIGE_SYSTEM(STBurn, 1)
void STBurn(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
  burnMs(stBurnMs);
}

LAIGE_SYSTEM(STNoop, 1)
void STNoop(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

#if defined(LAIGE_ALLOC_COUNTER)
// Two plain functions for the two-system zero-alloc window (a
// manual def needs a distinct function per registration name).
// Only the non-sanitizer trees (the alloc-counter trees) define the
// test that uses them.
void fnNoopA(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

void fnNoopB(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}
#endif

// ---------------------------------------------------------------------------
// World + def builders (the scheduler/system_registry pattern)
// ---------------------------------------------------------------------------

World makeWorld() {
  auto w = World::create(World::Options{16});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(16) failed: "
                  << laige::errorName(w.error());
    abort();
  }
  World world = std::move(w).takeValue();
  // STTag is registered in every test world: the test systems declare
  // it in their I/O (the M1-SYS-01 io_unregistered check). The success
  // path logs nothing, so this setup does not reach the test sinks.
  if (!world.registerComponent<STTag>().ok()) {
    ADD_FAILURE() << "registerComponent<STTag> failed";
    abort();
  }
  return world;
}

SystemDef makeDef(const char* name, laige::SystemFn fn,
                  laige::fpx16_16 budgetMs) {
  return SystemDef{name, fn, budgetMs, nullptr};
}

// ---------------------------------------------------------------------------
// Log capture (the logging_tests / scheduler_tests pattern)
// ---------------------------------------------------------------------------

// A test-only Sink that records every emitted event (the logging
// facade is a process singleton; the tests that use it restore the
// default console sink at the end — the scheduler_tests pattern).
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

MemorySink* sink = nullptr;

// Install the capture sink (a 60 s rate window: the tests' repeated
// events stay within one window, so the rate-limited repeats are
// suppressed and summarized at shutdown — the scheduler_tests
// pattern).
MemorySink* installCaptureSink() {
  auto mem = std::make_unique<MemorySink>();
  MemorySink* memPtr = mem.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(mem);
  opts.rateWindow = std::chrono::seconds(60);
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init failed";
    std::abort();
  }
  sink = memPtr;
  return memPtr;
}

void restoreConsoleSink() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger re-init with the default console sink failed";
    std::abort();
  }
  sink = nullptr;
}

std::size_t countEvents(const MemorySink& s, const char* event) {
  std::size_t n = 0;
  for (const auto& e : s.entries) {
    if (e.event == event) ++n;
  }
  return n;
}

const char* fieldValue(const MemorySink::Entry& entry, const char* key) {
  for (const auto& [k, v] : entry.fields) {
    if (k == key) return v.c_str();
  }
  return "";
}

// Parse a field value as a double (the field() double rendering is the
// shortest round-trip decimal — std::from_chars is exact).
double fieldMs(const MemorySink::Entry& e, const char* key) {
  const char* v = fieldValue(e, key);
  EXPECT_TRUE(v[0] != '\0') << "field " << key << " missing";
  char* end = nullptr;
  const double d = std::strtod(v, &end);
  EXPECT_TRUE(end != nullptr && *end == '\0')
      << "field " << key << " unparsable: " << v;
  return d;
}

// Run one tick (beginFrame + runSystems), asserting the run status.
void runTick(World& w, const SystemSchedule& sched) {
  w.beginFrame();
  ASSERT_TRUE(w.runSystems(sched).ok());
}

}  // namespace

// ---------------------------------------------------------------------------
// Healthy path: nothing is logged, the scalars track the runs
// ---------------------------------------------------------------------------

TEST(SystemTiming, HealthyTicksLogNothingAndTrackStats) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld();
  // Budget 100 ms: the success-path silence asserted below is a
  // LOG-003 property of runs that STAY UNDER budget — and shared-
  // runner preemption can only stretch a microsecond noop tick. A
  // 100 ms deschedule spike on one such tick is the shared-runner
  // noise floor this repo treats as negligible (the archetype
  // churn's kChurnMaxNsPerRowShifted comment), so the budget carries
  // the slack a 1 ms budget could not.
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STNoop", &STNoop,
                                           laige::fpx16_16::fromInt32(100)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());
  for (int tick = 0; tick < 8; ++tick) {
    runTick(w, sched);
  }

  // The success path is silent (LOG-003): no event at all.
  EXPECT_EQ(mem->entries.size(), 0u);

  auto st = w.systemTimingStats(SystemId{1});
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(st.value().runs, 8u);
  EXPECT_EQ(st.value().warns, 0u);
  EXPECT_EQ(st.value().errors, 0u);
  // A noop system on an empty world runs in microseconds — well
  // under its 100 ms budget. The lower bound is >= 0.0, not > 0.0:
  // a run shorter than the platform's steady_clock tick measures as
  // exactly 0.0 ms (the start and end reads land on the same tick),
  // and that sub-resolution reading is a legitimate value, not a
  // failure (the tracked-state check is the runs counter above;
  // lastMs is a diagnostic). The upper bound is the budget, not the
  // microsecond expectation: a preemption-stretched run is a
  // legitimate over-budget value, not a failure of the tracked
  // state (and would carry its own budget_overrun event, which the
  // silence assertion above rejects).
  EXPECT_GE(st.value().lastMs, 0.0);
  EXPECT_LT(st.value().lastMs, 100.0);

  const laige::Histogram* win = w.systemTimingWindow(SystemId{1});
  ASSERT_TRUE(win != nullptr);
  // 8 ticks < the window capacity: every sample is stored.
  EXPECT_EQ(win->count(), 8u);
  EXPECT_EQ(win->totalRecorded(), 8u);

  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The documented multipliers: warn at 1×, error at 3×
// ---------------------------------------------------------------------------

TEST(SystemTiming, OverBudgetSystemWarnsAtTheDocumentedMultiplier) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld();
  // Budget 5 ms, burn ~6.5 ms: over 1× (warn) but nominally under 3×
  // (15 ms). Shared-runner preemption can only STRETCH the busy-wait
  // run (it can finish late, never early), so each tick's measured
  // time is either nominal (under 15 ms: the warn only) or stretched
  // (15 ms or more: the warn plus the documented same-tick critical).
  // Both branches are consistent executions of the documented
  // multipliers — the assertions branch on the measured time, so the
  // test is immune to the preemption that stretched the first run
  // past 3× in the 2026-09-21 macOS arm64 CI run (two events, not
  // one, failed the old exact-count assertion there).
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STBurn", &STBurn,
                                           laige::fpx16_16::fromInt32(5)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());

  stBurnMs = 6.5;
  runTick(w, sched);
  auto st = w.systemTimingStats(SystemId{1});
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(st.value().runs, 1u);
  // The 1× rule always holds: the burn floor (~6.5 ms) exceeds the
  // budget, and preemption only stretches the run — the warn always
  // fires.
  EXPECT_EQ(st.value().warns, 1u);
  const bool firstCritical = st.value().lastMs >= 15.0;
  EXPECT_EQ(st.value().errors, firstCritical ? 1u : 0u);

  if (firstCritical) {
    // Stretched past 3×: the documented same-tick order — the warn
    // first, the critical after (the sibling CriticallyOverBudget
    // test pins that order unconditionally).
    ASSERT_EQ(mem->entries.size(), 2u);
    EXPECT_EQ(mem->entries[0].event, "budget_overrun");
    EXPECT_EQ(mem->entries[0].severity, laige::log::Severity::Warn);
    EXPECT_EQ(mem->entries[1].event, "budget_critical");
    EXPECT_EQ(mem->entries[1].severity, laige::log::Severity::Error);
    EXPECT_GE(fieldMs(mem->entries[1], "measured_ms"), 15.0);
  } else {
    // Nominal: exactly one entry — the warn, with the documented
    // fields.
    ASSERT_EQ(mem->entries.size(), 1u);
    const auto& e = mem->entries[0];
    EXPECT_EQ(e.event, "budget_overrun");
    EXPECT_EQ(e.subsystem, "system");
    EXPECT_EQ(e.severity, laige::log::Severity::Warn);
    EXPECT_STREQ(fieldValue(e, "system"), "STBurn");
    EXPECT_STREQ(fieldValue(e, "id"), "1");
    EXPECT_DOUBLE_EQ(fieldMs(e, "budget_ms"), 5.0);
    const double measured = fieldMs(e, "measured_ms");
    EXPECT_GE(measured, 6.0);
    EXPECT_LT(measured, 15.0);
    // The rolling p99: the window holds exactly this one sample.
    EXPECT_NEAR(fieldMs(e, "p99_ms"), measured, 1.0);
    EXPECT_STREQ(fieldValue(e, "window_samples"), "1");
  }

  // The next over-budget tick: the counters keep counting. The
  // overrun repeat is rate-limited (LOG-004) — no new entry. A
  // stretched second tick adds its critical: logged as the first
  // breach of its class, suppressed as a repeat after one.
  stBurnMs = 6.5;
  runTick(w, sched);
  auto st2 = w.systemTimingStats(SystemId{1});
  ASSERT_TRUE(st2.ok());
  EXPECT_EQ(st2.value().runs, 2u);
  EXPECT_EQ(st2.value().warns, 2u);
  const bool secondCritical = st2.value().lastMs >= 15.0;
  EXPECT_EQ(st2.value().errors,
            (firstCritical ? 1u : 0u) + (secondCritical ? 1u : 0u));
  // One overrun entry always; the critical entry appears iff some
  // tick was the first (logged) breach of its class.
  EXPECT_EQ(mem->entries.size(),
            1u + (firstCritical ? 1u : 0u) +
            (!firstCritical && secondCritical ? 1u : 0u));

  // Shutdown: one rate_limited summary per suppressed class — the
  // overrun always (the second tick's repeat), the critical only
  // when both ticks were stretched (the second is the suppressed
  // repeat of the first's logged event).
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(mem->entries.size(),
            2u + (firstCritical ? 1u : 0u) +
            (!firstCritical && secondCritical ? 1u : 0u) +
            (firstCritical && secondCritical ? 1u : 0u));
  EXPECT_EQ(countEvents(*mem, "rate_limited"),
            1u + (firstCritical && secondCritical ? 1u : 0u));
  const MemorySink::Entry* overrunSummary = nullptr;
  const MemorySink::Entry* criticalSummary = nullptr;
  for (const auto& e : mem->entries) {
    if (e.event != "rate_limited") continue;
    if (std::strcmp(fieldValue(e, "event"), "budget_overrun") == 0)
      overrunSummary = &e;
    if (std::strcmp(fieldValue(e, "event"), "budget_critical") == 0)
      criticalSummary = &e;
  }
  ASSERT_TRUE(overrunSummary != nullptr);
  EXPECT_STREQ(fieldValue(*overrunSummary, "suppressed"), "1");
  if (firstCritical && secondCritical) {
    ASSERT_TRUE(criticalSummary != nullptr);
    EXPECT_STREQ(fieldValue(*criticalSummary, "suppressed"), "1");
  } else {
    EXPECT_TRUE(criticalSummary == nullptr);
  }
  sink = nullptr;
}

TEST(SystemTiming, CriticallyOverBudgetSystemErrorsAfterTheWarn) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld();
  // Budget 1 ms, burn ~7 ms: over 3× (3 ms) — the error event fires,
  // and the warn (a lower threshold, the same breach) fires first in
  // the same tick.
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STBurn", &STBurn,
                                           laige::fpx16_16::fromInt32(1)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());
  stBurnMs = 7.0;
  runTick(w, sched);

  // Two events, in the documented order: the warn, then the error.
  ASSERT_EQ(mem->entries.size(), 2u);
  EXPECT_EQ(mem->entries[0].event, "budget_overrun");
  EXPECT_EQ(mem->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_EQ(mem->entries[1].event, "budget_critical");
  EXPECT_EQ(mem->entries[1].severity, laige::log::Severity::Error);
  EXPECT_EQ(mem->entries[1].subsystem, "system");
  for (const auto& e : mem->entries) {
    EXPECT_STREQ(fieldValue(e, "system"), "STBurn");
    EXPECT_STREQ(fieldValue(e, "id"), "1");
    EXPECT_DOUBLE_EQ(fieldMs(e, "budget_ms"), 1.0);
    EXPECT_GE(fieldMs(e, "measured_ms"), 7.0);
    EXPECT_GE(fieldMs(e, "p99_ms"), 7.0);
    EXPECT_STREQ(fieldValue(e, "window_samples"), "1");
  }

  auto st = w.systemTimingStats(SystemId{1});
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(st.value().runs, 1u);
  EXPECT_EQ(st.value().warns, 1u);
  EXPECT_EQ(st.value().errors, 1u);

  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The rolling window: oldest-sample drop and stats over the window
// ---------------------------------------------------------------------------

TEST(SystemTiming, RollingWindowDropsOldestSamples) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld();
  // Budget 50 ms: the 7 ms burn never crosses it — no events, pure
  // window mechanics.
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STBurn", &STBurn,
                                           laige::fpx16_16::fromInt32(50)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());
  const std::uint32_t cap = laige::kSystemTimingWindowSamples;
  const auto* win = w.systemTimingWindow(SystemId{1});
  ASSERT_TRUE(win != nullptr);

  // Phase 1: 3*cap healthy ticks — the window is full of fast samples.
  stBurnMs = 0.0;
  for (std::uint32_t i = 0; i < 3 * cap; ++i) {
    runTick(w, sched);
  }
  EXPECT_EQ(win->count(), cap);
  EXPECT_EQ(win->totalRecorded(), 3 * cap);
  EXPECT_LT(win->stats().max, 5.0);  // fast ticks only

  // Phase 2: cap burn ticks — the window is now full of burn samples.
  stBurnMs = 7.0;
  for (std::uint32_t i = 0; i < cap; ++i) {
    runTick(w, sched);
  }
  EXPECT_EQ(win->count(), cap);
  EXPECT_EQ(win->totalRecorded(), 4 * cap);
  EXPECT_GT(win->stats().min, 6.5);  // the fast samples rolled out

  // Phase 3: cap healthy ticks — the burn samples rolled out (the
  // window reset: exactly the stored window is visible).
  stBurnMs = 0.0;
  for (std::uint32_t i = 0; i < cap; ++i) {
    runTick(w, sched);
  }
  EXPECT_EQ(win->count(), cap);
  EXPECT_EQ(win->totalRecorded(), 5 * cap);
  EXPECT_LT(win->stats().max, 5.0);  // the old 7 ms samples are gone
  EXPECT_LT(win->stats().p99, 5.0);

  auto st = w.systemTimingStats(SystemId{1});
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(st.value().runs, 5 * cap);
  EXPECT_EQ(st.value().warns, 0u);
  EXPECT_EQ(st.value().errors, 0u);
  // The success path stayed silent through every phase.
  EXPECT_EQ(mem->entries.size(), 0u);

  std::printf("system-timing window ticks=%u capacity=%u total=%llu\n",
              5u * cap, cap,
              static_cast<unsigned long long>(win->totalRecorded()));
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The NFR-13.3 message grammar
// ---------------------------------------------------------------------------

TEST(SystemTiming, EventsFollowTheErrorGrammar) {
  // NFR-13.3: every engine error follows
  // {code} | {what} | {why} | {fix} | {doc_anchor} — the timing events
  // carry the same 5-field line in their message text (identical in
  // every build; the dynamic values are structured fields, never
  // message text).
  MemorySink* mem = installCaptureSink();
  World w = makeWorld();
  // Two systems, one burn amount (6.5 ms), both over their 2 ms
  // budgets by 3× or more (6.5 >= 6 — both event classes fire on
  // EVERY tick, whatever preemption stretches the runs; a warn-only
  // system would make the event sequence depend on whether
  // preemption stretched its run past the 3× threshold — the flake
  // the OverBudget test's branch removes). The first system (the
  // registration/schedule order) logs the window's first overrun and
  // first critical; the second system's repeats of both classes are
  // rate-limited per (subsystem, event, severity) (LOG-004) and
  // summarized at shutdown.
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STBurnWarn", &STBurn,
                                           laige::fpx16_16::fromInt32(2)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STBurnCrit", &STBurn,
                                           laige::fpx16_16::fromInt32(2)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());
  stBurnMs = 6.5;
  runTick(w, sched);

  // The tick emits exactly two events, in the documented order:
  // system 1's warn, then system 1's critical. System 2's warn (the
  // second budget_overrun in this 60 s rate window) and system 2's
  // critical (the second budget_critical) are rate-limited and
  // summarized at shutdown.
  EXPECT_EQ(countEvents(*mem, "budget_overrun"), 1u);
  EXPECT_EQ(countEvents(*mem, "budget_critical"), 1u);
  ASSERT_EQ(mem->entries.size(), 2u);
  EXPECT_EQ(mem->entries[0].event, "budget_overrun");
  EXPECT_EQ(mem->entries[0].severity, laige::log::Severity::Warn);
  EXPECT_STREQ(fieldValue(mem->entries[0], "system"), "STBurnWarn");
  EXPECT_EQ(mem->entries[1].event, "budget_critical");
  EXPECT_EQ(mem->entries[1].severity, laige::log::Severity::Error);
  EXPECT_STREQ(fieldValue(mem->entries[1], "system"), "STBurnWarn");

  for (const auto& e : mem->entries) {
    // Split the message on " | ": exactly 5 fields, none empty.
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
      const std::size_t pos = e.message.find(" | ", start);
      if (pos == std::string::npos) {
        fields.push_back(e.message.substr(start));
        break;
      }
      fields.push_back(e.message.substr(start, pos - start));
      start = pos + 3;
    }
    ASSERT_EQ(fields.size(), 5u) << "message: " << e.message;
    for (const auto& f : fields) {
      EXPECT_FALSE(f.empty()) << "message: " << e.message;
    }
    // {code} names the event; {doc_anchor} points at the timing docs.
    EXPECT_EQ(fields[0], e.event);
    EXPECT_EQ(fields[4], "docs/api/system_timing.md");
  }

  // Shutdown: one rate_limited summary per suppressed class — system
  // 2's overrun repeat and system 2's critical repeat, each counting
  // exactly one suppressed event.
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(mem->entries.size(), 4u);
  EXPECT_EQ(countEvents(*mem, "rate_limited"), 2u);
  const MemorySink::Entry* overrunSummary = nullptr;
  const MemorySink::Entry* criticalSummary = nullptr;
  for (const auto& e : mem->entries) {
    if (e.event != "rate_limited") continue;
    if (std::strcmp(fieldValue(e, "event"), "budget_overrun") == 0)
      overrunSummary = &e;
    if (std::strcmp(fieldValue(e, "event"), "budget_critical") == 0)
      criticalSummary = &e;
  }
  ASSERT_TRUE(overrunSummary != nullptr);
  EXPECT_STREQ(fieldValue(*overrunSummary, "suppressed"), "1");
  ASSERT_TRUE(criticalSummary != nullptr);
  EXPECT_STREQ(fieldValue(*criticalSummary, "suppressed"), "1");
  sink = nullptr;
}

// ---------------------------------------------------------------------------
// The profiler feed: query validation + the window reference
// ---------------------------------------------------------------------------

TEST(SystemTiming, QueriesValidateIdsAndNoSystemsWorld) {
  World w = makeWorld();
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STNoop", &STNoop,
                                           laige::fpx16_16::fromInt32(1)),
                                   Io<STTag, Access::Read>{})
                  .ok());

  // Pure queries: invalid ids fail without logging.
  auto r0 = w.systemTimingStats(SystemId{0});
  ASSERT_FALSE(r0.ok());
  EXPECT_EQ(r0.error(), ErrorCode::InvalidArgument);
  auto r2 = w.systemTimingStats(SystemId{2});
  ASSERT_FALSE(r2.ok());
  EXPECT_EQ(r2.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(w.systemTimingWindow(SystemId{0}), nullptr);
  EXPECT_EQ(w.systemTimingWindow(SystemId{2}), nullptr);
  ASSERT_TRUE(w.systemTimingStats(SystemId{1}).ok());
  ASSERT_TRUE(w.systemTimingWindow(SystemId{1}) != nullptr);

  // A world with no registered systems: id 1 is above the (empty)
  // registry — invalid in both queries.
  World empty = makeWorld();
  auto re = empty.systemTimingStats(SystemId{1});
  ASSERT_FALSE(re.ok());
  EXPECT_EQ(re.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(empty.systemTimingWindow(SystemId{1}), nullptr);
}

TEST(SystemTiming, TimingStateTravelsWithMove) {
  World w = makeWorld();
  ASSERT_TRUE(w
                  .registerSystem(makeDef("STNoop", &STNoop,
                                           laige::fpx16_16::fromInt32(1)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());
  for (int tick = 0; tick < 5; ++tick) {
    runTick(w, sched);
  }
  World w2 = std::move(w);

  // The runs, the counters, and the window travel with the world.
  auto st = w2.systemTimingStats(SystemId{1});
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(st.value().runs, 5u);
  EXPECT_EQ(st.value().warns, 0u);
  EXPECT_EQ(st.value().errors, 0u);
  const laige::Histogram* win = w2.systemTimingWindow(SystemId{1});
  ASSERT_TRUE(win != nullptr);
  EXPECT_EQ(win->totalRecorded(), 5u);

  // The moved-from world is a valid empty world: the timing queries
  // fail as pure queries (no registry, no timing table).
  auto rm = w.systemTimingStats(SystemId{1});
  ASSERT_FALSE(rm.ok());
  EXPECT_EQ(rm.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(w.systemTimingWindow(SystemId{1}), nullptr);
  EXPECT_EQ(w.systemCount(), 0u);
}

// ---------------------------------------------------------------------------
// The zero-allocation healthy path (M1 zero-alloc property, PERF-003)
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
TEST(SystemTiming, HealthyTicksAllocateNothing) {
  // The M1-ECS-03/07 pattern: the test-only operator-new counter
  // (non-sanitizer trees; the sanitizer trees prove it leak-free).
  // The 100 ms budgets carry the same preemption slack as the
  // HealthyTicksLogNothingAndTrackStats budget above (a 100 ms
  // deschedule spike on a microsecond noop tick is the shared-runner
  // noise floor; a stretched run would log a budget_overrun, which
  // the silence assertion below rejects).
  MemorySink* mem = installCaptureSink();
  World w = makeWorld();
  ASSERT_TRUE(w
                  .registerSystem(makeDef("NoopA", &fnNoopA,
                                           laige::fpx16_16::fromInt32(100)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  ASSERT_TRUE(w
                  .registerSystem(makeDef("NoopB", &fnNoopB,
                                           laige::fpx16_16::fromInt32(100)),
                                   Io<STTag, Access::Read>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());
  laige::test::resetAllocCounter();
  for (int tick = 0; tick < 100; ++tick) {
    runTick(w, sched);
  }
  const std::uint64_t allocs = laige::test::allocCounter();
  // Per tick per system: two clock reads, one O(1) ring write, two
  // comparisons — nothing touches the heap while the systems stay
  // under budget.
  std::printf("system-timing-zeroalloc ticks=100 allocs=%llu\n",
              static_cast<unsigned long long>(allocs));
  EXPECT_EQ(allocs, 0u);
  EXPECT_EQ(mem->entries.size(), 0u);
  restoreConsoleSink();
}
#endif
