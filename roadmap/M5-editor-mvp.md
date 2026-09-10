# M5 — Editor MVP

**PRD:** §15 M5, §7.8 · **Duration:** 6–8 weeks
**Scope (PRD):** Scene editor (iso grid overlay), tilemap editor (height brush),
animation editor, asset browser, play mode, profiler overlay, replay viewer.
**Exit criteria (PRD):** A dev can build a small **isometric** game *entirely* in
the editor.

Rules specific to this milestone: the editor is a **separate binary** sharing the
engine core (FR-8.7) — no editor code may be linked into engine, game, or server
builds (include-graph lint). The editor reuses the M2 UI widget system (retained
mode); it does not add a second UI framework. Editor actions are transactions
(undo/redo), not ad-hoc state edits.

P1 items in FR-8.6/8.8 and FR-11.4 (script console, custom inspector UI, memory
inspector) are placed here because the editor owns their surfaces; they are
explicitly marked P1 and may be descoped from 1.0 by a decision step if M5 slips
(PRD §17: editor complexity is the top-likelihood risk).

---

## Decisions

- [ ] **M5-DEC-01 · Confirm standalone editor architecture (D-EDITOR)**
  - **Refs:** PRD §18.4, FR-8.7
  - **Depends:** —
  - **Scope:**
    - Confirm: editor is a standalone binary (default per FR-8.7); embedded play-mode remains P0 inside it.
    - ADR `docs/decisions/000X-editor-architecture.md`.
  - **Verify:** ADR exists.
  - **Size:** docs only

## Editor core

- [ ] **M5-ED-01 · Editor binary skeleton**
  - **Refs:** FR-8.7 (separate binary; engine never links editor code)
  - **Depends:** M5-DEC-01, M2-UI-02
  - **Scope:**
    - `laige-editor` executable: opens a project (`.laige`), loads scene data (M3-ASSET-06 reader), renders via the engine's renderer, hosts a retained UI shell (menus, docks — built from M2 widgets).
    - No editor symbols in engine libs: include-graph lint rule "editor includes engine, never the reverse" + link check (editor binary is the only target allowed to link `laige-editor` code).
    - Headless editor mode for tests: `laige-editor --headless --project X --script Y` runs a scripted editor session (open, edit, save, verify) — this is how editor behavior is CI-tested without a display.
    - Unit/integration tests: opens fixture project, lists entities, saves without mutation (byte-identical scene output).
  - **Verify:** `ctest -R editor_core` green; lint rule blocks engine→editor includes; headless session runs in CI.
  - **Size:** ~300 lines + tests

- [ ] **M5-ED-02 · Scene editor viewport (pan/zoom)**
  - **Refs:** FR-8.1 (2.5D viewport, pan/zoom, projection-aware)
  - **Depends:** M5-ED-01, M2-CAM-01
  - **Scope:**
    - Editor viewport: renders the active scene through the engine renderer with an editor camera (pan by drag/keys, zoom, scroll); projection mode follows the scene (iso/side/top-down/free).
    - Viewport is read-only render + selection hit-testing (picking reuses M2-ISO-03 for iso; entity-bound-box picking for other modes, documented).
    - No sim running in view mode (view-only; play mode is M5-ED-12).
    - Tests (headless): scripted camera commands produce documented camera state; picking a known entity returns its id (deterministic fixture scene).
  - **Verify:** `ctest -R editor_viewport` green.
  - **Size:** ~250 lines + tests

- [ ] **M5-ED-03 · Isometric grid overlay + snap**
  - **Refs:** FR-8.1 (iso grid overlay with snap in iso mode)
  - **Depends:** M5-ED-02, M2-ISO-03
  - **Scope:**
    - Grid overlay: renders grid lines (cell size from scene tilemap) with hover highlight (cell under cursor via M2-ISO-03 picking); snap mode: entity placement/transforms snap to grid (config: on/off, snap axes documented).
    - Overlay is presentation-only (editor pass, never mutates scene data — ARCH-009).
    - Tests (headless): hover cell at 4 camera positions golden-checked; snap placement lands exactly on cell centers.
  - **Verify:** `ctest -R editor_grid` green.
  - **Size:** ~200 lines + tests

