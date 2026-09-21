// laige-sim determinism mode suite (M1-DET-01).
//
// Step Verify scope (roadmap/M1-heartbeat.md, `ctest -R determinism_mode`):
//   - a trivial moving-entity sim produces BIT-IDENTICAL per-tick state
//     hashes in two consecutive runs (same build, same seed) — the
//     same-build verification ARCH-010 promises (cross-target is
//     M1-DET-04's detcheck matrix)
//   - a different seed DIVERGES (the seed is part of the replay
//     identity, ADR 0002)
//   - the per-system PRNG substreams match the Prng::deriveSubstream
//     golden contract exactly, and substreams are independent
//   - determinism disabled (World::Options::deterministic = false)
//     yields SystemContext::rng == nullptr (the documented M1
//     semantics, determinism.h)
//   - the engine selects the configured SimMath backend (the built-in
//     component and the presentation snapshot)
//   - the provisional config surface: the seed and determinism keys
//     (defaults, valid values, the rejection table)
//
// The hash convention: FNV-1a 64-bit, big-endian byte order per u64 —
// the same constants and convention as the fpx16_16 determinism KAT
// (math_fixed_tests) and the Prng golden vectors (prng_tests). The
// per-tick hash covers (tick, then per entity in each<> order: the
// handle words and the raw component words) — a pure function of the
// tick's state, no addresses, no wall clock (ARCH-010).
//
// The sim in the tests is a trivial mover: one PRNG draw per tick
// (fixed position in the system's run: before the iteration — the
// call order is the replay state) nudges the velocity; each entity
// then integrates position += velocity through SimMathFpx16 ops only
// (G-R8: no raw FP, the SimMath op surface, ADR 0002).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/json.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/result.h"
#include "laige/sim/engine.h"
#include "laige/sim/presentation.h"
#include "laige/sim/system.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the
// build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "determinism_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "determinism_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "determinism_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test component (global scope: LAIGE_COMPONENT and
// LAIGE_DETERMINISM_SAFE specialize traits at global scope)
// ---------------------------------------------------------------------------

// The test velocity: SimMath-registered scalars (fpx16_16) — the
// determinism-safe storage the G-R8 trait accepts (determinism.h).
struct DetVel {
  laige::fpx16_16 vx{};
  laige::fpx16_16 vy{};
};
LAIGE_COMPONENT(DetVel);
// M1-DET-01 (G-R8): the member list IS the type's storage — verified
// by the trait at the mark site.
LAIGE_DETERMINISM_SAFE(DetVel, laige::fpx16_16, laige::fpx16_16);

// ---------------------------------------------------------------------------
// The trivial mover systems (FR-1.3: plain functions, no class)
// ---------------------------------------------------------------------------

namespace {

// Test plumbing: the PRNG draws the systems make (written only on the
// owner thread — PRD §10.2). A fixed array: no allocation in the tick
// path (PERF-003, the test's own zero-alloc discipline).
constexpr std::uint32_t kDetMaxDraws = 512;
std::uint32_t gDetMoveDraws[kDetMaxDraws];
std::size_t gDetMoveDrawCount = 0;
std::uint32_t gDetTwoDraws[kDetMaxDraws];
std::size_t gDetTwoDrawCount = 0;
bool gDetRngWasNull = false;

void resetDetPlumbing() {
  gDetMoveDrawCount = 0;
  gDetTwoDrawCount = 0;
  gDetRngWasNull = false;
}

}  // namespace

// The mover: one PRNG draw per tick (fixed position: before the
// iteration — the call order is the replay state), then integrates
// position += velocity for every entity through SimMathFpx16 ops only
// (G-R8). The draw nudges the velocity: the ONLY randomness → state
// edge in the sim.
LAIGE_SYSTEM(DetMove, 1)
void DetMove(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  std::uint32_t nudge = 0;
  if (ctx.rng != nullptr) {
    nudge = ctx.rng->next_range(0, 5);
    if (gDetMoveDrawCount < kDetMaxDraws) {
      gDetMoveDraws[gDetMoveDrawCount] = nudge;
    }
    ++gDetMoveDrawCount;
  }
  const laige::fpx16_16 step =
      laige::fpx16_16::fromInt32(static_cast<std::int32_t>(nudge));
  static_cast<void>(ctx.each<laige::Position2DFpx16, DetVel>(
      [step](laige::Entity e, laige::Position2DFpx16& pos,
             DetVel& vel) {
        static_cast<void>(e);
        using M = laige::sim::SimMath<laige::sim::Fpx16_16>;
        const M::Vec2 velVec{vel.vx, vel.vy};
        pos.pos = M::add(pos.pos, velVec);  // pos += vel (SimMath ops)
        vel.vx = laige::fpx16_16::add(vel.vx, step);  // the nudge
      },
      laige::Write{}, laige::Write{}));
}

