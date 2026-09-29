// laige-render frame pipeline (M2-GL-02): the render thread, the
// lock-free frame handoff, and the vsync-paced frame clock.
//
// PRD §10.2 (threading model): rendering runs on its own thread — the
// frame pipeline (cull/batch → submit) — with a lock-free handoff of
// the frame descriptor from the main/sim thread. This header ships
// that handoff plus the clock that paces it. The pipeline's actual
// stages — the sprite batcher's cull/batch (M2-SPRITE-01) and the GPU
// submit (M2-SPRITE-02) — plug in as plain function callbacks; nothing
// in this header depends on GL (the GlContext's render-thread takeover
// and the window swap/present land with the engine's windowed wiring,
// M2-SPRITE-02 onward).
//
//   FrameDescriptor     One presentation frame's handed-off data
//   StageFn             A pipeline stage callback (cull/batch or submit)
//   RenderThreadOptions The render thread's configuration
//   RenderThread        The consumer thread + the single-slot handoff
//   RenderThreadStats   The since-construction counters
//   kMinFrameRateHz / kDefaultFrameRateHz / kMaxFrameRateHz
//                       The documented frame-rate range
//   FrameClockOptions   The frame clock's configuration
//   FrameClock          The vsync-paced frame clock (deadline grid +
//                       the render_time provider)
//
// ---------------------------------------------------------------------------
// The frame handoff (PRD §10.2, CONC-002)
// ---------------------------------------------------------------------------
//
// One producer (the main/sim thread — the engine's frame loop) and one
// consumer (the render thread). The handoff is a SINGLE slot holding
// one FrameDescriptor plus six atomics — no mutex, no condition
// variable, no queue (PERF-008: the backlog is bounded to exactly one
// pending frame by construction, never unbounded):
//
//   seq_          the monotonic publish sequence (u64). Even values are
//                 idle (seq_/2 = the latest published frame's number);
//                 an odd value marks a write in progress.
//   slot_         the FrameDescriptor (a 32-byte POD in plain memory —
//                 published through seq_)
//   consumedSeq_  the consumer's last fully-consumed seq (consumer →
//                 producer diagnostic, a release store)
//   inFlight_     true while a consumed frame's stages are running
//                 (consumer → producer diagnostic for waitIdle)
//   stop_         the shutdown request (producer → consumer)
//   submitted_    the publish counter (the producer's; relaxed)
//   rendered_     the rendered-frame counter (the consumer's — a frame
//                 is counted when its stages complete). seq_/
//                 consumedSeq_ track the LAST consumed frame's number,
//                 not the count: a drop overwrites the slot without the
//                 consumer ever consuming the older frame, so the
//                 count is a real counter, not a seq difference.
//
// CONC-002 synchronization argument (one producer, one consumer —
// partitioned ownership, no shared locks):
//
//   producer publish:   slot_ = frame              (plain store)
//                      seq_.store(s + 2, release)  (publishes slot_)
//   consumer copy:      a = seq_.load(acquire)     (the publication
//                                              barrier: an acquire load
//                                              synchronizes-with the
//                                              producer's release store)
//                      d = slot_                   (plain load — a
//                                              complete frame is
//                                              visible)
//                      b = seq_.load(acquire)      (a != b → torn →
//                                              retry)
//
// The consumer copies under the classic sequence lock (McIlroy/Dekker):
// the seq must be even before AND unchanged after the copy, so a frame
// that reaches the pipeline is ALWAYS a complete snapshot — a
// concurrent producer write may overlap the copy window, but it can
// never produce a consumed torn value (the end-check discards it). No
// ABA is possible: the seq only increases, and a 2^64 wrap would take
// ~292 years of frames at the 1000 Hz maximum. The diagnostic atomics
// (consumedSeq_/inFlight_/stop_/submitted_/rendered_) carry no
// correctness — seq_ alone does; they exist for the accounting,
// waitIdle, and the ordered shutdown.
//
// Backpressure (PERF-008, the scope's "never queue unboundedly"): the
// single slot means the consumer can lag by AT MOST one frame. When
// the producer publishes while a frame is still pending (seq_ >
// consumedSeq_), the OLDER frame is dropped — replaced in place — and
// one rate-limited Warn event is emitted (LOG-004):
//
//     render_thread/frame_dropped    fields: dropped_frame, new_frame
//
// The exact accounting invariant (stats) — every published frame is in
// exactly one bucket at every instant:
//
//     framesSubmitted = framesRendered + framesDropped + outstanding
//
// where outstanding = pending + in-flight: 1 iff a frame is currently
// pending in the slot (≤ 1, the single slot), plus 1 iff a consumed
// frame's stages are still running (≤ 1) — 2 at most. A frame moves
// pending → in-flight → rendered exactly once; a dropped frame is
// overwritten before it is ever consumed.
//
// One documented EVENT race (the stats stay exact): the drop event
// fires at publish time from the producer's snapshot of consumedSeq_;
// if the consumer's consumedSeq_ store for the frame in the slot lands
// in the same instant, the event can name a frame the consumer in fact
// renders. The event count can then exceed framesDropped by at most
// one per racing window — stats() (not the events) is the
// authoritative count the Profiler's frame-drop field reads
// (M2-SPRITE-04).
//
// ---------------------------------------------------------------------------
// The vsync-paced frame clock (M1-LOOP-02's render_time)
// ---------------------------------------------------------------------------
//
// The clock is a frame deadline grid on the engine's monotonic steady
// time base — the same epoch the GameLoop and the M1 headless engine
// use (the M1-LOOP-01 clock base):
//
//     deadlineNs(N) = referenceNs + N × periodNs     (N 1-based)
//     periodNs     = 10⁹ / frameRateHz              (one division at
//                                                     construction; the
//                                                     truncation shifts
//                                                     the grid by < 1 ns
//                                                     per frame — the
//                                                     render_time is the
//                                                     real clock reading,
//                                                     so no measurement
//                                                     error)
//
// windowed: the caller sets frameRateHz to the display's refresh rate
// (GlContext::refreshRateHz — 0 when unavailable, in which case the
// target rate stands in) so the deadline grid coincides with the
// display refresh period; the window's swap (vsync on, GLFW's default)
// lands each frame on a refresh boundary. The engine's windowed run
// paces its frame loop with waitFrame(N) and hands the returned frame
// time to the presentation/interpolation path
// (PresentationSnapshot::onRenderFrame, M1-LOOP-02) — replacing the
// M1 headless monotonic clock read.
//
// headless: frameRateHz is the target pace (the CI path — no display,
// no vsync; the same deadline grid and the same render_time contract).
//
// The render_time is a wall-clock fact (ARCH-009): non-deterministic by
// design, never part of replay state or the simulation state hash
// (the M1-LOOP-02 alpha contract).
//
// ---------------------------------------------------------------------------
// Ordered shutdown (CONC-006)
// ---------------------------------------------------------------------------
//
// RenderThread::shutdown() is the engine-shutdown hook: it requests
// the stop, JOINS the render thread, and marks the object stopped —
// ordered (the thread never outlives the call), idempotent (a second
// call is a no-op), and safe on a stopped object. It does NOT flush a
// pending frame — the owner calls waitIdle() first when the last frame
// must render (the M1-HEAD-01 ordered-shutdown precedent: an explicit
// sequence, never a hidden drain). The destructor shuts down, so a
// render thread is never left detached (CONC-005: no detached
// threads).
//
// ---------------------------------------------------------------------------
// Ownership / lifetime
// ---------------------------------------------------------------------------
//
//   - RenderThread is move-only; the move stops the source (joins the
//     running thread when one is live) and the moved-to object is
//     STOPPED — the GlContext/Engine moved-out precedent. Construct it
//     in place (the constructor spawns the thread immediately).
//   - The constructor spawns the consumer thread IN THE BODY, after
//     every state member is initialized (`thread_` is declared LAST —
//     member initializers run in declaration order, so spawning from a
//     member initializer would let the consumer read members before
//     their initializers ran: a ThreadSanitizer data race). The
//     thread-start edge then publishes the fully-initialized state
//     ([intro.multithread]). A thread-spawn failure is a platform
//     error: with exceptions disabled (NFR-8.10) the std::thread throw
//     cannot be caught — the process terminates (a documented
//     platform boundary, CORE-008: the failure is never silent).
//   - submitFrame/waitIdle/shutdown are OWNER-THREAD (main/sim thread)
//     calls — the one producer of the handoff; the render thread only
//     runs the stage callbacks.
//   - The stage callbacks run ON the render thread, once per rendered
//     frame, in batchStage → submitStage order; they MUST be bounded
//     and non-blocking (API-005: a stage that blocks, waitIdle and the
//     shutdown join block too — the owner's responsibility).
//   - Options::onStart runs ONCE on the render thread before the first
//     frame (the GlContext::makeCurrent takeover hook — the render
//     thread must own the context's current thread before any GL
//     work); bounded and non-blocking.
//   - FrameDescriptor::frameData is a NON-owning per-frame payload
//     pointer (e.g. the engine's PresentationSnapshot view); the
//     producer owns it and it MUST outlive the frame's render —
//     waitIdle() before release (one producer, one consumer, one
//     pending frame: the handoff carries no reference count).
//   - frameIndex is the producer's 1-based frame counter; the handoff
//     does not validate it (seq_ is the handoff's own ordering) — the
//     engine's producer uses its frame counter, which then equals the
//     seq/2 ordering the stats account on.
//   - FrameClock is move-only; a stopped (default/moved-from) clock is
//     a no-op: every operation returns 0 (the GlContext stopped-state
//     precedent — the clock has no failure to log).
//
// ---------------------------------------------------------------------------
// Performance (PERF-003/002, DOC-004)
// ---------------------------------------------------------------------------
//
// Hot path (submitFrame): a few atomic loads + one plain 32-byte copy +
// one release store — no allocation, no lock, no virtual dispatch, no
// std::function (PERF-006), no logging on the healthy path (the drop
// path is cold: one rate-limited Warn). The consumer's between-frame
// wait is a yield spin (no busy-burn: the OS reschedules during the
// ~16 ms vsync gap). FrameClock: deadlineNs is O(1) integer math;
// waitFrame is one bounded sleep_until (no spin).
//
// RenderThreadStats::framesDropped is the source of the Profiler's
// frame-drop field (M2-SPRITE-04, docs/api/profiler.md).
//
// Misuse warnings:
//   - never publish from a thread other than the owner (a second
//     producer races the seqlock) — the sim thread is the producer;
//   - never release per-frame data the handoff still carries: waitIdle()
//     + shutdown() first;
//   - a stopped (destroyed/moved-from) RenderThread rejects
//     submitFrame with InvalidArgument (no log — the stopped-state
//     precedent);
//   - dropped frames run NO stage (the drop is the scope's
//     backpressure, not a silent skip — it is logged).

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

