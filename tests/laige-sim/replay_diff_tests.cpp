// laige-sim replay diff suite (M1-DET-05).
//
// Step Verify scope (roadmap/M1-heartbeat.md, `ctest -R replay_diff`):
//   - `World::stateDiff` (the bounded per-(entity, component)
//     comparison: canonical order — slots ascending, presence items
//     before component items, components ascending in the union of the
//     two worlds' sets — the exact difference set, and the bounded
//     report vs the total count)
//   - `World::componentStateHash` (the component-state part of the
//     state hash — canonical steps 1-4, the PRNG substream state
//     excluded; a pure function of the state like stateHash)
//   - `laige::diffReplays` (the diff driver): the identity +
//     determinism rejections (structured warns, never silent), the
//     lock-step tick walk aligned on the component-state hash, the
//     first divergent tick, the bounded state diff at that tick, the
//     length divergence, and the honest fullStateDivergent flag —
//     plus the roadmap's integration test: two replays that first
//     diverge at tick 37 report tick 37 and the diverged component
//
// Suite names: StateDiff.* and ReplayDiff.* — deliberately NOT
// Replay*-prefixed (the replay_record CTest entry selects exactly the
// Replay* suites from this shared executable).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/result.h"
#include "laige/sim/engine.h"
#include "laige/sim/replay.h"
#include "laige/sim/replay_diff.h"
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
              "replay_diff_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "replay_diff_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "replay_diff_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// The test component + the deferred-draw scenario system
// ---------------------------------------------------------------------------

struct RdValue {
  laige::fpx16_16 v{};
};
LAIGE_COMPONENT(RdValue);
LAIGE_DETERMINISM_SAFE(RdValue, laige::fpx16_16);

// The scenario's per-entity completed-tick count (the scenario
// clock). Per-entity, NOT a shared global: the diff driver runs two
// worlds in lock step, and a shared counter would double-advance
// (one bump per world per tick pair).
struct RdTick {
  laige::fpx16_16 t{};
};
LAIGE_COMPONENT(RdTick);
LAIGE_DETERMINISM_SAFE(RdTick, laige::fpx16_16);

namespace {

// Test plumbing for the deferred-draw scenario (the owner thread
// writes it only — PRD §10.2).
std::uint64_t gRdDrawFromTick = 0;  // 0 = never draw

}  // namespace

// The deferred draw: bumps every entity's per-entity tick count
// (RdTick), and — when armed (gRdDrawFromTick != 0) and an entity's
// completed tick count has REACHED it — draws once per entity from
// the system's PRNG substream (in the deterministic row order — the
// call order is the replay state, PRD §10.3) and writes the drawn
// value (as an integer fixed point) to the entity's RdValue. Before
// the arm tick the system only bumps the counters (identical state
// increments across seeds) — that is what makes the component state
// identical across seeds until the arm tick, and the first component
// divergence land exactly at it (the roadmap's tick-37 integration
// scenario).
LAIGE_SYSTEM(RdDraw, 1)
void RdDraw(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx.each<RdValue, RdTick>(
      [&ctx](laige::Entity e, RdValue& val, RdTick& tick) {
        static_cast<void>(e);
        // The Q16.16 raw, exact integer ticks (no fraction): raw >> 16.
        const std::uint32_t completed =
            static_cast<std::uint32_t>(tick.t.raw >> 16) + 1;
        tick.t = laige::fpx16_16::fromInt32(
            static_cast<std::int32_t>(completed));
        if (gRdDrawFromTick != 0 && completed >= gRdDrawFromTick) {
          const std::uint32_t value =
              ctx.rng != nullptr ? ctx.rng->next_range(0, 1000) : 0;
          val.v = laige::fpx16_16::fromInt32(static_cast<std::int32_t>(value));
        }
      },
      laige::Write{}, laige::Write{}));
}

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

laige::World makeWorld(std::uint32_t capacity, std::uint64_t seed,
                       bool deterministic = true) {
  laige::World::Options opts;
  opts.capacity = capacity;
  opts.seed = seed;
  opts.deterministic = deterministic;
  laige::Result<laige::World, laige::ErrorCode> r =
      laige::World::create(opts);
  if (!r.ok()) {
    ADD_FAILURE() << "World::create failed";
    abort();
  }
  return std::move(r).takeValue();
}

