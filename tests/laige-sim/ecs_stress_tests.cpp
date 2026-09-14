// laige-sim ECS stress + memory accounting suite (M1-ECS-07).
//
// Step scope (roadmap/M1-heartbeat.md, M1-ECS-07):
//   - Stress test: 10k entities, 6 component types, 10k frames of
//     add/remove churn. Verify: no leaks (the green `ctest -R ecs_stress`
//     run in the ASan tree is the check — ASan's leak report fails the
//     run), pool high-water stability (zero reservation/growth delta
//     over the measured window), and iteration staying within the
//     documented cost (query.h: bounded archetype scan + one visit per
//     matching entity — checked as a platform-robust ns-per-visit
//     throughput floor, CORE-001: measured, not assumed).
//   - Memory accounting: the accounted storage bytes (EntityStats +
//     ArchetypeStats) are printed as machine-greppable lines on every
//     ctest run and recorded in
//     docs/benchmarks/baselines/m1-ecs-stress.md (AGENTS §12).
//
// Workload (deterministic under the fixed test seed; docs/testing.md §4):
//   - kEntities = 10000 entities (the PRD §8.1 reference scene size);
//     the world capacity is exactly 10000, so the scene sits at the
//     100% G-R3 level for the whole run (the 100% warn fires once at
//     setup and never re-crosses: the entity count never changes).
//   - 6 registered component types (Pos, Vel, Flag, Quad, Pair, Tag —
//     distinct sizes/alignments to exercise the column strides). The
//     base sets are fixed by entity index (4 archetypes:
//     {Pos,Vel}, {Pos,Vel,Flag}, {Pos,Quad}, {Pos,Pair}); Tag is the
//     churned component.
//   - Each frame: beginFrame() drives the guardrails (the owning loop
//     drives it — here the test is the loop), then a seeded cyclic
//     permutation supplies kAddPicks adds (Tag added to the picked
//     entity when it lacks it) and kRemovePicks removes (Tag removed
//     when present). 128 ops per frame — within the DEFAULT G-R4
//     budget of 256, so the stress window must run with zero churn
//     warns (asserted). Then one `each<Pos, Tag>` iteration (Read,
//     Read) visits every Tagged entity: visit count + 64-bit FNV-1a
//     checksum over (slot, generation, tag value).
//   - A kWarmupFrames = 700-frame warm-up (one full 625-frame cohort
//     period — 8 cycles x 10000/128 picks per cycle — plus margin)
//     lets every archetype's columns grow to their high water before
//     the measured window. The window's zero reservation delta is the
//     "pool high-water stable" claim.
//
// Runs as CTest `ecs_stress` (the step's Verify command:
// `ctest -R ecs_stress`, required green under ASan): a filtered view
// of the shared laige-sim_tests executable, selecting exactly the
// EcsStress suite below.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/sim/entity.h"

#include "laige_test_seed.h"

#if defined(LAIGE_ALLOC_COUNTER)
#include "logging_alloc_counter.h"
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "ecs_stress_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "ecs_stress_tests must be built with exceptions "
              "disabled (NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "ecs_stress_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define STRESS_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define STRESS_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if STRESS_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "ecs_stress_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test component types (global scope on purpose — LAIGE_COMPONENT must
// specialize the primary template in its enclosing namespace)
// ---------------------------------------------------------------------------

// 8 B, 4 B-aligned: the basic packed-column case.
struct StressPos {
  std::int32_t x{};
  std::int32_t y{};
};
LAIGE_COMPONENT(StressPos)

// 8 B, 8 B-aligned: the SoA column alignment case.
struct StressVel {
  std::int64_t v{};
};
LAIGE_COMPONENT(StressVel)

// 4 B: the small-column case.
struct StressFlag {
  std::int32_t f{};
};
LAIGE_COMPONENT(StressFlag)

// 16 B: the wide-column case (the largest row stride in the workload).
struct StressQuad {
  std::int32_t a{};
  std::int32_t b{};
  std::int32_t c{};
  std::int32_t d{};
};
LAIGE_COMPONENT(StressQuad)

// 8 B from 4 B members: a mixed-stride column.
struct StressPair {
  std::int16_t w{};
  std::int16_t h{};
  std::int16_t p{};
  std::int16_t q{};
};
LAIGE_COMPONENT(StressPair)

