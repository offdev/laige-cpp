// laige-core SimMath fpx16_16 backend suite (M0-CORE-04).
//
// Step Verify scope (roadmap/M0-foundations.md):
//   - `ctest -R math_fixed` green (suites: FixedPointBasics,
//     FixedPointRounding, FixedPointSaturation, FixedPointConversions,
//     FixedPointSimMath, FixedPointDispatch, FixedPointDeterminism)
//   - Green under ASan+UBSan (canonical build-asan tree)
//   - Exhaustive edge cases: min/max, wrap candidates, rounding ties
//   - Property test: the same 4096-tick op sequence run through the
//     SimMath ops and through an independent raw-int64 reference
//     implementation produces identical results, and the sequence's
//     FNV-1a state hash is a committed known-answer constant —
//     identical across two different compiler builds (verified locally;
//     CI hookup lands in M1-DET-04)
//
// House convention (also enforced by -Wall -Werror): raw `float`
// equality comparisons trigger -Wfloat-equal, so float checks go through
// `double` comparisons (exact dyadic oracles) — never EXPECT_EQ on
// floats. Every value under test comes from a fpx16_16 / SimMath op;
// float/double values appear only as oracles for known values
// (PRD §10.3, ADR 0002).

#include <cstdint>
#include <limits>
#include <type_traits>

#include "gtest/gtest.h"
#include "laige/sim_math.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "math_fixed_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "math_fixed_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "math_fixed_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

