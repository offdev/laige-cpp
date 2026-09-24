// laige-sim zero sim-loop allocation assertion suite (M1-ALLOC-01).
//
// Step scope (roadmap/M1-heartbeat.md, M1-ALLOC-01):
//   - The G-R1 debug per-tick assertion (PRD §9.3, budgets.json
//     sim_heap_allocs target 0, PERF-003): GameLoop::runOneTick arms
//     the process-wide allocation watch (laige/alloc_watch.h) before
//     every tick body and checks it after a completed tick — any heap
//     allocation inside the tick (a system, the onTick hook, the
//     replay recorder, engine storage growth, even a hot-path log)
//     fails with one structured Error event
//     (alloc/sim_tick_allocation — the offending call site in the
//     site field) + the debug assert (FR-12.3: actionable, never
//     silent). Release: no check, no crash — the pool-overflow
//     degradation is already logged through the pool accounting.
//
// This suite:
//   - runs the M1-ECS-07 workload (10k entities, 6 component types,
//     10k frames of add/remove churn + iteration) THROUGH THE GAME
//     LOOP as registered systems and proves every tick allocates zero
//     heap — in debug non-sanitizer builds the engine's own per-tick
//     assertion enforces this (an allocating tick aborts the run),
//     and the suite asserts it explicitly through the watch where
//     the watch is live (LAIGE_ALLOC_COUNTER);
//   - proves the scratch-system failure case (the step's Verify: "a
//     std::vector deliberately placed in a scratch system fails the
//     assertion"): the deliberate per-tick vector aborts the process
//     in debug builds where the watch is live (forked SIGABRT child —
//     the entity_tests DestroyStaleAbortsInDebug pattern), and is
//     NOT asserted in release or sanitizer builds (no crash — the
//     documented degradation path);
//   - round-trips the watch API itself (arm, allocate, read the count
//     + the first offending site, re-arm resets the window).
//
// Runs as CTest `zero_alloc` (the step's Verify command:
// `ctest -R zero_alloc`).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/sim/entity.h"
#include "laige/sim/game_loop.h"
#include "laige/sim/system.h"

#include "laige_test_seed.h"

#if defined(LAIGE_ALLOC_COUNTER)
#include "logging_alloc_counter.h"
#endif

#if defined(__unix__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "zero_alloc_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "zero_alloc_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "zero_alloc_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define ZERO_ALLOC_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define ZERO_ALLOC_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if ZERO_ALLOC_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "zero_alloc_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test component types (global scope on purpose — LAIGE_COMPONENT must
// specialize the primary template in its enclosing namespace)
// ---------------------------------------------------------------------------

// The M1-ECS-07 workload's six component types (same sizes/alignments
// as the stress suite's — 8 B/8 B/4 B/16 B/8 B-from-4 B/4 B).
struct ZAPos {
  std::int32_t x{};
  std::int32_t y{};
};
LAIGE_COMPONENT(ZAPos)
// M1-DET-01 (G-R8): integer-only storage (declared I/O of ZAIter).
LAIGE_DETERMINISM_SAFE(ZAPos, std::int32_t, std::int32_t);

struct ZAVel {
  std::int64_t v{};
};
LAIGE_COMPONENT(ZAVel)

struct ZAFlag {
  std::int32_t f{};
};
LAIGE_COMPONENT(ZAFlag)

struct ZAQuad {
  std::int32_t a{};
  std::int32_t b{};
  std::int32_t c{};
  std::int32_t d{};
};
LAIGE_COMPONENT(ZAQuad)

struct ZAPair {
  std::int16_t w{};
  std::int16_t h{};
  std::int16_t p{};
  std::int16_t q{};
};
LAIGE_COMPONENT(ZAPair)

// The churned component: toggled between the base archetypes every
// tick (the add/remove churn of the workload).
struct ZATag {
  std::int32_t t{};  // the tick that added it (value churn too)
};
LAIGE_COMPONENT(ZATag)
// M1-DET-01 (G-R8): integer-only storage (declared I/O of ZAChurn
// and ZAIter).
LAIGE_DETERMINISM_SAFE(ZATag, std::int32_t);

