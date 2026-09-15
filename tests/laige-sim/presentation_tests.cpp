// laige-sim presentation snapshot + interpolation state suite
// (M1-LOOP-02).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - interpolation is LINEAR between ticks (exact Q16.16 lerp values
//     at alpha 0, 0.5, 0.75, and the Q16.16 rounding of a near-1
//     alpha — all expectations in exact raw units, no float
//     round-trips)
//   - the alpha CLAMPS to [0, 1] — never extrapolates (a render
//     before the anchor clamps to 0; a clock jump a full tick or
//     more past the anchor clamps to 1; exact values in between are
//     preserved)
//   - entities ADDED BETWEEN TICKS sample correctly — they snap to
//     their current Position2D value (the documented scope behavior),
//     then interpolate normally from the second tick after creation
//   - the catch-up case: one frame running several ticks refreshes
//     prev/curr per tick (the sample interpolates the LATEST tick's
//     interval, not a multi-tick span)
//   - the GameLoop's onTick hook drives the snapshot once per
//     COMPLETED tick (a failed tick does not fire it); the
//     startReferenceNs wiring keeps the alpha anchored to the loop's
//     time base
//   - sample_position rejects stale handles (warn-once, the
//     World::check precedent) and live handles without a Position2D
//     (a negative query, no warn)
//   - create() validates the tick rate (20–120 Hz, the loop's range)
//     with one rate-limited warn (presentation/tick_rate_invalid)
//   - a moved snapshot transfers its state; the source is stopped
//     (no world access, no logging)
//   - no heap allocation on the per-frame refresh/sample path
//     (test-only operator-new counter, non-sanitizer trees; the
//     sanitizer trees prove the same loop leak-free)
//   - the fp32_pinned backend instantiates the same contract (one
//     linear-interpolation check on the float alpha path)
//
// Runs as CTest `presentation` (the step's Verify command:
// `ctest -R presentation`): a filtered view of the shared
// laige-sim_tests executable, selecting exactly the suites below.
//
// Q16.16 raw reference (value = raw / 65536): 0.5 = 32768,
// 0.75 = 49152, 1.0 = 65536, 1.5 = 98304, 1.75 = 114688,
// 2.0 = 131072, 2.5 = 163840, 3.5 = 229376, 6.5 = 425984.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim_math.h"
#include "laige/sim/entity.h"
#include "laige/sim/game_loop.h"
#include "laige/sim/presentation.h"
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
              "presentation_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "presentation_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "presentation_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define PRESENTATION_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define PRESENTATION_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

static_assert(PRESENTATION_TESTS_ACTIVE_CPLUSPLUS >= 202002L,
              "presentation_tests must be built with C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");

namespace {

using laige::Access;
using laige::Entity;
using laige::ErrorCode;
using laige::GameLoop;
using laige::Io;
using laige::Position2DFp32;
using laige::Position2DFpx16;
using laige::PresentationSnapshot;
using laige::Status;
using laige::SystemDef;
using laige::SystemFn;
using laige::SystemSchedule;
using laige::World;
using laige::Write;
using laige::fpx16_16;
using laige::sim::Fp32Pinned;
using laige::sim::Fpx16_16;
using laige::sim::SimMathFpx16;
using Vec2 = laige::sim::SimMathFpx16::Vec2;

// The test tick rate: 100 Hz — the tick period is EXACTLY 1e7 ns
// (1e9 / 100, integer), so every anchor A(T) = T × 1e7 ns and every
// expected alpha below is an exact rational of small numerator (no
// floating-point expectations anywhere in the suite).
constexpr std::uint32_t kTestRateHz = 100;
constexpr std::int64_t kTickNs = 10000000;  // 1e9 / kTestRateHz

// Q16.16 raw constructors (the exact expected values are expressed in
// raw units — no float round-trip in the test).
fpx16_16 q16(std::int32_t raw) { return fpx16_16{raw}; }
fpx16_16 fx(std::int32_t v) { return fpx16_16::fromInt32(v); }
Vec2 vec(std::int32_t xRaw, std::int32_t yRaw) {
  return Vec2{q16(xRaw), q16(yRaw)};
}

// The expected-value helper (Vec2 has no operator==; the expectations
// are exact bit equality on the Q16.16 raw units).
void expectVec(const char* what, Vec2 v, std::int32_t xRaw,
               std::int32_t yRaw) {
  EXPECT_EQ(v.x.raw, xRaw) << what;
  EXPECT_EQ(v.y.raw, yRaw) << what;
}

// ---------------------------------------------------------------------------
// The synthetic clock (the Options::nowNs injection seam — the
// LoggerOptions::ClockFn precedent). Monotonic by construction: the
// tests only advance it.
// ---------------------------------------------------------------------------

std::int64_t gSynthClockNs = 0;

std::int64_t synthNowNs() {
  return gSynthClockNs;
}

// ---------------------------------------------------------------------------
// World builder (the game_loop_tests pattern)
// ---------------------------------------------------------------------------

World makeWorld(std::uint32_t capacity) {
  auto w = World::create(World::Options{capacity});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << capacity << ") failed: "
                  << laige::errorName(w.error());
    std::abort();
  }
  World world = std::move(w).takeValue();
  if (!world.registerComponent<Position2DFpx16>().ok()) {
    ADD_FAILURE() << "registerComponent<Position2DFpx16> failed";
    std::abort();
  }
  return world;
}

