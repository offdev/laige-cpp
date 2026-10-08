# World coordinates, depth convention, and render ordering (ARCH-008)

AGENTS §6 ARCH-008: "Define and document the engine's world axes,
handedness, units, depth convention, render ordering, and conversion
rules." This document is the canonical home for those rules. It was
created with **M2-ISO-01** (isometric depth keys); the earlier interim
homes were the `Vec2`/`Vec3` comments in
[`laige/sim_math.h`](../../src/laige-core/include/laige/sim_math.h) and
the matrices conventions in
[`laige/render/matrices.h`](../../src/laige-render/include/laige/render/matrices.h)
(both remain pinned in code and are restated here for reference).

## 1. World space

- **Axes:** `(x, y)` span the **ground plane**; `+z` is **height/up**.
  The 2.5D world of PRD §4: the simulation is axis-aligned 2D plus a
  depth/height value — physics, collision, pathing, and replicated
  state are all 2D (no oblique simulation, ever).
- **Handedness:** right-handed — `x × y = +z`.
- **Units:** **world units**, dimensionless. One world unit is one tile
  of the tile map (M2-TILE-01 documents the tile map's grid in world
  units). Distances, heights, and camera parameters are all in world
  units.
- **Coordinate domain:** `|x|, |y| ≤ 32767` world units — the Q16.16
  fixed-point bound (`fpx16_16.h`: ±32767.996) so the domain is
  identical on both SimMath backends (ADR 0002). Scene content lives
  well inside this bound; the bound is a world-size guarantee, not a
  per-frame cost.
- **Simulation coordinates are the source of truth.** Render code reads
  them (presentation snapshots, M1-LOOP-02) and never writes them
  (ARCH-009).

## 2. Screen space (NDC)

OpenGL conventions (pinned in
[`matrices.h`](../../src/laige-render/include/laige/render/matrices.h)):
NDC x right, **NDC y up**, NDC z in `[-1, +1]` with the near plane at
`z = -1`. View matrices map world → camera space; projection matrices
map camera → NDC; the isometric matrices and `planeOrtho()` return the
combined world → NDC affine matrix in one step. **NDC is an
intermediate of the render path only** — no game-facing API returns or
accepts NDC (RENDER-006: camera projection must not leak into game
logic).

## 3. The isometric projection family (ADR 0005)

All isometric presets are **affine oblique projections** of the form
(pinned in `matrices.h`; NDC y up):

```
NDC_x = dx.x*x + dy.x*y
NDC_y = dx.y*x + dy.y*y + zUnit*z        (screen_z = 0)
```

with both ground axes projecting **downward** (negative NDC-y
contribution) and a positive `(x + y)` screen-y contribution:

| Preset | dx | dy | zUnit | A |
|---|---|---|---|---|
| 2:1 dimetric (default, ADR 0005) | `(2k, −k)` | `(−2k, −k)` | `k` | `k` |
| True 30°/60° | `(s, −s/√3)` | `(−s, −s/√3)` | `s/√3` | `s/√3` |
| Custom shear | user | user | user | must satisfy §5 |