- [ ] **M5-ED-04 · Entity hierarchy + selection**
  - **Refs:** FR-8.1 (entity hierarchy, selection, multi-select)
  - **Depends:** M5-ED-02, M5-ED-03
  - **Scope:**
    - Hierarchy panel: scene entities as a tree (root → layers → entities; names editable, stable ids underneath); select/click in panel or viewport (shared selection state); multi-select (shift/box-select documented); selection drives all other panels.
    - Create/delete entity actions (transactions — M5-ED-07).
    - Tests (headless): select-by-id, select-by-pick, multi-select box (documented corners), delete removes from scene data (verify saved scene).
  - **Verify:** `ctest -R editor_hierarchy` green.
  - **Size:** ~250 lines + tests

- [ ] **M5-ED-05 · Component inspector (built-in components)**
  - **Refs:** FR-8.1 (component inspector)
  - **Depends:** M5-ED-04
  - **Scope:**
    - Inspector panel: for the selected entity, lists components with editors for built-in components (transform/2D + depth, sprite frame, animation clip, physics body params, camera, light) — values edit scene data (transactions).
    - Validation: out-of-range values rejected with the actionable message (FR-12.3); changes are immediately visible in the viewport (1-frame latency, documented).
    - Tests (headless): edit a transform value → saved scene contains it; invalid value rejected and state unchanged.
  - **Verify:** `ctest -R editor_inspector` green.
  - **Size:** ~300 lines + tests

- [ ] **M5-ED-06 · Transform gizmo (2D + depth)**
  - **Refs:** FR-8.1 (transform gizmo 2D + depth)
  - **Depends:** M5-ED-05
  - **Scope:**
    - Gizmo: drag handles for X/Y world axes + depth (up-down on the iso axis, documented control scheme); multi-select: gizmo moves all selected (offsets preserved, documented); snap per M5-ED-03.
    - Drag produces *preview* state (rendered live) committed as one transaction on release (undo = one step).
    - Tests (headless): scripted drag → exact transform delta; release without move → no transaction recorded.
  - **Verify:** `ctest -R editor_gizmo` green.
  - **Size:** ~250 lines + tests

- [ ] **M5-ED-07 · Undo/redo (transaction-based)**
  - **Refs:** FR-8.1 (undo/redo transaction-based)
  - **Depends:** M5-ED-04
  - **Scope:**
    - Transaction system: every editor mutation (create/delete/edit/brush stroke) is one transaction (snapshot-diff based, bounded memory: transaction log depth configurable, named, overflow → oldest dropped + warn); undo/redo stacks; redo cleared on new edit (standard, documented).
    - Transactions are atomic: a multi-stroke brush drag = one transaction (documented grouping rules).
    - Tests (headless): 50 edits → undo 50 → scene byte-identical to start; redo restores; mixed operations (create + move + delete) round-trip.
  - **Verify:** `ctest -R editor_undo` green.
  - **Size:** ~250 lines + tests

## Tilemap editor

- [ ] **M5-ED-08 · Tile brushes: paint, erase, flood, stamp**
  - **Refs:** FR-8.2 (tile brush, eraser, flood, stamp)
  - **Depends:** M5-ED-05, M2-TILE-01
  - **Scope:**
    - Brush tools over the active tilemap layer: paint (single/multi-cell drag), erase, flood fill (4- or 8-connectivity documented, bounded work + cap), stamp (rotated/stamped tile pattern from a palette).
    - All brush operations are transactions (one stroke = one transaction, M5-ED-07); every paint triggers the incremental depth-key update path (M2-ISO-02) where the height changes.
    - Flood cap: max cells per fill (named), overflow → partial fill + warning (never unbounded).
    - Tests (headless): each tool on a fixture tilemap → exact expected tile state; flood cap honored; undo restores.
  - **Verify:** `ctest -R editor_tile_brush` green.
  - **Size:** ~300 lines + tests

- [ ] **M5-ED-09 · Height brush + animation layers**
  - **Refs:** FR-8.2 (height (depth) brush, animation layers)
  - **Depends:** M5-ED-08, M2-ISO-02
  - **Scope:**
    - Height brush: sets per-tile depth/height values (the isometric staple); brush size/strength (linear falloff documented); live depth-key update (stepped terrain renders correctly immediately); budget: height-paint on 10k dirty cells stays within the M2-ISO-02 budget (measured).
    - Animation layers: per-layer tile animation assignment (frame cycle params from M2-TILE-02), layer visibility toggle.
    - Tests (headless): height brush produces exact per-tile heights (golden); depth rebuild after 10k-cell paint ≤ 0.2 ms (budget entry re-run); animation layer toggle changes render (golden).
  - **Verify:** `ctest -R editor_height_brush` green; budget recorded.
  - **Size:** ~250 lines + tests

## Sprite/animation editor

