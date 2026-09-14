// laige-sim deterministic iteration order suite (M1-ECS-05).
//
// Step scope (roadmap/M1-heartbeat.md; contract documented in
// docs/api/iteration_order.md):
//   - the documented iteration contract: archetypes in registration
//     (first-seen/creation) order, entities within an archetype in id
//     order, the dense-id order surviving moves
//   - the convergence property: two worlds whose operation sequences
//     interleave create/destroy in different ways but converge on the
//     identical final state — including the entity->id assignment —
//     iterate identically
//   - component moves (add/remove) are exercised throughout the
//     histories so the dense-id-order scheme is pinned, not assumed
//
// The "no unordered containers in the iteration path" half of the step
// is a structural property of the implementation (archetype.h: the
// entity->archetype map is direct indexing, not even a hash; the one
// hash structure, the component type-key index, is lookup-only and
// never iterated). It is verified by inspection here and will be
// enforced in CI by the M1-DET-01 sim source scan; this suite pins the
// observable consequence — the visit order is a pure function of the
// world state, never of the operation history.
//
// Runs as CTest `iter_order` (the step's Verify command:
// `ctest -R iter_order`): a filtered view of the shared
// laige-sim_tests executable selecting exactly the IterOrder* suites.
//
// Scenario (fixed PRNG seed, docs/testing.md §4): a 20-slot world, 16
// logical entities created in index order (entity i takes slot 19-i —
// the LIFO free list pops 19, 18, ...), dead set i % 5 == 2 = {2, 7,
// 12}, component types IterA/IterB/IterC registered in that order
// (ids 1, 2, 3). Per-entity component scripts (values 100*i + {1,2,3},
// distinct per entity):
//
//   even i (i != 4):  add A, add B, add C, remove C, remove B  -> {A}
//   i == 4:           the same + remove A                     -> {}
//   odd i:            add A, add B, remove B, add B           -> {A,B}
//
// The first-seen archetype order is {A}=1, {A,B}=2, {A,B,C}=3 under
// every interleaving the builder can produce (the round-robin reaches
// each script step in script order; the within-round order never
// introduces a new set out of round order). Final live state (both
// sequences, always): archetype 1 holds the even live entities in
// slots {5, 9, 11, 13, 19}; archetype 2 the odd live entities in slots
// {4, 6, 8, 10, 14, 16, 18}; entity 4 is live and component-less in
// slot 15 — 13 live entities, archetype 3 ({A,B,C}) empty.
//
// Sequence A (serial): create all 16 in order; every entity (the dead
// ones included) runs its script in turn; the dead entities are
// destroyed last, in index order.
// Sequence B (interleaved): the SAME create phase — the LIFO free list
// REQUIRES it for the identical entity->id assignment: a live entity's
// slot is whatever the free list's top holds at its create, and any
// earlier dead-slot destroy would have pushed that slot back on top.
// The difference is WHEN the dead destroys happen: in B each dead
// entity is destroyed at a PRNG-assigned round boundary (scattered
// through the component phase, instead of A's end), the component
// steps are applied round-robin over the live entities in per-round
// PRNG permutations (the per-entity script order is preserved), and
// PRNG-placed scratch create/destroy pairs (component-less, free-list-
// neutral, placed only on the round boundaries — after every live
// create) ride the round boundaries.
// The two histories converge on an identical final state in every
// field the iteration contract covers: live slots, entity->slot
// assignment, archetype assignment, archetype id assignment, component
// values, and even the dead slots' generation counters (each dead slot
// is created once and destroyed once in both sequences). They differ
// only in bookkeeping the iteration cannot observe: the churn
// counters (totalCreated/totalAdds/totalRemoves), the free-list ORDER
// of the pushed dead slots (and hence the slot a hypothetical NEXT
// create() would pop — not part of any iteration), and the
// generation counters of the never-created slots a scratch pair pops
// (still dead, still generation-irrelevant). The oracle below
// recomputes the expected visit sequences from the final state and
// from each sequence's op walk (first-seen archetype order); the tests
// assert observed(world A) == observed(world B) == oracle for five
// queries.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/prng.h"
#include "laige/sim/entity.h"

#include "laige_test_seed.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "iter_order_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "iter_order_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "iter_order_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define ITER_ORDER_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define ITER_ORDER_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if ITER_ORDER_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "iter_order_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test component types (global scope on purpose — LAIGE_COMPONENT must
// sit in the primary template's enclosing namespace)
// ---------------------------------------------------------------------------

struct IterA {
  std::int32_t v;
};
LAIGE_COMPONENT(IterA)

struct IterB {
  std::int32_t v;
};
LAIGE_COMPONENT(IterB)

struct IterC {
  std::int32_t v;
};
LAIGE_COMPONENT(IterC)