Entity makeEntity(World& world, Vec2 pos) {
  auto r = world.create();
  if (!r.ok()) {
    ADD_FAILURE() << "World::create() failed: " << laige::errorName(r.error());
    std::abort();
  }
  Entity e = std::move(r).takeValue();
  if (!world
           .addComponent<Position2DFpx16>(e, Position2DFpx16{pos})
           .ok()) {
    ADD_FAILURE() << "addComponent<Position2DFpx16> failed";
    std::abort();
  }
  return e;
}

void setPos(World& world, Entity e, Vec2 pos) {
  if (!world
           .addComponent<Position2DFpx16>(e, Position2DFpx16{pos})
           .ok()) {
    ADD_FAILURE() << "addComponent<Position2DFpx16> (overwrite) failed";
    std::abort();
  }
}

// ---------------------------------------------------------------------------
// The movement system for the hook test: +1 unit on x per tick.
// ---------------------------------------------------------------------------

void fnPMove(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  const Vec2 step{fx(1), fx(0)};
  // ctx.each can only fail on a nested iteration — the guard logs it
  // itself (ecs/iteration_nested); the system has nothing to add
  // (the scheduler_tests convention).
  static_cast<void>(ctx.each<Position2DFpx16>(
      [&step](Entity, Position2DFpx16& p) {
        p.pos = SimMathFpx16::add(p.pos, step);
      },
      Write{}));
}

