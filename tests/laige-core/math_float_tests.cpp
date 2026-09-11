// laige-core SimMath fp32_pinned backend suite (M0-CORE-03).
//
// Step Verify scope (roadmap/M0-foundations.md):
//   - `ctest -R math_float` green (suites: SimMathBasics, SimMathNanInf,
//     SimMathProperties, SimMathDispatch)
//   - NaN/Inf handling is *defined* and tested — the documented policy of
//     the header, not "whatever the CPU does" (SimMathNanInf)
//   - Associativity guard: the pinned flag set (-ffp-contract=off) is
//     verified at runtime — DotProductCanary and LerpIsNotFmaFused fail
//     if the compiler FMA-contracts or reassociates the pinned
//     expressions
//   - Compile-time dispatch: SimMath<Backend> is stateless, one
//     instantiation per backend (ADR 0002, PERF-006)
//
// This translation unit is a sim target: it is built with the pinned
// flag set (laige_apply_simmath_policy). std::sqrt / std::numeric_limits
// appear only as oracles for known values; every value under test comes
// from a SimMath op (PRD §10.3, ADR 0002).
//
// House convention (also enforced by -Wall -Werror): raw `float`
// equality comparisons trigger -Wfloat-equal, so all float equality
// checks here go through bits() (bit-exact uint32 comparison) or same()
// (the SimMath NaN-aware equality), never EXPECT_EQ on floats.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "gtest/gtest.h"
#include "laige/sim_math.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "math_float_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "math_float_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "math_float_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

namespace {

using Sim = laige::sim::SimMathFp32;
using Vec2 = Sim::Vec2;
using Vec3 = Sim::Vec3;
using Scalar = Sim::Scalar;

const Scalar kNaN = std::numeric_limits<Scalar>::quiet_NaN();
const Scalar kInf = std::numeric_limits<Scalar>::infinity();
const Scalar kNegInf = -kInf;

// Bit-exact comparison helpers (memcpy, not pointer casts — CPP-004).
std::uint32_t bits(Scalar f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}

// NaN-aware equality for EXPECT_*: a raw == is false for (NaN, NaN),
// which is the IEEE classification SimMath documents.
bool same(Scalar a, Scalar b) {
  return Sim::equals(a, b) || (Sim::isNaN(a) && Sim::isNaN(b));
}

}  // namespace

// ---------------------------------------------------------------------------
// SimMathBasics — exact IEEE values, ±0, bit-exact known results
// ---------------------------------------------------------------------------

TEST(SimMathBasics, KnownValuesBitExact) {
  // 0.1f + 0.2f == 0.3f: the exact sum 0.30000000447034836 rounds to the
  // same binary32 as the 0.3 literal.
  EXPECT_EQ(bits(Sim::add(0.1f, 0.2f)), bits(0.3f));
  // 1/3 in binary32: the repeating 1.01010101... significand rounds up
  // at bit 24 to 0x3EAAAAAB.
  EXPECT_EQ(bits(Sim::div(1.0f, 3.0f)), 0x3EAAAAABu);
  // (1/3) * 3 rounds back to exactly 1.0 (the product
  // 1.0000000298... is within half an ulp of 1.0).
  EXPECT_EQ(Sim::mul(Sim::div(1.0f, 3.0f), 3.0f), 1.0f);
  // 1e-8 < half ulp(1.0) ≈ 5.96e-8 → 1.0 + 1e-8f rounds to exactly 1.0.
  EXPECT_EQ(Sim::add(1.0f, 1e-8f), 1.0f);
  // 3-4-5 right triangle, exact in binary32.
  EXPECT_EQ(Sim::length(Vec2{3.0f, 4.0f}), 5.0f);
  // length((1,1)) is the correctly-rounded sqrt(2) (oracle: std::sqrt).
  EXPECT_EQ(bits(Sim::length(Vec2{1.0f, 1.0f})), bits(std::sqrt(2.0f)));
  // 8 * 0.25 is exact dyadic scaling → lerp from 0 at t = 0.25 is exact.
  EXPECT_EQ(Sim::lerp(0.0f, 8.0f, 0.25f), 2.0f);
  // lerp(a, b, 1) is exact for dyadic a, b (a + fl(b-a) rounds back to b).
  EXPECT_EQ(Sim::lerp(1.0f, 2.0f, 1.0f), 2.0f);
}

