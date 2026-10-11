// laige-render UI widget tree tests (M2-UI-01): the step's Verify
// command is `ctest -R ui_tree`.
//
// The contract pinned against: the create validation + the stopped
// state + the move semantics (UiTreeCreate), the tree CRUD — the
// seven ADR 0006 kinds, the child-list structure, the kind / parent /
// geometry / slider domain validation, the widget budget failure +
// one rate-limited warn, the subtree removal (the stale ids, the LIFO
// slot recycling, the generation bump), the root's non-removability
// (UiTreeCrud), the layout goldens — the nine-anchor matrix against
// hand-computed rects, the offsets, the container padding, the list
// stacking + scroll normalization, the negative rects, the
// coordinate saturation, the zero-rect-before-layout, the layout
// idempotence (UiTreeLayout), the focus system — the focusable kinds
// only, the deterministic tab traversal order (nested tree, the
// hand-computed focusable sequence), the wrap, the pointer-press
// focus, the removal clears the focus (UiTreeFocus), the scripted
// pointer sequences — the enter/leave/press/release transitions, the
// button click (press + release on the same button; the off-button
// click), the slider drag (the exact value goldens), the input text
// event, the panel press (no focus change), the zero-size widgets
// contain nothing, the event overflow (the drop count + the warn)
// (UiTreeEvents), and the 1 000-frame zero-allocation loop
// (PERF-003, UiTreeZeroAlloc).
//
// No GL anywhere: the step is the retained tree only (the rendering
// lands with M2-UI-02). No font asset, no environment variables, no
// budget gate: the step's roadmap scope has no standalone budgets.json
// entry (the per-frame UI cost is part of the composite 50k render-CPU
// budget, M2-PERF-01).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/render/ui_widgets.h"
#include "laige/result.h"

namespace {

using laige::ErrorCode;
using laige::Result;
using laige::Status;
using laige::render::UiAnchor;
using laige::render::UiEvent;
using laige::render::UiEventKind;
using laige::render::UiNode;
using laige::render::UiPointerState;
using laige::render::UiTree;
using laige::render::UiWidgetId;
using laige::render::UiWidgetKind;
using laige::render::UiWidgetSpec;
using laige::render::kInvalidUiWidgetId;
using laige::render::kUiWidgetKindCount;

constexpr std::uint32_t kDefaultMaxWidgets = 128;
constexpr std::uint32_t kDefaultMaxEvents = 64;

UiTree makeTree(std::uint32_t maxWidgets, std::uint32_t maxEvents,
                std::int32_t w, std::int32_t h) {
  auto r = UiTree::create(UiTree::Options{maxWidgets, maxEvents, w, h});
  if (!r.ok()) {
    ADD_FAILURE() << "UiTree::create failed";
    std::abort();
  }
  return std::move(r).takeValue();
}

UiTree makeDefaultTree() {
  return makeTree(kDefaultMaxWidgets, kDefaultMaxEvents, 400, 300);
}

UiWidgetId addWidget(UiTree& t, UiWidgetKind kind, UiAnchor anchor,
                     std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h, UiWidgetId parent = kInvalidUiWidgetId,
                     std::int32_t padding = 0, bool opaque = false) {
  UiWidgetSpec s;
  s.kind = kind;
  s.parent = parent;
  s.anchor = anchor;
  s.x = x;
  s.y = y;
  s.width = w;
  s.height = h;
  s.padding = padding;
  s.opaque = opaque;
  auto r = t.addChild(s);
  if (!r.ok()) {
    ADD_FAILURE() << "addChild failed";
    std::abort();
  }
  return std::move(r).takeValue();
}

UiWidgetId addSlider(UiTree& t, std::int32_t x, std::int32_t y,
                     std::int32_t w, std::int32_t h, float min, float max,
                     float value) {
  UiWidgetSpec s;
  s.kind = UiWidgetKind::Slider;
  s.x = x;
  s.y = y;
  s.width = w;
  s.height = h;
  s.sliderMin = min;
  s.sliderMax = max;
  s.sliderValue = value;
  auto r = t.addChild(s);
  if (!r.ok()) {
    ADD_FAILURE() << "addSlider failed";
    std::abort();
  }
  return std::move(r).takeValue();
}

void expectOk(const Status& s, const char* what) {
  if (s.isError()) {
    ADD_FAILURE() << what << " failed";
  }
}

void frame(UiTree& t, float x, float y, bool pressed) {
  UiPointerState ptr;
  ptr.x = x;
  ptr.y = y;
  ptr.pressed = pressed;
  expectOk(t.handlePointer(ptr), "handlePointer");
}

void layoutTree(UiTree& t) {
  expectOk(t.layout(), "layout");
}

// One expected event (the kind-specific payload defaults to zero).
struct EventExpect {
  UiEventKind kind{};
  UiWidgetId widget{kInvalidUiWidgetId};
  float sliderValue{0.0f};
  std::uint32_t textId{0};
  bool expectText{false};
};

void expectEvents(const UiTree& t, std::vector<EventExpect> want,
                  std::uint64_t dropped = 0) {
  const std::span<const UiEvent> ev = t.events();
  EXPECT_EQ(want.size(), ev.size());
  std::size_t i = 0;
  for (const auto& e : ev) {
    if (i >= want.size()) break;
    const EventExpect& w = want[i];
    EXPECT_EQ(w.kind, e.kind) << "event " << i << ": wrong kind";
    EXPECT_EQ(w.widget, e.widget) << "event " << i << ": wrong widget";
    if (w.kind == UiEventKind::SliderChanged) {
      EXPECT_FLOAT_EQ(w.sliderValue, e.sliderValue)
          << "event " << i << ": wrong slider value";
    }
    if (w.expectText) {
      EXPECT_EQ(w.textId, e.text.id) << "event " << i << ": wrong text id";
    }
    ++i;
  }
  if (dropped != 0) {
    EXPECT_EQ(dropped, t.droppedEventsTotal());
  }
}

// Log capture (the sprite_batcher_tests MemorySink pattern — rate
// limiting OFF so the tests assert per-event counts, not the facade's
// LOG-004 window).
class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
  };

  void emit(const laige::log::LogRecord& record) override {
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    for (const auto& f : record.fields) {
      e.fields.emplace_back(std::string(f.name), f.value);
    }
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    std::abort();
  }
  return ptr;
}

