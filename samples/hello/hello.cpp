// hello.laige (M1-SAMPLE-01) — the headless template game.
//
// The canonical Laige game shape (PRD §13, NFR-13.5): one component,
// one system, one entity, deterministic by default (S-7). It shows the
// component marks (FR-1.2, G-R8), the LAIGE_SYSTEM declaration (FR-1.3),
// the registration order (part of the replay identity — ADR 0002), and
// the per-tick state-hash stream (docs/api/detcheck.md).
//
// Usage:  hello [--replay LOG] | hello --log LOG
//   stdout  N+1 '<tick> <hash>' lines (tick 0 = initial state, then one
//           per completed tick; 16 lowercase hex) — nothing else
//   stderr  the one-line summary + diagnostics
//   exit    0 ok, 1 run failure, 2 usage / IO / identity error
//
// N = 300 (the M1 CI scenario length, kScenarioTicks); in --log mode the
// log's frame count defines N. The config is the canonical sample
// config, embedded below and documented in config.json (identical
// values): the M1 template keeps the CLI minimal (CORE-004); the
// engine's CLI (laige-run) exercises the declarative config surface.
//
// The tick loop uses the engine's own tick primitives (one beginFrame()
// + one runSystems() per tick — the runReplay shape, laige/sim/
// replay.h): the detcheck contract needs a per-completed-tick state hash
// on stdout, and the M1 Engine (run_headless) owns its loop and exposes
// no per-tick hook. The Engine remains the runner for the built-in
// scenario (laige-run, tools/run). Details: samples/hello/README.md.

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim/engine.h"  // EngineConfig
#include "laige/sim/replay.h"  // ReplayRecorder, loadReplay, identity

using M = laige::sim::SimMathFpx16;  // the ADR 0002 default backend

// The game's only component (FR-1.2): the player's 2D position (PRD §4:
// the simulation is 2D). Marked a component and determinism-safe (G-R8):
// its single member is a SimMath vector. The marks specialize engine
// trait templates (laige::detail) and therefore live at namespace
// scope, outside the file's anonymous namespace.
struct PlayerPos { M::Vec2 pos{}; };
LAIGE_COMPONENT(PlayerPos)
LAIGE_DETERMINISM_SAFE(PlayerPos, M::Vec2)

namespace {

// Named gameplay constants (CORE-005), world units: the player moves 1
// unit per tick along the diagonal and wraps in a 32-unit box — a full
// crossing is 32 ticks (~0.53 s at the canonical 60 Hz).
constexpr std::uint32_t kScenarioTicks = 300;    // the M1 CI scenario
constexpr M::Scalar kVelocity = M::Scalar::fromInt32(1);   // 1 unit/tick
constexpr M::Scalar kBoxMin = M::Scalar::fromInt32(-16);
constexpr M::Scalar kBoxMax = M::Scalar::fromInt32(16);
constexpr M::Scalar kBoxSize = M::Scalar::sub(kBoxMax, kBoxMin);  // 32 units

// The game's only system (FR-1.3: plain function, 1 ms budget, declared
// Write on PlayerPos): advance each axis by the constant velocity and
// wrap inside the box. One step crosses at most one wrap boundary —
// exact in fixed point (ADR 0002).
LAIGE_SYSTEM(MovePlayer, 1)
void MovePlayer(laige::World&, laige::SystemContext& ctx) {
  // A system returns nothing (FR-1.3): a failed each (a nested
  // iteration or a write-during-read guard) warns through the logging
  // facade in release and asserts in debug — the query guard's
  // documented behavior (query.h), so the Status is cast away here.
  static_cast<void>(ctx.each<PlayerPos>([](laige::Entity, PlayerPos& p) {
    auto stepAxis = [](M::Scalar& v) {
      v = M::add(v, kVelocity);
      if (v >= kBoxMax) v = M::sub(v, kBoxSize);
      if (v < kBoxMin) v = M::add(v, kBoxSize);
    };
    stepAxis(p.pos.x);
    stepAxis(p.pos.y);
  }, laige::Write{}));
}

void report(const char* stage, laige::ErrorCode code) {
  std::fprintf(stderr, "hello: %s: %s\n", stage, laige::errorText(code));
}

}  // namespace