// The churned component: toggled between the base archetypes every
// frame (the add/remove churn of the workload).
struct StressTag {
  std::int32_t t{};  // the frame that added it (value churn too)
};
LAIGE_COMPONENT(StressTag)

namespace {

// The PRNG substream id for this file (docs/testing.md §4, M0-TEST-01):
// distinct from the archetype suite (1003) and the iter-order scenario
// instantiations (1005-1007).
inline constexpr std::uint32_t kStressTestsSubstreamId = 1008;

// The workload parameters (CORE-005: named, justified constants):
//   kEntities     the PRD §8.1 reference scene size (10k entities).
//   kFrames       the step's window length ("10k frames").
//   kWarmupFrames one full 625-frame cohort period (8 cycles x
//                 10000/128 picks per cycle) plus margin: every
//                 archetype's columns reach their high water before
//                 the window, so the window's zero reservation delta
//                 is the "pool high-water stable" claim.
//   kAddPicks / kRemovePicks  128 ops per frame — within the DEFAULT
//                 G-R4 budget (kDefaultChurnPerFrameBudget = 256), so
//                 the window must complete with zero churn warns.
//                 At this rate the steady-state structural churn is
//                 ~16 moves/frame (~160k over the window) plus 1.28M
//                 has() probes and 10k iterations over ~5k matches —
//                 the full-scale stress on the documented workload,
//                 in bounded wall time (~10 s on the -O0 Debug tree).
inline constexpr std::uint32_t kEntities = 10000;
inline constexpr std::uint32_t kFrames = 10000;
inline constexpr std::uint32_t kWarmupFrames = 700;
inline constexpr std::uint32_t kAddPicks = 64;
inline constexpr std::uint32_t kRemovePicks = 64;

// The window's iteration throughput floor: max ns per visited entity
// (CORE-005 — the measured evidence and derivation live in the
// EcsStress test; the same platform-robust pattern as the churn test's
// kChurnMaxNsPerRowShifted: wall time per unit of accounted work,
// immune to shared-runner preemption noise, catching an
// order-of-magnitude per-visit regression). Measured P0 evidence
// (Debug, -O0, this workload): ~58 ns/visit Linux x64 (g++ 16.2.1),
// ~72 ns/visit Linux x64 (clang++ 22.1.8); the 600 ns floor is
// >= 8x the slowest measured.
inline constexpr double kStressMaxNsPerVisit = 600.0;

// FNV-1a 64 (FNV-1a spec constants, fnv.org): the visit checksum.
inline constexpr std::uint64_t kFnvOffset64 = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime64 = 0x100000001b3ull;

// One world with the default G-R4 budget, taken out of its Result
// (Result::value() is const; takeValue() && moves the storage out —
// the documented ownership-transfer path, result.h).
laige::World makeWorld(std::uint32_t capacity) {
  auto w = laige::World::create(laige::World::Options{capacity});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << capacity
                  << ") failed: " << laige::errorName(w.error());
    abort();
  }
  return std::move(w).takeValue();
}

// One frame's churn + iteration sample.
struct FrameSample {
  bool ok;  // every attempted op and the iteration succeeded
  std::uint64_t adds;  // successful structural Tag adds
  std::uint64_t removes;  // successful structural Tag removes
  std::uint64_t visits;  // entities visited by the frame's iteration
  double iterMs;  // the frame's iteration wall time (ms)
};

