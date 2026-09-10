# M3 — Game Feel

**PRD:** §15 M3, §7.3–§7.7 · **Duration:** 4–6 weeks
**Scope (PRD):** Physics (incl. CCD, joints), input actions, audio, sprite-sheet
animation + state machines, tilemaps (height brush), asset import/atlas CLI.
**Exit criteria (PRD):** Physics budgets + determinism AC; playable **isometric**
sample in-engine.

Rules specific to this milestone: physics is 2D and deterministic (FR-3.3, §10.3);
CCD/joints are scoped per the PRD risk table (cut joints to P1 only if budget
evidence says so — that is a decision step, not a silent cut). The asset pipeline is
headless (`laige-asset`) and runs in CI. Everything here must keep sim-loop
allocations at 0 (M1-ALLOC-01 assertion).

---

## Physics

- [ ] **M3-PHYS-01 · Rigid bodies: state, types, shapes (circle/point)**
  - **Refs:** FR-3.1 (kinematic/dynamic/static; point & circle); FR-3.3
  - **Depends:** M1-ECS-03, M0-CORE-04
  - **Scope:**
    - `RigidBody` component: position (2D), velocity, angular velocity, mass/inverse mass, inertia, body type (kinematic/dynamic/static); shapes: point, circle.
    - Body creation API (pool-backed, no per-op heap); mass from shape + density (formula documented).
    - Deterministic: all math via engine ops / `fpx16_16` per D-MATH.
    - Unit tests: creation/validation (negative mass → error), shape invariants, mass formulas golden-checked.
  - **Verify:** `ctest -R phys_bodies` green.
  - **Size:** ~250 lines + tests

- [ ] **M3-PHYS-02 · Spatial hash broadphase**
  - **Refs:** FR-3.2 (spatial-hash broadphase, engine-managed, auto-sized); SCALE-001
  - **Depends:** M3-PHYS-01
  - **Scope:**
    - Spatial hash over dynamic bodies: cell size derived from average body radius (auto-sized, documented formula); rebuild policy (per-tick rebuild vs incremental — documented choice with measured cost).
    - Deterministic: cell traversal order fixed; insertion uses deterministic hash (no `unordered_map` — PERF-004/§10.3; open-addressing table with documented order).
    - Queries: `overlap(shape, mask)` → candidate pair list (deduplicated, ordered).
    - Zero per-tick allocation (table pre-sized; growth bounded + logged).
    - Unit tests: candidate set correct for known layouts; pair dedup; rebuild cost recorded for 10k bodies; deterministic across two runs.
  - **Verify:** `ctest -R phys_broadphase` green; rebuild cost baseline recorded.
  - **Size:** ~300 lines + tests

- [ ] **M3-PHYS-03 · Narrowphase: circle-circle + contacts**
  - **Refs:** FR-3.2 (narrowphase, contacts with manifolds)
  - **Depends:** M3-PHYS-02
  - **Scope:**
    - Circle-circle narrowphase: penetration depth, contact normal (deterministic tie-break when centers equal — documented), contact manifold (1 point + normal + depth).
    - Contact pool (bounded, per-tick reset — no heap).
    - Unit tests: tangent (zero penetration → no contact), overlapping, contained, equal-center case, golden manifolds.
  - **Verify:** `ctest -R phys_narrow_circle` green.
  - **Size:** ~150 lines + tests

- [ ] **M3-PHYS-04 · Convex polygon shapes + polygon-polygon SAT**
  - **Refs:** FR-3.1 (convex polygon, composite), FR-3.2 (SAT/Minkowski)
  - **Depends:** M3-PHYS-03
  - **Scope:**
    - Convex polygon shape (vertices CCW, ≤ 8 verts documented limit, auto-convex-hull on import with degenerate input rejected); composite shape = list of circles/polygons (≤ 4 documented).
    - Polygon-polygon SAT: support functions, min-overlap axis, manifold (1 or 2 points), deterministic axis selection (first-min by vertex index).
    - Unit tests: hull construction (degenerate/collinear inputs rejected), SAT golden cases (edge-face, corner-edge, corner-corner, parallel edges), composite queries.
  - **Verify:** `ctest -R phys_narrow_poly` green.
  - **Size:** ~400 lines + tests (largest physics step; split into shape / SAT if needed)