- [ ] **M5-ED-10 · Frame editor**
  - **Refs:** FR-8.3 (frame editor, preview playback)
  - **Depends:** M5-ED-05, M3-ANIM-01
  - **Scope:**
    - Frame editor for sprite-sheet animations: frame list (add/remove/reorder, per-frame duration editable — ticks), frame picker from atlas (rect drag with grid snap to atlas cells), preview playback (plays the clip in the editor viewport, deterministic playback for tests).
    - Edits write animation data (M3-ANIM-01 format) as transactions.
    - Tests (headless): build a 4-frame clip via scripted actions → animation data golden; playback frame sequence deterministic.
  - **Verify:** `ctest -R editor_frames` green.
  - **Size:** ~250 lines + tests

- [ ] **M5-ED-11 · State-machine graph editor**
  - **Refs:** FR-8.3 (state-machine graph editor)
  - **Depends:** M5-ED-10, M3-ANIM-02
  - **Scope:**
    - Graph editor for sheet-based ASMs: node/transition create/delete, parameter conditions editable (the P0 parameter set), graph validation (unreachable nodes, missing transitions to required states → warnings, not silent).
    - Preview: run the FSM in the editor with scripted parameter changes; node/transition highlights follow (headless: state sequence logged).
    - Tests (headless): build the 4-node FSM from M3-ANIM-02's test by editor actions → byte-identical FSM data; validation warnings fire for a crafted broken graph.
  - **Verify:** `ctest -R editor_fsm_graph` green.
  - **Size:** ~300 lines + tests

## Asset browser & play/debug

- [ ] **M5-ED-12 · Asset browser**
  - **Refs:** FR-8.4 (import, preview, atlas view, texture memory view)
  - **Depends:** M3-ASSET-03, M5-ED-01
  - **Scope:**
    - Browser: project asset list (thumbnails/preview for textures, waveforms-lite for audio — document the cheap preview model), import button (runs `laige-asset import` path in-process), atlas view (shows packed atlas with rects, sizes, pinned state), texture memory view (per-atlas bytes + total vs budget, G-R9).
    - Import failures surface the exact structured error (NFR-13.3) inline.
    - Tests (headless): import a new texture fixture → appears in browser data + atlas repacked (deterministic); corrupt file → inline error, no crash.
  - **Verify:** `ctest -R editor_assets` green.
  - **Size:** ~250 lines + tests

- [ ] **M5-ED-13 · Play mode (in-editor run/stop)**
  - **Refs:** FR-8.5 (play mode, stop, P0)
  - **Depends:** M5-ED-07, M1-HEAD-01
  - **Scope:**
    - Play: editor hands the current scene (including unsaved edits — documented: play uses a *copy*; edits during play are discarded on stop unless "save on stop" is set) to a running engine instance (sim + render in-process, editor UI overlaid as debug widgets).
    - Stop: ordered shutdown of the play instance (CONC-006), editor state restored (selection, viewport) exactly.
    - Play records a replay log automatically (debug runs, FR-11.3) → usable by the replay viewer.
    - Tests (headless): play 300 ticks with scripted input → replay log written; stop → editor state identical to pre-play (verified); unsaved-edit copy semantics verified.
  - **Verify:** `ctest -R editor_play` green.
  - **Size:** ~300 lines + tests

- [ ] **M5-ED-14 · Profiler overlay**
  - **Refs:** FR-8.5 (profiler overlay: per-system time, allocs, draw calls, net traffic)
  - **Depends:** M5-ED-13, M1-PROF-01, M2-SPRITE-04
  - **Scope:**
    - Overlay panel (toggleable, cheap-when-hidden per DBG-004): per-system time (last N ticks, bar list), allocation counts (sim + batcher), draw calls + texture binds, net bytes (0 until M6 — field present, n/a labeled), tick/frame time percentiles.
    - Reads profiler snapshots only (DBG-005: no locks into sim state, no mutation).
    - Capture: "capture counters to report" (DBG-007) — timestamped file export from the overlay (one action).
    - Tests (headless): overlay data matches the profiler snapshot exactly for a scripted run; hidden overlay adds ≤ 1% tick time (measured, DBG-004).
  - **Verify:** `ctest -R editor_profiler` green; hidden-cost measurement recorded.
  - **Size:** ~250 lines + tests

