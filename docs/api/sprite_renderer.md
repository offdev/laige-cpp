# Sprite renderer (`laige::render::SpriteRenderer`)

The frame pipeline's **submit stage** for the sprite pass
(M2-SPRITE-02): the GPU instanced draw calls + the minimal GLSL 3.30
sprite shader. FR-2.1: "one draw call per (atlas, material, blend)
group per frame; GPU-instanced; supports rotation, scale, tint,
per-sprite depth, UV sub-rect"; RENDER-001: state changes and draw
submissions observable (the per-frame / since-construction counters
below — the M2-SPRITE-04 profiler feed). The renderer consumes the
M2-SPRITE-01 batcher's BUILT frame and makes exactly ONE
`glDrawArraysInstanced` per (atlas, material, blend) group; the
per-group state (one texture bind, one blend function) is set once, so
state changes are O(group count), not O(sprite count).

Public header + implementation:
`src/laige-render/include/laige/render/sprite_renderer.h`,
`src/laige-render/sprite_renderer.cpp`.

Unit/integration suite: `ctest -R sprite_draw`
(`tests/laige-render/sprite_draw_tests.cpp`). The `SpriteDrawState` /
`SpriteDrawSmoke` / `SpriteDrawPipeline` suites require a usable
OpenGL 3.3 core environment (the CI path: Mesa software GL on the
offscreen FBO — `GlContext::createHeadless`); they self-
`GTEST_SKIP` on an environment failure, never on an engine failure.
`SpriteDrawCreate` runs everywhere (no GL: options validation, the
stopped state). The smoke test is the roadmap's offscreen render of a
1 000-sprite scene: it asserts `draw call count == group count`
(3 == 3 in the scene), the GL-side primitive cross-check
(2 000 == 2 × 1 000), the state-change counts, and the whole 128×128
frame against a CPU reference rasterizer (±1-byte per channel — the
GPU float32 vs the reference double — plus rounding-exact probe
pixels), and logs the machine-greppable line
`sprite-draw: size=128x128 groups=3 draw_calls=3 instances=1000
primitives=2000 texture_binds=2 blend_changes=3 ...`.

## The API

```cpp
struct SpriteDrawStats {   // the per-frame counters of the last successful submit
  std::uint32_t drawCalls;     // instanced draw calls (== group count)
  std::uint32_t textureBinds;  // texture binds in the pass
  std::uint32_t blendChanges;  // blend-function changes in the pass
  std::uint32_t instances;     // instances drawn this frame
  std::uint32_t primitives;    // PRIMITIVES_GENERATED (query on only)
};

struct SpriteDrawTotals {    // since-construction (successful submits only)
  std::uint64_t frames, drawCalls, textureBinds, blendChanges, instances, primitives;
};

class SpriteRenderer {
 public:
  struct Options {
    std::uint32_t maxInstances{0};   // per-frame instance budget
    std::uint32_t maxAtlases{0};     // atlas-id domain [0, maxAtlases)
    bool primitiveQuery{false};      // opt-in PRIMITIVES_GENERATED query
  };
  static constexpr std::uint32_t kSpriteRendererMaxInstances = 0xFFFFFFFFu;
  static constexpr std::uint32_t kSpriteRendererMaxAtlases = 4096u;

  SpriteRenderer() noexcept;                 // stopped state
  [[nodiscard]] static Result<SpriteRenderer, ErrorCode>
  create(const GlContext& ctx, Options) noexcept;  // one-time setup
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint32_t maxInstances() const noexcept;
  [[nodiscard]] std::uint32_t maxAtlases() const noexcept;
  [[nodiscard]] Status bindAtlas(std::uint32_t atlasId, std::uint32_t w,
                                 std::uint32_t h, std::span<const std::uint8_t> rgba);
  [[nodiscard]] Status submit(SpriteBatcher& batcher, const Mat4& worldToNdc);
  [[nodiscard]] SpriteDrawStats frameStats() const noexcept;  // last success (else zero)
  [[nodiscard]] SpriteDrawTotals totals() const noexcept;
  // move-only (copy deleted)
};
```