// The deliberate G-R1 violation (the scratch system of the step's
// Verify case): one heap allocation per tick, destroyed at the end
// of the tick (no leak — the sanitizer trees prove it).
LAIGE_SYSTEM(ZAScratch, 1)
void ZAScratch(laige::World&, laige::SystemContext&) {
  std::vector<std::int32_t> v(16, 1);  // the deliberate allocation
  volatile std::int32_t sink = v[0];
  static_cast<void>(sink);
}

namespace {

// The PRNG substream id for this file (docs/testing.md §4,
// M0-TEST-01): distinct from the archetype suite (1003), the
// iter-order scenario instantiations (1005-1007), and the stress
// suite (1008).
inline constexpr std::uint32_t kZeroAllocSubstreamId = 1012;

// The M1-ECS-07 workload parameters (the ecs_stress_tests scheme —
// CORE-005: named, justified constants):
//   kEntities       the PRD §8.1 reference scene size (10k entities).
//   kFrames         the step's window length ("10k-entity ticks").
//   kWarmupFrames   one full 625-frame cohort period (8 cycles x
//                   10000/128 picks per cycle) plus margin: every
//                   archetype's columns reach their high water before
//                   the window, so the window's zero reservation delta
//                   is the "pool high-water stable" claim.
//   kAddPicks / kRemovePicks  128 ops per tick — within the DEFAULT
//                   G-R4 budget (kDefaultChurnPerFrameBudget = 256),
//                   so the window must complete with zero churn warns.
inline constexpr std::uint32_t kEntities = 10000;
inline constexpr std::uint32_t kFrames = 10000;
inline constexpr std::uint32_t kWarmupFrames = 700;
inline constexpr std::uint32_t kAddPicks = 64;
inline constexpr std::uint32_t kRemovePicks = 64;

// The loop's clock (the game_loop_tests synthetic-clock pattern): 60
// Hz, one tick's worth of clock per frame -> exactly one tick per
// frame (the accumulator's exact due computation). One 60 Hz tick,
// in whole nanoseconds: 16666667 ns (ceil — 1e9/60 = 16666666.67;
// the floor loses a nanosecond per step and drifts off the exact
// due count over 10k ticks, the game_loop_tests constant).
inline constexpr std::uint32_t kRateHz = 60;
inline constexpr std::int64_t kTickNs = 16666667;

// FNV-1a 64 (FNV-1a spec constants, fnv.org): the visit checksum.
inline constexpr std::uint64_t kFnvOffset64 = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime64 = 0x100000001b3ull;

// The workload's state (file scope on purpose — systems are plain
// functions with no state objects, the FR-1.3 shape; the test TU is
// the state owner).
std::vector<laige::Entity> gEntities;
std::vector<std::uint32_t> gPermutation;
std::uint32_t gCursor = 0;
std::uint64_t gChecksum = kFnvOffset64;
std::uint64_t gTickCounter = 0;
std::uint64_t gChurnAdds = 0;
std::uint64_t gChurnRemoves = 0;
std::uint64_t gVisits = 0;
bool gChurnOk = true;
bool gIterOk = true;

std::atomic<std::int64_t> gClockNs{0};
std::int64_t clockNow() noexcept {
  return gClockNs.load(std::memory_order_relaxed);
}

// The churn phase of the M1-ECS-07 frame, as a system: kAddPicks adds
// of ZATag (where absent) + kRemovePicks removes (where present) off
// the cyclic permutation — the 128-ops-per-tick scheme (the
// ecs_stress runFrame contract; skips are not counted — the G-R4
// counting contract, entity.h). 100 ms budget: the workload tick is
// ~1 ms in the -O0 Debug tree, so the declared budget must never
// breach (a budget_overrun warn would itself allocate — the G-R1
// window must stay log-free).
LAIGE_SYSTEM(ZAChurn, 100)
void ZAChurn(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(ctx);
  const std::int32_t tick = static_cast<std::int32_t>(gTickCounter);
  for (std::uint32_t i = 0; i < kAddPicks && gChurnOk; ++i) {
    const std::uint32_t pos = gPermutation[gCursor];
    gCursor = (gCursor + 1) % kEntities;
    const laige::Entity e = gEntities[pos];
    if (world.has<ZATag>(e)) continue;  // skip, not counted
    if (!world.addComponent<ZATag>(e, ZATag{tick}).ok()) {
      gChurnOk = false;
    } else {
      ++gChurnAdds;
    }
  }
  for (std::uint32_t i = 0; i < kRemovePicks && gChurnOk; ++i) {
    const std::uint32_t pos = gPermutation[gCursor];
    gCursor = (gCursor + 1) % kEntities;
    const laige::Entity e = gEntities[pos];
    if (!world.has<ZATag>(e)) continue;  // skip, not counted
    if (!world.removeComponent<ZATag>(e).ok()) {
      gChurnOk = false;
    } else {
      ++gChurnRemoves;
    }
  }
  ++gTickCounter;
}

// The iteration phase of the M1-ECS-07 frame, as a system: one
// each<ZAPos, ZATag> (Read, Read) over every Tagged entity, counting
// visits and folding (slot, generation, tag value) into the 64-bit
// FNV-1a checksum (the ecs_stress scheme).
LAIGE_SYSTEM(ZAIter, 100)
void ZAIter(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(ctx);
  const bool ok = world
                      .each<ZAPos, ZATag>(
                          [](laige::Entity e, const ZAPos&,
                             const ZATag& tag) {
                            ++gVisits;
                            gChecksum ^=
                                static_cast<std::uint64_t>(e.id);
                            gChecksum *= kFnvPrime64;
                            gChecksum ^=
                                static_cast<std::uint64_t>(
                                    e.generation);
                            gChecksum *= kFnvPrime64;
                            gChecksum ^= static_cast<std::uint64_t>(
                                              static_cast<std::uint32_t>(
                                                  tag.t));
                            gChecksum *= kFnvPrime64;
                          },
                          laige::Read{}, laige::Read{})
                      .ok();
  if (!ok) gIterOk = false;
}

// One workload world at exactly its scene budget (setup phase: backing
// allocations and archetype growth allowed — the window under test
// starts after the warm-up): 6 registered types, 10k entities in the
// 4 base archetypes (by index — the ecs_stress setup), the seeded
// pick permutation (docs/testing.md §4: deterministic per seed), and
// the two workload systems.
laige::World makeWorkloadWorld() {
  auto w = laige::World::create(laige::World::Options{kEntities});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << kEntities
                  << ") failed: " << laige::errorName(w.error());
    abort();
  }
  laige::World world = std::move(w).takeValue();
  // Setup-phase failures are fatal (ADD_FAILURE + abort — the helper
  // returns a value, so no ASSERT_ macros; the ecs_stress makeWorld
  // pattern).
  if (!world.registerComponent<ZAPos>().ok() ||
      !world.registerComponent<ZAVel>().ok() ||
      !world.registerComponent<ZAFlag>().ok() ||
      !world.registerComponent<ZAQuad>().ok() ||
      !world.registerComponent<ZAPair>().ok() ||
      !world.registerComponent<ZATag>().ok()) {
    ADD_FAILURE() << "component registration failed";
    abort();
  }
  EXPECT_EQ(world.componentCount(), 6u);

