# Isometric reference scene (`laige::render` scene)

The worst-case isometric reference scene of PRD §4 — the deterministic,
seed-driven scene every visual and performance acceptance test renders
(M2-SCENE-01; PRD §4, §8.1, §9.1 S-5; AGENTS ARCH-007/009/010, SCALE-004/005,
TEST-005, PERF-002/003, CORE-005/008, RENDER-001/003; ADR 0002). The step's
scope: a headless, seed-deterministic builder that generates the worst-case
isometric scene (50k overlapping sprites on tiled ground with stepped
heights, 3 parallax layers, particle emitters, a UI panel, and the
tile-height terrain) and emits it as data the engine can load. The full
scene FORMAT is M3; this step's versioned binary format is the M2 stand-in
(the in-memory builder + the simple serialization the tests use).

Public header:
`src/laige-render/include/laige/render/iso_reference_scene.h`
(header-only: the builder, the format functions, and the
`IsoReferenceSceneWorld`/`loadIsoReferenceScene`/
`declareIsoReferenceScene` templates over the SimMath backends).
Unit suite: `ctest -R scene_iso`
(`tests/laige-render/scene_iso_tests.cpp`) — pure data + batcher
bookkeeping, no GL environment required: it runs in every local tree and
in CI. The CLI's same-seed byte-identity gate is the separate
`scene_iso_cli` entry (tools/scene-iso); the parser's fuzz gate is
`fuzz_scene_iso_parse` (tools/fuzz).

The scene is a **test fixture**, not an asset: it carries no image data
(the atlases are the scene's ids — the game/tests bind their textures),
it is presentation state only (ARCH-009 — never sim state, never in the
replay hash), and its determinism scope is the PRng's (ARCH-010): same
seed, same bytes, every build and P0 platform (the Prng + the exact
2^-16 position scale — no FMA-susceptible float composition in the
builder). The deterministic gate that pins the scope is
`IsoReferenceSceneBuilder` (same-seed byte identity, different-seed
difference, both variants) plus the `scene_iso_cli` ctest entry (two
same-seed CLI invocations per variant compared byte for byte).

## The API

| Symbol | Returns | Notes |
|---|---|---|
| `buildIsoReferenceScene(seed, variant)` | `IsoReferenceScene` | total function (no failure): the scene is a pure function of (seed, variant) — O(scene size) time + O(scene size) memory (two vectors), setup only |
| `serializeIsoReferenceScene(scene)` | `vector<u8>` | total function: one allocation (the output), setup only; same scene, same bytes |
| `readIsoReferenceScene(bytes)` | `Result<IsoReferenceScene>` | the versioned, domain-validated read of an UNTRUSTED byte stream (SCALE-004): any violation fails the whole read with `MalformedInput` (nothing is partially applied) |
| `writeIsoReferenceScene(scene, path)` | `Status` | atomic temp + rename (a killed run never leaves a partial file); `IoError` on open/write/rename failure |
| `loadIsoReferenceScene<Backend>(scene)` | `Result<IsoReferenceSceneWorld<Backend>>` | the setup-path build of the scene's five engine objects; first failure wins (the failed step's error); O(scene size), setup only |
| `declareIsoReferenceScene(world, batcher, cameraPos)` | `Status` | ONE call = the whole world pass (parallax + tiles + sprites + particles); O(frame items), NO allocation; a stopped world -> `InvalidArgument` |

The value types are plain data (copyable, wire-explicit — struct padding
never leaks into the format): `IsoRefSceneVec2` (a float pair),
`IsoRefSceneSprite` (the 14-byte wire record: position, standing step
height, atlas, blend, frame), `IsoRefSceneParallaxLayer` (67 bytes),
`IsoRefSceneEmitter` (56 bytes), `IsoRefSceneUiWidget` (23 bytes), and
`IsoReferenceScene` (the whole scene: the terrain grid + the content
vectors + the fixed arrays).

`IsoReferenceSceneWorld<Backend>` is move-only (the SpriteBatcher
precedent) and owns the scene's five engine objects plus a copy of the
scene that built it and the 4 precomputed sprite-sheet frame UVs:

