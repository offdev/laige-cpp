// laige-sim headless engine run (M1-HEAD-01; FR-1.6, ARCH-003,
// AC-6.2, CONC-006) + the opt-in replay recording (M1-DET-02).
//
// Implementation of the Engine declared in include/laige/sim/engine.h
// — see that header for the full contract (the lifecycle, the run
// contract, the ordered shutdown, the config surface (M1-CFG-01:
// EngineConfig + the versioned schema live in laige/sim/config.h),
// the determinism scope, the replay recording, the performance
// notes) and docs/api/engine.md for the API document and the
// laige-run CLI contract.
//
// Hot-path cost (per headless frame): one clock read, one bounded
// GameLoop::frame() dispatch, one snapshot onRenderFrame, one sleep —
// no allocation and no logging on the healthy path (PERF-003,
// LOG-003; the per-frame breakdown in engine.h "Performance").
// Replay recording (M1-DET-02, opt-in debug builds only) adds one
// bounded stdio write per completed tick when enabled, and one null
// check per completed tick when disabled. The always-on profiler
// (M1-PROF-01, enabled by default) adds two steady_clock reads + one
// O(1) ring write per frame and two clock reads + one ring write per
// completed tick (the GameLoop's runOneTick) — no allocation; the
// disabled state pays one branch each (the measured enabled cost is
// bounded at 1% of a 10k-entity tick — the m1-profiler-cost
// baseline).

#include "laige/sim/engine.h"  // the Engine contract (this header)

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#include "laige/budget_harness.h"  // TimeIt (the per-frame timing, M1-PROF-01)
#include "laige/logging.h"