namespace {

using Sim = laige::sim::SimMathFpx16;
using Scalar = Sim::Scalar;  // laige::fpx16_16
using Vec2 = Sim::Vec2;
using Vec3 = Sim::Vec3;
using Fpx = laige::sim::Fpx16_16;  // the backend's raw primitives

Scalar S(float v) { return Scalar::fromFloat(v); }
constexpr Scalar R(std::int32_t raw) { return Scalar{raw}; }

constexpr std::int32_t kMinRaw = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kMaxRaw = std::numeric_limits<std::int32_t>::max();

// ---------------------------------------------------------------------------
// Fixed 4096-tick op sequence (the determinism test's workload).
// Exposes add, sub, mul, div, negate, sqrt, lerp, clamp, normalize, and
// every saturation path of interest.
// ---------------------------------------------------------------------------

struct FxState {
  std::int32_t px{}, py{}, vx{}, vy{};
};

// Engine path: the SimMath<Fpx16_16> ops under test.
FxState engineRun() {
  Vec2 pos{S(1.5f), S(-2.25f)};
  Vec2 vel{S(0.25f), S(0.75f)};
  const Scalar g = S(0.015625f);      // 1/64, dyadic
  const Scalar drag = S(0.9921875f);  // 127/128, dyadic
  const Vec2 velLo{S(-16.0f), S(-16.0f)}, velHi{S(16.0f), S(16.0f)};
  const Vec2 posLo{S(-32.0f), S(-32.0f)}, posHi{S(32.0f), S(32.0f)};
  const Vec2 home{S(5.0f), S(-3.0f)};
  const Vec2 kick{S(0.1f), S(0.0f)};
  for (std::int32_t t = 0; t < 4096; ++t) {
    vel.y = Sim::add(vel.y, Scalar::negate(g));
    vel = Sim::mul(vel, drag);
    vel = Sim::clamp(vel, velLo, velHi);
    pos = Sim::add(pos, vel);
    pos = Sim::clamp(pos, posLo, posHi);
    if (t % 512 == 255) pos = Sim::lerp(pos, home, S(0.125f));
    if (t % 1024 == 511) vel = Sim::normalize(Sim::add(vel, kick));
    if (t % 2048 == 1023) vel.x = Sim::div(vel.x, S(2.0f));
  }
  return FxState{pos.x.raw, pos.y.raw, vel.x.raw, vel.y.raw};
}

// Reference path: the SAME op sequence written directly on int64_t — an
// independent implementation of the documented fpx16_16 policy (no
// fpx16_16 helpers are called, and the integer sqrt uses a pure-integer
// Newton iteration instead of the engine's double-seeded one). It
// catches drift between the type's ops and the documented policy.
FxState refRun() {
  auto clampR = [](std::int64_t v) -> std::int32_t {
    if (v < kMinRaw) return kMinRaw;
    if (v > kMaxRaw) return kMaxRaw;
    return static_cast<std::int32_t>(v);
  };
  auto addR = [&](std::int32_t a, std::int32_t b) {
    return clampR(static_cast<std::int64_t>(a) + b);
  };
  auto subR = [&](std::int32_t a, std::int32_t b) {
    return clampR(static_cast<std::int64_t>(a) - b);
  };
  auto mulR = [&](std::int32_t a, std::int32_t b) {
    const std::int64_t p = static_cast<std::int64_t>(a) * b;
    std::int64_t q = p >> 16;
    const std::int64_t rem = p & 0xFFFF;
    if (rem >= 0x8000 && (rem > 0x8000 || (q & 1))) ++q;
    return clampR(q);
  };
  auto divR = [&](std::int32_t a, std::int32_t b) {
    const std::int64_t num = static_cast<std::int64_t>(a) << 16;
    const std::int64_t den = b;
    if (den == 0) return a == 0 ? 0 : (a < 0 ? kMinRaw : kMaxRaw);
    std::int64_t q = num / den;
    const std::int64_t r = num % den;
    const std::int64_t ad = den < 0 ? -den : den;
    const std::int64_t ar = r < 0 ? -r : r;
    if (2 * ar > ad || (2 * ar == ad && (q & 1))) q += (q >= 0) ? 1 : -1;
    return clampR(q);
  };
  auto isqrtNewt = [](std::uint64_t n) -> std::uint64_t {
    if (n == 0) return 0;
    int bits = 0;
    for (std::uint64_t t = n; t; t >>= 1) ++bits;
    std::uint64_t s = 1ull << ((bits + 1) / 2);  // ≥ sqrt(n)
    for (;;) {
      const std::uint64_t t = (s + n / s) >> 1;
      if (t >= s) break;
      s = t;
    }
    while (s * s > n) --s;
    while ((s + 1) * (s + 1) <= n) ++s;
    return s;
  };
  auto sqrtR = [&](std::int32_t v) -> std::int32_t {
    if (v <= 0) return 0;
    const std::uint64_t n = static_cast<std::uint64_t>(static_cast<std::uint32_t>(v)) << 16;
    const std::uint64_t s = isqrtNewt(n);
    const std::uint64_t r = s + (n - s * s > s ? 1 : 0);
    return static_cast<std::int32_t>(r);
  };
  auto normR = [&](std::int32_t x, std::int32_t y) -> Vec2 {
    const std::int32_t len = sqrtR(addR(mulR(x, x), mulR(y, y)));
    if (len == 0) return Vec2{};
    return Vec2{R(divR(x, len)), R(divR(y, len))};
  };
  auto clampC = [](std::int32_t v, std::int32_t lo, std::int32_t hi) {
    return v > hi ? hi : (v < lo ? lo : v);
  };
  auto lerpR = [&](std::int32_t a, std::int32_t b, std::int32_t t) {
    return addR(a, mulR(subR(b, a), t));
  };

  // Starting state: (1.5, -2.25), (0.25, 0.75) in Q16.16 raw units.
  std::int32_t px = 102400, py = -147456, vx = 16384, vy = 49152;
  const std::int32_t g = 1024;         // 1/64
  const std::int32_t drag = 65024;     // 127/128 * 2^16 = 127 * 512
  const std::int32_t vLo = -1048576, vHi = 1048576;  // ±16
  const std::int32_t pLo = -2097152, pHi = 2097152;  // ±32
  const std::int32_t hx = 327680, hy = -196608;      // (5, -3)
  const std::int32_t eighths = 8192;                 // 1/8
  // S(0.1f) = nearbyint(0.1f * 65536) = nearbyint(6553.6) = 6554.
  const std::int32_t kickX = 6554;
  const std::int32_t two = 131072;

  for (std::int32_t t = 0; t < 4096; ++t) {
    vy = addR(vy, clampR(-static_cast<std::int64_t>(g)));  // negate(g), exact
    vx = mulR(vx, drag);
    vy = mulR(vy, drag);
    vx = clampC(vx, vLo, vHi);
    vy = clampC(vy, vLo, vHi);
    px = addR(px, vx);
    py = addR(py, vy);
    px = clampC(px, pLo, pHi);
    py = clampC(py, pLo, pHi);
    if (t % 512 == 255) {
      px = lerpR(px, hx, eighths);
      py = lerpR(py, hy, eighths);
    }
    if (t % 1024 == 511) {
      const Vec2 n = normR(addR(vx, kickX), vy);
      vx = n.x.raw;
      vy = n.y.raw;
    }
    if (t % 2048 == 1023) vx = divR(vx, two);
  }
  return FxState{px, py, vx, vy};
}

// FNV-1a 64-bit over the final state, big-endian byte order per raw
// value (endianness-independent); unsigned overflow is well-defined.
std::uint64_t stateHash(const FxState& s) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  auto feed = [&h](std::int32_t v) {
    const std::uint32_t u = static_cast<std::uint32_t>(v);
    for (int shift = 24; shift >= 0; shift -= 8) {
      h ^= (u >> shift) & 0xFFu;
      h *= 0x100000001b3ull;
    }
  };
  feed(s.px);
  feed(s.py);
  feed(s.vx);
  feed(s.vy);
  return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// FixedPointBasics — exact values, identities, total order
// ---------------------------------------------------------------------------

TEST(FixedPointBasics, FormatAndConstants) {
  static_assert(sizeof(Scalar) == 4);
  EXPECT_EQ(Scalar{}.raw, 0);
  EXPECT_EQ(Scalar::min().raw, kMinRaw);    // -32768.0
  EXPECT_EQ(Scalar::max().raw, kMaxRaw);    // +32767.99998474
  EXPECT_EQ(Scalar::one().raw, 1 << 16);
  EXPECT_TRUE(Scalar::min() < Scalar::max());
  EXPECT_TRUE(Scalar::min() <= Scalar::max());
  EXPECT_TRUE(Scalar::max() > Scalar::min());
  EXPECT_TRUE(Scalar::max() >= Scalar::min());
  EXPECT_FALSE(Scalar::min() == Scalar::max());
  EXPECT_TRUE(Scalar::one() == Scalar::one());
  // Total order: isOrdered is always true (no NaN), isFinite always true,
  // isNaN / isInf always false.
  EXPECT_TRUE(Sim::isOrdered(Scalar::min(), Scalar::max()));
  EXPECT_TRUE(Sim::isFinite(Scalar::max()));
  EXPECT_TRUE(Sim::isFinite(Scalar::min()));
  EXPECT_FALSE(Sim::isNaN(Scalar::one()));
  EXPECT_FALSE(Sim::isInf(Scalar::max()));
  EXPECT_FALSE(Sim::isInf(Scalar::min()));
}

TEST(FixedPointBasics, ExactArithmetic) {
  // Exact dyadic sums/differences (no rounding occurs).
  EXPECT_EQ(Sim::add(S(1.5f), S(2.25f)), S(3.75f));
  EXPECT_EQ(Sim::sub(S(1.5f), S(2.25f)), S(-0.75f));
  // Exact dyadic product.
  EXPECT_EQ(Sim::mul(S(0.5f), S(3.5f)), S(1.75f));
  // Exact divisions (quotients are dyadic).
  EXPECT_EQ(Sim::div(S(1.0f), S(2.0f)), S(0.5f));
  EXPECT_EQ(Sim::div(S(-4.0f), S(2.0f)), S(-2.0f));
  // Rounded division, exact value: 3/5 = 0.6 → round(0.6 * 2^16) = 39322.
  EXPECT_EQ(Sim::div(S(3.0f), S(5.0f)).raw, 39322);
  // Identities.
  EXPECT_TRUE(Sim::equals(Sim::add(S(2.5f), Scalar{}), S(2.5f)));
  EXPECT_TRUE(Sim::equals(Sim::sub(S(2.5f), S(2.5f)), Scalar{}));
  EXPECT_TRUE(Sim::equals(Sim::mul(S(2.5f), Scalar::one()), S(2.5f)));
  EXPECT_TRUE(Sim::equals(Sim::div(S(2.5f), Scalar::one()), S(2.5f)));
  // Negation (exact away from min).
  EXPECT_TRUE(Sim::equals(Sim::sub(Scalar{}, S(1.5f)), S(-1.5f)));
  EXPECT_EQ(Scalar::negate(S(1.5f)), S(-1.5f));
  EXPECT_EQ(Scalar::negate(Scalar{}), Scalar{});
  // The non-saturating boundary: max + min is exactly -1 ulp.
  EXPECT_EQ(Sim::add(Scalar::max(), Scalar::min()), R(-1));
}

TEST(FixedPointBasics, Commutativity) {
  // Integer addition/multiplication are commutative; the saturating
  // clamps are symmetric, so the engine ops stay commutative too.
  const std::int32_t xs[] = {0, 1, -1, 0x8000, -0x8000, 1 << 16, -(1 << 16),
                             2147418112, -2147483648, 2147483647, 32768,
                             12345678};
  for (const std::int32_t a : xs)
    for (const std::int32_t b : xs) {
      EXPECT_EQ(Sim::add(R(a), R(b)), Sim::add(R(b), R(a))) << "a=" << a;
      EXPECT_EQ(Sim::mul(R(a), R(b)), Sim::mul(R(b), R(a))) << "a=" << a;
    }
}

// ---------------------------------------------------------------------------
// FixedPointRounding — round-to-nearest ties-to-even, exhaustive ties
// ---------------------------------------------------------------------------

TEST(FixedPointRounding, MulTiesToEven) {
  // Exact ties (product low 16 bits == 0x8000): the EVEN neighbor wins.
  // 1 ulp * 0.5 = 0.5 ulp → 0 (even).
  EXPECT_EQ(Scalar::mul(R(1), R(0x8000)), R(0));
  // 1 ulp * 1.5 = 1.5 ulp → 2 (even).
  EXPECT_EQ(Scalar::mul(R(1), R(0x18000)), R(2));
  // -1 ulp * 1.5 = -1.5 ulp → -2 (even).
  EXPECT_EQ(Scalar::mul(R(-1), R(0x18000)), R(-2));
  // Non-tie roundings: 0.75 → 1, -0.75 → -1, 0.25 → 0, -0.25 → 0.
  EXPECT_EQ(Scalar::mul(R(3), R(0x4000)), R(1));
  EXPECT_EQ(Scalar::mul(R(-3), R(0x4000)), R(-1));
  EXPECT_EQ(Scalar::mul(R(1), R(0x4000)), R(0));
  EXPECT_EQ(Scalar::mul(R(-1), R(0x4000)), R(0));
}

TEST(FixedPointRounding, DivTiesToEven) {
  // Exact ties (remainder == half divisor): the EVEN neighbor wins.
  // 1 ulp / 2 = 0.5 ulp → 0 (even).
  EXPECT_EQ(Scalar::div(R(1), R(131072)), R(0));
  // 3 ulp / 2 = 1.5 ulp → 2 (even).
  EXPECT_EQ(Scalar::div(R(3), R(131072)), R(2));
  // Non-tie roundings: 1/3 ulp = 21845.33… → 21845; 1/6 ulp = 10922.66… →
  // 10923.
  EXPECT_EQ(Scalar::div(R(1), R(3)), R(21845));
  EXPECT_EQ(Scalar::div(R(1), R(6)), R(10923));
  // x / x is exactly one for odd, non-power-of-two x (no rounding).
  EXPECT_EQ(Scalar::div(R(1234567), R(1234567)), Scalar::one());
  EXPECT_EQ(Scalar::div(R(-1234567), R(-1234567)), Scalar::one());
}

TEST(FixedPointRounding, ToInt32TiesToEven) {
  // 0.5 → 0 (even), 1.5 → 2 (even), -0.5 → 0, -1.5 → -2 (even).
  EXPECT_EQ(Scalar::toInt32(R(0x8000)), 0);
  EXPECT_EQ(Scalar::toInt32(R(0x18000)), 2);
  EXPECT_EQ(Scalar::toInt32(R(-0x8000)), 0);
  EXPECT_EQ(Scalar::toInt32(R(-0x18000)), -2);
  // Near the top: 32767.5 → 32768 (even); 32766.5 → 32766 (even).
  EXPECT_EQ(Scalar::toInt32(R(0x7FFF8000)), 32768);
  EXPECT_EQ(Scalar::toInt32(R(0x7FFE8000)), 32766);
  // Non-tie: 0.25 → 0, 0.75 → 1.
  EXPECT_EQ(Scalar::toInt32(R(0x4000)), 0);
  EXPECT_EQ(Scalar::toInt32(R(0xC000)), 1);
  // Exact integers round to themselves.
  EXPECT_EQ(Scalar::toInt32(R(12345678)), 12345678 / 65536);
}

TEST(FixedPointRounding, FromFloatTiesToEven) {
  // v * 2^16 exactly k + 0.5: the EVEN neighbor wins.
  EXPECT_EQ(Scalar::fromFloat(0.5f / 65536.0f), R(0));    // 0.5 → 0
  EXPECT_EQ(Scalar::fromFloat(1.5f / 65536.0f), R(2));    // 1.5 → 2
  EXPECT_EQ(Scalar::fromFloat(-0.5f / 65536.0f), R(0));   // -0.5 → 0
  EXPECT_EQ(Scalar::fromFloat(-1.5f / 65536.0f), R(-2));  // -1.5 → -2
  // Non-tie: 0.25 ulp-ish values.
  EXPECT_EQ(Scalar::fromFloat(0.25f / 65536.0f), R(0));
  EXPECT_EQ(Scalar::fromFloat(0.75f / 65536.0f), R(1));
}

TEST(FixedPointRounding, SqrtRounding) {
  // Perfect squares are exact.
  EXPECT_EQ(Scalar::sqrt(S(25.0f)), S(5.0f));
  EXPECT_EQ(Scalar::sqrt(S(0.25f)), S(0.5f));
  EXPECT_EQ(Scalar::sqrt(S(1.0f)), S(1.0f));
  // Non-squares round to nearest (no ties can occur for sqrt of an
  // integer): round(sqrt(2) * 2^16) = round(92681.90…) = 92682;
  // round(sqrt(3) * 2^16) = round(113511.68…) = 113512.
  EXPECT_EQ(Scalar::sqrt(S(2.0f)), R(92682));
  EXPECT_EQ(Scalar::sqrt(S(3.0f)), R(113512));
  // Domain: sqrt(0) = 0; sqrt of a negative is defined as +0 (no NaN).
  EXPECT_EQ(Scalar::sqrt(R(0)), R(0));
  EXPECT_EQ(Scalar::sqrt(R(-1)), R(0));
  EXPECT_EQ(Scalar::sqrt(Scalar::min()), R(0));
}

// ---------------------------------------------------------------------------
// FixedPointSaturation — overflow is defined (saturating); wrap
// candidates
// ---------------------------------------------------------------------------

TEST(FixedPointSaturation, AddSubSaturate) {
  EXPECT_EQ(Sim::add(Scalar::max(), R(1)), Scalar::max());
  EXPECT_EQ(Sim::add(Scalar::max(), Scalar::max()), Scalar::max());
  EXPECT_EQ(Sim::add(Scalar::min(), Scalar::min()), Scalar::min());
  EXPECT_EQ(Sim::add(Scalar::min(), R(-1)), Scalar::min());
  EXPECT_EQ(Sim::sub(Scalar::min(), Scalar::max()), Scalar::min());
  EXPECT_EQ(Sim::sub(Scalar::max(), Scalar::min()), Scalar::max());
  // Wrap candidates that stay in range: no saturation.
  EXPECT_EQ(Sim::sub(Scalar::max(), R(1)), R(kMaxRaw - 1));  // 2147483646
  EXPECT_EQ(Sim::add(Scalar::min(), R(1)), R(kMinRaw + 1));  // -2147483647
}

TEST(FixedPointSaturation, MulSaturates) {
  EXPECT_EQ(Scalar::mul(Scalar::max(), Scalar::max()), Scalar::max());
  EXPECT_EQ(Scalar::mul(Scalar::min(), Scalar::min()), Scalar::max());
  EXPECT_EQ(Scalar::mul(Scalar::max(), Scalar::min()), Scalar::min());
  EXPECT_EQ(Scalar::mul(S(2.0f), Scalar::max()), Scalar::max());
  // min * 1 ulp = -0.5 exactly (in range — no saturation); min * (1 +
  // 1 ulp) = -32768.5 (not representable — saturates to min).
  EXPECT_EQ(Scalar::mul(Scalar::min(), R(1)), R(-32768));
  EXPECT_EQ(Scalar::mul(Scalar::min(), R(65537)), Scalar::min());
  EXPECT_EQ(Scalar::mul(R(65537), Scalar::min()), Scalar::min());
  // The non-saturating boundary: 181 * 181 = 32761 ≤ max; 181 * 182 =
  // 32942 > max.
  EXPECT_EQ(Scalar::mul(S(181.0f), S(181.0f)), S(32761.0f));
  EXPECT_EQ(Scalar::mul(S(181.0f), S(182.0f)), Scalar::max());
}

TEST(FixedPointSaturation, DivAndNegateSaturate) {
  EXPECT_EQ(Scalar::div(Scalar::max(), R(1)), Scalar::max());
  EXPECT_EQ(Scalar::div(Scalar::min(), R(1)), Scalar::min());
  // max / min = -0.99999999953… → exactly -1 (rounded, in range).
  EXPECT_EQ(Scalar::div(Scalar::max(), Scalar::min()), R(-(1 << 16)));
  // Division by zero is defined: x/0 → ±max (sign of x), 0/0 → +0.
  EXPECT_EQ(Scalar::div(R(1), R(0)), Scalar::max());
  EXPECT_EQ(Scalar::div(R(-1), R(0)), Scalar::min());
  EXPECT_EQ(Scalar::div(R(0), R(0)), R(0));
  EXPECT_EQ(Scalar::div(R(0), Scalar::max()), R(0));
  // Negation saturates only at min: -(-2^16) is not representable.
  // -max = -32767.99998 IS representable (exact).
  EXPECT_EQ(Scalar::negate(Scalar::min()), Scalar::max());
  EXPECT_EQ(Scalar::negate(Scalar::max()), R(-2147483647));
  EXPECT_EQ(Scalar::negate(R(0)), R(0));
}

TEST(FixedPointSaturation, ConversionSaturation) {
  // fromInt32 saturates outside the Q16.16 range (defined, no UB).
  EXPECT_EQ(Scalar::fromInt32(32768), Scalar::max());
  EXPECT_EQ(Scalar::fromInt32(-32768), Scalar::min());
  EXPECT_EQ(Scalar::fromInt32(kMaxRaw), Scalar::max());
  EXPECT_EQ(Scalar::fromInt32(kMinRaw), Scalar::min());
  EXPECT_EQ(Scalar::fromInt32(32767), R(32767 << 16));
  // fromFloat saturates at ±inf and |v| ≥ 32768; NaN → +0 (defined).
  EXPECT_EQ(Scalar::fromFloat(32768.0f), Scalar::max());
  EXPECT_EQ(Scalar::fromFloat(-32768.0f), Scalar::min());
  EXPECT_EQ(Scalar::fromFloat(std::numeric_limits<float>::infinity()),
            Scalar::max());
  EXPECT_EQ(
      Scalar::fromFloat(-std::numeric_limits<float>::infinity()),
      Scalar::min());
  EXPECT_EQ(Scalar::fromFloat(std::numeric_limits<float>::quiet_NaN()),
            R(0));
}

// ---------------------------------------------------------------------------
// FixedPointConversions — int32/float round trips and exact values
// ---------------------------------------------------------------------------

TEST(FixedPointConversions, IntRoundTrip) {
  const std::int32_t vs[] = {-32768, -12345, -1, 0, 1, 12345, 32767};
  for (const std::int32_t v : vs) {
    EXPECT_EQ(Scalar::toInt32(Scalar::fromInt32(v)), v) << "v=" << v;
  }
  // The exact range endpoints.
  EXPECT_EQ(Scalar::toInt32(Scalar::max()), 32768);  // 32767.99998 → 32768
  EXPECT_EQ(Scalar::toInt32(Scalar::min()), -32768);
}

TEST(FixedPointConversions, ToFloatOneRounding) {
  // Exact dyadic values: raw → float is exact for |raw| < 2^24.
  EXPECT_EQ(static_cast<double>(Scalar::toFloat(R(1 << 16))), 1.0);
  EXPECT_EQ(static_cast<double>(Scalar::toFloat(R(1 << 15))), 0.5);
  EXPECT_EQ(static_cast<double>(Scalar::toFloat(R(1))), 1.0 / 65536.0);
  EXPECT_EQ(static_cast<double>(Scalar::toFloat(R(12345678))),
            12345678.0 / 65536.0);
  // The single rounding at the top: raw 2^31-1 rounds to 2^31 in
  // binary32, which scales to exactly 32768.0.
  EXPECT_EQ(static_cast<double>(Scalar::toFloat(Scalar::max())), 32768.0);
  EXPECT_EQ(static_cast<double>(Scalar::toFloat(Scalar::min())), -32768.0);
}

TEST(FixedPointConversions, FromFloatExactAndRoundTrip) {
  // Exact dyadic conversions.
  EXPECT_EQ(Scalar::fromFloat(0.5f), R(1 << 15));
  EXPECT_EQ(Scalar::fromFloat(1.0f / 65536.0f), R(1));
  EXPECT_EQ(Scalar::fromFloat(-32767.0f), R(-32767 << 16));
  EXPECT_EQ(Scalar::fromFloat(0.0f), R(0));
  EXPECT_EQ(Scalar::fromFloat(-0.0f), R(0));
  // Round trip: fromFloat(toFloat(x)) lands on the nearest float-
  // representable raw. For |raw| < 2^24 the float holds all 24 bits and
  // the round trip is exact; beyond that, the binary32 mantissa error is
  // bounded by half a ulp of raw, ≤ 2^6 = 64 raw units (raw < 2^31).
  // Deterministic LCG scan over the whole range.
  std::uint32_t seed = 0x12345678u;
  for (int i = 0; i < 20000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const std::int32_t raw = static_cast<std::int32_t>(seed);
    const Scalar rt = Scalar::fromFloat(Scalar::toFloat(R(raw)));
    const std::int32_t diff = rt.raw > raw ? rt.raw - raw : raw - rt.raw;
    EXPECT_LE(diff, 64) << "raw=" << raw;
    if (raw >= 0 && raw < (1 << 24)) {
      EXPECT_EQ(rt.raw, raw) << "raw=" << raw;
    }
  }
}

// ---------------------------------------------------------------------------
// FixedPointSimMath — the shared op surface over the fixed-point backend
// ---------------------------------------------------------------------------

TEST(FixedPointSimMath, FactoryAndScalars) {
  const Sim m = Sim::create();
  EXPECT_EQ(m.add(S(1.0f), S(2.0f)), S(3.0f));
  EXPECT_EQ(m.sub(S(3.0f), S(1.0f)), S(2.0f));
  EXPECT_EQ(m.mul(S(2.0f), S(3.0f)), S(6.0f));
  EXPECT_EQ(m.div(S(8.0f), S(2.0f)), S(4.0f));
  EXPECT_EQ(m.clamp(S(15.0f), S(0.0f), S(10.0f)), S(10.0f));
  EXPECT_EQ(m.clamp(S(-5.0f), S(0.0f), S(10.0f)), Scalar{});
  EXPECT_EQ(m.lerp(S(0.0f), S(8.0f), S(0.25f)), S(2.0f));
  EXPECT_EQ(m.length(Vec2{S(3.0f), S(4.0f)}), S(5.0f));
  EXPECT_TRUE(Sim::equals(m.normalize(Vec2{S(0.0f), S(5.0f)}),
                          Vec2{Scalar{}, Scalar::one()}));
}

TEST(FixedPointSimMath, LerpExactAndExtrapolation) {
  // lerp(a, b, 0) is exact for every a, b ((b-a)*0 = 0).
  EXPECT_TRUE(Sim::equals(Sim::lerp(S(1.25f), S(-7.5f), Scalar{}),
                          S(1.25f)));
  // Dyadic midpoints are exact.
  EXPECT_EQ(Sim::lerp(S(1.0f), S(3.0f), S(0.5f)), S(2.0f));
  EXPECT_TRUE(Sim::equals(Sim::lerp(Vec2{S(0.0f), S(4.0f)},
                                    Vec2{S(2.0f), S(0.0f)}, S(0.5f)),
                          Vec2{S(1.0f), S(2.0f)}));
  // t outside [0,1] extrapolates by the same expression (defined).
  EXPECT_EQ(Sim::lerp(S(0.0f), S(2.0f), S(2.0f)), S(4.0f));
  EXPECT_EQ(Sim::lerp(S(0.0f), S(2.0f), S(-1.0f)), S(-2.0f));
  // Saturation inside lerp, fully determined by the documented chain:
  // max - min = 2^32 - 1 → saturates to max; max * 0.5 rounds (tie,
  // odd floor) to 2^30 = 16384.0; min + 16384.0 = -16384.0 exactly.
  EXPECT_EQ(Sim::lerp(Scalar::min(), Scalar::max(), S(0.5f)),
            R(-1073741824));
}

TEST(FixedPointSimMath, ClampAndVectors) {
  EXPECT_EQ(Sim::clamp(S(10.0f), S(0.0f), S(10.0f)), S(10.0f));
  EXPECT_TRUE(Sim::equals(
      Sim::clamp(Vec2{S(15.0f), S(-5.0f)}, Vec2{Scalar{}, Scalar{}},
                 Vec2{S(10.0f), S(10.0f)}),
      Vec2{S(10.0f), Scalar{}}));
  // Vector arithmetic is component-wise.
  const Vec2 p{S(1.5f), S(-2.25f)};
  const Vec2 q{S(0.25f), S(4.0f)};
  EXPECT_TRUE(Sim::equals(Sim::add(p, q), Vec2{S(1.75f), S(1.75f)}));
  EXPECT_TRUE(Sim::equals(Sim::sub(p, q), Vec2{S(1.25f), S(-6.25f)}));
  EXPECT_TRUE(Sim::equals(Sim::mul(p, q), Vec2{S(0.375f), S(-9.0f)}));
  EXPECT_TRUE(Sim::equals(Sim::mul(p, S(2.0f)), Vec2{S(3.0f), S(-4.5f)}));
  EXPECT_TRUE(Sim::equals(Sim::mul(S(2.0f), p), Sim::mul(p, S(2.0f))));
  // The zero vector is the additive identity (bit-exact).
  EXPECT_TRUE(Sim::equals(Sim::add(p, Vec2{}), p));
  EXPECT_TRUE(Sim::equals(Sim::sub(p, p), Vec2{}));
  // Vector equality is component-wise exact.
  EXPECT_TRUE(Sim::equals(Vec2{S(1.0f), S(2.0f)}, Vec2{S(1.0f), S(2.0f)}));
  EXPECT_TRUE(Sim::notEquals(Vec2{S(1.0f), S(2.0f)}, Vec2{S(1.0f), S(3.0f)}));
  EXPECT_TRUE(Sim::equals(Vec3{S(1.0f), S(2.0f), S(3.0f)},
                          Vec3{S(1.0f), S(2.0f), S(3.0f)}));
}

TEST(FixedPointSimMath, LengthAndNormalize) {
  // 3-4-5 is exact (25 is a perfect square).
  EXPECT_EQ(Sim::length(Vec2{S(3.0f), S(4.0f)}), S(5.0f));
  EXPECT_EQ(Sim::length(Vec2{}), Scalar{});
  // length is invariant under per-component sign flips (the exact
  // squares are identical; away from min, where negate saturates).
  const Vec2 v{S(0.1f), S(-2.5f)};
  const Vec2 nx{Scalar::negate(v.x), v.y};
  const Vec2 ny{v.x, Scalar::negate(v.y)};
  const Vec2 nxy{Scalar::negate(v.x), Scalar::negate(v.y)};
  EXPECT_TRUE(Sim::equals(Sim::length(v), Sim::length(nx)));
  EXPECT_TRUE(Sim::equals(Sim::length(v), Sim::length(ny)));
  EXPECT_TRUE(Sim::equals(Sim::length(v), Sim::length(nxy)));
  // normalize: zero vector → zero vector (policy); 3-4-5 → (0.6, 0.8)
  // exactly (39322 = round(3/5 * 2^16), 52429 = round(4/5 * 2^16) —
  // the same raws fromFloat(0.6f)/fromFloat(0.8f) produce).
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec2{}), Vec2{}));
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec3{}), Vec3{}));
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec2{S(3.0f), S(4.0f)}),
                          Vec2{S(0.6f), S(0.8f)}));
  EXPECT_TRUE(Sim::equals(Sim::normalize(Vec3{S(0.0f), S(0.0f), S(3.0f)}),
                          Vec3{Scalar{}, Scalar{}, Scalar::one()}));
  // The 3-4-12 → 13 Vec3 case is exact.
  EXPECT_EQ(Sim::length(Vec3{S(3.0f), S(4.0f), S(12.0f)}), S(13.0f));
}