namespace {

// ---------------------------------------------------------------------------
// Scenario constants (CORE-005: named; the KAT below is pinned to them)
// ---------------------------------------------------------------------------

constexpr std::uint32_t kScenarioCapacity = 20;
constexpr std::uint32_t kScenarioEntities = 16;
constexpr std::uint32_t kDeadModulo = 5;
constexpr std::uint32_t kDeadRemainder = 2;
constexpr std::uint32_t kComponentlessEntity = 4;
constexpr std::uint32_t kMaxScriptSteps = 6;  // the i == 4 script length
constexpr std::uint32_t kLiveEntityCount = 13;  // 16 - 3 dead
// Entity i is created at slot kScenarioCapacity - 1 - i (the LIFO free
// list pops 19, 18, ... first).
constexpr std::uint16_t slotOf(std::uint32_t i) {
  return static_cast<std::uint16_t>(kScenarioCapacity - 1 - i);
}

// Component-set masks (registration order: A = 1, B = 2, C = 3).
constexpr std::uint32_t kSetA = 1u << 0;
constexpr std::uint32_t kSetB = 1u << 1;
constexpr std::uint32_t kSetC = 1u << 2;

// Substream ids for the property test's three scenario instantiations
// (docs/testing.md §4: stable test identities; 1003 belongs to the
// archetype suite, so this file owns 1005-1007).
constexpr std::uint32_t kScenarioStreamIds[3] = {1005u, 1006u, 1007u};

// ---------------------------------------------------------------------------
// The operation model
// ---------------------------------------------------------------------------

struct Op {
  enum class Kind : std::uint8_t {
    Create,
    Destroy,
    AddA,
    AddB,
    AddC,
    RemA,
    RemB,
    RemC,
    ScratchCreate,
    ScratchDestroy,
  } kind;
  std::uint32_t entity{};  // logical index; unused for scratch ops
  std::int32_t value{};  // component value for add ops; 0 otherwise
};

inline bool isDead(std::uint32_t i) {
  return i % kDeadModulo == kDeadRemainder;
}

// The per-entity component script (see the file preamble). Every step
// is a structural move (adds of absent components, removes of present
// ones) — no in-place overwrites, no no-ops — so both sequences churn
// rows between archetypes continuously.
std::vector<Op> scriptFor(std::uint32_t i) {
  const std::int32_t a = static_cast<std::int32_t>(100 * i + 1);
  const std::int32_t b = static_cast<std::int32_t>(100 * i + 2);
  const std::int32_t c = static_cast<std::int32_t>(100 * i + 3);
  if (i % 2 == 1) {
    return {{Op::Kind::AddA, i, a},
            {Op::Kind::AddB, i, b},
            {Op::Kind::RemB, i, 0},
            {Op::Kind::AddB, i, b}};
  }
  if (i == kComponentlessEntity) {
    return {{Op::Kind::AddA, i, a},
            {Op::Kind::AddB, i, b},
            {Op::Kind::AddC, i, c},
            {Op::Kind::RemC, i, 0},
            {Op::Kind::RemB, i, 0},
            {Op::Kind::RemA, i, 0}};
  }
  return {{Op::Kind::AddA, i, a},
          {Op::Kind::AddB, i, b},
          {Op::Kind::AddC, i, c},
          {Op::Kind::RemC, i, 0},
          {Op::Kind::RemB, i, 0}};
}

// ---------------------------------------------------------------------------
// Sequence construction
// ---------------------------------------------------------------------------

struct Scenario {
  std::vector<Op> seqA;
  std::vector<Op> seqB;
  std::uint32_t extraPairs{};  // PRNG-placed scratch pairs (greppable line)
};

// Build the two convergent operation sequences. `deterministic` skips
// every PRNG draw (all dead destroys land at the final boundary in
// index order, identity round order, no extra pairs) — the KAT's
// degenerate interleaving; the rng is then never read.
Scenario buildScenario(laige::Prng rng, bool deterministic) {
  Scenario s;
  // Sequence A: create all, run every script in turn (the dead
  // entities included — they hold their archetype rows until the final
  // destroy), destroy the dead entities last, in index order.
  for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
    s.seqA.push_back({Op::Kind::Create, i, 0});
  }
  for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
    for (const Op& op : scriptFor(i)) {
      s.seqA.push_back(op);
    }
  }
  for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
    if (isDead(i)) {
      s.seqA.push_back({Op::Kind::Destroy, i, 0});
    }
  }
  // Sequence B: the SAME create phase as A — a live entity's slot is
  // whatever the free list's top holds at its create, and an earlier
  // dead-slot destroy would have pushed that slot back on top (the
  // LIFO requirement, file preamble). The difference is WHEN the dead
  // destroys happen: in B each dead entity is destroyed at a
  // PRNG-assigned round boundary (0 = before round 1, ..., 6 = after
  // round 6), interleaved THROUGH the component phase instead of at
  // its end. The component steps run round-robin over the live
  // entities (the dead ones carry no script in B), each round in a
  // Fisher-Yates permutation (the per-entity script order is
  // preserved); optional scratch pairs (component-less, free-list-
  // neutral — no live create follows any of them) ride the boundaries
  // ahead of the dead destroys.
  std::vector<std::uint32_t> deadBoundaries(kScenarioEntities,
                                            kMaxScriptSteps);
  for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
    if (isDead(i) && !deterministic) {
      deadBoundaries[i] = rng.next_range(0u, kMaxScriptSteps + 1u);
    }
  }
  for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
    s.seqB.push_back({Op::Kind::Create, i, 0});
  }
  for (std::uint32_t step = 1; step <= kMaxScriptSteps; ++step) {
    const std::uint32_t boundary = step - 1;
    if (!deterministic && rng.next_range(0u, 4u) == 0u) {
      s.seqB.push_back({Op::Kind::ScratchCreate, 0, 0});
      s.seqB.push_back({Op::Kind::ScratchDestroy, 0, 0});
      ++s.extraPairs;
    }
    for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
      if (isDead(i) && deadBoundaries[i] == boundary) {
        s.seqB.push_back({Op::Kind::Destroy, i, 0});
      }
    }
    std::vector<std::uint32_t> live;
    live.reserve(kScenarioEntities);
    for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
      if (!isDead(i)) live.push_back(i);
    }
    if (!deterministic) {
      for (std::uint32_t j = live.size(); j > 1; --j) {
        const std::uint32_t k = rng.next_range(0u, j);
        std::swap(live[j - 1], live[k]);
      }
    }
    for (std::uint32_t i : live) {
      const std::vector<Op> script = scriptFor(i);
      if (step <= script.size()) {
        s.seqB.push_back(script[step - 1]);
      }
    }
  }
  if (!deterministic && rng.next_range(0u, 4u) == 0u) {
    s.seqB.push_back({Op::Kind::ScratchCreate, 0, 0});
    s.seqB.push_back({Op::Kind::ScratchDestroy, 0, 0});
    ++s.extraPairs;
  }
  for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
    if (isDead(i) && deadBoundaries[i] == kMaxScriptSteps) {
      s.seqB.push_back({Op::Kind::Destroy, i, 0});
    }
  }
  return s;
}