namespace laige {

namespace {

// Nanoseconds per second (the clock time base; the
// game_loop.cpp constant).
inline constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;

// The stable subsystem names (LOG-001). kConfigSubsystem is defined in
// laige/sim/config.h (M1-CFG-01 owns the config surface).
inline constexpr const char* kEngineSubsystem = "engine";
inline constexpr const char* kReplaySubsystem = "replay";

// The headless clock source (M1-LOOP-01): the monotonic steady_clock
// as nanoseconds since its epoch (the windowed clock arrives with
// M2-GL-02).
std::int64_t steadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The config-surface rejection messages (the config/<key>_invalid
// events, the config/hot_reload_* events) live in laige/sim/config.h
// (M1-CFG-01 owns the config surface — one text for one rejection,
// LOG-002); Engine::create's tick-rate re-validation shares
// kConfigTickRateInvalidMessage from there.
//
// NFR-13.3 5-field grammar, identical in every build ({code} |
// {what} | {why} | {fix} | {doc_anchor}): the machine-parseable
// message stays build-stable; the dynamic values are structured
// fields, never message text (the system_timing.cpp precedent).
//
// M1-DET-02: the replay recording messages (NFR-13.3 5-field grammar;
// the dynamic values are structured fields, never message text).
#if defined(NDEBUG)
// The debug-only message lives only in release builds (the
// guardrails.cpp pattern: a debug-build-never-seen message must not
// trip -Wunused-const-variable in debug trees).
inline constexpr const char* kRecordDisabledMessage =
    "record_disabled | replay recording is a debug-build feature | "
    "this binary was built without debug support (NDEBUG defined — "
    "release build) | rebuild with CMAKE_BUILD_TYPE=Debug, or remove "
    "the --replay flag | docs/api/replay.md";
#endif

inline constexpr const char* kRecordAlreadyStartedMessage =
    "record_already_started | startReplayRecording was called twice | "
    "one engine records at most one replay per run | call "
    "startReplayRecording once, after all registration and before "
    "run_headless | docs/api/replay.md";

inline constexpr const char* kRecordStartFailedMessage =
    "record_start_failed | the replay recorder could not be started | "
    "the temporary file could not be created (disk full, bad path), or "
    "the size limit is below kMinReplaySizeLimit | check the path and "
    "disk space, or raise maxBytes | docs/api/replay.md";

inline constexpr const char* kRecordFailedMessage =
    "record_failed | replay recording failed and the run stopped | the "
    "total size limit was reached, or the log file could not be "
    "written | raise the size limit or free disk space; the run "
    "returned the failure Status and no partial log remains at the "
    "final path | docs/api/replay.md";

inline constexpr const char* kRecordAbortedMessage =
    "record_aborted | the replay recording ended without finalization "
    "| the run failed before the log was finalized (or the engine was "
    "shut down pre-run) | the temporary file was removed (no partial "
    "replay on disk); re-run the scenario with recording | "
    "docs/api/replay.md";

// M1-PROF-01: the profile report messages (NFR-13.3 5-field grammar;
// the dynamic values are structured fields, never message text).
inline constexpr const char* kReportAlreadyStartedMessage =
    "report_already_started | startProfileReport was called twice | "
    "one engine writes at most one profile report per run | call "
    "startProfileReport once, after all registration and before "
    "run_headless | docs/api/engine.md";

inline constexpr const char* kReportPathInvalidMessage =
    "report_path_invalid | the requested profile report path is empty "
    "| the report needs a file path to write to at the end of the run "
    "| pass a non-empty path (laige-run --prof-out <path>) | "
    "docs/api/engine.md";

inline constexpr const char* kReportWriteFailedMessage =
    "report_write_failed | the per-run profile report could not be "
    "written to its final path | the file could not be created or "
    "fully written (disk full, bad path, read-only filesystem) | "
    "check the path and disk space and re-run; the run itself "
    "completed (the report is diagnostics and never gates the "
    "simulation) | docs/api/engine.md";

inline constexpr const char* kReportAbortedMessage =
    "report_aborted | the profile report ended without finalization | "
    "the engine was shut down before the run that should have written "
    "it (or the run never started) | no report was written (diagnostics "
    "only — nothing to clean up); re-run the scenario with "
    "startProfileReport | docs/api/engine.md";

}  // namespace

// ---------------------------------------------------------------------------
// Engine setup
// ---------------------------------------------------------------------------

Engine::Engine() = default;

Result<Engine, ErrorCode> Engine::create(const EngineConfig& config) noexcept {
  // Validate the typed config first (API-008: reject before allocating;
  // the GameLoop re-validates the same range at loop construction —
  // the single warn here, subsystem "config", is the config-surface
  // rejection).
  if (config.tickRateHz < kMinTickRateHz || config.tickRateHz > kMaxTickRateHz) {
    // The config surface's rejection (M1-CFG-01: the message text
    // lives in config.h with the rest of the config/<key>_invalid
    // messages — one text for one rejection, LOG-002).
    LAIGE_LOG_WARN(kConfigSubsystem, "tick_rate_invalid",
                   kConfigTickRateInvalidMessage,
                   laige::log::field("tick_rate_hz", config.tickRateHz));
    return ErrorCode::InvalidArgument;
  }
  World::Options worldOptions;
  worldOptions.capacity = config.entityCapacity;
  worldOptions.churnPerFrameBudget = config.churnPerFrameBudget;
  // M1-DET-01 (determinism mode): the master seed and the mode flag
  // (the per-system PRNG substreams derive from them at registration
  // — registerSystem; the seed is part of the replay identity,
  // ADR 0002).
  worldOptions.seed = config.seed;
  worldOptions.deterministic = config.determinism.enabled;
  Result<World, ErrorCode> worldResult = World::create(worldOptions);
  if (worldResult.isError()) {
    // The World's validation (capacity > 65536 -> InvalidArgument, no
    // warn there — the World::create precedent).
    return worldResult.error();
  }
  Engine engine;
  engine.world_ = std::make_unique<World>(std::move(worldResult).takeValue());
  // M1-PROF-01: the always-on profiler (one object + its two fixed
  // window storages — the engine's setup, not the run's: the run's
  // own setup allocation count is unchanged, engine.h "Performance").
  engine.profiler_ = std::make_unique<Profiler>(Profiler::Options{});
  // The engine's built-ins always register FIRST (stable
  // registration order for the deterministic ComponentTypeIds,
  // ARCH-010; the game's components follow through world()). M1-DET-01:
  // the ONE built-in matching the configured SimMath backend is
  // registered (ADR 0002, factory-selected at init); the game registers
  // the matching Position2D alias for its own systems — registering
  // the other alias is a duplicate-component rejection (one alias per
  // world, the component.h contract).
  const Result<ComponentTypeId, ErrorCode> builtin =
      config.determinism.math == SimMathBackend::FloatPinned32
          ? engine.world_->registerComponent<Position2DFp32>()
          : engine.world_->registerComponent<Position2DFpx16>();
  if (builtin.isError()) {
    // Unreachable on a fresh world (the type is registered once per
    // world); propagated anyway — never silent (CORE-008).
    return builtin.error();
  }
  engine.config_ = config;
  return engine;
}

// ---------------------------------------------------------------------------
// The presentation onTick wiring (M1-LOOP-02)
// ---------------------------------------------------------------------------

void Engine::onTickHook(void* context, World& world,
                        std::uint64_t tick) noexcept {
  (void)world;  // the snapshot owns its own non-owning world view
  static_cast<Engine*>(context)->onTickHookDispatch(tick);
}

void Engine::onTickHookDispatch(std::uint64_t tick) noexcept {
  // The first loop frame runs zero ticks (game_loop.h), so the
  // snapshot exists by the time this hook can fire; a missing
  // snapshot is still a no-op, never a crash (the handle's
  // hasSnapshot() guard — the never-crash contract, CORE-008).
  if (snapshot_.hasSnapshot()) snapshot_.onTick(snapshot_.context, tick);
  // M1-DET-02: one recorded input frame per completed tick (the hook
  // fires with the completed tick count — game_loop.h). M1 frames are
  // ZERO-LENGTH: no input system exists yet (M3-INPUT-03 defines the
  // payload shape and source); the frame record still carries the
  // tick and the length field, so the log is forward-ready. A write
  // failure STOPS THE RUN: the sticky replayFail_ makes runFrames
  // return it, and the engine's shutdown still happens (CONC-006).
  if (replayRecorder_ != nullptr && !replayFail_.isError()) {
    const Status writeStatus = replayRecorder_->writeFrame(tick, nullptr, 0);
    if (writeStatus.isError()) {
      replayFail_ = writeStatus;
      LAIGE_LOG_ERROR(kReplaySubsystem, "record_failed", kRecordFailedMessage,
                      laige::log::field("path", replayRecorder_->path()),
                      laige::log::field("tick", tick),
                      laige::log::field("error",
                                        laige::errorName(writeStatus.error())));
    }
  }
}

// ---------------------------------------------------------------------------
// run_headless (the header preamble "run_headless contract")
// ---------------------------------------------------------------------------

Status Engine::run_headless(std::uint64_t maxTicks,
                            std::uint32_t frameBudgetTicks) noexcept {
  // A stopped engine (shutdown or moved-from) is a no-op failure
  // without logging (the stopped-state precedent — the GameLoop's
  // moved-out frame()).
  if (shutDown_ || world_ == nullptr) {
    return ErrorCode::InvalidArgument;
  }
  // M1-DET-01: the replay-identity fields (the seed and the math
  // backend — ADR 0002: both are part of the replay identity). The
  // math field carries the ADR 0002 backend id string.
  LAIGE_LOG_INFO(kEngineSubsystem, "run_started",
                 "Headless run started",
                 laige::log::field("tick_rate_hz", config_.tickRateHz),
                 laige::log::field("tick_target", maxTicks),
                 laige::log::field("frame_budget_ticks", frameBudgetTicks),
                 laige::log::field("seed", config_.seed),
                 laige::log::field("determinism",
                                   config_.determinism.enabled),
                 laige::log::field("math",
                                   config_.determinism.math ==
                                           SimMathBackend::FloatPinned32
                                       ? "fp32_pinned"
                                       : "fpx16_16"));
  // The schedule is computed ONCE, at the start of the run (the
  // game's registrations must precede run_headless — the header's
  // misuse warning; a stale schedule is the loop's documented
  // failure).
  Status runStatus = world_->scheduleSystems(schedule_);
  if (runStatus.ok()) {
    // The engine's always-on profiler is handed to the loop as a
    // NON-OWNING view (the per-completed-tick time feed — game_loop.h
    // Options::profiler; the profiler outlives the loop: it is
    // released in the shutdown AFTER the loop, the header preamble).
    Result<GameLoop, ErrorCode> loopResult =
        GameLoop::create(*world_, schedule_,
                         GameLoop::Options{
                             config_.tickRateHz,
                             frameBudgetTicks,
                             nullptr,               // default headless clock
                             &Engine::onTickHook,   // the M1-LOOP-02 hook
                             this,
                             profiler_.get()});     // the M1-PROF-01 feed
    if (loopResult.ok()) {
      loop_ = std::make_unique<GameLoop>(std::move(loopResult).takeValue());
      // The first frame establishes the loop's start reference and
      // runs zero ticks (game_loop.h) — the snapshot is anchored on
      // EXACTLY that reference (the presentation.h "alpha contract";
      // the hook fires only on completed ticks, so the snapshot
      // exists before it can fire).
      const Status firstFrame = loop_->frame();
      if (firstFrame.ok()) {
        // M1-DET-01: the snapshot of the CONFIGURED SimMath backend
        // (ADR 0002, factory-selected at init) — one allocation
        // (the snapshot object; its slot table is the run's third
        // setup allocation), wrapped in the type-erased handle
        // (detail::PresentationHandle — no virtual dispatch, PERF-006).
        Result<detail::PresentationHandle, ErrorCode> snapshotResult =
            config_.determinism.math == SimMathBackend::FloatPinned32
                ? detail::createPresentationHandle<sim::Fp32Pinned>(
                      *world_, loop_->startReferenceNs(),
                      config_.tickRateHz)
                : detail::createPresentationHandle<sim::Fpx16_16>(
                      *world_, loop_->startReferenceNs(),
                      config_.tickRateHz);
        if (snapshotResult.ok()) {
          snapshot_ = std::move(snapshotResult).takeValue();
          runStatus = runFrames(maxTicks);
        } else {
          runStatus = snapshotResult.error();
        }
      } else {
        runStatus = firstFrame;
      }
    } else {
      runStatus = loopResult.error();
    }
  }
  // M1-DET-02: a SUCCESSFUL run with an active recording finalizes
  // the recorder (flush + trailer + atomic temp-to-final rename)
  // before the shutdown — the log appears at the final path only
  // then. A mid-run recording failure (replayFail_) or a failed frame
  // skips the finalization: the recorder's destructor (in shutdown)
  // removes the temp file, and no partial log is ever left at the
  // final path (replay.h "Recorder contract").
  if (replayRecorder_ != nullptr && !replayFail_.isError() && runStatus.ok()) {
    const Status finishStatus = replayRecorder_->finish();
    if (finishStatus.ok()) {
      LAIGE_LOG_INFO(kReplaySubsystem, "record_finished",
                     "Replay log written (atomic temp + rename)",
                     laige::log::field("path", replayRecorder_->path()),
                     laige::log::field("bytes",
                                       replayRecorder_->bytesWritten()),
                     laige::log::field("frames",
                                       replayRecorder_->frameCount()));
    } else {
      // Finalization failure (rename error, or the cap leaves no room
      // for the trailer): the run reports it — never silent
      // (CORE-008). The rename-failure case leaves the temp file for
      // inspection (replay.h documents that).
      LAIGE_LOG_ERROR(kReplaySubsystem, "record_failed", kRecordFailedMessage,
                      laige::log::field("path", replayRecorder_->path()),
                      laige::log::field("error",
                                        laige::errorName(finishStatus.error())));
      runStatus = finishStatus;
    }
  }
  // M1-PROF-01: the per-run profile snapshot + the opt-in report,
  // both BEFORE the shutdown (the world-pulled fields' source — the
  // world — is still live). The snapshot is captured on EVERY path
  // (success, failed frame, failed start alike — the per-run summary
  // describes what actually happened). The report is finalized on
  // every path too, when started: a zero-tick run writes a
  // zero-tick report (CORE-008: no silent omission). A write failure
  // does NOT fail the run (diagnostics never gate the simulation):
  // it is sticky (profileReportStatus_) and logged, and the caller
  // decides (laige-run maps it to exit 2).
  lastProfile_ = profiler_->snapshot(*world_);
  if (!profileReportPath_.empty()) {
    profileReportFinalized_ = true;
    const Result<std::uint64_t, ErrorCode> report =
        writeProfile(*profiler_, *world_, profileReportPath_,
                     ProfileFormat::Json);
    if (report.ok()) {
      LAIGE_LOG_INFO("profiler", "report_written",
                     "Per-run profile report written",
                     laige::log::field("path", profileReportPath_),
                     laige::log::field("bytes", report.value()));
    } else {
      profileReportStatus_ = Status(report.error());
      LAIGE_LOG_ERROR("profiler", "report_write_failed",
                      kReportWriteFailedMessage,
                      laige::log::field("path", profileReportPath_),
                      laige::log::field("error",
                                        laige::errorName(report.error())));
    }
  }
  // The loop's accounting BEFORE it is destroyed in shutdown (the
  // profiler feed; zeros when the loop never existed).
  lastStats_ = (loop_ != nullptr) ? loop_->stats() : GameLoopStats{};
  LAIGE_LOG_INFO(kEngineSubsystem, "run_finished",
                 "Headless run finished",
                 laige::log::field("ticks", lastStats_.ticks),
                 laige::log::field("dropped_ticks", lastStats_.droppedTicks),
                 laige::log::field("dropped_frames", lastStats_.droppedFrames),
                 laige::log::field(
                     "status",
                     runStatus.ok() ? std::string_view("ok")
                                    : std::string_view(
                                          laige::errorName(runStatus.error()))));
  // CONC-006: shutdown ALWAYS happens (the engine's lifecycle ends in
  // the ordered teardown, success or failure).
  shutdown();
  return runStatus;
}

// The frame drive: bounded, paced, allocation-free (engine.h
// "Performance"). One clock read, one loop frame, one snapshot
// refresh, one sleep per frame.
Status Engine::runFrames(std::uint64_t maxTicks) noexcept {
  const std::int64_t startNs = loop_->startReferenceNs();
  const std::int64_t rate = static_cast<std::int64_t>(loop_->tickRateHz());
  // M1-PROF-01: the frame-time feed. The frame time covers the frame's
  // sim work plus the presentation refresh, EXCLUDING the pacing sleep
  // (the profiler.h contract). A failed frame is not recorded (the
  // frame did not complete). Profiler null or disabled: one branch,
  // nothing else (DBG-004).
  Profiler* prof = profiler_.get();
  const bool timing = (prof != nullptr) && prof->enabled();
  for (;;) {
    // M1-DET-02: a replay-recording failure stops the run (at most
    // one frame's worth of ticks runs after the failing write — the
    // hook's sticky error is observed here and at the frame boundary).
    if (replayFail_.isError()) return replayFail_;
    if (maxTicks != 0 && loop_->currentTick() >= maxTicks) break;
    const std::int64_t now = steadyNowNs();
    if (timing) {
      const TimeIt timer;
      const Status frameStatus = loop_->frame();
      if (frameStatus.isError()) return frameStatus;
      if (replayFail_.isError()) return replayFail_;
      // The frame's clock reading goes to the presentation state
      // (presentation.h wiring: the engine reads the frame clock once
      // per frame and passes it to the snapshot). The snapshot exists
      // before runFrames runs (created in run_headless) — the guard is
      // the never-crash contract (CORE-008).
      if (snapshot_.hasSnapshot()) {
        snapshot_.onRenderFrame(snapshot_.context, now);
      }
      prof->recordFrame(timer.elapsedMs());
    } else {
      const Status frameStatus = loop_->frame();
      if (frameStatus.isError()) return frameStatus;
      if (replayFail_.isError()) return replayFail_;
      if (snapshot_.hasSnapshot()) {
        snapshot_.onRenderFrame(snapshot_.context, now);
      }
    }
    if (maxTicks != 0 && loop_->currentTick() >= maxTicks) {
      break;  // no sleep after the final tick (a bounded run ends)
    }
    // Pacing: sleep until the next tick's due time (one bounded sleep
    // per frame — the headless pacing; the sleep granularity is
    // platform-dependent, and the loop's bounded catch-up absorbs a
    // late wake, game_loop.h). The next due time is exact integer
    // math (the game_loop.cpp ticksDue identity): with T completed
    // ticks, due(T+1) = floor((T+1) x 1e9 / rate) is computed as
    // q x 1e9 + r x 1e9 / rate for (T+1) = q x rate + r — every
    // intermediate product is in range (r < rate, so r x 1e9 < 1.2e11).
    const std::int64_t dueTicks = static_cast<std::int64_t>(loop_->currentTick()) + 1;
    const std::int64_t fullSeconds = dueTicks / rate;
    const std::int64_t remainder = dueTicks % rate;
    const std::int64_t nextDueNs =
        startNs + fullSeconds * kNanosecondsPerSecond +
        remainder * kNanosecondsPerSecond / rate;
    if (nextDueNs > now) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(nextDueNs - now));
    }
  }
  return Status{};
}

