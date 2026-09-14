// laige-sim fixed-timestep game loop core suite (M1-LOOP-01).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - fixed 60 Hz over a synthetic 10 s clock -> EXACT tick count
//     (600 ticks at exactly 10.000000 s — the integer due
//     computation, no floating-point accumulator drift)
//   - the overload path drops EXACTLY the documented amount
//     (want - maxCatchUpTicks per frame) and logs once per episode
//     (the facade's rate limiting: one tick_dropped per window,
//     the suppressed repeats summarized at shutdown)
//   - config validation (tick rate 20–120 Hz, max catch-up >= 1)
//     with the structured warns (loop/tick_rate_invalid,
//     loop/catchup_invalid)
//   - the healthy cadence runs one tick per frame, zero drops, the
//     success path silent (LOG-003)
//   - the first frame establishes the clock reference (zero ticks)
//   - a stale schedule freezes the tick count and surfaces the
//     runSystems Status (CORE-008: never silent)
//   - a backward clock jump asserts in debug / clamps in release
//     (the monotonic clock-source contract)
//   - the default headless clock (steady_clock) drives real frames
//   - move transfers the tick state; the moved-from loop is stopped
//   - no heap allocation on the healthy frame path (test-only
//     operator-new counter, non-sanitizer trees; the sanitizer
//     trees prove it leak-free)
//
// Runs as CTest `game_loop` (the step's Verify command:
// `ctest -R game_loop`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the suites below.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/sim/entity.h"
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
              "game_loop_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "game_loop_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "game_loop_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define GAME_LOOP_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define GAME_LOOP_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

static_assert(GAME_LOOP_TESTS_ACTIVE_CPLUSPLUS >= 202002L,
              "game_loop_tests must be built with C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");

