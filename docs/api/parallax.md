# Parallax layers (`laige::render` parallax)

The named background / midground / foreground layer model of FR-2.3 —
the parallax factor, the world-space offset, the UV scroll (auto or
manual) with the exact wrap at the texture boundary, and the batch
path into the sprite batcher (M2-PAR-01; PRD §15 M2, FR-2.3 —
"Parallax layers: named background/midground/foreground layers with
parallax factor, offset, UV scroll, blend", §9.1 S-5, ARCH-009,
G-R11; AGENTS RENDER-001/003, CORE-002/004/005/008, PERF-002/003; ADR
0002). Public header:
`src/laige-render/include/laige/render/parallax.h` (header-only — the
API is a template over the SimMath backends, the
[`presentation.h`](../src/laige-sim/include/laige/sim/presentation.h)
pattern). Unit suite: `ctest -R parallax`
(`tests/laige-render/parallax_tests.cpp`) — pure data + batcher
bookkeeping, no GL environment required: it runs in every local tree
and in CI, and the goldens are hand-computed from the documented
formula.

The canonical narrative home is
[`docs/concepts/coordinates.md`](../concepts/coordinates.md) §4.10
(ARCH-008); the header preamble carries the machine-checked contract.
The layer quads' keys are the
[`iso_depth_key.md`](iso_depth_key.md) (M2-ISO-01) keys with the
layer's depth-layer value (engine-owned, G-R11); the batch path
declares into [`sprite_batcher.md`](sprite_batcher.md)
(M2-SPRITE-01). Tilemap-source layers (the M2-TILE-02 hook) declare
through the tilemap's path —
[`tilemap.md`](tilemap.md) — with this layer's `worldOffset`
translation and `depthLayer`.

## The API

| Member | Returns | Notes |
|---|---|---|
| `ParallaxLayers<Backend>::create(options)` | `Result<ParallaxLayers>` | `maxLayers` in `[1, kParallaxLayersMaxLayers]` (64; default `kParallaxLayersDefaultLayers` = 8) — validates (no log), pre-sizes the slot table (setup path) |
| `ParallaxLayers() = default` | the stopped state | `valid()` false, every operation `InvalidArgument`, no log (the RenderThread stopped-state precedent) |
| `setLayer(def)` | `Status` | registers or REPLACES the layer (setup/config path — the scene config's hot-reload, FR-1.5); validates the def in the documented order (first failure wins); a rejection leaves the slot unchanged; a success RESETS the layer's UV offset to (0, 0) |
| `setUvOffset(id, uv)` | `Status` | sets the layer's CURRENT UV offset (any finite value — wrapped to [0, 1)²); unknown id → `InvalidArgument` (no log); non-finite → `InvalidArgument` + one rate-limited `parallax/uv_offset_invalid` warn |
| `advanceScrolls()` | void | the AUTO layers' per-frame UV advance (once per frame, before the declarations); manual layers untouched; O(layers), no allocation, no logging, no GL |
| `declareTo(batcher, cameraPos)` | `Status` | the render path: declares every SET, ENABLED Image-source layer's wrap quads into the batcher's frame window (the 2 x 2 split); Tilemap-source layers are skipped (the M2-TILE-02 hook — no log); O(layers x 4) batcher adds, no allocation, no logging, no GL |
| `worldOffsetAt(id, cameraPos)` | `Vec2` | the EXACT formula (1): `factor * (cameraPos - center) + offset` — pure O(1), no allocation, no logging, no GL |
| introspection | | `valid()`, `maxLayers()`, `layerCount()`, `has(id)`, `layerAt(id)`, `uvOffsetAt(id)` |

`ParallaxLayerDef` is a plain value (the scene config — the value the
game declares): `id` (slot id < `maxLayers`), `source`
(`Image` | `Tilemap`), `factor` (in [0, 1] — one source of truth),
`center` (the reference camera position — where the layer sits at
exactly `offset`), `offset` (the world-space offset at `p = center`),
`size` (world units; Image only), `uv` (the atlas sub-rect; Image
only), `atlasId`, `tilemapId` (Tilemap only), `materialId`, `blend`,
`depthLayer` (the M2-ISO-01 key layer value — the presets are the
`kParallaxDepthLayer*` constants; a custom layer picks any value in
the M2-ISO-01 domain [-512, +511]), `scrollMode`
(`Manual` | `Auto`), `scrollSpeed` (UV units PER FRAME per axis —
negative scrolls the other way), `enabled`. The named presets:
`kParallaxLayerBackground` = 0 (bg), `kParallaxLayerMidground` = 1
(mid), `kParallaxLayerForeground` = 2 (fg),
`kParallaxLayerCustomBase` = 3 (custom layers: any id ≥ 3).

## The model

A layer is a **world-space rectangle** of one texture (the Image
source) or of one tilemap (the Tilemap source, M2-TILE-02). The
layer's content position at camera position `p` (the camera's
ground-plane (x, y) — the M2-CAM-01 presentation position) is the
EXACT formula:

```
worldOffset(p) = factor * (p - center) + offset        (1)
```

Everything is WORLD space (PRD §4, RENDER-006): (1) is a world-space
translation; nothing in it is screen space. The camera position is
the presentation-side input (ARCH-009) — the sim never sees it.

- **`factor` = 0**: the layer is FIXED IN WORLD SPACE at `offset`
  (it moves full-speed against the camera — maximum parallax, a
  close-by background). **`factor` = 1**: the layer moves 1:1 WITH the
  camera (it is fixed on screen — no parallax, the sky layer).
  Between: the layer's screen position shifts by
  `(factor - 1) * (p - center)`.
- **`center`**: the reference camera position (default (0, 0) — the
  scene's world origin).
- **`offset`**: the layer's world-space offset at `p = center`
  (default (0, 0) — the content origin).

The Image source renders the layer's texture over the world rectangle
`[worldOffset(p), worldOffset(p) + size)` — `size` is the rectangle's
extent in world units. The texture's v axis (v = 0 = first uploaded
texel row, the M2-SPRITE-02 contract) maps to the world +y direction
(downward on screen under the iso projections —
[coordinates.md](../concepts/coordinates.md)).

The Tilemap source references a tilemap (its `tilemapId`): the
tilemap defines its own world grid, and M2-TILE-02 declares its tiles
with this layer's `worldOffset` translation and its `depthLayer`. In
M2-PAR-01 a Tilemap-source layer is DATA ONLY: `declareTo` skips it
(the hook), its `size`/`uv` fields are not validated.

## The render order (the background-first contract)

The batcher draws one group per distinct (atlas, material, blend) in
its deterministic group order — ascending (atlas, material, blend)
(RENDER-003, M2-SPRITE-01) — and, within a group, the instances in the
M2-ISO-01 key order (the layer field dominant). The parallax layer's
quads carry the key's LAYER field:

```
kParallaxDepthLayerBackground  = -2   (the `bg` preset)
kParallaxDepthLayerMidground   = -1   (the `mid` preset)
kIsoDepthGroundLayer           =  0   (the ground — M2-ISO-01)
kParallaxDepthLayerForeground  = +1   (the `fg` preset)
```

- **WITHIN a shared (atlas, material, blend) group**: the layer field
  dominates the key, so every background-layer quad sorts before
  every ground object and every foreground quad AFTER it, whatever
  the quads' v — background first, engine-guaranteed (the M2-ISO-01
  "layer dominates" contract; the tests pin the ordering against the
  independent M2-ISO-01 oracle).
- **ACROSS groups**: the batcher's group order (ascending (atlas,
  material, blend)) is the draw order — the scene's SET-UP must
  assign the parallax layers' atlas ids so the group order matches
  the depth order: background layers' atlas ids BELOW the world
  content's, foreground layers' ABOVE it (the same convention as the
  M2-TILE-01 texture-id assignment).
- **A layer's own quads** (the wrap split) tile its rectangle without
  overlap: their relative order is the deterministic (key,
  declaration) total order and is visually irrelevant.

## The UV scroll (auto or manual) + the exact wrap

Each layer carries a CURRENT UV OFFSET in [0, 1)² — the texture's
wrap position within its rectangle. Per texture, the sample UV at a
world point `w` in the rectangle is

```
u = frac((w.x - X) / size.x + uvOffset.x)      X, Y = the
v = frac((w.y - Y) / size.y + uvOffset.y)      rectangle's
                                         world origin corner
```

(the M2-SPRITE-02 UV convention: v = 0 is the texture TOP).
Increasing the offset scrolls the texture toward +u (+x world) and +v
(+y world).

