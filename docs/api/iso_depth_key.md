# Isometric depth keys (`laige::render` iso depth key)

The engine-owned 32-bit sortable depth key for isometric render
ordering (M2-ISO-01; PRD §4, FR-2.2, FR-2.11 base, AC-4.4, S-5, G-R11;
AGENTS ARCH-009, RENDER-003, CORE-005, PERF-002/003/006; ADR 0005).
Public header:
`src/laige-render/include/laige/render/iso_depth_key.h` (header-only —
the API is a template over the SimMath backends, the
[`presentation.h`](../src/laige-sim/include/laige/sim/presentation.h)
pattern). Unit suite: `ctest -R iso_depth_key`
(`tests/laige-render/iso_depth_key_tests.cpp`) — pure math, no GL
environment required: it runs in every local tree and in CI, and the
goldens are hand-computed from the documented formula.

The canonical narrative home for the coordinate/depth conventions is
[`docs/concepts/coordinates.md`](../concepts/coordinates.md) (created
with this step, ARCH-008); the header preamble carries the
machine-checked contract.

## The API

| Function | Returns | Notes |
|---|---|---|
| `isoDepthKey<Backend>(pos, stepHeight, layer)` | `uint32_t` key | the 32-bit depth key (below) |
| `isoDepthKeyParts(key)` | `IsoDepthKeyParts {layer, quantizedDepth}` | exact inverse of `isoDepthKey` |
| `isoDepthOrderLess(keyA, idA, keyB, idB)` | `bool` | the (key, entity id) total order (RENDER-003) |
| `isoShearSupported(axes)` | `bool` | the depth-key-supported shear check |

All are stateless pure functions: no state created, no state read, no
GL calls, no allocation, no logging.

### `isoDepthKey<Backend>`

```cpp
template <typename Backend>
uint32_t isoDepthKey(sim::SimMath<Backend>::Vec2 pos,
                     std::int32_t stepHeight,
                     std::int32_t layer) noexcept;
```

- **`pos`** — the object's world ground-plane position `(x, y)` in
  world units, the SimMath backend's `Vec2`. The intended source is the
  presentation snapshot's interpolated position
  (`PresentationSnapshot::sample_position`, M1-LOOP-02) —
  presentation-only (ARCH-009).
- **`stepHeight`** — the tile/step height the object **stands on**:
  the elevation of its standing surface, an integer number of world
  units (the tile map's per-tile height, M2-TILE-01). *Not* the
  object's own sprite height.
- **`layer`** — the render layer (`kIsoDepthGroundLayer` = 0 default;
  the parallax layer values land with M2-PAR-01 — background layers
  negative/sorted first, foreground positive/sorted last).

**The formula** (world space only — never screen space, PRD §4):

```
v   = (x + y) − stepHeight            the painter's-order value
q   = round((x + y) · 16)             one rounding: nearest, ties away
d   = q − stepHeight · 16             from zero, in 1/16-world units
key = (layer + 512) << 22 | (d + 2^21)  bits 31..22 layer, 21..0 depth
```

Under every depth-key-supported shear the object's base projects to
`NDC_y = −A·v` (`A > 0`), so **unsigned key order = back-to-front
painter's order**: `keyA < keyB` ⇒ `vA ≤ vB` exactly (the monotone
rounding never inverts), and equal keys mean `|vA − vB| ≤ 1/16` world
units (same screen row to within the documented precision). Layer
ascending is the coarse primary order (background first).

**Domain and saturation** (named constants in the header, CORE-005):
`|x|, |y| ≤ kIsoDepthMaxWorldUnits` (32767), `|stepHeight| ≤
kIsoDepthMaxStepHeight` (2047), `|layer| ≤ kIsoDepthLayerMax` (511).
The function is **total** — it is a per-sprite hot-path call, so it
carries no per-call asserts (PERF-006); instead: non-finite fp32 input
saturates to the domain bound (NaN → lower bound, IEEE), and
out-of-domain finite input saturates at the packing boundary.
In-domain the key is exact on both backends.

### The (key, entity id) total order (RENDER-003)

The render order is lexicographic `(key, entity id)`: M2-SORT-01's
stable radix sort preserves insertion order for equal keys, and the
batcher (M2-SPRITE-01) inserts in the engine's deterministic entity-id
iteration order (FR-1.2) — so equal keys resolve by entity id.
`isoDepthOrderLess` is that comparison (strict weak ordering). This
realizes the FR-2.2 tie tuple "(layer, depth, entity id)": layer and
depth (step height) are packed into the key; the entity id is the
final stable tie-break.

### `isoShearSupported`

True iff the shear is finite, invertible (`det ≠ 0`), and
`−dx.y == −dy.y == zUnit > 0` (exact float equality): both ground axes
project downward with the same slope `A` and the height unit equals
`A`. Both built-in presets pass; a custom shear must pass to use
isometric depth sorting (M2-CAM-02 validates scene shears against it).

