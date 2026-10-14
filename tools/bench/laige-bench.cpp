// laige-bench (M0-CORE-08, extended by M1-BENCH-01) — the canonical
// benchmark command (docs/getting-started/building.md is the source of
// truth):
//
//   ./build/bin/laige-bench --suite=<name> [--runs=N] [--warmup=N]
//                           [--budget=<budget-name>]... [--budgets=<path>]
//                           [--math=<fixed_point_16_16|float_pinned_32>]
//                           [--report=<path>]
//
// Runs the named suite: `warmup` discarded iterations, then `runs`
// timed iterations recorded into a laige::Histogram; prints the AGENTS
// 12 summary (sample count, min/mean/p50/p95/p99/max, context). With
// one or more `--budget=<name>` the result is additionally checked
// against each named budgets.json entry (loadBudgets + budgetCheck)
// and the pass/fail reports are printed; a failed check exits
// non-zero so the command is CI-gateable (PRD 8.1 policy: a budget
// regression fails CI). `--math` selects the SimMath backend (ADR
// 0002) the `sim-tick`, `render-50k`, and `ac4` / `render-sort-10k`
// suites run on; the other suites ignore it.
//
// Exit codes:
//   0  ok (and every budget check passed, when --budget was given)
//   1  usage error, unknown suite, or budgets.json unreadable/malformed
//   2  a budget check failed (loud failure, CORE-008)
//
// Suites:
//   synthetic   M0-EXIT-01's deterministic 4096-step LCG+double pipeline
//               — the harness stand-in workload that proved the
//               measurement pipeline (timer, histogram, report,
//               budget check) end to end before the real workloads.
//               Its baseline: docs/benchmarks/baselines/m0-synthetic.md.
//   sim-tick    M1-BENCH-01's PRD 8.1 "Simulation tick" workload:
//               10 000 entities (scene budget at 100%), 2 000 of them
//               dynamic bodies carrying the built-in Position2D of the
//               selected SimMath backend plus a velocity component,
//               driven at 60 Hz by the engine's GameLoop on an exact
//               synthetic clock (one tick per frame, the
//               M1-PROF-01 measurement pattern). Two systems run per
//               tick: BenchMove (pos += vel over every dynamic body)
//               and BenchHash (the per-tick deterministic state hash,
//               World::stateHash — the M1 determinism work made
//               representative in the tick). One measured sample is
//               one completed tick (frame() over the synthetic clock).
//               Its baseline: docs/benchmarks/baselines/m1-sim-tick.md.
//               The measured tick is the simulation tick itself
//               (beginFrame + the systems + the loop's bookkeeping);
//               the presentation snapshot (M1-LOOP-02) is NOT part of
//               it — presentation is separate from the authoritative
//               tick by ARCH-009.
//   render-50k   M2-PERF-01's PRD 8.1 "50k visible sprites" workload:
//               the FULL M2-SCENE-01 reference scene (50 000 sprites,
//               65 536 terrain tiles, 3 parallax layers, 8 particle
//               emitters, 2 UI widgets) rendered OFFSCREEN on a
//               1280 x 720 headless FBO (the M2-GOLD-01 reference
//               resolution), both SimMath backends (--math). One
//               measured iteration = one frame: the sim-side pacing
//               (one particle update per frame, OUTSIDE the measured
//               window — the sim tick is the `sim_tick` budget's
//               territory, ARCH-002), then the measured window =
//               the frame's batch stage (world beginFrame +
//               declareIsoReferenceScene + build, UI beginFrame +
//               declareUi + build), then the submit stage renders the
//               built frame offscreen (world pass first, UI pass on
//               top — the engine's frame order). The window's CPU
//               time is the `sprites_50k_cpu` sample (mean); the
//               frame's draw-call count (both passes' frameStats
//               drawCalls — one instanced draw per group, RENDER-001)
//               is the `sprites_50k_draw_calls` sample (max). The
//               allocation watch (M1-ALLOC-01 extended to the
//               batcher) arms around every measured window: a window
//               with a nonzero count aborts the run (the first
//               offending site, the G-R1 per-tick assertion's shape —
//               the batch stage MUST allocate nothing, FR-2.2/
//               PERF-003); the driver's own GL work sits outside the
//               window (like the logging facade's emit — the
//               counting backend's attribution contract). On a host
//               without an offscreen GL 3.3 context the suite runs
//               CPU-ONLY: the window + zero-allocation still measure,
//               the draw-call histogram stays empty (a named check
//               against it fails NO_SAMPLES, loud per CORE-008).
//               Its baseline: docs/benchmarks/baselines/m2-render-50k.md.
//   ac4         M2-AC-01's AC-4.x acceptance workload (the dedicated
//               AC-4.3 suite under the alias `render-sort-10k` —
//               two entries, one workload): 10 000 overlapping
//               sprites (a 100 x 10 cell grid, 10 sprites stacked
//               per cell at the same position and step height — the
//               sort's equal-key ties + the frame's overdraw),
//               scale 1.0, two atlases x two blends = four
//               (atlas, material, blend) groups, sorted and drawn in
//               one 60 FPS frame (the `sprites_10k_frame` sample =
//               the whole frame's CPU ms, batch stage + submit
//               stage, 16.7 ms budget) with the batcher zero-
//               allocation assertion; setup additionally asserts the
//               AC-4.4 engine default (the default ProjectionView's
//               mode is Iso — ADR 0005). Both SimMath backends
//               (--math). Its baseline:
//               docs/benchmarks/baselines/m2-ac4.md.
//
// The synthetic suite is a deterministic, allocation-free stand-in
// workload; the sim-tick suite is deterministic too (fixed seed
// 0x1F055EED, the repo-wide test-seed convention — docs/testing.md;
// no randomness in the measured path) and, in debug non-sanitizer
// builds, every measured tick additionally passes the engine's own
// G-R1 per-tick zero-allocation assertion (GameLoop::runOneTick arms
// the allocation watch — owner: the tick thread, M1-ALLOC-01) — an
// allocating tick aborts the run.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "laige/alloc_watch.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/render/gl_context.h"
#include "laige/render/iso_camera.h"
#include "laige/render/iso_depth_key.h"
#include "laige/render/iso_reference_scene.h"
#include "laige/render/projection.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/sprite_renderer.h"
#include "laige/render/ui_render.h"
#include "laige/result.h"
#include "laige/sim/component.h"  // LAIGE_COMPONENT
#include "laige/sim/determinism.h"  // LAIGE_DETERMINISM_SAFE
#include "laige/sim/entity.h"
#include "laige/sim/game_loop.h"
#include "laige/sim/presentation.h"  // Position2D
#include "laige/sim_math.h"  // SimMath backends (laige-core)
#include "laige/sim/system.h"  // SystemDef, LAIGE_SYSTEM, Io

// TEMPORARY DIAGNOSTIC (removed with the root-cause fix): the ac4
// entries segfault at process exit on the CI Linux lanes but do not
// crash under gdb, under ASan, or locally (a Heisenbug), so capture
// the crashing stack in-process. -O0 Debug keeps frame pointers for
// backtrace(); --export-dynamic (a temporary CMake option below)
// gives backtrace_symbols real names.
#if defined(__linux__)
#include <csignal>
#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// The benchmark's workload component (FR-1.2, G-R8): the dynamic
// body's 2D velocity in the selected SimMath backend's Vec2. The
// marks specialize engine trait templates (laige::detail) and
// therefore live at namespace scope, outside the file's anonymous
// namespace (the presentation.h Position2D precedent).
template <typename Backend>
struct BenchVel {
  laige::sim::SimMath<Backend>::Vec2 v{};
};

LAIGE_COMPONENT(BenchVel<laige::sim::Fpx16_16>);
LAIGE_COMPONENT(BenchVel<laige::sim::Fp32Pinned>);
LAIGE_DETERMINISM_SAFE(
    BenchVel<laige::sim::Fpx16_16>,
    laige::sim::SimMath<laige::sim::Fpx16_16>::Vec2);
LAIGE_DETERMINISM_SAFE(
    BenchVel<laige::sim::Fp32Pinned>,
    laige::sim::SimMath<laige::sim::Fp32Pinned>::Vec2);

