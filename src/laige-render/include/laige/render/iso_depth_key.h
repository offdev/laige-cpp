// laige-render isometric depth key (M2-ISO-01): the engine-owned
// 32-bit sortable depth key for isometric render ordering.
//
// PRD §4: "the render depth key is computed from the axis-aligned world,
// not from screen space" (presentation-only, ARCH-009). FR-2.2:
// "deterministic, stable z-order from (layer, depth, entity id); in
// isometric mode the depth key = ground (x + y) contribution +
// tile/step height". G-R11: isometric depth is engine-owned — game code
// never writes its own z-ordering math. RENDER-003: the order is
// deterministic and the tie-break is explicit and stable. ADR 0005: the
// key is preset-independent (2:1 dimetric, true 30°/60°, custom shear).
//
//   IsoDepthKeyParts      The unpacked key (layer, quantized depth)
//   isoDepthKey()         The 32-bit key from (x, y, step height, layer)
//   isoDepthKeyParts()    The exact inverse of isoDepthKey()
//   isoDepthOrderLess()   The explicit stable total render order:
//                         (key, entity id)
//   isoShearSupported()   Whether an iso shear is depth-key-supported
//
// ---------------------------------------------------------------------------
// The formula (the canonical home is docs/concepts/coordinates.md,
// ARCH-008; this header carries the machine-checked contract)
// ---------------------------------------------------------------------------
//
// Inputs — world (sim) coordinates only, never screen space (PRD §4):
//
//   (x, y)   the object's ground-plane position in world units (the sim
//            Vec2 of the selected SimMath backend — PRD §10.3; the
//            presentation snapshot's interpolated position is the
//            intended source, M1-LOOP-02)
//   z        the tile/step height the object STANDS ON: the elevation of
//            its standing surface, an integer number of world units
//            (the tile map's per-tile height, M2-TILE-01 — not the
//            object's own sprite height)
//   layer    the render layer, an integer. kIsoDepthGroundLayer (0) is
//            the default ground layer; M2-PAR-01 documents the parallax
//            layer values (background layers sort before the ground,
//            foreground layers after it).
//
// The painter's-order value (world space, one scalar per object):
//
//   v(x, y, z) = (x + y) - z
//
// Under every depth-key-supported shear (below) an object's base
// projects to NDC_y = -A * v with A > 0 (NDC y is UP, matrices.h
// conventions), so the back-to-front painter's order is ascending v:
// smaller v = higher on screen = further from the camera = drawn first.
// The object's own sprite height does not enter v: a tall sprite is
// anchored by its BASE (its standing surface), which is exactly what
// painter's ordering needs.
//
// Quantization (one rounding — monotone, so the key order NEVER inverts
// the true v order):
//
//   q = round((x + y) * kIsoDepthQuantScale)     [1/16 world units]
//     round = nearest, ties AWAY FROM ZERO; (x + y) is one SimMath add
//     of the backend (fpx16_16: saturating, exact int64 rounding;
//     fp32_pinned: one exact double product + one llround — ADR 0002)
//   d = q - z * kIsoDepthQuantScale               [1/16 world units]
//
// 32-bit layout (UNSIGNED order = lexicographic (layer, d) =
// back-to-front):
//
//   key = (l + 512) << 22  |  (d + 2^21)
//
//   bits 31..22  biased layer l, 10 bits, -512..+511
//   bits  21..0  biased quantized depth d, 22 bits, -2^21..+2^21-1
//
// Consequences (all machine-checked by the tests/laige-render suite,
// CTest entry `iso_depth_key`):
//
//   - keyA < keyB  =>  vA <= vB exactly (ties-away rounding is
//     monotone): an earlier-drawn object is never in front of a
//     later-drawn one in the painter's order of the same layer.
//   - keyA == keyB =>  |vA - vB| <= 1/16 world units: the two bases sit
//     on (nearly) the same screen row; their relative order is the
//     deterministic insertion order (the tie-break below), and either
//     order is visually degenerate (same screen row, differing only in
//     screen x).
//   - layer dominates: every layer-(-1) object sorts before every
//     layer-0 object, whatever its v (background first — M2-PAR-01).
//
// ---------------------------------------------------------------------------
// The tie-break (RENDER-003, RENDER-003's explicit stable order)
// ---------------------------------------------------------------------------
//
// The total render order is lexicographic (key, entity id):
//
//   - M2-SORT-01's stable radix sort preserves insertion order for
//     equal keys;
//   - the batcher (M2-SPRITE-01) inserts sprites in the engine's
//     deterministic entity-id iteration order (FR-1.2),
//
// so equal keys resolve by entity id — isoDepthOrderLess() is that
// comparison. The roadmap/FR-2.2 tie tuple "(layer, depth, entity id)"
// is realized by packing layer and depth (step height) INTO the key,
// with the entity id as the final stable tie-break.
//
// ---------------------------------------------------------------------------
// Supported iso shears (the back-to-front contract)
// ---------------------------------------------------------------------------
//
// A shear axes (matrices.h IsoAxes) is depth-key-supported iff:
//
//   all components finite, det(dx, dy) != 0 (invertible ground map),
//   and  -dx.y == -dy.y == zUnit > 0        (exact float equality)
//
// i.e. both ground axes project DOWNWARD with the SAME slope A and the
// height unit equals A (in y-down pixels: screen_y = A*(x + y) - A*z).
// Both built-in presets satisfy it exactly (2:1 dimetric: A = scale;
// true 30°/60°: A = scale/√3); a custom shear must satisfy it to be
// depth-key-supported — isoShearSupported() is the checker, and
// M2-CAM-02 validates scene shears against it. An invertible shear that
// violates it still renders (isoMatrix is total over invertible
// shears) but its depth-key order is NOT guaranteed: that scene should
// not use isometric depth sorting.
//
// ---------------------------------------------------------------------------
// Domain, saturation, failure behavior (CORE-005, CORE-008)
// ---------------------------------------------------------------------------
//
// Documented domain (named constants below):
//
//   |x|, |y| <= kIsoDepthMaxWorldUnits      (32767 world units — the
//                                            Q16.16 fixed-point world
//                                            bound; both backends'
//                                            coordinate range)
//   |z|     <= kIsoDepthMaxStepHeight       (2047 world units)
//   |layer| <= kIsoDepthLayerMax            (511)
//
// Exactness zone: the key's (x + y) contribution is exact on BOTH
// backends for |x + y| <= 32767 — the fpx16_16 storage bound
// (+/-(2^16 - 2^-16)) is the tighter one (the fp32_pinned backend is
// exact to |x + y| <= 65534 = kIsoDepthMaxWorldSum; the 32767 bound
// keeps the backends' keys mutually consistent by design, and no scene
// anywhere near it — 32767 world units ~ 32 km at 1 m/tile).
//
// The function is TOTAL (it is a per-sprite hot-path call — no
// per-call asserts, PERF-006; misconfiguration is bounded, never UB):
//
//   - non-finite fp32 input saturates to the domain bound (+inf -> +,
//     -inf -> -, NaN -> the LOWER bound — IEEE NaN compares false);
//   - beyond the exactness zone each backend saturates in its own
//     documented way (fpx16_16: the backend's saturating add;
//     fp32_pinned: the clamp above) and out-of-range layer/depth
//     saturate at the packing boundary. The saturations are MONOTONE,
//     so the key order is never inverted anywhere — it only coarsens
//     at the bound (distinct extreme sums collapse to the bound's
//     key; the tie-break applies).
//
// In the exactness zone the key is exact for both backends (and equal
// across backends for exactly representable inputs — the tests pin
// it).
//
// ---------------------------------------------------------------------------
// Determinism, threading, performance (PERF-002/003/006, ADR 0002)
// ---------------------------------------------------------------------------
//
// Pure function of the backend's sim state: deterministic per the
// backend's ADR 0002 scope (fpx16_16 bit-exact on every platform;
// fp32_pinned bit-exact per same build/ISA). Presentation-only
// (ARCH-009): never part of replay state or the sim state hash — keys
// across backends are NOT promised equal (they agree for exactly
// representable inputs, e.g. the grid-locked 1/16 lattice, which the
// tests pin).
//
// O(1): a few integer ops + one rounding; no allocation, no logging,
// no locks, no GL calls. Callable from any thread at any phase (pure —
// the M2-GL-03 matrix-construction pattern). Called once per sprite
// per frame (the M2-SPRITE-02 batch path): budgeted as trivial
// (PERF-002).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - Do not derive the key from screen-space coordinates or camera
//     state (PRD §4, FR-2.2): the key is world-space by contract — a
//     screen-space z-order breaks under zoom, custom shears, and
//     camera motion, and is exactly what this API exists to prevent
//     (S-5, G-R11).
//   - Do not hand-roll per-sprite z-ordering in game code (G-R11):
//     the per-sprite depth override lands with M2-SPRITE-01 as a
//     counted + warned escape hatch ("prefer tile height").
//   - z is the object's STANDING SURFACE elevation (the tile height),
//     not the object's height — passing the sprite's top elevation
//     pushes it behind its own base's row.
 
