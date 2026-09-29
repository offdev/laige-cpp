// laige-render tests (M2-GL-02): the render thread + the lock-free frame
// handoff + the vsync-paced frame clock.
//
// Suite map (the `render_thread` CTest entry selects exactly these):
//   FrameClock
//             the frame deadline grid, the wait pacing, and the
//             stopped-clock state (no GL required);
//   RenderThreadHandoff
//             the single-slot lock-free handoff: in-order delivery, the
//             backpressure drop (PERF-008), the stage ordering, the
//             shutdown contract (CONC-006) (no GL required);
//   RenderThreadOffscreen
//             the full pipeline against an offscreen GlContext (the
//             3000-frame no-deadlock run, the slowed-submit drop path) —
//             needs a usable OpenGL 3.3 environment, always present on
//             the P0 CI runners; on a local machine without a GL driver
//             the suite GTEST_SKIPs with the clean Status reason (the
//             GlContextSmoke precedent, docs/api/gl_context.md).

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "laige/render/frame_pipeline.h"
#include "laige/render/gl_context.h"
#include "laige/logging.h"

using laige::ErrorCode;
using laige::Result;
using laige::Status;
using laige::render::FrameClock;
using laige::render::FrameClockOptions;
using laige::render::FrameDescriptor;
using laige::render::GlContext;
using laige::render::RenderThread;
using laige::render::RenderThreadOptions;
using laige::render::RenderThreadStats;
using laige::render::kDefaultFrameRateHz;
using laige::render::kMaxFrameRateHz;
using laige::render::kMinFrameRateHz;

namespace {

// The engine's monotonic steady time base in ns (the M1 clock epoch).
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

std::int64_t steadyNowNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
}

// The documented RGBA8 encoding of the per-frame clear color:
// red = (frameIndex % 4) * 0.25f → 0.0/0.25/0.5/0.75 → 0/64/128/191
// (the GL 3.3 clear-color conversion, the GlContextSmoke precedent).
const std::uint8_t kRedChannel[4] = {0, 64, 128, 191};

// ------------------------------------------------------------------------
// Log capture (the ecs_guardrails MemorySink pattern)
// ------------------------------------------------------------------------

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

// Installs a fresh capture sink with rate limiting OFF (the tests
// assert per-event counts, not the facade's LOG-004 window).
MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

std::size_t countEvents(const MemorySink& sink, std::string_view subsystem,
                        std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

bool hasField(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return true;
  }
  return false;
}

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

// ------------------------------------------------------------------------
// Handoff test stages (free functions — StageFn is a plain function
// pointer, PERF-006)
// ------------------------------------------------------------------------

// The handoff scenario state: the recorded frame indices (written ONLY
// by the render thread — read by the main thread after waitIdle(),
// whose acquire/release atomics provide the happens-before) and an
// optional per-frame stage delay (the artificially slowed submit).
struct HandoffState {
  std::vector<std::uint64_t> recorded;
  std::uint64_t stageDelayUs{0};
};

void stageRecord(void* context, const FrameDescriptor& frame) noexcept {
  auto* s = static_cast<HandoffState*>(context);
  if (s->stageDelayUs != 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(s->stageDelayUs));
  }
  s->recorded.push_back(frame.frameIndex);
}

// The batch/submit ordering check: each stage appends its marker to the
// shared list (the render thread only).
struct MarkOrderState {
  std::vector<char> marks;
};

void stageMarkBatch(void* context, const FrameDescriptor&) noexcept {
  static_cast<MarkOrderState*>(context)->marks.push_back('b');
}

void stageMarkSubmit(void* context, const FrameDescriptor&) noexcept {
  static_cast<MarkOrderState*>(context)->marks.push_back('s');
}

struct OnStartState {
  std::thread::id owner;
  std::thread::id start;
  std::atomic<bool> done{false};
};

void hookRecordStart(void* context) noexcept {
  auto* h = static_cast<OnStartState*>(context);
  h->start = std::this_thread::get_id();
  h->done.store(true, std::memory_order_release);
}

}  // namespace

