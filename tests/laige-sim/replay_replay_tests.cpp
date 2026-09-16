// laige-sim deterministic state hash + replay execution suite
// (M1-DET-03).
//
// Step Verify scope (roadmap/M1-heartbeat.md, `ctest -R
// replay_replay`):
//   - `world.stateHash(tick)`: the deterministic 64-bit hash of the
//     world's LIVE sim state — exactly the tick, the live handles, the
//     archetype assignment, every live component's bytes, and every
//     system's PRNG substream state — and a PURE function of the state
//     (the free list, dead-slot generations, empty archetypes, the
//     registry/capacity, presentation, timing, and guardrail state are
//     excluded; convergent worlds hash identically)
//   - the replay execution half (`laige::runReplay`): the 500-tick
//     recorded scenario replays to the identical per-tick hash stream
//     (the step's integration test); a perturbed second run diverges
//     first at tick 7 (the first-divergence report the laige-replay
//     tool and the baselines compare); an identity mismatch is a
//     rejected replay (InvalidArgument + the structured
//     replay/identity_mismatch warn, never a silent divergence); a
//     log recorded with determinism disabled is not replayable
//   - the engine round trip: a recorded headless engine run replays to
//     the same per-tick hashes (debug builds — the recorder is
//     debug-only, M1-DET-02)
//
// Suite names: StateHash.* and DetReplay.* — deliberately NOT
// Replay*-prefixed (the replay_record CTest entry selects exactly the
// Replay* suites from this shared executable).
//
// The sim in the tests: a trivial mover (RpMove) — one PRNG draw per
// tick (fixed position: before the iteration — the call order is the
// replay state) nudges the velocity; each entity then integrates
// position += velocity through SimMathFpx16 ops only (G-R8, ADR
// 0002). The perturbation probe (RpPerturb) fires once, at a
// test-assigned tick, when armed — the detcheck synthetic-perturbed
// failure-path stand-in.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
              "replay_replay_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "replay_replay_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "replay_replay_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test components (global scope: LAIGE_COMPONENT and
// LAIGE_DETERMINISM_SAFE specialize traits at global scope; distinct
// from the other suites' types in the shared executable)
// ---------------------------------------------------------------------------

struct RpPos {
  laige::fpx16_16 x{};
  laige::fpx16_16 y{};
};
LAIGE_COMPONENT(RpPos);
LAIGE_DETERMINISM_SAFE(RpPos, laige::fpx16_16, laige::fpx16_16);

struct RpVel {
  laige::fpx16_16 vx{};
  laige::fpx16_16 vy{};
};
LAIGE_COMPONENT(RpVel);
LAIGE_DETERMINISM_SAFE(RpVel, laige::fpx16_16, laige::fpx16_16);

// A third component (the identity-mismatch test's extra registration).
struct RpExtra {
  laige::fpx16_16 v{};
};
LAIGE_COMPONENT(RpExtra);
LAIGE_DETERMINISM_SAFE(RpExtra, laige::fpx16_16);

// ---------------------------------------------------------------------------
// The trivial mover + the perturbation probe (FR-1.3: plain functions)
// ---------------------------------------------------------------------------

namespace {

// Test plumbing for the perturbation scenario (the owner thread writes
// it only — PRD §10.2).
std::uint64_t gRpTickCounter = 0;  // completed ticks (RpMove advances it)
std::uint64_t gRpPerturbTick = 0;  // 0 = never perturb

}  // namespace

// The mover: one PRNG draw per tick (fixed position: before the
// iteration), then integrates position += velocity for every entity
// through SimMathFpx16 ops only (G-R8). The draw nudges the velocity —
// the only randomness -> state edge in the sim.
LAIGE_SYSTEM(RpMove, 1)
void RpMove(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  ++gRpTickCounter;
  std::uint32_t nudge = 0;
  if (ctx.rng != nullptr) {
    nudge = ctx.rng->next_range(0, 5);
  }
  const laige::fpx16_16 step =
      laige::fpx16_16::fromInt32(static_cast<std::int32_t>(nudge));
  static_cast<void>(ctx.each<RpPos, RpVel>(
      [step](laige::Entity e, RpPos& p, RpVel& v) {
        static_cast<void>(e);
        p.x = laige::fpx16_16::add(p.x, v.vx);
        p.y = laige::fpx16_16::add(p.y, v.vy);
        v.vx = laige::fpx16_16::add(v.vx, step);
      },
      laige::Write{}, laige::Write{}));
}