namespace {

using laige::ErrorCode;
using laige::GameLoop;
using laige::GameLoopStats;
using laige::Status;
using laige::SystemDef;
using laige::SystemSchedule;
using laige::World;

// ---------------------------------------------------------------------------
// The synthetic clock (the Options::nowNs injection seam — the
// LoggerOptions::ClockFn precedent). Monotonic by construction: the
// tests only advance it (the backward-jump tests drive the documented
// failure path explicitly).
// ---------------------------------------------------------------------------

std::int64_t gSynthClockNs = 0;

std::int64_t synthNowNs() {
  return gSynthClockNs;
}

// One 60 Hz tick, in whole nanoseconds (16666667 ns — the exact-tick
// tests use steps of this size so every clock reading is an integer
// number of nanoseconds; 60 * 16666667 = 1000000020 ns > 1 s by
// exactly 20 ns, which is why the ten-second test mixes 16666667-ns
// and 16666666-ns steps to land on exactly 10.000000 s).
inline constexpr std::int64_t kSynthTickNs = 16666667;

// ---------------------------------------------------------------------------
// The test systems (zero-I/O noops — a system that touches no
// components, the M1-SYS-01 zero-Io-pack form)
// ---------------------------------------------------------------------------

void fnNoopA(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

void fnNoopB(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

SystemDef makeDef(const char* name, laige::SystemFn fn) {
  return SystemDef{name, fn, laige::fpx16_16::fromInt32(1), nullptr};
}

// ---------------------------------------------------------------------------
// World + schedule + loop builders
// ---------------------------------------------------------------------------

World makeWorld() {
  auto w = World::create(World::Options{8});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(8) failed: " << laige::errorName(w.error());
    abort();
  }
  World world = std::move(w).takeValue();
  if (!world.registerSystem(makeDef("GLNoopA", &fnNoopA)).ok()) {
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

GameLoop makeLoop(World& world, const SystemSchedule& sched,
                  std::uint32_t tickRateHz, std::uint32_t maxCatchUp) {
  GameLoop::Options opts;
  opts.tickRateHz = tickRateHz;
  opts.maxCatchUpTicks = maxCatchUp;
  opts.nowNs = &synthNowNs;
  auto r = GameLoop::create(world, sched, std::move(opts));
  if (!r.ok()) {
    ADD_FAILURE() << "GameLoop::create failed: " << laige::errorName(r.error());
    abort();
  }
  return std::move(r).takeValue();
}

// One frame on the synthetic clock, advancing it by `stepNs` and
// asserting the frame status.
void synthFrame(GameLoop& loop, std::int64_t stepNs) {
  gSynthClockNs += stepNs;
  ASSERT_TRUE(loop.frame().ok());
}

// One frame on the synthetic clock that must FAIL with `expected`
// (the stale-schedule test — the failure must be surfaced, never
// silent, CORE-008).
void synthFrameExpectFailure(GameLoop& loop, std::int64_t stepNs,
                             ErrorCode expected) {
  gSynthClockNs += stepNs;
  const Status s = loop.frame();
  ASSERT_FALSE(s.ok());
  EXPECT_EQ(s.error(), expected);
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

}  // namespace

// ---------------------------------------------------------------------------
// Configuration validation (FR-1.1: 20–120 Hz; catch-up >= 1)
// ---------------------------------------------------------------------------

TEST(GameLoop, CreateValidatesTheConfig) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);

  GameLoop::Options base;
  base.nowNs = &synthNowNs;

  // Below the range (19 Hz): rejected, one tick_rate_invalid warn.
  {
    GameLoop::Options o = base;
    o.tickRateHz = 19;
    auto r = GameLoop::create(w, sched, o);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  // Above the range (121 Hz): rejected (the second warn for the same
  // key is rate-limited within the 60 s window — the summary at
  // shutdown carries it, checked below).
  {
    GameLoop::Options o = base;
    o.tickRateHz = 121;
    auto r = GameLoop::create(w, sched, o);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  // The range endpoints are legal.
  {
    GameLoop::Options o = base;
    o.tickRateHz = 20;
    auto r = GameLoop::create(w, sched, o);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().tickRateHz(), 20u);
  }
  {
    GameLoop::Options o = base;
    o.tickRateHz = 120;
    auto r = GameLoop::create(w, sched, o);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().tickRateHz(), 120u);
  }
  // Zero catch-up: rejected, one catchup_invalid warn.
  {
    GameLoop::Options o = base;
    o.maxCatchUpTicks = 0;
    auto r = GameLoop::create(w, sched, o);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  // Catch-up of 1 is the minimum legal value.
  {
    GameLoop::Options o = base;
    o.maxCatchUpTicks = 1;
    auto r = GameLoop::create(w, sched, o);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().maxCatchUpTicks(), 1u);
  }
  // The defaults (an empty Options): 60 Hz, 5 catch-up ticks.
  {
    GameLoop::Options o = base;
    auto r = GameLoop::create(w, sched, o);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().tickRateHz(), laige::kDefaultTickRateHz);
    EXPECT_EQ(r.value().maxCatchUpTicks(), laige::kDefaultMaxCatchUpTicks);
  }

  // The warns: exactly one tick_rate_invalid (the 19 Hz case; the
  // 121 Hz repeat is rate-limited — LOG-004) and one catchup_invalid,
  // subsystem "loop", with the rejected value as a structured field.
  EXPECT_EQ(countEvents(*mem, "tick_rate_invalid"), 1u);
  EXPECT_EQ(countEvents(*mem, "catchup_invalid"), 1u);
  ASSERT_EQ(mem->entries.size(), 2u);
  const auto& rateEntry = mem->entries[0];
  EXPECT_EQ(rateEntry.subsystem, "loop");
  EXPECT_EQ(rateEntry.severity, laige::log::Severity::Warn);
  EXPECT_STREQ(fieldValue(rateEntry, "tick_rate_hz"), "19");
  const auto& catchEntry = mem->entries[1];
  EXPECT_EQ(catchEntry.subsystem, "loop");
  EXPECT_EQ(catchEntry.severity, laige::log::Severity::Warn);
  EXPECT_STREQ(fieldValue(catchEntry, "max_catch_up"), "0");

  // Shutdown: the suppressed 121 Hz repeat is summarized.
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(mem->entries.size(), 3u);
  EXPECT_EQ(mem->entries[2].event, "rate_limited");
  EXPECT_STREQ(fieldValue(mem->entries[2], "event"), "tick_rate_invalid");
  EXPECT_STREQ(fieldValue(mem->entries[2], "suppressed"), "1");
  sink = nullptr;
}

// ---------------------------------------------------------------------------
// The first frame: start reference, zero ticks
// ---------------------------------------------------------------------------

TEST(GameLoop, FirstFrameEstablishesTheClockAndRunsNothing) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, 5);

  ASSERT_TRUE(loop.frame().ok());
  EXPECT_EQ(loop.currentTick(), 0u);
  const GameLoopStats st = loop.stats();
  EXPECT_EQ(st.frames, 1u);
  EXPECT_EQ(st.ticks, 0u);
  EXPECT_EQ(st.droppedTicks, 0u);
  EXPECT_EQ(st.droppedFrames, 0u);
  // The success path is silent (LOG-003): no event at all.
  EXPECT_EQ(mem->entries.size(), 0u);

  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// Exact tick count over a synthetic 10 s clock (the step's Verify:
// fixed 60 Hz -> exact tick count)
// ---------------------------------------------------------------------------

TEST(GameLoop, ExactTicksOverTenSeconds) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, 5);

  // Frame 0 at t = 0 establishes the start reference.
  ASSERT_TRUE(loop.frame().ok());
  EXPECT_EQ(loop.currentTick(), 0u);

  // 600 frames advancing exactly 10.000000 s (1e10 ns) in whole
  // nanoseconds: 400 steps of 16666667 ns plus 200 of 16666666 ns
  // (= 400*16666667 + 200*16666666 = 10^10). The intermediate
  // readings are NOT whole multiples of the (non-integer-ns) tick
  // period — a floating accumulator would drift; the integer due
  // computation must not.
  for (std::uint32_t f = 1; f <= 400; ++f) {
    synthFrame(loop, 16666667);
  }
  // t = 400 * 16666667 = 6666666800 ns = 6.6666668 s:
  // floor(6666666800 * 60 / 10^9) = floor(400.000008) = 400 ticks.
  EXPECT_EQ(loop.currentTick(), 400u);
  for (std::uint32_t f = 401; f <= 600; ++f) {
    synthFrame(loop, 16666666);
  }
  // t = 10^10 ns exactly: due = (10^10 / 10^9) * 60 = 600 ticks —
  // EXACTLY 600 (a float accumulator over ms would floor to 599).
  EXPECT_EQ(loop.currentTick(), 600u);

  const GameLoopStats st = loop.stats();
  EXPECT_EQ(st.frames, 601u);  // the start frame + 600 advancing frames
  EXPECT_EQ(st.ticks, 600u);
  EXPECT_EQ(st.droppedTicks, 0u);
  EXPECT_EQ(st.droppedFrames, 0u);
  // The success path stayed silent over all 601 frames.
  EXPECT_EQ(mem->entries.size(), 0u);

  std::printf("game-loop exact frames=%llu ticks=%llu dropped=%llu\n",
              static_cast<unsigned long long>(st.frames),
              static_cast<unsigned long long>(st.ticks),
              static_cast<unsigned long long>(st.droppedTicks));
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The overload path: drop exactly want - maxCatchUp per frame, log
// once per episode (rate-limited)
// ---------------------------------------------------------------------------

