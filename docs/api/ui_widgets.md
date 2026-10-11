# The retained UI widget tree (`laige::render` ui_widgets)

The retained-widget-tree half of the UI work (M2-UI-01): a
pool-backed tree of the ADR 0006 widget set — exactly **panel,
button, text, image, list, slider, input**, in that canonical order —
with stable generation-checked widget ids, a per-frame layout pass,
a focus system, and a per-frame bounded list of pointer events. This
step is the **retained data only**: no rendering (the draw pass lands
with M2-UI-02), no simulation coupling (ARCH-009: the tree is
presentation state, owned by the game's UI update, and never mutates
authoritative state).

The widget set, the focusable kinds (button, slider, input), the
container kinds (panel, list), and the per-kind events are the ADR
0006 decisions ([docs/decisions/0006-ui-scope.md](../decisions/0006-ui-scope.md));
this document is the shipped contract for the tree's storage, layout,
frame protocol, and events.

Public header:
`src/laige-render/include/laige/render/ui_widgets.h` (header-only — no
new `.cpp`, no GL calls, no new dependency). Unit suite:
`ctest -R ui_tree` (`tests/laige-render/ui_tree_tests.cpp`): the create
validation + stopped state + move semantics, the tree CRUD (the seven
kinds, the child-list structure, the domain validation, the widget
budget, the subtree removal + LIFO slot recycling), the layout
goldens (the nine-anchor matrix, offsets, padding, list stacking +
scroll, saturation, zero-rect-before-layout), the focus system (the
deterministic tab order, the pointer-press focus, the removal clears),
the scripted pointer sequences (enter/leave/press/release, the button
click, the slider drag, the input text, the event overflow), and the
1 000-frame zero-allocation loop.

## The API

```cpp
// One frame's raw pointer state (the FR-4.4 hook input).
struct UiPointerState {
  float x{0.0f};      // screen px, y down
  float y{0.0f};
  bool pressed{false}; // the primary button
};

// The per-frame events (the ADR 0006 event column).
enum class UiEventKind : std::uint8_t {
  PointerEnter = 0, PointerLeave = 1,
  PointerPress = 2, PointerRelease = 3,
  ButtonPressed = 4,   // a press + release on the same Button
  SliderChanged = 5,   // sliderValue carries the new value
  InputText = 6,       // text carries the new string handle
};
struct UiEvent {
  UiEventKind kind{};
  UiWidgetId widget{};  // kInvalidUiWidgetId = no widget
  float sliderValue{0.0f};
  StringRef text{};
};

class UiTree {  // move-only
 public:
  struct Options {
    std::uint32_t maxWidgets{1024};  // INCLUDING the root
    std::uint32_t maxEvents{64};
    std::int32_t screenWidth{1280};
    std::int32_t screenHeight{720};
  };
  [[nodiscard]] static laige::Result<UiTree, laige::ErrorCode>
  create(Options options) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] UiWidgetId root() const noexcept;
  [[nodiscard]] const UiNode* node(UiWidgetId id) noexcept; // null-safe
  [[nodiscard]] bool isValid(UiWidgetId id) const noexcept;
  [[nodiscard]] std::uint32_t widgetCount() const noexcept;
  [[nodiscard]] laige::PoolStats widgetStats() const noexcept;

  [[nodiscard]] laige::Result<UiWidgetId, laige::ErrorCode>
  addChild(const UiWidgetSpec& spec) noexcept;
  [[nodiscard]] laige::Status remove(UiWidgetId id) noexcept;

  [[nodiscard]] laige::Status layout() noexcept;
  void beginFrame() noexcept;
  [[nodiscard]] laige::Status handlePointer(const UiPointerState& ptr) noexcept;
  [[nodiscard]] UiWidgetId pointerHit() const noexcept;
  [[nodiscard]] std::span<const UiEvent> events() const noexcept;
  [[nodiscard]] std::uint64_t droppedEventsTotal() const noexcept;

  [[nodiscard]] UiWidgetId focus() const noexcept;
  [[nodiscard]] laige::Status focusWidget(UiWidgetId id) noexcept;
  void focusNext() noexcept;
  void focusPrev() noexcept;

  [[nodiscard]] laige::Status setPlacement(UiWidgetId id, UiAnchor anchor,
                                           std::int32_t x, std::int32_t y,
                                           std::int32_t width, std::int32_t height)
      noexcept;
  [[nodiscard]] laige::Status setSliderValue(UiWidgetId id, float value)
      noexcept;
  [[nodiscard]] laige::Status setScroll(UiWidgetId id, std::int32_t scroll)
      noexcept;
  [[nodiscard]] laige::Status setInputText(UiWidgetId id, StringRef text)
      noexcept;
};
```