int main(int argc, char** argv) {
  std::string replayPath, logPath;
  std::uint32_t ticks = kScenarioTicks;
  if (argc % 2 == 0) { std::fprintf(stderr, "hello: an option is missing its value\n"); return 2; }
  for (int i = 1; i < argc; i += 2) {
    const std::string_view a = argv[i], v = argv[i + 1];
    if (a == "--replay") replayPath = v;
    else if (a == "--log") logPath = v;
    else { std::fprintf(stderr, "hello: unknown option '%.*s' (see samples/hello/README.md)\n", static_cast<int>(a.size()), a.data()); return 2; }
  }
  if (!logPath.empty() && !replayPath.empty()) { std::fprintf(stderr, "hello: --log and --replay are exclusive\n"); return 2; }

  // 1. The canonical config (identical to config.json): 60 Hz, scene
  //    budget 8, the engine churn default, the house seed
  //    (docs/testing.md), deterministic fpx16_16 — the only backend this
  //    template supports (ADR 0002).
  const laige::EngineConfig config{60, 8, 256, 0x1F055EED, {true, laige::SimMathBackend::FixedPoint16_16}};

  // 2. World + the game's registration (the setup phase; the registration
  //    order is part of the replay identity — ADR 0002), then the player
  //    entity at the box center (the initial state the tick-0 line covers)
  //    and the pre-run system schedule (M1-SYS-02).
  laige::World::Options o{config.entityCapacity, config.churnPerFrameBudget, config.seed, config.determinism.enabled};
  auto wr = laige::World::create(o);
  if (wr.isError()) { report("world", wr.error()); return 2; }
  laige::World world = std::move(wr).takeValue();
  const laige::Status setup = [&world] {
    const auto c = world.registerComponent<PlayerPos>();
    if (c.isError()) return laige::Status(c.error());
    const auto s = world.registerSystem(MovePlayer_Def, laige::Io<PlayerPos, laige::Access::Write>{});
    if (s.isError()) return laige::Status(s.error());
    const auto e = world.create();
    if (e.isError()) return laige::Status(e.error());
    return world.addComponent<PlayerPos>(e.value(), PlayerPos{});
  }();
  if (setup.isError()) { report("world", setup.error()); return 2; }
  laige::SystemSchedule schedule;
  if (auto ss = world.scheduleSystems(schedule); ss.isError()) { report("world", ss.error()); return 2; }

  // 3. Replay mode: load the recorded log and check its replay identity
  //    against this (world, config) (ADR 0002: a mismatch is a rejected
  //    replay, never a silent divergence). The log's frame count defines
  //    the run length.
  if (!logPath.empty()) {
    const auto lg = laige::loadReplay(logPath);
    if (lg.isError()) { report("replay", lg.error()); return 2; }
    if (!laige::replayIdentityDiff(lg.value(), world, config).empty()) { report("replay: identity mismatch", laige::ErrorCode::InvalidArgument); return 2; }
    ticks = static_cast<std::uint32_t>(lg.value().frames.size());
  }

  // 4. Run mode: optional replay recording (opt-in, DEBUG BUILDS ONLY —
  //    the engine's startReplayRecording policy; release rejects it).
  std::unique_ptr<laige::ReplayRecorder> recorder;
  if (!replayPath.empty()) {
#if defined(NDEBUG)
    std::fprintf(stderr, "hello: replay: recording is a debug-build feature (NDEBUG)\n");
    return 2;
#else
    auto r = laige::ReplayRecorder::create(laige::makeReplayIdentity(world, config), replayPath, laige::kDefaultReplaySizeLimit);
    if (r.isError()) { report("replay", r.error()); return 2; }
    recorder = std::make_unique<laige::ReplayRecorder>(std::move(r).takeValue());
#endif
  }

  // 5. The tick loop: the tick-0 line (initial state), then one line per
  //    completed tick — stdout carries nothing else.
  std::printf("0 %016llx\n", static_cast<unsigned long long>(world.stateHash(0)));
  laige::Status runStatus{};
  std::uint64_t completed = 0;
  for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
    world.beginFrame();
    const laige::Status step = world.runSystems(schedule);
    if (step.isError()) { runStatus = step; break; }
    if (recorder != nullptr) { const laige::Status f = recorder->writeFrame(tick, nullptr, 0); if (f.isError()) { runStatus = f; break; } }
    ++completed;
    std::printf("%llu %016llx\n", static_cast<unsigned long long>(tick), static_cast<unsigned long long>(world.stateHash(tick)));
  }

  // 6. Ordered teardown (CONC-006): finalize the log only on success (an
  //    unfinished recorder's destructor removes its temp file), clear the
  //    world, shut down the logging facade.
  if (recorder != nullptr) runStatus = runStatus.ok() ? recorder->finish() : runStatus;
  std::fprintf(stderr, "hello %s ticks=%llu status=%s\n", logPath.empty() ? "headless" : "replay", static_cast<unsigned long long>(completed), runStatus.ok() ? "ok" : laige::errorName(runStatus.error()));
  static_cast<void>(world.clear());
  laige::log::Logger::instance().shutdown();
  return runStatus.ok() ? 0 : 1;
}