- [ ] **M3-PHYS-05 · Narrowphase: circle-polygon**
  - **Refs:** FR-3.2 (SAT/Minkowski)
  - **Depends:** M3-PHYS-03, M3-PHYS-04
  - **Scope:**
    - Circle-vs-convex-polygon: closest point on polygon (edge/corner), manifold with up to 2 contact points (corner case), deterministic.
    - Unit tests: center-inside polygon, per-edge contacts, corner contacts (2-point manifold), tangent, golden cases.
  - **Verify:** `ctest -R phys_narrow_circle_poly` green.
  - **Size:** ~250 lines + tests

- [ ] **M3-PHYS-06 · Integration: semi-implicit Euler + sleep**
  - **Refs:** FR-3.3 (fixed tick, fully deterministic); FR-3.6 (no variable step); SCALE-001 (sleeping)
  - **Depends:** M3-PHYS-03
  - **Scope:**
    - Integration in the physics system (fixed tick only — **no variable-step API exists**, FR-3.6): `v += a·dt; x += v·dt` with documented dt = tick rate inverse; gravity + damping (config, documented units).
    - Contact resolution: position correction (Baumgarte split documented: percent + slop, values named) + velocity impulse; solver iteration count fixed (config, default documented).
    - Sleep: resting bodies (velocity < ε for N ticks, ε named) sleep; wake on contact/force; sleeping bodies skip integration (deterministic wake order documented).
    - Unit tests: free fall matches analytic solution exactly (fixed-point bit-exact); two-body stack stable (no jitter beyond documented tolerance over 600 ticks); sleep/wake correctness.
  - **Verify:** `ctest -R phys_integrate` green; determinism: same scene, two runs → identical per-tick hashes (uses M1-DET-03 hashing).
  - **Size:** ~350 lines + tests

- [ ] **M3-PHYS-07 · Continuous collision detection (CCD)**
  - **Refs:** FR-3.2 (CCD for fast movers)
  - **Depends:** M3-PHYS-06
  - **Scope:**
    - CCD for flagged bodies (`ccd_enabled` — player-critical bodies): swept circle vs static shapes within the tick interval (TOI solve); body is moved to the earliest TOI, then remaining time integrated normally (documented).
    - Bounded work: CCD only for flagged bodies; per-tick CCD pair count capped (cap named, overflow → warn, never unbounded).
    - Unit tests: fast bullet (10 cells/tick) does not tunnel through a 1-cell wall; TOI exact for head-on and grazing; capped overflow warns exactly once.
  - **Verify:** `ctest -R phys_ccd` green.
  - **Size:** ~300 lines + tests

- [ ] **M3-PHYS-08 · Joints: distance + hinge**
  - **Refs:** FR-3.4 (deterministic solver, fixed iteration)
  - **Depends:** M3-PHYS-06
  - **Scope:**
    - Joint base (deterministic solve inside the solver loop, fixed iteration count with the contact solver); **distance joint** (max-length clamp + optional spring) and **hinge joint** (pin + optional motor speed limit, motor torque config documented).
    - Joint pool, per-tick reset; joint break force (config, documented units) → joint removed + event emitted.
    - Unit tests: pendulum (hinge + gravity) matches expected period within documented tolerance; distance joint holds two bodies at rest; break force removes joint deterministically.
  - **Verify:** `ctest -R phys_joints_1` green.
  - **Size:** ~300 lines + tests