#pragma once

#include <cmath>
#include <cstdint>

#include "laige/render/matrices.h"  // IsoAxes (the shear-support checker)
#include "laige/sim_math.h"         // the SimMath backends (core)

namespace laige::render {

// ---------------------------------------------------------------------------
// Named quantization constants (CORE-005)
// ---------------------------------------------------------------------------

// Quantization scale: 16 key units per world unit of v = x + y - z.
// The tie window of equal keys is 1/16 world units (the "documented
// precision" of the key order, docs/concepts/coordinates.md).
inline constexpr std::int32_t kIsoDepthQuantScale = 16;

// Bits of the quantized-depth field (biased) in the 32-bit key.
inline constexpr std::int32_t kIsoDepthFineBits = 22;
// Bits of the layer field (biased): 32 - fine bits (exactly 32).
inline constexpr std::int32_t kIsoDepthLayerBits = 32 - kIsoDepthFineBits;

// Field biases (the bias makes each field unsigned within the key: a
// 22-bit field holding -2^21..+2^21-1 is biased by 2^21; a 10-bit field
// holding -512..+511 by 512 — CORE-005: the bit widths above are the
// only "magic" numbers — every other constant derives from them).
inline constexpr std::uint32_t kIsoDepthFineBias =
    1u << (kIsoDepthFineBits - 1);
inline constexpr std::uint32_t kIsoDepthLayerBias =
    1u << (kIsoDepthLayerBits - 1);

// The quantized-depth field's unsigned mask (bits 0..21).
inline constexpr std::uint32_t kIsoDepthFineMask =
    (1u << kIsoDepthFineBits) - 1u;

// Documented domain (the "world units" of PRD §4; 1 unit = 1 tile in the
// tile map — M2-TILE-01). kIsoDepthMaxWorldUnits is the Q16.16
// fixed-point coordinate bound (fpx16_16.h: +/-32767.996), so the domain
// is the same on both SimMath backends (ADR 0002).
inline constexpr std::int32_t kIsoDepthMaxWorldUnits = 32767;
// The fp32_pinned quantizer's saturation bound: |x + y| <= 2 * the
// per-coordinate bound. (The fpx16_16 backend's saturating add is
// tighter — its storage bound is +/- (2^16 - 2^-16); see the
// "Exactness zone" note above for the cross-backend statement.)
inline constexpr std::int32_t kIsoDepthMaxWorldSum = 2 * kIsoDepthMaxWorldUnits;
// Step heights (tile elevations) in world units.
inline constexpr std::int32_t kIsoDepthMaxStepHeight = 2047;
// Render layers (the parallax step documents the values; background
// layers are negative, foreground positive — M2-PAR-01).
inline constexpr std::int32_t kIsoDepthLayerMax =
    static_cast<std::int32_t>(kIsoDepthLayerBias) - 1;

// The default ground layer (k = 0: no layer offset — the plain isometric
// scene of the reference scene M2-SCENE-01 and the template
// M2-SAMPLE-01).
inline constexpr std::int32_t kIsoDepthGroundLayer = 0;

// ---------------------------------------------------------------------------
// The unpacked key (the exact inverse of isoDepthKey(); the debug/
// diagnostics view of a key — DBG-007/008)
// ---------------------------------------------------------------------------

struct IsoDepthKeyParts {
  // The (unbiased) layer field: -512..+511.
  std::int32_t layer{};
  // The quantized depth d = round((x + y) * scale) - z * scale, in
  // 1/16-world-unit key units: -2^21..+2^21-1.
  std::int32_t quantizedDepth{};
};

// The exact inverse of isoDepthKey(): key -> (layer, quantized depth).
// O(1); no allocation.
[[nodiscard]] inline IsoDepthKeyParts isoDepthKeyParts(std::uint32_t key)
    noexcept {
  return IsoDepthKeyParts{
      static_cast<std::int32_t>(key >> kIsoDepthFineBits) -
          static_cast<std::int32_t>(kIsoDepthLayerBias),
      static_cast<std::int32_t>(key & kIsoDepthFineMask) -
          static_cast<std::int32_t>(kIsoDepthFineBias)};
}

// ---------------------------------------------------------------------------
// The explicit stable total render order (RENDER-003)
// ---------------------------------------------------------------------------

// The lexicographic (key, entity id) comparison — the total order the
// render pipeline sorts by. The key carries (layer, quantized depth);
// the entity id is the final stable tie-break (the stable sort of
// M2-SORT-01 + the batcher's deterministic entity-id insertion order,
// FR-1.2). Strict weak ordering (never true for (a, a)); O(1).
//
// @budget O(1); two integer comparisons.
[[nodiscard]] inline bool isoDepthOrderLess(std::uint32_t keyA,
                                            std::uint32_t entityA,
                                            std::uint32_t keyB,
                                            std::uint32_t entityB) noexcept {
  if (keyA != keyB) return keyA < keyB;
  return entityA < entityB;
}

namespace detail {

// round(n / 2^16), nearest, ties AWAY FROM ZERO — monotone (never
// decreases as n increases), exact integer arithmetic (bit-exact on
// every platform — ADR 0002). n = (x + y).raw * 16: |n| <= 2^35 fits
// int64_t (CPP-004).
constexpr std::int64_t roundHalfAway16(std::int64_t n) noexcept {
  constexpr std::int64_t kDenom = 1ll << 16;
  constexpr std::int64_t kHalf = 1ll << 15;
  if (n >= 0) return (n + kHalf) / kDenom;
  return -((-n + kHalf) / kDenom);
}

// Clamp (the packing-boundary saturation — the total-function contract
// above; no UB for any input, CPP-004).
constexpr std::int64_t clampInt64(std::int64_t v, std::int64_t lo,
                                  std::int64_t hi) noexcept {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// The per-backend quantization of the sum s = x + y into
// q = round(s * kIsoDepthQuantScale). The caller computes s with ONE
// SimMath add of the backend (the ADR 0002 arithmetic — fpx16_16:
// saturating; fp32_pinned: one IEEE addition).
template <typename Backend>
struct IsoDepthSumQuant {
  using Scalar = typename sim::SimMath<Backend>::Scalar;
  static std::int64_t quantize(Scalar s) noexcept;
};

template <>
struct IsoDepthSumQuant<sim::Fp32Pinned> {
  // Total on every float (the header's "Domain, saturation" contract):
  // non-finite -> the domain bound (+inf -> +, -inf and NaN -> -; IEEE
  // NaN compares false in `s > 0.0f`); out-of-domain finite -> the
  // domain bound. In-range: the float -> double cast is exact and the
  // x 16 scale is an exact power of two, so llround performs ONE
  // rounding of the exact value 16*s — nearest, ties away from zero,
  // the same rule as the fixed-point path (monotone).
  static std::int64_t quantize(float s) noexcept {
    constexpr float kBound = static_cast<float>(kIsoDepthMaxWorldSum);
    if (!std::isfinite(s) || s > kBound || s < -kBound) {
      s = (s > 0.0f) ? kBound : -kBound;  // NaN -> -kBound (IEEE)
    }
    return std::llround(static_cast<double>(s) *
                        static_cast<double>(kIsoDepthQuantScale));
  }
};

template <>
struct IsoDepthSumQuant<sim::Fpx16_16> {
  // s is a saturating Q16.16 add: always finite, |s| <= 32767.996 —
  // well inside the domain's sum bound. n = s.raw * 16 is exact in
  // int64_t; the rounding is exact integer arithmetic (bit-exact on
  // every platform, ADR 0002).
  static std::int64_t quantize(fpx16_16 s) noexcept {
    const std::int64_t n =
        static_cast<std::int64_t>(s.raw) * kIsoDepthQuantScale;
    return roundHalfAway16(n);
  }
};

}  // namespace detail

// ---------------------------------------------------------------------------
// The 32-bit isometric depth key (M2-ISO-01)
// ---------------------------------------------------------------------------

// The deterministic 32-bit sortable depth key of an isometric object
// (the formula, bit layout, domain, and shear contract: the header
// preamble; the canonical narrative: docs/concepts/coordinates.md).
//
//   pos         the object's world ground-plane position (x, y) in
//               world units — the sim Vec2 of the selected SimMath
//               backend (the presentation snapshot's interpolated
//               position, M1-LOOP-02; presentation-only, ARCH-009)
//   stepHeight  the tile/step height the object STANDS ON, world units
//               (integer; the standing surface's elevation, not the
//               object's own height)
//   layer       the render layer (kIsoDepthGroundLayer = 0 default;
//               M2-PAR-01 documents the parallax values)
//
// Returns the key: unsigned order (keyA < keyB) = back-to-front
// painter's order of the same layer, with layer ascending as the coarse
// primary order (background first). Equal keys: the stable insertion
// order (the (key, entity id) total order — isoDepthOrderLess).
//
// @budget O(1): one backend add + one rounding + a few integer ops; no
// allocation, no logging, no GL. Pure — callable from any thread at
// any phase. Deterministic per the backend's ADR 0002 scope.
template <typename Backend>
[[nodiscard]] inline std::uint32_t isoDepthKey(
    typename sim::SimMath<Backend>::Vec2 pos, std::int32_t stepHeight,
    std::int32_t layer) noexcept {
  using M = sim::SimMath<Backend>;
  // One backend add (fpx16_16: saturating; fp32_pinned: one IEEE add).
  const auto s = M::add(pos.x, pos.y);
  const std::int64_t q = detail::IsoDepthSumQuant<Backend>::quantize(s);
  // d = q - z*scale, clamped to the quantized-depth field (the
  // packing-boundary saturation of the total-function contract).
  const std::int64_t d = detail::clampInt64(
      q - static_cast<std::int64_t>(stepHeight) * kIsoDepthQuantScale,
      -static_cast<std::int64_t>(kIsoDepthFineBias),
      static_cast<std::int64_t>(kIsoDepthFineBias) - 1);
  // The layer field, clamped to +/-kIsoDepthLayerMax (saturation).
  const std::int32_t l = static_cast<std::int32_t>(
      detail::clampInt64(static_cast<std::int64_t>(layer),
                         -static_cast<std::int64_t>(kIsoDepthLayerBias),
                         static_cast<std::int64_t>(kIsoDepthLayerBias) - 1));
  // (layer, d) packed as unsigned lexicographic order.
  return (static_cast<std::uint32_t>(static_cast<std::uint32_t>(l) +
                                     kIsoDepthLayerBias)
          << kIsoDepthFineBits) |
         static_cast<std::uint32_t>(static_cast<std::uint64_t>(d) +
                                    kIsoDepthFineBias);
}

// ---------------------------------------------------------------------------
// The supported-iso-shear checker (the back-to-front contract)
// ---------------------------------------------------------------------------

// True iff `axes` is a depth-key-supported isometric shear: all
// components finite, the ground map invertible (det != 0), and
// -dx.y == -dy.y == zUnit > 0 (EXACT float equality — both ground axes
// project downward with the same slope A and the height unit equals A,
// so NDC_y = -A*(x + y - z): the header preamble's shear contract).
// Both built-in presets pass (2:1 dimetric, true 30°/60°); a custom
// shear must pass to use isometric depth sorting (M2-CAM-02 validates
// scene shears against this). O(1); no allocation.
[[nodiscard]] inline bool isoShearSupported(IsoAxes axes) noexcept {
  if (!std::isfinite(axes.dx.x) || !std::isfinite(axes.dx.y) ||
      !std::isfinite(axes.dy.x) || !std::isfinite(axes.dy.y) ||
      !std::isfinite(axes.zUnit)) {
    return false;
  }
  const float a = -axes.dx.y;  // the +x ground axis' downward slope
  if (a <= 0.0f) return false;        // both axes must project downward
  if (a != -axes.dy.y) return false;   // equal downward slopes (A == B)
  if (a != axes.zUnit) return false;   // height unit equals A (A == C)
  const float det = axes.dx.x * axes.dy.y - axes.dx.y * axes.dy.x;
  return det != 0.0f;  // invertible ground map (the isoMatrix precondition)
}

}  // namespace laige::render
