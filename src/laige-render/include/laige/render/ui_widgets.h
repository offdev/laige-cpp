// laige-render UI widget tree (M2-UI-01): the retained, screen-space
// widget tree of FR-2.8 — the widget half of the M2 UI scope.
//
// FR-2.8: "Text rendering ...; immediate-mode-style UI widget tree
// (panel, button, text, image, list, slider, input) with a retained
// mode for menus; UI is a separate render pass, screen-space." The
// retained mode lands here: the seven-widget set confirmed by ADR
// 0006 (the M2-DEC-02 D-UI decision), a pool-backed node store with
// stable widget ids, the anchor + box layout model, the focus system
// (focusable widgets, tab/pointer focus), and the bounded per-frame
// pointer-event list. The FR-4.4 UI-first input routing hook is the
// raw pointer state: this step publishes the pointer hit + the focus
// handle; the action-based routing (a typed key goes to the focused
// input before the world) lands with M3-INPUT-04.
//
// M2-UI-01 carries NO rendering: the tree is presentation state read
// by the M2-UI-02 UI render pass (widgets render as batched quads +
// text through the M2-SPRITE-02 path, screen space, identity camera).
// No simulation coupling (ARCH-009): the tree never mutates sim state
// — it emits the per-frame event list, and the game routes those
// events.
//
//   UiWidgetKind      The seven ADR 0006 widget kinds
//   UiAnchor          The nine anchor positions (the layout model)
//   UiWidgetId        The stable, generation-checked widget id
//   UiNode            One retained widget (the pool element)
//   UiWidgetSpec      The value the game declares for one widget
//   UiPointerState    One frame's raw pointer state
//   UiEventKind       The seven event kinds
//   UiEvent           One delivered event (the per-frame list)
//   UiTree            The retained tree (layout, focus, events)
//
// ---------------------------------------------------------------------------
// The model (documented units)
// ---------------------------------------------------------------------------
//
// All layout values are SCREEN PIXELS, screen space, y DOWN — the
// UI pass's coordinate system (the M2-UI-02 identity screen matrix
// maps (x, y) to NDC; nothing world-space leaks in, RENDER-006). The
// root node's rect is the screen rect (`UiTree::Options`
// screenWidth/Height, default 1280 x 720 — the M2-GOLD-01 reference
// resolution).
//
// The widget set is EXACTLY the seven ADR 0006 widgets, in the
// ADR's canonical order (the M2-DEC-02 Verify clause: "widget list in
// ADR == widget list implemented in M2-UI-01"):
//
//   Panel   structural container; opaque-background option (the
//           M2-UI-02 occlusion-culling target); container of children
//   Button  press action (the menus / options primitive); label,
//           art rect, tint; focusable
//   Text    static text/label: the M2-TEXT-02 `TextItem` value (the
//           string-table handle, the glyph path, the scale, the
//           alignment, the max-width wrap); focusable: no
//   Image   fixed UI-atlas sub-rect (static art and decoration)
//   List    bounded scrollable container: children stacked
//           top-to-bottom in insertion order; container of children
//   Slider  continuous value in [min, max]; focusable
//   Input   single-line text entry (the M3-INPUT-04 keyboard
//           consumer); content is a string-table handle; focusable
//
// Only `Panel` and `List` may have children (a container); only
// `Button`, `Slider`, and `Input` are focusable. The other fields of
// the spec / node are kind-specific (the `UiWidgetSpec` field
// comments name which kind reads which).
//
// ---------------------------------------------------------------------------
// The layout model (anchor + box layout)
// ---------------------------------------------------------------------------
//
// Each node carries: an `anchor` (one of the nine positions), the
// anchor offsets `x` / `y` (screen px, y down), and the node size
// `width` / `height` (screen px). Containers (`Panel`, `List`)
// additionally carry `padding` (px, all four sides). `layout()`
// computes every node's `rect` (screen px, y down, half-open
// [x, x+w) x [y, y+h)) from the parent's CONTENT rect:
//
//   content rect of P   (Px + P.pad, Py + P.pad) x
//                       max(0, P.w - 2·P.pad) x max(0, P.h - 2·P.pad)
//   anchor point        (Cx + anchorH(anchor, Cw),
//                        Cy + anchorV(anchor, Ch))
//                       anchorH/V: Left/Top -> 0, Center -> extent/2
//                       (integer floor), Right/Bottom -> extent
//   node rect           (anchor point + (x, y) - the node's ANCHOR
//                       POINT in its own size): the anchor says which
//                       point of the node sits at the anchor point:
//                       TopLeft/Top/... offset (0, 0), *Center* /
//                       Center* offset (w/2, 0) / (0, h/2),
//                       Center (w/2, h/2), *Right/Bottom* (w, h)
//
// So a TopLeft-anchored node sits at (anchor point + offset); a
// Center-anchored node is CENTERED on (anchor point + offset); a
// TopRight node's right edge sits at the anchor point + x. All
// layout arithmetic is exact integer (int64) math (the only floats
// are the presentation conversion to render quads, ARCH-009).
//
// The `List` is the one stacking container: its children are laid
// out top-to-bottom in INSERTION ORDER from the content top, each
// child at its own size (the child's anchor / x / y are IGNORED in
// a list — a row's indent is a nested panel's job), and the list's
// `scroll` (px, y down) shifts the whole stack up. `layout()`
// normalizes `scroll` into the reachable range
// [0, max(0, contentHeight − contentH)] (a list shorter than its
// content scrolls to the bottom; a taller list never scrolls).
//
// Domain: a node's spec fields are bounded (width/height/padding/
// scroll in [0, kUiMaxDimPx], x/y in [-kUiMaxDimPx, kUiMaxDimPx] —
// 2^20 px, far beyond any screen). Computed rect coordinates are
// SATURATED to +/- kUiMaxCoordPx (2^21 − 1): a node pushed that far
// offscreen renders nothing anyway (the M2-UI-02 cull) and the
// saturation keeps the int32 storage exact — the total-function
// contract of the iso-picking saturation (no per-frame failure).
//
// A node's rect is VALID only after `layout()`: a node created or
// re-placed after the last `layout()` has a zero rect (it contains
// no pointer point until the next layout — the documented frame
// protocol below calls layout() every frame).
//
// ---------------------------------------------------------------------------
// The frame protocol + the pointer events (FR-4.4 hook)
// ---------------------------------------------------------------------------
//
// Per rendered frame (the game's main-thread update, before the
// M2-UI-02 UI pass declares the frame's quads):
//
//   layout()                O(n): every node's rect (above)
//   handlePointer(ptr)      O(n): one frame's pointer state — resets
//                           the event list, hit-tests, and delivers
//                           this frame's events (below); exactly ONE
//                           of handlePointer / beginFrame per frame
//   (game input handling)   e.g. setInputText on the focused input
//   events()                read the frame's event list
//
// `beginFrame()` is the pointerless variant (a keyboard-only frame):
// it resets the event list so `setInputText` events of this frame do
// not stack onto the previous frame's list.
//
// The pointer HIT is the TOPMOST containing node: the LAST node in
// the tree's traversal order (preorder, children in insertion order
// — the M2-UI-02 draw order, back-to-front, RENDER-003) whose rect
// contains the point (half-open, so a point exactly on a node's
// right or bottom edge is OUTSIDE that node — the iso-picking
// boundary rule). The root is LAYOUT-ONLY: it never receives pointer
// events (a pointer over empty screen space hits NOTHING — the
// world gets the input). An opaque panel in front of a button
// therefore occludes the pointer to the button, exactly as it
// occludes the render (the ADR 0006 occlusion-culling target).
//
// The per-frame events (the ADR 0006 event column, delivered to the
// topmost hit node only; a frame emits, in this order):
//
//   PointerLeave  the previous frame's topmost node N != this frame's
//                 top (N leaves first, then the enter — the
//                 deterministic order)
//   PointerEnter  this frame's topmost node N != the previous frame's
//   PointerPress  the pointer transitions not-pressed -> pressed
//                 WHILE on node N (the pointer is outside every node
//                 -> no press event; the press is still tracked for
//                 the release)
//   PointerRelease the pointer transitions pressed -> not-pressed
//                 WHILE on node N
//   ButtonPressed  a PointerRelease on a Button that ALSO received
//                 the frame's PointerPress (press and release on the
//                 same button — a click; the press moves off the
//                 button before the release -> no ButtonPressed)
//   SliderChanged  the slider drag (below) changes the value
//   InputText      `setInputText` changed the focused-or-not input's
//                 text (programmatic — the M3-INPUT-04 keyboard
//                 path will route typed keys here)
//
// The SLIDER DRAG: a pointer press ON a slider records the slider
// as the press owner and immediately sets the value from the press
// x (SliderChanged when it moves); while the pointer stays pressed
// the slider keeps tracking the pointer x (even when the pointer
// leaves the slider's rect — the standard drag; the value is
// clamped to [min, max]); the release ends the drag. The value from
// x: `min + clamp((x - rectX) / width, 0, 1) * (max - min)` (a
// zero-width slider takes no pointer input — no division).
//
// The FR-4.4 UI-FIRST ROUTING HOOK (raw pointer state, M2):
// `pointerHit()` returns the topmost node under the LAST processed
// pointer position (the invalid id when the pointer is over empty
// screen space). The game checks it before routing the pointer to
// the world: a hit means the UI consumed the frame's pointer input.
// `focus()` is the keyboard half of the hook: the focused input is
// the widget that consumes typed keys (M3-INPUT-04 routes the keys;
// M2's `setInputText` is the programmatic path).
//
// ---------------------------------------------------------------------------
// The focus system
// ---------------------------------------------------------------------------
//
// Focusable widgets: `Button`, `Slider`, `Input` (the ADR 0006 focus
// row; `Panel` / `Text` / `Image` / `List` are never focused).
//
//   focus()         the focused widget (the invalid id when none)
//   focusWidget(id) explicit focus (tab / editor selection): invalid,
//                   stale, or non-focusable -> InvalidArgument
//   focusNext()     tab forward: the next focusable widget in the
//                   traversal order (the deterministic preorder);
//                   from the last, wraps to the first; from
//                   unfocused, the first focusable
//   focusPrev()     tab backward (the previous focusable, wrapping to
//                   the last; from unfocused, the last focusable)
//   (pointer)       a PointerPress ON a focusable widget takes the
//                   focus (pointer focus — the standard UI behavior)
//   (removal)       removing a focused widget (or a subtree that
//                   contains it) clears the focus
//
// The traversal order is the DETERMINISTIC preorder (parent before
// children, children in insertion order) — the same order the
// M2-UI-02 pass draws in and the hit test uses.
//
// ---------------------------------------------------------------------------
// Storage (CORE-005, PERF-004)
// ---------------------------------------------------------------------------
//
//   widget pool   Pool<UiNode> (the M0-CORE-05 stable-handle pool —
//                 generation-checked ids, CPP-007; LIFO slot
//                 recycling; no per-operation allocation)
//   event list    one UiEvent[maxEvents] array (the frame's events)
//
// `UiTree::create` performs the two backing allocations (setup
// path); EVERY per-frame and per-operation path (addChild, remove,
// layout, handlePointer, the focus ops, the setters) allocates
// nothing (PERF-003 — the zero-allocation proof test).
//
// ---------------------------------------------------------------------------
// Ownership, threading, failure (CORE-009, CONC-001, CORE-008)
// ---------------------------------------------------------------------------
//
// One owner: the game's UI update thread (the same single-owner
// pattern as the SpriteBatcher and the StringTable). The render
// thread never touches the tree: the M2-UI-02 UI pass declares the
// frame's UI quads from the tree's rect/payload state in the cull/
// batch stage (the PresentationSnapshot pattern, ARCH-009). The tree
// is NOT thread-safe by design (CONC-001).
//
// The tree is move-only (the house pattern): the moved-from tree is
// stopped (capacity 0). Every `StringRef` stored in a node (the
// button label, the text item's text, the input text) is a
// NON-OWNING handle into the scene's StringTable (PRD §10.4 — the
// table and its handles must outlive every frame that reads the
// tree, the TextItem.font lifetime contract). The `TextItem.item`
// of a Text widget carries its own font pointer (non-owning, the
// scene's glyph atlas).
//
// Failure (no silent failure, CORE-008; first failure wins):
//
//   - create: maxWidgets outside [2, kUiTreeMaxWidgets], maxEvents
//     outside [1, kUiTreeMaxEvents], or a screen dimension outside
//     [1, kUiMaxDimPx] -> InvalidArgument (no allocation, no log);
//   - addChild: stopped tree, a kind outside the seven-widget set,
//     an invalid / non-container parent, an out-of-domain spec
//     field, or a slider with min >= max (a NaN min/max fails the
//     comparison) -> InvalidArgument (no log); the widget budget ->
//     BudgetExhausted + one rate-limited Warn `ui_tree/widget_budget`
//     (the facade windows the repeats, LOG-004);
//   - remove: stopped tree, the root, or a stale/invalid id ->
//     InvalidArgument (no log);
//   - layout / handlePointer / the setters / focusWidget: stopped
//     tree or a stale/invalid id (a setter's kind mismatch, a
//     non-focusable focusWidget target, an out-of-domain value) ->
//     InvalidArgument (no log);
//   - event overflow: a frame that emits more than maxEvents events
//     keeps the FIRST maxEvents (the later ones are dropped) + one
//     rate-limited Warn `ui_tree/event_overflow` per dropped event
//     (the cumulative count in droppedEventsTotal()).
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003/004, DOC-004)
// ---------------------------------------------------------------------------
//
//   create      O(maxWidgets): the two backing allocations. Setup
//               path only.
//   addChild    O(subtree-tail walk of the parent's children in the
//               worst case — O(1) for the common append): one pool
//               create. No allocation; the budget Warn is a cold,
//               rate-limited path (LOG-003).
//   remove      O(subtree size): every node of the subtree is
//               unlinked + destroyed exactly once. No allocation.
//   layout      O(n): one preorder pass over the n live widgets. No
//               allocation, no logging.
//   handlePointer O(n): one preorder hit test + a constant number of
//               transitions. No allocation; the overflow Warn is a
//               cold, rate-limited path.
//   focusNext / focusPrev  O(n) each (one or two preorder passes).
//   events()    O(1); the list is the frame's array (no copy).
//
//   Call site: once per frame per UI scene, in the game's UI update
//   (before the M2-UI-02 UI pass) — never in the simulation tick
//   (ARCH-002). The per-frame UI cost is part of the composite 50k
//   render-CPU budget (PRD §8.1 — measured with M2-PERF-01; no
//   standalone budgets.json entry for this step).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
// - Do not mutate the structural fields (`parent`, `firstChild`,
//   `nextSibling`) of a node directly: they are the tree's own
//   invariants (addChild / remove maintain them). The layout /
//   payload fields have the documented domains; a spec violation at
//   add time is a caller bug the validation rejects.
// - Do not call handlePointer without layout() first: a node added or
//   re-placed since the last layout has a zero rect and contains no
//   pointer point (the frame protocol calls layout() every frame).
// - Do not hold a widget id past its remove(): the pool's
//   generation check invalidates it (CPP-007) — stale ids fail the
//   null-safe reads and every operation that takes an id.
// - Do not point a node's StringRef / TextItem at a dead table or
//   atlas: the scene's StringTable and glyph atlases must outlive
//   every frame that reads the tree (PRD §10.4, the TextItem.font
//   contract).
// - Do not expect per-widget hover state: only the TOPMOST hit node
//   receives enter/leave/press/release (a node underneath an opaque
//   panel gets nothing while occluded — the documented model).
// - Do not expect programmatic slider changes to emit
//   SliderChanged: only the pointer drag emits it (setSliderValue is
//   the silent set — the M2-UI-02 render reads the value).
// - Do not read the event list across handlePointer calls: each
//   handlePointer / beginFrame starts the frame's list fresh.
//
// Canonical narrative: docs/concepts/coordinates.md §4.13 (ARCH-008);
// API contract: docs/api/ui_widgets.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/pools.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/text_items.h"
#include "laige/result.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The seven ADR 0006 widget kinds, in the ADR's canonical order
// (the M2-DEC-02 Verify clause: the implemented set is exactly this).
inline constexpr std::uint32_t kUiWidgetKindCount = 7;

