# M4 — Scripting & Customization

**PRD:** §15 M4, §7.9, §9 (unsafe tier), FR-2.9/2.10 (P1) · **Duration:** 3–4 weeks
**Scope (PRD):** Lua VM, generated bindings, sandbox, custom component traits
end-to-end, unsafe API v1, custom passes.
**Exit criteria (PRD):** Script budget watchdog works; a custom-render-pass sample runs.

Rules specific to this milestone: the safe API tier is now complete and frozen for
1.0 (PRD §16.5: 0 breaking changes after M4) — every step here only *adds*. The
`laige::unsafe` tier follows §9.2 exactly (namespaced, documented, instrumented,
bounded).

---

## Decisions

- [ ] **M4-DEC-01 · Confirm Lua 5.4 (D-LUA)**
  - **Refs:** PRD §18.3, §7.9, §11 (Lua row — optional module)
  - **Depends:** —
  - **Scope:**
    - Confirm the scripting choice: Lua 5.4 (recommended), vs no scripting in 1.0, vs embedded WASM.
    - ADR `docs/decisions/000X-scripting.md` (decision + memory/time budget implications + upgrade path).
  - **Verify:** ADR exists.
  - **Size:** docs only

## Scripting

- [ ] **M4-SCRIPT-01 · Vendor Lua 5.4 (optional module)**
  - **Refs:** PRD §11 (Lua row), §7.9
  - **Depends:** M4-DEC-01, M0-DEP-01
  - **Scope:**
    - Vendor Lua 5.4 (deps.lock); CMake option `LAIGE_SCRIPT` (default OFF per PRD §11 "off by default"); `laige-script` module created; engine builds and CI runs identically with the option off (AC-6.1: no silently removed features — option-off means "scripting disabled", documented, not "broken").
    - Lua built without its own C exceptions interacting with engine `-fno-exceptions`: `lua_pcall`-only boundary, documented.
    - Unit test: VM creates, runs a fixture script, disposes — with `LAIGE_SCRIPT=ON` in CI.
  - **Verify:** CI builds green with option on **and** off; `ctest -R lua_vm` green (on-build).
  - **Size:** ~100 lines + wiring

- [ ] **M4-SCRIPT-02 · VM integration + tick budget watchdog (G-R10)**
  - **Refs:** FR-9.2 (bounded time per tick), PRD §9.3 G-R10, FR-12.3
  - **Depends:** M4-SCRIPT-01
  - **Scope:**
    - One VM per game; scripts run as a system in the sim tick with a declared budget (default from config, named units).
    - Watchdog: script over budget → **kill the script tick** (longjmp out of Lua, documented), loud structured error (`{code}|{what}|{why}|{fix}` including script + line); debug mode: suspend VM + expose state for the console (M5-ED-16 later); release: continue game (degrade per NFR-8.5).
    - Script tick is deterministic w.r.t. game state: same inputs → same script effects (no wall-clock reads exposed — enforced, see M4-SCRIPT-04).
    - Unit tests: a busy-loop script is killed at exactly the documented budget (±1 Lua op, documented tolerance); after kill, the game tick completes and the next tick runs; two consecutive over-budget ticks don't wedge the VM.
  - **Verify:** `ctest -R script_watchdog` green (on-build CI).
  - **Size:** ~250 lines + tests

- [ ] **M4-SCRIPT-03 · Generated Lua bindings**
  - **Refs:** FR-9.3 (bindings generated from the API manifest — one source of truth), NFR-13.1
  - **Depends:** M4-SCRIPT-02, M0-TOOL-01
  - **Scope:**
    - Codegen: `laige-api.json` → Lua binding sources (typed accessors for the safe API surface: world/entity/component handles as Lua userdata with finalizers, Result → return-value + error string, no raw engine pointers escape).
    - Parity test: every safe-API symbol in the manifest has exactly one binding (generated report compared in CI); every binding maps back to a manifest symbol (no orphans).
    - Handles in Lua: 32-bit value semantics (S-1) — no lifetime management in Lua; finalizer only detaches the reference.
    - Unit tests: a Lua script creates an entity, adds a component, runs a system call, reads it back — exact values.
  - **Verify:** `ctest -R lua_bindings` green; parity report green in CI (manifest↔bindings both directions).
  - **Size:** ~350 lines (codegen) + tests

