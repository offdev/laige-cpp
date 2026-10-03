# Isometric depth key table (`laige::render` iso depth table)

The per-scene-chunk, precomputed, incrementally-updated mapping from the
tile grid to engine-owned isometric depth keys (M2-ISO-02; PRD §4,
FR-2.2 — "precomputed at scene build and incrementally updated on
tile/height changes (not recomputed per frame)", §8.1 — 10k dirty cells
≤ 0.2 ms, AC-4.4 base, S-5, G-R11; AGENTS ARCH-008/009, RENDER-003,
CORE-002/005, PERF-002/003, SCALE-001/003; ADR 0002). Public header:
`src/laige-render/include/laige/render/iso_depth_table.h` (header-only —
the API is a template over the SimMath backends, the
[`presentation.h`](../src/laige-sim/include/laige/sim/presentation.h)
pattern). Unit suite: `ctest -R iso_depth_table`
(`tests/laige-render/iso_depth_table_tests.cpp`) — pure data, no GL
environment required: it runs in every local tree and in CI, the
goldens are hand-computed from the documented formula, and the
`IsoDepthTableBudget` suite gates the PRD §8.1 `iso_depthkey_rebuild`
budget (the reference platform, non-instrumented trees — methodology
§4/§5).

The canonical narrative home is
[`docs/concepts/coordinates.md`](../concepts/coordinates.md) §4.5
(ARCH-008); the header preamble carries the machine-checked contract.
The per-sprite key this table precomputes is
[`iso_depth_key.md`](iso_depth_key.md) (M2-ISO-01).

## The API

| Member | Returns | Notes |
|---|---|---|
| `IsoDepthKeyTable<Backend>::create(options)` | `Result<IsoDepthKeyTable>` | validates the options, pre-sizes the initial grid's chunks, flat-ground init (setup path) |
| `rebuild(heights)` | `Status` | from-scratch: stores the whole height grid, recomputes every key through the full M2-ISO-01 function (setup path) |
| `setTile(gx, gy, height)` | `Status` | the incremental update: stores the height, recomputes the affected cells only (the update path) |
| `ensureChunk(gx, gy)` | `Status` | growth: extends the covered region to the chunk containing the tile (setup path; bounded + logged) |
| `keyAt(gx, gy)` | `uint32_t` | the cell's current depth key (render read path; O(1)) |
| `tileHeightAt(gx, gy)` | `int32_t` | the cell's last stored tile height (diagnostics view — DBG-008) |
| `covers(gx, gy)` | `bool` | whether the cell lies in the covered region (O(1)) |
| introspection | `int32_t` / `size_t` | `layer()`, `chunkTiles()`, `originTileX/Y()`, `widthTiles()`, `heightTiles()`, `coveredTileMinX/Y()`, `coveredTileMaxX/Y()`, `chunkCount()`, `maxChunks()`, `coveredCellCount()` |

`Options` (the `create` argument, validated — first failure wins,
`InvalidArgument`): `originTileX/Y` (default 0), `widthTiles` /
`heightTiles` (required ≥ 1), `chunkTiles` (default
`kIsoDepthTableChunkTiles` = 16; a power of two ≥ 1 — keeps cell
addressing shift/mask, division-free), `maxChunks` (default
`kIsoDepthTableDefaultMaxChunks` = 1024; the growth cap), `layer`
(default `kIsoDepthGroundLayer` = 0 — a parallax tile LAYER gets its
own table with its layer value, M2-PAR-01).

### The cell model

One `CellRecord` per covered cell: `{ key, qBase, height }`.
`qBase` is the backend-quantized ground contribution of the cell's
**center** (gx + 0.5, gy + 0.5) — a pure function of position, computed
once per chunk at creation through the same backend add + quantize the
M2-ISO-01 key uses; `height` is the tile's current step height; `key`
is the current depth key — bit-identical to
`isoDepthKey<Backend>(center, height, layer)` (the header preamble's
derivation; the tests pin it).

The covered region is the **chunk-aligned superset** of the requested
grid: it may extend up to `chunkTiles − 1` tiles past a requested edge
(the partial edge chunks carry real cells, flat unless the scene sets
them). `rebuild`'s span is the covered rectangle —
`coveredCellCount()` cells, row-major, tileX fastest.

### `setTile` — the incremental update (FR-2.2)

