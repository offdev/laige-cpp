# Determinism-safe storage (`laige/sim/determinism.h`)

The compile-time half of the G-R8 determinism guarantee (M1-DET-01;
PRD §10.3, AGENTS ARCH-010/S-7/G-R8, ADR 0002). Public header:
`src/laige-sim/include/laige/sim/determinism.h` (header-only; no
implementation file). The concept-level contract — what is
deterministic, at what scope, enforced how — is
[concepts/determinism.md](../concepts/determinism.md); this document
covers the API surface.

## Quick start

```cpp
#include <laige/sim/determinism.h>

struct Health {
  std::int32_t current{};
  std::int32_t max{};
};
LAIGE_COMPONENT(Health);
// The member list IS the type's storage: verified at this site.
LAIGE_DETERMINISM_SAFE(Health, std::int32_t, std::int32_t);

// Now a system may declare I/O for Health: the G-R8 static_assert in
// World::registerSystem accepts it.
world.registerSystem(MySystem_Def,
    laige::Io<Health, laige::Access::ReadWrite>{});
```

## The API

`laige::SimMathBackend` (enum class, `uint8_t`) — the backend ids the
config surface names:

| Value | Config id string | Backend |
|---|---|---|
| `FixedPoint16_16` | `"fixed_point_16_16"` | Q16.16, the default; bit-exact across build/platform/ISA/compiler (ADR 0002). |
| `FloatPinned32` | `"float_pinned_32"` | IEEE `float`, pinned flags; same-build/same-ISA scope (ADR 0002). |

`laige::DeterminismConfig` — the engine's determinism block:

| Member | Default | Contract |
|---|---|---|
| `bool enabled` | `true` | Deterministic mode: SimMath-only sim, per-system PRNG substreams, replay identity (seed + backend + config + inputs). |
| `SimMathBackend math` | `FixedPoint16_16` | The selected SimMath backend (compile-time dispatch at engine init; part of replay identity). |

`laige::detail::IsDeterminismSafe<T>` (trait) — true when `T` is
determinism-safe storage:

| `T` | Safe |
|---|---|
| Any integer type (`int8_t` … `uint64_t`) | yes |
| Any enum type | yes |
| `laige::fpx16_16` | yes |
| `float` | yes — the `fp32_pinned` backend's registered `Scalar` (ADR 0002). `float` is the *backend type*, not "raw float in the sim": using it is legal only through SimMath-registered types (this scalar or a `SimMath<Fp32Pinned>` vector). |
| `SimMath<Fpx16_16>::Vec2/Vec3` | yes |
| `SimMath<Fp32Pinned>::Vec2/Vec3` | yes |
| `double` | **no** — no SimMath backend uses it; never determinism-safe. |
| Any other type (unmarked struct, `std::string`, …) | no (primary template is false). |

`LAIGE_DETERMINISM_SAFE(Type, MemberTypes...)` — the mark:

- Declares that `Type`'s members are **exactly** the listed member types
  (every member; order is irrelevant — the list is a set of types).
- Specializes `IsDeterminismSafe<Type>` with the verified member list:
  `value = areDeterminismSafeMembers<MemberTypes...>()` (the &&-fold;
  an empty list is vacuously safe).
- **Fails at the mark site** (a `static_assert` in the specialization): a
  non-safe type in the list — e.g. a `double` member — is a compile
  error there, before any system can declare the component in its I/O.
  The error names the mark and points here.
- Write it once per type, at namespace scope, next to the type
  definition (the `LAIGE_COMPONENT` precedent). A type that is itself an
  integer, an enum, or a SimMath-registered scalar/vector needs **no**
  mark. A member type that is itself a user struct must be marked in
  turn (recursion).

`laige::detail::areDeterminismSafeMembers<Ts...>()` — the &&-fold the
mark expands to (true when every listed type is safe).

`laige::detail::IoComponentSafety<Tag>` — the fold helper
`World::registerSystem` uses: `Io<T, Access>` →
`IsDeterminismSafe<T>::value`; non-`Io` tags are vacuously true (the
`IsIoTag` static_assert fires first, so they never reach this fold).