- [ ] **M4-SCRIPT-04 · Sandbox + memory limits + hot-reload**
  - **Refs:** FR-9.2 (no internal access, memory limits, hot-reload in debug), NFR-13.6
  - **Depends:** M4-SCRIPT-03
  - **Scope:**
    - Sandbox: the only C API Lua sees is the generated binding module; engine internals not exposed (audited: binding module is the only registered metatable set — enforced by a startup check that enumerates Lua globals).
    - Memory limit: `lua_alloc` hook with a per-script cap (config, named units); over cap → structured error + VM reset (documented), game continues.
    - Hot-reload (debug builds only): `script.reload(name)` recompiles from disk; state migration policy documented (global state lost unless stored in the engine-managed script-state table — documented); release builds: reload API absent.
    - No wall-clock/randomness escape: `os.time`/`math.random` removed from the sandbox (determinism, NFR-13.4); engine-provided `laige.tick`/`laige.rng` instead.
    - Unit tests: global audit finds no engine tables; memory cap triggers at the documented limit; hot-reload preserves the engine-state table; removed stdlib functions are absent (tested).
  - **Verify:** `ctest -R lua_sandbox` green.
  - **Size:** ~250 lines + tests

## Customization (safe tier)

- [ ] **M4-TRAIT-01 · Custom component traits end-to-end**
  - **Refs:** PRD §9.1 S-8, Appendix B (shape of the API)
  - **Depends:** M1-ECS-02, M4-SCRIPT-03
  - **Scope:**
    - Full `LAIGE_COMPONENT(Type, .replicate(...), .inspector(...), .serialize(...))` trait surface (PRD Appendix B shape): replication rules, memory class (pooled/arena), inspector schema, serialization hints.
    - End-to-end: a user component with traits registers, stores, iterates, appears in the API manifest (NFR-13.1), is usable from Lua bindings (generated accessor), and serializes to scene files (M3-ASSET-06 path) — data-carrier case (no system) explicitly legal.
    - Trait validation: conflicting traits (e.g. replicated + non-serializable) → registration error with the documented reason.
    - Unit tests: the PRD Appendix B `Health` example (as written) compiles and round-trips: store → scene save → load → same values; Lua reads `current`.
  - **Verify:** `ctest -R component_traits` green; Appendix B example is a committed, compiling test fixture.
  - **Size:** ~300 lines + tests

- [ ] **M4-TRAIT-02 · Custom system API (user-registered)**
  - **Refs:** FR-1.3, PRD §9.1 S-6
  - **Depends:** M4-TRAIT-01, M1-SYS-02
  - **Scope:**
    - `LAIGE_SYSTEM` usable in game code (already registered in the world) with declared budget + declared I/O; custom systems get exactly the same enforcement as built-ins (G-R5 timing, iteration legality, deterministic-mode math checks).
    - System order: user systems can declare `depends_on` built-in or other user systems (scheduler validation from M1-SYS-02).
    - Unit test: a custom system over budget is flagged exactly like a built-in (same event, same grammar).
  - **Verify:** `ctest -R custom_systems` green.
  - **Size:** ~150 lines + tests

## Unsafe tier

- [ ] **M4-UNSAFE-01 · `laige::unsafe` v1 (direct draw)**
  - **Refs:** PRD §9.2 U-1…U-4, §9.3 G-R6
  - **Depends:** M2-SPRITE-02, M1-PROF-01
  - **Scope:**
    - `laige::unsafe` namespace: v1 = direct-draw API (`unsafe::draw_quad(...)`, `unsafe::bind_texture(...)` — the exact set documented in the header with "use when / never when" per U-4).
    - Instrumented (U-2): every unsafe call counted per frame in the profiler (new fields `unsafe_draw_calls`, `unsafe_calls_total`); reported in every frame graph (M1-PROF-02 line).
    - Bounded (U-3): GPU resources tracked (texture binds released via `unsafe::release`), memory charged to the caller's budget, budgets still apply; misuse in debug → assert with the contract restated.
    - Header docs are parseable (machine-readable contract tags: `@contract: no-alloc`, `@thread: render`, `@phase: submit` — feeds the API manifest).
    - Unit tests: counter increments exactly; a draw without a live context → documented error (no crash); release of a live resource is flagged.
  - **Verify:** `ctest -R unsafe` green; frame graph shows the unsafe line (fixture scene).
  - **Size:** ~250 lines + tests

