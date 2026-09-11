// laige-core memory pools suite (M0-CORE-05).
//
// Step Verify scope:
//   - budget exhaustion returns a Status error (ErrorCode::BudgetExhausted),
//     no crash, no leak (ASan runs this suite leak-free; the Tracked
//     counters prove every placement-new has a matching destroy)
//   - reset semantics: ArenaPool::reset() destroys all live elements,
//     rewinds the cursor, and is idempotent
//   - generation-checked stale handle detection (CPP-007): stale,
//     cleared, reused-slot, and default handles all fail isValid();
//     the debug assert in Pool::at() aborts a forked child on a stale
//     handle (S-9: fail loudly in debug)
//   - accounting: PoolStats element/byte counts, peak, and churn
//     (totalCreated) track the documented semantics
//
// Runs as CTest `pools` (the step's Verify command is `ctest -R pools`):
// a filtered view of the shared laige-core_tests executable, selecting
// exactly the suites below.

#include <cstdint>
#include <string_view>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/pools.h"

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "pools_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "pools_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "pools_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define POOLS_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define POOLS_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if POOLS_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "pools_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

namespace {

// A T whose constructor/destructor calls are observable: proves create()
// placement-news and destroy()/reset()/clear()/destruction run exactly
// once per element (leak-free; ASan verifies independently).
class Tracked {
 public:
  explicit Tracked(int value) : value_(value) {
    ++constructed_;
    ++live_;
  }
  ~Tracked() {
    ++destroyed_;
    --live_;
  }

  int value() const { return value_; }

  static int constructed() { return constructed_; }
  static int destroyed() { return destroyed_; }
  static int live() { return live_; }
  static void resetCounters() {
    constructed_ = 0;
    destroyed_ = 0;
    live_ = 0;
  }

 private:
  int value_{};
  static inline int constructed_{0};
  static inline int destroyed_{0};
  static inline int live_{0};
};

// An element with non-trivial alignment: pins the ElementSlot stride
// (max(sizeof(T), alignof(T))) in the bytes accounting.
struct alignas(16) Padded {
  float a{};
  char b{};
};

}  // namespace

// ---------------------------------------------------------------------------
// ArenaPool<T>
// ---------------------------------------------------------------------------

TEST(ArenaPoolBasics, CreateAssignsAscendingSlotsAndReadsBack) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{4});
  const auto s0 = pool.create(10);
  const auto s1 = pool.create(20);
  const auto s2 = pool.create(30);
  ASSERT_TRUE(s0.ok() && s1.ok() && s2.ok());
  EXPECT_EQ(s0.value(), 0u);
  EXPECT_EQ(s1.value(), 1u);
  EXPECT_EQ(s2.value(), 2u);
  EXPECT_EQ(pool.at(s0.value()).value(), 10);
  EXPECT_EQ(pool.at(s1.value()).value(), 20);
  EXPECT_EQ(pool.at(s2.value()).value(), 30);
  EXPECT_EQ(pool.inUse(), 3u);
  EXPECT_EQ(pool.capacity(), 4u);
  EXPECT_TRUE(pool.isValid(2));
  EXPECT_FALSE(pool.isValid(3));
  EXPECT_EQ(pool.get(2), &pool.at(2));
  EXPECT_EQ(pool.get(3), nullptr);
}

TEST(ArenaPoolBasics, ResetDestroysAllAndRewindsCursor) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{3});
  ASSERT_TRUE(pool.create(10).ok());
  ASSERT_TRUE(pool.create(20).ok());
  ASSERT_TRUE(pool.create(30).ok());
  EXPECT_EQ(Tracked::live(), 3);

  pool.reset();
  EXPECT_EQ(Tracked::live(), 0);
  EXPECT_EQ(Tracked::destroyed(), 3);
  EXPECT_EQ(pool.inUse(), 0);
  EXPECT_FALSE(pool.isValid(0));
  EXPECT_EQ(pool.get(0), nullptr);

  // Idempotent: a second reset is a no-op.
  pool.reset();
  EXPECT_EQ(Tracked::destroyed(), 3);

  // The cursor rewound: the next create reuses slot 0.
  const auto s = pool.create(40);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s.value(), 0u);
  EXPECT_EQ(pool.at(s.value()).value(), 40);
}