// ---------------------------------------------------------------------------
// shutdown (the header preamble "The ordered shutdown")
// ---------------------------------------------------------------------------

void Engine::shutdown() noexcept {
  // IDEMPOTENT (CONC-006): the destructor and a second call are
  // no-ops.
  if (shutDown_) return;
  // 1. systems: the loop stops first — no frame can start after this
  //    point (the system phase is over).
  loop_.reset();
  // M1-DET-02: an UNFINISHED recording is abandoned here (CONC-006:
  // every owned job is released in the ordered teardown). The
  // recorder's destructor removes the temp file — no partial log is
  // ever left at the final path. The structured warn makes the
  // abandonment visible (CORE-008: never silent); the successful-run
  // path finalized the recorder in run_headless, so this fires only
  // for a failed run or a pre-run teardown.
  if (replayRecorder_ != nullptr && !replayRecorder_->finished()) {
    LAIGE_LOG_WARN(kReplaySubsystem, "record_aborted", kRecordAbortedMessage,
                   laige::log::field("path", replayRecorder_->path()),
                   laige::log::field("bytes", replayRecorder_->bytesWritten()));
  }
  replayRecorder_.reset();
  replayFail_ = Status{};
  // 2. world: every live entity is destroyed (the per-entity
  //    component data is released with its rows; the registries
  //    survive — the entity.h clear() contract).
  if (world_ != nullptr) {
    static_cast<void>(world_->clear());
  }
  // 3. pools: the presentation record table, then the world's backing
  //    storage (the per-slot tables, the archetype table and column
  //    blocks, the type-key index), then the profiler (M1-PROF-01).
  //    The snapshot is released BEFORE the world: it holds a
  //    non-owning world view. The profiler holds no world reference
  //    (it pulls cold) and is released after the world.
  snapshot_.reset();
  world_.reset();
  // M1-PROF-01: a STARTED report that was never finalized (a pre-run
  // teardown, or a run that ended before the finalization — neither
  // is reachable for a completed run_headless, which always
  // finalizes) is abandoned: the structured warn makes it visible
  // (CORE-008: never silent). The report is diagnostics, so there is
  // no file to clean up — the write happens only at run end.
  if (!profileReportPath_.empty() && !profileReportFinalized_) {
    LAIGE_LOG_WARN("profiler", "report_aborted", kReportAbortedMessage,
                   laige::log::field("path", profileReportPath_));
    // Aborting finalizes the report's lifecycle (profileReportActive()
    // goes false): the report was started but never written — the
    // warn above carries the state.
    profileReportFinalized_ = true;
  }
  profiler_.reset();
  // 4. logging: the facade's controlled shutdown (the rate-limit
  //    summaries drain, the sink flushes, the facade retires —
  //    LOG-007; idempotent).
  laige::log::Logger::instance().shutdown();
  shutDown_ = true;
}