- [ ] **M3-PHYS-09 · Joints: slider, weld, motor**
  - **Refs:** FR-3.4 (slider, weld, motor — enough for vehicles/cranes/doors)
  - **Depends:** M3-PHYS-08
  - **Scope:**
    - **Slider joint** (axis-locked translation, range limits), **weld joint** (freezes relative pose; break force), **motor joint** (drive angular velocity to target within torque limit) — same solver, same determinism guarantees.
    - Unit tests: slider body moves only along axis (perpendicular velocity == 0, exact in fixed-point); weld holds pose under load then breaks at the documented force; motor reaches target speed and holds (no overshoot beyond documented tolerance).
    - Decision point (documented here, not silent): if these three joints push the M3 budget, scope them to P1 per the PRD risk table — record the decision with the measurement in `docs/decisions/`.
  - **Verify:** `ctest -R phys_joints_2` green.
  - **Size:** ~350 lines + tests

- [ ] **M3-PHYS-10 · Collision layers/masks + query APIs**
  - **Refs:** FR-3.5 (layers & masks; raycast, overlap, sweep)
  - **Depends:** M3-PHYS-02
  - **Scope:**
    - Collision layer/mask matrix (16×16 documented size, config file + API); pairs filtered before narrowphase.
    - Query APIs (all spatial-hash-bounded, cost documented per API-007): `raycast(origin, dir, mask)`, `overlap(shape, mask)`, `sweep(shape, from, to, mask)` — deterministic result order (nearest-first, ties by body id).
    - Unit tests: matrix filtering exact; raycast hit order golden; sweep detects tunneling cases that per-step overlap misses.
  - **Verify:** `ctest -R phys_queries` green.
  - **Size:** ~250 lines + tests

- [ ] **M3-PHYS-11 · Fixed-point physics path (bit-exact)**
  - **Refs:** FR-3.3 (Q16.16 option for lockstep), PRD §10.3, AC-10.3 (later consumer)
  - **Depends:** M3-PHYS-06, M0-CORE-04
  - **Scope:**
    - Physics core runs in `fpx16_16` when `deterministic_fixed_point` config is on (the lockstep/MMO path): all positions/velocities/impulses in fixed-point; float path remains for non-deterministic/debug use (documented which is which).
    - No precision loss beyond documented bounds: max position/velocity in Q16.16 documented (world units range), overflow → saturation + **error event** (never silent).
    - Determinism test: 60 s of physics (100 bodies, joints, CCD) → identical state hashes on Debug+ASan and Release builds (local; CI two-compiler job extends M1-DET-04 to physics).
  - **Verify:** `ctest -R phys_fixedpoint` green; bit-exact test passes; range/overflow behavior tested.
  - **Size:** ~300 lines + tests

- [ ] **M3-PHYS-12 · Physics budget + fuzz + no-variable-step proof**
  - **Refs:** FR-3.6, PRD §8.1 (10k dynamic bodies @ 60 Hz ≤ 1.5 ms; zero per-tick alloc), NFR-8.7
  - **Depends:** M3-PHYS-11, M3-PHYS-10
  - **Scope:**
    - `laige-bench --suite=phys-10k`: 10k dynamic bodies (mix of sleeping/awake), 60 Hz, 3000 ticks; budget entry `phys_10k_tick` (≤ 1.5 ms); zero per-tick allocation asserted (M1-ALLOC-01).
    - Fuzz targets: malformed shapes (self-intersecting, > max verts, NaN inputs rejected at API), extreme velocities (1e6), contact storms (all bodies overlapping) — no crash, no hang (timeout guard), no UB (ASan).
    - API surface proof for FR-3.6: static-check test asserting no public symbol in `laige-sim` exposes variable-step stepping (grep/lint over public headers).
  - **Verify:** suite green on CI reference machine; fuzz `--runs=1000` clean per target; API-surface test green.
  - **Size:** ~250 lines + fuzz + baseline

- [ ] **M3-PHYS-13 · Physics ↔ ECS integration**
  - **Refs:** FR-3.x, ARCH-009 (presentation never mutates sim)
  - **Depends:** M3-PHYS-12
  - **Scope:**
    - Physics as a declared system in the scheduler (budget from config; G-R5 applies): reads/writes `RigidBody` only; contact events emitted to an event queue (consumed by user systems — e.g. damage, footstep triggers).
    - Kinematic body API: user systems set kinematic positions (for moving platforms/doors) — the standard 2.5D pattern.
    - Integration tests: contact event fires exactly once per contact onset (no per-tick spam); kinematic platform carries a dynamic body (classic test).
  - **Verify:** `ctest -R phys_integration` green.
  - **Size:** ~200 lines + tests