// The second mover (the substream-independence test): draws its OWN
// substream once per tick; touches no state.
LAIGE_SYSTEM(DetTwo, 1)
void DetTwo(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  if (ctx.rng != nullptr) {
    if (gDetTwoDrawCount < kDetMaxDraws) {
      gDetTwoDraws[gDetTwoDrawCount] = ctx.rng->next_range(0, 5);
    }
    ++gDetTwoDrawCount;
  }
  static_cast<void>(ctx.each<laige::Position2DFpx16>(
      [](laige::Entity e, const laige::Position2DFpx16& p) {
        static_cast<void>(e);
        static_cast<void>(p);
      },
      laige::Read{}));
}

// The RNG probe (the determinism-disabled test): records that the
// context's rng was nullptr (no random source, the documented M1
// semantics).
LAIGE_SYSTEM(DetRngProbe, 1)
void DetRngProbe(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  if (ctx.rng == nullptr) gDetRngWasNull = true;
  static_cast<void>(ctx.each<laige::Position2DFpx16>(
      [](laige::Entity e, const laige::Position2DFpx16& p) {
        static_cast<void>(e);
        static_cast<void>(p);
      },
      laige::Read{}));
}

namespace {

// ---------------------------------------------------------------------------
// The state hash (ARCH-010: no addresses, no wall clock)
// ---------------------------------------------------------------------------

// FNV-1a 64-bit, big-endian byte order per u64 (endianness-independent
// — the same convention as the fpx16_16 determinism KAT and the Prng
// golden vectors).
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

// One tick's state hash: (tick, then per entity in each<> order — the
// M1-ECS-05 iteration order: the handle words and the raw component
// words). A pure function of the tick's state.
std::uint64_t hashTick(laige::World& world, std::uint64_t tick) {
  std::vector<std::uint64_t> words;
  words.push_back(tick);
  static_cast<void>(world.each<laige::Position2DFpx16, DetVel>(
      [&](laige::Entity e, const laige::Position2DFpx16& pos,
          const DetVel& vel) {
        words.push_back(e.id);
        words.push_back(e.generation);
        words.push_back(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(pos.pos.x.raw)));
        words.push_back(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(pos.pos.y.raw)));
        words.push_back(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(vel.vx.raw)));
        words.push_back(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(vel.vy.raw)));
      },
      laige::Read{}, laige::Read{}));
  return fnv1a64(words.data(), words.size());
}

// ---------------------------------------------------------------------------
// The fixture world: two entities, the mover system (plus optionally a
// second drawing system)
// ---------------------------------------------------------------------------

laige::World makeDetWorld(std::uint64_t seed, bool deterministic,
                          bool withSecondSystem) {
  laige::World::Options opts;
  opts.capacity = 64;
  opts.seed = seed;
  opts.deterministic = deterministic;
  laige::Result<laige::World, laige::ErrorCode> w =
      laige::World::create(opts);
  if (!w.ok()) {
    ADD_FAILURE() << "World::create failed";
    abort();
  }
  laige::World world = std::move(w).takeValue();
  const laige::Result<laige::ComponentTypeId, laige::ErrorCode> p =
      world.registerComponent<laige::Position2DFpx16>();
  const laige::Result<laige::ComponentTypeId, laige::ErrorCode> v =
      world.registerComponent<DetVel>();
  const laige::Result<laige::SystemId, laige::ErrorCode> s =
      world.registerSystem(
          DetMove_Def,
          laige::Io<laige::Position2DFpx16, laige::Access::Write>{},
          laige::Io<DetVel, laige::Access::Write>{});
  if (!p.ok() || !v.ok() || !s.ok()) {
    ADD_FAILURE() << "fixture registration failed";
    abort();
  }
  if (withSecondSystem) {
    const laige::Result<laige::SystemId, laige::ErrorCode> s2 =
        world.registerSystem(
            DetTwo_Def,
            laige::Io<laige::Position2DFpx16, laige::Access::Read>{});
    if (!s2.ok()) {
      ADD_FAILURE() << "second system registration failed";
      abort();
    }
  }
  // Two entities (fixed initial state — the sim's initial condition):
  // e1 at (3, -2) with velocity (1, 0); e2 at (-1, 4) with velocity
  // (0, 1).
  auto e1r = world.create();
  auto e2r = world.create();
  if (!e1r.ok() || !e2r.ok()) {
    ADD_FAILURE() << "fixture entity creation failed";
    abort();
  }
  using Pos = laige::Position2DFpx16;
  using M = laige::sim::SimMath<laige::sim::Fpx16_16>;
  static_cast<void>(world.addComponent(
      e1r.value(), Pos{M::Vec2{laige::fpx16_16::fromInt32(3),
                                laige::fpx16_16::fromInt32(-2)}}));
  static_cast<void>(
      world.addComponent(e1r.value(), DetVel{laige::fpx16_16::fromInt32(1),
                                             laige::fpx16_16::fromInt32(0)}));
  static_cast<void>(world.addComponent(
      e2r.value(), Pos{M::Vec2{laige::fpx16_16::fromInt32(-1),
                                laige::fpx16_16::fromInt32(4)}}));
  static_cast<void>(
      world.addComponent(e2r.value(), DetVel{laige::fpx16_16::fromInt32(0),
                                             laige::fpx16_16::fromInt32(1)}));
  return world;
}