// ---------------------------------------------------------------------------
// The oracle: an independent model of the final state and of the
// first-seen archetype order (ids), per sequence
// ---------------------------------------------------------------------------

struct EntityState {
  std::uint16_t slot{};
  std::uint16_t generation{};
  bool alive{false};
  std::uint32_t set{0};  // component-set mask
  std::int32_t valueA{0};
  std::int32_t valueB{0};
  std::int32_t valueC{0};
};

struct OracleResult {
  std::vector<EntityState> entities;
  std::vector<std::uint32_t> archetypeOrder;  // masks, first-seen order
};

OracleResult runOracle(const std::vector<Op>& seq) {
  OracleResult r;
  r.entities.resize(kScenarioEntities);
  std::vector<std::uint16_t> gen(kScenarioCapacity, 1u);
  std::vector<std::uint16_t> freeStack;
  freeStack.reserve(kScenarioCapacity);
  for (std::uint32_t i = 0; i < kScenarioCapacity; ++i) {
    freeStack.push_back(static_cast<std::uint16_t>(i));
  }
  std::uint16_t scratchSlot{};
  auto noteSet = [&r](std::uint32_t set) {
    if (set == 0) return;
    for (std::uint32_t m : r.archetypeOrder) {
      if (m == set) return;
    }
    r.archetypeOrder.push_back(set);
  };
  for (const Op& op : seq) {
    EntityState* e = nullptr;
    switch (op.kind) {
      case Op::Kind::Create:
      case Op::Kind::ScratchCreate: {
        const std::uint16_t slot = freeStack.back();
        freeStack.pop_back();
        if (op.kind == Op::Kind::Create) {
          e = &r.entities[op.entity];
          e->slot = slot;
          e->generation = gen[slot];
          e->alive = true;
        } else {
          scratchSlot = slot;
        }
        break;
      }
      case Op::Kind::Destroy:
      case Op::Kind::ScratchDestroy: {
        std::uint16_t slot;
        if (op.kind == Op::Kind::Destroy) {
          e = &r.entities[op.entity];
          slot = e->slot;
          e->alive = false;
          e->set = 0;
          e->valueA = 0;
          e->valueB = 0;
          e->valueC = 0;
        } else {
          slot = scratchSlot;
        }
        gen[slot] = static_cast<std::uint16_t>(gen[slot] + 1u);
        if (gen[slot] == 0) gen[slot] = 1u;  // the engine's reserved-0 skip
        freeStack.push_back(slot);
        break;
      }
      case Op::Kind::AddA:
        e = &r.entities[op.entity];
        e->set |= kSetA;
        e->valueA = op.value;
        break;
      case Op::Kind::AddB:
        e = &r.entities[op.entity];
        e->set |= kSetB;
        e->valueB = op.value;
        break;
      case Op::Kind::AddC:
        e = &r.entities[op.entity];
        e->set |= kSetC;
        e->valueC = op.value;
        break;
      case Op::Kind::RemA:
        e = &r.entities[op.entity];
        e->set &= ~kSetA;
        e->valueA = 0;
        break;
      case Op::Kind::RemB:
        e = &r.entities[op.entity];
        e->set &= ~kSetB;
        e->valueB = 0;
        break;
      case Op::Kind::RemC:
        e = &r.entities[op.entity];
        e->set &= ~kSetC;
        e->valueC = 0;
        break;
      default:
        break;
    }
    // The engine creates an archetype for the op's TARGET set (its
    // state after the op; empty = the entity leaves the archetypes).
    if (e != nullptr &&
        (op.kind == Op::Kind::AddA || op.kind == Op::Kind::AddB ||
         op.kind == Op::Kind::AddC || op.kind == Op::Kind::RemA ||
         op.kind == Op::Kind::RemB || op.kind == Op::Kind::RemC)) {
      noteSet(e->set);
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// The contract's prediction: the visit sequence of a query, computed
// from the final state + first-seen archetype order (no operation
// history)
// ---------------------------------------------------------------------------

struct Visit {
  std::uint16_t slot{};
  std::uint16_t generation{};
  std::int32_t a{0};
  std::int32_t b{0};
  std::int32_t c{0};
};

inline bool operator==(const Visit& x, const Visit& y) noexcept {
  return x.slot == y.slot && x.generation == y.generation && x.a == y.a &&
         x.b == y.b && x.c == y.c;
}

std::vector<Visit> expectedVisits(const OracleResult& o, std::uint32_t queryMask) {
  std::vector<Visit> out;
  if (queryMask == 0) {
    // The empty query: every live entity, ascending slot order, no
    // archetype grouping.
    std::vector<std::uint32_t> liveIdx;
    liveIdx.reserve(kScenarioEntities);
    for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
      if (o.entities[i].alive) liveIdx.push_back(i);
    }
    std::sort(liveIdx.begin(), liveIdx.end(),
              [&o](std::uint32_t x, std::uint32_t y) {
                return o.entities[x].slot < o.entities[y].slot;
              });
    for (std::uint32_t i : liveIdx) {
      const EntityState& e = o.entities[i];
      out.push_back(
          Visit{e.slot, e.generation, e.valueA, e.valueB, e.valueC});
    }
    return out;
  }
  // Non-empty query: the matching archetypes (superset match) in
  // ascending archetype id = first-seen order; within each, the live
  // entities in ascending slot order (the dense-id scheme).
  std::vector<std::uint32_t> entityIdx;
  entityIdx.reserve(kScenarioEntities);
  for (std::uint32_t ai = 0; ai < o.archetypeOrder.size(); ++ai) {
    const std::uint32_t archSet = o.archetypeOrder[ai];
    if ((archSet & queryMask) != queryMask) continue;
    entityIdx.clear();
    for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
      if (o.entities[i].alive && o.entities[i].set == archSet) {
        entityIdx.push_back(i);
      }
    }
    std::sort(entityIdx.begin(), entityIdx.end(),
              [&o](std::uint32_t x, std::uint32_t y) {
                return o.entities[x].slot < o.entities[y].slot;
              });
    for (std::uint32_t i : entityIdx) {
      const EntityState& e = o.entities[i];
      out.push_back(
          Visit{e.slot, e.generation, e.valueA, e.valueB, e.valueC});
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Running a sequence on a real World and collecting the observed visits
// ---------------------------------------------------------------------------

struct WorldRun {
  laige::World world;
  std::vector<laige::Entity> handles;
  laige::Entity scratch{};
};

WorldRun makeRun() {
  auto w = laige::World::create(laige::World::Options{kScenarioCapacity});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << kScenarioCapacity << ") failed: "
                  << laige::errorName(w.error());
    std::abort();
  }
  laige::World world = std::move(w).takeValue();
  auto ra = world.registerComponent<IterA>();
  auto rb = world.registerComponent<IterB>();
  auto rc = world.registerComponent<IterC>();
  if (!ra.ok() || !rb.ok() || !rc.ok()) {
    ADD_FAILURE() << "registerComponent failed";
    std::abort();
  }
  // WorldRun is an aggregate (World is move-only, no default ctor —
  // the factory is its only construction path, entity.h).
  return WorldRun{std::move(world),
                  std::vector<laige::Entity>(kScenarioEntities),
                  laige::Entity{}};
}

bool applyOps(WorldRun& run, const std::vector<Op>& seq) {
  for (const Op& op : seq) {
    switch (op.kind) {
      case Op::Kind::Create: {
        auto r = run.world.create();
        if (!r.ok()) return false;
        run.handles[op.entity] = r.value();
        break;
      }
      case Op::Kind::ScratchCreate: {
        auto r = run.world.create();
        if (!r.ok()) return false;
        run.scratch = r.value();
        break;
      }
      case Op::Kind::Destroy:
        if (!run.world.destroy(run.handles[op.entity]).ok()) return false;
        break;
      case Op::Kind::ScratchDestroy:
        if (!run.world.destroy(run.scratch).ok()) return false;
        break;
      case Op::Kind::AddA:
        if (!run.world.addComponent<IterA>(run.handles[op.entity],
                                          IterA{op.value})
                 .ok()) {
          return false;
        }
        break;
      case Op::Kind::AddB:
        if (!run.world.addComponent<IterB>(run.handles[op.entity],
                                          IterB{op.value})
                 .ok()) {
          return false;
        }
        break;
      case Op::Kind::AddC:
        if (!run.world.addComponent<IterC>(run.handles[op.entity],
                                          IterC{op.value})
                 .ok()) {
          return false;
        }
        break;
      case Op::Kind::RemA:
        if (!run.world.removeComponent<IterA>(run.handles[op.entity]).ok()) {
          return false;
        }
        break;
      case Op::Kind::RemB:
        if (!run.world.removeComponent<IterB>(run.handles[op.entity]).ok()) {
          return false;
        }
        break;
      case Op::Kind::RemC:
        if (!run.world.removeComponent<IterC>(run.handles[op.entity]).ok()) {
          return false;
        }
        break;
      default:
        return false;
    }
  }
  return true;
}

// A rejected iteration (a nested each — impossible in this synchronous
// single-threaded flow, but never silently ignored: CORE-008) records
// a test failure; the visit vector is then incomplete and the
// comparisons below fail as well.
void expectEachOk(const char* label, const laige::Status& s) {
  EXPECT_TRUE(s.ok()) << "World::each rejected the iteration: " << label;
}

std::vector<Visit> collectAll(laige::World& w) {
  std::vector<Visit> out;
  expectEachOk("each<>",
               w.each<>([&](laige::Entity e) {
                 out.push_back(Visit{e.id, e.generation, 0, 0, 0});
               }));
  return out;
}

std::vector<Visit> collectA(laige::World& w) {
  std::vector<Visit> out;
  expectEachOk(
      "each<IterA>",
      w.each<IterA>(
          [&](laige::Entity e, const IterA& a) {
            out.push_back(Visit{e.id, e.generation, a.v, 0, 0});
          },
          laige::Read{}));
  return out;
}

std::vector<Visit> collectB(laige::World& w) {
  std::vector<Visit> out;
  expectEachOk(
      "each<IterB>",
      w.each<IterB>(
          [&](laige::Entity e, const IterB& b) {
            out.push_back(Visit{e.id, e.generation, 0, b.v, 0});
          },
          laige::Read{}));
  return out;
}

std::vector<Visit> collectC(laige::World& w) {
  std::vector<Visit> out;
  expectEachOk(
      "each<IterC>",
      w.each<IterC>(
          [&](laige::Entity e, const IterC& c) {
            out.push_back(Visit{e.id, e.generation, 0, 0, c.v});
          },
          laige::Read{}));
  return out;
}

std::vector<Visit> collectAB(laige::World& w) {
  std::vector<Visit> out;
  expectEachOk(
      "each<IterA, IterB>",
      w.each<IterA, IterB>(
          [&](laige::Entity e, const IterA& a, const IterB& b) {
            out.push_back(Visit{e.id, e.generation, a.v, b.v, 0});
          },
          laige::Read{}, laige::Read{}));
  return out;
}

// Per-visit comparison of slot, generation, and the query's components
// (the unqueried components of a Visit are 0 on the observed side and
// are never compared).
void expectVisits(const char* label, const std::vector<Visit>& observed,
                  const std::vector<Visit>& expected, std::uint32_t queryMask,
                  const char* context) {
  EXPECT_EQ(observed.size(), expected.size())
      << label << " (" << context << ")";
  for (std::size_t i = 0; i < observed.size() && i < expected.size(); ++i) {
    EXPECT_EQ(observed[i].slot, expected[i].slot)
        << label << " visit " << i << " (" << context << ")";
    EXPECT_EQ(observed[i].generation, expected[i].generation)
        << label << " visit " << i << " (" << context << ")";
    if ((queryMask & kSetA) != 0) {
      EXPECT_EQ(observed[i].a, expected[i].a)
          << label << " visit " << i << " (" << context << ")";
    }
    if ((queryMask & kSetB) != 0) {
      EXPECT_EQ(observed[i].b, expected[i].b)
          << label << " visit " << i << " (" << context << ")";
    }
    if ((queryMask & kSetC) != 0) {
      EXPECT_EQ(observed[i].c, expected[i].c)
          << label << " visit " << i << " (" << context << ")";
    }
  }
}

// FNV-1a 64-bit, big-endian byte order per u64 (the docs/testing.md §4
// KAT convention): the machine-greppable scenario hash.
std::uint64_t fnv1a64(const std::uint64_t* values, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis (FNV-1a spec)
  for (std::size_t i = 0; i < n; ++i) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (values[i] >> shift) & 0xFFull;
      h *= 0x100000001b3ull;  // FNV prime (FNV-1a spec)
    }
  }
  return h;
}

std::uint64_t hashVisits(const std::vector<Visit>& v) {
  std::vector<std::uint64_t> words;
  words.reserve(v.size() * 4);
  for (const Visit& x : v) {
    words.push_back(static_cast<std::uint64_t>(x.slot) |
                    (static_cast<std::uint64_t>(x.generation) << 16));
    words.push_back(static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(x.a)));
    words.push_back(static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(x.b)));
    words.push_back(static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(x.c)));
  }
  return fnv1a64(words.data(), words.size());
}