TEST(SimMathBasics, SignedZero) {
  // IEEE signed zero: 0 + -0 = +0, and the sign survives multiplication.
  EXPECT_EQ(bits(Sim::add(0.0f, -0.0f)), 0u);
  EXPECT_EQ(bits(Sim::mul(-1.0f, 0.0f)), 0x80000000u);
  // ±0 compare equal and are not ordered against each other.
  EXPECT_TRUE(Sim::equals(0.0f, -0.0f));
  EXPECT_FALSE(Sim::less(0.0f, -0.0f));
  EXPECT_FALSE(Sim::greater(0.0f, -0.0f));
  // Vector add normalizes (-0, -0) + (0, 0) to (+0, +0).
  EXPECT_TRUE(Sim::equals(Sim::add(Vec2{-0.0f, -0.0f}, Vec2{0.0f, 0.0f}),
                          Vec2{0.0f, 0.0f}));
  // Vec2 equality is component-wise and treats ±0 as equal.
  EXPECT_TRUE(Sim::equals(Vec2{-0.0f, 0.0f}, Vec2{0.0f, 0.0f}));
  EXPECT_FALSE(Sim::notEquals(Vec2{-0.0f, 0.0f}, Vec2{0.0f, 0.0f}));
}

TEST(SimMathBasics, CommutativityBitExact) {
  // IEEE addition and multiplication are commutative (including ±0 and
  // denormals); the guard pins that no reassociation or sign quirk
  // breaks it in this build.
  const Scalar xs[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.1f, -0.1f, 3.25f, -3.25f,
                       std::numeric_limits<Scalar>::max(),
                       std::numeric_limits<Scalar>::denorm_min()};
  for (const Scalar x : xs) {
    for (const Scalar y : xs) {
      EXPECT_EQ(bits(Sim::add(x, y)), bits(Sim::add(y, x))) << "x=" << x;
      EXPECT_EQ(bits(Sim::mul(x, y)), bits(Sim::mul(y, x))) << "x=" << x;
    }
  }
}

TEST(SimMathBasics, Division) {
  // x / x == 1 for every finite nonzero x.
  const Scalar xs[] = {1.0f, -1.0f, 0.1f, 123456.75f, 6.5536e-5f};
  for (const Scalar x : xs) EXPECT_EQ(Sim::div(x, x), 1.0f);
  // inf / inf is NaN (IEEE), never 1.
  EXPECT_TRUE(Sim::isNaN(Sim::div(kInf, kInf)));
  EXPECT_TRUE(Sim::isNaN(Sim::div(kNegInf, kNegInf)));
  // Exact dyadic divisions.
  EXPECT_EQ(Sim::div(8.0f, 2.0f), 4.0f);
  EXPECT_EQ(Sim::div(-8.0f, 2.0f), -4.0f);
  EXPECT_EQ(bits(Sim::div(1.0f, -2.0f)), bits(-0.5f));
}

