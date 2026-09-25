# ADR 0005 — Default isometric projection for new projects: 2:1 dimetric

- **Status:** Accepted
- **Date:** 2026-09-25
- **Decider:** Roadmap step M2-DEC-01 (D-ISO)
- **Refs:** PRD v0.2 note, §4 (projection list), §15 (M2 exit), §18 item 8;
  FR-2.4, FR-2.5, FR-2.11, AC-4.1, AC-4.4, NFR-13.5; roadmap D-ISO, M2-PROJ-01,
  M2-CAM-02, M2-ISO-01, M2-SCENE-01, M2-SAMPLE-01, M2-AC-01

## Context

PRD v0.2 makes isometric the **primary projection**: the default template, the
reference scene for all visual/performance acceptance tests, and the focus of
new first-class requirements (depth keys, picking, grid-snap camera). PRD §18
item 8 (roadmap decision D-ISO) leaves one question open: which isometric
variant is the **template default** — 2:1 dimetric (the pixel-art standard) or
true 30°/60° isometric. Both must remain selectable per scene (FR-2.5).

The PRD already points one way, and this ADR confirms it rather than
re-deciding from scratch:

- §4: "Isometric (primary): 2:1 dimetric (pixel-art default) and true 30°/60°
  isometric, plus arbitrary shear per scene."
- FR-2.5: "Isometric (primary; **2:1 default**, true iso, custom shear)."
- Roadmap D-ISO register row, default if unresolved: "2:1 dimetric
  (PRD v0.2: pixel-art default)".
- Downstream M2 steps already assume it: M2-CAM-02 ("2:1 dimetric (default
  per D-ISO)"), M2-SAMPLE-01 (`iso-arena.laige`, "2:1 dimetric default (per
  D-ISO)"), AC-4.4 ("isometric is the default for the project template").

Both candidates are oblique presentations of the same axis-aligned 2D
simulation (PRD §4: no oblique simulation); they differ only in the camera
constants:

| | 2:1 dimetric (pixel-art standard) | True 30°/60° isometric |
|---|---|---|
| Construction | Affine: 45°-azimuth oblique view with the vertical squashed to ½ — the two ground axes are equally foreshortened, the vertical differs ("di"metric) | True orthographic axonometric: 45° azimuth, 35.264° elevation (`arcsin 1/√3`) — all three axes equally foreshortened |
| Screen delta per unit ground step | `(±2, 1)·k` — **integer** | `(±√3, 1)·k` — irrational |
| Ground-axis angle to screen horizontal | 26.565° (`arctan ½`) | 30° (hence "30°/60°": axes at 30°, tile corner at 60°) |
| Ground-axis foreshortening | `√2/2 ≈ 0.707` | `√(2/3) ≈ 0.816` |
| Vertical foreshortening | ½ | `√(2/3) ≈ 0.816` |
| Grid corners at 1× zoom | integer pixel positions | sub-pixel positions (√3 factor) |

The engine machinery is preset-independent: both variants are affine maps
with both world axes projecting **downward** on screen and a positive
`(x + y)` screen-y contribution, so the axis-aligned depth key
`f(x, y, tile height, layer)` (FR-2.2; exact 32-bit form is M2-ISO-01) and the
O(1) picking inverse (FR-2.11; M2-ISO-03) work for both — only the constants
differ. Both presets are one oblique camera matrix built once per scene
(M2-GL-03 / M2-CAM-02); there is **no per-frame cost difference** between
them. The choice is aesthetic and workflow, not a capability or performance
question.

## Decision

- **The default projection for new projects and templates is 2:1 dimetric.**
  The engine's default scene config selects it: projection mode `iso`
  (already the default per PRD v0.2) with iso preset `dimetric_2_1`.
- **Both presets remain selectable per scene/view** (FR-2.5, the AC-4.1
  config-only pattern): the scene config carries one iso-preset selector with
  the values `dimetric_2_1` (**default**), `true_iso_30_60`, and a custom
  shear (the two axis screen-deltas, per FR-2.5's "arbitrary shear per
  scene"; M2-CAM-02 owns the exact config key and the matrix builders from
  M2-GL-03). A game that wants true 30°/60° changes only its config — no
  simulation, depth-key, picking, or asset change.
