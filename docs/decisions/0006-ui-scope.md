# ADR 0006 — M2 retained UI widget set: the seven FR-2.8 widgets

- **Status:** Accepted
- **Date:** 2026-09-25
- **Decider:** Roadmap step M2-DEC-02 (D-UI)
- **Refs:** PRD §18 item 5, FR-2.8, FR-4.4, §11 (UI framework is ours), §15 (M2
  scope "basic UI"); roadmap D-UI, M2-TEXT-02, M2-UI-01, M2-UI-02,
  M2-SCENE-01, M2-PERF-01, M2-EXIT-01; M3-INPUT-04; M5-ED-01

## Context

PRD §18 item 5 (roadmap decision D-UI) leaves one question open for M2: the
retained-mode UI widget set — "confirm minimal list for M2
(panel/button/text/image/list/input) vs. defer full UI to M4." Note that the
question text omits `slider`, while the normative P0 requirement FR-2.8 names
the widget list as "(panel, button, text, image, list, slider, input)" and the
decision register's D-UI default-if-unresolved is exactly that seven-widget
FR-2.8 list. The M2-DEC-02 scope restates the same seven widgets as the
default. This ADR confirms that default rather than re-deciding from scratch.

M2 needs *some* UI rather than deferring all of it to M4:

- M2's own acceptance chain includes UI: the M2-SCENE-01 reference scene
  carries "a UI panel", M2-UI-02 renders the UI pass and the first golden UI
  scene, and M2-TEXT-02's `TextItem` exists "in the UI pass" — deferring UI to
  M4 would orphan those steps.
- PRD §15 M2 scope names "basic UI" explicitly.
- M5-ED-01's editor shell ("menus, docks — built from M2 widgets") composes
  this set; deferring to M4 would not remove the editor's UI work.
- The UI framework is ours (PRD §11: explicitly *not* a dependency), so the
  set size is a scope choice, not a dependency constraint (DEP-001).

The seven widgets map one-to-one onto M2's downstream consumers; the event
column matches M2-UI-01's per-frame event list ("pointer enter/leave/
press/release, button pressed, slider changed, input text"):