#include "laige/result.h"

namespace laige::render {

// One presentation frame's handed-off data (PRD §10.2: the frame
// descriptor of the lock-free handoff). A 32-byte POD — one plain copy
// per publish (PERF-005: no pointer graph, no allocation).
struct FrameDescriptor {
  // The producer's 1-based frame counter (see the header ownership
  // section).
  std::uint64_t frameIndex{};
  // The last completed sim tick at publish time (0 before the first
  // tick — the GameLoop's currentTick() value).
  std::uint64_t simTick{};
  // The frame's presentation time (ns on the engine's monotonic steady
  // time base) — the presentation/interpolation render_time
  // (M1-LOOP-02).
  std::int64_t renderTimeNs{};
  // Opaque per-frame payload (NON-owning; the producer owns it and it
  // must outlive the frame's render — the header ownership section).
  void* frameData{};
};

// A frame pipeline stage callback: the cull/batch stage (before
// submit) or the submit stage. Runs ON the render thread, once per
// rendered frame, with the frame's descriptor. MUST be bounded and
// non-blocking (API-005). nullptr = the stage is skipped (a no-op).
using StageFn = void (*)(void* context,
                         const FrameDescriptor& frame) noexcept;

// The render thread's configuration (API-006: an option structure, not
// positional booleans).
struct RenderThreadOptions {
  // The cull/batch stage (runs before submitStage); nullptr = no-op.
  StageFn batchStage{nullptr};
  // The submit stage (the frame's draw work); nullptr = no-op.
  StageFn submitStage{nullptr};
  // The stage callbacks' user context (non-owning; the owner's
  // responsibility — e.g. the batcher's state).
  void* stageContext{nullptr};
  // The render-thread start hook: runs ONCE on the render thread
  // before the first frame (the GlContext::makeCurrent takeover);
  // nullptr = none. Bounded, non-blocking (API-005).
  using StartFn = void (*)(void* context) noexcept;
  StartFn onStart{nullptr};
  // onStart's context (non-owning); nullptr when onStart is null.
  void* onStartContext{nullptr};
};

// The since-construction counters (the cold snapshot; the authoritative
// source of the Profiler's frame-drop field, M2-SPRITE-04).
struct RenderThreadStats {
  // Every publish (including dropped frames).
  std::uint64_t framesSubmitted{};
  // Frames whose pipeline stages have completed (a frame is counted
  // when its submit stage returns; after waitIdle or shutdown, every
  // published frame is accounted — rendered, dropped, or outstanding).
  std::uint64_t framesRendered{};
  // Older frames dropped by the backpressure (PERF-008) — the exact
  // count: framesSubmitted - framesRendered - outstanding (the header
  // preamble's invariant).
  std::uint64_t framesDropped{};
};

// The render thread + the single-slot lock-free frame handoff
// (PRD §10.2; the synchronization argument is in the header preamble).
// The constructor spawns the consumer thread after all state members
// are initialized; shutdown() joins it. Move-only.
class RenderThread {
 public:
  // Spawns the consumer thread (in the body, after every state member
  // is initialized — the ownership section's order argument) and runs
  // Options::onStart on it (the GL takeover hook). A thread-spawn
  // failure terminates the process (exceptions disabled — a documented
  // platform boundary, CORE-008). One Info lifecycle event
  // (render_thread/thread_started).
  explicit RenderThread(RenderThreadOptions options) noexcept;
  ~RenderThread();  // shuts down (joins the thread; CONC-006)

