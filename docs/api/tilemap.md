# Tilemap (`laige::render` tilemap)

The chunked tile grid data, the auto-depth wiring of the M2-ISO-02
depth key table, and the static tile-quad batch path into the sprite
batcher (M2-TILE-01; PRD §15 M2, FR-2.6 — "Tilemap: chunks, per-tile
depth/height, auto-depth", §9.1 S-5, ARCH-009, G-R11; AGENTS RENDER-001/
003, CORE-002/004/005/008, PERF-002/003; ADR 0002). Public header:
`src/laige-render/include/laige/render/tilemap.h` (header-only — the
API is a template over the SimMath backends, the
[`presentation.h`](../src/laige-sim/include/laige/sim/presentation.h)
pattern). Unit suite: `ctest -R tilemap`
(`tests/laige-render/tilemap_tests.cpp`) — pure data + batcher
bookkeeping, no GL environment required: it runs in every local tree
and in CI, and the goldens are hand-computed from the documented
formula.

The canonical narrative home is
[`docs/concepts/coordinates.md`](../concepts/coordinates.md) §4.9
(ARCH-008); the header preamble carries the machine-checked contract.
The per-tile key this tilemap precomputes is
[`iso_depth_table.md`](iso_depth_table.md) (M2-ISO-02) on top of
[`iso_depth_key.md`](iso_depth_key.md) (M2-ISO-01); the batch path
declares into [`sprite_batcher.md`](sprite_batcher.md) (M2-SPRITE-01).

## The API