std::size_t countEvents(const MemorySink& sink, std::string_view subsystem,
                        std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

const MemorySink::Entry* firstEvent(const MemorySink& sink,
                                    std::string_view event) {
  for (const auto& e : sink.entries) {
    if (e.event == event) return &e;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// UiTreeCreate — the create validation, the stopped state, the move
// ---------------------------------------------------------------------------

TEST(UiTreeCreate, ValidationMatrix) {
  // maxWidgets: below 2 (the root needs a slot), above the cap.
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{0, 64, 400, 300}).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{1, 64, 400, 300}).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{65537, 64, 400, 300}).error());
  // maxEvents: 0 (the list needs a slot), above the cap.
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{1024, 0, 400, 300}).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{1024, 4097, 400, 300}).error());
  // The screen dimensions: 0, above the domain.
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{1024, 64, 0, 300}).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{1024, 64, 400, 0}).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{1024, 64, (1 << 20) + 1, 300})
                .error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            UiTree::create(UiTree::Options{1024, 64, 400, (1 << 20) + 1})
                .error());
  // The domain boundaries themselves are valid.
  EXPECT_TRUE(UiTree::create(UiTree::Options{2, 1, 1, 1}).ok());
  EXPECT_TRUE(UiTree::create(UiTree::Options{65536, 4096, 1 << 20, 1 << 20})
                  .ok());
}

TEST(UiTreeCreate, SuccessIntrospection) {
  UiTree t = makeTree(16, 64, 640, 480);
  EXPECT_TRUE(t.valid());
  const UiWidgetId root = t.root();
  EXPECT_TRUE(t.isValid(root));
  const UiNode* r = t.node(root);
  ASSERT_NE(nullptr, r);
  EXPECT_EQ(UiWidgetKind::Panel, r->kind);
  EXPECT_EQ(0, r->rectX);
  EXPECT_EQ(0, r->rectY);
  EXPECT_EQ(640, r->rectW);
  EXPECT_EQ(480, r->rectH);
  EXPECT_EQ(0, r->padding);
  EXPECT_FALSE(r->opaque);
  EXPECT_EQ(1u, t.widgetCount());
  const laige::PoolStats s = t.widgetStats();
  EXPECT_EQ(16u, s.capacity);
  EXPECT_EQ(1u, s.inUse);
  EXPECT_EQ(1u, s.peakInUse);
  EXPECT_EQ(1ull, s.totalCreated);
}

TEST(UiTreeCreate, StoppedState) {
  UiTree t;
  EXPECT_FALSE(t.valid());
  UiPointerState ptr;
  ptr.x = 10.0f;
  ptr.y = 10.0f;
  EXPECT_EQ(ErrorCode::InvalidArgument, t.handlePointer(ptr).error());
  UiWidgetSpec spec;
  spec.kind = UiWidgetKind::Button;
  EXPECT_EQ(ErrorCode::InvalidArgument, t.addChild(spec).error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.remove(kInvalidUiWidgetId).error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.layout().error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.focusWidget(kInvalidUiWidgetId).error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.setInputText(kInvalidUiWidgetId,
                                                        laige::render::StringRef{1}).error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.setSliderValue(kInvalidUiWidgetId,
                                                          1.0f).error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.setScroll(kInvalidUiWidgetId, 1).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setPlacement(kInvalidUiWidgetId, UiAnchor::TopLeft, 0, 0, 1, 1).error());
  EXPECT_EQ(nullptr, t.node(kInvalidUiWidgetId));
  EXPECT_FALSE(t.isValid(kInvalidUiWidgetId));
  EXPECT_EQ(0u, t.widgetCount());
  EXPECT_EQ(0ull, t.droppedEventsTotal());
  EXPECT_TRUE(t.events().empty());
  EXPECT_EQ(kInvalidUiWidgetId, t.focus());
  EXPECT_EQ(kInvalidUiWidgetId, t.pointerHit());
  t.beginFrame();  // total: no crash, no state change
  t.focusNext();
  t.focusPrev();
}

TEST(UiTreeCreate, MoveSemantics) {
  UiTree t = makeDefaultTree();
  const UiWidgetId root = t.root();
  UiTree t2 = std::move(t);
  EXPECT_TRUE(t2.valid());
  EXPECT_EQ(root, t2.root());
  EXPECT_TRUE(t2.isValid(root));
  EXPECT_FALSE(t.valid());
  EXPECT_EQ(0u, t.widgetCount());
  // The root's rect is intact on the target.
  const UiNode* r = t2.node(root);
  ASSERT_NE(nullptr, r);
  EXPECT_EQ(400, r->rectW);
  EXPECT_EQ(300, r->rectH);
}

// ---------------------------------------------------------------------------
// UiTreeCrud — the seven kinds, the structure, the validation, the
// budget, the subtree removal, the LIFO recycling
// ---------------------------------------------------------------------------

TEST(UiTreeCrud, AllSevenKindsUnderRoot) {
  UiTree t = makeDefaultTree();
  const UiWidgetId root = t.root();
  const UiWidgetId p = addWidget(t, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                 0, 0, 100, 100);
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 50, 20);
  const UiWidgetId tx = addWidget(t, UiWidgetKind::Text, UiAnchor::TopLeft,
                                  0, 0, 80, 16);
  const UiWidgetId im = addWidget(t, UiWidgetKind::Image, UiAnchor::TopLeft,
                                  0, 0, 32, 32);
  const UiWidgetId li = addWidget(t, UiWidgetKind::List, UiAnchor::TopLeft,
                                  0, 0, 60, 120);
  const UiWidgetId sl = addSlider(t, 0, 0, 100, 20, 0.0f, 10.0f, 3.0f);
  const UiWidgetId in = addWidget(t, UiWidgetKind::Input, UiAnchor::TopLeft,
                                  0, 0, 120, 24);
  // The child list: insertion order, the root's first child chain.
  const UiNode* rp = t.node(root);
  ASSERT_NE(nullptr, rp);
  EXPECT_EQ(p, rp->firstChild);
  EXPECT_EQ(b, t.node(p)->nextSibling);
  EXPECT_EQ(tx, t.node(b)->nextSibling);
  EXPECT_EQ(im, t.node(tx)->nextSibling);
  EXPECT_EQ(li, t.node(im)->nextSibling);
  EXPECT_EQ(sl, t.node(li)->nextSibling);
  EXPECT_EQ(in, t.node(sl)->nextSibling);
  EXPECT_EQ(kInvalidUiWidgetId, t.node(in)->nextSibling);
  // Every child's parent is the root; the kinds + the slider clamp.
  for (const UiWidgetId id :
       {p, b, tx, im, li, sl, in}) {
    EXPECT_EQ(root, t.node(id)->parent);
  }
  EXPECT_EQ(8u, t.widgetCount());  // the root + the seven
  EXPECT_FLOAT_EQ(3.0f, t.node(sl)->sliderValue);
  // The slider value clamps at creation (value 100 -> max 10; NaN -> min).
  UiWidgetSpec over;
  over.kind = UiWidgetKind::Slider;
  over.width = 10;
  over.height = 10;
  over.sliderMin = 0.0f;
  over.sliderMax = 10.0f;
  over.sliderValue = 100.0f;
  auto r = t.addChild(over);
  ASSERT_TRUE(r.ok());
  EXPECT_FLOAT_EQ(10.0f, t.node(std::move(r).takeValue())->sliderValue);
  UiWidgetSpec nan;
  nan.kind = UiWidgetKind::Slider;
  nan.width = 10;
  nan.height = 10;
  nan.sliderMin = 0.0f;
  nan.sliderMax = 10.0f;
  nan.sliderValue = std::nanf("");
  r = t.addChild(nan);
  ASSERT_TRUE(r.ok());
  EXPECT_FLOAT_EQ(0.0f, t.node(std::move(r).takeValue())->sliderValue);
}

