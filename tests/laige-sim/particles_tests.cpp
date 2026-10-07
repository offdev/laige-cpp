// M2-PART-01: CPU particle simulation tests (the step's Verify
// command: `ctest -R particles`).
//
// The ParticleSystem<Backend> contract pinned against: the create
// domain + stopped state + move semantics, the addEmitter
// validation matrix (the pinned warn fields, first failure wins,
// state-unchanged-on-rejection, the registry budget), the burst
// spawn (exact particle state on a degenerate def), the per-tick
// advance + death (hand-computed Q16.16/float goldens, the swap
// removal's live order), the continuous rate, the pool exhaustion
// behavior (the exact per-tick live/dropped/warn table), the exact
// color fade (integer arithmetic), the determinism contract (fixed
// seed -> identical trajectories, the 4-draw-per-spawn Prng
// contract, the machine-greppable state hash), the stats feed, and
// the zero-allocation update window (PERF-003 — FR-2.7 "pooled").
//
// No GL needed — runs in every tree.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/result.h"
#include "laige/sim/particles.h"
#include "laige/sim_math.h"

namespace {

using laige::ParticleSystem;
using laige::ParticleEmitterDef;

// ---------------------------------------------------------------------------
// Backend-agnostic scalar construction (dyadic test values only)
// ---------------------------------------------------------------------------

template <typename Backend>
using M_ = laige::sim::SimMath<Backend>;

template <typename Backend>
typename M_<Backend>::Scalar sc(std::int64_t num, std::int64_t den = 1) {
  using Scalar = typename M_<Backend>::Scalar;
  if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
    // Exact for the dyadic test values used here.
    return laige::fpx16_16{
        static_cast<std::int32_t>(num * (std::int64_t(1) << 16) / den)};
  } else {
    return static_cast<Scalar>(static_cast<float>(num) /
                               static_cast<float>(den));
  }
}

// A valid, non-degenerate base def (the validation matrix mutates one
// field at a time from this).
template <typename Backend>
ParticleEmitterDef<Backend> baseDef() {
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> d;
  d.origin = Vec2{sc<Backend>(1), sc<Backend>(0)};
  d.depth = sc<Backend>(0);
  d.velMin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.velMax = Vec2{sc<Backend>(1), sc<Backend>(1)};
  d.lifeMin = 5;
  d.lifeMax = 10;
  d.sizeMin = sc<Backend>(1, 2);
  d.sizeMax = sc<Backend>(3, 2);
  d.tint[0] = 255;
  d.tint[1] = 255;
  d.tint[2] = 255;
  d.tint[3] = 255;
  d.fadeTicks = 0;
  d.continuousRate = 0;
  return d;
}

// A fully degenerate def (min == max everywhere): a spawn from it is
// deterministic without any Prng value (the lerp endpoints coincide).
template <typename Backend>
ParticleEmitterDef<Backend> degDef(std::int64_t ox, std::int64_t oy,
                                   std::int64_t depth, std::int64_t vx,
                                   std::int64_t vy, std::uint32_t life,
                                   std::int64_t size, std::uint8_t a,
                                   std::uint32_t fade) {
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> d = baseDef<Backend>();
  d.origin = Vec2{sc<Backend>(ox), sc<Backend>(oy)};
  d.depth = sc<Backend>(depth);
  d.velMin = Vec2{sc<Backend>(vx), sc<Backend>(vy)};
  d.velMax = Vec2{sc<Backend>(vx), sc<Backend>(vy)};
  d.lifeMin = life;
  d.lifeMax = life;
  d.sizeMin = sc<Backend>(size);
  d.sizeMax = sc<Backend>(size);
  d.tint[0] = 10;
  d.tint[1] = 20;
  d.tint[2] = 30;
  d.tint[3] = a;
  d.fadeTicks = fade;
  d.continuousRate = 0;
  return d;
}

// ---------------------------------------------------------------------------
// Log capture (the archetype_tests MemorySink pattern: rate limiting
// OFF — the per-tick warn budget is the system's own flag)
// ---------------------------------------------------------------------------

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

MemorySink* installSink() {
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

std::size_t countEvents(const MemorySink& sink, std::string_view subsystem,
                        std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

// ---------------------------------------------------------------------------
// The machine-greppable state hash (the docs/testing.md §4 KAT
// convention: FNV-1a 64, big-endian byte order per u64)
// ---------------------------------------------------------------------------

std::uint64_t particlesFnv1a64(const std::uint64_t* values, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis (FNV-1a spec)
  for (std::size_t i = 0; i < n; ++i) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (values[i] >> shift) & 0xFFull;
      h *= 0x100000001b3ull;  // FNV prime (FNV-1a spec)
    }
  }
  return h;
}

template <typename Backend>
std::uint64_t particlesStateHash(const ParticleSystem<Backend>& sys) {
  using Scalar = typename M_<Backend>::Scalar;
  std::vector<std::uint64_t> w;
  auto addScalar = [&](Scalar s) {
    if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
      w.push_back(static_cast<std::uint64_t>(
          static_cast<std::uint32_t>(s.raw)));
    } else {
      std::uint32_t bits;
      std::memcpy(&bits, &s, sizeof(bits));
      w.push_back(bits);
    }
  };
  w.push_back(sys.prngSeed());
  w.push_back(sys.maxParticles());
  w.push_back(sys.maxEmitters());
  w.push_back(sys.emitterCount());
  w.push_back(sys.liveCount());
  w.push_back(sys.spawnedTotal());
  w.push_back(sys.droppedTotal());
  w.push_back(sys.prngStatePart1());
  w.push_back(sys.prngStatePart2());
  for (const auto& p : sys.liveParticles()) {
    addScalar(p.pos.x);
    addScalar(p.pos.y);
    addScalar(p.depth);
    addScalar(p.vel.x);
    addScalar(p.vel.y);
    addScalar(p.size);
    w.push_back(p.age);
    w.push_back(p.life);
    w.push_back(p.fadeTicks);
    w.push_back((std::uint64_t(p.tint[0]) << 24) |
                (std::uint64_t(p.tint[1]) << 16) |
                (std::uint64_t(p.tint[2]) << 8) | std::uint64_t(p.tint[3]));
  }
  return particlesFnv1a64(w.data(), w.size());
}