// The fixture scenario: capacity 64, RdValue (id 1) and RdTick
// (id 2) registered, RdDraw (system id 1) registered, and two
// entities with RdValue(0) + RdTick(0) — the fixed initial state both
// replays share (slot assignment is LIFO from the top: e1 lands in
// slot capacity-1, e2 in capacity-2).
struct RdScenario {
  laige::World world;
  laige::Entity e1{};
  laige::Entity e2{};
};

RdScenario makeRdWorld(std::uint64_t seed, bool deterministic = true) {
  laige::World world = makeWorld(64, seed, deterministic);
  if (!world.registerComponent<RdValue>().ok() ||
      !world.registerComponent<RdTick>().ok()) {
    ADD_FAILURE() << "component registration failed";
    abort();
  }
  if (!world.registerSystem(
           RdDraw_Def,
           laige::Io<RdValue, laige::Access::Write>{},
           laige::Io<RdTick, laige::Access::Write>{}).ok()) {
    ADD_FAILURE() << "RdDraw registration failed";
    abort();
  }
  const laige::Result<laige::Entity, laige::ErrorCode> e1 = world.create();
  const laige::Result<laige::Entity, laige::ErrorCode> e2 = world.create();
  if (!e1.ok() || !e2.ok()) {
    ADD_FAILURE() << "entity creation failed";
    abort();
  }
  static_cast<void>(world.addComponent(e1.value(), RdValue{
                                       laige::fpx16_16::fromInt32(0)}));
  static_cast<void>(world.addComponent(e1.value(), RdTick{
                                       laige::fpx16_16::fromInt32(0)}));
  static_cast<void>(world.addComponent(e2.value(), RdValue{
                                       laige::fpx16_16::fromInt32(0)}));
  static_cast<void>(world.addComponent(e2.value(), RdTick{
                                       laige::fpx16_16::fromInt32(0)}));
  return RdScenario{std::move(world), e1.value(), e2.value()};
}

// The shared test config (the scenario's identity).
laige::EngineConfig makeConfig(std::uint64_t seed) {
  laige::EngineConfig config;
  config.tickRateHz = 60;
  config.entityCapacity = 64;
  config.churnPerFrameBudget = 256;
  config.seed = seed;
  return config;
}

// Runs `ticks` ticks on `world` (one beginFrame() + one runSystems per
// tick — the replay driver's frame discipline) and returns the
// per-tick component-state hashes (tick 0 first).
std::vector<std::uint64_t>
runTicksComponentState(laige::World& world, std::uint64_t ticks) {
  laige::SystemSchedule sched;
  if (!world.scheduleSystems(sched).ok()) {
    ADD_FAILURE() << "scheduleSystems failed";
    abort();
  }
  std::vector<std::uint64_t> hashes;
  hashes.reserve(ticks + 1);
  hashes.push_back(world.componentStateHash(0));
  for (std::uint64_t t = 1; t <= ticks; ++t) {
    world.beginFrame();
    if (!world.runSystems(sched).ok()) {
      ADD_FAILURE() << "runSystems failed at tick " << t;
      abort();
    }
    hashes.push_back(world.componentStateHash(t));
  }
  return hashes;
}

// Records `ticks` ticks of `world` to a replay log at `path` (one
// zero-length frame per completed tick, the M1-DET-02 shape).
struct RecordResult {
  std::string path;
};

RecordResult recordRun(laige::World& world, const laige::EngineConfig& config,
                       std::uint64_t ticks, const std::string& path) {
  const laige::ReplayIdentity identity =
      laige::makeReplayIdentity(world, config);
  laige::Result<laige::ReplayRecorder, laige::ErrorCode> recR =
      laige::ReplayRecorder::create(identity, path, 0);
  if (!recR.ok()) {
    ADD_FAILURE() << "ReplayRecorder::create failed";
    abort();
  }
  laige::ReplayRecorder rec = std::move(recR).takeValue();
  laige::SystemSchedule sched;
  if (!world.scheduleSystems(sched).ok()) {
    ADD_FAILURE() << "scheduleSystems failed";
    abort();
  }
  for (std::uint64_t t = 1; t <= ticks; ++t) {
    world.beginFrame();
    if (!world.runSystems(sched).ok()) {
      ADD_FAILURE() << "runSystems failed at tick " << t;
      abort();
    }
    const laige::Status w = rec.writeFrame(t, nullptr, 0);
    if (w.isError()) {
      ADD_FAILURE() << "writeFrame failed at tick " << t;
      abort();
    }
  }
  if (!rec.finish().ok()) {
    ADD_FAILURE() << "ReplayRecorder::finish failed";
    abort();
  }
  return RecordResult{path};
}