TEST(UiTreeCrud, KindDomainAndParents) {
  UiTree t = makeDefaultTree();
  // A kind outside the ADR 0006 seven (a garbage value too).
  UiWidgetSpec bad;
  bad.kind = static_cast<UiWidgetKind>(kUiWidgetKindCount);
  EXPECT_EQ(ErrorCode::InvalidArgument, t.addChild(bad).error());
  bad.kind = static_cast<UiWidgetKind>(0xFF);
  EXPECT_EQ(ErrorCode::InvalidArgument, t.addChild(bad).error());
  // A leaf parent: only Panel / List are containers.
  const UiWidgetId btn = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                   0, 0, 10, 10);
  const UiWidgetId txt = addWidget(t, UiWidgetKind::Text, UiAnchor::TopLeft,
                                   0, 0, 10, 10);
  UiWidgetSpec child;
  child.kind = UiWidgetKind::Button;
  child.parent = btn;
  child.width = 5;
  child.height = 5;
  EXPECT_EQ(ErrorCode::InvalidArgument, t.addChild(child).error());
  child.parent = txt;
  EXPECT_EQ(ErrorCode::InvalidArgument, t.addChild(child).error());
  // A stale parent (removed).
  child.parent = kInvalidUiWidgetId;
  auto r = t.addChild(child);
  ASSERT_TRUE(r.ok());
  const UiWidgetId c = std::move(r).takeValue();
  expectOk(t.remove(c), "remove(c)");
  child.parent = c;  // stale now
  EXPECT_EQ(ErrorCode::InvalidArgument, t.addChild(child).error());
  // The root's non-removability + the stale remove.
  EXPECT_EQ(ErrorCode::InvalidArgument, t.remove(t.root()).error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.remove(kInvalidUiWidgetId).error());
}

TEST(UiTreeCrud, GeometryDomain) {
  UiTree t = makeDefaultTree();
  auto failDomain = [&t](std::int32_t x, std::int32_t y, std::int32_t w,
                         std::int32_t h, std::int32_t pad) {
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Button;
    s.x = x;
    s.y = y;
    s.width = w;
    s.height = h;
    s.padding = pad;
    return t.addChild(s).error();
  };
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(0, 0, -1, 10, 0));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(0, 0, 10, -1, 0));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(0, 0, (1 << 20) + 1, 10, 0));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(0, 0, 10, (1 << 20) + 1, 0));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(-((1 << 20) + 1), 0, 10, 10, 0));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(0, -((1 << 20) + 1), 10, 10, 0));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain((1 << 20) + 1, 0, 10, 10, 0));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(0, 0, 10, 10, -1));
  EXPECT_EQ(ErrorCode::InvalidArgument, failDomain(0, 0, 10, 10, (1 << 20) + 1));
  // The domain boundaries are valid.
  UiWidgetSpec ok;
  ok.kind = UiWidgetKind::Button;
  ok.x = -(1 << 20);
  ok.width = 1 << 20;
  ok.height = 1 << 20;
  ok.padding = 1 << 20;
  EXPECT_TRUE(t.addChild(ok).ok());
}

TEST(UiTreeCrud, SliderDomain) {
  UiTree t = makeDefaultTree();
  auto failSlider = [&t](float min, float max) -> laige::ErrorCode {
    UiWidgetSpec s;
    s.kind = UiWidgetKind::Slider;
    s.width = 10;
    s.height = 10;
    s.sliderMin = min;
    s.sliderMax = max;
    const auto r = t.addChild(s);
    if (r.ok()) {
      ADD_FAILURE() << "expected an InvalidArgument failure";
      return ErrorCode::Unknown;
    }
    return r.error();
  };
  EXPECT_EQ(ErrorCode::InvalidArgument, failSlider(5.0f, 5.0f));  // min == max
  EXPECT_EQ(ErrorCode::InvalidArgument, failSlider(5.0f, 4.0f));  // min > max
  EXPECT_EQ(ErrorCode::InvalidArgument,
            failSlider(std::nanf(""), 5.0f));  // NaN min
  EXPECT_EQ(ErrorCode::InvalidArgument,
            failSlider(5.0f, std::nanf("")));  // NaN max
  // A valid domain succeeds.
  UiWidgetSpec ok;
  ok.kind = UiWidgetKind::Slider;
  ok.width = 10;
  ok.height = 10;
  ok.sliderMin = 0.0f;
  ok.sliderMax = 1.0f;
  ok.sliderValue = 0.25f;
  EXPECT_TRUE(t.addChild(ok).ok());
}

// ---------------------------------------------------------------------------
// UiTreeLayout — the anchor goldens, the offsets, the padding, the list
// stacking + scroll, the saturation, the zero rect, the idempotence
// ---------------------------------------------------------------------------

