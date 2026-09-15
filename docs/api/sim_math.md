# SimMath — the deterministic math interface (`laige::sim`)

The one math interface for deterministic simulation code (M0-CORE-03,
M0-CORE-04, ADR 0002, PRD §10.3). Public header:
`src/laige-core/include/laige/sim_math.h`; pinned instantiations:
`src/laige-core/sim_math.cpp` (fp32_pinned) and
`src/laige-core/sim_math_fixed.cpp` (fpx16_16). Engine math ops are the
only floating-point allowed in deterministic paths — never platform
intrinsics outside the engine (PRD §10.3). The *enforcement* of
"SimMath only" in sim code lands in M1-DET-01 (G-R8); M0 ships the API,
both backends, and the pinned flag set.

## Backends (ADR 0002)

SimMath has **one op surface** and **two backends**, selected **once per
engine/zone init** from game config (`determinism.math`) and dispatched
at compile time — one template instantiation per backend, no per-call
indirection (PERF-006):

| Backend | Config id | Storage | Determinism scope (ARCH-010) |
|---|---|---|---|
| `Fpx16_16` (default) | `"fixed_point_16_16"` | Q16.16 in `int32_t`, `int64_t` intermediates | Bit-exact across all builds, platforms, ISAs, and compilers by the language standard. The default backend; required for lockstep and authoritative MMO. |
| `Fp32Pinned` (opt-in) | `"float_pinned_32"` | IEEE binary32 (`float`) | Bit-exact across runs of the same build on the same platform/ISA. Cross-ISA is **not** promised until the CI detcheck matrix proves it (M1-DET); a failing pair is declared unsupported for this backend. |

The **backend id is part of replay identity**: replay = inputs + seed +
math backend + config hash. Cross-backend replays are not bit-exact and
are not supported. The PRD §12.3 bandwidth degradation ladder is
defined on `fpx16_16`; `fp32_pinned` zones do not participate in it.

The `determinism.math` config plumbing lands with the config step
(FR-1.5); until then, deterministic/lockstep engine code targets
`SimMathFpx16` (the default) and opt-in IEEE-float zones target
`SimMathFp32`.

## Quick start

```cpp
#include <laige/sim_math.h>

// Once, at engine/zone init (factory-selected, ADR 0002). The
// DEFAULT backend is fpx16_16:
const auto math = laige::sim::SimMathFpx16::create();

// Sim hot path (fixed timestep, ARCH-002):
laige::sim::SimMathFpx16::Vec2 pos{laige::fpx16_16::fromInt32(1),
                                   laige::fpx16_16::fromInt32(2)};
laige::sim::SimMathFpx16::Vec2 vel{laige::fpx16_16::fromFloat(0.5f),
                                   laige::fpx16_16::fromFloat(-0.25f)};
pos = math.add(pos, math.mul(vel, laige::fpx16_16::fromFloat(1.0f / 60.0f)));
pos = math.clamp(pos, laige::sim::SimMathFpx16::Vec2{},
                 laige::sim::SimMathFpx16::Vec2{laige::fpx16_16::fromInt32(64),
                                                laige::fpx16_16::fromInt32(64)});
vel = math.normalize(vel);

// Opt-in IEEE-float zones (single-player / non-lockstep, ADR 0002):
const auto fmath = laige::sim::SimMathFp32::create();
```

Game and system code is written once against the interface — there is no
per-game double code path (ADR 0002).

## The op surface

All ops are static and inline on the stateless `SimMath<Backend>` —
O(1), no allocation, `noexcept`, zero per-call indirection (PERF-006).
`Vec2`/`Vec3` are trivially copyable value types for dense sim state
(PERF-004); default-initialized to the zero vector.