// A temp path under gtest's temp dir (the replay_record_tests pattern).
std::string tempPath(const char* name) {
  return std::string(::testing::TempDir()) + name;
}

// The fpx16_16 stored in a StateDiffItem byte view, decoded as its
// integer value (every P0 target is little-endian — PRD §6). The
// fpx16_16 stores a Q16.16 raw (value = raw / 2^16, raw in an
// int32_t); the test values are integers, so the raw is the integer
// left-shifted 16 — we right-shift to recover it (exact, since the
// test values are integers in [0, 1000)).
std::uint32_t decodeFpx16(const std::uint8_t* bytes, std::uint32_t size) {
  EXPECT_EQ(size, sizeof(std::uint32_t));
  std::uint32_t raw = 0;
  for (std::uint32_t i = 0; i < size; ++i) {
    raw |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
  }
  return raw >> 16;
}

// Collects the stateDiff items (the test-local report).
std::vector<laige::World::StateDiffItem>
collectStateDiff(const laige::World& a, const laige::World& b,
                 std::uint32_t maxItems) {
  std::vector<laige::World::StateDiffItem> items;
  items.reserve(maxItems);
  const std::uint32_t total = a.stateDiff(b, maxItems, [&](const auto& item) {
    items.push_back(item);
  });
  EXPECT_LE(items.size(), maxItems);
  (void)total;
  return items;
}

}  // namespace

// ---------------------------------------------------------------------------
// The log event capture (the determinism_tests MemorySink pattern —
// Warn+ only, rate limiting off)
// ---------------------------------------------------------------------------

namespace {

class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
  };

  void emit(const laige::log::LogRecord& record) override {
    if (record.severity < laige::log::Severity::Warn) return;
    entries.push_back(Entry{record.severity, std::string(record.subsystem),
                            std::string(record.event),
                            std::string(record.message)});
  }
  void flush() override {}

  std::vector<Entry> entries;
};

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

}  // namespace

// ---------------------------------------------------------------------------
// The component-state hash (M1-DET-05; entity.h componentStateHash)
// ---------------------------------------------------------------------------

TEST(StateDiff, ComponentStateHashMatchesStateHashWithoutPrng) {
  // No system registered: the full state hash and the component-state
  // hash cover the SAME state (there is no substream to exclude) —
  // they must be equal, and a component change moves both.
  laige::World a = makeWorld(8, 7);
  static_cast<void>(a.registerComponent<RdValue>());
  const laige::Result<laige::Entity, laige::ErrorCode> e1 = a.create();
  ASSERT_TRUE(e1.ok());
  static_cast<void>(a.addComponent(e1.value(), RdValue{
                                         laige::fpx16_16::fromInt32(1)}));
  EXPECT_EQ(a.componentStateHash(0), a.stateHash(0));
  EXPECT_EQ(a.componentStateHash(5), a.stateHash(5));
  // A component byte change moves the component-state hash (and the
  // full one, which still equals it — no substream to exclude).
  const std::uint64_t before = a.componentStateHash(0);
  RdValue* v = a.get<RdValue>(e1.value());
  ASSERT_NE(v, nullptr);
  v->v = laige::fpx16_16::fromInt32(2);
  EXPECT_NE(a.componentStateHash(0), before);
  EXPECT_EQ(a.componentStateHash(0), a.stateHash(0));
}

// ---------------------------------------------------------------------------
// The bounded state diff (M1-DET-05; entity.h World::stateDiff)
// ---------------------------------------------------------------------------

TEST(StateDiff, IdenticalWorldsReportNothing) {
  const std::uint64_t seed = 0x4242;
  RdScenario a = makeRdWorld(seed);
  RdScenario b = makeRdWorld(seed);
  // Identical initial state: nothing differs.
  EXPECT_TRUE(collectStateDiff(a.world, b.world,
                              laige::kMaxReplayDiffEntries).empty());
  // Run identical ticks on both (unarmed: no draws, no mutations):
  // still nothing differs.
  gRdDrawFromTick = 0;
  static_cast<void>(runTicksComponentState(a.world, 5));
  static_cast<void>(runTicksComponentState(b.world, 5));
  EXPECT_TRUE(collectStateDiff(a.world, b.world,
                              laige::kMaxReplayDiffEntries).empty());
}