  gEntities.resize(kEntities);
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    auto e = world.create();
    if (!e.ok()) {  // the scene fills the budget exactly
      ADD_FAILURE() << "world.create() failed at entity " << i;
      abort();
    }
    gEntities[i] = e.value();
    const std::int32_t base = static_cast<std::int32_t>(i);
    if (!world
             .addComponent<ZAPos>(gEntities[i], ZAPos{base, -base})
             .ok()) {
      ADD_FAILURE() << "addComponent<ZAPos> failed at entity " << i;
      abort();
    }
    switch (i % 4) {
      case 0:
        if (!world
                 .addComponent<ZAVel>(
                     gEntities[i], ZAVel{static_cast<std::int64_t>(i)})
                 .ok()) {
          ADD_FAILURE() << "addComponent<ZAVel> failed at entity " << i;
          abort();
        }
        break;
      case 1:
        if (!world
                 .addComponent<ZAVel>(
                     gEntities[i], ZAVel{static_cast<std::int64_t>(i)})
                 .ok() ||
            !world
                 .addComponent<ZAFlag>(gEntities[i], ZAFlag{base})
                 .ok()) {
          ADD_FAILURE() << "component add failed at entity " << i;
          abort();
        }
        break;
      case 2:
        if (!world
                 .addComponent<ZAQuad>(
                     gEntities[i], ZAQuad{base, base, base, base})
                 .ok()) {
          ADD_FAILURE() << "addComponent<ZAQuad> failed at entity " << i;
          abort();
        }
        break;
      case 3:
        if (!world
                 .addComponent<ZAPair>(
                     gEntities[i],
                     ZAPair{
                         static_cast<std::int16_t>(i),
                         static_cast<std::int16_t>(i % 100),
                         static_cast<std::int16_t>(i % 50),
                         static_cast<std::int16_t>(i % 25)})
                 .ok()) {
          ADD_FAILURE() << "addComponent<ZAPair> failed at entity " << i;
          abort();
        }
        break;
    }
  }
  // 5 archetypes: the 4 base sets plus the transient {ZAPos} set
  // created while the first component of each entity is attached
  // (the ecs_stress setup property).
  EXPECT_EQ(world.archetypeCount(), 5u);

  // The pick order: a seeded Fisher-Yates permutation (the ecs_stress
  // scheme — setup-phase allocation, outside the window).
  laige::Prng rng = laige::testing::TestPrng(kZeroAllocSubstreamId);
  gPermutation.resize(kEntities);
  for (std::uint32_t i = 0; i < kEntities; ++i) gPermutation[i] = i;
  for (std::uint32_t i = kEntities; i > 1; --i) {
    const std::uint32_t j = rng.next_range(0, i);  // [0, i)
    std::swap(gPermutation[i - 1], gPermutation[j]);
  }

  if (!world
           .registerSystem(ZAChurn_Def,
                           laige::Io<ZATag, laige::Access::Write>{})
           .ok() ||
      !world
           .registerSystem(
               ZAIter_Def,
               laige::Io<ZAPos, laige::Access::Read>{},
               laige::Io<ZATag, laige::Access::Read>{})
           .ok()) {
    ADD_FAILURE() << "system registration failed";
    abort();
  }
  return world;
}

