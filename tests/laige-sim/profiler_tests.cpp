// laige-sim profiler suite (M1-PROF-01; PRD FR-11.1).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - the counter model: exact percentile values through a known
//     sample sequence, window rollover (the window bounds, the
//     since-construction counts unbounded), the render/network
//     adders, and disabled = a no-op that preserves recorded state
//   - the snapshot: the no-arg form (no world access) and the world
//     form (the entity counts, the sim alloc count, the system count
//     pulled cold from the world)
//   - the GameLoop wiring: per-completed-tick timing recorded on
//     success only (a failed tick is not recorded — the tick-count
//     contract), null and disabled profilers pay nothing but a branch
//   - the Engine wiring: the per-run profileStats() cache, the
//     opt-in report (written at run end as version-1 JSON on every
//     run path, parseable; double-start / empty-path / stopped-engine
//     rejections; a write failure is sticky and never fails the run;
//     a pre-run shutdown abandons the report with no file on disk)
//   - the record path allocates nothing (the test-only operator-new
//     counter, non-sanitizer trees)
//   - the enabled-cost check: profiler ON vs OFF over 10k-entity
//     ticks — the measured overhead is bounded at 1% (CORE-001,
//     DBG-004; the canonical-tree baseline is
//     docs/benchmarks/baselines/m1-profiler-cost.md)
//
// The CTest entry is `profiler` (this suite, all of it — the
// machine-greppable profiler-zeroalloc / profiler-cost lines land in
// the ctest output).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for _fsopen
#endif

#include "gtest/gtest.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/json.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim/entity.h"
#include "laige/sim/engine.h"
#include "laige/sim/game_loop.h"
#include "laige/sim/profiler.h"
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
              "profiler_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "profiler_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "profiler_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

using laige::ErrorCode;
using laige::GameLoop;
using laige::HistogramStats;
using laige::Profiler;
using laige::ProfilerStats;
using laige::Result;
using laige::Status;
using laige::SystemDef;
using laige::SystemSchedule;
using laige::World;

// ---------------------------------------------------------------------------
// Test systems (zero-I/O noops — the game_loop_tests pattern) and the
// synthetic clock (the Options::nowNs injection seam)
// ---------------------------------------------------------------------------

