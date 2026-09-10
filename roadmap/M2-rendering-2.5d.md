# M2 — 2.5D Rendering

**PRD:** §15 M2, §7.2 · **Duration:** 4–6 weeks
**Scope (PRD):** GL context, sprite batcher, **isometric depth keys + picking
(primary)**, depth sort, parallax, camera (incl. grid-snap iso presets), projection
modes, particles, text, basic UI.
**Exit criteria (PRD):** 50k sprites ≤ 30 draw calls on worst-case isometric scene;
isometric is the default template; all AC-4.x incl. AC-4.4 pass.

Rules specific to this milestone: isometric is the **primary projection** — every
visual acceptance test uses the reference isometric scene (M2-SCENE-01). No
simulation code may depend on the projection mode (AC-4.2). All rendering goes
through the batcher (S-5); the unsafe draw path does not exist yet (M4-UNSAFE-01).

---

## Decisions

- [ ] **M2-DEC-01 · Confirm 2:1 dimetric as template default (D-ISO)**
  - **Refs:** PRD §18.8, v0.2 note
  - **Depends:** —
  - **Scope:**
    - Confirm default projection for new projects: 2:1 dimetric (pixel-art standard) vs true 30°/60° iso. Both remain selectable per scene.
    - ADR `docs/decisions/000X-iso-default.md`.
  - **Verify:** ADR exists.
  - **Size:** docs only

- [ ] **M2-DEC-02 · Confirm minimal UI widget set (D-UI)**
  - **Refs:** PRD §18.5, FR-2.8
  - **Depends:** —
  - **Scope:**
    - Confirm the M2 retained UI widget list (default: panel, button, text, image, list, slider, input); anything extra is deferred and named here.
    - ADR `docs/decisions/000X-ui-scope.md`.
  - **Verify:** ADR exists; widget list in ADR == widget list implemented in M2-UI-01.
  - **Size:** docs only

## GL infrastructure

- [ ] **M2-GL-01 · Vendor GLFW + GLAD; context + headless CI rendering**
  - **Refs:** PRD §6 (OpenGL 3.3 core), §11 (GLFW, GLAD rows); AC-6.1/AC-6.2
  - **Depends:** M0-DEP-01
  - **Scope:**
    - Vendor GLFW and GLAD per PRD §11 (update `deps.lock`); `laige-render` module created (include-graph lint: render depends on core + sim, never reverse).
    - Context creation: OpenGL 3.3 core profile, version check, capability query, failure → `Status` (no silent fallback).
    - Headless CI rendering: Linux CI renders to an offscreen FBO (or EGL surfaceless where available) so GL tests run without a display; document the exact mechanism.
    - Unit tests: context creation on CI; missing GL 3.3 → clean error.
  - **Verify:** GL smoke test green in CI on all 3 P0 OSes (offscreen where headless); `deps.lock` hash check green.
  - **Size:** ~250 lines + wiring

- [ ] **M2-GL-02 · Render thread + frame pipeline handoff**
  - **Refs:** PRD §10.2 (render thread, lock-free handoff); CONC-002, CONC-006
  - **Depends:** M2-GL-01
  - **Scope:**
    - Render thread running the frame pipeline: (cull/batch → submit); frame descriptor handed off from the main/sim thread through a lock-free single-slot handoff (one consumer, one producer; document the synchronization argument per CONC-002).
    - Ordered shutdown: render thread joins on engine shutdown (CONC-006), idempotent.
    - Backpressure: if the render thread lags > 1 frame, drop the older frame (log event, rate-limited) — never queue unboundedly (PERF-008).
    - Integration test: 3000 frames headless (offscreen), no deadlock (TSan job), drop path exercised with an artificially slowed submit.
  - **Verify:** `ctest -R render_thread` green under TSan; frame-drop counter observable via profiler (M2-SPRITE-04 field).
  - **Size:** ~250 lines + tests

