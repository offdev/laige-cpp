// laige-sim headless engine run (M1-HEAD-01; FR-1.6, ARCH-003,
// AC-6.2, CONC-006) + the opt-in replay recording (M1-DET-02).
//
// Implementation of the Engine, EngineConfig, and parseEngineConfig
// declared in include/laige/sim/engine.h — see that header for the
// full contract (the lifecycle, the run contract, the ordered
// shutdown, the provisional config surface, the determinism scope,
// the replay recording, the performance notes) and docs/api/engine.md
// for the API document and the laige-run CLI contract.
//
// Hot-path cost (per headless frame): one clock read, one bounded
// GameLoop::frame() dispatch, one snapshot onRenderFrame, one sleep —
// no allocation and no logging on the healthy path (PERF-003,
// LOG-003; the per-frame breakdown in engine.h "Performance").
// Replay recording (M1-DET-02, opt-in debug builds only) adds one
// bounded stdio write per completed tick when enabled, and one null
// check per completed tick when disabled.

#include "laige/sim/engine.h"  // the Engine contract (this header)

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#include "laige/json.h"
#include "laige/logging.h"

namespace laige {

namespace {

// Nanoseconds per second (the clock time base; the
// game_loop.cpp constant).
inline constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;

// The stable subsystem names (LOG-001).
inline constexpr const char* kEngineSubsystem = "engine";
inline constexpr const char* kConfigSubsystem = "config";
inline constexpr const char* kReplaySubsystem = "replay";

// The headless clock source (M1-LOOP-01): the monotonic steady_clock
// as nanoseconds since its epoch (the windowed clock arrives with
// M2-GL-02).
std::int64_t steadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// NFR-13.3 5-field grammar, identical in every build ({code} |
// {what} | {why} | {fix} | {doc_anchor}): the machine-parseable
// message stays build-stable; the dynamic values are structured
// fields, never message text (the system_timing.cpp precedent).
inline constexpr const char* kNotAnObjectMessage =
    "not_an_object | the engine config document is not a JSON object | "
    "the headless config must be a top-level object | wrap the config "
    "in a top-level object ({} for all defaults) | docs/api/engine.md";

inline constexpr const char* kTickRateInvalidMessage =
    "tick_rate_invalid | the configured tick_rate_hz is invalid | the "
    "value must be an exact integer in the 20-120 Hz range | set "
    "tick_rate_hz to a value in 20-120 (the default is 60) | "
    "docs/api/engine.md";

inline constexpr const char* kEntityBudgetInvalidMessage =
    "entity_budget_invalid | the configured entity_budget is invalid | "
    "the value must be an exact integer in 0-65536 (the 16-bit entity "
    "id space) | set entity_budget to a value in 0-65536 (the "
    "scene's declared budget, G-R3) | docs/api/engine.md";

inline constexpr const char* kChurnBudgetInvalidMessage =
    "churn_budget_invalid | the configured churn_per_frame_budget is "
    "invalid | the value must be a non-negative exact integer | set "
    "churn_per_frame_budget to a non-negative integer (the default is "
    "256; 0 disables the G-R4 guardrail) | docs/api/engine.md";

inline constexpr const char* kUnknownKeyMessage =
    "unknown_key | the config key is not part of the M1-HEAD-01 config "
    "surface | the key is not (yet) consumed by the headless run "
    "(M1-CFG-01 lands the full declarative schema) | remove the key, "
    "or wait for M1-CFG-01 | docs/api/engine.md";

// M1-DET-01: the determinism config keys (seed, determinism.*).
inline constexpr const char* kSeedInvalidMessage =
    "seed_invalid | the configured seed is invalid | the value must be "
    "an exact integer in 0-2^53 (the ADR 0003 JSON number bound: "
    "doubles are exact to 2^53; the programmatic EngineConfig.seed "
    "accepts the full 64 bits) | set seed to an integer in 0-2^53 "
    "(the default is 0) | docs/api/engine.md";

inline constexpr const char* kDeterminismInvalidMessage =
    "determinism_invalid | the configured determinism block is invalid "
    "| the block must be a JSON object ({} for all defaults); the "
    "nested keys enabled (bool) and math (string) have their own "
    "checks below | wrap the determinism block in an object | "
    "docs/api/engine.md";

inline constexpr const char* kDeterminismEnabledInvalidMessage =
    "determinism_enabled_invalid | the configured determinism.enabled "
    "is invalid | the value must be a JSON boolean (default true — "
    "deterministic by default, S-7) | set determinism.enabled to true "
    "or false | docs/api/engine.md";

inline constexpr const char* kDeterminismMathInvalidMessage =
    "determinism_math_invalid | the configured determinism.math is "
    "invalid | the value must be the string \"fixed_point_16_16\" "
    "(default) or \"float_pinned_32\" (the ADR 0002 backend ids) | "
    "set determinism.math to one of the two backend ids | "
    "docs/api/engine.md";

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

// The JSON seed bound: the largest value a JSON number can hold
// exactly (doubles are exact integers to 2^53 — ADR 0003). The
// programmatic EngineConfig.seed has no such bound (full uint64).
inline constexpr std::uint64_t kMaxJsonSeed = 1ull << 53;

// True when `value` is a JSON number holding an exact unsigned integer
// in [lo, hi] (hi must be <= 2^53, where doubles are exact — ADR 0003
// number policy); stores the value in `out` on success. The double
// here is the JSON number policy's storage type (ADR 0003: numbers
// are parsed to double and must round-trip exactly), not simulation
// math — see the exception markers.
bool parseIntInRange(const JsonValue& value, std::uint32_t lo,
                     std::uint32_t hi, std::uint32_t* out) noexcept {
  if (!value.isNumber()) return false;
  const double d = value.asNumber();  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy: doubles store JSON numbers exactly only to 2^53 (ADR 0003); this is config parsing, not sim math
  if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(hi) ||  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
      d != std::floor(d)) {
    return false;
  }
  const std::uint64_t u = static_cast<std::uint64_t>(d);
  if (u < lo) return false;
  *out = static_cast<std::uint32_t>(u);
  return true;
}

// The 64-bit twin for the seed key: an exact unsigned integer in
// [lo, hi] (hi <= 2^53 — the ADR 0003 JSON bound, kMaxJsonSeed).
// Same policy as parseIntInRange: config parsing, not sim math.
bool parseUint64InRange(const JsonValue& value, std::uint64_t lo,
                        std::uint64_t hi, std::uint64_t* out) noexcept {
  if (!value.isNumber()) return false;
  const double d = value.asNumber();  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
  if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(hi) ||  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
      d != std::floor(d)) {
    return false;
  }
  const std::uint64_t u = static_cast<std::uint64_t>(d);
  if (u < lo) return false;
  *out = u;
  return true;
}