// One loop frame advancing the synthetic clock by exactly one tick
// (the game_loop_tests synthFrame pattern): 1 tick per frame.
bool frameOneTick(laige::GameLoop& loop) {
  gClockNs.fetch_add(kTickNs, std::memory_order_relaxed);
  return loop.frame().ok();
}

// The scratch-system run (the non-asserted branches of the failure
// test): ticks of the deliberate vector allocation must complete
// without an assert (release: the check is compiled out; sanitizer
// debug: the watch is compiled out — the leak-free sanitizer run of
// the same loop is the fallback check). Compiled only where the
// non-asserted branch exists (the debug + live-watch tree uses the
// forked child instead — -Wunused-function would otherwise fire).
#if defined(NDEBUG) || !defined(LAIGE_ALLOC_COUNTER)
bool runScratchTicks(std::uint32_t ticks) {
  auto w = laige::World::create(laige::World::Options{8});
  if (!w.ok()) return false;
  laige::World world = std::move(w).takeValue();
  if (!world.registerSystem(ZAScratch_Def).ok()) return false;
  laige::SystemSchedule sched;
  if (!world.scheduleSystems(sched).ok()) return false;
  laige::GameLoop::Options opts;
  opts.tickRateHz = kRateHz;
  opts.nowNs = &clockNow;
  auto r = laige::GameLoop::create(world, sched, opts);
  if (!r.ok()) return false;
  laige::GameLoop loop = std::move(r).takeValue();
  gClockNs.store(0, std::memory_order_relaxed);
  if (!loop.frame().ok()) return false;  // start reference (zero ticks)
  for (std::uint32_t i = 0; i < ticks; ++i) {
    if (!frameOneTick(loop)) return false;
  }
  return loop.currentTick() == ticks;
}
#endif  // NDEBUG || !LAIGE_ALLOC_COUNTER

}  // namespace

// ---------------------------------------------------------------------------
// The M1-ECS-07 workload THROUGH THE GAME LOOP: 10k entities x 6
// component types x 10k ticks of add/remove churn + iteration — zero
// heap allocations per tick (G-R1), pool high-water stable
// ---------------------------------------------------------------------------