- **`create(ctx, options)`** is the **set-up path** (renderer
  construction, before the frame pipeline starts — never inside a
  frame, PERF-002): validates the options (first failure wins, one
  rate-limited Warn `options_invalid`), requires a valid context
  (`makeCurrent`, idempotent), then compiles the sprite shader
  (vertex + fragment, one link), creates the quad VBO (32 B,
  `GL_STATIC_DRAW`), the per-frame instance buffer
  (`maxInstances * 52` B, `GL_DYNAMIC_DRAW`), and the VAO (quad
  attribute at divisor 0; the five instance attributes at divisor 1),
  and the atlas registry (`maxAtlases` × 8 B). Any GL failure returns
  `GlUnavailable` with ONE structured Error event (`program_creation_failed`
  or `resource_creation_failed`, with the GL error code + the sanitized
  info log) and no partial renderer (the failure is the stopped state).
  `Options::maxInstances` / `maxAtlases` domains:
  `[1, kSpriteRendererMaxInstances]` / `[1, kSpriteRendererMaxAtlases]`.
  The 4 096-slot registry (32 KB) is far beyond the PRD's 50k-sprite
  scene (≤ 30 draw calls = tens of atlases at most).
- **`bindAtlas(atlasId, w, h, rgba)`** is the **set-up/asset path**
  (one call per atlas per scene load; the asset system re-uploads them
  later, M3 — RENDER-004: no unexpected GPU work in the frame hot
  path). The context must be current on the calling thread. `atlasId ∈
  [0, maxAtlases)`, `w, h ∈ [1, capabilities().maxTextureSize]`,
  `rgba.size() == w*h*4`. Uploads GL_RGBA8, `GL_NEAREST`,
  `GL_CLAMP_TO_EDGE`, no mipmaps (the UV sub-rects are pixel-exact —
  the M2-GOLD-01 contract). Re-binding an id REPLACES the texture
  (the old one is deleted; the last bind wins). Validation failures:
  `InvalidArgument` (no GL work, no logging — the precondition
  contract); a GL upload failure: `GlUnavailable` + one Error
  (`atlas_upload_failed`).
- **`submit(batcher, worldToNdc)`** is the per-frame draw (the frame
  protocol: after the batch stage's `beginFrame()`/`add()`×n/`build()`).
  Preconditions, checked in order (first failure wins — a FAILED
  submit leaves the frame counters ZERO, the since-construction
  totals UNCHANGED, and draws nothing; the pass re-establishes its own
  GL state at the start of every successful submit, so a failure
  leaves no stale sprite-pass state):
  1. the renderer valid (stopped → `InvalidArgument`, no logging);
  2. the context valid + current (`makeCurrent`, idempotent; a
     cross-thread live takeover → `GlUnavailable`, the P0 EGL
     contract);
  3. the batcher built for the current frame (`batcher.frameBuilt()`
     — an open window with declared items → `InvalidArgument`:
     declared items are never silently dropped, CORE-008);
  4. the frame's instance count ≤ `maxInstances` (else
     `BudgetExhausted` + one rate-limited Warn `instance_capacity` —
     the budget, PERF-008);
  5. every group's atlas in the registry AND bound (else
     `InvalidArgument` — the stateless pre-state validation, one
     failure per frame; the batcher admits any u32 atlas id, so the
     registry range check precedes the registry read).

  Then: pack the frame's instances (group order = the batcher's
  published order; in-group = the back-to-front order) into the
  pre-allocated staging (52 B per instance — the items' floats are
  copied verbatim, no float arithmetic — the GPU owns the math,
  FR-2.2/PERF-003), ONE `glBufferSubData` upload, and the per-group
  state + draw. `worldToNdc` is the frame's combined world → NDC
  matrix (`IsoCamera::matrix()` — M2-CAM-02 — or the M2-PROJ-01 view's
  combined matrix; RENDER-006: the conversion boundary is the
  matrix). The batcher reference is non-const only because the
  batcher's pool accessors are (the M2-SPRITE-01 house quirk);
  `submit` mutates nothing on the batcher. A GL failure in the pass
  returns `GlUnavailable` + one Error (`submit_failed`, with the GL
  error code).
- **`frameStats()`** — the per-frame counters of the last SUCCESSFUL
  submit (a failed submit reads zero). **`totals()`** — the
  since-construction counters (successful submits only). Both O(1), no
  allocation.