| Accessor | Notes |
|---|---|
| `valid()` | true iff the load succeeded (default / moved-from state is stopped) |
| `scene()` | the owned scene copy (M2-PERF-01 reads the scene's parameters through it) |
| `tiles()` / `parallax()` / `particles()` / `ui()` / `strings()` | the engine objects (the tilemap's accessor asserts the live state — the caller checks `valid()` first) |
| `contentFrameCount()` / `worstCaseFrameCount()` | the declared frame's minimum / worst-case size (a scrolled auto parallax layer splits into up to 4 quads — the parallax wrap contract; the batcher's capacity must cover the worst case plus the live particles) |

## The scene contents (the worst-case composition)

Built from one seed through three deterministic Prng substreams (terrain
= 1, sprites = 2, emitters = 3 — the M0-CORE-06 pattern):

- **FULL** (`IsoRefSceneVariant::Full`, the §8.1 budget scene): a 256 x 256
  tile grid (65 536 tiles) at the world origin, partitioned into 16 x 16-tile
  plateaus — each plateau a uniform PRNG-drawn height (step x 2 world
  units, step in [0, 24)) — so the terrain is a terraced, stepped surface
  with 24 distinct heights in [0, 46] (the M2-ISO-01 |z| <= 2047 domain
  with wide margin); 50 000 sprites at uniform-random ground positions
  (each `tile + fraction`, the fraction a uniform dyadic in [0, 1) at
  2^-16 resolution — exact in both SimMath backends), each standing on its
  PRNG-chosen tile (the sprite's `stepHeight` field is that tile's height
  — one source of truth); 3 image parallax layers (factors 0 / 0.5 / 1.0 —
  the M2-PAR-01 preset factors; 128 x 128 world units each); 8 continuous
  particle emitters (rate 2/tick, 40..120 tick lives, additive orange
  sparks); a 2-widget UI (one opaque panel, one "OK" button) on the 1280 x
  720 reference screen.
- **SMALL** (`IsoRefSceneVariant::Small`, the fast unit-test fixture): the
  same composition scaled down (32 x 32 grid of 8 x 8-tile plateaus, 1 000
  sprites).

Worst-case properties (why this is the reference scene):

- 50 000 sprites over 65 536 tiles means several sprites per depth row:
  the `(x + y) - z` value quantizes to at most a few thousand distinct
  32-bit keys, so the M2-SORT-01 radix sort sees a dense multi-sprite
  tie band on every row (the worst-case overlap for depth sorting,
  PRD §4);
- the 50k sprites + 65k tile quads + parallax quads + live particles
  fill a ~116k-item frame — the batcher's frame budget exercised at its
  reference size;
- the atlas-id plan (below) pins the §8.1 workload to 14
  (atlas, material, blend) groups = 14 draw calls in the world pass —
  under the 30-draw-call budget, asserted by the test suite.

## The atlas-id plan (the cross-group render order)

The batcher's cross-group draw order is ascending (atlasId, materialId,
blend) (M2-SPRITE-01), so the scene assigns atlas ids that make group
order = painter's order (the parallax.h scene-setup convention —
background first, ground next, world objects, particles, foreground
last):

| atlas id | content | depth layer |
|---|---|---|
| 0 | background parallax layer | -2 |
| 1 | mid parallax layer | -1 |
| 2 | ground tilemap (all tiles) | 0 |
| 3..6 | the 4 sprite atlases (Alpha / Additive) | 0 |
| 7 | the particle atlas (Additive) | 0 |
| 8 | foreground parallax layer | +1 |

= `kIsoRefSceneSpritePassGroups` = 14 groups. The UI pass is a separate
batcher (M2-UI-02) with its own ids: widget art atlas 100, font atlas
101 (unused when the pass declares without a default font — the
label-less world pass).

## The binary format (version 1)