| Op | Exact expression / policy |
|---|---|
| `add` / `sub` / `mul` / `div` (scalar, `Vec2`) | One correctly-rounded operation per component in the backend's format (pinned — see the backend sections below). `mul(v, v)` is component-wise; `mul(v, s)` / `mul(s, v)` is scaling. |
| `less` / `lessEqual` / `greater` / `greaterEqual` (scalar) | Ordered comparisons: false when either operand is NaN (fp32); total order (fpx16_16 has no NaN). |
| `equals` / `notEquals` (scalar, `Vec2`, `Vec3`) | IEEE: `equals` is false when either operand is NaN; `notEquals` is true. Exact bit equality for fixed-point. Vector forms are component-wise. |
| `isNaN` / `isInf` / `isFinite` / `isOrdered` (scalar) | Backend classification: IEEE for fp32_pinned; for fpx16_16 — no NaN/Inf exist, so `isNaN`/`isInf` are always false, `isFinite` always true, `isOrdered` always true. |
| `clamp(x, lo, hi)` (scalar, `Vec2`) | Requires finite `lo`/`hi` and `lo <= hi` (debug-assert; release UB — the engine's Result/Status convention). fp32: NaN x → NaN, ±inf x → the bound. fpx16_16: plain total-order bounding. |
| `lerp(a, b, t)` (scalar, `Vec2`) | Exactly `a + (b - a) * t`: one sub, one mul, one add — **two roundings, never FMA-fused** (fp32). fpx16_16: the same expression with saturating, ties-to-even integer ops; `t` outside [0,1] extrapolates by the same expression (defined). |
| `length(v)` (`Vec2`, `Vec3`) | Exactly `sqrt(x*x + y*y [+ z*z])`: the component squarings, the adds, then one correctly-rounded sqrt, in that order. fp32_pinned: SSE2 vsqrtss / NEON vsqrt. fpx16_16: accurate while the sum of squares stays inside the Q16.16 range, saturating beyond (documented below). Always ≥ 0; `length((0,0)) = 0`. |
| `normalize(v)` (`Vec2`, `Vec3`) | `v / length(v)`, component-wise division in the backend's format. The zero vector is defined to normalize to the zero vector — SimMath never injects NaN from a zero-length input. NaN/inf components propagate per the backend's policy. |
| `create()` | The factory form of backend selection (stateless; holding the handle is free). |

## fpx16_16 policy (default backend)

Type: `laige::fpx16_16` (`src/laige-core/include/laige/fpx16_16.h`),
backend: `laige::sim::Fpx16_16`.

**Storage / range / resolution.** `std::int32_t raw`, value = raw / 2^16:
range `[-32768.0, 32767.99998474]`, resolution 2^-16 ≈ 1.5259e-5 units
(~65k sub-tile steps per unit) — ample for tile-based 2D worlds
(ADR 0002).

**Determinism scope (ARCH-010).** Bit-exact across **all** builds,
platforms, ISAs, and compilers — guaranteed by the C++20 language
standard (two's complement is mandated; every integer operation used is
defined). No pinned compiler flags are needed for this backend: there is
no fused integer operation for a compiler to discover, and no rounding
mode to change. Cross-compiler agreement is pinned by the committed
known-answer hash in `tests/laige-core/math_fixed_tests.cpp`
(`FixedPointDeterminism`) and verified locally on g++ 16.2.1 and
clang++ 22.1.8 (the CI hookup lands in M1-DET-04).

**Rounding mode.** Round-to-nearest, **ties-to-even** — the
fixed-point analogue of IEEE round-to-nearest-even, so both backends
share one documented rounding convention. Exact ties are: `mul` —
product whose low 16 bits are exactly 0x8000; `div` — remainder of
exactly half the divisor; `toInt32` — value exactly k + 0.5;
`fromFloat` — v × 2^16 exactly k + 0.5 (reachable only while |v| < 2^23);
`sqrt` — no exact ties exist for the square root of an integer, so
round-to-nearest ≡ ties-to-even there.

**Overflow: saturation (defined for every input — CPP-004, no UB).**
All arithmetic computes in `int64_t` and saturates to the type's range:
`add(max, max) = max`, `sub(min, max) = min`, `mul(min, min) = max`,
`div(max, 1 ulp) = max`, `negate(min) = max`. (Only `negate(min)`
saturates — `-max = -32767.99998` is representable.)

**Division by zero (defined; never traps on a P0 target).**
`x / 0 = ±max()` with the sign of x; `0 / 0 = +0` — the saturation
analogue of IEEE `x/0 = ±inf` (the type has no NaN/Inf).

**Comparisons.** Total order — every pair comparable, `equals` is exact
bit equality.

**Conversions.**
`fromInt32(v)`: exact for |v| ≤ 32768; saturates outside the Q16.16
range. `toInt32(x)`: round-to-nearest, ties-to-even (0.5 → 0, 1.5 → 2,
-0.5 → 0, -1.5 → -2); result in [-32768, 32768]. `toFloat(x)`: one
rounding (raw → nearest binary32, then the exact 2^-16 scale).
`fromFloat(v)`: NaN → +0 (defined); ±inf and |v| ≥ 32768 saturate;
otherwise one rounding of the exact scale v × 2^16, ties-to-even (the
pinned default rounding mode — no `fesetround` is ever called).

**`length` accuracy bound.** The squarings and sum are saturating Q16.16
ops, so `length` is accurate while `x*x + y*y [+ z*z]` stays inside the
Q16.16 range (per component, |v| ≲ 181.02 for an axis-aligned vector;
e.g. `length((181, 0)) = 181` exact, `length((182, 0)) = sqrt(max) ≈
181.0193` — an under-estimate, still deterministic). Local sim math
(per-entity, per-tile: velocity updates, near-neighbor distances,
collision) stays well inside the bound; world-span distances (up to
~92682) are out of scope for this format and belong to M1-DET-02's
determinism audit with a wider path if the engine needs them.

## fp32_pinned NaN/Inf policy

Defined, not "whatever the CPU does" (full text in the header):

- **Arithmetic** (`add`/`sub`/`mul`/`div`, `sqrt`): IEEE-754 binary32,
  round-to-nearest-even. A NaN operand yields NaN; 0/0 = NaN; x/0 = ±inf
  (no trap); inf − inf = NaN; inf × 0 = NaN; x/inf = ±0.
- **Comparisons:** ordered — false when either operand is NaN;
  `notEquals` is true when either is NaN; NaN is not equal to itself.
- **`clamp`:** NaN in → NaN out; ±inf in → the corresponding bound.
- **`lerp` / `length`:** NaN in → NaN out; the exact pinned expressions
  above.
- **`normalize`:** zero vector → zero vector (policy); NaN/inf propagate
  per IEEE.
- **Signed zero** follows IEEE: `-0.0f` is representable,
  `-0.0f == +0.0f` is true, `0 + -0 = +0`, `-1 * 0 = -0`.
- **No op may signal an FP exception or trap** on a P0 target; no flush
  to zero (denormals are first-class: e.g. `sqrt(denorm_min)` is a
  finite denormal).

## fp32_pinned pinned flag set (ADR 0002)

The bit-exact promise holds only if every compiler emits the same
operation sequence. The pinned set is applied by
`laige_apply_simmath_policy()` (root `CMakeLists.txt`) to every target
that carries deterministic sim math — today `laige-core` and its tests;
from M1 on, every sim module (`laige-sim`, and authoritative-sim code in
`laige-net`/`laige-server`):

| Compiler | Flags | Why |
|---|---|---|
| GCC / Clang / AppleClang | `-ffp-contract=off -fno-associative-math` | Never fuse `a*b+c` into a single-rounding FMA (protects `lerp`'s and `length`'s documented two-rounding); never reassociate sums (already the default; passed explicitly so the pinned set is visible on every compile line). |
| MSVC 2022 | `/fp:precise` | MSVC does not FMA-contract C expressions and never reassociates at this setting (passed explicitly — it is the default — so the pinned set is visible on every compile line). |

Banned in sim translation units, all compilers (re-audited at every
toolchain upgrade, ADR 0002):

- `-ffast-math` / `-funsafe-math-optimizations` / `/fp:fast` — they
  enable reassociation, reciprocal math, and FMA contraction, and remove
  the NaN/Inf guarantees above.
- Floating-point intrinsics (`__builtin_*`, `_mm_*`, `_Float*`, FPU
  intrinsics) outside `sim_math.h`.
- Rounding-mode changes (`fesetround`) and FP exception modes (`FE_*`,
  `SetErrorMode`): the pinned mode is round-to-nearest-even, the default
  on every P0 target.

The runtime FMA canaries in `tests/laige-core/math_float_tests.cpp`
(`DotProductCanaryDetectsFmaContraction`, `LerpIsNotFmaFused`) fail
loudly if the flags are ever missing — verified by a negative build with
`-ffp-contract=fast -mfma`.

## Determinism scope (ARCH-010)

- `fpx16_16` (default): bit-exact across **all builds, platforms, ISAs,
  and compilers** by the language standard. The required backend for
  lockstep (AC-10.3) and authoritative MMO (PRD §12).
- `fp32_pinned`: bit-exact across **runs of the same build on the same
  platform/ISA**. Cross-ISA bit-exactness (x86-64 vs arm64) is not
  promised: it holds only if the compilers emit the same operation
  sequence, which is defended by the pinned set and re-audited at every
  toolchain upgrade. The M1-DET detcheck matrix generates the
  per-platform support list; any desyncing pair is declared unsupported
  for this backend.

**Enforcement (G-R8, M1-DET-01):** SimMath ops are the *only* math
allowed in deterministic sim systems. Raw `float`/`double` and
platform intrinsics outside SimMath are forbidden, enforced at two
layers: the compile-time trait in `World::registerSystem` (component
storage must be determinism-safe —
[api/determinism.md](determinism.md)) and the CI source scan
`tools/laige-determinism-lint` (sim translation units). The full scope
statement is [concepts/determinism.md](../concepts/determinism.md).

## Performance (DOC-004)

- **Complexity:** every op is O(1); no loops, no recursion (`fpx16_16`
  `sqrt` is a bounded integer correction loop, ≤ a few iterations).
- **Allocation:** none (value types, inline calls).
- **Indirection:** none — `SimMath<Backend>` is a stateless template;
  ops are static inline with one instantiation per backend (PERF-006:
  no virtual dispatch, no `std::function`, no `std::map`, no locks).
- **Backend cost:** `fpx16_16` multiply/divide run on 64-bit
  intermediates (~2× binary32 cost in tight loops; no transcendental
  calls — `sqrt` is integer arithmetic). Both backends must meet the
  §8.1 sim budget (10k entities @ 60 Hz ≤ 3 ms); measured in M1
  (CORE-001).
- **Trap:** computing the same math two different ways (e.g.
  `lerp(a,b,t)` vs `a*(1-t)+b*t`, reordering a sum, or hand-rolled
  integer math that skips the documented rounding/saturation) produces
  different bits and breaks replays. Use one pinned expression per
  quantity and keep it that way.

## Misuse warnings

- Raw `float` operators in deterministic sim code bypass SimMath and
  break the determinism contract (PRD §10.3). Enforcement: M1-DET-01.
- `fpx16_16` has **no implicit scalar constructors and no arithmetic
  operators**: `5` is not `5/65536`, and hand-rolled `a.raw * b.raw`
  skips the documented rounding and saturation. Construct via
  `fromInt32`/`fromFloat`, compute via the SimMath ops (or the type's
  named static ops).
- Replays across backends are not bit-exact: the backend id is part of
  replay identity (ADR 0002).
- `clamp`'s `lo`/`hi` must be finite and ordered; NaN/Inf bounds are
  undefined behavior in release builds (debug builds assert).
- A translation unit that instantiates `SimMath<Fp32Pinned>` must carry
  the pinned flag set (`laige_apply_simmath_policy`); otherwise the
  bit-exact promise for that TU is void. (`SimMath<Fpx16_16>` needs no
  special flags — its determinism is the language standard's.)
- `fpx16_16` `length` saturates beyond the Q16.16 range of the sum of
  squares (documented bound above); world-span distances need the wider
  path from M1-DET-02.

## Verification

- `ctest -R math_fixed` — suites `FixedPointBasics` (exact values,
  identities, total order), `FixedPointRounding` (exhaustive
  ties-to-even cases for mul/div/toInt32/fromFloat, sqrt rounding),
  `FixedPointSaturation` (min/max, wrap candidates, divide-by-zero,
  conversion saturation), `FixedPointConversions` (int/float round
  trips), `FixedPointSimMath` (the op surface, lerp/clamp/normalize,
  the length accuracy bound), `FixedPointDispatch` (stateless
  compile-time dispatch, `noexcept` contract, backend contract),
  `FixedPointDeterminism` (engine path vs independent raw-int64
  reference agree; committed known-answer hash of the 4096-tick op
  sequence). Verified green under ASan+UBSan, and the known-answer
  hash is identical on g++ 16.2.1 and clang++ 22.1.8 (two-compiler
  local run; CI hookup M1-DET-04).
- `ctest -R math_float` — suites `SimMathBasics` (bit-exact known
  values, ±0, commutativity, vector ops), `SimMathNanInf` (the full
  NaN/Inf policy), `SimMathProperties` (idempotence, round-trip
  tolerances, the pinned-flag runtime canaries), `SimMathDispatch`
  (stateless compile-time dispatch, `noexcept` contract, backend
  contract).