## Input

- [ ] **M3-INPUT-01 · GLFW input backend (keyboard, mouse, gamepad)**
  - **Refs:** FR-4.1 (keyboard, mouse, gamepad; touch is P2)
  - **Depends:** M2-GL-01
  - **Scope:**
    - Poll GLFW state into engine input state each frame: keyboard keys, mouse (position, buttons, scroll), gamepad (axes, buttons — GLFW joystick); device connect/disconnect events.
    - Input state is a per-frame snapshot (bounded, no heap); backend abstraction so headless tests can feed synthetic input (no GLFW needed for tests).
    - Unit/integration tests: synthetic backend drives the same code path as GLFW (parity test); connect/disconnect events delivered.
  - **Verify:** `ctest -R input_backend` green; GLFW path smoke-tested on desktop CI job.
  - **Size:** ~250 lines + tests

- [ ] **M3-INPUT-02 · Action abstraction + remapping**
  - **Refs:** FR-4.2 (named actions, runtime remapping, per-profile save, deadzone)
  - **Depends:** M3-INPUT-01
  - **Scope:**
    - `ActionMap`: named actions → bindings (key/mouse/gamepad buttons, axes); digital + axis actions; per-axis deadzone (config, named); runtime remapping API; save/load profiles to JSON (versioned, validated).
    - Action evaluation per frame: `action_value(name)` (axis, signed, clamped [-1,1]) and `action_pressed/released` (edge events).
    - Unit tests: deadzone exact at boundary; axis + digital combined; remap takes effect next frame (no mid-frame staleness); profile round-trip.
  - **Verify:** `ctest -R input_actions` green.
  - **Size:** ~250 lines + tests