TEST(StateDiff, ComponentBytesAndOrder) {
  const std::uint64_t seed = 0x4242;
  RdScenario a = makeRdWorld(seed);
  RdScenario b = makeRdWorld(seed);
  const std::uint16_t slot1a = a.e1.id;
  const std::uint16_t slot2a = a.e2.id;
  // e1 differs, e2 matches (one component-type difference).
  a.world.get<RdValue>(a.e1)->v = laige::fpx16_16::fromInt32(7);
  std::vector<laige::World::StateDiffItem> items =
      collectStateDiff(a.world, b.world, laige::kMaxReplayDiffEntries);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].slot, slot1a);
  EXPECT_EQ(items[0].componentId, 1u);
  EXPECT_TRUE(items[0].presentInA && items[0].presentInB);
  EXPECT_EQ(decodeFpx16(items[0].bytesA, items[0].size), 7u);
  EXPECT_EQ(decodeFpx16(items[0].bytesB, items[0].size), 0u);

  // Both entities differ: canonical order is slots ASCENDING (e2 lands
  // in the lower slot — LIFO slot assignment — so e2 is reported
  // first).
  a.world.get<RdValue>(a.e2)->v = laige::fpx16_16::fromInt32(9);
  items = collectStateDiff(a.world, b.world, laige::kMaxReplayDiffEntries);
  ASSERT_EQ(items.size(), 2u);
  const std::uint16_t lowSlot = slot1a < slot2a ? slot1a : slot2a;
  const std::uint16_t highSlot = slot1a < slot2a ? slot2a : slot1a;
  EXPECT_EQ(items[0].slot, lowSlot);
  EXPECT_EQ(items[1].slot, highSlot);
}

TEST(StateDiff, PresenceDifferences) {
  const std::uint64_t seed = 0x4242;
  RdScenario a = makeRdWorld(seed);
  RdScenario b = makeRdWorld(seed);
  const std::uint16_t slot1a = a.e1.id;
  const std::uint16_t slot2a = a.e2.id;

  // Entity presence: b's e2 is destroyed (a_only, component 0).
  static_cast<void>(b.world.destroy(b.e2));
  std::vector<laige::World::StateDiffItem> items =
      collectStateDiff(a.world, b.world, laige::kMaxReplayDiffEntries);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].slot, slot2a);
  EXPECT_EQ(items[0].componentId, 0u);
  EXPECT_TRUE(items[0].presentInA && !items[0].presentInB);
  EXPECT_EQ(items[0].size, 0u);
  EXPECT_EQ(items[0].bytesA, nullptr);
  EXPECT_EQ(items[0].bytesB, nullptr);

  // Component presence: both worlds keep only e1, and b's e1 has lost
  // its component (a_only, component 1).
  static_cast<void>(a.world.destroy(a.e2));  // b's e2 was destroyed above
  static_cast<void>(b.world.removeComponent<RdValue>(b.e1));
  items = collectStateDiff(a.world, b.world, laige::kMaxReplayDiffEntries);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].slot, slot1a);
  EXPECT_EQ(items[0].componentId, 1u);
  EXPECT_TRUE(items[0].presentInA && !items[0].presentInB);
  EXPECT_EQ(decodeFpx16(items[0].bytesA, items[0].size), 0u);
  EXPECT_EQ(items[0].bytesB, nullptr);
}

TEST(StateDiff, BoundedReportCountsEverything) {
  const std::uint64_t seed = 0x4242;
  const std::uint32_t count = 10;
  laige::World a = makeWorld(count + 4, seed);
  laige::World b = makeWorld(count + 4, seed);
  static_cast<void>(a.registerComponent<RdValue>());
  static_cast<void>(b.registerComponent<RdValue>());
  for (std::uint32_t i = 1; i <= count; ++i) {
    const laige::Result<laige::Entity, laige::ErrorCode> ea = a.create();
    const laige::Result<laige::Entity, laige::ErrorCode> eb = b.create();
    ASSERT_TRUE(ea.ok() && eb.ok());
    static_cast<void>(a.addComponent(
        ea.value(), RdValue{laige::fpx16_16::fromInt32(static_cast<std::int32_t>(i))}));
    static_cast<void>(b.addComponent(
        eb.value(), RdValue{laige::fpx16_16::fromInt32(0)}));
  }
  // All 10 entities differ; the report is bounded to 3, the count is
  // exact.
  std::vector<laige::World::StateDiffItem> items;
  items.reserve(3);
  const std::uint32_t total = a.stateDiff(b, 3, [&](const auto& item) {
    items.push_back(item);
  });
  EXPECT_EQ(total, count);
  ASSERT_EQ(items.size(), 3u);
  // Canonical order: strictly ascending slots.
  for (std::size_t i = 1; i < items.size(); ++i) {
    EXPECT_LT(items[i - 1].slot, items[i].slot);
  }
  for (const auto& item : items) {
    EXPECT_EQ(item.componentId, 1u);
    EXPECT_TRUE(item.presentInA && item.presentInB);
    EXPECT_NE(decodeFpx16(item.bytesA, item.size),
              decodeFpx16(item.bytesB, item.size));
  }
}

