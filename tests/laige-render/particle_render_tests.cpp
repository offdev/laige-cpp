// M2-PART-02: particle rendering tests (the step's Verify command:
// `ctest -R particle_render`).
//
// The declareParticles contract pinned against: the per-particle
// declaration goldens on both backends (the world position / square
// scale / tint + fade alpha / UV / atlas / blend / layer fields, the
// engine-owned depth key against the M2-ISO-01 oracle + the
// hand-computed dyadic goldens), the depth conversion (the sub-unit
// ties-to-even rounding, the packing-boundary saturation), the exact
// color fade through the declaration, the in-group (key, live-order)
// ordering, the multi-emitter-set group count (one draw call per
// emitter set, RENDER-001), the failure paths (stopped batcher,
// closed window, the overflow policy), the determinism contract
// (fixed seed -> identical declared (key, position) sequence, the
// machine-greppable state hash), the 1 000-frame zero-allocation
// declare loop (PERF-003), and the PRD §8.1-style 10 000-particle
// conversion budget gate (LAIGE_PARTICLE_RENDER_BUDGET — gated on the
// Linux non-instrumented trees, ungated elsewhere).
//
// No GL needed — runs in every tree.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "laige/alloc_watch.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/logging.h"
#include "laige/render/iso_depth_key.h"
#include "laige/render/particle_render.h"
#include "laige/render/sprite_batcher.h"
#include "laige/result.h"
#include "laige/sim/particles.h"
#include "laige/sim_math.h"

namespace {

using laige::ParticleSystem;
using laige::ParticleEmitterDef;
using laige::render::BlendMode;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using laige::render::SpriteItem;
using laige::render::ParticleDeclareOptions;
using laige::render::declareParticles;
using laige::render::particleDepthToStepHeight;
using laige::render::kIsoDepthGroundLayer;

template <typename Backend>
using M_ = laige::sim::SimMath<Backend>;

template <typename Backend>
using P_ = ParticleSystem<Backend>::Particle;

// ---------------------------------------------------------------------------
// Backend-agnostic scalar construction (dyadic test values only)
// ---------------------------------------------------------------------------

template <typename Backend>
typename M_<Backend>::Scalar sc(std::int64_t num, std::int64_t den = 1) {
  using Scalar = typename M_<Backend>::Scalar;
  if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
    // Exact for the dyadic test values used here (integer division
    // truncation: the raw value num * 2^16 / den).
    return laige::fpx16_16{
        static_cast<std::int32_t>(num * (std::int64_t(1) << 16) / den)};
  } else {
    return static_cast<Scalar>(static_cast<float>(num) /
                               static_cast<float>(den));
  }
}

template <typename Backend>
const char* backendName() {
  if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
    return "fpx16_16";
  } else {
    return "fp32_pinned";
  }
}

// A fully degenerate emitter def (min == max everywhere): a burst
// from it is deterministic without any Prng value (the lerp
// endpoints coincide).
template <typename Backend>
ParticleEmitterDef<Backend> degenerateDef(std::int64_t ox, std::int64_t oy,
                                          std::int64_t depthNum,
                                          std::int64_t depthDen,
                                          std::uint32_t life,
                                          std::int64_t sizeNum,
                                          std::int64_t sizeDen,
                                          std::uint8_t r, std::uint8_t g,
                                          std::uint8_t b, std::uint8_t a) {
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> d;
  d.origin = Vec2{sc<Backend>(ox), sc<Backend>(oy)};
  d.depth = sc<Backend>(depthNum, depthDen);
  d.velMin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.velMax = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.lifeMin = life;
  d.lifeMax = life;
  d.sizeMin = sc<Backend>(sizeNum, sizeDen);
  d.sizeMax = sc<Backend>(sizeNum, sizeDen);
  d.tint[0] = r;
  d.tint[1] = g;
  d.tint[2] = b;
  d.tint[3] = a;
  d.fadeTicks = 0;
  d.continuousRate = 0;
  return d;
}

// The default options (the particle convention: the shared atlas 7,
// additive blend, the full atlas, the ground layer).
ParticleDeclareOptions baseOptions() {
  ParticleDeclareOptions o;
  o.atlasId = 7;
  o.materialId = 0;
  o.blend = BlendMode::Additive;
  o.uv = laige::render::SpriteUvRect{0.0f, 0.0f, 1.0f, 1.0f};
  o.layer = kIsoDepthGroundLayer;
  return o;
}

// The machine-greppable state hash (the docs/testing.md §4 KAT
// convention: FNV-1a 64, big-endian byte order per u64).
std::uint64_t particlesFnv1a64(const std::uint64_t* values, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis (FNV-1a spec)
  for (std::size_t i = 0; i < n; ++i) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (values[i] >> shift) & 0xFFull;
      h *= 0x10000001b3ull;  // FNV prime (FNV-1a spec)
    }
  }
  return h;
}

// The float bit pattern (the KAT hash's payload — the declared render
// position, exact per backend).
std::uint32_t floatBits(float v) {
  std::uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
}

// ---------------------------------------------------------------------------
// The budget report context helpers (the iso_depth_table_tests.cpp
// pattern — BudgetsFilePath is gated builds only; MachineLine and
// kCompilerId are unconditional: the run function's gated code is
// compiled in every tree, unreachable when ungated)
// ---------------------------------------------------------------------------

// The budgets.json path (the repo convention): LAIGE_BUDGETS_PATH,
// then "budgets.json" in the working directory (the ctest gate sets
// the env var to the repo root — tests/laige-render/CMakeLists.txt).
// Platform boundary (the budget_harness_tests.cpp pattern): MSVC
// deprecates plain getenv (C4996, fatal under /WX). Gated builds only
// (the ungated trees never load the table — the helper is defined
// only where it is used, -Wunused-function under -Werror):
#if defined(LAIGE_PARTICLE_RENDER_BUDGET)
std::string BudgetsFilePath() {
#if defined(_MSC_VER)
  constexpr std::size_t kMax = 4096;
  char buf[kMax];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "LAIGE_BUDGETS_PATH") != 0) {
    return std::string("budgets.json");
  }
  if (len == 0) return std::string("budgets.json");
  return std::string(buf, len);