`UiWidgetId` is the pool handle (`laige::PoolHandle`: slot index +
generation). `kInvalidUiWidgetId` is `{0, 0}` — never a live id.
`UiWidgetSpec` carries the kind, the parent (the invalid id = the
root), the layout spec (anchor, x, y, width, height, padding), and the
kind-specific payload (`opaque` for panel; `label`, `image`,
`atlasId`, `tint` for button/image; `item` for text — the M2-TEXT-02
`TextItem`; `scroll` for list; `sliderMin/Max/Value` for slider;
`text` for input).

## The layout model

`layout()` computes every node's rect in one iterative preorder pass
(the parent always before its children — no recursion, a 65k-deep
chain is safe), exact integer math:

- The **root** is the layout-only screen container: rect
  `(0, 0, screenWidth, screenHeight)`. It never receives pointer
  events, is never focusable, and is never removable.
- A **panel/leaf child** of a container C is placed by its anchor
  point in C's content box (C's rect inset by its padding, clamped to
  a 0 x 0 box when the padding is wider than the rect), plus its
  offset, minus the anchor offset in its own size:
  `rect = (anchorPoint + offset) − anchorOffsetInOwnSize`.
  Concretely, for content `(cx, cy, cw, ch)` and a child of size
  `(w, h)` at `(x, y)`: the anchor point is `cx + {0, cw/2, cw} ×
  {0, ch/2, ch}` for the column × row, and the anchor offset is
  `{0, w/2, w} × {0, h/2, h}` — e.g. `Center` places the child's
  CENTER at the content center; `TopRight` places its top-right
  corner at the content's top-right corner.
- A **list** stacks its children top-to-bottom in insertion order at
  the content's left edge, IGNORING each child's anchor and offsets.
  The list's `scroll` (px, y down) shifts the stack up; `layout()`
  normalizes it into `[0, max(0, totalContentHeight − contentHeight)]`
  and writes the normalized value back (a list whose content is
  shorter than its rect never scrolls).
- Computed coordinates are saturated into `±(2^21 − 1)`
  (`kUiMaxCoordPx`): a node pushed that far offscreen renders nothing
  (the M2-UI-02 cull), and the saturation keeps the int32 storage
  exact.
- A node's rect is **zero until the next `layout()`** — a fresh
  `addChild` and every `setPlacement` zero the node's rect. Zero-rect
  nodes contain no pointer point.
- The layout is deterministic (RENDER-003): the same tree + specs give
  the same rects on every platform (integer math only).

The spec domains (validated at the boundary, first failure wins,
`InvalidArgument`, no log): width/height/padding/scroll in
`[0, 2^20]`; x/y in `±2^20`; the screen dimensions in `[1, 2^20]`;
slider `min < max` (a NaN is rejected) with the initial value clamped
into `[min, max]` (a NaN snaps to min).

## The frame protocol and pointer events

The game calls, once per frame, on its owner thread:

1. `layout()` — recompute the rects (O(n)).
2. `handlePointer(ptr)` — process the frame's raw pointer state
   (O(n), zero allocation).
   ...or `beginFrame()` for a pointerless (keyboard-only) frame, which
   only resets the event list.
3. Read `events()` (a span valid until the next frame boundary) and
   `pointerHit()`.

`handlePointer` resets the frame's event list, hit-tests the
**topmost** node — the LAST node in traversal order whose rect
contains the pointer point, half-open containment (`left ≤ x <
right`, `top ≤ y < bottom`; the root excluded), then delivers, in
order:

- **Leave-then-enter**: when the topmost hit changes, `PointerLeave`
  for the previous top (if any) then `PointerEnter` for the new one.
- **Press** (released → pressed): `PointerPress` on the top; a
  focusable top takes the focus; a slider top starts the drag (the
  value is set from the pointer x and `SliderChanged` is emitted when
  it changes). The press owner is recorded even when the top is empty.
- **Release** (pressed → released): `PointerRelease` on the top;
  `ButtonPressed` follows it **iff the press started on that same
  Button** (a press that moved off the button emits no `ButtonPressed`
  — a drag off the button is not a click).