## Custom passes & 2D lighting

- [ ] **M4-PASS-01 · Custom shader pass API**
  - **Refs:** FR-2.9 (P1 — scoped into M4 by PRD §15), API-006
  - **Depends:** M4-UNSAFE-01, M2-GL-02
  - **Scope:**
    - Pass API: users register passes (fullscreen or layer-specific) with a name, shader (source compiled at pass registration — not on the gameplay hot path, RENDER-004), order position, declared budget; engine schedules passes in the documented pipeline order (after world, before UI by default; explicit positions supported).
    - Pass budget enforced (G-R5 pattern); over-budget pass → warn + frame graph flag, never silently slow the game.
    - Pass state handoff: read/write render targets documented (which target a pass may read/write — the set is explicit, not "any FBO").
    - Unit/integration test: a trivial fullscreen tint pass runs in order, budgeted, and the offscreen golden output changes as expected (golden fixture).
  - **Verify:** `ctest -R passes` green (offscreen CI).
  - **Size:** ~300 lines + tests

- [ ] **M4-LIGHT-01 · 2D lighting (per-layer ambient + point lights)**
  - **Refs:** FR-2.10 (P1 — placed here: it is a render feature next to passes)
  - **Depends:** M4-PASS-01
  - **Scope:**
    - Per-layer ambient tint; point lights (pos, radius, intensity, color) rendered as a cheap light pass (additive blend sprites / fullscreen mask — document the chosen technique + cost); light budget (max lights per layer, named, overflow → drop weakest + warn).
    - Baked mode: lights can be baked into a layer texture at scene load (headless, budgeted) — runtime lights only for dynamic ones.
    - No real-time 3D lighting (PRD §7.2 boundary respected).
    - Unit/integration test: 32-point-light scene renders within documented pass cost (recorded); baked == runtime for static lights (golden comparison within tolerance).
  - **Verify:** `ctest -R lighting` green; cost baseline recorded.
  - **Size:** ~300 lines + tests

## Sample & gate

- [ ] **M4-SAMPLE-01 · Custom-render-pass sample**
  - **Refs:** PRD §15 M4 exit ("a custom-render-pass sample runs")
  - **Depends:** M4-PASS-01, M4-TRAIT-02
  - **Scope:**
    - `samples/custom-pass/`: extends `iso-arena` — adds (a) a user component with traits (health bar data), (b) a user system (health regen, PRD Appendix B shape), (c) a custom pass (simple vignette), (d) a Lua script driving a debug spawn (scripting optional at build time).
    - **≤ 150 lines of game code**; builds with `LAIGE_SCRIPT` on and off (off: the Lua part compiles out via a documented feature macro — no broken paths).
    - Runs in CI (offscreen, 200 frames, golden check of the vignette).
  - **Verify:** CI green on both build configs; line-count check ≤ 150.
  - **Size:** ~200 lines (sample + wiring)

- [ ] **M4-EXIT-01 · M4 exit gate**
  - **Refs:** PRD §15 M4 exit criteria
  - **Depends:** all other M4 steps
  - **Scope:**
    - Confirm and record: (1) script watchdog kills at documented budget (test link), (2) custom-render-pass sample runs (CI link), (3) manifest↔bindings parity green (CI link), (4) unsafe calls visible in the frame graph (fixture link).
    - **Freeze checkpoint:** public safe API is now frozen for 1.0 (PRD §16.5); any further safe-API change requires an ADR. Record the freeze in `docs/api/`.
    - Update Progress Board.
  - **Verify:** all evidence links present; freeze note committed; no open M4 step.
  - **Size:** docs only