// ---------------------------------------------------------------------------
// The diff driver (M1-DET-05; replay_diff.h diffReplays)
// ---------------------------------------------------------------------------

namespace {

// Records two scenarios (seeds seedA/seedB, `ticks` ticks each) and
// runs diffReplays on the caller's FRESH scenario worlds `wa`/`wb`
// (the recording consumed ticks — the diff replays from scratch).
// The caller owns `wa`/`wb`: the result's entry byte views point into
// their component columns (replay_diff.h — non-owning views, PERF-005)
// and are read AFTER runDiff returns. On success writes the result to
// `out` and returns true; on a failure records a test failure and
// returns false.
bool runDiff(std::uint64_t seedA, std::uint64_t seedB,
             std::uint64_t ticksA, std::uint64_t ticksB,
             std::uint32_t maxEntries, const char* nameA, const char* nameB,
             RdScenario& wa, RdScenario& wb,
             laige::ReplayDiffResult* out) {
  RdScenario recA = makeRdWorld(seedA);
  recordRun(recA.world, makeConfig(seedA), ticksA, tempPath(nameA));
  RdScenario recB = makeRdWorld(seedB);
  recordRun(recB.world, makeConfig(seedB), ticksB, tempPath(nameB));

  const laige::Result<laige::ReplayLog, laige::ErrorCode> logAR =
      laige::loadReplay(tempPath(nameA));
  if (!logAR.ok()) {
    ADD_FAILURE() << "loadReplay A failed: " << laige::errorText(logAR.error());
    return false;
  }
  const laige::Result<laige::ReplayLog, laige::ErrorCode> logBR =
      laige::loadReplay(tempPath(nameB));
  if (!logBR.ok()) {
    ADD_FAILURE() << "loadReplay B failed: " << laige::errorText(logBR.error());
    return false;
  }

  const laige::Result<laige::ReplayDiffResult, laige::ErrorCode> diff =
      laige::diffReplays(logAR.value(), logBR.value(), wa.world, wb.world,
                         makeConfig(seedA), makeConfig(seedB), maxEntries);
  if (!diff.ok()) {
    ADD_FAILURE() << "diffReplays failed: " << laige::errorText(diff.error());
    return false;
  }
  *out = diff.value();
  return true;
}

}  // namespace

TEST(ReplayDiff, IdenticalReplaysSameSeed) {
  gRdDrawFromTick = 0;  // never draw: the component state never changes
  laige::ReplayDiffResult r;
  RdScenario wa = makeRdWorld(0x11111111);
  RdScenario wb = makeRdWorld(0x11111111);
  ASSERT_TRUE(runDiff(0x11111111, 0x11111111, 4, 4,
                      laige::kReplayDiffDefaultEntries,
                      "rd-ident-a.log", "rd-ident-b.log", wa, wb, &r));
  EXPECT_TRUE(r.identical);
  EXPECT_FALSE(r.lengthDivergence);
  EXPECT_EQ(r.firstDivergentTick, 0u);  // the documented placeholder
  EXPECT_FALSE(r.fullStateDivergent);
  EXPECT_EQ(r.framesA, 4u);
  EXPECT_EQ(r.framesB, 4u);
  EXPECT_EQ(r.differingItems, 0u);
  EXPECT_TRUE(r.entries.empty());
}

TEST(ReplayDiff, ZeroFrameLogsIdentical) {
  gRdDrawFromTick = 0;
  laige::ReplayDiffResult r;
  RdScenario wa = makeRdWorld(0x11111111);
  RdScenario wb = makeRdWorld(0x11111111);
  ASSERT_TRUE(runDiff(0x11111111, 0x11111111, 0, 0,
                      laige::kReplayDiffDefaultEntries,
                      "rd-zero-a.log", "rd-zero-b.log", wa, wb, &r));
  EXPECT_TRUE(r.identical);
  EXPECT_EQ(r.framesA, 0u);
  EXPECT_EQ(r.framesB, 0u);
  EXPECT_FALSE(r.fullStateDivergent);  // same seed: same substreams
}

