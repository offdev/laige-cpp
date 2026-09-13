// laige-sim query API + iteration-legality suite (M1-ECS-04).
//
// Step Verify scope (roadmap/M1-heartbeat.md):
//   - ctx.each<T1, T2, ...>(access_flags, fn) — in M1 the query is
//     World::each (M1-SYS-01's SystemContext delegates to it): iterate
//     the entities having ALL the listed components (superset match),
//     access declared per component (Read / Write tags; the callback
//     gets a const T& for Read, a T& for Write)
//   - iteration-legality enforcement: a write during a read
//     iteration, or an add/remove/destroy/clear touching an iterated
//     archetype (and a nested each()) asserts in debug (forked
//     SIGABRT child) and returns Status + skip-with-log in release
//     (FR-12.3; the rules are documented in query.h)
//   - no hidden allocations in the query path: the 10k-entity
//     iteration window allocates zero heap (test-only operator-new
//     counter, non-sanitizer trees; the sanitizer trees cover the
//     property with their leak-free run of the same loop)
//   - mixed-component queries return the exact sets; the visit order
//     is archetype-id order, then ascending slot order (M1-ECS-05
//     pins the deterministic contract over this order)
//
// Runs as CTest `query` (the step's Verify command: `ctest -R query`):
// a filtered view of the shared laige-sim_tests executable, selecting
// exactly the suites below (the Query* prefix).

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/sim/entity.h"

#if defined(LAIGE_ALLOC_COUNTER)
#include "logging_alloc_counter.h"
#endif

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "query_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "query_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "query_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define QUERY_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define QUERY_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if QUERY_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "query_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test component types (global scope on purpose)
//
// LAIGE_COMPONENT specializes laige::detail::ComponentTraits, which
// the C++ standard requires to be declared in the primary template's
// enclosing namespace — so the marks cannot sit in an anonymous
// namespace. QueryUnreg is marked but never registered (the
// "unregistered query matches nothing" property).
// ---------------------------------------------------------------------------

struct QueryPos {
  std::int32_t x;
  std::int32_t y;
};
LAIGE_COMPONENT(QueryPos)

struct QueryVel {
  std::int64_t v;
};
LAIGE_COMPONENT(QueryVel)

struct QueryFlag {
  std::int32_t f;
};
LAIGE_COMPONENT(QueryFlag)

struct QueryUnreg {
  std::int32_t v;
};
LAIGE_COMPONENT(QueryUnreg)

namespace {

// One world, taken out of its Result so the test holds a mutable
// lvalue (Result::value() is const; takeValue() && moves the storage
// out — the documented ownership-transfer path).
laige::World makeWorld(std::uint32_t capacity) {
  auto w = laige::World::create(laige::World::Options{capacity});
  if (!w.ok()) {
    ADD_FAILURE() << "World::create(" << capacity
                  << ") failed: " << laige::errorName(w.error());
    abort();
  }
  return std::move(w).takeValue();
}

// The visited handles' slot ids, sorted — the exact-set comparison
// (visit order is pinned separately, where the test cares about it).
std::vector<std::uint16_t> sortedSlotIds(const std::vector<laige::Entity>& v) {
  std::vector<std::uint16_t> out;
  out.reserve(v.size());
  for (const auto& e : v) out.push_back(e.id);
  std::sort(out.begin(), out.end());
  return out;
}

// The expected slot ids, sorted (the set comparison is order-blind).
std::vector<std::uint16_t> sortedIds(std::initializer_list<std::uint16_t> ids) {
  std::vector<std::uint16_t> out(ids);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The forked debug-assert child (POSIX, debug builds only)
//
// One world, one live entity e = {QueryPos} (plus QueryVel for the
// remove case), then scenario k runs ONE illegal mutation inside an
// active each<QueryPos>(Read) iteration. The M1-ECS-04 guard must
// abort the process (SIGABRT) inside the mutation; the trailing
// _exit(1) is reached only when NO assert fired (a regression — the
// parent fails the test). The child builds its world itself (no
// gtest calls after fork()).
// ---------------------------------------------------------------------------

#if defined(__unix__) && !defined(NDEBUG)
void runGuardViolationCase(std::uint32_t k) {
  auto w = laige::World::create(laige::World::Options{2});
  if (!w.ok()) _exit(117);
  laige::World world = std::move(w).takeValue();
  if (!world.registerComponent<QueryPos>().ok() ||
      !world.registerComponent<QueryVel>().ok()) {
    _exit(117);
  }
  auto r = world.create();
  if (!r.ok()) _exit(117);
  const laige::Entity e = r.value();
  if (!world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok()) _exit(117);
  if (k == 2 && !world.addComponent<QueryVel>(e, QueryVel{7}).ok()) {
    _exit(117);
  }
  switch (k) {
    case 0:  // in-place write to the Read-declared component
      (void)world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                    (void)world.addComponent<QueryPos>(
                                        e2, QueryPos{9, 9});
                                  },
                                 laige::Read{});
      break;
    case 1:  // structural add: {Pos} -> {Pos,Vel}; the source is iterated
      (void)world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                    (void)world.addComponent<QueryVel>(
                                        e2, QueryVel{9});
                                  },
                                 laige::Read{});
      break;
    case 2:  // structural remove: e leaves the iterated archetype
      (void)world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                    (void)world.removeComponent<QueryPos>(e2);
                                  },
                                 laige::Read{});
      break;
    case 3:  // destroy of an iterated entity
      (void)world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                    (void)world.destroy(e2);
                                  },
                                 laige::Read{});
      break;
    case 4:  // clear() under the active iteration
      (void)world.each<QueryPos>([&](laige::Entity, const QueryPos&) {
                                    (void)world.clear();
                                  },
                                 laige::Read{});
      break;
    case 5:  // nested iteration
      (void)world.each<QueryPos>([&](laige::Entity, const QueryPos&) {
                                    (void)world.each<QueryPos>(
                                        [](laige::Entity, const QueryPos&) {},
                                        laige::Read{});
                                  },
                                 laige::Read{});
      break;
    default:
      _exit(118);
  }
  _exit(1);  // unreachable when the guard works
}
#endif