TEST(SimMathBasics, VectorOps) {
  const Vec2 p{1.5f, -2.25f};
  const Vec2 q{0.25f, 4.0f};
  EXPECT_TRUE(Sim::equals(Sim::add(p, q), Vec2{1.75f, 1.75f}));
  EXPECT_TRUE(Sim::equals(Sim::sub(p, q), Vec2{1.25f, -6.25f}));
  EXPECT_TRUE(Sim::equals(Sim::mul(p, q), Vec2{0.375f, -9.0f}));
  EXPECT_TRUE(Sim::equals(Sim::mul(p, 2.0f), Vec2{3.0f, -4.5f}));
  EXPECT_TRUE(Sim::equals(Sim::mul(2.0f, p), Sim::mul(p, 2.0f)));
  // 0.5 scaling is exact dyadic scaling.
  EXPECT_TRUE(Sim::equals(Sim::mul(p, 0.5f), Vec2{0.75f, -1.125f}));
  // The zero vector is the additive identity (bit-exact).
  EXPECT_TRUE(Sim::equals(Sim::add(p, Vec2{}), p));
  EXPECT_TRUE(Sim::equals(Sim::sub(p, p), Vec2{}));
  // Vec3: 3-4-12 → 13 exact.
  EXPECT_EQ(Sim::length(Vec3{3.0f, 4.0f, 12.0f}), 13.0f);
  // normalize is exact for dyadic components.
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec2{0.0f, 2.0f}), Vec2{0.0f, 1.0f}));
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec2{2.0f, 0.0f}), Vec2{1.0f, 0.0f}));
  EXPECT_TRUE(
      Sim::equals(Sim::normalize(Vec2{-2.0f, 0.0f}), Vec2{-1.0f, 0.0f}));
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec3{0.0f, 0.0f, 3.0f}),
                          Vec3{0.0f, 0.0f, 1.0f}));
}

// ---------------------------------------------------------------------------
// SimMathNanInf — the documented NaN/Inf policy, defined and tested
// ---------------------------------------------------------------------------

TEST(SimMathNanInf, ArithmeticPropagatesNaN) {
  EXPECT_TRUE(Sim::isNaN(Sim::add(kNaN, 1.0f)));
  EXPECT_TRUE(Sim::isNaN(Sim::add(1.0f, kNaN)));
  EXPECT_TRUE(Sim::isNaN(Sim::sub(kNaN, 1.0f)));
  EXPECT_TRUE(Sim::isNaN(Sim::mul(kNaN, 1.0f)));
  EXPECT_TRUE(Sim::isNaN(Sim::div(kNaN, 1.0f)));
  EXPECT_TRUE(Sim::isNaN(Sim::div(1.0f, kNaN)));
  // IEEE exceptional values.
  EXPECT_TRUE(Sim::isNaN(Sim::div(0.0f, 0.0f)));
  EXPECT_TRUE(Sim::isNaN(Sim::add(kInf, kNegInf)));
  EXPECT_TRUE(Sim::isNaN(Sim::sub(kInf, kInf)));
  EXPECT_TRUE(Sim::isNaN(Sim::mul(kInf, 0.0f)));
  EXPECT_TRUE(Sim::isNaN(Sim::div(kInf, kInf)));
  EXPECT_TRUE(Sim::isNaN(laige::sim::Fp32Pinned::sqrt(-1.0f)));
  // NaN propagates through vectors component-wise.
  EXPECT_TRUE(Sim::isNaN(Sim::add(Vec2{kNaN, 1.0f}, Vec2{0.0f, 0.0f}).x));
  EXPECT_TRUE(Sim::isNaN(Sim::length(Vec2{kNaN, 0.0f})));
}

TEST(SimMathNanInf, DivisionByZeroPolicy) {
  // No trap on P0 targets: x/0 = ±inf with the sign of x * sign(0).
  EXPECT_EQ(bits(Sim::div(1.0f, 0.0f)), bits(kInf));
  EXPECT_EQ(bits(Sim::div(-1.0f, 0.0f)), bits(kNegInf));
  EXPECT_EQ(bits(Sim::div(1.0f, -0.0f)), bits(kNegInf));
  EXPECT_TRUE(Sim::isNaN(Sim::div(0.0f, 0.0f)));
  // x/inf = ±0 (signed), 0/inf = +0.
  EXPECT_EQ(Sim::div(1.0f, kInf), 0.0f);
  EXPECT_EQ(bits(Sim::div(-1.0f, kInf)), 0x80000000u);
  EXPECT_EQ(Sim::div(0.0f, kInf), 0.0f);
}