  // The move stops the source (joins a live thread) and the moved-to
  // object is STOPPED (the GlContext precedent) — construct in place.
  RenderThread(RenderThread&&) noexcept;
  RenderThread& operator=(RenderThread&&) noexcept;
  RenderThread(const RenderThread&) = delete;
  RenderThread& operator=(const RenderThread&) = delete;

  // True while the thread is running (false once stopped/moved-from).
  [[nodiscard]] bool running() const noexcept;

  // The owner-thread (main/sim) publish of one frame — the HOT PATH:
  // O(1) atomics + one 32-byte copy, no allocation, no lock, no log on
  // the healthy path. Single-slot backpressure: a pending frame (the
  // consumer more than one frame behind) is DROPPED in place — one
  // rate-limited render_thread/frame_dropped warn. Stopped →
  // InvalidArgument (no log — the stopped-state precedent).
  // @budget O(1); no allocation; one release store.
  [[nodiscard]] laige::Status submitFrame(const FrameDescriptor& frame) noexcept;

  // The owner-thread barrier: blocks until every published frame is
  // fully processed (no pending frame, no in-flight pipeline). Bounded
  // by the single slot (at most one frame's pipeline work) plus the
  // stage callbacks' bound (API-005). No-op on a stopped object.
  // @budget O(1) yield-spin; one bounded wait per frame.
  void waitIdle() noexcept;