TEST(UiTreeLayout, NineAnchorGoldens) {
  // Parent panel: rect (100, 50) x 200 x 100, no padding. Child 20 x 10.
  UiTree t = makeDefaultTree();
  const UiWidgetId p = addWidget(t, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                 100, 50, 200, 100);
  struct Case {
    UiAnchor anchor;
    std::int32_t x;
    std::int32_t y;
  };
  const Case cases[] = {
      {UiAnchor::TopLeft, 0, 0},
      {UiAnchor::TopCenter, 0, 0},
      {UiAnchor::TopRight, 0, 0},
      {UiAnchor::CenterLeft, 0, 0},
      {UiAnchor::Center, 0, 0},
      {UiAnchor::CenterRight, 0, 0},
      {UiAnchor::BottomLeft, 0, 0},
      {UiAnchor::BottomCenter, 0, 0},
      {UiAnchor::BottomRight, 0, 0},
  };
  // One child per anchor, in this order (the hand-computed goldens:
  // the anchor point + the offset, minus the node's anchor point).
  const std::int32_t goldens[][2] = {
      {100, 50},   {190, 50},   {280, 50},
      {100, 95},   {190, 95},   {280, 95},
      {100, 140},  {190, 140},  {280, 140},
  };
  for (std::size_t i = 0; i < 9; ++i) {
    const UiWidgetId c = addWidget(t, UiWidgetKind::Button, cases[i].anchor,
                                    cases[i].x, cases[i].y, 20, 10, p);
    layoutTree(t);
    const UiNode* n = t.node(c);
    EXPECT_EQ(goldens[i][0], n->rectX) << "anchor " << i;
    EXPECT_EQ(goldens[i][1], n->rectY) << "anchor " << i;
    EXPECT_EQ(20, n->rectW);
    EXPECT_EQ(10, n->rectH);
    EXPECT_EQ(p, n->parent);
  }
  // An offset shifts the node (TopLeft, x +5, y -3).
  const UiWidgetId off = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                   5, -3, 20, 10, p);
  layoutTree(t);
  EXPECT_EQ(105, t.node(off)->rectX);
  EXPECT_EQ(47, t.node(off)->rectY);
}

TEST(UiTreeLayout, ContainerPadding) {
  // Panel (100, 50) x 200 x 100 with padding 10: content (110, 60) x
  // 180 x 80.
  UiTree t = makeDefaultTree();
  const UiWidgetId p = addWidget(t, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                 100, 50, 200, 100, kInvalidUiWidgetId, 10);
  const UiWidgetId a = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 20, 10, p);
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopRight,
                                 0, 0, 20, 10, p);
  const UiWidgetId c = addWidget(t, UiWidgetKind::Button, UiAnchor::Center,
                                 0, 0, 20, 10, p);
  layoutTree(t);
  EXPECT_EQ(110, t.node(a)->rectX);
  EXPECT_EQ(60, t.node(a)->rectY);
  EXPECT_EQ(270, t.node(b)->rectX);  // 110 + 180 - 20
  EXPECT_EQ(60, t.node(b)->rectY);
  EXPECT_EQ(190, t.node(c)->rectX);  // 110 + 90 - 10
  EXPECT_EQ(95, t.node(c)->rectY);   // 60 + 40 - 5
  // A padding wider than the container: the content is 0 x 0 at its
  // inset origin (10, 10); the TopLeft child lands on that origin.
  UiTree t2 = makeDefaultTree();
  const UiWidgetId p2 = addWidget(t2, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                  0, 0, 10, 10, kInvalidUiWidgetId, 10);
  const UiWidgetId c2 = addWidget(t2, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  0, 0, 4, 4, p2);
  layoutTree(t2);
  EXPECT_EQ(10, t2.node(c2)->rectX);  // content origin (10, 10)
  EXPECT_EQ(10, t2.node(c2)->rectY);
}

TEST(UiTreeLayout, ListStackingAndScroll) {
  // List (0, 0) x 100 x 50; children 100 x 40 + 100 x 40 (total 80 >
  // 50 -> maxScroll 30).
  UiTree t = makeDefaultTree();
  const UiWidgetId li = addWidget(t, UiWidgetKind::List, UiAnchor::TopLeft,
                                  0, 0, 100, 50);
  const UiWidgetId a = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 100, 40, li);
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 100, 40, li);
  // No scroll: stacked from the content top (the child anchor /
  // offsets are ignored in a list).
  layoutTree(t);
  EXPECT_EQ(0, t.node(a)->rectX);
  EXPECT_EQ(0, t.node(a)->rectY);
  EXPECT_EQ(0, t.node(b)->rectX);
  EXPECT_EQ(40, t.node(b)->rectY);
  // A scroll within range shifts the stack up.
  expectOk(t.setScroll(li, 5), "setScroll(5)");
  layoutTree(t);
  EXPECT_EQ(-5, t.node(a)->rectY);
  EXPECT_EQ(35, t.node(b)->rectY);
  // An out-of-range scroll normalizes to maxScroll (30).
  expectOk(t.setScroll(li, 1000), "setScroll(1000)");
  layoutTree(t);
  EXPECT_EQ(30, t.node(li)->scroll);
  EXPECT_EQ(-30, t.node(a)->rectY);
  EXPECT_EQ(10, t.node(b)->rectY);
  // A list shorter than its content never scrolls (total 45 < 50 ->
  // maxScroll 0): a set scroll normalizes to 0.
  UiTree t2 = makeDefaultTree();
  const UiWidgetId li2 = addWidget(t2, UiWidgetKind::List, UiAnchor::TopLeft,
                                   0, 0, 100, 50);
  const UiWidgetId a2 = addWidget(t2, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  0, 0, 100, 10, li2);
  const UiWidgetId b2 = addWidget(t2, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  0, 0, 100, 20, li2);
  const UiWidgetId c2 = addWidget(t2, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  0, 0, 100, 15, li2);
  expectOk(t2.setScroll(li2, 7), "setScroll(7)");
  layoutTree(t2);
  EXPECT_EQ(0, t2.node(li2)->scroll);
  EXPECT_EQ(0, t2.node(a2)->rectY);
  EXPECT_EQ(10, t2.node(b2)->rectY);
  EXPECT_EQ(30, t2.node(c2)->rectY);
}

TEST(UiTreeLayout, NegativeRectsAndSaturation) {
  // A node extending outside the screen: the exact rect (the domain
  // allows it — the render culls it, M2-UI-02).
  UiTree t = makeDefaultTree();  // 400 x 300
  const UiWidgetId n = addWidget(t, UiWidgetKind::Image, UiAnchor::TopLeft,
                                 -300, -200, 20, 10);
  layoutTree(t);
  EXPECT_EQ(-300, t.node(n)->rectX);
  EXPECT_EQ(-200, t.node(n)->rectY);
  // The saturation: a 2^20 x 2^20 screen, a zero-size child anchored
  // BottomRight with x = 2^20, y = 2^20: both computed corners are
  // 2^21 (one past the 2^21 - 1 bound) -> both saturate to the bound.
  UiTree t2 = makeTree(16, 16, 1 << 20, 1 << 20);
  const UiWidgetId s = addWidget(t2, UiWidgetKind::Button, UiAnchor::BottomRight,
                                 1 << 20, 1 << 20, 0, 0);
  layoutTree(t2);
  EXPECT_EQ((1 << 21) - 1, t2.node(s)->rectX);
  EXPECT_EQ((1 << 21) - 1, t2.node(s)->rectY);
}

