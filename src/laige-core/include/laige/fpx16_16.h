// laige-core — fpx16_16: signed Q16.16 fixed-point scalar (M0-CORE-04).
//
// ADR 0002 (Deterministic math strategy): the DEFAULT deterministic
// backend, required for lockstep (AC-10.3) and authoritative MMO. The
// SimMath op surface over this type is `SimMathFpx16`
// (laige/sim_math.h); sim code uses the SimMath ops, which delegate to
// the static ops here (PRD §10.3).
//
// ---------------------------------------------------------------------------
// Storage and range
// ---------------------------------------------------------------------------
// `std::int32_t raw`: value = raw / 2^16 (16 integer bits, 16 fraction
// bits).
//   Range:      [-32768.0, 32767.99998474] = [-2^16, 2^16 - 2^-16]
//   Resolution: 2^-16 ≈ 1.5259e-5 units (~65k sub-tile steps per unit)
//
// ---------------------------------------------------------------------------
// Determinism scope (ARCH-010)
// ---------------------------------------------------------------------------
// Bit-exact across ALL builds, platforms, ISAs, and compilers —
// guaranteed by the C++20 language standard (two's complement is
// mandated; every integer operation used here is defined). Unlike
// `fp32_pinned`, no pinned compiler flags are required: there is no
// fused integer operation for a compiler to discover, and no rounding
// mode to change.
//
// ---------------------------------------------------------------------------
// Overflow policy (defined for every input — CPP-004, no UB)
// ---------------------------------------------------------------------------
// All arithmetic computes in `std::int64_t` and SATURATES to the type's
// range. No op can overflow:
//   add(max, max) = max, sub(min, max) = min, mul(min, min) = max,
//   div(max, 1 ulp) = max, negate(min) = max, and so on.
//
// ---------------------------------------------------------------------------
// Rounding policy (mul, div, toInt32, fromFloat, sqrt)
// ---------------------------------------------------------------------------
// Round-to-nearest, TIES-TO-EVEN — the fixed-point analogue of IEEE
// round-to-nearest-even, so both SimMath backends share one documented
// rounding convention. Exact ties are:
//   mul:       product whose low 16 bits are exactly 0x8000;
//   div:       remainder of exactly half the divisor magnitude;
//   toInt32:   value exactly k + 0.5 for an integer k;
//   fromFloat: v * 2^16 exactly k + 0.5 (reachable only while |v| < 2^23,
//              where the float ulp ≤ 0.5);
//   sqrt:      NO exact ties exist for the square root of an integer
//              (n - s² can never equal s + 0.25 for integer n), so
//              round-to-nearest and ties-to-even coincide there.
//
// ---------------------------------------------------------------------------
// Division by zero (defined; never traps on a P0 target)
// ---------------------------------------------------------------------------
//   x / 0 = ±max() with the sign of x;  0 / 0 = +0.
// The saturation analogue of IEEE x/0 = ±inf (the type has no NaN/Inf).
//
// ---------------------------------------------------------------------------
// Comparisons and classification
// ---------------------------------------------------------------------------
// Total order — every pair of values is comparable, `equals` is exact
// bit equality, `isNaN`/`isInf` are always false, `isFinite` always
// true (the SimMath backend delegates these).
//
// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------
//   fromInt32(v): exact for |v| ≤ 32768 (v = -32768 maps exactly to
//                 min()); saturates outside the Q16.16 range (defined).
//   toInt32(x):   round-to-nearest, ties-to-even (0.5 → 0, 1.5 → 2,
//                 -0.5 → 0, -1.5 → -2). Result in [-32768, 32768].
//   toFloat(x):   ONE rounding: raw → nearest binary32, then the exact
//                 power-of-two scale 2^-16.
//   fromFloat(v): NaN → +0 (defined); ±inf and |v| ≥ 32768 saturate;
//                 otherwise one rounding of the exact scale v * 2^16,
//                 ties-to-even (the pinned default rounding mode,
//                 ADR 0002 — no `fesetround` is ever called).
//
// ---------------------------------------------------------------------------
// API safety (CORE-008, PRD §10.3)
// ---------------------------------------------------------------------------
// The type deliberately has NO implicit constructor from integer or
// float scalars and NO arithmetic operators (no operator+, operator*,
// …). Silent unit confusion (`5` vs `5 / 2^16`) and bypassing the
// documented rounding/saturation policy are exactly what the engine
// must prevent: construct via `fromInt32`/`fromFloat`, compute through
// the static ops (or SimMath). Only comparison operators are provided —
// they are exact and required by the SimMath op surface.

