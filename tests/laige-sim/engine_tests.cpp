// laige-sim headless engine run suite (M1-HEAD-01).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - the double-shutdown test is green (CONC-006: shutdown is
//     idempotent — after a run, without a run, and a third call)
//   - the Engine lifecycle: config -> world -> systems -> loop (the
//     built-in Position2D registers FIRST, the game's registrations
//     follow, the run schedules once and drives the loop)
//   - run_headless completes the requested ticks exactly under a
//     healthy cadence (frame budget 1: one tick per frame, no drops)
//     and reports the loop accounting (the profiler feed)
//   - the config surface (M1-CFG-01): the versioned schema loader, the
//     rejection table, and the hot reloader live in
//     game_config_tests.cpp (the CTest entry `game_config`)
//   - a stopped engine (post-run / moved-from) fails without logging
//     (the stopped-state precedent)
//   - no GPU/window symbols: guaranteed by the include-graph lint
//     (NFR-8.11 — the engine links only laige-sim + laige-core; no
//     dedicated test here, the lint job IS the check)
//   - the zero-allocation headless frame path (PERF-003; test-only
//     operator-new counter, non-sanitizer trees — the sanitizer
//     trees prove the run leak-free)
//
// The laige-run CLI smoke (1000-tick run exits 0 on all P0 OSes) is
// the CTest entry `laige_run_smoke` (tools/run); the engine-level
// suites below run as CTest `engine`.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim/engine.h"
#include "laige/sim/game_loop.h"
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
              "engine_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "engine_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "engine_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test fixtures (the MemorySink capture pattern —
// ecs_guardrails_tests.cpp)
// ---------------------------------------------------------------------------

// The scratch system: counts completed ticks (no component I/O —
// the scheduler's zero-entry case). The global counter is written
// only on the owner thread (PRD §10.2: one world, one thread).
namespace {
std::uint64_t gTickCount = 0;
}

LAIGE_SYSTEM(EngTickCounter, 1)
void EngTickCounter(laige::World&, laige::SystemContext&) {
  ++gTickCount;
}

namespace {

// One log event captured from the facade (severity >= Warn only —
// the Info lifecycle events of the engine run are not asserted
// here; they are low-volume by design).
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
// assert the source-level behavior, not the facade's LOG-004 window.
// (Re-)initializes the facade — required because an Engine shutdown
// retires it (CONC-006/LOG-007).
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
    if (e.event == event) ++n;
  }
  return n;
}

// Creates an engine, failing the test loudly on a setup error (the
// test configs below are all valid — a failure here is a bug in the
// test or the engine, never a scenario).
laige::Engine makeEngine(laige::EngineConfig config) {
  laige::Result<laige::Engine, laige::ErrorCode> r =
      laige::Engine::create(config);
  if (!r.ok()) {
    ADD_FAILURE() << "Engine::create failed: " << laige::errorName(r.error());
    abort();
  }
  return std::move(r).takeValue();
}

}  // namespace

// ---------------------------------------------------------------------------
// Engine::create (config validation, the built-in registration)
// ---------------------------------------------------------------------------

TEST(EngineCreate, DefaultsBuildAnEmptyWorld) {
  laige::EngineConfig config;  // all defaults
  laige::Result<laige::Engine, laige::ErrorCode> result =
      laige::Engine::create(config);
  ASSERT_TRUE(result.ok());
  laige::Engine engine = std::move(result).takeValue();
  // The built-in component registers FIRST (ARCH-010): one
  // registered type, the world otherwise empty.
  laige::World* world = engine.world();
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->componentCount(), 1u);
  EXPECT_EQ(world->systemCount(), 0u);
  // The config echo carries the validated values.
  EXPECT_EQ(engine.config().tickRateHz, laige::kDefaultTickRateHz);
  EXPECT_EQ(engine.config().entityCapacity, 0u);
  EXPECT_EQ(engine.config().churnPerFrameBudget,
            laige::kDefaultChurnPerFrameBudget);
  engine.shutdown();
}