TEST(SimMathNanInf, Classification) {
  EXPECT_TRUE(Sim::isNaN(kNaN));
  EXPECT_FALSE(Sim::isNaN(0.0f));
  EXPECT_TRUE(Sim::isInf(kInf));
  EXPECT_TRUE(Sim::isInf(kNegInf));
  EXPECT_FALSE(Sim::isInf(kNaN));
  EXPECT_TRUE(Sim::isFinite(1234.5f));
  EXPECT_FALSE(Sim::isFinite(kNaN));
  EXPECT_FALSE(Sim::isFinite(kInf));
  // No flush-to-zero: sqrt of the smallest denormal is a finite
  // (denormal) value, not 0 (pinned build, ADR 0002).
  const Scalar tiny =
      laige::sim::Fp32Pinned::sqrt(std::numeric_limits<Scalar>::denorm_min());
  EXPECT_TRUE(Sim::isFinite(tiny));
  EXPECT_TRUE(Sim::greater(tiny, 0.0f));
}

TEST(SimMathNanInf, OrderedComparisons) {
  // NaN: every ordered comparison is false, notEquals is true.
  EXPECT_FALSE(Sim::less(kNaN, 1.0f));
  EXPECT_FALSE(Sim::less(1.0f, kNaN));
  EXPECT_FALSE(Sim::lessEqual(kNaN, 1.0f));
  EXPECT_FALSE(Sim::greater(kNaN, 1.0f));
  EXPECT_FALSE(Sim::greaterEqual(1.0f, kNaN));
  EXPECT_FALSE(Sim::equals(kNaN, kNaN));  // NaN is not equal to itself
  EXPECT_FALSE(Sim::equals(1.0f, kNaN));
  EXPECT_TRUE(Sim::notEquals(kNaN, 1.0f));
  EXPECT_FALSE(Sim::isOrdered(kNaN, 1.0f));
  EXPECT_TRUE(Sim::isOrdered(-1.0f, 1.0f));
  // Normal ordering still works, and ±inf sit at the ends.
  EXPECT_TRUE(Sim::less(-1.0f, 0.0f));
  EXPECT_TRUE(Sim::lessEqual(0.0f, 0.0f));
  EXPECT_TRUE(Sim::greater(1.0f, 0.0f));
  EXPECT_TRUE(Sim::greaterEqual(1.0f, 1.0f));
  EXPECT_TRUE(Sim::less(kNegInf, 1.0f));
  EXPECT_TRUE(Sim::greater(kInf, 1.0f));
}

TEST(SimMathNanInf, ClampPolicy) {
  EXPECT_EQ(Sim::clamp(5.0f, 0.0f, 10.0f), 5.0f);
  EXPECT_EQ(Sim::clamp(15.0f, 0.0f, 10.0f), 10.0f);
  EXPECT_EQ(Sim::clamp(-5.0f, 0.0f, 10.0f), 0.0f);
  EXPECT_EQ(Sim::clamp(10.0f, 0.0f, 10.0f), 10.0f);
  // Defined NaN/Inf policy: NaN x propagates; ±inf x clamps to the bound.
  EXPECT_TRUE(Sim::isNaN(Sim::clamp(kNaN, 0.0f, 10.0f)));
  EXPECT_EQ(Sim::clamp(kInf, 0.0f, 10.0f), 10.0f);
  EXPECT_EQ(Sim::clamp(kNegInf, 0.0f, 10.0f), 0.0f);
  // Vec2 clamp is component-wise.
  EXPECT_TRUE(Sim::equals(
      Sim::clamp(Vec2{15.0f, -5.0f}, Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f}),
      Vec2{10.0f, 0.0f}));
}