| Widget | Retained role (M2-UI-01) | Own events (on top of the generic pointer enter/leave/press/release) |
|---|---|---|
| `panel` | Structural container holding child widgets; opaque-background option — the occlusion-culling target of M2-UI-02 | — |
| `button` | Press action; the primary interactive primitive (menus, options) | button pressed |
| `text` | Static text/label; content from the M2-TEXT-02 `TextItem` (string table — no per-frame `std::string`), with its alignment and max-width wrap | — |
| `image` | Fixed UI-atlas sub-rect; static art and decoration | — |
| `list` | Bounded scrollable container of child widgets (e.g. menu rows); children receive the generic pointer events | — (children's events) |
| `slider` | Continuous value within a documented `[min, max]` | slider changed |
| `input` | Single-line text entry; the widget that consumes keyboard events when focused (M3-INPUT-04's focus test) | input text |

Focus follows M2-UI-01's focus system (focusable widgets, tab/pointer focus,
the UI-first routing hook); the minimum keyboard consumer is `input` — the
widget M3-INPUT-04's routing test names ("with focused input widget, typing
goes to the widget and not to a bound world action").

## Decision

- **The M2 retained widget set is exactly seven widgets, in this canonical
  order: `panel`, `button`, `text`, `image`, `list`, `slider`, `input`.**
  M2-UI-01 implements exactly this list — the M2-DEC-02 Verify clause is
  "widget list in ADR == widget list implemented in M2-UI-01" — and its
  widget-kind enum follows this order.
- **The PRD §18 item 5 question text's omission of `slider` is treated as a
  typo in the question.** The normative list is FR-2.8's, and the register's
  D-UI default-if-unresolved is the same seven-widget list; confirming seven
  widgets matches both.
- **The widget candidates below are deferred out of M2 (named per the
  M2-DEC-02 scope).** They land later as additive widget kinds — the set only
  grows; none of the deferrals is a breaking change to the M2-UI-01 surface.

Deferred widgets:

| Deferred widget | Why out of M2 | Expected landing |
|---|---|---|
| checkbox (toggle) | No M2 acceptance fixture needs a boolean control; `button` + `slider` cover M2 settings UI | later step (M4/M5) as an additive widget kind, on demand |
| progress bar | No M2 game/UI content requires a status bar; the §15 profiler counters are not widgets and do not render in M2 | later step with the first consumer (e.g. editor load progress) |
| tab (tab bar) | Multi-pane switching is an editor feature; the M5 shell can compose from `button` + `panel` if needed | M5 editor, additively |
| dropdown (combo) | A collapsed list adds no M2 capability; a menu is `panel` + `list` | later step, on demand |
| tooltip | Hover hint; convenience, required by no M2 step | later step, on demand |
| multi-line input (text area) | M2 `input` is single-line; the first P0 consumer is chat (FR-10.7, P1, M6) | M6 with chat, or earlier if the editor needs it |
| separator (divider) | Decorative line; covered by the `image` widget where needed | none — covered by existing widgets |

- The UI stays presentation-only (ARCH-009): widgets never mutate sim state —
  they emit the M2-UI-01 per-frame event list, and routing to the world is
  M3-INPUT-04 (FR-4.4).
- No new dependencies: the UI framework is ours (PRD §11); widgets render as
  batched quads + text through the M2-SPRITE-02 path in the screen-space UI
  pass (M2-UI-02), one texture per UI atlas.

## Alternatives considered

- **The PRD §18 question's six-widget list (no slider)** — rejected: it
  contradicts the normative FR-2.8 list and the register's own D-UI default,
  and dropping `slider` removes a single continuous-value control with no M2
  cost benefit (its event and render surface are one row of the M2-UI-01 event
  list and one batched-quad pattern shared with the other widgets).
- **Deferring the whole UI to M4** (the §18 alternative) — rejected: M2's exit
  chain requires UI (M2-TEXT-02's `TextItem` "in the UI pass", the M2-SCENE-01
  reference scene's UI panel, M2-UI-02's golden scene), PRD §15 M2 scope names
  "basic UI", and M5-ED-01 composes the editor shell from the M2 widgets —
  deferring to M4 would orphan M2 steps without removing the editor work.
- **A larger M2 set (checkbox/progress/tab in P0)** — rejected (CORE-004,
  smallest complete change): no M2 acceptance criterion (AC-4.x, the 50k-sprite
  budget, the golden scene) exercises them, and shipping unused widget kinds
  would force M2-UI-01's CRUD/layout/focus tests to cover unused paths. The
  set is extensible additively — the deferred list above names the expected
  growth.

## Evidence

- This is a scope-confirmation decision, not a performance claim, so no
  CORE-001 measurement applies. The cost is bounded by construction: each
  widget is a batched quad (or text run) in the M2-SPRITE-02 UI pass — one
  texture per UI atlas (M2-UI-02) — so the UI pass adds a small, bounded number
  of (atlas, blend) groups to the PRD §8.1 ≤ 30 draw-call budget; M2-PERF-01
  measures the 50k-sprite reference scene including its UI panel, and the
  M2-UI-02 golden scene pins the render.
- Normative references: FR-2.8 (the P0 widget list), PRD §18 item 5, the
  roadmap D-UI register row; the M2-UI-01/M2-UI-02/M2-TEXT-02/M2-SCENE-01/
  M2-EXIT-01 scope wording; M3-INPUT-04 (the focus test names the `input`
  widget); M5-ED-01 (the editor shell is built from M2 widgets); PRD §11 (UI
  framework ours — no vendoring decision to make, DEP-001).
- Enforcement after this ADR: M2-UI-01 implements exactly the seven widgets and
  re-checks the list equality with this ADR in its Verify clause; M2-EXIT-01
  records the D-UI status alongside the other M2 scope statuses.

## Consequences

- M2-UI-01's widget-kind set is fixed at {panel, button, text, image, list,
  slider, input} in this order; its tests (tree CRUD, layout goldens, focus
  traversal, event delivery) cover exactly these kinds.
- M2-UI-02 renders all seven kinds in the screen-space UI pass; its golden
  fixture uses a subset of them.
- The M2-SCENE-01 reference scene's UI (a panel plus a few widgets) fits the
  set; its draw-call contribution is counted in M2-PERF-01's ≤ 30 budget.
- M3-INPUT-04's routing tests run against `input` (keyboard) and the
  pointer-focused widgets; no routing change is needed when deferred kinds
  land.
- M5-ED-01 composes the editor shell: menu = `panel` + `list` + `button`, dock
  = `panel` (+ `list`); deferred kinds (tab, dropdown, multi-line input,
  checkbox) are added as new widget kinds in M5 or earlier if a P0 consumer
  appears — additively, never breaking.
- No change to simulation, determinism, or replication (ARCH-009); no new
  dependency; `laige-api.json` unchanged (docs only).

## Review conditions

- Reopen (new ADR, additively) if a P0 consumer — a sample game, the
  M2-SCENE-01 scene, or the M5 editor — needs one of the named deferred
  widgets before M2-UI-02: add it as a new widget kind behind the same
  retained tree and extend M2-UI-01's tests; the seven-widget set stays
  implemented and tested.
- Reopen as a breaking change only if the set must *shrink*, which would break
  the M2-UI-01 Verify equality and M5's composition plan; no mechanism for
  this is known.
- If the PRD is revised, the §18 item 5 question text should be corrected to
  include `slider` (the typo this ADR resolves); the normative FR-2.8 list
  itself is unchanged.
- Any change to the set must be recorded by M2-EXIT-01 and re-verified by
  M2-UI-01's list-equality check against this ADR or its successor.
