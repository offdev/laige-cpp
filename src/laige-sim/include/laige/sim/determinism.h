// laige-sim determinism mode (M1-DET-01).
//
// PRD §10.3 (determinism contract), §9.1 S-7 (deterministic by
// default), §9.3 G-R8 (determinism violation → compile error);
// ADR 0002 (deterministic math strategy); FR-1.4 (determinism mode);
// NFR-8.3 (cross-platform bit-identity verified in CI).
//
// This header carries the M1 determinism surface of laige-sim:
//
//   SimMathBackend          The two SimMath backends (ADR 0002) as a
//                           typed config value: FixedPoint16_16 (the
//                           config id "fixed_point_16_16", the
//                           default) and FloatPinned32 (the config id
//                           "float_pinned_32", opt-in).
//   DeterminismConfig       The typed determinism block of
//                           EngineConfig: enabled (default true —
//                           deterministic by default, S-7) and math
//                           (the selected backend).
//   detail::IsDeterminismSafe<T>
//                           The G-R8 trait: true when T's storage is
//                           bit-deterministic — every member
//                           (recursively) is an integer, an enum, a
//                           SimMath-registered scalar (fpx16_16,
//                           float), or a SimMath-registered vector
//                           (SimMath<B>::Vec2/Vec3). The primary
//                           template is FALSE: an unmarked type is
//                           unsafe (the compile-time error this step
//                           ships; the CI source scan below is the
//                           second, independent enforcement layer).
//   LAIGE_DETERMINISM_SAFE  The marker: declares that a user struct's
//                           members are exactly the listed member
//                           types; the trait then verifies each
//                           listed type.
//
// ---------------------------------------------------------------------------
// The trait mechanism (G-R8 / S-7: "using raw float/double inside
// deterministic systems is a compile-time/trait error")
// ---------------------------------------------------------------------------
//
// Where the check bites: World::registerSystem (entity.h) — every
// component a system declares I/O for must satisfy
// IsDeterminismSafe<T> (a static_assert with the actionable message).
// M1 systems are deterministic by default (S-7); M1 has no
// non-deterministic system registration path (the first one lands
// with the M2 work, through the documented U-1..U-4 unsafe tier if
// raw math is ever needed outside SimMath).
//
// What the trait checks (and deliberately does not):
//
//   - Storage: every member of the component's type is
//     determinism-safe storage. `double` is NEVER safe (no SimMath
//     backend uses it — ADR 0002); `float` IS safe, because it is
//     the fp32_pinned backend's Scalar — its USE must still go
//     through SimMath ops, which the CI source scan enforces (the
//     trait cannot see usage).
//   - Recursion: a nested struct is safe only when ITS type is
//     verified the same way — marked (or itself a registered
//     scalar/vector/integer/enum).
//   - Not usage: "only engine math ops" for the stored values is
//     enforced by the source scan plus the ADR 0002 pinned flag set
//     (the fp32_pinned backend's exact two-rounding semantics).
//
// Why the marker (not automatic member reflection): standard C++20
// has no aggregate-member enumeration, and a reflection hack would
// be a clever low-level trick without a demonstrated benefit
// (CPP-018). The marker is explicit, greppable, and safe by default:
// a forgotten mark is a compile error, and the CI source scan
// independently rejects a raw float/double token anywhere in a sim
// translation unit — including a misdeclared struct's `double m;`
// member — so the two layers cover each other's blind spots
// (documented in docs/concepts/determinism.md).
//
// ---------------------------------------------------------------------------
// The CI source scan (tools/laige-determinism-lint)
// ---------------------------------------------------------------------------
//
// The second, independent enforcement layer (PRD §10.3: "no
// unordered containers in sim hot paths"; ADR 0002: no platform
// intrinsics or raw FP outside the engine in sim translation units):
// a textual scan of the sim module's translation units
// (src/laige-sim/** today; laige-core is exempt — it is the
// engine-math home that defines the banned types) rejecting:
//
//   D1  raw `float`/`double` type tokens,
//   D2  the std::unordered_{map,set,multimap,multiset} container
//       tokens,
//
// with comments and string literals stripped (the documented
// false-positive policy — tools/laige-determinism-lint header) and a
// per-line exception marker for the documented off-determinism-path
// uses (wall-clock diagnostics, the presentation alpha conversion,
// the JSON number policy):
//
//   // LAIGE-DETERM-EXCEPTION: G-R8 <one-line reason>
//
// The scanner validates the marker format (rule id G-R8, non-empty
// reason); a malformed marker is a violation, and every suppressed
// line is counted and reported (EXC-006: new markers need human
// review — they are visible in every CI run and in ctest).
//
// ---------------------------------------------------------------------------
// PRNG substreams (PRD §10.3: "seeded engine PRNG, per-substream;
// seed is part of the replay")
// ---------------------------------------------------------------------------
//
// Each registered system owns a PRNG substream derived from (the
// world's seed, the system's SystemId) — the Prng::deriveSubstream
// contract (laige/prng.h, M0-CORE-06): a pure function of (seed, id),
// so the derivation is bit-identical across runs, and substreams
// never interleave (a system draws only from its own stream; the
// draw order is the call order — the replay state). The world holds
// the streams in its system registry (one std::optional<Prng> per
// system record — setup-path only); World::Options::seed selects the
// master seed (default 0 — a valid master seed: the Prng's state is
// nonzero for every 64-bit seed, prng.h) and World::Options::
// deterministic selects whether the streams exist (SystemContext::
// rng is nullptr when disabled). The seed is part of the replay
// identity (ADR 0002: replay = inputs + seed + math backend + config
// hash).
//
// ---------------------------------------------------------------------------
// Determinism mode semantics (M1 scope; the ARCH-010 scope statement)
// ---------------------------------------------------------------------------
//
//   enabled: true (default)
//       The engine runs in deterministic mode: the selected SimMath
//       backend drives the engine's built-ins (the Position2D
//       component and the presentation snapshot, engine.h), and every
//       system receives its PRNG substream (SystemContext::rng). The
//       simulation state after N completed ticks is a pure function
//       of (config, seed, registration order, N, inputs). Verified
//       same-build by the `determinism_mode` CTest suite;
//       cross-target verification (build/platform/ISA/compiler
//       matrix) is M1-DET-04's detcheck matrix.
//   enabled: false
//       M1 semantics: the engine does NOT create the per-system PRNG
//       substreams (SystemContext::rng is nullptr — a system that
//       draws must handle nullptr as "no random source"), and the
//       run is NOT replayable (no replay identity: the seed and the
//       substreams are part of it). Nothing else changes in M1: the
//       SimMath-only trait still applies (it is a compile-time fact
//       about the types, not a runtime mode), the engine still runs
//       the selected backend's built-ins, and no M1 built-in system
//       consumes randomness. (The first non-deterministic system
//       lands with M2.)
//
// The full promised scope (what is and is not deterministic, and
// under which build/platform conditions) is stated in
// docs/concepts/determinism.md; this header's API contract is in
// docs/api/determinism.md.

