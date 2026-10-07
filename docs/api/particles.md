# Particle simulation (`laige::sim` particles)

The CPU-simulation half of FR-2.7 — "Lightweight GPU particle
system (CPU-simulated, 2D + depth), budgeted, pooled" (PRD §15 M2,
FR-2.7; AGENTS CORE-002/004/005/008, PERF-002/003/005/008, ARCH-002/
009/010, ADR 0002). A bounded, pooled particle system updated once
per completed simulation tick: burst + continuous emitters, per-
particle 2D position + constant depth, velocity, life, size, and an
exact integer color fade. The rendering half — particles as batched
sprites (shared particle atlas, one draw call per emitter set, depth
from the particle's depth value) — is **M2-PART-02**, which consumes
this header's read-only surface (`liveParticles()`). Public header:
`src/laige-sim/include/laige/sim/particles.h` (header-only — the API
is a template over the SimMath backends, the
[`presentation.h`](../../src/laige-sim/include/laige/sim/presentation.h)
pattern). Unit suite: `ctest -R particles`
(`tests/laige-sim/particles_tests.cpp`) — pure engine math + pool
bookkeeping, no GL environment required: it runs in every local tree
and in CI, and the goldens are hand-computed (Q16.16 raws / dyadic
floats, the exact per-tick pool table, the 4-draw Prng contract).

The particle state's place in the 2.5D model:
[`docs/concepts/coordinates.md`](../concepts/coordinates.md) §4.11
(ARCH-008) + the §5 conversion row.

## The API

| Member | Returns | Notes |
|---|---|---|
| `ParticleSystem<Backend>::create(options)` | `Result<ParticleSystem>` | `maxParticles` in `[1, kParticlePoolMaxParticles]` (65 536; default `kParticlePoolDefaultMaxParticles` = 4096), `maxEmitters` in `[1, kParticleEmitterMaxEmitters]` (256; default `kParticleEmitterDefaultMaxEmitters` = 32), `seed` (any u64) — validates (no log), pre-allocates the pool + emitter table (setup path) |
| `ParticleSystem() = default` | the stopped state | `valid()` false; `addEmitter`/`burst` → `InvalidArgument` (no log); `update()` a no-op; `liveParticles()` empty; `stats()` zero (the RenderThread stopped-state precedent) |
| `addEmitter(def)` | `Result<ParticleEmitterId>` | registers an emitter (setup phase); validates `def` in the documented order (first failure wins); a rejection leaves the registry unchanged + one rate-limited warn; the registry is full → `BudgetExhausted` + warn; O(1) |
| `burst(id, count)` | `Status` | spawns `count` particles from emitter `id` NOW (sim phase — the game's spawn policy); stopped or unknown id → `InvalidArgument` (no log); count 0 a no-op success; O(count) |
| `update()` | void | ONE sim tick: advance → emit → overflow report (the per-tick contract below); called exactly once per completed tick by the game's tick driver (ARCH-002); O(live + Σrates), no allocation, no logging on the happy path |
| `liveParticles()` | `span<const Particle>` | the render read path (M2-PART-02): the compact live prefix `[0, liveCount)` — read it after the sim phase (the M2-GL-02 cull/batch stage); O(1), never writes |
| `fadeAlpha(p)` | `uint8` | the particle's current fade alpha (0..255) — exact u32 integer arithmetic (the color-fade section below) |
| introspection | | `valid()`, `maxParticles()`, `maxEmitters()`, `seed()`, `liveCount()`, `spawnedTotal()`, `droppedTotal()`, `stats()`, `emitterCount()`, `hasEmitter(id)`, `emitterAt(id)`, `prngSeed()`, `prngStatePart1()`, `prngStatePart2()` |

`Particle` (the per-particle state — 40 bytes on both backends):
`pos` (sim space), `depth` (constant over life — the M2-ISO-01
depth-key input M2-PART-02 feeds the batcher), `vel` (world units per
tick, constant), `age`/`life` (sim ticks), `size` (world units),
`tint[4]` (base RGBA, u8), `fadeTicks` (the emitter's fade window).

`ParticleEmitterDef<Backend>` is a plain value (the scene config —
validated AT USE TIME by `addEmitter`): `origin` (the spawn position —
a particle spawns exactly here; no position jitter in M2-PART-01),
`depth`, `velMin`/`velMax` (the per-tick velocity box, componentwise),
`lifeMin`/`lifeMax` (sim ticks), `sizeMin`/`sizeMax` (world units),
`tint[4]` (default white), `fadeTicks` (0 = no fade),
`continuousRate` (particles PER SIMULATION TICK; 0 = burst-only —
unbounded by design: the pool budget bounds the work, overflow
drops).

## The per-tick contract (ARCH-002)

`update()` runs EXACTLY ONCE per completed simulation tick, driven by
the game's tick path (a game system, or the loop's tick hook — the
`TileMap::advanceAnimations` pattern). The sim never depends on the
render frame rate. In-tick order:

1. **Advance** every currently live particle (live-array order):
   `age += 1`, `pos += vel` (one SimMath vector add — ADR 0002);
   `age >= life` kills (swap-removed with the last live particle; the
   swapped-in particle — not yet advanced this tick — is re-examined
   at the same index).
2. **Emit** from every emitter in registration order
   (`continuousRate` particles each).
3. **Report** the tick's overflow: at most ONE rate-limited warn
   (the pool-budget section below).

A particle spawned during tick T's update (the continuous rate) is
live after ticks T..T+life-1 (age 0..life-1) and dies during tick
T+life's update — visible for EXACTLY `life` renders. A particle
spawned by `burst()` before tick T's update gets advanced in that
same tick (age 1 after the update) — visible for `life - 1` renders;
call `burst()` in the sim phase, never between frames. Velocity is
constant (no acceleration — out of scope); depth is constant.

## The pool budget (FR-2.7 "budgeted, pooled", PERF-003/008)

The pool is `maxParticles` PRE-ALLOCATED slots (the setup path — with
the emitter table, the system's only allocations). Live particles
occupy the FIRST `liveCount` slots: a death swap-removes the last
live particle into the dead slot, so the live order is spawn order
with swap removal (deterministic, cache-friendly).

Overflow: a spawn into a full pool is **DROPPED** — counted
(`droppedTotal`, the per-tick bookkeeping) and reported with at most
ONE `particles/pool_overflow` warn per tick (fields `dropped` /
`live` / `capacity`; the LOG-004 logger rate window is a second
layer). A drop never mutates live state, never throws, never grows
the pool, and — critically for determinism — **consumes no Prng
draws**: the pool check precedes the draws, so a drop never perturbs
the stream. The `droppedTotal` gauge is the budget's observability
(DBG-008): a steadily growing `droppedTotal` means the scene wants
more particles than the budget allows (raise `maxParticles` at
create, or thin the emitters).

## Determinism (ARCH-010, ADR 0002)

The state after N updates is a pure function of (seed, emitter
definitions, operation sequence), per backend:

- **fpx16_16** — bit-identical across all builds, platforms, ISAs,
  and compilers (the language-standard guarantee).
- **fp32_pinned** — bit-identical across runs of the same build on the
  same platform/ISA (the detcheck matrix owns cross-target claims).

Each SUCCESSFUL spawn consumes exactly **FOUR Prng draws in a fixed
order**: `vx`, `vy`, `life`, `size`. Each uniform scalar is
`lerp(min, max, u / 2^24)` where `u = next_range(0, 2^24)` — the
Prng's 24-bit tap resolution — one documented rounding per backend
(the ADR 0002 lerp contract); `life` is an integer draw. Motion is
SimMath ops only; the fade is exact integer arithmetic. Nothing reads
platform intrinsics, addresses, or wall-clock time. The Prng state is
part of the deterministic state: `prngSeed()` / `prngStatePart1()` /
`prngStatePart2()` expose it for the World `stateHash` / replay
identity (the M1-DET-03 pattern); the unit suite pins it (a
fixed-seed replay of the same operation sequence produces a
bit-identical live set + a machine-greppable FNV-1a state hash, and
an independent `Prng` advanced by `4 × spawnedTotal` draws lands on
the system's stream state).

## The color fade (exact integer arithmetic)

`fadeTicks == 0`: no fade (the tint alpha is constant). Otherwise:
`remaining = life - age` (a live particle always has `age < life` —
no underflow), and

```
alpha = remaining >= fadeTicks ? tint.a
      : tint.a * remaining / fadeTicks     (u32 multiply + divide)
```

— the alpha ramps linearly from the fade window's start to 0 at
death. One exact multiply + divide per read; no floating point —
bit-identical on every platform (max product 255 × 2^20 < 2^32). The
fade touches the alpha channel only; the RGB channels are constant.
`fadeAlpha(p)` exposes the computed value to the render pass.

## Failure (CORE-008, API-008) — first failure wins

| Operation | Failure | Behavior |
|---|---|---|
| `create` | `maxParticles` ∉ [1, 2^16] or `maxEmitters` ∉ [1, 256] | `InvalidArgument` (no log) |
| `addEmitter` | stopped | `InvalidArgument` (no log) |
| `addEmitter` | registry full | `BudgetExhausted` + warn `particles/emitters_exhausted` (field `capacity`) |
| `addEmitter` | a def field out of domain | `InvalidArgument` + one rate-limited warn `particles/emitter_invalid` (fields `emitter`, `field`); the registry is unchanged |
| `burst` | stopped or unknown emitter id | `InvalidArgument` (no log); count 0 is a no-op success |

The def validation order (first failure wins — the def is caller-owned
scene metadata, untrusted input, SCALE-004 — every field is checked):
`origin` (finite) → `depth` (finite) → `vel_min` (finite) → `vel_max`
(finite) → `vel_box` (`velMin <= velMax` per axis) → `life_min` (>= 1)
→ `life_box` (`lifeMin <= lifeMax`) → `life_max` (<= 2^20) →
`size_min` (finite, >= 0) → `size_max` (finite) → `size_box`
(`sizeMin <= sizeMax`) → `fade_ticks` (<= `lifeMax`). The happy paths
log nothing (LOG-003).

## Ownership, lifetime, threading (CORE-009, CONC-001)

One owner thread (the sim thread). `create` is the setup path (two
pre-allocations: the pool, the emitter table); nothing allocates
afterward — the unit suite proves a 500-tick burst/update window
allocates zero heap blocks on the loop thread (the allocation watch,
`LAIGE_ALLOC_COUNTER`; the sanitizer trees prove the same via
leak-free runs). Move-only: the move is an O(1) pointer swap and the
moved-from system is stopped (its Prng copy shares the stream position
with the live system — safe because the moved-from system is stopped
and never draws again). The render pass reads `liveParticles()`
(read-only, ARCH-009) after the sim phase: presentation reads
authoritative state and never mutates it; nothing in the render pass
writes here.

## Performance (DOC-004)

- **`update()`**: O(live + Σ continuousRates) — one SimMath add + one
  age compare per live particle, 4 Prng draws + one slot write per
  successful spawn, zero heap allocation, zero logging on the happy
  path. At the worst-case 65 536 pool with 256 emitters this is a
  bounded, cache-friendly sequential scan (the pool is a flat array).
- **`burst(id, n)`**: O(n) — the same per-spawn cost, plus at most one
  rate-limited warn when the pool is full.
- **`addEmitter` / introspection**: O(1).
- **Memory**: `maxParticles × 40 B` + `(maxEmitters + 1) × def`
  pre-allocated at create (2.5 MB at the 65 536 worst case) — nothing
  after. The `Particle` layout is 40 B on both backends (no padding
  surprises: two Vec2 + a Scalar + fixed-width fields).
- **Budgeting**: the pool is the budget (FR-2.7 "budgeted, pooled") —
  overflow drops instead of growing; watch `droppedTotal`
  (`stats()`) for scenes that exceed their particle budget. The
  particle→sprite budget (particles rendered through the batcher) is
  measured in M2-PART-02 against the PRD §8.1 scene budget.
- **Traps**: calling `update()` more than once per tick (double
  advance — the tick driver owns the rate); `burst()` between frames
  (the bursted particles get advanced by the next `update()`, so they
  lose one render's worth of life — see the per-tick contract);
  relying on `liveParticles()` span order for anything but
  determinism (the order is spawn order with swap removal —
  deterministic, but a death reshuffles; M2-PART-02 sorts by the
  depth key, not the span order).

## Example (performant) and misuse warning

```cpp
auto r = ParticleSystem<laige::sim::Fpx16_16>::create(
    {.maxParticles = 4096, .maxEmitters = 4, .seed = 1});
ParticleSystem<laige::sim::Fpx16_16> system = std::move(r).takeValue();

// Setup phase: one continuous emitter (sparks) + one burst emitter.
laige::ParticleEmitterDef<laige::sim::Fpx16_16> sparks;
sparks.origin = { /* sim-space Vec2 */ };
sparks.depth  = /* the emitter's depth (M2-ISO-01 key input) */;
sparks.velMin = { /* -1, -1 */ };
sparks.velMax = { /*  1,  1 */ };
sparks.lifeMin = 12;
sparks.lifeMax = 24;
sparks.sizeMin = /* 0.25 */;
sparks.sizeMax = /* 0.5 */;
sparks.tint = {255, 180, 60, 255};
sparks.fadeTicks = 8;
sparks.continuousRate = 2;  // 2 particles per sim tick
system.addEmitter(sparks);

// Per completed sim tick (the tick driver calls update() once):
system.update();

// Render phase (M2-PART-02): read-only.
for (const auto& p : system.liveParticles()) {
  // p.pos / p.depth / p.size / laige::ParticleSystem<...>::fadeAlpha(p)
}
```

**Misuse:** spawning from the render thread (the sim thread owns the
system — one owner, CONC-001); treating `liveCount()` as a promise to
get a slot (a spawn can be DROPPED when the pool is full — that is
the budget, not an error); expecting `burst()` particles to be visible
for `life` renders (they get advanced by the same tick's `update()` —
the per-tick contract); adding emitters during a tick (setup phase —
`addEmitter` is not part of the update path); assuming the span order
is a render order (it is a spawn order with swap removal — M2-PART-02
sorts by the depth key).

## Related

- [`sprite_batcher.md`](sprite_batcher.md) — the render half
  (M2-PART-02 consumes `liveParticles()`).
- [`iso_depth_key.md`](iso_depth_key.md) — the depth key the particle
  `depth` feeds (M2-ISO-01).
- [`tilemap.md`](tilemap.md) — the `advanceAnimations` per-sim-tick
  pattern this step's `update()` follows.
- [`presentation.md`](presentation.md) — the templated-backends
  pattern (this header's shape).
- [`prng.md`](prng.md) — the Prng substream (M0-CORE-06) and the
  24-bit tap resolution.
- [`determinism`](../concepts/determinism.md) — the determinism scope
  (ARCH-010) + the G-R8 enforcement.