TEST(ReplayDiff, InitialStateDivergesAtTick0) {
  gRdDrawFromTick = 0;
  // Pre-seed b's e1 differently (the recording sees it — a different
  // initial state): the divergence is at tick 0.
  const std::uint64_t seed = 0x11111111;
  RdScenario recA = makeRdWorld(seed);
  recordRun(recA.world, makeConfig(seed), 2, tempPath("rd-tick0-a.log"));
  RdScenario recB = makeRdWorld(seed);
  recB.world.get<RdValue>(recB.e1)->v = laige::fpx16_16::fromInt32(7);
  recordRun(recB.world, makeConfig(seed), 2, tempPath("rd-tick0-b.log"));

  RdScenario wa = makeRdWorld(seed);
  RdScenario wb = makeRdWorld(seed);
  wb.world.get<RdValue>(wb.e1)->v = laige::fpx16_16::fromInt32(7);

  const laige::Result<laige::ReplayLog, laige::ErrorCode> logAR =
      laige::loadReplay(tempPath("rd-tick0-a.log"));
  ASSERT_TRUE(logAR.ok());
  const laige::Result<laige::ReplayLog, laige::ErrorCode> logBR =
      laige::loadReplay(tempPath("rd-tick0-b.log"));
  ASSERT_TRUE(logBR.ok());

  // maxEntries = 1: the report is bounded, the count is exact.
  const laige::Result<laige::ReplayDiffResult, laige::ErrorCode> diff =
      laige::diffReplays(logAR.value(), logBR.value(), wa.world, wb.world,
                         makeConfig(seed), makeConfig(seed), 1);
  ASSERT_TRUE(diff.ok()) << laige::errorText(diff.error());
  const laige::ReplayDiffResult& r = diff.value();
  EXPECT_FALSE(r.identical);
  EXPECT_FALSE(r.lengthDivergence);
  EXPECT_EQ(r.firstDivergentTick, 0u);
  EXPECT_TRUE(r.fullStateDivergent);  // the initial state bytes differ
  EXPECT_EQ(r.differingItems, 1u);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].slot, wb.e1.id);
  EXPECT_EQ(r.entries[0].componentId, 1u);
  EXPECT_TRUE(r.entries[0].presentInA && r.entries[0].presentInB);
  EXPECT_EQ(decodeFpx16(r.entries[0].bytesA, r.entries[0].size), 0u);
  EXPECT_EQ(decodeFpx16(r.entries[0].bytesB, r.entries[0].size), 7u);
}

TEST(ReplayDiff, LengthDivergence) {
  gRdDrawFromTick = 0;
  laige::ReplayDiffResult r;
  RdScenario wa = makeRdWorld(0x11111111);
  RdScenario wb = makeRdWorld(0x11111111);
  ASSERT_TRUE(runDiff(0x11111111, 0x11111111, 5, 2,
                      laige::kReplayDiffDefaultEntries,
                      "rd-length-a.log", "rd-length-b.log", wa, wb, &r));
  EXPECT_FALSE(r.identical);
  EXPECT_TRUE(r.lengthDivergence);
  EXPECT_EQ(r.firstDivergentTick, 3u);  // the first tick only A has
  EXPECT_EQ(r.framesA, 5u);
  EXPECT_EQ(r.framesB, 2u);
  EXPECT_EQ(r.differingItems, 0u);
  EXPECT_TRUE(r.entries.empty());
}

// ---------------------------------------------------------------------------
// The roadmap's integration test: two replays that first diverge at
// tick 37 report tick 37 and the diverged component.
// ---------------------------------------------------------------------------