- **Manual mode**: the caller drives the offset — `setUvOffset` (any
  finite value; it is WRAPPED to [0, 1)²).
- **Auto mode**: the engine advances the offset by `scrollSpeed` (UV
  units PER FRAME — frames are the presentation pace; frame-rate
  independence is the caller's concern, the M2-CAM-01 lerp
  precedent) on each `advanceScrolls()` call — once per frame, before
  the declarations.

The WRAP is exact at the texture boundary: `wrap(x) = x - floor(x)`
(in [0, 1) for every finite x; 1.0 wraps to exactly 0.0; -0.25 wraps
to exactly 0.75 — dyadic values wrap bit-exactly, which the tests
pin).

**Rendering the wrap through the batcher (the 2 x 2 split):** a
single `SpriteItem` carries ONE UV rect — no wrap — so a scrolled
layer declares its rectangle as the four wrap-aligned quads (fixed
order q00, q10, q01, q11; a quad whose range is empty — uvOffset 0
on that axis — is skipped). With `o = worldOffset(p)`,
`wx = o.x + (1 - uvOffset.x) * size.x`,
`wy = o.y + (1 - uvOffset.y) * size.y`:

| Quad | World rect | Atlas uv (base range) |
|---|---|---|
| q00 | `[o.x, wx) x [o.y, wy)` | `[ox, 1) x [oy, 1)` (always) |
| q10 | `[wx, o.x+sx) x [o.y, wy)` | `[0, ox) x [oy, 1)` (ox > 0) |
| q01 | `[o.x, wx) x [wy, o.y+sy)` | `[ox, 1) x [0, oy)` (oy > 0) |
| q11 | `[wx, o.x+sx) x [wy, o.y+sy)` | `[0, ox) x [0, oy)` (both > 0) |

The world rects tile the rectangle exactly (no gap, no overlap — the
tests pin the areas summing to the rectangle's area), and each quad's
atlas uv is the layer's `uv` sub-rect mapped over its base range (the
atlas sub-rect mapping, the tests pin it).

Each quad declares `pos` = its world center, `scale` = its extent,
`rotation` 0, the full-default tint, `atlasId`/`materialId`/`blend`
from the def, and `depthKey` = the M2-ISO-01 key of the quad's world
center at the def's `depthLayer` (engine-owned, G-R11 —
`depthOverride` stays false).

## Ownership, lifetime, threading

- **Ownership:** one owner — the sim/scene-owner thread (the scene);
  the M2-GL-02 frame pipeline's cull/batch stage owns the render-side
  declaration. Move-only (CORE-009).
- **Storage:** pre-sized at creation (the only allocation: `create` —
  one slot table, 48 B/slot at 8 layers ≈ 384 B default / 3 KB at 64).
- **Phases (CONC-001):** writes (`setLayer`, `setUvOffset`,
  `advanceScrolls`) in the sim/setup phase; reads (`worldOffsetAt`,
  `uvOffsetAt`, `declareTo`) in the render phase; the phases do not
  overlap. Not thread-safe by design.
- **ARCH-009:** the layer state (definitions + the scroll offsets) is
  presentation state — it reads the camera's presentation position,
  never sim state, and is never part of replay state or the
  simulation state hash (headless-buildable — no GL anywhere in this
  API).
- **No silent failure (CORE-008):** every failure path returns the
  `Status`/`Result` (the caller handles and logs); the rejected
  definitions log one rate-limited `parallax/layer_invalid` warn
  (fields `layer`, `field`) — LOG-004; the happy paths log nothing
  (the Status is the failure channel — LOG-002 — and they are
  budget-critical — LOG-003).

## Performance (DOC-004)

| Operation | Cost | Allocations |
|---|---|---|
| `create` | O(maxLayers) — one slot table | setup only |
| `setLayer` | O(1) (validate-before-write + two stores) | none |
| `setUvOffset` | O(1) (the wrap + a store) | none |
| `advanceScrolls` | O(layers) (auto layers only) | none |
| `declareTo` | O(layers x 4) — up to four O(1) batcher adds per layer | none |
| `worldOffsetAt` / `uvOffsetAt` / introspection | O(1) | none |