TEST(ZeroAlloc, TenKWorkloadThroughTheLoopAllocatesNothing) {
  laige::World world = makeWorkloadWorld();
  laige::SystemSchedule sched;
  ASSERT_TRUE(world.scheduleSystems(sched).ok());

  // Warm-up (kWarmupFrames = one full cohort period + margin), run as
  // DIRECT world ticks (beginFrame + runSystems, no GameLoop) BEFORE
  // the loop starts: the archetype column doublings, the four
  // Tagged transient archetypes, and the entity-budget level events
  // are setup-phase growth (the ecs_stress scheme) — outside the
  // per-tick G-R1 window. By the time the loop runs, every column is
  // at its high water and the window's churn moves only between the
  // pre-reserved blocks.
  for (std::uint32_t f = 0; f < kWarmupFrames; ++f) {
    world.beginFrame();  // void noexcept (entity.h)
    if (!world.runSystems(sched).ok()) {
      ADD_FAILURE() << "warm-up tick " << f << " failed";
      break;
    }
  }

  laige::GameLoop::Options opts;
  opts.tickRateHz = kRateHz;
  opts.nowNs = &clockNow;
  auto loopR = laige::GameLoop::create(world, sched, opts);
  ASSERT_TRUE(loopR.ok());
  laige::GameLoop loop = std::move(loopR).takeValue();
  gClockNs.store(0, std::memory_order_relaxed);
  ASSERT_TRUE(loop.frame().ok());  // start reference (zero ticks)

  const laige::ArchetypeStats before = world.archetypeStats();

  // The measured window: 10k ticks, exactly one per frame.
#if defined(LAIGE_ALLOC_COUNTER)
  // Per-tick windows (the M1-ALLOC-01 model): the engine's per-tick
  // arm resets the watch at the start of each tick, so the counter
  // read after a frame is exactly that tick's allocation count.
  std::uint64_t windowAllocs = 0;
#endif
  bool windowOk = true;
  for (std::uint32_t f = 0; f < kFrames; ++f) {
    if (!frameOneTick(loop)) {
      ADD_FAILURE() << "window frame " << f << " failed";
      windowOk = false;
      break;
    }
#if defined(LAIGE_ALLOC_COUNTER)
    windowAllocs += laige::test::allocCounter();
#endif
  }
  const laige::ArchetypeStats after = world.archetypeStats();
  ASSERT_TRUE(windowOk);
  // The loop ran exactly the window's ticks (the warm-up was direct
  // world ticks, before the loop existed).
  EXPECT_EQ(loop.currentTick(), static_cast<std::uint64_t>(kFrames));

  // Sanity: the workload actually churned (a silent no-op system
  // would make the zero-allocation claim vacuous).
  EXPECT_TRUE(gChurnOk);
  EXPECT_TRUE(gIterOk);
  ASSERT_GT(gChurnAdds + gChurnRemoves, 0u);
  ASSERT_GT(gVisits, 0u);

  // Pool high-water stability: the window reserved nothing new — the
  // churn moved only between the pre-reserved column blocks (the
  // reserve policy, archetype.h; the ecs_stress window claim). Only
  // ZATag was toggled: every entity keeps its base components.
  EXPECT_EQ(after.totalReservations, before.totalReservations);
  EXPECT_EQ(after.totalArchetypeGrowth, before.totalArchetypeGrowth);
  EXPECT_EQ(after.rowsLive, kEntities);
  EXPECT_EQ(world.entityCount(), kEntities);

#if defined(LAIGE_ALLOC_COUNTER)
  // The window's zero-allocation property (non-sanitizer trees):
  // every tick's window read zero heap allocations. In debug builds
  // the engine's own per-tick G-R1 assertion additionally proves the
  // property for every tick of the run (an allocating tick would have
  // aborted this run before the reads). The sanitizer trees prove the
  // same property with the leak-free sanitizer run of the same loop
  // plus the reservation delta above (the established fallback).
  EXPECT_EQ(windowAllocs, 0u);
#endif

  // Machine-greppable line for the record (CORE-001 / AGENTS §12:
  // the measured property, on every ctest run).
  std::printf(
      "zero-alloc window: ticks=%llu churn_adds=%llu churn_removes=%llu "
      "visits=%llu allocs=%llu reservations_delta=%llu "
      "checksum=0x%016llx\n",
      static_cast<unsigned long long>(loop.currentTick()),
      static_cast<unsigned long long>(gChurnAdds),
      static_cast<unsigned long long>(gChurnRemoves),
      static_cast<unsigned long long>(gVisits),
#if defined(LAIGE_ALLOC_COUNTER)
      static_cast<unsigned long long>(windowAllocs),
#else
      0ULL,
#endif
      static_cast<unsigned long long>(
          after.totalReservations - before.totalReservations),
      static_cast<unsigned long long>(gChecksum));
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// The scratch-system failure case: a deliberate std::vector in a
// registered system fails the per-tick assertion (debug + live watch)
// and is not asserted (release / sanitizer: no crash — the
// degradation path)
// ---------------------------------------------------------------------------

TEST(ZeroAlloc, ScratchSystemAllocationFailsTheTickAssertion) {
#if defined(NDEBUG)
  // Release: the G-R1 check is compiled out (the game_loop.h
  // "zero-allocation check" contract) — an allocating game system does
  // not crash a release build; the degradation is the already-logged
  // pool accounting. The ticks must still complete cleanly.
  EXPECT_TRUE(runScratchTicks(5));
#elif !defined(LAIGE_ALLOC_COUNTER)
  // Sanitizer debug: the counting backend is compiled out (the
  // runtimes own operator new/delete) — the assertion cannot fire
  // here; the leak-free sanitizer run of the same loop is the
  // fallback check. The ticks must still complete cleanly.
  EXPECT_TRUE(runScratchTicks(5));
#else
#if defined(__unix__)
  // Debug + live watch: the per-tick assertion must fire. Exercised
  // in a forked child so the test process survives (the entity_tests
  // DestroyStaleAbortsInDebug pattern): the child must die on
  // SIGABRT inside the first scratch tick.
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: one scratch tick — the completed tick's window holds the
    // vector's allocation, so the G-R1 assert must fire.
    auto w = laige::World::create(laige::World::Options{8});
    if (!w.ok()) _exit(117);
    laige::World world = std::move(w).takeValue();
    if (!world.registerSystem(ZAScratch_Def).ok()) _exit(117);
    laige::SystemSchedule sched;
    if (!world.scheduleSystems(sched).ok()) _exit(117);
    laige::GameLoop::Options opts;
    opts.tickRateHz = kRateHz;
    opts.nowNs = &clockNow;
    auto r = laige::GameLoop::create(world, sched, opts);
    if (!r.ok()) _exit(117);
    laige::GameLoop loop = std::move(r).takeValue();
    gClockNs.store(0, std::memory_order_relaxed);
    if (!loop.frame().ok()) _exit(117);  // start reference
    gClockNs.store(kTickNs, std::memory_order_relaxed);
    (void)loop.frame();  // tick 1 completes with an allocation -> assert
    _exit(1);  // unreachable: the parent fails below without the assert
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
      << "expected the G-R1 tick-allocation assert to abort the child "
         "(SIGABRT)";
#else
  GTEST_SKIP() << "fork() is not available on Windows; the G-R1 "
                 "tick-allocation assert is exercised on the POSIX jobs.";
#endif
#endif
}

// ---------------------------------------------------------------------------
// The watch API itself: arm, allocate, read the count + the first
// offending site, re-arm resets the window
// ---------------------------------------------------------------------------

TEST(ZeroAlloc, WatchCountsAllocationsAndCapturesTheFirstSite) {
#if !defined(LAIGE_ALLOC_COUNTER)
  // Sanitizer trees: the counting backend is compiled out (the
  // runtimes own operator new/delete) — the watch is a no-op there
  // (the fallback pattern, alloc_watch.h scope section).
  GTEST_SKIP() << "the counting backend is compiled out in sanitizer "
                 "trees (allocWatchLive() is false)";
#else
  EXPECT_TRUE(laige::allocWatchLive());
  laige::allocWatchArm();
  const laige::AllocWatchReading empty = laige::allocWatchRead();
  EXPECT_EQ(empty.allocs, 0u);
  EXPECT_EQ(empty.firstSite, nullptr);
  {
    std::vector<std::int32_t> v(4, 7);  // one heap allocation
    volatile std::int32_t sink = v[0];
    static_cast<void>(sink);
  }
  const laige::AllocWatchReading after = laige::allocWatchRead();
  EXPECT_GE(after.allocs, 1u);
  EXPECT_NE(after.firstSite, nullptr);  // the actionable call site
  // A re-arm resets the window (the per-tick window model).
  laige::allocWatchArm();
  const laige::AllocWatchReading reset = laige::allocWatchRead();
  EXPECT_EQ(reset.allocs, 0u);
  EXPECT_EQ(reset.firstSite, nullptr);
#endif
}