// The perturbation probe: when armed (gRpPerturbTick != 0) and the
// mover has completed exactly that tick, adds 1 to every entity's
// RpExtra value — the state perturbation that makes the second world
// diverge (the detcheck synthetic-perturbed fixture's semantics).
// Registered in BOTH worlds (the registrations are identical — the
// identity is unchanged); only the global arm differs. It writes
// RpExtra (not RpPos) so the schedule keeps one writer per component
// (the scheduler rejects a double writer — M1-SYS-02).
LAIGE_SYSTEM(RpPerturb, 1)
void RpPerturb(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  if (gRpPerturbTick != 0 && gRpTickCounter == gRpPerturbTick) {
    static_cast<void>(ctx.each<RpExtra>(
        [](laige::Entity e, RpExtra& p) {
          static_cast<void>(e);
          p.v = laige::fpx16_16::add(p.v, laige::fpx16_16::fromInt32(1));
        },
        laige::Write{}));
  }
}

// A system that draws nothing (the "PRNG state enters the hash even
// when undrawn" test).
LAIGE_SYSTEM(RpIdle, 1)
void RpIdle(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// An independent FNV-1a 64 over u64 words, big-endian byte order per
// word (the house convention — a test-local implementation, kept
// independent of the engine's stateHash path so the KAT checks one
// against the other).
std::uint64_t fnv1a64Words(const std::uint64_t* w, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis
  for (std::size_t i = 0; i < n; ++i) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (w[i] >> shift) & 0xFFull;
      h *= 0x100000001b3ull;  // FNV prime
    }
  }
  return h;
}

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

// The fixture world: capacity 64, RpPos/RpVel registered (ids 1, 2),
// RpMove (id 1) registered, and — when withPerturb — the RpExtra
// component (id 3) + RpPerturb (id 2) with both entities carrying
// RpExtra(0). Two entities in the fixed initial state (e1 at (3, -2),
// velocity (1, 0); e2 at (-1, 4), velocity (0, 1)).
laige::World makeRpWorld(std::uint64_t seed, bool withPerturb,
                         bool deterministic = true) {
  laige::World world = makeWorld(64, seed, deterministic);
  if (!world.registerComponent<RpPos>().ok() ||
      !world.registerComponent<RpVel>().ok()) {
    ADD_FAILURE() << "component registration failed";
    abort();
  }
  if (withPerturb && !world.registerComponent<RpExtra>().ok()) {
    ADD_FAILURE() << "RpExtra registration failed";
    abort();
  }
  if (!world.registerSystem(
          RpMove_Def,
          laige::Io<RpPos, laige::Access::Write>{},
          laige::Io<RpVel, laige::Access::Write>{}).ok()) {
    ADD_FAILURE() << "mover registration failed";
    abort();
  }
  if (withPerturb &&
      !world.registerSystem(
          RpPerturb_Def,
          laige::Io<RpExtra, laige::Access::Write>{}).ok()) {
    ADD_FAILURE() << "perturb registration failed";
    abort();
  }
  const laige::Result<laige::Entity, laige::ErrorCode> e1 = world.create();
  const laige::Result<laige::Entity, laige::ErrorCode> e2 = world.create();
  if (!e1.ok() || !e2.ok()) {
    ADD_FAILURE() << "entity creation failed";
    abort();
  }
  static_cast<void>(world.addComponent(
      e1.value(), RpPos{laige::fpx16_16::fromInt32(3),
                        laige::fpx16_16::fromInt32(-2)}));
  static_cast<void>(world.addComponent(
      e2.value(), RpPos{laige::fpx16_16::fromInt32(-1),
                        laige::fpx16_16::fromInt32(4)}));
  static_cast<void>(world.addComponent(
      e1.value(), RpVel{laige::fpx16_16::fromInt32(1),
                        laige::fpx16_16::fromInt32(0)}));
  static_cast<void>(world.addComponent(
      e2.value(), RpVel{laige::fpx16_16::fromInt32(0),
                        laige::fpx16_16::fromInt32(1)}));
  if (withPerturb) {
    static_cast<void>(world.addComponent(
        e1.value(), RpExtra{laige::fpx16_16::fromInt32(0)}));
    static_cast<void>(world.addComponent(
        e2.value(), RpExtra{laige::fpx16_16::fromInt32(0)}));
  }
  return world;
}

