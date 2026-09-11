// laige-core SimMath — the single deterministic-math interface
// (M0-CORE-03; roadmap S-7 / G-R8).
//
// ADR 0002 (Deterministic math strategy): all deterministic simulation
// code uses exactly one math interface, SimMath, with two backends,
// selected once per engine/zone init from game config
// (`determinism.math`):
//
//   Backend        Config id             Storage                    Determinism scope (ARCH-010)
//   -------------  ------------------    -------------------------  ----------------------------------------
//   Fp32Pinned     "float_pinned_32"     IEEE binary32 (`float`)    Bit-exact across runs of the same
//   (this file)                                                 build on the same platform/ISA.
//                                                        Cross-ISA is NOT promised until the CI
//                                                        detcheck matrix proves it (M1-DET); a
//                                                        failing pair is declared unsupported
//                                                        for this backend.
//   Fpx16_16       "fixed_point_16_16"   Q16.16 in `int32_t`,       Bit-exact across all builds,
//   (M0-CORE-04)                     `int64_t` intermediates         platforms, ISAs, and compilers
//                                                        by the language standard. The default
//                                                        backend; required for lockstep and
//                                                        authoritative MMO.
//
// PRD §10.3: in deterministic paths, engine math ops are the only
// floating-point allowed — never platform intrinsics outside the
// engine. This step ships the API, the `fp32_pinned` backend, and the
// pinned flag set; the *enforcement* of "SimMath only" in sim code
// lands in M1-DET-01 (G-R8).
//
// ---------------------------------------------------------------------------
// fp32_pinned: the pinned flag set (ADR 0002)
// ---------------------------------------------------------------------------
// The `fp32_pinned` promise is "bit-exact across runs of the same build
// on the same platform/ISA". It holds only if every compiler emits the
// same operation sequence for the pinned expressions below, which is
// defended by the pinned flag set — applied by
// `laige_apply_simmath_policy()` (root CMakeLists.txt) to every target
// that carries deterministic sim math (today: `laige-core` and its
// tests; from M1 on: every sim module, e.g. `laige-sim`):
//
//   GCC / Clang / AppleClang:
//     -ffp-contract=off      a*b+c is never fused into a single-rounding
//                            FMA. This is what keeps `lerp()` and
//                            `length()` at the documented two-rounding
//                            below: under -ffp-contract=fast, lerp(a,b,t)
//                            could round as FMA(a, b-a, t) and length()
//                            as FMA(x, x, y*y) — each a different bit
//                            result.
//     -fno-associative-math  Sums are never reassociated (already the
//                            default; passed explicitly so the pinned
//                            set is visible on every compile line).
//   MSVC 2022:
//     /fp:precise            The documented precision model; MSVC does
//                            not FMA-contract C expressions and never
//                            reassociates at this setting. Passed
//                            explicitly (it is the default) so the
//                            pinned set is visible on every compile
//                            line.
//
//   Banned in sim translation units, all compilers (re-audited at every
//   toolchain upgrade, ADR 0002):
//     - -ffast-math / -funsafe-math-optimizations / /fp:fast — they
//       enable reassociation, reciprocal math, and FMA contraction and
//       remove the NaN/Inf guarantees below.
//     - Floating-point intrinsics (`__builtin_*`, `_mm_*`, `_Float*`,
//       FPU intrinsics) outside this header.
//     - Rounding-mode changes (`fesetround`) and FP exception modes
//       (`FE_*`, `SetErrorMode`): the pinned mode is
//       round-to-nearest-even — the default on every P0 target — and no
//       op may trap on a P0 target.
//
// ---------------------------------------------------------------------------
// fp32_pinned: NaN/Inf policy — defined, not "whatever the CPU does"
// ---------------------------------------------------------------------------
// All ops are IEEE-754 binary32, round-to-nearest-even:
//
//   Arithmetic (`add`/`sub`/`mul`/`div`, `sqrt`): a NaN operand yields a
//     NaN result (quiet NaN propagates). The exceptional values are
//     IEEE-defined: 0/0 = NaN, x/0 = ±inf (no trap), inf - inf = NaN,
//     inf * 0 = NaN, x/inf = ±0.
//   Comparisons: `less`/`lessEqual`/`greater`/`greaterEqual`/`equals`
//     are *ordered* — false when either operand is NaN. `notEquals` is
//     true when either operand is NaN. `isOrdered(a,b)` is true iff
//     neither is NaN. `isNaN`, `isInf`, `isFinite` are the IEEE
//     classifications.
//   `clamp(x, lo, hi)`: requires `lo` and `hi` finite and `lo <= hi`
//     (debug builds assert; release builds treat a violation as
//     undefined behavior — the engine's Result/Status convention).
//     NaN in → NaN out; ±inf in → the corresponding bound.
//   `lerp(a, b, t)` is exactly a + (b - a) * t: one sub, one mul, one
//     add — two roundings, never fused (pinned -ffp-contract=off). It is
//     NOT interchangeable with a*(1-t) + b*t (different rounding;
//     replays diverge). t outside [0,1] extrapolates by the same
//     expression (defined behavior). NaN in any operand → NaN out.
//   `length(v)` is exactly sqrt(x*x + y*y): two muls, one add, one
//     correctly-rounded sqrt (SSE2 vsqrtss / NEON vsqrt), in that order;
//     the pinned flags keep x*x + y*y from FMA-contraction. `length` is
//     always >= 0; length((0,0)) = +0.
//   `normalize(v)` = v / length(v), component-wise IEEE division. The
//     zero vector is defined to normalize to the zero vector — SimMath
//     never injects NaN from a zero-length input (IEEE 0/0 would give
//     NaN). NaN/inf components propagate per IEEE (an infinite vector
//     can normalize to NaN components: inf/inf = NaN).
//   Signed zero follows IEEE: -0.0f is representable, -0.0f == +0.0f is
//     true, 0 + -0 = +0, and -1 * 0 = -0.
//
//   No op may signal an FP exception or trap on a P0 target
//   (documented policy, enforced by the pinned build).
//
// ---------------------------------------------------------------------------
// Performance (PERF-006, PERF-003)
// ---------------------------------------------------------------------------
// SimMath<Backend> is a stateless template: all ops are static and
// inline, one template instantiation per backend, zero per-call
// indirection (no virtual dispatch, no std::function), O(1), no
// allocation, noexcept. Vec2/Vec3 are trivially copyable value types
// (PERF-004) for dense sim state.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//   - Raw `float` operators in deterministic sim code bypass SimMath
//     and break the determinism contract (PRD §10.3). Enforcement:
//     M1-DET-01.
//   - The backend id is part of replay identity (ADR 0002): replays
//     across backends are not bit-exact and are not supported.
//   - The `fp32_pinned` backend is single-ISA by definition (ADR 0002);
//     it does not participate in the PRD §12.3 bandwidth degradation
//     ladder, which is defined on `fpx16_16`.

