# Laige Roadmap — Implementation Checklist

This folder is the **working implementation plan** for the Laige engine, derived from
[`../PRD.md`](../PRD.md) and governed by [`../AGENTS.md`](../AGENTS.md).

It is a checklist designed so that **one AI agent (or one human) can safely pick up
exactly one step at a time**, implement it in a small patch, verify it, and move on —
without ever facing "thousands of lines at once". Every step is small on purpose.

---

## 1. How to use this roadmap (agent contract)

Before picking work, an implementing agent MUST:

1. Read `../AGENTS.md` (the engineering contract) and the relevant `PRD.md` sections.
2. Pick the **lowest-ID unchecked step** whose `Depends` are all checked.
   Milestones are sequential (M0 before M1, etc.); within a milestone, follow file
   order unless `Depends` says otherwise.
3. Implement **exactly the Scope bullets of that step. Nothing else.**
   - No refactoring unrelated code.
   - No features from later steps.
   - No new dependencies, new public API, or new modules unless the step says so.
4. **Size limit:** a step should land in **≤ ~400 lines of code including tests**.
   If it clearly won't, **stop and split the step** (create `Mx-XXX-04a`, `-04b`, …,
   record the split in the Change Log below) instead of delivering a giant patch.
5. Verify before checking the box:
   - Run the step's `Verify` command(s).
   - Complete the applicable items of the AGENTS.md §16 acceptance checklist
     (tests, formatting, no new warnings, docs updated in the same change,
     logging/diagnostics where the step creates runtime behavior).
6. **Check the box in the same PR/commit** that implements the step. PR title MUST
   start with the step ID: `[M2-ISO-01] Isometric depth key computation`.
7. Update the **Progress Board** counts and add one line to the **Change Log**.
8. If a step's scope **conflicts with AGENTS.md or the PRD, stop and surface the
   conflict** (AGENTS.md §1). Do not silently relax a rule.
9. **Decision steps** (`Mx-DEC-…`) produce an ADR under `docs/decisions/`. No dependent
   step may start before its ADR exists.

### Step ID scheme

```
M<milestone>-<SUBSYSTEM>-<sequence>      e.g.  M2-ISO-03
```

| Tag | Subsystem | Tag | Subsystem |
|---|---|---|---|
| `DEC` | Decisions / ADRs | `INPUT` | Input |
| `REPO` | Repository layout | `AUDIO` | Audio |
| `BUILD` | Build system | `ANIM` | Animation |
| `CI` | CI pipelines | `ASSET` | Assets & import |
| `DEP` | Dependencies / vendoring | `SCRIPT` | Scripting |
| `CORE` | Core (Result, log, math, pools, PRNG, config) | `TRAIT` | Component traits |
| `TOOL` | Tooling (manifest, lints, checks) | `UNSAFE` | Unsafe API |
| `TEST` | Test infrastructure | `PASS` | Custom render passes |
| `DOC` | Documentation | `LIGHT` | 2D lighting |
| `ECS` | Entity-component system | `ED` | Editor |
| `SYS` | System framework | `NET` | Networking |
| `LOOP` | Game loop | `LOAD` | Load harness |
| `DET` | Determinism / replay | `SHARD` | Sharding / zones |
| `CFG` | Configuration | `PERSIST` | Persistence seam |
| `PROF` | Profiling / observability | `SIM` | Simulation scale-out |
| `ALLOC` | Allocation guardrails | `SEC` | Security / anti-cheat |
| `GL` | GL context / render infra | `OPS` | Server ops |
| `CAM` | Camera | `SOAK` | Soak testing |
| `PROJ` | Projection modes | `REL` | Releases |
| `ISO` | Isometric (depth keys, picking, presets) | `TAG` | Tagging / changelog |
| `SORT` | Depth sorting | `BENCH` | Benchmarks / budgets |
| `SPRITE` | Sprite batcher | `PERF` | Performance acceptance |
| `SCENE` | Reference scene / scene data | `AC` | Acceptance-criteria suites |
| `TILE` | Tilemaps | `SAMPLE` | Templates / sample games |
| `PAR` | Parallax layers | `WEBGL` / `VULKAN` / `MOBILE` / `MESH` / `TOUCH` / `SKELE` | M9 stretch |
| `TEXT` | Fonts / text | `EXIT` | Milestone gate |
| `UI` | UI widgets | | |
| `GOLD` | Golden/image tests | | |