- The default template (`iso-arena.laige`, NFR-13.5) and the M2-SCENE-01
  reference scene render in 2:1 dimetric; M2-AC-01 asserts the engine default
  (AC-4.4) and M2-EXIT-01 checks the template config.

Rationale:

1. **Pixel-art standard.** Laige's flagship genre is the isometric ARPG
   (PRD §3, §15) and the engine's default art paths (bitmap glyphs, tilemaps,
   the reference scene) are pixel-art-oriented; 2:1 dimetric is the standard
   pixel-art isometric projection.
2. **Integer grid↔screen mapping at 1× zoom.** `(±2, 1)·k` maps integer grid
   coordinates to integer pixel positions at 1× — no sub-pixel shimmer, no
   anti-aliasing ambiguity, and clean pick-boundary behavior (M2-ISO-03's
   documented boundary rule). True iso's `√3/2` factor puts every grid corner
   at an irrational position, which degrades the pixel-art workflow at 1×.
3. **Zero reversal cost.** The PRD already designates 2:1 as the pixel-art
   default (§4, FR-2.5) and the roadmap register's unresolved default is 2:1;
   confirming costs nothing, while making true iso the default would invert
   the PRD's designation for a style that is not the flagship.
4. **No simulation impact.** The choice is presentation-only (ARCH-009):
   simulation stays axis-aligned 2D, depth keys are computed from the
   axis-aligned world, never screen space (PRD §4, FR-2.2), and replication
   is unaffected.

## Alternatives considered

- **True 30°/60° isometric as the default** — the "photographic" iso look
  common in non-pixel-art iso games. Rejected as the default: it inverts the
  PRD v0.2/§4 designation, and its irrational projection factor degrades the
  pixel-art workflow (sub-pixel shimmer at 1×). It remains fully available
  per scene.
- **No default — make the user choose** — rejected: AC-4.4 and the M2 exit
  criterion ("isometric is the default template") require a single
  out-of-the-box look, and forcing the choice onto the majority case adds
  friction where API-001 wants the obvious path to be the good one.
- **Arbitrary custom shear as the default** — rejected: it generalizes the
  mechanism but defines no template look; custom shear remains the third
  preset value.

## Evidence

- This is a design decision **confirming an already-stated PRD default**, not
  a measured performance claim: both presets are one oblique camera matrix
  with no per-frame cost difference (2:1's integer deltas are, if anything,
  the cheaper constant set), so there is no performance differential to
  measure (CORE-001).
- Normative references: PRD v0.2 note, §4, §15, §18 item 8; FR-2.5; AC-4.4;
  NFR-13.5; roadmap D-ISO register row and the M2-CAM-02/M2-SAMPLE-01 scope
  wording that D-ISO unblocks.
- Enforcement after this ADR: M2-CAM-02 implements the preset selector with
  2:1 dimetric as the default; M2-AC-01 asserts the engine default; M2-EXIT-01
  checks the template config (the "Verify: ADR exists" step gate is met by
  this document).

## Consequences

- M2-PROJ-01 / M2-CAM-02 implement the preset selector with `dimetric_2_1` as
  the engine default; M2-GL-03's matrix builders support all three preset
  classes (2:1 dimetric, 30°/60°, custom shear) from the start — pure
  functions, trivial cost.
- The M2-SCENE-01 reference scene and all golden references (M2-GOLD-01) are
  pinned to 2:1 dimetric; changing the default later is a golden-referencing
  change (review conditions below).
- New-project templates and the future editor's (M5) "new iso scene" default
  use 2:1 dimetric.
- No changes to simulation, determinism, replication, or the depth-key
  formula (ARCH-009, PRD §4).

## Review conditions

- Reopen (new ADR + PRD revision) if pixel-art isometric stops being the
  flagship style and template demand measurably favors the other look.
- Reopen if 2:1 dimetric ever fails a render budget that true 30°/60° meets —
  no mechanism is known for this (same matrix class, no per-frame cost
  difference), but the budget suite (M2-PERF-01) would expose it.
- Any change of the default preset must regenerate and re-approve the
  M2-GOLD-01 golden references in the same change (TEST-008), and re-run
  M2-AC-01's engine-default assertion.