// Runs `ticks` ticks on `world` (one beginFrame() + one runSystems per
// tick — the replay driver's frame discipline) and returns the
// per-tick state hashes (tick 0 first).
std::vector<std::uint64_t> runTicks(laige::World& world, std::uint64_t ticks) {
  laige::SystemSchedule sched;
  if (!world.scheduleSystems(sched).ok()) {
    ADD_FAILURE() << "scheduleSystems failed";
    abort();
  }
  std::vector<std::uint64_t> hashes;
  hashes.reserve(ticks + 1);
  hashes.push_back(world.stateHash(0));
  for (std::uint64_t t = 1; t <= ticks; ++t) {
    world.beginFrame();
    if (!world.runSystems(sched).ok()) {
      ADD_FAILURE() << "runSystems failed at tick " << t;
      abort();
    }
    hashes.push_back(world.stateHash(t));
  }
  return hashes;
}

// Records `ticks` ticks of `world` to a replay log at `path` (one
// zero-length frame per completed tick, the M1-DET-02 shape) and
// returns the per-tick state hashes (tick 0 first).
struct RecordResult {
  std::string path;
  std::vector<std::uint64_t> hashes;
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
  std::vector<std::uint64_t> hashes;
  hashes.reserve(ticks + 1);
  hashes.push_back(world.stateHash(0));
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
    hashes.push_back(world.stateHash(t));
  }
  if (!rec.finish().ok()) {
    ADD_FAILURE() << "ReplayRecorder::finish failed";
    abort();
  }
  return RecordResult{path, std::move(hashes)};
}

// A temp path under gtest's temp dir (the replay_record_tests pattern).
std::string tempPath(const char* name) {
  return std::string(::testing::TempDir()) + name;
}

// ---------------------------------------------------------------------------
// The log event capture (the determinism_tests MemorySink pattern —
// Warn+ only, rate limiting off)
// ---------------------------------------------------------------------------

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

// The shared test config (the 500-tick scenario's identity).
laige::EngineConfig makeConfig(std::uint64_t seed) {
  laige::EngineConfig config;
  config.tickRateHz = 60;
  config.entityCapacity = 64;
  config.churnPerFrameBudget = 256;
  config.seed = seed;
  return config;
}

}  // namespace

// ---------------------------------------------------------------------------
// The state hash (M1-DET-03; entity.h "Deterministic state hash")
// ---------------------------------------------------------------------------

TEST(StateHash, EmptyWorldKAT) {
  // The canonical encoding of an empty world is the word stream
  // [tick, liveCount] — checked against the independent FNV
  // implementation (the KAT: a change to the encoding fails here).
  laige::World w0 = makeWorld(0, 0);
  const std::uint64_t w00[] = {0, 0};
  const std::uint64_t w07[] = {7, 0};
  EXPECT_EQ(w0.stateHash(0), fnv1a64Words(w00, 2));
  EXPECT_EQ(w0.stateHash(7), fnv1a64Words(w07, 2));
}

TEST(StateHash, CapacityIsNotState) {
  // The world capacity is config, not state (entity.h scope): two
  // empty worlds of different capacity hash identically.
  laige::World small = makeWorld(0, 5);
  laige::World large = makeWorld(8, 5);
  EXPECT_EQ(small.stateHash(0), large.stateHash(0));
  EXPECT_EQ(small.stateHash(3), large.stateHash(3));
}

TEST(StateHash, TickAndHandlesEnterTheHash) {
  laige::World w = makeWorld(4, 9);
  static_cast<void>(w.registerComponent<RpPos>());
  const laige::Result<laige::Entity, laige::ErrorCode> e1 = w.create();
  ASSERT_TRUE(e1.ok());
  static_cast<void>(w.addComponent(e1.value(), RpPos{
                                     laige::fpx16_16::fromInt32(5),
                                     laige::fpx16_16::fromInt32(5)}));
  // The tick counter enters.
  EXPECT_NE(w.stateHash(5), w.stateHash(6));
  // The slot id enters: a one-entity world of capacity 2 lives in
  // slot 1, not slot 3.
  laige::World w2 = makeWorld(2, 9);
  static_cast<void>(w2.registerComponent<RpPos>());
  const laige::Result<laige::Entity, laige::ErrorCode> e2 = w2.create();
  ASSERT_TRUE(e2.ok());
  static_cast<void>(w2.addComponent(e2.value(), RpPos{
                                     laige::fpx16_16::fromInt32(5),
                                     laige::fpx16_16::fromInt32(5)}));
  EXPECT_NE(w.stateHash(0), w2.stateHash(0));
  // The generation enters: destroy + recreate bumps it (same slot).
  const std::uint64_t before = w.stateHash(0);
  ASSERT_TRUE(w.destroy(e1.value()).ok());
  const laige::Result<laige::Entity, laige::ErrorCode> e3 = w.create();
  ASSERT_TRUE(e3.ok());
  EXPECT_NE(before, w.stateHash(0));
  static_cast<void>(e3.value());
}