| Member | Returns | Notes |
|---|---|---|
| `TileMap<Backend>::create(options)` | `Result<TileMap>` | validates the options (the table's create — first failure wins), pre-sizes the table chunks + the 8 B/tile data array, flat/empty init (setup path) |
| `setTile(gx, gy, textureId, height, animationId)` | `Status` | the per-tile edit: stores the tile data, routes the height into the table's `setTile` — the auto-depth update (update path) |
| `rebuild(tiles)` | `Status` | scene load: stores the whole grid (the data + the heights through the table's from-scratch rebuild; setup path) |
| `declareTo(batcher, options)` | `Status` | the render path: declares the static tile quads into the batcher's frame window (the batch path) |
| `tileAt(gx, gy)` | `TileData` | the tile's data: `{textureId, height (the table's), animationId}` (render read path; O(1)) |
| `depthKeyAt(gx, gy)` | `uint32_t` | the tile's current depth key (the table's — auto-depth; O(1)) |
| `tileHeightAt(gx, gy)` | `int32_t` | the tile's last stored height (diagnostics view — DBG-008) |
| `covers(gx, gy)` | `bool` | whether the tile lies in the requested grid (O(1)) |
| introspection | `int32_t` / `size_t` | `originTileX/Y()`, `widthTiles()`, `heightTiles()`, `layer()`, `chunkTiles()`, `tileCount()` |

`TileMap::Options` **is** the table's `Options` (one type — no
duplicated validation): `originTileX/Y` (default 0), `widthTiles` /
`heightTiles` (required ≥ 1), `chunkTiles` (default
`kIsoDepthTableChunkTiles` = 16; a power of two ≥ 1), `maxChunks`
(default `kIsoDepthTableDefaultMaxChunks` = 1024), `layer` (default
`kIsoDepthGroundLayer` = 0 — a parallax tile LAYER gets its own
tilemap with its layer value, M2-PAR-01 — the values are documented
in [`parallax.md`](parallax.md)). `TileData` is a plain value
(8 B of data + the table's height — the value the game writes on
load/edit and reads back).

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
  `setTile` and `rebuild` — heights validated over the whole span
  before any write): a rejected operation leaves the tile data AND
  the table unchanged; the returned `Status` is the failure channel
  the caller handles and logs (LOG-002).

## `declareTo` — the static tile-quad batch path (S-5)

The frame pipeline's cull/batch stage calls `declareTo(batcher,
options)` once per frame per tilemap: one `SpriteItem` per tile of
the requested grid, in the grid's **row-major order** (tileY outer,
tileX fastest — the tile's grid position is a static tile's stable
identity, the FR-1.2 entity-order analog; the insertion order the
M2-SORT-01 stable sort turns into the (key, insertion position) total
order — RENDER-003). Each quad (the fixed-frame model, the header
preamble):

- `pos` = the tile's **center** `(gx + 0.5, gy + 0.5)` (the same point
  the table quantizes; the float conversion is exact — dyadic,
  `|gx| ≤ 32766`);
- `scale` = (1, 1) — the unit quad spans the tile's world cell;
- `rotation` = 0; `uv` = the `DeclareOptions::uv` **fixed frame**
  (default (0, 0, 1, 1) = the full tile texture; per-tile UV frames —
  tile-sheet frames, animated frames — are the asset/animation steps,
  M2-TILE-02 / M3-ASSET-01 — `SpriteItem.uv` already carries them);
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
`declareTo`, first failure wins, nothing declared on failure). On a
stopped batcher the first add fails (`BudgetExhausted`) — nothing
declared. The WHOLE requested grid is declared (visible-rect culling
lands with M2-PERF-01 — the composite 50k budget's worst case is the
full grid).

## Ownership, lifetime, threading

- **Ownership:** one owner — the sim/scene-owner thread (the scene);
  the M2-GL-02 frame pipeline's cull/batch stage owns the render-side
  declaration. Move-only (CORE-009).
- **Storage:** pre-sized at creation (the only allocations: `create` —
  one table storage + one 8 B/tile data array — and `rebuild`'s
  setup-path temporary covered-height span).
- **Phases (CONC-001):** writes (`setTile`, `rebuild`) in the sim
  phase; reads (`tileAt`, `depthKeyAt`, `tileHeightAt`, `covers`,
  `declareTo`) in the render phase; the phases do not overlap (the
  table's contract). Not thread-safe by design.
- **ARCH-009:** the tile data is presentation-side (headless-buildable
  — no GL anywhere in this API); the render phase consumes it
  read-only (nothing in the batch path mutates the tilemap).
- **No silent failure (CORE-008):** every failure path returns the
  `Status`/`Result` (the caller handles and logs); the happy update
  paths log nothing (the Status is the failure channel — LOG-002 —
  and they are budget-critical — LOG-003).

## Performance (DOC-004)

| Operation | Cost | Allocations |
|---|---|---|
| `create` | O(covered cells) — the table's create + one data array | setup only |
| `setTile` | O(1) (the table's `setTile`, radius 0 + two stores) | none |
| `rebuild` | O(covered cells) (the table's from-scratch rebuild) | one setup temporary |
| `declareTo` | O(tileCount) — one O(1) batcher add per tile | none |
| `tileAt` / `depthKeyAt` / `tileHeightAt` / `covers` | O(1) | none |

- **No per-frame allocation** (FR-2.2): the `declareTo` loop allocates
  nothing (the adds are O(1) operations over the batcher's
  pre-allocated storage — the zero-allocation proof, 1000 frames ×
  256 tiles, the tests).
- **No standalone budget entry**: the per-frame declare cost is PART
  of the composite 50k render-CPU budget (PRD §8.1,
  `sprites_50k_cpu` — M2-PERF-01 measures the reference scene with
  tile quads included).
- **Budget-critical:** the `setTile`/`rebuild`/`declareTo` paths make
  no GL calls, no logging, no per-call allocation (the
  `iso_depthkey_rebuild` budget's per-update cost — the table's
  M2-ISO-02 budget — bounds the depth half of `setTile`).
- **Common traps:** declaring more than the batcher's frame budget
  (the frame overflow policy applies — drop the oldest + warn);
  recreating the tilemap per frame (the table IS the precomputation —
  FR-2.2 "not recomputed per frame"); reading `tileAt`/`depthKeyAt`
  on a tile outside the requested grid (UB — check `covers()` for
  untrusted input; the batch path reads only the requested grid by
  construction).

## Example (performant) and misuse warning

```cpp
using laige::render::TileMap;
using laige::render::TileData;
using laige::sim::Fpx16_16;

// Scene load (sim phase, setup):
TileMap<Fpx16_16>::Options o;
o.originTileX = 0; o.originTileY = 0;
o.widthTiles = 64; o.heightTiles = 64;
auto r = TileMap<Fpx16_16>::create(o);
auto map = std::move(r).takeValue();
std::vector<TileData> tiles = loadTiles();  // the game's format
map.rebuild(tiles);   // the heights go into the depth table (auto-depth)

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
checked). The animationId is DATA ONLY in M2 (M2-TILE-02 drives the
frame cycle from it — the batch path never touches it).