Stores the height and recomputes the keys of the affected cells only —
the edited cell plus its documented neighborhood,
`kIsoDepthTableUpdateRadius` (radius **0** for the M2-ISO-01 formula:
a cell's key is a function of the cell's own (x, y, height, layer)
alone, and the table stores each cell's own height — so a height edit
changes exactly that tile's key). Each affected cell is recomputed from
its **own** stored height, so the neighborhood loop is correct for any
radius a future formula may need; widening the named constant changes
no API. Rejected edits (uncovered cell, out-of-domain height) leave the
table unchanged; the returned `Status` is the failure channel the
caller handles and logs (LOG-002: no duplicate engine log per call).

### `rebuild` — from scratch (scene load)

Stores the whole height grid and recomputes **every** key through the
full M2-ISO-01 function — independent of the stored `qBase`. Property:
`rebuild(final grid) == any sequence of `setTile` calls reaching the
same grid — the property test pins it (the roadmap's "rebuild-from-
scratch == incremental result").

### `ensureChunk` — bounded, logged growth (SCALE-001/003)

Extends the covered region to include the chunk containing the tile
(the region stays a chunk-aligned rectangle). Bounded by `maxChunks`
(exceeding it: `BudgetExhausted`) and logged (one Debug event per
created chunk, one rate-limited Warn on the cap —
`render/iso_depth_table_growth_cap`). No-op success when the tile is
already covered; `InvalidArgument` when the chunk's extreme tile
centers would leave the key domain.

## Ownership, lifetime, threading

- **Ownership:** one owner — the sim/scene-owner thread (the
  scene/tilemap). The table is move-only (CORE-009): created by
  `create()`, handed to its owner; destroyed with the scene. It owns
  its chunk storages; nothing else references them.
- **Threading / phase:** writes (`setTile`, `rebuild`, `ensureChunk`)
  happen in the simulation phase (a terrain edit is a sim-side command
  — M2-TILE-01); reads (`keyAt`, `covers`, `tileHeightAt`) happen in
  the render phase (the tile batch path — M2-TILE-01/M2-SPRITE-01).
  The phases never overlap (the frame pipeline's tick → handoff →
  render ordering, M2-GL-02), so no synchronization is needed; the
  table is never shared with a concurrent writer.
- **Backend selection:** the template parameter is the scene's
  selected SimMath backend (ADR 0002) — the same dispatch pattern as
  `PresentationSnapshot<Backend>` and `isoDepthKey<Backend>`.

## Performance (PERF-001/002/003, DOC-004)

- **`create` / `ensureChunk` / `rebuild` — setup paths.** One flat
  allocation for the covered — chunk-aligned — region at `create` (a
  pre-sized row-major `CellRecord` array); `ensureChunk` growth
  re-allocates exactly once (setup path, bounded by `maxChunks`,
  logged); `rebuild` is O(covered cells) backend work (the
  from-scratch contract). Never called per frame.
- **`setTile` — O(1), zero allocation, no logging on success, no GL.**
  The body is flat (no helper calls, no container calls) because the
  budget gate is measured on the canonical **Debug** tree
  (methodology §4), where a helper call is a real function call —
  notably the cell is read through a cached raw pointer, since
  `unique_ptr::operator[]` is a six-level call chain on that tree.
  Budget: **10k dirty cells ≤ 0.2 ms mean** (PRD §8.1,
  `iso_depthkey_rebuild`) — measured **0.087 ms (fpx16_16) /
  0.086 ms (fp32_pinned)** on the canonical Debug tree (worse of
  backends recorded in `budgets.json` and
  [baselines/m2-iso-depth-table.md](../benchmarks/baselines/m2-iso-depth-table.md));
  the gate also passes on the reference-class clang -O0 tree (0.152 ms
  mean — the flat path clears the 0.2 ms bar with margin on both
  backends). The zero-allocation contract is asserted by the update
  suite where the allocation watch is live (the non-sanitizer trees).
- **`keyAt` / `covers` / `tileHeightAt` — O(1)** flat-index reads, no
  allocation, no logging.
- **Common traps:**
  - **Rebuilding the table per frame** to "refresh" keys — the FR-2.2
    anti-pattern; the table IS the precomputation. Per-frame keys for
    *moving* sprites come from `isoDepthKey` (M2-ISO-01).
  - **`keyAt` on an uncovered cell is undefined behavior** (a program
    bug — the read path is check-free by contract, PERF-002/006, the
    `isoDepthKey` total-within-domain pattern). Check `covers()` when
    the tile coordinate comes from untrusted input; the batch path
    reads only covered tiles by construction (it culls against the
    covered region).
  - **Pass the tile's own height** to `setTile` (its standing-surface
    elevation), not the sprite's top.
  - **`rebuild`'s span is the covered rectangle**, not the requested
    grid (the chunk-aligned superset — `coveredCellCount()` cells).

## Determinism, replication, network authority

- Deterministic per the backend's ADR 0002 scope: fpx16_16 bit-exact on
  every platform; fp32_pinned bit-exact per same build/ISA.
- **Presentation-only (ARCH-009):** the contents are a deterministic
  function of (options, edit sequence, backend) — presentation state,
  never authoritative sim state, replay state, or the sim state hash.
- **Not replicated.** The table is a per-client presentation concern
  (ARCH-003); replicated state carries only the tile heights (the sim-
  side data) it is computed from. Grid-locked cells agree across
  backends (dyadic centers in the exactness zone) — the tests pin it.

## Failure behavior / invalidation

- `create`: option validation, first failure wins
  (`InvalidArgument`) — grid extents, `chunkTiles` (power of two),
  `maxChunks ≥ 1`, layer domain, tile domain, grid-vs-cap.
- `setTile` / `rebuild`: boundary validation (`InvalidArgument`) —
  uncovered cell (`setTile`), out-of-domain height (`setTile`/
  `rebuild`), wrong span size (`rebuild`). A rejected call leaves the
  table unchanged.
- `ensureChunk`: `BudgetExhausted` at the cap (logged);
  `InvalidArgument` beyond the tile domain.
- There is no per-frame invalidation: the table is the precomputation.
  Its contents change only through `setTile`/`rebuild`/`ensureChunk`
  (sim phase).

## Performant example

```cpp
// Scene load (sim thread, setup): the 128 x 128 tile grid, one 16 x 16
// chunk per 256 tiles, layer 0 — the fpx16_16 backend (the engine
// default, ADR 0002).
using Table = laige::render::IsoDepthKeyTable<laige::sim::Fpx16_16>;
Table::Options opts;
opts.widthTiles = 128;
opts.heightTiles = 128;
auto table = Table::create(opts);
if (!table.ok()) { /* log the InvalidArgument reason (LOG-002) */ }
// The from-scratch load of the height grid (the covered rectangle —
// the chunk-aligned superset — row-major, tileX fastest):
std::vector<std::int32_t> heights(table.value().coveredCellCount());
/* ...fill from the scene's tile heights... */
auto rebuilt = table.value().rebuild(heights);
if (!rebuilt.ok()) { /* log the InvalidArgument reason */ }

// Terrain edit (sim phase, M2-TILE-01): one tile height write — O(1),
// zero allocation:
auto edited = table.value().setTile(gx, gy, /*newHeight=*/h);
if (!edited.ok()) { /* log: uncovered cell or out-of-domain height */ }

// Render phase (the tile batch, M2-TILE-01/SPRITE-01): read the keys —
// only covered tiles (the batch culls against the covered region):
if (table.value().covers(gx, gy)) {
  const std::uint32_t key = table.value().keyAt(gx, gy);
  laige::render::SpriteItem item;       // the declared tile sprite
  item.depthKey = key;                  // (the tile grid's precomputed key)
  batcher.add(item);                    // M2-SPRITE-01, in tile order
}
// Equal keys: the stable sort (M2-SORT-01) + the entity-id insertion
// order fix the order — isoDepthOrderLess (M2-ISO-01) is the
// comparison the sort uses.
```

## Misuse warnings

- **Do not call `setTile` per frame to "refresh" keys** (FR-2.2): the
  table is updated only on tile/height *changes*; per-frame per-sprite
  keys for moving sprites come from `isoDepthKey` (M2-ISO-01).
- **Do not use the table for moving objects.** It maps the static tile
  grid; a sprite's standing tile is a *read* (the sprite's current
  grid cell's `keyAt`), never a *write*.
- **Do not pass `rebuild` the requested grid.** The span is the
  covered rectangle (`coveredCellCount()` cells) — the
  chunk-aligned superset (the preamble's cell model).
- **Do not share the table with a concurrent writer.** One owner,
  sim-phase writes, render-phase reads (the frame pipeline's phase
  ordering, M2-GL-02) — no locks, no atomics, by contract.

## Related

- [`concepts/coordinates.md`](../concepts/coordinates.md) §4.5 — the
  canonical narrative (ARCH-008).
- [`iso_depth_key.md`](iso_depth_key.md) — the per-sprite key the table
  precomputes (M2-ISO-01).
- [`budget_harness.md`](budget_harness.md) — the `iso_depthkey_rebuild`
  gate's harness.
- [baselines/m2-iso-depth-table.md](../benchmarks/baselines/m2-iso-depth-table.md)
  — the recorded 10k-dirty-cell baseline.
- Roadmap: M2-ISO-02 (this), M2-TILE-01 (tilemap wiring), M2-SORT-01
  (stable radix sort), M2-SPRITE-01/02 (batcher), M2-PAR-01 (layer
  values).