// The dense-id-order pin over an `each<IterA>` sequence: archetype 1's
// group is a strictly ascending-slot prefix, archetype 2's group a
// strictly ascending suffix, and the boundary is not a global slot
// order (archetype 2's first slot 4 is smaller than archetype 1's last
// slot 19 — a globally-ordered walk would visit them differently). The
// sequence must be the ORACLE's expected visits (full component state):
// the observed each<IterA> visits do not expose IterB, so the group
// boundary is invisible in them.
void expectArchetypeOrder(const std::vector<Visit>& visitsA) {
  std::size_t boundary = 0;
  while (boundary < visitsA.size() && visitsA[boundary].b == 0) {
    ++boundary;  // archetype 1 = {A}: no IterB value
  }
  EXPECT_EQ(boundary, 5u) << "archetype-1 group size";
  for (std::size_t g = 0; g < 2; ++g) {
    const std::size_t start = (g == 0) ? 0u : boundary;
    const std::size_t end = (g == 0) ? boundary : visitsA.size();
    for (std::size_t i = start + 1; i < end; ++i) {
      EXPECT_LT(visitsA[i - 1].slot, visitsA[i].slot)
          << "dense-id order broken in group " << g << " at visit " << i;
    }
  }
  // The scenario distinguishes archetype order from a global slot
  // order (the pin is not vacuous): archetype 2 opens below the top of
  // archetype 1.
  ASSERT_LT(boundary, visitsA.size());
  EXPECT_LT(visitsA[boundary].slot, visitsA[boundary - 1].slot)
      << "scenario must interleave the two archetype groups";
}

}  // namespace