#else
  const char* env = std::getenv("LAIGE_BUDGETS_PATH");
  return (env != nullptr && *env != '\0') ? std::string(env)
                                          : std::string("budgets.json");
#endif
}
#endif  // LAIGE_PARTICLE_RENDER_BUDGET

// The machine line for the budget report context (the laige-bench
// operator convention: LAIGE_BENCH_MACHINE, empty when unset — the
// baseline document records the machine facts):
std::string MachineLine() {
#if defined(_MSC_VER)
  constexpr std::size_t kMax = 4096;
  char buf[kMax];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "LAIGE_BENCH_MACHINE") != 0) {
    return std::string();
  }
  return std::string(buf, len);
#else
  const char* env = std::getenv("LAIGE_BENCH_MACHINE");
  return (env != nullptr) ? std::string(env) : std::string();
#endif
}

#define LAIGE_PARTICLE_STR2(x) #x
#define LAIGE_PARTICLE_STR(x) LAIGE_PARTICLE_STR2(x)
#if defined(__clang__)
constexpr char kCompilerId[] = "Clang " LAIGE_PARTICLE_STR(__clang_major__)
    "." LAIGE_PARTICLE_STR(__clang_minor__) "."
    LAIGE_PARTICLE_STR(__clang_patchlevel__);
#elif defined(__GNUC__)
constexpr char kCompilerId[] = "GCC " __VERSION__;
#elif defined(_MSC_VER)
constexpr char kCompilerId[] = "MSVC " LAIGE_PARTICLE_STR(_MSC_FULL_VER);
#else
constexpr char kCompilerId[] = "unknown compiler";
#endif

// ---------------------------------------------------------------------------
// ParticleRenderDeclare — the per-particle declaration goldens
// ---------------------------------------------------------------------------