- **No per-frame allocation** (FR-2.2): the `advanceScrolls` +
  `declareTo` + `build` loop allocates nothing (the adds are O(1)
  operations over the batcher's pre-allocated storage — the
  zero-allocation proof, 1000 frames × 3 layers (up to 4 quads each) +
  2 sprites, the tests).
- **No standalone budget entry**: the per-frame declare cost is PART
  of the composite 50k render-CPU budget (PRD §8.1,
  `sprites_50k_cpu` — M2-PERF-01 measures the reference scene with the
  parallax layers included — the M2-SCENE-01 reference scene has 3
  parallax layers within the 50k-sprite / ≤30-draw-call budget).
- **Draw calls** (FR-2.1, RENDER-001): the batcher's
  (atlas, material, blend) grouping renders each parallax layer in
  one draw call per DISTINCT (atlasId, materialId, blend) of its quads
  — a full-texture un-scrolled layer is ONE quad (one instance); a
  scrolled layer is up to four quads in ONE group (one draw call).
- **Common traps:** forgetting the `advanceScrolls()` per-frame call
  in Auto mode (the offset freezes — no error, no log: the caller
  paces it at the frame's pace); calling `advanceScrolls` more than
  once per frame (the texture scrolls faster than `scrollSpeed`
  says); setting `scrollSpeed` as if it were UV units per SECOND
  (frames are the unit — frame-rate independence is the caller's
  concern); hand-writing the quad's depth key (the key is the
  M2-ISO-01 key of the quad's center at the layer's `depthLayer` —
  G-R11); declaring on a built frame (`beginFrame` first — the
  precondition is checked, first failure wins, nothing declared);
  relying on the ACROSS-group order without following the scene's
  atlas-id convention (background ids below the world content's,
  foreground ids above — above).

## Example (performant) and misuse warning

```cpp
using laige::render::ParallaxLayerDef;
using laige::render::ParallaxLayers;
using laige::sim::Fpx16_16;

// Scene load (sim phase, setup):
ParallaxLayers<Fpx16_16> layers =
    std::move(ParallaxLayers<Fpx16_16>::create({}).valueIfOk().value());
ParallaxLayerDef bg = {};
bg.id = laige::render::kParallaxLayerBackground;
bg.factor = 0.25f;
bg.offset = {1.0f, 2.0f};
bg.size = {2.0f, 2.0f};
bg.atlasId = 0;                     // below the world content's ids
bg.depthLayer = laige::render::kParallaxDepthLayerBackground;
bg.scrollMode = laige::render::ParallaxScrollMode::Auto;
bg.scrollSpeed = {0.02f, 0.0f};    // UV units PER FRAME
layers.setLayer(bg);

// Per frame (the render thread's cull/batch stage):
layers.advanceScrolls();           // once per frame, before the declares
batcher.beginFrame();
layers.declareTo(batcher, camera.position());  // the camera's (x, y)
// ... declare the frame's dynamic sprites ...
batcher.build();                   // the layer quads sort + group with the rest
```

**Misuse warning:** don't hand-write the layer quads' depth — the key
is the M2-ISO-01 key of each quad's world center at the layer's
`depthLayer` (G-R11). Don't skip the scene's atlas-id convention:
within one (atlas, material, blend) group the layer field guarantees
background-first, but ACROSS groups the draw order is the group
order — background layers' atlas ids below the world content's,
foreground layers' above it. Don't treat `scrollSpeed` as per-second
(frames are the unit). A Tilemap-source layer is DATA ONLY in
M2-PAR-01 (M2-TILE-02 declares its tiles — the hook).

## Related

- [`iso_depth_key.md`](iso_depth_key.md) — the key's layer field the
  parallax layer values land in (M2-ISO-01).
- [`sprite_batcher.md`](sprite_batcher.md) — the declare, don't draw
  batcher the layer quads go into (M2-SPRITE-01).
- [`tilemap.md`](tilemap.md) — the Tilemap-source hook (M2-TILE-01;
  the M2-TILE-02 declaration).
- [`sprite_renderer.md`](sprite_renderer.md) — the instanced draw the
  batched groups go through (M2-SPRITE-02).
- [`../concepts/coordinates.md`](../concepts/coordinates.md) §4.10 —
  the canonical narrative (ARCH-008).
- Roadmap: M2-PAR-01 (this), M2-TILE-02 (tilemap-source layers),
  M2-SCENE-01 (the reference scene's 3 parallax layers).
