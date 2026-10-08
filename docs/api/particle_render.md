# Particle rendering (`laige::render` particle_render)

The rendering half of FR-2.7 — "Lightweight GPU particle system
(CPU-simulated, 2D + depth), budgeted, pooled" (PRD §15 M2, FR-2.7;
AGENTS CORE-002/004/005/008, PERF-002/003, RENDER-001/003/006,
ARCH-009, ADR 0002). The O(n) particle → sprite conversion pass:
every LIVE particle of a
[`ParticleSystem<Backend>`](particles.md) (M2-PART-01) becomes one
[`SpriteItem`](sprite_batcher.md) declaration in the sprite
batcher's frame window — shared particle atlas, one draw call per
emitter set (one system = one `(atlasId, materialId, blend)` group,
RENDER-001), depth from the particle's depth value (the M2-ISO-01
key, the engine-owned `depthKey`, G-R11). Public header:
`src/laige-render/include/laige/render/particle_render.h` (header-only
— the API is a template over the SimMath backends, the
[`presentation.h`](../src/laige-sim/include/laige/sim/presentation.h)
pattern). Unit suite: `ctest -R particle_render`
(`tests/laige-render/particle_render_tests.cpp`) — pure data + batcher
bookkeeping, no GL environment required: it runs in every local tree
and in CI, and the goldens are hand-computed from the M2-ISO-01 key
formula (dyadic values in the cross-backend exactness zone).

The canonical narrative home is
[`docs/concepts/coordinates.md`](../concepts/coordinates.md) §4.11
(ARCH-008) + the §5 conversion row. The pass reads the
[`particles.md`](particles.md) read-only surface (`liveParticles()`,
`fadeAlpha`) — no write-back (ARCH-009: presentation never mutates
authoritative state); the depth goes through
[`iso_depth_key.md`](iso_depth_key.md) (M2-ISO-01) with the particle
depth quantized to an integer step height (this header's
`particleDepthToStepHeight`); the declarations land in
[`sprite_batcher.md`](sprite_batcher.md) (M2-SPRITE-01).

## The API

| Member | Returns | Notes |
|---|---|---|
| `particleDepthToStepHeight<Backend>(depth)` | `int32` | the particle's depth value → the M2-ISO-01 step height: clamp to `[-kIsoDepthMaxStepHeight, +kIsoDepthMaxStepHeight]` (±2047) FIRST, then round nearest, ties-to-even (fpx: `fpx16_16::toInt32`; fp32: `std::nearbyint` in the pinned rounding mode — the two backends agree on dyadic depths); NaN (fp32) clamps to +2047 (the float-order fall-through — the key is presentation-only, ARCH-009); pure O(1), no allocation, no logging, no GL |
| `declareParticles<Backend>(batcher, system, options)` | `Status` | the O(live) conversion pass: every live particle (live-array order — spawn order with swap removal, the deterministic declaration order) becomes one `SpriteItem` in the batcher's OPEN frame window (the frame-protocol section); the FIRST failed add fails the call with the batcher's error; empty system → no-op success (no adds attempted); O(live) batcher adds, no allocation, no logging, no GL |