TEST(ReplayDiff, ThirtySevenTickDivergence) {
  // The deferred draw arms at completed tick 37: the component state
  // is identical (and the only full-state divergence is the substream
  // seed) through tick 36, and the first component divergence lands
  // exactly at tick 37 — the tick-37 report the step's Verify names.
  gRdDrawFromTick = 37;
  const std::uint64_t seedA = 0x11111111;
  const std::uint64_t seedB = 0x22222222;

  laige::ReplayDiffResult r;
  RdScenario wa = makeRdWorld(seedA);
  RdScenario wb = makeRdWorld(seedB);
  ASSERT_TRUE(runDiff(seedA, seedB, 60, 60, laige::kReplayDiffDefaultEntries,
                      "rd-37-a.log", "rd-37-b.log", wa, wb, &r));
  gRdDrawFromTick = 0;

  EXPECT_FALSE(r.identical);
  EXPECT_FALSE(r.lengthDivergence);
  EXPECT_EQ(r.firstDivergentTick, 37u);
  EXPECT_TRUE(r.fullStateDivergent);  // the substream seed differs at tick 0
  EXPECT_EQ(r.framesA, 60u);
  EXPECT_EQ(r.framesB, 60u);
  // Both entities carry the drawn value at tick 37: two differences,
  // canonical slot order (ascending), the component is RdValue (id 1).
  EXPECT_EQ(r.differingItems, 2u);
  ASSERT_EQ(r.entries.size(), 2u);
  const std::uint16_t lowSlot = r.entries[0].slot;
  const std::uint16_t highSlot = r.entries[1].slot;
  EXPECT_LT(lowSlot, highSlot);
  for (const laige::World::StateDiffItem& item : r.entries) {
    EXPECT_EQ(item.componentId, 1u);
    EXPECT_TRUE(item.presentInA && item.presentInB);
    // The two seeds drew different values at tick 37 (independent
    // xorshift substreams — a match would be a 1-in-1000 flake, and
    // the fixed seeds make it a deterministic property of this test).
    EXPECT_NE(decodeFpx16(item.bytesA, item.size),
              decodeFpx16(item.bytesB, item.size));
  }
  // Machine-greppable identity line (docs/testing.md §4): the seeds,
  // the divergence tick, and the drawn values (the KAT of the
  // scenario: a change to the draw path or the diff fails here).
  std::printf("replay-diff-37 seed_a=0x%016llx seed_b=0x%016llx tick=37 "
              "items=2 value_a=%u value_b=%u\n",
              static_cast<unsigned long long>(seedA),
              static_cast<unsigned long long>(seedB),
              decodeFpx16(r.entries[0].bytesA, r.entries[0].size),
              decodeFpx16(r.entries[0].bytesB, r.entries[0].size));
  std::fflush(stdout);
}

// The KAT for the scenario's draw: at the arm tick (37), each entity
// draws once from the world's system-1 substream (derived from
// (seed, 1)) reduced to [0, 1000) — in the deterministic row order,
// so the LOWER slot (e2) takes the FIRST draw and e1 the second —
// pinned against an independent Prng (the test-local check of the
// draw path against the house PRNG contract).
TEST(ReplayDiff, DrawValueMatchesIndependentPrng) {
  const std::uint64_t seed = 0x11111111;
  laige::Prng ref = laige::Prng::deriveSubstream(seed, 1);
  const std::uint32_t drawLow = ref.next_range(0, 1000);  // e2 (slot 62)
  const std::uint32_t drawHigh = ref.next_range(0, 1000);  // e1 (slot 63)

  gRdDrawFromTick = 37;
  RdScenario a = makeRdWorld(seed);
  // Exactly 37 ticks: the arm tick draws once per entity — the
  // values end at the two consecutive draws of the substream.
  recordRun(a.world, makeConfig(seed), 37, tempPath("rd-kat-a.log"));
  gRdDrawFromTick = 0;

  RdValue* v1 = a.world.get<RdValue>(a.e1);
  RdValue* v2 = a.world.get<RdValue>(a.e2);
  ASSERT_NE(v1, nullptr);
  ASSERT_NE(v2, nullptr);
  EXPECT_EQ(v1->v,
            laige::fpx16_16::fromInt32(static_cast<std::int32_t>(drawHigh)));
  EXPECT_EQ(v2->v,
            laige::fpx16_16::fromInt32(static_cast<std::int32_t>(drawLow)));
  std::printf("replay-diff-37-kat seed=0x%016llx tick=37 draw_low=%u "
              "draw_high=%u\n",
              static_cast<unsigned long long>(seed), drawLow, drawHigh);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// The rejections (CORE-008: never silent — structured warns + errors)
// ---------------------------------------------------------------------------

TEST(ReplayDiff, IdentityMismatchRejected) {
  MemorySink* sink = installCaptureSink();
  const std::uint64_t seed = 42;
  const laige::EngineConfig config = makeConfig(seed);

  gRdDrawFromTick = 0;
  RdScenario a = makeRdWorld(seed);
  recordRun(a.world, config, 4, tempPath("rd-ident-a.log"));
  RdScenario b = makeRdWorld(seed);
  recordRun(b.world, config, 4, tempPath("rd-ident-b.log"));
  RdScenario wa = makeRdWorld(seed);
  RdScenario wb = makeRdWorld(seed);

  const laige::Result<laige::ReplayLog, laige::ErrorCode> logAR =
      laige::loadReplay(tempPath("rd-ident-a.log"));
  ASSERT_TRUE(logAR.ok());
  const laige::Result<laige::ReplayLog, laige::ErrorCode> logBR =
      laige::loadReplay(tempPath("rd-ident-b.log"));
  ASSERT_TRUE(logBR.ok());

  // Control: the matching identity diff is empty and the run succeeds.
  EXPECT_TRUE(laige::replayIdentityDiff(logAR.value(), wa.world, config)
                  .empty());
  EXPECT_TRUE(laige::diffReplays(logAR.value(), logBR.value(), wa.world,
                                 wb.world, config, config)
                  .ok());
  EXPECT_EQ(countEvents(*sink, "diff_identity_mismatch"), 0u);

  // Case 1: logA's config tick rate differs -> A is named, once.
  laige::EngineConfig tickCfg = config;
  tickCfg.tickRateHz = 120;
  const laige::Result<laige::ReplayDiffResult, laige::ErrorCode> r1 =
      laige::diffReplays(logAR.value(), logBR.value(), wa.world, wb.world,
                         tickCfg, config);
  EXPECT_EQ(r1.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "diff_identity_mismatch"), 1u);

  // Case 2: logB's config churn budget differs -> B is named, once
  // (the cumulative count is now 2).
  laige::EngineConfig churnCfg = config;
  churnCfg.churnPerFrameBudget = 255;
  const laige::Result<laige::ReplayDiffResult, laige::ErrorCode> r2 =
      laige::diffReplays(logAR.value(), logBR.value(), wa.world, wb.world,
                         config, churnCfg);
  EXPECT_EQ(r2.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "diff_identity_mismatch"), 2u);

  // Case 3: BOTH mismatch -> one report names both (the cumulative
  // count is now 4).
  const laige::Result<laige::ReplayDiffResult, laige::ErrorCode> r3 =
      laige::diffReplays(logAR.value(), logBR.value(), wa.world, wb.world,
                         tickCfg, churnCfg);
  EXPECT_EQ(r3.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "diff_identity_mismatch"), 4u);
  restoreLogger();
}