// ---------------------------------------------------------------------------
// Accessors and lifetime
// ---------------------------------------------------------------------------

World* Engine::world() noexcept { return world_.get(); }

const EngineConfig& Engine::config() const noexcept { return config_; }

bool Engine::isShutDown() const noexcept { return shutDown_; }

GameLoopStats Engine::stats() const noexcept { return lastStats_; }

const Profiler* Engine::profiler() const noexcept {
  return profiler_.get();  // nullptr after shutdown (released member)
}

ProfilerStats Engine::profileStats() const noexcept { return lastProfile_; }

Status Engine::profileReportStatus() const noexcept {
  return profileReportStatus_;
}

bool Engine::profileReportActive() const noexcept {
  return !profileReportPath_.empty() && !profileReportFinalized_;
}

// ---------------------------------------------------------------------------
// The profile report (M1-PROF-01; the contract in engine.h "The
// profiler" and docs/api/profiler.md)
// ---------------------------------------------------------------------------

Status Engine::startProfileReport(std::string_view path) noexcept {
  // The report is diagnostics, not replay state: EVERY build (no
  // NDEBUG gate — unlike startReplayRecording).
  // A stopped engine (shutdown or moved-from) is a no-op failure
  // without logging (the stopped-state precedent).
  if (shutDown_ || world_ == nullptr) {
    return Status(ErrorCode::InvalidArgument);
  }
  if (path.empty()) {
    LAIGE_LOG_WARN("profiler", "report_path_invalid",
                   kReportPathInvalidMessage);
    return Status(ErrorCode::InvalidArgument);
  }
  if (!profileReportPath_.empty()) {
    LAIGE_LOG_WARN("profiler", "report_already_started",
                   kReportAlreadyStartedMessage);
    return Status(ErrorCode::InvalidArgument);
  }
  profileReportPath_ = std::string(path);
  LAIGE_LOG_INFO("profiler", "report_started",
                 "Profile report requested (written at run end, JSON)",
                 laige::log::field("path", profileReportPath_));
  return Status{};
}