// The screen-pixel dimension / offset domain: 2^20 px (a 20-bit
// screen dimension — far beyond any UI, and < 2^24 so every
// coordinate inside the domain is exact in float, the M2-SPRITE-03
// float-exact domain).
inline constexpr std::uint32_t kUiMaxDimPx = 1u << 20;

// The computed-rect coordinate saturation bound (2^21 − 1): a node
// pushed that far offscreen renders nothing (the M2-UI-02 cull);
// the saturation keeps the int32 storage exact (the header's layout
// domain section).
inline constexpr std::int32_t kUiMaxCoordPx = (1 << 21) - 1;

// The widget-pool budget domain: [2 (the root + one widget), 65536]
// (65536 x ~240 B/node ~= 15 MB — the declared scene UI budget, S-6).
inline constexpr std::uint32_t kUiTreeMaxWidgets = 65536;
// The documented default (a typical game UI: a menu, a settings
// panel, a HUD — a few hundred widgets at most).
inline constexpr std::uint32_t kUiDefaultMaxWidgets = 1024;

// The per-frame event-list domain: [1, 4096] (a frame emits at most
// ~7 events in the documented model; 64 is 9x headroom).
inline constexpr std::uint32_t kUiTreeMaxEvents = 4096;
inline constexpr std::uint32_t kUiDefaultMaxEvents = 64;