TEST(SimMathNanInf, LerpNanInfPolicy) {
  // NaN in any operand → NaN out.
  EXPECT_TRUE(Sim::isNaN(Sim::lerp(kNaN, 1.0f, 0.5f)));
  EXPECT_TRUE(Sim::isNaN(Sim::lerp(1.0f, kNaN, 0.5f)));
  EXPECT_TRUE(Sim::isNaN(Sim::lerp(1.0f, 2.0f, kNaN)));
  // IEEE: (inf - inf) = NaN, so lerp over an infinite span is NaN;
  // lerp(inf, -inf, t) hits inf + (-inf) = NaN.
  EXPECT_TRUE(Sim::isNaN(Sim::lerp(kInf, kInf, 0.5f)));
  EXPECT_TRUE(Sim::isNaN(Sim::lerp(kInf, kNegInf, 0.5f)));
}

TEST(SimMathNanInf, LengthPolicy) {
  // length((0,0)) = +0 (exact bits).
  EXPECT_EQ(bits(Sim::length(Vec2{})), 0u);
  // Infinite components give an infinite length.
  EXPECT_TRUE(Sim::isInf(Sim::length(Vec2{kInf, 0.0f})));
  EXPECT_TRUE(Sim::isInf(Sim::length(Vec2{kNegInf, 0.0f})));
  EXPECT_TRUE(Sim::isInf(Sim::length(Vec2{kInf, kNegInf})));
  // length is always >= 0 (ordered comparison with +0).
  EXPECT_TRUE(Sim::greaterEqual(Sim::length(Vec2{0.1f, -2.5f}), 0.0f));
  EXPECT_TRUE(Sim::greaterEqual(Sim::length(Vec2{}), 0.0f));
}

TEST(SimMathNanInf, NormalizePolicy) {
  // Policy: the zero vector normalizes to the zero vector — SimMath
  // never injects NaN from a zero-length input (IEEE 0/0 would give
  // NaN).
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec2{}), Vec2{}));
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec3{}), Vec3{}));
  // NaN components propagate.
  EXPECT_TRUE(Sim::isNaN(Sim::normalize(Vec2{kNaN, 1.0f}).x));
  // 3-4-5 normalizes exactly to (0.6, 0.8) (3/5 and 4/5 are exact
  // binary32 roundings of the literals).
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec2{3.0f, 4.0f}),
                          Vec2{0.6f, 0.8f}));
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec2{-3.0f, -4.0f}),
                          Vec2{-0.6f, -0.8f}));
}

// ---------------------------------------------------------------------------
// SimMathProperties — property tests + the pinned-flag runtime guards
// ---------------------------------------------------------------------------

TEST(SimMathProperties, ClampIdempotent) {
  // clamp(clamp(x)) == clamp(x) for every x in the policy domain,
  // including NaN and ±inf.
  const Scalar xs[] = {-1e30f, kNegInf, -1.0f, -0.0f, 0.0f, 1.0f, 7.5f,
                       std::numeric_limits<Scalar>::denorm_min(), kInf,
                       std::numeric_limits<Scalar>::max(), kNaN};
  for (const Scalar x : xs) {
    const Scalar once = Sim::clamp(x, -10.0f, 10.0f);
    EXPECT_TRUE(same(Sim::clamp(once, -10.0f, 10.0f), once)) << "x=" << x;
  }
}

TEST(SimMathProperties, LerpExactMidpointsAndExtrapolation) {
  // lerp(a, b, 0) is exact for every finite a, b: (b-a)*0 = ±0 and
  // a ± 0 = a.
  const Scalar aVals[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 3.25f};
  const Scalar bVals[] = {0.0f, 2.0f, -0.5f, 0.2f, 12345.25f};
  for (const Scalar a : aVals)
    for (const Scalar b : bVals)
      EXPECT_TRUE(same(Sim::lerp(a, b, 0.0f), a)) << "a=" << a << " b=" << b;
  // Midpoint with t = 0.5 is exact for dyadic a, b (*0.5 is exact
  // scaling).
  EXPECT_EQ(Sim::lerp(1.0f, 2.0f, 0.5f), 1.5f);
  EXPECT_EQ(Sim::lerp(-3.0f, 3.0f, 0.5f), 0.0f);
  EXPECT_TRUE(
      Sim::equals(Sim::lerp(Vec2{0.0f, 4.0f}, Vec2{2.0f, 0.0f}, 0.5f),
                  Vec2{1.0f, 2.0f}));
  // t outside [0,1] extrapolates by the same expression (defined).
  EXPECT_EQ(Sim::lerp(0.0f, 2.0f, 2.0f), 4.0f);
  EXPECT_EQ(Sim::lerp(0.0f, 2.0f, -1.0f), -2.0f);
}