// ---------------------------------------------------------------------------
// Replay recording (M1-DET-02; the contract in engine.h "Replay
// recording" and docs/api/replay.md)
// ---------------------------------------------------------------------------

Status Engine::startReplayRecording(std::string_view path,
                                    std::uint64_t maxBytes) noexcept {
#if defined(NDEBUG)
  // Replay recording is a debug-build feature (M1-DET-02 scope):
  // release builds reject it with a structured warn (CORE-008: never
  // silent). The recorder itself is compiled out of this branch.
  LAIGE_LOG_WARN(kReplaySubsystem, "record_disabled", kRecordDisabledMessage);
  return Status(ErrorCode::InvalidArgument);
#else
  // A stopped engine (shutdown or moved-from) is a no-op failure
  // without logging (the stopped-state precedent — the GameLoop's
  // moved-out frame()).
  if (shutDown_ || world_ == nullptr) {
    return Status(ErrorCode::InvalidArgument);
  }
  if (replayRecorder_ != nullptr) {
    LAIGE_LOG_WARN(kReplaySubsystem, "record_already_started",
                   kRecordAlreadyStartedMessage);
    return Status(ErrorCode::InvalidArgument);
  }
  // The replay identity (ADR 0002) is captured at recording start:
  // the component registry must be complete by now (the caller's
  // registration phase — engine.h "Replay recording").
  const ReplayIdentity identity = makeReplayIdentity(*world_, config_);
  Result<ReplayRecorder, ErrorCode> created =
      ReplayRecorder::create(identity, path, maxBytes);
  if (created.isError()) {
    LAIGE_LOG_WARN(kReplaySubsystem, "record_start_failed",
                   kRecordStartFailedMessage,
                   laige::log::field("path", path),
                   laige::log::field("error",
                                     laige::errorName(created.error())));
    return Status(created.error());
  }
  replayRecorder_ =
      std::make_unique<ReplayRecorder>(std::move(created).takeValue());
  LAIGE_LOG_INFO(kReplaySubsystem, "record_started",
                 "Replay recording started (opt-in, M1-DET-02)",
                 laige::log::field("path", path),
                 laige::log::field("size_limit",
                                   maxBytes == 0 ? kDefaultReplaySizeLimit
                                                 : maxBytes),
                 laige::log::field("seed", config_.seed),
                 laige::log::field("math_backend",
                                   static_cast<int>(config_.determinism.math)),
                 laige::log::field("config_hash", identity.configHash),
                 laige::log::field("schema_hash",
                                   identity.componentSchemaHash));
  return Status{};
#endif
}