TEST(UiTreeLayout, ZeroRectBeforeLayoutAndIdempotence) {
  UiTree t = makeDefaultTree();
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 10, 10, 20, 10);
  // No layout yet: the rect is zero -> the node contains no pointer
  // point.
  UiPointerState ptr;
  ptr.x = 15.0f;
  ptr.y = 15.0f;
  expectOk(t.handlePointer(ptr), "handlePointer");
  EXPECT_EQ(kInvalidUiWidgetId, t.pointerHit());
  EXPECT_TRUE(t.events().empty());
  EXPECT_EQ(0, t.node(b)->rectW);
  // After layout: the rect is valid + the hit lands on the button.
  expectOk(t.layout(), "layout");
  EXPECT_EQ(10, t.node(b)->rectX);
  EXPECT_EQ(10, t.node(b)->rectY);
  EXPECT_EQ(20, t.node(b)->rectW);
  EXPECT_EQ(10, t.node(b)->rectH);
  frame(t, 15.0f, 15.0f, false);
  EXPECT_EQ(b, t.pointerHit());
  // Idempotence: a second layout leaves the rects unchanged.
  const UiNode* before = t.node(b);
  const std::int32_t bx = before->rectX;
  const std::int32_t by = before->rectY;
  expectOk(t.layout(), "layout");
  EXPECT_EQ(bx, t.node(b)->rectX);
  EXPECT_EQ(by, t.node(b)->rectY);
  // setPlacement zeros the rect until the next layout.
  expectOk(t.setPlacement(b, UiAnchor::TopLeft, 20, 20, 30, 12),
           "setPlacement");
  EXPECT_EQ(0, t.node(b)->rectW);
  frame(t, 25.0f, 26.0f, false);
  EXPECT_EQ(kInvalidUiWidgetId, t.pointerHit());
  expectOk(t.layout(), "layout");
  EXPECT_EQ(20, t.node(b)->rectX);
  EXPECT_EQ(20, t.node(b)->rectY);
  EXPECT_EQ(30, t.node(b)->rectW);
  EXPECT_EQ(12, t.node(b)->rectH);
}

// ---------------------------------------------------------------------------
// UiTreeCrud (continued) — the budget + the subtree removal + the LIFO
// recycling
// ---------------------------------------------------------------------------

TEST(UiTreeCrud, WidgetBudgetAndWarn) {
  MemorySink* sink = installCaptureSink();
  // Budget 3: the root + two user widgets; the third add fails.
  UiTree t = makeTree(3, 16, 320, 200);
  const UiWidgetId a = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 10, 10);
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 12, 0, 10, 10);
  UiWidgetSpec c;
  c.kind = UiWidgetKind::Button;
  c.width = 10;
  c.height = 10;
  auto r = t.addChild(c);
  EXPECT_EQ(ErrorCode::BudgetExhausted, r.error());
  EXPECT_EQ(3u, t.widgetCount());  // the root + the two: no growth
  const MemorySink::Entry* e = firstEvent(*sink, "widget_budget");
  ASSERT_NE(nullptr, e);
  EXPECT_EQ("ui_tree", e->subsystem);
  EXPECT_EQ(laige::log::Severity::Warn, e->severity);
  EXPECT_EQ("3", fieldOf(*e, "capacity"));
  EXPECT_EQ(1ull, countEvents(*sink, "ui_tree", "widget_budget"));
  (void)a;
  (void)b;
}

TEST(UiTreeCrud, SubtreeRemovalAndLifoRecycling) {
  UiTree t = makeDefaultTree();
  const UiWidgetId root = t.root();
  // The tree: P -> {A -> {A1}, B} (P, A, A1, B are the subtree of P).
  const UiWidgetId p = addWidget(t, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                 0, 0, 100, 100);
  const UiWidgetId a = addWidget(t, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                 0, 0, 50, 50, p);
  const UiWidgetId a1 = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  0, 0, 10, 10, a);
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 52, 0, 10, 10, p);
  EXPECT_EQ(5u, t.widgetCount());  // the root + the four
  expectOk(t.remove(p), "remove");
  // Every node of the subtree is stale (the generation bumps).
  for (const UiWidgetId id : {p, a, a1, b}) {
    EXPECT_FALSE(t.isValid(id)) << "stale id still valid";
    EXPECT_EQ(nullptr, t.node(id));
  }
  EXPECT_EQ(1u, t.widgetCount());  // the root only
  EXPECT_EQ(kInvalidUiWidgetId, t.node(root)->firstChild);
  // The LIFO recycling: the post-order destruction order is A1, A, B,
  // P -> the last freed slot (P's) is the next create's slot, with the
  // generation bumped (the stale P handle is still invalid).
  const UiWidgetId np = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  0, 0, 10, 10);
  EXPECT_TRUE(t.isValid(np));
  EXPECT_EQ(p.index, np.index);
  EXPECT_EQ(p.generation + 1, np.generation);
  EXPECT_FALSE(t.isValid(p));
  EXPECT_EQ(2u, t.widgetCount());
  // Two more direct children under the root: the chain is
  // np -> x -> y (np is the recycled slot from above).
  const UiWidgetId x = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 10, 10);
  const UiWidgetId y = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 12, 0, 10, 10);
  EXPECT_EQ(np, t.node(root)->firstChild);
  EXPECT_EQ(x, t.node(np)->nextSibling);
  EXPECT_EQ(y, t.node(x)->nextSibling);
  EXPECT_EQ(kInvalidUiWidgetId, t.node(y)->nextSibling);
  // Removing the middle sibling splices it out of the chain.
  expectOk(t.remove(x), "remove x");
  EXPECT_EQ(np, t.node(root)->firstChild);
  EXPECT_EQ(y, t.node(np)->nextSibling);
  EXPECT_EQ(kInvalidUiWidgetId, t.node(y)->nextSibling);
}

// ---------------------------------------------------------------------------
// UiTreeFocus — the focusable kinds, the deterministic tab order, the
// pointer focus, the removal clears
// ---------------------------------------------------------------------------