// A stable machine-searchable name for a JsonValue's kind (the
// `value_kind` field of the rejection warns; LOG-001).
const char* jsonKindName(const JsonValue& value) noexcept {
  switch (value.kind()) {
    case JsonKind::Null: return "null";
    case JsonKind::Bool: return "bool";
    case JsonKind::Number: return "number";
    case JsonKind::String: return "string";
    case JsonKind::Array: return "array";
    case JsonKind::Object: return "object";
  }
  return "unknown";
}

}  // namespace

// ---------------------------------------------------------------------------
// parseEngineConfig (the provisional M1-HEAD-01 config surface — the
// keys, defaults, and rejection table are in engine.h)
// ---------------------------------------------------------------------------

Result<EngineConfig, ErrorCode> parseEngineConfig(const JsonValue& doc) noexcept {
  if (!doc.isObject()) {
    LAIGE_LOG_WARN(kConfigSubsystem, "not_an_object", kNotAnObjectMessage);
    return ErrorCode::InvalidArgument;
  }
  EngineConfig config;
  for (const auto& [key, value] : doc.asObject()) {
    if (key == "tick_rate_hz") {
      if (!parseIntInRange(value, kMinTickRateHz, kMaxTickRateHz,
                           &config.tickRateHz)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "tick_rate_invalid",
                         kTickRateInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "tick_rate_invalid",
                         kTickRateInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind", jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
    } else if (key == "entity_budget") {
      if (!parseIntInRange(value, 0, Entity::kMaxEntities,
                           &config.entityCapacity)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "entity_budget_invalid",
                         kEntityBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "entity_budget_invalid",
                         kEntityBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind", jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
    } else if (key == "churn_per_frame_budget") {
      const std::uint32_t kMaxUint32 =
          std::numeric_limits<std::uint32_t>::max();
      if (!parseIntInRange(value, 0, kMaxUint32, &config.churnPerFrameBudget)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "churn_budget_invalid",
                         kChurnBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "churn_budget_invalid",
                         kChurnBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind", jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
    } else if (key == "seed") {
      std::uint64_t seed = 0;
      if (!parseUint64InRange(value, 0, kMaxJsonSeed, &seed)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "seed_invalid",
                         kSeedInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "seed_invalid",
                         kSeedInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind", jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
      config.seed = seed;
    } else if (key == "determinism") {
      if (!value.isObject()) {
        LAIGE_LOG_WARN(kConfigSubsystem, "determinism_invalid",
                       kDeterminismInvalidMessage,
                       laige::log::field("key", key),
                       laige::log::field("value_kind", jsonKindName(value)));
        return ErrorCode::InvalidArgument;
      }
      for (const auto& [dkey, dvalue] : value.asObject()) {
        if (dkey == "enabled") {
          if (!dvalue.isBool()) {
            LAIGE_LOG_WARN(kConfigSubsystem,
                           "determinism_enabled_invalid",
                           kDeterminismEnabledInvalidMessage,
                           laige::log::field("key", dkey),
                           laige::log::field("value_kind",
                                             jsonKindName(dvalue)));
            return ErrorCode::InvalidArgument;
          }
          config.determinism.enabled = dvalue.asBool();
        } else if (dkey == "math") {
          if (!dvalue.isString()) {
            LAIGE_LOG_WARN(kConfigSubsystem, "determinism_math_invalid",
                           kDeterminismMathInvalidMessage,
                           laige::log::field("key", dkey),
                           laige::log::field("value_kind",
                                             jsonKindName(dvalue)));
            return ErrorCode::InvalidArgument;
          }
          const std::string_view m = dvalue.asString();
          if (m == "fixed_point_16_16") {
            config.determinism.math = SimMathBackend::FixedPoint16_16;
          } else if (m == "float_pinned_32") {
            config.determinism.math = SimMathBackend::FloatPinned32;
          } else {
            LAIGE_LOG_WARN(kConfigSubsystem, "determinism_math_invalid",
                           kDeterminismMathInvalidMessage,
                           laige::log::field("key", dkey),
                           laige::log::field("value", std::string{m}));
            return ErrorCode::InvalidArgument;
          }
        } else {
          // Unknown nested key: WARN (forward-compat) and ignore — the
          // M1-CFG-01 rule, applied inside the determinism block too.
          LAIGE_LOG_WARN(kConfigSubsystem, "unknown_key", kUnknownKeyMessage,
                         laige::log::field("key", dkey));
        }
      }
    } else {
      // Unknown key: WARN (forward-compat) and ignore — the M1-CFG-01
      // rule, applied to the provisional surface (never silent).
      LAIGE_LOG_WARN(kConfigSubsystem, "unknown_key", kUnknownKeyMessage,
                     laige::log::field("key", key));
    }
  }
  return config;
}

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
    LAIGE_LOG_WARN(kConfigSubsystem, "tick_rate_invalid",
                   kTickRateInvalidMessage,
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
    Result<GameLoop, ErrorCode> loopResult =
        GameLoop::create(*world_, schedule_,
                         GameLoop::Options{
                             config_.tickRateHz,
                             frameBudgetTicks,
                             nullptr,               // default headless clock
                             &Engine::onTickHook,   // the M1-LOOP-02 hook
                             this});
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
  for (;;) {
    // M1-DET-02: a replay-recording failure stops the run (at most
    // one frame's worth of ticks runs after the failing write — the
    // hook's sticky error is observed here and at the frame boundary).
    if (replayFail_.isError()) return replayFail_;
    if (maxTicks != 0 && loop_->currentTick() >= maxTicks) break;
    const std::int64_t now = steadyNowNs();
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
  //    blocks, the type-key index). The snapshot is released BEFORE
  //    the world: it holds a non-owning world view.
  snapshot_.reset();
  world_.reset();
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
      replayRecorder_(std::move(other.replayRecorder_)),
      replayFail_(other.replayFail_),
      shutDown_(other.shutDown_) {
  // The source becomes a STOPPED engine (the GameLoop moved-out
  // precedent): nothing left to release, nothing to flush. Its
  // recording (if any) is TRANSFERRED, not abandoned — the world it
  // recorded is the same moved world (the recorder's identity still
  // describes it).
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
    replayRecorder_ = std::move(other.replayRecorder_);
    replayFail_ = other.replayFail_;
    shutDown_ = other.shutDown_;
    other.shutDown_ = true;
  }
  return *this;
}

Engine::~Engine() noexcept { shutdown(); }

}  // namespace laige