// Runs n ticks (the schedule once, then runSystems per tick) and
// returns the per-tick state hashes.
std::vector<std::uint64_t> runTicks(laige::World& world, std::uint64_t n) {
  laige::SystemSchedule sched;
  if (!world.scheduleSystems(sched).ok()) {
    ADD_FAILURE() << "scheduleSystems failed";
    abort();
  }
  std::vector<std::uint64_t> hashes;
  hashes.reserve(n);
  for (std::uint64_t t = 1; t <= n; ++t) {
    if (!world.runSystems(sched).ok()) {
      ADD_FAILURE() << "runSystems failed at tick " << t;
      abort();
    }
    hashes.push_back(hashTick(world, t));
  }
  return hashes;
}

}  // namespace

// ---------------------------------------------------------------------------
// DeterminismMode: the promised-scope verification (ARCH-010)
// ---------------------------------------------------------------------------

TEST(DeterminismMode, SameSeedIdenticalTickStreams) {
  // Two consecutive runs (same build, same process): the same seed
  // must produce bit-identical per-tick state hashes for 256 ticks.
  resetDetPlumbing();
  laige::World a = makeDetWorld(42, true, false);
  std::vector<std::uint64_t> streamA = runTicks(a, 256);

  resetDetPlumbing();
  laige::World b = makeDetWorld(42, true, false);
  std::vector<std::uint64_t> streamB = runTicks(b, 256);

  ASSERT_EQ(streamA.size(), streamB.size());
  for (std::size_t t = 0; t < streamA.size(); ++t) {
    if (streamA[t] != streamB[t]) {
      ADD_FAILURE() << "tick " << t << " hashes differ: "
                    << "0x" << std::hex << streamA[t] << " vs 0x"
                    << streamB[t];
      return;
    }
  }
  // The stream is non-trivial: the state actually moves (the first and
  // last tick differ).
  EXPECT_NE(streamA.front(), streamA.back());
  // Machine-greppable summary (the CI log of this suite).
  std::printf("determinism-tick-stream same-seed ticks=256 "
              "first=0x%016llx last=0x%016llx\n",
              static_cast<unsigned long long>(streamA.front()),
              static_cast<unsigned long long>(streamA.back()));
}

TEST(DeterminismMode, DifferentSeedDiverges) {
  // The seed is part of the replay identity (ADR 0002): a different
  // seed must change the per-tick state (the substreams differ).
  resetDetPlumbing();
  laige::World a = makeDetWorld(42, true, false);
  std::vector<std::uint64_t> streamA = runTicks(a, 256);

  resetDetPlumbing();
  laige::World b = makeDetWorld(43, true, false);
  std::vector<std::uint64_t> streamB = runTicks(b, 256);

  ASSERT_EQ(streamA.size(), streamB.size());
  bool anyDiffer = false;
  for (std::size_t t = 0; t < streamA.size(); ++t) {
    if (streamA[t] != streamB[t]) {
      anyDiffer = true;
      break;
    }
  }
  EXPECT_TRUE(anyDiffer)
      << "seeds 42 and 43 produced identical streams — the seed is not "
         "reaching the sim (the PRNG substream wiring)";
}

TEST(DeterminismMode, SubstreamsMatchPrngDerivation) {
  // The system's substream IS Prng::deriveSubstream(seed, systemId):
  // the draws the system made must equal the draws of an independently
  // constructed Prng with the same derivation (the golden
  // cross-check).
  resetDetPlumbing();
  laige::World world = makeDetWorld(7, true, false);
  runTicks(world, 64);

  ASSERT_EQ(gDetMoveDrawCount, 64u);
  laige::Prng golden = laige::Prng::deriveSubstream(7, 1);  // system id 1
  for (std::size_t t = 0; t < 64; ++t) {
    const std::uint32_t expected = golden.next_range(0, 5);
    if (gDetMoveDraws[t] != expected) {
      ADD_FAILURE() << "tick " << t << ": system draw " << gDetMoveDraws[t]
                    << " != deriveSubstream(7,1) draw " << expected;
      return;
    }
  }
}