  // Ordered idempotent shutdown (CONC-006): stop request + thread join +
  // stopped mark; a second call is a no-op; safe on a stopped object.
  // It does NOT flush a pending frame — call waitIdle() first when the
  // last frame must render. One Info lifecycle event
  // (render_thread/thread_stopped) per actual stop.
  void shutdown() noexcept;

  // The since-construction counters (cold: a few relaxed atomic reads).
  // Invariant (exact at every instant): framesSubmitted =
  // framesRendered + framesDropped + outstanding, where outstanding =
  // pending slot (≤ 1) + in-flight frame (≤ 1) — the header preamble's
  // accounting section.
  [[nodiscard]] RenderThreadStats stats() const noexcept;

 private:
  // The consumer loop (the render thread body — the header preamble's
  // seqlock argument).
  void runConsumer() noexcept;

  RenderThreadOptions options_;
  // The handoff state (the header preamble's synchronization argument).
  std::atomic<std::uint64_t> seq_{0};        // even = idle; odd = writing
  FrameDescriptor slot_{};                   // plain POD, seq_-published
  std::atomic<std::uint64_t> consumedSeq_{0};
  std::atomic<bool> inFlight_{false};
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> rendered_{0};   // stages completed (consumer)
  bool valid_{true};
  // Declared LAST, and started in the constructor BODY, never in a
  // member initializer: C++ initializes members in DECLARATION order,
  // so starting the thread while `thread_` was still mid-construction
  // let the consumer read the members above before their in-class
  // initializers ran (a ThreadSanitizer data race — Thread-start
  // establishes the happens-before edge only for what happened BEFORE
  // the start, [intro.multithread]). With `thread_` last, the thread
  // start publishes the fully-initialized state. The order is
  // load-bearing; do not reorder.
  std::thread thread_;
};

// The documented frame-rate range (CORE-005; API-006): 1–1000 Hz.
inline constexpr std::uint32_t kMinFrameRateHz = 1;
inline constexpr std::uint32_t kDefaultFrameRateHz = 60;
inline constexpr std::uint32_t kMaxFrameRateHz = 1000;

// The frame clock's configuration (API-006).
struct FrameClockOptions {
  // The pace of the frame deadline grid (Hz), in
  // [kMinFrameRateHz, kMaxFrameRateHz]. Windowed: the display's
  // refresh rate (GlContext::refreshRateHz, 0 → the target rate);
  // headless: the target pace.
  std::uint32_t frameRateHz{kDefaultFrameRateHz};
};

// The vsync-paced frame clock (the frame deadline grid + the
// render_time provider — the header preamble's clock section).
// Move-only; a stopped (default/moved-from) clock is a no-op (every
// operation returns 0).
class FrameClock {
 public:
  // The stopped state (the failed-create and moved-from form).
  FrameClock() noexcept;
  ~FrameClock() = default;
  FrameClock(FrameClock&&) noexcept;
  FrameClock& operator=(FrameClock&&) noexcept;
  FrameClock(const FrameClock&) = delete;
  FrameClock& operator=(const FrameClock&) = delete;