// The documented accuracy bound of the fixed-point length: accurate while
// x*x + y*y stays inside the Q16.16 range, saturating beyond (defined,
// deterministic — see fpx16_16.h and docs/api/sim_math.md).
TEST(FixedPointSimMath, LengthSaturatesBeyondTheQ1616Range) {
  // 181^2 = 32761 ≤ max: exact (25-style perfect square).
  EXPECT_EQ(Sim::length(Vec2{S(181.0f), Scalar{}}), S(181.0f));
  // 182^2 = 33124 > max: the square saturates to max, so the length is
  // sqrt(max) ≈ 181.0193 — an UNDERestimate of the true length (the
  // saturation direction for length), still deterministic.
  EXPECT_EQ(Sim::length(Vec2{S(182.0f), Scalar{}}),
            Fpx::sqrt(Scalar::max()));
  EXPECT_TRUE(Sim::less(Sim::length(Vec2{S(182.0f), Scalar{}}),
                        S(182.0f)));
  // Two moderate components whose sum of squares overflows.
  EXPECT_EQ(Sim::length(Vec2{S(150.0f), S(150.0f)}),
            Fpx::sqrt(Scalar::max()));
  // Full-range component.
  EXPECT_EQ(Sim::length(Vec2{Scalar::max(), Scalar{}}),
            Fpx::sqrt(Scalar::max()));
}