- [ ] **M2-GL-03 · Vendor GLM + camera/matrix utilities**
  - **Refs:** PRD §11 (GLM row — rendering side only)
  - **Depends:** M2-GL-01
  - **Scope:**
    - Vendor GLM (render-only; sim math stays engine math — enforced by include-graph lint rule "sim includes no GLM").
    - Matrix utilities: ortho, perspective, look-at, 2D-plane projection, and the isometric camera matrix builders (2:1 dimetric, 30°/60°, custom shear) as pure functions with unit tests.
  - **Verify:** `ctest -R matrices` green: iso matrix maps a known grid point to the expected screen position (hand-computed golden values).
  - **Size:** ~200 lines + tests

## Camera & projection

- [ ] **M2-CAM-01 · 3D camera core**
  - **Refs:** FR-2.4 (position, look-at, FOV/ortho, follow, shake, zoom, constraints)
  - **Depends:** M2-GL-03
  - **Scope:**
    - `laige::Camera`: position, look-at, ortho or perspective (FOV), zoom with clamping (min/max per scene config), rectangular bounds constraint (camera position clamped to a rect), smooth follow (target + lerp factor, documented), shake (bounded, decaying, deterministic given input).
    - Camera state is presentation-only (ARCH-009): it never mutates sim state.
    - Unit tests: zoom clamp exact at bounds; constraint keeps camera inside rect for adversarial inputs; shake decays to 0 within documented ticks; follow is deterministic given fixed input.
  - **Verify:** `ctest -R camera` green.
  - **Size:** ~250 lines + tests

- [ ] **M2-PROJ-01 · Projection modes + screen↔world transforms**
  - **Refs:** FR-2.5, FR-2.11 (base), PRD §4 (projection list)
  - **Depends:** M2-CAM-01
  - **Scope:**
    - `ProjectionMode` per scene/view: `iso`, `side_view`, `top_down`, `free_cinematic` (values documented; `iso` default).
    - Screen↔world transform API per mode: `world_to_screen(p2d, depth)`, `screen_to_world_ray(screen)` — pure functions, deterministic, O(1); side-view maps Y=height, top-down X/Y ground plane, free-cinematic uses the 3D camera ray.
    - No projection code in sim modules (AC-4.2 — include-graph lint rule added).
    - Unit tests: round-trip `screen_to_world(world_to_screen(p)) == p` within documented precision for all four modes.
  - **Verify:** `ctest -R projection` green; lint rule blocks a sim→render include.
  - **Size:** ~250 lines + tests

- [ ] **M2-CAM-02 · Isometric camera presets + grid-snap mode**
  - **Refs:** FR-2.4 (iso presets, grid-snap), PRD §4 (isometric first-class)
  - **Depends:** M2-DEC-01, M2-PROJ-01
  - **Scope:**
    - Iso presets: 2:1 dimetric (default per D-ISO), true 30°/60°, custom shear — one config value selects, matrix from M2-GL-03.
    - **Grid-snap camera mode**: camera position quantized to grid coordinates (the standard isometric game feel); snap happens on release (or continuously, documented choice); zoom is clamped to grid-aligned levels.
    - Unit tests: in grid-snap mode the camera always lands on a grid coordinate for any input; zoom levels are exactly the documented set.
  - **Verify:** `ctest -R iso_camera` green.
  - **Size:** ~200 lines + tests

## Isometric core (primary projection)

- [ ] **M2-ISO-01 · Isometric depth key computation**
  - **Refs:** FR-2.2, AC-4.4, S-5 (engine-owned depth); RENDER-003
  - **Depends:** M2-PROJ-01
  - **Scope:**
    - Deterministic 32-bit sortable depth key from axis-aligned world state: key = f(x, y, tile/step height, layer) — document the exact formula and quantization (CORE-005); computed from sim coordinates, **never** from screen space (PRD §4).
    - Keys sort back-to-front for all supported iso shears; equal keys break ties by (layer, depth, entity id) — explicit stable order (RENDER-003).
    - Unit tests: hand-computed golden keys for a small stepped-terrain scene; property test: on a 10k random scene, key order matches painter's-order expectation for a set of hand-checked overlapping pairs.
  - **Verify:** `ctest -R iso_depth_key` green; formula documented in `docs/concepts/coordinates.md` (created/updated here — ARCH-008).
  - **Size:** ~150 lines + tests