TEST(StateHash, ComponentValuesEnterTheHash) {
  laige::World w = makeWorld(4, 9);
  static_cast<void>(w.registerComponent<RpPos>());
  const laige::Result<laige::Entity, laige::ErrorCode> e = w.create();
  ASSERT_TRUE(e.ok());
  static_cast<void>(
      w.addComponent(e.value(),
                     RpPos{laige::fpx16_16::fromInt32(1),
                           laige::fpx16_16::fromInt32(1)}));
  const std::uint64_t h1 = w.stateHash(0);
  static_cast<void>(w.addComponent(
      e.value(), RpPos{laige::fpx16_16::fromInt32(2),
                       laige::fpx16_16::fromInt32(1)}));
  EXPECT_NE(h1, w.stateHash(0));
  // The same final value reached by two histories hashes the same
  // (the hash is a function of the state, not the path): overwrite
  // back to (1,1) in place (the archetype is unchanged).
  static_cast<void>(w.addComponent(
      e.value(), RpPos{laige::fpx16_16::fromInt32(1),
                       laige::fpx16_16::fromInt32(1)}));
  EXPECT_EQ(h1, w.stateHash(0));
}

TEST(StateHash, ArchetypeAssignmentEntersTheHash) {
  // The same bytes in two different component sets -> different hashes
  // (the signature words enter the stream).
  laige::World a = makeWorld(4, 9);
  laige::World b = makeWorld(4, 9);
  static_cast<void>(a.registerComponent<RpPos>());
  static_cast<void>(a.registerComponent<RpVel>());
  static_cast<void>(b.registerComponent<RpPos>());
  static_cast<void>(b.registerComponent<RpVel>());
  auto ea = a.create();
  auto eb = b.create();
  ASSERT_TRUE(ea.ok() && eb.ok());
  static_cast<void>(a.addComponent(
      ea.value(), RpPos{laige::fpx16_16::fromInt32(7),
                        laige::fpx16_16::fromInt32(7)}));
  static_cast<void>(b.addComponent(
      eb.value(), RpVel{laige::fpx16_16::fromInt32(7),
                        laige::fpx16_16::fromInt32(7)}));
  EXPECT_NE(a.stateHash(0), b.stateHash(0));
  // Adding a component (a different set) changes the hash too.
  static_cast<void>(a.addComponent(
      ea.value(), RpVel{laige::fpx16_16::fromInt32(0),
                        laige::fpx16_16::fromInt32(0)}));
  EXPECT_NE(a.stateHash(0), b.stateHash(0));
}

TEST(StateHash, ConvergentWorldsHashIdentically) {
  // Sequence A (direct): create s3, add RpPos(5), destroy, create s3
  // (gen 2), add RpPos(7).
  laige::World a = makeWorld(4, 0);
  static_cast<void>(a.registerComponent<RpPos>());
  auto a1 = a.create();
  ASSERT_TRUE(a1.ok());
  static_cast<void>(a.addComponent(
      a1.value(), RpPos{laige::fpx16_16::fromInt32(5),
                        laige::fpx16_16::fromInt32(5)}));
  ASSERT_TRUE(a.destroy(a1.value()).ok());
  auto a2 = a.create();
  ASSERT_TRUE(a2.ok());
  static_cast<void>(a.addComponent(
      a2.value(), RpPos{laige::fpx16_16::fromInt32(7),
                        laige::fpx16_16::fromInt32(7)}));
  // Sequence B (interleaved): the same creates/destroys in a different
  // order — B also creates (and destroys) a scratch entity, so slot 2
  // ends at generation 2 in B but generation 1 in A (a dead-slot
  // generation difference the hash must NOT see).
  laige::World b = makeWorld(4, 0);
  static_cast<void>(b.registerComponent<RpPos>());
  auto b1 = b.create();  // slot 3
  auto b2 = b.create();  // slot 2
  ASSERT_TRUE(b1.ok() && b2.ok());
  ASSERT_TRUE(b.destroy(b2.value()).ok());
  ASSERT_TRUE(b.destroy(b1.value()).ok());
  auto b3 = b.create();  // slot 3 again (gen 2)
  ASSERT_TRUE(b3.ok());
  static_cast<void>(b.addComponent(
      b3.value(), RpPos{laige::fpx16_16::fromInt32(7),
                        laige::fpx16_16::fromInt32(7)}));
  // Identical live state (slot 3, gen 2, RpPos(7)) -> identical hash,
  // despite the different dead-slot generations.
  EXPECT_EQ(a.stateHash(0), b.stateHash(0));

  // Archetype-id divergence: C first-sees {RpVel} then {RpPos} (the
  // {RpVel} archetype is EMPTY in the final state); D first-sees
  // {RpPos} only. Same live state -> same hash (the per-set ordering
  // is by SIGNATURE, and empty archetypes are excluded — entity.h
  // scope; the assigned ids are first-seen history).
  laige::World c = makeWorld(4, 0);
  static_cast<void>(c.registerComponent<RpPos>());
  static_cast<void>(c.registerComponent<RpVel>());
  auto c1 = c.create();
  ASSERT_TRUE(c1.ok());
  static_cast<void>(c.addComponent(c1.value(),
                                   RpVel{laige::fpx16_16::fromInt32(7),
                                         laige::fpx16_16::fromInt32(7)}));
  static_cast<void>(c.removeComponent<RpVel>(c1.value()));
  static_cast<void>(c.addComponent(c1.value(),
                                   RpPos{laige::fpx16_16::fromInt32(7),
                                         laige::fpx16_16::fromInt32(7)}));
  laige::World d = makeWorld(4, 0);
  static_cast<void>(d.registerComponent<RpPos>());
  static_cast<void>(d.registerComponent<RpVel>());
  auto d1 = d.create();
  ASSERT_TRUE(d1.ok());
  static_cast<void>(d.addComponent(d1.value(),
                                   RpPos{laige::fpx16_16::fromInt32(7),
                                         laige::fpx16_16::fromInt32(7)}));
  EXPECT_EQ(c.stateHash(0), d.stateHash(0));
}

