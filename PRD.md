# Laige Engine — Product Requirements Document

| | |
|---|---|
| **Document** | PRD — Laige 2.5D Multi-OS Game Engine (C++) |
| **Version** | 0.3 (draft) |
| **Status** | Proposed — pending review |
| **Owner** | Engine team |
| **Last updated** | 2026-09-10 |

> Name: **Laige** — an acronym for *Legendary AI Game Engine*. Confirmed as the final name on 2026-09-10 (decision D-NAME, ADR 0001).
>
> **v0.3 change:** engine name confirmed as "Laige" (acronym for *Legendary AI Game Engine*); §18 item 1 resolved (ADR 0001).
>
> **v0.2 change:** isometric is designated the **primary projection** — most Laige games will be isometric. It is the default template, the reference scene for all visual/performance acceptance tests, and the focus of new first-class requirements (depth keys, picking, grid-snap camera, grid-aligned AOI).

---

## 1. Executive Summary

Laige is a cross-platform (Windows / Linux / macOS) **2.5D game engine written in C++20**, designed so that developers — human or AI agent — can build 2.5D games **easily, fast, and stably**, from small single-player titles up to large authoritative multiplayer games (MMOs). **Isometric is the primary projection**: most games built with Laige will be isometric, and the engine is tuned so that the isometric case is the fastest, safest, and most polished path.

The engine's three defining properties:

1. **2.5D-native, isometric-first.** The logical simulation is 2D (flat playfield, 2D physics, 2D state — cheap, deterministic, easy to replicate); the presentation is 3D (3D camera, depth-sorted sprites/billboards, parallax, isometric/oblique projection). Games get depth and cinematic camera freedom without paying 3D simulation or 3D networking costs.
2. **Performance-guarded API.** The public API is flexible (custom systems, custom components, custom rendering passes, scripting hooks, unsafe escape hatches) but *tight*: the safe-by-default path is the only path where common performance mistakes (per-frame allocations, unbatched draw calls, variable-timestep physics, hand-rolled replication) are possible at all. Dangerous patterns are either impossible, or explicitly opt-in behind a documented, warned `unsafe` namespace.
3. **Minimal dependencies.** At most a handful of small, mature, header-only or single-file third-party libraries. Everything performance- or determinism-critical (ECS, 2D physics, networking protocol, asset format) is built into the engine. Nothing ships that isn't justified.

Success = a developer can ship a playable isometric 2.5D game with the editor in a day, and a team can scale the same codebase to an MMO with thousands of concurrent players without rewriting subsystems.

---

## 2. Vision & Problem

**Vision.** One engine, one codebase, one API for: an isometric action RPG (the flagship style — most Laige games will be isometric), a 20-minute single-player platformer, a co-op dungeon crawler, a real-time strategy-lite, and a persistent 2.5D MMO — with deterministic simulation at the core in all of them.

**Problem.** Existing engines are either:
- **General 3D engines** (Unity, Unreal, Godot): 2D is a compromise layered on 3D; the API surface is so wide that developers (and AI agents writing code against it) routinely produce slow, allocation-heavy, hard-to-debug code; MMO-grade networking is bolted on, not built in.
- **Lightweight 2D engines**: fast and simple, but 2D-only presentation (no depth, no 3D camera), no credible path to MMO scale, thin tooling. Isometric in these engines is usually hacked together (manual z-fighting workarounds, hand-rolled picking).

There is a gap: a *narrow* engine that is *deep* — small API, opinionated defaults, hard performance ceilings by construction, first-class networking, and tooling good enough for professional use.

---

## 3. Target Users & Use Cases

| Persona | Needs | Laige's answer |
|---|---|---|
| Solo/indie dev | Fast iteration, few concepts, great defaults | One editor, safe API, batteries-included (audio, physics, input, UI), templates |
| Small team (2–10) | Custom systems, profiling, reproducible builds | ECS escape hatches, built-in profiler, deterministic replays, headless server mode |
| MMO team | Authoritative server, sharding, bandwidth control | Built-in authoritative server runtime, AOI streaming, generated serialization, delta sync, lockstep mode |
| **AI agent / LLM-generated code** | Predictable API, machine-readable docs, safe-by-default, actionable errors | Machine-readable API manifest, deterministic behavior, guardrails that make wrong code *fail loudly* instead of being slow, stable versioned ABI |

**Representative games in scope (all must be buildable without leaving the engine):**
- **Isometric 2.5D action RPG (single-player + online co-op) — the flagship.** Default projection for the project template; the reference game for visual and performance acceptance.
- Single-player 2.5D platformer with parallax, tilemaps, 2D skeletal animation.
- Small-scale deterministic lockstep multiplayer (≤8 players) e.g. twin-stick arena.
- Large 2.5D MMO: hundreds of players per zone, authoritative server, client prediction, chat, trading UI, instance dungeons.

---

## 4. What "2.5D" Means in Laige (Definition of Done)

"2.5D" in Laige is a precise architectural contract, not a visual style:

- **Simulation space is 2D.** Entities have position/orientation in a plane (local X/Y; world Y may be "height" or "depth" depending on projection mode — see §4.2). All physics, collision, pathing, AI spatial queries, and *replicated state* are 2D.
- **Rendering space is 3D.** The camera is a full 3D camera (position, orientation, projection). Entities are rendered as depth-sorted textured quads/billboards/sprites with a world-space **depth value** used for z-ordering; optional lightweight 3D content (simple meshes, 3D backgrounds) is a stretch goal, not part of the core promise.
- **Depth is presentation-only.** The z/depth value is a rendering attribute, not a simulated physical dimension. This keeps state small, physics cheap, and networking cheap — the single most important decision for the MMO goal.
- **Isometric is the primary projection** — most Laige games will be isometric (ARPGs, strategy-lite, dungeon crawlers, MMO hubs). Consequences:
  - Isometric is the **default camera/projection for new projects** (templates ship with it).
  - It has the **deepest feature set**: deterministic depth keys derived from (x, y, tile/step height, layer), grid snapping, grid-snap camera, screen→grid picking (click-to-select / click-to-move).
  - It is the **reference scene** for all visual and performance acceptance tests (worst-case overlap for depth sorting).