// ---------------------------------------------------------------------------
// Known-answer test: the degenerate interleaving (no PRNG draws) with
// the committed visit sequences.
// ---------------------------------------------------------------------------

TEST(IterOrder, DeterministicVisitOrderKAT) {
  const std::uint64_t seed = laige::testing::TestSeed();
  const Scenario s = buildScenario(laige::Prng(seed), /*deterministic=*/true);
  const OracleResult oa = runOracle(s.seqA);
  const OracleResult ob = runOracle(s.seqB);

  // Convergence at the oracle level, incl. the entity->slot assignment.
  EXPECT_EQ(oa.archetypeOrder, ob.archetypeOrder);
  EXPECT_EQ(oa.archetypeOrder,
            (std::vector<std::uint32_t>{kSetA, kSetA | kSetB,
                                        kSetA | kSetB | kSetC}));
  for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
    const EntityState& ea = oa.entities[i];
    const EntityState& eb = ob.entities[i];
    EXPECT_EQ(ea.alive, eb.alive) << "entity " << i;
    if (ea.alive) {
      EXPECT_EQ(ea.slot, slotOf(i)) << "entity " << i;
      EXPECT_EQ(ea.slot, eb.slot) << "entity " << i;
      EXPECT_EQ(ea.generation, eb.generation) << "entity " << i;
      EXPECT_EQ(ea.set, eb.set) << "entity " << i;
      EXPECT_EQ(ea.valueA, eb.valueA) << "entity " << i;
      EXPECT_EQ(ea.valueB, eb.valueB) << "entity " << i;
      EXPECT_EQ(ea.valueC, eb.valueC) << "entity " << i;
    }
  }

  WorldRun ra = makeRun();
  WorldRun rb = makeRun();
  ASSERT_TRUE(applyOps(ra, s.seqA));
  ASSERT_TRUE(applyOps(rb, s.seqB));
  EXPECT_EQ(ra.world.archetypeCount(), oa.archetypeOrder.size());
  EXPECT_EQ(rb.world.archetypeCount(), ob.archetypeOrder.size());
  EXPECT_EQ(ra.world.entityCount(), kLiveEntityCount);
  EXPECT_EQ(rb.world.entityCount(), kLiveEntityCount);

  // The committed known-answer visit sequences (KAT; the oracle must
  // reproduce them — a mismatch in either direction fails loudly).
  // The empty query carries the full component state of every live
  // entity (entity i is at slot 19-i; e4 at slot 15 is component-less).
  const std::vector<Visit> katAll = {
      {4, 1, 1501, 1502, 0}, {5, 1, 1401, 0, 0}, {6, 1, 1301, 1302, 0},
      {8, 1, 1101, 1102, 0}, {9, 1, 1001, 0, 0}, {10, 1, 901, 902, 0},
      {11, 1, 801, 0, 0}, {13, 1, 601, 0, 0}, {14, 1, 501, 502, 0},
      {15, 1, 0, 0, 0}, {16, 1, 301, 302, 0}, {18, 1, 101, 102, 0},
      {19, 1, 1, 0, 0}};
  const std::vector<Visit> katA = {
      {5, 1, 1401, 0, 0},  {9, 1, 1001, 0, 0},  {11, 1, 801, 0, 0},
      {13, 1, 601, 0, 0},  {19, 1, 1, 0, 0},    {4, 1, 1501, 1502, 0},
      {6, 1, 1301, 1302, 0}, {8, 1, 1101, 1102, 0}, {10, 1, 901, 902, 0},
      {14, 1, 501, 502, 0}, {16, 1, 301, 302, 0}, {18, 1, 101, 102, 0}};
  const std::vector<Visit> katB = {
      {4, 1, 1501, 1502, 0}, {6, 1, 1301, 1302, 0}, {8, 1, 1101, 1102, 0},
      {10, 1, 901, 902, 0}, {14, 1, 501, 502, 0}, {16, 1, 301, 302, 0},
      {18, 1, 101, 102, 0}};
  const std::vector<Visit> katC = {};

  const std::vector<Visit> expAll = expectedVisits(oa, 0);
  const std::vector<Visit> expA = expectedVisits(oa, kSetA);
  const std::vector<Visit> expAB = expectedVisits(oa, kSetA | kSetB);
  const std::vector<Visit> expB = expectedVisits(oa, kSetB);
  const std::vector<Visit> expC = expectedVisits(oa, kSetC);
  EXPECT_EQ(expAll, katAll);
  EXPECT_EQ(expA, katA);
  EXPECT_EQ(expAB, katB);
  EXPECT_EQ(expB, katB);
  EXPECT_EQ(expC, katC);

  const std::vector<Visit> allA = collectAll(ra.world);
  const std::vector<Visit> allB = collectAll(rb.world);
  const std::vector<Visit> aA = collectA(ra.world);
  const std::vector<Visit> aB = collectA(rb.world);
  const std::vector<Visit> abA = collectAB(ra.world);
  const std::vector<Visit> abB = collectAB(rb.world);
  const std::vector<Visit> bA = collectB(ra.world);
  const std::vector<Visit> bB = collectB(rb.world);
  const std::vector<Visit> cA = collectC(ra.world);
  const std::vector<Visit> cB = collectC(rb.world);

  expectVisits("each<> world A vs oracle", allA, expAll, 0, "KAT");
  expectVisits("each<> world B vs world A", allB, allA, 0, "KAT");
  expectVisits("each<A> world A vs oracle", aA, expA, kSetA, "KAT");
  expectVisits("each<A> world B vs world A", aB, aA, kSetA, "KAT");
  expectVisits("each<A,B> world A vs oracle", abA, expAB, kSetA | kSetB, "KAT");
  expectVisits("each<A,B> world B vs world A", abB, abA, kSetA | kSetB, "KAT");
  expectVisits("each<B> world A vs oracle", bA, expB, kSetB, "KAT");
  expectVisits("each<B> world B vs world A", bB, bA, kSetB, "KAT");
  expectVisits("each<C> world A vs oracle", cA, expC, kSetC, "KAT");
  expectVisits("each<C> world B vs world A", cB, cA, kSetC, "KAT");

  expectArchetypeOrder(expA);

  const std::uint64_t hash =
      hashVisits(allA) * 31u + hashVisits(aA) * 31u + hashVisits(abA);
  std::printf("iter-order kat seed=0x%016llx extra_pairs=0 live=%u "
              "visits=%zu/%zu/%zu/%zu/%zu fnv1a=0x%016llx\n",
              static_cast<unsigned long long>(seed), kLiveEntityCount,
              allA.size(), aA.size(), abA.size(), bA.size(), cA.size(),
              static_cast<unsigned long long>(hash));
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Property test: convergent worlds with interleaved create/destroy and
// component moves iterate identically (fixed PRNG seed, three
// instantiations).
// ---------------------------------------------------------------------------