#pragma once

#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>

namespace laige {

class fpx16_16 {
 public:
  // Q16.16 raw units: value = raw / 2^16. Public for inspection and
  // serialization; treat as opaque outside the helpers below.
  std::int32_t raw{};

  // --- Named values (CORE-005) ------------------------------------------
  // -2^16 = -32768.0 and +2^16 - 2^-16 = +32767.99998474.
  static constexpr std::int32_t kMinRaw =
      std::numeric_limits<std::int32_t>::min();
  static constexpr std::int32_t kMaxRaw =
      std::numeric_limits<std::int32_t>::max();

  static constexpr fpx16_16 min() noexcept { return fpx16_16{kMinRaw}; }
  static constexpr fpx16_16 max() noexcept { return fpx16_16{kMaxRaw}; }
  static constexpr fpx16_16 one() noexcept { return fpx16_16{1 << 16}; }

  // Total order (no NaN: every pair of Q16.16 values is comparable).
  constexpr auto operator<=>(const fpx16_16&) const noexcept = default;

  // --- Arithmetic (saturating; policy in the header preamble) ----------

  // a + b, saturating. The exact sum |a.raw + b.raw| ≤ 2^32 fits
  // int64_t (no overflow — CPP-004).
  static constexpr fpx16_16 add(fpx16_16 a, fpx16_16 b) noexcept {
    return fpx16_16{clampRaw(static_cast<std::int64_t>(a.raw) + b.raw)};
  }

  // a - b, saturating. The exact difference fits int64_t.
  static constexpr fpx16_16 sub(fpx16_16 a, fpx16_16 b) noexcept {
    return fpx16_16{clampRaw(static_cast<std::int64_t>(a.raw) - b.raw)};
  }

  // a * b, round-to-nearest ties-to-even, saturating. The exact product
  // |a.raw * b.raw| ≤ 2^62 fits int64_t.
  static constexpr fpx16_16 mul(fpx16_16 a, fpx16_16 b) noexcept {
    return fpx16_16{roundToQ16(static_cast<std::int64_t>(a.raw) * b.raw)};
  }

  // a / b, round-to-nearest ties-to-even, saturating. Division by zero
  // is defined (never traps): x/0 → ±max() (sign of x), 0/0 → +0.
  static fpx16_16 div(fpx16_16 a, fpx16_16 b) noexcept {
    const std::int64_t num = static_cast<std::int64_t>(a.raw) << 16;
    const std::int64_t den = b.raw;
    if (den == 0) {
      if (a.raw == 0) return fpx16_16{};
      return a.raw < 0 ? fpx16_16::min() : fpx16_16::max();
    }
    std::int64_t q = num / den;  // truncate toward zero
    const std::int64_t r = num % den;  // sign of num, |r| < |den|
    const std::int64_t ad = den < 0 ? -den : den;
    const std::int64_t ar = r < 0 ? -r : r;
    // Round to nearest; on a tie (2|r| == |den|) keep q when it is even,
    // otherwise step away from zero (the even neighbor). 2*ar < 2^32 and
    // |q| ≤ 2^47: everything fits int64_t.
    if (2 * ar > ad || (2 * ar == ad && (q & 1))) q += (q >= 0) ? 1 : -1;
    return fpx16_16{clampRaw(q)};
  }

  // -a, saturating: negate(min()) == max() because -(-2^16) is not
  // representable. Equivalent to SimMath::sub(Scalar{}, a).
  static constexpr fpx16_16 negate(fpx16_16 a) noexcept {
    return sub(fpx16_16{}, a);
  }