- **Other projection modes are first-class**, as simpler special cases of the same pipeline:
  1. **Isometric (primary):** 2:1 dimetric (pixel-art default) and true 30°/60° isometric, plus arbitrary shear per scene. Simulation stays axis-aligned 2D (no oblique simulation — keeps collision and pathing axis-aligned and deterministic); the render depth key is computed from the axis-aligned world, not from screen space.
  2. **Side-view (2.5D platformer):** camera looks at the X/Z plane; Y = height; sprites billboarded; depth = Z.
  3. **Top-down / oblique:** camera looks down; X/Y = ground plane; depth from Y (or sprite z-order); supports classic top-down and ¾-view.
  4. **Free 3D camera over a 2D field** (cinematic/cutscene mode).
- **Parallax & layers:** background/midground/foreground layers render with configurable parallax factors — core renderer feature, not a plugin.

**Acceptance criteria (AC):**
- AC-4.1: A platformer (side-view) and an isometric ARPG both run from the same engine build, differing only in camera/projection configuration.
- AC-4.2: No simulation, physics, pathing, or network code depends on the projection mode.
- AC-4.3: Depth sorting is correct (visually verified) for 10,000 overlapping sprites at 60 FPS on mid-range hardware.
- AC-4.4: **Isometric first-class set:** screen→grid picking is exact at all supported zoom levels; depth keys are correct with tile/step heights (stepped terrain); isometric is the default for the project template; depth-key rebuild after a terrain edit meets the §8.1 budget.

---

## 5. Goals & Non-Goals

### Goals
- G1: Make 2.5D game development *easy* — low concept count, sane defaults, working editor, excellent docs.
- G2: Be **performant by construction** — hard, measurable performance budgets (§8), guardrails (§9), zero steady-state allocation in hot paths.
- G3: Be **stable** — no-UB guarantee, deterministic simulation, graceful degradation, crash-free CI, reproducible builds.
- G4: Scale from **single-player → MMO** without changing architecture: one deterministic simulation core, one networking stack, one server runtime.
- G5: **Flexible but tight API**: rich customization surface + explicit unsafe escape hatches that are impossible to misuse silently (§9).
- G6: **Minimal dependencies** (§11): small approved list, all mature, each with documented justification; no runtime dependency growth without a PRD revision.
- G7: **AI-agent-friendly** development experience (§13).

### Non-Goals (v1)
- Full 3D engine (no 3D physics, no 3D character animation, no skeletal 3D rigs). 3D meshes for static scenery: stretch goal, M8+.
- Console targets (PC first; mobile/web stretch).
- Built-in asset store, marketplace, or online services (we provide the networking stack, not the service).
- Editor as a runtime dependency — games and servers run headless without any editor code.
- Built-in multiplayer matchmaking/accounting services.

---

## 6. Target Platforms

| Platform | Priority | Notes |
|---|---|---|
| **Windows 10/11 (x64, arm64 stretch)** | P0 | MSVC 2022 primary, clang-cl secondary |
| **Linux (x64, arm64)** | P0 | glibc ≥ 2.31; major distros; MSVC-free CI path |
| **macOS (arm64 + Intel)** | P0 | No support for apple fanboys. Fuck apple. |
| Web (WASM + WebGL2) | P2 | M8+; desktop targets first |
| Android / iOS | P3 | Stretch; only if WebGL/WASM path is solid |

**Rendering API:** OpenGL 3.3 core (desktop) / OpenGL ES 3.0 (web/mobile). One renderer, version-gated feature layers. Vulkan is an explicitly *optional later* backend; the renderer API is defined so a second backend is a clean addition (no promise in v1).

**Acceptance criteria:**
- AC-6.1: One source tree compiles on all P0 platforms with the same feature set (no feature flags that silently remove subsystems).
- AC-6.2: Server mode builds headless (no GPU required) on Linux and Windows.

---

## 7. Functional Requirements

Requirement IDs are tracked. `P0` = must ship in M1–M5, `P1` = M6–M7, `P2` = M8+.

### 7.1 Core (M1) — `P0`
- **FR-1.1 Game loop:** Fixed-timestep simulation (default 60 Hz, configurable 20–120 Hz) decoupled from variable rendering; render interpolation of positions (2D-aware, projection-correct) between simulation ticks. `P0`
- **FR-1.2 ECS core:** Archetype/SoA storage; dense entity iteration; O(1) component access; deterministic iteration order; component add/remove via pool (not heap). Entity is a 32-bit handle; components identified by compile-time trait types. `P0`
- **FR-1.3 System framework:** Systems are plain, registered functions with declared (static or auto-measured) time budgets and declared input/output component access; engine enforces iteration legality (no mutation while iterating the same storage). `P0`
- **FR-1.4 Determinism mode:** Simulation core must be bit-identical across platforms, compilers, and run times when run in deterministic mode (see §10.3). Deterministic replays (input log → state) must be replayable and diffable. `P0`
- **FR-1.5 Configuration:** Declarative game config (tick rate, budgets, camera defaults, asset roots) in JSON; runtime config overrides; hot-reload of non-simulation config in debug. `P0`
- **FR-1.6 Headless mode:** The entire engine (except presentation) runs without a window/GPU — required for servers, CI, and replay tooling. `P0`

### 7.2 Rendering — 2.5D (M2) — `P0`
- **FR-2.1 Sprite pipeline:** Batched textured quads; one draw call per (atlas, material, blend) group per frame; GPU-instanced; supports rotation, scale, tint, per-sprite depth, UV sub-rect, atlas UV animation (sheet frames). `P0`
- **FR-2.2 Depth & sorting:** Deterministic, stable z-order from (layer, depth, entity id). In isometric mode the depth key = ground (x + y) contribution + tile/step height, precomputed at scene build and **incrementally updated on tile/height changes** (not recomputed per frame). Bucket/radix sort, no per-frame allocation. `P0`
- **FR-2.3 Parallax layers:** Named background/mid/foreground layers with parallax factor, offset, UV scroll, blend. `P0`
- **FR-2.4 Camera:** Full 3D camera (position, look-at, FOV/ortho), smooth follow/target, shake, zoom, screen-shake, camera constraints (rect bounds). Isometric presets (2:1, true iso), **grid-snap camera mode** (camera locked to grid coordinates — the standard isometric game feel), and zoom clamping. `P0`
- **FR-2.5 Projection modes:** Isometric (primary; 2:1 default, true iso, custom shear), side-view, top-down/oblique, free-cinematic — selectable per scene/view (see §4). `P0`
- **FR-2.6 Tiles & tilemaps:** Tilemap component (chunks), tile animation, per-tile depth/height for 2.5D stepped terrain (auto-depth from tile Y-height — the isometric staple), parallax tile layers. `P0`
- **FR-2.7 Particles:** Lightweight GPU particle system (CPU-simulated, 2D + depth), budgeted, pooled. `P0`
- **FR-2.8 Text & UI:** Text rendering (bitmap or SDF font, `P0` for bitmap / SDF `P1`); immediate-mode-style UI widget tree (panel, button, text, image, list, slider, input) with a retained mode for menus; UI is a separate render pass, screen-space. `P0`
- **FR-2.9 Custom passes:** Users may add shader passes (fullscreen, layer-specific) via a stable pass API; passes are budgeted and ordered; the `unsafe` direct-draw path exists but is warned and gated (§9). `P1`
- **FR-2.10 Lighting (2D):** Per-layer ambient + point light masks (cheap, baked or runtime), no real-time 3D lighting. `P1`
- **FR-2.11 World picking & screen transforms:** Screen↔world transforms per projection mode; **isometric picking** (screen → ground plane → grid cell) is O(1), deterministic, and exposed in the safe API for click-to-select / click-to-move — a must-have for isometric games. `P0`