#pragma once

#include <cstdint>
#include <type_traits>

#include "laige/sim_math.h"  // fpx16_16, SimMath, the backend traits

namespace laige {

// The SimMath backend a deterministic run uses (ADR 0002; the typed
// form of the config ids "fixed_point_16_16" / "float_pinned_32").
// Selected once at engine init (Engine::create consumes it — the
// ADR's "factory-selected at init", no per-call dispatch).
enum class SimMathBackend : std::uint8_t {
  // The default backend: Q16.16 in int32_t storage, int64_t
  // intermediates (laige/fpx16_16.h). Bit-exact across all builds,
  // platforms, ISAs, and compilers (guaranteed by the C++20 language
  // standard — ADR 0002); required for lockstep (AC-10.3) and
  // authoritative MMO.
  FixedPoint16_16,
  // Opt-in IEEE float semantics: binary32 (`float`) with the pinned
  // flag set (ADR 0002, laige/sim_math.h). Bit-exact across runs of
  // the same build on the same platform/ISA; cross-ISA identity is
  // the detcheck matrix's job (M1-DET-04) — a failing pair is
  // declared unsupported for this backend.
  FloatPinned32,
};

// The typed determinism block of EngineConfig (M1-DET-01; ADR 0002).
// A plain value: the engine copies it (config echo, engine.h) — no
// ownership, no state, trivially copyable.
struct DeterminismConfig {
  // Deterministic mode on/off (S-7: deterministic by default). M1
  // semantics in the header preamble "Determinism mode semantics".
  bool enabled{true};
  // The SimMath backend the deterministic run uses (ADR 0002).
  SimMathBackend math{SimMathBackend::FixedPoint16_16};
};

namespace detail {

// IsDeterminismSafe<T>: true when T's STORAGE is bit-deterministic —
// every member (recursively) is an integer, an enum, a
// SimMath-registered scalar, or a SimMath-registered vector.
//
// UNSAFE BY DEFAULT: the primary template is false, so a type with no
// specialization below (and no LAIGE_DETERMINISM_SAFE mark) fails the
// G-R8 check — the compile-time error (S-7: "using raw float/double
// inside deterministic systems is a compile-time/trait error").
template <typename T, typename = void>
struct IsDeterminismSafe {
  static constexpr bool value = false;
};

// Integers: bit-exact storage on every platform (C++20 two's
// complement — the language-standard guarantee ADR 0002 relies on).
template <typename T>
struct IsDeterminismSafe<T, std::enable_if_t<std::is_integral_v<T>>> {
  static constexpr bool value = true;
};

// Enums: the underlying type is always an integral type (so the
// storage is integer-exact, like the integers above).
template <typename T>
struct IsDeterminismSafe<T, std::enable_if_t<std::is_enum_v<T>>> {
  static constexpr bool value = true;
};

// The SimMath-registered scalars (ADR 0002: one per backend):
// fpx16_16 (the fpx16_16 backend) and float (the fp32_pinned
// backend's binary32 Scalar). `double` is intentionally NOT here: no
// SimMath backend uses it, so it is never determinism-safe storage —
// the compile-time half of G-R8's "compile error".
template <>
struct IsDeterminismSafe<fpx16_16> {
  static constexpr bool value = true;
};
template <>
struct IsDeterminismSafe<float> {  // LAIGE-DETERM-EXCEPTION: G-R8 registers float as the fp32_pinned backend's SimMath-registered Scalar (ADR 0002) — this line declares the registration, it is not a raw-float use
  static constexpr bool value = true;
};

// The SimMath-registered vector types (one pair per backend): the 2D
// and 3D value types of the two SimMath instantiations
// (laige/sim_math.h).
template <>
struct IsDeterminismSafe<sim::SimMath<sim::Fpx16_16>::Vec2> {
  static constexpr bool value = true;
};
template <>
struct IsDeterminismSafe<sim::SimMath<sim::Fpx16_16>::Vec3> {
  static constexpr bool value = true;
};
template <>
struct IsDeterminismSafe<sim::SimMath<sim::Fp32Pinned>::Vec2> {
  static constexpr bool value = true;
};
template <>
struct IsDeterminismSafe<sim::SimMath<sim::Fp32Pinned>::Vec3> {
  static constexpr bool value = true;
};

// The member-list verifier for the LAIGE_DETERMINISM_SAFE marker:
// true when every listed member type is determinism-safe (the &&-fold
// over an empty pack is its identity: an empty member list is
// vacuously safe).
template <typename... Members>
constexpr bool areDeterminismSafeMembers() noexcept {
  return (IsDeterminismSafe<
              std::remove_cv_t<std::remove_reference_t<Members>>>::value &&
          ...);
}

}  // namespace detail

// Mark Type as a determinism-safe storage/component type (G-R8,
// S-7): declare that Type's members are exactly the listed member
// types (every member; order is irrelevant — the list is a set of
// types). The mark specializes the trait with the verified member
// list:
//
//   LAIGE_DETERMINISM_SAFE(MyComponent, fpx16_16, fpx16_16,
//                          sim::SimMath<sim::Fpx16_16>::Vec2);
//
// Write it once per type, at namespace scope, next to the type
// definition (the LAIGE_COMPONENT precedent, component.h). A type
// that is itself an integer, an enum, or a SimMath-registered
// scalar/vector needs NO mark (the trait covers it directly). A
// member type that is itself a user struct must be marked in turn
// (recursion). The listed types are verified at the mark site: a
// `double` in the list is a compile error here (the static_assert in
// the specialization below), before any system can declare the
// component in its I/O — the failure names the mark, not the first
// system that happened to use the component.
#define LAIGE_DETERMINISM_SAFE(Type, ...) \
  template <> \
  struct laige::detail::IsDeterminismSafe<Type> { \
    static_assert( \
        laige::detail::areDeterminismSafeMembers<__VA_ARGS__>(), \
        "LAIGE_DETERMINISM_SAFE(" #Type \
        ", ...): the member list must be determinism-safe — every " \
        "listed type must be an integer, an enum, fpx16_16, float, " \
        "a SimMath Vec2/Vec3, or a marked user struct; a `double` " \
        "member is never legal (no SimMath backend uses it — ADR " \
        "0002). See docs/concepts/determinism.md"); \
    static constexpr bool value = \
        laige::detail::areDeterminismSafeMembers<__VA_ARGS__>(); \
  };

}  // namespace laige