## Ownership, lifetime, threading

- **Ownership:** none. All functions return by value; nothing to own,
  release, or invalidate. `IsoDepthKeyParts` is a plain value.
- **Threading / phase:** pure functions — callable from any thread at
  any phase (sim tick, render thread, main thread); no shared state,
  no locks, no GL context. The per-frame render path calls
  `isoDepthKey` once per sprite (M2-SPRITE-02's batch phase).
- **Backend selection:** the template parameter is the scene's
  selected SimMath backend (ADR 0002, the engine/zone init selection) —
  the same dispatch pattern as `PresentationSnapshot<Backend>` and
  `Position2D<Backend>`.

## Performance (PERF-001/003, DOC-004)

- **O(1)** per key: one backend add + one rounding (integer for
  fpx16_16; one exact double product + `llround` for fp32_pinned) + a
  few integer ops. Nothing scales with scene size.
- **Zero allocation, zero logging, zero GL calls, no per-call
  asserts** on any path (the saturation is a few comparisons — the
  total-function contract; disabled diagnostics are absent by
  construction, DBG-004).
- The 10k-sprite per-frame cost is measured with the M2-PERF-01
  render suite (the key step itself is a trivial fraction of the
  §8.1 render CPU budget); the M2-ISO-02 table step adds the
  precompute/incremental path (≤ 0.2 ms for 10k dirty cells,
  `iso_depth_rebuild` budget).
- **Common trap:** recomputing keys from screen-space coordinates, or
  calling `isoDepthKey` from a getter that also runs a scene
  traversal (API-003) — the key is the *result* of a world-state
  read, O(1) in itself.

## Determinism, replication, network authority

- Deterministic per the backend's ADR 0002 scope: fpx16_16 bit-exact on
  every platform (the key is pure integer arithmetic over Q16.16);
  fp32_pinned bit-exact per same build/ISA.
- **Presentation-only (ARCH-009):** the key is never part of replay
  state or the sim state hash. It is derived from the presentation
  snapshot (itself a read-only copy of authoritative state).
- **Not replicated.** Depth keys are a per-client presentation
  concern; servers compute nothing of this API (ARCH-003). Replicated
  state carries only the world state the key is computed from.

## Failure behavior / invalidation

- No errors: the function is total (saturation above). There is no
  state to invalidate — each call is an independent pure computation.
- Out-of-domain input is a **misconfiguration** (scene content beyond
  the documented world size / step range), bounded by saturation,
  never UB (CPP-004, CORE-008).

## Performant example

```cpp
// Render phase, per sprite (the M2-SPRITE-02 batch pass): O(1), no
// allocation — the fpx16_16 backend (the engine default, ADR 0002).
using M = laige::sim::SimMathFpx16;
auto [posOk, pos] = snap.sample_position(entity);   // M1-LOOP-02
if (posOk.ok()) {
  const std::uint32_t key = laige::render::isoDepthKey<M>(
      pos, /*stepHeight=*/tileHeight,  // the tile the entity stands on
      /*layer=*/laige::render::kIsoDepthGroundLayer);
  batcher.add(key, entity, /*sprite data...*/);  // M2-SPRITE-01
}
// Equal keys: the stable sort (M2-SORT-01) + the entity-id insertion
// order below fix the order — laige::render::isoDepthOrderLess is the
// comparison the sort uses.
```

## Misuse warnings

- **Do not derive the key from screen space or camera state** (PRD
  §4, FR-2.2): a screen-space z-order breaks under zoom, custom
  shears, and camera motion, and is exactly what this API exists to
  prevent (S-5, G-R11: engine-owned depth).
- **Do not hand-roll per-sprite z-ordering in game code** (G-R11): the
  per-sprite depth override lands with M2-SPRITE-01 as a counted +
  warned escape hatch ("prefer tile height").
- **Pass the standing-surface height, not the sprite's top.** A tree 3
  units tall standing on ground passes `stepHeight = 0`, not `3` —
  the key anchors the object's BASE (its standing surface).
- **Do not use the key for non-iso scenes.** The formula is the
  isometric painter's order (FR-2.2); side_view / top_down modes get
  their own ordering with M2-PROJ-01 (depth = z / y respectively).

## Related

- [`concepts/coordinates.md`](../concepts/coordinates.md) — the
  canonical coordinate/depth/ordering conventions (ARCH-008).
- [`matrices.md`](matrices.md) — the iso preset matrices the key's
  shear contract is pinned against (M2-GL-03).
- [`presentation.md`](presentation.md) — the interpolated positions the
  key reads (M1-LOOP-02).
- Roadmap: M2-ISO-01 (this), M2-ISO-02 (key table), M2-SORT-01 (stable
  radix sort), M2-SPRITE-01/02 (batcher), M2-PAR-01 (layer values).