// ------------------------------------------------------------------------
// FrameClock — the frame deadline grid (no GL)
// ------------------------------------------------------------------------

TEST(FrameClock, RateValidation) {
  auto* sink = installCaptureSink();
  FrameClockOptions bad;
  bad.frameRateHz = 0;
  Result<FrameClock> r0 = FrameClock::create(bad);
  ASSERT_TRUE(r0.isError());
  EXPECT_EQ(r0.error(), ErrorCode::InvalidArgument);

  bad.frameRateHz = kMaxFrameRateHz + 1;
  Result<FrameClock> r1 = FrameClock::create(bad);
  ASSERT_TRUE(r1.isError());
  EXPECT_EQ(r1.error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "render_thread", "frame_rate_invalid"), 2u);
  restoreLogger();
}

TEST(FrameClock, BoundaryRatesAccepted) {
  const std::uint32_t rates[] = {kMinFrameRateHz, kDefaultFrameRateHz,
                                 kMaxFrameRateHz};
  for (const std::uint32_t rate : rates) {
    FrameClockOptions opts;
    opts.frameRateHz = rate;
    Result<FrameClock> r = FrameClock::create(opts);
    ASSERT_TRUE(r.ok()) << "rate " << rate << " rejected";
    FrameClock c = std::move(r).takeValue();
    EXPECT_TRUE(c.valid());
    EXPECT_EQ(c.frameRateHz(), rate);
    EXPECT_EQ(c.periodNs(), static_cast<std::uint64_t>(kNsPerSecond) / rate);
  }
}

TEST(FrameClock, DeadlineGridIsExact) {
  FrameClockOptions opts;
  opts.frameRateHz = 100;
  FrameClock c = std::move(FrameClock::create(opts)).takeValue();
  const std::int64_t ref = c.referenceNs();
  const std::uint64_t period = c.periodNs();
  // 1-based, exact integer grid (the seconds-free form, no float):
  EXPECT_EQ(c.deadlineNs(1), ref + static_cast<std::int64_t>(period));
  EXPECT_EQ(c.deadlineNs(2), ref + static_cast<std::int64_t>(2 * period));
  EXPECT_EQ(c.deadlineNs(1000), ref + static_cast<std::int64_t>(1000 * period));
  EXPECT_EQ(c.deadlineNs(1001) - c.deadlineNs(1000),
            static_cast<std::int64_t>(period));
  // The 2^63 ns frame-time bound: beyond it the deadline clamps
  // (documented, never UB — CPP-004):
  const std::uint64_t maxIndex =
      (static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) -
       static_cast<std::uint64_t>(ref)) /
      period;
  EXPECT_EQ(c.deadlineNs(maxIndex),
            ref + static_cast<std::int64_t>(maxIndex * period));
  EXPECT_EQ(c.deadlineNs(maxIndex + 1),
            (std::numeric_limits<std::int64_t>::max)());
}

TEST(FrameClock, WaitFramePacesAndReturnsFrameTime) {
  FrameClockOptions opts;
  opts.frameRateHz = 1000;  // 1 ms frames
  FrameClock c = std::move(FrameClock::create(opts)).takeValue();
  const std::int64_t d1 = c.deadlineNs(1);
  const std::int64_t f1 = c.waitFrame(1);
  const std::int64_t f2 = c.waitFrame(2);
  // Never early (the pace), and the 1 ms grid (no multi-ms stall):
  EXPECT_GE(f1, d1);
  EXPECT_GE(f2, f1);
  EXPECT_LE(f2 - f1, 500'000'000);
  // A late producer gets the ACTUAL frame time, not the deadline
  // (ARCH-009 wall-clock fact — the M1-LOOP-02 alpha contract clamps a
  // late frame to 1.0): after sleeping 3 ms past the 2 ms deadline,
  // frame 3's deadline (3 ms) is already behind, so waitFrame returns
  // now — measurably past the deadline:
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  const std::int64_t d3 = c.deadlineNs(3);
  const std::int64_t f3 = c.waitFrame(3);
  EXPECT_GE(f3, d3);
  EXPECT_GE(f3 - d3, 1'000'000);      // at least 1 ms late: the real time
  EXPECT_LE(f3 - d3, 100'000'000);    // sanity: no multi-second stall
}