- **`Options::primitiveQuery`** (off by default): the opt-in GL
  `PRIMITIVES_GENERATED` query around every submit — the GL-side
  dispatch cross-check + the M2-SPRITE-04 primitive feed. ON: one
  query object per submit + a `glFinish` — a CPU/GPU sync, a
  DIAGNOSTIC mode for the offscreen test/CI path and the profiler,
  never the shipping frame loop (RENDER-005). The 32-bit
  `glGetQueryObjectuiv` read caps the count at 2^32−1 primitives —
  beyond the realistic frame of 2^31 instances (2 per instance); the
  glad-generated `glQueryCounter` has a broken 2-argument signature
  in the vendored 2.0.8 loader, hence the 32-bit read.

## One draw per group (RENDER-001/003)

The renderer draws the batcher's groups IN THE Batcher's published
order — ascending (atlas, material, blend) — and each group's
instances IN THE Batcher's in-group back-to-front order (the
deterministic render order, RENDER-003). Per group, the state changes
only when the group DIFFERS from the previous group's state:

- **texture bind**: `glBindTexture` only when the group's atlas
  differs from the last bound atlas → counted in `textureBinds`;
- **blend function**: `glBlendFunc` only when the group's BlendMode
  differs from the last set function (Alpha:
  `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`; Additive: `ONE`/`ONE`) → counted
  in `blendChanges`;
- **instanced draw**: ONE `glDrawArraysInstanced(GL_TRIANGLE_STRIP,
  0, 4, n_group)` per group → counted in `drawCalls` / `instances`.

The last-atlas / last-blend state PERSISTS ACROSS FRAMES (the GPU's
state, not the engine's): frame 1 of a 3-group scene sets 3 blend
functions + 2 texture binds; every later frame sets 2 blend functions
(the first group matches the previous frame's last) + 2 binds. 100
frames: 300 draw calls, 100 000 instances, 200 texture binds, 201
blend changes — exact in `SpriteDrawPipeline`.

GL 3.3 core has no draw-call query primitive: the dispatch count is
the engine's own bookkeeping (`drawCalls` — what the M2-SPRITE-04
profiler ships), and the GL-side cross-check is the opt-in
`PRIMITIVES_GENERATED` count (`primitives` — 2 per instance of the
4-vertex strip: 2 000 for the 1 000-sprite scene).

## The shader and the pass's GL state

The whole M2 sprite feature is one minimal GLSL 3.30 program (no
geometry/TES, no samplers beyond the atlas, no uniforms beyond the
matrix + the atlas unit):

- **Vertex**: the unit quad (4-vertex strip, per-vertex `aCorner ∈
  [-0.5, 0.5]²`) is scaled by the per-instance WORLD-unit scale and
  translated to the per-instance world position (the scale is applied
  BEFORE the projection — the `SpriteItem.scale` contract), projected
  through `uWorldToNdc` (2D ground plane: z = 0), and the projected
  offset is rotated by the per-instance rotation IN SCREEN SPACE (NDC
  — the `SpriteItem.rotation` contract). The per-vertex UV is the
  per-instance UV sub-rect mapped onto the quad
  (`(-0.5,-0.5) → u0/v0`, `(0.5,0.5) → u1/v1`).
- **Fragment**: `texture(uAtlas, vUv) * vTint` (the multiplicative
  RGBA tint — the `SpriteItem.tint` contract), blended per the
  per-group BlendMode.

The pass's GL state model (what `submit` establishes per frame, in
order): the render-target frame buffer (`GlContext::frameBuffer()` —
the offscreen FBO on headless contexts; the surfaceless default frame
buffer is not a valid draw target), the viewport matched to the
render-target size (the driver default is 0×0 — every draw would
clip to nothing), the DEPTH TEST OFF (the painter's order is the
batcher's — the 2.5D depth is engine-owned, FR-2.2 / M2-ISO-01, never
derived from the projection), the BLEND ENABLED, the sprite program +
the per-frame `uWorldToNdc` uniform, and each group's atlas on
texture unit 0. After the pass (success or failure): the program and
the VAO are restored to 0 (the pass owns only its own program/VAO);
the blend function, the texture bind, the frame buffer, and the
viewport PERSIST (the next pass re-establishes what it needs — the
counters count only the changes that actually happen).

The GPU's cos/sin (the rotation) is driver-float: bit-exact across
runs of the same driver, not across drivers — the golden-image
contract (M2-GOLD-01) pins the environment; this step's tests verify
a frame against a CPU reference with a ±1-byte per-channel tolerance
(the GPU float32 vs the reference double) plus rounding-exact probe
pixels.