## Where it is enforced

1. **The mark site** — `LAIGE_DETERMINISM_SAFE` fails fast on a bad
   member list (compile error at the declaration).
2. **`World::registerSystem`** (entity.h) — the third `static_assert`
   folds `IoComponentSafety` over the system's declared I/O: a component
   that is not determinism-safe fails with an actionable message (mark
   the component, or change the storage; points here). The check is over
   *declared* I/O — a system that writes a `double` through some other
   path is caught by the source scan below, not the trait.
3. **`tools/laige-determinism-lint`** — the textual scan of every
   `src/laige-sim/**` translation unit (raw `float`/`double` type tokens,
   float/double literals, `unordered_*` containers), with same-line
   `// LAIGE-DETERM-EXCEPTION: G-R8 <reason>` markers as the documented
   false-positive policy. CI job `determinism-lint` (both workflows) +
   ctest `determinism-lint-*`.

The two layers are complementary by design: the trait covers *component
storage* (what a system's I/O names); the scan covers *sim translation
units* (what the code does). Neither subsumes the other.

## Determinism scope (ARCH-010)

This header defines *storage safety*, not the determinism scope itself.
The scope statement lives in
[concepts/determinism.md](../concepts/determinism.md): same-build
bit-identity (verified), the per-backend scopes of ADR 0002, and the
cross-target work left to M1-DET-04.

## Performance (DOC-004)

Everything here is `constexpr` template metaprogramming evaluated at
compile time: zero runtime cost, zero allocations, no state. The
`static_assert`s cost compile time only (one fold per
`registerSystem` call site; the mark's check is one &&-fold at the mark
site).

## Misuse warnings

- **An unmarked user struct is never safe** — "all my members are ints"
  is not the declaration; the mark is. The trait's primary template is
  false on purpose (fail-closed).
- **The member list must be complete.** Listing a subset of the members
  claims the type has no other members; a missing non-safe member is a
  lie the next `registerSystem` will not catch (the mark already
  passed). Keep the list in sync with the struct (same-file, next to
  it).
- **`float` in a mark means the fp32_pinned backend's Scalar.** Storing
  raw `float` in a component that is meant to run under `fpx16_16` is a
  backend mismatch, not a G-R8 violation — use `SimMath<Fpx16_16>`
  types for backend-independent sim state.
- **`double` is not "almost safe".** It has no backend; there is no
  mode where it is legal in sim storage.

## Verification

- `ctest -R trait_compile` — the compile-check fixtures
  (`tests/laige-sim/compile_fail/`): `trait_compile_ok` (a marked safe
  component compiles), `trait_compile_reject_double` (a `double` member
  fails), `trait_compile_reject_unmarked` (an unmarked struct fails),
  `trait_compile_reject_bad_mark` (a `double` in the mark's member list
  fails at the mark site).
- `ctest -R determinism_mode` — `determinism_tests.cpp`: the
  DeterminismMode / DeterminismEngine / DeterminismConfigParse suites
  (same-seed identical 256-tick hash streams, seed divergence, substream
  golden cross-check + independence, disabled-mode null rng, backend
  selection, the config keys).
- `ctest -R determinism-lint` + CI `determinism-lint` — the source scan
  (fixtures + real tree).

## Related

- [concepts/determinism.md](../concepts/determinism.md) — the scope and
  the two-layer enforcement.
- [ADR 0002](../decisions/0002-deterministic-math.md) — SimMath, the two
  backends, replay identity.
- [api/sim_math.md](sim_math.md) — the SimMath op surface (the only math
  allowed in deterministic systems).
- [api/prng.md](prng.md) — `laige::Prng` substream derivation (the
  per-system streams `SystemContext.rng` points at).
- [api/engine.md](engine.md) — the `seed`/`determinism` config keys and
  the backend selection at init.
- [api/entity.md](entity.md) — `World::Options.seed`/`deterministic` and
  the `registerSystem` G-R8 static_assert.
- [api/system_registry.md](system_registry.md) — `SystemContext.rng` and
  the per-system substreams.
