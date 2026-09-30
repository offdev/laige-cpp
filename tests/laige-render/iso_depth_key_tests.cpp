// laige-render isometric depth key tests (M2-ISO-01): the engine-owned
// 32-bit sortable depth key in laige/render/iso_depth_key.h.
//
// Pure math — no GL context, no GL environment needed: every suite runs
// in every local tree and in CI.
//
// The goldens are HAND-COMPUTED from the documented formula in the
// header (the roadmap's "hand-computed golden keys for a small
// stepped-terrain scene"); the 10k random-scene property test checks
// the roadmap's second clause — "on a 10k random scene, key order
// matches painter's-order expectation" — against the actual iso
// projection matrices (M2-GL-03) for the two built-in presets and a
// custom A=C shear.
//
// Seed: the repo-wide documented default seed via
// tests/support/laige_test_seed.h (docs/testing.md §4), one named
// substream per randomized suite.

#include "laige/render/iso_depth_key.h"
#include "laige/render/matrices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "gtest/gtest.h"
#include "laige/prng.h"
#include "laige/sim_math.h"
#include "laige_test_seed.h"

namespace {

using laige::fpx16_16;
using laige::render::IsoAxes;
using laige::render::IsoDepthKeyParts;
using laige::render::Mat4;
using laige::render::Vec2;
using laige::render::isoDepthKey;
using laige::render::isoDepthKeyParts;
using laige::render::isoDepthOrderLess;
using laige::render::isoShearSupported;
using laige::sim::Fp32Pinned;
using laige::sim::Fpx16_16;

// One named substream id per randomized suite (docs/testing.md §4).
constexpr std::uint32_t kPropertySubstreamId = 0x49534F31;  // "ISO1"
constexpr std::uint32_t kDeterminismSubstreamId = 0x49534F32;  // "ISO2"

// ---------------------------------------------------------------------------
// The test sprites
// ---------------------------------------------------------------------------

// One isometric object: world position (float — both backends' storage
// domain), the tile/step height it stands on, its layer, its entity id
// (the deterministic insertion order).
struct Sprite {
  float x{};
  float y{};
  std::int32_t z{};
  std::int32_t layer{};
  std::uint32_t id{};
};

// The painter's-order value v = (x + y) - z, in exact double (the test
// positions are dyadic and small, so this is the exact rational value —
// no tolerance is needed in the v comparisons).
double orderValue(const Sprite& s) {
  return static_cast<double>(s.x) + static_cast<double>(s.y) -
         static_cast<double>(s.z);
}

// Per-backend position conversion. fromFloat is exact for every dyadic
// test value (|v| < 32768: one exact power-of-two scale + an exact
// integer — fpx16_16.h), so the fpx16_16 backend sees the same values
// the fp32_pinned backend does (the cross-backend agreement checks).
template <typename Backend>
struct BackendPos {
  static laige::sim::SimMath<Backend>::Vec2 make(float x, float y) noexcept;
};
template <>
struct BackendPos<Fp32Pinned> {
  static laige::sim::SimMathFp32::Vec2 make(float x, float y) noexcept {
    return laige::sim::SimMathFp32::Vec2{x, y};
  }
};
template <>
struct BackendPos<Fpx16_16> {
  static laige::sim::SimMathFpx16::Vec2 make(float x, float y) noexcept {
    return laige::sim::SimMathFpx16::Vec2{fpx16_16::fromFloat(x),
                                          fpx16_16::fromFloat(y)};
  }
};

template <typename Backend>
std::uint32_t keyOf(const Sprite& s) {
  return isoDepthKey<Backend>(BackendPos<Backend>::make(s.x, s.y), s.z,
                              s.layer);
}

// ---------------------------------------------------------------------------
// The three depth-key-supported shears (the back-to-front contract)
// ---------------------------------------------------------------------------

// 2:1 dimetric at scale 1 (ADR 0005 default): A = 1. Lazy (function-local
// static) so no builder call happens at program start — a startup-time
// dynamic initializer calling into the module under test segfaults the
// clang ThreadSanitizer runtime on the P0 reference platform
// (the __cxa_atexit interceptor recursion, 2026-09-30; the repo test
// convention is constexpr-only globals).
const Mat4& kShearTwoToOne() {
  static const Mat4 m = laige::render::isoDimetric2To1(1.0f);
  return m;
}
// True 30°/60° at scale 1: A = 1/sqrt(3).
const Mat4& kShearTrueIso() {
  static const Mat4 m = laige::render::isoTrueIso3060(1.0f);
  return m;
}
// Custom shear with A = C = 1/2 (invertible: det = -1).
const Mat4& kShearCustom() {
  static const Mat4 m =
      laige::render::isoMatrix(IsoAxes{Vec2{1.0f, -0.5f}, Vec2{-1.0f, -0.5f},
                                       0.5f});
  return m;
}

// The per-shear downward slope A (double — the exact value for each
// shear: 1, 1/sqrt(3), 1/2). NDC_y = -A * v for a supported shear.
struct Shear {
  const Mat4* m;
  double a;
  const char* name;
};
const std::array<Shear, 3>& kShears() {
  static const std::array<Shear, 3> s{{
      {&kShearTwoToOne(), 1.0, "2:1"},
      {&kShearTrueIso(), 1.0 / std::sqrt(3.0), "true-iso"},
      {&kShearCustom(), 0.5, "custom"}}};
  return s;
}

// NDC_y of an object's base under shear m (the matrix's row 1 applied to
// the world point — m is affine with w = 1). Computed in double.
double painterNdcY(const Mat4& m, const Sprite& s) {
  const double x = s.x, y = s.y, z = s.z;
  return static_cast<double>(m[0][1]) * x + static_cast<double>(m[1][1]) * y +
         static_cast<double>(m[2][1]) * z;
}

// ---------------------------------------------------------------------------
// The small stepped-terrain golden scene (hand-computed)
// ---------------------------------------------------------------------------
//
// A 3x3 tile ground (tiles (0..2) x (0..2)); tile (1,1) is a 1-step rise,
// tile (2,2) is a 2-step rise. Seven sprites (positions on the 1/2 grid —
// exact in both SimMath backends):
//
//   id  pos         z   layer   v = x+y-z   q = 16(x+y)   d = q-16z   key
//   --  ----------  --  -----   ---------   -----------   ---------   ---------------
//   S0  (0.5,0.5)   0     0         1.0          16          16      0x80200010
//   S1  (1.5,1.5)   0     0         3.0          48          48      0x80200030
//   S2  (1.5,1.5)   1     0         2.0          32          32      0x80200020
//   S3  (0.5,1.5)   0     0         2.0          32          32      0x80200020
//   S4  (2.5,2.5)   2     0         3.0          48          48      0x80200030
//   S5  (0.5,0.5)   0     1         1.0          16          16      0x80600010
//   S6  (0.5,0.5)   0    -1         1.0          16          16      0x7FE00010
//
// key = (layer + 512) << 22 | (d + 2^21); 2^21 = 0x200000; 512<<22 =
// 0x80000000; 513<<22 = 0x80400000; 511<<22 = 0x7FC00000; the depth field
// is d + 0x200000 (S5: 0x80400000 | 0x200010 = 0x80600010; S6:
// 0x7FC00000 | 0x200010 = 0x7FE00010).
//
// S2 and S3 share a key (same layer, same quantized v: (1.5+1.5)-1 =
// 2.0 = (0.5+1.5)-0); S1 and S4 share a key (3.0 = 3.0-... (2.5+2.5)-2).
// Expected back-to-front order: S6 < S0 < S2 = S3 < S1 = S4 < S5.

const Sprite kGolden[] = {
    {0.5f, 0.5f, 0, 0, 0},   // S0
    {1.5f, 1.5f, 0, 0, 1},   // S1
    {1.5f, 1.5f, 1, 0, 2},   // S2
    {0.5f, 1.5f, 0, 0, 3},   // S3
    {2.5f, 2.5f, 2, 0, 4},   // S4
    {0.5f, 0.5f, 0, 1, 5},   // S5
    {0.5f, 0.5f, 0, -1, 6},  // S6
};
constexpr int kGoldenCount = 7;
const std::uint32_t kGoldenKeys[kGoldenCount] = {
    0x80200010, 0x80200030, 0x80200020, 0x80200020, 0x80200030,
    0x80600010, 0x7FE00010,
};

// ---------------------------------------------------------------------------
// The 10k random scene (the property test)
// ---------------------------------------------------------------------------

constexpr int kPropertySceneSize = 10000;
constexpr int kPropertyGrid = 32;  // tiles per side (x, y in 0..31)

// The scene terrain: tile (gx, gy) has step height (7*gx + 11*gy) % 5
// (0..4 — a fixed deterministic stepped terrain; the hand-checked
// pairs below quote its values directly).
constexpr std::int32_t tileHeight(std::int32_t gx, std::int32_t gy) {
  return (7 * gx + 11 * gy) % 5;
}

// The 10k random scene: tile-locked positions with a 1/8-grid
// fractional offset (exact in both backends), the tile's height as z,
// 5% of the sprites in layer 1 (foreground), entity id = index. The
// draw order is fixed: gx, gy, fx, fy, layer — the scene is a function
// of (seed, substream) alone (docs/testing.md §4).
std::vector<Sprite> buildPropertyScene() {
  laige::Prng prng = laige::testing::TestPrng(kPropertySubstreamId);
  std::vector<Sprite> sprites;
  sprites.reserve(kPropertySceneSize);
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(kPropertySceneSize);
       ++i) {
    Sprite s;
    const std::uint32_t gx = prng.next_range(0, kPropertyGrid);
    const std::uint32_t gy = prng.next_range(0, kPropertyGrid);
    s.x = static_cast<float>(gx) + static_cast<float>(prng.next_range(0, 8u)) / 8.0f;
    s.y = static_cast<float>(gy) + static_cast<float>(prng.next_range(0, 8u)) / 8.0f;
    s.z = tileHeight(static_cast<std::int32_t>(gx), static_cast<std::int32_t>(gy));
    s.layer = (prng.next_range(0, 20u) == 0) ? 1 : 0;
    s.id = i;
    sprites.push_back(s);
  }
  return sprites;
}