TEST(GameLoop, OverloadDropsExactlyTheDocumentedAmountAndLogsOnce) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, 2);  // max catch-up: 2 ticks

  // Frame 0 at t = 0.
  ASSERT_TRUE(loop.frame().ok());
  // Three overloaded frames, each demanding 10 ticks
  // (10 * 16666667 ns): want = 10, 18, 26; toRun = 2 every frame;
  // dropped = 8, 16, 24 (EXACTLY want - maxCatchUp each time).
  for (int f = 0; f < 3; ++f) {
    synthFrame(loop, 10 * kSynthTickNs);
  }
  EXPECT_EQ(loop.currentTick(), 6u);  // 2 ticks per frame, 3 frames

  const GameLoopStats st = loop.stats();
  EXPECT_EQ(st.frames, 4u);
  EXPECT_EQ(st.ticks, 6u);
  EXPECT_EQ(st.droppedTicks, 48u);  // 8 + 16 + 24
  EXPECT_EQ(st.droppedFrames, 3u);

  // The overload logged ONCE (LOG-004: one event per rate window per
  // (subsystem, event, severity); the two repeats are suppressed and
  // summarized at shutdown — checked below).
  EXPECT_EQ(countEvents(*mem, "tick_dropped"), 1u);
  ASSERT_EQ(mem->entries.size(), 1u);
  const auto& e = mem->entries[0];
  EXPECT_EQ(e.subsystem, "loop");
  EXPECT_EQ(e.severity, laige::log::Severity::Warn);
  EXPECT_STREQ(fieldValue(e, "dropped"), "8");  // the first frame's amount
  EXPECT_STREQ(fieldValue(e, "total_dropped"), "8");
  EXPECT_STREQ(fieldValue(e, "max_catch_up"), "2");
  EXPECT_STREQ(fieldValue(e, "tick_rate_hz"), "60");

  // The NFR-13.3 5-field grammar (build-stable message text; the
  // dynamic values are structured fields — the system_timing
  // precedent): split on " | ", exactly 5 fields, none empty.
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
  EXPECT_EQ(fields[0], "tick_dropped");
  EXPECT_EQ(fields[4], "docs/api/game_loop.md");

  // Shutdown: the two suppressed repeats are summarized.
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(mem->entries.size(), 2u);
  EXPECT_EQ(mem->entries[1].event, "rate_limited");
  EXPECT_STREQ(fieldValue(mem->entries[1], "event"), "tick_dropped");
  EXPECT_STREQ(fieldValue(mem->entries[1], "suppressed"), "2");

  std::printf("game-loop drops frames=%llu ticks=%llu dropped=%llu\n",
              static_cast<unsigned long long>(st.droppedFrames),
              static_cast<unsigned long long>(st.ticks),
              static_cast<unsigned long long>(st.droppedTicks));
  sink = nullptr;
}

// ---------------------------------------------------------------------------
// The healthy cadence: one tick per frame, zero drops, silent
// ---------------------------------------------------------------------------

TEST(GameLoop, HealthyCadenceRunsOneTickPerFrame) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, laige::kDefaultMaxCatchUpTicks);

  ASSERT_TRUE(loop.frame().ok());
  for (int f = 0; f < 120; ++f) {
    synthFrame(loop, kSynthTickNs);  // one tick's time per frame
  }
  // One tick due per frame: exactly 120 ticks, nothing dropped.
  EXPECT_EQ(loop.currentTick(), 120u);
  const GameLoopStats st = loop.stats();
  EXPECT_EQ(st.frames, 121u);
  EXPECT_EQ(st.ticks, 120u);
  EXPECT_EQ(st.droppedTicks, 0u);
  // The loop drives runSystems once per tick: the system's measured
  // run count tracks the ticks exactly (the M1-SYS-03 feed).
  auto ts = w.systemTimingStats(laige::SystemId{1});
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts.value().runs, 120u);
  // The success path is silent: no budget events either (the noop
  // system runs in microseconds against its 1 ms budget).
  EXPECT_EQ(mem->entries.size(), 0u);

  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// A stale schedule: the tick count freezes, the Status surfaces
// ---------------------------------------------------------------------------

