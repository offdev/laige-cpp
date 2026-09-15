// laige-sim fixed-timestep game loop core (M1-LOOP-01; FR-1.1,
// ARCH-002, PRD §10.2).
//
// Implementation of the GameLoop members declared in
// include/laige/sim/game_loop.h — see that header (the member docs,
// the accumulator contract, the overload behavior, the beginFrame
// wiring, the failure and determinism scopes) and
// docs/api/game_loop.md for the full API contract.
//
// Hot-path cost (per frame): one clock read, a few integer ops (the
// exact due computation, the bounded run loop), and up to
// maxCatchUpTicks runSystems dispatches — no allocation and no
// logging on the success path (PERF-003, LOG-003). The tick_dropped
// warn is cold (an overload episode).

#include "laige/sim/game_loop.h"  // the GameLoop contract (this header)

#include <cassert>
#include <chrono>
#include <cstdint>
#include <utility>

#include "laige/logging.h"

namespace laige {

namespace {

// Nanoseconds per second (the clock time base; CORE-005 named
// constant).
inline constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;

// The stable subsystem name for game-loop events (LOG-001).
inline constexpr const char* kLoopSubsystem = "loop";

// The headless clock source (M1-LOOP-01): the monotonic steady_clock
// as nanoseconds since its epoch (the windowed clock arrives with
// M2-GL-02; the LoggerOptions::clock injection precedent supplies the
// test seam — Options::nowNs).
std::int64_t steadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// NFR-13.3 5-field grammar, identical in every build ({code} |
// {what} | {why} | {fix} | {doc_anchor}): the machine-parseable
// message stays build-stable; the dynamic values are structured
// fields, never message text (the system_timing.cpp precedent).
inline constexpr const char* kTickDroppedMessage =
    "tick_dropped | the frame's due-tick demand exceeded the max "
    "catch-up tick budget | the frame took longer than max_catch_up "
    "ticks of simulation time (sim work too heavy, a long frame, or a "
    "spiking clock source) | reduce per-tick work, lower the tick "
    "rate, or raise the max catch-up ticks through typed "
    "configuration | docs/api/game_loop.md";

inline constexpr const char* kTickRateInvalidMessage =
    "tick_rate_invalid | the configured tick rate is outside the "
    "supported 20-120 Hz range | the tick rate is validated at loop "
    "construction | pass a rate in the 20-120 Hz range (the default "
    "is 60) | docs/api/game_loop.md";

inline constexpr const char* kCatchUpInvalidMessage =
    "catchup_invalid | the configured max catch-up tick count is not "
    "positive | a zero limit would never run a tick | pass a max "
    "catch-up tick count of 1 or more (the default is 5) | "
    "docs/api/game_loop.md";

// The number of complete ticks within elapsedNs at tickRateHz —
// floor(elapsedNs × rate / 10⁹) in EXACT integer arithmetic: the
// seconds part (elapsedNs / 10⁹) times the rate plus the sub-seconds
// remainder part (the remainder × rate < 10⁹ × 120, so no
// intermediate product overflows; the split is an identity — floor
// distributes over the seconds/remainder decomposition). No
// floating point (ARCH-010: the tick count is replay state).
std::uint64_t ticksDue(std::int64_t elapsedNs, std::uint32_t tickRateHz) {
  const std::int64_t seconds = elapsedNs / kNanosecondsPerSecond;
  const std::int64_t remainder = elapsedNs % kNanosecondsPerSecond;
  return static_cast<std::uint64_t>(seconds) * tickRateHz +
         static_cast<std::uint64_t>(remainder) * tickRateHz /
             static_cast<std::uint64_t>(kNanosecondsPerSecond);
}

}  // namespace

Result<GameLoop, ErrorCode>
GameLoop::create(World& world, const SystemSchedule& schedule,
                 Options options) noexcept {
  // Configuration validation (normative order, first failure wins;
  // every failure one rate-limited structured warn — FR-12.3/CORE-008:
  // never silent).
  if (options.tickRateHz < kMinTickRateHz ||
      options.tickRateHz > kMaxTickRateHz) {
    LAIGE_LOG_WARN(kLoopSubsystem, "tick_rate_invalid",
                   kTickRateInvalidMessage,
                   laige::log::field("tick_rate_hz", options.tickRateHz));
    return ErrorCode::InvalidArgument;
  }
  if (options.maxCatchUpTicks == 0) {
    LAIGE_LOG_WARN(kLoopSubsystem, "catchup_invalid", kCatchUpInvalidMessage,
                   laige::log::field("max_catch_up",
                                     options.maxCatchUpTicks));
    return ErrorCode::InvalidArgument;
  }
  return GameLoop(world, schedule, std::move(options));
}

GameLoop::GameLoop(World& world, const SystemSchedule& schedule,
                   Options options) noexcept
    : world_(&world),
      schedule_(&schedule),
      options_(std::move(options)),
      valid_(true),
      started_(false),
      startNs_(0),
      frames_(0),
      ticks_(0),
      droppedTicks_(0),
      droppedFrames_(0) {}

GameLoop::GameLoop(GameLoop&& other) noexcept
    : world_(other.world_),
      schedule_(other.schedule_),
      options_(std::move(other.options_)),
      valid_(other.valid_),
      started_(other.started_),
      startNs_(other.startNs_),
      frames_(other.frames_),
      ticks_(other.ticks_),
      droppedTicks_(other.droppedTicks_),
      droppedFrames_(other.droppedFrames_) {
  other.valid_ = false;  // the source becomes a stopped loop
}

GameLoop& GameLoop::operator=(GameLoop&& other) noexcept {
  if (this != &other) {
    world_ = other.world_;
    schedule_ = other.schedule_;
    options_ = std::move(other.options_);
    valid_ = other.valid_;
    started_ = other.started_;
    startNs_ = other.startNs_;
    frames_ = other.frames_;
    ticks_ = other.ticks_;
    droppedTicks_ = other.droppedTicks_;
    droppedFrames_ = other.droppedFrames_;
    other.valid_ = false;  // the source becomes a stopped loop
  }
  return *this;
}

Status GameLoop::frame() noexcept {
  if (!valid_) {
    // A moved-from loop is stopped (preamble "Failure behavior"): a
    // pure-failure Status, no world access, no log (the
    // moved-from-world precedent, entity.h).
    return ErrorCode::InvalidArgument;
  }
  std::int64_t nowNs =
      (options_.nowNs != nullptr) ? options_.nowNs() : steadyNowNs();
  if (!started_) {
    // The first call establishes the start reference: zero ticks.
    started_ = true;
    startNs_ = nowNs;
  } else {
    // The clock source contract is MONOTONIC (steady_clock is by
    // definition; the synthetic test clocks advance, never rewind).
    // A backward reading is misuse: assert loudly in debug (S-9
    // style); in release, clamp to the start reference — the frame
    // contributes no time (degraded, never undefined behavior).
    assert(nowNs >= startNs_ &&
           "GameLoop clock source must be monotonic");
    if (nowNs < startNs_) nowNs = startNs_;
  }
  ++frames_;
  if (started_ && nowNs == startNs_) return Status{};  // no time elapsed
  const std::int64_t elapsedNs = nowNs - startNs_;
  const std::uint64_t due = ticksDue(elapsedNs, options_.tickRateHz);
  // Invariant: ticks_ <= due (a frame only adds min(want, cap) ticks,
  // and due is monotone in the clock) — the want below is >= 0.
  if (due <= ticks_) return Status{};
  const std::uint64_t want = due - ticks_;
  const std::uint64_t toRun =
      (want < options_.maxCatchUpTicks) ? want
                                        : options_.maxCatchUpTicks;
  for (std::uint64_t i = 0; i < toRun; ++i) {
    const Status s = runOneTick();
    if (!s.ok()) return s;  // failed tick: not counted, world untouched
  }
  if (want > options_.maxCatchUpTicks) {
    // The frame's due demand exceeded the max catch-up bound: the
    // unrun due ticks are dropped (preamble "Overload behavior") —
    // exactly want − maxCatchUpTicks of them, counted and logged
    // (rate-limited per (subsystem, event, severity), LOG-004).
    const std::uint64_t dropped = want - options_.maxCatchUpTicks;
    droppedTicks_ += dropped;
    ++droppedFrames_;
    LAIGE_LOG_WARN(kLoopSubsystem, "tick_dropped", kTickDroppedMessage,
                   laige::log::field("dropped", dropped),
                   laige::log::field("total_dropped", droppedTicks_),
                   laige::log::field("max_catch_up",
                                     options_.maxCatchUpTicks),
                   laige::log::field("tick_rate_hz", options_.tickRateHz));
  }
  return Status{};
}

Status GameLoop::runOneTick() noexcept {
  // One tick: the frame's beginFrame() (once per frame — the preamble
  // "beginFrame wiring") plus one system-phase dispatch. The tick
  // counts only when the system phase completed (preamble "Failure
  // behavior"); the onTick hook (M1-LOOP-02, preamble "Per-tick
  // presentation hook") fires only for completed ticks — a failed
  // tick's state never completed, so nothing observes it.
  world_->beginFrame();
  const Status s = world_->runSystems(*schedule_);
  if (s.ok()) {
    ++ticks_;
    if (options_.onTick != nullptr) {
      options_.onTick(options_.onTickContext, *world_, ticks_);
    }
  }
  return s;
}

std::uint64_t GameLoop::currentTick() const noexcept { return ticks_; }

std::int64_t GameLoop::startReferenceNs() const noexcept {
  return startNs_;  // 0 before the first frame established the base
}

std::uint32_t GameLoop::tickRateHz() const noexcept {
  return options_.tickRateHz;
}

std::uint32_t GameLoop::maxCatchUpTicks() const noexcept {
  return options_.maxCatchUpTicks;
}

GameLoopStats GameLoop::stats() const noexcept {
  return GameLoopStats{frames_, ticks_, droppedTicks_, droppedFrames_};
}

}  // namespace laige