## Performance (PERF-002/003, RENDER-001 — DOC-004)

Per frame: **O(G × 5 + n) CPU work** (G = group count, n = frame
instance count): the per-group instance attribute-pointer setup (5
`glVertexAttribPointer` calls — the VAO captures them; no GPU work),
the state checks, ONE upload of `n × 52` B (`glBufferSubData`), and G
instanced draw calls — plus the per-frame state setup (frame buffer
bind, viewport, depth/blend, program + uniform: O(1)). **Zero
allocation**: the staging buffer (the frame budget), the instance
buffer, and the atlas registry are sized at create; the per-frame
pack is a contiguous float copy (the items' floats verbatim — no
arithmetic, the GPU owns the math).

The state changes are observable (RENDER-001 — the M2-SPRITE-04
profiler feed): `textureBinds`, `blendChanges`, `drawCalls`,
`instances`, `primitives` (query on only). The composite 50k
render-CPU budget (PRD §8.1) is measured with this stage at
M2-PERF-01; there is no standalone budgets.json entry here (the sort
cost is the `depth_sort_10k` budget — M2-SORT-01).

Traps:

- **The render target**: the pass binds `ctx.frameBuffer()` and sets
  the viewport per frame — on a surfaceless context the default frame
  buffer is not a valid draw target, and the driver's default
  viewport is 0×0 (a clear shows nothing wrong; only a draw exposes
  it).
- **The state persists**: the last atlas/blend from frame N-1 carry
  into frame N — the counters count real changes, so a
  multi-frame pipeline's per-frame counts differ from the first
  frame's (the `SpriteDrawPipeline` totals pin both).
- **`primitiveQuery` syncs**: the query read takes a `glFinish` —
  CPU/GPU serialization (RENDER-005). Diagnostic mode only; never
  enable it in the shipping frame loop.
- **A failed submit draws nothing and counts nothing** — but it also
  does not leave stale sprite-pass state (the next successful submit
  re-establishes everything).

## Ownership, lifetime, threading (CORE-009, CONC-001)

Move-only (copy deleted). The default state is **stopped** (the failed
create / moved-from): `valid()` is false, every operation fails
`InvalidArgument` with no logging, the counters read zero. The
renderer is created on ONE `GlContext` and holds a NON-OWNING pointer
to it: **the context must outlive the renderer** (the engine creates
the context first, destroys it last; destroying the renderer first is
the required shutdown order — the GPU resources are deleted through
the context in the destructor). ONE owner thread (the render
thread's submit stage — CONC-001); no locks, no atomics: the counters
are plain reads/writes on the owner thread (DBG-005: the profiler
reads them on the owner thread or from a snapshot taken there).
Presentation-only (ARCH-009): `submit()` reads the batcher's built
frame (presentation state) and the given matrix; it never mutates sim
state.

## Misuse warnings

- Create the renderer with `maxInstances >=` the batcher's
  `maxSprites` — a frame above the renderer's budget fails
  `BudgetExhausted` (the batcher's own overflow policy bounds the
  frame to ITS budget first).
- `bindAtlas` is the setup/asset path (one call per atlas per scene
  load), not a per-frame call: it creates a GL texture.
- `submit` requires the batcher BUILT for the current frame — an open
  window with declared items fails `InvalidArgument`; it is never
  drawn as an empty frame (CORE-008).
- The context must be valid; `submit` makes it current on the calling
  thread (idempotent — the M2-GL-02 onStart hook does it once, submit
  re-affirms it); a cross-thread live takeover fails `GlUnavailable`
  (the P0 EGL stack contract — release on the old thread first).

## Related

- `docs/api/sprite_batcher.md` — the declare/build half the renderer
  consumes (M2-SPRITE-01).
- `docs/api/depth_sort.md` — the back-to-front order (M2-SORT-01).
- `docs/api/iso_camera.md` — the world → NDC matrix (M2-CAM-02).
- `docs/api/gl_context.md` — the context, the FBO render target, the
  handoff protocol (M2-GL-01).
- `docs/api/frame_pipeline.md` — the submit stage's home, the frame
  handoff, the render thread (M2-GL-02).
- `docs/concepts/coordinates.md` — the 2.5D depth convention
  (FR-2.2), the NDC-z always-0 contract.