// The hand-computed dyadic goldens (layer 0: key = 0x80200000 + d,
// d = round((x + y) * 16) - z * 16 — the M2-ISO-01 formula):
//
//   A (1, 2) depth 3     d =  48 - 48  =    0  -> 0x80200000
//   B (0, 0) depth 0     d =   0 -  0  =    0  -> 0x80200000 (ties A)
//   C (2, 2) depth 1     d =  64 - 16  =   48  -> 0x80200030
//   D (0, 0) depth 2     d =   0 - 32  =  -32  -> 0x801FFFE0
//   E (1, 1) depth 0.5   z = 0 (ties-to-even), d = 32 -> 0x80200020
//   F (1, 1) depth 1.5   z = 2 (ties-to-even), d = 0  -> 0x80200000
//
// Expected in-group order (back-to-front, equal keys keep spawn
// order A, B, C, D, E, F): D, A, B, F, E, C.
template <typename Backend>
void declareGolden() {
  using Vec2 = typename M_<Backend>::Vec2;
  constexpr std::uint32_t kSeed = 1;
  auto rc = ParticleSystem<Backend>::create({16, 6, kSeed});
  ASSERT_TRUE(rc.ok());
  auto sys = std::move(rc).takeValue();
  const auto eA = sys.addEmitter(degenerateDef<Backend>(1, 2, 3, 1, 4, 1, 2,
                                                         255, 128, 64, 200));
  const auto eB = sys.addEmitter(degenerateDef<Backend>(0, 0, 0, 1, 4, 1, 1,
                                                         10, 20, 30, 40));
  const auto eC = sys.addEmitter(degenerateDef<Backend>(2, 2, 1, 1, 4, 1, 4,
                                                         255, 0, 0, 255));
  const auto eD = sys.addEmitter(degenerateDef<Backend>(0, 0, 2, 1, 4, 1, 1,
                                                         0, 255, 255, 128));
  const auto eE = sys.addEmitter(degenerateDef<Backend>(1, 1, 1, 2, 2, 1, 1,
                                                         1, 2, 3, 4));
  const auto eF = sys.addEmitter(degenerateDef<Backend>(1, 1, 3, 2, 4, 2, 1,
                                                         7, 7, 7, 255));
  ASSERT_TRUE(eA.ok());
  ASSERT_TRUE(eB.ok());
  ASSERT_TRUE(eC.ok());
  ASSERT_TRUE(eD.ok());
  ASSERT_TRUE(eE.ok());
  ASSERT_TRUE(eF.ok());
  ASSERT_TRUE(sys.burst(eA.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eB.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eC.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eD.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eE.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eF.value(), 1).ok());
  ASSERT_EQ(sys.liveCount(), 6u);

  auto bc = SpriteBatcher::create({16});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  const ParticleDeclareOptions opts = baseOptions();
  ASSERT_TRUE(declareParticles(batcher, sys, opts).ok());
  ASSERT_EQ(batcher.frameCount(), 6u);
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_TRUE(batcher.frameBuilt());
  // One emitter set -> ONE (atlas, material, blend) group -> one draw
  // call (RENDER-001, FR-2.7).
  ASSERT_EQ(batcher.batchCount(), 1u);
  const SpriteBatch& batch = batcher.batches().front();
  EXPECT_EQ(7u, batch.atlasId);
  EXPECT_EQ(0u, batch.materialId);
  EXPECT_EQ(BlendMode::Additive, batch.blend);
  ASSERT_EQ(6u, batch.instances.size());

  // The expected (key, origin, depth-int) table in the documented
  // in-group order (back-to-front, equal keys keep spawn order):
  struct Expected {
    std::uint32_t key;
    Vec2 origin;
    std::int32_t depthInt;
    float size;
    float tint[4];
  };
  const Expected kExpected[6] = {
      {0x801FFFE0u, Vec2{sc<Backend>(0), sc<Backend>(0)}, 2, 1.0f,
       {0.0f, static_cast<float>(255) / 255.0f, static_cast<float>(255) / 255.0f,
        static_cast<float>(128) / 255.0f}},
      {0x80200000u, Vec2{sc<Backend>(1), sc<Backend>(2)}, 3, 0.5f,
       {1.0f, static_cast<float>(128) / 255.0f, static_cast<float>(64) / 255.0f,
        static_cast<float>(200) / 255.0f}},
      {0x80200000u, Vec2{sc<Backend>(0), sc<Backend>(0)}, 0, 1.0f,
       {static_cast<float>(10) / 255.0f, static_cast<float>(20) / 255.0f,
        static_cast<float>(30) / 255.0f, static_cast<float>(40) / 255.0f}},
      {0x80200000u, Vec2{sc<Backend>(1), sc<Backend>(1)}, 2, 2.0f,
       {static_cast<float>(7) / 255.0f, static_cast<float>(7) / 255.0f,
        static_cast<float>(7) / 255.0f, 1.0f}},
      {0x80200020u, Vec2{sc<Backend>(1), sc<Backend>(1)}, 0, 1.0f,
       {static_cast<float>(1) / 255.0f, static_cast<float>(2) / 255.0f,
        static_cast<float>(3) / 255.0f, static_cast<float>(4) / 255.0f}},
      {0x80200030u, Vec2{sc<Backend>(2), sc<Backend>(2)}, 1, 0.25f,
       {1.0f, 0.0f, 0.0f, 1.0f}},
  };
  for (std::size_t i = 0; i < batch.instances.size(); ++i) {
    const SpriteItem& item = batcher.at(batch.instances[i]);
    const Expected& e = kExpected[i];
    EXPECT_EQ(e.key, item.depthKey)
        << "instance " << i << " (key " << item.depthKey << " vs expected "
        << e.key << ")";
    // The engine-owned key == the M2-ISO-01 oracle on the same input:
    EXPECT_EQ(laige::render::isoDepthKey<Backend>(e.origin, e.depthInt,
                                                  kIsoDepthGroundLayer),
              item.depthKey);
    EXPECT_FALSE(item.depthOverride);
    EXPECT_EQ(e.size, item.scale.x);
    EXPECT_EQ(e.size, item.scale.y);
    const float itemTint[4] = {item.tint.r, item.tint.g, item.tint.b,
                               item.tint.a};
    for (int c = 0; c < 4; ++c) {
      EXPECT_FLOAT_EQ(e.tint[c], itemTint[c]) << "instance " << i
                                              << " tint channel " << c;
    }
    EXPECT_EQ(0.0f, item.rotation);
    EXPECT_EQ(0u, item.frameIndex);
    // Field-wise (SpriteUvRect has no operator== — the parallax
    // convention):
    EXPECT_FLOAT_EQ(opts.uv.u0, item.uv.u0);
    EXPECT_FLOAT_EQ(opts.uv.v0, item.uv.v0);
    EXPECT_FLOAT_EQ(opts.uv.u1, item.uv.u1);
    EXPECT_FLOAT_EQ(opts.uv.v1, item.uv.v1);
    EXPECT_EQ(7u, item.atlasId);
    EXPECT_EQ(0u, item.materialId);
    EXPECT_EQ(BlendMode::Additive, item.blend);
    // The world position at the RENDER-006 boundary (one documented
    // rounding per backend — exact here: dyadic in the exactness zone).
    if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
      EXPECT_FLOAT_EQ(laige::fpx16_16::toFloat(e.origin.x), item.pos.x);
      EXPECT_FLOAT_EQ(laige::fpx16_16::toFloat(e.origin.y), item.pos.y);
    } else {
      EXPECT_FLOAT_EQ(e.origin.x, item.pos.x);
      EXPECT_FLOAT_EQ(e.origin.y, item.pos.y);
    }
  }
}

TEST(ParticleRenderDeclare, GoldenFields) {
  declareGolden<laige::sim::Fpx16_16>();
  declareGolden<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderDepth — the depth conversion (RENDER-006)
// ---------------------------------------------------------------------------

template <typename Backend>
void depthConversion() {
  // The quantization table (dyadic values — the cross-backend exactness
  // zone: both backends must agree, the header's depth-conversion
  // section):
  //   3 -> 3, 0.5 -> 0 (ties-to-even), 1.5 -> 2, -0.5 -> 0,
  //   -1.5 -> -2, 2046.5 -> 2046 (ties-to-even), 3000 -> 2047
  //   (clamped), -3000 -> -2047 (clamped), 0 -> 0, 2047 -> 2047.
  struct Case {
    std::int64_t num;
    std::int64_t den;
    std::int32_t expect;
  };
  const Case kCases[] = {
      {3, 1, 3}, {1, 2, 0}, {3, 2, 2}, {-1, 2, 0}, {-3, 2, -2},
      {4093, 2, 2046}, {3000, 1, 2047}, {-3000, 1, -2047}, {0, 1, 0},
      {2047, 1, 2047},
  };
  for (const Case& c : kCases) {
    EXPECT_EQ(c.expect, particleDepthToStepHeight<Backend>(sc<Backend>(c.num, c.den)))
        << backendName<Backend>() << " depth " << c.num << "/" << c.den;
  }

  // Through the declaration: a depth-0.5 particle renders at z = 0
  // (the tie-to-even rounding) and a depth-1.5 particle at z = 2.
  auto rc = ParticleSystem<Backend>::create({4, 2, 42});
  ASSERT_TRUE(rc.ok());
  auto sys = std::move(rc).takeValue();
  const auto eHalf =
      sys.addEmitter(degenerateDef<Backend>(0, 0, 1, 2, 2, 1, 1, 255, 255, 255, 255));
  const auto eOneHalf =
      sys.addEmitter(degenerateDef<Backend>(0, 0, 3, 2, 2, 1, 1, 255, 255, 255, 255));
  ASSERT_TRUE(eHalf.ok());
  ASSERT_TRUE(eOneHalf.ok());
  ASSERT_TRUE(sys.burst(eHalf.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eOneHalf.value(), 1).ok());
  auto bc = SpriteBatcher::create({4});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareParticles(batcher, sys, baseOptions()).ok());
  ASSERT_TRUE(batcher.build().ok());
  // The exact keys (hand-computed: (0, 0) z = 0 -> 0x80200000; z = 2 ->
  // 0x80200000 - 32 = 0x801FFFE0). The z = 2 particle is back-most (d =
  // -32 < d = 0) so it sorts BEFORE the z = 0 one:
  const SpriteBatch& batch = batcher.batches().front();
  ASSERT_EQ(2u, batch.instances.size());
  EXPECT_EQ(0x801FFFE0u, batcher.at(batch.instances[0]).depthKey);
  EXPECT_EQ(0x80200000u, batcher.at(batch.instances[1]).depthKey);
}