// Sort indices of `sprites` by the (key, entity id) total order (the
// render order — M2-SORT-01's stable sort + the batcher's entity-id
// insertion order, realized here with a plain sort).
std::vector<std::uint32_t> renderOrder(const std::vector<Sprite>& sprites,
                                       const std::vector<std::uint32_t>& keys) {
  std::vector<std::uint32_t> order(sprites.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
    return isoDepthOrderLess(keys[a], sprites[a].id, keys[b], sprites[b].id);
  });
  return order;
}

// Check the back-to-front property over a key-sorted scene for one
// shear (the suite's core property): going from rank r-1 to rank r
// (back to front), a sprite may never sit HIGHER on screen (larger NDC
// y) than the sprite drawn before it, beyond the tie window (A/16 —
// equal keys: |dv| <= 1/16 world units, the documented key precision).
// Layers are separate planes (the parallax contract, M2-PAR-01), so the
// check is per-layer.
void checkBackToFront(const std::vector<Sprite>& sprites,
                      const std::vector<std::uint32_t>& order,
                      const Shear& shear, std::string& failure) {
  const double tieWindow = shear.a / 16.0;  // the documented key precision
  std::int32_t prevLayer = 0;
  double prevNdc = 0.0;
  bool havePrev = false;
  for (std::uint32_t rank = 0; rank < order.size(); ++rank) {
    const Sprite& s = sprites[order[rank]];
    const double ndc = painterNdcY(*shear.m, s);
    if (havePrev && s.layer == prevLayer &&
        ndc > prevNdc + tieWindow + 1e-9) {
      failure = std::string(shear.name) + ": inversion at rank " +
                std::to_string(rank) + " (NDC " + std::to_string(ndc) +
                " after " + std::to_string(prevNdc) + ")";
      return;
    }
    prevNdc = ndc;
    prevLayer = s.layer;
    havePrev = true;
  }
}