TEST(UiTreeFocus, FocusableKindsOnly) {
  UiTree t = makeDefaultTree();
  const UiWidgetId p = addWidget(t, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                 0, 0, 50, 50);
  const UiWidgetId txt = addWidget(t, UiWidgetKind::Text, UiAnchor::TopLeft,
                                   52, 0, 50, 20);
  const UiWidgetId im = addWidget(t, UiWidgetKind::Image, UiAnchor::TopLeft,
                                  52, 22, 10, 10);
  const UiWidgetId li = addWidget(t, UiWidgetKind::List, UiAnchor::TopLeft,
                                  0, 52, 100, 50);
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 10, 10, p);
  const UiWidgetId s = addSlider(t, 52, 22, 10, 10, 0.0f, 1.0f, 0.5f);
  const UiWidgetId in = addWidget(t, UiWidgetKind::Input, UiAnchor::TopLeft,
                                  0, 0, 50, 20, li);
  for (const UiWidgetId id : {p, txt, im, li}) {
    EXPECT_EQ(ErrorCode::InvalidArgument, t.focusWidget(id).error());
  }
  EXPECT_EQ(kInvalidUiWidgetId, t.focus());
  expectOk(t.focusWidget(b), "focusWidget(b)");
  EXPECT_EQ(b, t.focus());
  expectOk(t.focusWidget(s), "focusWidget(s)");
  EXPECT_EQ(s, t.focus());
  expectOk(t.focusWidget(in), "focusWidget(in)");
  EXPECT_EQ(in, t.focus());
  // A stale / invalid target fails.
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.focusWidget(kInvalidUiWidgetId).error());
  expectOk(t.remove(in), "remove(in)");
  EXPECT_EQ(ErrorCode::InvalidArgument, t.focusWidget(in).error());
}

TEST(UiTreeFocus, DeterministicTabOrder) {
  // The tree (400 x 300): root -> {P, T, S, I}; P -> {B1, I1}.
  // The preorder: root, P, B1, I1, T, S, I.
  // The focusable sequence: B1, I1, S, I.
  UiTree t = makeDefaultTree();
  const UiWidgetId p = addWidget(t, UiWidgetKind::Panel, UiAnchor::TopLeft,
                                 10, 10, 100, 50);
  const UiWidgetId b1 = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  0, 0, 20, 10, p);
  const UiWidgetId i1 = addWidget(t, UiWidgetKind::Input, UiAnchor::TopLeft,
                                  0, 12, 50, 20, p);
  const UiWidgetId txt = addWidget(t, UiWidgetKind::Text, UiAnchor::TopLeft,
                                   120, 10, 50, 20);
  const UiWidgetId s = addSlider(t, 120, 40, 50, 20, 0.0f, 1.0f, 0.5f);
  const UiWidgetId in = addWidget(t, UiWidgetKind::Input, UiAnchor::TopLeft,
                                  180, 10, 50, 20);
  (void)txt;
  // focusNext from unfocused: the first focusable, then in order,
  // wrapping from the last back to the first.
  EXPECT_EQ(kInvalidUiWidgetId, t.focus());
  t.focusNext();
  EXPECT_EQ(b1, t.focus());
  t.focusNext();
  EXPECT_EQ(i1, t.focus());
  t.focusNext();
  EXPECT_EQ(s, t.focus());
  t.focusNext();
  EXPECT_EQ(in, t.focus());
  t.focusNext();  // the wrap
  EXPECT_EQ(b1, t.focus());
  // focusPrev from the current (b1): wraps to the last (in), then back.
  t.focusPrev();
  EXPECT_EQ(in, t.focus());
  t.focusPrev();
  EXPECT_EQ(s, t.focus());
  t.focusPrev();
  EXPECT_EQ(i1, t.focus());
  t.focusPrev();
  EXPECT_EQ(b1, t.focus());
  t.focusPrev();  // the wrap
  EXPECT_EQ(in, t.focus());
  // focusPrev from unfocused: the last focusable (the removal of the
  // focused input clears the focus first).
  expectOk(t.focusWidget(in), "focusWidget(in)");
  expectOk(t.remove(in), "remove(in)");  // the removal clears the focus
  EXPECT_EQ(kInvalidUiWidgetId, t.focus());
  t.focusPrev();
  EXPECT_EQ(s, t.focus());
}

TEST(UiTreeFocus, PointerPressTakesFocus) {
  UiTree t = makeDefaultTree();
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 10, 10, 50, 20);
  // A non-focusable text (its handle is unused: the press targets its
  // rect by coordinate).
  addWidget(t, UiWidgetKind::Text, UiAnchor::TopLeft, 80, 10, 50, 20);
  layoutTree(t);
  // A press on the focusable button takes the focus.
  frame(t, 15.0f, 15.0f, true);
  EXPECT_EQ(b, t.focus());
  frame(t, 15.0f, 15.0f, false);
  // A press on the non-focusable text leaves the focus unchanged.
  frame(t, 85.0f, 15.0f, true);
  EXPECT_EQ(b, t.focus());
  frame(t, 85.0f, 15.0f, false);
  // A press on empty screen space leaves the focus unchanged.
  frame(t, 300.0f, 250.0f, true);
  EXPECT_EQ(b, t.focus());
  frame(t, 300.0f, 250.0f, false);
  // The removal of the focused widget clears the focus.
  expectOk(t.remove(b), "remove(b)");
  EXPECT_EQ(kInvalidUiWidgetId, t.focus());
}

// ---------------------------------------------------------------------------
// UiTreeEvents — the scripted pointer sequences
// ---------------------------------------------------------------------------

namespace {

// The shared event scene: B (10, 10) x 50 x 20, S (100, 10) x 100 x 20
// (0..10), I (200, 10) x 50 x 20, P (0, 100) x 400 x 100.
struct EventScene {
  UiTree t;
  UiWidgetId b;
  UiWidgetId s;
  UiWidgetId i;
  UiWidgetId p;
};

EventScene makeEventScene() {
  EventScene sc;
  sc.t = makeDefaultTree();
  sc.b = addWidget(sc.t, UiWidgetKind::Button, UiAnchor::TopLeft, 10, 10, 50, 20);
  sc.s = addSlider(sc.t, 100, 10, 100, 20, 0.0f, 10.0f, 0.0f);
  sc.i = addWidget(sc.t, UiWidgetKind::Input, UiAnchor::TopLeft, 200, 10, 50, 20);
  sc.p = addWidget(sc.t, UiWidgetKind::Panel, UiAnchor::TopLeft, 0, 100, 400,
                   100, kInvalidUiWidgetId, 0, true);
  layoutTree(sc.t);
  return sc;
}

}  // namespace