// The six debug-assert tests: one per illegal-mutation scenario.
// POSIX + debug only (fork); the Windows and release jobs skip with
// a reason (the release degradation path is the *InRelease test).
#if defined(__unix__)
#  if defined(NDEBUG)
#    define QUERY_DEBUG_ASSERT_TEST(Name, CaseId) \
  TEST(QueryLegality, Name) { \
    GTEST_SKIP() << "assert-based iteration-legality detection is a " \
                   "debug-build property (NDEBUG build)"; \
  }
#  else
#    define QUERY_DEBUG_ASSERT_TEST(Name, CaseId) \
  TEST(QueryLegality, Name) { \
    const pid_t pid = fork(); \
    ASSERT_GE(pid, 0); \
    if (pid == 0) { \
      runGuardViolationCase(CaseId); \
    } \
    int status = 0; \
    ASSERT_EQ(waitpid(pid, &status, 0), pid); \
    EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) \
        << "expected the iteration-legality assert to abort the child " \
           "(SIGABRT)"; \
  }
#  endif
#else
#  define QUERY_DEBUG_ASSERT_TEST(Name, CaseId) \
  TEST(QueryLegality, Name) { \
    GTEST_SKIP() << "fork() is not available on Windows; the debug assert " \
                   "is exercised on the POSIX jobs."; \
  }
#endif

// ---------------------------------------------------------------------------
// Query semantics: exact sets, order, unregistered types, access tags
// ---------------------------------------------------------------------------