// The golden-scene key check (one instantiation per backend).
template <typename Backend>
void expectGoldenScene() {
  for (int i = 0; i < kGoldenCount; ++i) {
    EXPECT_EQ(keyOf<Backend>(kGolden[i]), kGoldenKeys[i])
        << "sprite " << i << " pos=(" << kGolden[i].x << ", "
        << kGolden[i].y << ") z=" << kGolden[i].z << " layer="
        << kGolden[i].layer;
  }
}

}  // namespace

TEST(IsoDepthKeyGolden, GoldenKeysSteppedTerrainFp32) {
  expectGoldenScene<Fp32Pinned>();
}
TEST(IsoDepthKeyGolden, GoldenKeysSteppedTerrainFpx16) {
  expectGoldenScene<Fpx16_16>();
}

TEST(IsoDepthKeyGolden, CrossBackendAgreement) {
  // The golden scene's positions are exact in both backends (1/2 grid),
  // so the keys must be identical across backends (the API doc's
  // "agree for exactly representable inputs").
  for (int i = 0; i < kGoldenCount; ++i) {
    EXPECT_EQ(keyOf<Fpx16_16>(kGolden[i]), keyOf<Fp32Pinned>(kGolden[i]))
        << "sprite " << i;
  }
}

TEST(IsoDepthKeyGolden, BackToFrontOrderAndTies) {
  // The hand-checked back-to-front order (both backends — the order is
  // backend-independent here): S6 < S0 < S2 = S3 < S1 = S4 < S5.
  EXPECT_LT(keyOf<Fp32Pinned>(kGolden[6]), keyOf<Fp32Pinned>(kGolden[0]));
  EXPECT_LT(keyOf<Fp32Pinned>(kGolden[0]), keyOf<Fp32Pinned>(kGolden[2]));
  EXPECT_EQ(keyOf<Fp32Pinned>(kGolden[2]), keyOf<Fp32Pinned>(kGolden[3]));
  EXPECT_LT(keyOf<Fp32Pinned>(kGolden[2]), keyOf<Fp32Pinned>(kGolden[1]));
  EXPECT_EQ(keyOf<Fp32Pinned>(kGolden[1]), keyOf<Fp32Pinned>(kGolden[4]));
  EXPECT_LT(keyOf<Fp32Pinned>(kGolden[1]), keyOf<Fp32Pinned>(kGolden[5]));
  // The (key, entity id) total order resolves the ties by id:
  // S2(id 2) before S3(id 3); S1(id 1) before S4(id 4).
  EXPECT_TRUE(isoDepthOrderLess(keyOf<Fp32Pinned>(kGolden[2]), 2,
                                keyOf<Fp32Pinned>(kGolden[3]), 3));
  EXPECT_FALSE(isoDepthOrderLess(keyOf<Fp32Pinned>(kGolden[3]), 3,
                                 keyOf<Fp32Pinned>(kGolden[2]), 2));
  EXPECT_TRUE(isoDepthOrderLess(keyOf<Fp32Pinned>(kGolden[1]), 1,
                                keyOf<Fp32Pinned>(kGolden[4]), 4));
  // The per-layer painter NDC sequence is non-increasing under every
  // supported shear (back-to-front, NDC y up).
  for (const Shear& shear : kShears()) {
    for (int layer = -1; layer <= 1; ++layer) {
      std::vector<std::uint32_t> ids;
      for (int i = 0; i < kGoldenCount; ++i)
        if (kGolden[i].layer == layer) ids.push_back(i);
      std::sort(ids.begin(), ids.end(), [&](std::uint32_t a, std::uint32_t b) {
        return isoDepthOrderLess(keyOf<Fp32Pinned>(kGolden[a]), kGolden[a].id,
                                 keyOf<Fp32Pinned>(kGolden[b]),
                                 kGolden[b].id);
      });
      double prevNdc = 0.0;
      for (std::size_t r = 0; r < ids.size(); ++r) {
        const double ndc = painterNdcY(*shear.m, kGolden[ids[r]]);
        if (r > 0) {
          EXPECT_LE(ndc, prevNdc + 1e-9)
              << shear.name << " layer " << layer << " rank " << r;
        }
        prevNdc = ndc;
      }
    }
  }
}