- [ ] **M5-ED-15 · Deterministic replay viewer (scrub + diff)**
    - **Refs:** FR-8.5 (replay viewer: scrub + diff against live state), FR-11.3
    - **Depends:** M5-ED-13, M1-DET-05
    - **Scope:**
      - Viewer: load a replay log; scrub (jump to tick t — replays fast-forward headlessly to t, deterministic); displays per-tick state summary (entity count, key positions, system times) and can render the tick offscreen (golden-comparable).
      - Diff mode: compare replay tick t against live play-mode state at tick t → the M1-DET-05 diff output in a panel (first divergent component + tick).
      - Tests (headless): scrub to t → state == direct-run state at t (hash equality); diff of two replays matching M1-DET-05's expected output.
    - **Verify:** `ctest -R editor_replay` green.
    - **Size:** ~250 lines + tests

## P1 editor features (explicit P1 — descopable by decision)

- [ ] **M5-ED-16 · Custom component inspector UI (FR-8.8, P1)**
  - **Refs:** FR-8.8 (components register inspector UI via declarative schema — no editor code for custom components)
  - **Depends:** M4-TRAIT-01, M5-ED-05
  - **Scope:**
    - Inspector renders user-component fields from the trait's declarative schema (slider, curve, asset-ref, enum — the P0 schema set documented); the PRD Appendix B `Health` example renders two sliders with no editor code change.
    - Schema validation at registration (bad schema → registration error, FR-12.3).
    - Tests: Appendix B component inspects/edits in the headless editor session (value round-trip).
  - **Verify:** `ctest -R editor_custom_inspector` green.
  - **Size:** ~250 lines + tests

- [ ] **M5-ED-17 · Script console (FR-8.6, P1)**
  - **Refs:** FR-8.6 (evaluate script expressions against a running game)
  - **Depends:** M5-ED-13, M4-SCRIPT-04
  - **Scope:**
    - Console panel (editor, play mode only): evaluates Lua expressions against the running VM (the M4-SCRIPT-04 sandbox applies — same limits, same error grammar); expression history (bounded, per session).
    - Safe-by-default: console has no additional powers beyond the binding surface (audited — M4-SCRIPT-04 global audit re-run in editor context).
    - Tests (headless): evaluate `world.entity_count()` → correct; malicious expression (attempt to escape sandbox) rejected with the documented error.
  - **Verify:** `ctest -R editor_console` green.
  - **Size:** ~150 lines + tests

- [ ] **M5-ED-18 · Memory inspector (FR-11.4, P1)**
  - **Refs:** FR-11.4 (live pools, per-system memory, peak tracking)
  - **Depends:** M5-ED-14, M1-PROF-01
  - **Scope:**
    - Memory panel: per-pool live bytes/counts (entities, components, contacts, particles, audio, net, string table — the PRD §10.4 pool list), per-system attribution, high-water marks over the session, reset-on-frame pools shown separately (documented which counters are per-frame-reset vs cumulative).
    - Reads pool accounting only (no new instrumentation into hot paths beyond the existing counters — DBG-004 cost measured).
    - Tests: values match pool accounting API exactly for a scripted workload.
  - **Verify:** `ctest -R editor_memory` green; hidden-cost measurement recorded.
  - **Size:** ~200 lines + tests

## Validation & gate

- [ ] **M5-TEST-01 · Editor smoke + golden validation**
  - **Refs:** TEST-008, NFR-8.4 (editor stability)
  - **Depends:** M5-ED-15
  - **Scope:**
    - End-to-end scripted session (`laige-editor --headless --script build_iso_game.lua`): from an empty project — create scene, build tilemap with height steps, create player + walls, animate, set camera, import assets, pack, **play 600 ticks headless, stop, save** — exits 0 with a saved project that then runs standalone via `laige-run`.
    - Viewport golden tests: 3 canonical editor views (iso grid, inspector open, tilemap height brush active) compared to committed references (tolerance per M2-GOLD-01).
    - 72 h editor soak scheduled for M7/M8 (not here); here: 4 h soak on CI nightly (leak-free, ASan).
  - **Verify:** scripted session green on CI (all 3 P0 OSes); goldens green; nightly 4 h soak leak-free.
  - **Size:** scripts + goldens

- [ ] **M5-EXIT-01 · M5 exit gate**
  - **Refs:** PRD §15 M5 exit criteria
  - **Depends:** all other M5 steps
  - **Scope:**
    - Confirm and record: the scripted "build a small isometric game entirely in the editor" session (M5-TEST-01) is green and the produced game runs standalone; P1 editor steps (ED-16/17/18) status recorded (done, or descope decision with ADR per the milestone intro rule).
    - Update Progress Board.
  - **Verify:** evidence links present; no open P0 M5 step.
  - **Size:** docs only