TEST(QueryBasics, MixedComponentsExactSet) {
  // Six entities spanning five component sets; every query returns
  // exactly its superset match (extra components do not exclude an
  // entity; a missing one does).
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
  ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
  ASSERT_TRUE(world.registerComponent<QueryFlag>().ok());
  laige::Entity e0, e1, e2, e3, e4, e5;
  {
    auto r = world.create(); ASSERT_TRUE(r.ok()); e0 = r.value();
    auto r2 = world.create(); ASSERT_TRUE(r2.ok()); e1 = r2.value();
    auto r3 = world.create(); ASSERT_TRUE(r3.ok()); e2 = r3.value();
    auto r4 = world.create(); ASSERT_TRUE(r4.ok()); e3 = r4.value();
    auto r5 = world.create(); ASSERT_TRUE(r5.ok()); e4 = r5.value();
    auto r6 = world.create(); ASSERT_TRUE(r6.ok()); e5 = r6.value();
  }
  ASSERT_TRUE(world.addComponent<QueryPos>(e0, QueryPos{0, 0}).ok());
  ASSERT_TRUE(world.addComponent<QueryPos>(e1, QueryPos{10, 11}).ok());
  ASSERT_TRUE(world.addComponent<QueryVel>(e1, QueryVel{100}).ok());
  ASSERT_TRUE(world.addComponent<QueryPos>(e2, QueryPos{20, 21}).ok());
  ASSERT_TRUE(world.addComponent<QueryVel>(e2, QueryVel{200}).ok());
  ASSERT_TRUE(world.addComponent<QueryFlag>(e2, QueryFlag{2}).ok());
  ASSERT_TRUE(world.addComponent<QueryVel>(e3, QueryVel{300}).ok());
  ASSERT_TRUE(world.addComponent<QueryPos>(e5, QueryPos{50, 51}).ok());
  ASSERT_TRUE(world.addComponent<QueryFlag>(e5, QueryFlag{5}).ok());
  // e4 carries no components.

  // <Pos, Vel> matches {Pos,Vel} (e1) and {Pos,Vel,Flag} (e2) — e2 is
  // the superset case: its extra Flag does not exclude it.
  {
    std::vector<laige::Entity> vis;
    std::int32_t sumX = 0;
    std::int64_t sumV = 0;
    auto s = world.each<QueryPos, QueryVel>(
        [&](laige::Entity e, const QueryPos& p, const QueryVel& v) {
          vis.push_back(e);
          sumX += p.x;
          sumV += v.v;
        },
        laige::Read{}, laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(vis.size(), 2u);
    EXPECT_EQ(sortedSlotIds(vis), sortedIds({e1.id, e2.id}));
    EXPECT_EQ(sumX, 10 + 20);
    EXPECT_EQ(sumV, 100 + 200);
  }
  // <Flag> matches exactly the two Flag carriers.
  {
    std::vector<laige::Entity> vis;
    std::int32_t sumF = 0;
    auto s = world.each<QueryFlag>([&](laige::Entity e, const QueryFlag& f) {
                                      vis.push_back(e);
                                      sumF += f.f;
                                    },
                                   laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sortedSlotIds(vis), sortedIds({e2.id, e5.id}));
    EXPECT_EQ(sumF, 2 + 5);
  }
  // <Pos> matches the four Pos carriers.
  {
    std::vector<laige::Entity> vis;
    auto s = world.each<QueryPos>([&](laige::Entity e, const QueryPos&) {
                                     vis.push_back(e);
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sortedSlotIds(vis), sortedIds({e0.id, e1.id, e2.id, e5.id}));
  }
  // <Pos, Flag> matches {Pos,Flag} (e5) and {Pos,Vel,Flag} (e2).
  {
    std::vector<laige::Entity> vis;
    auto s = world.each<QueryPos, QueryFlag>(
        [&](laige::Entity e, const QueryPos&, const QueryFlag&) {
          vis.push_back(e);
        },
        laige::Read{}, laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sortedSlotIds(vis), sortedIds({e2.id, e5.id}));
  }
}

TEST(QueryBasics, EmptyQueryVisitsAllLiveInSlotOrder) {
  // each<> visits every live entity (component-less ones included) in
  // ascending slot-id order.
  laige::World world = makeWorld(8);
  laige::Entity e0, e1, e2, e3;
  {
    auto r = world.create(); ASSERT_TRUE(r.ok()); e0 = r.value();
    auto r2 = world.create(); ASSERT_TRUE(r2.ok()); e1 = r2.value();
    auto r3 = world.create(); ASSERT_TRUE(r3.ok()); e2 = r3.value();
    auto r4 = world.create(); ASSERT_TRUE(r4.ok()); e3 = r4.value();
  }
  ASSERT_TRUE(world.destroy(e2).ok());
  std::vector<std::uint16_t> vis;
  auto s = world.each<>([&](laige::Entity e) { vis.push_back(e.id); });
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(vis.size(), 3u);
  // Strictly ascending slot order:
  for (std::size_t i = 1; i < vis.size(); ++i) {
    EXPECT_LT(vis[i - 1], vis[i]);
  }
  EXPECT_EQ(vis,
            (std::vector<std::uint16_t>{e3.id, e1.id, e0.id}));
}

TEST(QueryBasics, EmptyQueryToleratesConcurrentDestroy) {
  // The empty query iterates no archetype rows, so a destroy inside
  // the callback is LEGAL (query.h): the destroyed entity is simply
  // not visited when the scan has not reached it yet; a destroyed
  // entity is never re-visited. No guard violation, no Status.
  laige::World world = makeWorld(8);
  laige::Entity e0, e1, e2, e3;
  {
    auto r = world.create(); ASSERT_TRUE(r.ok()); e0 = r.value();
    auto r2 = world.create(); ASSERT_TRUE(r2.ok()); e1 = r2.value();
    auto r3 = world.create(); ASSERT_TRUE(r3.ok()); e2 = r3.value();
    auto r4 = world.create(); ASSERT_TRUE(r4.ok()); e3 = r4.value();
  }
  int visits = 0;
  int destroys = 0;
  auto s = world.each<>([&](laige::Entity e) {
    visits++;
    if (e == e1) {
      destroys += world.destroy(e).ok() ? 1 : 0;
    }
  });
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(visits, 4);      // e1 was visited before its destroy ran
  EXPECT_EQ(destroys, 1);
  EXPECT_FALSE(world.isValid(e1));
  EXPECT_EQ(world.entityCount(), 3u);
}

TEST(QueryBasics, UnregisteredTypeMatchesNothing) {
  // A listed component not registered in this world matches nothing:
  // the iteration runs zero times and returns ok (a pure query, like
  // has<T> reading false). No archetype is created for the query.
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
  laige::Entity e;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
  int visits = 0;
  auto s = world.each<QueryUnreg>(
      [&](laige::Entity, const QueryUnreg&) { visits++; }, laige::Read{});
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(visits, 0);
  EXPECT_EQ(world.archetypeCount(), 1u);
}

TEST(QueryBasics, AccessTagsDetermineReferenceKind) {
  // Read gives a const reference (a write through it would not
  // compile); Write gives a mutable reference — the intended mutation
  // path, legal by the guard (a write during a WRITE iteration).
  laige::World world = makeWorld(2);
  ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
  ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
  laige::Entity e;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
  ASSERT_TRUE(world.addComponent<QueryVel>(e, QueryVel{0}).ok());

  int reads = 0;
  auto s = world.each<QueryPos>([&](laige::Entity, const QueryPos& p) {
                                   reads += (p.x == 1 && p.y == 2) ? 1 : 0;
                                 },
                                laige::Read{});
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(reads, 1);

  int writes = 0;
  auto s2 = world.each<QueryPos, QueryVel>(
      [&](laige::Entity, const QueryPos& p, QueryVel& v) {
        if (p.x == 1) {
          v.v = 12345;
          writes++;
        }
      },
      laige::Read{}, laige::Write{});
  ASSERT_TRUE(s2.ok());
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(world.get<QueryVel>(e)->v, 12345);
}

TEST(QueryBasics, IterationOrderIsArchetypeThenSlot) {
  // Visit order = ascending archetype id (creation order), then
  // ascending slot id within the archetype (M1-ECS-05 pins this as
  // the deterministic contract).
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
  ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
  laige::Entity a, b, c, d;
  {
    auto r = world.create(); ASSERT_TRUE(r.ok()); a = r.value();
    auto r2 = world.create(); ASSERT_TRUE(r2.ok()); b = r2.value();
    auto r3 = world.create(); ASSERT_TRUE(r3.ok()); c = r3.value();
    auto r4 = world.create(); ASSERT_TRUE(r4.ok()); d = r4.value();
  }
  // Archetype creation order: {Pos} (a) -> {Pos,Vel} (a) -> {Vel} (c).
  ASSERT_TRUE(world.addComponent<QueryPos>(a, QueryPos{0, 0}).ok());
  ASSERT_TRUE(world.addComponent<QueryVel>(a, QueryVel{0}).ok());
  ASSERT_TRUE(world.addComponent<QueryPos>(b, QueryPos{1, 0}).ok());
  ASSERT_TRUE(world.addComponent<QueryVel>(c, QueryVel{1}).ok());
  // d joins the existing {Pos} archetype (no new archetype).
  ASSERT_TRUE(world.addComponent<QueryPos>(d, QueryPos{2, 0}).ok());

  // <Pos> visits {Pos} (id 1: d, b in slot order) then {Pos,Vel}
  // (id 2: a).
  {
    std::vector<std::uint16_t> vis;
    auto s = world.each<QueryPos>([&](laige::Entity e, const QueryPos&) {
                                     vis.push_back(e.id);
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(vis,
              (std::vector<std::uint16_t>{d.id, b.id, a.id}));
  }
  // <Vel> visits {Pos,Vel} (id 2: a) then {Vel} (id 3: c).
  {
    std::vector<std::uint16_t> vis;
    auto s = world.each<QueryVel>([&](laige::Entity e, const QueryVel&) {
                                     vis.push_back(e.id);
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(vis, (std::vector<std::uint16_t>{a.id, c.id}));
  }
  // <Pos, Vel> visits only {Pos,Vel}.
  {
    std::vector<std::uint16_t> vis;
    auto s = world.each<QueryPos, QueryVel>(
        [&](laige::Entity e, const QueryPos&, const QueryVel&) {
          vis.push_back(e.id);
        },
        laige::Read{}, laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(vis, (std::vector<std::uint16_t>{a.id}));
  }
}

// ---------------------------------------------------------------------------
// Iteration legality: the legal mutations, the guard's release, and the
// release-mode skip behavior
// ---------------------------------------------------------------------------

TEST(QueryLegality, LegalMutationsDuringIteration) {
  // The legal side of the legality rules (query.h): create() touches
  // no rows; a structural move whose source AND target archetypes are
  // outside the matched set is legal; Write references and in-place
  // addComponent overwrites of Write-declared components are the
  // intended mutation paths.
  laige::World world = makeWorld(8);
  ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
  ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
  ASSERT_TRUE(world.registerComponent<QueryFlag>().ok());
  laige::Entity eIter, eOther;
  {
    auto r = world.create(); ASSERT_TRUE(r.ok()); eIter = r.value();
    auto r2 = world.create(); ASSERT_TRUE(r2.ok()); eOther = r2.value();
  }
  ASSERT_TRUE(world.addComponent<QueryPos>(eIter, QueryPos{1, 2}).ok());
  ASSERT_TRUE(world.addComponent<QueryVel>(eIter, QueryVel{0}).ok());
  ASSERT_TRUE(world.addComponent<QueryFlag>(eOther, QueryFlag{9}).ok());

  laige::Entity created{};
  int visits = 0, creates = 0, moves = 0;
  auto s = world.each<QueryPos>([&](laige::Entity, const QueryPos&) {
                                   visits++;
                                   auto r = world.create();
                                   if (r.ok()) {
                                     created = r.value();
                                     creates++;
                                   }
                                   // {Flag} -> {Flag,Vel}: a new archetype
                                   // (not in <Pos>'s matched set) and the
                                   // source {Flag} is not matched either.
                                   moves += world.addComponent<QueryVel>(
                                                 eOther, QueryVel{1})
                                                 .ok()
                                       ? 1
                                       : 0;
                                 },
                                laige::Read{});
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(visits, 1);
  EXPECT_EQ(creates, 1);
  EXPECT_TRUE(world.isValid(created));
  EXPECT_EQ(moves, 1);
  EXPECT_TRUE(world.has<QueryVel>(eOther));
  EXPECT_TRUE(world.has<QueryFlag>(eOther));
  // {Pos} (eIter, now empty), {Pos,Vel} (eIter), {Flag} (eOther, now
  // empty), {Flag,Vel} (eOther) — the move created the fourth
  // archetype (empty archetypes are kept: archetype.h).
  EXPECT_EQ(world.archetypeCount(), 4u);
  // The iterated entity was untouched by the callback's mutations.
  EXPECT_EQ(world.get<QueryVel>(eIter)->v, 0);

  // Write-declared component: the reference store and the in-place
  // addComponent overwrite are both legal writes.
  int writes = 0;
  auto s2 = world.each<QueryPos, QueryVel>(
      [&](laige::Entity e, const QueryPos&, QueryVel& v) {
        v.v += 1;  // direct store through the Write reference
        writes += world.addComponent<QueryVel>(e, QueryVel{7}).ok() ? 1 : 0;
      },
      laige::Read{}, laige::Write{});
  ASSERT_TRUE(s2.ok());
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(world.get<QueryVel>(eIter)->v, 7);
}

TEST(QueryLegality, GuardReleasesAfterIteration) {
  // The guard lives from the first callback to the last: after each()
  // returns, the same structural mutation is legal again.
  laige::World world = makeWorld(4);
  ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
  ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
  laige::Entity e;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
  int visits = 0;
  auto s = world.each<QueryPos>(
      [&](laige::Entity, const QueryPos&) { visits++; }, laige::Read{});
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(visits, 1);
  // Legal now — no active iteration:
  EXPECT_TRUE(world.addComponent<QueryVel>(e, QueryVel{9}).ok());
  EXPECT_TRUE(world.has<QueryVel>(e));
}

TEST(QueryLegality, IllegalMutationsSkippedInRelease) {
  // The release degradation path (FR-12.3): every illegal mutation
  // returns InvalidArgument, is SKIPPED (never applied), and the
  // iteration CONTINUES over the unmutated storage. Debug builds
  // assert instead (the *AbortsInDebug tests).
#ifdef NDEBUG
  // In-place write to a Read-declared component:
  {
    laige::World world = makeWorld(2);
    ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
    laige::Entity e;
    {
      auto r = world.create();
      ASSERT_TRUE(r.ok());
      e = r.value();
    }
    ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
    int calls = 0;
    int errors = 0;
    auto s = world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                     calls++;
                                     auto s2 = world.addComponent<QueryPos>(
                                         e2, QueryPos{9, 9});
                                     if (s2.isError() &&
                                         s2.error() ==
                                             laige::ErrorCode::InvalidArgument) {
                                       errors++;
                                     }
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(errors, 1);
    EXPECT_EQ(world.get<QueryPos>(e)->x, 1);  // the write was skipped
    // The same mutation succeeds after the iteration ended:
    EXPECT_TRUE(world.addComponent<QueryPos>(e, QueryPos{9, 9}).ok());
    EXPECT_EQ(world.get<QueryPos>(e)->x, 9);
  }
  // Structural add (source archetype iterated); the iteration
  // CONTINUES over both entities:
  {
    laige::World world = makeWorld(4);
    ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
    ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
    laige::Entity e0, e1;
    {
      auto r = world.create(); ASSERT_TRUE(r.ok()); e0 = r.value();
      auto r2 = world.create(); ASSERT_TRUE(r2.ok()); e1 = r2.value();
    }
    ASSERT_TRUE(world.addComponent<QueryPos>(e0, QueryPos{0, 0}).ok());
    ASSERT_TRUE(world.addComponent<QueryPos>(e1, QueryPos{1, 0}).ok());
    int visits = 0, errors = 0;
    auto s = world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                     visits++;
                                     auto s2 = world.addComponent<QueryVel>(
                                         e2, QueryVel{9});
                                     if (s2.isError() &&
                                         s2.error() ==
                                             laige::ErrorCode::InvalidArgument) {
                                       errors++;
                                     }
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(visits, 2);  // both entities visited
    EXPECT_EQ(errors, 2);  // both moves rejected
    EXPECT_FALSE(world.has<QueryVel>(e0));
    EXPECT_FALSE(world.has<QueryVel>(e1));
    EXPECT_EQ(world.archetypeCount(), 1u);  // no {Pos,Vel} created
  }
  // Structural remove (the entity leaves the iterated archetype):
  {
    laige::World world = makeWorld(2);
    ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
    ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
    laige::Entity e;
    {
      auto r = world.create();
      ASSERT_TRUE(r.ok());
      e = r.value();
    }
    ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
    ASSERT_TRUE(world.addComponent<QueryVel>(e, QueryVel{7}).ok());
    int errors = 0;
    auto s = world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                     auto s2 =
                                         world.removeComponent<QueryPos>(e2);
                                     if (s2.isError() &&
                                         s2.error() ==
                                             laige::ErrorCode::InvalidArgument) {
                                       errors++;
                                     }
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(errors, 1);
    EXPECT_TRUE(world.has<QueryPos>(e));  // still in {Pos,Vel}
    EXPECT_EQ(world.entityCount(), 1u);
  }
  // Destroy of an iterated entity:
  {
    laige::World world = makeWorld(2);
    ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
    laige::Entity e;
    {
      auto r = world.create();
      ASSERT_TRUE(r.ok());
      e = r.value();
    }
    ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
    int errors = 0;
    auto s = world.each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                     auto s2 = world.destroy(e2);
                                     if (s2.isError() &&
                                         s2.error() ==
                                             laige::ErrorCode::InvalidArgument) {
                                       errors++;
                                     }
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(errors, 1);
    EXPECT_TRUE(world.isValid(e));  // still alive
  }
  // clear() under the iteration:
  {
    laige::World world = makeWorld(2);
    ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
    laige::Entity e;
    {
      auto r = world.create();
      ASSERT_TRUE(r.ok());
      e = r.value();
    }
    ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
    int errors = 0;
    auto s = world.each<QueryPos>([&](laige::Entity, const QueryPos&) {
                                     auto s2 = world.clear();
                                     if (s2.isError() &&
                                         s2.error() ==
                                             laige::ErrorCode::InvalidArgument) {
                                       errors++;
                                     }
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(errors, 1);
    EXPECT_TRUE(world.isValid(e));
    EXPECT_EQ(world.entityCount(), 1u);
  }
  // Nested iteration:
  {
    laige::World world = makeWorld(2);
    ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
    laige::Entity e;
    {
      auto r = world.create();
      ASSERT_TRUE(r.ok());
      e = r.value();
    }
    ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());
    int outer = 0, inner = 0, errors = 0;
    auto s = world.each<QueryPos>([&](laige::Entity, const QueryPos&) {
                                     outer++;
                                     auto s2 = world.each<QueryPos>(
                                         [&](laige::Entity, const QueryPos&) {
                                           inner++;
                                         },
                                         laige::Read{});
                                     if (s2.isError() &&
                                         s2.error() ==
                                             laige::ErrorCode::InvalidArgument) {
                                       errors++;
                                     }
                                   },
                                  laige::Read{});
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(outer, 1);
    EXPECT_EQ(inner, 0);  // the nested iteration never ran
    EXPECT_EQ(errors, 1);
  }
#else
  GTEST_SKIP() << "release degradation path (NDEBUG); the debug assert "
                 "path is covered by the *AbortsInDebug tests.";
#endif
}

QUERY_DEBUG_ASSERT_TEST(InplaceWriteToReadComponentAbortsInDebug, 0)
QUERY_DEBUG_ASSERT_TEST(AddDuringIterationAbortsInDebug, 1)
QUERY_DEBUG_ASSERT_TEST(RemoveDuringIterationAbortsInDebug, 2)
QUERY_DEBUG_ASSERT_TEST(DestroyDuringIterationAbortsInDebug, 3)
QUERY_DEBUG_ASSERT_TEST(ClearDuringIterationAbortsInDebug, 4)
QUERY_DEBUG_ASSERT_TEST(NestedEachAbortsInDebug, 5)

// ---------------------------------------------------------------------------
// Logging (LOG-001/002/004): one rate-limited warn per violation event,
// the suppressed repeats summarized on shutdown (release builds only —
// in debug the assert fires before the log)
// ---------------------------------------------------------------------------

namespace {

// A test-only Sink that records every emitted event (the logging
// facade is a process singleton; this test owns its window and
// restores the default console sink at the end).
class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
  };

  void emit(const laige::log::LogRecord& record) override {
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    for (const auto& f : record.fields) {
      e.fields.emplace_back(std::string(f.name), f.value);
    }
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

}  // namespace

TEST(QueryLogging, IllegalMutationWarnsOnceInRelease) {
#ifdef NDEBUG
  // The world setup comes first so its archetype_created Info goes to
  // the console; the sink window then holds only the violation burst.
  laige::World world = makeWorld(2);
  auto reg = world.registerComponent<QueryPos>();
  ASSERT_TRUE(reg.ok());
  const std::uint32_t posId = reg.value().value;
  laige::Entity e;
  {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    e = r.value();
  }
  ASSERT_TRUE(world.addComponent<QueryPos>(e, QueryPos{1, 2}).ok());

  auto sink = std::make_unique<MemorySink>();
  auto* sinkPtr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateWindow = std::chrono::seconds(60);  // the burst stays in-window
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(opts)).ok());

  // Three illegal in-place writes: one logged warn, two suppressed
  // (the same rate-limit window the stale-handle suite relies on).
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(world
                    .each<QueryPos>([&](laige::Entity e2, const QueryPos&) {
                                        (void)world.addComponent<QueryPos>(
                                            e2, QueryPos{9, 9});
                                      },
                                    laige::Read{})
                    .ok());
  }
  // Controlled shutdown drains the pending rate-limit summary
  // (CONC-006/LOG-007/LOG-004).
  laige::log::Logger::instance().shutdown();
  ASSERT_EQ(sinkPtr->entries.size(), 2u);
  EXPECT_EQ(sinkPtr->entries[0].subsystem, "ecs");
  EXPECT_EQ(sinkPtr->entries[0].event, "iteration_write_during_read");
  EXPECT_EQ(sinkPtr->entries[0].severity, laige::log::Severity::Warn);
  bool foundEntity = false, foundGeneration = false;
  bool foundComponent = false, foundArchetype = false;
  for (const auto& [key, value] : sinkPtr->entries[0].fields) {
    if (key == "entity_id" &&
        value == std::to_string(e.id)) foundEntity = true;
    if (key == "generation" &&
        value == std::to_string(e.generation)) foundGeneration = true;
    if (key == "component_id" && value == std::to_string(posId)) {
      foundComponent = true;
    }
    if (key == "archetype_id") foundArchetype = true;
  }
  EXPECT_TRUE(foundEntity);
  EXPECT_TRUE(foundGeneration);
  EXPECT_TRUE(foundComponent);
  EXPECT_TRUE(foundArchetype);
  EXPECT_EQ(sinkPtr->entries[1].event, laige::log::kRateLimitedEvent);
  bool foundEventField = false, foundCountField = false;
  for (const auto& [key, value] : sinkPtr->entries[1].fields) {
    if (key == "event" && value == "iteration_write_during_read") {
      foundEventField = true;
    }
    if (key == "suppressed" && value == "2") foundCountField = true;
  }
  EXPECT_TRUE(foundEventField);
  EXPECT_TRUE(foundCountField);

  // Restore the default console sink for the remaining tests.
  laige::log::LoggerOptions defaults;
  ASSERT_TRUE(laige::log::Logger::instance().init(std::move(defaults)).ok());
#else
  GTEST_SKIP() << "the warn path is release-only (NDEBUG): in debug builds "
                 "the assert fires before the log.";
#endif
}

// ---------------------------------------------------------------------------
// Zero allocations in the query path (PERF-003; the M1-ALLOC-01 standing
// assertion lands later — the test-only operator-new counter, non-
// sanitizer trees; the sanitizer trees cover the property with their
// leak-free run of the same loop)
// ---------------------------------------------------------------------------

TEST(QueryZeroAlloc, TenKEntityIterationZeroAlloc) {
  constexpr std::uint32_t kEntities = 10000;
  laige::World world = makeWorld(kEntities);
  ASSERT_TRUE(world.registerComponent<QueryPos>().ok());
  ASSERT_TRUE(world.registerComponent<QueryVel>().ok());
  ASSERT_TRUE(world.registerComponent<QueryFlag>().ok());

  std::vector<laige::Entity> entities;
  entities.reserve(kEntities);
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    auto r = world.create();
    ASSERT_TRUE(r.ok());
    entities.push_back(r.value());
    ASSERT_TRUE(world.addComponent<QueryPos>(
        entities.back(), QueryPos{static_cast<std::int32_t>(i), 0})
        .ok());
    ASSERT_TRUE(world.addComponent<QueryVel>(entities.back(), QueryVel{0})
        .ok());
  }
  // Warm-up: exercise archetype growth (Flag added then removed from
  // every entity) so the window under test starts in steady state.
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    ASSERT_TRUE(world
                    .addComponent<QueryFlag>(
                        entities[i], QueryFlag{static_cast<std::int32_t>(i)})
                    .ok());
    ASSERT_TRUE(world.removeComponent<QueryFlag>(entities[i]).ok());
  }
  const laige::ArchetypeStats before = world.archetypeStats();
  EXPECT_EQ(before.rowsLive, kEntities);

  std::uint64_t visits = 0;
  std::int64_t sumX = 0;
#if defined(LAIGE_ALLOC_COUNTER)
  laige::test::resetAllocCounter();
#endif
  const auto t0 = std::chrono::steady_clock::now();
  // Pass 1: Read/Write — a legal in-place write through the reference
  // on every visit (the system mutation path, no World call).
  auto s = world.each<QueryPos, QueryVel>(
      [&](laige::Entity e, const QueryPos& p, QueryVel& v) {
        visits++;
        sumX += p.x;
        v.v = static_cast<std::int64_t>(e.id);
      },
      laige::Read{}, laige::Write{});
  ASSERT_TRUE(s.ok());
  // Pass 2: Read/Read — a pure read pass in the same window.
  std::uint64_t visits2 = 0;
  auto s2 = world.each<QueryPos>([&](laige::Entity, const QueryPos&) {
                                    visits2++;
                                  },
                                 laige::Read{});
  ASSERT_TRUE(s2.ok());
  const auto t1 = std::chrono::steady_clock::now();
  EXPECT_EQ(visits, static_cast<std::uint64_t>(kEntities));
  EXPECT_EQ(visits2, static_cast<std::uint64_t>(kEntities));
  EXPECT_EQ(sumX, 49995000);  // 0 + 1 + ... + 9999

  // The writes landed (in place, no archetype change): the callback
  // stored each entity's own slot id (first created = slot 9999,
  // last created = slot 0 — LIFO slot allocation).
  EXPECT_EQ(world.get<QueryVel>(entities[0])->v,
            static_cast<std::int64_t>(entities[0].id));
  EXPECT_EQ(world.get<QueryVel>(entities[9999])->v,
            static_cast<std::int64_t>(entities[9999].id));
  // No structural change: the reservation is unchanged (no growth, no
  // moves) — the query path is pool-steady.
  const laige::ArchetypeStats after = world.archetypeStats();
  EXPECT_EQ(after.rowsLive, kEntities);
  EXPECT_EQ(after.totalReservations, before.totalReservations);
  EXPECT_EQ(after.bytesReserved, before.bytesReserved);
  EXPECT_EQ(after.totalAdds, before.totalAdds);
  EXPECT_EQ(after.totalRemoves, before.totalRemoves);
  EXPECT_EQ(after.totalArchetypeGrowth, before.totalArchetypeGrowth);

#if defined(LAIGE_ALLOC_COUNTER)
  const std::uint64_t allocs = laige::test::allocCounter();
  std::printf(
      "query-iteration zero-alloc window: %llu heap allocations over "
      "2 passes x %u entity visits\n",
      static_cast<unsigned long long>(allocs),
      static_cast<unsigned>(kEntities));
  EXPECT_EQ(allocs, 0u);
#else
  // Sanitizer trees: the counter is excluded there; the leak-free
  // sanitizer run of this same loop is the zero-allocation evidence
  // (the M1-ECS-03 churn test's fallback, ASan + pool accounting).
#endif
  // Measured, not assumed (CORE-001): the machine-greppable timing
  // line for the M1-BENCH-01 tick baseline.
  const double us =
      std::chrono::duration<double, std::micro>(t1 - t0).count();
  std::printf(
      "query-iteration %u visits x 2 passes took %.1f us (%.3f us/visit)\n",
      static_cast<unsigned>(kEntities), us, us / (2.0 * kEntities));
}