// The documented default screen (the M2-GOLD-01 reference
// resolution).
inline constexpr std::int32_t kUiDefaultScreenWidth = 1280;
inline constexpr std::int32_t kUiDefaultScreenHeight = 720;

// ---------------------------------------------------------------------------
// The widget kinds (ADR 0006 — the canonical order)
// ---------------------------------------------------------------------------

// The seven ADR 0006 widget kinds, in the ADR's canonical order. The
// enumerator values are stable (PRD §9.4: additive only).
enum class UiWidgetKind : std::uint8_t {
  // The structural container (children; the opaque-background option
  // — the M2-UI-02 occlusion-culling target).
  Panel = 0,
  // The press action (the menus / options primitive); focusable.
  Button = 1,
  // The static text/label (the M2-TEXT-02 TextItem value).
  Text = 2,
  // The fixed UI-atlas sub-rect (static art and decoration).
  Image = 3,
  // The bounded scrollable container (children stacked in insertion
  // order); the `scroll` px state.
  List = 4,
  // The continuous value in [min, max]; focusable.
  Slider = 5,
  // The single-line text entry (the M3-INPUT-04 keyboard consumer);
  // focusable.
  Input = 6,
};

// True for the focusable kinds (the ADR 0006 focus row): Button,
// Slider, Input. O(1).
[[nodiscard]] inline bool uiWidgetIsFocusable(UiWidgetKind kind) noexcept {
  return kind == UiWidgetKind::Button || kind == UiWidgetKind::Slider ||
         kind == UiWidgetKind::Input;
}

// True for the container kinds (the kinds that may have children):
// Panel, List. O(1).
[[nodiscard]] inline bool uiWidgetIsContainer(UiWidgetKind kind) noexcept {
  return kind == UiWidgetKind::Panel || kind == UiWidgetKind::List;
}

// ---------------------------------------------------------------------------
// The nine anchor positions (the layout model)
// ---------------------------------------------------------------------------

// The node's anchor: which point of the node is placed at the
// parent's anchor point (the header's layout section). The canonical
// 3 x 3 grid, row-major (Top, Center, Bottom x Left, Center, Right).
enum class UiAnchor : std::uint8_t {
  TopLeft = 0,
  TopCenter = 1,
  TopRight = 2,
  CenterLeft = 3,
  Center = 4,
  CenterRight = 5,
  BottomLeft = 6,
  BottomCenter = 7,
  BottomRight = 8,
};

// The anchor's HORIZONTAL point inside a content extent (Left -> 0,
// Center -> extent/2 integer floor, Right -> extent). Exact int64.
[[nodiscard]] inline std::int64_t anchorPointH(UiAnchor anchor,
                                              std::int64_t extent) noexcept {
  switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::CenterLeft:
    case UiAnchor::BottomLeft:
      return 0;
    case UiAnchor::TopCenter:
    case UiAnchor::Center:
    case UiAnchor::BottomCenter:
      return extent / 2;
    case UiAnchor::TopRight:
    case UiAnchor::CenterRight:
    case UiAnchor::BottomRight:
      return extent;
  }
  return 0;  // unreachable: the enum domain is exhaustive
}

// The anchor's VERTICAL point inside a content extent (Top -> 0,
// Center -> extent/2 integer floor, Bottom -> extent). Exact int64.
[[nodiscard]] inline std::int64_t anchorPointV(UiAnchor anchor,
                                              std::int64_t extent) noexcept {
  switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::TopCenter:
    case UiAnchor::TopRight:
      return 0;
    case UiAnchor::CenterLeft:
    case UiAnchor::Center:
    case UiAnchor::CenterRight:
      return extent / 2;
    case UiAnchor::BottomLeft:
    case UiAnchor::BottomCenter:
    case UiAnchor::BottomRight:
      return extent;
  }
  return 0;  // unreachable: the enum domain is exhaustive
}

// The HORIZONTAL offset of the node's anchor point from its top-left
// (Left -> 0, Center -> w/2 integer floor, Right -> w). Exact int64.
[[nodiscard]] inline std::int64_t anchorOffsetH(UiAnchor anchor,
                                               std::int64_t w) noexcept {
  switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::CenterLeft:
    case UiAnchor::BottomLeft:
      return 0;
    case UiAnchor::TopCenter:
    case UiAnchor::Center:
    case UiAnchor::BottomCenter:
      return w / 2;
    case UiAnchor::TopRight:
    case UiAnchor::CenterRight:
    case UiAnchor::BottomRight:
      return w;
  }
  return 0;  // unreachable: the enum domain is exhaustive
}

// The VERTICAL offset of the node's anchor point from its top-left
// (Top -> 0, Center -> h/2 integer floor, Bottom -> h). Exact int64.
[[nodiscard]] inline std::int64_t anchorOffsetV(UiAnchor anchor,
                                               std::int64_t h) noexcept {
  switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::TopCenter:
    case UiAnchor::TopRight:
      return 0;
    case UiAnchor::CenterLeft:
    case UiAnchor::Center:
    case UiAnchor::CenterRight:
      return h / 2;
    case UiAnchor::BottomLeft:
    case UiAnchor::BottomCenter:
    case UiAnchor::BottomRight:
      return h;
  }
  return 0;  // unreachable: the enum domain is exhaustive
}

// The computed-rect coordinate saturation (the header's layout domain
// section): exact, total, no failure.
[[nodiscard]] inline std::int32_t saturateCoord(std::int64_t v) noexcept {
  if (v < -static_cast<std::int64_t>(kUiMaxCoordPx)) return kUiMaxCoordPx;
  if (v > static_cast<std::int64_t>(kUiMaxCoordPx)) return kUiMaxCoordPx;
  return static_cast<std::int32_t>(v);
}

// ---------------------------------------------------------------------------
// The stable widget id (CPP-007)
// ---------------------------------------------------------------------------

// The stable, generation-checked widget id (the M0-CORE-05 pool
// handle): valid while the widget is live; a destroyed widget's id is
// rejected by the generation bump and is never valid again (a 2^32
// free wrap of one slot is the documented, effectively unreachable
// exception — the Pool::isValid contract). The default id {0, 0} is
// never valid.
using UiWidgetId = laige::PoolHandle;

// The invalid widget id (never a live handle — the Pool reserves
// generation 0).
inline constexpr UiWidgetId kInvalidUiWidgetId{0, 0};

// ---------------------------------------------------------------------------
// One retained widget (the pool element)
// ---------------------------------------------------------------------------

