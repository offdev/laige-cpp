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
// 0002) the `sim-tick` suite runs on; the other suites ignore it.
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
//
// The synthetic suite is a deterministic, allocation-free stand-in
// workload; the sim-tick suite is deterministic too (fixed seed
// 0x1F055EED, the repo-wide test-seed convention — docs/testing.md;
// no randomness in the measured path) and, in debug non-sanitizer
// builds, every measured tick additionally passes the engine's own
// G-R1 per-tick zero-allocation assertion (GameLoop::runOneTick arms
// the process-wide allocation watch, M1-ALLOC-01) — an allocating
// tick aborts the run.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/result.h"
#include "laige/sim/component.h"  // LAIGE_COMPONENT
#include "laige/sim/determinism.h"  // LAIGE_DETERMINISM_SAFE
#include "laige/sim/entity.h"
#include "laige/sim/game_loop.h"
#include "laige/sim/presentation.h"  // Position2D
#include "laige/sim_math.h"  // SimMath backends (laige-core)
#include "laige/sim/system.h"  // SystemDef, LAIGE_SYSTEM, Io

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

// --- The suite table ---------------------------------------------------------

struct Suite {
  const char* name;
  const char* description;  // printed with --list; docs live in this file
  bool (*setup)(const Config&);  // nullptr: no setup (stateless suite)
  void (*iteration)();
  void (*teardown)();  // nullptr: no teardown
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

      BudgetReportContext ctx;
      ctx.workload = entry->workload.c_str();
      ctx.build = build.c_str();
      ctx.machine = envMachine.c_str();  // outlives the budgetCheck call
      ctx.warmup = static_cast<std::uint32_t>(cfg.warmup);

      const laige::BudgetCheckResult check =
          laige::budgetCheck(*entry, histogram, ctx);
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