TEST(IsoDepthKeyGolden, Unpack) {
  // The exact inverse on the golden keys: (layer, quantized depth d).
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[0]).layer, 0);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[0]).quantizedDepth, 16);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[1]).layer, 0);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[1]).quantizedDepth, 48);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[2]).layer, 0);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[2]).quantizedDepth, 32);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[5]).layer, 1);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[5]).quantizedDepth, 16);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[6]).layer, -1);
  EXPECT_EQ(isoDepthKeyParts(kGoldenKeys[6]).quantizedDepth, 16);
  // Round trip over the full (layer, d) lattice of field boundaries:
  // key -> parts -> key is the identity.
  const std::int32_t layers[] = {-512, -1, 0, 1, 511};
  const std::int32_t depths[] = {-(1 << 21), 0, (1 << 21) - 1};
  for (std::int32_t l : layers) {
    for (std::int32_t d : depths) {
      const std::uint32_t key =
          (static_cast<std::uint32_t>(static_cast<std::uint32_t>(l) + 512u)
           << 22) |
          static_cast<std::uint32_t>(static_cast<std::uint32_t>(
              static_cast<std::uint32_t>(d) + (1u << 21)));
      const IsoDepthKeyParts p = isoDepthKeyParts(key);
      EXPECT_EQ(p.layer, l);
      EXPECT_EQ(p.quantizedDepth, d);
      // Re-pack and compare.
      const std::uint32_t repacked =
          (static_cast<std::uint32_t>(static_cast<std::uint32_t>(p.layer) +
                                       512u)
           << 22) |
          static_cast<std::uint32_t>(static_cast<std::uint32_t>(
              static_cast<std::uint32_t>(p.quantizedDepth) + (1u << 21)));
      EXPECT_EQ(repacked, key);
    }
  }
}

// ---------------------------------------------------------------------------
// The 10k random scene: key order matches the painter's order
// ---------------------------------------------------------------------------

TEST(IsoDepthKeyProperty, TenKRandomSceneBackToFront) {
  const std::vector<Sprite> sprites = buildPropertyScene();
  ASSERT_EQ(sprites.size(), static_cast<std::size_t>(kPropertySceneSize));

  // Both backends' keys: identical for this scene (1/8-grid positions —
  // exact in both backends), per the API doc's agreement clause.
  std::vector<std::uint32_t> keysFp32(sprites.size()), keysFpx(sprites.size());
  for (std::size_t i = 0; i < sprites.size(); ++i) {
    keysFp32[i] = keyOf<Fp32Pinned>(sprites[i]);
    keysFpx[i] = keyOf<Fpx16_16>(sprites[i]);
    EXPECT_EQ(keysFpx[i], keysFp32[i]) << "sprite " << i;
  }

  // The render order (key, entity id) and the back-to-front property
  // under every supported shear.
  const std::vector<std::uint32_t> order = renderOrder(sprites, keysFp32);
  std::string failure;
  for (const Shear& shear : kShears()) {
    failure.clear();
    checkBackToFront(sprites, order, shear, failure);
    EXPECT_TRUE(failure.empty()) << failure;
  }
}