TEST(ArenaPoolBasics, PeakAndChurnSurviveReset) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{4});
  ASSERT_TRUE(pool.create(1).ok());
  ASSERT_TRUE(pool.create(2).ok());
  ASSERT_TRUE(pool.create(3).ok());
  pool.reset();
  ASSERT_TRUE(pool.create(4).ok());
  ASSERT_TRUE(pool.create(5).ok());
  const laige::PoolStats s = pool.stats();
  EXPECT_EQ(s.peakInUse, 3u);   // high-water mark since construction
  EXPECT_EQ(s.totalCreated, 5u);  // churn across both frames
  EXPECT_EQ(s.inUse, 2u);
}

TEST(ArenaPoolBasics, ZeroBudgetPoolRejectsEverything) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{0});
  const auto r = pool.create(1);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::BudgetExhausted);
  const laige::PoolStats s = pool.stats();
  EXPECT_EQ(s.capacity, 0u);
  EXPECT_EQ(s.bytesCapacity, 0u);
}

TEST(ArenaPoolBasics, MoveTransfersOwnership) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> a(laige::ArenaPool<Tracked>::Options{3});
  ASSERT_TRUE(a.create(7).ok());
  ASSERT_TRUE(a.create(8).ok());

  laige::ArenaPool<Tracked> b(std::move(a));
  EXPECT_EQ(b.inUse(), 2u);
  EXPECT_EQ(b.capacity(), 3u);
  EXPECT_EQ(b.at(1).value(), 8);
  EXPECT_EQ(a.inUse(), 0u);
  EXPECT_EQ(a.capacity(), 0u);
  EXPECT_FALSE(a.create(9).ok());  // moved-from: empty arena
  EXPECT_EQ(Tracked::live(), 2);   // the move moved nothing but pointers
}

TEST(ArenaPoolBasics, MoveAssignmentTransfersOwnership) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> a(laige::ArenaPool<Tracked>::Options{2});
  ASSERT_TRUE(a.create(7).ok());
  laige::ArenaPool<Tracked> b(laige::ArenaPool<Tracked>::Options{1});
  b = std::move(a);
  EXPECT_EQ(b.inUse(), 1u);
  EXPECT_EQ(b.capacity(), 2u);
  EXPECT_EQ(b.at(0).value(), 7);
  EXPECT_EQ(a.inUse(), 0u);
  EXPECT_EQ(a.capacity(), 0u);
}

// ---------------------------------------------------------------------------
// ArenaPool budget (the step's Verify clause: exhaustion -> Status error,
// no crash, no leak)
// ---------------------------------------------------------------------------

TEST(ArenaPoolBudget, ExhaustionReturnsBudgetExhaustedAndNeverGrows) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{2});
  ASSERT_TRUE(pool.create(1).ok());
  ASSERT_TRUE(pool.create(2).ok());

  // The 3rd create hits the budget: an error value, no crash, no growth.
  const auto r = pool.create(3);
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(pool.inUse(), 2u);
  EXPECT_EQ(pool.stats().capacity, 2u);
  EXPECT_EQ(Tracked::live(), 2);

  // Repeated exhaustion stays a clean error (no partial state).
  EXPECT_EQ(pool.create(4).error(), laige::ErrorCode::BudgetExhausted);

  // The budget is reusable after reset (per-frame contract).
  pool.reset();
  EXPECT_TRUE(pool.create(5).ok());
  EXPECT_EQ(pool.at(0).value(), 5);
  EXPECT_EQ(Tracked::live(), 1);
}

// ---------------------------------------------------------------------------
// Pool<T> — handles, recycling, stability
// ---------------------------------------------------------------------------

TEST(PoolBasics, CreateReadsBackAndDestroyRecyclesLifo) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{3});
  const auto a = pool.create(1);
  const auto b = pool.create(2);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(pool.at(a.value()).value(), 1);
  EXPECT_EQ(pool.at(b.value()).value(), 2);
  EXPECT_EQ(pool.inUse(), 2u);

  // Freeing a's slot and creating again recycles it (LIFO free list);
  // b is undisturbed — slot stability while live (CPP-007).
  ASSERT_TRUE(pool.destroy(a.value()).ok());
  EXPECT_EQ(pool.inUse(), 1u);
  const auto c = pool.create(3);
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.value().index, a.value().index);
  EXPECT_EQ(pool.at(b.value()).value(), 2);
  EXPECT_EQ(pool.at(c.value()).value(), 3);
}