namespace {

#if defined(__linux__)
// TEMPORARY DIAGNOSTIC: the in-process exit-time segfault capture
// (see the include above). The backtrace goes to stderr (the CI job
// log under ctest --output-on-failure); the default handler is
// restored and the signal re-raised, so the original crash outcome
// is unchanged.
[[noreturn]] void Ac4CrashHandler(int sig) {
  // The crashed thread id matters: if it is not the main thread, the
  // crash is inside a library worker racing with the exit path.
  char line[128];
  std::snprintf(line, sizeof(line),
                "ac4 diagnostic: fatal signal %d in exit path, "
                "crashed tid=%ld, stack:\n",
                sig, static_cast<long>(::syscall(SYS_gettid)));
  (void)!::write(STDERR_FILENO, line,
                 static_cast<std::size_t>(std::strlen(line)));
  void* frames[64];
  const int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  ::signal(sig, SIG_DFL);
  ::raise(sig);
  // The re-raised signal terminates the process (default disposition);
  // reach here only if it was blocked or ignored — terminate directly
  // (the process is dying either way, no cleanup to run).
  std::_Exit(128 + sig);
}
#endif

// The parsed command-line configuration (defined before the suites —
// the sim-tick suite's setup callback reads the selected backend).
struct Config {
  std::string suite;
  std::uint64_t runs = 1000;
  std::uint64_t warmup = 100;
  std::vector<std::string> budgetNames;  // repeatable --budget
  std::string budgetsPath;  // empty -> env LAIGE_BUDGETS_PATH -> "budgets.json"
  std::string reportPath;
  // The sim-tick suite's SimMath backend (ADR 0002): false =
  // float_pinned_32, true = fixed_point_16_16 (the default).
  bool mathFpx16 = true;
};

// --- The synthetic suite ----------------------------------------------------

// LCG64 constants (Marsaglia, "Random Numbers", 2003 — the 64-bit LCG;
// cited per CPP-014). The constants are arbitrary and named (CORE-005):
// the workload models no physical quantity — its job is to exercise
// the measurement pipeline (timer, histogram, report, budget check).
constexpr std::uint64_t kSyntheticLcgMultiplier = 6364136223846793005ULL;
constexpr std::uint64_t kSyntheticLcgIncrement = 1442695040888963407ULL;
constexpr int kSyntheticSteps = 4096;
constexpr std::uint64_t kSyntheticState0 = 0x1234567890ABCDEFULL;

// Measured iteration: a fixed 4096-step mixed 64-bit-integer + double
// pipeline. Deterministic, bounded, allocation-free. The accumulator is
// stored through a module-level volatile so the pipeline cannot be
// optimized away.
volatile std::int64_t g_syntheticSink = 0;

void syntheticIteration() {
  double scale = 1.0;
  std::uint64_t state = kSyntheticState0;
  for (int i = 0; i < kSyntheticSteps; ++i) {
    state = state * kSyntheticLcgMultiplier + kSyntheticLcgIncrement;
    scale += 0.5 * static_cast<double>((state >> 33) & 0xFF);
  }
  g_syntheticSink = static_cast<std::int64_t>(scale * 1.0e6) +
                    static_cast<std::int64_t>(state >> 32);
}

// --- The sim-tick suite (M1-BENCH-01; PRD 8.1 "Simulation tick") -------------

// The workload constants (CORE-005, named — PRD 8.1 reference values):
//   10 000 entities      the §8.1 workload's entity count (the scene
//                        budget at 100% — the M1-ECS-07 stress shape);
//   2 000 dynamic bodies the §8.1 workload's moving bodies (M1
//                        measures the ECS-only slice — kinematic
//                        movement; the physics lands in M3);
//   60 Hz                FR-1.1's default tick rate;
//   0x1F055EED           the repo-wide test-seed convention
//                        (docs/testing.md §4; methodology §6).
constexpr std::uint32_t kSimTickEntities = 10000;
constexpr std::uint32_t kSimTickDynamic = 2000;
constexpr std::uint32_t kSimTickTickRateHz = 60;
constexpr std::uint64_t kSimTickSeed = 0x1F055EED;

// One 60 Hz tick, in whole nanoseconds (10⁹ / 60 rounded up — the
// game_loop_tests constant): advancing the synthetic clock by exactly
// this many ns makes each frame run exactly one due tick (the exact
// integer due computation, M1-LOOP-01: floor(k × 16 666 667 × 60 / 10⁹)
// = k for every k the workload can reach).
constexpr std::int64_t kSimTickNs = 16666667;

// Initial-state shape (deterministic, index-derived — no RNG in the
// measured path): the dynamic bodies scatter over a 50×50 grid span
// (one body per cell of the first 2000 cells: x = col−25 ∈ −25..24,
// y = row ∈ 0..49) with a
// small per-axis drift (vx ∈ -3..3, vy ∈ -5..5 world units per tick).
// Max coordinate magnitude over the full run (warmup + measured,
// 4 000 ticks) is 20 049 units (49 + 5·4000) — inside fpx16_16's
// ±32 768 Q16.16 range, so no saturating-overflow edge is ever
// touched in the measured window.
constexpr std::int32_t kSimTickGridSpan = 50;
constexpr std::int32_t kSimTickVelXMax = 3;
constexpr std::int32_t kSimTickVelYMax = 5;

// The synthetic clock (the GameLoop's injectable nowNs — the
// profiler_tests precedent): exact 60 Hz pacing with no wall-clock
// dependence, so the measured samples are ticks, not real-time.
std::int64_t g_simClockNs = 0;
std::int64_t simClockNowNs() { return g_simClockNs; }

// The suite's per-run state (one World, one loop, one backend). The
// setup path performs the workload's only allocations (the World's
// storage, this struct, the unique_ptr holders) — every tick after
// setup is allocation-free (the G-R1 assertion in debug builds makes
// that a property, not a claim).
struct SimTickState {
  std::unique_ptr<laige::World> world;
  laige::SystemSchedule schedule{};
  std::unique_ptr<laige::GameLoop> loop;
  // Completed ticks so far (the loop's tick count at frame start —
  // the stateHash "number of completed ticks" parameter's value).
  std::uint64_t completedTicks = 0;
  // The state hash of the last tick the BenchHash system ran (a
  // pure function of the workload — printed in teardown as the
  // run's determinism fingerprint).
  std::uint64_t lastHash = 0;
  // True for the fpx16_16 backend (the default, ADR 0002).
  bool fpx16 = true;
};

std::unique_ptr<SimTickState> g_simTick;

// Loud benchmark failure: the workload's contract broke (a setup
// rejection, a failed frame). A failed measurement must never be
// silent (CORE-008) — exit 1, distinct from the budget-check 2.
[[noreturn]] void simTickFail(const std::string& what) {
  std::fprintf(stderr, "laige-bench: sim-tick failed: %s\n",
               what.c_str());
  std::fflush(stderr);
  std::exit(1);
}

// The movement systems: pos += vel over every dynamic body, through
// the active backend's SimMath add (the pinned op surface, ADR 0002 —
// one correctly-rounded addition per component, never fused). One
// concrete system per backend (the LAIGE_SYSTEM macro binds a
// function address; both instantiations live in this translation
// unit, the hello-fp32 two-builds pattern folded into one binary).
LAIGE_SYSTEM(BenchMoveFpx16, 1)
void BenchMoveFpx16(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  // A failed each() is unreachable here (both components are registered
  // and no iteration is active — the query_tests (void) discard pattern).
  (void)ctx.each<laige::Position2DFpx16, BenchVel<laige::sim::Fpx16_16>>(
      [](laige::Entity, laige::Position2DFpx16& pos,
         BenchVel<laige::sim::Fpx16_16>& vel) {
        pos.pos = laige::sim::SimMathFpx16::add(pos.pos, vel.v);
      },
      laige::Write{}, laige::Write{});
}

LAIGE_SYSTEM(BenchMoveFp32, 1)
void BenchMoveFp32(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  // A failed each() is unreachable here (both components are registered
  // and no iteration is active — the query_tests (void) discard pattern).
  (void)ctx.each<laige::Position2DFp32, BenchVel<laige::sim::Fp32Pinned>>(
      [](laige::Entity, laige::Position2DFp32& pos,
         BenchVel<laige::sim::Fp32Pinned>& vel) {
        pos.pos = laige::sim::SimMathFp32::add(pos.pos, vel.v);
      },
      laige::Write{}, laige::Write{});
}

// The state-hash system: the per-tick deterministic state hash
// (World::stateHash, M1-DET-03) — the M1 determinism work made part
// of the measured tick. It reads the world's live state as a cold
// const read (NOT a query — no iteration-legality interaction),
// labeled with the number of completed ticks so far (during tick T
// that is T-1: the state before tick T's completion, the parameter's
// documented contract). O(capacity + live component bytes); no
// allocation (the streaming FNV, state_hash.cpp).
LAIGE_SYSTEM(BenchHash, 2)
void BenchHash(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(ctx);
  std::uint64_t h = world.stateHash(g_simTick->completedTicks);
  g_simTick->lastHash = h;
}

bool simTickSetup(const Config& cfg) {
  auto st = std::make_unique<SimTickState>();
  st->fpx16 = cfg.mathFpx16;

  laige::World::Options options{};
  options.capacity = kSimTickEntities;
  options.seed = kSimTickSeed;
  auto w = laige::World::create(options);
  if (!w.ok()) {
    simTickFail("World::create failed: " +
                   std::string(laige::errorName(w.error())));
  }
  st->world = std::make_unique<laige::World>(std::move(w).takeValue());

  // Component registration order (the engine's built-ins first —
  // presentation.h/ARCH-010 stable-order convention; one component
  // per backend: the selected backend's Position2D + the velocity).
  if (st->fpx16) {
    if (!st->world->registerComponent<laige::Position2DFpx16>().ok() ||
        !st->world->registerComponent<BenchVel<laige::sim::Fpx16_16>>()
             .ok()) {
      simTickFail("registerComponent failed");
    }
  } else {
    if (!st->world->registerComponent<laige::Position2DFp32>().ok() ||
        !st->world->registerComponent<BenchVel<laige::sim::Fp32Pinned>>()
             .ok()) {
      simTickFail("registerComponent failed");
    }
  }

  // System registration order = execution order (no depends_on):
  // movement first, then the state hash (the hash covers the
  // post-movement state of each tick).
  const bool moveRegistered = st->fpx16
      ? st->world
            ->registerSystem(
                BenchMoveFpx16_Def,
                laige::Io<laige::Position2DFpx16, laige::Access::Write>{},
                laige::Io<BenchVel<laige::sim::Fpx16_16>,
                         laige::Access::Write>{})
            .ok()
      : st->world
            ->registerSystem(
                BenchMoveFp32_Def,
                laige::Io<laige::Position2DFp32, laige::Access::Write>{},
                laige::Io<BenchVel<laige::sim::Fp32Pinned>,
                         laige::Access::Write>{})
            .ok();
  if (!moveRegistered || !st->world->registerSystem(BenchHash_Def).ok()) {
    simTickFail("registerSystem failed");
  }

  // The scene: all 10 000 entities; the first 2 000 are dynamic
  // bodies (Position2D + velocity, deterministic initial state from
  // the entity index); the remaining 8 000 are bare entities (no
  // components — the static scenery's M1 stand-in).
  for (std::uint32_t i = 0; i < kSimTickEntities; ++i) {
    auto e = st->world->create();
    if (!e.ok()) {
      simTickFail("World::create failed at the scene budget");
    }
    const laige::Entity entity = std::move(e).takeValue();
    if (i < kSimTickDynamic) {
      // One body per grid cell, column-major over the 50×50 span:
      // i = 50·col + row — the first 2 000 cells (40 columns of 50).
      const std::int32_t x =
          static_cast<std::int32_t>((i / kSimTickGridSpan) %
                                         kSimTickGridSpan) -
          kSimTickGridSpan / 2;
      const std::int32_t y = static_cast<std::int32_t>(i % kSimTickGridSpan);
      const std::int32_t vx = static_cast<std::int32_t>(i % (2 * kSimTickVelXMax + 1)) - kSimTickVelXMax;
      const std::int32_t vy = static_cast<std::int32_t>(i % (2 * kSimTickVelYMax + 1)) - kSimTickVelYMax;
      if (st->fpx16) {
        laige::Position2DFpx16 pos{};
        pos.pos = laige::sim::SimMathFpx16::Vec2{laige::fpx16_16::fromInt32(x),
                                                 laige::fpx16_16::fromInt32(y)};
        BenchVel<laige::sim::Fpx16_16> vel{};
        vel.v = laige::sim::SimMathFpx16::Vec2{laige::fpx16_16::fromInt32(vx),
                                               laige::fpx16_16::fromInt32(vy)};
        if (!st->world->addComponent<laige::Position2DFpx16>(entity, pos).ok() ||
            !st->world->addComponent<BenchVel<laige::sim::Fpx16_16>>(entity, vel)
                 .ok()) {
          simTickFail("addComponent failed");
        }
      } else {
        laige::Position2DFp32 pos{};
        pos.pos = laige::sim::SimMathFp32::Vec2{
            static_cast<float>(x), static_cast<float>(y)};
        BenchVel<laige::sim::Fp32Pinned> vel{};
        vel.v =
            laige::sim::SimMathFp32::Vec2{static_cast<float>(vx),
                                          static_cast<float>(vy)};
        if (!st->world->addComponent<laige::Position2DFp32>(entity, pos).ok() ||
            !st->world->addComponent<BenchVel<laige::sim::Fp32Pinned>>(
                 entity, vel)
                 .ok()) {
          simTickFail("addComponent failed");
        }
      }
    }
  }

  if (!st->world->scheduleSystems(st->schedule).ok()) {
    simTickFail("scheduleSystems failed");
  }

  laige::GameLoop::Options loopOptions{};
  loopOptions.tickRateHz = kSimTickTickRateHz;
  loopOptions.nowNs = &simClockNowNs;
  auto l = laige::GameLoop::create(*st->world, st->schedule,
                                   std::move(loopOptions));
  if (!l.ok()) {
    simTickFail("GameLoop::create failed: " +
                   std::string(laige::errorName(l.error())));
  }
  st->loop = std::make_unique<laige::GameLoop>(std::move(l).takeValue());

  // Prime the loop: the first frame() establishes the start reference
  // and runs zero ticks (M1-LOOP-01) — every later frame runs exactly
  // one tick on the exact synthetic clock.
  if (!st->loop->frame().ok()) {
    simTickFail("the loop's first frame failed");
  }
  g_simTick = std::move(st);
  return true;
}

// One measured iteration = one completed tick: advance the synthetic
// clock by exactly one due tick and run one frame (the GameLoop's
// bounded tick dispatch — beginFrame + runSystems + the loop's
// bookkeeping; the engine's G-R1 per-tick zero-allocation assertion
// arms around the tick body in debug non-sanitizer builds).
void simTickIteration() {
  SimTickState& st = *g_simTick;
  g_simClockNs += kSimTickNs;
  if (!st.loop->frame().ok()) {
    simTickFail("a frame failed (stale schedule or failed tick)");
  }
  // Track the loop's own completed-tick count (read after the frame —
  // during tick T the systems still see T-1: the stateHash parameter's
  // documented contract).
  st.completedTicks = st.loop->stats().ticks;
}

void simTickTeardown() {
  SimTickState& st = *g_simTick;
  // The final state hash (after every tick of the run — warmup +
  // measured): a pure const read, the run's determinism fingerprint
  // (a pure function of the workload and backend — the ARCH-010
  // scope per backend, ADR 0002).
  const std::uint64_t finalHash = st.world->stateHash(st.completedTicks);
  std::printf(
      "sim-tick: backend=%s entities=%u dynamic=%u rate_hz=%u "
      "ticks=%llu final_hash=0x%016llx\n",
      st.fpx16 ? "fixed_point_16_16" : "float_pinned_32",
      kSimTickEntities, kSimTickDynamic, kSimTickTickRateHz,
      static_cast<unsigned long long>(st.completedTicks),
      static_cast<unsigned long long>(finalHash));
  // Ordered teardown (CONC-006, the M1-HEAD-01 engine shutdown order):
  // stop the loop (it holds a non-owning world view) before clearing.
  st.loop.reset();
  static_cast<void>(st.world->clear());
  g_simTick.reset();  // releases schedule + world + the state
}

// --- The render-50k suite (M2-PERF-01; PRD 8.1 "50k visible sprites") -------

// The workload constants (CORE-005, named — the PRD §8.1 reference
// values and the M2-SCENE-01 scene shape):
//   Full variant       the §8.1 workload scene (M2-SCENE-01: 50 000
//                      sprites, 65 536 tiles, 3 parallax layers,
//                      8 emitters, 2 UI widgets);
//   1280 x 720         the M2-GOLD-01 reference resolution — the
//                      offscreen FBO size;
//   0x1F055EED         the repo-wide test-seed convention (docs/
//                      testing.md §4; methodology §6);
//   1 / 512            the Dimetric2To1 NDC scale (matrices.h): the
//                      scene's 256-tile ground spans 512·k NDC, so
//                      k = 1/512 fits the ground to the screen width
//                      exactly (dyadic — exactly representable);
//   64                the UI batcher's capacity (the scene's 2 widget
//                      quads + margin — the M2-SCENE-01 ui-pass shape);
//   16 x 16           the solid atlases' texel size (the scene carries
//                      no image data — the GL smoke tests' solidAtlas
//                      convention; the setup path only).
constexpr std::int32_t kRender50Width = 1280;   // the M2-GOLD-01 reference
constexpr std::int32_t kRender50Height = 720;   // resolution (FBO size)
constexpr std::uint32_t kRender50UiCapacity = 64;
constexpr std::uint32_t kRender50AtlasTexels = 16;
constexpr float kRender50CameraScale = 1.0f / 512.0f;
constexpr std::uint32_t kRender50Seed = 0x1F055EED;

// The suite's per-run state, per SimMath backend (the world is a
// backend template — the sim-tick state's bool-dispatch shape folded
// into two typed states). The setup path performs the workload's
// allocations (the world, the batchers, the renderer's instance
// buffer, the GL resources); every frame after setup allocates
// NOTHING in the measured window (the allocation watch makes that a
// property, not a claim — the sim-tick G-R1 precedent).
template <typename Backend>
struct Render50State {
  laige::render::IsoReferenceSceneWorld<Backend> world;
  // The GL resources live AFTER the context in declaration order only
  // so the REVERSED destruction order destroys them first (the
  // renderer holds a non-owning context view — sprite_renderer.h
  // Ownership).
  laige::render::GlContext ctx;
  std::unique_ptr<laige::render::SpriteBatcher> worldBatcher;
  std::unique_ptr<laige::render::SpriteBatcher> uiBatcher;
  std::unique_ptr<laige::render::SpriteRenderer> worldRenderer;
  std::unique_ptr<laige::render::SpriteRenderer> uiRenderer;
  laige::render::IsoCamera camera;
  laige::render::Mat4 worldToNdc{};  // built once (the static camera)
  bool glAvailable = true;  // false: CPU-only mode (no offscreen GL)
};

Render50State<laige::sim::Fpx16_16> g_render50Fpx;
Render50State<laige::sim::Fp32Pinned> g_render50Fp32;
bool g_render50FpxActive = false;

// The suite's second quantities (the file header): the batch-stage
// CPU window (ms) and the frame's draw-call count (world + UI pass).
// Both are capacity-runs rolling windows recorded on EVERY iteration
// (warmup included) — the window holds exactly the measured region's
// last `runs` samples (the Histogram's drop-oldest contract). Created
// in setup (Histogram has no default constructor — the fixed capacity
// IS the configuration).
struct Render50Histograms {
  std::unique_ptr<laige::Histogram> cpu;
  std::unique_ptr<laige::Histogram> drawCalls;
};
Render50Histograms g_render50Hists;

// The Suite table's histogramFor hook (below): the suite-owned
// histogram for a budget name this suite records.
const laige::Histogram* render50HistogramFor(std::string_view name) {
  if (name == "sprites_50k_cpu") return g_render50Hists.cpu.get();
  if (name == "sprites_50k_draw_calls")
    return g_render50Hists.drawCalls.get();
  return nullptr;
}

// Loud benchmark failure (the simTickFail precedent): the workload's
// contract broke (a failed declare/build/submit, a batch-stage
// allocation). Exit 1 — distinct from the budget-check 2.
[[noreturn]] void render50Fail(const std::string& what) {
  std::fprintf(stderr, "laige-bench: render-50k failed: %s\n",
               what.c_str());
  std::fflush(stderr);
  std::exit(1);
}

// One solid 16x16 RGBA8 atlas (deterministic per-id color — the
// content is irrelevant to the budget: the frame's cost is a function
// of the quad count, not the texels).
std::vector<std::uint8_t> render50SolidAtlas(std::uint32_t id) {
  std::vector<std::uint8_t> d(kRender50AtlasTexels * kRender50AtlasTexels *
                              4u);
  const std::uint8_t r = static_cast<std::uint8_t>(10 + 37 * id);
  const std::uint8_t g = static_cast<std::uint8_t>(20 + 53 * id);
  const std::uint8_t b = static_cast<std::uint8_t>(30 + 91 * id);
  for (std::size_t i = 0; i < d.size(); i += 4) {
    d[i + 0] = r;
    d[i + 1] = g;
    d[i + 2] = b;
    d[i + 3] = 255;
  }
  return d;
}

template <typename Backend>
bool render50SetupImpl(const Config& cfg, Render50State<Backend>& st) {
  // The §8.1 workload scene (M2-SCENE-01, the repo-wide seed).
  const laige::render::IsoReferenceScene scene =
      laige::render::buildIsoReferenceScene(kRender50Seed,
                                            laige::render::IsoRefSceneVariant::Full);
  auto worldResult = laige::render::loadIsoReferenceScene<Backend>(scene);
  if (!worldResult.ok()) {
    render50Fail(std::string("loadIsoReferenceScene failed: ") +
                 laige::errorName(worldResult.error()));
  }
  st.world = std::move(worldResult).takeValue();
  if (!st.world.valid()) {
    render50Fail("the loaded world is stopped");
  }
  // The UI tree lays out ONCE (the scene's 2 widgets are static —
  // declareUi requires the layout; the scene test's setup shape).
  if (!st.world.ui().layout().ok()) {
    render50Fail("the UI tree layout failed");
  }

  // The world batcher: the scene's worst-case frame count (50k sprites
  // + 65k tiles + 4 quads per parallax layer) PLUS the particle pool's
  // live cap (the header's frame-budget contract).
  const std::uint32_t worldCapacity =
      st.world.worstCaseFrameCount() + st.world.particles().maxParticles();
  {
    laige::render::SpriteBatcher::Options o{};
    o.maxSprites = worldCapacity;
    auto r = laige::render::SpriteBatcher::create(o);
    if (!r.ok()) render50Fail("the world batcher create failed");
    st.worldBatcher =
        std::make_unique<laige::render::SpriteBatcher>(std::move(r).takeValue());
  }
  {
    laige::render::SpriteBatcher::Options o{};
    o.maxSprites = kRender50UiCapacity;
    auto r = laige::render::SpriteBatcher::create(o);
    if (!r.ok()) render50Fail("the UI batcher create failed");
    st.uiBatcher =
        std::make_unique<laige::render::SpriteBatcher>(std::move(r).takeValue());
  }

  // The offscreen GL 3.3 context (the CI headless mechanism — Mesa
  // surfaceless EGL, M2-GL-01). A host without it runs CPU-ONLY
  // (loud, the suite header's contract): the batch-stage window and
  // the zero-allocation still measure; the draw-call histogram stays
  // empty.
  auto cr = laige::render::GlContext::createHeadless(
      kRender50Width, kRender50Height);
  if (cr.ok()) {
    st.ctx = std::move(cr).takeValue();
    // The submit stages: one per pass. The world pass carries the
    // scene's 9 atlases (0..8, the atlas-id plan); the UI pass its
    // widget atlas (100). maxDrawCalls stays the documented 64 (2x
    // the scene's worst-case frame draw calls — no G-R2 warn fires).
    {
      laige::render::SpriteRenderer::Options o{};
      o.maxInstances = worldCapacity;
      o.maxAtlases = laige::render::kIsoRefSceneFgAtlas + 1u;
      auto r = laige::render::SpriteRenderer::create(st.ctx, o);
      if (!r.ok()) render50Fail("the world renderer create failed");
      st.worldRenderer = std::make_unique<laige::render::SpriteRenderer>(
          std::move(r).takeValue());
      for (std::uint32_t id = laige::render::kIsoRefSceneBgAtlas;
           id <= laige::render::kIsoRefSceneFgAtlas; ++id) {
        const std::vector<std::uint8_t> atlas = render50SolidAtlas(id);
        if (!st.worldRenderer
                 ->bindAtlas(id, kRender50AtlasTexels, kRender50AtlasTexels,
                             atlas)
                 .ok()) {
          render50Fail("bindAtlas failed for the world atlas " +
                       std::to_string(id));
        }
      }
    }
    {
      laige::render::SpriteRenderer::Options o{};
      o.maxInstances = kRender50UiCapacity;
      // The scene's sparse atlas-id plan (0..8 world, 100 widget,
      // 101 font — iso_reference_scene.h): the registry's domain is
      // [0, maxAtlases), so the UI pass must cover id 100.
      o.maxAtlases = laige::render::kIsoRefSceneUiFontAtlas + 1u;
      auto r = laige::render::SpriteRenderer::create(st.ctx, o);
      if (!r.ok()) render50Fail("the UI renderer create failed");
      st.uiRenderer = std::make_unique<laige::render::SpriteRenderer>(
          std::move(r).takeValue());
      const std::vector<std::uint8_t> atlas =
          render50SolidAtlas(laige::render::kIsoRefSceneUiWidgetAtlas);
      if (!st.uiRenderer
               ->bindAtlas(laige::render::kIsoRefSceneUiWidgetAtlas,
                           kRender50AtlasTexels, kRender50AtlasTexels,
                           atlas)
               .ok()) {
        render50Fail("bindAtlas failed for the UI widget atlas");
      }
    }
  } else {
    st.glAvailable = false;
    std::fprintf(stderr,
                 "laige-bench: render-50k: no offscreen GL 3.3 context (%s) "
                 "— running CPU-only (the draw-call budget is NOT measured)\n",
                 laige::errorText(cr.error()));
    std::fflush(stderr);
  }

  // The static camera: the Dimetric2To1 preset (ADR 0005 default)
  // centered on the ground's 256 x 256 grid center; the matrix is
  // built ONCE (the camera never moves — "once per frame change",
  // iso_camera.h). The parallax declare's camera position is the
  // camera's ground position (the two agree: the screen center).
  laige::render::IsoCameraOptions camOpts{};
  camOpts.camera.position = glm::vec3{128.0f, 128.0f, 0.0f};
  camOpts.camera.target = glm::vec3{128.0f, 128.0f, -1.0f};
  camOpts.preset = laige::render::IsoPreset{
      laige::render::IsoPresetKind::Dimetric2To1, kRender50CameraScale, {}};
  auto cam = laige::render::IsoCamera::create(camOpts);
  if (!cam.ok()) {
    render50Fail(std::string("IsoCamera::create failed: ") +
                 laige::errorName(cam.error()));
  }
  st.camera = std::move(cam).takeValue();
  st.worldToNdc = st.camera.matrix();

  // The suite's histograms (capacity = runs: every measured sample is
  // kept — the no-truncation contract).
  g_render50Hists.cpu =
      std::make_unique<laige::Histogram>(laige::Histogram::Options{cfg.runs});
  g_render50Hists.drawCalls =
      std::make_unique<laige::Histogram>(laige::Histogram::Options{cfg.runs});
  return true;
}

bool render50Setup(const Config& cfg) {
  if (cfg.mathFpx16) {
    g_render50FpxActive = true;
    return render50SetupImpl<laige::sim::Fpx16_16>(cfg, g_render50Fpx);
  }
  g_render50FpxActive = false;
  return render50SetupImpl<laige::sim::Fp32Pinned>(cfg, g_render50Fp32);
}

// One measured iteration = one offscreen frame (the suite header):
// the sim-side pacing (OUTSIDE the window), the measured batch-stage
// window, the zero-allocation read, then the submit stage's offscreen
// render (OUTSIDE the window — its cost is the frame protocol's
// separate phase, RENDER-005's CPU/GPU split; the M2-PROF-01
// profiler measures it, not this budget).
template <typename Backend>
void render50IterationImpl(Render50State<Backend>& st) {
  // The sim-side pacing: one particle update per frame (ARCH-002:
  // the sim advances in whole ticks; at the workload's 60 Hz frame
  // cadence that is one tick per frame). OUTSIDE the measured window
  // (the sim tick is the `sim_tick` budget's territory).
  st.world.particles().update();

  const laige::render::Vec2 cameraPos{128.0f, 128.0f};  // static camera

  // The measured window: the frame's batch stage (both passes).
  // The allocation watch arms around it (M1-ALLOC-01 extended to the
  // batcher — the G-R1 per-tick assertion's shape): a window with a
  // nonzero count is a contract violation, not a measurement.
  laige::allocWatchArm();
  laige::TimeIt timer;
  st.worldBatcher->beginFrame();
  const laige::Status d = laige::render::declareIsoReferenceScene<Backend>(
      st.world, *st.worldBatcher, cameraPos);
  if (!d.ok()) {
    render50Fail(std::string("declareIsoReferenceScene failed: ") +
                 laige::errorName(d.error()));
  }
  const laige::Status wb = st.worldBatcher->build();
  if (!wb.ok()) {
    render50Fail(std::string("the world batcher build failed: ") +
                 laige::errorName(wb.error()));
  }
  st.uiBatcher->beginFrame();
  laige::render::UiDeclareOptions uiOpts{};  // no default font (the scene's
                                             // label-less world pass)
  const laige::Status u = laige::render::declareUi(
      *st.uiBatcher, st.world.strings(), st.world.ui(), uiOpts);
  if (!u.ok()) {
    render50Fail(std::string("declareUi failed: ") +
                 laige::errorName(u.error()));
  }
  const laige::Status ub = st.uiBatcher->build();
  if (!ub.ok()) {
    render50Fail(std::string("the UI batcher build failed: ") +
                 laige::errorName(ub.error()));
  }
  const double windowMs = timer.elapsedMs();
  const laige::AllocWatchReading reading = laige::allocWatchRead();
  if (laige::allocWatchLive() && reading.allocs != 0u) {
    render50Fail("the batch stage allocated " +
                 std::to_string(reading.allocs) +
                 " heap block(s) this frame (first site " +
                 (reading.firstSite != nullptr ? "captured" : "unknown") +
                 ") — FR-2.2 zero-allocation contract");
  }
  g_render50Hists.cpu->record(windowMs);

  // The submit stage: the offscreen render of the built frame (the
  // frame order: world pass, then the UI pass on top). The draw-call
  // count is the engine's own bookkeeping (RENDER-001 — one instanced
  // draw per (atlas, material, blend) group per pass).
  if (st.glAvailable) {
    const laige::Status ws =
        st.worldRenderer->submit(*st.worldBatcher, st.worldToNdc);
    if (!ws.ok()) {
      render50Fail(std::string("the world submit failed: ") +
                   laige::errorName(ws.error()));
    }
    const laige::Status us = st.uiRenderer->submit(
        *st.uiBatcher,
        laige::render::uiScreenMatrix(kRender50Width, kRender50Height));
    if (!us.ok()) {
      render50Fail(std::string("the UI submit failed: ") +
                   laige::errorName(us.error()));
    }
    const std::uint32_t draws =
        st.worldRenderer->frameStats().drawCalls +
        st.uiRenderer->frameStats().drawCalls;
    g_render50Hists.drawCalls->record(static_cast<double>(draws));
  }
}

void render50Iteration() {
  if (g_render50FpxActive) {
    render50IterationImpl<laige::sim::Fpx16_16>(g_render50Fpx);
  } else {
    render50IterationImpl<laige::sim::Fp32Pinned>(g_render50Fp32);
  }
}

template <typename Backend>
void render50TeardownImpl(const Render50State<Backend>& st, const char* name) {
  const laige::HistogramStats cpu = g_render50Hists.cpu->stats();
  std::uint32_t drawsMax = 0;
  if (st.glAvailable) {
    drawsMax =
        static_cast<std::uint32_t>(g_render50Hists.drawCalls->stats().max);
  }
  // The machine-greppable identity line (the sim-tick fingerprint
  // precedent): the workload shape + the run's measured extremes.
  std::printf(
      "render-50k: backend=%s variant=full sprites=%llu tiles=%llu "
      "parallax=%llu emitters=%llu ui_widgets=%llu resolution=%ux%u "
      "mode=%s draw_calls=%u frames=%llu cpu_min_ms=%.6f cpu_max_ms=%.6f\n",
      name,
      static_cast<unsigned long long>(st.world.scene().spriteCount),
      static_cast<unsigned long long>(st.world.scene().tileCount),
      static_cast<unsigned long long>(st.world.scene().parallaxCount),
      static_cast<unsigned long long>(st.world.scene().emitterCount),
      static_cast<unsigned long long>(st.world.scene().uiWidgetCount),
      kRender50Width, kRender50Height,
      st.glAvailable ? "gl" : "cpu",
      drawsMax,
      static_cast<unsigned long long>(cpu.n),
      cpu.min, cpu.max);
  std::fflush(stdout);
  // Ordered teardown (CONC-006): the renderers (they hold context
  // views) before the batchers and the context; the world (no GL)
  // last. The state's member declaration order guarantees the
  // reverse-destruction order above on scope exit.
}

void render50Teardown() {
  if (g_render50FpxActive) {
    render50TeardownImpl(g_render50Fpx, "fixed_point_16_16");
  } else {
    render50TeardownImpl(g_render50Fp32, "float_pinned_32");
  }
}

// --- The ac4 suite (M2-AC-01: PRD §4 AC-4.3 + AC-4.4, budgets
//     sprites_10k_frame) ------------------------------------------------------
//
// The AC-4.3 workload: 10 000 overlapping sprites sorted and drawn in
// one 60 FPS frame (16.7 ms, the §8.1 60 FPS frame budget). The sprites
// are stacked K deep over a 100 x 10 cell grid: 1000 cells at their
// centers (gx - 49.5, gy - 4.5), 10 sprites per cell at the SAME
// position and step height ((gx >> 4) + (gy >> 4), 0..6 — the G-R11
// engine-owned isoDepthKey, 10-way equal-key ties per stack, broken by
// declaration order per RENDER-003), scale 1.0, atlas k mod 2, Alpha
// for k < 5 else Additive: four (atlas, material, blend) groups —
// 3000 / 2000 / 2000 / 3000 instances — so the frame is exactly four
// instanced draw calls (RENDER-001). The grid's 1x1 tiles edge-touch on
// screen and each stack overlaps itself 10 times: the sort's
// equal-key ties AND the overdraw are both exercised.
//
// One measured iteration = one frame (the AC-4.3 "sorted + drawn at
// 60 FPS" — the whole frame, batch stage + submit stage): the batch
// stage (beginFrame + 10 000 adds + build) with the M1-ALLOC-01
// zero-allocation watch armed around it (a frame that allocates
// aborts, the render-50k batch-stage assertion's shape), then the
// submit stage's offscreen render. The `sprites_10k_frame` sample is
// the whole frame's CPU ms; it is recorded only when an offscreen GL
// 3.3 context is available — on a host without one the frame histogram
// stays empty and a named budget check against it fails NO_SAMPLES
// (loud, the render-50k draw-call precedent; the batch-stage
// zero-allocation still measures CPU-only).
//
// The AC-4.4 engine-default assertion lives in setup (the suite is the
// "asserted here" of the AC-4.4 scope): ProjectionView{}'s default
// mode MUST be Iso (ADR 0005) — a default drift is a loud failure,
// not a baseline shift. The world matrix is the Iso default's
// documented builder (isoDimetric2To1) at the grid-fit scale.
//
// The `render-sort-10k` suite entry is the same workload under the
// dedicated suite name the AC-4.3 scope uses; `ac4` is the acceptance
// name the CI wiring uses (--suite=ac4, ctest -R ac4).

constexpr std::uint32_t kAc4Sprites = 10000;    // the AC-4.3 sprite count
constexpr std::uint32_t kAc4GridCols = 100;     // the x cell count
constexpr std::uint32_t kAc4GridRows = 10;      // the y cell count
constexpr std::uint32_t kAc4StackPerCell = 10;  // sprites stacked per cell
constexpr float kAc4BenchIsoScale = 1.0f / 128.0f;  // the grid's NDC fit
                                               // NDC x = s(2x - y) in
                                               // [-0.83203125, 0.83203125],
                                               // NDC y = -s(x + y) in
                                               // [-0.4375, 0.4375], over the
                                               // 1x1 tiles' quad corners
                                               // (x' in [-50.5, 50.5],
                                               // y' in [-5.5, 5.5])
constexpr float kAc4SpriteScale = 1.0f;        // the 1x1 world tile

// The float cell center -> the backend's Vec2 (the RENDER-006 load
// boundary, the iso_reference_scene.h isoRefBackendVec2 pattern):
// exact for the workload's dyadic half-integer centers in both
// backends (|v| < 2^23, Q16.16 exact).
template <typename Backend>
typename laige::sim::SimMath<Backend>::Vec2 ac4BackendVec2(float x,
                                                           float y) {
  using laige::sim::Fp32Pinned;
  if constexpr (std::is_same_v<Backend, Fp32Pinned>) {
    return typename laige::sim::SimMath<Backend>::Vec2{x, y};
  } else {
    return typename laige::sim::SimMath<Backend>::Vec2{
        laige::fpx16_16::fromFloat(x), laige::fpx16_16::fromFloat(y)};
  }
}

// The workload's items for one SimMath backend (the depth keys are
// backend-quantized — the per-backend state, the render-50k shape).
template <typename Backend>
std::vector<laige::render::SpriteItem> ac4Items() {
  using laige::render::BlendMode;
  using laige::render::SpriteItem;
  using laige::render::isoDepthKey;

  std::vector<SpriteItem> items;
  items.reserve(kAc4Sprites);
  for (std::uint32_t gy = 0; gy < kAc4GridRows; ++gy) {
    for (std::uint32_t gx = 0; gx < kAc4GridCols; ++gx) {
      const float x =
          static_cast<float>(gx) - static_cast<float>(kAc4GridCols) / 2.0f
          + 0.5f;  // the cell center, in -49.5..49.5
      const float y =
          static_cast<float>(gy) - static_cast<float>(kAc4GridRows) / 2.0f
          + 0.5f;  // the cell center, in -4.5..4.5
      const int stepHeight =
          static_cast<int>(gx >> 4) + static_cast<int>(gy >> 4);
      for (std::uint32_t k = 0; k < kAc4StackPerCell; ++k) {
        SpriteItem it;
        it.pos = {x, y};
        it.depthKey = isoDepthKey<Backend>(ac4BackendVec2<Backend>(x, y),
                                           stepHeight, /*layer=*/0);
        it.uv = {0.0f, 0.0f, 1.0f, 1.0f};
        it.scale = {kAc4SpriteScale, kAc4SpriteScale};
        it.rotation = 0.0f;
        it.tint = {1.0f, 1.0f, 1.0f, 1.0f};
        it.atlasId = k % 2u;
        it.materialId = 0;
        it.blend = (k < 5u) ? BlendMode::Alpha : BlendMode::Additive;
        items.push_back(it);
      }
    }
  }
  return items;
}

struct Ac4State {
  std::vector<laige::render::SpriteItem> items;
  laige::render::Mat4 worldToNdc{};  // built once (the static matrix)
  bool glAvailable = true;  // false: CPU-only mode (no offscreen GL)
  // The GL resources live AFTER the context in declaration order only
  // so the REVERSED destruction order destroys them first (the
  // renderer holds a non-owning context view).
  laige::render::GlContext ctx;
  std::unique_ptr<laige::render::SpriteBatcher> batcher;
  std::unique_ptr<laige::render::SpriteRenderer> renderer;
};

Ac4State g_ac4Fpx;
Ac4State g_ac4Fp32;
bool g_ac4FpxActive = false;

// The suite's quantities (the section header): the whole frame's CPU
// ms (batch + submit) and the frame's draw-call count. Both record on
// EVERY iteration (warmup included) and only while GL is available
// (a CPU-only run leaves them empty — the NO_SAMPLES loud-fail path).
struct Ac4Histograms {
  std::unique_ptr<laige::Histogram> frame;
  std::unique_ptr<laige::Histogram> drawCalls;
};
Ac4Histograms g_ac4Hists;

const laige::Histogram* ac4HistogramFor(std::string_view name) {
  if (name == "sprites_10k_frame") return g_ac4Hists.frame.get();
  return nullptr;
}

[[noreturn]] void ac4Fail(const std::string& what) {
  std::fprintf(stderr, "laige-bench: ac4 failed: %s\n", what.c_str());
  std::fflush(stderr);
  std::exit(1);
}

template <typename Backend>
bool ac4SetupImpl(const Config& cfg, Ac4State& st) {
  // The AC-4.4 engine-default assertion (asserted HERE, the AC-4.4
  // scope's wording): the default ProjectionView's mode is Iso (ADR
  // 0005) — a default drift breaks new-project templates (the
  // isometric-first-class set) and is a loud failure.
  const laige::render::ProjectionView view{};
  if (view.mode != laige::render::ProjectionMode::Iso) {
    ac4Fail("the engine's default projection is not Iso (AC-4.4: the "
            "default ProjectionView.mode drifted from ProjectionMode::Iso)");
  }

  st.items = ac4Items<Backend>();
  st.worldToNdc = laige::render::isoDimetric2To1(kAc4BenchIsoScale);

  {
    laige::render::SpriteBatcher::Options o{};
    o.maxSprites = kAc4Sprites;
    auto r = laige::render::SpriteBatcher::create(o);
    if (!r.ok()) ac4Fail("the world batcher create failed");
    st.batcher =
        std::make_unique<laige::render::SpriteBatcher>(std::move(r).takeValue());
  }

  // The offscreen GL 3.3 context (the CI headless mechanism — Mesa
  // surfaceless EGL, the render-50k precedent). A host without it runs
  // CPU-ONLY (loud): the batch-stage zero-allocation still measures;
  // the frame histogram stays empty (the NO_SAMPLES loud-fail).
  auto cr = laige::render::GlContext::createHeadless(kRender50Width,
                                                     kRender50Height);
  if (cr.ok()) {
    st.ctx = std::move(cr).takeValue();
    laige::render::SpriteRenderer::Options o{};
    o.maxInstances = kAc4Sprites;
    o.maxAtlases = 2;  // the workload's atlas ids 0..1
    auto r = laige::render::SpriteRenderer::create(st.ctx, o);
    if (!r.ok()) ac4Fail("the world renderer create failed");
    st.renderer = std::make_unique<laige::render::SpriteRenderer>(
        std::move(r).takeValue());
    for (std::uint32_t id = 0; id < 2u; ++id) {
      const std::vector<std::uint8_t> atlas = render50SolidAtlas(id);
      if (!st.renderer
               ->bindAtlas(id, kRender50AtlasTexels, kRender50AtlasTexels,
                           atlas)
               .ok()) {
        ac4Fail("bindAtlas failed for the ac4 atlas " + std::to_string(id));
      }
    }
  } else {
    st.glAvailable = false;
    std::fprintf(stderr,
                 "laige-bench: ac4: no offscreen GL 3.3 context (%s) — "
                 "running CPU-only (the frame budget is NOT measured)\n",
                 laige::errorText(cr.error()));
    std::fflush(stderr);
  }

  g_ac4Hists.frame =
      std::make_unique<laige::Histogram>(laige::Histogram::Options{cfg.runs});
  g_ac4Hists.drawCalls =
      std::make_unique<laige::Histogram>(laige::Histogram::Options{cfg.runs});
  return true;
}

bool ac4Setup(const Config& cfg) {
  if (cfg.mathFpx16) {
    g_ac4FpxActive = true;
    return ac4SetupImpl<laige::sim::Fpx16_16>(cfg, g_ac4Fpx);
  }
  g_ac4FpxActive = false;
  return ac4SetupImpl<laige::sim::Fp32Pinned>(cfg, g_ac4Fp32);
}

// One measured iteration = one frame (the section header): the batch
// stage with the zero-allocation watch armed around it, then the
// submit stage's offscreen render; the whole frame's CPU ms is the
// `sprites_10k_frame` sample (GL runs only).
void ac4Iteration(Ac4State& st) {
  laige::allocWatchArm();
  laige::TimeIt timer;
  st.batcher->beginFrame();
  for (const laige::render::SpriteItem& it : st.items) {
    const laige::Result<std::uint32_t> a = st.batcher->add(it);
    if (!a.ok()) {
      ac4Fail(std::string("batcher.add failed: ") + laige::errorName(a.error()));
    }
  }
  const laige::Status wb = st.batcher->build();
  if (!wb.ok()) {
    ac4Fail(std::string("the world batcher build failed: ") +
            laige::errorName(wb.error()));
  }
  const laige::AllocWatchReading reading = laige::allocWatchRead();
  if (laige::allocWatchLive() && reading.allocs != 0u) {
    ac4Fail("the batch stage allocated " +
            std::to_string(reading.allocs) +
            " heap block(s) this frame (first site " +
            (reading.firstSite != nullptr ? "captured" : "unknown") +
            ") — FR-2.2 zero-allocation contract");
  }
  if (st.glAvailable) {
    const laige::Status ws =
        st.renderer->submit(*st.batcher, st.worldToNdc);
    if (!ws.ok()) {
      ac4Fail(std::string("the world submit failed: ") +
              laige::errorName(ws.error()));
    }
    g_ac4Hists.frame->record(timer.elapsedMs());
    g_ac4Hists.drawCalls->record(
        static_cast<double>(st.renderer->frameStats().drawCalls));
  }
}

void ac4IterationDispatch() {
  if (g_ac4FpxActive) {
    ac4Iteration(g_ac4Fpx);
  } else {
    ac4Iteration(g_ac4Fp32);
  }
}

void ac4TeardownImpl(const Ac4State& st, const char* name) {
  // The machine-greppable identity line (the render-50k fingerprint
  // precedent): the workload shape + the run's measured extremes. The
  // frame statistics are printed only for GL runs — a CPU-only run
  // records no frame samples (the NO_SAMPLES loud-fail, the
  // render-50k draw-call precedent).
  if (st.glAvailable) {
    const laige::HistogramStats frame = g_ac4Hists.frame->stats();
    const std::uint32_t drawsMax =
        static_cast<std::uint32_t>(g_ac4Hists.drawCalls->stats().max);
    // The RENDER-001 group count is part of the AC-4.3 contract (the
    // four (atlas, material, blend) groups — one instanced draw each).
    if (drawsMax != 4u) {
      ac4Fail("the frame drew " + std::to_string(drawsMax) +
              " draw call(s) — the four (atlas, material, blend) groups "
              "must draw exactly four (RENDER-001)");
    }
    std::printf(
        "ac4: backend=%s sprites=%llu grid=%ux%u stack=%u groups=4 "
        "default=Iso resolution=%ux%u mode=gl draw_calls=%u frames=%llu "
        "frame_min_ms=%.6f frame_max_ms=%.6f\n",
        name,
        static_cast<unsigned long long>(st.items.size()),
        kAc4GridCols, kAc4GridRows, kAc4StackPerCell,
        kRender50Width, kRender50Height,
        drawsMax,
        static_cast<unsigned long long>(frame.n),
        frame.min, frame.max);
  } else {
    std::printf(
        "ac4: backend=%s sprites=%llu grid=%ux%u stack=%u groups=4 "
        "default=Iso resolution=%ux%u mode=cpu\n",
        name,
        static_cast<unsigned long long>(st.items.size()),
        kAc4GridCols, kAc4GridRows, kAc4StackPerCell,
        kRender50Width, kRender50Height);
  }
  std::fflush(stdout);
}

void ac4Teardown() {
  if (g_ac4FpxActive) {
    ac4TeardownImpl(g_ac4Fpx, "fixed_point_16_16");
  } else {
    ac4TeardownImpl(g_ac4Fp32, "float_pinned_32");
  }
}

// --- The suite table ---------------------------------------------------------

struct Suite {
  const char* name;
  const char* description;  // printed with --list; docs live in this file
  bool (*setup)(const Config&);  // nullptr: no setup (stateless suite)
  void (*iteration)();
  void (*teardown)();  // nullptr: no teardown
  // The suite-owned histogram for a budget name the suite records
  // (the render-50k suite measures two quantities per frame — the
  // batch-stage CPU window and the frame's draw-call count — that the
  // one generic timer cannot carry): a `--budget=<name>` check against
  // one of these uses the returned histogram instead of the generic
  // iteration-timer histogram. nullptr = no suite histograms.
  const laige::Histogram* (*histogramFor)(std::string_view budgetName) =
      nullptr;
};

const Suite kSuites[] = {
    {"synthetic",
     "deterministic 4096-step LCG+double pipeline; harness stand-in "
     "workload (M0; the M0-EXIT-01 baseline workload)",
     nullptr, syntheticIteration, nullptr},
    {"sim-tick",
     "PRD 8.1 simulation tick: 10k entities, 2k dynamic bodies "
     "(Position2D + velocity), 60 Hz, systems BenchMove + BenchHash "
     "(state hash) — the M1-BENCH-01 workload, both SimMath backends "
     "(--math)",
     simTickSetup, simTickIteration, simTickTeardown},
    {"render-50k",
     "PRD 8.1 50k visible sprites: the FULL M2-SCENE-01 reference "
     "scene (50k sprites, 65k tiles, 3 parallax layers, particles, "
     "UI) rendered offscreen at 1280x720 — the batch-stage CPU "
     "(sprites_50k_cpu) + the frame's draw calls "
     "(sprites_50k_draw_calls) + the batcher zero-allocation "
     "assertion; both SimMath backends (--math)",
     render50Setup, render50Iteration, render50Teardown,
     render50HistogramFor},
    {"ac4",
     "M2-AC-01 AC-4.x acceptance: 10 000 overlapping sprites "
     "(100 x 10 grid, 10 stacked per cell — equal-key ties + "
     "overdraw) sorted + drawn in one 60 FPS frame "
     "(sprites_10k_frame) + the batcher zero-allocation assertion + "
     "the AC-4.4 engine-default projection assert (Iso); both "
     "SimMath backends (--math). Same workload as render-sort-10k.",
     ac4Setup, ac4IterationDispatch, ac4Teardown, ac4HistogramFor},
    {"render-sort-10k",
     "PRD 4 AC-4.3 dedicated suite: 10 000 overlapping sprites "
     "(100 x 10 grid, 10 stacked per cell) sorted + drawn at 60 FPS "
     "(sprites_10k_frame) + the batcher zero-allocation assertion; "
     "both SimMath backends (--math). Same workload as ac4.",
     ac4Setup, ac4IterationDispatch, ac4Teardown, ac4HistogramFor},
};

bool parseUint(std::string_view text, std::uint64_t& out) {
  if (text.empty()) return false;
  std::uint64_t v = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + std::uint64_t(c - '0');
  }
  out = v;
  return true;
}