TEST(ParticleRenderDepth, Conversion) {
  depthConversion<laige::sim::Fpx16_16>();
  depthConversion<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderFade — the exact color fade through the declaration
// ---------------------------------------------------------------------------

template <typename Backend>
void fadeGolden() {
  // life 8, fadeTicks 4, tint.a 255: a particle with `remaining` ticks
  // left renders alpha = remaining >= fadeTicks ? 255 : 255 *
  // remaining / fadeTicks (exact u32, M2-PART-01). Eight burst
  // particles, declared once per tick (before each update):
  //   age 0..7 -> remaining 8..1 -> alpha {255, 255, 255, 255, 255,
  //   191, 127, 63}.
  constexpr std::uint8_t kExpected[8] = {255, 255, 255, 255, 255, 191, 127,
                                         63};
  auto rc = ParticleSystem<Backend>::create({8, 1, 7});
  ASSERT_TRUE(rc.ok());
  auto sys = std::move(rc).takeValue();
  // The fade def (fadeTicks 4 — degenerateDef's fade is always 0):
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> d;
  d.origin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.depth = sc<Backend>(0);
  d.velMin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.velMax = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.lifeMin = 8;
  d.lifeMax = 8;
  d.sizeMin = sc<Backend>(1, 2);
  d.sizeMax = sc<Backend>(1, 2);
  d.tint[0] = 255;
  d.tint[1] = 0;
  d.tint[2] = 255;
  d.tint[3] = 255;
  d.fadeTicks = 4;
  d.continuousRate = 0;
  const auto e = sys.addEmitter(d);
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(sys.burst(e.value(), 8).ok());
  auto bc = SpriteBatcher::create({8});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  for (std::uint32_t k = 0; k < 8; ++k) {
    batcher.beginFrame();
    ASSERT_TRUE(declareParticles(batcher, sys, baseOptions()).ok());
    ASSERT_TRUE(batcher.build().ok());
    ASSERT_EQ(8u, batcher.frameCount());
    const SpriteBatch& batch = batcher.batches().front();
    for (std::size_t i = 0; i < batch.instances.size(); ++i) {
      const SpriteItem& item = batcher.at(batch.instances[i]);
      EXPECT_FLOAT_EQ(static_cast<float>(kExpected[k]) / 255.0f, item.tint.a)
          << "age " << k;
      // The RGB channels are constant (the fade touches alpha only):
      EXPECT_FLOAT_EQ(1.0f, item.tint.r);
      EXPECT_FLOAT_EQ(0.0f, item.tint.g);
      EXPECT_FLOAT_EQ(1.0f, item.tint.b);
    }
    sys.update();
  }
  // After the 8th update all particles are dead: the declaration is a
  // no-op success (the empty span declares nothing).
  batcher.beginFrame();
  ASSERT_TRUE(declareParticles(batcher, sys, baseOptions()).ok());
  EXPECT_EQ(0u, batcher.frameCount());
  ASSERT_TRUE(batcher.build().ok());
  EXPECT_EQ(0u, batcher.batchCount());
}

TEST(ParticleRenderFade, ExactAlpha) {
  fadeGolden<laige::sim::Fpx16_16>();
  fadeGolden<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderOrder — the in-group (key, live-order) total order
// ---------------------------------------------------------------------------

template <typename Backend>
void orderGolden() {
  // Spawn order A, B, C, D (the live order — the tie-break):
  //   A (1, 2) depth 3 -> 0x80200000
  //   B (0, 0) depth 0 -> 0x80200000 (ties A: spawn order keeps A first)
  //   C (2, 2) depth 1 -> 0x80200030
  //   D (0, 0) depth 2 -> 0x801FFFE0 (back-most: drawn first)
  // Expected in-group order: D, A, B, C.
  auto rc = ParticleSystem<Backend>::create({4, 4, 9});
  ASSERT_TRUE(rc.ok());
  auto sys = std::move(rc).takeValue();
  const auto eA = sys.addEmitter(degenerateDef<Backend>(1, 2, 3, 1, 4, 1, 1, 255, 255, 255, 255));
  const auto eB = sys.addEmitter(degenerateDef<Backend>(0, 0, 0, 1, 4, 1, 1, 255, 255, 255, 255));
  const auto eC = sys.addEmitter(degenerateDef<Backend>(2, 2, 1, 1, 4, 1, 1, 255, 255, 255, 255));
  const auto eD = sys.addEmitter(degenerateDef<Backend>(0, 0, 2, 1, 4, 1, 1, 255, 255, 255, 255));
  ASSERT_TRUE(eA.ok());
  ASSERT_TRUE(eB.ok());
  ASSERT_TRUE(eC.ok());
  ASSERT_TRUE(eD.ok());
  ASSERT_TRUE(sys.burst(eA.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eB.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eC.value(), 1).ok());
  ASSERT_TRUE(sys.burst(eD.value(), 1).ok());
  auto bc = SpriteBatcher::create({4});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareParticles(batcher, sys, baseOptions()).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(1u, batcher.batchCount());
  const SpriteBatch& batch = batcher.batches().front();
  ASSERT_EQ(4u, batch.instances.size());
  // The keys back-to-front (non-decreasing, the M2-SORT-01 contract):
  const std::uint32_t kExpectedKeys[4] = {0x801FFFE0u, 0x80200000u,
                                          0x80200000u, 0x80200030u};
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(kExpectedKeys[i], batcher.at(batch.instances[i]).depthKey);
  }
  // The tie (A, B at 0x80200000) keeps spawn order: A's origin (1, 2)
  // before B's (0, 0).
  const SpriteItem& tieA = batcher.at(batch.instances[1]);
  const SpriteItem& tieB = batcher.at(batch.instances[2]);
  if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
    EXPECT_FLOAT_EQ(laige::fpx16_16::toFloat(sc<Backend>(1)), tieA.pos.x);
    EXPECT_FLOAT_EQ(laige::fpx16_16::toFloat(sc<Backend>(0)), tieB.pos.x);
  } else {
    EXPECT_FLOAT_EQ(1.0f, tieA.pos.x);
    EXPECT_FLOAT_EQ(0.0f, tieB.pos.x);
  }
}