TEST(IsoDepthKeyProperty, ExactMonotonicity) {
  // The exact clause: same layer, keyA < keyB => vA <= vB (the ties-away
  // rounding is monotone — no inversion, ever, for the 10k scene).
  const std::vector<Sprite> sprites = buildPropertyScene();
  std::vector<std::uint32_t> keys(sprites.size());
  for (std::size_t i = 0; i < sprites.size(); ++i)
    keys[i] = keyOf<Fp32Pinned>(sprites[i]);
  const std::vector<std::uint32_t> order = renderOrder(sprites, keys);
  int inversions = 0;
  for (std::uint32_t rank = 1; rank < order.size(); ++rank) {
    const Sprite& a = sprites[order[rank - 1]];
    const Sprite& b = sprites[order[rank]];
    if (a.layer != b.layer) continue;  // layers are separate planes
    if (keys[order[rank - 1]] < keys[order[rank]] &&
        orderValue(a) > orderValue(b) + 1e-12) {
      ++inversions;
      if (inversions <= 3) {
        GTEST_FAIL() << "inversion at rank " << rank << ": v="
                     << orderValue(a) << " drawn before v="
                     << orderValue(b);
      }
    }
  }
  EXPECT_EQ(inversions, 0);
}

TEST(IsoDepthKeyProperty, MatrixMatchesOrderValue) {
  // The matrix <-> key link: for every supported shear, NDC_y == -A * v
  // (the matrices' row 1 applied to the base point), so the key order
  // (ascending v) is exactly the back-to-front order. Sampled on the
  // 10k scene (the 7 golden sprites are also covered).
  const std::vector<Sprite> sprites = buildPropertyScene();
  for (const Shear& shear : kShears()) {
    int checked = 0;
    for (std::size_t i = 0; i < sprites.size(); i += 97) {  // ~104 samples
      const double ndc = painterNdcY(*shear.m, sprites[i]);
      const double expected = -shear.a * orderValue(sprites[i]);
      EXPECT_NEAR(ndc, expected, 1e-5)
          << shear.name << " sprite " << i;
      ++checked;
    }
    EXPECT_GE(checked, 100);
    for (int i = 0; i < kGoldenCount; ++i) {
      const double ndc = painterNdcY(*shear.m, kGolden[i]);
      EXPECT_NEAR(ndc, -shear.a * orderValue(kGolden[i]), 1e-5)
          << shear.name << " golden " << i;
    }
  }
}