- **Drag** (pressed → pressed): the press owner — if a slider — keeps
  tracking the pointer x (the value clamps at the rect's edges).

The slider value comes from the pointer x over the slider's width:
`value = min + t · (max − min)` with `t = (x − rectX) / width` clamped
to `[0, 1]` (a zero-width slider takes no input — no division).
`setSliderValue` is the programmatic path: it clamps (NaN → min) and
emits **no** `SliderChanged` (only the pointer drag does).

**Overflow policy**: a frame keeps its first `Options::maxEvents`
events; a later event of the frame is dropped, counted in
`droppedEventsTotal()`, and logged once per dropped event as a
rate-limited Warn `ui_tree/event_overflow` (the facade windows the
repeats, LOG-004). The default 64 is 9x the ~7 events a frame emits
in the documented model.

## The focus system

The focusable kinds are **button, slider, input** (ADR 0006). The
traversal order is the deterministic preorder (parent before
children, children in insertion order). `focusNext()` /
`focusPrev()` step through the focusable widgets in that order with
wraparound; from an unfocused state, `focusNext` takes the first and
`focusPrev` the last. `focusWidget(id)` sets the focus explicitly
(tab key / editor selection). A pointer press on a focusable widget
takes the focus; a press on a non-focusable widget leaves it.
Removing the focused widget — or any ancestor of it — clears the
focus. The focus state is read by `focus()`; this step delivers no
keyboard events (the action routing lands with M3-INPUT-04).

## Storage and ownership

The tree owns two allocations made at `create` (the set-up path): the
fixed-capacity widget pool (`laige::Pool<UiNode>`) and the event
array. The widget budget is `maxWidgets` INCLUDING the root node
(user widgets: `maxWidgets − 1`). Widget ids are stable generation-
checked handles (CPP-007): `remove` destroys the subtree in
post-order and the pool's LIFO recycling reuses the slots with a
generation bump, so every stale id is detected by
`isValid`/`node` (a stale id reads as `nullptr`, never UB). `remove`
does not free memory (the pool is fixed at create). The tree is
move-only; the moved-from tree is stopped (`valid()` false, every
id-taking operation fails `InvalidArgument`, the reads are empty).
All operations run on the owner thread (CONC-001); there is no
concurrent access contract because there is no shared state.

## Performance

- **Allocation**: zero on every per-frame path (`layout`,
  `handlePointer`, `beginFrame`, the focus/tab operations, the
  setters, `events`, `pointerHit`) — proven by the 1 000-frame
  zero-allocation loop in `ctest -R ui_tree` (PERF-003). The two
  allocations happen once, at `create` (PERF-008: the pool never
  grows; a budget overrun fails `BudgetExhausted` instead of
  allocating).
- **Complexity**: `layout` and `handlePointer` are O(n) over the live
  widgets (single iterative passes; the hit test is a full preorder
  scan — fine at the declared scene UI budget, below); `addChild` is
  O(1) typical (O(parent's child tail) worst case — the insertion
  append); `remove` is O(subtree size); `focusNext`/`focusPrev` are
  O(n) (one or two passes); the reads are O(1).
- **Memory**: one `UiNode` per widget (~240 B — the kind, the three
  structural links, the layout spec, the computed rect, and the
  kind-specific payload) in one contiguous pool block, plus the event
  array (`maxEvents × 16 B`). The 65 536-widget cap is ~15 MB
  (S-6).
- **Hot-path traps**: the rects are only valid after a `layout` of
  the current frame — calling `handlePointer` before `layout` (or
  after a `setPlacement` without a relayout) hit-tests zero rects
  (total, but not what you want). Every `addChild`/`setPlacement`
  zeros one node's rect — relayout that frame. The event span from
  `events()` is invalidated by the next `handlePointer`/`beginFrame`
  — consume it in the same frame. Dropping events is observable:
  `droppedEventsTotal()` and the `ui_tree/event_overflow` warn.

## Misuse warning

Do not store `UiNode*` pointers across frames — the pool slots are
recycled (the LIFO order means a removed node's slot can be reused by
the next `addChild` of another subtree). Store `UiWidgetId` handles
and re-read through `node()` / `isValid()`. Do not read kind-specific
fields without checking the kind first (a button's `sliderValue` is
uninitialized domain — the spec's default). The tree is presentation
state: never drive simulation from it (ARCH-009), and keep its owner
thread the game's UI update thread.
