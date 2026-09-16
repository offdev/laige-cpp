# Determinism (ARCH-010, S-7, G-R8, ADR 0002)

The engine's determinism contract, stated at the scope ARCH-010
requires: **what is deterministic, under which build, verified how, and
what it is not**. This document is the normative home for the M1-DET-01
step (roadmap/M1-heartbeat.md); the strategy itself — two SimMath
backends, one op surface — is [ADR 0002](../decisions/0002-deterministic-math.md).

## The guarantee (scope)

Deterministic mode (`EngineConfig.determinism.enabled == true`, the
default) guarantees:

- **Bit-identical simulation state, run after run, on the same build.**
  The same config, the same seed, the same inputs, run twice in the same
  build (same compiler, platform, architecture) produce identical
  per-tick state — verified by
  `determinism_tests.cpp` (`DeterminismMode.*`): a trivial moving-entity
  sim produces identical FNV-1a per-tick state hashes over 256 ticks in
  two consecutive runs (the machine-greppable
  `determinism-tick-stream` line lands in the ctest output).
- **The guarantee is per-build by construction of the default backend.**
  `fpx16_16` (Q16.16 integer arithmetic) is bit-identical across build,
  platform, ISA, and compiler by the C++20 language standard — no flag
  archaeology. `fp32_pinned` is bit-identical across runs of the same
  build on the same platform/ISA; its cross-ISA scope is *not* promised
  until the detcheck matrix proves it (M1-DET-04). ADR 0002 states both
  scopes in its decision table; this document is where the promised
  scope lives once the tests exist.
- **The seed is part of replay identity.** A different seed diverges
  (`DeterminismMode.DifferentSeedDiverges`): replay identity is
  inputs + seed + math backend + config (ADR 0002).
- **The math backend is part of replay identity.** Cross-backend replays
  are not bit-exact and are not supported (ADR 0002): a replay recorded
  under `fpx16_16` must be replayed under `fpx16_16`.

**What it is not (yet):**

- Cross-compile / cross-platform bit-identity is not part of this step's
  verification — it is M1-DET-04's detcheck matrix (two build
  configurations, both backends).
- `fp32_pinned` cross-ISA determinism is not promised; any desyncing CI
  pair is declared unsupported for that backend (ADR 0002 review
  conditions).
- Replay *execution* (the `laige-replay` runner feeding a recorded
  input stream back through the sim, `world.state_hash`) is
  M1-DET-03. Replay *recording* has landed with M1-DET-02: the
  versioned log format, the `ReplayRecorder`, and the
  `Engine::startReplayRecording` / `laige-run --replay` wiring
  ([api/replay.md](../api/replay.md)).