// One uniform scalar exactly as the engine samples it (the test-side
// oracle for the 4-draw contract).
template <typename Backend>
typename M_<Backend>::Scalar oracleSample(laige::Prng& oracle,
                                          typename M_<Backend>::Scalar lo,
                                          typename M_<Backend>::Scalar hi) {
  using M = M_<Backend>;
  const std::uint32_t u =
      oracle.next_range(0, laige::kParticleSampleDenominator);
  const auto t = M::div(static_cast<typename M::Scalar>(u),
                        static_cast<typename M::Scalar>(laige::kParticleSampleDenominator));
  return M::lerp(lo, hi, t);
}

// ---------------------------------------------------------------------------
// ParticlesCreate
// ---------------------------------------------------------------------------

template <typename Backend>
void createDomain() {
  using Sys = ParticleSystem<Backend>;
  // Defaults.
  auto r = Sys::create({});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  EXPECT_TRUE(sys.valid());
  EXPECT_EQ(sys.maxParticles(), laige::kParticlePoolDefaultMaxParticles);
  EXPECT_EQ(sys.maxEmitters(), laige::kParticleEmitterDefaultMaxEmitters);
  EXPECT_EQ(sys.seed(), 0u);
  EXPECT_EQ(sys.liveCount(), 0u);
  EXPECT_EQ(sys.emitterCount(), 0u);
  EXPECT_EQ(sys.spawnedTotal(), 0u);
  EXPECT_EQ(sys.droppedTotal(), 0u);
  const auto stats = sys.stats();
  EXPECT_EQ(stats.live, 0u);
  EXPECT_EQ(stats.capacity, laige::kParticlePoolDefaultMaxParticles);
  EXPECT_EQ(stats.spawnedTotal, 0u);
  EXPECT_EQ(stats.droppedTotal, 0u);

  // Explicit options echo.
  auto r2 = Sys::create({1, 1, 0x1234});
  ASSERT_TRUE(r2.ok());
  auto sys2 = std::move(r2).takeValue();
  EXPECT_EQ(sys2.maxParticles(), 1u);
  EXPECT_EQ(sys2.maxEmitters(), 1u);
  EXPECT_EQ(sys2.seed(), 0x1234u);

  // The domain edges (first failure wins; no log on create rejection).
  MemorySink* sink = installSink();
  EXPECT_FALSE(Sys::create({0, 1, 0}).ok());
  EXPECT_FALSE(Sys::create({laige::kParticlePoolMaxParticles + 1, 1, 0}).ok());
  EXPECT_TRUE(Sys::create({laige::kParticlePoolMaxParticles, 1, 0}).ok());
  EXPECT_FALSE(Sys::create({1, 0, 0}).ok());
  EXPECT_FALSE(Sys::create({1, laige::kParticleEmitterMaxEmitters + 1, 0})
                   .ok());
  EXPECT_TRUE(Sys::create({1, laige::kParticleEmitterMaxEmitters, 0}).ok());
  EXPECT_TRUE(Sys::create({0, 0, 0}).error() == laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(sink->entries.size(), 0u) << "create logs nothing (rejection or success)";
  restoreLogger();
}

TEST(ParticlesCreate, CreateDomain) {
  createDomain<laige::sim::Fpx16_16>();
  createDomain<laige::sim::Fp32Pinned>();
}

template <typename Backend>
void stoppedState() {
  using Sys = ParticleSystem<Backend>;
  Sys stopped;
  EXPECT_FALSE(stopped.valid());
  EXPECT_EQ(stopped.maxParticles(), 0u);
  EXPECT_EQ(stopped.maxEmitters(), 0u);
  EXPECT_EQ(stopped.liveCount(), 0u);
  EXPECT_EQ(stopped.emitterCount(), 0u);
  EXPECT_TRUE(stopped.liveParticles().empty());
  EXPECT_FALSE(stopped.hasEmitter(1));
  EXPECT_EQ(stopped.emitterAt(1), nullptr);
  const auto stats = stopped.stats();
  EXPECT_EQ(stats.live, 0u);
  EXPECT_EQ(stats.capacity, 0u);
  EXPECT_EQ(stats.spawnedTotal, 0u);
  EXPECT_EQ(stats.droppedTotal, 0u);

  MemorySink* sink = installSink();
  auto r = stopped.addEmitter(baseDef<Backend>());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  auto st = stopped.burst(1, 1);
  EXPECT_EQ(st.error(), laige::ErrorCode::InvalidArgument);
  stopped.update();  // no-op (no crash)
  EXPECT_EQ(sink->entries.size(), 0u) << "the stopped state logs nothing";
  restoreLogger();
}

TEST(ParticlesCreate, StoppedState) {
  stoppedState<laige::sim::Fpx16_16>();
  stoppedState<laige::sim::Fp32Pinned>();
}

template <typename Backend>
void moveStopsSource() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({4, 2, 7});
  ASSERT_TRUE(r.ok());
  auto src = std::move(r).takeValue();
  auto d = degDef<Backend>(3, 4, 5, 1, -1, 7, 2, 40, 2);
  ASSERT_TRUE(src.addEmitter(d).ok());
  ASSERT_TRUE(src.burst(1, 2).ok());
  EXPECT_EQ(src.liveCount(), 2u);

  MemorySink* sink = installSink();
  Sys dst(std::move(src));
  EXPECT_TRUE(dst.valid());
  EXPECT_EQ(dst.liveCount(), 2u);
  EXPECT_EQ(dst.emitterCount(), 1u);
  EXPECT_EQ(dst.seed(), 7u);
  EXPECT_EQ(dst.maxParticles(), 4u);
  EXPECT_EQ(dst.maxEmitters(), 2u);
  EXPECT_EQ(dst.stats().spawnedTotal, 2u);

  // The moved-from system is stopped (and logs nothing).
  EXPECT_FALSE(src.valid());
  EXPECT_EQ(src.liveCount(), 0u);
  EXPECT_EQ(src.burst(1, 1).error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(src.addEmitter(d).error(), laige::ErrorCode::InvalidArgument);
  src.update();  // no-op
  EXPECT_EQ(sink->entries.size(), 0u);
  restoreLogger();
}

TEST(ParticlesCreate, MoveStopsSource) {
  moveStopsSource<laige::sim::Fpx16_16>();
  moveStopsSource<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesEmitter
// ---------------------------------------------------------------------------

template <typename Backend>
void denseIds() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({8, 4, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  auto d1 = baseDef<Backend>();
  auto d2 = baseDef<Backend>();
  d2.continuousRate = 2;
  auto id1 = sys.addEmitter(d1);
  auto id2 = sys.addEmitter(d2);
  ASSERT_TRUE(id1.ok());
  ASSERT_TRUE(id2.ok());
  EXPECT_EQ(*id1.valueIfOk(), 1u);
  EXPECT_EQ(*id2.valueIfOk(), 2u);
  EXPECT_EQ(sys.emitterCount(), 2u);
  EXPECT_TRUE(sys.hasEmitter(1));
  EXPECT_TRUE(sys.hasEmitter(2));
  EXPECT_FALSE(sys.hasEmitter(0));
  EXPECT_FALSE(sys.hasEmitter(3));
  const auto* e1 = sys.emitterAt(1);
  ASSERT_NE(e1, nullptr);
  EXPECT_EQ(e1->origin.x, d1.origin.x);
  EXPECT_EQ(e1->origin.y, d1.origin.y);
  EXPECT_EQ(e1->continuousRate, 0u);
  const auto* e2 = sys.emitterAt(2);
  ASSERT_NE(e2, nullptr);
  EXPECT_EQ(e2->continuousRate, 2u);
}

TEST(ParticlesEmitter, DenseIds) {
  denseIds<laige::sim::Fpx16_16>();
  denseIds<laige::sim::Fp32Pinned>();
}

template <typename Backend>
void validationMatrix() {
  using Sys = ParticleSystem<Backend>;
  using Scalar = typename M_<Backend>::Scalar;
  constexpr bool kIsFp32 =
      std::is_same_v<Backend, laige::sim::Fp32Pinned>;

  // (mutator, expected field name, applies-to-this-backend)
  struct Case {
    void (*mutate)(ParticleEmitterDef<Backend>&);
    const char* field;
    bool apply;
  };
  auto expectReject = [&](ParticleEmitterDef<Backend> d, const char* field,
                           std::uint32_t emitterId) {
    auto r = Sys::create({8, 4, 0});
    ASSERT_TRUE(r.ok());
    auto sys = std::move(r).takeValue();
    MemorySink* sink = installSink();
    auto res = sys.addEmitter(d);
    EXPECT_EQ(res.error(), laige::ErrorCode::InvalidArgument)
        << "field " << field;
    // First failure wins: exactly one warn, with the pinned fields.
    EXPECT_EQ(countEvents(*sink, "particles", "emitter_invalid"), 1u)
        << "field " << field;
    if (!sink->entries.empty()) {
      const auto& e = sink->entries.front();
      EXPECT_EQ(fieldOf(e, "field"), field);
      EXPECT_EQ(fieldOf(e, "emitter"), std::to_string(emitterId));
    }
    // State unchanged on rejection.
    EXPECT_EQ(sys.emitterCount(), 0u);
    EXPECT_FALSE(sys.hasEmitter(emitterId));
    restoreLogger();
  };

  auto mutate = [&](void (*fn)(ParticleEmitterDef<Backend>&),
                    const char* field, bool apply) {
    if (!apply) return;
    ParticleEmitterDef<Backend> d = baseDef<Backend>();
    fn(d);
    expectReject(d, field, 1);
  };

  if constexpr (kIsFp32) {
    const Scalar nan = std::numeric_limits<float>::quiet_NaN();
    {
      ParticleEmitterDef<Backend> d = baseDef<Backend>();
      d.origin.x = nan;
      expectReject(d, "origin", 1);
    }
    {
      ParticleEmitterDef<Backend> d = baseDef<Backend>();
      d.depth = nan;
      expectReject(d, "depth", 1);
    }
    {
      ParticleEmitterDef<Backend> d = baseDef<Backend>();
      d.velMin.x = nan;
      expectReject(d, "vel_min", 1);
    }
    {
      ParticleEmitterDef<Backend> d = baseDef<Backend>();
      d.velMax.y = nan;
      expectReject(d, "vel_max", 1);
    }
    // sizeMax non-finite (sizeMin is fine — the order reaches it).
    {
      ParticleEmitterDef<Backend> d = baseDef<Backend>();
      d.sizeMax = nan;
      expectReject(d, "size_max", 1);
    }
    // First failure wins: origin (earlier) before life_min.
    {
      ParticleEmitterDef<Backend> d = baseDef<Backend>();
      d.origin.x = nan;
      d.lifeMin = 0;
      expectReject(d, "origin", 1);
    }
  } else {
    // fpx16_16 has no NaN — the box/domain cases still apply, and the
    // first-failure-wins case uses vel_box (earlier than life_min).
    ParticleEmitterDef<Backend> d = baseDef<Backend>();
    d.velMin.x = sc<Backend>(2);
    d.velMax.x = sc<Backend>(1);
    d.lifeMin = 0;
    expectReject(d, "vel_box", 1);
  }

  mutate([](ParticleEmitterDef<Backend>& d) {
           d.velMin.x = sc<Backend>(1);
           d.velMax.x = sc<Backend>(1, 2);
         },
         "vel_box", true);
  mutate([](ParticleEmitterDef<Backend>& d) { d.lifeMin = 0; }, "life_min",
         true);
  mutate([](ParticleEmitterDef<Backend>& d) {
           d.lifeMin = 10;
           d.lifeMax = 5;
         },
         "life_box", true);
  mutate([](ParticleEmitterDef<Backend>& d) {
           d.lifeMin = 1;
           d.lifeMax = laige::kParticleMaxLifeTicks + 1;
         },
         "life_max", true);
  mutate([](ParticleEmitterDef<Backend>& d) { d.sizeMin = sc<Backend>(-1); },
         "size_min", true);
  mutate([](ParticleEmitterDef<Backend>& d) {
           d.sizeMin = sc<Backend>(2);
           d.sizeMax = sc<Backend>(1);
         },
         "size_box", true);
  mutate([](ParticleEmitterDef<Backend>& d) {
           d.lifeMax = 5;
           d.fadeTicks = 6;
         },
         "fade_ticks", true);
}

TEST(ParticlesEmitter, ValidationMatrix) {
  validationMatrix<laige::sim::Fpx16_16>();
  validationMatrix<laige::sim::Fp32Pinned>();
}

template <typename Backend>
void registryExhaustion() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({8, 2, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  ASSERT_TRUE(sys.addEmitter(baseDef<Backend>()).ok());
  ASSERT_TRUE(sys.addEmitter(baseDef<Backend>()).ok());
  MemorySink* sink = installSink();
  auto res = sys.addEmitter(baseDef<Backend>());
  EXPECT_EQ(res.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(countEvents(*sink, "particles", "emitters_exhausted"), 1u);
  EXPECT_EQ(fieldOf(sink->entries.front(), "capacity"), "2");
  EXPECT_EQ(sys.emitterCount(), 2u);
  restoreLogger();
}

TEST(ParticlesEmitter, RegistryExhaustion) {
  registryExhaustion<laige::sim::Fpx16_16>();
  registryExhaustion<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesBurst
// ---------------------------------------------------------------------------

template <typename Backend>
void burstExact() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({8, 2, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  // Degenerate def: every spawn is bit-predictable.
  auto d = degDef<Backend>(3, 4, 5, 1, -1, 7, 2, 40, 2);
  auto id = sys.addEmitter(d);
  ASSERT_TRUE(id.ok());
  ASSERT_TRUE(sys.burst(*id.valueIfOk(), 3).ok());
  EXPECT_EQ(sys.liveCount(), 3u);
  EXPECT_EQ(sys.spawnedTotal(), 3u);
  EXPECT_EQ(sys.droppedTotal(), 0u);

  MemorySink* sink = installSink();
  auto sv = sys.burst(*id.valueIfOk(), 0);  // no-op success
  EXPECT_TRUE(sv.ok());
  EXPECT_EQ(sys.liveCount(), 3u);
  EXPECT_EQ(sys.burst(0, 1).error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(sys.burst(2, 1).error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(sink->entries.size(), 0u) << "burst logs nothing on the happy path / unknown-id rejection";
  restoreLogger();

  // Pin every particle field (spawn order).
  const auto live = sys.liveParticles();
  ASSERT_EQ(live.size(), 3u);
  for (std::size_t i = 0; i < live.size(); ++i) {
    const auto& p = live[i];
    if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
      EXPECT_EQ(p.pos.x.raw, 3 * 65536) << "particle " << i;
      EXPECT_EQ(p.pos.y.raw, 4 * 65536) << "particle " << i;
      EXPECT_EQ(p.depth.raw, 5 * 65536) << "particle " << i;
      EXPECT_EQ(p.vel.x.raw, 65536) << "particle " << i;
      EXPECT_EQ(p.vel.y.raw, -65536) << "particle " << i;
      EXPECT_EQ(p.size.raw, 2 * 65536) << "particle " << i;
    } else {
      EXPECT_EQ(p.pos.x, 3.0f) << "particle " << i;
      EXPECT_EQ(p.pos.y, 4.0f) << "particle " << i;
      EXPECT_EQ(p.depth, 5.0f) << "particle " << i;
      EXPECT_EQ(p.vel.x, 1.0f) << "particle " << i;
      EXPECT_EQ(p.vel.y, -1.0f) << "particle " << i;
      EXPECT_EQ(p.size, 2.0f) << "particle " << i;
    }
    EXPECT_EQ(p.age, 0u) << "particle " << i;
    EXPECT_EQ(p.life, 7u) << "particle " << i;
    EXPECT_EQ(p.fadeTicks, 2u) << "particle " << i;
    EXPECT_EQ(p.tint[0], 10u) << "particle " << i;
    EXPECT_EQ(p.tint[1], 20u) << "particle " << i;
    EXPECT_EQ(p.tint[2], 30u) << "particle " << i;
    EXPECT_EQ(p.tint[3], 40u) << "particle " << i;
  }
}

TEST(ParticlesBurst, BurstExact) {
  burstExact<laige::sim::Fpx16_16>();
  burstExact<laige::sim::Fp32Pinned>();
}

template <typename Backend>
void burstPoolDrop() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({2, 1, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  auto d = degDef<Backend>(0, 0, 0, 1, 0, 10, 1, 255, 0);
  auto id = sys.addEmitter(d);
  ASSERT_TRUE(id.ok());
  MemorySink* sink = installSink();
  ASSERT_TRUE(sys.burst(*id.valueIfOk(), 5).ok());
  EXPECT_EQ(sys.liveCount(), 2u);
  EXPECT_EQ(sys.spawnedTotal(), 2u);
  EXPECT_EQ(sys.droppedTotal(), 3u);
  // The burst reports its own tick's overflow: exactly one warn.
  EXPECT_EQ(countEvents(*sink, "particles", "pool_overflow"), 1u);
  EXPECT_EQ(fieldOf(sink->entries.front(), "dropped"), "3");
  EXPECT_EQ(fieldOf(sink->entries.front(), "live"), "2");
  EXPECT_EQ(fieldOf(sink->entries.front(), "capacity"), "2");
  restoreLogger();
}

TEST(ParticlesBurst, BurstPoolDrop) {
  burstPoolDrop<laige::sim::Fpx16_16>();
  burstPoolDrop<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesUpdate
// ---------------------------------------------------------------------------

template <typename Backend>
void advanceAndDeath() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({8, 1, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  // vel (0.5, -0.25) per tick, life 5: the hand-computed goldens.
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> def = baseDef<Backend>();
  def.origin = Vec2{sc<Backend>(1), sc<Backend>(2)};
  def.depth = sc<Backend>(0);
  def.velMin = Vec2{sc<Backend>(1, 2), sc<Backend>(-1, 4)};
  def.velMax = def.velMin;
  def.lifeMin = 5;
  def.lifeMax = 5;
  def.sizeMin = sc<Backend>(1);
  def.sizeMax = sc<Backend>(1);
  def.fadeTicks = 0;
  def.continuousRate = 0;
  auto id = sys.addEmitter(def);
  ASSERT_TRUE(id.ok());
  ASSERT_TRUE(sys.burst(*id.valueIfOk(), 1).ok());

  MemorySink* sink = installSink();
  // After update k (k = 1..4): age k, pos (1 + 0.5k, 2 - 0.25k).
  for (int k = 1; k <= 4; ++k) {
    sys.update();
    const auto live = sys.liveParticles();
    ASSERT_EQ(live.size(), 1u) << "update " << k;
    EXPECT_EQ(live[0].age, std::uint32_t(k)) << "update " << k;
    if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
      // Q16.16 raw: x = 65536 + 32768k; y = 131072 - 16384k.
      EXPECT_EQ(live[0].pos.x.raw, 65536 + 32768 * k) << "update " << k;
      EXPECT_EQ(live[0].pos.y.raw, 131072 - 16384 * k) << "update " << k;
    } else {
      EXPECT_EQ(live[0].pos.x, static_cast<float>(1) + 0.5f * k)
          << "update " << k;
      EXPECT_EQ(live[0].pos.y, 2.0f - 0.25f * k) << "update " << k;
    }
  }
  // The 5th update: age 5 >= life 5 -> dead (visible for exactly 5
  // renders: age 0..4).
  sys.update();
  EXPECT_EQ(sys.liveCount(), 0u);
  EXPECT_EQ(sink->entries.size(), 0u) << "the happy advance path logs nothing";
  restoreLogger();
}

TEST(ParticlesUpdate, AdvanceAndDeath) {
  advanceAndDeath<laige::sim::Fpx16_16>();
  advanceAndDeath<laige::sim::Fp32Pinned>();
}

template <typename Backend>
void swapRemoveOrder() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({8, 2, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  // Two emitters: A (life 2, origin (0,0)) and B (life 10, origin
  // (9,9)). Spawn A then B -> live [A, B].
  auto a = sys.addEmitter(degDef<Backend>(0, 0, 0, 1, 0, 2, 1, 255, 0));
  auto b = sys.addEmitter(degDef<Backend>(9, 9, 0, 1, 0, 10, 1, 255, 0));
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(sys.burst(1, 1).ok());
  ASSERT_TRUE(sys.burst(2, 1).ok());
  ASSERT_EQ(sys.liveCount(), 2u);
  sys.update();  // A age 1, B age 1
  ASSERT_EQ(sys.liveCount(), 2u);
  sys.update();  // A age 2 -> dies (swap-removed); B age 2
  ASSERT_EQ(sys.liveCount(), 1u);
  const auto live = sys.liveParticles();
  ASSERT_EQ(live.size(), 1u);
  // The survivor is B, now at index 0 (the swap removed A's slot).
  if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
    EXPECT_EQ(live[0].pos.x.raw, 9 * 65536 + 2 * 65536);
    EXPECT_EQ(live[0].pos.y.raw, 9 * 65536);
  } else {
    EXPECT_EQ(live[0].pos.x, 11.0f);
    EXPECT_EQ(live[0].pos.y, 9.0f);
  }
  EXPECT_EQ(live[0].life, 10u);
}

TEST(ParticlesUpdate, SwapRemoveOrder) {
  swapRemoveOrder<laige::sim::Fpx16_16>();
  swapRemoveOrder<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesContinuous
// ---------------------------------------------------------------------------

template <typename Backend>
void continuousRateAndOracle() {
  using Sys = ParticleSystem<Backend>;
  using Vec2 = typename M_<Backend>::Vec2;
  constexpr std::uint64_t kSeed = 0x0101010101010101ull;
  auto r = Sys::create({10, 1, kSeed});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  ParticleEmitterDef<Backend> def = baseDef<Backend>();
  def.origin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  def.depth = sc<Backend>(0);
  def.velMin = Vec2{sc<Backend>(0), sc<Backend>(-1)};
  def.velMax = Vec2{sc<Backend>(1), sc<Backend>(0)};
  // life 20 (fixed): no deaths in this short scenario — the pool
  // fills and overflows cleanly.
  def.lifeMin = 20;
  def.lifeMax = 20;
  def.sizeMin = sc<Backend>(1, 2);
  def.sizeMax = sc<Backend>(3, 2);
  def.tint[0] = 1;
  def.tint[1] = 2;
  def.tint[2] = 3;
  def.tint[3] = 255;
  def.fadeTicks = 0;
  def.continuousRate = 2;
  auto id = sys.addEmitter(def);
  ASSERT_TRUE(id.ok());

  // Independent oracle for the first two spawns (the 4-draw contract:
  // vx, vy, life, size — in order).
  laige::Prng oracle(kSeed);
  auto expectParticle = [&](const auto& p, const char* label) {
    const auto vx = oracleSample<Backend>(oracle, def.velMin.x, def.velMax.x);
    const auto vy = oracleSample<Backend>(oracle, def.velMin.y, def.velMax.y);
    const std::uint32_t life = oracle.next_range(def.lifeMin, def.lifeMax + 1);
    const auto size = oracleSample<Backend>(oracle, def.sizeMin, def.sizeMax);
    EXPECT_EQ(p.vel.x, vx) << label;
    EXPECT_EQ(p.vel.y, vy) << label;
    EXPECT_EQ(p.age, 0u) << label;
    EXPECT_EQ(p.life, life) << label;
    EXPECT_EQ(p.size, size) << label;
    EXPECT_EQ(p.pos.x, sc<Backend>(0)) << label;
    EXPECT_EQ(p.pos.y, sc<Backend>(0)) << label;
    EXPECT_EQ(p.depth, sc<Backend>(0)) << label;
    EXPECT_EQ(p.tint[0], 1u) << label;
    EXPECT_EQ(p.fadeTicks, 0u) << label;
  };

  sys.update();  // spawn 2
  {
    const auto live = sys.liveParticles();
    ASSERT_EQ(live.size(), 2u);
    expectParticle(live[0], "spawn 1");
    expectParticle(live[1], "spawn 2");
  }
  sys.update();  // spawn 2 more
  EXPECT_EQ(sys.liveCount(), 4u);
  EXPECT_EQ(sys.droppedTotal(), 0u);
  // Fill the pool (10), then overflow: exactly one warn per tick.
  MemorySink* sink = installSink();
  for (int t = 0; t < 3; ++t) {
    sys.update();  // t=3..5: live 6, 8, 10
  }
  EXPECT_EQ(sys.liveCount(), 10u);
  EXPECT_EQ(sys.droppedTotal(), 0u);
  sys.update();  // t=6: live 10, 2 drops
  EXPECT_EQ(sys.liveCount(), 10u);
  EXPECT_EQ(sys.droppedTotal(), 2u);
  EXPECT_EQ(countEvents(*sink, "particles", "pool_overflow"), 1u);
  EXPECT_EQ(fieldOf(sink->entries.front(), "dropped"), "2");
  EXPECT_EQ(fieldOf(sink->entries.front(), "capacity"), "10");
  sys.update();  // t=7: 2 more drops — still ONE warn per tick
  EXPECT_EQ(sys.droppedTotal(), 4u);
  EXPECT_EQ(countEvents(*sink, "particles", "pool_overflow"), 2u);
  restoreLogger();
}

TEST(ParticlesContinuous, RateAndOracle) {
  continuousRateAndOracle<laige::sim::Fpx16_16>();
  continuousRateAndOracle<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesDeterminism
// ---------------------------------------------------------------------------

template <typename Backend>
void runDeterminismSequence(ParticleSystem<Backend>& sys) {
  // 50 ticks: one continuous emitter (rate 3, life 3..8, wide boxes)
  // + a burst(2, 5) after every 10th tick (emitter 2, life 5..5,
  // degenerate). Pool capacity 8 -> drops occur throughout.
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> c = baseDef<Backend>();
  c.velMin = Vec2{sc<Backend>(-1), sc<Backend>(-1, 2)};
  c.velMax = Vec2{sc<Backend>(2), sc<Backend>(1)};
  c.lifeMin = 3;
  c.lifeMax = 8;
  c.sizeMin = sc<Backend>(1, 4);
  c.sizeMax = sc<Backend>(2);
  c.tint[0] = 200;
  c.tint[3] = 128;
  c.fadeTicks = 3;
  c.continuousRate = 3;
  auto rc = sys.addEmitter(c);
  ASSERT_TRUE(rc.ok());
  auto rb = sys.addEmitter(degDef<Backend>(7, 7, 1, 0, 1, 5, 3, 64, 0));
  ASSERT_TRUE(rb.ok());
  for (int t = 0; t < 50; ++t) {
    sys.update();
    if (t % 10 == 0) {
      ASSERT_TRUE(sys.burst(2, 5).ok());
    }
  }
}

template <typename Backend>
void sameSeedSameTrajectory(const char* backendName) {
  using Sys = ParticleSystem<Backend>;
  constexpr std::uint64_t kSeed = 0x0123456789abcdefull;

  auto ra = Sys::create({8, 4, kSeed});
  ASSERT_TRUE(ra.ok());
  auto a = std::move(ra).takeValue();
  runDeterminismSequence(a);
  auto rb = Sys::create({8, 4, kSeed});
  ASSERT_TRUE(rb.ok());
  auto b = std::move(rb).takeValue();
  runDeterminismSequence(b);

  // The whole live set is bit-identical (every field).
  const auto la = a.liveParticles();
  const auto lb = b.liveParticles();
  ASSERT_EQ(la.size(), lb.size());
  for (std::size_t i = 0; i < la.size(); ++i) {
    EXPECT_TRUE(M_<Backend>::equals(la[i].pos, lb[i].pos)) << "particle " << i;
    EXPECT_TRUE(M_<Backend>::equals(la[i].vel, lb[i].vel)) << "particle " << i;
    EXPECT_TRUE(M_<Backend>::equals(la[i].depth, lb[i].depth))
        << "particle " << i;
    EXPECT_TRUE(M_<Backend>::equals(la[i].size, lb[i].size))
        << "particle " << i;
    EXPECT_EQ(la[i].age, lb[i].age) << "particle " << i;
    EXPECT_EQ(la[i].life, lb[i].life) << "particle " << i;
    EXPECT_EQ(la[i].fadeTicks, lb[i].fadeTicks) << "particle " << i;
    for (int ch = 0; ch < 4; ++ch) {
      EXPECT_EQ(la[i].tint[ch], lb[i].tint[ch]) << "particle " << i;
    }
  }
  EXPECT_EQ(a.spawnedTotal(), b.spawnedTotal());
  EXPECT_EQ(a.droppedTotal(), b.droppedTotal());
  EXPECT_EQ(a.prngStatePart1(), b.prngStatePart1());
  EXPECT_EQ(a.prngStatePart2(), b.prngStatePart2());

  // The machine-greppable state hash (the KAT convention).
  const std::uint64_t hash = particlesStateHash(a);
  printf("particles-determinism backend=%s seed=0x%016llx ticks=50 "
         "spawns=%llu fnv1a=0x%016llx\n",
         backendName, static_cast<unsigned long long>(kSeed),
         static_cast<unsigned long long>(a.spawnedTotal()),
         static_cast<unsigned long long>(hash));

  // A different seed diverges (the replay-identity contract).
  auto rc = Sys::create({8, 4, kSeed + 1});
  ASSERT_TRUE(rc.ok());
  auto c = std::move(rc).takeValue();
  runDeterminismSequence(c);
  EXPECT_NE(particlesStateHash(c), hash);

  // The 4-draw contract: an independent Prng advanced by 4 x
  // spawnedTotal draws lands on the system's stream state (a DROPPED
  // spawn consumes no draws — the pool check precedes the draws).
  laige::Prng oracle(kSeed);
  for (std::uint32_t i = 0; i < a.spawnedTotal() * 4; ++i) {
    oracle.next_u64();
  }
  EXPECT_EQ(oracle.statePart1(), a.prngStatePart1());
  EXPECT_EQ(oracle.statePart2(), a.prngStatePart2());
  EXPECT_GT(a.droppedTotal(), 0u)
      << "the scenario must exercise the drop path (no draws consumed)";
}

TEST(ParticlesDeterminism, SameSeedSameTrajectory) {
  sameSeedSameTrajectory<laige::sim::Fpx16_16>("fpx16_16");
  sameSeedSameTrajectory<laige::sim::Fp32Pinned>("fp32_pinned");
}

// ---------------------------------------------------------------------------
// ParticlesFade
// ---------------------------------------------------------------------------

TEST(ParticlesFade, ExactFadeTable) {
  using Particle = ParticleSystem<laige::sim::Fpx16_16>::Particle;
  // life 8, fade 4, tint.a 255: remaining = 8 - age.
  Particle p;
  p.life = 8;
  p.fadeTicks = 4;
  p.tint[0] = 9;
  p.tint[1] = 9;
  p.tint[2] = 9;
  p.tint[3] = 255;
  const std::uint8_t expected[8] = {255, 255, 255, 255, 255, 191, 127, 63};
  for (std::uint32_t age = 0; age < 8; ++age) {
    p.age = age;
    EXPECT_EQ(ParticleSystem<laige::sim::Fpx16_16>::fadeAlpha(p),
              expected[age])
        << "age " << age;
  }
  // tint.a 200: 200*3/4 = 150, 200*2/4 = 100, 200/4 = 50.
  p.tint[3] = 200;
  p.age = 5;
  EXPECT_EQ(ParticleSystem<laige::sim::Fpx16_16>::fadeAlpha(p), 150);
  p.age = 6;
  EXPECT_EQ(ParticleSystem<laige::sim::Fpx16_16>::fadeAlpha(p), 100);
  p.age = 7;
  EXPECT_EQ(ParticleSystem<laige::sim::Fpx16_16>::fadeAlpha(p), 50);
  // fadeTicks 0: no fade — the alpha is constant.
  p.fadeTicks = 0;
  for (std::uint32_t age = 0; age < 8; ++age) {
    p.age = age;
    EXPECT_EQ(ParticleSystem<laige::sim::Fpx16_16>::fadeAlpha(p), 200)
        << "age " << age;
  }
}

template <typename Backend>
void fadeThroughSystem() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({4, 1, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  auto d = degDef<Backend>(0, 0, 0, 0, 0, 8, 1, 255, 4);
  auto id = sys.addEmitter(d);
  ASSERT_TRUE(id.ok());
  ASSERT_TRUE(sys.burst(*id.valueIfOk(), 1).ok());
  const std::uint8_t expected[8] = {255, 255, 255, 255, 255, 191, 127, 63};
  for (int k = 0; k < 8; ++k) {
    const auto live = sys.liveParticles();
    ASSERT_EQ(live.size(), 1u) << "tick " << k;
    EXPECT_EQ(Sys::fadeAlpha(live[0]), expected[k]) << "tick " << k;
    sys.update();  // age k+1; the 8th update kills (age 8 >= life 8)
  }
  EXPECT_EQ(sys.liveCount(), 0u);
}

TEST(ParticlesFade, FadeThroughSystem) {
  fadeThroughSystem<laige::sim::Fpx16_16>();
  fadeThroughSystem<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesPoolExhaustion
// ---------------------------------------------------------------------------

template <typename Backend>
void exactExhaustionTable() {
  using Sys = ParticleSystem<Backend>;
  // capacity 3, one emitter (rate 2, life 4): the hand-computed
  // per-tick table (the header's per-tick contract: advance -> emit).
  // A cohort emitted at tick T is live at the end of ticks T..T+3 and
  // dies at the end of T+4 — the pool is full from tick 2 on (2
  // deaths per cohort tick exactly refill the dropped slots, except
  // where the odd live count leaves a drop).
  auto r = Sys::create({3, 1, 42});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  auto d = degDef<Backend>(0, 0, 0, 1, 0, 4, 1, 255, 0);
  d.continuousRate = 2;
  auto id = sys.addEmitter(d);
  ASSERT_TRUE(id.ok());
  const std::uint32_t liveExpected[10] = {2, 3, 3, 3, 3, 3, 3, 3, 3, 3};
  const std::uint32_t droppedExpected[10] = {0, 1, 3, 5, 5, 6, 8, 10, 10, 11};
  // CUMULATIVE warn count (one per tick where drops occur: ticks
  // 2, 3, 4, 6, 7, 8, 10 — 7 total).
  const std::uint32_t warnsExpected[10] = {0, 1, 2, 3, 3, 4, 5, 6, 6, 7};
  MemorySink* sink = installSink();
  for (int t = 0; t < 10; ++t) {
    sys.update();
    EXPECT_EQ(sys.liveCount(), liveExpected[t]) << "tick " << (t + 1);
    EXPECT_EQ(sys.droppedTotal(), droppedExpected[t]) << "tick " << (t + 1);
    EXPECT_EQ(countEvents(*sink, "particles", "pool_overflow"),
              warnsExpected[t])
        << "tick " << (t + 1) << " (at most one warn per tick)";
  }
  restoreLogger();
}

TEST(ParticlesPoolExhaustion, ExactTable) {
  exactExhaustionTable<laige::sim::Fpx16_16>();
  exactExhaustionTable<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesStats
// ---------------------------------------------------------------------------

template <typename Backend>
void profilerFeed() {
  using Sys = ParticleSystem<Backend>;
  auto r = Sys::create({4, 1, 0});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  auto d = degDef<Backend>(0, 0, 0, 1, 0, 3, 1, 255, 0);
  d.continuousRate = 2;
  auto id = sys.addEmitter(d);
  ASSERT_TRUE(id.ok());
  // rate 2, life 3, capacity 4: t1 live 2 / t2 live 4 / t3 +2 drops /
  // t4 the t1 cohort dies, +2 spawn -> live 4 / t5 the t2 cohort dies,
  // +2 spawn -> live 4. 10 attempts, 2 drops -> 8 successful spawns.
  for (int t = 0; t < 5; ++t) {
    sys.update();
  }
  const auto stats = sys.stats();
  EXPECT_EQ(stats.live, 4u);
  EXPECT_EQ(stats.capacity, 4u);
  EXPECT_EQ(stats.spawnedTotal, 8u);
  EXPECT_EQ(stats.droppedTotal, 2u);
}

TEST(ParticlesStats, ProfilerFeed) {
  profilerFeed<laige::sim::Fpx16_16>();
  profilerFeed<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticlesZeroAlloc
// ---------------------------------------------------------------------------

template <typename Backend>
void updateLoopAllocatesNothing() {
  using Sys = ParticleSystem<Backend>;
  // capacity 512, two continuous emitters (rates 4 + 2, life 42 —
  // steady state 252 live, no drops) + a burst of 8 every 50 ticks:
  // 500 ticks of the full update path allocate nothing (PERF-003).
  auto r = Sys::create({512, 2, 13});
  ASSERT_TRUE(r.ok());
  auto sys = std::move(r).takeValue();
  auto e1 = sys.addEmitter(degDef<Backend>(0, 0, 0, 1, 0, 42, 1, 255, 0));
  auto e2 = sys.addEmitter(degDef<Backend>(1, 0, 1, -1, 1, 42, 1, 255, 0));
  ASSERT_TRUE(e1.ok());
  ASSERT_TRUE(e2.ok());
  auto runTicks = [&]() {
    for (int t = 0; t < 500; ++t) {
      ASSERT_TRUE(sys.burst(1, 4).ok()) << "tick " << t;
      ASSERT_TRUE(sys.burst(2, 2).ok()) << "tick " << t;
      sys.update();
    }
  };
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();  // the watch's owner is THIS thread
    runTicks();
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "500 ticks of burst/update allocated " << reading.allocs
        << " heap blocks on the loop thread";
    printf("particles-zeroalloc ticks=500 allocs=%llu\n",
           static_cast<unsigned long long>(reading.allocs));
  } else {
    runTicks();  // sanitizer tree: the leak-free run covers it
  }
  // The counters agree with the no-drop arithmetic: 500 x 6 spawns;
  // a particle bursted at tick k is live at the end of tick 500 iff
  // k >= 460 (it dies at the end of tick k + life - 1 = k + 41) ->
  // 41 cohorts x 6.
  EXPECT_EQ(sys.spawnedTotal(), 500u * 6u);
  EXPECT_EQ(sys.droppedTotal(), 0u);
  EXPECT_EQ(sys.liveCount(), 41u * 6u);
}

TEST(ParticlesZeroAlloc, UpdateLoopAllocatesNothing) {
  updateLoopAllocatesNothing<laige::sim::Fpx16_16>();
  updateLoopAllocatesNothing<laige::sim::Fp32Pinned>();
}

}  // namespace