- [ ] **M2-ISO-02 · Depth key table + incremental updates**
  - **Refs:** FR-2.2 (precomputed, incremental update), §8.1 (≤ 0.2 ms for 10k dirty cells)
  - **Depends:** M2-ISO-01
  - **Scope:**
    - Per-scene-chunk depth key table (tile grid → key), built at scene load (headless-buildable: the table is sim-side data).
    - Incremental update API: tile height change → only affected cells recomputed (cell + documented neighborhood radius); no full rebuild.
    - Budget test: 10k dirty cells update ≤ 0.2 ms (`budgets.json` entry `iso_depth_rebuild`).
    - Zero per-update allocation (tables pre-sized per chunk, growth bounded + logged).
    - Unit tests: single-tile edit changes only documented cells; rebuild-from-scratch == incremental result (property test); budget test records baseline.
  - **Verify:** `ctest -R iso_depth_table` green; baseline recorded in `docs/benchmarks/baselines/`.
  - **Size:** ~250 lines + tests

- [ ] **M2-ISO-03 · Isometric picking (screen → grid cell)**
  - **Refs:** FR-2.11 (O(1), exact at all zoom), AC-4.4; PRD §4 (click-to-select/move)
  - **Depends:** M2-ISO-01, M2-CAM-02
  - **Scope:**
    - `screen_to_grid(screen, camera, grid_config)`: screen → ground plane → grid cell, O(1) inverse of the iso projection, exact at all supported zoom levels (integer grid cell, no float drift at cell boundaries — document the boundary rule).
    - Exposed in the safe API (S-5: users never write z-ordering/picking math).
    - Unit tests (golden): a set of hand-checked screen points at 4 zoom levels map to the expected cells; property test: `screen_to_grid(world_to_screen(cell_center)) == cell` for 10k random cells × zooms; boundary behavior documented + tested.
  - **Verify:** `ctest -R iso_picking` green; per-pick cost measured ≤ 0.01 ms (§8.1) and recorded.
  - **Size:** ~150 lines + tests

## Sorting & sprite batcher

- [ ] **M2-SORT-01 · Deterministic depth sort (radix/bucket)**
  - **Refs:** FR-2.2 (bucket/radix, no per-frame alloc), RENDER-003, AC-4.3
  - **Depends:** M2-ISO-01
  - **Scope:**
    - Sort 32-bit depth keys with a stable radix/bucket sort into pre-allocated storage (no per-frame allocation); stable tie-break preserved (RENDER-003).
    - Deterministic: same input multiset → same output order, always (property test with shuffled inputs).
    - Stress test: 10k sprites sorted per frame for 3000 frames, cost recorded (part of AC-4.3 at 60 FPS).
    - Unit tests: stability on equal keys; 0/1/all-equal key edge cases.
  - **Verify:** `ctest -R depth_sort` green; 10k-sort cost recorded in baseline.
  - **Size:** ~250 lines + tests