- PRNG *state introspection* (reading a substream's state words) is
  M1-DET-03 (its state words join `world.state_hash`);
  `laige::Prng` deliberately has no state getters.

## Why only SimMath ops (G-R8)

In deterministic systems, **raw `float`/`double` and platform
intrinsics outside SimMath are forbidden** (PRD §10.3, S-7, G-R8): the
two reasons are (a) FMA contraction, reassociation, and per-compiler
defaults silently change IEEE results between builds, and (b)
`double` has no SimMath backend at all — it is never determinism-safe
storage (ADR 0002). The ban is enforced at two independent layers that
cover each other's blind spots:

1. **Compile-time trait (components).** Every component a system
   declares I/O for must be *determinism-safe* storage, checked by a
   `static_assert` in `World::registerSystem` (entity.h). The mechanism
   is in [laige/sim/determinism.h](../../src/laige-sim/include/laige/sim/determinism.h)
   (see [api/determinism.md](../api/determinism.md) for the API):
   - `detail::IsDeterminismSafe<T>` — false by default (primary
     template); true for integers, enums, `fpx16_16`, `float` (the
     `fp32_pinned` backend's registered `Scalar`), and the four
     `SimMath<B>::Vec2/Vec3` types (one pair per backend). `double` is
     intentionally not safe: no backend uses it.
   - `LAIGE_DETERMINISM_SAFE(Type, MemberTypes...)` — the declaration
     that a user struct's storage is exactly the listed member types.
     The member list is verified at the mark site (a `static_assert` in
     the specialization: a `double` in the list is a compile error there,
     before any system can use the component), and the mark specializes
     the trait. An unmarked user struct is never safe, even if its
     members look safe: the mark is the declaration, not a heuristic.
   - `World::registerSystem` folds the trait over the system's
     declared I/O (`detail::IoComponentSafety<Io<T, Access>>`): a
     non-safe component fails the `static_assert` with an actionable
     message naming the fix (mark the component, or change the storage).
   - The compile-check fixtures (`tests/laige-sim/compile_fail/`, CTest
     `trait_compile_*`) prove both halves: a marked safe component
     compiles; an unmarked struct, a `double` member, and a `double` in
     the mark's member list each fail to compile with the G-R8 message.
2. **Source scan (translation units).** `tools/laige-determinism-lint`
   scans every `src/laige-sim/**` translation unit for raw `float` /
   `double` type tokens, float/double literals, and `unordered_*`
   containers (PRD §10.3 also bans unordered containers in sim hot
   paths — a deterministic container, if ever needed, gets an ADR
   first). The trait cannot see helper code outside component storage;
   the lint cannot see template instantiations; together they cover
   the sim module. Runs in CI (`determinism-lint` job, both workflows)
   and in ctest (`determinism-lint-*` fixture tests + real tree).

## The source scan and its exception policy

The lint is textual: it strips comments, string/char literals, and raw
strings before matching, so a `float` in a comment or a string is not a
violation (documentation is not code). It matches case-sensitive,
word-bounded tokens, so `Float`, `fromFloat`, `next_float01`, `toFloat`
are not violations (they are not the type token).

**Exceptions** are explicit, same-line markers:

```cpp
double budgetMs = 1.0;  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic only (ARCH-009: never enters sim state)
```

Rules (the documented false-positive / legitimate-off-path policy):

- The marker must be a trailing `//` comment **on the offending line**,
  must name the rule id `G-R8`, and must carry a non-empty reason. A
  line containing `LAIGE-DETERM-EXCEPTION` that does not match the
  format is itself a violation (a marker cannot be half-written).
- Every suppressed line is counted and printed in the lint output
  (EXC-006: exceptions stay visible — they appear in every CI run and
  ctest, so new markers need human review by construction).
- A marker is only legitimate for uses that provably never touch
  deterministic state: wall-clock diagnostics (the M1-SYS-03
  system-timing doubles — ARCH-009), the presentation alpha conversion
  (a wall-clock fact by design), the JSON number policy (ADR 0003:
  config parsing stores numbers as doubles and must round-trip them
  exactly), and the trait's own registration of `float` as the
  `fp32_pinned` backend's `Scalar`.

## PRNG substreams

Randomness in deterministic mode is a *named input*, not an accident:

- The engine seed (`EngineConfig.seed`, default
  `laige::kDefaultSimulationSeed == 0`) is the master seed; it is part
  of the config and of replay identity (ADR 0002) and is logged on
  `engine/run_started`.
- **Each registered system gets its own substream**, derived as
  `Prng::deriveSubstream(seed, systemId)` (`systemId` is the dense
  registration id, starting at 1; id 0 is the master and is never
  assigned to a system). The derivation is the Prng's own contract
  ([api/prng.md](../api/prng.md)): `deriveSubstream(seed, id) ==
  Prng(seed + id * kSplitmix64Increment)` — a fresh, independent stream
  that never interleaves with another's.
- The system's stream lives in `SystemRecord` and is **advanced in
  place** during `runSystems` — its current state *is* the replay state.
  `SystemContext.rng` points at it (or is `nullptr` in a world created
  with `deterministic == false`).
- A system that draws is deterministic *because the draw is at a fixed
  position in the system's run* (e.g. before its iteration) and comes
  from a fixed seed — the call order is the replay state, not the
  machine.
- `DeterminismMode.SubstreamsMatchPrngDerivation` cross-checks the
  wiring against an independently constructed `Prng::deriveSubstream`;
  `DeterminismMode.SubstreamsAreIndependent` verifies two systems draw
  from different streams.
- No system *needs* randomness: a system that never draws `ctx.rng`
  costs nothing (one null check per tick).

## Deterministic mode vs. disabled

`EngineConfig.determinism.enabled` (default `true`) selects the mode:

| | enabled (default) | disabled |
|---|---|---|
| Math | SimMath ops only (the active backend) | SimMath still recommended; the engine does not police a game that opts out |
| PRNG substreams | created per system (`SystemContext.rng` non-null) | none (`SystemContext.rng == nullptr`; `DeterminismMode.DeterminismDisabledHasNoSubstream`) |
| G-R8 trait | enforced in `World::registerSystem` | enforced (the trait is a property of the component storage, not the mode — a sim component that stored `double` was never legal) |
| Replay identity | seed + backend + config + inputs | not claimed |

Disabling determinism is an escape hatch for non-deterministic
prototypes, not a different math policy: the same SimMath ops, the same
storage rules — only the PRNG and the replay promise are switched off.

## The config surface (provisional)

`EngineConfig` (laige/sim/engine.h) carries the two keys:

```cpp
struct EngineConfig {
  std::uint32_t tickRateHz{...};
  std::uint32_t entityCapacity{...};
  std::uint32_t churnPerFrameBudget{...};
  std::uint64_t seed{laige::kDefaultSimulationSeed};      // 0..2^64-1 (programmatic)
  DeterminismConfig determinism{};                        // {enabled, math}
};
```

- `DeterminismConfig { bool enabled{true}; SimMathBackend math{FixedPoint16_16}; }`
  with `SimMathBackend::FixedPoint16_16` (id `fixed_point_16_16`, the
  default) and `SimMathBackend::FloatPinned32` (id `float_pinned_32`).
- The JSON surface is **provisional** (M1-HEAD-01): `parseEngineConfig`
  accepts `{"seed": 0..2^53, "determinism": {"enabled": bool,
  "math": "fixed_point_16_16"|"float_pinned_32"}}`. The seed is bounded
  to `2^53` in JSON because ADR 0003 stores numbers as doubles (exact to
  2^53); the programmatic `EngineConfig.seed` is the full `uint64_t`.
  Unknown nested keys warn (`config/unknown_key`) and are ignored — the
  forward-compat rule. **M1-CFG-01 owns the final config schema**; these
  keys land on the provisional surface until then.
- The engine selects the backend once at init (compile-time dispatch,
  ADR 0002): it registers the matching built-in component
  (`Position2DFpx16` or `Position2DFp32`) first and builds the
  presentation snapshot for the same backend. The selection is logged on
  `engine/run_started` (`seed`, `determinism`, `math` fields).

## Verification (this step)

- `ctest -R determinism_mode` — the `DeterminismMode.*`
  (same-seed identical 256-tick hash streams; different-seed divergence;
  substream golden cross-check + independence; disabled-mode null rng),
  `DeterminismEngine.*` (backend selection: built-in component +
  snapshot), and `DeterminismConfigParse.*` (the seed/determinism keys:
  defaults, valid values, the rejection table) suites.
- `ctest -R trait_compile` — the G-R8 trait compile-checks (one positive
  fixture, three negative fixtures, each asserting the actionable G-R8
  message).
- `ctest -R determinism-lint` — the source-scan fixture tests + real
  tree (plus the `determinism-lint` CI job in both workflows).
- Cross-target: M1-DET-04 (the detcheck matrix over both backends).

## Related

- [ADR 0002 — Deterministic math strategy](../decisions/0002-deterministic-math.md)
  (the two backends, one op surface, replay identity).
- [ADR 0003 — Config JSON](../decisions/0003-config-json.md) (the
  double-number policy the JSON seed bound comes from).
- [api/determinism.md](../api/determinism.md) — the trait API
  (`IsDeterminismSafe`, `LAIGE_DETERMINISM_SAFE`).
- [api/prng.md](../api/prng.md) — `laige::Prng`, substream derivation.
- [api/sim_math.md](../api/sim_math.md) — the SimMath op surface and
  backend policies.
- [api/engine.md](../api/engine.md) — the `seed`/`determinism` config
  keys and the backend selection.
- [api/replay.md](../api/replay.md) — the replay recording
  (M1-DET-02): the versioned log format, the recorder, the replay
  identity hashes.
- [api/detcheck.md](../api/detcheck.md) — the replay-comparison tool
  (M1-DET-04 runs it against the two backends).