`A` is the per-world-unit **downward** screen slope of the ground axes
(`A = −dx.y = −dy.y = zUnit` for a depth-key-supported shear). The
scene config selects the preset via the M2-CAM-02 `IsoCamera` preset
config (shipped — `docs/api/iso_camera.md`; the matrix builders are
M2-GL-03's `isoDimetric2To1` / `isoTrueIso3060` / `isoMatrix`).

## 4. The depth convention: the isometric depth key (M2-ISO-01)

**Depth is presentation-only** (PRD §4, ARCH-009). In isometric scenes
the engine owns the z-order: game code never computes it (S-5, G-R11).
The order is a deterministic function of **world state only** — never
screen space, camera state, or zoom (PRD §4):

```
key = f(x, y, z, layer)          (32-bit, uint32, unsigned order)
```

### 4.1 The order value

For an object at ground position `(x, y)` standing on a surface of
tile/step height `z` (world units; the standing surface's elevation —
**not** the object's own sprite height):

```
v(x, y, z) = (x + y) − z
```

Under any depth-key-supported shear, the object's base projects to
`NDC_y = −A·v` with `A > 0`. The painter's back-to-front order is
therefore **ascending v**: smaller v = higher on screen = further from
the camera = drawn first. A tall object is anchored by its **base**:
its sprite height never enters v (the standard isometric painter's
rule — a tree is sorted by its trunk, and the tile in front of it
correctly overdraws the trunk's lower part).

### 4.2 Quantization and the 32-bit layout

`v` is quantized in one rounding (monotone — the key order never
inverts the true v order) and packed with the layer into 32 bits
(constants in `laige/render/iso_depth_key.h`, CORE-005):

```
q = round((x + y) · 16)          round = nearest, ties away from zero
d = q − z · 16                   (quantized depth, 1/16 world units)
key = (layer + 512) << 22  |  (d + 2^21)

bits 31..22  biased layer   10 bits   −512..+511
bits  21..0  biased depth   22 bits   −2^21..+2^21−1
```

- **Exactness:** `keyA < keyB` (same layer) ⇒ `vA ≤ vB` *exactly*
  (ties-away rounding is monotone). `keyA == keyB` ⇒ `|vA − vB| ≤ 1/16`
  world units — the documented precision of the key order (the two
  bases sit on the same screen row to within 1/16 unit; either order
  is visually degenerate).
- **Domain** (documented; the function is total — no per-call asserts
  on the hot path, saturated outside): `|x|, |y| ≤ 32767`
  (`kIsoDepthMaxWorldUnits`, the Q16.16 coordinate bound),
  `|z| ≤ 2047` (`kIsoDepthMaxStepHeight`), `|layer| ≤ 511`
  (`kIsoDepthLayerMax`). Non-finite fp32 input saturates to the domain
  bound (NaN → lower bound); out-of-range values saturate at the
  packing boundary.
- **Exactness zone:** the `(x + y)` contribution is exact on **both**
  backends for `|x + y| ≤ 32767` (the fpx16_16 storage bound is the
  tighter constraint; the fp32 backend is exact to `|x + y| ≤ 65534`).
  Beyond it, each backend's documented saturation applies — the
  saturations are monotone, so the key order is never inverted
  anywhere, it only coarsens at the bound (extreme corner inputs
  collapse to the bound's key; the tie-break applies). No real scene
  approaches the bound (32767 world units ≈ 32 km at 1 m/tile).
- **Backends:** the key is a pure function of the SimMath backend's
  state (ADR 0002 scope per backend; fpx16_16 exact on all platforms —
  it is integer arithmetic). Across backends keys are not promised
  equal (presentation, not replay state — ARCH-009); they agree for
  exactly representable inputs inside the exactness zone (e.g. the
  1/16 grid lattice), which the tests pin.

### 4.3 The total render order (RENDER-003)

The render order is the lexicographic tuple **(key, entity id)**:

1. **key** (this step, M2-ISO-01): layer ascending as the coarse
   primary order (background layers first — the parallax layer values
   are documented, M2-PAR-01, §4.10), then the quantized depth;
2. **entity id**: equal keys keep the deterministic insertion order —
   the stable sort (M2-SORT-01) preserves it and the batcher
   (M2-SPRITE-01) inserts in the engine's deterministic entity-id
   iteration order (FR-1.2). `isoDepthOrderLess(key, id)` in
   `laige/render/iso_depth_key.h` is the comparison.

This realizes the FR-2.2 / roadmap tie tuple "(layer, depth, entity
id)": layer and depth (step height) are packed **into** the key; the
entity id is the final stable tie-break.

### 4.4 Supported iso shears

A shear `(dx, dy, zUnit)` (`IsoAxes`, `matrices.h`) is
**depth-key-supported** iff all components are finite, the ground map
is invertible (`det ≠ 0`), and

```
−dx.y == −dy.y == zUnit > 0        (exact float equality)
```

i.e. `A = C` (both ground axes project downward with the same slope and
the height unit equals that slope). Both built-in presets satisfy it
exactly; a custom shear must satisfy it to use isometric depth
sorting (`isoShearSupported()` is the checker; the M2-CAM-02
`IsoCamera` preset config validates scene shears —
`docs/api/iso_camera.md`). An invertible shear that violates it still
renders, but its depth-key order is not guaranteed.

### 4.5 The depth key table: precomputation and incremental updates (M2-ISO-02)

§4.1's per-sprite key is the *computation*; the scene's static tile
grid has the *precomputed* form (FR-2.2: "precomputed at scene build
and incrementally updated on tile/height changes"):
`IsoDepthKeyTable<Backend>` (`laige/render/iso_depth_table.h`) holds
one depth key per tile of the scene's tile grid, organized in square
chunks (default 16×16 tiles — 256 cells each):

- **Built at scene load** (setup path): one flat, pre-sized storage
  for the covered — chunk-aligned — region (one allocation at
  creation; growth re-allocates once, per growth). Every cell holds
  `{key, qBase, height}` — `qBase` is the backend-quantized ground
  contribution of the cell's *center* (a pure function of position,
  computed once at creation/growth), and `height` is the tile's
  current step height (0 = flat ground on a fresh table).
- **Incremental updates**: `setTile(gx, gy, h)` stores the new height
  and recomputes only the affected cells — the edited cell plus its
  documented neighborhood (`kIsoDepthTableUpdateRadius`; radius 0 for
  the §4.1 formula, which couples a cell's key to the cell's own
  (x, y, height, layer) alone). O(1), zero allocation, no GL calls.
- **Rebuild from scratch** (`rebuild`): the scene-load path; every key
  through the full §4.1 function, so
  `rebuild(final grid) == any edit sequence reaching the same grid`
  (the property the tests pin).
- **Bounded + logged growth** (`ensureChunk`): streamed regions extend
  the covered region chunk by chunk, up to a documented cap
  (`BudgetExhausted` beyond it).
- **Budget**: 10k dirty cells ≤ 0.3 ms mean (PRD §8.1,
  `iso_depthkey_rebuild` — re-baselined 2026-10-04) —
  [baselines/m2-iso-depth-table-budget-rebaseline.md](../benchmarks/baselines/m2-iso-depth-table-budget-rebaseline.md).

Tiles are grid-locked (M2-TILE-01), so the table's cells agree with
`isoDepthKey` at the same positions bit-for-bit — and across backends
(dyadic centers in the exactness zone). *Moving* sprites keep the
per-frame `isoDepthKey` (§4.1); the table serves the static tile grid
(the tilemap, M2-TILE-01) only.

### 4.6 The deterministic depth sort (M2-SORT-01)

§4.3's total render order is realized per frame by
`laige::render::DepthSort` (`laige/render/depth_sort.h`) — the stable,
deterministic, pre-allocated sorter for the frame's 32-bit keys:

- **Pre-allocated, zero per-frame allocation**: `create(capacity)` at
  scene set-up owns one flat storage (16 B/slot); `sort(keys)` per
  frame is O(4n + 4·256) over pre-allocated buffers — no heap, no
  logging, no GL (PERF-003; FR-2.2 "no per-frame allocation").
- **Stable**: equal keys keep their INPUT order. The batcher
  (M2-SPRITE-01) inserts the frame's keys in the deterministic
  entity-id iteration order (FR-1.2), so the sorted output is the
  §4.3 (key, entity id) total order — without the sorter ever seeing
  the ids (the input position IS the entity order). RENDER-003's
  "tie-breaking explicit and stable" is this stability plus the
  insertion order.
- **Algorithm**: 4 × 8-bit LSD radix (stable bucket) passes — one
  stable 256-bucket counting sort per 8-bit digit, least significant
  digit first (Knuth, TAOCP Vol. 3 §7.2.1). Pure integer arithmetic:
  same key sequence → bit-identical sorted order on every platform
  and build (RENDER-003; presentation-only — ARCH-009/010 scope).
- **Failure**: `sort(n > capacity)` → `BudgetExhausted`, the sorter
  unchanged (the previous frame's order is intact); the batcher
  handles and logs it (LOG-002).
- **Budget**: 10k keys sorted, mean ≤ 1.0 ms (PRD §8.1,
  `depth_sort_10k`) —
  [baselines/m2-depth-sort.md](../benchmarks/baselines/m2-depth-sort.md).

### 4.7 The sprite batcher (M2-SPRITE-01)

The frame's sorted keys become the frame's draw groups by
`laige::render::SpriteBatcher` (`laige/render/sprite_batcher.h`) — the
engine-owned "declare, don't draw" window (S-5): the game DECLARES the
frame's sprites (`beginFrame` → `add(item)` × n → `build()`), the
engine batches:

- **Declare, don't draw**: the game declares `SpriteItem`s (world
  position, the M2-ISO-01 depth key, UV sub-rect, rotation, scale,
  tint, blend, atlas/material refs); there is no "draw this quad now"
  in the safe API (S-5).
- **Grouping (FR-2.1)**: `build()` produces one group per DISTINCT
  (atlas, material, blend) combination — the submit stage (M2-SPRITE-02)
  makes ONE instanced draw call per group, so texture binds and blend
  changes are O(group count), not O(sprite count) (RENDER-001).
- **Order (RENDER-003)**: the group order is ascending
  (atlas, material, blend) (a function of the distinct group keys
  alone); each group's instances keep the frame's GLOBAL
  back-to-front order (§4.6's stable sort RESTRICTED to the group) —
  the (key, entity id) total order per group.
- **Overflow (PERF-008, S-2)**: the frame budget is fixed at set-up
  (`Options::maxSprites`); a frame beyond it drops the OLDEST
  declaration + warns (never grows, never silent).
- **G-R11**: a manually-set depth key (`depthOverride`) is counted per
  frame + warned once per frame ("prefer tile height") — the escape
  hatch, not the default path.
- **Zero per-frame allocation** (FR-2.2, PERF-003): the batch is pure
  integer bookkeeping over the pre-allocated storage (~132 B per
  capacity slot — 6.6 MB at the 50k stress budget).

### 4.8 The sprite draw (M2-SPRITE-02)

The frame's groups become pixels by
`laige::render::SpriteRenderer`
(`laige/render/sprite_renderer.h`) — the frame pipeline's SUBMIT
stage: the engine draws the built frame with ONE GPU-instanced draw
call per group (FR-2.1, RENDER-001). The minimal GLSL 3.30 shader
computes, per instance:

- **World position + scale**: `world = pos + corner * scale` — the
  unit quad's corner ([-0.5, 0.5]²) is scaled in WORLD units and
  translated to the world position (the scale is applied BEFORE the
  projection — the `SpriteItem.scale` contract);
- **Projection**: `worldToNdc * world` (the frame's combined camera
  matrix — §5's conversion boundary; the 2D ground plane, z = 0);
- **Rotation IN SCREEN SPACE (NDC)**: the projected offset is rotated
  by the per-instance rotation — the `SpriteItem.rotation` contract
  (rotation is a screen-space property, not a world property);
- **UV sub-rect**: the per-instance UV rect mapped onto the quad
  (`(-0.5,-0.5) → u0/v0`, `(0.5,0.5) → u1/v1`) — for animated sprites
  the rect is `spriteFrameUv`'s output for the caller-set frame index
  under the atlas's sheet layout (M2-SPRITE-03);
- **Tint**: `texture(uAtlas, uv) * tint` (multiplicative RGBA).

The sprites are painted back-to-front (the batcher's §4.7 order) with
the DEPTH TEST DISABLED for the pass — the 2.5D depth is engine-owned
(FR-2.2, §4), never derived from the projection. The per-group state
(one texture bind, one blend function) is set once; the state changes
and the draw submissions are observable (`SpriteDrawStats` /
`SpriteDrawTotals` — the M2-SPRITE-04 profiler feed).

### 4.9 The tilemap: tile quads on the depth table (M2-TILE-01/02)

The scene's static tile grid is
`laige::render::TileMap<Backend>`
(`laige/render/tilemap.h`) — the data + the auto-depth wiring of §4.5
+ the batch path of §4.7 + the tile animation and parallax tile layer
(M2-TILE-02):

- **Chunked grid data** (FR-2.6): the requested grid of tiles, per
  tile a `textureId` (the atlas reference) and an `animationId` (0 =
  a STATIC tile; 1..maxAnimations = the animation slot — M2-TILE-02
  cycles the frame from it), in one flat pre-sized array (8 B/tile).
  The tile's **height lives in the owned `IsoDepthKeyTable` alone**
  (one source of truth): tile `(gx, gy)` is the world cell
  `[gx, gx+1) × [gy, gy+1)` (§5.1's grid at `g = 1`), so the tilemap
  is grid-locked by construction.
- **Auto-depth** (FR-2.6): a tile's Y height is automatically
  reflected in its depth key — `setTile`/`rebuild` route the heights
  into the table (§4.5), which recomputes exactly the affected cell's
  key (radius 0; `rebuild(final grid) == any edit sequence reaching
  the same grid` — the §4.5 property). The game never writes the key
  (G-R11).
- **The batch path** (S-5): `declareTo(batcher, options)` declares
  one `SpriteItem` per tile — the tile's **center** `(gx + 0.5, gy +
  0.5)` (the same point the table quantizes), scale (1, 1) (the unit
  quad spans the tile's cell), rotation 0, the tile's **current frame**
  UV (the static tile's fixed frame — default the full tile texture —
  or the animation's frame UV, M2-TILE-02) + `frameIndex` (the
  M2-SPRITE-03 hook), the table's key (auto-depth), the tile's
  texture as `atlasId` — in the grid's row-major order (the tile's
  grid position is a static tile's stable identity, the §4.6
  insertion-order analog; RENDER-003).
- **Tile animation** (M2-TILE-02): an ANIMATION is a slot of the
  tilemap's pre-sized animation table — the frame COUNT, the
  documented TICK RATE (`frameTicks` — the sim ticks per frame), and
  the tile sheet's frame layout (the M2-SPRITE-03 layout). The scene
  owner calls `advanceAnimations()` ONCE PER SIM TICK (ARCH-002 —
  per sim tick, never per render frame): every set animation's frame
  steps every `frameTicks` ticks, wrapping at `frameCount`
  (`frame(ticks) = (ticks / frameTicks) mod frameCount`). All tiles of
  one animation share its phase (per-tile offsets are the M3 editor's
  control). The frame UVs are precomputed at `setAnimation` (the setup
  allocation); the per-frame path only reads them (no per-frame
  allocation, FR-2.2). The frame state is presentation state
  (ARCH-009).
- **Bounded draw calls** (FR-2.1, RENDER-001): the batcher's
  (atlas, material, blend) grouping renders the tilemap in one draw
  call per DISTINCT (textureId, material, blend) combination — tiles
  of one chunk sharing one texture and blend form ONE group (one
  draw call per chunk group); the count is a function of the distinct
  group keys, never of the tile count.
- **The parallax tile layer** (M2-TILE-02, the M2-PAR-01 hook): a
  parallax layer with the `Tilemap` source declares the tilemap's
  quads UNDER the layer (`declareTo` overload): every quad is
  TRANSLATED by the layer's `worldOffset(cameraPos)` (§4.10), and its
  key is the §4.1 key of the translated center at the TILEMAP's own
  `Options::layer` (the scene-setup convention: the layer's def
  `depthLayer` equals it — bg/mid/fg tilemaps get -2/-1/+1). The
  translated keys are computed per tile per frame (the camera-
  dependent translation is not precomputable — O(tileCount), zero
  allocation).

### 4.10 The parallax layers: the named background/midground/foreground model (M2-PAR-01)

The scene's parallax layers are
`laige::render::ParallaxLayers<Backend>`
(`laige/render/parallax.h`) — the named bg/mid/fg model of FR-2.3,
declared into the batcher of §4.7 (S-5) with the §4.1 keys:

- **The offset formula** (world space only — PRD §4, RENDER-006): a
  layer's content position at camera position `p` (the camera's
  ground-plane (x, y), the M2-CAM-01 presentation position) is
  `worldOffset(p) = factor * (p - center) + offset` — the EXACT
  formula (the tests pin it bit-exactly for dyadic values).
  `factor` in [0, 1]: 0 = fixed in world space (maximum parallax, a
  close-by background), 1 = fixed on screen (no parallax, the sky
  layer); `center` = the reference camera position (default the world
  origin); `offset` = the world-space offset at `p = center`.
- **The depth layer values** (the §4.1 layer field, engine-owned —
  G-R11): `kParallaxDepthLayerBackground` = -2 (the `bg` preset),
  `kParallaxDepthLayerMidground` = -1 (the `mid` preset),
  `kIsoDepthGroundLayer` = 0 (the ground),
  `kParallaxDepthLayerForeground` = +1 (the `fg` preset); a custom
  layer picks any value in the M2-ISO-01 domain [-512, +511]
  (more negative = further back).
- **Background first** (the documented render order): WITHIN a shared
  (atlas, material, blend) group the layer field dominates the key —
  every background-layer quad sorts before every ground object and
  every foreground quad after it, whatever the quads' v
  (engine-guaranteed, the §4.1 "layer dominates" contract). ACROSS
  groups the draw order is the batcher's group order (ascending
  (atlas, material, blend), §4.7) — the scene's SET-UP assigns the
  parallax layers' atlas ids so the group order matches the depth
  order: background ids BELOW the world content's, foreground ids
  ABOVE it (the M2-TILE-01 texture-id convention).
- **The UV scroll** (auto or manual): each layer carries a CURRENT UV
  OFFSET in [0, 1)²; Auto advances it by `scrollSpeed` (UV units PER
  FRAME) on `advanceScrolls()` — once per frame, before the
  declarations — and the wrap is EXACT at the texture boundary
  (`wrap(x) = x - floor(x)`: 1.0 → exactly 0.0). A scrolled layer
  renders through the 2 x 2 wrap split (up to four quads — one
  SpriteItem carries one UV rect, no wrap); the quad's key is the
  §4.1 key of the quad's world center at the layer's `depthLayer`.
- **Tilemap-source layers** (M2-TILE-02): a layer's `source = Tilemap`
  names a tilemap; the tilemap declares its tiles UNDER the layer
  (the `TileMap::declareTo` overload, §4.9) — the quads translated by
  this layer's `worldOffset`, their keys at the tilemap's own
  `Options::layer` (the scene-setup convention: this def's
  `depthLayer` must equal it). This registry's own `declareTo` skips
  Tilemap-source layers (no log — the game declares the tiles through
  the tilemap's path, not this one); its `size`/`uv` fields are not
  validated.

### 4.11 The particles: the CPU-sim particle state (M2-PART-01)

The particle state (`ParticleSystem<Backend>::Particle`, the
[particle simulation](../api/particles.md) step) lives in SIM space:
each particle carries a 2D position `(x, y)` + a CONSTANT depth value
+ a per-tick velocity + a life (sim ticks) + a size. The per-tick
advance (`age += 1`, `pos += vel` — one SimMath vector add, ADR
0002) runs in the simulation tick (ARCH-002); the render path reads
the state read-only (`liveParticles()` — the M2-GL-02 cull/batch
stage). The particle's depth value is the §4.1 key input the
rendering half
([`particle_render.md`](../api/particle_render.md), M2-PART-02)
feeds the batcher (particles render as batched sprites — one draw
call per emitter set, RENDER-001; the depth is quantized to an
integer step height first, then packed as the key). The state is
deterministic
(ARCH-010): a pure function of (seed, emitter definitions, operation
sequence) — fpx16_16 bit-identical across platforms/builds,
fp32_pinned same-build/same-platform.

## 5. Conversion rules (the module boundaries, RENDER-006)

| Conversion | Direction | Owner | Status |
|---|---|---|---|
| World → depth key | sim state → `uint32` key | `laige-render` (`isoDepthKey`, M2-ISO-01) | **This document / shipped** |
| Tile grid → depth key table | tile heights → precomputed per-tile keys | `laige-render` (`IsoDepthKeyTable`, M2-ISO-02) | **This document / shipped** |
| Depth key → render order | key (+ entity id) → sorted order → groups | `laige-render` (`DepthSort`, M2-SORT-01 stable radix sort; `SpriteBatcher`, M2-SPRITE-01 batcher) | **Shipped (M2-SORT-01 + M2-SPRITE-01)** |
| Screen → world (per mode) | picking, screen↔world transforms | `laige-render` (`ProjectionView`: `worldToScreen`, `screenToWorldRay`, `screenToWorld`, M2-PROJ-01; `screenToGrid` iso grid picking, M2-ISO-03) | **Shipped (M2-PROJ-01 + M2-ISO-03)** |
| World → screen (render) | sim state → NDC → pixels | camera + preset matrix (M2-CAM-01/02, M2-GL-03), `ProjectionView::worldToScreen` (M2-PROJ-01), sprite draw (M2-SPRITE-02) | **Shipped** (matrices + camera core M2-CAM-01, iso presets + grid-snap M2-CAM-02, world→screen transform M2-PROJ-01, pixels: `SpriteRenderer::submit`'s offscreen instanced draw M2-SPRITE-02) |
| Atlas frame → UV sub-rect | animation frame index + sheet layout → UV rect | `laige-render` (`spriteFrameUv`, M2-SPRITE-03) | **Shipped (M2-SPRITE-03)** |
| Tile grid → tile quads | tile data + table keys (+ the animation's frame state; the parallax layer's `worldOffset`) → declared sprites | `laige-render` (`TileMap::declareTo` — static + animated frames, the parallax tile layer overload, M2-TILE-01/02) | **Shipped (M2-TILE-01 + M2-TILE-02)** |
| Camera position → parallax offset | camera (x, y) + layer def → world-space offset (the exact formula) + wrap quads | `laige-render` (`ParallaxLayers::worldOffsetAt` / `declareTo`, M2-PAR-01) | **Shipped (M2-PAR-01)** |
| Particle state → sprite items | particle (pos, depth, size, faded tint) → declared sprites (one batch per emitter set, the key from the particle's depth) | `laige-render` (`declareParticles` / `particleDepthToStepHeight` — consumes `ParticleSystem<Backend>::liveParticles()`, read-only after the sim phase, M2-PART-02) | **Shipped (M2-PART-02)** |

### 5.1 The isometric grid picking (M2-ISO-03)

The safe screen → grid-cell transform (FR-2.11; PRD §4's
click-to-select / click-to-move): `screenToGrid(screen, camera, grid)`
in [`laige/render/iso_picking.h`](../../src/laige-render/include/laige/render/iso_picking.h)
— engine-owned (S-5, G-R11: game code never inverts the iso matrix
itself). The screen point is **NDC** (the projection.h convention —
the documented pixel ↔ NDC conversion is the input boundary,
RENDER-006); the grid plane is the ground plane (z = 0). The inverse
is the O(1) 2×2 solve on the frame matrix's ground rows (no per-pick
4×4 inverse):

```
det = a·d − b·c
w.x = (d·(ndc.x − tx) − b·(ndc.y − ty)) / det
w.y = (a·(ndc.y − ty) − c·(ndc.x − tx)) / det
```

- **The grid cells are half-open** (the documented boundary rule):
  cell `(gx, gy)` is `[gx·g, (gx+1)·g) × [gy·g, (gy+1)·g)` — i.e.
  `gx = floor(w.x / g)`. A point exactly on a cell's lower or left
  boundary belongs to THAT cell; exactly on its upper or right
  boundary to the cell beyond it; a corner to the cell to its upper
  right. The grid is anchored at the world origin: the tile map's
  tile `(gx, gy)` (M2-TILE-01) is exactly this cell at `g = 1`, with
  center `(gx + 0.5, gy + 0.5)` — the grid the M2-CAM-02 grid-snap
  camera locks to.
- **Exact at all supported zoom levels:** the inverse is a fixed
  sequence of float ops — zoom enters only through the matrix's
  entries — so the same stored screen point resolves to the same
  cell at every zoom the camera supports (the continuous
  `[zoomMin, zoomMax]` range without snap, the dyadic ladder with
  snap).
- **Precision (the boundary zone):** the computed ground point
  `ŵ` is within `16·2⁻²⁴·κ·(|e| + |w|)` world units of the exact
  ground point (`κ` = the ∞-norm condition number of the ground 2×2:
  4.5 for 2:1 dimetric, ~2.73 for 30/60 — constants in
  `iso_picking.h`). A point is guaranteed to resolve to its exact
  cell unless its exact ground coordinate lies within that bound of a
  cell boundary; inside the zone either of the two adjacent cells may
  be returned (the float rounding decides — deterministic per build).
  Domain-worst zone: `kIsoPickDomainBoundaryEps` (~4 world units at
  `|e| = |w| = 32767`, `κ` ≤ 64).
- **Total function:** non-finite screen input saturates at the
  documented world domain (±32767; NaN → lower bound — the
  isoDepthKey convention) and the result cell fits int32 for every
  valid cell size (`g >= kIsoPickMinCellSize = 1e-4`). A stopped
  camera picks with the identity matrix (degenerate but total).
- **Budget:** one pick mean ≤ 0.01 ms (PRD §8.1, `iso_picking`) —
  [baselines/m2-iso-picking.md](../benchmarks/baselines/m2-iso-picking.md).

Rules:

- **Sim never sees projection.** No projection mode, camera, or screen
  quantity appears in `laige-sim`/`laige-net` public surface
  (AC-4.2; the include-graph lint landed with M2-PROJ-01 — rule R2
  blocks every `laige-sim` → `laige-render` include, fixture-tested by
  the `include-lint-sim-to-render` CTest entry; the companion surface
  check lands with M2-AC-01).
- **Render reads sim state, never writes it** (ARCH-009): the depth
  key is computed from the presentation snapshot's interpolated
  position (M1-LOOP-02) — a read-only boundary.
- **The key is world-space; the screen is a projection of the world.**
  Deriving ordering from screen coordinates is the misuse this design
  exists to prevent (PRD §4; G-R11).

## 6. Render ordering (summary)

For one isometric frame, objects render in ascending
`(key, entity id)` order (back to front), batched by the sprite
batcher (M2-SPRITE-01) and drawn by the sprite renderer (M2-SPRITE-02)
through the engine's stable depth sort (`DepthSort`, §4.6,
M2-SORT-01).
Parallax layers (M2-PAR-01, §4.10) render background-first via their
depth-layer values (the layer field dominates within a group; the
scene's atlas-id convention orders the groups); the UI pass
(M2-UI-02) is a separate screen-space pass rendered after all world
passes. Determinism of the order is total: same world
state → same keys → same order, every frame (RENDER-003).

## Related

- [`api/iso_depth_key.md`](../api/iso_depth_key.md) — the depth-key API
  contract (M2-ISO-01).
- [`api/projection.md`](../api/projection.md) — the projection modes and
  screen↔world transforms contract (M2-PROJ-01).
- [`api/iso_picking.md`](../api/iso_picking.md) — the isometric grid
  picking contract: `screenToGrid` (screen → ground → grid cell)
  (M2-ISO-03).
- [`api/depth_sort.md`](../api/depth_sort.md) — the deterministic
  stable depth sort contract: `DepthSort` (keys → sorted order)
  (M2-SORT-01).
- [`api/sprite_batcher.md`](../api/sprite_batcher.md) — the declare,
  don't draw batcher contract: `SpriteBatcher` (declared sprites →
  (atlas, material, blend) groups) (M2-SPRITE-01).
- [`api/sprite_renderer.md`](../api/sprite_renderer.md) — the instanced
  draw contract: `SpriteRenderer` (groups → one instanced draw call
  each, the frame's pixels) (M2-SPRITE-02).
- [`api/sprite_frames.md`](../api/sprite_frames.md) — the atlas UV
  frame animation hook: `SpriteFrameLayout` + `spriteFrameUv`
  (frame index + sheet layout → the item's UV sub-rect) (M2-SPRITE-03).
- [`api/tilemap.md`](../api/tilemap.md) — the tilemap contract:
  `TileMap` (chunked tile grid + the auto-depth wiring of the depth
  table + the static tile-quad batch path) (M2-TILE-01).
- [`api/parallax.md`](../api/parallax.md) — the parallax layer
  contract: `ParallaxLayers` (the named bg/mid/fg model — the offset
  formula, the depth-layer values, the UV scroll + exact wrap, the
  2 x 2 wrap-split batch path) (M2-PAR-01).
- [`api/particles.md`](../api/particles.md) — the CPU particle
  simulation contract: `ParticleSystem` (the bounded pool, the
  burst + continuous emitters, the per-tick advance, the exact
  integer fade, the determinism contract) (M2-PART-01).
- [`api/particle_render.md`](../api/particle_render.md) — the
  particle rendering contract: `declareParticles` (the O(n)
  particle → sprite pass — the engine-owned depth key from the
  particle's depth, the exact fade through the tint, one draw call
  per emitter set) (M2-PART-02).
- [`api/matrices.md`](../api/matrices.md) — the matrix builders and NDC
  conventions (M2-GL-03).
- [`decisions/0005-iso-default.md`](../decisions/0005-iso-default.md) —
  the 2:1 dimetric default (ADR 0005).
- [`api/presentation.md`](../api/presentation.md) — the interpolated
  world positions the key is computed from (M1-LOOP-02).
- PRD §4 ("What 2.5D means"), §10.1 (module map), §10.3 (determinism),
  FR-2.2, FR-2.5, FR-2.11, AC-4.2/4.4.
