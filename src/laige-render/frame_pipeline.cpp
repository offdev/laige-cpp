// laige-render frame pipeline implementation (M2-GL-02).
//
// The synchronization argument (CONC-002; the full case is in the
// header preamble): one producer, one consumer; a single-slot handoff
// published through the monotonic seq_ atomic (release/acquire) with a
// seqlock copy — no mutex, no condition variable, no allocation in the
// hot path (PERF-003/006). The memory-order case:
//
//   producer publish:  slot_ = frame             (plain store)
//                     seq_.store(s + 2, release) (publishes slot_)
//   consumer copy:     a = seq_.load(acquire)    (synchronizes-with the
//                                                 producer's release
//                                                 store)
//                     d = slot_                   (plain load — the
//                                                 complete frame is
//                                                 visible)
//                     b = seq_.load(acquire)      (a != b → torn → retry)
//
// A torn copy can never be consumed (the end-check); the seq only
// increases (no ABA) and wraps only after ~292 years of frames. The
// consumedSeq_/inFlight_/stop_/submitted_ atomics carry the
// diagnostics, the waitIdle barrier, and the shutdown request — none
// gates correctness; seq_ alone does (CONC-002: partitioned ownership
// + an immutable snapshot, no shared locks).

#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#include "laige/render/frame_pipeline.h"
#include "laige/logging.h"

namespace laige::render {

namespace {

// The engine's monotonic steady time base in ns (the M1 clock epoch —
// the GameLoop's default clock source, M1-LOOP-01). One read per call;
// no allocation.
std::int64_t steadyNowNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
}

// deadlineNs with the 2^63 ns frame-time bound (the GameLoop clock
// bound, game_loop.h): frameIndex × periodNs must stay below
// INT64_MAX − referenceNs; beyond it the deadline clamps to INT64_MAX
// (documented, never UB — CPP-004).
std::int64_t boundedDeadline(std::int64_t referenceNs,
                             std::uint64_t periodNs,
                             std::uint64_t frameIndex) {
  const std::int64_t room =
      static_cast<std::int64_t>((std::numeric_limits<std::int64_t>::max)()) -
      referenceNs;
  const std::uint64_t maxIndex =
      (periodNs == 0 || room < 0)
          ? 0
          : static_cast<std::uint64_t>(room) / periodNs;
  if (frameIndex > maxIndex) {
    return (std::numeric_limits<std::int64_t>::max)();
  }
  return referenceNs + static_cast<std::int64_t>(frameIndex * periodNs);
}

}  // namespace

// ------------------------------------------------------------------------
// FrameClock
// ------------------------------------------------------------------------

FrameClock::FrameClock() noexcept = default;

FrameClock::FrameClock(FrameClock&& other) noexcept
    : rate_(other.rate_),
      periodNs_(other.periodNs_),
      referenceNs_(other.referenceNs_),
      valid_(other.valid_) {
  other.valid_ = false;
}

FrameClock& FrameClock::operator=(FrameClock&& other) noexcept {
  if (this != &other) {
    rate_ = other.rate_;
    periodNs_ = other.periodNs_;
    referenceNs_ = other.referenceNs_;
    valid_ = other.valid_;
    other.valid_ = false;
  }
  return *this;
}

