# Frame pipeline (`laige::render::RenderThread`, `laige::render::FrameClock`)

The render thread, the lock-free frame handoff, and the vsync-paced
frame clock (M2-GL-02; PRD §10.2, §11; AGENTS ARCH-002/ARCH-009,
CONC-002/CONC-005/CONC-006, PERF-008). Public header:
`src/laige-render/include/laige/render/frame_pipeline.h`;
implementation: `src/laige-render/frame_pipeline.cpp`. Unit suites:
`ctest -R render_thread` (`tests/laige-render/render_thread_tests.cpp`)
— `FrameClock` and `RenderThreadHandoff` run without any GL
environment; `RenderThreadOffscreen` needs a usable OpenGL 3.3
environment (always present on the P0 CI runners, the
`GlContextSmoke` convention). On success the offscreen runs print
one machine-greppable line each in the ctest output (the
`docs/testing.md` machine-line convention):

```text
render-thread: kind=<lowrate|3000|slowed> frames=<n> rendered=<n> dropped=<n> pixel=ok status=ok
```

Rendering runs on its own thread — the frame pipeline (cull/batch →
submit) — with the frame descriptor handed off from the main/sim
thread. This step lands the handoff, the thread, the clock, the
backpressure, and the shutdown; the pipeline's actual stages — the
sprite batcher's cull/batch (M2-SPRITE-01) and the GPU submit
(M2-SPRITE-02) — plug in as the plain function callbacks below. Nothing
in this module depends on GL in the handoff itself (the `GlContext`
render-thread takeover is the `onStart` hook — release-then-bind per
`docs/api/gl_context.md`; the window swap/present lands with the
engine's windowed wiring, M2-SPRITE-02 onward).

## The frame handoff (PRD §10.2, CONC-002)

One producer (the main/sim thread — the engine's frame loop) and one
consumer (the render thread). The handoff is a **single slot** holding
one `FrameDescriptor` plus six atomics — no mutex, no condition
variable, no queue:

- `FrameDescriptor` — a 32-byte POD: `frameIndex` (the producer's
  1-based frame counter), `simTick` (the last completed sim tick at
  publish time), `renderTimeNs` (the frame's presentation time on the
  engine's monotonic steady time base — the
  presentation/interpolation `render_time`, M1-LOOP-02), and
  `frameData` (an opaque non-owning per-frame payload pointer — the
  sim state read arrives with the sprite stages, M2-SPRITE-02; the
  handoff itself never touches sim types, so no render→sim module edge
  is needed yet).
- `StageFn` — a plain `noexcept` function-pointer callback (no
  `std::function`, no virtual dispatch — PERF-006). `batchStage` runs
  before `submitStage`, once per rendered frame, **on the render
  thread**; `nullptr` skips the stage.
- `RenderThreadOptions` — the stages, their context, and the
  `onStart`/`onStop` hooks.

The full synchronization argument (the atomic-slot handoff — memory
orders, the ordering re-check, the no-ABA case, the 2^64 wrap bound) is
in the header preamble; the short form: the producer publishes with
`slot_.store(frame, release)` then `seq_.store(s + 1, release)`; the
consumer acquire-loads `seq_`, loads the atomic slot (a complete copy,
never torn), and re-checks `seq_` — an unchanged value means the copy
is a complete, current frame, otherwise a newer publication crossed
the copy window and the copy is retried. The ATOMIC slot (not a
plain-memory sequence lock) is what makes the handoff free of data
races under the C++ memory model — a plain seqlock's concurrent
plain read/write of the slot is the exact access pair the P0 CI TSan
lane reports (CONC-007). The diagnostic atomics (`consumedSeq_`,
`inFlight_`, `stop_`, `submitted_`, `rendered_`) carry no correctness —
`seq_` and `slot_` alone do; they exist for the accounting, `waitIdle`,
and the ordered shutdown (CONC-002: partitioned ownership and an
immutable snapshot, no shared locks).

**Backpressure (PERF-008, "never queue unboundedly").** The single slot
means the consumer can lag by **at most one frame**. When the producer
publishes while a frame is still pending (`seq_ > consumedSeq_`), the
**older frame is dropped in place** — replaced in the slot — and one
rate-limited Warn event is emitted (LOG-004: the facade's per-second
window suppresses repeats):

```text
render_thread/frame_dropped    fields: dropped_frame, new_frame
```

The drop runs **no stage** (it is the scope's backpressure, not a
silent skip — it is logged, FR-12.3). One documented event race: the
drop event fires at publish time from the producer's snapshot of
`consumedSeq_`, so it can name a frame the consumer finishes consuming
in the same instant — the **event** count can exceed `framesDropped` by
at most one per racing window; `stats()` is the authoritative count.

**The exact accounting invariant.** Every published frame is in exactly
one bucket at every instant:

```text
framesSubmitted = framesRendered + framesDropped + outstanding
```

where `outstanding` = the pending slot (≤ 1) + the in-flight frame (≤
1, a consumed frame whose stages are still running) — 2 at most. A
frame moves pending → in-flight → rendered exactly once; a dropped
frame is overwritten before it is ever consumed. `rendered` is a real
counter (incremented when a frame's stages complete), not a seq
difference: a drop overwrites the slot without the consumer ever
consuming the older frame, so `seq/2` would count the frame *number*
of the last consumed frame, not the number of frames rendered.
`RenderThreadStats::framesDropped` is the source of the Profiler's
frame-drop field (M2-SPRITE-04, `docs/api/profiler.md`).

## The API

| Operation | Behavior | Complexity / allocation |
|---|---|---|
| `RenderThread(options)` | Spawns the consumer thread **in the constructor body, after every state member is initialized** (`thread_` is declared last — see Threading and phase), runs `options.onStart` on it (the `GlContext::makeCurrent` takeover hook) and, at shutdown, `options.onStop` on it **after the last frame and before the thread exits** (the `GlContext::release` hand-back hook — the P0 EGL stack cannot rebind a context last held by a dead thread). One Info event, `render_thread/thread_started`. A thread-spawn failure terminates the process (exceptions disabled, NFR-8.10 — a documented platform boundary, CORE-008: the failure is never silent) | one-time setup: one thread + one log line |
| `submitFrame(frame)` | The owner-thread (main/sim) publish — the **hot path**: a few atomic loads + one plain 32-byte copy + one release store, no allocation, no lock, no log on the healthy path. Single-slot backpressure: a pending frame (the consumer more than one frame behind) is **dropped in place** — one rate-limited `render_thread/frame_dropped` warn. Stopped → `InvalidArgument` (no log — the stopped-state precedent) | O(1); no allocation; one release store |
| `waitIdle()` | The owner-thread barrier: blocks until every published frame is fully processed (no pending frame, no in-flight pipeline). Bounded by the single slot plus the stage callbacks' bound (API-005). No-op on a stopped object | O(1) yield-spin; one bounded wait per frame |
| `shutdown()` | Ordered idempotent shutdown (CONC-006): stop request + **join** + stopped mark; a second call is a no-op; safe on a stopped object. It does **not** flush a pending frame — the owner calls `waitIdle()` first when the last frame must render (the M1-HEAD-01 ordered-shutdown precedent). One Info event per actual stop, `render_thread/thread_stopped` | O(1) + the join (bounded by one frame's pipeline work) |
| `stats()` | The since-construction counters (`RenderThreadStats`); the invariant above holds at every instant | a few relaxed atomic reads; no allocation |
| `running()` | True while the thread is running (false once stopped/moved-from) | O(1) |
| `FrameClock::create(options)` | Validates the frame rate (outside `[kMinFrameRateHz, kMaxFrameRateHz]` → `InvalidArgument` + one rate-limited `render_thread/frame_rate_invalid` warn) and records the reference (one `steady_clock` read). Returns `Result<FrameClock, ErrorCode>` | O(1), one division, no allocation |
| `FrameClock::deadlineNs(n)` | Frame `n`'s presentation deadline (1-based; exact integer grid `referenceNs + n × periodNs`; clamped to `INT64_MAX` beyond the 2^63 ns frame-time bound — never UB, CPP-004) | O(1) integer math |
| `FrameClock::waitFrame(n)` | Paces the owner thread: blocks until frame `n`'s deadline (one bounded `sleep_until`, no spin), then returns the frame's **actual** presentation time — on time the deadline, late the real clock reading (ARCH-009 wall-clock fact; the M1-LOOP-02 alpha contract clamps a late frame to 1.0). Stopped clock → 0 | O(1) + one bounded sleep |
| `FrameClock` accessors | `valid()`, `frameRateHz()`, `periodNs()`, `referenceNs()`, `nowNs()` — a stopped (default/moved-from) clock returns 0 for all | O(1) |

**Move semantics.** `RenderThread` is move-only; the move **stops the
source** (joins a live thread) and the moved-to object is **stopped**
(the `GlContext` moved-out precedent) — construct it in place.
`FrameClock` is move-only; a moved-from clock is stopped (every
operation returns 0 — the clock has no failure to log).

**Ownership and lifetime.** `submitFrame`/`waitIdle`/`shutdown` are
owner-thread calls (the one producer); the render thread only runs the
stage callbacks. `FrameDescriptor::frameData` is a **non-owning**
per-frame payload pointer (e.g. the engine's `PresentationSnapshot`
view): the producer owns it and it must outlive the frame's render —
`waitIdle()` before release (one producer, one consumer, one pending
frame: the handoff carries no reference count). The stage callbacks and
`onStart`/`onStop` run **on the render thread**: the stages once per
rendered frame, `onStart` once before the first frame, `onStop` once
after the last frame and before the thread exits (the GL context's
release hand-back — the P0 EGL stack cannot rebind a context last
held by a dead thread); they must be bounded and non-blocking
(API-005: a stage or hook that blocks, `waitIdle` and the shutdown
join block too).
`frameIndex` is the producer's 1-based frame counter; the handoff does
not validate it (`seq_` is the handoff's own ordering).

**Threading and phase.** The render thread is spawned by the
constructor and joined by `shutdown()` (or the destructor) — never
detached (CONC-005). The engine's shutdown calls `waitIdle()` then
`shutdown()` when the last frame must render, or `shutdown()` alone
when it does not (CONC-006: ordered, testable, idempotent). No engine
locks anywhere in the module (CONC-002); the only thread-join is the
shutdown join.

**The construction order is load-bearing.** C++ initializes members in
declaration order; `thread_` is declared **last** and is started in
the constructor **body** — never in a member initializer. The
thread-start synchronization edge ([intro.multithread]) publishes only
what happened *before* the start, so spawning from a member
initializer would let the consumer read the state members before
their in-class initializers ran (a data race caught by
ThreadSanitizer in CI). With the thread started last, the consumer
never observes a partially-initialized object.

**Failure behavior (NFR-008 / CORE-008).** The handoff has no runtime
failure to report: `submitFrame` on a stopped object returns
`InvalidArgument` (a precondition query, no log); the only logged
events are the two lifecycle Info events, the rate-limited `Warn`
backpressure drop, and the rate-limited `Warn` invalid clock rate.
`FrameClock::create` returns `InvalidArgument` with one rate-limited
Warn (never silent). A thread-spawn failure terminates the process
(see the API table) — with exceptions disabled there is no recoverable
error path (documented platform boundary).

## The frame clock: vsync pacing (M1-LOOP-02's render_time)

The clock is a **frame deadline grid** on the engine's monotonic
steady time base — the same epoch the `GameLoop` and the M1 headless
engine use (the M1-LOOP-01 clock base):

```text
deadlineNs(N) = referenceNs + N × periodNs     (N 1-based; exact integer)
periodNs      = 10⁹ / frameRateHz              (one division at create)
```

- **Windowed:** the caller sets `frameRateHz` to the display's refresh
  rate (`GlContext::refreshRateHz` — 0 when unavailable, in which case
  the target rate stands in) so the deadline grid coincides with the
  display refresh period; the window's swap (vsync on, GLFW's
  default) lands each frame on a refresh boundary. This **replaces
  the M1 headless monotonic clock read** (M1-LOOP-01) as the frame
  pacer: the engine's frame loop calls `waitFrame(N)` and hands the
  returned frame time to the presentation/interpolation path
  (`PresentationSnapshot::onRenderFrame`, M1-LOOP-02).
- **Headless:** `frameRateHz` is the target pace (the CI path — no
  display, no vsync; the same deadline grid, the same `render_time`
  contract).
- The `render_time` is a **wall-clock fact** (ARCH-009):
  non-deterministic by design, never part of replay state or the
  simulation state hash.

The documented frame-rate range is `[kMinFrameRateHz, kMaxFrameRateHz]`
= 1–1000 Hz (`kDefaultFrameRateHz` = 60).

## Performance (PERF-003/002, DOC-004)

**Hot path (`submitFrame`).** A few atomic loads + one 32-byte atomic
release store (the single producer makes the store contend-free — one
CAS attempt on every P0 compiler, no lock) + one release store — no
allocation, no lock, no virtual dispatch, no `std::function`
(PERF-006), no logging on the healthy path (the drop path is cold: one
rate-limited Warn). The consumer's
between-frame wait is a yield spin (no busy-burn: the OS reschedules
during the ~16 ms vsync gap). `FrameClock`: `deadlineNs` is O(1)
integer math; `waitFrame` is one bounded `sleep_until` (no spin — the
producer's wait is cadence, not work, PERF-002).

**No hot-path logging, no hot-path allocation.** `stats()` is a few
relaxed atomic reads (cold); the stage callbacks are the frame's real
work (M2-SPRITE-02 budget, not this step's).

**Misuse warnings.**

- Never publish from a thread other than the owner (a second producer
  races the handoff's single-slot protocol — `seq_`/`slot_` are
  producer-owned) — the sim thread is the producer.
- Never release per-frame data the handoff still carries: `waitIdle()`
  + `shutdown()` first.
- A stopped (destroyed/moved-from) `RenderThread` rejects `submitFrame`
  with `InvalidArgument` (no log — the stopped-state precedent).
- A stage that blocks blocks `waitIdle` and the shutdown join (API-005)
  — the stage callbacks' bound is the caller's responsibility.
- `waitFrame` is the **owner-thread** call: calling it from the render
  thread paces nothing (the render thread runs the stages, it does not
  publish).

## Performant example

```cpp
// Setup (once, e.g. in the render module's start — the M2-GL-02
// windowed path; the headless CI path uses a target rate):
laige::render::GlContext gl = /* createWindowed / createHeadless */;
laige::render::FrameClockOptions co;
co.frameRateHz = gl.refreshRateHz();  // 0 → the target rate stands in
laige::render::FrameClock clock =
    std::move(laige::render::FrameClock::create(co)).takeValue();

// Hand off the GL context (release-then-bind — docs/api/gl_context.md):
// the P0 EGL stack rejects a takeover while the context is still
// current on another live thread, so the old owner releases first.
gl.release().ok();                     // main/sim thread: no longer current

laige::render::RenderThreadOptions opts;
opts.batchStage = &cullBatch;          // M2-SPRITE-01 (render thread)
opts.submitStage = &gpuSubmit;         // M2-SPRITE-02 (render thread)
opts.stageContext = &batcher;
opts.onStart = &takeover;              // render thread: gl.makeCurrent()
opts.onStartContext = &gl;
opts.onStop = &handBack;               // render thread: gl.release()
opts.onStopContext = &gl;              // after the last frame, before the
                                       // thread exits (a dead thread's
                                       // context cannot be rebound on the
                                       // P0 EGL stack)
laige::render::RenderThread thread(opts);

// Per frame (the main/sim thread — the one producer):
const std::int64_t renderTime = clock.waitFrame(frameIndex);
laige::render::FrameDescriptor d;
d.frameIndex = frameIndex;
d.simTick = loop.currentTick();
d.renderTimeNs = renderTime;           // → PresentationSnapshot::onRenderFrame
d.frameData = &snapshot;               // non-owning; waitIdle before release
thread.submitFrame(d);                 // O(1); a late producer drops the
                                       // older frame (logged), never queues

// Engine shutdown (ordered, CONC-006):
thread.waitIdle();                     // the last frame renders
thread.shutdown();                     // joins the render thread
```