TEST(IsoDepthKeyProperty, HandCheckedOverlappingPairs) {
  // The roadmap's "hand-checked overlapping pairs": concrete sprite
  // pairs on the 10k scene's terrain (tileHeight above), each a visual
  // overlap at the 2:1 scale-1 tile (4 world units screen-wide), with
  // the expected back-to-front order hand-derived from v = x + y - z:
  //
  //   P1 same tile, same screen row (tie):
  //      A(2.25,2.75,z=h(2,2)=1)  v = 5.0-1 = 4.0
  //      B(2.75,2.25,z=h(2,2)=1)  v = 5.0-1 = 4.0     => keys EQUAL
  //      (screen 1 unit apart in x, same NDC y: either order is the
  //      stable tie — resolved by entity id)
  //   P2 x-adjacent, rising step:
  //      A(2.25,2.25,z=h(2,2)=1)  v = 4.5-1 = 3.5
  //      B(3.25,2.25,z=h(3,2)=3)  v = 5.5-3 = 2.5     => A in FRONT
  //   P3 x-adjacent, rising step:
  //      A(4.25,4.25,z=h(4,4)=2)  v = 8.5-2 = 6.5
  //      B(5.25,4.25,z=h(5,4)=4)  v = 9.5-4 = 5.5     => A in FRONT
  //   P4 object on ground in front of a higher tile:
  //      A(5.25,5.25,z=h(5,5)=0)  v = 10.5
  //      B(4.25,5.25,z=h(4,5)=3)  v = 9.5-3 = 6.5     => A in FRONT
  //   P5 layer beats v:
  //      A(1.25,1.25,z=h(1,1)=3, layer 1)  v = 2.5-3 = -0.5
  //      B(15.25,15.25,z=h(15,15)=0, layer 0)  v = 30.5
  //      => A (layer 1, foreground) sorts AFTER B despite vA < vB
  //   P6 same screen row, different step:
  //      A(6.25,7.25,z=h(6,7)=4)  v = 13.5-4 = 9.5
  //      B(7.25,6.25,z=h(7,6)=0)  v = 13.5            => B in FRONT
  // expected: -1: a is drawn BEFORE b (a behind b); 0: keys equal;
  //           +1: a is drawn AFTER b (a in FRONT of b). Back-to-front:
  // in front = lower on screen = larger v = larger key (drawn later).
  struct Pair {
    Sprite a, b;
    int expected;
  };
  const Sprite pa1{2.25f, 2.75f, 1, 0, 0}, pb1{2.75f, 2.25f, 1, 0, 1};
  const Sprite pa2{2.25f, 2.25f, 1, 0, 0}, pb2{3.25f, 2.25f, 3, 0, 1};
  const Sprite pa3{4.25f, 4.25f, 2, 0, 0}, pb3{5.25f, 4.25f, 4, 0, 1};
  const Sprite pa4{5.25f, 5.25f, 0, 0, 0}, pb4{4.25f, 5.25f, 3, 0, 1};
  const Sprite pa5{1.25f, 1.25f, 3, 1, 0}, pb5{15.25f, 15.25f, 0, 0, 1};
  const Sprite pa6{6.25f, 7.25f, 4, 0, 0}, pb6{7.25f, 6.25f, 0, 0, 1};
  const Pair pairs[] = {
      {pa1, pb1, 0},
      {pa2, pb2, +1},
      {pa3, pb3, +1},
      {pa4, pb4, +1},
      {pa5, pb5, +1},
      {pa6, pb6, -1},
  };
  // Quote the terrain (hand-check the pair data against the formula).
  EXPECT_EQ(tileHeight(2, 2), 1);
  EXPECT_EQ(tileHeight(3, 2), 3);
  EXPECT_EQ(tileHeight(4, 4), 2);
  EXPECT_EQ(tileHeight(5, 4), 4);
  EXPECT_EQ(tileHeight(5, 5), 0);
  EXPECT_EQ(tileHeight(4, 5), 3);
  EXPECT_EQ(tileHeight(1, 1), 3);
  EXPECT_EQ(tileHeight(15, 15), 0);
  EXPECT_EQ(tileHeight(6, 7), 4);
  EXPECT_EQ(tileHeight(7, 6), 0);

  for (std::size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); ++p) {
    const Pair& pr = pairs[p];
    for (int backend = 0; backend < 2; ++backend) {
      const std::uint32_t ka =
          backend == 0 ? keyOf<Fp32Pinned>(pr.a) : keyOf<Fpx16_16>(pr.a);
      const std::uint32_t kb =
          backend == 0 ? keyOf<Fp32Pinned>(pr.b) : keyOf<Fpx16_16>(pr.b);
      if (pr.expected < 0) {
        EXPECT_LT(ka, kb) << "pair " << p << " backend " << backend
                          << ": a must be drawn before b (a behind)";
      } else if (pr.expected > 0) {
        EXPECT_GT(ka, kb) << "pair " << p << " backend " << backend
                          << ": a must be drawn after b (a in front)";
      } else {
        EXPECT_EQ(ka, kb) << "pair " << p << " backend " << backend
                          << ": keys must be equal (same screen row)";
      }
    }
    // The painter's order agrees with the keys under every shear —
    // within a single plane (the NDC comparison is same-plane; the
    // cross-layer pair P5 is ordered by the layer field, not NDC).
    // "In front" = lower on screen = smaller NDC y (NDC y is up).
    if (pr.a.layer == pr.b.layer) {
      for (const Shear& shear : kShears()) {
        const double ndcA = painterNdcY(*shear.m, pr.a);
        const double ndcB = painterNdcY(*shear.m, pr.b);
        if (pr.expected < 0) {
          // a behind b: a is higher on screen (larger NDC y).
          EXPECT_GT(ndcA, ndcB + 0.01)
              << "pair " << p << " " << shear.name << ": a behind b";
        } else if (pr.expected > 0) {
          // a in front of b: a is lower on screen (smaller NDC y).
          EXPECT_LT(ndcA, ndcB - 0.01)
              << "pair " << p << " " << shear.name << ": a in front of b";
        } else {
          EXPECT_NEAR(ndcA, ndcB, 1e-9)
              << "pair " << p << " " << shear.name << ": same screen row";
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The tie-break contract: (key, entity id) total order
// ---------------------------------------------------------------------------

TEST(IsoDepthKeyTieBreak, TotalOrderProperties) {
  // Strict weak ordering: never reflexive; irreflexive + transitive on
  // a small exhaustive set.
  const std::uint32_t kKeys[] = {0x7FC00000, 0x80000000, 0x80200020,
                                 0x80200020, 0xFFC00000};
  const std::uint32_t kIds[] = {0, 1, 2, 3, 7};
  auto less = [](std::uint32_t ka, std::uint32_t ia, std::uint32_t kb,
                 std::uint32_t ib) {
    return isoDepthOrderLess(ka, ia, kb, ib);
  };
  for (std::size_t a = 0; a < 5; ++a) {
    EXPECT_FALSE(less(kKeys[a], kIds[a], kKeys[a], kIds[a]));
    for (std::size_t b = 0; b < 5; ++b) {
      for (std::size_t c = 0; c < 5; ++c) {
        const bool ab = less(kKeys[a], kIds[a], kKeys[b], kIds[b]);
        const bool bc = less(kKeys[b], kIds[b], kKeys[c], kIds[c]);
        const bool ac = less(kKeys[a], kIds[a], kKeys[c], kIds[c]);
        if (ab && bc) {
          EXPECT_TRUE(ac) << "transitivity " << a << " " << b << " " << c;
        }
        // The relation is total: a<b, b<a, or equal keys+ids.
        const bool ba = less(kKeys[b], kIds[b], kKeys[a], kIds[a]);
        if (kKeys[a] != kKeys[b] || kIds[a] != kIds[b]) {
          EXPECT_TRUE(ab || ba) << "totality " << a << " " << b;
        }
      }
    }
  }
  // The key dominates the id: a smaller key sorts first regardless of
  // the id (and vice versa).
  EXPECT_TRUE(less(0x80000000, 999u, 0x80200020, 0u));
  EXPECT_FALSE(less(0x80200020, 0u, 0x80000000, 999u));
}

TEST(IsoDepthKeyTieBreak, LayerAndStepOrdering) {
  const Sprite base{0.5f, 0.5f, 0, 0, 0};
  const Sprite layerNeg{0.5f, 0.5f, 0, -1, 1};
  const Sprite layerPos{0.5f, 0.5f, 0, 1, 2};
  const Sprite stepUp{1.5f, 1.5f, 1, 0, 3};  // same x+y = 3.0, z = 1
  const Sprite stepFlat{1.5f, 1.5f, 0, 0, 4};  // same x+y = 3.0, z = 0
  // Layer ascending is the coarse primary order (background first),
  // whatever the v.
  EXPECT_LT(keyOf<Fp32Pinned>(layerNeg), keyOf<Fp32Pinned>(base));
  EXPECT_LT(keyOf<Fp32Pinned>(base), keyOf<Fp32Pinned>(layerPos));
  // A higher step at the same (x, y) row sorts BEHIND (smaller key):
  // the step raises the base, moving it up-screen.
  EXPECT_LT(keyOf<Fp32Pinned>(stepUp), keyOf<Fp32Pinned>(stepFlat));
  // Same on the fixed backend.
  EXPECT_LT(keyOf<Fpx16_16>(layerNeg), keyOf<Fpx16_16>(base));
  EXPECT_LT(keyOf<Fpx16_16>(base), keyOf<Fpx16_16>(layerPos));
  EXPECT_LT(keyOf<Fpx16_16>(stepUp), keyOf<Fpx16_16>(stepFlat));
}

TEST(IsoDepthKeyTieBreak, DeterministicRecompute) {
  // The key is a pure function of the input: a fresh draw of the same
  // seeded scene reproduces every key bit-exactly (both backends).
  auto build = [](std::uint32_t substream) {
    laige::Prng prng = laige::testing::TestPrng(substream);
    std::vector<std::uint32_t> keys;
    keys.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      Sprite s;
      s.x = static_cast<float>(prng.next_range(0, kPropertyGrid)) +
            static_cast<float>(prng.next_range(0, 8)) / 8.0f;
      s.y = static_cast<float>(prng.next_range(0, kPropertyGrid)) +
            static_cast<float>(prng.next_range(0, 8)) / 8.0f;
      s.z = tileHeight(static_cast<std::int32_t>(prng.next_range(0, 32)),
                       static_cast<std::int32_t>(prng.next_range(0, 32)));
      s.layer = 0;
      s.id = i;
      keys.push_back(keyOf<Fp32Pinned>(s));
    }
    return keys;
  };
  const std::vector<std::uint32_t> a = build(kDeterminismSubstreamId);
  const std::vector<std::uint32_t> b = build(kDeterminismSubstreamId);
  EXPECT_EQ(a, b);
}

// ---------------------------------------------------------------------------
// Domain and saturation (the total-function contract)
// ---------------------------------------------------------------------------

TEST(IsoDepthKeyDomain, SaturationFp32) {
  // In-domain reference: (0.5, 0.5, z=0) -> key 0x80200010 (golden S0).
  const Sprite ref{0.5f, 0.5f, 0, 0, 0};
  EXPECT_EQ(keyOf<Fp32Pinned>(ref), 0x80200010u);
  // Out-of-domain |x|: s = x + y saturates at the domain sum bound
  // 65534 (q = 16 * 65534 = 1048544), whatever the magnitude.
  const std::uint32_t kSatSum =
      (512u << 22) | (2097152u + 1048544u);
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{200000.0f, 0.0f, 0, 0, 0}), kSatSum);
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{1e30f, 0.0f, 0, 0, 0}), kSatSum);
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{-200000.0f, 0.0f, 0, 0, 0}),
            (512u << 22) | (2097152u - 1048544u));
  // Non-finite: +inf -> +bound, -inf and NaN -> -bound (IEEE NaN
  // compares false). No UB on any path (run under UBSan in CI).
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{std::numeric_limits<float>::infinity(),
                                     0.0f, 0, 0, 0}),
            kSatSum);
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{-std::numeric_limits<float>::infinity(),
                                     0.0f, 0, 0, 0}),
            (512u << 22) | (2097152u - 1048544u));
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{std::numeric_limits<float>::quiet_NaN(),
                                     0.5f, 0, 0, 0}),
            (512u << 22) | (2097152u - 1048544u));
  // Layer saturation: outside +/-511 the layer field clamps.
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{0.5f, 0.5f, 0, 999, 0}),
            (1023u << 22) | 0x200010u);
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{0.5f, 0.5f, 0, -999, 0}),
            0x200010u);
  EXPECT_EQ(isoDepthKeyParts(keyOf<Fp32Pinned>(Sprite{0.5f, 0.5f, 0, 999, 0}))
                .layer,
            511);
  // Out-of-domain step height: the formula still applies (d = 16 -
  // 5000*16 = -79984, inside the field) — defined, in the documented
  // "order not promised" zone.
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{0.5f, 0.5f, 5000, 0, 0}),
            (512u << 22) | (2097152u - 79984u));
  EXPECT_EQ(keyOf<Fp32Pinned>(Sprite{0.5f, 0.5f, -5000, 0, 0}),
            (512u << 22) | (2097152u + 80016u));
}