TEST(StateHash, PrngStateEntersTheHash) {
  // No systems vs one undrawn system: the substream's existence (and
  // state) enters the hash even before any draw.
  laige::World plain = makeWorld(64, 42);
  static_cast<void>(plain.registerComponent<RpPos>());
  laige::World withIdle = makeWorld(64, 42);
  static_cast<void>(withIdle.registerComponent<RpPos>());
  ASSERT_TRUE(withIdle.registerSystem(RpIdle_Def).ok());
  EXPECT_NE(plain.stateHash(0), withIdle.stateHash(0));

  // The same seed + same systems + same draws -> identical streams;
  // a different seed diverges from tick 0.
  const std::uint64_t seedA = 0xA5A5;
  laige::World a = makeRpWorld(seedA, false);
  laige::World b = makeRpWorld(seedA, false);
  const std::vector<std::uint64_t> ha = runTicks(a, 8);
  const std::vector<std::uint64_t> hb = runTicks(b, 8);
  ASSERT_EQ(ha.size(), hb.size());
  for (std::size_t i = 0; i < ha.size(); ++i) {
    EXPECT_EQ(ha[i], hb[i]) << "tick " << i;
  }
  laige::World c = makeRpWorld(seedA + 1, false);
  const std::vector<std::uint64_t> hc = runTicks(c, 8);
  EXPECT_NE(ha[0], hc[0]);  // the substream seeds differ at tick 0

  // The draw position is the replay state: hashing the same world
  // before vs after one tick differs (the stream advanced).
  EXPECT_NE(ha[0], ha[1]);
}

#if defined(LAIGE_ALLOC_COUNTER)
// The state hash is allocation-free (entity.h @budget): a world with
// entities, components, and systems hashes cleanly under the counter.
TEST(StateHash, NoAllocation) {
  laige::World w = makeRpWorld(0x9E37, true);
  // Warm-up: exercise the world's one-time touch paths (and the
  // logger singleton) before the measured window.
  static_cast<void>(w.stateHash(0));
  runTicks(w, 2);
  laige::test::resetAllocCounter();
  for (int i = 0; i < 16; ++i) {
    static_cast<void>(w.stateHash(static_cast<std::uint64_t>(i)));
  }
  EXPECT_EQ(laige::test::allocCounter(), 0u);
}
#endif

// ---------------------------------------------------------------------------
// The replay execution half (M1-DET-03; replay.h "Replay execution")
// ---------------------------------------------------------------------------