TEST(EngineCreate, ExplicitConfigEchoes) {
  const laige::EngineConfig config{120, 4096, 100};
  laige::Result<laige::Engine, laige::ErrorCode> result =
      laige::Engine::create(config);
  ASSERT_TRUE(result.ok());
  laige::Engine engine = std::move(result).takeValue();
  EXPECT_EQ(engine.config().tickRateHz, 120u);
  EXPECT_EQ(engine.config().entityCapacity, 4096u);
  EXPECT_EQ(engine.config().churnPerFrameBudget, 100u);
  engine.shutdown();
}

TEST(EngineCreate, TickRateOutOfRangeRejected) {
  MemorySink* sink = installCaptureSink();
  for (const std::uint32_t bad : {0u, 19u, 121u, 1000u}) {
    const laige::EngineConfig config{bad, 0, 0};
    const laige::Result<laige::Engine, laige::ErrorCode> result =
        laige::Engine::create(config);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
    EXPECT_EQ(countEvents(*sink, "tick_rate_invalid"),
              static_cast<std::size_t>(sink->entries.size()));
  }
  EXPECT_EQ(sink->entries.size(), 4u);  // one warn per rejection
  restoreLogger();
}

TEST(EngineCreate, EntityBudgetAboveHandleSpaceRejected) {
  MemorySink* sink = installCaptureSink();
  const laige::EngineConfig config{60, laige::Entity::kMaxEntities + 1, 0};
  const laige::Result<laige::Engine, laige::ErrorCode> result =
      laige::Engine::create(config);
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(sink->entries.size(), 0u);  // the World::create precedent: no warn
  restoreLogger();
}

// ---------------------------------------------------------------------------
// run_headless (the bounded run, the loop accounting, the stop states)
// ---------------------------------------------------------------------------

TEST(EngineRun, BoundedRunCompletesExactlyWithSystems) {
  gTickCount = 0;
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 128, 256});
  const laige::Result<laige::SystemId, laige::ErrorCode> registered =
      engine.world()->registerSystem(EngTickCounter_Def);
  ASSERT_TRUE(registered.ok());
  const laige::Status status = engine.run_headless(5, 1);
  // Frame budget 1: each frame runs AT MOST one tick, so the run
  // lands EXACTLY on the target under any cadence (a late frame
  // drops, it does not overshoot — the M1-LOOP-01 contract).
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(gTickCount, 5u);
  EXPECT_EQ(engine.stats().ticks, 5u);
  // (droppedTicks is NOT asserted: a loaded runner may drop ticks
  // between frames — the M1-LOOP-01 overload behavior; the tick
  // count still lands exactly on the target under frame budget 1.)
  EXPECT_TRUE(engine.isShutDown());  // the run always ends in shutdown
}

TEST(EngineRun, BoundedRunWithoutSystems) {
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 64, 256});
  const laige::Status status = engine.run_headless(2, 1);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(engine.stats().ticks, 2u);
  EXPECT_GE(engine.stats().frames, 3u);  // first frame (0 ticks) + 2
  EXPECT_TRUE(engine.isShutDown());
}

TEST(EngineRun, ZeroFrameBudgetRejected) {
  MemorySink* sink = installCaptureSink();
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 64, 256});
  const laige::Status status = engine.run_headless(2, 0);
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "catchup_invalid"), 1u);
  // The run ALWAYS ends in the ordered shutdown (CONC-006) — a
  // failed start included: the world is released, and the caller's
  // shutdown() is the idempotent no-op.
  EXPECT_TRUE(engine.isShutDown());
  EXPECT_EQ(engine.world(), nullptr);
  engine.shutdown();  // the idempotency the step verifies
  EXPECT_TRUE(engine.isShutDown());
  restoreLogger();
}

TEST(EngineRun, SecondRunFailsWithoutLogging) {
  MemorySink* sink = installCaptureSink();
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 64, 256});
  ASSERT_TRUE(engine.run_headless(1, 1).ok());
  // The first run is wall-clock paced: on a loaded runner a frame can
  // run longer than one tick of simulation time, and the loop then
  // warns (loop/tick_dropped) — the documented M1-LOOP-01 overload
  // behavior (the BoundedRun tests above deliberately do not assert
  // the drop count for this reason). Those entries belong to the
  // first run's accounting, not the stopped-state failure: the
  // assertion below is scoped to the SECOND run.
  const std::size_t entriesAfterFirstRun = sink->entries.size();
  const laige::Status second = engine.run_headless(1, 1);
  ASSERT_TRUE(second.isError());
  EXPECT_EQ(second.error(), laige::ErrorCode::InvalidArgument);
  // The stopped-state failure is a pure failure: no log (the
  // GameLoop's moved-out precedent); the Info lifecycle events of
  // both runs are below the capture sink's Warn floor anyway.
  EXPECT_EQ(sink->entries.size(), entriesAfterFirstRun);
  restoreLogger();
}