TEST(DeterminismMode, SubstreamsAreIndependent) {
  // Two systems draw from DIFFERENT substreams (ids 1 and 2): their
  // draw sequences must differ (independence — substreams never
  // interleave, PRD §10.3).
  resetDetPlumbing();
  laige::World world = makeDetWorld(99, true, true);
  runTicks(world, 128);

  ASSERT_EQ(gDetMoveDrawCount, 128u);
  ASSERT_EQ(gDetTwoDrawCount, 128u);
  // Golden cross-checks for BOTH systems.
  laige::Prng g1 = laige::Prng::deriveSubstream(99, 1);
  laige::Prng g2 = laige::Prng::deriveSubstream(99, 2);
  for (std::size_t t = 0; t < 128; ++t) {
    EXPECT_EQ(gDetMoveDraws[t], g1.next_range(0, 5)) << "tick " << t;
    EXPECT_EQ(gDetTwoDraws[t], g2.next_range(0, 5)) << "tick " << t;
  }
  // Independence: the two streams differ (128 draws from different
  // substreams — a match is a vanishingly small coincidence; a full
  // match would be a derivation bug).
  bool anyDiffer = false;
  for (std::size_t t = 0; t < 128; ++t) {
    if (gDetMoveDraws[t] != gDetTwoDraws[t]) {
      anyDiffer = true;
      break;
    }
  }
  EXPECT_TRUE(anyDiffer)
      << "substreams 1 and 2 produced identical draw sequences";
}

TEST(DeterminismMode, DeterminismDisabledHasNoSubstream) {
  // World::Options::deterministic = false: no substreams are created —
  // SystemContext::rng is nullptr (the documented M1 semantics,
  // determinism.h). The system draws nothing.
  resetDetPlumbing();
  laige::World::Options opts;
  opts.capacity = 64;
  opts.deterministic = false;
  laige::Result<laige::World, laige::ErrorCode> wr =
      laige::World::create(opts);
  if (!wr.ok()) {
    ADD_FAILURE() << "World::create failed";
    abort();
  }
  laige::World world = std::move(wr).takeValue();
  static_cast<void>(world.registerComponent<laige::Position2DFpx16>());
  static_cast<void>(world.registerSystem(
      DetRngProbe_Def,
      laige::Io<laige::Position2DFpx16, laige::Access::Read>{}));
  static_cast<void>(world.create());
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());
  for (int t = 0; t < 8; ++t) {
    ASSERT_TRUE(world.runSystems(sched).ok());
  }
  EXPECT_TRUE(gDetRngWasNull)
      << "determinism disabled but the system saw a non-null rng";
}

// ---------------------------------------------------------------------------
// DeterminismEngine: the backend selection (ADR 0002)
// ---------------------------------------------------------------------------

TEST(DeterminismEngine, DefaultBackendIsFixedPoint) {
  laige::EngineConfig config;
  config.entityCapacity = 16;
  // Default: FixedPoint16_16 — the engine registered Position2DFpx16.
  laige::Engine engine =
      std::move(laige::Engine::create(config)).takeValue();
  laige::World* world = engine.world();
  ASSERT_NE(world, nullptr);
  // Duplicate built-in: rejected (one alias per world).
  EXPECT_FALSE(
      world->registerComponent<laige::Position2DFpx16>().ok());
  // The other alias is available to the game.
  EXPECT_TRUE(world->registerComponent<laige::Position2DFp32>().ok());
  // A bounded run completes (the fpx16_16 snapshot drives it).
  EXPECT_TRUE(engine.run_headless(30, 8).ok());
  engine.shutdown();
}

TEST(DeterminismEngine, ConfiguredFloatPinnedBackend) {
  laige::EngineConfig config;
  config.entityCapacity = 16;
  config.determinism.math = laige::SimMathBackend::FloatPinned32;
  laige::Result<laige::Engine, laige::ErrorCode> r =
      laige::Engine::create(config);
  ASSERT_TRUE(r.ok());
  laige::Engine engine = std::move(r).takeValue();
  laige::World* world = engine.world();
  ASSERT_NE(world, nullptr);
  // The engine registered the FP32 built-in: the duplicate is rejected,
  // the other alias is available.
  EXPECT_FALSE(
      world->registerComponent<laige::Position2DFp32>().ok());
  EXPECT_TRUE(world->registerComponent<laige::Position2DFpx16>().ok());
  // The config echo carries the selection.
  EXPECT_EQ(engine.config().determinism.math,
            laige::SimMathBackend::FloatPinned32);
  // A bounded run completes (the fp32_pinned snapshot drives it).
  EXPECT_TRUE(engine.run_headless(30, 8).ok());
  engine.shutdown();
}