TEST(PoolBasics, HandlesAreStableWhileLive) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{4});
  const auto a = pool.create(1);
  const auto b = pool.create(2);
  const auto d = pool.create(3);
  ASSERT_TRUE(a.ok() && b.ok() && d.ok());
  ASSERT_TRUE(pool.destroy(b.value()).ok());
  ASSERT_TRUE(pool.destroy(d.value()).ok());
  // a survives the destructions of every other element.
  EXPECT_TRUE(pool.isValid(a.value()));
  EXPECT_EQ(pool.at(a.value()).value(), 1);
}

TEST(PoolBasics, ZeroBudgetPoolRejectsEverything) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{0});
  const auto r = pool.create(1);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::BudgetExhausted);
  const laige::PoolStats s = pool.stats();
  EXPECT_EQ(s.capacity, 0u);
  EXPECT_EQ(s.bytesCapacity, 0u);
}

// ---------------------------------------------------------------------------
// Pool budget (the step's Verify clause: exhaustion -> Status error, no
// crash, no leak, no silent growth)
// ---------------------------------------------------------------------------

TEST(PoolBudget, ExhaustionReturnsBudgetExhaustedAndNeverGrows) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{2});
  const auto a = pool.create(1);
  const auto b = pool.create(2);
  ASSERT_TRUE(a.ok() && b.ok());

  const auto r = pool.create(3);
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(pool.inUse(), 2u);
  EXPECT_EQ(pool.stats().capacity, 2u);  // never grew
  EXPECT_EQ(Tracked::live(), 2);

  // Repeated exhaustion stays a clean error.
  EXPECT_EQ(pool.create(4).error(), laige::ErrorCode::BudgetExhausted);

  // Destroying one element frees budget for the next create.
  ASSERT_TRUE(pool.destroy(a.value()).ok());
  const auto c = pool.create(3);
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(pool.at(c.value()).value(), 3);
  EXPECT_EQ(pool.inUse(), 2u);
  EXPECT_EQ(Tracked::live(), 2);
}

// ---------------------------------------------------------------------------
// Pool stale-handle detection (CPP-007)
// ---------------------------------------------------------------------------

TEST(PoolStale, StaleAfterDestroyFailsIsValidAndGet) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{2});
  const auto h = pool.create(1);
  ASSERT_TRUE(h.ok());
  ASSERT_TRUE(pool.destroy(h.value()).ok());
  EXPECT_FALSE(pool.isValid(h.value()));
  EXPECT_EQ(pool.get(h.value()), nullptr);
}

TEST(PoolStale, StaleAfterClear) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{2});
  const auto a = pool.create(1);
  const auto b = pool.create(2);
  ASSERT_TRUE(a.ok() && b.ok());
  pool.clear();
  EXPECT_FALSE(pool.isValid(a.value()));
  EXPECT_FALSE(pool.isValid(b.value()));
  EXPECT_EQ(pool.inUse(), 0u);

  // The pool is reusable: fresh handles are valid, old ones are not.
  const auto c = pool.create(3);
  ASSERT_TRUE(c.ok());
  EXPECT_TRUE(pool.isValid(c.value()));
  EXPECT_EQ(pool.at(c.value()).value(), 3);
  EXPECT_FALSE(pool.isValid(a.value()));
  EXPECT_FALSE(pool.isValid(b.value()));
}

TEST(PoolStale, ReusedSlotBumpsGeneration) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{1});
  const auto h1 = pool.create(1);
  ASSERT_TRUE(h1.ok());
  ASSERT_TRUE(pool.destroy(h1.value()).ok());
  const auto h2 = pool.create(2);
  ASSERT_TRUE(h2.ok());

  // Same slot (the only one), next generation: the old handle is stale,
  // the new one is live. This is the generation check the roadmap
  // names — a stale handle can never pass again (short of a 2^32
  // wrap of one slot, documented in the header).
  EXPECT_EQ(h2.value().index, h1.value().index);
  EXPECT_EQ(h2.value().generation, h1.value().generation + 1);
  EXPECT_FALSE(pool.isValid(h1.value()));
  EXPECT_TRUE(pool.isValid(h2.value()));
  EXPECT_EQ(pool.at(h2.value()).value(), 2);
}

TEST(PoolStale, DefaultHandleIsNeverValid) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{1});
  EXPECT_FALSE(pool.isValid(laige::PoolHandle{}));
  EXPECT_EQ(pool.get(laige::PoolHandle{}), nullptr);
}

