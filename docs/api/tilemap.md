# Tilemap (`laige::render` tilemap)

The chunked tile grid data, the auto-depth wiring of the M2-ISO-02
depth key table, the static tile-quad batch path into the sprite
batcher (M2-TILE-01), the data-driven tile animation frame cycle and
the parallax tile layer declaration (M2-TILE-02). PRD §15 M2,
FR-2.6 — "Tilemap: chunks, per-tile depth/height, auto-depth",
"tile animation", "parallax tile layers" — §9.1 S-5, ARCH-009, G-R11;
AGENTS RENDER-001/003, CORE-002/004/005/008, PERF-002/003; ADR 0002.
Public header:
`src/laige-render/include/laige/render/tilemap.h` (header-only — the
API is a template over the SimMath backends, the
[`presentation.h`](../src/laige-sim/include/laige/sim/presentation.h)
pattern). Unit suites: `ctest -R tilemap`
(`tests/laige-render/tilemap_tests.cpp`) and `ctest -R tilemap_anim`
(`tests/laige-render/tilemap_anim_tests.cpp`) — pure data + batcher
bookkeeping, no GL environment required: they run in every local tree
and in CI, and the goldens are hand-computed from the documented
formula.

The canonical narrative home is
[`docs/concepts/coordinates.md`](../concepts/coordinates.md) §4.9
(ARCH-008); the header preamble carries the machine-checked contract.
The per-tile key this tilemap precomputes is
[`iso_depth_table.md`](iso_depth_table.md) (M2-ISO-02) on top of
[`iso_depth_key.md`](iso_depth_key.md) (M2-ISO-01); the batch path
declares into [`sprite_batcher.md`](sprite_batcher.md) (M2-SPRITE-01);
the frame UVs come from [`sprite_frames.md`](sprite_frames.md)
(M2-SPRITE-03); the parallax tile layer consumes
[`parallax.md`](parallax.md) (M2-PAR-01).

## The API