// One frame (see the file preamble for the exact scheme): beginFrame
// drives the guardrails; the add phase picks kAddPicks entities from
// the cyclic permutation and adds Tag where absent; the remove phase
// picks kRemovePicks and removes Tag where present (skips are not
// counted — the G-R4 counting contract, entity.h); then the frame's
// iteration visits every entity with both Pos and Tag (superset
// match: all 4 Tagged variant archetypes), counting visits and
// folding (slot, generation, tag value) into the 64-bit FNV-1a
// checksum. No allocation anywhere (the M1 zero-allocation property;
// the window's zero operator-new count is asserted in the test).
FrameSample runFrame(laige::World& world,
                    const std::vector<laige::Entity>& entities,
                    const std::vector<std::uint32_t>& perm,
                    std::uint32_t& cursor, std::uint32_t frame,
                    std::uint64_t& checksum) {
  world.beginFrame();
  FrameSample s{true, 0, 0, 0, 0.0};
  for (std::uint32_t i = 0; i < kAddPicks && s.ok; ++i) {
    const std::uint32_t pos = perm[cursor];
    cursor = (cursor + 1) % kEntities;
    const laige::Entity e = entities[pos];
    if (world.has<StressTag>(e)) continue;  // skip, not counted
    if (!world
             .addComponent<StressTag>(e,
                                      StressTag{static_cast<std::int32_t>(frame)})
             .ok()) {
      s.ok = false;
    } else {
      ++s.adds;
    }
  }
  for (std::uint32_t i = 0; i < kRemovePicks && s.ok; ++i) {
    const std::uint32_t pos = perm[cursor];
    cursor = (cursor + 1) % kEntities;
    const laige::Entity e = entities[pos];
    if (!world.has<StressTag>(e)) continue;  // skip, not counted
    if (!world.removeComponent<StressTag>(e).ok()) {
      s.ok = false;
    } else {
      ++s.removes;
    }
  }
  if (!s.ok) return s;
  laige::TimeIt iterTimer;
  auto st = world.each<StressPos, StressTag>(
      [&](const laige::Entity& e, const StressPos&, const StressTag& tag) {
        ++s.visits;
        checksum ^= static_cast<std::uint64_t>(e.id);
        checksum *= kFnvPrime64;
        checksum ^= static_cast<std::uint64_t>(e.generation);
        checksum *= kFnvPrime64;
        checksum ^= static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(tag.t));
        checksum *= kFnvPrime64;
      },
      laige::Read{}, laige::Read{});
  s.iterMs = iterTimer.elapsedMs();
  if (!st.ok()) s.ok = false;
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// The stress step (M1-ECS-07 scope): 10k entities x 6 component types x
// 10k frames of add/remove churn — no leaks (ASan tree), pool
// high-water stable, iteration within the documented cost, accounted
// memory printed for the baseline record
// ---------------------------------------------------------------------------