#pragma once

#include <cassert>
#include <cmath>
#include <limits>

namespace laige::sim {

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

// A backend is a stateless type providing the arithmetic primitives that
// the SimMath op surface is built from (ADR 0002: one op surface, two
// implementations). Contract:
//
//   - `Scalar`: the backend's storage/operation type.
//   - `add` / `sub` / `mul` / `div` / `sqrt`: the five pinned arithmetic
//     primitives. Each must be one correctly-rounded operation of the
//     backend's format (no fused or reassociated sequence),
//     deterministic by the scope documented in the backend.
//
// Adding a backend is an additive change (ADR 0002 review conditions);
// `Fpx16_16` lands in M0-CORE-04.

// IEEE-754 binary32 with pinned semantics (ADR 0002). See the header
// preamble for the pinned flag set and the full NaN/Inf policy.
struct Fp32Pinned {
  using Scalar = float;

  // a + b: one IEEE binary32 addition, round-to-nearest-even. The
  // build's -ffp-contract=off guarantees this does not fuse with an
  // adjacent multiply (e.g. inside lerp()).
  static Scalar add(Scalar a, Scalar b) noexcept { return a + b; }
  static Scalar sub(Scalar a, Scalar b) noexcept { return a - b; }
  static Scalar mul(Scalar a, Scalar b) noexcept { return a * b; }
  // IEEE binary32 division: x/0 = ±inf (no trap on P0 targets),
  // 0/0 = NaN.
  static Scalar div(Scalar a, Scalar b) noexcept { return a / b; }
  // Correctly-rounded binary32 square root (SSE2 vsqrtss / NEON vsqrt).
  // sqrt of a negative value is NaN (IEEE); no trap.
  static Scalar sqrt(Scalar a) noexcept { return std::sqrt(a); }
};

// ---------------------------------------------------------------------------
// The SimMath op surface (ADR 0002): one interface, one template
// instantiation per backend
// ---------------------------------------------------------------------------

template <typename Backend>
struct SimMath {
  using Scalar = typename Backend::Scalar;