  // sqrt(a), round-to-nearest (no exact ties exist — see the header
  // preamble), saturating. sqrt of a negative value is defined as +0
  // (the fixed-point analogue of the IEEE domain error, with no NaN).
  static fpx16_16 sqrt(fpx16_16 a) noexcept {
    const std::int32_t v = a.raw;
    if (v <= 0) return fpx16_16{};
    // sqrt(v / 2^16) in Q16.16 units = round(sqrt(v) * 2^8)
    //                            = round(sqrt(v * 2^16)).
    const std::uint64_t n = static_cast<std::uint64_t>(v) << 16;  // < 2^47
    const std::uint64_t s = isqrtFloor(n);
    // sqrt(n) ≥ s + 0.5  ⟺  n ≥ s² + s + 0.25  ⟺  (n integer)  n - s² > s.
    const std::uint64_t r = s + (n - s * s > s ? 1 : 0);
    return fpx16_16{static_cast<std::int32_t>(r)};  // r < 2^24: in range
  }

  // --- Conversions -------------------------------------------------------

  // int32_t → Q16.16, exact for |v| ≤ 32768 (v = -32768 maps exactly to
  // min()); saturates outside the Q16.16 range (defined — no UB for any
  // input, CPP-004).
  static constexpr fpx16_16 fromInt32(std::int32_t v) noexcept {
    if (v >= 32768) return max();
    if (v <= -32768) return min();
    return fpx16_16{static_cast<std::int32_t>(
        static_cast<std::int64_t>(v) << 16)};
  }

  // Q16.16 → int32_t, round-to-nearest ties-to-even. The result lies in
  // [-32768, 32768] and always fits int32_t.
  static constexpr std::int32_t toInt32(fpx16_16 x) noexcept {
    return static_cast<std::int32_t>(roundHalfToEven(x.raw));
  }

  // Q16.16 → float: one rounding (raw → nearest binary32), then the
  // exact power-of-two scale 2^-16 (exact exponent adjustment).
  static float toFloat(fpx16_16 x) noexcept {
    return static_cast<float>(x.raw) / 65536.0f;
  }

  // float → Q16.16: NaN → +0 (defined); ±inf and |v| ≥ 32768 saturate;
  // otherwise one rounding of the exact scale v * 2^16, ties-to-even.
  static fpx16_16 fromFloat(float v) noexcept {
    if (std::isnan(v)) return fpx16_16{};
    if (v >= 32768.0f) return max();
    if (v <= -32768.0f) return min();
    // |v| < 32768: v * 65536.0f is exact (power-of-two scaling) and lies
    // in (-2^31, 2^31); the largest float below 2^31 is 2^31 - 128, so
    // the rounded result always fits the Q16.16 range (no clamp needed).
    const float x = v * 65536.0f;
    const std::int64_t q = static_cast<std::int64_t>(std::nearbyint(x));
    return fpx16_16{static_cast<std::int32_t>(q)};
  }

 private:
  // Clamp a Q16.16 raw quantity to the type's range (saturation).
  static constexpr std::int32_t clampRaw(std::int64_t v) noexcept {
    if (v < kMinRaw) return kMinRaw;
    if (v > kMaxRaw) return kMaxRaw;
    return static_cast<std::int32_t>(v);
  }

  // Round p / 2^16 to the nearest integer, ties-to-even. `p` is a signed
  // "32.32" fixed-point quantity; the result is NOT clamped — callers
  // clamp. (p >> 16 is the arithmetic floor for two's complement; the
  // remainder is p - q·2^16 in [0, 2^16).)
  static constexpr std::int64_t roundHalfToEven(std::int64_t p) noexcept {
    const std::int64_t q = p >> 16;
    const std::int64_t rem = p & 0xFFFF;
    return q + (rem >= 0x8000 && (rem > 0x8000 || (q & 1)) ? 1 : 0);
  }

  // roundHalfToEven followed by saturation — the single rounding step of
  // mul.
  static constexpr std::int32_t roundToQ16(std::int64_t p) noexcept {
    return clampRaw(roundHalfToEven(p));
  }

  // floor(sqrt(n)) for n < 2^47. The binary64 estimate is exact for n
  // (n fits in 53 bits) and correctly rounded (IEC 60559), so it is
  // off by at most 1; the integer correction loops terminate. (s+1)² ≤
  // 2^48: no overflow in the loop conditions.
  static std::uint64_t isqrtFloor(std::uint64_t n) noexcept {
    std::uint64_t s =
        static_cast<std::uint64_t>(std::sqrt(static_cast<double>(n)));
    while ((s + 1) * (s + 1) <= n) ++s;
    while (s * s > n) --s;
    return s;
  }
};

}  // namespace laige