// ---------------------------------------------------------------------------
// shutdown (CONC-006: ordered, idempotent)
// ---------------------------------------------------------------------------

TEST(EngineShutdown, DoubleShutdownAfterRun) {
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 64, 256});
  ASSERT_TRUE(engine.run_headless(3, 1).ok());
  ASSERT_TRUE(engine.isShutDown());
  engine.shutdown();  // second call: no-op
  engine.shutdown();  // third call: no-op
  SUCCEED();
}

TEST(EngineShutdown, DoubleShutdownWithoutRun) {
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 64, 256});
  engine.shutdown();
  engine.shutdown();
  EXPECT_TRUE(engine.isShutDown());
  SUCCEED();
}

TEST(EngineShutdown, ShutdownReleasesTheWorld) {
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 8, 256});
  ASSERT_TRUE(engine.world()->create().ok());  // one live entity
  engine.shutdown();
  EXPECT_TRUE(engine.isShutDown());
  EXPECT_EQ(engine.world(), nullptr);  // released, queryable as null
}

// ---------------------------------------------------------------------------
// The zero-allocation headless frame path (PERF-003, G-R1). The
// M1-ALLOC-01 per-tick watch arms around every tick, so this probe
// reads the LAST completed tick's window after the run (the
// one-shot setup allocations land before the first arm) — and in
// debug non-sanitizer builds the engine's own per-tick assertion
// additionally proves every tick of the run allocated nothing
// (an allocating tick would abort the run). The sanitizer trees
// prove the run leak-free.
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
TEST(EngineRun, HeadlessFramePathAllocatesNothing) {
  MemorySink* sink = installCaptureSink();
  // Gate the engine's Info lifecycle events and the loop's drop
  // warns OFF for the window: the healthy path must not log (and a
  // gated-off event costs no allocation — LOG-003).
  laige::log::Logger::instance().setSubsystemLevel("engine",
                                                    laige::log::Level::Off);
  laige::log::Logger::instance().setSubsystemLevel("loop",
                                                    laige::log::Level::Off);
  // Warm-up: construct the logger singleton and exercise the world
  // allocation pattern BEFORE the measured window (the first
  // logger- and world-touching calls of the process allocate their
  // one-time state).
  {
    laige::Engine warmup =
        makeEngine(laige::EngineConfig{60, 8, 256});
    warmup.shutdown();
  }
  laige::Engine engine =
      makeEngine(laige::EngineConfig{60, 64, 256});
  ASSERT_TRUE(engine.world()->registerSystem(EngTickCounter_Def).ok());
  laige::test::resetAllocCounter();
  const laige::Status status = engine.run_headless(3, 1);
  const std::uint64_t allocs = laige::test::allocCounter();
  // The run's setup path allocates exactly three times, all one-shot:
  // the GameLoop object, the PresentationSnapshot object, and the
  // presentation slot record table (24 B x capacity — the
  // presentation.h storage contract). Those land BEFORE the first
  // tick's G-R1 watch arm (M1-ALLOC-01), so the window read after
  // the run holds the LAST completed tick's allocations: zero — the
  // steady-state frame path (clock read, loop frame, snapshot
  // refresh, sleep) touches no heap (the PERF-003 hot-path property;
  // probe-verified for 1, 2, 3, and 10 ticks, M1-HEAD-01).
  std::printf("engine-zeroalloc ticks=%llu allocs=%llu\n",
              static_cast<unsigned long long>(engine.stats().ticks),
              static_cast<unsigned long long>(allocs));
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(engine.stats().ticks, 3u);
  EXPECT_EQ(allocs, 0u);
  EXPECT_EQ(sink->entries.size(), 0u);
  restoreLogger();
}
#endif