// ---------------------------------------------------------------------------
// FixedPointDispatch — compile-time dispatch mechanics (ADR 0002,
// PERF-006)
// ---------------------------------------------------------------------------

TEST(FixedPointDispatch, StatelessCompileTimeDispatch) {
  // One template instantiation per backend, no per-call indirection: the
  // type is stateless and trivially copyable, so SimMath ops inline to
  // the backend's primitive operations.
  static_assert(std::is_trivially_copyable_v<Scalar>);
  static_assert(sizeof(Scalar) == 4);
  static_assert(std::is_trivially_copyable_v<Sim>);
  static_assert(sizeof(Sim) == 1);
  static_assert(std::is_trivially_copyable_v<Vec2>);
  static_assert(sizeof(Vec2) == 8);
  static_assert(std::is_trivially_copyable_v<Vec3>);
  static_assert(sizeof(Vec3) == 12);

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
}

TEST(FixedPointDispatch, Fpx16BackendContract) {
  // The backend contract (ADR 0002): one correctly-rounded operation per
  // primitive, deterministic by the language standard; no NaN/Inf.
  using B = laige::sim::Fpx16_16;
  static_assert(std::is_same_v<B::Scalar, Scalar>);
  static_assert(std::is_trivially_copyable_v<B>);
  EXPECT_EQ(B::add(S(1.0f), S(1.0f)), S(2.0f));
  EXPECT_EQ(B::sub(S(3.0f), S(1.0f)), S(2.0f));
  EXPECT_EQ(B::mul(S(2.0f), S(3.0f)), S(6.0f));
  EXPECT_EQ(B::div(S(8.0f), S(2.0f)), S(4.0f));
  EXPECT_EQ(B::sqrt(S(4.0f)), S(2.0f));
  EXPECT_EQ(B::sqrt(S(0.0f)), Scalar{});
  EXPECT_EQ(B::sqrt(S(-1.0f)), Scalar{});  // defined: negative → +0
  EXPECT_FALSE(B::isNaN(S(1.0f)));
  EXPECT_FALSE(B::isInf(Scalar::max()));
}