A flat little-endian binary, written field by field (the explicit layout
below — struct padding never leaks in; floats are IEEE-754 binary32;
multi-byte integers little-endian; no checksum — the reader's domain
validation is the integrity contract, and the determinism check is the
generator's):

| offset | size | field |
|---|---|---|
| 0 | 4 | version (u32; MUST be 1 for this reader) |
| 4 | 1 | variant (u8: 0 = small, 1 = full) |
| 5 | 3 | reserved (zero) |
| 8 | 8 | seed (u64) |
| 16 | 4 | originTileX (i32) |
| 20 | 4 | originTileY (i32) |
| 24 | 4 | widthTiles (i32) |
| 28 | 4 | heightTiles (i32) |
| 32 | 4 | plateauTiles (i32) |
| 36 | 4 | stepHeight (i32) |
| 40 | 4 | maxSteps (i32) |
| 44 | 4 | tileCount (u32) |
| 48 | 4 | spriteCount (u32) |
| 52 | 4 | emitterCount (u32) |
| 56 | 4 | uiWidgetCount (u32) |
| 60 | 4 | parallaxCount (u32) |
| 64 | 4 | reserved (zero) |
| 68 | 4 x tileCount | tileHeights (i32 per tile, row-major, tileX fastest) |
| ... | 14 x spriteCount | sprites: x (f32), y (f32), stepHeight (i16), atlasId (u8), blend (u8), frame (u8), reserved (u8) |
| ... | 67 x parallaxCount | parallax layers: factor (f32), center (2 f32), offset (2 f32), size (2 f32), uv (4 f32), atlasId (u32), materialId (u32), blend (u8), scrollMode (u8), enabled (u8), reserved (u8), depthLayer (i32), scrollSpeed (2 f32) |
| ... | 56 x emitterCount | emitters: originX/Y (2 f32), depth (i32), velMin/velMax (4 f32), lifeMin/lifeMax (2 u32), sizeMin/sizeMax (2 f32), tint (4 u8), fadeTicks (u32), continuousRate (u32) |
| ... | 23 x uiWidgetCount | UI widgets: kind (u8), opaque (u8), x/y/w/h (4 i32), atlasId (u32), label (u8) |

The reader treats the bytes as untrusted input (SCALE-004): every field
is bounds- and domain-checked (the header's grid/height domains, the
sprite position/atlas/blend/frame domains, the parallax factor/size/uv/
depthLayer domains, the emitter life/size domains, the widget kind/
geometry domains, and the exact total length — trailing bytes are
rejected), and any violation fails the whole read with `MalformedInput`.
Versioning (ARCH-007): the reader rejects any other version with
`MalformedInput` — it never guesses; a future format version bumps
`version` and gets a new reader. The untrusted-input surface has a fuzz
target (`fuzz_scene_iso_parse`, 1000 bounded runs every commit —
TEST-005 / SCALE-005) whose corpus includes valid v1 small + full
scenes as mutate/truncate bases.

## The engine-side world (load + declare)

`loadIsoReferenceScene<Backend>(scene)` builds, in order (first failure
wins, the world is left stopped on failure — nothing owned):

1. the terrain `TileMap` (origin/size from the scene; layer 0; one
   `rebuild` of the scene's heights; one (atlas 2, material 0, Alpha)
   group for the whole ground);
2. the `ParallaxLayers` registry (the scene's 3 image layers in
   declaration order — bg, mid, fg);
3. the `ParticleSystem` (the scene's emitters; the system's Prng seeded
   from the scene's seed — deterministic live particles);
4. the `StringTable` (the button's interned "OK") + the `UiTree`
   (the scene's 2 widgets on the 1280 x 720 reference screen; each
   widget's art rect is the whole atlas — the scene assigns the
   texture, M3-ASSET-01);
5. the 4 sprite-sheet frame UVs (the tight 32 x 32 sheet of 16 x 16
   texel frames — precomputed once at load, never per sprite).

`declareIsoReferenceScene(world, batcher, cameraPos)` is the per-frame
declare (the batcher's frame window must be open — the frame protocol;
`cameraPos` is the render Vec2, the parallax's presentation input,
RENDER-006): the parallax layers (after their once-per-frame scroll
advance — the documented pacing), the terrain tiles (one group), the
sprites (the scene's array order — the RENDER-003 deterministic
insertion, each with `isoDepthKey(pos, the standing tile's height,
layer 0)` and its precomputed frame UV), and the live particles (one
additive group). First failure wins: the sub-step's error is the call's
error, and the items already declared by this call remain in the frame
(the batcher owns the frame state — the game re-declares the whole
frame every frame).

## The CLI tool (`laige-scene-iso`)

```
laige-scene-iso --seed <u64> --variant <full|small> --out <path>
```

Builds the scene deterministically from the seed, serializes it, writes
it atomically (temp + rename), then reads the file back and verifies the
round trip (byte-identical + a clean parse — no silent failure,
CORE-008). Headless: no GL context, no window, no sim world (ARCH-003).
Exit codes: 0 = written + verified (the machine-greppable summary line
records variant, seed, byte count, shape, and the measured generation
time — CORE-001), 2 = usage / IO / round-trip failure. The step's
"< 5 s" generation budget is the tool's property (the `scene_iso_cli`
ctest entry's 60 s timeout guards it with wide margin; the absolute
number is machine-dependent — the summary line records it). The
determinism gate (`scene_iso_cli`) runs the tool twice per variant with
the same seed and compares the output files byte for byte — the
roadmap's Verify clause in executable form.

## Performance

- **Build**: O(scene size) time + O(scene size) memory (two vectors),
  setup only; the full scene serializes to ~0.94 MB. No §8.1
  budgets.json entry (the generation budget is the tool's — the
  budgets.json count is unchanged).
- **Load**: O(scene size), setup only (the tilemap's rebuild, the three
  setLayer, the eight addEmitter, the two addChild, one intern, the 4
  frame-UV precomputations) — a handful of setup allocations (the
  objects' own — the house pattern).
- **Declare**: O(frame items) per frame, **NO allocation** (the batcher's
  storage is pre-sized; the per-sprite isoDepthKey is a handful of
  integer ops; the UV rects are the world's 4 precomputed values). The
  declare is one component of the §8.1 "50k sprites ... <= 2 ms CPU"
  composite budget — M2-PERF-01 measures the whole frame (tile quads
  included), not this step alone. The zero-allocation property is
  asserted by `IsoReferenceSceneZeroAlloc` (1 000 frames, both
  backends — the sanitizer trees' instrumentation is the check there).

## Misuse warnings

- **The scene file is a test fixture, not an asset**: it carries no
  image data, and it is NOT versioned scene SAVE data (ARCH-007 applies
  to the file's own format only — the M3 scene format supersedes it).
- **The parallax auto layers ADVANCE on every
  `declareIsoReferenceScene` call**: declare once per frame, in the
  render phase, never per sprite or in a tight retry loop (the scroll
  offsets are presentation state — a double advance is a wrong scroll,
  not an error the engine can detect).
- **Size the world pass's batcher to the scene's frame**: the full scene
  declares ~116k items per frame (`worstCaseFrameCount()` + the live
  particles); an undersized batcher drops the oldest items (the
  batcher's overflow policy — silent degradation, LOG-004) and the
  frame is still built.