void usage(std::FILE* out) {
  std::fprintf(out,
               "usage: laige-bench --suite=<name> [--runs=N] "
               "[--warmup=N] [--budget=<budget-name>]... "
               "[--budgets=<path>] "
               "[--math=<fixed_point_16_16|float_pinned_32>] "
               "[--report=<path>] [--list]\n");
}

bool parseArgs(int argc, char** argv, Config& cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    const bool hasValue = eq != std::string::npos && eq + 1 < arg.size();
    const std::string key = hasValue ? arg.substr(0, eq) : arg;
    const std::string value = hasValue ? arg.substr(eq + 1) : "";

    if (key == "--suite") {
      if (!hasValue) return false;
      cfg.suite = value;
    } else if (key == "--runs") {
      if (!hasValue || !parseUint(value, cfg.runs) || cfg.runs == 0)
        return false;
    } else if (key == "--warmup") {
      if (!hasValue || !parseUint(value, cfg.warmup)) return false;
    } else if (key == "--budget") {
      if (!hasValue || value.empty()) return false;
      // Duplicate names are a usage error (strict parsing, API-008):
      // checking one entry twice would print two identical reports.
      for (const std::string& name : cfg.budgetNames) {
        if (name == value) return false;
      }
      cfg.budgetNames.push_back(value);
    } else if (key == "--budgets") {
      if (!hasValue) return false;
      cfg.budgetsPath = value;
    } else if (key == "--math") {
      if (!hasValue) return false;
      if (value == "fixed_point_16_16") {
        cfg.mathFpx16 = true;
      } else if (value == "float_pinned_32") {
        cfg.mathFpx16 = false;
      } else {
        return false;  // unknown backend (ADR 0002's two ids only)
      }
    } else if (key == "--report") {
      if (!hasValue) return false;
      cfg.reportPath = value;
    } else if (key == "--list") {
      for (const Suite& s : kSuites)
        std::printf("%-12s %s\n", s.name, s.description);
      std::exit(0);
    } else {
      return false;  // unknown argument
    }
  }
  return !cfg.suite.empty();  // --suite is required
}