TEST(EcsStress, TenKEntitiesSixTypesTenKFramesChurn) {
  // Setup (phase: backing allocations and archetype growth allowed —
  // the window under test starts after the warm-up): the world at
  // exactly its scene budget, 6 registered types, 10k entities in the
  // 4 base archetypes (by index — no randomness in the base sets).
  laige::World world = makeWorld(kEntities);
  ASSERT_TRUE(world.registerComponent<StressPos>().ok());
  ASSERT_TRUE(world.registerComponent<StressVel>().ok());
  ASSERT_TRUE(world.registerComponent<StressFlag>().ok());
  ASSERT_TRUE(world.registerComponent<StressQuad>().ok());
  ASSERT_TRUE(world.registerComponent<StressPair>().ok());
  ASSERT_TRUE(world.registerComponent<StressTag>().ok());
  EXPECT_EQ(world.componentCount(), 6u);

  std::vector<laige::Entity> entities(kEntities);
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    auto e = world.create();
    ASSERT_TRUE(e.ok());  // the scene fills the budget exactly
    entities[i] = e.value();
    const std::int32_t base = static_cast<std::int32_t>(i);
    ASSERT_TRUE(world.addComponent<StressPos>(entities[i],
                                              StressPos{base, -base})
                    .ok());
    switch (i % 4) {
      case 0:
        ASSERT_TRUE(world
                        .addComponent<StressVel>(
                            entities[i], StressVel{static_cast<std::int64_t>(i)})
                        .ok());
        break;
      case 1:
        ASSERT_TRUE(world
                        .addComponent<StressVel>(
                            entities[i], StressVel{static_cast<std::int64_t>(i)})
                        .ok());
        ASSERT_TRUE(world
                        .addComponent<StressFlag>(entities[i],
                                                   StressFlag{base})
                        .ok());
        break;
      case 2:
        ASSERT_TRUE(world
                        .addComponent<StressQuad>(
                            entities[i], StressQuad{base, base, base, base})
                        .ok());
        break;
      case 3:
        ASSERT_TRUE(world
                        .addComponent<StressPair>(
                            entities[i],
                            StressPair{
                                static_cast<std::int16_t>(i),
                                static_cast<std::int16_t>(i % 100),
                                static_cast<std::int16_t>(i % 50),
                                static_cast<std::int16_t>(i % 25)})
                        .ok());
        break;
    }
  }
  // 5 archetypes: the 4 base sets plus the transient {Pos} set created
  // while the first component of each entity is attached (each entity
  // leaves {Pos} immediately — its rows stay reserved at the initial
  // 16, the M1 "archetypes are never destroyed" rule).
  EXPECT_EQ(world.archetypeCount(), 5u);

  // The pick order: a seeded Fisher-Yates permutation (docs/testing.md
  // §4: deterministic per seed, LAIGE_TEST_SEED overridable).
  laige::Prng rng = laige::testing::TestPrng(kStressTestsSubstreamId);
  std::vector<std::uint32_t> perm(kEntities);
  for (std::uint32_t i = 0; i < kEntities; ++i) perm[i] = i;
  for (std::uint32_t i = kEntities; i > 1; --i) {
    const std::uint32_t j = rng.next_range(0, i);  // [0, i)
    std::swap(perm[i - 1], perm[j]);
  }

  // Warm-up (kWarmupFrames = one full cohort period + margin): the
  // archetypes' columns grow to their high water before the window.
  std::uint32_t cursor = 0;
  std::uint64_t checksum = kFnvOffset64;
  bool warmupOk = true;
  for (std::uint32_t f = 0; f < kWarmupFrames; ++f) {
    const FrameSample s = runFrame(world, entities, perm, cursor, f, checksum);
    if (!s.ok) {
      ADD_FAILURE() << "warm-up frame " << f << " failed";
      warmupOk = false;
      break;
    }
  }
  ASSERT_TRUE(warmupOk);

  const laige::ArchetypeStats before = world.archetypeStats();
  // The setup phase attached ~32.5k components without a driven
  // beginFrame(), so the G-R4 counter accumulated them in one window
  // and its warn-once-per-lifetime degradation may have fired (the
  // documented no-frame behavior, entity.h). The window's claim is
  // that no NEW churn warn fires in it: the delta must be zero.
  const laige::GuardrailStats beforeGuardrails = world.guardrailStats();

  // The measured window: 10k frames of churn + iteration.
  laige::Histogram hist(laige::Histogram::Options{kFrames});
  std::uint64_t totalAdds = 0;
  std::uint64_t totalRemoves = 0;
  std::uint64_t totalVisits = 0;
  std::uint64_t tagsPeak = 0;
  double iterMsTotal = 0.0;
#if defined(LAIGE_ALLOC_COUNTER)
  // The window starts here: the permutation, histogram, and warm-up
  // allocated above, so the reset lands between setup and the ops.
  laige::test::resetAllocCounter();
#endif
  bool windowOk = true;
  for (std::uint32_t f = 0; f < kFrames; ++f) {
    const FrameSample s = runFrame(world, entities, perm, cursor,
                                   kWarmupFrames + f, checksum);
    if (!s.ok) {
      ADD_FAILURE() << "window frame " << f << " failed";
      windowOk = false;
      break;
    }
    hist.record(s.iterMs);
    totalAdds += s.adds;
    totalRemoves += s.removes;
    totalVisits += s.visits;
    if (s.visits > tagsPeak) tagsPeak = s.visits;
    iterMsTotal += s.iterMs;
  }
  const laige::ArchetypeStats after = world.archetypeStats();
  ASSERT_TRUE(windowOk);

  // Zero pool overflow and pool high-water stability: every op
  // succeeded (no BudgetExhausted, no growth failure) and the window
  // reserved nothing new — the churn moved only between the
  // pre-reserved column blocks (the reserve policy, archetype.h).
  EXPECT_EQ(after.totalReservations, before.totalReservations);
  EXPECT_EQ(after.totalArchetypeGrowth, before.totalArchetypeGrowth);
  EXPECT_EQ(after.totalAdds, before.totalAdds + totalAdds);
  EXPECT_EQ(after.totalRemoves, before.totalRemoves + totalRemoves);
  // Only Tag was toggled: every entity keeps its base components.
  EXPECT_EQ(after.rowsLive, kEntities);
  EXPECT_EQ(world.entityCount(), kEntities);
  // No archetype may reserve more than the world capacity (the
  // documented growth cap, archetype.h).
  EXPECT_LE(after.rowsReserved,
            std::uint32_t(laige::kMaxArchetypes) * kEntities);