  // 2D world/vector: positions, velocities, deltas. Default-initialized
  // to the zero vector (the additive identity; all components +0). The
  // coordinate system (axes, handedness, units) is defined in
  // docs/concepts/coordinates.md (M0-DOC-02).
  struct Vec2 {
    Scalar x{};
    Scalar y{};
  };

  // 2.5D world space: (x, y) is the ground plane, z is depth/height.
  struct Vec3 {
    Scalar x{};
    Scalar y{};
    Scalar z{};
  };

  // Factory (ADR 0002): backend selection is compile-time dispatch,
  // factory-selected once per engine/zone init. The returned handle is
  // stateless — holding one for a sim session costs nothing.
  [[nodiscard]] static SimMath create() noexcept { return SimMath{}; }

  // ------------------------------------------------------------------
  // Arithmetic (IEEE per the backend's policy; NaN/Inf per the header
  // preamble; division by zero does not trap)
  // ------------------------------------------------------------------
  [[nodiscard]] static Scalar add(Scalar a, Scalar b) noexcept {
    return Backend::add(a, b);
  }
  [[nodiscard]] static Scalar sub(Scalar a, Scalar b) noexcept {
    return Backend::sub(a, b);
  }
  [[nodiscard]] static Scalar mul(Scalar a, Scalar b) noexcept {
    return Backend::mul(a, b);
  }
  [[nodiscard]] static Scalar div(Scalar a, Scalar b) noexcept {
    return Backend::div(a, b);
  }

  // Component-wise vector arithmetic (same IEEE policy per component).
  [[nodiscard]] static Vec2 add(Vec2 a, Vec2 b) noexcept {
    return Vec2{add(a.x, b.x), add(a.y, b.y)};
  }
  [[nodiscard]] static Vec2 sub(Vec2 a, Vec2 b) noexcept {
    return Vec2{sub(a.x, b.x), sub(a.y, b.y)};
  }
  [[nodiscard]] static Vec2 mul(Vec2 a, Vec2 b) noexcept {
    return Vec2{mul(a.x, b.x), mul(a.y, b.y)};
  }
  [[nodiscard]] static Vec2 mul(Vec2 a, Scalar s) noexcept {
    return Vec2{mul(a.x, s), mul(a.y, s)};
  }
  [[nodiscard]] static Vec2 mul(Scalar s, Vec2 a) noexcept {
    return mul(a, s);
  }

  // ------------------------------------------------------------------
  // Length / normalize
  // ------------------------------------------------------------------
  // length is exactly sqrt(x*x + y*y): two muls, one add, one
  // correctly-rounded sqrt, in that order (pinned -ffp-contract=off
  // keeps x*x + y*y from FMA-contraction, which would round once and
  // change the result).
  [[nodiscard]] static Scalar length(Vec2 v) noexcept {
    return Backend::sqrt(
        Backend::add(Backend::mul(v.x, v.x), Backend::mul(v.y, v.y)));
  }
  [[nodiscard]] static Scalar length(Vec3 v) noexcept {
    return Backend::sqrt(Backend::add(
        Backend::add(Backend::mul(v.x, v.x), Backend::mul(v.y, v.y)),
        Backend::mul(v.z, v.z)));
  }

  // Unit vector: v / length(v), component-wise IEEE division. The zero
  // vector is defined to normalize to the zero vector (SimMath never
  // injects NaN from a zero-length input). NaN/inf components propagate
  // per IEEE (an infinite vector can normalize to NaN components).
  [[nodiscard]] static Vec2 normalize(Vec2 v) noexcept {
    const Scalar len = length(v);
    if (equals(len, Scalar{0.0})) return Vec2{};
    return Vec2{div(v.x, len), div(v.y, len)};
  }
  [[nodiscard]] static Vec3 normalize(Vec3 v) noexcept {
    const Scalar len = length(v);
    if (equals(len, Scalar{0.0})) return Vec3{};
    return Vec3{div(v.x, len), div(v.y, len), div(v.z, len)};
  }