// --- Output ------------------------------------------------------------------

// The compile-time build identity for the AGENTS 12 "build" context
// field (compiler + version + build type). CMake stamps the build type
// via LAIGE_BENCH_BUILD_TYPE.
#define LAIGE_BENCH_STR2(x) #x
#define LAIGE_BENCH_STR(x) LAIGE_BENCH_STR2(x)
// __clang__ is checked BEFORE __GNUC__ (Clang defines the GCC-compat
// macros, and __VERSION__ carries a compiler-specific format that
// would be mis-attributed by the GCC branch — "Clang 22.1.8" on
// recent Clang, "16.2.1 20260810" on GCC). The Clang id is built from
// the version macros: stable across Clang versions regardless of the
// __VERSION__ spelling.
#if defined(__clang__)
constexpr char kCompilerId[] = "Clang " LAIGE_BENCH_STR(__clang_major__) "."
                               LAIGE_BENCH_STR(__clang_minor__) "."
                               LAIGE_BENCH_STR(__clang_patchlevel__);
#elif defined(__GNUC__)
constexpr char kCompilerId[] = "GCC " __VERSION__;
#elif defined(_MSC_VER)
constexpr char kCompilerId[] = "MSVC " LAIGE_BENCH_STR(_MSC_VER);
#else
constexpr char kCompilerId[] = "unknown";
#endif
#ifndef LAIGE_BENCH_BUILD_TYPE
#define LAIGE_BENCH_BUILD_TYPE "unknown"
#endif