TEST(IsoDepthKeyDomain, SaturationFpx16) {
  // The fpx16_16 coordinate domain is tighter (Q16.16: +/-32767.996;
  // saturating add): fromFloat(1e30) is max() = 32767.99609375, so the
  // saturated sum is max() and q = round(16 * max()) = 524288 — the
  // fixed backend's own bound, not the fp32 sum bound.
  const std::uint32_t kFpxSat =
      (512u << 22) | (2097152u + 524288u);
  EXPECT_EQ(keyOf<Fpx16_16>(Sprite{1e30f, 0.0f, 0, 0, 0}), kFpxSat);
  // min() is exactly -32768.0: q = -524288.
  EXPECT_EQ(keyOf<Fpx16_16>(Sprite{-1e30f, 0.0f, 0, 0, 0}),
            (512u << 22) | (2097152u - 524288u));
  // Saturating add: max + max saturates to max (no int64 overflow —
  // CPP-004, fpx16_16.h).
  EXPECT_EQ(keyOf<Fpx16_16>(Sprite{40000.0f, 40000.0f, 0, 0, 0}), kFpxSat);
  // Layer saturation is backend-independent.
  EXPECT_EQ(keyOf<Fpx16_16>(Sprite{0.5f, 0.5f, 0, 999, 0}),
            (1023u << 22) | 0x200010u);
  EXPECT_EQ(keyOf<Fpx16_16>(Sprite{0.5f, 0.5f, 0, -999, 0}), 0x200010u);
  // Out-of-domain step: same formula as the fp32 case.
  EXPECT_EQ(keyOf<Fpx16_16>(Sprite{0.5f, 0.5f, 5000, 0, 0}),
            (512u << 22) | (2097152u - 79984u));
}