TEST(SimMathProperties, LengthInvariantUnderSign) {
  // (-x)*(-x) == x*x bit-exactly (IEEE sign rule), so length is
  // invariant under per-component sign flips — even for NaN/inf
  // inputs.
  const Vec2 vs[] = {Vec2{0.1f, -2.5f}, Vec2{0.0f, 0.0f},
                     Vec2{123456.75f, 6.5536e-5f}};
  for (const Vec2 v : vs) {
    EXPECT_TRUE(same(Sim::length(v), Sim::length(Vec2{-v.x, v.y})));
    EXPECT_TRUE(same(Sim::length(v), Sim::length(Vec2{v.x, -v.y})));
    EXPECT_TRUE(same(Sim::length(v), Sim::length(Vec2{-v.x, -v.y})));
  }
  EXPECT_TRUE(same(Sim::length(Vec2{kNaN, 0.0f}),
                   Sim::length(Vec2{kNaN, -0.0f})));
}

TEST(SimMathProperties, NormalizeRoundTripWithinTolerance) {
  // length(normalize(v)) is within 4 ulp of 1.0: the error budget of the
  // two component divisions, two squarings, the sum, and the sqrt, each
  // at most 2^-24 relative (documented tolerance, CORE-005).
  const Scalar tol = 4.0f * std::numeric_limits<Scalar>::epsilon();
  const Vec2 vs[] = {Vec2{0.1f, 0.2f}, Vec2{3.0f, 4.0f},
                     Vec2{-7.25f, 0.03125f}, Vec2{123456.75f, -9.765625e-4f},
                     Vec2{1.0f, 1.0f}, Vec2{6.5536e-5f, 0.0f}};
  for (const Vec2 v : vs) {
    const Scalar len = Sim::length(Sim::normalize(v));
    EXPECT_TRUE(Sim::isFinite(len));
    EXPECT_LE(len, 1.0f + tol);
    EXPECT_GE(len, 1.0f - tol);
  }
}

TEST(SimMathProperties, AddSubInverseOnExactValues) {
  // On dyadic values in range, (a + b) - b == a is bit-exact (no
  // rounding occurs, so the inverse property holds).
  const Scalar xs[] = {0.0f, 1.0f, -1.0f, 0.25f, -0.25f, 4096.0f, -4096.0f};
  const Scalar ys[] = {1.0f, -1.0f, 2.5f, -2.5f, 0.5f, 8192.0f};
  for (const Scalar x : xs)
    for (const Scalar y : ys)
      EXPECT_EQ(Sim::sub(Sim::add(x, y), y), x) << "x=" << x << " y=" << y;
}