void fnPNoop(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

SystemDef makeDef(const char* name, SystemFn fn) {
  return SystemDef{name, fn, laige::fpx16_16::fromInt32(1), nullptr};
}

// The GameLoop onTick hook's thunk (the M1-HEAD-01 wiring shape): the
// snapshot's onTick behind the void* context.
void onTickThunk(void* ctx, laige::World& world, std::uint64_t tick) noexcept {
  static_cast<void>(world);
  static_cast<PresentationSnapshot<Fpx16_16>*>(ctx)->onTick(tick);
}

// ---------------------------------------------------------------------------
// Log capture (the logging_tests / game_loop_tests pattern)
// ---------------------------------------------------------------------------

// A test-only Sink that records every emitted event (the logging
// facade is a process singleton; the tests that use it restore the
// default console sink at the end — the game_loop_tests pattern).
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
// suppressed and summarized at shutdown — the game_loop_tests
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

// The events that are NOT the world's own setup-lifecycle events.
// The World emits Info events when a component set first appears
// (ecs/archetype_created) and when its row capacity doubles (bounded
// reservation, ecs/archetype_grow); a burst of setup adds can also
// trip the rate-limited ecs/churn_per_frame warn (the 500 setup adds
// of the zero-alloc test exceed the 256 per-frame budget). Those are
// the setup path's, never the snapshot's — the snapshot's own
// silence (LOG-003) is asserted on this count.
std::size_t snapshotEvents(const MemorySink& s) {
  std::size_t n = 0;
  for (const auto& e : s.entries) {
    if (e.event != "archetype_created" && e.event != "archetype_grow" &&
        e.event != "churn_per_frame") {
      ++n;
    }
  }
  return n;
}

const MemorySink::Entry* findEvent(const MemorySink& s, const char* event) {
  for (const auto& e : s.entries) {
    if (e.event == event) return &e;
  }
  return nullptr;
}

const char* fieldValue(const MemorySink::Entry& entry, const char* key) {
  for (const auto& [k, v] : entry.fields) {
    if (k == key) return v.c_str();
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// create(): tick-rate validation (the loop's documented 20–120 Hz
// range; one rate-limited warn)
// ---------------------------------------------------------------------------

TEST(Presentation, CreateValidatesTheTickRate) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld(8);

  // Below the range (19 Hz): rejected, one tick_rate_invalid warn.
  {
    auto r = PresentationSnapshot<Fpx16_16>::create(
        w, 0, PresentationSnapshot<Fpx16_16>::Options{19});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  // Above the range (121 Hz): rejected (the second warn for the same
  // key is rate-limited within the 60 s window — the summary at
  // shutdown carries it, checked below).
  {
    auto r = PresentationSnapshot<Fpx16_16>::create(
        w, 0, PresentationSnapshot<Fpx16_16>::Options{121});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
  }
  // The range endpoints are legal.
  {
    auto r = PresentationSnapshot<Fpx16_16>::create(
        w, 0, PresentationSnapshot<Fpx16_16>::Options{20});
    ASSERT_TRUE(r.ok());
  }
  {
    auto r = PresentationSnapshot<Fpx16_16>::create(
        w, 0, PresentationSnapshot<Fpx16_16>::Options{120});
    ASSERT_TRUE(r.ok());
  }
  // The default (an empty Options): kDefaultTickRateHz (60).
  {
    auto r = PresentationSnapshot<Fpx16_16>::create(w, 0, {});
    ASSERT_TRUE(r.ok());
  }

  // The warns: exactly one tick_rate_invalid (the 19 Hz case; the
  // 121 Hz repeat is rate-limited — LOG-004), subsystem
  // "presentation", with the rejected value as a structured field.
  EXPECT_EQ(countEvents(*mem, "tick_rate_invalid"), 1u);
  ASSERT_EQ(mem->entries.size(), 1u);
  const auto& rateEntry = mem->entries[0];
  EXPECT_EQ(rateEntry.subsystem, "presentation");
  EXPECT_EQ(rateEntry.severity, laige::log::Severity::Warn);
  EXPECT_STREQ(fieldValue(rateEntry, "tick_rate_hz"), "19");

  // Shutdown: the suppressed 121 Hz repeat is summarized.
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(mem->entries.size(), 2u);
  EXPECT_EQ(mem->entries[1].event, "rate_limited");
  EXPECT_STREQ(fieldValue(mem->entries[1], "event"), "tick_rate_invalid");
  EXPECT_STREQ(fieldValue(mem->entries[1], "suppressed"), "1");
  sink = nullptr;
}

// ---------------------------------------------------------------------------
// Linear interpolation between ticks (the step's core Verify): exact
// Q16.16 lerp values at alpha 0, 0.5, 0.75, and the Q16.16 rounding
// of a near-1 alpha
// ---------------------------------------------------------------------------

TEST(Presentation, LinearInterpolationBetweenTicks) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld(64);
  Entity e = makeEntity(w, vec(0, 0));
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> snap = std::move(snapR).takeValue();

  // Before any tick: alpha is 0 and every sample snaps (no refresh
  // has run) — the entity's current value, (0,0).
  snap.onRenderFrame(5000000);
  EXPECT_EQ(snap.alpha().raw, 0);
  auto s0 = snap.sample_position(e);
  ASSERT_TRUE(s0.ok());
  expectVec("pre-tick snap", s0.value(), 0, 0);

  // Tick 1: the sim moves the entity to (1,0) (manual drive). First
  // sight: the snapshot snaps — prev = curr = (1,0).
  setPos(w, e, vec(65536, 0));
  snap.onTick(1);
  // Render exactly at A(1) = 1e7: alpha 0 -> prev (== curr here).
  snap.onRenderFrame(kTickNs);
  EXPECT_EQ(snap.alpha().raw, 0);
  auto s1 = snap.sample_position(e);
  ASSERT_TRUE(s1.ok());
  expectVec("at A(1)", s1.value(), 65536, 0);

  // Tick 2: the entity moves to (2,0). Now prev = (1,0), curr = (2,0).
  setPos(w, e, vec(131072, 0));
  snap.onTick(2);

  // Render exactly at A(2) = 2e7: alpha 0 -> prev (1,0).
  snap.onRenderFrame(2 * kTickNs);
  EXPECT_EQ(snap.alpha().raw, 0);
  auto s2 = snap.sample_position(e);
  ASSERT_TRUE(s2.ok());
  expectVec("at A(2)", s2.value(), 65536, 0);

  // Render at the MIDPOINT of (A(2), A(3)) — A(2) + 5e6: alpha is
  // exactly 0.5 (5e8 / 1e9); the lerp is exactly (1.5, 0) (raw 98304).
  snap.onRenderFrame(2 * kTickNs + 5000000);
  EXPECT_EQ(snap.alpha().raw, 32768);  // exactly 0.5 in Q16.16
  auto s3 = snap.sample_position(e);
  ASSERT_TRUE(s3.ok());
  expectVec("midpoint", s3.value(), 98304, 0);

  // Render at the 3/4 point — A(2) + 7500000: alpha exactly 0.75
  // (75e7 / 1e9); the lerp is exactly (1.75, 0) (raw 114688).
  snap.onRenderFrame(2 * kTickNs + 7500000);
  EXPECT_EQ(snap.alpha().raw, 49152);  // exactly 0.75 in Q16.16
  auto s4 = snap.sample_position(e);
  ASSERT_TRUE(s4.ok());
  expectVec("3/4 point", s4.value(), 114688, 0);

  // Render 0.01 tick short of A(3) — A(2) + 9900000: alpha exactly
  // 0.99 (99e7 / 1e9) — not exactly representable in Q16.16 (raw
  // 64881 = 0.989993...): the documented single rounding, then the
  // lerp exactly (1 + 1 × 64881/65536, 0) = (raw 130417, 0).
  snap.onRenderFrame(2 * kTickNs + 9900000);
  EXPECT_EQ(snap.alpha().raw, 64881);
  auto s5 = snap.sample_position(e);
  ASSERT_TRUE(s5.ok());
  expectVec("near-1 alpha", s5.value(), 130417, 0);

  // Machine-greppable evidence line (the step's Verify).
  std::printf("presentation linear rate=100Hz prev=(1,0) curr=(2,0) "
              "alpha(0.5)=%d/65536 sample=(%d/65536, 0)\n",
              32768, 98304);

  // The snapshot's own path is silent (LOG-003): the only entry is
  // the world's setup archetype_created Info.
  EXPECT_EQ(snapshotEvents(*mem), 0u);
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The alpha clamps to [0, 1] — never extrapolates: a render before the
// anchor (or far before it) clamps to 0; a render exactly at the next
// anchor is 1.0; a clock jump a full tick or more past the anchor
// clamps to 1; exact values in between are preserved
// ---------------------------------------------------------------------------

TEST(Presentation, AlphaClampsToTheUnitInterval) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld(64);
  Entity e = makeEntity(w, vec(65536, 0));
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> snap = std::move(snapR).takeValue();

  // Drive to lastTick = 2: prev = (1,0), curr = (2,0); anchors
  // A(2) = 2e7, A(3) = 3e7.
  snap.onTick(1);
  setPos(w, e, vec(131072, 0));
  snap.onTick(2);

  // Render at the start reference (before the first anchor): clamps
  // to 0 -> prev.
  snap.onRenderFrame(0);
  EXPECT_EQ(snap.alpha().raw, 0);
  auto sStart = snap.sample_position(e);
  ASSERT_TRUE(sStart.ok());
  expectVec("at start", sStart.value(), 65536, 0);

  // Render 1 ns before A(2): the exact alpha is -1e-7 — clamps to 0.
  snap.onRenderFrame(2 * kTickNs - 1);
  EXPECT_EQ(snap.alpha().raw, 0);
  auto sBefore = snap.sample_position(e);
  ASSERT_TRUE(sBefore.ok());
  expectVec("1ns before anchor", sBefore.value(), 65536, 0);

  // Render 1 ns past A(2): the exact alpha is 1e-7 — below the
  // Q16.16 resolution (raw 0), so the sample is still exactly prev.
  snap.onRenderFrame(2 * kTickNs + 1);
  EXPECT_EQ(snap.alpha().raw, 0);
  auto sAfter = snap.sample_position(e);
  ASSERT_TRUE(sAfter.ok());
  expectVec("1ns past anchor", sAfter.value(), 65536, 0);

  // Render 10 us past A(2): alpha exactly 0.001 (1e4 * 100 / 1e9 =
  // 1e6 / 1e9 -> Q16 raw 66); the lerp x = 1 + 1 × 66/65536 = raw
  // 65536 + 66 = 65602 — a non-degenerate small alpha moves the
  // sample off prev.
  snap.onRenderFrame(2 * kTickNs + 10000);
  EXPECT_EQ(snap.alpha().raw, 66);
  auto sSmall = snap.sample_position(e);
  ASSERT_TRUE(sSmall.ok());
  expectVec("10us past anchor", sSmall.value(), 65602, 0);

  // Render exactly at A(3) — one full tick past A(2): alpha exactly
  // 1.0 (raw 65536) -> exactly curr.
  snap.onRenderFrame(3 * kTickNs);
  EXPECT_EQ(snap.alpha().raw, 65536);  // exactly 1.0 in Q16.16
  auto sNext = snap.sample_position(e);
  ASSERT_TRUE(sNext.ok());
  expectVec("at A(3)", sNext.value(), 131072, 0);

  // CLOCK JUMP: a render half a tick past A(3) (R = 35 ms) — a full
  // tick or more past the anchor A(2): clamps to 1 -> exactly curr.
  snap.onRenderFrame(35000000);
  EXPECT_EQ(snap.alpha().raw, 65536);
  auto sJump = snap.sample_position(e);
  ASSERT_TRUE(sJump.ok());
  expectVec("half tick past A(3)", sJump.value(), 131072, 0);

  // Larger jumps: 5 s and 16.7 min past the start — both clamp to 1
  // (the exact value would be ~498 and ~99998 full ticks past the
  // anchor).
  snap.onRenderFrame(5000000000);
  EXPECT_EQ(snap.alpha().raw, 65536);
  auto s5s = snap.sample_position(e);
  ASSERT_TRUE(s5s.ok());
  expectVec("5s jump", s5s.value(), 131072, 0);
  snap.onRenderFrame(1000000000000);
  EXPECT_EQ(snap.alpha().raw, 65536);
  auto sMin = snap.sample_position(e);
  ASSERT_TRUE(sMin.ok());
  expectVec("16.7min jump", sMin.value(), 131072, 0);

  // An absurd reading (285 years) still clamps — the branch guard
  // (no overflow, CPP-004) keeps the sample exactly curr.
  snap.onRenderFrame(9000000000000000000);
  EXPECT_EQ(snap.alpha().raw, 65536);
  auto sYear = snap.sample_position(e);
  ASSERT_TRUE(sYear.ok());
  expectVec("285y jump", sYear.value(), 131072, 0);

  // A render reading BELOW the start reference (a non-monotonic render
  // clock — a wiring misuse): clamps to the start (no time before the
  // base) — never UB, alpha 0.
  snap.onRenderFrame(-5);
  EXPECT_EQ(snap.alpha().raw, 0);
  auto sNeg = snap.sample_position(e);
  ASSERT_TRUE(sNeg.ok());
  expectVec("below start", sNeg.value(), 65536, 0);

  // The snapshot's own path is silent (LOG-003): the only entry is
  // the world's setup archetype_created Info.
  EXPECT_EQ(snapshotEvents(*mem), 0u);
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// Entities added between ticks snap to their current value (the
// documented scope behavior), then interpolate normally from the
// second tick after creation
// ---------------------------------------------------------------------------

TEST(Presentation, EntityAddedBetweenTicksSnapsToCurr) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld(64);
  Entity e1 = makeEntity(w, vec(65536, 0));
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> snap = std::move(snapR).takeValue();

  // Tick 1 (e1 first sight: snaps to (1,0)); tick 2 (e1 -> (2,0)).
  snap.onTick(1);
  setPos(w, e1, vec(131072, 0));
  snap.onTick(2);

  // BETWEEN TICKS: e2 is created at (5,5) — after the last onTick.
  Entity e2 = makeEntity(w, vec(327680, 327680));

  // Sample e2 before any refresh has seen it: SNAP — exactly the
  // current world value (no phantom interpolation, no error).
  snap.onRenderFrame(2 * kTickNs + 5000000);  // alpha 0.5
  auto sNew = snap.sample_position(e2);
  ASSERT_TRUE(sNew.ok());
  expectVec("new entity snap", sNew.value(), 327680, 327680);

  // e1 interpolates as usual in the same frame.
  auto s1 = snap.sample_position(e1);
  ASSERT_TRUE(s1.ok());
  expectVec("e1 midpoint", s1.value(), 98304, 0);  // (1.5, 0)

  // Tick 3: e1 -> (3,0); e2 -> (6,6). e2 is first sight: snaps to
  // (6,6).
  setPos(w, e1, vec(196608, 0));
  setPos(w, e2, vec(393216, 393216));
  snap.onTick(3);
  snap.onRenderFrame(3 * kTickNs + 5000000);  // alpha 0.5
  auto sNew3 = snap.sample_position(e2);
  ASSERT_TRUE(sNew3.ok());
  expectVec("e2 tick-3 snap", sNew3.value(), 393216, 393216);
  auto s13 = snap.sample_position(e1);
  ASSERT_TRUE(s13.ok());
  expectVec("e1 tick-3 midpoint", s13.value(), 163840, 0);  // (2.5, 0)

  // Tick 4: e2 -> (7,7). Now e2 has a real prev/curr: it interpolates
  // — at alpha 0.5 exactly (6.5, 6.5) (raw 425984).
  setPos(w, e2, vec(458752, 458752));
  snap.onTick(4);
  snap.onRenderFrame(4 * kTickNs + 5000000);  // alpha 0.5
  auto sNew4 = snap.sample_position(e2);
  ASSERT_TRUE(sNew4.ok());
  expectVec("e2 tick-4 midpoint", sNew4.value(), 425984, 425984);

  // The snapshot's own path is silent (LOG-003): the only entry is
  // the world's setup archetype_created Info (e2 reuses e1's
  // archetype).
  EXPECT_EQ(snapshotEvents(*mem), 0u);
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The catch-up case (one frame running several ticks): prev/curr
// refresh PER TICK — the sample interpolates the LATEST tick's
// interval, not a multi-tick span
// ---------------------------------------------------------------------------

TEST(Presentation, CatchUpFrameInterpolatesTheLatestTick) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld(64);
  Entity e = makeEntity(w, vec(0, 0));
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> snap = std::move(snapR).takeValue();

  // One frame runs TWO ticks (the manual catch-up form): (1,0) then
  // (2,0).
  setPos(w, e, vec(65536, 0));
  snap.onTick(1);
  setPos(w, e, vec(131072, 0));
  snap.onTick(2);

  // Render at A(2) + 5e6: alpha 0.5 against the LATEST interval
  // (prev = end of tick 1 = (1,0), curr = end of tick 2 = (2,0)) —
  // not (0,0) -> (2,0) over the whole frame.
  snap.onRenderFrame(2 * kTickNs + 5000000);
  EXPECT_EQ(snap.alpha().raw, 32768);
  auto s = snap.sample_position(e);
  ASSERT_TRUE(s.ok());
  expectVec("catch-up midpoint", s.value(), 98304, 0);  // (1.5, 0)

  // The snapshot's own path is silent (LOG-003): the only entry is
  // the world's setup archetype_created Info.
  EXPECT_EQ(snapshotEvents(*mem), 0u);
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// sample_position rejects stale handles (warn-once) and live handles
// without a Position2D (a negative query, no warn)
// ---------------------------------------------------------------------------

TEST(Presentation, SampleRejectsStaleHandlesAndMissingComponents) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld(64);
  Entity e1 = makeEntity(w, vec(65536, 0));
  auto e2R = w.create();  // a live entity WITHOUT a Position2D
  ASSERT_TRUE(e2R.ok());
  Entity e2 = std::move(e2R).takeValue();
  Entity e3 = makeEntity(w, vec(131072, 0));
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> snap = std::move(snapR).takeValue();
  snap.onTick(1);
  snap.onRenderFrame(kTickNs);

  // The synced entity samples fine.
  auto sOk = snap.sample_position(e1);
  ASSERT_TRUE(sOk.ok());
  expectVec("synced sample", sOk.value(), 65536, 0);

  // Destroy e1: its handle is stale. sample_position fails (the
  // World::check precedent: warn-once, every build — never silent).
  ASSERT_TRUE(w.destroy(e1).ok());
  auto sStale = snap.sample_position(e1);
  ASSERT_FALSE(sStale.ok());
  EXPECT_EQ(sStale.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*mem, "stale_entity_access"), 1u);

  // A second use of the same stale handle: the warn is rate-limited
  // (no new event within the window) — the failure is still the
  // returned error.
  auto sStale2 = snap.sample_position(e1);
  ASSERT_FALSE(sStale2.ok());
  EXPECT_EQ(sStale2.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*mem, "stale_entity_access"), 1u);

  // A live handle WITHOUT a Position2D: a negative query (like
  // has<T>() reading false) — the error is returned, NO new event.
  auto sMissing = snap.sample_position(e2);
  ASSERT_FALSE(sMissing.ok());
  EXPECT_EQ(sMissing.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(snapshotEvents(*mem), 1u);  // only the stale warn above

  // A live entity that LOST its component since the last refresh:
  // also a negative query, no new event.
  ASSERT_TRUE(w.removeComponent<Position2DFpx16>(e3).ok());
  auto sLost = snap.sample_position(e3);
  ASSERT_FALSE(sLost.ok());
  EXPECT_EQ(sLost.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(snapshotEvents(*mem), 1u);

  // The stale warn's shape (subsystem "ecs", the World::check
  // precedent).
  const MemorySink::Entry* warnEntry = findEvent(*mem, "stale_entity_access");
  ASSERT_NE(warnEntry, nullptr);
  EXPECT_EQ(warnEntry->subsystem, "ecs");
  EXPECT_EQ(warnEntry->severity, laige::log::Severity::Warn);
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// The GameLoop's onTick hook drives the snapshot once per COMPLETED
// tick (the M1-HEAD-01 wiring shape): the alpha stays anchored to the
// loop's time base (startReferenceNs), a catch-up frame refreshes per
// tick, and a failed tick (a stale schedule) does not fire the hook
// ---------------------------------------------------------------------------

TEST(Presentation, HookDrivesTheSnapshotPerCompletedTick) {
  MemorySink* mem = installCaptureSink();
  gSynthClockNs = 0;
  World w = makeWorld(64);
  Entity e = makeEntity(w, vec(0, 0));
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> snap = std::move(snapR).takeValue();

  // The loop: 100 Hz on the synthetic clock, the onTick hook wired
  // to the snapshot (the engine's wiring shape).
  ASSERT_TRUE(w.registerSystem(makeDef("PMove", &fnPMove),
                               Io<Position2DFpx16, Access::Write>{})
                  .ok());
  SystemSchedule sched;
  ASSERT_TRUE(w.scheduleSystems(sched).ok());
  GameLoop::Options lopts;
  lopts.tickRateHz = kTestRateHz;
  lopts.nowNs = &synthNowNs;
  lopts.onTick = &onTickThunk;
  lopts.onTickContext = &snap;
  auto loopR = GameLoop::create(w, sched, std::move(lopts));
  ASSERT_TRUE(loopR.ok());
  GameLoop loop = std::move(loopR).takeValue();

  // Frame 0 at t = 0 establishes the start reference (zero ticks).
  ASSERT_TRUE(loop.frame().ok());
  EXPECT_EQ(loop.startReferenceNs(), 0);
  EXPECT_EQ(snap.lastTick(), 0u);

  // The engine reads the clock once per frame and passes it to both
  // the loop and the snapshot (the documented same-clock wiring).
  auto frameAndRender = [&](std::int64_t nowNs) {
    gSynthClockNs = nowNs;
    ASSERT_TRUE(loop.frame().ok());
    snap.onRenderFrame(gSynthClockNs);
    EXPECT_EQ(snap.lastTick(), loop.currentTick());
  };

  // t = 15 ms: one tick completed (the sim moved the entity to
  // (1,0)); alpha = (15ms − A(1)) / 10ms = 0.5 (the sub-anchor
  // window). The sample: prev == curr == (1,0) (tick 1 snapped).
  frameAndRender(15000000);
  EXPECT_EQ(loop.currentTick(), 1u);
  EXPECT_EQ(snap.alpha().raw, 32768);
  auto s1 = snap.sample_position(e);
  ASSERT_TRUE(s1.ok());
  expectVec("tick 1", s1.value(), 65536, 0);

  // t = 25 ms: tick 2 (the entity at (2,0)); alpha 0.5 against the
  // LATEST interval (1,0) -> (2,0).
  frameAndRender(25000000);
  EXPECT_EQ(loop.currentTick(), 2u);
  EXPECT_EQ(snap.alpha().raw, 32768);
  auto s2 = snap.sample_position(e);
  ASSERT_TRUE(s2.ok());
  expectVec("tick 2", s2.value(), 98304, 0);  // (1.5, 0)

  // CATCH-UP frame: t = 45 ms jumps 20 ms — the frame runs ticks 3
  // and 4 (both fire the hook); the entity ends at (4,0); alpha 0.5
  // against (3,0) -> (4,0).
  frameAndRender(45000000);
  EXPECT_EQ(loop.currentTick(), 4u);
  EXPECT_EQ(snap.lastTick(), 4u);
  EXPECT_EQ(snap.alpha().raw, 32768);
  auto s4 = snap.sample_position(e);
  ASSERT_TRUE(s4.ok());
  expectVec("tick 4", s4.value(), 229376, 0);  // (3.5, 0)

  // A failed tick does NOT fire the hook: registering a system after
  // scheduling stales the schedule; frame() fails (rate-limited warn),
  // the tick count freezes, and the snapshot stays at tick 4.
  ASSERT_TRUE(w.registerSystem(makeDef("PNoop", &fnPNoop)).ok());
  gSynthClockNs = 55000000;
  const Status sFail = loop.frame();
  ASSERT_FALSE(sFail.ok());
  EXPECT_EQ(sFail.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(loop.currentTick(), 4u);
  EXPECT_EQ(snap.lastTick(), 4u);
  EXPECT_EQ(countEvents(*mem, "schedule_stale"), 1u);

  // The success path up to here was silent except the failure's own
  // event (no presentation event of the loop's own — LOG-002).
  EXPECT_EQ(snapshotEvents(*mem), 1u);
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// Move transfers the tick state; the moved-from snapshot is stopped
// (no world access, no logging)
// ---------------------------------------------------------------------------

TEST(Presentation, MovedSnapshotIsStopped) {
  MemorySink* mem = installCaptureSink();
  World w = makeWorld(64);
  Entity e = makeEntity(w, vec(65536, 0));
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> a = std::move(snapR).takeValue();
  a.onTick(1);
  setPos(w, e, vec(131072, 0));
  a.onTick(2);
  a.onRenderFrame(2 * kTickNs + 5000000);  // alpha 0.5

  // Move: the state (lastTick, alpha, the record table) transfers.
  PresentationSnapshot<Fpx16_16> b = std::move(a);
  EXPECT_EQ(b.lastTick(), 2u);
  EXPECT_EQ(b.alpha().raw, 32768);
  auto sB = b.sample_position(e);
  ASSERT_TRUE(sB.ok());
  expectVec("moved-to sample", sB.value(), 98304, 0);  // (1.5, 0)

  // The source is stopped: every operation fails with Invalid-
  // Argument, NO world access, NO logging.
  EXPECT_EQ(a.lastTick(), 0u);
  EXPECT_EQ(a.alpha().raw, 0);
  auto sA = a.sample_position(e);
  ASSERT_FALSE(sA.ok());
  EXPECT_EQ(sA.error(), ErrorCode::InvalidArgument);
  // No world access, no log from the stopped snapshot (LOG-003): the
  // only entry is the world's setup archetype_created Info.
  EXPECT_EQ(snapshotEvents(*mem), 0u);

  // Move assignment: b's state moves into c; b is stopped.
  auto snapR2 = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR2.ok());
  PresentationSnapshot<Fpx16_16> c = std::move(snapR2).takeValue();
  c = std::move(b);
  EXPECT_EQ(c.lastTick(), 2u);
  auto sC = c.sample_position(e);
  ASSERT_TRUE(sC.ok());
  expectVec("move-assigned sample", sC.value(), 98304, 0);
  auto sB2 = b.sample_position(e);
  ASSERT_FALSE(sB2.ok());
  EXPECT_EQ(sB2.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(snapshotEvents(*mem), 0u);
  restoreConsoleSink();
}

// ---------------------------------------------------------------------------
// No heap allocation on the per-frame refresh/sample path (the
// step's "no per-frame heap" claim — the test-only operator-new
// counter, non-sanitizer trees; the sanitizer trees prove the same
// loop leak-free, the game_loop_tests pattern)
// ---------------------------------------------------------------------------

#if defined(LAIGE_ALLOC_COUNTER)
TEST(Presentation, HealthyFramesAllocateNothing) {
  MemorySink* mem = installCaptureSink();
  const std::uint32_t kEntities = 500;
  World w = makeWorld(4096);
  std::vector<Entity> ents;
  ents.reserve(kEntities);
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    ents.push_back(makeEntity(w, vec(fx(static_cast<std::int32_t>(i % 7)).raw,
                                       0)));
  }
  auto snapR = PresentationSnapshot<Fpx16_16>::create(
      w, 0, PresentationSnapshot<Fpx16_16>::Options{kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fpx16_16> snap = std::move(snapR).takeValue();

  // Setup is done: reset the counter, then run the 100-frame window —
  // per frame: 500 position updates (direct column writes), one
  // onTick (500 visits over the bounded archetype scan), one
  // onRenderFrame (integer ops), and 100 sample_position calls.
  laige::test::resetAllocCounter();
  for (std::uint64_t t = 1; t <= 100; ++t) {
    for (std::uint32_t i = 0; i < kEntities; ++i) {
      w.get<Position2DFpx16>(ents[i])->pos.x =
          fx(static_cast<std::int32_t>((i + t) % 7));
    }
    snap.onTick(t);
    snap.onRenderFrame(static_cast<std::int64_t>(t) * kTickNs + 5000000);
    for (std::uint32_t i = 0; i < 100; ++i) {
      auto s = snap.sample_position(ents[i]);
      if (!s.ok()) {
        ADD_FAILURE() << "sample_position failed at frame " << t;
      }
    }
  }
  const std::uint64_t allocs = laige::test::allocCounter();
  std::printf("presentation-zeroalloc frames=100 ticks=100 entities=%u "
              "allocs=%llu\n",
              kEntities, static_cast<unsigned long long>(allocs));
  EXPECT_EQ(allocs, 0u);
  // The 500 setup adds emit the world's own lifecycle events (one
  // archetype_created, the bounded archetype_grow doublings, and one
  // rate-limited churn_per_frame warn — 500 > the 256 per-frame
  // budget); the 100-frame window itself adds nothing.
  EXPECT_EQ(snapshotEvents(*mem), 0u);
  restoreConsoleSink();
}
#endif

// ---------------------------------------------------------------------------
// The fp32_pinned backend instantiates the same contract: one linear-
// interpolation check on the float alpha path (the AlphaConversion
// float division)
// ---------------------------------------------------------------------------

TEST(Presentation, Fp32BackendInterpolatesLinearly) {
  MemorySink* mem = installCaptureSink();
  auto wR = World::create(World::Options{64});
  ASSERT_TRUE(wR.ok());
  World w = std::move(wR).takeValue();
  ASSERT_TRUE(w.registerComponent<Position2DFp32>().ok());
  auto eR = w.create();
  ASSERT_TRUE(eR.ok());
  Entity e = std::move(eR).takeValue();
  ASSERT_TRUE(w
                  .addComponent<Position2DFp32>(e, Position2DFp32{{1.0f, 2.0f}})
                  .ok());
  auto snapR =
      PresentationSnapshot<Fp32Pinned>::create(w, 0,
                                               PresentationSnapshot<Fp32Pinned>::Options{
                                                   kTestRateHz});
  ASSERT_TRUE(snapR.ok());
  PresentationSnapshot<Fp32Pinned> snap = std::move(snapR).takeValue();

  // Tick 1 (snap at (1,2)); tick 2 -> (3,4).
  snap.onTick(1);
  ASSERT_TRUE(w
                  .addComponent<Position2DFp32>(e, Position2DFp32{{3.0f, 4.0f}})
                  .ok());
  snap.onTick(2);

  // Render at the midpoint: alpha exactly 0.5f (5e8 / 1e9 — exact in
  // binary32); the lerp is exactly (2.0, 3.0) (every step exact in
  // binary32 under the pinned flag set).
  snap.onRenderFrame(2 * kTickNs + 5000000);
  EXPECT_FLOAT_EQ(snap.alpha(), 0.5f);
  auto sMid = snap.sample_position(e);
  ASSERT_TRUE(sMid.ok());
  EXPECT_FLOAT_EQ(sMid.value().x, 2.0f);
  EXPECT_FLOAT_EQ(sMid.value().y, 3.0f);

  // Render exactly at A(2): alpha 0 -> prev (1,2).
  snap.onRenderFrame(2 * kTickNs);
  EXPECT_FLOAT_EQ(snap.alpha(), 0.0f);
  auto sPrev = snap.sample_position(e);
  ASSERT_TRUE(sPrev.ok());
  EXPECT_FLOAT_EQ(sPrev.value().x, 1.0f);
  EXPECT_FLOAT_EQ(sPrev.value().y, 2.0f);

  // The snapshot's own path is silent (LOG-003): the only entry is
  // the world's setup archetype_created Info.
  EXPECT_EQ(snapshotEvents(*mem), 0u);
  restoreConsoleSink();
}