// The step's integration test: a 500-tick scenario is recorded,
// replayed on a fresh identically-registered world, and the per-tick
// hashes are identical (bit-exact replay, FR-11.3).
TEST(DetReplay, FiveHundredTickRecordReplay) {
  const std::uint64_t seed = 0x0123456789abcdefULL;
  const laige::EngineConfig config = makeConfig(seed);
  laige::World a = makeRpWorld(seed, false);
  const RecordResult rec = recordRun(a, config, 500,
                                     tempPath("replay-500-a.log"));

  laige::World b = makeRpWorld(seed, false);
  const laige::Result<laige::ReplayLog, laige::ErrorCode> logR =
      laige::loadReplay(rec.path);
  ASSERT_TRUE(logR.ok());
  ASSERT_EQ(logR.value().frames.size(), 500u);
  const laige::Result<laige::ReplayRunResult, laige::ErrorCode> rr =
      laige::runReplay(logR.value(), b, config);
  ASSERT_TRUE(rr.ok());
  const std::vector<std::uint64_t>& hashes = rr.value().tickHashes;
  ASSERT_EQ(hashes.size(), 501u);  // tick 0..500
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    EXPECT_EQ(hashes[i], rec.hashes[i]) << "tick " << i;
  }
  // The stream is non-trivial (the mover actually moved things).
  EXPECT_NE(hashes[0], hashes[500]);
  // Machine-greppable identity line (docs/testing.md §4): the seed,
  // the frame count, and the first/last hash — byte-identical across
  // CI runs of the same commit.
  std::printf("replay-500 seed=0x%016llx frames=500 hash0=0x%016llx "
              "hash500=0x%016llx\n",
              static_cast<unsigned long long>(seed),
              static_cast<unsigned long long>(hashes[0]),
              static_cast<unsigned long long>(hashes[500]));
  std::fflush(stdout);
}

// The perturbation fixture: two identically-registered worlds (the
// probe registered in both), one with the probe armed at tick 7 —
// identical hashes through tick 6, first divergence at tick 7 (the
// first-divergence report laige-replay --expect and the baselines
// compare).
TEST(DetReplay, PerturbedDivergesAtTick7) {
  const std::uint64_t seed = 0x5EED;
  gRpTickCounter = 0;
  gRpPerturbTick = 0;
  laige::World clean = makeRpWorld(seed, true);  // probe registered, unarmed
  const std::vector<std::uint64_t> hClean = runTicks(clean, 20);

  gRpTickCounter = 0;
  gRpPerturbTick = 7;
  laige::World perturbed = makeRpWorld(seed, true);
  const std::vector<std::uint64_t> hPert = runTicks(perturbed, 20);
  gRpPerturbTick = 0;

  ASSERT_EQ(hClean.size(), hPert.size());
  std::size_t firstDiff = hClean.size();
  for (std::size_t i = 0; i < hClean.size(); ++i) {
    if (hClean[i] != hPert[i]) {
      firstDiff = i;
      break;
    }
  }
  EXPECT_EQ(firstDiff, 7u);
}

// An identity mismatch is a REJECTED replay: InvalidArgument, the
// structured warn names the differing fields, and each field of the
// diff is exactly the field that changed (ADR 0002).
TEST(DetReplay, IdentityMismatchRejected) {
  MemorySink* sink = installCaptureSink();
  const std::uint64_t seed = 42;
  const laige::EngineConfig config = makeConfig(seed);
  laige::World a = makeRpWorld(seed, false);
  const RecordResult rec = recordRun(a, config, 8,
                                     tempPath("replay-identity-a.log"));
  const laige::Result<laige::ReplayLog, laige::ErrorCode> logR =
      laige::loadReplay(rec.path);
  ASSERT_TRUE(logR.ok());
  const laige::ReplayLog& log = logR.value();

  // Control: the matching identity is empty and the replay runs.
  laige::World match = makeRpWorld(seed, false);
  EXPECT_TRUE(laige::replayIdentityDiff(log, match, config).empty());
  ASSERT_TRUE(laige::runReplay(log, match, config).ok());
  EXPECT_EQ(countEvents(*sink, "identity_mismatch"), 0u);

  laige::EngineConfig seedCfg = config;
  seedCfg.seed = 43;
  laige::EngineConfig tickCfg = config;
  tickCfg.tickRateHz = 120;
  laige::EngineConfig backendCfg = config;
  backendCfg.determinism.math = laige::SimMathBackend::FloatPinned32;
  laige::EngineConfig churnCfg = config;
  churnCfg.churnPerFrameBudget = 255;
  laige::World extraWorld = makeRpWorld(seed, false);
  static_cast<void>(extraWorld.registerComponent<RpExtra>());

  auto checkCase = [&](const char* name, const laige::EngineConfig& cfg,
                       laige::World& w, bool seedDiff, bool tickRateDiff,
                       bool schemaDiff, bool backendDiff, bool cfgHashDiff) {
    const laige::ReplayIdentityDiff d =
        laige::replayIdentityDiff(log, w, cfg);
    EXPECT_EQ(d.seed, seedDiff) << name;
    EXPECT_EQ(d.tickRateHz, tickRateDiff) << name;
    EXPECT_EQ(d.componentSchemaHash, schemaDiff) << name;
    EXPECT_EQ(d.mathBackendId, backendDiff) << name;
    EXPECT_EQ(d.configHash, cfgHashDiff) << name;
    const laige::Result<laige::ReplayRunResult, laige::ErrorCode> r =
        laige::runReplay(log, w, cfg);
    EXPECT_TRUE(r.isError()) << name;
    if (r.isError()) {
      EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument) << name;
    }
  };
  // The world is unchanged for the config-side cases (the `match`
  // world — fresh from the control replay above); the schema case
  // uses the extra-registration world.
  checkCase("seed", seedCfg, match, true, false, false, false, true);
  checkCase("tick_rate", tickCfg, match, false, true, false, false, true);
  checkCase("backend", backendCfg, match, false, false, false, true, true);
  checkCase("churn", churnCfg, match, false, false, false, false, true);
  checkCase("schema", config, extraWorld, false, false, true, false, false);
  EXPECT_EQ(countEvents(*sink, "identity_mismatch"), 5u);
  restoreLogger();
}