**Performance acceptance:** ≤ 30 draw calls/frame for a busy scene (50k visible sprites — worst-case isometric overlap with tile-height depth keys — 3 parallax layers, UI); ≤ 2 ms total CPU on mid-range laptop CPU; zero per-frame allocations in the render batcher.

### 7.3 Physics — 2D deterministic (M2/M3) — `P0`
- **FR-3.1 Rigid bodies:** Kinematic/dynamic/static; point & composite (convex polygon, circle) shapes; position, velocity, angular velocity (2D plane only). `P0`
- **FR-3.2 Collision:** SAT/Minkowski narrowphase; spatial-hash broadphase (engine-managed, auto-sized); contacts with manifolds; continuous collision for fast movers (CCD on player-critical bodies). `P0`
- **FR-3.3 Integration:** Semi-implicit Euler at the fixed tick; fully deterministic; fixed-point (Q16.16) option for bit-exact lockstep use; floating-point path uses engine-only math ops with identical semantics on all targets. `P0`
- **FR-3.4 Constraints/joints:** Hinge, slider, distance, motor, weld — enough for vehicles/cranes/dungeon doors; deterministic solver with fixed iteration count. `P0`
- **FR-3.5 Layers & masks:** Collision layer/mask matrix; query APIs (raycast, overlap, sweep) that are spatial-hash-bounded. `P0`
- **FR-3.6 No variable-timestep API.** Variable-step physics is simply not exposed. `P0`

**Performance acceptance:** 10,000 dynamic bodies at 60 Hz ≤ 1.5 ms on mid-range laptop CPU; zero per-tick allocations; deterministic replay reproduces physics bit-exactly.

### 7.4 Input (M3) — `P0`
- **FR-4.1 Devices:** Keyboard, mouse, gamepad (GLFW backends), touch (P2). `P0`
- **FR-4.2 Action abstraction:** Named actions mapped to bindings; remapping at runtime; saved per profile; axis + digital; deadzone config. `P0`
- **FR-4.3 Input state:** Per-tick sampled input frame (deterministic; consumed by simulation as an input event, not polled arbitrarily). `P0`
- **FR-4.4 UI input routing:** Input routes to UI first (focus system), then world. `P0`

### 7.5 Audio (M3) — `P0`
- **FR-5.1 Playback:** Music + SFX; pooled sources; bus structure (master/music/SFX/voice) with per-bus volume/filters. `P0`
- **FR-5.2 2D positional audio:** Pan by angle, falloff by distance, occlusion flags. `P0`
- **FR-5.3 Streaming:** Large assets stream with bounded memory. `P0`
- **FR-5.4 Lossless/lossy import:** Import WAV/OGG/FLAC/MP3 (via decoders) into engine asset format. `P0`

### 7.6 Animation (M3) — `P0`
- **FR-6.1 Sprite-sheet animation:** Frame-based (per sprite), state machine or free sequencing. `P0`
- **FR-6.2 2D skeletal (mesh) animation:** Simple 2D bone hierarchy (transform bones), keyframed clips, additive layers, weight-blended vertex animation (CPU or GPU) — for characters in 2.5D games. `P1`
- **FR-6.3 Animation state machine:** Node/transition/parameter-based; per-entity instances; debug visualization. `P0` (sheet-based) / `P1` (skeletal)
- **FR-6.4 Animation ↔ simulation contract:** Animations read/notify simulation (footsteps, events) via a one-way event API; animation never mutates physics directly. `P0`

### 7.7 Assets & Project (M3) — `P0`
- **FR-7.1 Project format:** `.laige` project = versioned manifest + asset directory + scene directory; git-friendly (all text sidecars; binary blobs content-addressed). `P0`
- **FR-7.2 Asset types:** Texture (→ atlas), sprite frame, tileset, audio, font, scene, animation, script, shader. `P0`
- **FR-7.3 Import pipeline:** Headless `laige-asset` CLI (import, atlas-pack, validate, pack) — runs in editor, in game (pack only), and in CI. `P0`
- **FR-7.4 Textures:** Automatic atlas packing (with user-pinned layouts); mipmaps; texture memory budget reporting. `P0`
- **FR-7.5 Scenes:** Binary scene format + JSON sidecar (for diffs/docs); scene = root transform, layers, entities + components; scene streaming (load/unload by region). `P0`
- **FR-7.6 Validation:** All assets validated on import (format, size, bounds); invalid assets fail loudly at import time, never at runtime. `P0`
- **FR-7.7 Compression:** Content-addressed chunks, miniz-compressed packs for distribution. `P0`