TEST(FrameClock, NowSharesTheM1TimeBase) {
  FrameClock c =
      std::move(FrameClock::create(FrameClockOptions{})).takeValue();
  // Same epoch as the M1 headless monotonic clock (steady_clock ns):
  // the two reads must agree to within the call overhead (10 ms).
  const std::int64_t a = c.nowNs();
  const std::int64_t b = steadyNowNs();
  EXPECT_GE(a, b - 10'000'000);
  EXPECT_LE(a, b + 10'000'000);
  // Monotonic:
  EXPECT_GE(c.nowNs(), a);
}

TEST(FrameClock, StoppedClockIsNoOp) {
  FrameClock stopped;
  EXPECT_FALSE(stopped.valid());
  EXPECT_EQ(stopped.frameRateHz(), 0u);
  EXPECT_EQ(stopped.periodNs(), 0u);
  EXPECT_EQ(stopped.referenceNs(), 0);
  EXPECT_EQ(stopped.nowNs(), 0);
  EXPECT_EQ(stopped.deadlineNs(1), 0);
  EXPECT_EQ(stopped.waitFrame(1), 0);

  FrameClock moved(std::move(stopped));
  EXPECT_FALSE(stopped.valid());   // the source is stopped
  EXPECT_FALSE(moved.valid());     // the moved-to clock is stopped
  EXPECT_EQ(moved.nowNs(), 0);
}

// ------------------------------------------------------------------------
// RenderThreadHandoff — the single-slot lock-free handoff (no GL)
// ------------------------------------------------------------------------

TEST(RenderThreadHandoff, FramesDeliverInOrder_NoDrops) {
  HandoffState state;
  RenderThreadOptions opts;
  opts.submitStage = &stageRecord;
  opts.stageContext = &state;
  RenderThread thread(opts);
  ASSERT_TRUE(thread.running());

  // One frame per waitIdle: the consumer always keeps up, so no frame
  // is ever pending when the next publish lands (0 drops expected).
  for (std::uint64_t i = 1; i <= 200; ++i) {
    FrameDescriptor d;
    d.frameIndex = i;
    d.simTick = i * 2;
    d.renderTimeNs = static_cast<std::int64_t>(i) * 1'000'000;
    ASSERT_TRUE(thread.submitFrame(d).ok());
    thread.waitIdle();
  }
  const RenderThreadStats s = thread.stats();
  EXPECT_EQ(s.framesSubmitted, 200u);
  EXPECT_EQ(s.framesRendered, 200u);
  EXPECT_EQ(s.framesDropped, 0u);
  // In-order delivery: every index exactly once, ascending.
  ASSERT_EQ(state.recorded.size(), 200u);
  for (std::uint64_t i = 0; i < state.recorded.size(); ++i) {
    EXPECT_EQ(state.recorded[i], i + 1);
  }
  thread.shutdown();
}