  // Validates the frame rate (outside [kMinFrameRateHz,
  // kMaxFrameRateHz] → InvalidArgument + one rate-limited
  // render_thread/frame_rate_invalid warn) and records the reference
  // (one steady_clock read — setup path).
  [[nodiscard]] static laige::Result<FrameClock, laige::ErrorCode>
  create(FrameClockOptions options) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint32_t frameRateHz() const noexcept;
  [[nodiscard]] std::uint64_t periodNs() const noexcept;
  // The grid origin (the steady_clock reading of create()).
  [[nodiscard]] std::int64_t referenceNs() const noexcept;
  // The clock's time base (the engine's monotonic steady ns — the
  // M1 clock epoch). O(1).
  [[nodiscard]] std::int64_t nowNs() const noexcept;
  // Frame N's presentation deadline (N 1-based; exact integer;
  // clamped to INT64_MAX beyond the 2^63 ns frame-time bound — the
  // GameLoop clock bound). O(1).
  [[nodiscard]] std::int64_t deadlineNs(std::uint64_t frameIndex) const noexcept;
  // Paces the owner thread: blocks until frame N's deadline (one
  // bounded sleep), then returns the frame's ACTUAL presentation time
  // (the render_time for the presentation/interpolation path — on
  // time the deadline, late the real clock reading; ARCH-009
  // wall-clock fact).
  // @budget O(1) + one bounded sleep_until.
  [[nodiscard]] std::int64_t waitFrame(std::uint64_t frameIndex) noexcept;

 private:
  std::uint32_t rate_{0};
  std::uint64_t periodNs_{0};
  std::int64_t referenceNs_{0};
  bool valid_{false};
};

}  // namespace laige::render