void fnNoopA(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

void fnNoopB(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

namespace {

SystemDef makeDef(const char* name, laige::SystemFn fn,
                  std::int32_t budgetMs) {
  return SystemDef{name, fn, laige::fpx16_16::fromInt32(budgetMs), nullptr};
}

std::int64_t gSynthClockNs = 0;

std::int64_t synthNowNs() { return gSynthClockNs; }

// One 60 Hz tick, in whole nanoseconds (the game_loop_tests constant).
inline constexpr std::int64_t kSynthTickNs = 16666667;

World makeWorld() {
  auto w = World::create(World::Options{8});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(8) failed: " << laige::errorName(w.error());
    abort();
  }
  World world = std::move(w).takeValue();
  if (!world.registerSystem(makeDef("GLNoopA", &fnNoopA, 1)).ok()) {
    ADD_FAILURE() << "registerSystem(GLNoopA) failed";
    abort();
  }
  return world;
}

SystemSchedule makeSchedule(World& world) {
  SystemSchedule sched;
  if (!world.scheduleSystems(sched).ok()) {
    ADD_FAILURE() << "scheduleSystems failed";
    abort();
  }
  return sched;
}

// The file helpers (report readers/cleaners — C stdio, the writeProfile
// implementation's own boundary).
//
// Portable file open (CPP-009 platform boundary, the
// replay_record_tests.cpp / src/laige-sim/replay.cpp precedent): MSVC's
// CRT deprecates plain `fopen` (C4996, fatal under the engine's /WX
// policy). As in replay.cpp, the MSVC path uses `_fsopen(path, mode,
// _SH_DENYNO)` — plain-`fopen` sharing semantics (the secure `fopen_s`
// opens with `_SH_SECURE` and would deny it).
#if defined(_MSC_VER)
std::FILE* openReportFile(const std::string& path, const char* mode) {
  return ::_fsopen(path.c_str(), mode, _SH_DENYNO);
}
#else
std::FILE* openReportFile(const std::string& path, const char* mode) {
  return std::fopen(path.c_str(), mode);
}
#endif

std::string readFile(const std::string& path) {
  std::FILE* f = openReportFile(path, "rb");
  if (f == nullptr) return {};
  std::string out;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

bool fileExists(const std::string& path) {
  std::FILE* f = openReportFile(path, "rb");
  if (f != nullptr) std::fclose(f);
  return f != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Log capture (the engine_tests MemorySink pattern — Warn and up only:
// the engine's Info lifecycle events are below the floor)
// ---------------------------------------------------------------------------

namespace {

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

// Installs a fresh capture sink with rate limiting OFF (the
// engine_tests pattern — re-initializes the facade after an Engine
// shutdown retires it).
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

// Creates an engine, failing loudly on a setup error (the engine_tests
// makeEngine pattern).
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
// The counter model (always-on, fixed storage, no allocation)
// ---------------------------------------------------------------------------

TEST(ProfilerCounters, FreshProfilerHasEmptyWindows) {
  Profiler p(Profiler::Options{});
  const ProfilerStats s = p.snapshot();
  EXPECT_EQ(s.ticks, 0u);
  EXPECT_EQ(s.frames, 0u);
  EXPECT_EQ(s.drawCalls, 0u);
  EXPECT_EQ(s.textureBinds, 0u);
  EXPECT_EQ(s.netBytes, 0u);
  EXPECT_EQ(s.tickTimeMs.n, 0u);
  EXPECT_EQ(s.frameTimeMs.n, 0u);
  EXPECT_TRUE(std::isnan(s.tickTimeMs.min));  // empty window -> NaN stats
  EXPECT_FALSE(s.worldAvailable);
  EXPECT_TRUE(p.enabled());
}

TEST(ProfilerCounters, RecordedTicksLandInTheWindow) {
  Profiler p(Profiler::Options{});
  for (std::uint32_t i = 1; i <= 100; ++i) {
    p.recordTick(static_cast<double>(i));
  }
  const HistogramStats t = p.tickTime();
  EXPECT_EQ(t.n, 100u);
  EXPECT_DOUBLE_EQ(t.min, 1.0);
  EXPECT_DOUBLE_EQ(t.max, 100.0);
  EXPECT_DOUBLE_EQ(t.mean, 50.5);
  EXPECT_DOUBLE_EQ(t.p50, 50.0);
  EXPECT_DOUBLE_EQ(t.p95, 95.0);
  EXPECT_DOUBLE_EQ(t.p99, 99.0);
  EXPECT_EQ(p.snapshot().ticks, 100u);
}

TEST(ProfilerCounters, WindowRolloverKeepsTheLatestSamples) {
  Profiler::Options o;
  o.tickWindowSamples = 8;
  o.frameWindowSamples = 8;
  Profiler p(o);
  for (std::uint32_t i = 1; i <= 20; ++i) {
    p.recordTick(static_cast<double>(i));
  }
  const HistogramStats t = p.tickTime();
  EXPECT_EQ(t.n, 8u);  // the window is bounded
  EXPECT_DOUBLE_EQ(t.min, 13.0);  // the oldest stored sample
  EXPECT_DOUBLE_EQ(t.max, 20.0);
  EXPECT_DOUBLE_EQ(t.mean, 16.5);
  EXPECT_DOUBLE_EQ(t.p50, 16.0);  // nearest-rank over [13..20]
  EXPECT_EQ(p.snapshot().ticks, 20u);  // the counter does not truncate
}

TEST(ProfilerCounters, FrameWindowIsIndependent) {
  Profiler p(Profiler::Options{});
  p.recordFrame(5.0);
  p.recordTick(7.0);
  const ProfilerStats s = p.snapshot();
  EXPECT_EQ(s.frames, 1u);
  EXPECT_EQ(s.ticks, 1u);
  EXPECT_DOUBLE_EQ(s.frameTimeMs.min, 5.0);
  EXPECT_DOUBLE_EQ(s.tickTimeMs.min, 7.0);
}

TEST(ProfilerCounters, ZeroCapacityWindowDropsEverySample) {
  Profiler::Options o;
  o.tickWindowSamples = 0;
  o.frameWindowSamples = 0;
  Profiler p(o);
  p.recordTick(1.0);
  p.recordFrame(2.0);
  const ProfilerStats s = p.snapshot();
  EXPECT_EQ(s.ticks, 1u);  // the counters still count
  EXPECT_EQ(s.frames, 1u);
  EXPECT_EQ(s.tickTimeMs.n, 0u);
  EXPECT_TRUE(std::isnan(s.tickTimeMs.p50));
}

TEST(ProfilerCounters, RenderAndNetworkAddersAccumulate) {
  Profiler p(Profiler::Options{});
  p.addDrawCalls(3);
  p.addDrawCalls(2);
  p.addTextureBinds(1);
  p.addNetBytes(10);
  const ProfilerStats s = p.snapshot();
  EXPECT_EQ(s.drawCalls, 5u);
  EXPECT_EQ(s.textureBinds, 1u);
  EXPECT_EQ(s.netBytes, 10u);
}

TEST(ProfilerCounters, DisabledIsANoOpAndPreservesRecordedState) {
  Profiler p(Profiler::Options{});
  p.recordTick(1.0);
  p.recordFrame(2.0);
  p.setEnabled(false);
  p.recordTick(3.0);
  p.recordFrame(4.0);
  p.addDrawCalls(9);
  const ProfilerStats s = p.snapshot();
  EXPECT_EQ(s.ticks, 1u);  // the disabled records are dropped
  EXPECT_EQ(s.frames, 1u);
  EXPECT_EQ(s.drawCalls, 0u);
  EXPECT_DOUBLE_EQ(s.tickTimeMs.min, 1.0);  // recorded state preserved
  p.setEnabled(true);
  p.recordTick(5.0);
  EXPECT_EQ(p.snapshot().ticks, 2u);
}

TEST(ProfilerCounters, MovedFromProfilerIsStopped) {
  Profiler a(Profiler::Options{});
  a.recordTick(1.0);
  Profiler b(std::move(a));
  a.recordTick(2.0);  // a stopped profiler records nothing
  const ProfilerStats stopped = a.snapshot();
  EXPECT_EQ(stopped.ticks, 0u);  // a stopped profiler reports empty
  EXPECT_EQ(stopped.tickTimeMs.n, 0u);
  const ProfilerStats moved = b.snapshot();
  EXPECT_EQ(moved.ticks, 1u);
  EXPECT_DOUBLE_EQ(moved.tickTimeMs.min, 1.0);
}

// ---------------------------------------------------------------------------
// The snapshot (the no-arg form, the world-pulled fields)
// ---------------------------------------------------------------------------

TEST(ProfilerSnapshot, SnapshotWithoutWorldLeavesWorldFieldsEmpty) {
  Profiler p(Profiler::Options{});
  p.recordTick(1.0);
  const ProfilerStats s = p.snapshot();
  EXPECT_EQ(s.ticks, 1u);
  EXPECT_FALSE(s.worldAvailable);
  EXPECT_EQ(s.entitiesAlive, 0u);
  EXPECT_EQ(s.entitiesTotal, 0u);
  EXPECT_EQ(s.entityCapacity, 0u);
  EXPECT_EQ(s.simAllocs, 0u);
  EXPECT_EQ(s.systems, 0u);
}

TEST(ProfilerSnapshot, SnapshotWithWorldPullsTheFields) {
  Profiler p(Profiler::Options{});
  auto w = World::create(World::Options{16});
  ASSERT_TRUE(w.ok());
  World world = std::move(w).takeValue();
  std::vector<laige::Entity> entities;
  for (int i = 0; i < 5; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());
    entities.push_back(std::move(e).takeValue());
  }
  ASSERT_TRUE(world.registerSystem(makeDef("ProfNoop", &fnNoopA, 1)).ok());
  const ProfilerStats s = p.snapshot(world);
  EXPECT_TRUE(s.worldAvailable);
  EXPECT_EQ(s.entitiesAlive, 5u);  // World::stats().inUse
  EXPECT_EQ(s.entitiesTotal, 5u);  // World::stats().totalCreated
  EXPECT_EQ(s.entityCapacity, 16u);  // the declared scene budget
  EXPECT_EQ(s.systems, 1u);  // World::systemCount()
  // The sim alloc count: the pool accounting's sum (M1-ECS-03).
  EXPECT_EQ(s.simAllocs, world.archetypeStats().totalReservations);
}

// ---------------------------------------------------------------------------
// The GameLoop wiring (per-completed-tick timing)
// ---------------------------------------------------------------------------

namespace {

GameLoop makeLoopWithProfiler(World& world, const SystemSchedule& sched,
                              Profiler* prof, std::uint32_t maxCatchUp) {
  GameLoop::Options opts;
  opts.tickRateHz = 60;
  opts.maxCatchUpTicks = maxCatchUp;
  opts.nowNs = &synthNowNs;
  opts.profiler = prof;
  auto r = GameLoop::create(world, sched, std::move(opts));
  if (!r.ok()) {
    ADD_FAILURE() << "GameLoop::create failed: " << laige::errorName(r.error());
    abort();
  }
  return std::move(r).takeValue();
}

}  // namespace

TEST(ProfilerGameLoop, CompletedTicksAreRecorded) {
  gSynthClockNs = 0;
  Profiler prof(Profiler::Options{});
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoopWithProfiler(w, sched, &prof, 5);

  ASSERT_TRUE(loop.frame().ok());  // the first frame: zero ticks
  for (int i = 0; i < 5; ++i) {
    gSynthClockNs += kSynthTickNs;
    ASSERT_TRUE(loop.frame().ok());
  }
  const ProfilerStats s = prof.snapshot();
  EXPECT_EQ(loop.currentTick(), 5u);
  EXPECT_EQ(s.ticks, 5u);  // one sample per completed tick
  EXPECT_EQ(s.tickTimeMs.n, 5u);
  EXPECT_GE(s.tickTimeMs.min, 0.0);
  // The frame feed is the engine's, not the loop's (profiler.h).
  EXPECT_EQ(s.frames, 0u);
  EXPECT_FALSE(s.worldAvailable);
}

TEST(ProfilerGameLoop, FailedTicksAreNotRecorded) {
  gSynthClockNs = 0;
  Profiler prof(Profiler::Options{});
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoopWithProfiler(w, sched, &prof, 5);

  ASSERT_TRUE(loop.frame().ok());
  gSynthClockNs += kSynthTickNs;
  ASSERT_TRUE(loop.frame().ok());  // tick 1 completes

  // A registration after scheduling makes the schedule stale (the
  // game_loop_tests pattern): the next frame fails in runSystems.
  ASSERT_TRUE(w.registerSystem(makeDef("ProfNoop2", &fnNoopB, 1)).ok());
  gSynthClockNs += kSynthTickNs;
  const Status bad = loop.frame();
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::InvalidArgument);
  // The failed tick is not counted and not recorded (the tick-count
  // contract — profiler.h, game_loop.h).
  EXPECT_EQ(loop.currentTick(), 1u);
  EXPECT_EQ(prof.snapshot().ticks, 1u);
}

TEST(ProfilerGameLoop, DisabledProfilerRecordsNothing) {
  gSynthClockNs = 0;
  Profiler prof(Profiler::Options{});
  prof.setEnabled(false);  // the DBG-002 profile switch
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoopWithProfiler(w, sched, &prof, 5);

  ASSERT_TRUE(loop.frame().ok());
  for (int i = 0; i < 3; ++i) {
    gSynthClockNs += kSynthTickNs;
    ASSERT_TRUE(loop.frame().ok());
  }
  EXPECT_EQ(loop.currentTick(), 3u);
  EXPECT_EQ(prof.snapshot().ticks, 0u);  // one branch per tick, nothing else
}

TEST(ProfilerGameLoop, NullProfilerLeavesTheLoopUnchanged) {
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoopWithProfiler(w, sched, nullptr, 5);

  ASSERT_TRUE(loop.frame().ok());
  for (int i = 0; i < 3; ++i) {
    gSynthClockNs += kSynthTickNs;
    ASSERT_TRUE(loop.frame().ok());
  }
  EXPECT_EQ(loop.currentTick(), 3u);
}

// ---------------------------------------------------------------------------
// The Engine wiring (the per-run cache, the opt-in report)
// ---------------------------------------------------------------------------

TEST(ProfilerEngine, RunProducesProfileStats) {
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.world()
                  ->registerSystem(makeDef("EngProfNoop", &fnNoopA, 1))
                  .ok());
  const Status st = engine.run_headless(5, 1);
  ASSERT_TRUE(st.ok());
  const ProfilerStats ps = engine.profileStats();
  EXPECT_EQ(ps.ticks, 5u);
  EXPECT_GE(ps.frames, 3u);  // first frame (0 ticks) + 5
  EXPECT_TRUE(ps.worldAvailable);
  EXPECT_EQ(ps.entitiesAlive, 0u);
  EXPECT_EQ(ps.entitiesTotal, 0u);
  EXPECT_EQ(ps.entityCapacity, 128u);
  EXPECT_EQ(ps.systems, 1u);
  EXPECT_GE(ps.simAllocs, 0u);
  // The run ended in the ordered shutdown: the profiler is released
  // (the world() nullptr precedent).
  EXPECT_EQ(engine.profiler(), nullptr);
}

TEST(ProfilerEngine, ReportIsWrittenAtRunEnd) {
  const std::string path = "profiler_test_report.json";
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.world()
                  ->registerSystem(makeDef("EngProfNoop", &fnNoopA, 1))
                  .ok());
  ASSERT_TRUE(engine.startProfileReport(path).ok());
  EXPECT_TRUE(engine.profileReportActive());
  const Status st = engine.run_headless(3, 1);
  ASSERT_TRUE(st.ok());
  EXPECT_TRUE(engine.profileReportStatus().ok());
  EXPECT_FALSE(engine.profileReportActive());

  // The report exists and is the version 1 JSON schema (profiler.h /
  // docs/api/profiler.md).
  std::string text = readFile(path);
  std::remove(path.c_str());
  ASSERT_FALSE(text.empty());
  const Result<laige::JsonValue> parsed = laige::parseJson(text);
  ASSERT_TRUE(parsed.ok());
  const laige::JsonValue& root = parsed.value();
  ASSERT_TRUE(root.isObject());
  const laige::JsonValue* version = root.findMember("version");
  ASSERT_NE(version, nullptr);
  EXPECT_EQ(version->asNumber(), 1.0);
  const laige::JsonValue* counters = root.findMember("counters");
  ASSERT_NE(counters, nullptr);
  EXPECT_EQ(counters->findMember("ticks")->asNumber(), 3.0);
  const laige::JsonValue* tickTime = root.findMember("tick_time_ms");
  ASSERT_NE(tickTime, nullptr);
  ASSERT_TRUE(tickTime->isObject());
  EXPECT_EQ(tickTime->findMember("n")->asNumber(), 3.0);
  const laige::JsonValue* worldObj = root.findMember("world");
  ASSERT_NE(worldObj, nullptr);
  EXPECT_EQ(worldObj->findMember("systems")->asNumber(), 1.0);
  const laige::JsonValue* systems = root.findMember("systems");
  ASSERT_NE(systems, nullptr);
  ASSERT_TRUE(systems->isArray());
  ASSERT_EQ(systems->asArray().size(), 1u);
  // The M1-SYS-03 per-system window: the system ran 3 ticks.
  const laige::JsonValue* window = systems->asArray().front().findMember(
      "window_ms");
  ASSERT_NE(window, nullptr);
  ASSERT_TRUE(window->isObject());
  EXPECT_EQ(window->findMember("n")->asNumber(), 3.0);
  EXPECT_EQ(systems->asArray().front().findMember("runs")->asNumber(), 3.0);
}

TEST(ProfilerEngine, FailedStartStillWritesTheReport) {
  // A zero frame budget is rejected before the loop exists — the run
  // writes a zero-tick report anyway (the finalization is on every
  // run path; CORE-008: no silent omission).
  const std::string path = "profiler_test_zero.json";
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.startProfileReport(path).ok());
  const Status st = engine.run_headless(2, 0);
  ASSERT_TRUE(st.isError());
  EXPECT_EQ(st.error(), ErrorCode::InvalidArgument);
  EXPECT_TRUE(engine.profileReportStatus().ok());
  std::string text = readFile(path);
  std::remove(path.c_str());
  ASSERT_FALSE(text.empty());
  const Result<laige::JsonValue> parsed = laige::parseJson(text);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value().findMember("counters")
                  ->findMember("ticks")
                  ->asNumber(),
            0.0);
}