Result<FrameClock, ErrorCode> FrameClock::create(
    FrameClockOptions options) noexcept {
  if (options.frameRateHz < kMinFrameRateHz ||
      options.frameRateHz > kMaxFrameRateHz) {
    // Rate outside the documented range — a configuration error
    // (API-008), never silent (FR-12.3); rate-limited by the facade
    // (LOG-004).
    LAIGE_LOG_WARN("render_thread", "frame_rate_invalid",
                   "the frame clock's frame rate is outside the documented "
                   "range",
                   laige::log::field("frame_rate_hz", options.frameRateHz),
                   laige::log::field("min_hz", kMinFrameRateHz),
                   laige::log::field("max_hz", kMaxFrameRateHz));
    return Result<FrameClock, ErrorCode>::failure(ErrorCode::InvalidArgument);
  }
  FrameClock clock;
  clock.rate_ = options.frameRateHz;
  // One division at construction (CORE-005): the floor truncation
  // shifts the deadline grid by < 1 ns per frame — the render_time is
  // the real clock reading, so there is no measurement error.
  clock.periodNs_ =
      (static_cast<std::uint64_t>(1'000'000'000) / options.frameRateHz);
  clock.referenceNs_ = steadyNowNs();
  clock.valid_ = true;
  return Result<FrameClock, ErrorCode>::success(std::move(clock));
}

bool FrameClock::valid() const noexcept { return valid_; }

std::uint32_t FrameClock::frameRateHz() const noexcept {
  return valid_ ? rate_ : 0;
}

std::uint64_t FrameClock::periodNs() const noexcept {
  return valid_ ? periodNs_ : 0;
}

std::int64_t FrameClock::referenceNs() const noexcept {
  return valid_ ? referenceNs_ : 0;
}

std::int64_t FrameClock::nowNs() const noexcept {
  return valid_ ? steadyNowNs() : 0;
}

std::int64_t FrameClock::deadlineNs(std::uint64_t frameIndex) const noexcept {
  if (!valid_) {
    return 0;
  }
  return boundedDeadline(referenceNs_, periodNs_, frameIndex);
}

std::int64_t FrameClock::waitFrame(std::uint64_t frameIndex) noexcept {
  if (!valid_) {
    return 0;
  }
  const std::int64_t deadline = deadlineNs(frameIndex);
  const std::int64_t now = steadyNowNs();
  if (now < deadline) {
    // One bounded sleep to the deadline (no spin — the producer's wait
    // is cadence, not work; PERF-002).
    const auto until = std::chrono::steady_clock::time_point{} +
                       std::chrono::nanoseconds(deadline);
    std::this_thread::sleep_until(until);
  }
  // The frame's ACTUAL presentation time (the render_time): on time the
  // deadline, late the real clock reading (ARCH-009 wall-clock fact —
  // the M1-LOOP-02 alpha contract clamps a late frame to 1.0).
  const std::int64_t after = steadyNowNs();
  return after > deadline ? after : deadline;
}

// ------------------------------------------------------------------------
// RenderThread
// ------------------------------------------------------------------------

RenderThread::RenderThread(RenderThreadOptions options) noexcept
    : options_(options) {
  // All state members are fully initialized before this line
  // (`thread_` is declared LAST — see the header's member note), so
  // the thread-start synchronization edge makes the whole state
  // visible to the consumer ([intro.multithread]). A spawn failure
  // throws std::system_error → terminate under -fno-exceptions (the
  // header's failure section; the thread-join paths never run).
  thread_ = std::thread(&RenderThread::runConsumer, this);
  LAIGE_LOG_INFO("render_thread", "thread_started",
                 "the render thread started");
}

RenderThread::~RenderThread() { shutdown(); }

RenderThread::RenderThread(RenderThread&& other) noexcept
    : valid_(false) {
  // The move stops the source first (joins a live thread) so the
  // handoff state is idle and can be copied without racing the
  // consumer (the header's ownership section).
  if (other.valid_) {
    other.shutdown();
  }
  options_ = std::move(other.options_);
  thread_ = std::move(other.thread_);
  seq_.store(other.seq_.load(std::memory_order_relaxed),
             std::memory_order_relaxed);
  slot_ = other.slot_;
  consumedSeq_.store(other.consumedSeq_.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
  inFlight_.store(other.inFlight_.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
  stop_.store(other.stop_.load(std::memory_order_relaxed),
              std::memory_order_relaxed);
  submitted_.store(other.submitted_.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
  rendered_.store(other.rendered_.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
  // The source is stopped; the moved-to object is STOPPED (the
  // GlContext precedent).
}

RenderThread& RenderThread::operator=(RenderThread&& other) noexcept {
  if (this != &other) {
    shutdown();  // stop + join the current thread before the swap
    options_ = std::move(other.options_);
    thread_ = std::move(other.thread_);
    seq_.store(other.seq_.load(std::memory_order_relaxed),
               std::memory_order_relaxed);
    slot_ = other.slot_;
    consumedSeq_.store(other.consumedSeq_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    inFlight_.store(other.inFlight_.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
    stop_.store(other.stop_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
    submitted_.store(other.submitted_.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
    rendered_.store(other.rendered_.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
    valid_ = false;
    if (other.valid_) {
      other.shutdown();
    }
  }
  return *this;
}

bool RenderThread::running() const noexcept { return valid_; }

Status RenderThread::submitFrame(const FrameDescriptor& frame) noexcept {
  // Stopped (moved-from/destroyed/shut down): a precondition violation,
  // not an engine failure — no log (the stopped-state precedent).
  if (!valid_) {
    return Status(ErrorCode::InvalidArgument);
  }
  for (;;) {
    const std::uint64_t s = seq_.load(std::memory_order_relaxed);
    if (s & 1u) {
      continue;  // a consumer copy is in progress: retry the publish
    }
    // Backpressure (PERF-008): the consumer can lag by at most one
    // frame (the single slot). seq > consumedSeq_ means a frame is
    // pending — the consumer is more than one frame behind: drop the
    // OLDER frame in place (never queue unboundedly) and log it
    // (rate-limited, LOG-004). The documented ±1 event race (the
    // header preamble) keeps the stats exact either way.
    const std::uint64_t consumed =
        consumedSeq_.load(std::memory_order_relaxed);
    if (s > consumed) {
      LAIGE_LOG_WARN("render_thread", "frame_dropped",
                     "the render thread is more than one frame behind; the "
                     "older frame was dropped",
                     laige::log::field("dropped_frame", s / 2u),
                     laige::log::field("new_frame", frame.frameIndex));
    }
    slot_ = frame;
    seq_.store(s + 2u, std::memory_order_release);
    submitted_.fetch_add(1u, std::memory_order_relaxed);
    return Status();
  }
}

void RenderThread::runConsumer() noexcept {
  // The render-thread start hook (the GlContext::makeCurrent takeover —
  // the render thread owns the context's current thread before any GL
  // work). Bounded, non-blocking (API-005).
  if (options_.onStart != nullptr) {
    options_.onStart(options_.onStartContext);
  }
  std::uint64_t lastSeen = 0;
  for (;;) {
    // Wait for a new frame or the stop request (a yield spin — no lock;
    // the OS reschedules during the vsync gap).
    while (!stop_.load(std::memory_order_acquire)) {
      if (seq_.load(std::memory_order_acquire) > lastSeen) {
        break;
      }
      std::this_thread::yield();
    }
    if (stop_.load(std::memory_order_acquire)) {
      break;  // shutdown: a pending frame is NOT flushed (waitIdle first)
    }
    // The seqlock copy (the preamble's argument): the seq must be even
    // and unchanged across the copy, or the copy is torn and retried.
    FrameDescriptor d;
    std::uint64_t a;
    for (;;) {
      a = seq_.load(std::memory_order_acquire);
      if (a & 1u) {
        continue;
      }
      d = slot_;
      const std::uint64_t b = seq_.load(std::memory_order_acquire);
      if (a == b) {
        break;
      }
    }
    lastSeen = a;
    // Mark the frame in flight BEFORE its consumed seq is visible so
    // waitIdle's pending/in-flight union covers it (the preamble).
    inFlight_.store(true, std::memory_order_release);
    consumedSeq_.store(a, std::memory_order_release);
    if (options_.batchStage != nullptr) {
      options_.batchStage(options_.stageContext, d);
    }
    if (options_.submitStage != nullptr) {
      options_.submitStage(options_.stageContext, d);
    }
    // The frame is fully rendered (its stages completed): count it
    // (the preamble's accounting — seq_/consumedSeq_ track the LAST
    // consumed frame's number, not the count, once drops overwrite
    // pending frames).
    rendered_.fetch_add(1u, std::memory_order_relaxed);
    inFlight_.store(false, std::memory_order_release);
  }
  // The stop hook: AFTER the last frame and BEFORE the thread exits —
  // resources the render thread owned (e.g. the GL context: the P0 EGL
  // stack cannot rebind a context last held by a dead thread) must be
  // handed back while this thread is still alive. Bounded, non-blocking
  // (API-005).
  if (options_.onStop != nullptr) {
    options_.onStop(options_.onStopContext);
  }
}

void RenderThread::waitIdle() noexcept {
  while (valid_ && !stop_.load(std::memory_order_acquire)) {
    const std::uint64_t seq = seq_.load(std::memory_order_acquire);
    const std::uint64_t consumed =
        consumedSeq_.load(std::memory_order_acquire);
    // A frame is outstanding iff it is pending (not yet consumed) or
    // in flight (consumed, stages running); the in-flight mark is set
    // before the consumed store, so the union has no gap (the
    // preamble).
    if (seq <= consumed && !inFlight_.load(std::memory_order_acquire)) {
      break;
    }
    std::this_thread::yield();
  }
}

void RenderThread::shutdown() noexcept {
  if (!valid_) {
    return;  // idempotent (CONC-006): a second call is a no-op
  }
  valid_ = false;
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();  // ordered: the thread never outlives the call
  }
  LAIGE_LOG_INFO("render_thread", "thread_stopped",
                 "the render thread stopped");
}

RenderThreadStats RenderThread::stats() const noexcept {
  RenderThreadStats s;
  const std::uint64_t seq = seq_.load(std::memory_order_relaxed);
  const std::uint64_t consumed = consumedSeq_.load(std::memory_order_relaxed);
  // Outstanding = pending slot (≤ 1) + in-flight frame (≤ 1): a frame
  // that is neither rendered nor dropped.
  const std::uint64_t outstanding =
      ((seq > consumed) ? 1u : 0u) +
      (inFlight_.load(std::memory_order_relaxed) ? 1u : 0u);
  s.framesSubmitted = submitted_.load(std::memory_order_relaxed);
  s.framesRendered = rendered_.load(std::memory_order_relaxed);
  // The exact accounting invariant (the header preamble): every
  // published frame is rendered, outstanding (≤ 2), or dropped — no
  // underflow is possible.
  s.framesDropped = s.framesSubmitted - s.framesRendered - outstanding;
  return s;
}

}  // namespace laige::render
