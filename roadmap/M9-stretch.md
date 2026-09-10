# M9+ — Stretch (post-1.0)

**PRD:** §15 M9, §5 non-goals · **Status:** proposals only

**Rule for this file:** every item here is a **proposal step**, not an implementation
step. An M9 item becomes real work only after: (1) a PRD revision adds its scope,
(2) an ADR is written under `docs/decisions/`, (3) it is split into normal-sized
steps with IDs `M9-<SUBSYSTEM>-01…` in its own milestone file. Until then, none of
the sub-work below is in scope for any agent.

The PRD's P1 labels (M6–M7) vs the M9 stretch list overlap for a few features
(skeletal animation, custom inspector UI). Those were placed in M5/M7 where their
surfaces live and are already tracked there; only the *unplaced* stretch items are
listed here.

---

- [ ] **M9-WEBGL-01 · WebGL/WASM target proposal**
  - **Refs:** PRD §6 (Web P2, M8+), §11 (GLAD/OpenGL ES 3.0 note)
  - **Scope (proposal only):**
    - Draft the PRD revision: WASM build of the engine + GL ES 3.0 renderer layer (one renderer, version-gated feature layers per PRD §6), what stays identical (sim, net protocol, assets) and what changes (audio backend, file I/O, threading model — WASM threads caveat documented).
    - ADR skeleton: context-creation boundary, CI job (headless WebGL context via a browser harness — note: browser testing uses LibreWolf per the workspace AGENTS.md, never Chrome), performance budget deltas (no §8.1 claims on web until measured).
    - Exit of the proposal: PRD revision accepted + ADR accepted → work gets its own milestone file.
  - **Size:** docs only

- [ ] **M9-SKELE-01 · 2D skeletal (mesh) animation proposal**
  - **Refs:** FR-6.2 (P1), FR-6.3 (P1 skeletal), PRD §15 M9 ("2D skeletal animation polish")
  - **Scope (proposal only):**
    - Draft the PRD revision: 2D bone hierarchy (transform bones), keyframed clips, additive layers, weight-blended vertex animation (CPU or GPU — decision with a measured comparison per CORE-001), editor support (extends M5 frame editor), asset format (bone/clip types in the pack manifest).
    - Explicit boundary: 2D skeletal only (PRD §5 non-goal: no 3D character animation, no 3D rigs).
    - Exit of the proposal: PRD revision + ADR (CPU vs GPU path decided on measurement, or CPU-first with a GPU ADR deferred) → own milestone file.
  - **Size:** docs only

- [ ] **M9-MESH-01 · 3D mesh scenery proposal**
  - **Refs:** PRD §5 non-goals ("3D meshes for static scenery: stretch goal, M8+"), §15 M9
  - **Scope (proposal only):**
    - Draft the PRD revision: static low-poly meshes for scenery (no 3D physics, no 3D lighting beyond the 2D lighting pass extended minimally — decision), mesh asset type + import (GLTF-ish or own format — decision with DEP review), integration with the depth-key system (meshes participate in depth sorting by a documented key rule), draw-call budget impact on §8.1 (re-measured).
    - Explicit non-goals reaffirmed: no 3D physics, no 3D character animation.
    - Exit of the proposal: PRD revision + ADR → own milestone file.
  - **Size:** docs only

- [ ] **M9-VULKAN-01 · Optional Vulkan backend proposal**
  - **Refs:** PRD §6 ("Vulkan explicitly optional later; renderer API defined so a second backend is a clean addition")
  - **Depends (conceptual):** renderer API stability from M2/M4
  - **Scope (proposal only):**
    - Draft the PRD revision: Vulkan as a second backend behind the existing renderer abstraction (no feature promise in 1.0; parity list defined: what Vulkan 1.x backend must match — §8.1 budgets, golden images, determinism of *simulation* unaffected), driver coverage policy per P0 OS, CI strategy (vulkan-sdk per OS).
    - Non-goal guardrail: no Vulkan-only features; GL 3.3 remains the reference.
    - Exit of the proposal: PRD revision + ADR → own milestone file.
  - **Size:** docs only

- [ ] **M9-MOBILE-01 · Android/iOS target proposal**
  - **Refs:** PRD §6 (P3; "only if the WebGL/WASM path is solid" — dependency on M9-WEBGL-01's outcome, stated, not assumed)
    - **Scope (proposal only):**
    - Draft the PRD revision: native mobile (or WASM-embedded — decided by the WEBGL outcome), touch input (FR-4.1 P2 scope), audio backend per OS, memory budgets (new, mobile-class — §8.1 numbers do not carry over until measured), no editor on mobile (editor stays desktop).
    - Exit of the proposal: PRD revision + ADR + (if WASM route) M9-WEBGL-01 closed first → own milestone file.
  - **Size:** docs only

- [ ] **M9-TOUCH-01 · Touch input proposal**
  - **Refs:** FR-4.1 (touch, P2)
  - **Depends (conceptual):** M9-MOBILE-01 or M9-WEBGL-01 (a touch platform must exist)
  - **Scope (proposal only):**
    - Draft the PRD revision: touch as a device in the M3 input backend (touch points → virtual pointer + actions), multi-touch (2–10 points, documented), virtual gamepad overlay (P2 stretch, optional), action-abstraction reuse (no new action model — touch binds to existing actions).
    - Determinism note: touch input frames follow the M3-INPUT-03 format (replayable).
    - Exit of the proposal: PRD revision + ADR → own milestone file (or folded into the mobile milestone file).
  - **Size:** docs only

---

## Tracking

| Proposal | PRD revision | ADR | Status |
|---|---|---|---|
| M9-WEBGL-01 | — | — | ⬜ |
| M9-SKELE-01 | — | — | ⬜ |
| M9-MESH-01 | — | — | ⬜ |
| M9-VULKAN-01 | — | — | ⬜ |
| M9-MOBILE-01 | — | — | ⬜ |
| M9-TOUCH-01 | — | — | ⬜ |
