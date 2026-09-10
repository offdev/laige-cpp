# SimMath — the deterministic math interface (`laige::sim`)

The one math interface for deterministic simulation code (M0-CORE-03,
ADR 0002, PRD §10.3). Public header:
`src/laige-core/include/laige/sim_math.h`; pinned instantiation:
`src/laige-core/sim_math.cpp`. Engine math ops are the only
floating-point allowed in deterministic paths — never platform
intrinsics outside the engine (PRD §10.3). The *enforcement* of
"SimMath only" in sim code lands in M1-DET-01 (G-R8); this step ships
the API, the `fp32_pinned` backend, and the pinned flag set.

## Backends (ADR 0002)

SimMath has **one op surface** and **two backends**, selected **once per
engine/zone init** from game config (`determinism.math`) and dispatched
at compile time — one template instantiation per backend, no per-call
indirection (PERF-006):

| Backend | Config id | Storage | Determinism scope (ARCH-010) |
|---|---|---|---|
| `Fp32Pinned` (this step) | `"float_pinned_32"` | IEEE binary32 (`float`) | Bit-exact across runs of the same build on the same platform/ISA. Cross-ISA is **not** promised until the CI detcheck matrix proves it (M1-DET); a failing pair is declared unsupported for this backend. |
| `Fpx16_16` (M0-CORE-04) | `"fixed_point_16_16"` | Q16.16 in `int32_t`, `int64_t` intermediates | Bit-exact across all builds, platforms, ISAs, and compilers by the language standard. The default backend; required for lockstep and authoritative MMO. |

The **backend id is part of replay identity**: replay = inputs + seed +
math backend + config hash. Cross-backend replays are not bit-exact and
are not supported. The PRD §12.3 bandwidth degradation ladder is
defined on `fpx16_16`; `fp32_pinned` zones do not participate in it.

## Quick start

```cpp
#include <laige/sim_math.h>

// Once, at engine/zone init (factory-selected, ADR 0002):
const auto math = laige::sim::SimMathFp32::create();

// Sim hot path (fixed timestep, ARCH-002):
laige::sim::SimMathFp32::Vec2 pos{1.0f, 2.0f};
laige::sim::SimMathFp32::Vec2 vel{0.5f, -0.25f};
pos = math.add(pos, math.mul(vel, 1.0f / 60.0f));  // one tick
pos = math.clamp(pos, laige::sim::SimMathFp32::Vec2{0.0f, 0.0f},
                  laige::sim::SimMathFp32::Vec2{64.0f, 64.0f});
vel = math.normalize(vel);
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
| `add` / `sub` / `mul` / `div` (scalar, `Vec2`) | One IEEE binary32 operation per component, round-to-nearest-even (pinned — see below). `mul(v, v)` is component-wise; `mul(v, s)` / `mul(s, v)` is scaling. |
| `less` / `lessEqual` / `greater` / `greaterEqual` (scalar) | Ordered IEEE comparisons: false when either operand is NaN. |
| `equals` / `notEquals` (scalar, `Vec2`, `Vec3`) | IEEE: `equals` is false when either operand is NaN; `notEquals` is true. Vector forms are component-wise. |
| `isNaN` / `isInf` / `isFinite` / `isOrdered` (scalar) | IEEE classifications; `isOrdered(a,b)` is true iff neither is NaN (the `totalOrder` predicate, not the negation of arithmetic `!=`). |
| `clamp(x, lo, hi)` (scalar, `Vec2`) | Requires finite `lo`/`hi` and `lo <= hi` (debug-assert; release UB — the engine's Result/Status convention). NaN x → NaN; ±inf x → the bound. |
| `lerp(a, b, t)` (scalar, `Vec2`) | Exactly `a + (b - a) * t`: one sub, one mul, one add — **two roundings, never FMA-fused**. Not interchangeable with `a*(1-t) + b*t` (different rounding; replays diverge). t outside [0,1] extrapolates by the same expression (defined). |
| `length(v)` (`Vec2`, `Vec3`) | Exactly `sqrt(x*x + y*y [+ z*z])`: the component squarings, the adds, then one correctly-rounded sqrt (SSE2 vsqrtss / NEON vsqrt), in that order. Always ≥ 0; `length((0,0)) = +0`. |
| `normalize(v)` (`Vec2`, `Vec3`) | `v / length(v)`, component-wise IEEE division. The zero vector is defined to normalize to the zero vector — SimMath never injects NaN from a zero-length input (IEEE 0/0 would give NaN). NaN/inf components propagate per IEEE (an infinite vector can normalize to NaN components). |
| `create()` | The factory form of backend selection (stateless; holding the handle is free). |

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

`fp32_pinned` is bit-exact across **runs of the same build on the same
platform/ISA**. Cross-ISA bit-exactness (x86-64 vs arm64) is not
promised: it holds only if the compilers emit the same operation
sequence, which is defended by the pinned set and re-audited at every
toolchain upgrade. The M1-DET detcheck matrix generates the
per-platform support list; any desyncing pair is declared unsupported
for this backend. `fpx16_16` (M0-CORE-04) is the cross-ISA default and
the required backend for lockstep (AC-10.3) and authoritative MMO.

## Performance (DOC-004)

- **Complexity:** every op is O(1); no loops, no recursion.
- **Allocation:** none (value types, inline calls).
- **Indirection:** none — `SimMath<Backend>` is a stateless template;
  ops are static inline with one instantiation per backend (PERF-006:
  no virtual dispatch, no `std::function`, no `std::map`, no locks).
- **Budget:** both backends must meet the §8.1 sim budget (10k entities
  @ 60 Hz ≤ 3 ms); measured in M1 (CORE-001).
- **Trap:** computing the same math two different ways (e.g.
  `lerp(a,b,t)` vs `a*(1-t)+b*t`, or reordering a sum) produces
  different bits and breaks replays. Use one pinned expression per
  quantity and keep it that way.

## Misuse warnings

- Raw `float` operators in deterministic sim code bypass SimMath and
  break the determinism contract (PRD §10.3). Enforcement: M1-DET-01.
- Replays across backends are not bit-exact: the backend id is part of
  replay identity (ADR 0002).
- `clamp`'s `lo`/`hi` must be finite and ordered; NaN/Inf bounds are
  undefined behavior in release builds (debug builds assert).
- A translation unit that instantiates `SimMath<Fp32Pinned>` must carry
  the pinned flag set (`laige_apply_simmath_policy`); otherwise the
  bit-exact promise for that TU is void.

## Verification

`ctest -R math_float` — suites `SimMathBasics` (bit-exact known values,
±0, commutativity, vector ops), `SimMathNanInf` (the full NaN/Inf
policy), `SimMathProperties` (idempotence, round-trip tolerances, the
pinned-flag runtime canaries), `SimMathDispatch` (stateless
compile-time dispatch, `noexcept` contract, backend contract).