TEST(ProfilerEngine, DoubleReportStartRejected) {
  MemorySink* sink = installCaptureSink();
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.startProfileReport("a_report.json").ok());
  const Status bad = engine.startProfileReport("b_report.json");
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "report_already_started"), 1u);
  engine.shutdown();
  restoreLogger();
}

TEST(ProfilerEngine, EmptyReportPathRejected) {
  MemorySink* sink = installCaptureSink();
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  const Status bad = engine.startProfileReport("");
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "report_path_invalid"), 1u);
  engine.shutdown();
  restoreLogger();
}

TEST(ProfilerEngine, ReportWriteFailureDoesNotFailTheRun) {
  MemorySink* sink = installCaptureSink();
  const std::string path = "/nonexistent-laige-dir/report.json";
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.startProfileReport(path).ok());
  const Status st = engine.run_headless(2, 1);
  // The run is not gated by diagnostics (CORE-002's priority order).
  ASSERT_TRUE(st.ok());
  ASSERT_TRUE(engine.profileReportStatus().isError());
  EXPECT_EQ(engine.profileReportStatus().error(), ErrorCode::IoError);
  EXPECT_EQ(countEvents(*sink, "report_write_failed"), 1u);
  EXPECT_FALSE(fileExists(path));  // no partial report on disk
  engine.shutdown();  // the run already shut down; idempotent
  restoreLogger();
}