TEST(RenderThreadHandoff, BackpressureDropsOlderFrames) {
  auto* sink = installCaptureSink();
  HandoffState state;
  state.stageDelayUs = 4000;  // the slowed submit (the scope's drop path)
  RenderThreadOptions opts;
  opts.submitStage = &stageRecord;
  opts.stageContext = &state;
  RenderThread thread(opts);

  // 20 back-to-back publishes (no pacing): the consumer renders one
  // frame per 4 ms, so a pending frame exists for every publish after
  // the first — the older frame is dropped in place (PERF-008).
  for (std::uint64_t i = 1; i <= 20; ++i) {
    FrameDescriptor d;
    d.frameIndex = i;
    ASSERT_TRUE(thread.submitFrame(d).ok());
  }
  thread.waitIdle();
  const RenderThreadStats s = thread.stats();
  EXPECT_EQ(s.framesSubmitted, 20u);
  // The exact accounting invariant (no pending frame after waitIdle):
  EXPECT_EQ(s.framesRendered + s.framesDropped, s.framesSubmitted);
  EXPECT_GE(s.framesRendered, 1u);   // the first frame always renders
  EXPECT_GT(s.framesDropped, 0u);    // the drop path fired
  // The rendered frames are strictly ascending (no re-render, no gap in
  // the seqlock accounting — the drops are the missing indices):
  for (std::size_t i = 1; i < state.recorded.size(); ++i) {
    EXPECT_GT(state.recorded[i], state.recorded[i - 1]);
  }
  // The drop is logged, never silent (FR-12.3; LOG-004 rate-limited in
  // production, per-event here). The producer publishes 1..20 in
  // order, so every dropped frame is the one just before the survivor:
  const std::size_t drops =
      countEvents(*sink, "render_thread", "frame_dropped");
  EXPECT_GE(drops, 1u);
  for (const auto& e : sink->entries) {
    if (e.subsystem == "render_thread" && e.event == "frame_dropped") {
      EXPECT_EQ(e.severity, laige::log::Severity::Warn);
      ASSERT_TRUE(hasField(e, "dropped_frame"));
      ASSERT_TRUE(hasField(e, "new_frame"));
      const long droppedFrame = std::stol(fieldOf(e, "dropped_frame"));
      const long newFrame = std::stol(fieldOf(e, "new_frame"));
      EXPECT_EQ(newFrame, droppedFrame + 1);
    }
  }
  thread.shutdown();
  restoreLogger();
}

TEST(RenderThreadHandoff, BatchStageRunsBeforeSubmitStage) {
  MarkOrderState state;
  RenderThreadOptions opts;
  opts.batchStage = &stageMarkBatch;
  opts.submitStage = &stageMarkSubmit;
  opts.stageContext = &state;
  RenderThread thread(opts);
  for (std::uint64_t i = 1; i <= 10; ++i) {
    FrameDescriptor d;
    d.frameIndex = i;
    ASSERT_TRUE(thread.submitFrame(d).ok());
    thread.waitIdle();
  }
  thread.shutdown();
  // Both stages ran, batch BEFORE submit, once per frame:
  // b,s,b,s,... (the stage order in runConsumer).
  ASSERT_EQ(state.marks.size(), 20u);
  for (std::size_t i = 0; i < state.marks.size(); ++i) {
    EXPECT_EQ(state.marks[i], (i % 2u == 0u) ? 'b' : 's');
  }
}

TEST(RenderThreadHandoff, OnStartHookRunsOnTheRenderThread) {
  OnStartState hook;
  hook.owner = std::this_thread::get_id();
  RenderThreadOptions opts;
  opts.onStart = &hookRecordStart;
  opts.onStartContext = &hook;
  RenderThread thread(opts);
  // The hook runs once on the render thread BEFORE the first frame is
  // consumed: submit one frame and wait for it — at waitIdle's return
  // the hook has completed (no racy check right after the spawn).
  FrameDescriptor d;
  d.frameIndex = 1;
  ASSERT_TRUE(thread.submitFrame(d).ok());
  thread.waitIdle();
  ASSERT_TRUE(hook.done.load(std::memory_order_acquire));
  EXPECT_NE(hook.start, hook.owner);  // the hook ran off-owner
  thread.shutdown();
}

TEST(RenderThreadHandoff, ShutdownIsOrderedAndIdempotent) {
  HandoffState state;
  RenderThreadOptions opts;
  opts.submitStage = &stageRecord;
  opts.stageContext = &state;
  RenderThread thread(opts);
  FrameDescriptor d;
  d.frameIndex = 1;
  ASSERT_TRUE(thread.submitFrame(d).ok());
  thread.waitIdle();
  thread.shutdown();
  EXPECT_FALSE(thread.running());
  thread.shutdown();  // idempotent (CONC-006): a no-op, no crash
  EXPECT_FALSE(thread.running());
  // A stopped thread rejects every publish (no log — the
  // stopped-state precedent):
  EXPECT_EQ(thread.submitFrame(d).error(), ErrorCode::InvalidArgument);
  const RenderThreadStats s = thread.stats();
  EXPECT_EQ(s.framesSubmitted, 1u);
  EXPECT_EQ(s.framesRendered, 1u);
  EXPECT_EQ(s.framesDropped, 0u);
}