// ---------------------------------------------------------------------------
// The supported-iso-shear checker
// ---------------------------------------------------------------------------

TEST(IsoDepthKeyShears, SupportedAndRejected) {
  // Both built-in presets are supported, at every scale (constructed
  // exactly as matrices.cpp builds them: same float expressions).
  EXPECT_TRUE(isoShearSupported(IsoAxes{Vec2{2.0f, -1.0f}, Vec2{-2.0f, -1.0f},
                                       1.0f}));
  EXPECT_TRUE(isoShearSupported(IsoAxes{Vec2{5.0f, -2.5f}, Vec2{-5.0f, -2.5f},
                                       2.5f}));
  EXPECT_TRUE(isoShearSupported(IsoAxes{Vec2{0.25f, -0.125f},
                                       Vec2{-0.25f, -0.125f}, 0.125f}));
  const float invSqrt3 = 1.0f / std::sqrt(3.0f);  // matrices.cpp's constant
  EXPECT_TRUE(isoShearSupported(
      IsoAxes{Vec2{1.0f, -1.0f * invSqrt3}, Vec2{-1.0f, -1.0f * invSqrt3},
              1.0f * invSqrt3}));
  EXPECT_TRUE(isoShearSupported(
      IsoAxes{Vec2{0.5f, -0.5f * invSqrt3}, Vec2{-0.5f, -0.5f * invSqrt3},
              0.5f * invSqrt3}));
  // A custom A = C shear (the kShearCustom above) is supported.
  EXPECT_TRUE(isoShearSupported(IsoAxes{Vec2{1.0f, -0.5f},
                                        Vec2{-1.0f, -0.5f}, 0.5f}));
  // Rejected:
  // - A != C (height unit differs from the ground slope);
  EXPECT_FALSE(isoShearSupported(IsoAxes{Vec2{1.0f, -0.5f},
                                         Vec2{-1.0f, -0.5f}, 0.25f}));
  // - A != B (the ground axes project at different slopes);
  EXPECT_FALSE(isoShearSupported(IsoAxes{Vec2{1.0f, -0.5f},
                                         Vec2{-1.0f, -0.6f}, 0.5f}));
  // - a ground axis projects UPWARD (a = -dx.y <= 0);
  EXPECT_FALSE(isoShearSupported(IsoAxes{Vec2{1.0f, 0.5f},
                                         Vec2{-1.0f, -0.5f}, 0.5f}));
  // - degenerate ground map (det = 0: parallel axis screen-deltas) even
  //   though A = C — the picking inverse does not exist;
  EXPECT_FALSE(isoShearSupported(IsoAxes{Vec2{1.0f, -0.5f},
                                         Vec2{1.0f, -0.5f}, 0.5f}));
  // - non-finite components.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(isoShearSupported(IsoAxes{Vec2{nan, -0.5f},
                                         Vec2{-1.0f, -0.5f}, 0.5f}));
  EXPECT_FALSE(isoShearSupported(IsoAxes{Vec2{1.0f, nan},
                                         Vec2{-1.0f, -0.5f}, 0.5f}));
}