TEST(ProfilerEngine, PreRunShutdownAbortsTheReport) {
  MemorySink* sink = installCaptureSink();
  const std::string path = "profiler_test_aborted.json";
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  ASSERT_TRUE(engine.startProfileReport(path).ok());
  engine.shutdown();  // no run — the report is abandoned
  EXPECT_EQ(countEvents(*sink, "report_aborted"), 1u);
  EXPECT_FALSE(fileExists(path));  // diagnostics only: nothing to clean up
  EXPECT_FALSE(engine.profileReportActive());  // the lifecycle ended
  restoreLogger();
}

TEST(ProfilerEngine, StoppedEngineRejectsTheReport) {
  laige::Engine engine = makeEngine(laige::EngineConfig{60, 128, 256});
  engine.shutdown();
  const Status bad = engine.startProfileReport("x_report.json");
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.error(), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// The report formats (the text greppable form, the write contract)
// ---------------------------------------------------------------------------

TEST(ProfilerReport, TextReportIsGreppable) {
  gSynthClockNs = 0;
  Profiler prof(Profiler::Options{});
  World w = makeWorld();  // the system GLNoopA
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoopWithProfiler(w, sched, &prof, 5);
  ASSERT_TRUE(loop.frame().ok());
  for (int i = 0; i < 3; ++i) {
    gSynthClockNs += kSynthTickNs;
    ASSERT_TRUE(loop.frame().ok());
  }
  const std::string path = "profiler_test_text.txt";
  const Result<std::uint64_t, ErrorCode> written =
      writeProfile(prof, w, path, laige::ProfileFormat::Text);
  ASSERT_TRUE(written.ok());
  std::string text = readFile(path);
  std::remove(path.c_str());
  EXPECT_EQ(written.value(), text.size());
  EXPECT_NE(text.find("laige-profile version=1"), std::string::npos);
  EXPECT_NE(text.find("laige-profile counters:"), std::string::npos);
  EXPECT_NE(text.find("laige-profile tick_ms: n=3"), std::string::npos);
  EXPECT_NE(text.find("laige-profile frame_ms: n=0"), std::string::npos);
  EXPECT_NE(text.find("laige-profile world:"), std::string::npos);
  EXPECT_NE(text.find("laige-profile system id=1 name=GLNoopA"),
            std::string::npos);
  EXPECT_NE(text.find("window: n=3"), std::string::npos);
}

TEST(ProfilerReport, WriteFailureLeavesNoPartialFile) {
  World w = makeWorld();
  Profiler prof(Profiler::Options{});
  const std::string path = "/nonexistent-laige-dir/text.txt";
  const Result<std::uint64_t, ErrorCode> written =
      writeProfile(prof, w, path, laige::ProfileFormat::Text);
  ASSERT_TRUE(written.isError());
  EXPECT_EQ(written.error(), ErrorCode::IoError);
  EXPECT_FALSE(fileExists(path));
}

// ---------------------------------------------------------------------------
// The record path allocates nothing (PERF-003; the M1-ALLOC-01
// assertion will supersede this probe once it exists — the
// engine_tests zero-allocation pattern, non-sanitizer trees)
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
TEST(ProfilerZeroAlloc, RecordPathAllocatesNothing) {
  // Warm-up: exercise the one-time state (the Histogram backing
  // buffers) BEFORE the measured window.
  {
    Profiler warm(Profiler::Options{});
    warm.recordTick(1.0);
    static_cast<void>(warm.snapshot());
  }
  Profiler::Options o;
  o.tickWindowSamples = 16;
  o.frameWindowSamples = 16;
  Profiler p(o);
  laige::test::resetAllocCounter();
  for (std::uint32_t i = 0; i < 1000; ++i) {
    p.recordTick(static_cast<double>(i));
    p.recordFrame(static_cast<double>(i));
  }
  p.addDrawCalls(1);
  p.addTextureBinds(1);
  p.addNetBytes(1);
  // The snapshot's stats() pass sorts into the pre-reserved scratch
  // buffer — no heap. (The format surface is a cold path and DOES
  // allocate — it is not part of the record path.)
  const ProfilerStats s = p.snapshot();
  const std::uint64_t allocs = laige::test::allocCounter();
  std::printf("profiler-zeroalloc ticks=%llu allocs=%llu\n",
              static_cast<unsigned long long>(s.ticks),
              static_cast<unsigned long long>(allocs));
  EXPECT_EQ(s.ticks, 1000u);
  EXPECT_EQ(s.frames, 1000u);
  EXPECT_EQ(allocs, 0u);
}
#endif

// ---------------------------------------------------------------------------
// The enabled-cost check (CORE-001, DBG-004): the profiler ON vs OFF
// over 10k-entity ticks must stay within 1% (the roadmap's
// disabled-cost gate; the canonical-tree baseline is
// docs/benchmarks/baselines/m1-profiler-cost.md).
//
// Non-sanitizer trees only (the LAIGE_ALLOC_COUNTER gate — the
// zero-allocation probe's precedent): sanitizer instrumentation is
// not representative of shipping performance — it inflates the
// profiler's fixed per-tick cost (the extra clock reads + the ring
// write) disproportionately, and the measured overhead there (1.46%
// on the ASan tree, 2026-09-21) measures the INSTRUMENTATION, not
// the profiler. The gate is enforced on every non-instrumented P0 CI
// job (the linux-gcc and linux-clang P0 jobs, and the
// windows-msvc and both macOS jobs' ctest alike).
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
// The cost workload's components (global scope on purpose —
// LAIGE_COMPONENT specializes the primary template in its enclosing
// namespace, the ecs_stress_tests pattern).
struct ProfCostPos {
  std::int32_t x{};
  std::int32_t y{};
};
LAIGE_COMPONENT(ProfCostPos)

struct ProfCostVel {
  std::int64_t v{};
};
LAIGE_COMPONENT(ProfCostVel)

void fnCostMove(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(ctx);
  // Representative work (the PRD §8.1 reference scene: 10k entities,
  // one component write per entity per tick — a plain SoA scan with a
  // dependent write per row).
  static_cast<void>(world.each<ProfCostPos, ProfCostVel>(
      [](const laige::Entity&, ProfCostPos& pos, const ProfCostVel& vel) {
        pos.x += static_cast<std::int32_t>(vel.v);
        pos.y += static_cast<std::int32_t>(vel.v >> 32);
      },
      laige::Write{}, laige::Read{}));
}

namespace {

inline constexpr std::uint32_t kCostEntities = 10000;  // PRD §8.1
inline constexpr std::uint32_t kCostTicks = 2000;  // samples PER ARM
inline constexpr std::uint32_t kCostWindow = 2000;  // the profiler's tick window

// The two arms' frame-time p50s of one INTERLEAVED run (ms): the
// profiler is enabled on every other tick (tick i+1 is enabled iff i
// is odd), so each ON sample is adjacent to an OFF sample ~1 ms
// apart — the two subsequences sample the SAME machine-state
// trajectory (a CI runner's frequency ramp or thermal oscillation
// under sustained load shifts the whole trajectory, not one arm vs
// the other — the phase cancels in the A/B ratio at the
// adjacent-sample level; a blocked or per-run-interleaved order
// measured two phases of that trajectory and showed the drift
// itself as overhead — the macOS arm64 CI failure of M1-PROF-01,
// 2.14% on a 0.255 ms tick). A transient CI stall slows at most
// one tick = one sample of 2000 in one window — no p50 effect, so
// no best-of-N repetition is needed.
struct CostSample {
  double onP50{};
  double offP50{};
};

CostSample runCostInterleaved() {
  gSynthClockNs = 0;
  World::Options wo;
  wo.capacity = kCostEntities;
  wo.churnPerFrameBudget = 0;  // the bulk setup attach predates any frame
  auto w = World::create(wo);
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(10000) failed: " << laige::errorName(w.error());
    abort();
  }
  World world = std::move(w).takeValue();
  if (!world.registerComponent<ProfCostPos>().ok()) {
    ADD_FAILURE() << "registerComponent(ProfCostPos) failed";
    abort();
  }
  if (!world.registerComponent<ProfCostVel>().ok()) {
    ADD_FAILURE() << "registerComponent(ProfCostVel) failed";
    abort();
  }
  std::vector<laige::Entity> entities;
  entities.reserve(kCostEntities);
  for (std::uint32_t i = 0; i < kCostEntities; ++i) {
    auto e = world.create();
    if (!e.ok()) {
      ADD_FAILURE() << "world.create() failed at entity " << i;
      abort();
    }
    entities.push_back(std::move(e).takeValue());
    if (!world.addComponent<ProfCostPos>(
            entities.back(),
            ProfCostPos{static_cast<std::int32_t>(i),
                        static_cast<std::int32_t>(i * 2)})
             .ok()) {
      ADD_FAILURE() << "addComponent(ProfCostPos) failed at entity " << i;
      abort();
    }
    if (!world.addComponent<ProfCostVel>(
            entities.back(), ProfCostVel{static_cast<std::int64_t>(i)})
             .ok()) {
      ADD_FAILURE() << "addComponent(ProfCostVel) failed at entity " << i;
      abort();
    }
  }
  const SystemDef def = makeDef("ProfMove", &fnCostMove, 16);
  if (!world.registerSystem(def).ok()) {
    ADD_FAILURE() << "registerSystem(ProfMove) failed";
    abort();
  }
  SystemSchedule sched = makeSchedule(world);

  Profiler::Options po;
  po.tickWindowSamples = kCostWindow;  // == kCostTicks: every ON sample stored
  po.enabled = false;  // start disabled; toggled per tick below
  Profiler prof(po);

  GameLoop loop = makeLoopWithProfiler(world, sched, &prof, 1);
  // The test-side frame windows: every frame is recorded in exactly
  // one of the two (by its arm) — identical wrapping in both arms,
  // it cancels in the A/B ratio. The profiler's own tick window
  // stores the ON arm's ticks only (the OFF ticks are not
  // measured — the disabled branch is one branch).
  laige::Histogram onWindow(laige::Histogram::Options{kCostTicks});
  laige::Histogram offWindow(laige::Histogram::Options{kCostTicks});
  if (!loop.frame().ok()) {  // the start reference (zero ticks)
    ADD_FAILURE() << "the start-reference frame failed";
    abort();
  }
  for (std::uint32_t i = 0; i < 2 * kCostTicks; ++i) {
    const bool enabled = (i % 2 == 1);  // tick i+1: odd => ON, even => OFF
    prof.setEnabled(enabled);
    gSynthClockNs += kSynthTickNs;
    const laige::TimeIt timer;
    if (!loop.frame().ok()) {
      ADD_FAILURE() << "frame " << i << " failed";
      abort();
    }
    if (enabled) {
      onWindow.record(timer.elapsedMs());
    } else {
      offWindow.record(timer.elapsedMs());
    }
  }
  EXPECT_EQ(loop.currentTick(), static_cast<std::uint64_t>(2 * kCostTicks));
  const ProfilerStats s = prof.snapshot();
  EXPECT_EQ(s.ticks, static_cast<std::uint64_t>(kCostTicks));  // ON ticks only
  EXPECT_EQ(s.tickTimeMs.n, static_cast<std::uint64_t>(kCostWindow));
  return CostSample{onWindow.stats().p50, offWindow.stats().p50};
}

}  // namespace

TEST(ProfilerCost, EnabledCostBoundedToOnePercent) {
  // Warm-up run (cache/page-fault effects fall out of the measured
  // windows — the benchmark's warm-up discipline, AGENTS §12). It
  // also warms both arms' one-time state (the ON arm's ring).
  const CostSample warmup = runCostInterleaved();
  static_cast<void>(warmup);

  // One measured interleaved run — the per-tick A/B structure is
  // stall-proof and drift-free by construction (runCostInterleaved),
  // so no repetition is needed.
  const CostSample m = runCostInterleaved();
  std::printf("profiler-cost on_p50=%.6g off_p50=%.6g overhead_pct=%.6g\n",
              m.onP50, m.offP50, (m.onP50 - m.offP50) / m.offP50 * 100.0);
  ASSERT_GT(m.offP50, 0.0);
  const double overhead = (m.onP50 - m.offP50) / m.offP50;
  // The always-on enabled cost is bounded at 1% of a 10k-entity tick
  // (CORE-001, DBG-004; roadmap/M1-heartbeat.md M1-PROF-01).
  EXPECT_LE(overhead, 0.01);
}
#endif