TEST(PoolStale, DestroyInvalidHandleFailsWithInvalidArgument) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{1});
  const auto s = pool.destroy(laige::PoolHandle{});
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);

  // Double destroy of a live element fails cleanly (no double free).
  const auto h = pool.create(1);
  ASSERT_TRUE(h.ok());
  ASSERT_TRUE(pool.destroy(h.value()).ok());
  const auto s2 = pool.destroy(h.value());
  EXPECT_FALSE(s2.ok());
  EXPECT_EQ(s2.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(Tracked::destroyed(), 1);
  EXPECT_EQ(Tracked::live(), 0);
}

TEST(PoolStale, AtStaleHandleAbortsInDebug) {
  Tracked::resetCounters();
  // S-9: a stale handle through at() must fail loudly in debug (the
  // generation check asserts). Exercised in a forked child so the test
  // process survives: the child must die on SIGABRT. POSIX only (fork);
  // the Windows jobs skip with a reason.
#if defined(__unix__)
#  if defined(NDEBUG)
  GTEST_SKIP() << "assert-based stale detection is a debug-build property";
#  else
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{1});
    const auto r = pool.create(7);
    if (!r.ok() || !pool.destroy(r.value()).ok()) _exit(117);
    const Tracked& probe = pool.at(r.value());  // stale: the debug assert
    static_cast<void>(probe);                   // must fire before this
    _exit(1);  // unreachable: no assert would make the parent fail below
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
      << "expected the stale-handle assert to abort the child (SIGABRT)";
#  endif
#else
  GTEST_SKIP() << "fork() is not available on Windows; the stale-handle "
                 "assert is exercised on the POSIX jobs.";
#endif
}

// ---------------------------------------------------------------------------
// Destruction accounting (no leaks: every placement-new is destroyed)
// ---------------------------------------------------------------------------

TEST(PoolDestruction, DestructorDestroysRemainingLiveElements) {
  Tracked::resetCounters();
  {
    laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{4});
    ASSERT_TRUE(pool.create(1).ok());
    ASSERT_TRUE(pool.create(2).ok());
    ASSERT_TRUE(pool.create(3).ok());
    ASSERT_TRUE(pool.destroy(pool.create(4).value()).ok());
    EXPECT_EQ(Tracked::live(), 3);
  }  // pool destroyed: the 3 live elements must go with it
  EXPECT_EQ(Tracked::constructed(), 4);
  EXPECT_EQ(Tracked::destroyed(), 4);
  EXPECT_EQ(Tracked::live(), 0);
}

TEST(PoolDestruction, ClearDestroysEveryLiveElement) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{4});
  ASSERT_TRUE(pool.create(1).ok());
  ASSERT_TRUE(pool.create(2).ok());
  ASSERT_TRUE(pool.create(3).ok());
  pool.clear();
  EXPECT_EQ(Tracked::destroyed(), 3);
  EXPECT_EQ(Tracked::live(), 0);
}

TEST(PoolDestruction, ArenaDestructorDestroysRemainingLiveElements) {
  Tracked::resetCounters();
  {
    laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{4});
    ASSERT_TRUE(pool.create(1).ok());
    ASSERT_TRUE(pool.create(2).ok());
  }
  EXPECT_EQ(Tracked::constructed(), 2);
  EXPECT_EQ(Tracked::destroyed(), 2);
  EXPECT_EQ(Tracked::live(), 0);
}

// ---------------------------------------------------------------------------
// Accounting (PoolStats; PRD §10.4, FR-11.4, G-R4)
// ---------------------------------------------------------------------------