TEST(ReplayDiff, DeterminismDisabledRejected) {
  // The M1-DET-03 precedent: the log is recorded UNDER the disabled
  // config (so its config hash matches — the identity check passes)
  // and a non-deterministic world; the diff is then rejected by the
  // determinism check itself, with the structured warn naming the log.
  MemorySink* sink = installCaptureSink();
  const std::uint64_t seed = 42;
  const laige::EngineConfig config = makeConfig(seed);
  laige::EngineConfig offCfg = config;
  offCfg.determinism.enabled = false;

  gRdDrawFromTick = 0;
  RdScenario a = makeRdWorld(seed);
  recordRun(a.world, config, 4, tempPath("rd-det-a.log"));
  RdScenario b = makeRdWorld(seed, /*deterministic*/ false);
  recordRun(b.world, offCfg, 4, tempPath("rd-det-b.log"));
  RdScenario wa = makeRdWorld(seed);
  RdScenario wb = makeRdWorld(seed, /*deterministic*/ false);

  const laige::Result<laige::ReplayLog, laige::ErrorCode> logAR =
      laige::loadReplay(tempPath("rd-det-a.log"));
  ASSERT_TRUE(logAR.ok());
  const laige::Result<laige::ReplayLog, laige::ErrorCode> logBR =
      laige::loadReplay(tempPath("rd-det-b.log"));
  ASSERT_TRUE(logBR.ok());

  // The identity checks pass (each log matches its own config) —
  // the rejection is the determinism check, not the identity check.
  EXPECT_TRUE(laige::replayIdentityDiff(logAR.value(), wa.world, config)
                  .empty());
  EXPECT_TRUE(laige::replayIdentityDiff(logBR.value(), wb.world, offCfg)
                  .empty());
  const laige::Result<laige::ReplayDiffResult, laige::ErrorCode> r =
      laige::diffReplays(logAR.value(), logBR.value(), wa.world, wb.world,
                         config, offCfg);
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "diff_determinism_disabled"), 1u);
  EXPECT_EQ(countEvents(*sink, "diff_identity_mismatch"), 0u);
  restoreLogger();
}