// One retained widget: the structural links (the tree's own — never
// assigned directly, the header's misuse warnings), the layout spec
// + the computed rect (screen px, y down), and the kind-specific
// payload. A plain value type (no ownership; the StringRefs / the
// TextItem font pointers are NON-OWNING scene handles — the header's
// ownership section).
struct UiNode {
  // The widget kind (ADR 0006 — immutable after creation).
  UiWidgetKind kind{UiWidgetKind::Panel};
  // The structural links (managed by addChild / remove — the tree's
  // own invariants): the parent (kInvalidUiWidgetId for the root),
  // the first child, and the next sibling (kInvalidUiWidgetId = none).
  UiWidgetId parent{kInvalidUiWidgetId};
  UiWidgetId firstChild{kInvalidUiWidgetId};
  UiWidgetId nextSibling{kInvalidUiWidgetId};
  // The layout spec (the header's layout section; screen px, y down):
  // the anchor, the anchor offsets, and the node size.
  UiAnchor anchor{UiAnchor::TopLeft};
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  // The container padding, px, all four sides (Panel / List only).
  std::int32_t padding{0};
  // The computed rect (layout() fills it; screen px, y down, half-
  // open). Zero before the first layout (a zero rect contains no
  // pointer point).
  std::int32_t rectX{0};
  std::int32_t rectY{0};
  std::int32_t rectW{0};
  std::int32_t rectH{0};
  // Panel: the opaque-background option (the M2-UI-02 occlusion
  // target).
  bool opaque{false};
  // Button: the label's string-table handle (PRD §10.4 — the scene's
  // StringTable must outlive the frame that renders it).
  StringRef label{};
  // Button / Image: the art's UV sub-rect in the UI atlas.
  SpriteUvRect image{};
  // Button / Image: the UI atlas's texture id (the scene's bindAtlas
  // id — M2-SPRITE-02).
  std::uint32_t atlasId{0};
  // Button / Image: the multiplicative tint (M2-SPRITE-02).
  SpriteTint tint{};
  // Text: the text block (the M2-TEXT-02 value — the string handle,
  // the glyph path + font pointer, the scale, the alignment, the
  // max-width wrap). The widget's rect is the spec's width/height
  // (the game sizes it at set-up, e.g. from measureText).
  TextItem item{};
  // List: the scroll offset, px, y down (layout() normalizes it into
  // the reachable range).
  std::int32_t scroll{0};
  // Slider: the value domain [min, max] (min < max — the addChild
  // validation) and the current value (the pointer drag +
  // setSliderValue write it; presentation-only float, ARCH-009).
  float sliderMin{0.0f};
  float sliderMax{0.0f};
  float sliderValue{0.0f};
  // Input: the current text's string-table handle (setInputText
  // writes it + emits the InputText event).
  StringRef text{};
};

