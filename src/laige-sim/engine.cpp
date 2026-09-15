// laige-sim headless engine run (M1-HEAD-01; FR-1.6, ARCH-003,
// AC-6.2, CONC-006).
//
// Implementation of the Engine, EngineConfig, and parseEngineConfig
// declared in include/laige/sim/engine.h — see that header for the
// full contract (the lifecycle, the run contract, the ordered
// shutdown, the provisional config surface, the determinism scope,
// the performance notes) and docs/api/engine.md for the API document
// and the laige-run CLI contract.
//
// Hot-path cost (per headless frame): one clock read, one bounded
// GameLoop::frame() dispatch, one snapshot onRenderFrame, one sleep —
// no allocation and no logging on the healthy path (PERF-003,
// LOG-003; the per-frame breakdown in engine.h "Performance").

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

// True when `value` is a JSON number holding an exact unsigned integer
// in [lo, hi] (hi must be <= 2^53, where doubles are exact — ADR 0003
// number policy); stores the value in `out` on success.
bool parseIntInRange(const JsonValue& value, std::uint32_t lo,
                     std::uint32_t hi, std::uint32_t* out) noexcept {
  if (!value.isNumber()) return false;
  const double d = value.asNumber();
  if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(hi) ||
      d != std::floor(d)) {
    return false;
  }
  const std::uint64_t u = static_cast<std::uint64_t>(d);
  if (u < lo) return false;
  *out = static_cast<std::uint32_t>(u);
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
  // ARCH-010; the game's components follow through world()).
  const Result<ComponentTypeId, ErrorCode> builtin =
      engine.world_->registerComponent<Position2DFpx16>();
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
  // snapshot exists by the time this hook can fire; a null snapshot
  // is still a no-op, never a crash.
  if (snapshot_ != nullptr) snapshot_->onTick(tick);
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
  LAIGE_LOG_INFO(kEngineSubsystem, "run_started",
                 "Headless run started",
                 laige::log::field("tick_rate_hz", config_.tickRateHz),
                 laige::log::field("tick_target", maxTicks),
                 laige::log::field("frame_budget_ticks", frameBudgetTicks));
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
        using Snapshot = PresentationSnapshot<sim::Fpx16_16>;
        Result<Snapshot, ErrorCode> snapshotResult = Snapshot::create(
            *world_, loop_->startReferenceNs(),
            Snapshot::Options{config_.tickRateHz});
        if (snapshotResult.ok()) {
          snapshot_ = std::make_unique<Snapshot>(
              std::move(snapshotResult).takeValue());
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
    if (maxTicks != 0 && loop_->currentTick() >= maxTicks) break;
    const std::int64_t now = steadyNowNs();
    const Status frameStatus = loop_->frame();
    if (frameStatus.isError()) return frameStatus;
    // The frame's clock reading goes to the presentation state
    // (presentation.h wiring: the engine reads the frame clock once
    // per frame and passes it to the snapshot).
    snapshot_->onRenderFrame(now);
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

Engine::Engine(Engine&& other) noexcept
    : world_(std::move(other.world_)),
      loop_(std::move(other.loop_)),
      snapshot_(std::move(other.snapshot_)),
      schedule_(other.schedule_),
      config_(other.config_),
      lastStats_(other.lastStats_),
      shutDown_(other.shutDown_) {
  // The source becomes a STOPPED engine (the GameLoop moved-out
  // precedent): nothing left to release, nothing to flush.
  other.shutDown_ = true;
}

Engine& Engine::operator=(Engine&& other) noexcept {
  if (this != &other) {
    // Release this engine's current state first (ordered; a no-op
    // when already stopped).
    shutdown();
    world_ = std::move(other.world_);
    loop_ = std::move(other.loop_);
    snapshot_ = std::move(other.snapshot_);
    schedule_ = other.schedule_;
    config_ = other.config_;
    lastStats_ = other.lastStats_;
    shutDown_ = other.shutDown_;
    other.shutDown_ = true;
  }
  return *this;
}

Engine::~Engine() noexcept { shutdown(); }

}  // namespace laige