TEST(PoolStats, ReportsElementAndByteCounts) {
  Tracked::resetCounters();
  laige::Pool<Tracked> pool(laige::Pool<Tracked>::Options{4});
  const std::size_t stride = sizeof(Tracked);  // 4 B, alignment 4
  const std::size_t bookkeeping = 9;           // 4 B gen + 1 B alive + 4 B freelist

  laige::PoolStats s = pool.stats();
  EXPECT_EQ(s.capacity, 4u);
  EXPECT_EQ(s.inUse, 0u);
  EXPECT_EQ(s.peakInUse, 0u);
  EXPECT_EQ(s.totalCreated, 0u);
  EXPECT_EQ(s.bytesCapacity, 4u * (stride + bookkeeping));
  EXPECT_EQ(s.bytesInUse, 0u);

  ASSERT_TRUE(pool.create(1).ok());
  ASSERT_TRUE(pool.create(2).ok());
  s = pool.stats();
  EXPECT_EQ(s.inUse, 2u);
  EXPECT_EQ(s.peakInUse, 2u);
  EXPECT_EQ(s.totalCreated, 2u);
  EXPECT_EQ(s.bytesInUse, 2u * stride);
  EXPECT_EQ(s.bytesCapacity, 4u * (stride + bookkeeping));

  // A create immediately destroyed: inUse falls back, peakInUse keeps
  // the high-water mark, and the churn count (totalCreated) keeps the
  // create — G-R4 counts churn, not liveness.
  const auto c = pool.create(3);
  ASSERT_TRUE(c.ok());
  ASSERT_TRUE(pool.destroy(c.value()).ok());
  s = pool.stats();
  EXPECT_EQ(s.inUse, 2u);
  EXPECT_EQ(s.peakInUse, 3u);
  EXPECT_EQ(s.totalCreated, 3u);
  EXPECT_EQ(s.bytesInUse, 2u * stride);
}

TEST(PoolStats, ArenaReportsElementAndByteCounts) {
  Tracked::resetCounters();
  laige::ArenaPool<Tracked> pool(laige::ArenaPool<Tracked>::Options{4});
  const std::size_t stride = sizeof(Tracked);
  laige::PoolStats s = pool.stats();
  EXPECT_EQ(s.bytesCapacity, 4u * stride);
  EXPECT_EQ(s.bytesInUse, 0u);
  ASSERT_TRUE(pool.create(1).ok());
  s = pool.stats();
  EXPECT_EQ(s.inUse, 1u);
  EXPECT_EQ(s.bytesInUse, 1u * stride);
}

TEST(PoolStats, AlignedElementsUseAlignedStride) {
  Tracked::resetCounters();
  laige::Pool<Padded> pool(laige::Pool<Padded>::Options{2});
  ASSERT_TRUE(pool.create().ok());
  // sizeof(Padded) == 16 (alignas(16)); the slot stride matches.
  EXPECT_EQ(pool.stats().bytesInUse, 16u);
  EXPECT_EQ(pool.stats().bytesCapacity, 2u * (16u + 9u));
  EXPECT_EQ(pool.at(pool.create().value()).a, 0.0f);
}

// ---------------------------------------------------------------------------
// Pool<T> move semantics
// ---------------------------------------------------------------------------

TEST(PoolMove, MoveTransfersOwnership) {
  Tracked::resetCounters();
  laige::Pool<Tracked> a(laige::Pool<Tracked>::Options{3});
  const auto h = a.create(7);
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(a.inUse(), 1u);

  laige::Pool<Tracked> b(std::move(a));
  EXPECT_EQ(b.inUse(), 1u);
  EXPECT_EQ(b.capacity(), 3u);
  EXPECT_TRUE(b.isValid(h.value()));
  EXPECT_EQ(b.at(h.value()).value(), 7);
  EXPECT_EQ(b.stats().totalCreated, 1u);
  // The source is a valid empty pool: nothing is reachable from it.
  EXPECT_EQ(a.inUse(), 0u);
  EXPECT_EQ(a.capacity(), 0u);
  EXPECT_FALSE(a.isValid(h.value()));
  EXPECT_EQ(a.get(h.value()), nullptr);
  EXPECT_FALSE(a.create(1).ok());
  EXPECT_EQ(a.stats().capacity, 0u);
  EXPECT_EQ(Tracked::live(), 1);
}

TEST(PoolMove, MoveAssignmentTransfersOwnership) {
  Tracked::resetCounters();
  laige::Pool<Tracked> a(laige::Pool<Tracked>::Options{2});
  const auto h = a.create(7);
  ASSERT_TRUE(h.ok());
  laige::Pool<Tracked> b(laige::Pool<Tracked>::Options{1});
  b = std::move(a);
  EXPECT_EQ(b.inUse(), 1u);
  EXPECT_EQ(b.capacity(), 2u);
  EXPECT_TRUE(b.isValid(h.value()));
  EXPECT_EQ(b.at(h.value()).value(), 7);
  EXPECT_EQ(a.inUse(), 0u);
  EXPECT_EQ(a.capacity(), 0u);
  EXPECT_FALSE(a.isValid(h.value()));
}