### Status format

Each step is one top-level checklist item:

```markdown
- [ ] **Mx-XXX-nn · Title**
  - **Refs:** <PRD FR/AC/NFR ids, AGENTS.md rule ids>
  - **Depends:** <step ids, or —>
  - **Scope:** <exact deliverable, 1–4 bullets>
  - **Verify:** <command(s) + expected result>
  - **Size:** <rough LOC including tests — a sanity ceiling, not a target>
```

`- [ ]` = open, `- [x]` = done. A step stays open until its Verify command is green
**and** AGENTS.md §16 is satisfied for the change.

### Canonical commands (established by M0-BUILD-01; referenced throughout)

| Purpose | Command |
|---|---|
| Configure | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` |
| Build | `cmake --build build -j` |
| Test | `ctest --test-dir build --output-on-failure` |
| ASan/UBSan build | `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_ASAN=ON` |
| TSan build | `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_TSAN=ON` |
| Fuzz (bounded) | `./build/bin/laige-fuzz <target> --runs=1000` |
| Benchmarks | `./build/bin/laige-bench --suite=<name>` |
| Determinism check | `./build/bin/laige-detcheck --scenario=<name>` |
| API manifest | `cmake --build build --target laige-api` |

Exact flags are fixed by `M0-BUILD-01`; later steps must use the documented form.

---

## 2. Milestone index

| File | Milestone (PRD §15) | Exit criteria (PRD) | Steps |
|---|---|---|---|
| [M0-foundations.md](M0-foundations.md) | **M0 — Foundations** (2–3 wks) | CI green on 3 OSes; core unit-tested; budget harness wired | 22 |
| [M1-heartbeat.md](M1-heartbeat.md) | **M1 — Heartbeat** (3–4 wks) | 10k entities @ 60 Hz ≤ 3 ms; replay bit-exact; zero-alloc assertion passes | 25 |
| [M2-rendering-2.5d.md](M2-rendering-2.5d.md) | **M2 — 2.5D Rendering** (4–6 wks) | 50k sprites ≤ 30 draw calls (worst-case iso); isometric is the default template; all AC-4.x pass | 32 |
| [M3-game-feel.md](M3-game-feel.md) | **M3 — Game Feel** (4–6 wks) | Physics budgets + determinism AC; playable isometric sample in-engine | 36 |
| [M4-scripting-customization.md](M4-scripting-customization.md) | **M4 — Scripting & Customization** (3–4 wks) | Script budget watchdog works; a custom-render-pass sample runs | 12 |
| [M5-editor-mvp.md](M5-editor-mvp.md) | **M5 — Editor MVP** (6–8 wks) | A dev can build a small isometric game *entirely* in the editor | 21 |
| [M6-networking.md](M6-networking.md) | **M6 — Networking** (6–8 wks) | Lockstep bit-exact; 100-player zone at 20 Hz; bandwidth budgets met | 16 |
| [M7-mmo-scale.md](M7-mmo-scale.md) | **M7 — MMO Scale** (8–12 wks) | AC-10.1/10.2/10.3 met; nightly load green 2 weeks straight | 15 |
| [M8-release-1.0.md](M8-release-1.0.md) | **M8 — Release 1.0** (4–6 wks) | All P0 FRs closed; budgets green; 3 reference games shipped | 8 |
| [M9-stretch.md](M9-stretch.md) | **M9+ — Stretch** | Per-feature proposals (each gated by its own ADR) | 6 |

P1 items (PRD §7: "M6–M7") that are rendering/editor/tooling in nature are placed in
the milestone that owns their subsystem (noted per step); the PRD's P1/M9 overlap is
flagged where it occurs rather than silently re-scoped.

---

## 3. Decision register (PRD §18 open questions)

Steps tagged `DEC` close these. ADRs live in `docs/decisions/`.

| ID | Question (PRD §18) | Blocking step(s) | Default if unresolved |
|---|---|---|---|
| D-NAME | Engine name & license (MIT proposed) | M0-REPO-01 | keep "Laige", MIT |
| D-MATH | Fixed-point default for all deterministic paths, or float-pinned + FP only for lockstep | M1-DET-01, M3-PHYS-11 | Q16.16 default for deterministic mode (PRD §10.3 recommends it for lockstep/MMO) |
| D-JSON | Config JSON: tiny in-engine parser vs vendored library | M0-CORE-07 | in-engine bounded parser (no new dep) |
| D-ISO | 2:1 dimetric vs true iso as template default | M2-CAM-02 | 2:1 dimetric (PRD v0.2: pixel-art default) |
| D-UI | Confirm minimal retained UI widget set for M2 | M2-UI-01 | the FR-2.8 P0 list (panel/button/text/image/list/slider/input) |
| D-EDITOR | Editor embedded vs standalone | M5-ED-01 | standalone binary (FR-8.7) |
| D-LUA | Confirm Lua 5.4 (vs no scripting in 1.0, vs WASM) | M4-SCRIPT-01 | Lua 5.4, optional module, off by default |
| D-NAT | M6 NAT traversal scope: STUN-only vs STUN+TURN | M6-NET-11 | STUN-style + relay endpoint, no TURN server in-engine |
| D-PERSIST | Persistence seam: external store only vs built-in SQLite | M7-PERSIST-01 | external store only (PRD §12.5) |

---

## 4. Progress board

Updated in the same PR that closes steps. "Done" = box checked + Verify green.

| Milestone | Steps | Done | Status |
|---|---|---|---|
| M0 | 22 | 0 | ⬜ not started |
| M1 | 25 | 0 | ⬜ not started |
| M2 | 32 | 0 | ⬜ not started |
| M3 | 36 | 0 | ⬜ not started |
| M4 | 12 | 0 | ⬜ not started |
| M5 | 21 | 0 | ⬜ not started |
| M6 | 16 | 0 | ⬜ not started |
| M7 | 15 | 0 | ⬜ not started |
| M8 | 8 | 0 | ⬜ not started |
| M9 | 6 | 0 | ⬜ proposals only |
| **Total** | **193** | **0** | |

---

## 5. Change log

One line per completed (or split/renumbered) step.

| Date | Step | Commit | Note |
|---|---|---|---|
| — | — | — | (empty) |

---

## 6. Global invariants that apply to *every* step

These come from AGENTS.md / PRD and are restated here so no step can be done
"cheaper" by forgetting them:

- **Determinism scope is always stated** (ARCH-010); deterministic-mode code uses
  engine math ops only (PRD §10.3, S-7).
- **Zero steady-state allocation in sim/render hot paths** (PRD §8.1, G-R1, PERF-003);
  enforced by the M1-ALLOC-01 assertion once it exists, then asserted in every
  later hot-path step's Verify.
- **No exceptions / no RTTI / no `dynamic_cast`** in engine core or public API (FR-12.1,
  NFR-8.10). Errors are `Result<T,E>` / `Status` with the §9.4 error grammar.
- **Every new runtime subsystem registers its essential counters and one inspection
  view** before its step is checked (DBG-008 / FR-11.1).
- **Docs, tests, benchmarks ship in the same change as the code** (CORE-006, DOC-007).
- **New third-party code requires a PRD-revision entry in the decision register**
  first (PRD §11). Vendoring an already-listed dependency is fine (DEP steps say so).
- **Budgets are measured, never assumed** (CORE-001): any step that claims a budget
  number must run the harness and record the result in `docs/benchmarks/`.