`ParticleDeclareOptions` is a plain value (the scene config — the
emitter set's shared render attributes): `atlasId` (the shared
particle atlas), `materialId`, `blend` (default `BlendMode::Additive`
— the particle convention: "Additive (src + dst): particles, light,
glow"), `uv` (the shared atlas sub-rect; default the full atlas),
`layer` (the M2-ISO-01 key layer value; default
`kIsoDepthGroundLayer` = 0 — parallax particle layers are out of
scope for this step). One emitter set = one `ParticleSystem` = one
`(atlasId, materialId, blend)` group = one instanced draw call
(RENDER-001, FR-2.7): multiple sets declare into the same frame
window and group independently.

`kTintChannelDivisor` = 255.0f (the u8 → render-float tint divisor).

## Per-particle declaration (the conversion contract)

Each live particle `p` declares one `SpriteItem` (the batcher's
frame-scoped slot; read it back with the batcher's `at(slot)`):

| `SpriteItem` field | Value |
|---|---|
| `pos` | `p.pos` at the RENDER-006 boundary (sim units → render floats — one documented rounding per component per backend: fpx `fpx16_16::toFloat`, fp32 identity) |
| `depthKey` | `isoDepthKey<Backend>(p.pos, particleDepthToStepHeight<Backend>(p.depth), options.layer)` — the engine-owned key (G-R11); `depthOverride` stays false |
| `scale` | `(p.size, p.size)` at the RENDER-006 boundary (the square sprite, world units) |
| `tint` | `p.tint[0..2] / 255` (RGB, u8 → float) + `fadeAlpha(p) / 255` (alpha — the M2-PART-01 exact u32 fade, one presentation-only rounding per channel) |
| `uv` | `options.uv` (the shared rect — no per-particle UV in this step) |
| `frameIndex` / `rotation` | 0 (no per-particle animation / rotation in this step) |
| `atlasId` / `materialId` / `blend` | `options` (the set's shared attributes) |

Declaration order is the live-array order (spawn order with swap
removal, M2-PART-01) — the deterministic tie-break within equal keys
(RENDER-003, the batcher's stable sort keeps it).

## The depth conversion (RENDER-006, ADR 0002)

The particle's depth is a SIM-space value; the M2-ISO-01 key takes an
INTEGER step height (`|z| <= kIsoDepthMaxStepHeight` = 2047, the key's
documented z domain). `particleDepthToStepHeight` bridges the two:

1. **Clamp FIRST** to `[-2047, +2047]` (per backend — SimMath has no
   ordering: fpx compares the raw integers, fp32 the IEEE floats; a
   NaN falls through to the +2047 clamp, the float-order
   fall-through),
2. **Round** nearest, ties-to-even — fpx `fpx16_16::toInt32` (the
   fpx16_16.h policy), fp32 `std::nearbyint` (the pinned default
   rounding mode — the same policy, NOT `llround`, which rounds half
   away from zero and would diverge on 0.5),

so sub-unit depths land on the nearest integer step: 0.5 → 0, 1.5 →
2 (both backends agree on dyadic depths — the cross-backend exactness
zone; the key is presentation-only outside it, ARCH-009). Saturation:
±3000 → ±2047. The key then packs exactly as the M2-ISO-01 table:
`(layer + 512) << 22 | (round((x + y) * 16) - z * 16 + 2^21)`.

## The frame protocol

`declareParticles` declares into the batcher's frame window — the
window must be OPEN (after `create()`/`beginFrame()`, before
`build()`). The per-frame pattern (the M2-GL-02 cull/batch stage):

```cpp
batcher.beginFrame();
declareParticles(batcher, systemA, optionsA);  // emitter set A
declareParticles(batcher, systemB, optionsB);  // emitter set B
// ... other scene declarations (sprites, tiles, parallax) ...
batcher.build();
// submit: batcher.batches() — one instanced draw call per group
```

- **Stopped batcher** (never created / capacity 0): the first add
  fails → `BudgetExhausted` (an empty system still succeeds — no adds
  are attempted).
- **Closed window** (after `build()`): the first add fails →
  `InvalidArgument` (the batcher's frame-protocol contract).
- **Frame budget overflow** (more particles than the batcher's
  `maxSprites`): the batcher's own policy applies — drop OLDEST
  declaration + one rate-limited warn, the add succeeds, and
  `declareParticles` returns success (the overflow is a
  batcher-internal decision, not a conversion failure).
- **First failure wins**: a failed add returns the batcher's error
  immediately (the conversion is a plain loop; no recovery).

## Performance (DOC-004)

- **Complexity**: O(live) per call — one key computation, one tint
  computation, and one batcher `add` per live particle. No scan of
  dead pool slots (the live prefix, M2-PART-01).
- **Allocations**: NONE — the particle state is pooled
  (M2-PART-01) and the batcher's frame storage is pre-allocated at
  `create` (FR-2.7 "pooled"). Verified: 1 000 frames of
  10 000-particle declare+build loops allocate 0 heap blocks on the
  loop thread (`ParticleRenderZeroAlloc`, the alloc-watch gate).
- **Budget**: the conversion pass is budgeted — the
  `particle_render_10k` entry in [`budgets.json`](../../budgets.json)
  (mean, ms, target 2.0, the PRD §8.1-style gate; measured 1.09108 on
  the canonical Debug tree, the worse of the two backends). The
  batcher's `build`/sort cost is the separate
  `depth_sort_10k` budget (the frame protocol keeps the two
  quantities apart: the budget measures `declareParticles` alone —
  `beginFrame`/`build` are outside the measured window). Baseline:
  [`docs/benchmarks/baselines/m2-particle-render.md`](../benchmarks/baselines/m2-particle-render.md).
- **Batching**: one emitter set = one `(atlasId, materialId, blend)`
  group = ONE instanced draw call (RENDER-001) regardless of particle
  count — 10 000 particles of one set cost one draw call. Multiple
  sets (different atlas/blend) group independently.
- **Common traps**:
  - declaring into a CLOSED window (after `build()`) —
    `InvalidArgument` on the first add (call `beginFrame()` first);
  - forgetting `beginFrame()` between frames — the previous frame's
    window is closed by `build()`;
  - treating `depthKey` as authoritative state — it is
    presentation-only (ARCH-009); the sim depth is the source of
    truth;
  - per-particle rotation / animation / UV in this step — out of
    scope (`frameIndex`/`rotation` stay 0; the shared `uv` rect
    covers the set);
  - more live particles than the batcher's `maxSprites` — the
    oldest particles are dropped (the batcher's budget; size the
    batcher to the scene's worst-case visible count, the
    SpriteBatcher `create` contract).

## Determinism (ARCH-010, ADR 0002)

The declared `(depthKey, pos.x, pos.y)` sequence is a pure function
of the system state (seed, emitter definitions, operation sequence)
per backend: same seed → identical declared sequence (the
`ParticleRenderDeterminism` suite pins a machine-greppable FNV-1a
state hash, the docs/testing.md §4 KAT convention); a different seed
diverges. Cross-backend agreement is promised on dyadic values (the
exactness zone) — not a cross-platform guarantee (ARCH-010 scope).