- [ ] **M3-INPUT-03 · Per-tick input frames (deterministic)**
  - **Refs:** FR-4.3 (per-tick sampled frame; consumed as events, not polled); PRD §10.3
  - **Depends:** M3-INPUT-02, M1-DET-02
  - **Scope:**
    - At each sim tick, the frame loop samples the action state into an `InputFrame` (pooled, bounded: N actions, documented capacity; overflow → drop + warn).
    - `InputFrame` is part of the replay log (replaces the M1 opaque blob with a concrete format — versioned per ARCH-007) and the sole input source for simulation (systems read the current tick's frame; no arbitrary polling of the backend in sim).
    - Deterministic: same raw events + same tick boundaries → identical InputFrames (test with synthetic input at sub-tick timing edges).
    - Unit tests: event timing across tick boundaries (press at 99.9% of tick → which frame? documented + tested); replay round-trip of input frames.
  - **Verify:** `ctest -R input_frames` green; replay with recorded inputs reproduces hashes (extends M1-DET-04 scenario).
  - **Size:** ~250 lines + tests

- [ ] **M3-INPUT-04 · UI-first input routing**
  - **Refs:** FR-4.4 (input routes to UI first, then world)
  - **Depends:** M3-INPUT-03, M2-UI-01
  - **Scope:**
    - Routing: each frame, pointer/keyboard events go to the focused UI widget if one exists and consumes them; unconsumed events reach the world (sim input). Focus enter/exit emits events (UI gains/loses focus — e.g. open menu disables world input for mapped actions, documented).
    - Deterministic routing order documented; no event duplicated to both (consumption is exclusive, documented exception list for pass-through keys like ESC).
    - Unit tests: with focused input widget, typing goes to the widget and not to a bound world action; ESC closes UI and re-enables world actions; no double-consumption (counted in tests).
  - **Verify:** `ctest -R input_routing` green.
  - **Size:** ~150 lines + tests

## Audio

- [ ] **M3-AUDIO-01 · Vendor miniaudio + bus structure**
  - **Refs:** FR-5.1 (buses: master/music/SFX/voice; per-bus volume/filters), PRD §11
  - **Depends:** M0-DEP-01
  - **Scope:**
    - Vendor miniaudio (deps.lock); device init/shutdown (ordered, graceful on device loss — degradation per NFR-8.5: audio-off mode, logged); headless mode: no device opened (FR-1.6) with a null backend.
    - Bus tree: master → {music, sfx, voice}; per-bus volume (0..1, named units), per-bus enable, simple lowpass filter (P0 filter set documented); mix order documented.
    - Unit tests (null backend): bus volume math exact; disable propagation; device-loss path on a fake backend → degraded mode, no crash.
  - **Verify:** `ctest -R audio_buses` green; headless engine runs without audio (existing M1 test extended).
  - **Size:** ~300 lines + tests

- [ ] **M3-AUDIO-02 · Pooled sources: music + SFX**
  - **Refs:** FR-5.1 (pooled sources)
  - **Depends:** M3-AUDIO-01
  - **Scope:**
    - Source pool (bounded, per-bus counts documented); `play_sfx(ref)` / `play_music(ref, loop)` / `stop`; pool exhaustion → steal least-recently-used SFX (music never stolen; documented) + warn.
    - Playback params: volume, loop, fade in/out (ms, documented).
    - Unit tests (null backend): pool counts exact; steal behavior; fade values at sampled times.
  - **Verify:** `ctest -R audio_sources` green.
  - **Size:** ~200 lines + tests

- [ ] **M3-AUDIO-03 · 2D positional audio**
  - **Refs:** FR-5.2 (pan by angle, falloff by distance, occlusion flags)
  - **Depends:** M3-AUDIO-02
  - **Scope:**
    - Positional source: relative to a listener (entity or world point); pan by angle (stereo), falloff by distance (inverse-distance/linear curves documented), min/max distance (clamped, named), occlusion flag (user-set per source, attenuates by documented amount).
    - Update cost: O(sources) per tick, bounded by pool; zero allocation.
    - Unit tests: pan at ±90° golden; falloff exact at min/mid/max distances; occlusion attenuation exact.
  - **Verify:** `ctest -R audio_positional` green.
  - **Size:** ~200 lines + tests

- [ ] **M3-AUDIO-04 · Import + streaming (WAV/OGG/FLAC/MP3)**
  - **Refs:** FR-5.3 (bounded-memory streaming), FR-5.4 (lossless/lossy import)
  - **Depends:** M3-AUDIO-01
  - **Scope:**
    - Import via miniaudio import API: WAV (PCM), OGG, FLAC, MP3 → engine audio asset format (decoded PCM, documented sample rate/mono-stereo policy); validation on import (bad file → import failure, never runtime surprise — FR-7.6).
    - Streaming: assets above a size threshold stream from the pack (bounded ring buffer, size documented); underrun counted (profiler field).
    - Unit tests: each format decodes a fixture to identical PCM (cross-format golden on the same source); truncated/corrupt file → import error; streaming underrun counter increments under a slowed reader.
  - **Verify:** `ctest -R audio_import` green; fuzz `audio_decode` clean (registered target, M3-ASSET-08).
  - **Size:** ~250 lines + tests

- [ ] **M3-AUDIO-05 · Audio observability + budget**
  - **Refs:** DBG-008 (audio voices, underruns), FR-11.1
  - **Depends:** M3-AUDIO-04
  - **Scope:**
    - Profiler fields: active voices, virtualized/stealed count, per-bus level (current mix), streaming buffer fill, underruns, audio CPU time (mix cost).
    - Mix budget: total mix work measured; over budget → documented degradation (drop voice-bus effects first — order documented) + log event.
    - Unit test: fields exact for a scripted playback sequence.
  - **Verify:** `ctest -R audio_obs` green.
  - **Size:** ~150 lines + tests

## Animation

- [ ] **M3-ANIM-01 · Sprite-sheet animation (frame-based)**
  - **Refs:** FR-6.1 (frame-based per sprite, state machine or free sequencing)
  - **Depends:** M2-SPRITE-03, M1-ECS-03
  - **Scope:**
    - `AnimSheet` component: frames (atlas frame ids + durations, documented units in ticks), play modes (once/loop/pingpong), speed scale.
    - `AnimPlayer` system (budgeted): advances frame per tick, writes the current frame index into the sprite (via M2-SPRITE-03 hook); finished event emitted (once mode).
    - Deterministic: frame at tick t is a pure function of (frames, durations, start tick, speed) — tested.
    - Unit tests: boundary ticks (frame change exactly at the documented tick); pingpong sequence golden; finished event exactly once.
  - **Verify:** `ctest -R anim_sheet` green.
  - **Size:** ~200 lines + tests

- [ ] **M3-ANIM-02 · Animation state machine (sheet-based)**
  - **Refs:** FR-6.3 (node/transition/parameter; per-entity instances) — sheet-based P0
  - **Depends:** M3-ANIM-01
  - **Scope:**
    - ASM: nodes (each runs an `AnimSheet` or is idle), transitions (condition on parameters — integer/enum parameters only in P0, documented), parameters per entity instance; per-entity instance state in an ECS component (pool-backed).
    - Transition rules: no mid-frame re-entry (evaluated once per tick, order documented); guard + trigger model documented (trigger fires once).
    - Unit tests: a 4-node FSM (idle/run/attack/attack2) with parameter conditions walks all paths deterministically; trigger-once verified; instance isolation (two entities, same FSM, independent params).
  - **Verify:** `ctest -R anim_fsm` green.
  - **Size:** ~300 lines + tests

- [ ] **M3-ANIM-03 · Animation ↔ simulation contract (one-way events)**
  - **Refs:** FR-6.4 (one-way event API; animation never mutates physics)
  - **Depends:** M3-ANIM-02, M3-PHYS-13
  - **Scope:**
    - Event API: animation systems emit events (frame reached, clip finished, trigger fired — named event ids) into the M3-PHYS-13 event queue pattern; simulation/user systems consume them (footstep SFX, attack hit windows).
    - Architecture test: no animation system holds a write access to `RigidBody` (declared I/O check in scheduler — the compile/schedule-time enforcement from M1-SYS-02 extended).
    - Unit tests: footstep event fires on the documented frame of a walk clip; consuming system reacts next tick.
  - **Verify:** `ctest -R anim_contract` green; I/O-declaration test proves animation→physics writes are impossible.
  - **Size:** ~150 lines + tests

- [ ] **M3-ANIM-04 · Drive the sprite batcher end-to-end**
  - **Refs:** FR-6.1, FR-2.1 (sheet frames in render)
  - **Depends:** M3-ANIM-01, M2-SPRITE-02
  - **Scope:**
    - Wire: `AnimPlayer` → sprite frame index → batcher UV; a 4-frame walk cycle renders (offscreen golden test: 4 consecutive frames show 4 distinct UVs).
    - Cost check: animation update for 10k animated entities ≤ 0.3 ms tick-time share (recorded; no PRD budget exists, so this baseline is recorded, not gated, until a PRD revision).
  - **Verify:** `ctest -R anim_render` green (golden frames); cost recorded in baseline.
  - **Size:** ~100 lines + tests

## Assets & project

- [ ] **M3-ASSET-01 · `.laige` project format**
  - **Refs:** FR-7.1 (versioned manifest + asset dir + scene dir; git-friendly), ARCH-007
  - **Depends:** M0-CORE-07
  - **Scope:**
    - Project layout: `project.laige` (JSON manifest: format version, engine min version, asset dir, scene dir, config) + `assets/` (raw source) + `packed/` (content-addressed output).
    - Content addressing: every packed blob is `sha256[:16]`-named; manifest lists blobs + hashes; text sidecars for diffs (per-asset JSON metadata).
    - Validation: missing/unknown manifest fields → explicit errors; version check (readers reject unsupported versions explicitly — ARCH-007).
    - Unit tests: create/validate/round-trip a fixture project; corrupt hash → loud failure.
  - **Verify:** `ctest -R project_format` green.
  - **Size:** ~250 lines + tests

- [ ] **M3-ASSET-02 · Asset store (runtime side)**
  - **Refs:** FR-7.2 (asset types), FR-7.4 (texture memory budget), S-2 (no per-frame heap)
  - **Depends:** M3-ASSET-01, M2-SPRITE-01
  - **Scope:**
    - `AssetRef` handles (32-bit, generation-checked — CPP-007); asset store loads from packs: texture → atlas, sprite frame, tileset, audio, font, animation (data), scene (M3-ASSET-06), shader (M4).
    - Loading is explicit (`store.load(ref)` — API-003: named as potentially expensive; budgeted queue with backpressure for streaming assets), in-memory residency tracked, texture memory budget report (G-R9 runtime side).
    - Unit tests: load/unload lifecycle; double-free detection; texture memory report exact for known fixtures; load-failure (missing blob) → `Status`, asset marked failed (fallback state, documented).
  - **Verify:** `ctest -R asset_store` green.
  - **Size:** ~300 lines + tests

- [ ] **M3-ASSET-03 · `laige-asset` CLI: import + validate**
  - **Refs:** FR-7.3 (import, atlas-pack, validate, pack), FR-7.6 (fail loudly at import)
  - **Depends:** M3-ASSET-02
  - **Scope:**
    - `laige-asset import <project>`: decode textures (stb_image), audio (M3-AUDIO-04), fonts (stb_truetype); validate each (format, size, bounds — e.g. texture > max size → import failure with the exact reason); write validated intermediate files.
    - `laige-asset validate <project>`: re-validate without re-import (fast path for CI).
    - Every failure is a structured error (NFR-13.3 grammar) naming the offending asset — never a silent skip.
    - Unit tests: each asset type imports a valid fixture; each has ≥ 1 corrupt-fixture test failing with the documented error.
  - **Verify:** `ctest -R asset_import` green; CI runs `laige-asset validate` on all sample projects.
  - **Size:** ~300 lines + tests

- [ ] **M3-ASSET-04 · Atlas packing**
  - **Refs:** FR-7.4 (automatic atlas packing, user-pinned layouts, mipmaps, memory reporting)
  - **Depends:** M3-ASSET-03
  - **Scope:**
    - `laige-asset atlas <project>`: deterministic MaxRects-style packing (fixed order: input order documented — deterministic output, ARCH-010); user-pinned layouts (JSON: fixed rect per texture, validated against overlaps); power-of-two atlas sizes (policy documented); mipmaps generated on pack (GPU upload uses mips — render side already binds mips if present).
    - Texture memory report per atlas (bytes, with/without mips) in the pack manifest.
    - Unit tests: packing deterministic (same input → same layout bytes); pinned layout respected; overlap in pinned layout → import-time error; memory report exact.
  - **Verify:** `ctest -R atlas` green.
  - **Size:** ~350 lines + tests

- [ ] **M3-ASSET-05 · Pack + compression**
  - **Refs:** FR-7.5 (scene streaming by region — data side), FR-7.7 (content-addressed chunks, miniz)
  - **Depends:** M3-ASSET-01, M3-ASSET-04
  - **Scope:**
    - `laige-asset pack <project>`: content-addressed chunks → single `.lpk` pack, miniz-compressed (deps.lock update for miniz); pack header (version, chunk table with hashes); runtime reader (M3-ASSET-02 loads from it).
    - Malformed pack handling: truncated, bad hash, unknown version → explicit reader errors (NFR-8.7 fuzz target `pack_read`).
    - Round-trip test: import → pack → load in a headless engine → all assets resident and valid.
  - **Verify:** `ctest -R pack` green; fuzz `pack_read --runs=1000` clean.
  - **Size:** ~300 lines + tests

- [ ] **M3-ASSET-06 · Scene format (binary + JSON sidecar)**
  - **Refs:** FR-7.5 (binary scene + JSON sidecar; scene = root transform, layers, entities + components)
  - **Depends:** M3-ASSET-02, M1-ECS-03
  - **Scope:**
    - Scene binary: versioned, content-addressed; contains layers, parallax setup, tilemaps, entities + component data (serialized via a generated/simple bitpacker for built-in components — custom-component serialization codegen is M4/M6 territory; P0 covers built-ins + data-carrier custom components with trivial POD serialization documented).
    - JSON sidecar per scene (for diffs/docs): same content in text; binary and sidecar must agree (validate mode cross-checks).
    - Load/save round-trip: world built from scene == world saved from scene (state hash equality).
  - **Verify:** `ctest -R scene_format` green; fuzz `scene_read` clean.
  - **Size:** ~350 lines + tests

- [ ] **M3-ASSET-07 · Scene streaming (load/unload by region)**
  - **Refs:** FR-7.5 (scene streaming by region), SCALE-001 (bounded per-tick work)
  - **Depends:** M3-ASSET-06, M2-TILE-01
  - **Scope:**
    - Scenes chunk into regions (grid-aligned, size documented); engine loads/unloads regions around a camera point (config radius), budgeted: max region-loads per frame (named), load queue with backpressure (PERF-008).
    - Region load = entities + tile chunks appear/disappear deterministically (enter/leave events, documented order); unload frees pools (no leak — ASan soak over 100 region transitions).
    - Unit tests: walk a 10×10 region map → exactly the documented regions resident at each position; load budget never exceeded (counted); zero leak after 1000 region transitions (ASan).
  - **Verify:** `ctest -R scene_stream` green under ASan.
  - **Size:** ~300 lines + tests

- [ ] **M3-ASSET-08 · Import fuzz targets (all asset surfaces)**
  - **Refs:** NFR-8.7 (asset import fuzzed in CI), TEST-005
  - **Depends:** M3-ASSET-05, M3-AUDIO-04
  - **Scope:**
    - Register fuzz targets: `texture_import`, `audio_decode`, `font_raster`, `scene_read`, `pack_read`, `json_config` (from M0); each runs bounded per-commit (`--runs=1000`) and nightly long.
    - Each target: no crash, no hang (per-input timeout), no leak (ASan nightly), errors via `Status` only.
    - Baseline corpus committed (valid + hand-broken fixtures) so regressions are caught before fuzzing.
  - **Verify:** all targets `--runs=1000` clean on CI; corpus committed; nightly long-run wired.
  - **Size:** ~200 lines + corpus

## Sample

- [ ] **M3-SAMPLE-01 · Playable isometric sample (in-engine)**
  - **Refs:** PRD §15 M3 exit ("playable isometric sample"), §16 (flagship style)
  - **Depends:** M3-PHYS-13, M3-INPUT-04, M3-ANIM-04, M3-ASSET-07, M3-AUDIO-02
  - **Scope:**
    - `samples/iso-flagship/`: small playable isometric scene — tile ground with a height step, player (circle body + sprite-sheet walk/attack anim), keyboard+mouse input (move + click-to-move via iso picking), collision with walls, SFX on hit/attack, grid-snap camera, 2 parallax layers, minimal UI (health text).
    - Built from `hello`-style patterns: **≤ 300 lines of game code**, commented; assets imported via `laige-asset` in CI.
    - Deterministic: replay of a scripted input sequence reproduces hashes (input frames from M3-INPUT-03).
    - Runs windowed locally, headless in CI (1000 ticks, scripted input, no visual assertions beyond hash + no-crash).
  - **Verify:** CI builds, packs assets, runs 1000 ticks with replay check; game-code line count ≤ 300 (CI check).
  - **Size:** ~400 lines (sample + fixtures) — the cap of the small-step budget; split into scene-setup / player-system / UI if it grows

## Milestone gate

- [ ] **M3-EXIT-01 · M3 exit gate**
  - **Refs:** PRD §15 M3 exit criteria
  - **Depends:** all other M3 steps
  - **Scope:**
    - Confirm and record: (1) physics budget green (`phys_10k_tick` ≤ 1.5 ms, link), (2) physics determinism AC: fixed-point bit-exact across builds (link), (3) playable isometric sample runs in CI (link), (4) zero per-tick allocations in physics (M1-ALLOC-01 assertion, link).
    - Update Progress Board.
  - **Verify:** all evidence links present; no open M3 step.
  - **Size:** docs only