TEST(ParticleRenderOrder, KeyLiveOrder) {
  orderGolden<laige::sim::Fpx16_16>();
  orderGolden<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderEmitterSets — one (atlas, material, blend) group per
// emitter set (one draw call per set, RENDER-001)
// ---------------------------------------------------------------------------

template <typename Backend>
void emitterSets() {
  auto rcA = ParticleSystem<Backend>::create({8, 1, 101});
  auto rcB = ParticleSystem<Backend>::create({8, 1, 102});
  auto rcC = ParticleSystem<Backend>::create({8, 1, 103});
  ASSERT_TRUE(rcA.ok());
  ASSERT_TRUE(rcB.ok());
  ASSERT_TRUE(rcC.ok());
  auto sysA = std::move(rcA).takeValue();
  auto sysB = std::move(rcB).takeValue();
  auto sysC = std::move(rcC).takeValue();
  const auto eA = sysA.addEmitter(degenerateDef<Backend>(0, 0, 0, 1, 4, 1, 1, 255, 255, 255, 255));
  const auto eB = sysB.addEmitter(degenerateDef<Backend>(3, 0, 1, 1, 4, 1, 1, 255, 255, 255, 255));
  const auto eC = sysC.addEmitter(degenerateDef<Backend>(0, 3, 0, 1, 4, 1, 1, 255, 255, 255, 255));
  ASSERT_TRUE(eA.ok());
  ASSERT_TRUE(eB.ok());
  ASSERT_TRUE(eC.ok());
  ASSERT_TRUE(sysA.burst(eA.value(), 2).ok());
  ASSERT_TRUE(sysB.burst(eB.value(), 3).ok());
  ASSERT_TRUE(sysC.burst(eC.value(), 1).ok());
  // Three sets: (atlas 5, Additive), (atlas 5, Alpha), (atlas 9,
  // Additive) — the group order is ascending (atlas, material,
  // blend): (5, Alpha), (5, Additive), (9, Additive).
  ParticleDeclareOptions optsA = baseOptions();
  optsA.atlasId = 5;
  ParticleDeclareOptions optsB = baseOptions();
  optsB.atlasId = 5;
  optsB.blend = BlendMode::Alpha;
  ParticleDeclareOptions optsC = baseOptions();
  optsC.atlasId = 9;
  auto bc = SpriteBatcher::create({8});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(declareParticles(batcher, sysA, optsA).ok());
  ASSERT_TRUE(declareParticles(batcher, sysB, optsB).ok());
  ASSERT_TRUE(declareParticles(batcher, sysC, optsC).ok());
  ASSERT_TRUE(batcher.build().ok());
  ASSERT_EQ(3u, batcher.batchCount());
  const auto batches = batcher.batches();
  EXPECT_EQ(5u, batches[0].atlasId);
  EXPECT_EQ(BlendMode::Alpha, batches[0].blend);
  EXPECT_EQ(3u, batches[0].instances.size());
  EXPECT_EQ(5u, batches[1].atlasId);
  EXPECT_EQ(BlendMode::Additive, batches[1].blend);
  EXPECT_EQ(2u, batches[1].instances.size());
  EXPECT_EQ(9u, batches[2].atlasId);
  EXPECT_EQ(BlendMode::Additive, batches[2].blend);
  EXPECT_EQ(1u, batches[2].instances.size());
}

TEST(ParticleRenderEmitterSets, OneGroupPerSet) {
  emitterSets<laige::sim::Fpx16_16>();
  emitterSets<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderFailure — the failure paths
// ---------------------------------------------------------------------------

template <typename Backend>
void failurePaths() {
  // A one-particle system (the non-empty case).
  auto rc = ParticleSystem<Backend>::create({4, 1, 55});
  ASSERT_TRUE(rc.ok());
  auto sys = std::move(rc).takeValue();
  const auto e = sys.addEmitter(degenerateDef<Backend>(0, 0, 0, 1, 4, 1, 1, 255, 255, 255, 255));
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(sys.burst(e.value(), 1).ok());
  // The stopped batcher (default-constructed, capacity 0): the first
  // add fails with BudgetExhausted (the batcher's stopped-state
  // contract).
  SpriteBatcher stopped;
  const auto rStopped = declareParticles(stopped, sys, baseOptions());
  EXPECT_FALSE(rStopped.ok());
  EXPECT_EQ(laige::ErrorCode::BudgetExhausted, rStopped.error());
  // The closed window (build already ran): InvalidArgument (the
  // batcher's frame-protocol contract).
  auto bc = SpriteBatcher::create({4});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  batcher.beginFrame();
  ASSERT_TRUE(batcher.build().ok());  // closes the window
  const auto rClosed = declareParticles(batcher, sys, baseOptions());
  EXPECT_FALSE(rClosed.ok());
  EXPECT_EQ(laige::ErrorCode::InvalidArgument, rClosed.error());
  // The batcher's overflow policy: a capacity-2 frame with 3
  // particles drops the OLDEST declaration (the batcher's own
  // rate-limited warn) — declareParticles itself succeeds (no failed
  // add; the overflow is a batcher-internal policy).
  auto rc3 = ParticleSystem<Backend>::create({4, 1, 56});
  ASSERT_TRUE(rc3.ok());
  auto sys3 = std::move(rc3).takeValue();
  const auto e3 = sys3.addEmitter(degenerateDef<Backend>(0, 0, 0, 1, 4, 1, 1, 255, 255, 255, 255));
  ASSERT_TRUE(e3.ok());
  ASSERT_TRUE(sys3.burst(e3.value(), 3).ok());
  auto bc2 = SpriteBatcher::create({2});
  ASSERT_TRUE(bc2.ok());
  auto small = std::move(bc2).takeValue();
  small.beginFrame();
  ASSERT_TRUE(declareParticles(small, sys3, baseOptions()).ok());
  EXPECT_EQ(2u, small.frameCount());
}

TEST(ParticleRenderFailure, Paths) {
  failurePaths<laige::sim::Fpx16_16>();
  failurePaths<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderDeterminism — fixed seed -> identical declared sequence
// ---------------------------------------------------------------------------

// Builds a two-emitter system with a velocity box (non-degenerate —
// the Prng draws the positions) and bursts 5 particles per emitter.
template <typename Backend>
ParticleSystem<Backend> buildSeededSystem(std::uint64_t seed) {
  auto rc = ParticleSystem<Backend>::create({64, 2, seed});
  if (!rc.ok()) {
    std::fprintf(stderr, "buildSeededSystem: create failed\n");
    std::abort();
  }
  auto sys = std::move(rc).takeValue();
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> d;
  d.origin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.depth = sc<Backend>(0, 2);
  d.velMin = Vec2{sc<Backend>(1, 4), sc<Backend>(1, 4)};
  d.velMax = Vec2{sc<Backend>(1, 2), sc<Backend>(1, 2)};
  d.lifeMin = 20;
  d.lifeMax = 40;
  d.sizeMin = sc<Backend>(1, 4);
  d.sizeMax = sc<Backend>(1, 2);
  d.tint[0] = 255;
  d.tint[1] = 128;
  d.tint[2] = 64;
  d.tint[3] = 255;
  d.fadeTicks = 8;
  d.continuousRate = 0;
  auto e1 = sys.addEmitter(d);
  if (!e1.ok()) {
    std::fprintf(stderr, "buildSeededSystem: addEmitter failed\n");
    std::abort();
  }
  d.origin = Vec2{sc<Backend>(5), sc<Backend>(-2)};
  d.depth = sc<Backend>(2);
  auto e2 = sys.addEmitter(d);
  if (!e2.ok()) {
    std::fprintf(stderr, "buildSeededSystem: addEmitter failed\n");
    std::abort();
  }
  if (!sys.burst(e1.value(), 5).ok() || !sys.burst(e2.value(), 5).ok()) {
    std::fprintf(stderr, "buildSeededSystem: burst failed\n");
    std::abort();
  }
  for (int i = 0; i < 3; ++i) sys.update();
  return sys;
}

// Hashes one frame's declared (key, pos.x, pos.y) sequence — the
// machine-greppable render-state fingerprint (docs/testing.md §4).
template <typename Backend>
std::uint64_t declaredStateHash(const ParticleSystem<Backend>& sys,
                                SpriteBatcher& batcher) {
  batcher.beginFrame();
  const auto r = declareParticles(batcher, sys, baseOptions());
  if (!r.ok()) {
    std::fprintf(stderr, "declaredStateHash: declare failed\n");
    std::abort();
  }
  if (!batcher.build().ok()) {
    std::fprintf(stderr, "declaredStateHash: build failed\n");
    std::abort();
  }
  // (the declared sequence — one key + two float positions per
  // particle, in the frame's rendered order)
  std::vector<std::uint64_t> payload;
  payload.reserve(batcher.frameCount() * 3);
  const auto batches = batcher.batches();
  for (const SpriteBatch& batch : batches) {
    for (std::uint32_t slot : batch.instances) {
      const SpriteItem& item = batcher.at(slot);
      payload.push_back(item.depthKey);
      payload.push_back(floatBits(item.pos.x));
      payload.push_back(floatBits(item.pos.y));
    }
  }
  return particlesFnv1a64(payload.data(), payload.size());
}

template <typename Backend>
void determinism() {
  constexpr std::uint64_t kSeed = 0xC0FFEE11ull;
  auto sys1 = buildSeededSystem<Backend>(kSeed);
  auto sys2 = buildSeededSystem<Backend>(kSeed);
  auto sys3 = buildSeededSystem<Backend>(kSeed + 1);
  auto bc = SpriteBatcher::create({64});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  const std::uint64_t h1 = declaredStateHash<Backend>(sys1, batcher);
  const std::uint64_t h2 = declaredStateHash<Backend>(sys2, batcher);
  const std::uint64_t h3 = declaredStateHash<Backend>(sys3, batcher);
  std::printf("particle-render-determinism backend=%s seed=0x%016llx "
              "live=%llu fnv1a=0x%016llx\n",
              backendName<Backend>(),
              static_cast<unsigned long long>(kSeed),
              static_cast<unsigned long long>(sys1.liveCount()),
              static_cast<unsigned long long>(h1));
  EXPECT_EQ(h1, h2);
  EXPECT_NE(h1, h3);  // a different seed diverges
}

TEST(ParticleRenderDeterminism, SeedStable) {
  determinism<laige::sim::Fpx16_16>();
  determinism<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderZeroAlloc — the per-frame declare loop allocates
// nothing (PERF-003 — FR-2.7 "pooled")
// ---------------------------------------------------------------------------

template <typename Backend>
void zeroAllocLoop() {
  constexpr std::uint32_t kFrames = 1000;
  constexpr std::uint32_t kParticles = 10000;
  auto rc = ParticleSystem<Backend>::create({kParticles, 1, 0xB00D});
  ASSERT_TRUE(rc.ok());
  auto sys = std::move(rc).takeValue();
  using Vec2 = typename M_<Backend>::Vec2;
  ParticleEmitterDef<Backend> d;
  d.origin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.depth = sc<Backend>(0, 2);
  d.velMin = Vec2{sc<Backend>(1, 4), sc<Backend>(1, 4)};
  d.velMax = Vec2{sc<Backend>(1, 2), sc<Backend>(1, 2)};
  d.lifeMin = 20;
  d.lifeMax = 40;
  d.sizeMin = sc<Backend>(1, 4);
  d.sizeMax = sc<Backend>(1, 2);
  d.tint[0] = 255;
  d.tint[1] = 128;
  d.tint[2] = 64;
  d.tint[3] = 255;
  d.fadeTicks = 8;
  d.continuousRate = 0;
  const auto e = sys.addEmitter(d);
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(sys.burst(e.value(), kParticles).ok());
  auto bc = SpriteBatcher::create({kParticles});
  ASSERT_TRUE(bc.ok());
  auto batcher = std::move(bc).takeValue();
  const ParticleDeclareOptions opts = baseOptions();
  auto runFrames = [&]() {
    for (std::uint32_t f = 0; f < kFrames; ++f) {
      batcher.beginFrame();
      if (!declareParticles(batcher, sys, opts).ok()) {
        std::fprintf(stderr, "zeroAllocLoop: declare failed\n");
        std::abort();
      }
      if (!batcher.build().ok()) {
        std::fprintf(stderr, "zeroAllocLoop: build failed\n");
        std::abort();
      }
    }
  };
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    runFrames();
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(0ull, reading.allocs)
        << kFrames << " frames of " << kParticles << "-particle declare "
        << "allocated " << reading.allocs << " heap blocks on the loop "
        << "thread";
    std::printf("particle-render-zeroalloc backend=%s frames=%u "
                "particles=%u allocs=%llu\n",
                backendName<Backend>(), kFrames, kParticles,
                static_cast<unsigned long long>(reading.allocs));
  } else {
    runFrames();  // sanitizer tree: the leak-free run covers it
  }
}

TEST(ParticleRenderZeroAlloc, DeclareLoopAllocatesNothing) {
  zeroAllocLoop<laige::sim::Fpx16_16>();
  zeroAllocLoop<laige::sim::Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// ParticleRenderBudget — the PRD §8.1-style 10k-particle conversion
// budget (the `particle_render_10k` entry)
// ---------------------------------------------------------------------------

// The workload shape (named constants — CORE-005):
constexpr std::uint32_t kBudgetParticles = 10000;
constexpr std::uint32_t kBudgetTicks = 10;  // the position spread
constexpr std::uint64_t kBudgetSeed = 0xC0FFEE00ull;
constexpr std::int32_t kBudgetWarmup = 100;
constexpr std::int32_t kBudgetRuns = 3000;

// One backend run of the budget workload: a 10 000-particle system
// (one emitter with a velocity box — the positions spread over the
// kBudgetTicks update ticks; all alive: min life 20 > 10), declared
// into a capacity-10 000 batcher per frame. `entry == nullptr`
// selects the UNGATED run (the sanitizer and non-reference
// platforms): the workload runs for its leak/correctness value, no
// timing. A non-null `entry` selects the GATED run: warm-up + the
// measured window (the O(live) conversion pass ONLY — the batcher's
// build/sort cost is the `depth_sort_10k` budget's territory) +
// budgetCheck; the result carries the report.
template <typename Backend>
laige::BudgetCheckResult runBudgetBackend(const laige::BudgetEntry* entry) {
  using Vec2 = typename M_<Backend>::Vec2;
  auto rc = ParticleSystem<Backend>::create({kBudgetParticles, 1,
                                             kBudgetSeed});
  if (!rc.ok()) {
    std::fprintf(stderr, "runBudgetBackend: create failed\n");
    std::abort();
  }
  auto sys = std::move(rc).takeValue();
  ParticleEmitterDef<Backend> d;
  d.origin = Vec2{sc<Backend>(0), sc<Backend>(0)};
  d.depth = sc<Backend>(0, 2);
  d.velMin = Vec2{sc<Backend>(1, 4), sc<Backend>(1, 4)};
  d.velMax = Vec2{sc<Backend>(1, 2), sc<Backend>(1, 2)};
  d.lifeMin = 20;
  d.lifeMax = 40;
  d.sizeMin = sc<Backend>(1, 4);
  d.sizeMax = sc<Backend>(1, 2);
  d.tint[0] = 255;
  d.tint[1] = 128;
  d.tint[2] = 64;
  d.tint[3] = 255;
  d.fadeTicks = 8;
  d.continuousRate = 0;
  const auto e = sys.addEmitter(d);
  if (!e.ok()) {
    std::fprintf(stderr, "runBudgetBackend: addEmitter failed\n");
    std::abort();
  }
  if (!sys.burst(e.value(), kBudgetParticles).ok()) {
    std::fprintf(stderr, "runBudgetBackend: burst failed\n");
    std::abort();
  }
  for (std::uint32_t t = 0; t < kBudgetTicks; ++t) sys.update();
  // The workload shape (CORE-005): all 10 000 must be alive (min life
  // 20 > kBudgetTicks) — a shape failure is a test failure:
  if (sys.liveCount() != kBudgetParticles) {
    std::fprintf(stderr, "runBudgetBackend: liveCount %u != %u after "
                        "the %u update ticks\n",
                 sys.liveCount(), kBudgetParticles, kBudgetTicks);
    std::abort();
  }
  auto bc = SpriteBatcher::create({kBudgetParticles});
  if (!bc.ok()) {
    std::fprintf(stderr, "runBudgetBackend: batcher create failed\n");
    std::abort();
  }
  auto batcher = std::move(bc).takeValue();
  const ParticleDeclareOptions opts = baseOptions();
  auto frame = [&]() {
    batcher.beginFrame();
    if (!declareParticles(batcher, sys, opts).ok()) {
      std::fprintf(stderr, "runBudgetBackend: declare failed\n");
      std::abort();
    }
    if (!batcher.build().ok()) {
      std::fprintf(stderr, "runBudgetBackend: build failed\n");
      std::abort();
    }
  };
  // The ungated run (leak/correctness only, no timing — entry is
  // always nullptr when ungated, so the gated code below compiles but
  // is unreachable there):
  if (entry == nullptr) {
    for (std::int32_t i = 0; i < kBudgetWarmup; ++i) frame();
    return {};
  }
  // The gated run: warm-up (first-touch costs) then the measured
  // window (the conversion pass ONLY — beginFrame/build are the
  // frame protocol, outside the measured quantity):
  for (std::int32_t i = 0; i < kBudgetWarmup; ++i) frame();
  laige::Histogram hist(
      laige::Histogram::Options{static_cast<std::size_t>(kBudgetRuns)});
  for (std::int32_t i = 0; i < kBudgetRuns; ++i) {
    batcher.beginFrame();
    laige::TimeIt timer;
    if (!declareParticles(batcher, sys, opts).ok()) {
      std::fprintf(stderr, "runBudgetBackend: declare failed\n");
      std::abort();
    }
    hist.record(timer.elapsedMs());
    if (!batcher.build().ok()) {
      std::fprintf(stderr, "runBudgetBackend: build failed\n");
      std::abort();
    }
  }
  // The AGENTS §12 context (stable pointers — the ctx fields are
  // const char*; the strings live to the end of the call):
  const std::string buildLine = std::string(kCompilerId) +
#if defined(LAIGE_PARTICLE_RENDER_BUILD_TYPE)
      ", CMake " LAIGE_PARTICLE_RENDER_BUILD_TYPE
#else
      ", CMake build type unknown"
#endif
      ", engine policy (NFR-8.10)";
  const std::string machine = MachineLine();
  laige::BudgetReportContext ctx;
  ctx.workload = entry->workload.c_str();
  ctx.build = buildLine.c_str();
  ctx.machine = machine.c_str();
  ctx.warmup = static_cast<std::uint32_t>(kBudgetWarmup);
  return laige::budgetCheck(*entry, hist, ctx);
}

TEST(ParticleRenderBudget, TenThousandParticles) {
  // The gated run loads the entry (the reference platform); the
  // ungated run passes a null entry (the workload still runs):
  const laige::BudgetEntry* entry = nullptr;
#if defined(LAIGE_PARTICLE_RENDER_BUDGET)
  const std::string path = BudgetsFilePath();
  const laige::Result<laige::BudgetTable, laige::ErrorCode> loaded =
      laige::loadBudgets(path);
  ASSERT_TRUE(loaded.ok()) << "loadBudgets(\"" << path << "\") failed: "
                           << laige::errorText(loaded.error());
  entry = loaded.value().find("particle_render_10k");
  ASSERT_NE(entry, nullptr)
      << "budgets.json has no particle_render_10k entry";
  ASSERT_EQ(entry->metric, laige::BudgetMetric::Mean);
#endif
  // Both backends run the workload (ADR 0002: the gate is backend-
  // complete); the budgets.json `measured` records the worse of the
  // two when gated:
  const laige::BudgetCheckResult fpx16 =
      runBudgetBackend<laige::sim::Fpx16_16>(entry);
#if defined(LAIGE_PARTICLE_RENDER_BUDGET)
  // Land the stable report in the ctest log (AGENTS §12):
  std::fputs(fpx16.report.c_str(), stdout);
  std::fflush(stdout);
  EXPECT_TRUE(fpx16.passed) << "fpx16_16: " << fpx16.report;
#endif
  const laige::BudgetCheckResult fp32 =
      runBudgetBackend<laige::sim::Fp32Pinned>(entry);
#if defined(LAIGE_PARTICLE_RENDER_BUDGET)
  std::fputs(fp32.report.c_str(), stdout);
  std::fflush(stdout);
  EXPECT_TRUE(fp32.passed) << "fp32_pinned: " << fp32.report;
#else
  // The ungated run: a test body without assertions passes — the
  // workload's value here is the leak/race/correctness coverage the
  // sanitizer runtimes (and the non-reference runners) provide.
  (void)fpx16;
  (void)fp32;
#endif
}

}  // namespace