  // ------------------------------------------------------------------
  // Interpolation / bounding
  // ------------------------------------------------------------------
  // lerp is exactly a + (b - a) * t (see the header preamble for the
  // rounding contract and the non-interchangeability with a*(1-t)+b*t).
  // t outside [0,1] extrapolates by the same expression (defined).
  [[nodiscard]] static Scalar lerp(Scalar a, Scalar b, Scalar t) noexcept {
    return Backend::add(a, Backend::mul(Backend::sub(b, a), t));
  }
  [[nodiscard]] static Vec2 lerp(Vec2 a, Vec2 b, Scalar t) noexcept {
    return Vec2{lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
  }

  // Bounding: requires `lo` and `hi` finite and `lo <= hi` (debug builds
  // assert; release builds treat a violation as undefined behavior — the
  // engine's Result/Status convention). NaN x → NaN; ±inf x → the
  // corresponding bound.
  [[nodiscard]] static Scalar clamp(Scalar x, Scalar lo, Scalar hi) noexcept {
    assert(isFinite(lo) && isFinite(hi) && lessEqual(lo, hi) &&
           "SimMath::clamp requires finite, ordered lo/hi (ADR 0002 policy)");
    if (greater(x, hi)) return hi;
    if (less(x, lo)) return lo;
    return x;
  }
  [[nodiscard]] static Vec2 clamp(Vec2 v, Vec2 lo, Vec2 hi) noexcept {
    return Vec2{clamp(v.x, lo.x, hi.x), clamp(v.y, lo.y, hi.y)};
  }

  // ------------------------------------------------------------------
  // Comparison (IEEE, ordered)
  // ------------------------------------------------------------------
  // NaN policy: less/lessEqual/greater/greaterEqual/equals are false
  // when either operand is NaN; notEquals is true. The eq/ne primitives
  // below are the only place in the engine where a raw floating-point
  // equality comparison appears — deliberate bit-level comparisons that
  // implement the documented policy — so the -Wfloat-equal heuristic (a
  // -Wall member, which does not understand the pinned NaN/Inf
  // contract) is scoped away there (CORE-010: no global suppression).
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
  static bool eq(Scalar a, Scalar b) noexcept { return a == b; }
  static bool ne(Scalar a, Scalar b) noexcept { return a != b; }
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

  static bool less(Scalar a, Scalar b) noexcept { return a < b; }
  static bool lessEqual(Scalar a, Scalar b) noexcept { return a <= b; }
  static bool greater(Scalar a, Scalar b) noexcept { return a > b; }
  static bool greaterEqual(Scalar a, Scalar b) noexcept { return a >= b; }
  static bool equals(Scalar a, Scalar b) noexcept { return eq(a, b); }
  static bool notEquals(Scalar a, Scalar b) noexcept { return ne(a, b); }
  // True iff neither operand is NaN (IEEE 754-2008 `totalOrder`
  // predicate, i.e. the negation of `isunordered` — not the negation of
  // the arithmetic != operator).
  static bool isOrdered(Scalar a, Scalar b) noexcept {
    return !isNaN(a) && !isNaN(b);
  }
  // IEEE classifications.
  static bool isNaN(Scalar x) noexcept { return ne(x, x); }
  static bool isInf(Scalar x) noexcept {
    const Scalar inf = std::numeric_limits<Scalar>::infinity();
    return eq(x, inf) || eq(x, -inf);
  }
  static bool isFinite(Scalar x) noexcept { return !isNaN(x) && !isInf(x); }

  // Vector equality (component-wise; treats ±0 as equal, per IEEE).
  static bool equals(Vec2 a, Vec2 b) noexcept {
    return equals(a.x, b.x) && equals(a.y, b.y);
  }
  static bool notEquals(Vec2 a, Vec2 b) noexcept { return !equals(a, b); }
  static bool equals(Vec3 a, Vec3 b) noexcept {
    return equals(a.x, b.x) && equals(a.y, b.y) && equals(a.z, b.z);
  }
  static bool notEquals(Vec3 a, Vec3 b) noexcept { return !equals(a, b); }
};

// The `fp32_pinned` SimMath (this step; `determinism.math` config id
// "float_pinned_32"). `SimMathFpx16_16` follows in M0-CORE-04.
using SimMathFp32 = SimMath<Fp32Pinned>;

}  // namespace laige::sim