TEST(IterOrder, ConvergentWorldsIterateIdentically) {
  const std::uint64_t seed = laige::testing::TestSeed();
  for (std::uint32_t streamId : kScenarioStreamIds) {
    const char* ctx = "property";
    laige::Prng rng = laige::testing::TestPrng(streamId);
    const Scenario s = buildScenario(rng, /*deterministic=*/false);
    const OracleResult oa = runOracle(s.seqA);
    const OracleResult ob = runOracle(s.seqB);

    // The two histories converge on the identical iteration-relevant
    // final state: archetype id assignment, live slots (the
    // entity->id assignment), archetype assignment, component values.
    EXPECT_EQ(oa.archetypeOrder, ob.archetypeOrder) << ctx;
    EXPECT_EQ(oa.archetypeOrder,
              (std::vector<std::uint32_t>{kSetA, kSetA | kSetB,
                                          kSetA | kSetB | kSetC}))
        << ctx;
    for (std::uint32_t i = 0; i < kScenarioEntities; ++i) {
      const EntityState& ea = oa.entities[i];
      const EntityState& eb = ob.entities[i];
      EXPECT_EQ(ea.alive, eb.alive) << "entity " << i << " (" << ctx << ")";
      if (ea.alive) {
        EXPECT_EQ(ea.slot, slotOf(i)) << "entity " << i << " (" << ctx << ")";
        EXPECT_EQ(ea.slot, eb.slot)
            << "entity->id assignment, entity " << i << " (" << ctx << ")";
        EXPECT_EQ(ea.generation, eb.generation)
            << "entity " << i << " (" << ctx << ")";
        EXPECT_EQ(ea.set, eb.set) << "entity " << i << " (" << ctx << ")";
        EXPECT_EQ(ea.valueA, eb.valueA) << "entity " << i << " (" << ctx << ")";
        EXPECT_EQ(ea.valueB, eb.valueB) << "entity " << i << " (" << ctx << ")";
        EXPECT_EQ(ea.valueC, eb.valueC) << "entity " << i << " (" << ctx << ")";
      }
    }

    WorldRun ra = makeRun();
    WorldRun rb = makeRun();
    ASSERT_TRUE(applyOps(ra, s.seqA)) << "sequence A op failed (" << ctx << ")";
    ASSERT_TRUE(applyOps(rb, s.seqB)) << "sequence B op failed (" << ctx << ")";
    EXPECT_EQ(ra.world.archetypeCount(), oa.archetypeOrder.size()) << ctx;
    EXPECT_EQ(rb.world.archetypeCount(), ob.archetypeOrder.size()) << ctx;
    EXPECT_EQ(ra.world.entityCount(), kLiveEntityCount) << ctx;
    EXPECT_EQ(rb.world.entityCount(), kLiveEntityCount) << ctx;

    const std::vector<Visit> expAll = expectedVisits(oa, 0);
    const std::vector<Visit> expA = expectedVisits(oa, kSetA);
    const std::vector<Visit> expAB = expectedVisits(oa, kSetA | kSetB);
    const std::vector<Visit> expB = expectedVisits(oa, kSetB);
    const std::vector<Visit> expC = expectedVisits(oa, kSetC);

    const std::vector<Visit> allA = collectAll(ra.world);
    const std::vector<Visit> allB = collectAll(rb.world);
    const std::vector<Visit> aA = collectA(ra.world);
    const std::vector<Visit> aB = collectA(rb.world);
    const std::vector<Visit> abA = collectAB(ra.world);
    const std::vector<Visit> abB = collectAB(rb.world);
    const std::vector<Visit> bA = collectB(ra.world);
    const std::vector<Visit> bB = collectB(rb.world);
    const std::vector<Visit> cA = collectC(ra.world);
    const std::vector<Visit> cB = collectC(rb.world);

    expectVisits("each<> world A vs oracle", allA, expAll, 0, ctx);
    expectVisits("each<> world B vs world A", allB, allA, 0, ctx);
    expectVisits("each<A> world A vs oracle", aA, expA, kSetA, ctx);
    expectVisits("each<A> world B vs world A", aB, aA, kSetA, ctx);
    expectVisits("each<A,B> world A vs oracle", abA, expAB, kSetA | kSetB, ctx);
    expectVisits("each<A,B> world B vs world A", abB, abA, kSetA | kSetB, ctx);
    expectVisits("each<B> world A vs oracle", bA, expB, kSetB, ctx);
    expectVisits("each<B> world B vs world A", bB, bA, kSetB, ctx);
    expectVisits("each<C> world A vs oracle", cA, expC, kSetC, ctx);
    expectVisits("each<C> world B vs world A", cB, cA, kSetC, ctx);

    expectArchetypeOrder(expA);

    // Machine-greppable identity line (docs/testing.md §4): the seed,
    // the PRNG-placed extra pairs, the visit counts, and the FNV-1a 64
    // hash of world A's five visit sequences — byte-identical across
    // CI runs of the same commit.
    const std::uint64_t hash =
        hashVisits(allA) * 31u + hashVisits(aA) * 31u + hashVisits(abA);
    std::printf("iter-order scenario=%u seed=0x%016llx extra_pairs=%u "
                "live=%u visits=%zu/%zu/%zu/%zu/%zu fnv1a=0x%016llx\n",
                streamId, static_cast<unsigned long long>(seed),
                s.extraPairs, kLiveEntityCount, allA.size(), aA.size(),
                abA.size(), bA.size(), cA.size(),
                static_cast<unsigned long long>(hash));
    std::fflush(stdout);
  }
}