TEST(RenderThreadHandoff, ShutdownDoesNotFlushPendingFrame) {
  HandoffState state;
  state.stageDelayUs = 2000;  // slower than the publish below can finish
  RenderThreadOptions opts;
  opts.submitStage = &stageRecord;
  opts.stageContext = &state;
  RenderThread thread(opts);
  FrameDescriptor d;
  d.frameIndex = 1;
  ASSERT_TRUE(thread.submitFrame(d).ok());
  // shutdown() does NOT flush a pending frame (the ordered-shutdown
  // contract: waitIdle() first when the last frame must render). The
  // frame may or may not have been consumed by the stop request —
  // both outcomes are legal; the accounting must be exact either way:
  thread.shutdown();
  const RenderThreadStats s = thread.stats();
  EXPECT_EQ(s.framesSubmitted, 1u);
  EXPECT_LE(s.framesRendered, 1u);
  EXPECT_EQ(s.framesDropped, 0u);  // a pending frame is never a drop
  // The exact invariant (pending = 1 iff the frame was never consumed):
  const std::uint64_t pending = (s.framesRendered == 0u) ? 1u : 0u;
  EXPECT_EQ(s.framesRendered + s.framesDropped + pending, s.framesSubmitted);
}

TEST(RenderThreadHandoff, MoveStopsSourceAndTarget) {
  HandoffState state;
  RenderThreadOptions opts;
  opts.submitStage = &stageRecord;
  opts.stageContext = &state;
  RenderThread a(opts);
  ASSERT_TRUE(a.running());
  FrameDescriptor d;
  d.frameIndex = 1;
  ASSERT_TRUE(a.submitFrame(d).ok());
  a.waitIdle();
  RenderThread b(std::move(a));
  EXPECT_FALSE(a.running());  // the source is stopped
  EXPECT_FALSE(b.running());  // the moved-to object is STOPPED (the
                              // GlContext precedent)
  EXPECT_EQ(a.submitFrame(d).error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(b.submitFrame(d).error(), ErrorCode::InvalidArgument);
}

// ------------------------------------------------------------------------
// RenderThreadOffscreen — the full pipeline against an offscreen FBO
// (GL required; GTEST_SKIPs without a usable OpenGL 3.3 environment)
// ------------------------------------------------------------------------

namespace {

struct GlStageState {
  GlContext* gl{};
  std::vector<std::uint64_t> recorded;
  std::uint64_t stageDelayUs{0};
  bool glFailed{false};
};

// The submit stage: the per-frame clear (the frame pipeline's draw
// work — M2-SPRITE-02 replaces this with the instanced draw).
void stageGlClear(void* context, const FrameDescriptor& frame) noexcept {
  auto* s = static_cast<GlStageState*>(context);
  if (s->stageDelayUs != 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(s->stageDelayUs));
  }
  s->recorded.push_back(frame.frameIndex);
  if (!s->gl->clear((frame.frameIndex % 4u) * 0.25f, 0.0f, 0.0f, 1.0f)
           .ok()) {
    s->glFailed = true;
  }
}

// The render-thread takeover hook (the GlContext::makeCurrent contract,
// M2-GL-02): the render thread owns the context's current thread
// before any GL work.
void hookGlMakeCurrent(void* context) noexcept {
  auto* s = static_cast<GlStageState*>(context);
  if (!s->gl->makeCurrent().ok()) {
    s->glFailed = true;
  }
}

// Creates the offscreen context; on failure records the clean Status
// reason (the GTEST_SKIP with the message happens in the test body —
// the GlContextSmoke precedent) and returns false.
bool makeGlContext(GlContext& out, std::string& reason) {
  Result<GlContext> r = GlContext::createHeadless(16, 16);
  if (r.isError()) {
    reason = laige::errorText(r.error());
    return false;
  }
  out = std::move(r).takeValue();
  return true;
}

}  // namespace

