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
scene config selects the preset (M2-CAM-02; the matrix builders are
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
   land with M2-PAR-01), then the quantized depth;
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
sorting (`isoShearSupported()` is the checker; M2-CAM-02 validates
scene shears). An invertible shear that violates it still renders, but
its depth-key order is not guaranteed.

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
- **Budget**: 10k dirty cells ≤ 0.2 ms mean (PRD §8.1,
  `iso_depthkey_rebuild`) —
  [baselines/m2-iso-depth-table.md](../benchmarks/baselines/m2-iso-depth-table.md).

Tiles are grid-locked (M2-TILE-01), so the table's cells agree with
`isoDepthKey` at the same positions bit-for-bit — and across backends
(dyadic centers in the exactness zone). *Moving* sprites keep the
per-frame `isoDepthKey` (§4.1); the table serves the static tile grid
(the tilemap, M2-TILE-01) only.

## 5. Conversion rules (the module boundaries, RENDER-006)

| Conversion | Direction | Owner | Status |
|---|---|---|---|
| World → depth key | sim state → `uint32` key | `laige-render` (`isoDepthKey`, M2-ISO-01) | **This document / shipped** |
| Tile grid → depth key table | tile heights → precomputed per-tile keys | `laige-render` (`IsoDepthKeyTable`, M2-ISO-02) | **This document / shipped** |
| Depth key → render order | key (+ entity id) → sorted batches | M2-SORT-01 (stable radix sort), M2-SPRITE-01 (batcher) | planned |
| Screen → world (per mode) | picking, screen↔world transforms | `laige-render` (`ProjectionView`: `worldToScreen`, `screenToWorldRay`, `screenToWorld`, M2-PROJ-01), M2-ISO-03 (iso grid picking) | **Shipped (M2-PROJ-01)** / M2-ISO-03 planned |
| World → screen (render) | sim state → NDC → pixels | camera + preset matrix (M2-CAM-01/02, M2-GL-03), `ProjectionView::worldToScreen` (M2-PROJ-01), sprite draw (M2-SPRITE-02) | partially shipped (matrices, camera core M2-CAM-01, world→screen transform M2-PROJ-01) |

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
`(key, entity id)` order (back to front), drawn by the sprite batcher
(M2-SPRITE-02) through the batcher's stable depth sort (M2-SORT-01).
Parallax layers (M2-PAR-01) render background-first via their layer
values; the UI pass (M2-UI-02) is a separate screen-space pass rendered
after all world passes. Determinism of the order is total: same world
state → same keys → same order, every frame (RENDER-003).

## Related

- [`api/iso_depth_key.md`](../api/iso_depth_key.md) — the depth-key API
  contract (M2-ISO-01).
- [`api/projection.md`](../api/projection.md) — the projection modes and
  screen↔world transforms contract (M2-PROJ-01).
- [`api/matrices.md`](../api/matrices.md) — the matrix builders and NDC
  conventions (M2-GL-03).
- [`decisions/0005-iso-default.md`](../decisions/0005-iso-default.md) —
  the 2:1 dimetric default (ADR 0005).
- [`api/presentation.md`](../api/presentation.md) — the interpolated
  world positions the key is computed from (M1-LOOP-02).
- PRD §4 ("What 2.5D means"), §10.1 (module map), §10.3 (determinism),
  FR-2.2, FR-2.5, FR-2.11, AC-4.2/4.4.