// ---------------------------------------------------------------------------
// FixedPointDeterminism — property test: same op sequence, two
// implementations, one hash (ARCH-010, TEST-004)
// ---------------------------------------------------------------------------

TEST(FixedPointDeterminism, EngineAndReferenceImplementationsAgree) {
  const FxState a = engineRun();
  const FxState b = refRun();
  EXPECT_EQ(a.px, b.px);
  EXPECT_EQ(a.py, b.py);
  EXPECT_EQ(a.vx, b.vx);
  EXPECT_EQ(a.vy, b.vy);
}

TEST(FixedPointDeterminism, OperationSequenceKnownAnswerHash) {
  const FxState s = engineRun();
  const std::uint64_t h = stateHash(s);
  // Known-answer constant: the hash of the fixed 4096-tick op sequence
  // above. Verified identical on g++ 16.2.1 and clang++ 22.1.8 (local
  // two-compiler run; the CI hookup lands in M1-DET-04). If this constant
  // changes, either the op sequence or the documented fpx16_16 policy has
  // changed — both are replay identity (ADR 0002); investigate before
  // updating.
  constexpr std::uint64_t kSequenceHash = 0xF02728762777C581ull;  // KAT
  EXPECT_EQ(h, kSequenceHash) << "hash=0x" << std::hex << h;
}