#if defined(LAIGE_ALLOC_COUNTER)
  // The window's zero-allocation property (non-sanitizer trees): only
  // the pre-reserved column blocks are touched. (The sanitizer trees
  // prove the same property with the leak-free run of this window —
  // the step's ASan Verify — plus the reservation delta above.)
  EXPECT_EQ(laige::test::allocCounter(), 0u);
#endif

  // Iteration within the documented cost (query.h: bounded archetype
  // scan + one visit per matching entity). The window-wide
  // throughput floor (CORE-001: measured, not assumed): the sum of the
  // per-frame iteration times over the window, per visited entity. A
  // regression that adds per-visit work (an extra indirection, a
  // hidden allocation, a per-visit log) pushes the per-visit cost up
  // by orders of magnitude; preemption noise spreads across the whole
  // window and cannot. Measured P0 evidence (Debug, -O0, this
  // workload): ~58 ns/visit on Linux x64 (g++ 16.2.1) and ~72
  // ns/visit on Linux x64 (clang++ 22.1.8); the 600 ns floor is
  // >= 8x the slowest measured.
  ASSERT_GT(totalVisits, 0u);
  const double nsPerVisit =
      iterMsTotal * 1e6 / static_cast<double>(totalVisits);
  EXPECT_LT(nsPerVisit, kStressMaxNsPerVisit);

  // The window issued no new guardrail warns: the per-frame churn
  // (128 ops max) never exceeded the default 256 budget, and the
  // entity count sat at the 100% level the whole time (the 100% warn
  // fired once at setup and never re-crossed).
  const laige::GuardrailStats g = world.guardrailStats();
  EXPECT_EQ(g.churnWarns, beforeGuardrails.churnWarns);
  EXPECT_EQ(g.entityBudgetWarns[0], 1u);
  EXPECT_EQ(g.entityBudgetWarns[1], 1u);
  EXPECT_EQ(g.entityBudgetWarns[2], 1u);
  EXPECT_EQ(g.entityBudgetLevel, 100u);
  EXPECT_LE(g.frameChurn, g.churnPerFrameBudget);

  // Machine-greppable stats lines for the baseline record (CORE-001 /
  // AGENTS §12: the measured cost and the accounted work/memory, on
  // every ctest run).
  const laige::HistogramStats st = hist.stats();
  ASSERT_EQ(st.n, kFrames);
  const laige::EntityStats es = world.stats();
  std::printf(
      "ecs-stress window: frames=%u adds=%llu removes=%llu "
      "visits=%llu tags_peak=%llu checksum=0x%016llx\n",
      kFrames, static_cast<unsigned long long>(totalAdds),
      static_cast<unsigned long long>(totalRemoves),
      static_cast<unsigned long long>(totalVisits),
      static_cast<unsigned long long>(tagsPeak),
      static_cast<unsigned long long>(checksum));
  std::printf("ecs-stress iteration: %s ns_per_visit=%.1f\n",
              laige::formatStatsLine(st).c_str(), nsPerVisit);
  std::printf(
      "ecs-stress memory: entities=%u entity_bytes_capacity=%llu "
      "entity_bytes_inuse=%llu archetypes=%u rows_live=%u "
      "rows_reserved=%llu bytes_reserved=%llu\n",
      kEntities, static_cast<unsigned long long>(es.bytesCapacity),
      static_cast<unsigned long long>(es.bytesInUse),
      after.archetypeCount, after.rowsLive,
      static_cast<unsigned long long>(after.rowsReserved),
      static_cast<unsigned long long>(after.bytesReserved));
  std::fflush(stdout);

  // Spot-check the final state through the public API.
  EXPECT_TRUE(world.has<StressPos>(entities[0]));
  EXPECT_TRUE(world.has<StressPos>(entities[3]));
  EXPECT_NE(world.get<StressPos>(entities[0]), nullptr);
  EXPECT_EQ(world.entityCount(), kEntities);
}