TEST(RenderThreadOffscreen, LowRate_NoDrops_FullPipeline) {
  GlContext ctx;
  std::string reason;
  if (!makeGlContext(ctx, reason)) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment here: " << reason;
  }
  ASSERT_TRUE(ctx.valid());
  // Headless: no display rate (the frame clock's target rate stands in):
  EXPECT_EQ(ctx.refreshRateHz(), 0u);
  // The handoff protocol (docs/api/gl_context.md): the old owner
  // (main) releases the context, so the render thread's takeover in
  // onStart is a FRESH bind — the P0 EGL stack rejects a takeover
  // while the context is still current on another live thread.
  ASSERT_TRUE(ctx.release().ok());

  GlStageState state;
  state.gl = &ctx;
  RenderThreadOptions opts;
  opts.submitStage = &stageGlClear;
  opts.stageContext = &state;
  opts.onStart = &hookGlMakeCurrent;
  opts.onStartContext = &state;
  RenderThread thread(opts);

  // 30 frames at 100 Hz (10 ms pace): the consumer (a µs-scale clear)
  // always keeps up — 0 drops expected on a healthy machine.
  FrameClockOptions co;
  co.frameRateHz = 100;
  FrameClock clock = std::move(FrameClock::create(co)).takeValue();
  for (std::uint64_t i = 1; i <= 30; ++i) {
    FrameDescriptor d;
    d.frameIndex = i;
    d.simTick = i;
    d.renderTimeNs = clock.waitFrame(i);  // the vsync-paced render_time
    ASSERT_TRUE(thread.submitFrame(d).ok());
  }
  thread.waitIdle();
  const RenderThreadStats s = thread.stats();
  EXPECT_EQ(s.framesSubmitted, 30u);
  EXPECT_EQ(s.framesRendered, 30u);
  EXPECT_EQ(s.framesDropped, 0u);
  ASSERT_EQ(state.recorded.size(), 30u);

  EXPECT_FALSE(state.glFailed);  // the render-thread GL work succeeded

  // Ordered shutdown → the main thread takes over the context and
  // readbacks the LAST rendered frame's color (end-to-end proof the
  // descriptor reached the GL pipeline):
  thread.shutdown();
  EXPECT_TRUE(ctx.makeCurrent().ok());
  std::uint8_t px[4] = {0, 0, 0, 0};
  ASSERT_TRUE(ctx.readPixel(0, 0, px).ok());
  const std::uint64_t last = state.recorded.back();
  EXPECT_EQ(px[0], kRedChannel[last % 4u]);
  EXPECT_EQ(px[1], 0u);
  EXPECT_EQ(px[2], 0u);
  EXPECT_EQ(px[3], 255u);

  // Machine-greppable line (the docs/testing.md convention):
  std::cout << "render-thread: kind=lowrate frames=30 rendered="
            << s.framesRendered << " dropped=" << s.framesDropped
            << " pixel=ok status=ok\n";
}