### 7.8 Editor (M5) — `P0` for MVP
- **FR-8.1 Scene editor:** 2.5D viewport (pan/zoom, projection-mode aware; isometric grid overlay with snap in isometric mode), entity hierarchy, component inspector, transform gizmo (2D + depth), selection, multi-select, undo/redo (transaction-based). `P0`
- **FR-8.2 Tilemap editor:** Tile brush, eraser, flood, stamp, height (depth) brush, animation layers. `P0`
- **FR-8.3 Sprite/animation editor:** Frame editor, state-machine graph editor, preview playback. `P0`
- **FR-8.4 Asset browser:** Import, preview, atlas view, texture memory view. `P0`
- **FR-8.5 Play/Debug:** Run game in-editor (play mode), stop, profiler overlay (per-system time, allocs, draw calls, net traffic), deterministic replay viewer (scrub + diff against live state). `P0`
- **FR-8.6 Script console:** Evaluate script expressions against a running game (scripting on). `P1`
- **FR-8.7 Editor architecture:** Editor is a separate binary sharing the engine core; the engine never links editor code; no editor is needed to build/run a game or server. `P0`
- **FR-8.8 Custom component UI:** Components may register inspector UI (sliders, curves, asset refs) via a small declarative schema — no editor code required for custom components. `P1`

### 7.9 Scripting (M4) — `P1` (design in M1)
- **FR-9.1 Scriptable surface:** Game logic can be written in an embedded scripting language (proposed: Lua 5.4, vendored) alongside C++; C++ remains first-class (no scripting requirement to build a game). `P1`
- **FR-9.2 Sandbox:** Scripts run with bounded time per tick (watchdog), no direct access to engine internals beyond a stable, documented binding; memory limits per script; hot-reload in debug. `P1`
- **FR-9.3 Bindings are generated** from the same API manifest used for docs (§13) — one source of truth. `P1`

### 7.10 Networking — client/server (M6/M7) — `P0`
- **FR-10.1 Modes:** (a) **Authoritative server** (default; MMO path), (b) **Lockstep** (small deterministic multiplayer, client-side simulation, ≤ N players, input exchange), (c) **P2P relay** (stretch, P2). `P0` for (a) and (b).
- **FR-10.2 Transport:** UDP; reliable-ordered channels for chat/UI/commands, unreliable for movement/state; retransmission + congestion-aware send rate; NAT traversal basics (STUN-style, relay support). `P0`
- **FR-10.3 Entity replication:** Per-component replication rules declared in component traits (replicated: server→all / client→server / both; rate; compression hints). Serialization is **generated** from component definitions (bitpacked); hand-written serialization is not required and not exposed in the safe API. `P0`
- **FR-10.4 Interest management (AOI):** Spatial-hash interest regions — **grid-aligned for isometric worlds**, where the AOI is a rhombus on screen but an axis-aligned box in simulation space, so enter/leave streaming is cheap and deterministic per player; enter/leave streaming of entity state; per-player entity budget; occlusion by zone boundaries. `P0`
- **FR-10.5 Sync model:** Server tick (20–100 Hz, configurable); snapshot + delta; per-tick bandwidth budget per player with automatic degradation (lower rate, coarser data) when budget exceeded; client interpolation buffer. `P0`
- **FR-10.6 Prediction & reconciliation:** Built-in client-side prediction for player-controlled entities (movement, actions) with server reconciliation; lag compensation for hit detection via deterministic replay of recent server state (rewind window ≤ 500 ms). `P0`
- **FR-10.7 Chat/UI:** Built-in reliable chat channel, emotes, basic UI state sync (e.g. shop selection). `P1`
- **FR-10.8 Server runtime:** `laige-server` binary: multi-zone host, zone = one simulation instance with its own tick; world registry for zones/instances (dungeons, lobbies); player sessions, reconnection, kick/leave; headless by definition. `P0`
- **FR-10.9 Sharding:** Zones run in separate processes (or separate simulation instances in one process, P1); cross-zone gateway for travel/teleport with state handoff; horizontal scale = more zone processes + registry (external, e.g. a small database or file-backed registry). `P1`
- **FR-10.10 Anti-cheat posture:** Server-authoritative by default; all game-affecting state computed server-side; client input validated against rules; deterministic replay stored (bounded) for audit/dispute review. `P0`
- **FR-10.11 Protocol hygiene:** Versioned binary protocol; forward-compat negotiation; no reflection over the wire; packet budget telemetry; protocol changes gated by tests. `P0`

**Scale acceptance:**
- AC-10.1: One zone process sustains **2,000 concurrent players** at 20 Hz tick on a single 8-core machine, p95 server tick ≤ 8 ms, p95 client state latency budget ≤ 1 RTT.
- AC-10.2: Steady-state client bandwidth **≤ 1 KB/s per idle player**, ≤ 50 KB/s under heavy combat, with automatic degradation.
- AC-10.3: Lockstep mode is bit-deterministic across all connected clients.

### 7.11 Profiling & Tooling (M2 onward) — `P0`
- **FR-11.1 Built-in profiler:** Always-on (cheap) counters: per-system time, entity counts, alloc counts (target: 0 in sim), draw calls, texture binds, net bytes, tick time, frame time percentiles; exposed in editor overlay, CLI, and file export. `P0`
- **FR-11.2 Frame graph / budget report:** Per-frame breakdown against declared budgets; over-budget systems flagged. `P0`
- **FR-11.3 Replay:** Every debug run can be recorded (inputs + seed) and replayed bit-exactly; diff two replays by frame/state. `P0`
- **FR-11.4 Memory inspector:** Live pools, per-system memory, peak tracking. `P1`
- **FR-11.5 Determinism checker:** CI tool that runs the same seed on two targets/compilers and asserts bit-identity. `P0`

### 7.12 Error Handling, Logging, Telemetry (M1) — `P0`
- **FR-12.1 No exceptions, no RTTI, no `dynamic_cast` in engine core or public API.** Errors are `laige::Result<T, E>` / `laige::Status` + structured error codes. `P0`
- **FR-12.2 Logging:** Structured, leveled, sink-swappable (console, file, network); per-subsystem scopes; log volume is budgeted (no logging in hot paths by default). `P0`
- **FR-12.3 Debug builds are loud:** In debug, every guardrail violation (§9) is asserted/logged with an actionable message (what, why, how to fix, doc link). Release builds degrade gracefully (clamp, skip, warn-once) — never crash the game for a recoverable condition. `P0`
- **FR-12.4 Crash reports:** Opt-in minidump capture with symbolication; deterministic-replay association. `P1`
- **FR-12.5 Telemetry:** Developer-facing perf telemetry (opt-in, local) feeding the profiler; no user/PII telemetry in the engine. `P1`

---

## 8. Non-Functional Requirements

### 8.1 Performance budgets (hard, measured in CI)

