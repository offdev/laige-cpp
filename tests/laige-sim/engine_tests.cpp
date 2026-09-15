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
//   - the provisional config surface: defaults, the rejection table
//     (wrong type, non-integer, out of range), first-failure-wins,
//     unknown-key forward-compat warn (M1-CFG-01's rule)
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
#include "laige/json.h"
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

// Parses a JSON document from a literal (test cold path).
laige::JsonValue parseDoc(const char* text) {
  const laige::Result<laige::JsonValue> r = laige::parseJson(text);
  if (r.isError()) {
    ADD_FAILURE() << "test fixture JSON failed to parse: " << text;
    abort();
  }
  return r.value();
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
// parseEngineConfig (the provisional M1-HEAD-01 config surface)
// ---------------------------------------------------------------------------

TEST(EngineConfigParse, EmptyDocumentIsAllDefaults) {
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc("{}")).value();
  EXPECT_EQ(config.tickRateHz, laige::kDefaultTickRateHz);
  EXPECT_EQ(config.entityCapacity, 0u);
  EXPECT_EQ(config.churnPerFrameBudget, laige::kDefaultChurnPerFrameBudget);
}

TEST(EngineConfigParse, FullValidDocument) {
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc(
          R"({"tick_rate_hz":120,"entity_budget":65536,"churn_per_frame_budget":0})"))
          .value();
  EXPECT_EQ(config.tickRateHz, 120u);
  EXPECT_EQ(config.entityCapacity, 65536u);
  EXPECT_EQ(config.churnPerFrameBudget, 0u);
}