TEST(RenderThreadOffscreen, ThreeThousandFrames_NoDeadlock) {
  GlContext ctx;
  std::string reason;
  if (!makeGlContext(ctx, reason)) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment here: " << reason;
  }
  ASSERT_TRUE(ctx.valid());
  // Handoff protocol: main releases the context before the render
  // thread takes it over (a fresh bind — docs/api/gl_context.md).
  ASSERT_TRUE(ctx.release().ok());

  GlStageState state;
  state.gl = &ctx;
  RenderThreadOptions opts;
  opts.submitStage = &stageGlClear;
  opts.stageContext = &state;
  opts.onStart = &hookGlMakeCurrent;
  opts.onStartContext = &state;
  RenderThread thread(opts);

  // The step's integration run: 3000 frames, offscreen, no deadlock.
  // 1000 Hz pace (~3 s wall); the consumer is a µs-scale clear.
  FrameClockOptions co;
  co.frameRateHz = 1000;
  FrameClock clock = std::move(FrameClock::create(co)).takeValue();
  for (std::uint64_t i = 1; i <= 3000; ++i) {
    FrameDescriptor d;
    d.frameIndex = i;
    d.simTick = i;
    d.renderTimeNs = clock.waitFrame(i);
    ASSERT_TRUE(thread.submitFrame(d).ok());
  }
  thread.waitIdle();  // must return (no deadlock — the TSan job's gate)
  const RenderThreadStats s = thread.stats();
  EXPECT_EQ(s.framesSubmitted, 3000u);
  // The exact accounting invariant (every frame rendered, dropped, or —
  // never here after waitIdle — pending):
  EXPECT_EQ(s.framesRendered + s.framesDropped, s.framesSubmitted);
  EXPECT_GT(s.framesRendered, 0u);

  EXPECT_FALSE(state.glFailed);
  thread.shutdown();
  EXPECT_TRUE(ctx.makeCurrent().ok());
  std::uint8_t px[4] = {0, 0, 0, 0};
  ASSERT_TRUE(ctx.readPixel(0, 0, px).ok());
  const std::uint64_t last = state.recorded.back();
  EXPECT_EQ(px[0], kRedChannel[last % 4u]);

  std::cout << "render-thread: kind=3000 frames=3000 rendered="
            << s.framesRendered << " dropped=" << s.framesDropped
            << " pixel=ok status=ok\n";
}

TEST(RenderThreadOffscreen, SlowedSubmit_ExercisesDropPath) {
  auto* sink = installCaptureSink();
  GlContext ctx;
  std::string reason;
  if (!makeGlContext(ctx, reason)) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment here: " << reason;
  }
  ASSERT_TRUE(ctx.valid());
  // Handoff protocol: main releases the context before the render
  // thread takes it over (a fresh bind — docs/api/gl_context.md).
  ASSERT_TRUE(ctx.release().ok());

  GlStageState state;
  state.gl = &ctx;
  state.stageDelayUs = 4000;  // the artificially slowed submit
  RenderThreadOptions opts;
  opts.submitStage = &stageGlClear;
  opts.stageContext = &state;
  opts.onStart = &hookGlMakeCurrent;
  opts.onStartContext = &state;
  RenderThread thread(opts);

  // 15 back-to-back publishes against a 4 ms submit: the older frames
  // are dropped in place (PERF-008 — never queued unboundedly).
  for (std::uint64_t i = 1; i <= 15; ++i) {
    FrameDescriptor d;
    d.frameIndex = i;
    ASSERT_TRUE(thread.submitFrame(d).ok());
  }
  thread.waitIdle();
  const RenderThreadStats s = thread.stats();
  EXPECT_EQ(s.framesSubmitted, 15u);
  EXPECT_EQ(s.framesRendered + s.framesDropped, s.framesSubmitted);
  EXPECT_GE(s.framesRendered, 1u);
  EXPECT_GT(s.framesDropped, 0u);
  // The drop is logged (never silent — FR-12.3):
  EXPECT_GT(countEvents(*sink, "render_thread", "frame_dropped"), 0u);

  EXPECT_FALSE(state.glFailed);
  thread.shutdown();
  EXPECT_TRUE(ctx.makeCurrent().ok());
  std::uint8_t px[4] = {0, 0, 0, 0};
  ASSERT_TRUE(ctx.readPixel(0, 0, px).ok());
  const std::uint64_t last = state.recorded.back();
  EXPECT_EQ(px[0], kRedChannel[last % 4u]);

  std::cout << "render-thread: kind=slowed frames=15 rendered="
            << s.framesRendered << " dropped=" << s.framesDropped
            << " pixel=ok status=ok\n";
  restoreLogger();
}