TEST(GameLoop, StaleScheduleFreezesTheLoop) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();  // GLNoopA registered
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, 5);

  ASSERT_TRUE(loop.frame().ok());
  synthFrame(loop, kSynthTickNs);  // tick 1
  EXPECT_EQ(loop.currentTick(), 1u);

  // A registration after scheduling makes the schedule stale (the
  // M1-SYS-02 contract). The loop's next frame must surface the
  // runSystems failure — never silent (CORE-008).
  ASSERT_TRUE(w.registerSystem(makeDef("GLNoopB", &fnNoopB)).ok());
  synthFrameExpectFailure(loop, kSynthTickNs, ErrorCode::InvalidArgument);
  // The failed tick is not counted: the tick count froze at 1.
  EXPECT_EQ(loop.currentTick(), 1u);
  // runSystems raised its own warn (system/schedule_stale) — the loop
  // adds no event of its own.
  EXPECT_EQ(countEvents(*mem, "schedule_stale"), 1u);

  // The failure persists: the next frame re-derives the demand from
  // the clock and fails the same way (rate-limited: no new event).
  synthFrameExpectFailure(loop, kSynthTickNs, ErrorCode::InvalidArgument);
  EXPECT_EQ(loop.currentTick(), 1u);
  EXPECT_EQ(countEvents(*mem, "schedule_stale"), 1u);
  // No system ran in either failed frame: one measured run total.
  auto ts = w.systemTimingStats(laige::SystemId{1});
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts.value().runs, 1u);

  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The monotonic clock-source contract
// ---------------------------------------------------------------------------

TEST(GameLoop, BackwardClockJumpClampsInRelease) {
#ifdef NDEBUG
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, 5);

  ASSERT_TRUE(loop.frame().ok());
  // A forward frame: 10 ticks due, cap 5 -> 5 ticks run, 5 dropped.
  gSynthClockNs += 10 * kSynthTickNs;
  ASSERT_TRUE(loop.frame().ok());
  EXPECT_EQ(loop.currentTick(), 5u);
  // A backward jump below the start reference (misuse): release
  // clamps to the start reference — the frame contributes no time, no
  // tick, no new event (never undefined behavior).
  gSynthClockNs = -1000;
  ASSERT_TRUE(loop.frame().ok());
  EXPECT_EQ(loop.currentTick(), 5u);
  const GameLoopStats st = loop.stats();
  EXPECT_EQ(st.frames, 3u);
  EXPECT_EQ(st.ticks, 5u);
  EXPECT_EQ(st.droppedTicks, 5u);
  EXPECT_EQ(countEvents(*mem, "tick_dropped"), 1u);

  restoreConsoleSink();
#else
  GTEST_SKIP() << "the backward-jump clamp is a release-build property "
                 "(debug asserts — see BackwardClockJumpAbortsInDebug).";
#endif
}

TEST(GameLoop, BackwardClockJumpAbortsInDebug) {
#if defined(__unix__)
#  if defined(NDEBUG)
  GTEST_SKIP() << "assert-based monotonicity is a debug-build property";
#  else
  // The debug assert must fire (S-9 style). Exercised in a forked
  // child so the test process survives (the entity_tests
  // DestroyStaleAbortsInDebug pattern).
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: a forward frame, then a backward jump.
    auto w = World::create(World::Options{8});
    if (!w.ok()) _exit(117);
    World world = std::move(w).takeValue();
    if (!world.registerSystem(makeDef("GLNoopA", &fnNoopA)).ok()) _exit(117);
    SystemSchedule sched;
    if (!world.scheduleSystems(sched).ok()) _exit(117);
    GameLoop::Options opts;
    opts.nowNs = &synthNowNs;
    auto r = GameLoop::create(world, sched, opts);
    if (!r.ok()) _exit(117);
    GameLoop loop = std::move(r).takeValue();
    gSynthClockNs = 0;
    if (!loop.frame().ok()) _exit(117);
    gSynthClockNs = 10 * kSynthTickNs;
    if (!loop.frame().ok()) _exit(117);
    // Backward below the start reference: the debug assert must fire.
    gSynthClockNs = -1000;
    (void)loop.frame();
    _exit(1);  // unreachable: the parent fails below without the assert
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
      << "expected the monotonic-clock assert to abort the child (SIGABRT)";
#  endif
#else
  GTEST_SKIP() << "fork() is not available on Windows; the monotonic-clock "
                 "assert is exercised on the POSIX jobs.";
#endif
}