// The pinned flag set (ADR 0002: -ffp-contract=off) is verified at
// runtime, not only on the compile line. This loop is exactly the
// pattern FMA contraction targets: s = s + x[i]*y[i]. The reference path
// uses a volatile accumulator, so the optimizer must not transform it —
// it is the exact IEEE operation sequence of the expression. If the
// flags were missing (e.g. the target built with -ffp-contract=fast,
// -ffast-math, or MSVC /fp:fast), the engine path could fuse the
// multiply-add into one single-rounding FMA and the results diverge
// bit-wise; EXPECT_EQ then fails loudly (CORE-008: no silent failure).
TEST(SimMathProperties, DotProductCanaryDetectsFmaContraction) {
  constexpr int kN = 256;
  Scalar x[kN], y[kN];
  for (int i = 0; i < kN; ++i) {
    // Deterministic, bit-exact input generation: integer → float, then
    // exact literal scaling (no transcendental functions, no
    // platform-dependent libm).
    x[i] = static_cast<Scalar>((i * 7919) % 1000) * 0.001f - 0.5f;
    y[i] = static_cast<Scalar>((i * 104729) % 1000) * 0.001f - 0.5f;
  }
  volatile float ref = 0.0f;
  for (int i = 0; i < kN; ++i) ref = Sim::add(ref, Sim::mul(x[i], y[i]));
  float acc = 0.0f;
  for (int i = 0; i < kN; ++i) acc = Sim::add(acc, Sim::mul(x[i], y[i]));
  EXPECT_EQ(bits(acc), bits(static_cast<float>(ref)));
}

// SimMath::lerp is pinned to the two-rounding form a + (b - a) * t. If
// -ffp-contract=off were missing, the inlined lerp could be
// FMA-contracted to the single-rounding FMA(a, b-a, t), changing the
// result wherever the two roundings differ. The fused result is emulated
// exactly in double — for the magnitudes searched here every float
// intermediate stays within double's 53-bit significand — so the
// comparison is bit-exact. If the flags were missing, even the
// two-rounding reference would fuse, and the search would find no
// differing case; EXPECT_GE then fails loudly either way.
TEST(SimMathProperties, LerpIsNotFmaFused) {
  const Scalar aVals[] = {-4.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 2.0f, 4.0f,
                          0.1f, 0.2f, 0.3f, -0.3f, 1.5f, -2.5f};
  const Scalar bVals[] = {-4.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 4.0f,
                          0.2f, 0.3f, -0.2f, -1.5f, 2.5f, 0.7f, -0.7f};
  const Scalar tVals[] = {0.1f, 0.2f, 0.3f, 0.25f, 0.5f, 0.75f, 1.0f,
                          -0.25f};
  int fusedDiffers = 0;
  for (const Scalar a : aVals)
    for (const Scalar b : bVals)
      for (const Scalar t : tVals) {
        volatile float refA = a, refB = b, refT = t;
        const float twoStep = refA + (refB - refA) * refT;  // pinned form
        const float fused = static_cast<float>(
            static_cast<double>(refA) +
            (static_cast<double>(refB) - static_cast<double>(refA)) *
                static_cast<double>(refT));  // exact FMA emulation
        if (bits(twoStep) != bits(fused)) {
          ++fusedDiffers;
          EXPECT_EQ(bits(Sim::lerp(a, b, t)), bits(twoStep))
              << "a=" << a << " b=" << b << " t=" << t;
        }
      }
  // The search must actually find cases where the two roundings differ;
  // otherwise this test proves nothing.
  EXPECT_GE(fusedDiffers, 1);
}

// ---------------------------------------------------------------------------
// SimMathDispatch — compile-time dispatch mechanics (ADR 0002, PERF-006)
// ---------------------------------------------------------------------------