TEST(EngineConfigParse, TickRateRejects) {
  for (const char* bad : {"19", "121", "60.5", R"("60")", "true"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(std::string("{\"tick_rate_hz\":")
                                                .append(bad)
                                                .append("}")
                                                .c_str()));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  // The inclusive boundaries are accepted.
  for (const char* good : {"20", "120"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(std::string("{\"tick_rate_hz\":")
                                                .append(good)
                                                .append("}")
                                                .c_str()));
    ASSERT_TRUE(result.ok());
  }
}

TEST(EngineConfigParse, EntityBudgetRejects) {
  for (const char* bad : {"-1", "65537", "65536.5", R"("100")"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(std::string("{\"entity_budget\":")
                                                .append(bad)
                                                .append("}")
                                                .c_str()));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc(R"({"entity_budget":65536})"))
          .value();
  EXPECT_EQ(config.entityCapacity, 65536u);
}

TEST(EngineConfigParse, ChurnBudgetRejects) {
  for (const char* bad : {"-1", R"("10")", "10.5"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(std::string(
                                      "{\"churn_per_frame_budget\":")
                                      .append(bad)
                                      .append("}")
                                      .c_str()));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  const laige::EngineConfig config =
      laige::parseEngineConfig(
          parseDoc(R"({"churn_per_frame_budget":4294967295})"))
          .value();
  EXPECT_EQ(config.churnPerFrameBudget, 4294967295u);
}

TEST(EngineConfigParse, NotAnObjectRejected) {
  for (const char* doc : {"[]", "42", R"("config")", "null"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
}

TEST(EngineConfigParse, UnknownKeyWarnsAndIsIgnored) {
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::parseEngineConfig(
          parseDoc(R"({"tick_rate_hz":60,"camera":{"zoom":1}})"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().tickRateHz, 60u);
  EXPECT_EQ(countEvents(*sink, "unknown_key"), 1u);
  restoreLogger();
}

TEST(EngineConfigParse, FirstFailureWins) {
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::parseEngineConfig(
          parseDoc(R"({"tick_rate_hz":999,"entity_budget":-5})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "tick_rate_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_invalid"), 0u);
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
  const laige::Status second = engine.run_headless(1, 1);
  ASSERT_TRUE(second.isError());
  EXPECT_EQ(second.error(), laige::ErrorCode::InvalidArgument);
  // The stopped-state failure is a pure failure: no log (the
  // GameLoop's moved-out precedent); the Info lifecycle events of
  // the first run are below the capture sink's Warn floor anyway.
  EXPECT_EQ(sink->entries.size(), 0u);
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
// The zero-allocation headless frame path (PERF-003; the M1-ALLOC-01
// assertion will supersede this probe once it exists — ASan + pool
// accounting is the milestone's interim check)
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
#include <chrono>
#include <thread>

// TEMPORARY CI DIAGNOSTIC (delete before merge): bisects the
// Windows-only fourth heap allocation measured by
// HeadlessFramePathAllocatesNothing (allocs=4 on the windows-msvc job,
// 3 on every other P0 platform). It replicates
// Engine::run_headless(3, 1) stage by stage with allocation
// checkpoints, so the job log pinpoints the stage where the count
// moves on Windows.
namespace {

laige::PresentationSnapshot<laige::sim::Fpx16_16>* gDiagSnapshot = nullptr;

// The diagnostic-local mirror of Engine::onTickHookDispatch.
void diagOnTickHook(void* context, laige::World&, std::uint64_t tick) noexcept {
  (void)context;
  if (gDiagSnapshot != nullptr) gDiagSnapshot->onTick(tick);
}

// The shutdown() probe (the temporary Logger::setAllocProbe hook):
// prints the running allocation counter at every statement boundary
// inside Logger::shutdown(), localizing the Windows-only allocation.
int gDiagShutStep = 0;
void diagShutProbe() {
  std::printf("diag-shut step %d: allocs=%llu\n", ++gDiagShutStep,
              static_cast<unsigned long long>(laige::test::allocCounter()));
}

}  // namespace

TEST(EngineRun, DiagAllocBisectTemp) {
  MemorySink* sink = installCaptureSink();
  laige::log::Logger::instance().setSubsystemLevel("engine",
                                                    laige::log::Level::Off);
  laige::log::Logger::instance().setSubsystemLevel("loop",
                                                    laige::log::Level::Off);
  {
    laige::Engine warmup = makeEngine(laige::EngineConfig{60, 8, 256});
    warmup.shutdown();
  }
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 64, 256});
  ASSERT_TRUE(engine.world()->registerSystem(EngTickCounter_Def).ok());
  laige::test::resetAllocCounter();

  // Checkpoint printer (literals only: no allocation in the probe).
  auto stage = [](int n) {
    std::printf("diag-bisect stage %d: allocs=%llu\n", n,
                static_cast<unsigned long long>(
                    laige::test::allocCounter()));
  };
  auto nowNs = []() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  using Snapshot = laige::PresentationSnapshot<laige::sim::Fpx16_16>;
  stage(0);  // window start (the run_headless entry)
  {
    laige::SystemSchedule sched;
    ASSERT_TRUE(engine.world()->scheduleSystems(sched).ok());
    stage(1);  // after scheduleSystems

    laige::Result<laige::GameLoop, laige::ErrorCode> loopResult =
        laige::GameLoop::create(
            *engine.world(), sched,
            laige::GameLoop::Options{60, 1, nullptr, &diagOnTickHook,
                                     nullptr});
    ASSERT_TRUE(loopResult.ok());
    auto loop = std::make_unique<laige::GameLoop>(
        std::move(loopResult).takeValue());
    stage(2);  // after the GameLoop object allocation

    ASSERT_TRUE(loop->frame().ok());  // first frame: the start reference
    stage(3);  // after the first (zero-tick) frame

    laige::Result<Snapshot, laige::ErrorCode> snapResult =
        Snapshot::create(*engine.world(), loop->startReferenceNs(),
                         Snapshot::Options{60});
    ASSERT_TRUE(snapResult.ok());
    auto snap = std::make_unique<Snapshot>(
        std::move(snapResult).takeValue());
    gDiagSnapshot = snap.get();
    stage(4);  // after the snapshot object + slot-table allocation

    // The runFrames equivalent for maxTicks=3, frame budget 1:
    // three tick frames, a bounded sleep between frames 1-2 and 2-3,
    // none after the final tick.
    for (int f = 1; f <= 3; ++f) {
      const std::int64_t now = nowNs();
      ASSERT_TRUE(loop->frame().ok());
      snap->onRenderFrame(now);
      if (loop->currentTick() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(17));
      }
      stage(10 + f);  // after frame f
    }

    (void)loop->stats();
    stage(20);  // after the stats read

    // The shutdown() mirror (ordered: loop -> world clear -> snapshot).
    loop.reset();
    stage(21);  // after the loop release
    static_cast<void>(engine.world()->clear());
    stage(22);  // after the world clear
    snap.reset();
    gDiagSnapshot = nullptr;
    stage(23);  // after the snapshot release
  }
  gDiagShutStep = 0;
  laige::log::Logger::setAllocProbe(&diagShutProbe);
  laige::log::Logger::instance().shutdown();
  stage(24);  // after the first logger shutdown
  // The second (idempotent) call: localizes whether the Windows-only
  // allocation is per-call or one-shot (TEMPORARY round 3).
  laige::log::Logger::instance().shutdown();
  stage(25);  // after the second (idempotent) logger shutdown
  laige::log::Logger::setAllocProbe(nullptr);
  EXPECT_EQ(sink->entries.size(), 0u);
  restoreLogger();
  SUCCEED();
}

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
  // presentation.h storage contract). The steady-state frame path
  // (clock read, loop frame, snapshot refresh, sleep) touches no
  // heap: the count below is identical for 1, 2, 3, and 10 ticks
  // (probe-verified, M1-HEAD-01), i.e. zero per frame/per tick —
  // the PERF-003 hot-path property.
  std::printf("engine-zeroalloc ticks=%llu allocs=%llu\n",
              static_cast<unsigned long long>(engine.stats().ticks),
              static_cast<unsigned long long>(allocs));
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(engine.stats().ticks, 3u);
  EXPECT_EQ(allocs, 3u);
  EXPECT_EQ(sink->entries.size(), 0u);
  restoreLogger();
}
#endif