// --- Platform boundary (CPP-009, pattern: logging.cpp) ----------------------
//
// MSVC deprecates plain getenv/fopen (C4996, fatal under the engine /WX
// policy, NFR-8.10); the Windows branch uses the CRT's documented
// replacements with the same lookup/open semantics every other supported
// compiler provides. _fsopen(_SH_DENYNO) keeps plain-fopen sharing
// semantics (no _SH_SECURE re-open denial, see logging.cpp).

#if defined(_MSC_VER)
// Largest environment value this tool reads (a machine description or a
// budgets file path; both fit far inside the bound). Named per CORE-005;
// a value beyond it is treated as unset (the documented fallback applies).
// MSVC-only: getenv_s needs a caller-sized buffer, so the constant has no
// use outside this branch (CORE-010: no unused symbols under -Werror).
constexpr std::size_t kEnvValueMaxBytes = 4096;

std::string envValue(const char* name) {
  char buf[kEnvValueMaxBytes];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), name) != 0) return {};
  return std::string(buf, len);
}
std::FILE* openReportFile(const char* path, const char* mode) {
  return ::_fsopen(path, mode, _SH_DENYNO);
}
#else
std::string envValue(const char* name) {
  const char* v = std::getenv(name);
  return (v != nullptr) ? std::string(v) : std::string();
}
std::FILE* openReportFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  using laige::BudgetReportContext;
  using laige::Histogram;
  using laige::TimeIt;