TEST(SimMathDispatch, StatelessCompileTimeDispatch) {
  // One template instantiation per backend, no per-call indirection: the
  // type is stateless and trivially copyable, so SimMath ops inline to
  // the backend's primitive operations.
  static_assert(std::is_trivially_copyable_v<Sim>);
  static_assert(sizeof(Sim) == 1);
  static_assert(std::is_same_v<Sim::Scalar, float>);
  static_assert(std::is_trivially_copyable_v<Vec2>);
  static_assert(sizeof(Vec2) == sizeof(float) * 2);
  static_assert(std::is_trivially_copyable_v<Vec3>);
  static_assert(sizeof(Vec3) == sizeof(float) * 3);

  // A cast to a `noexcept` function pointer type is ill-formed unless the
  // pointee is itself noexcept, so each line fails the build if a SimMath
  // op ever stops being noexcept (PERF-006 hot-path contract).
  using AddFn =
      decltype(static_cast<Scalar (*)(Scalar, Scalar) noexcept>(Sim::add));
  using SubFn =
      decltype(static_cast<Scalar (*)(Scalar, Scalar) noexcept>(Sim::sub));
  using MulFn =
      decltype(static_cast<Scalar (*)(Scalar, Scalar) noexcept>(Sim::mul));
  using DivFn =
      decltype(static_cast<Scalar (*)(Scalar, Scalar) noexcept>(Sim::div));
  using LenFn = decltype(static_cast<Scalar (*)(Vec2) noexcept>(Sim::length));
  using NormFn = decltype(static_cast<Vec2 (*)(Vec2) noexcept>(Sim::normalize));
  using LerpFn = decltype(static_cast<Scalar (*)(Scalar, Scalar, Scalar)
                                  noexcept>(Sim::lerp));
  using ClampFn = decltype(static_cast<Scalar (*)(Scalar, Scalar, Scalar)
                                 noexcept>(Sim::clamp));
  static_assert(std::is_nothrow_invocable_v<AddFn, Scalar, Scalar>);
  static_assert(std::is_nothrow_invocable_v<SubFn, Scalar, Scalar>);
  static_assert(std::is_nothrow_invocable_v<MulFn, Scalar, Scalar>);
  static_assert(std::is_nothrow_invocable_v<DivFn, Scalar, Scalar>);
  static_assert(std::is_nothrow_invocable_v<LenFn, Vec2>);
  static_assert(std::is_nothrow_invocable_v<NormFn, Vec2>);
  static_assert(std::is_nothrow_invocable_v<LerpFn, Scalar, Scalar, Scalar>);
  static_assert(std::is_nothrow_invocable_v<ClampFn, Scalar, Scalar, Scalar>);

  // The factory form (ADR 0002: factory-selected at init): a held handle
  // dispatches to the same pinned ops.
  const Sim math = Sim::create();
  EXPECT_EQ(math.add(1.0f, 1.0f), 2.0f);
  EXPECT_EQ(math.sub(3.0f, 1.0f), 2.0f);
  EXPECT_EQ(math.mul(2.0f, 3.0f), 6.0f);
  EXPECT_EQ(math.div(8.0f, 2.0f), 4.0f);
  EXPECT_TRUE(Sim::equals(math.normalize(Vec2{0.0f, 5.0f}),
                          Vec2{0.0f, 1.0f}));
  EXPECT_EQ(math.clamp(15.0f, 0.0f, 10.0f), 10.0f);
  EXPECT_EQ(math.lerp(0.0f, 8.0f, 0.25f), 2.0f);
  EXPECT_EQ(math.length(Vec2{3.0f, 4.0f}), 5.0f);
}

TEST(SimMathDispatch, Fp32PinnedBackendContract) {
  // The backend contract (ADR 0002): one correctly-rounded operation per
  // primitive, deterministic by the header's pinned scope.
  using B = laige::sim::Fp32Pinned;
  static_assert(std::is_same_v<B::Scalar, float>);
  static_assert(std::is_trivially_copyable_v<B>);
  EXPECT_EQ(B::add(1.0f, 1.0f), 2.0f);
  EXPECT_EQ(B::sub(3.0f, 1.0f), 2.0f);
  EXPECT_EQ(B::mul(2.0f, 3.0f), 6.0f);
  EXPECT_TRUE(Sim::isInf(B::div(1.0f, 0.0f)));
  EXPECT_TRUE(Sim::isNaN(B::div(0.0f, 0.0f)));
  EXPECT_EQ(B::sqrt(4.0f), 2.0f);
  EXPECT_EQ(B::sqrt(0.0f), 0.0f);
  EXPECT_TRUE(Sim::isNaN(B::sqrt(-1.0f)));
  // inf*inf = inf, sqrt(inf) = inf.
  EXPECT_TRUE(Sim::isInf(B::sqrt(B::mul(kInf, kInf))));
}