- [ ] **M2-SPRITE-01 · Sprite item + batcher API (declare, don't draw)**
  - **Refs:** FR-2.1 (batched quads, one draw call per (atlas, material, blend) group), S-5
  - **Depends:** M2-SORT-01
  - **Scope:**
    - `SpriteItem` (pool-backed): position (2D), depth key, UV sub-rect, rotation, scale, tint, blend mode, atlas/texture ref, per-sprite depth override (counted + warned per G-R11).
    - Batcher API: `batcher.add(item)` — declaration only; grouping by (atlas, material, blend); sorted order from M2-SORT-01 feeds instance order within a group.
    - No per-frame allocation: batches pre-sized with documented growth policy (overflow → drop oldest + warn, never grow unboundedly).
    - Unit tests: grouping correctness (N atlases × blends produce the exact group count); G-R11 warning fires for manual depth override.
  - **Verify:** `ctest -R batcher` green.
  - **Size:** ~300 lines + tests

- [ ] **M2-SPRITE-02 · GPU instanced draw + sprite shader**
  - **Refs:** FR-2.1 (GPU-instanced), RENDER-001
  - **Depends:** M2-SPRITE-01, M2-GL-01
  - **Scope:**
    - One instanced draw call per (atlas, material, blend) group per frame; minimal GLSL 3.30 sprite shader (per-instance: world pos, UV rect, rotation, scale, tint; texture from atlas bind).
    - State change count minimized and observable (texture binds, blend changes — profiler fields).
    - Headless/offscreen render of a 1000-sprite scene produces non-empty, correct frame (golden image, M2-GOLD-01).
    - Unit/integration test: 1 draw call per group verified via GL counter (dispatch count query) on the offscreen path.
  - **Verify:** `ctest -R sprite_draw` green (offscreen CI); draw-call count == group count in test log.
  - **Size:** ~300 lines + tests

- [ ] **M2-SPRITE-03 · Atlas UV frame animation hook**
  - **Refs:** FR-2.1 (atlas UV animation, sheet frames)
  - **Depends:** M2-SPRITE-02
  - **Scope:**
    - Sprite item accepts an animation frame index → UV sub-rect computed from the atlas frame layout (frame size, row/col, margins documented).
    - This is the hook M3 animation drives; for now it is data-driven (frame index set by caller).
    - Unit tests: frame index → UV rect exact for a documented atlas layout; out-of-range frame → documented error, never wrap silently.
  - **Verify:** `ctest -R sprite_frames` green.
  - **Size:** ~100 lines + tests

- [ ] **M2-SPRITE-04 · Render observability + draw-call budget (G-R2)**
  - **Refs:** RENDER-001 (state changes, draw submissions observable), PRD §9.3 G-R2
  - **Depends:** M2-SPRITE-02
  - **Scope:**
    - Profiler fields: draw calls, texture binds, instance count, state changes (blend/program), upload volume, VRAM estimate (texture memory), render-target use.
    - Per-pass draw-call cap (configurable, documented default); exceeding → structured warn + frame graph flag (G-R2).
    - Unit tests: counters match a known small scene exactly (10 sprites, 2 atlases, 2 blends → expected counts); cap warning fires at the configured number.
  - **Verify:** `ctest -R render_counters` green.
  - **Size:** ~150 lines + tests

## Tiles & parallax

- [ ] **M2-TILE-01 · Tilemap data + auto-depth from tile height**
  - **Refs:** FR-2.6 (chunks, per-tile depth/height, auto-depth), PRD §4 (isometric staple)
  - **Depends:** M2-ISO-02
  - **Scope:**
    - Tilemap: chunked grid of tiles; per-tile: texture id, depth/height value, animation id (data only here); **auto-depth**: a tile's Y-height feeds its depth key automatically (wires M2-ISO-02).
    - Rendering: static tile quads batched through the sprite batcher (tiles are sprites with a fixed frame); one tilemap renders in a bounded number of draw calls (per chunk group).
    - Data is headless-buildable (sim-side); rendering consumes it read-only (ARCH-009).
    - Unit tests: height change → depth table increment (M2-ISO-02 path); tile quad positions/depths golden-checked for a 4×4 chunk.
  - **Verify:** `ctest -R tilemap` green.
  - **Size:** ~250 lines + tests

- [ ] **M2-TILE-02 · Parallax tile layers + tile animation**
  - **Refs:** FR-2.6 (parallax tile layers, tile animation)
  - **Depends:** M2-TILE-01, M2-PAR-01
  - **Scope:**
    - Tilemap layers can be assigned a parallax layer (background/mid/foreground) — parallax factors apply per M2-PAR-01.
    - Tile animation: per-tile frame cycling (frame index advances per documented tick count), data-driven (animation editor control is M5).
    - Unit tests: animated tile cycles frames at the documented rate; parallax offset at a given camera position golden-checked.
  - **Verify:** `ctest -R tilemap_anim` green.
  - **Size:** ~150 lines + tests

- [ ] **M2-PAR-01 · Parallax layers**
  - **Refs:** FR-2.3 (named layers, factor, offset, UV scroll, blend)
  - **Depends:** M2-CAM-01
  - **Scope:**
    - Named parallax layers (bg/mid/fg + custom): parallax factor (0..1), offset, UV scroll (auto or manual), blend mode; layer render order documented (background first).
    - A layer is either image-backed (single texture) or tilemap-backed (M2-TILE-02); both render through the batcher.
    - Unit tests: offset at camera position p == factor × (p − center) + offset (exact formula tested); scroll wraps exactly at texture boundary.
  - **Verify:** `ctest -R parallax` green.
  - **Size:** ~150 lines + tests

## Particles

- [ ] **M2-PART-01 · Particle simulation (CPU, 2D + depth)**
  - **Refs:** FR-2.7 (CPU-simulated, budgeted, pooled)
  - **Depends:** M1-ECS-03
  - **Scope:**
    - Particle pool (bounded, config max), emitters: burst + continuous; particle state: 2D position + depth, velocity, life, size, color fade; update in the sim tick (deterministic, engine math).
    - Budget: max particles config; overflow → drop (warn once per frame, rate-limited).
    - Unit tests: deterministic emitter (fixed seed) produces identical particle trajectories; pool exhaustion behavior exact.
  - **Verify:** `ctest -R particles` green.
  - **Size:** ~250 lines + tests

- [ ] **M2-PART-02 · Particle rendering through the batcher**
  - **Refs:** FR-2.7, RENDER-001
  - **Depends:** M2-PART-01, M2-SPRITE-02
  - **Scope:**
    - Particles render as batched sprites (shared particle atlas, one draw call for all particles of one emitter set); depth from particle depth value.
    - No per-frame allocation; particle→sprite conversion is an O(n) pass measured in the budget suite.
    - Unit test: 10k particles → exactly 1 draw call (same atlas/blend), cost recorded.
  - **Verify:** `ctest -R particle_render` green; cost recorded in baseline.
  - **Size:** ~150 lines + tests

## Text & UI

- [ ] **M2-TEXT-01 · Font rasterization (bitmap, P0)**
  - **Refs:** FR-2.8 (bitmap P0; SDF is P1 → M9), PRD §11 (stb_truetype)
  - **Depends:** M2-GL-01
  - **Scope:**
    - Vendor stb_truetype (deps.lock); rasterize the configured font + glyph set (default Latin-1 + documented extension) into a bitmap glyph atlas (fixed size, no runtime re-rasterization).
    - Glyph metrics API: advance, bearing, line height — documented units (px at 1x, scale factor documented).
    - Unit tests: atlas generation deterministic (same font+size → identical atlas bytes); missing glyph → documented fallback glyph, never crash.
  - **Verify:** `ctest -R font` green.
  - **Size:** ~200 lines + tests

- [ ] **M2-TEXT-02 · Text items in the UI pass**
  - **Refs:** FR-2.8 (text widget, screen-space pass)
  - **Depends:** M2-TEXT-01, M2-SPRITE-02
  - **Scope:**
    - `TextItem` (UI pass, screen-space): string (from string table — no per-frame `std::string` in the path, PRD §10.4), font, size, color, alignment, max-width wrap.
    - Text layout: measured widths via glyph advances (O(glyphs)); rendered as batched quads in the UI pass.
    - Unit tests: width measurement golden-checked for known strings; wrap at max-width exact; empty/oversized string behavior documented.
  - **Verify:** `ctest -R text_items` green.
  - **Size:** ~200 lines + tests

- [ ] **M2-UI-01 · UI widget tree (retained, screen-space)**
  - **Refs:** FR-2.8 (panel, button, text, image, list, slider, input), FR-4.4 (UI input routing base)
  - **Depends:** M2-DEC-02, M2-TEXT-02, M2-SPRITE-02
  - **Scope:**
    - Retained widget tree (pool-backed nodes, stable widget ids): the confirmed widget set from D-UI; layout (anchors, simple box layout — document the layout model); focus system (focusable widgets, tab/pointer focus, UI-first routing hook — raw pointer state for now, action-based routing in M3-INPUT-04).
    - Events: pointer enter/leave/press/release, button pressed, slider changed, input text — delivered as a per-frame event list (bounded, pooled).
    - No rendering here (M2-UI-02); no sim coupling (ARCH-009: UI never mutates sim state directly — it emits events).
    - Unit tests: tree CRUD + layout golden checks; focus traversal order deterministic; event delivery for scripted pointer sequences.
  - **Verify:** `ctest -R ui_tree` green.
  - **Size:** ~400 lines + tests (largest UI step; split into layout / focus if needed)

- [ ] **M2-UI-02 · UI rendering pass**
  - **Refs:** FR-2.8 (separate render pass, screen-space)
  - **Depends:** M2-UI-01, M2-SPRITE-02
  - **Scope:**
    - UI render pass: screen-space (identity camera), widgets rendered as batched quads + text; one texture per atlas (UI atlas); no world transform leaks (RENDER-006).
    - Visible-widget culling (offscreen + occluded by opaque panels — document the simple model).
    - Golden image test fixture: a fixed UI scene renders to the committed reference image (M2-GOLD-01).
  - **Verify:** `ctest -R ui_render` green (offscreen); golden image matches within documented tolerance.
  - **Size:** ~250 lines + tests

## Reference scene & performance acceptance

- [ ] **M2-SCENE-01 · Reference isometric scene generator**
  - **Refs:** PRD §4 (reference scene for all visual/perf tests), §8.1
  - **Depends:** M2-TILE-01, M2-SPRITE-01, M2-PART-01, M2-UI-01
  - **Scope:**
    - `laige-scene-iso` headless tool: generates the **worst-case isometric reference scene** deterministically from a seed — 50k overlapping sprites (random positions over a tiled ground with stepped heights), 3 parallax layers, particle emitters, UI panel, tile-height terrain; emits the scene as data the engine can load (full scene format lands M3; for now an in-memory builder + simple binary serialization used by tests).
    - Output is identical across runs/builds for a given seed (determinism — it is a test fixture, ARCH-010).
    - Also emits a small 1k-sprite variant for fast unit tests.
  - **Verify:** tool runs in < 5 s; two invocations with the same seed produce byte-identical output (tested); scene loads and batches without error.
  - **Size:** ~250 lines + tests

- [ ] **M2-PERF-01 · 50k-sprite budget test (PRD §8.1)**
  - **Refs:** PRD §8.1 (≤ 30 draw calls; ≤ 2 ms CPU; zero per-frame alloc in batcher), §15 M2 exit
  - **Depends:** M2-SCENE-01, M2-SPRITE-04, M2-PART-02, M2-UI-02
  - **Scope:**
    - `laige-bench --suite=render-50k`: render the full reference scene (50k sprites + 3 parallax layers + particles + UI) for 3000 frames offscreen; measure draw calls/frame, batcher CPU, per-frame allocs (must be 0 — M1-ALLOC-01 assertion extended to the batcher).
    - Budget entries `render_50k_drawcalls` (≤ 30), `render_50k_cpu` (≤ 2 ms) bound in `budgets.json`; baseline recorded with full AGENTS §12 metadata.
  - **Verify:** suite passes all three budgets on the CI reference machine; baseline file committed.
  - **Size:** ~200 lines + baseline

## Render profiling & acceptance

- [ ] **M2-PROF-01 · CPU/GPU render timing (independent)**
  - **Refs:** RENDER-005 (CPU/GPU measured independently), FR-11.1
  - **Depends:** M2-SPRITE-04, M2-GL-02
  - **Scope:**
    - CPU: per-pass CPU time (batch, sort, submit) from the render thread timers.
    - GPU: per-pass GPU time via GL sync objects where supported (desktop; headless/offscreen fallback documented — CPU time + "GPU n/a" field, never fake numbers); VRAM estimate from texture allocation tracking.
    - Stall detection: CPU waiting on GPU submit (implicit sync) measured and logged (rate-limited).
    - Unit/integration test: with a deliberately slow shader (fixture), GPU time > CPU time is observed on the desktop CI runner.
  - **Verify:** `ctest -R render_timing` green; fields present in profiler snapshot on all P0 OSes (GPU fields may read "n/a" headless — documented).
  - **Size:** ~250 lines + tests

- [ ] **M2-GOLD-01 · Golden image test infrastructure**
  - **Refs:** TEST-008 (golden tests, explicit tolerances)
  - **Depends:** M2-SPRITE-02
  - **Scope:**
    - Offscreen FBO capture + compare utility: fixed seed, fixed resolution (1280×720), fixed camera; compare captured frame to committed reference PNG with per-pixel tolerance (documented: e.g. max channel diff ≤ 2 on a 255 scale, 0.001% pixel budget) — tolerance is explicit in config, not hidden.
    - `laige-golden` tool: `--update` regenerates references (human-approved), default mode compares and lists failing pixels.
    - Wire the first two fixtures: sprite scene (M2-SPRITE-02) and UI scene (M2-UI-02).
  - **Verify:** `ctest -R golden` green on CI (deterministic offscreen render — if a driver breaks determinism, the CI job pins the failing environment and the step is reopened; no silent tolerance inflation).
  - **Size:** ~250 lines + tests

- [ ] **M2-AC-01 · AC-4.x acceptance suite**
  - **Refs:** PRD §4 AC-4.1 … AC-4.4
  - **Depends:** all M2 functional steps
  - **Scope:**
    - **AC-4.1:** one engine build runs both a side-view (platformer-style) and an isometric scene, differing **only** in config (two fixture configs, same binary, both render offscreen + golden-checked).
    - **AC-4.2:** proof no sim/physics/pathing/network code depends on projection mode: include-graph lint rule + surface check (no `ProjectionMode` in `laige-sim`/`laige-net` public surface) — automated in CI.
    - **AC-4.3:** 10k overlapping sprites sorted + drawn at 60 FPS on CI reference hardware (dedicated suite `render-sort-10k`).
    - **AC-4.4:** iso picking exact at all zoom levels (M2-ISO-03 tests) + depth-key rebuild after terrain edit within §8.1 budget (M2-ISO-02 tests) + isometric is the template default (checked in M2-SAMPLE-01 config).
    - Suite wired into CI as `laige-bench --suite=ac4` + `ctest -R ac4`.
  - **Verify:** `ctest -R ac4` and the AC-4 bench green; AC checklist recorded in `docs/benchmarks/baselines/m2-ac4.md`.
  - **Size:** test wiring + ~150 lines

## Template

- [ ] **M2-SAMPLE-01 · `iso-arena.laige` template (isometric default)**
  - **Refs:** NFR-13.5, PRD v0.2 (isometric default template), §4 AC-4.4
  - **Depends:** M2-AC-01
  - **Scope:**
    - `samples/iso-arena/`: small isometric scene — tile ground with a height step, 3 sprites, grid-snap camera, screen→grid picking prints the picked cell, 2:1 dimetric default (per D-ISO); **≤ 200 lines of game code**, commented.
    - Config sets projection=iso explicitly (the default for new projects — M2-AC-01 checks this).
    - Runs headless (offscreen) in CI and windowed locally; renders in < 2 s cold start (PRD §8.1 — measured on CI desktop job).
  - **Verify:** CI builds + runs it (headless, 100 frames, picking test case prints expected cell); cold-start time recorded in baseline.
  - **Size:** ~250 lines (sample + wiring)

## Milestone gate

- [ ] **M2-EXIT-01 · M2 exit gate**
  - **Refs:** PRD §15 M2 exit criteria
  - **Depends:** all other M2 steps
  - **Scope:**
    - Confirm and record: (1) 50k-sprite budget green (M2-PERF-01 report link), (2) isometric is the default template (sample + config check link), (3) all AC-4.x pass (M2-AC-01 report link), (4) zero per-frame allocation in the render batcher (M1-ALLOC-01 assertion extended, link).
    - Update Progress Board.
  - **Verify:** all evidence links present; no open M2 step.
  - **Size:** docs only