#if defined(__linux__)
  // TEMPORARY DIAGNOSTIC: capture the exit-time segfault stack (the
  // handler is process-wide, so it is still in force during the
  // static-destruction phase after main returns). The main thread id
  // is printed here so the crashed tid in the handler line can be
  // compared against it.
  {
    char line[64];
    std::snprintf(line, sizeof(line),
                  "ac4 diagnostic: main tid=%ld\n",
                  static_cast<long>(::syscall(SYS_gettid)));
    (void)!::write(STDERR_FILENO, line,
                   static_cast<std::size_t>(std::strlen(line)));
  }
  ::signal(SIGSEGV, &Ac4CrashHandler);
  ::signal(SIGBUS, &Ac4CrashHandler);
#endif

  Config cfg;
  if (!parseArgs(argc, argv, cfg)) {
    usage(stderr);
    return 1;
  }

  const Suite* suite = nullptr;
  for (const Suite& s : kSuites)
    if (s.name == cfg.suite) suite = &s;
  if (suite == nullptr) {
    std::fprintf(stderr, "laige-bench: unknown suite '%s' (--list)\n",
                 cfg.suite.c_str());
    return 1;
  }

  // Suite setup (stateful suites only — the workload's world,
  // systems, and loop are built once, before any measured iteration).
  if (suite->setup != nullptr && !suite->setup(cfg)) {
    return 1;  // simTickFail already reported and exits; kept for shape
  }

  // Warm-up (discarded) — the timer and the CPU caches settle before the
  // measured region (AGENTS 12 records the warm-up count in the report).
  for (std::uint64_t i = 0; i < cfg.warmup; ++i) suite->iteration();

  // Measured region: one TimeIt per iteration, recorded into a
  // histogram sized to the run (every sample is kept — no truncation).
  Histogram histogram(Histogram::Options{cfg.runs});
  for (std::uint64_t i = 0; i < cfg.runs; ++i) {
    TimeIt timer;
    suite->iteration();
    histogram.record(timer.elapsedMs());
  }

  if (suite->teardown != nullptr) suite->teardown();

  // Context the tool owns (AGENTS 12: the caller harness records
  // hardware/OS/compiler/build/workload). The operator may set
  // LAIGE_BENCH_MACHINE for the machine line; the baseline document
  // records the rest (docs/benchmarks/, M0-EXIT-01).
  const std::string envMachine = envValue("LAIGE_BENCH_MACHINE");
  const std::string build =
      std::string(kCompilerId) + ", " + LAIGE_BENCH_BUILD_TYPE;

  std::string output;
  output += "suite=";
  output += suite->name;
  output += " runs=";
  output += std::to_string(cfg.runs);
  output += " warmup=";
  output += std::to_string(cfg.warmup);

  int exitCode = 0;
  if (cfg.budgetNames.empty()) {
    // Plain run: the summary statistics block only (the workload line
    // carries the suite's own identity).
    const laige::HistogramStats s = histogram.stats();
    output += "\n  ";
    output += laige::formatStatsLine(s);
    output += "\n  context: workload=";
    output += suite->name;
    output += " build=";
    output += build;
    output += " machine=";
    output += envMachine;  // empty when the env var is unset
    output += " warmup=";
    output += std::to_string(cfg.warmup);
    output += "\n";
  } else {
    // Budget run: load budgets.json, check EVERY named entry against
    // the same histogram (each entry's metric picks its statistic —
    // sim_tick_avg means, sim_tick_p99 p99), print one AGENTS 12
    // report per entry. A single run gates every budget the workload
    // measures (the M1-BENCH-01 gate: avg AND p99, both backends).
    std::string budgetsPath = cfg.budgetsPath;
    if (budgetsPath.empty()) {
      const std::string envPath = envValue("LAIGE_BUDGETS_PATH");
      budgetsPath =
          envPath.empty() ? std::string("budgets.json") : envPath;
    }
    const laige::Result<laige::BudgetTable, laige::ErrorCode> table =
        laige::loadBudgets(budgetsPath);
    if (table.isError()) {
      std::fprintf(stderr, "laige-bench: loadBudgets(\"%s\") failed: %s\n",
                   budgetsPath.c_str(), laige::errorText(table.error()));
      return 1;
    }
    for (const std::string& name : cfg.budgetNames) {
      const laige::BudgetEntry* entry = table.value().find(name);
      if (entry == nullptr) {
        std::fprintf(stderr,
                     "laige-bench: no budget named '%s' in %s\n",
                     name.c_str(), budgetsPath.c_str());
        return 1;
      }

      // The histogram the check evaluates: the suite's own for a
      // budget name it records (the render-50k suite's CPU-window and
      // draw-call quantities), the generic timer histogram otherwise.
      const laige::Histogram* checked = &histogram;
      if (suite->histogramFor != nullptr) {
        const laige::Histogram* own = suite->histogramFor(name);
        if (own != nullptr) checked = own;
      }

      BudgetReportContext ctx;
      ctx.workload = entry->workload.c_str();
      ctx.build = build.c_str();
      ctx.machine = envMachine.c_str();  // outlives the budgetCheck call
      ctx.warmup = static_cast<std::uint32_t>(cfg.warmup);

      const laige::BudgetCheckResult check =
          laige::budgetCheck(*entry, *checked, ctx);
      output += "\n";
      output += check.report;
      if (!check.passed) exitCode = 2;
    }
  }

  std::fputs(output.c_str(), stdout);
  std::fflush(stdout);

  if (!cfg.reportPath.empty()) {
    std::FILE* f = openReportFile(cfg.reportPath.c_str(), "a");
    if (f == nullptr) {
      std::fprintf(stderr, "laige-bench: cannot open report file '%s'\n",
                   cfg.reportPath.c_str());
      return 1;
    }
    std::fputs(output.c_str(), f);
    std::fclose(f);
  }
  return exitCode;
}