bool Engine::replayRecordingActive() const noexcept {
  return replayRecorder_ != nullptr && !replayRecorder_->finished();
}

std::uint64_t Engine::replayBytesWritten() const noexcept {
  return replayRecorder_ != nullptr ? replayRecorder_->bytesWritten() : 0;
}

Engine::Engine(Engine&& other) noexcept
    : world_(std::move(other.world_)),
      loop_(std::move(other.loop_)),
      snapshot_(std::move(other.snapshot_)),
      schedule_(other.schedule_),
      config_(other.config_),
      lastStats_(other.lastStats_),
      profiler_(std::move(other.profiler_)),
      lastProfile_(other.lastProfile_),
      replayRecorder_(std::move(other.replayRecorder_)),
      replayFail_(other.replayFail_),
      profileReportPath_(std::move(other.profileReportPath_)),
      profileReportStatus_(other.profileReportStatus_),
      profileReportFinalized_(other.profileReportFinalized_),
      shutDown_(other.shutDown_) {
  // The source becomes a STOPPED engine (the GameLoop moved-out
  // precedent): nothing left to release, nothing to flush. Its
  // recording (if any) is TRANSFERRED, not abandoned — the world it
  // recorded is the same moved world (the recorder's identity still
  // describes it); the same for a started (unfinalized) profile
  // report: it travels with the engine and is finalized — or
  // abandoned with the report_aborted warn — by the destination's
  // run/shutdown.
  other.shutDown_ = true;
}

Engine& Engine::operator=(Engine&& other) noexcept {
  if (this != &other) {
    // Release this engine's current state first (ordered; a no-op
    // when already stopped). An active recording on THIS engine is
    // abandoned by that shutdown (the structured record_aborted warn)
    // before it is replaced.
    shutdown();
    world_ = std::move(other.world_);
    loop_ = std::move(other.loop_);
    snapshot_ = std::move(other.snapshot_);
    schedule_ = other.schedule_;
    config_ = other.config_;
    lastStats_ = other.lastStats_;
    profiler_ = std::move(other.profiler_);
    lastProfile_ = other.lastProfile_;
    replayRecorder_ = std::move(other.replayRecorder_);
    replayFail_ = other.replayFail_;
    profileReportPath_ = std::move(other.profileReportPath_);
    profileReportStatus_ = other.profileReportStatus_;
    profileReportFinalized_ = other.profileReportFinalized_;
    shutDown_ = other.shutDown_;
    other.shutDown_ = true;
  }
  return *this;
}

Engine::~Engine() noexcept { shutdown(); }

}  // namespace laige