| Member | Returns | Notes |
|---|---|---|
| `TileMap<Backend>::create(options)` | `Result<TileMap>` | validates `maxAnimations` first, then the table's create (first failure wins); pre-sizes the table chunks + the 8 B/tile data array + the animation slot array; flat/empty init (setup path) |
| `setTile(gx, gy, textureId, height, animationId)` | `Status` | the per-tile edit: stores the tile data, routes the height into the table's `setTile` — the auto-depth update (update path) |
| `rebuild(tiles)` | `Status` | scene load: stores the whole grid (the data + the heights through the table's from-scratch rebuild; setup path) |
| `setAnimation(id, def)` | `Status` | sets/replaces the animation slot `id` (setup / config path): frame count, tick rate, sheet layout; resets the phase; precomputes the frame UVs (one setup allocation) |
| `advanceAnimations()` | `void` | the sim phase's per-tick call (ONCE PER SIM TICK): advances every set animation's frame at its documented rate (O(maxAnimations), zero allocation) |
| `declareTo(batcher, options)` | `Status` | the render path: declares the tile quads (static fixed frames + animated frame UVs) into the batcher's frame window (the batch path) |
| `declareTo(batcher, options, layers, layerId, cameraPos)` | `Status` | the parallax tile layer path: the same quads TRANSLATED by the layer's `worldOffset(cameraPos)`, the keys at the translated centers at the tilemap's own `layer` (M2-PAR-01 hook) |
| `tileAt(gx, gy)` | `TileData` | the tile's data: `{textureId, height (the table's), animationId}` (render read path; O(1)) |
| `depthKeyAt(gx, gy)` | `uint32_t` | the tile's current depth key (the table's — auto-depth; O(1)) |
| `tileHeightAt(gx, gy)` | `int32_t` | the tile's last stored height (diagnostics view — DBG-008) |
| `covers(gx, gy)` | `bool` | whether the tile lies in the requested grid (O(1)) |
| `hasAnimation(id)` / `animationAt(id)` / `animationFrame(id)` | `bool` / `const TileAnimationDef&` / `uint32_t` | the animation introspection (the declare path's check; the debug view — DBG-008) |
| introspection | `int32_t` / `size_t` / `uint32_t` | `originTileX/Y()`, `widthTiles()`, `heightTiles()`, `layer()`, `chunkTiles()`, `maxAnimations()`, `tileCount()` |

`TileMap::Options` extends the table's grid options with
`maxAnimations` (default `kTileMapDefaultAnimations` = 8; the domain
[1, `kTileMapMaxAnimations` = 256] — the slot count, validated FIRST,
then the table's create validates the grid options): `originTileX/Y`
(default 0), `widthTiles` / `heightTiles` (required ≥ 1), `chunkTiles`
(default `kIsoDepthTableChunkTiles` = 16; a power of two ≥ 1),
`maxChunks` (default `kIsoDepthTableDefaultMaxChunks` = 1024), `layer`
(default `kIsoDepthGroundLayer` = 0 — a parallax tile LAYER gets its
own tilemap with its layer value, M2-PAR-01 — the values are
documented in [`parallax.md`](parallax.md)). `TileData` is a plain
value (8 B of data + the table's height — the value the game writes on
load/edit and reads back). `TileAnimationDef` is a plain value
(`frameCount`, `frameTicks`, the [`SpriteFrameLayout`](sprite_frames.md)
of the tile sheet).

## The tile model

One tile per cell of the **requested grid** `[originTileX,
originTileX + widthTiles) × [originTileY, originTileY + heightTiles)`:
tile `(gx, gy)` is the world cell `[gx, gx+1) × [gy, gy+1)` at the
world origin (coordinates.md §5.1 — the grid the M2-CAM-02 grid-snap
camera and the M2-ISO-03 picker resolve to), so the tilemap is
grid-locked by construction. The per-tile data (`textureId`,
`animationId`) lives in one flat pre-sized array (row-major, tileX
fastest — 8 B/tile, 400 KB at 50k tiles); the tile's **height lives in
the table alone** (one source of truth):

- `setTile` routes the height into the table's `setTile` — the
  auto-depth wiring (FR-2.6): the table stores the height and
  recomputes exactly that cell's key (the M2-ISO-02 incremental
  update, radius 0 — a height edit changes exactly that tile's key).
  O(1), zero allocation.
- `rebuild` maps the requested heights into the table's covered
  rectangle (the chunk-aligned superset — the margin stays flat
  ground) and calls the table's from-scratch `rebuild` (every key
  through the full M2-ISO-01 function). Property:
  `rebuild(final grid) == any sequence of setTile calls reaching the
  same grid` (the M2-ISO-02 property, pinned through the tilemap).
  The superset margin (up to `chunkTiles − 1` tiles past a requested
  edge) carries real table cells — flat unless the scene sets them —
  but they are **not tiles**: the tilemap's `covers()`/data/declare
  path never touch them.
- The data write happens AFTER the depth edit succeeded (both in
  `setTile` and `rebuild` — heights + animationIds validated over the
  whole span before any write): a rejected operation leaves the tile
  data AND the table unchanged; the returned `Status` is the failure
  channel the caller handles and logs (LOG-002).

## Tile animation (M2-TILE-02 — the data-driven frame cycle)

An ANIMATION is a slot of the tilemap's pre-sized animation table (ids
1..`maxAnimations`; **0 is the static sentinel** — the tile's UV is
the fixed frame). The scene SETS an animation (`setAnimation`, the
setup / config path — the parallax `setLayer` precedent): the frame
COUNT (`frameCount`, domain [1, `kTileAnimMaxFrames` = 64]), the
documented TICK RATE (`frameTicks` — the SIMULATION ticks per frame,
≥ 1), and the tile SHEET's frame layout (the M2-SPRITE-03
`SpriteFrameLayout`, texels — the sheet is its TIGHT sheet; a padded
atlas is the M3-ASSET-01 asset system's concern).

The frame CYCLE is the engine's per-tick advance: the scene owner
calls `advanceAnimations()` **once per sim tick** (the sim phase —
ARCH-002: the rate is per SIM tick, never per render frame; the cycle
is frame-rate-independent). Every set animation advances one tick per
call, and its frame steps forward every `frameTicks` ticks:

```
frame(ticks) = (ticks / frameTicks) mod frameCount
```

The cycle WRAPS (`frameCount − 1 → 0`); the cycled frames are the
sheet's FIRST `frameCount` frames, row-major (frame 0 = the sheet's
top-left, the M2-SPRITE-03 layout). A (re)set RESETS the phase (frame
0, tick 0). All tiles of one animation share its phase (per-tile phase
offsets are the M3 animation editor's control — the roadmap's
"data-driven" scope: the editor authors the defs, the engine cycles
them). The frame's UV is PRECOMPUTED at `setAnimation` (one
`spriteFrameUv` per frame — the setup allocation); the per-frame
declare path only READS the stored UV + frame (zero allocation, no
division — FR-2.2). The animation frame state is PRESENTATION state
(ARCH-009) — never part of replay state or the simulation state hash
(the parallax scroll-offset precedent).

The declared quad's frame fields (both declare paths): the **static**
tile carries `DeclareOptions::uv` (default (0, 0, 1, 1) = the full
tile texture) and `frameIndex` 0; the **animated** tile carries its
animation's CURRENT frame UV (the precomputed rect) and
`frameIndex` = the current frame (the M2-SPRITE-03 hook — the
batcher carries it untouched, the M3 animation renders from it).

## `declareTo` — the tile-quad batch path (S-5)

The frame pipeline's cull/batch stage calls `declareTo(batcher,
options)` once per frame per tilemap: one `SpriteItem` per tile of
the requested grid, in the grid's **row-major order** (tileY outer,
tileX fastest — the tile's grid position is a static tile's stable
identity, the FR-1.2 entity-order analog; the insertion order the
M2-SORT-01 stable sort turns into the (key, insertion position) total
order — RENDER-003). Each quad (the header preamble):

- `pos` = the tile's **center** `(gx + 0.5, gy + 0.5)` (the same point
  the table quantizes; the float conversion is exact — dyadic,
  `|gx| ≤ 32766`);
- `scale` = (1, 1) — the unit quad spans the tile's world cell;
- `rotation` = 0; `uv` = the tile's **current frame**: the static
  `DeclareOptions::uv` fixed frame, or the animation's frame UV
  (above);
- `depthKey` = the table's key for the cell (**auto-depth** — the
  game never computes it, G-R11; `depthOverride` stays false);
- `atlasId` = the tile's `textureId`; `materialId`/`blend` = the
  options'.

**Bounded draw calls** (FR-2.1, RENDER-001): the batcher's
(atlas, material, blend) grouping renders the tilemap in one draw
call per DISTINCT (textureId, material, blend) combination — tiles of
one chunk sharing one texture and blend form ONE group (one draw call
per chunk group); the draw-call count is a function of the distinct
group keys, never of the tile count.

Precondition: the batcher's frame window is open (a built frame's
window is closed — call `beginFrame` first; the check is in
`declareTo`, first failure wins, nothing declared on failure). An
animated tile whose animation slot is UNSET fails the declare (first
failure wins — nothing declared past it this frame; the slot's set is
the scene's setup responsibility). On a stopped batcher the first add
fails (`BudgetExhausted`) — nothing declared. The WHOLE requested
grid is declared (visible-rect culling lands with M2-PERF-01 — the
composite 50k budget's worst case is the full grid).

## The parallax tile layer (M2-TILE-02 — the M2-PAR-01 hook)

A parallax layer with the `Tilemap` source (the M2-PAR-01 registry)
names a tilemap; the tilemap declares its quads UNDER that layer:
`declareTo(batcher, options, layers, layerId, cameraPos)`. Every tile
quad is TRANSLATED by the layer's `worldOffset(cameraPos)` (the
M2-PAR-01 formula (1) — the layer's factor/center/offset against the
camera's presentation position, world space, RENDER-006), and its
depth key is the M2-ISO-01 key of the **translated** tile center at
the tilemap's own `layer` (`table_.layer()` — one source of truth).
**Scene-setup convention:** the layer's def `depthLayer` must equal
the tilemap's `Options::layer` (the bg/mid/fg tilemaps get layer
values −2/−1/+1, [`parallax.md`](parallax.md)); the declaration uses
the tilemap's value. The table's keys are at the untranslated
positions, so the translated keys are computed per tile per frame
(O(tileCount) `isoDepthKey` calls, zero allocation, no GL — part of
the composite 50k budget; the qBase + qOffset derivation is the
documented upgrade path if a profile ever shows it matters). The
animated/static frame handling is the same as `declareTo`.

Protocol (first failure wins, nothing declared, no log): a built
frame's closed window, an unset layer id, and a non-Tilemap-source
layer are `InvalidArgument` (the setup mispairing is the caller's); a
**disabled** layer declares NOTHING (OK, no log — the layer's
documented skip).

## Ownership, lifetime, threading

- **Ownership:** one owner — the sim/scene-owner thread (the scene);
  the M2-GL-02 frame pipeline's cull/batch stage owns the render-side
  declaration. Move-only (CORE-009).
- **Storage:** pre-sized at creation (the only allocations: `create` —
  one table storage + one 8 B/tile data array + the animation slot
  array — `rebuild`'s setup-path temporary, and `setAnimation`'s
  per-frame-UV array — the setup/config path, never the per-frame
  path).
- **Phases (CONC-001):** writes (`setTile`, `rebuild`,
  `setAnimation`) in the sim/config phase; the per-tick
  `advanceAnimations` in the sim phase (it advances the presentation
  frame state); reads (`tileAt`, `depthKeyAt`, `tileHeightAt`,
  `covers`, `declareTo`) in the render phase; the phases do not
  overlap (the table's contract). Not thread-safe by design.
- **ARCH-009:** the tile data and the animation frame state are
  presentation-side (headless-buildable — no GL anywhere in this
  API); the render phase consumes them read-only.
- **No silent failure (CORE-008):** every failure path returns the
  `Status`/`Result` (the caller handles and logs); the happy
  update/advance/declare paths log nothing (the Status is the failure
  channel — LOG-002 — and they are budget-critical — LOG-003).

## Performance (DOC-004)

| Operation | Cost | Allocations |
|---|---|---|
| `create` | O(covered cells) — the table's create + data array + animation slots | setup only |
| `setTile` | O(1) (the table's `setTile`, radius 0 + two stores) | none |
| `rebuild` | O(covered cells) (the table's from-scratch rebuild) | one setup temporary |
| `setAnimation` | O(frameCount) — the frame-UV precomputation | one setup array |
| `advanceAnimations` | O(maxAnimations) — a counter per set animation | none |
| `declareTo` | O(tileCount) — one O(1) batcher add per tile | none |
| `declareTo` (parallax) | O(tileCount) — the add + one `isoDepthKey` per tile | none |
| `tileAt` / `depthKeyAt` / `tileHeightAt` / `covers` | O(1) | none |

- **No per-frame allocation** (FR-2.2): the per-tick `advanceAnimations`
  and the per-frame `declareTo` loops (standalone + parallax) allocate
  nothing (the frame UVs are precomputed at `setAnimation`, the slot
  table at `create` — the zero-allocation proof, 1000 frames ×
  256 tiles + 64 parallax tiles, the tests).
- **No standalone budget entry**: the per-frame declare + per-tick
  advance cost is PART of the composite 50k render-CPU budget (PRD
  §8.1, `sprites_50k_cpu` — M2-PERF-01 measures the reference scene
  with tile quads included).
- **Budget-critical:** the `setTile`/`rebuild`/`declareTo`/
  `advanceAnimations` paths make no GL calls, no logging, no per-call
  allocation (the `iso_depthkey_rebuild` budget's per-update cost —
  the table's M2-ISO-02 budget — bounds the depth half of `setTile`).
- **Common traps:** declaring more than the batcher's frame budget
  (the frame overflow policy applies — drop the oldest + warn);
  recreating the tilemap per frame (the table IS the precomputation —
  FR-2.2 "not recomputed per frame"); reading `tileAt`/`depthKeyAt`
  on a tile outside the requested grid (UB — check `covers()` for
  untrusted input; the batch path reads only the requested grid by
  construction); calling `advanceAnimations` per RENDER frame
  instead of per sim tick (the animation speed changes — ARCH-002).

## Example (performant) and misuse warning

```cpp
using laige::render::TileMap;
using laige::render::TileData;
using laige::render::TileAnimationDef;
using laige::sim::Fpx16_16;