namespace detail {

// The slider-value clamp (the header's pointer-events section): into
// [min, max]; a NaN snaps to min (total on all floats — the min/max
// domain is min < max, guaranteed by the addChild validation).
[[nodiscard]] inline float uiClampSliderValue(float min, float max,
                                              float v) noexcept {
  if (!(v >= min) && !(v <= max)) return min;
  if (v < min) return min;
  if (v > max) return max;
  return v;
}

// The tree's preorder walk (parent before children, children in
// insertion order — the deterministic traversal / draw order,
// RENDER-003, the header's focus section): iterative (no recursion —
// a 65k-deep chain must not overflow the stack), O(n), no allocation.
// Note: the Pool API has no const element read (pools.h), so the
// walk takes the pool non-const (a walk of a logically const tree is
// a non-mutating operation — the visitor MUST NOT mutate the tree
// mid-walk: the walk follows the structural links it visits).
template <typename F>
inline void uiTreePreorder(laige::Pool<UiNode>& nodes,
                           UiWidgetId root, F&& visit) noexcept {
  UiWidgetId cur = root;
  for (;;) {
    const UiNode* n = nodes.get(cur);
    visit(cur, n);
    if (nodes.isValid(n->firstChild)) {
      cur = n->firstChild;
      continue;
    }
    // No children: climb to the nearest ancestor with a live next
    // sibling (or end at the root).
    UiWidgetId up = cur;
    UiWidgetId next = kInvalidUiWidgetId;
    for (;;) {
      const UiNode* un = nodes.get(up);
      if (nodes.isValid(un->nextSibling)) {
        next = un->nextSibling;
        break;
      }
      const UiWidgetId p = un->parent;
      if (p == kInvalidUiWidgetId) break;  // the root: no next
      up = p;
    }
    if (next == kInvalidUiWidgetId) break;
    cur = next;
  }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The widget declaration (the addChild input)
// ---------------------------------------------------------------------------

// The value the game DECLARES for one widget (S-5: declaration, not
// draw). A plain value: the kind + the shared layout fields, then the
// kind-specific payload (which kind reads which field — the UiNode
// field comments above). The structural links are NOT part of the
// spec (the tree maintains them); `parent` names the parent (the
// invalid id = the tree root).
struct UiWidgetSpec {
  // The widget kind (the ADR 0006 seven — validated at addChild).
  UiWidgetKind kind{UiWidgetKind::Panel};
  // The parent widget id (kInvalidUiWidgetId = the tree root).
  UiWidgetId parent{kInvalidUiWidgetId};
  // The layout spec (the header's layout section — the same domains
  // as the UiNode fields above).
  UiAnchor anchor{UiAnchor::TopLeft};
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  std::int32_t padding{0};  // Panel / List only
  // Panel: the opaque-background option.
  bool opaque{false};
  // Button: the label's string-table handle.
  StringRef label{};
  // Button / Image: the art's UV sub-rect + atlas id + tint.
  SpriteUvRect image{};
  std::uint32_t atlasId{0};
  SpriteTint tint{};
  // Text: the text block (the M2-TEXT-02 value).
  TextItem item{};
  // List: the initial scroll offset, px.
  std::int32_t scroll{0};
  // Slider: the value domain + the initial value (min < max required;
  // the value is clamped into [min, max] — a NaN snaps to min).
  float sliderMin{0.0f};
  float sliderMax{0.0f};
  float sliderValue{0.0f};
  // Input: the initial text's string-table handle.
  StringRef text{};
};

// ---------------------------------------------------------------------------
// One frame's raw pointer state (FR-4.4 hook)
// ---------------------------------------------------------------------------

// One frame's raw pointer state: the pointer position (screen px, y
// down) + the primary button's pressed state. The game reads the raw
// input (GLFW / the platform layer) and hands this value to
// handlePointer once per frame.
struct UiPointerState {
  float x{0.0f};
  float y{0.0f};
  bool pressed{false};
};

// ---------------------------------------------------------------------------
// The per-frame events (the ADR 0006 event column)
// ---------------------------------------------------------------------------

// The seven event kinds (the ADR 0006 event table). The enumerator
// values are stable (PRD §9.4: additive only).
enum class UiEventKind : std::uint8_t {
  // The pointer entered the widget's rect (the topmost hit changed to
  // this widget).
  PointerEnter = 0,
  // The pointer left the widget's rect (the topmost hit changed away
  // from this widget).
  PointerLeave = 1,
  // The pointer pressed while on this widget.
  PointerPress = 2,
  // The pointer released while on this widget.
  PointerRelease = 3,
  // A press + release on this Button (a click — the ADR 0006
  // "button pressed" event).
  ButtonPressed = 4,
  // The slider drag changed this slider's value (`sliderValue`).
  SliderChanged = 5,
  // The input's text changed (`text` — the new string handle).
  InputText = 6,
};

// One delivered event: the kind, the widget it was delivered to, and
// the kind-specific payload (sliderValue for SliderChanged, text for
// InputText — the other kinds leave them at their defaults).
struct UiEvent {
  UiEventKind kind{UiEventKind::PointerEnter};
  UiWidgetId widget{kInvalidUiWidgetId};
  float sliderValue{0.0f};
  StringRef text{};
};

// ---------------------------------------------------------------------------
// The retained UI tree (M2-UI-01)
// ---------------------------------------------------------------------------

// The retained widget tree: the pool-backed node store, the layout
// pass, the focus system, and the per-frame pointer-event list (the
// preamble: the model, the layout model, the frame protocol, the
// pointer events, the focus, the storage, the ownership, and the
// failure contracts).
//
// Move-only (CORE-009): the tree owns the widget pool + the event
// array; it is created by create() and handed to its owner (the
// game's UI update).
class UiTree {
 public:
  // The tree budgets (the declared scene UI budget, S-6, API-006):
  // the widget pool capacity (INCLUDING the root node — the user
  // widget budget is maxWidgets − 1), the per-frame event-list
  // capacity, and the screen rect (the root's rect).
  struct Options {
    std::uint32_t maxWidgets{kUiDefaultMaxWidgets};
    std::uint32_t maxEvents{kUiDefaultMaxEvents};
    std::int32_t screenWidth{kUiDefaultScreenWidth};
    std::int32_t screenHeight{kUiDefaultScreenHeight};
  };

  // The stopped state (default / moved-from): valid() false, every
  // id-taking operation fails InvalidArgument, the reads are empty.
  UiTree() noexcept : nodes_(laige::Pool<UiNode>::Options{0}) {}

  // The set-up path (scene load): the two backing allocations (the
  // widget pool + the event array) + the root node (the layout-only
  // screen container — it never receives pointer events).
  // @budget O(maxWidgets); two allocations (setup path only)
  [[nodiscard]] static laige::Result<UiTree, laige::ErrorCode>
  create(Options options) noexcept;

  UiTree(const UiTree&) = delete;
  UiTree& operator=(const UiTree&) = delete;
  // The move (the house pattern — the moved-from tree is stopped:
  // the pool's move zeroes the source's capacity, the scalars are
  // zeroed so the source reports the stopped state).
  UiTree(UiTree&& o) noexcept
      : nodes_(std::move(o.nodes_)),
        events_(std::move(o.events_)),
        eventCapacity_(o.eventCapacity_),
        eventCount_(o.eventCount_),
        droppedEventsTotal_(o.droppedEventsTotal_),
        screenWidth_(o.screenWidth_),
        screenHeight_(o.screenHeight_),
        rootId_(o.rootId_),
        pointerHit_(o.pointerHit_),
        prevTop_(o.prevTop_),
        pressOwner_(o.pressOwner_),
        focus_(o.focus_),
        prevPressed_(o.prevPressed_) {
    o.eventCapacity_ = 0;
    o.eventCount_ = 0;
    o.droppedEventsTotal_ = 0;
    o.screenWidth_ = 0;
    o.screenHeight_ = 0;
    o.rootId_ = kInvalidUiWidgetId;
    o.pointerHit_ = kInvalidUiWidgetId;
    o.prevTop_ = kInvalidUiWidgetId;
    o.pressOwner_ = kInvalidUiWidgetId;
    o.focus_ = kInvalidUiWidgetId;
    o.prevPressed_ = false;
  }
  // The move assignment (the members are moved DIRECTLY — delegating
  // through a temporary would recurse: the temporary's own
  // move-assignment constructs yet another temporary).
  UiTree& operator=(UiTree&& o) noexcept {
    if (this != &o) {
      nodes_ = std::move(o.nodes_);
      events_ = std::move(o.events_);
      eventCapacity_ = o.eventCapacity_;
      eventCount_ = o.eventCount_;
      droppedEventsTotal_ = o.droppedEventsTotal_;
      screenWidth_ = o.screenWidth_;
      screenHeight_ = o.screenHeight_;
      rootId_ = o.rootId_;
      pointerHit_ = o.pointerHit_;
      prevTop_ = o.prevTop_;
      pressOwner_ = o.pressOwner_;
      focus_ = o.focus_;
      prevPressed_ = o.prevPressed_;
      o.eventCapacity_ = 0;
      o.eventCount_ = 0;
      o.droppedEventsTotal_ = 0;
      o.screenWidth_ = 0;
      o.screenHeight_ = 0;
      o.rootId_ = kInvalidUiWidgetId;
      o.pointerHit_ = kInvalidUiWidgetId;
      o.prevTop_ = kInvalidUiWidgetId;
      o.pressOwner_ = kInvalidUiWidgetId;
      o.focus_ = kInvalidUiWidgetId;
      o.prevPressed_ = false;
    }
    return *this;
  }
  ~UiTree() noexcept = default;

  // True iff the tree is live (create succeeded).
  [[nodiscard]] bool valid() const noexcept {
    return nodes_.capacity() != 0;
  }

  // The root node id (the layout-only screen container — never
  // receives pointer events, never focusable, never removable).
  [[nodiscard]] UiWidgetId root() const noexcept { return rootId_; }

  // The null-safe node read: nullptr for a stale/invalid id or the
  // stopped state (the total read — never UB). O(1). (Non-const: the
  // Pool API has no const element read — the pools.h pattern.)
  [[nodiscard]] const UiNode* node(UiWidgetId id) noexcept {
    return nodes_.get(id);
  }

  // Generation-checked liveness (CPP-007). O(1).
  [[nodiscard]] bool isValid(UiWidgetId id) const noexcept {
    return nodes_.isValid(id);
  }

  // The live widget count (the root included; 0 in the stopped
  // state). O(1).
  [[nodiscard]] std::uint32_t widgetCount() const noexcept {
    return nodes_.inUse();
  }

  // The widget pool's accounting snapshot (PRD §10.4). O(1).
  [[nodiscard]] laige::PoolStats widgetStats() const noexcept {
    return nodes_.stats();
  }

  // Declares one widget under `spec.parent` (the invalid id = the
  // root): the validation (first failure wins, InvalidArgument, no
  // log — the header's failure section), the pool create (the
  // BudgetExhausted + one rate-limited Warn
  // `ui_tree/widget_budget`), and the child-list append (insertion
  // order — the traversal / draw order, RENDER-003). The new node's
  // rect is zero until the next layout().
  // @budget O(parent's child tail) worst case, O(1) typical; no
  // allocation on the success path
  [[nodiscard]] laige::Result<UiWidgetId, laige::ErrorCode>
  addChild(const UiWidgetSpec& spec) noexcept;

  // Destroys the subtree rooted at `id` (post-order: every node of
  // the subtree is unlinked + destroyed; the pool's LIFO recycling
  // reuses the slots, the generation bumps invalidate every stale
  // id). The root is not removable; a stale/invalid id fails. A
  // removed focused widget (or a subtree containing the focus)
  // clears the focus. The slots are NOT freed (the pool is fixed at
  // create — the recycled slots serve the next addChild).
  // @budget O(subtree size); no allocation
  [[nodiscard]] laige::Status remove(UiWidgetId id) noexcept;

  // Computes every node's rect from the layout spec (the header's
  // layout model): one preorder pass, exact integer math, the
  // coordinate saturation, the list stacking + scroll normalization.
  // The rect is valid from this call until the next spec change /
  // addChild (the frame protocol calls this every frame).
  // @budget O(n) (n = the live widget count); no allocation
  [[nodiscard]] laige::Status layout() noexcept;

  // The pointerless frame boundary: resets the frame's event list
  // (a keyboard-only frame — the game's setInputText events of this
  // frame do not stack onto the previous frame's list).
  void beginFrame() noexcept {
    if (valid()) eventCount_ = 0;
  }

  // Processes one frame's pointer state (the header's frame protocol
  // + pointer-events sections): resets the event list, hit-tests the
  // topmost containing node (the root excluded — layout-only),
  // delivers this frame's events (leave/enter, press/release, the
  // button press, the slider drag, the focus-on-pointer-press), and
  // records the pointer hit (pointerHit() — the FR-4.4 routing
  // hook). The rects must be current (layout() this frame).
  // @budget O(n) (n = the live widget count); no allocation
  [[nodiscard]] laige::Status handlePointer(const UiPointerState& ptr) noexcept;

  // The topmost node under the LAST processed pointer position (the
  // invalid id when the pointer is over empty screen space or no
  // pointer was processed yet). The FR-4.4 UI-first routing hook:
  // a hit means the UI consumed the frame's pointer input.
  [[nodiscard]] UiWidgetId pointerHit() const noexcept { return pointerHit_; }

  // The frame's event list (valid until the next handlePointer /
  // beginFrame — the header's frame protocol). O(1); no copy.
  [[nodiscard]] std::span<const UiEvent> events() const noexcept {
    if (events_ == nullptr || eventCount_ == 0) {
      return std::span<const UiEvent>{};
    }
    return std::span<const UiEvent>(events_.get(), eventCount_);
  }

  // The cumulative count of events dropped by the per-frame
  // overflow policy (0 = healthy). O(1).
  [[nodiscard]] std::uint64_t droppedEventsTotal() const noexcept {
    return droppedEventsTotal_;
  }

  // The focused widget (the invalid id when nothing is focused).
  [[nodiscard]] UiWidgetId focus() const noexcept { return focus_; }

  // Sets the focus explicitly (tab / editor selection): an
  // invalid/stale id or a non-focusable kind fails InvalidArgument
  // (no log).
  [[nodiscard]] laige::Status focusWidget(UiWidgetId id) noexcept;

  // Tab forward: the next focusable widget in the traversal order,
  // wrapping from the last to the first; from unfocused, the first
  // focusable; a no-op when there are no focusable widgets.
  // @budget O(n) (one or two preorder passes); no allocation
  void focusNext() noexcept;

  // Tab backward: the previous focusable widget in the traversal
  // order, wrapping from the first to the last; from unfocused, the
  // last focusable; a no-op when there are no focusable widgets.
  // @budget O(n) (one preorder pass); no allocation
  void focusPrev() noexcept;

  // Changes a widget's layout spec (the anchor + the offsets + the
  // size — the header's layout domains): an invalid/stale id or an
  // out-of-domain value fails InvalidArgument (no log). The node's
  // rect is zero until the next layout().
  [[nodiscard]] laige::Status
  setPlacement(UiWidgetId id, UiAnchor anchor, std::int32_t x, std::int32_t y,
               std::int32_t width, std::int32_t height) noexcept;

  // Sets a slider's value (clamped into [min, max] — a NaN snaps to
  // min): an invalid/stale id or a non-slider kind fails
  // InvalidArgument (no log). The programmatic set emits NO
  // SliderChanged (only the pointer drag does — the header's
  // pointer-events section).
  [[nodiscard]] laige::Status setSliderValue(UiWidgetId id,
                                             float value) noexcept;

  // Sets a list's scroll offset, px, y down (domain [0, kUiMaxDimPx];
  // layout() normalizes it into the reachable range): an
  // invalid/stale id, a non-list kind, or an out-of-domain value
  // fails InvalidArgument (no log).
  [[nodiscard]] laige::Status setScroll(UiWidgetId id,
                                        std::int32_t y) noexcept;

  // Sets an input's text (the scene's string-table handle — PRD
  // §10.4): a no-op (no event) when the handle is unchanged;
  // otherwise the node's text is updated and an InputText event is
  // appended to the frame's event list (the overflow policy applies).
  // An invalid/stale id or a non-input kind fails InvalidArgument
  // (no log).
  [[nodiscard]] laige::Status setInputText(UiWidgetId id,
                                           StringRef text) noexcept;

 private:
  // The root node value (the layout-only screen container).
  static UiNode rootNode(std::int32_t w, std::int32_t h) noexcept {
    UiNode n;
    n.kind = UiWidgetKind::Panel;
    n.width = w;
    n.height = h;
    n.padding = 0;
    n.rectX = 0;
    n.rectY = 0;
    n.rectW = w;
    n.rectH = h;
    return n;
  }

  // The half-open pointer containment (the iso-picking boundary rule:
  // the right / bottom edges are exclusive). The int32 -> float
  // conversion is exact (the coordinates are < 2^24 — the header's
  // layout domain).
  static bool pointInRect(const UiNode& n, float x, float y) noexcept {
    return n.rectW > 0 && n.rectH > 0 &&
           x >= static_cast<float>(n.rectX) &&
           x < static_cast<float>(n.rectX) + static_cast<float>(n.rectW) &&
           y >= static_cast<float>(n.rectY) &&
           y < static_cast<float>(n.rectY) + static_cast<float>(n.rectH);
  }

  // The slider drag (the header's pointer-events section): the value
  // from the pointer x (a zero-width slider takes no input — no
  // division); the SliderChanged event on change.
  void dragSlider(UiWidgetId id, float x) noexcept {
    UiNode& n = nodes_.at(id);
    if (n.width <= 0) return;
    float t = (x - static_cast<float>(n.rectX)) /
              static_cast<float>(n.width);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float v = detail::uiClampSliderValue(
        n.sliderMin, n.sliderMax, n.sliderMin + t * (n.sliderMax - n.sliderMin));
    if (v != n.sliderValue) {
      n.sliderValue = v;
      emitEvent(UiEvent{UiEventKind::SliderChanged, id, v,
                        StringRef{}});
    }
  }

  // The frame-event append (the header's failure section): the first
  // maxEvents events of a frame are kept; a later one is dropped +
  // one rate-limited Warn `ui_tree/event_overflow` (the facade
  // windows the repeats, LOG-004) + the cumulative count.
  void emitEvent(const UiEvent& ev) noexcept {
    if (eventCount_ == eventCapacity_) {
      ++droppedEventsTotal_;
      LAIGE_LOG_WARN("ui_tree", "event_overflow",
                     "Dropped a UI event beyond the frame event budget",
                     laige::log::field("maxEvents", eventCapacity_));
      return;
    }
    events_[eventCount_++] = ev;
  }

  // Places one node's DIRECT children (the layout walk visits the
  // parent before the children, so the node's rect is valid here —
  // the children's own children are placed when the walk reaches
  // them, no recursion): the anchor/offset placement for Panel /
  // leaf containers, the stacking + scroll normalization for List.
  void placeChildren(UiWidgetId id) noexcept {
    UiNode& n = nodes_.at(id);
    const std::int64_t pad = n.padding;
    const std::int64_t cx = static_cast<std::int64_t>(n.rectX) + pad;
    const std::int64_t cy = static_cast<std::int64_t>(n.rectY) + pad;
    const std::int64_t cw =
        (std::int64_t)n.rectW > 2 * pad ? (std::int64_t)n.rectW - 2 * pad
                                        : 0;
    const std::int64_t ch =
        (std::int64_t)n.rectH > 2 * pad ? (std::int64_t)n.rectH - 2 * pad
                                        : 0;
    UiWidgetId child = n.firstChild;
    if (n.kind == UiWidgetKind::List) {
      // The total content height (the scroll-clamp domain — the
      // header's layout model).
      std::int64_t total = 0;
      for (UiWidgetId c = child; c != kInvalidUiWidgetId;
           c = nodes_.at(c).nextSibling) {
        total += nodes_.at(c).height;
      }
      const std::int64_t maxScroll = total > ch ? total - ch : 0;
      if (n.scroll > maxScroll) {
        n.scroll = static_cast<std::int32_t>(maxScroll);
      }
      std::int64_t y = cy;
      for (UiWidgetId c = child; c != kInvalidUiWidgetId;
           c = nodes_.at(c).nextSibling) {
        UiNode& cn = nodes_.at(c);
        cn.rectX = saturateCoord(cx);
        cn.rectY = saturateCoord(y - n.scroll);
        cn.rectW = cn.width;
        cn.rectH = cn.height;
        y += cn.height;
      }
    } else {
      for (UiWidgetId c = child; c != kInvalidUiWidgetId;
           c = nodes_.at(c).nextSibling) {
        UiNode& cn = nodes_.at(c);
        const std::int64_t ax = cx + anchorPointH(cn.anchor, cw);
        const std::int64_t ay = cy + anchorPointV(cn.anchor, ch);
        const std::int64_t rx =
            ax + cn.x - anchorOffsetH(cn.anchor, cn.width);
        const std::int64_t ry =
            ay + cn.y - anchorOffsetV(cn.anchor, cn.height);
        cn.rectX = saturateCoord(rx);
        cn.rectY = saturateCoord(ry);
        cn.rectW = cn.width;
        cn.rectH = cn.height;
      }
    }
  }

  // The pointer hit test: the LAST node in the traversal order whose
  // rect contains the point (the root excluded — layout-only, the
  // header's pointer-events section).
  UiWidgetId topmostAt(float x, float y) noexcept {
    UiWidgetId best = kInvalidUiWidgetId;
    detail::uiTreePreorder(nodes_, rootId_,
                          [this, x, y, &best](UiWidgetId id,
                                              const UiNode* n) {
                            if (id == rootId_) return;
                            if (pointInRect(*n, x, y)) best = id;
                          });
    return best;
  }

  laige::Pool<UiNode> nodes_;
  std::unique_ptr<UiEvent[]> events_;
  std::uint32_t eventCapacity_{0};
  std::uint32_t eventCount_{0};
  std::uint64_t droppedEventsTotal_{0};
  std::int32_t screenWidth_{0};
  std::int32_t screenHeight_{0};
  UiWidgetId rootId_{kInvalidUiWidgetId};
  // The frame's pointer state (the header's pointer-events section):
  // the last processed hit, the previous frame's topmost, the
  // previous frame's pressed state, the press owner (the node the
  // current press started on — the button-click / slider-drag
  // state).
  UiWidgetId pointerHit_{kInvalidUiWidgetId};
  UiWidgetId prevTop_{kInvalidUiWidgetId};
  UiWidgetId pressOwner_{kInvalidUiWidgetId};
  UiWidgetId focus_{kInvalidUiWidgetId};
  bool prevPressed_{false};
};

// ---------------------------------------------------------------------------
// The implementations (the header's sections name each contract)
// ---------------------------------------------------------------------------

inline laige::Result<UiTree, laige::ErrorCode>
UiTree::create(Options options) noexcept {
  // The create-validation (first failure wins, InvalidArgument, no
  // allocation, no log — the header's failure section).
  if (options.maxWidgets < 2 || options.maxWidgets > kUiTreeMaxWidgets ||
      options.maxEvents < 1 || options.maxEvents > kUiTreeMaxEvents ||
      options.screenWidth < 1 ||
      options.screenWidth > static_cast<std::int32_t>(kUiMaxDimPx) ||
      options.screenHeight < 1 ||
      options.screenHeight > static_cast<std::int32_t>(kUiMaxDimPx)) {
    return laige::Result<UiTree, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  UiTree t;
  t.screenWidth_ = options.screenWidth;
  t.screenHeight_ = options.screenHeight;
  t.nodes_ = laige::Pool<UiNode>(laige::Pool<UiNode>::Options{
      options.maxWidgets});
  t.events_ = std::make_unique<UiEvent[]>(options.maxEvents);
  t.eventCapacity_ = options.maxEvents;
  // The root (the layout-only screen container — the budget is >= 2,
  // so this create cannot fail).
  const auto slot = t.nodes_.create(rootNode(options.screenWidth,
                                             options.screenHeight));
  if (slot.isError()) {
    return laige::Result<UiTree, laige::ErrorCode>::failure(slot.error());
  }
  t.rootId_ = slot.value();
  return laige::Result<UiTree, laige::ErrorCode>::success(std::move(t));
}

inline laige::Result<UiWidgetId, laige::ErrorCode>
UiTree::addChild(const UiWidgetSpec& spec) noexcept {
  if (!valid()) {
    return laige::Result<UiWidgetId, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  // The kind domain (the ADR 0006 seven — a garbage kind value fails
  // too, the header's failure section).
  if (static_cast<std::uint32_t>(spec.kind) >= kUiWidgetKindCount) {
    return laige::Result<UiWidgetId, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  // The parent: the root, or a live container.
  UiWidgetId parent = spec.parent;
  if (parent != kInvalidUiWidgetId) {
    const UiNode* p = nodes_.get(parent);
    if (p == nullptr || !uiWidgetIsContainer(p->kind)) {
      return laige::Result<UiWidgetId, laige::ErrorCode>::failure(
          laige::ErrorCode::InvalidArgument);
    }
  }
  // The geometry domains (the header's layout domain section).
  if (spec.width < 0 || spec.width > static_cast<std::int32_t>(kUiMaxDimPx) ||
      spec.height < 0 ||
      spec.height > static_cast<std::int32_t>(kUiMaxDimPx) ||
      spec.x < -static_cast<std::int32_t>(kUiMaxDimPx) ||
      spec.x > static_cast<std::int32_t>(kUiMaxDimPx) ||
      spec.y < -static_cast<std::int32_t>(kUiMaxDimPx) ||
      spec.y > static_cast<std::int32_t>(kUiMaxDimPx) ||
      spec.padding < 0 ||
      spec.padding > static_cast<std::int32_t>(kUiMaxDimPx) ||
      spec.scroll < 0 ||
      spec.scroll > static_cast<std::int32_t>(kUiMaxDimPx)) {
    return laige::Result<UiWidgetId, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  // The slider domain (min < max — a NaN min/max fails the
  // comparison; the value is clamped, a NaN snaps to min).
  float sliderValue = spec.sliderValue;
  if (spec.kind == UiWidgetKind::Slider) {
    if (!(spec.sliderMin < spec.sliderMax)) {
      return laige::Result<UiWidgetId, laige::ErrorCode>::failure(
          laige::ErrorCode::InvalidArgument);
    }
    sliderValue = detail::uiClampSliderValue(spec.sliderMin, spec.sliderMax,
                                          spec.sliderValue);
  }
  UiNode n;
  n.kind = spec.kind;
  n.anchor = spec.anchor;
  n.x = spec.x;
  n.y = spec.y;
  n.width = spec.width;
  n.height = spec.height;
  n.padding = spec.padding;
  n.opaque = spec.opaque;
  n.label = spec.label;
  n.image = spec.image;
  n.atlasId = spec.atlasId;
  n.tint = spec.tint;
  n.item = spec.item;
  n.scroll = spec.scroll;
  n.sliderMin = spec.sliderMin;
  n.sliderMax = spec.sliderMax;
  n.sliderValue = sliderValue;
  n.text = spec.text;
  // The rect is zero until the next layout() (the header's layout
  // domain section).
  const auto slot = nodes_.create(n);
  if (slot.isError()) {
    // The widget budget (PERF-008 — the pool never grows; the owning
    // system logs, the pools.h contract; LOG-004 windows the repeats).
    LAIGE_LOG_WARN("ui_tree", "widget_budget",
                   "UI widget creation exceeded the tree widget budget",
                   laige::log::field("capacity", nodes_.capacity()));
    return laige::Result<UiWidgetId, laige::ErrorCode>::failure(
        laige::ErrorCode::BudgetExhausted);
  }
  const UiWidgetId id = slot.value();
  // The child-list append (insertion order — the traversal / draw
  // order, RENDER-003).
  if (parent == kInvalidUiWidgetId) parent = rootId_;
  UiNode& p = nodes_.at(parent);
  nodes_.at(id).parent = parent;
  if (p.firstChild == kInvalidUiWidgetId) {
    p.firstChild = id;
  } else {
    UiWidgetId tail = p.firstChild;
    while (nodes_.at(tail).nextSibling != kInvalidUiWidgetId) {
      tail = nodes_.at(tail).nextSibling;
    }
    nodes_.at(tail).nextSibling = id;
  }
  return laige::Result<UiWidgetId, laige::ErrorCode>::success(id);
}

inline laige::Status UiTree::remove(UiWidgetId id) noexcept {
  if (!valid() || id == rootId_ || !nodes_.isValid(id)) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  // The iterative post-order (no recursion — a 65k-deep chain must not
  // overflow the stack): descend to the leftmost live descendant,
  // unlink + destroy the leaf, climb to its parent, repeat until the
  // root of the subtree is removed. Every node of the subtree is
  // unlinked + destroyed exactly once (O(subtree size)).
  UiWidgetId cur = id;
  for (;;) {
    UiNode& n = nodes_.at(cur);
    if (nodes_.isValid(n.firstChild)) {
      cur = n.firstChild;
      continue;
    }
    // cur is a leaf: unlink it from its parent's child list.
    const UiWidgetId nextParent = n.parent;
    UiNode& p = nodes_.at(nextParent);
    if (p.firstChild == cur) {
      p.firstChild = n.nextSibling;
    } else {
      // The cur's predecessor in the parent's child list (the
      // first-child branch above handled the no-predecessor case).
      UiWidgetId prev = kInvalidUiWidgetId;
      UiWidgetId s = p.firstChild;
      while (s != cur) {
        prev = s;
        s = nodes_.at(s).nextSibling;
      }
      nodes_.at(prev).nextSibling = n.nextSibling;
    }
    (void)nodes_.destroy(cur);  // valid by construction: Status ok
    if (cur == id) break;
    cur = nextParent;
  }
  // The removed subtree may have held the focus / the press owner.
  if (!nodes_.isValid(focus_)) focus_ = kInvalidUiWidgetId;
  if (!nodes_.isValid(pressOwner_)) pressOwner_ = kInvalidUiWidgetId;
  return laige::Status{};
}

inline laige::Status UiTree::layout() noexcept {
  if (!valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  UiNode& root = nodes_.at(rootId_);
  root.rectX = 0;
  root.rectY = 0;
  root.rectW = screenWidth_;
  root.rectH = screenHeight_;
  // The preorder pass (the parent is visited before its children, so
  // every placement reads a valid parent rect — the header's layout
  // model).
  detail::uiTreePreorder(nodes_, rootId_, [this](UiWidgetId id,
                                                 const UiNode*) {
    placeChildren(id);
  });
  return laige::Status{};
}

inline laige::Status UiTree::handlePointer(const UiPointerState& ptr) noexcept {
  if (!valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  // The frame boundary (the header's frame protocol): the event list
  // is this frame's.
  eventCount_ = 0;
  const UiWidgetId top = topmostAt(ptr.x, ptr.y);
  // The hover transitions (leave first, then enter — the
  // deterministic order, the header's pointer-events section).
  if (top != prevTop_) {
    if (nodes_.isValid(prevTop_)) {
      emitEvent(UiEvent{UiEventKind::PointerLeave, prevTop_, 0.0f,
                        StringRef{}});
    }
    if (top != kInvalidUiWidgetId) {
      emitEvent(UiEvent{UiEventKind::PointerEnter, top, 0.0f,
                        StringRef{}});
    }
  }
  // The press transitions (the button click + the slider drag — the
  // header's pointer-events section).
  if (!prevPressed_ && ptr.pressed) {
    pressOwner_ = top;
    if (top != kInvalidUiWidgetId) {
      emitEvent(UiEvent{UiEventKind::PointerPress, top, 0.0f,
                        StringRef{}});
      const UiNode& n = nodes_.at(top);
      if (uiWidgetIsFocusable(n.kind)) focus_ = top;
      if (n.kind == UiWidgetKind::Slider) dragSlider(top, ptr.x);
    }
  } else if (prevPressed_ && !ptr.pressed) {
    const UiWidgetId owner = pressOwner_;
    pressOwner_ = kInvalidUiWidgetId;
    if (top != kInvalidUiWidgetId) {
      emitEvent(UiEvent{UiEventKind::PointerRelease, top, 0.0f,
                        StringRef{}});
      // The button click: a press + release on the SAME button
      // (a press that moved off the button emits no ButtonPressed —
      // the header's pointer-events section).
      if (owner == top && nodes_.at(owner).kind == UiWidgetKind::Button) {
        emitEvent(UiEvent{UiEventKind::ButtonPressed, top, 0.0f,
                          StringRef{}});
      }
    }
  } else if (prevPressed_ && ptr.pressed) {
    // The drag frame: the press owner (a slider) keeps tracking.
    if (nodes_.isValid(pressOwner_) &&
        nodes_.at(pressOwner_).kind == UiWidgetKind::Slider) {
      dragSlider(pressOwner_, ptr.x);
    }
  }
  prevTop_ = top;
  prevPressed_ = ptr.pressed;
  pointerHit_ = top;
  return laige::Status{};
}

inline laige::Status UiTree::focusWidget(UiWidgetId id) noexcept {
  if (!valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  const UiNode* n = nodes_.get(id);
  if (n == nullptr || !uiWidgetIsFocusable(n->kind)) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  focus_ = id;
  return laige::Status{};
}

inline void UiTree::focusNext() noexcept {
  if (!valid()) return;
  // Pass 1: the first focusable AFTER the current focus (the
  // traversal order — the deterministic tab order, the header's
  // focus section).
  bool passed = false;
  UiWidgetId next = kInvalidUiWidgetId;
  detail::uiTreePreorder(nodes_, rootId_, [&](UiWidgetId id,
                                              const UiNode* n) {
    if (!uiWidgetIsFocusable(n->kind)) return;
    if (passed) {
      if (next == kInvalidUiWidgetId) next = id;
    } else if (id == focus_) {
      passed = true;
    }
  });
  if (next != kInvalidUiWidgetId) {
    focus_ = next;
    return;
  }
  // The wrap (or the unfocused start): the FIRST focusable in the
  // traversal order (a no-op when there are none).
  detail::uiTreePreorder(nodes_, rootId_, [&](UiWidgetId id,
                                              const UiNode* n) {
    if (uiWidgetIsFocusable(n->kind) && next == kInvalidUiWidgetId) {
      next = id;
    }
  });
  focus_ = next;
}

inline void UiTree::focusPrev() noexcept {
  if (!valid()) return;
  // The previous focusable in the traversal order (wrapping to the
  // last; from unfocused, the last — the header's focus section).
  UiWidgetId lastBefore = kInvalidUiWidgetId;
  UiWidgetId lastOverall = kInvalidUiWidgetId;
  bool found = false;
  detail::uiTreePreorder(nodes_, rootId_, [&](UiWidgetId id,
                                              const UiNode* n) {
    if (!uiWidgetIsFocusable(n->kind)) return;
    lastOverall = id;
    if (id == focus_) {
      found = true;
    } else if (!found) {
      lastBefore = id;
    }
  });
  focus_ = (found && lastBefore != kInvalidUiWidgetId) ? lastBefore
                                                       : lastOverall;
}

inline laige::Status
UiTree::setPlacement(UiWidgetId id, UiAnchor anchor, std::int32_t x,
                     std::int32_t y, std::int32_t width,
                     std::int32_t height) noexcept {
  if (!valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  if (width < 0 || width > static_cast<std::int32_t>(kUiMaxDimPx) ||
      height < 0 || height > static_cast<std::int32_t>(kUiMaxDimPx) ||
      x < -static_cast<std::int32_t>(kUiMaxDimPx) ||
      x > static_cast<std::int32_t>(kUiMaxDimPx) ||
      y < -static_cast<std::int32_t>(kUiMaxDimPx) ||
      y > static_cast<std::int32_t>(kUiMaxDimPx)) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  UiNode* n = nodes_.get(id);
  if (n == nullptr) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  n->anchor = anchor;
  n->x = x;
  n->y = y;
  n->width = width;
  n->height = height;
  // The rect is zero until the next layout() (the header's layout
  // domain section).
  n->rectW = 0;
  n->rectH = 0;
  return laige::Status{};
}

inline laige::Status UiTree::setSliderValue(UiWidgetId id, float value) noexcept {
  if (!valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  UiNode* n = nodes_.get(id);
  if (n == nullptr || n->kind != UiWidgetKind::Slider) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  n->sliderValue = detail::uiClampSliderValue(n->sliderMin, n->sliderMax,
                                              value);
  return laige::Status{};
}

inline laige::Status UiTree::setScroll(UiWidgetId id,
                                      std::int32_t y) noexcept {
  if (!valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  if (y < 0 || y > static_cast<std::int32_t>(kUiMaxDimPx)) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  UiNode* n = nodes_.get(id);
  if (n == nullptr || n->kind != UiWidgetKind::List) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  n->scroll = y;  // layout() normalizes into the reachable range
  return laige::Status{};
}

inline laige::Status UiTree::setInputText(UiWidgetId id,
                                         StringRef text) noexcept {
  if (!valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  UiNode* n = nodes_.get(id);
  if (n == nullptr || n->kind != UiWidgetKind::Input) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  if (n->text.id == text.id) return laige::Status{};  // no-op: no event
  n->text = text;
  emitEvent(UiEvent{UiEventKind::InputText, id, 0.0f, text});
  return laige::Status{};
}

}  // namespace laige::render