// ---------------------------------------------------------------------------
// The default headless clock (steady_clock) drives real frames
// ---------------------------------------------------------------------------

TEST(GameLoop, DefaultClockDrivesRealFrames) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop::Options opts;  // nowNs null -> the steady_clock default
  auto r = GameLoop::create(w, sched, opts);
  ASSERT_TRUE(r.ok());
  GameLoop loop = std::move(r).takeValue();

  ASSERT_TRUE(loop.frame().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(loop.frame().ok());

  const GameLoopStats st = loop.stats();
  EXPECT_EQ(st.frames, 2u);
  // 50 ms at 60 Hz is 3 ticks; allow preemption slack (the elapsed
  // time can only grow, so at least the 50 ms floor of 3 ticks).
  EXPECT_GE(st.ticks, 3u);
  // The world saw exactly the ticks the loop ran.
  auto ts = w.systemTimingStats(laige::SystemId{1});
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts.value().runs, st.ticks);
  // The success path is silent unless preemption overloaded the frame
  // (the only event that may then appear is the tick_dropped warn).
  for (const auto& e : mem->entries) {
    EXPECT_EQ(e.event, "tick_dropped");
  }

  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// Move: the state transfers, the source stops
// ---------------------------------------------------------------------------

TEST(GameLoop, MoveTransfersStateAndStopsTheSource) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, 5);

  ASSERT_TRUE(loop.frame().ok());
  for (int f = 0; f < 3; ++f) {
    synthFrame(loop, kSynthTickNs);
  }
  EXPECT_EQ(loop.currentTick(), 3u);

  GameLoop moved = std::move(loop);
  // The tick state traveled with the move.
  EXPECT_EQ(moved.currentTick(), 3u);
  const GameLoopStats st = moved.stats();
  EXPECT_EQ(st.frames, 4u);
  EXPECT_EQ(st.ticks, 3u);
  // The moved loop keeps driving from where the source left off.
  synthFrame(moved, kSynthTickNs);
  EXPECT_EQ(moved.currentTick(), 4u);

  // The moved-from loop is STOPPED: frame() fails without touching
  // the world and without logging (the moved-from-world precedent).
  auto s = loop.frame();
  ASSERT_FALSE(s.ok());
  EXPECT_EQ(s.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(mem->entries.size(), 0u);
  auto ts = w.systemTimingStats(laige::SystemId{1});
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts.value().runs, 4u);  // unchanged: the stopped loop ran none

  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The zero-allocation healthy frame path (PERF-003, the M1
// zero-allocation property; the M1-ECS-03/07 pattern)
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
TEST(GameLoop, HealthyFramesAllocateNothing) {
  // The test-only operator-new counter (non-sanitizer trees; the
  // sanitizer trees prove the loop leak-free).
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld();
  SystemSchedule sched = makeSchedule(w);
  GameLoop loop = makeLoop(w, sched, 60, 5);

  ASSERT_TRUE(loop.frame().ok());
  laige::test::resetAllocCounter();
  // 300 frames each demanding exactly 2 ticks (under the cap of 5):
  // 600 ticks, zero drops — the healthy catch-up path.
  for (int f = 0; f < 300; ++f) {
    synthFrame(loop, 2 * kSynthTickNs);
  }
  const std::uint64_t allocs = laige::test::allocCounter();
  // Per frame: one clock read, a few integer ops, up to 2 runSystems
  // dispatches (two clock reads + one ring write + two comparisons
  // per system) — nothing touches the heap while the systems stay
  // under budget and no drop fires.
  std::printf("game-loop-zeroalloc frames=300 ticks=%llu allocs=%llu\n",
              static_cast<unsigned long long>(loop.currentTick()),
              static_cast<unsigned long long>(allocs));
  EXPECT_EQ(loop.currentTick(), 600u);
  EXPECT_EQ(allocs, 0u);
  EXPECT_EQ(mem->entries.size(), 0u);
  restoreConsoleSink();
}
#endif