// Scene load (sim phase, setup):
TileMap<Fpx16_16>::Options o;
o.originTileX = 0; o.originTileY = 0;
o.widthTiles = 64; o.heightTiles = 64;
auto r = TileMap<Fpx16_16>::create(o);
auto map = std::move(r).takeValue();
std::vector<TileData> tiles = loadTiles();  // the game's format
map.rebuild(tiles);   // the heights go into the depth table (auto-depth)

// One tile animation (4 frames, every 2 sim ticks, a 2x2 grid of 16x16 frames):
TileAnimationDef anim;
anim.frameCount = 4; anim.frameTicks = 2;
anim.layout.frameWidth = 16; anim.layout.frameHeight = 16;
anim.layout.columns = 2; anim.layout.rows = 2;
map.setAnimation(1, anim);   // the tiles with animationId 1 cycle from it

// Per sim tick:
map.advanceAnimations();     // ONCE PER SIM TICK (ARCH-002)

// Per frame (the render thread's cull/batch stage):
batcher.beginFrame();
map.declareTo(batcher, TileMap<Fpx16_16>::DeclareOptions{});
// ... declare the frame's dynamic sprites ...
batcher.build();      // the tile quads sort + group with the rest
```

**Misuse warning:** don't hand-write the tile's depth — the key is the
table's (auto-depth, G-R11); setting `depthOverride` on a tile sprite
defeats the wiring (and counts/warns through the batcher). Don't
declare on a built frame (`beginFrame` first — the precondition is
checked). `animationId` is the engine's frame-cycle reference: 0 =
static (the fixed frame), 1..maxAnimations = the animation slot (the
batch path CONSUMES it — M2-TILE-02); the slot's set is the scene's
setup responsibility (an animated tile whose slot is unset fails the
declare). A parallax tile LAYER's def `depthLayer` must equal the
tilemap's `Options::layer` (the scene-setup convention — the
declaration uses the tilemap's).