TEST(UiTreeEvents, PointerSequence) {
  EventScene sc = makeEventScene();
  UiTree& t = sc.t;
  // f0: over empty screen space (the root is layout-only): no events.
  frame(t, 0.0f, 0.0f, false);
  expectEvents(t, {});
  EXPECT_EQ(kInvalidUiWidgetId, t.pointerHit());
  // f1: onto the button: enter.
  frame(t, 15.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerEnter, sc.b}});
  EXPECT_EQ(sc.b, t.pointerHit());
  // f2: onto the slider: leave (the button) first, then enter.
  frame(t, 150.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerLeave, sc.b},
                   {UiEventKind::PointerEnter, sc.s}});
  EXPECT_EQ(sc.s, t.pointerHit());
  // f3: press on the slider: press + the value from x (t = 0.5 -> 5.0),
  // and the focus.
  frame(t, 150.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::PointerPress, sc.s},
                   {UiEventKind::SliderChanged, sc.s, 5.0f}});
  EXPECT_EQ(sc.s, t.focus());
  EXPECT_FLOAT_EQ(5.0f, t.node(sc.s)->sliderValue);
  // f4: the drag frame (still pressed, inside the rect): the new
  // value (t = 0.2 -> 2.0).
  frame(t, 120.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::SliderChanged, sc.s, 2.0f}});
  // f4b / f4c: drags toward the rect's edges (the clamped value is
  // exact at the edges; in-rect frames emit no hover events).
  frame(t, 100.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::SliderChanged, sc.s, 0.0f}});
  frame(t, 199.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::SliderChanged, sc.s, 9.9f}});
  // f5: release: the release only (no button press — it's a slider).
  frame(t, 199.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerRelease, sc.s}});
  EXPECT_FLOAT_EQ(9.9f, t.node(sc.s)->sliderValue);
  // f6: back to the button: leave (the slider) first, then enter.
  frame(t, 15.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerLeave, sc.s},
                   {UiEventKind::PointerEnter, sc.b}});
  // f7: press on the button: press + the focus.
  frame(t, 15.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::PointerPress, sc.b}});
  EXPECT_EQ(sc.b, t.focus());
  // f8: release on the same button: the release + the button click.
  frame(t, 15.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerRelease, sc.b},
                   {UiEventKind::ButtonPressed, sc.b}});
  // f9: press again, then f10: move off the button WHILE pressed
  // (the leave / enter, no press event — the press is in progress).
  frame(t, 15.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::PointerPress, sc.b}});
  frame(t, 150.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::PointerLeave, sc.b},
                   {UiEventKind::PointerEnter, sc.s}});
  // f11: release off the button: the release only (no button click —
  // the press started on the button, the release on the slider).
  frame(t, 150.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerRelease, sc.s}});
  // f12: onto the panel: the enter (the panel is the topmost there —
  // it occludes nothing behind it in this scene, but it takes the
  // events of that rect).
  frame(t, 350.0f, 150.0f, false);
  expectEvents(t, {{UiEventKind::PointerLeave, sc.s},
                   {UiEventKind::PointerEnter, sc.p}});
  EXPECT_EQ(sc.p, t.pointerHit());
  // f13: a press on the non-focusable panel: the press, no focus.
  frame(t, 350.0f, 150.0f, true);
  expectEvents(t, {{UiEventKind::PointerPress, sc.p}});
  EXPECT_EQ(sc.b, t.focus());
  frame(t, 350.0f, 150.0f, false);
  expectEvents(t, {{UiEventKind::PointerRelease, sc.p}});
}

TEST(UiTreeEvents, ZeroSizeWidgetsContainNothing) {
  UiTree t = makeDefaultTree();
  const UiWidgetId zb = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                  10, 10, 0, 10);
  const UiWidgetId zs = addSlider(t, 100, 10, 0, 20, 0.0f, 1.0f, 0.5f);
  layoutTree(t);
  // A zero-width rect is empty (half-open): no hit, no press, no
  // slider input (no division by the zero width).
  frame(t, 10.5f, 15.0f, true);
  EXPECT_EQ(kInvalidUiWidgetId, t.pointerHit());
  expectEvents(t, {});
  EXPECT_FLOAT_EQ(0.5f, t.node(zs)->sliderValue);
  frame(t, 10.5f, 15.0f, false);
  expectEvents(t, {});
  (void)zb;
}

TEST(UiTreeEvents, InputTextEvents) {
  EventScene sc = makeEventScene();
  UiTree& t = sc.t;
  // beginFrame: the pointerless frame boundary (no pointer this
  // frame; the input events do not stack onto the previous list).
  frame(t, 0.0f, 0.0f, false);
  t.beginFrame();
  // A new text: one InputText event (the new handle).
  expectOk(t.setInputText(sc.i, laige::render::StringRef{1}),
             "setInputText");
  expectEvents(t, {{UiEventKind::InputText, sc.i, 0.0f, 1, true}});
  // The same handle again: a no-op (no event).
  expectOk(t.setInputText(sc.i, laige::render::StringRef{1}),
             "setInputText");
  expectEvents(t, {{UiEventKind::InputText, sc.i, 0.0f, 1, true}});
  // A different handle: the second event.
  expectOk(t.setInputText(sc.i, laige::render::StringRef{2}), "setInputText");
  expectEvents(t, {{UiEventKind::InputText, sc.i, 0.0f, 1, true},
                   {UiEventKind::InputText, sc.i, 0.0f, 2, true}});
  EXPECT_EQ(2u, t.node(sc.i)->text.id);
  // The failure paths: a non-input kind, a stale id, the stopped tree.
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setInputText(sc.b, laige::render::StringRef{3}).error());
  expectOk(t.remove(sc.i), "remove(sc.i)");
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setInputText(sc.i, laige::render::StringRef{3}).error());
  UiTree stopped;
  EXPECT_EQ(ErrorCode::InvalidArgument,
            stopped.setInputText(kInvalidUiWidgetId,
                                 laige::render::StringRef{1}).error());
}