// A log recorded with determinism DISABLED is not replayable
// (determinism.h): the identity matches (the config hash carries the
// mode), but runReplay rejects with the deterministic warn.
TEST(DetReplay, DeterminismDisabledRejected) {
  MemorySink* sink = installCaptureSink();
  const std::uint64_t seed = 7;
  laige::EngineConfig config = makeConfig(seed);
  config.determinism.enabled = false;
  laige::World a = makeRpWorld(seed, false, /*deterministic*/ false);
  const RecordResult rec = recordRun(a, config, 5,
                                     tempPath("replay-nodet-a.log"));
  const laige::Result<laige::ReplayLog, laige::ErrorCode> logR =
      laige::loadReplay(rec.path);
  ASSERT_TRUE(logR.ok());
  laige::World b = makeRpWorld(seed, false, /*deterministic*/ false);
  EXPECT_TRUE(laige::replayIdentityDiff(logR.value(), b, config).empty());
  const laige::Result<laige::ReplayRunResult, laige::ErrorCode> r =
      laige::runReplay(logR.value(), b, config);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "determinism_disabled"), 1u);
  restoreLogger();
}

#if !defined(NDEBUG)
// The engine round trip: a headless engine run records its replay log
// (M1-DET-02 wiring — run_headless ALWAYS ends in the ordered
// shutdown, CONC-006, so the recording engine's world is released by
// the time the run returns; the final-state check therefore goes
// through a world-level twin with the identical built-in + game
// registrations and initial state, run for exactly the recorded tick
// count). The log replays on fresh identically-registered engines to
// the same per-tick hashes (the recorder is debug-builds-only,
// M1-DET-02).
TEST(DetReplay, EngineRoundTrip) {
  const std::uint64_t seed = 0x2A;
  laige::EngineConfig config;
  config.tickRateHz = 60;
  config.entityCapacity = 8;
  config.churnPerFrameBudget = 256;
  config.seed = seed;

  auto makeEngine = [&]() {
    laige::Result<laige::Engine, laige::ErrorCode> r =
        laige::Engine::create(config);
    if (!r.ok()) {
      ADD_FAILURE() << "Engine::create failed";
      abort();
    }
    laige::Engine engine = std::move(r).takeValue();
    laige::World& w = *engine.world();
    if (!w.registerComponent<RpPos>().ok() ||
        !w.registerComponent<RpVel>().ok() ||
        !w.registerSystem(RpMove_Def,
                          laige::Io<RpPos, laige::Access::Write>{},
                          laige::Io<RpVel, laige::Access::Write>{})
             .ok()) {
      ADD_FAILURE() << "engine registration failed";
      abort();
    }
    const laige::Result<laige::Entity, laige::ErrorCode> e1 = w.create();
    const laige::Result<laige::Entity, laige::ErrorCode> e2 = w.create();
    if (!e1.ok() || !e2.ok()) {
      ADD_FAILURE() << "engine entity creation failed";
      abort();
    }
    static_cast<void>(w.addComponent(
        e1.value(), RpPos{laige::fpx16_16::fromInt32(3),
                          laige::fpx16_16::fromInt32(-2)}));
    static_cast<void>(w.addComponent(
        e2.value(), RpPos{laige::fpx16_16::fromInt32(-1),
                          laige::fpx16_16::fromInt32(4)}));
    static_cast<void>(w.addComponent(
        e1.value(), RpVel{laige::fpx16_16::fromInt32(1),
                          laige::fpx16_16::fromInt32(0)}));
    static_cast<void>(w.addComponent(
        e2.value(), RpVel{laige::fpx16_16::fromInt32(0),
                          laige::fpx16_16::fromInt32(1)}));
    return engine;
  };

  laige::Engine recEngine = makeEngine();
  const std::string path = tempPath("replay-engine-a.log");
  const laige::Status rec = recEngine.startReplayRecording(path, 0);
  ASSERT_TRUE(rec.ok());
  const laige::Status run = recEngine.run_headless(60);
  ASSERT_TRUE(run.ok());
  const std::uint64_t frameCount = recEngine.stats().ticks;
  EXPECT_GE(frameCount, 60u);  // the bounded run reached the target
  // (run_headless already ran the ordered shutdown — this second
  // call exercises the idempotency, the laige-run convention.)
  recEngine.shutdown();

  const laige::Result<laige::ReplayLog, laige::ErrorCode> logR =
      laige::loadReplay(path);
  ASSERT_TRUE(logR.ok());
  ASSERT_EQ(logR.value().frames.size(), frameCount);

  // Two fresh engines replay the SAME stream (bit-exact), and the
  // stream ends on the recording engine's final state.
  laige::Engine repA = makeEngine();
  const laige::Result<laige::ReplayRunResult, laige::ErrorCode> ra =
      laige::runReplay(logR.value(), *repA.world(), config);
  ASSERT_TRUE(ra.ok());
  laige::Engine repB = makeEngine();
  const laige::Result<laige::ReplayRunResult, laige::ErrorCode> rb =
      laige::runReplay(logR.value(), *repB.world(), config);
  ASSERT_TRUE(rb.ok());
  ASSERT_EQ(ra.value().tickHashes.size(), frameCount + 1);
  ASSERT_EQ(rb.value().tickHashes.size(), ra.value().tickHashes.size());
  for (std::size_t i = 0; i < ra.value().tickHashes.size(); ++i) {
    EXPECT_EQ(ra.value().tickHashes[i], rb.value().tickHashes[i])
        << "tick " << i;
  }
  // The world-level twin: the identical built-in + game registrations
  // (the engine registers Position2DFpx16 FIRST, then the game's —
  // the Engine::create order) and the identical initial state, run
  // for exactly the recorded tick count. Its stream starts where the
  // replay's starts and ends where it ends (the final state).
  laige::World twin = makeWorld(config.entityCapacity, seed);
  static_cast<void>(twin.registerComponent<laige::Position2DFpx16>());
  static_cast<void>(twin.registerComponent<RpPos>());
  static_cast<void>(twin.registerComponent<RpVel>());
  static_cast<void>(twin.registerSystem(RpMove_Def,
                                        laige::Io<RpPos, laige::Access::Write>{},
                                        laige::Io<RpVel, laige::Access::Write>{}));
  const laige::Result<laige::Entity, laige::ErrorCode> t1 = twin.create();
  const laige::Result<laige::Entity, laige::ErrorCode> t2 = twin.create();
  ASSERT_TRUE(t1.ok() && t2.ok());
  static_cast<void>(twin.addComponent(
      t1.value(), RpPos{laige::fpx16_16::fromInt32(3),
                        laige::fpx16_16::fromInt32(-2)}));
  static_cast<void>(twin.addComponent(
      t2.value(), RpPos{laige::fpx16_16::fromInt32(-1),
                        laige::fpx16_16::fromInt32(4)}));
  static_cast<void>(twin.addComponent(
      t1.value(), RpVel{laige::fpx16_16::fromInt32(1),
                        laige::fpx16_16::fromInt32(0)}));
  static_cast<void>(twin.addComponent(
      t2.value(), RpVel{laige::fpx16_16::fromInt32(0),
                        laige::fpx16_16::fromInt32(1)}));
  const std::vector<std::uint64_t> twinHashes = runTicks(twin, frameCount);
  ASSERT_EQ(twinHashes.size(), frameCount + 1);
  EXPECT_EQ(twinHashes[0], ra.value().tickHashes[0]);
  EXPECT_EQ(twinHashes.back(), ra.value().tickHashes.back());
  // And the stream is non-trivial.
  EXPECT_NE(ra.value().tickHashes[0],
            ra.value().tickHashes.back());
  repA.shutdown();
  repB.shutdown();
  std::printf("replay-engine frames=%llu hash0=0x%016llx hashN=0x%016llx\n",
              static_cast<unsigned long long>(frameCount),
              static_cast<unsigned long long>(ra.value().tickHashes[0]),
              static_cast<unsigned long long>(ra.value().tickHashes.back()));
  std::fflush(stdout);
}
#endif
