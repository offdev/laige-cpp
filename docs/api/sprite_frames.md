# Sprite frames (`laige::render::SpriteFrameLayout`, `laige::render::spriteFrameUv`)

The atlas UV frame animation hook (M2-SPRITE-03; FR-2.1: "atlas UV
animation (sheet frames)"): the atlas sheet frame LAYOUT (frame size,
row/col, margins — the documented sheet model below) and the pure
frame-index → UV sub-rect computation that turns a caller-set animation
frame index into the `SpriteItem.uv` the M2-SPRITE-02 shader consumes.
This is the data-driven half of the feature: for now the frame index is
set by the caller per frame (the game's declaration pipeline); M3
animation will own the frame-advance policy on top of this same layout
(the hook, below). Public header:
`src/laige-render/include/laige/render/sprite_frames.h` (header-only —
a pure function over a plain value; there is no implementation file).
Unit suite: `ctest -R sprite_frames`
(`tests/laige-render/sprite_frames_tests.cpp`) — pure float/integer
math, no GL environment required: it runs in every local tree and in
CI, with the hand-computed UV rects for documented layouts, the
documented failure contract (out-of-range frame → `InvalidArgument`,
never wrap), the sheet model against the documented formula + the
float-exact domain, and the `SpriteItem.frameIndex` hook through the
batcher.

## The API

```cpp
// The float-exact atlas domain: atlas dimensions in [1, 2^24].
inline constexpr std::uint32_t kSpriteFrameMaxAtlasTexels = 1u << 24;

// The atlas sheet's frame layout (texels — the sheet model, below).
struct SpriteFrameLayout {
  std::uint32_t frameWidth{};   // texels per frame (>= 1)
  std::uint32_t frameHeight{};  // texels per frame (>= 1)
  std::uint32_t columns{};      // frames per row (>= 1)
  std::uint32_t rows{};         // frame rows (>= 1)
  std::uint32_t frameSpacing{}; // gap between adjacent frames (texels)
  std::uint32_t sheetBorder{};  // margin at the sheet edge (texels)
};

// The UV sub-rect of `frameIndex` in the atlas, under `layout`.
// Out-of-range frameIndex -> InvalidArgument (never wraps).
[[nodiscard]] Result<SpriteUvRect> spriteFrameUv(
    std::uint32_t frameIndex, const SpriteFrameLayout& layout,
    std::uint32_t atlasWidth, std::uint32_t atlasHeight) noexcept;
```

`SpriteUvRect` is the batcher's per-sprite UV sub-rect
([`api/sprite_batcher.md`](sprite_batcher.md)): normalized `[0, 1]²`,
`u1 > u0`, `v1 > v0`.

## The sheet model (the documented layout)

A sprite sheet is a `rows × columns` grid of frames, **row-major, frame
0 at the top-left**:

```
frame i:  col = i % columns,  row = i / columns
```

Each frame is `frameWidth × frameHeight` texels. `frameSpacing` is the
gap **between** adjacent frames (texels); `sheetBorder` is the margin
between the sheet edge and the first/last frame on every side (texels).
Frame `(col, row)` occupies the texel rect

```
x: [border + col·(fw + spacing), border + col·(fw + spacing) + fw)
y: [border + row·(fh + spacing), border + row·(fh + spacing) + fh)
```

— the gap lives to the RIGHT of each frame and BELOW each row, and a
**tight** sheet is exactly

```
width  = 2·border + columns·fw + (columns − 1)·spacing
height = 2·border + rows·fh    + (rows − 1)·spacing
```

(a wider atlas is fine — the extra slack is just margin; the fit check
is the authoritative validity rule).

The UV rect is the frame's texel rect normalized into the atlas:
`u0 = x0/W`, `v0 = y0/H`, `u1 = x1/W`, `v1 = y1/H`. **V-axis
convention:** v = 0 is the FIRST texel row of the uploaded RGBA array
(the sheet's TOP row — the M2-SPRITE-02 `GL_NEAREST` "texel row =
floor(v·h)" contract), so frame row 0 carries the smallest v and a
frame's v rect is measured from the top, not the bottom.

## Failure (CORE-008, API-008) — first failure wins

The layout is CALLER-OWNED asset metadata (the game's import pipeline
for now, the asset system from M3 — untrusted input, SCALE-004), so
every field is validated at the use boundary. `spriteFrameUv` returns
`InvalidArgument` for:

| # | Condition | Why |
|---|---|---|
| 1 | `frameWidth`, `frameHeight`, `columns`, or `rows` is 0 | a zero extent is an invalid layout, not an "empty" one |
| 2 | `atlasWidth`/`atlasHeight` outside `[1, 2^24]` | the float-exact domain (below) |
| 3 | `frameIndex >= columns·rows` | the documented OUT-OF-RANGE contract: the engine never wraps silently |
| 4 | the frame's texel rect does not fit the atlas | the layout does not describe the sheet it claims to |

A failed call returns the error; it never produces a UV rect.

## Precision (the float-exact domain)

All texel coordinates are exact in float — and the invariant
`u1 > u0`, `v1 > v0` holds EXACTLY (two distinct `k/W` values never
round to the same float) — when the atlas dimensions are within
`[1, kSpriteFrameMaxAtlasTexels] = [1, 2^24]` (16 777 216 — far beyond
the practical driver maxTextureSize of 16K–32K): every pixel
coordinate is then `< 2^24`, exactly representable in float, and the UV
corners are the EXACTLY-rounded `k/W` values (one correctly-rounded
division per corner — no extra arithmetic, no `floor`/`ceil`, no
backend dependency). Outside the domain the call fails `InvalidArgument`
(the domain is part of the layout contract — a sheet that does not fit a
float-exact atlas is an invalid layout, not a degraded one).

The conversion is a PURE function: no allocation, no logging, no state,
no GL — a handful of integer checks + 4 float divisions, bit-identical
on every platform/build (render-side float — the ARCH-009/010
presentation-only scope; it is never in the sim state hash or the
replay state).

## The M3 hook (data-driven, ARCH-009)

For now the frame index is set by the caller: the game's declaration
pipeline holds each animated entity's current frame index (and its
atlas's `SpriteFrameLayout` + atlas size — asset-side data), and per
frame declares the sprite with

- `item.frameIndex = frame;` — the declared animation frame (the
  batcher carries it through untouched — presentation state, never
  sim state);
- `item.uv = spriteFrameUv(frame, layout, atlasW, atlasH);` — the
  frame's UV sub-rect, **what the M2-SPRITE-02 shader draws**.

M3 animation will drive the frame-advance policy (timers, events,
state machines) on top of this same layout — no engine change: it
writes the same two fields per frame. The caller's invariant: `uv` is
this item's `frameIndex` under its atlas's layout (the engine does not
cross-check the pair — `uv` is authoritative for the draw).

## Ownership, lifetime, threading (CORE-009, CONC-001)

- **Owner:** none — `SpriteFrameLayout` is a plain value (no ownership,
  nothing to release) and `spriteFrameUv` is a stateless free function.
- **Threading:** callable from any thread — no locks, no shared state.
  The declaration pipeline calls it once per animated sprite per frame
  (the render phase, after the tick→handoff — ARCH-002).
- **Lifetime:** the returned `SpriteUvRect` is a value; the caller owns
  it (the `SpriteItem` field). The layout outlives the calls that use
  it (the asset's metadata).

## Performance (PERF-002/003, DOC-004)

- **Complexity:** O(1) — a handful of integer checks + 4 float
  divisions per call.
- **Allocation:** zero (PERF-003) — proven by the zero-allocation test
  (1 000 consecutive conversions = 0 heap blocks, non-sanitizer
  trees).
- **Blocking/IO/GPU:** none — no logging, no GL calls.
- **Budget:** no standalone `budgets.json` entry — the per-frame
  conversion cost is part of the composite 50k render-CPU budget
  (2 ms, PRD §8.1 `sprites_50k_cpu`), measured when M2-PERF-01 lands
  with the submit stage.
- **Call site:** once per ANIMATED sprite per frame, in the declaration
  pipeline — never per texel, never in the simulation tick (ARCH-002).
  A 50k-sprite frame of fully-animated sprites is 50 000 × (4 float
  divisions + a few integer checks) — well inside the composite budget
  (the M2-PERF-01 measurement will say so with a number).

## Performant example

```cpp
// Per frame, per animated entity (the declaration pipeline — the
// frame pipeline's cull/batch stage, M2-GL-02):
SpriteItem item;
item.pos = entity.worldPos();
item.depthKey = laige::render::isoDepthKey<laige::fpx16_16>(
    entity.worldPos(), entity.stepHeight(), entity.layer());
item.frameIndex = anim.frame();                 // the M3 hook (for now: the caller)
item.uv = std::move(laige::render::spriteFrameUv(
    anim.frame(), anim.layout(), atlasWidth, atlasHeight)).takeValue();
item.atlasId = atlasId;
batcher.add(item);
```

## Misuse warnings

- **Do not wrap the frame index by hand around the layout:** the engine
  fails out-of-range indices (`InvalidArgument`, never wrap — CORE-008).
  A wrapping game computes the index itself (`frame % frameCount`); a
  non-wrapping animation clamps it.
- **Keep `uv` consistent with `frameIndex`:** the renderer draws `uv` —
  the index is the declaration's frame record. If the two disagree,
  the drawn frame is the `uv` one (the M3 invariant above).
- **The margins are texels, not world units or normalized UV:** the
  layout describes the atlas's pixels; the world-scale is
  `SpriteItem.scale`.
- **The atlas size must match the bound texture:** `atlasWidth`/
  `atlasHeight` are the `bindAtlas(id, w, h, rgba)` dimensions
  (M2-SPRITE-02) — a mismatch misaligns every frame's UV.
- **Do not use the result across frames:** the UV rect is a value copy
  per declaration (presentation state) — recompute it each frame from
  the frame's index.

## Related

- [`api/sprite_batcher.md`](sprite_batcher.md) — the declaration window
  that carries `SpriteItem` (the `frameIndex` + `uv` pair).
- [`api/sprite_renderer.md`](sprite_renderer.md) — the submit stage
  that draws the item's `uv` (M2-SPRITE-02).
- [`api/errors.md`](errors.md) — the `InvalidArgument` error code
  (M0-CORE-02).
- [`concepts/coordinates.md` §4.8](../concepts/coordinates.md) — the
  sprite-draw narrative (the UV sub-rect's place in the pipeline).