| Budget | Target | Measured on |
|---|---|---|
| Frame time (render) | p95 ≤ 8.3 ms @ 1080p (60 FPS) | Mid-range laptop (2019–2023 class) |
| Simulation tick (10k entities, 2k dynamic bodies) | ≤ 3.0 ms avg, ≤ 5 ms p99 | Same |
| 50k visible sprites (worst-case isometric overlap), 3 parallax layers, UI | ≤ 30 draw calls; ≤ 2 ms CPU | Same |
| Isometric depth-key rebuild (10k dirty cells after terrain edit) | ≤ 0.2 ms | Same |
| Isometric screen→grid picking | O(1), ≤ 0.01 ms per pick | Same |
| Steady-state heap allocations in sim loop | **0 per frame** (asserted in debug) | Debug builds |
| Engine base memory (empty scene, running) | ≤ 100 MB RSS | All P0 platforms |
| Cold start (game process → first frame) | ≤ 2 s on SSD, ≤ 5 s cold | P0 platforms |
| Build time (clean, engine + sample) | ≤ 10 min on CI, ≤ 5 min local (warm) | CI |
| Zone server (2k players @ 20 Hz) | p95 tick ≤ 8 ms; ≤ 4 GB RAM | 8-core server class |

**Policy:** budgets are part of CI. A PR that regresses any budget by > 10% (or breaches absolute target) fails CI unless the budget is revised via a PRD revision. (NFR-8.1)

### 8.2 Stability (NFR-8.2 … NFR-8.6)
- **NFR-8.2 No undefined behavior.** Core is ASan + UBSan + TSan clean in CI on all P0 platforms, always.
- **NFR-8.3 Determinism.** In deterministic mode, identical inputs + seed ⇒ bit-identical state on all P0 platforms/compilers, verified in CI every merge.
- **NFR-8.4 Zero-crash targets.** 0 crashes in 72 h soak (auto-generated gameplay + scripted abuse, e.g. 10k entities spawning/dying, input floods, network partitions) per release.
- **NFR-8.5 Graceful degradation.** Every subsystem defines its degraded mode (e.g. audio off, V-sync off, reduced AOI radius, dropped particle effects) and the engine selects degradation automatically under load; degradation events are logged and visible in the profiler.
- **NFR-8.6 Reproducible builds.** Same sources + versions ⇒ same binaries (pinned dependency hashes, fixed flags); release archives verified by checksum.
- **NFR-8.7 Security.** Network protocol is memory-safe against malformed packets (fuzzed in CI); asset import is sandboxed against malformed files (fuzzed in CI); no `system()`/`exec`-style APIs in the engine.

### 8.3 Portability & build (NFR-8.8 … NFR-8.10)
- **NFR-8.8** CMake ≥ 3.22; single configure; no autotools; no network access needed to build (all deps vendored).
- **NFR-8.9** Static + shared library builds; sample game links statically by default (no DLL hell).
- **NFR-8.10** C++20; -Wall -Werror on engine; no exceptions/RTTI in shipped API (enforced by compiler flags and static checks).

### 8.4 Maintainability
- **NFR-8.11** Engine core ≤ 50k LOC (excluding vendored deps and editor); each module independently testable; module boundaries enforced by include-graph lint in CI.
- **NFR-8.12** 100% of public API covered by doxygen; every public symbol has an example or is marked `@experimental`.
- **NFR-8.13** Dependency count is a tracked metric in CI (see §11).

---

## 9. API Design Principles — "Flexible but Tight"

This section is the core of the product. The API has **two tiers**:

### 9.1 The Safe API (default tier)
What every developer and AI agent writes against. Rules:

1. **S-1 Handles, not pointers.** Public API uses 32-bit entity handles, `ComponentRef`, `AssetRef`, `TextureRef` — never raw pointers to engine-owned data. Lifetime is engine-managed; use-after-free is structurally impossible through the safe API.
2. **S-2 No per-frame heap in the safe API.** Any safe-API call that would allocate is either (a) pooled internally, or (b) refuses with `Status::BudgetExhausted` in release / asserts in debug. The safe API has no `new`-equivalent; data goes through engine pools (`laige::Pool<T>`, arena-scoped).
3. **S-3 Generated serialization.** Network/asset serialization is generated from component trait definitions. Developers declare *what replicates*, not *how it packs*; the generator emits tight bitpacked code and a protocol manifest. There is no "just serialize this struct" escape in the safe API (that's how MMO bandwidth budgets die).
4. **S-4 Fixed-timestep only.** The safe API exposes no variable-step simulation, no manual physics stepping, no manual frame stepping.
5. **S-5 Rendering goes through the batcher.** Scene content is declared (layers, sprites, tiles, depth, material); the engine batches. There is no "draw this quad now" call in the safe API. (Isometric depth keys are engine-computed from the axis-aligned simulation state — users never write z-ordering code.)
6. **S-6 Declared budgets.** Systems, passes, scripts, and network handlers declare (or inherit) time/memory/network budgets. Exceeding them is a visible, actionable event, not a silent 1% frame drop.
7. **S-7 Deterministic by default.** Simulation code that runs in deterministic mode uses engine-provided math ops only (or fixed-point types); using raw `float`/`double` inside deterministic systems is a compile-time/trait error.
8. **S-8 Component traits are the customization surface.** Custom components = data + traits (replication rules, memory class, inspector schema, serialization). The engine generates what it can from the trait; the user writes systems that operate on them. Custom components with *no* system are legal (data carriers).
9. **S-9 Fail loudly in debug, degrade in release.** (§FR-12.3)

### 9.2 The Unsafe API (explicit escape hatch tier)
For the 5% of cases that need it (custom post FX, bespoke render paths, hand-rolled packet codecs, direct GL). Rules:

1. **U-1 Namespaced and documented.** Everything lives in `laige::unsafe` (C++) / equivalent marker in bindings; each symbol carries a doc comment stating the exact performance/safety contract the caller must uphold (e.g. "caller must not allocate; caller must batch; caller owns this GPU resource until `release`").
2. **U-2 Instrumented.** Unsafe calls are counted by the profiler and reported in every frame graph; an unsafe-heavy build is visible at a glance.
3. **U-3 Bounded.** Unsafe APIs are still inside engine guardrails (budgets still apply; GPU resources tracked; memory charged to the caller's budget). "Unsafe" means *you take responsibility*, not *no limits*.
4. **U-4 Reviewed by design.** Each unsafe API is justified in its header doc with "use when / never when" — written for humans and parseable for AI agents.

### 9.3 Guardrails enforced by the engine (the "tight" part)

| Guardrail | Mechanism | Violation behavior |
|---|---|---|
| G-R1 Zero sim-loop allocations | Debug: allocation counter + assert. Release: pool overflow → logged degradation | assert (debug) / clamp+log (release) |
| G-R2 Draw-call budget | Batcher reports; per-pass cap configurable | warn + frame graph flag |
| G-R3 Entity-count thresholds | ECS warns at 25%/50%/100% of declared scene budget | warn (debug: with advice) |
| G-R4 Per-frame component churn | Add/remove is pooled but counted; > threshold/tick | warn (debug: "move to spawn/despawn system") |
| G-R5 Per-system time budget | Declared vs measured; rolling p99 | warn + profiler highlight; over 3× → error event |
| G-R6 Unbatched submission | Only in `laige::unsafe`, counted, logged | count + log always |
| G-R7 Network bandwidth budget | Per-player per-tick budget with auto-degradation | auto-degrade + telemetry |
| G-R8 Determinism violation | Deterministic-mode code uses engine math only; trait-checked | compile error |
| G-R9 Asset size/texture budget | Import-time validation + runtime texture memory budget | import failure (never runtime surprise) |
| G-R10 Script tick budget | Watchdog: script over budget | kill script tick + loud error (debug: suspend) |
| G-R11 Hand-rolled depth sorting | Isometric depth keys are engine-owned (FR-2.2); user-settable per-sprite depth exists for overrides | per-sprite depth overrides counted + warned (debug: "prefer tile height") |

### 9.4 AI-agent-specific API properties (NFR — see §13)
- Machine-readable API manifest (JSON) generated from the same sources as the headers.
- Deterministic, side-effect-pure documentation: every function's postconditions are stated.
- Error strings are templates: `{what} / {why} / {fix} / {doc_anchor}`.
- Stable versioning: minor versions are additive-only; breaking changes = major; deprecation = 2 minors.
- `hello.laige` template compiles, runs, and renders in < 100 lines of code (isometric scene by default).

---

## 10. Architecture Overview

### 10.1 Module map (one codebase, three binaries)

```
laige-core      determinism, math, allocators, pools, ECS, systems, config, logging, Result
laige-sim       game loop, physics, input state, animation state, pathing/steering, AI hooks
laige-render    GL context, 2.5D batcher, isometric depth keys, parallax, tiles, particles, text/UI, camera, passes
laige-assets    asset store, import pipeline, atlas packing, validation, content addressing
laige-net       transport (UDP/ENet), protocol, replication, AOI, prediction/reconciliation
laige-server    zone runtime, world registry, sessions, sharding host
laige-script    (optional) Lua VM + generated bindings, sandbox
laige-editor    (separate binary) scene/sprite/tile editors, play mode, profiler UI, replay viewer
laige-sample    reference games: isometric ARPG (flagship), platformer, lockstep arena, MMO demo zone
```

Dependency rule: arrows only downward; `laige-core` depends on nothing internal; lint-enforced (NFR-8.11).

### 10.2 Threading model
- **Simulation: single-threaded by default** (determinism, simplicity, debuggability). Deterministic parallelism (fixed partitioning) is an optional M7 optimization for large zones, off by default, still bit-exact.
- **Rendering:** its own thread (frame pipeline: cull/batch → submit), lock-free handoff of the frame descriptor.
- **Network:** dedicated I/O thread; state handoff to the sim thread at tick boundaries (one copy, no locks in hot path).
- **Audio/asset loading:** worker pool, budgeted.
- No user-visible thread APIs in the safe API; threading is an engine implementation detail. (Custom systems are sim-thread code.)

### 10.3 Determinism contract
- Fixed timestep; all simulation time is integer ticks.
- Math: deterministic mode uses engine math ops (x86-64 `float` semantics pinned by compiler flags, or Q16.16 fixed-point for cross-ISA bit-exactness — fixed-point is the default for lockstep/MMO).
- Iteration order fixed (archetype order, entity id order); no unordered containers in sim hot paths (hash tables use deterministic hash + fixed iteration, or are banned in sim).
- Randomness: seeded engine PRNG (xorshift128+ or similar), per-substream; seed is part of the replay.
- Floating point: if used at all in deterministic paths, only via engine ops; never platform intrinsics outside the engine.

### 10.4 Memory model
- Engine-owned pools for: entities/components, contacts, particles, audio sources, network packets, strings (string table with `StringRef` handles — no per-frame `std::string` in sim).
- User data via `laige::Pool<T>` / arena (budgeted, reset-per-frame where applicable).
- All memory is accounted in the profiler (NFR-8.11, FR-11.4).

---

## 11. Dependency Policy

**Rule:** every third-party dependency is *vendored*, *mature*, *small*, and *justified in this table*. Adding a dependency requires a PRD revision + justification. Dependency count is a CI metric (target ≤ 10, of which ≤ 3 are compiled code; the rest header-only/single-file).

| Dependency | Why (not reinventing the wheel) | Form |
|---|---|---|
| **GLFW** | Windowing + input (keyboard/mouse/gamepad) across all P0 OSes; mature, tiny, battle-tested. Writing native window code on 3 OSes is the exact wheel we don't invent. | Compiled, vendored |
| **GLAD** | OpenGL loader (generated header for our GL version) | Header-only, vendored |
| **GLM** | Math foundation for the *rendering* side (cameras, projection matrices — incl. isometric camera matrices). Sim uses its own fixed-point math (§10.3). | Header-only, vendored |
| **stb_image / stb_image_write / stb_truetype** | Texture decode/encode, font rasterization — public domain, single-file, zero-risk | Single-file, vendored |
| **miniz** | Asset pack compression (zlib-compatible, no build) | Single-file, vendored |
| **miniaudio** | Audio playback backend (WASAPI/CoreAudio/ALSA/Pulse) in one file | Single-file, vendored |
| **ENet** | Reliable-sequenced UDP channels for the network stack (chat, commands); our layer adds app semantics on top | Compiled, vendored |
| **Lua 5.4** *(optional module)* | Embedded scripting (§7.9) | Compiled, vendored, off by default |
| **GoogleTest** *(dev-only)* | Unit/integration tests; never shipped | Dev-only |

**Explicitly not dependencies:** ECS (ours), 2D physics (ours — determinism is a first-class requirement Box2D doesn't guarantee for our use), serialization (generated, ours), UI framework (ours, 2.5D-scale), asset format (ours), server runtime (ours), isometric projection/picking/depth keys (ours — the primary projection gets native support, not a plugin).

**Versioning:** dependency versions pinned in `deps.lock`; CI builds from `deps.lock` hashes (reproducible builds, NFR-8.6).

---

## 12. MMO Requirements (deep dive)

The MMO path is not a plugin — it's the same deterministic simulation core run in an authoritative zone process with the replication layer on top.

1. **Zone model.** A zone = one simulation instance (own tick, own entity universe, own AOI). Typical zone capacities: 200 (dungeon), 500–1000 (city), 2000 (persistent hub) per process. Zones communicate via the world registry (gateway): travel, instance invites, global events.
2. **State model.** Replicated state is 2D (position, velocity, facing, animation id, a handful of component payloads). This is why 2.5D scales: idle player state is tens of bytes, not kilobytes. For isometric zones, positions replicate as grid-aligned 2D coordinates — smaller still.
3. **Bandwidth contract.** Per-player budgets (idle 1 KB/s, combat 50 KB/s, AC-10.2) are *enforced* by the replication layer with automatic degradation: lower update rate → coarser precision (fixed-point bit-count reduction) → region shrinkage. The degradation ladder is declared per component.
4. **Latency contract.** Client prediction for local player + reconciliation; interpolation buffer for others (adaptive 100–250 ms); hit detection via server rewind (≤ 500 ms window, FR-10.6).
5. **Persistence boundary.** The engine does *not* include a database. It exposes a clean persistence seam: `EntitySnapshot` export/import + event stream (commands), so game projects plug in any store (SQLite/Postgres/external service) without touching engine code.
6. **Ops surface.** `laige-server` is a plain process: config file, log to stdout, metrics endpoint (local HTTP, dev), graceful shutdown (flush snapshots), hot zone load/unload (P1), watchdog + auto-restart hooks.
7. **Scale-out.** Sharding = more zone processes + registry. No engine-internal distributed simulation in v1 (deliberate: keep the deterministic core single-process; scale by replication of *zones*, not of *state*).
8. **Testing.** MMO scenarios are CI-testable: scripted load generator (N bots, movement patterns, combat bursts, join/leave churn, partition/latency injection) runs headless against a zone process every merge (subset) and nightly (full 2k-player scenario).

---

## 13. AI-Agent-Friendly Development (NFR)

Developed with the expectation that a significant share of game code is written by LLM agents:

- **NFR-13.1 API manifest:** `laige-api.json` (generated, versioned) describing every public symbol: signature, pre/postconditions, budget implications, examples, `@experimental` flags. The manifest is the contract AI agents code against; it is tested to match the headers in CI.
- **NFR-13.2 Safe-by-default payoff:** because wrong-but-legal code is *visible* (budgets, warnings, profiler), an agent can detect its own mistakes from runtime output — errors are the observability channel.
- **NFR-13.3 Error message grammar:** every engine error follows `{code} | {what} | {why} | {fix} | {doc_anchor}`; parseable by both machines and humans.
- **NFR-13.4 Determinism & idempotency:** engine behavior is deterministic and has no hidden global state not surfaced by the API — same inputs, same outputs, always.
- **NFR-13.5 Templates:** `hello.laige`, `iso-arena.laige` (isometric, the default template), `platformer.laige`, `mmo-zone.laige` — minimal, fully working, heavily commented; the docs' "canonical patterns" section is written as copy-pasteable snippets.
- **NFR-13.6 Scripting sandbox** (FR-9.x) doubles as an isolation boundary for AI-generated runtime logic.
- **NFR-13.7 No magic:** no implicit behavior not visible in the API (no hidden autoloads, no global singletons exposed to user code).

---

## 14. Quality & Verification Plan

| Layer | What | When |
|---|---|---|
| Unit | Per-module, ASan/UBSan | Every commit |
| Integration | ECS↔sim↔render↔net contracts; replay determinism; protocol fuzz; isometric picking/depth-key golden tests | Every commit |
| Fuzz | Asset import, network packets, config parse | Every commit (bounded), nightly (long) |
| Perf | Budget suite (§8.1) on pinned CI hardware, incl. worst-case isometric scene | Every PR (subset), nightly (full) |
| Determinism | Two-target bit-identity check (seeded scenarios) | Every merge |
| Soak | 72 h auto-gameplay + abuse scenarios per release candidate | Per release |
| Cross-OS | All P0 platforms in CI; one per PR, all per merge | Per PR/merge |
| Load | MMO bot scenario (AC-10.1) | Nightly + pre-release |

---

## 15. Roadmap & Milestones

| Milestone | Scope | Exit criteria |
|---|---|---|
| **M0 — Foundations** (2–3 wks) | Repo, CI (all P0), build system, `laige-core` (math, pools, alloc, Result, logging, config), dep lock, API manifest generator | CI green on 3 OSes; core unit-tested; budget harness wired |
| **M1 — Heartbeat** (3–4 wks) | Game loop, ECS, systems, determinism, headless mode, profiler core, `hello.laige` (headless) | 10k entities @ 60 Hz ≤ 3 ms; replay bit-exact; zero-alloc assertion passes |
| **M2 — 2.5D Rendering** (4–6 wks) | GL context, sprite batcher, **isometric depth keys + picking (primary)**, depth sort, parallax, camera (incl. grid-snap iso presets), projection modes, particles, text, basic UI | 50k sprites ≤ 30 draw calls on worst-case isometric scene; **isometric is the default template**; all AC-4.x incl. AC-4.4 pass |
| **M3 — Game Feel** (4–6 wks) | Physics (incl. CCD, joints), input actions, audio, sprite-sheet animation + state machines, tilemaps (height brush), asset import/atlas CLI | Physics budgets + determinism AC; playable **isometric** sample in-engine |
| **M4 — Scripting & Customization** (3–4 wks) | Lua VM, generated bindings, sandbox, custom component traits end-to-end, unsafe API v1, custom passes | Script budget watchdog works; a custom-render-pass sample runs |
| **M5 — Editor MVP** (6–8 wks) | Scene editor (iso grid overlay), tilemap editor (height brush), animation editor, asset browser, play mode, profiler overlay, replay viewer | A dev can build a small **isometric** game *entirely* in the editor |
| **M6 — Networking** (6–8 wks) | Transport, protocol, replication, grid-aligned AOI, prediction/reconciliation, lockstep mode, `laige-server` v1 | Lockstep bit-exact; 100-player zone at 20 Hz; bandwidth budgets met |
| **M7 — MMO Scale** (8–12 wks) | Sharding, world registry, persistence seam, deterministic parallel sim (optional), load harness, anti-cheat review, 2k-player scenario | AC-10.1/10.2/10.3 met; nightly load green 2 weeks straight |
| **M8 — Release 1.0** (4–6 wks) | Docs pass, samples polish, reproducible releases, security review, 72 h soak, 1.0 tag | All P0 FRs closed; budgets green; 3 reference games shipped with the engine (isometric flagship first) |
| **M9+ — Stretch** | WebGL/WASM target, 2D skeletal animation polish, custom component inspector UI, mobile, optional Vulkan backend, 3D mesh scenery | Per-feature proposals |

---

## 16. Success Metrics

1. **Ease:** median time from fresh clone to running `hello.laige` ≤ 10 min (measured with 5 external devs, humans and agent-only builds).
2. **Performance:** all §8.1 budgets green on pinned hardware at 1.0 (the worst-case isometric reference scene is the primary baseline).
3. **Stability:** 0 crash bugs open at 1.0; determinism checker green for 30 consecutive merges.
4. **Scale:** one reference MMO demo sustains 2,000 bot-players per zone for 72 h with degradation telemetry healthy.
5. **API health:** 0 breaking changes during 1.0 cycle after M4; 100% of public API covered by manifest + examples.
6. **Dependency discipline:** ≤ 10 vendored deps at 1.0; 0 new deps since M0 without PRD revision.

---

## 17. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Deterministic 2D physics is harder than it looks (CCD, joints, fixed-point) | Med | High | Scope physics to the M3 feature list; fixed-point first for lockstep; fuzz + replay tests from day one; cut joints to P1 if needed |
| MMO scale slips (2k/zone) | Med | High | Load harness in M6 (not M7); zone-sharding design keeps the problem bounded; budgets degrade gracefully so demos work at 1k even if 2k slips |
| Scope creep toward full 3D | Med | High | Non-goals are contractual (§5); 3D mesh support only as M9 stretch with its own PRD revision |
| Isometric feature set eats the roadmap | Med | Med | Isometric scope is bounded by design (depth keys + picking + camera presets, FR-2.2/2.4/2.11); anything fancier (e.g. arbitrary oblique lighting) is P1/stretch |
| Editor complexity explodes | High | Med | Editor is a separate binary, P0 = the six FRs in §7.8; fancy editor features are all P1+ |
| GL quirks across OSes (drivers, macOS layering) | Med | Med | Pinned CI hardware per OS; shader minimalism (no fancy GLSL); GL 3.3 core only |
| AI-generated code stresses unsafe API misuse | Med | Med | Unsafe tier instrumented by default (§9.2); error grammar (NFR-13.3) turns failures into self-correcting signals |
| Dependency creep | Low | Med | CI dep-count metric + PRD-gated additions (§11) |
| Single-maintainer bandwidth (if applicable) | Med | High | Module boundaries allow external contributions; samples-as-tests; keep M0–M2 dependency-free of editor code |

---

## 18. Open Questions

1. **Name & license** — **Resolved 2026-09-10** (ADR 0001): name confirmed as "Laige" (*Legendary AI Game Engine*); engine MIT, assets/samples separately licensed.
2. **Fixed-point default** — Q16.16 for all deterministic paths, or float-pinned with fixed-point only for lockstep? (Affects M1 core math; decision needed before M1.)
3. **Lua vs alternative** — confirm Lua 5.4 (vs. no scripting in 1.0; vs. embedded WASM for scripting — heavier).
4. **Editor embedded vs standalone** — PRD assumes standalone binary (FR-8.7); confirm (embedded play-mode is P0 either way).
5. **UI framework scope** — retained-mode UI widget set: confirm minimal list for M2 (panel/button/text/image/list/input) vs. defer full UI to M4.
6. **Persistence seam details** — SQLite built-in (one more compiled dep) vs. pure external store? PRD currently: external only.
7. **Stun/relay** — how much NAT traversal does the transport ship in M6 (STUN-only vs. STUN+TURN)?
8. **Isometric defaults** — confirm 2:1 dimetric (pixel-art standard) as the template default vs. true isometric (30°/60°); both remain selectable per scene.

---

## Appendix A — Glossary

- **2.5D (Laige definition):** 2D simulation + 3D presentation (§4).
- **Isometric (primary projection):** the default Laige projection — a 2D grid simulation presented through an oblique 3D camera; deterministic depth key from (x, y, tile height, layer) (§4, §7.2).
- **Zone:** one authoritative simulation instance hosting a player population.
- **AOI (Area of Interest):** the spatial region whose entities a client receives.
- **Lockstep:** all clients simulate identically from exchanged inputs; no server.
- **Reconciliation:** client prediction corrected by authoritative server state.
- **Safe API / Unsafe API:** the two API tiers (§9).
- **Budget:** a declared, measured, enforced resource limit (time, memory, network, allocations).
- **Replay:** an input log + seed that reproduces a run bit-exactly.

## Appendix B — Example: a custom replicated component (shape of the API)

```cpp
// What a game writes. No raw pointers, no manual serialization, no allocation.
struct Health {
  int32_t  current = 100;
  int32_t  max     = 100;
};

LAIGE_COMPONENT(Health,
    .replicate(laige::rep::server_to_all, /*rate_hz*/ 5, /*bits*/ {16, 16})
    .inspector(laige::insp::slider("current", "max"),
               laige::insp::slider("max", 1, 100000)));

// A system: deterministic, single-threaded, budgeted.
LAIGE_SYSTEM(HealthRegen, /*budget_ms*/ 0.5)
void run(laige::sim::Context& ctx) {
  for (laige::iter::Entity e : ctx.each<laige::Tag<Player>, Health>()) {
    Health& h = ctx.get<Health>(e);
    if (h.current < h.max) h.current = laige::math::min(h.current + 1, h.max);
  }
}
```

*(Illustrative — final syntax is set in M0/M1. The invariants are the point: handles, pools, generated replication, declared budgets.)*