TEST(UiTreeEvents, EventOverflowDropsAndWarns) {
  MemorySink* sink = installCaptureSink();
  // maxEvents 1: each frame keeps its first event; the rest are
  // dropped (the cumulative count + one warn per dropped event).
  UiTree t = makeTree(16, 1, 400, 300);
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 10, 10, 50, 20);
  const UiWidgetId s = addSlider(t, 100, 10, 100, 20, 0.0f, 10.0f, 0.0f);
  layoutTree(t);
  // f1: enter (fits).
  frame(t, 15.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerEnter, b}}, 0);
  // f2: leave + enter: the leave fits, the enter is dropped.
  frame(t, 150.0f, 15.0f, false);
  expectEvents(t, {{UiEventKind::PointerLeave, b}}, 1);
  // f3: press + slider change: the press fits, the change is dropped.
  frame(t, 150.0f, 15.0f, true);
  expectEvents(t, {{UiEventKind::PointerPress, s}}, 2);
  EXPECT_EQ(2ull, t.droppedEventsTotal());
  EXPECT_EQ(2ull, countEvents(*sink, "ui_tree", "event_overflow"));
  const MemorySink::Entry* e = firstEvent(*sink, "event_overflow");
  ASSERT_NE(nullptr, e);
  EXPECT_EQ("ui_tree", e->subsystem);
  EXPECT_EQ(laige::log::Severity::Warn, e->severity);
  EXPECT_EQ("1", fieldOf(*e, "maxEvents"));
}

// ---------------------------------------------------------------------------
// UiTreeCrud (final) — the setters
// ---------------------------------------------------------------------------

TEST(UiTreeCrud, Setters) {
  UiTree t = makeDefaultTree();
  const UiWidgetId b = addWidget(t, UiWidgetKind::Button, UiAnchor::TopLeft,
                                 0, 0, 10, 10);
  const UiWidgetId s = addSlider(t, 0, 0, 100, 20, 0.0f, 10.0f, 5.0f);
  const UiWidgetId li = addWidget(t, UiWidgetKind::List, UiAnchor::TopLeft,
                                  0, 0, 50, 50);
  const UiWidgetId in = addWidget(t, UiWidgetKind::Input, UiAnchor::TopLeft,
                                  0, 0, 50, 20);
  // setSliderValue: the clamp (100 -> 10; NaN -> 0), no event.
  // (The pointer resets the frame over empty screen space — the
  // widgets all occupy the origin corner.)
  layoutTree(t);
  expectOk(t.handlePointer(UiPointerState{200.0f, 200.0f, false}),
           "handlePointer");
  expectOk(t.setSliderValue(s, 100.0f), "setSliderValue(100)");
  EXPECT_FLOAT_EQ(10.0f, t.node(s)->sliderValue);
  expectOk(t.setSliderValue(s, std::nanf("")), "setSliderValue(NaN)");
  EXPECT_FLOAT_EQ(0.0f, t.node(s)->sliderValue);
  expectOk(t.setSliderValue(s, 2.5f), "setSliderValue(2.5)");
  EXPECT_FLOAT_EQ(2.5f, t.node(s)->sliderValue);
  EXPECT_TRUE(t.events().empty());  // the programmatic set: no event
  // The kind / id failures.
  EXPECT_EQ(ErrorCode::InvalidArgument, t.setSliderValue(b, 1.0f).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setSliderValue(kInvalidUiWidgetId, 1.0f).error());
  // setScroll: the domain + the kind (layout normalizes the range).
  expectOk(t.setScroll(li, 5), "setScroll(5)");
  EXPECT_EQ(5, t.node(li)->scroll);
  EXPECT_EQ(ErrorCode::InvalidArgument, t.setScroll(li, -1).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setScroll(li, (1 << 20) + 1).error());
  EXPECT_EQ(ErrorCode::InvalidArgument, t.setScroll(b, 1).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setScroll(kInvalidUiWidgetId, 1).error());
  // setPlacement: the domain + the kind-independent geometry.
  expectOk(t.setPlacement(b, UiAnchor::Center, 1, -2, 20, 12),
           "setPlacement ok");
  EXPECT_EQ(UiAnchor::Center, t.node(b)->anchor);
  EXPECT_EQ(1, t.node(b)->x);
  EXPECT_EQ(-2, t.node(b)->y);
  EXPECT_EQ(20, t.node(b)->width);
  EXPECT_EQ(12, t.node(b)->height);
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setPlacement(b, UiAnchor::Center, 0, 0, -1, 1).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setPlacement(b, UiAnchor::Center, 0, 0, 1, (1 << 20) + 1).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setPlacement(b, UiAnchor::Center, -(1 << 20) - 1, 0, 1, 1).error());
  EXPECT_EQ(ErrorCode::InvalidArgument,
            t.setPlacement(kInvalidUiWidgetId, UiAnchor::Center, 0, 0, 1, 1).error());
  (void)in;
}

// ---------------------------------------------------------------------------
// UiTreeZeroAlloc — the per-frame loop allocates nothing (PERF-003)
// ---------------------------------------------------------------------------

TEST(UiTreeZeroAlloc, FrameLoopAllocatesNothing) {
  constexpr std::uint32_t kFrames = 1000;
  EventScene sc = makeEventScene();
  UiTree& t = sc.t;
  const std::vector<std::uint32_t> refs = {1, 2, 3};
  auto runFrames = [&]() {
    for (std::uint32_t f = 0; f < kFrames; ++f) {
      if (f % 32 == 0) {
        // A re-placement (the rect invalidates + the layout recomputes).
        if (t.setPlacement(sc.b, UiAnchor::TopLeft, 10, 10, 50, 20).isError()) {
          std::fprintf(stderr, "UiTreeZeroAlloc: setPlacement failed\n");
          std::abort();
        }
      }
      if (t.layout().isError()) {
        std::fprintf(stderr, "UiTreeZeroAlloc: layout failed\n");
        std::abort();
      }
      // The pointer: a deterministic sweep across the widget row, a
      // press cycle every 8 frames (the click + the slider drag).
      UiPointerState ptr;
      ptr.x = 50.0f + static_cast<float>(f % 300);
      ptr.y = 15.0f;
      ptr.pressed = (f % 8) < 2;
      if (t.handlePointer(ptr).isError()) {
        std::fprintf(stderr, "UiTreeZeroAlloc: handlePointer failed\n");
        std::abort();
      }
      if (f % 16 == 0) {
        laige::render::StringRef ref;
        ref.id = refs[(f / 16) % 3];
        if (t.setInputText(sc.i, ref).isError()) {
          std::fprintf(stderr, "UiTreeZeroAlloc: setInputText failed\n");
          std::abort();
        }
      }
    }
  };
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    runFrames();
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(0ull, reading.allocs)
        << kFrames << " frames of layout + pointer + input allocated "
        << reading.allocs << " heap blocks on the loop thread";
    std::printf("ui-tree-zeroalloc frames=%u allocs=%llu\n", kFrames,
                static_cast<unsigned long long>(reading.allocs));
  } else {
    runFrames();  // sanitizer tree: the leak-free run covers it
  }
}