// ---------------------------------------------------------------------------
// DeterminismConfigParse: the seed and determinism config keys (the
// provisional M1-HEAD-01 surface; M1-CFG-01 owns the final schema)
// ---------------------------------------------------------------------------

namespace {

// One log event captured from the facade (the engine_tests.cpp
// MemorySink pattern — Warn+ only, rate limiting off).
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

}  // namespace

TEST(DeterminismConfigParse, DefaultsAreDeterministicFixedPoint) {
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc(R"({"version":1})")).value();
  EXPECT_EQ(config.seed, 0u);
  EXPECT_TRUE(config.determinism.enabled);
  EXPECT_EQ(config.determinism.math, laige::SimMathBackend::FixedPoint16_16);
}

TEST(DeterminismConfigParse, SeedAndDeterminismValid) {
  const laige::EngineConfig config = laige::parseEngineConfig(parseDoc(
      R"({"version":1,"seed":123,"determinism":{"enabled":false,"math":"float_pinned_32"}})"))
      .value();
  EXPECT_EQ(config.seed, 123u);
  EXPECT_FALSE(config.determinism.enabled);
  EXPECT_EQ(config.determinism.math, laige::SimMathBackend::FloatPinned32);
}

TEST(DeterminismConfigParse, SeedAtTheJsonBoundIsExact) {
  // 2^53 is the largest exact-integer double (ADR 0003): accepted.
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc(R"({"version":1,"seed":9007199254740992})"))
          .value();
  EXPECT_EQ(config.seed, 9007199254740992ull);
}

TEST(DeterminismConfigParse, SeedRejections) {
  MemorySink* sink = installCaptureSink();
  // 2^53+2 (the smallest representable value ABOVE the exact-double
  // bound — 2^53+1 itself rounds to 2^53 in the ADR 0003 double and
  // is indistinguishable from the bound, so it is accepted as 2^53),
  // 1.5 (non-integer), -1 (negative), and a non-number: all
  // rejected, one warn each, first-failure-wins.
  const char* badDocs[] = {
      R"({"version":1,"seed":9007199254740994})",  // 2^53 + 2
      R"({"version":1,"seed":1.5})",
      R"({"version":1,"seed":-1})",
      R"({"version":1,"seed":"x"})",
  };
  for (const char* doc : badDocs) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> r =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(r.isError()) << doc;
    EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument) << doc;
  }
  EXPECT_EQ(countEvents(*sink, "seed_invalid"), 4u);
  restoreLogger();
}

TEST(DeterminismConfigParse, DeterminismRejections) {
  MemorySink* sink = installCaptureSink();
  const char* badDocs[] = {
      R"({"version":1,"determinism":true})",           // not an object
      R"({"version":1,"determinism":{"enabled":"yes"}})",  // not a bool
      R"({"version":1,"determinism":{"math":"fpx16"}})",      // unknown backend id
      R"({"version":1,"determinism":{"math":3}})",      // not a string
  };
  for (const char* doc : badDocs) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> r =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(r.isError()) << doc;
    EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument) << doc;
  }
  EXPECT_EQ(countEvents(*sink, "determinism_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "determinism_enabled_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "determinism_math_invalid"), 2u);
  restoreLogger();
}

TEST(DeterminismConfigParse, UnknownNestedKeyIsForwardCompat) {
  // An unknown key inside the determinism block: WARNED (one
  // config/unknown_key) and ignored — the M1-CFG-01 rule.
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> r =
      laige::parseEngineConfig(
          parseDoc(R"({"version":1,"determinism":{"unknown_future_key":1}})"));
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(countEvents(*sink, "unknown_key"), 1u);
  restoreLogger();
}

TEST(DeterminismConfigParse, FirstFailureWinsAcrossBlocks) {
  // A bad seed AND a bad determinism block: exactly ONE rejection
  // (the seed — document order), one warn.
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> r =
      laige::parseEngineConfig(
          parseDoc(R"({"version":1,"seed":-1,"determinism":{"math":"nope"}})"));
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "seed_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "determinism_math_invalid"), 0u);
  restoreLogger();
}
