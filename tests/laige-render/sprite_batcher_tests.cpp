// laige-render sprite batcher tests (M2-SPRITE-01): the engine-owned
// "declare, don't draw" sprite declaration window + batch builder in
// laige/render/sprite_batcher.h.
//
// Pure integer bookkeeping over pre-allocated storage — no GL context,
// no GL environment needed: every suite runs in every local tree and
// in CI. The groups suite pins the roadmap's grouping correctness
// (N atlases × materials × blends produce the exact group count, the
// deterministic group order, the per-group membership); the order
// suite pins the in-group instance order against hand-computed
// expected orders (the M2-SORT-01 sorted order restricted to the
// group, stable on equal keys); the overflow suite pins the
// documented drop-oldest + warn policy (PERF-008); the overrides
// suite pins the G-R11 counted + warned escape hatch; the edges suite
// pins the stopped state, the create validation, and the frame
// protocol (closed window, double build, slot access); the
// determinism suite pins the same-declarations → identical-batches
// property against the stable-sort oracle; the zero-allocation suite
// proves the per-frame paths allocate nothing (PERF-003, the
// iso_picking / depth_sort test precedent).
//
// Seed: the repo-wide documented default seed via
// tests/support/laige_test_seed.h (docs/testing.md §4), one named
// substream per randomized suite.

#include "laige/render/sprite_batcher.h"

#include <algorithm>
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
#include "laige/prng.h"
#include "laige_test_seed.h"

namespace {

using laige::render::BlendMode;
using laige::render::SpriteBatch;
using laige::render::SpriteBatcher;
using SpriteBatcherOptions = laige::render::SpriteBatcher::Options;
using laige::render::SpriteItem;
using laige::render::SpriteTint;
using laige::render::SpriteUvRect;
using laige::render::Vec2;

// The randomized suites' substream ids (docs/testing.md §4 — stable
// named constants, CORE-005).
constexpr std::uint32_t kDeterminismSubstreamId = 0x53425444;  // "SBDT"
constexpr std::uint32_t kZeroAllocSubstreamId = 0x5342544A;  // "SBZA"

// A deterministic 32-bit mix of i (the key generator — the
// precomputed workloads are built OUTSIDE every measured window: no
// RNG and no division in the measured path, the depth_sort test
// discipline). The splitmix64 finalizer (Stafford 2018) spreads the
// low bits of i, and the mask carves a realistic isometric key range
// (the M2-ISO-01 fine-depth field is 22 bits — the ~2.4 equal-key
// pairs per key of a 3000-sprite scene).
std::uint32_t mixKey(std::uint64_t i, std::uint32_t mask = 0xFFFFFFFFu) {
  std::uint64_t v =
      i * laige::Prng::kMixMultiplierA + laige::Prng::kSplitmix64Increment;
  v ^= v >> 30;
  v *= laige::Prng::kMixMultiplierA;
  v ^= v >> 27;
  v *= 0x94D049BB133111EBull;
  v ^= v >> 31;
  return static_cast<std::uint32_t>(v) & mask;
}

// One declared sprite with the test's identifying fields (key, group,
// override) and inert presentation fields.
SpriteItem makeItem(std::uint32_t depthKey, std::uint32_t atlasId,
                    std::uint32_t materialId, BlendMode blend,
                    bool depthOverride) {
  SpriteItem it;
  it.pos = Vec2{1.0f, 2.0f};
  it.depthKey = depthKey;
  it.depthOverride = depthOverride;
  it.uv = SpriteUvRect{0.0f, 0.0f, 1.0f, 1.0f};
  it.rotation = 0.0f;
  it.scale = Vec2{1.0f, 1.0f};
  it.tint = SpriteTint{};
  it.atlasId = atlasId;
  it.materialId = materialId;
  it.blend = blend;
  return it;
}

// ---------------------------------------------------------------------------
// Log capture (the iso_camera_tests MemorySink pattern — rate limiting
// OFF so the tests assert per-event counts, not the facade's LOG-004
// window).
// ---------------------------------------------------------------------------

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

MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

std::size_t countEvents(const MemorySink& sink, std::string_view subsystem,
                        std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

const MemorySink::Entry* firstEvent(const MemorySink& sink,
                                    std::string_view event) {
  for (const auto& e : sink.entries) {
    if (e.event == event) return &e;
  }
  return nullptr;
}

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

// ---------------------------------------------------------------------------
// The grouping correctness (roadmap M2-SPRITE-01 scope): N atlases ×
// materials × blends produce the EXACT group count, in the
// deterministic (atlas, material, blend) sorted order, each group's
// membership exact.
// ---------------------------------------------------------------------------

TEST(SpriteBatcherGroups, AtlasesTimesMaterialsTimesBlends) {
  constexpr std::uint32_t kAtlases = 2;
  constexpr std::uint32_t kMaterials = 3;
  constexpr std::uint32_t kBlends = 2;
  constexpr std::uint32_t kPerCombo = 2;
  const std::uint32_t n = kAtlases * kMaterials * kBlends * kPerCombo;

  SpriteBatcherOptions o;
  o.maxSprites = n;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  std::uint32_t slot = 0;
  for (std::uint32_t a = 0; a < kAtlases; ++a) {
    for (std::uint32_t m = 0; m < kMaterials; ++m) {
      for (std::uint32_t bl = 0; bl < kBlends; ++bl) {
        for (std::uint32_t k = 0; k < kPerCombo; ++k) {
          const auto add =
              b.add(makeItem(mixKey(n - slot), a, m,
                             bl == 0 ? BlendMode::Alpha : BlendMode::Additive,
                             false));
          ASSERT_TRUE(add.ok());
          ASSERT_EQ(add.value(), slot);
          ++slot;
        }
      }
    }
  }
  ASSERT_TRUE(b.build().ok());

  // The exact group count: every (atlas, material, blend) combination
  // is used, so exactly kAtlases * kMaterials * kBlends groups.
  EXPECT_EQ(b.batchCount(), kAtlases * kMaterials * kBlends);
  EXPECT_EQ(b.frameCount(), n);

  const auto batches = b.batches();
  // The deterministic group order: ascending (atlas, material, blend).
  for (std::size_t g = 1; g < batches.size(); ++g) {
    const auto& prev = batches[g - 1];
    const auto& cur = batches[g];
    const bool ordered =
        prev.atlasId < cur.atlasId ||
        (prev.atlasId == cur.atlasId &&
         (prev.materialId < cur.materialId ||
          (prev.materialId == cur.materialId &&
           static_cast<std::uint8_t>(prev.blend) <
               static_cast<std::uint8_t>(cur.blend))));
    EXPECT_TRUE(ordered) << "group " << g << " out of order";
  }
  // Each group's membership is exact: kPerCombo instances, every slot
  // of the declared (a, m, bl) items, no slot in two groups.
  std::vector<bool> seen(n, false);
  std::size_t total = 0;
  for (std::size_t g = 0; g < batches.size(); ++g) {
    const auto& batch = batches[g];
    EXPECT_EQ(batch.instances.size(), kPerCombo);
    for (const std::uint32_t s : batch.instances) {
      const SpriteItem& it = b.at(s);
      EXPECT_EQ(it.atlasId, batch.atlasId);
      EXPECT_EQ(it.materialId, batch.materialId);
      EXPECT_EQ(it.blend, batch.blend);
      EXPECT_FALSE(seen[s]) << "slot " << s << " in two groups";
      seen[s] = true;
    }
    total += batch.instances.size();
  }
  EXPECT_EQ(total, n);
  for (std::uint32_t s = 0; s < n; ++s) EXPECT_TRUE(seen[s]);
}

TEST(SpriteBatcherGroups, OnlyUsedCombinationsAreCounted) {
  // 3 atlases x 2 blends = 6 possible combinations; the
  // (atlas 1, Additive) combination is never declared -> 5 groups.
  SpriteBatcherOptions o;
  o.maxSprites = 16;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  std::uint32_t i = 0;
  for (std::uint32_t a = 0; a < 3; ++a) {
    for (std::uint32_t bl = 0; bl < 2; ++bl) {
      const bool used = !(a == 1 && bl == 1);
      if (used) {
        const auto add =
            b.add(makeItem(mixKey(1000 + i), a, 0,
                           bl == 0 ? BlendMode::Alpha : BlendMode::Additive,
                           false));
        ASSERT_TRUE(add.ok());
        ++i;
      }
    }
  }
  ASSERT_TRUE(b.build().ok());
  EXPECT_EQ(b.batchCount(), 5);
  const auto batches = b.batches();
  for (const auto& batch : batches) {
    EXPECT_FALSE(batch.atlasId == 1 && batch.blend == BlendMode::Additive);
  }
}

TEST(SpriteBatcherGroups, AllOneGroup) {
  SpriteBatcherOptions o;
  o.maxSprites = 10;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  for (std::uint32_t i = 0; i < 10; ++i) {
    const auto add = b.add(makeItem(mixKey(i), 7, 3, BlendMode::Alpha, false));
    ASSERT_TRUE(add.ok());
  }
  ASSERT_TRUE(b.build().ok());
  EXPECT_EQ(b.batchCount(), 1);
  EXPECT_EQ(b.batches()[0].instances.size(), 10u);
  EXPECT_EQ(b.batches()[0].atlasId, 7u);
  EXPECT_EQ(b.batches()[0].materialId, 3u);
  EXPECT_EQ(b.batches()[0].blend, BlendMode::Alpha);
}

// ---------------------------------------------------------------------------
// The in-group instance order: the M2-SORT-01 sorted order (stable on
// equal keys) RESTRICTED to the group.
// ---------------------------------------------------------------------------

TEST(SpriteBatcherOrder, SingleGroupSortedKeyOrder) {
  // One group; declaration order (entity order) vs keys:
  //   pos 0: key 5, pos 1: key 1, pos 2: key 3,
  //   pos 3: key 1, pos 4: key 9, pos 5: key 0
  // Stable sorted order: (0,5) (1,1) (1,3) (3,2) (5,0) (9,4)
  //   -> instance slots {5, 1, 3, 2, 0, 4}
  constexpr std::uint32_t keys[6] = {5, 1, 3, 1, 9, 0};
  constexpr std::uint32_t expected[6] = {5, 1, 3, 2, 0, 4};

  SpriteBatcherOptions o;
  o.maxSprites = 6;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  for (std::uint32_t i = 0; i < 6; ++i) {
    ASSERT_TRUE(b.add(makeItem(keys[i], 0, 0, BlendMode::Alpha, false)).ok());
  }
  ASSERT_TRUE(b.build().ok());
  ASSERT_EQ(b.batchCount(), 1);
  const auto inst = b.batches()[0].instances;
  ASSERT_EQ(inst.size(), 6);
  for (std::uint32_t i = 0; i < 6; ++i) EXPECT_EQ(inst[i], expected[i]);
}

TEST(SpriteBatcherOrder, InterleavedGroupsKeepGlobalOrder) {
  // Two groups (atlas 0 / atlas 1) with interleaved keys:
  //   pos 0: (A, key 10), pos 1: (B, key 5),
  //   pos 2: (A, key 7),  pos 3: (B, key 9)
  // Global sorted: (5,B1) (7,A2) (9,B3) (10,A0)
  //   -> A instances {2, 0}, B instances {1, 3}
  SpriteBatcherOptions o;
  o.maxSprites = 4;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  ASSERT_TRUE(b.add(makeItem(10, 0, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.add(makeItem(5, 1, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.add(makeItem(7, 0, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.add(makeItem(9, 1, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.build().ok());
  ASSERT_EQ(b.batchCount(), 2);
  const auto batches = b.batches();
  EXPECT_EQ(batches[0].atlasId, 0u);
  EXPECT_EQ(batches[1].atlasId, 1u);
  const auto a = batches[0].instances;
  const auto c = batches[1].instances;
  ASSERT_EQ(a.size(), 2);
  ASSERT_EQ(c.size(), 2);
  EXPECT_EQ(a[0], 2u);
  EXPECT_EQ(a[1], 0u);
  EXPECT_EQ(c[0], 1u);
  EXPECT_EQ(c[1], 3u);
}

TEST(SpriteBatcherOrder, EqualKeysKeepDeclarationOrder) {
  // All keys equal: the stable sort keeps the DECLARATION order —
  // instances == {0, 1, ..., 7} (the FR-1.2 entity-id order,
  // RENDER-003).
  SpriteBatcherOptions o;
  o.maxSprites = 8;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  for (std::uint32_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(b.add(makeItem(0xABCDu, 0, 0, BlendMode::Additive, false)).ok());
  }
  ASSERT_TRUE(b.build().ok());
  ASSERT_EQ(b.batchCount(), 1);
  const auto inst = b.batches()[0].instances;
  for (std::uint32_t i = 0; i < 8; ++i) EXPECT_EQ(inst[i], i);
}

// ---------------------------------------------------------------------------
// The overflow policy: bounded, drop oldest + warn (PERF-008, S-2).
// ---------------------------------------------------------------------------

TEST(SpriteBatcherOverflow, DropOldestAndWarn) {
  MemorySink* sink = installCaptureSink();
  SpriteBatcherOptions o;
  o.maxSprites = 4;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  // Six declarations into a budget of 4: the two OLDEST (keys 0, 1)
  // are dropped, the frame keeps keys 2..5.
  b.beginFrame();
  for (std::uint32_t i = 0; i < 6; ++i) {
    ASSERT_TRUE(b.add(makeItem(i, 0, 0, BlendMode::Alpha, false)).ok());
  }
  EXPECT_EQ(b.frameCount(), 4u);
  EXPECT_EQ(b.droppedTotal(), 2u);
  EXPECT_EQ(countEvents(*sink, "sprite_batcher", "frame_overflow_dropped"),
            2u);

  ASSERT_TRUE(b.build().ok());
  ASSERT_EQ(b.batchCount(), 1);
  const auto inst = b.batches()[0].instances;
  // Keys 2,3,4,5 in back-to-front order. The kept items live at slots
  // 2, 3 (original) and 0, 1 (ring-reused by the overflow adds):
  // key 2 -> slot 2, key 3 -> slot 3, key 4 -> slot 0, key 5 -> slot 1.
  ASSERT_EQ(inst.size(), 4);
  const std::uint32_t expected[4] = {2, 3, 0, 1};
  for (std::uint32_t i = 0; i < 4; ++i) EXPECT_EQ(inst[i], expected[i]);

  // The next frame is clean again: four declarations, no drops.
  b.beginFrame();
  for (std::uint32_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(b.add(makeItem(100 + i, 0, 0, BlendMode::Alpha, false)).ok());
  }
  EXPECT_EQ(b.frameCount(), 4u);
  EXPECT_EQ(b.droppedTotal(), 2u);  // unchanged
  EXPECT_EQ(countEvents(*sink, "sprite_batcher", "frame_overflow_dropped"),
            2u);  // unchanged
  ASSERT_TRUE(b.build().ok());
  EXPECT_EQ(b.batches()[0].instances.size(), 4u);
}

// ---------------------------------------------------------------------------
// G-R11: the manual depth-override escape hatch is counted + warned.
// ---------------------------------------------------------------------------

TEST(SpriteBatcherOverrides, CountedAndWarned) {
  MemorySink* sink = installCaptureSink();
  SpriteBatcherOptions o;
  o.maxSprites = 3;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  ASSERT_TRUE(b.add(makeItem(10, 0, 0, BlendMode::Alpha, false)).ok());
  // The override: a hand-set key (the escape hatch) — it sorts like
  // any key, but the declaration is counted + warned.
  ASSERT_TRUE(b.add(makeItem(3, 0, 0, BlendMode::Alpha, true)).ok());
  ASSERT_TRUE(b.add(makeItem(7, 0, 0, BlendMode::Alpha, false)).ok());
  EXPECT_EQ(b.overrideCount(), 1u);
  EXPECT_EQ(b.overrideTotal(), 1u);

  ASSERT_TRUE(b.build().ok());
  // The override participates in the sort at its key's position:
  // sorted keys (3,7,10) -> slots {1, 2, 0}.
  ASSERT_EQ(b.batchCount(), 1);
  const auto inst = b.batches()[0].instances;
  ASSERT_EQ(inst.size(), 3);
  EXPECT_EQ(inst[0], 1u);
  EXPECT_EQ(inst[1], 2u);
  EXPECT_EQ(inst[2], 0u);
  // The warn fired once with the per-frame count + the cumulative.
  const std::size_t warns =
      countEvents(*sink, "sprite_batcher", "depth_override_used");
  EXPECT_EQ(warns, 1u);
  const MemorySink::Entry* e = firstEvent(*sink, "depth_override_used");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->severity, laige::log::Severity::Warn);
  EXPECT_EQ(fieldOf(*e, "count"), "1");
  EXPECT_EQ(fieldOf(*e, "override_total"), "1");

  // The next frame: two overrides -> the per-frame count resets, the
  // cumulative adds, and a second warn fires (rate limiting is OFF in
  // the capture sink).
  b.beginFrame();
  ASSERT_TRUE(b.add(makeItem(0, 0, 0, BlendMode::Alpha, true)).ok());
  ASSERT_TRUE(b.add(makeItem(1, 0, 0, BlendMode::Alpha, true)).ok());
  EXPECT_EQ(b.overrideCount(), 2u);
  EXPECT_EQ(b.overrideTotal(), 3u);
  ASSERT_TRUE(b.build().ok());
  EXPECT_EQ(countEvents(*sink, "sprite_batcher", "depth_override_used"),
            2u);
  e = firstEvent(*sink, "depth_override_used");
  // firstEvent returns the FIRST event — read the last one instead:
  ASSERT_FALSE(sink->entries.empty());
  const MemorySink::Entry& last = sink->entries.back();
  EXPECT_EQ(last.event, "depth_override_used");
  EXPECT_EQ(fieldOf(last, "count"), "2");
  EXPECT_EQ(fieldOf(last, "override_total"), "3");
}

// ---------------------------------------------------------------------------
// The stopped state, the create validation, and the frame protocol.
// ---------------------------------------------------------------------------

TEST(SpriteBatcherEdges, StoppedState) {
  SpriteBatcher b;  // default: the EMPTY (capacity 0) batcher
  EXPECT_EQ(b.capacity(), 0u);
  EXPECT_EQ(b.frameCount(), 0u);
  EXPECT_EQ(b.batchCount(), 0u);
  EXPECT_FALSE(b.add(makeItem(0, 0, 0, BlendMode::Alpha, false)).ok());
  EXPECT_EQ(b.add(makeItem(0, 0, 0, BlendMode::Alpha, false)).error(),
            laige::ErrorCode::BudgetExhausted);
  EXPECT_TRUE(b.build().ok());  // empty output, total
  EXPECT_EQ(b.batchCount(), 0u);
  EXPECT_TRUE(b.batches().empty());
  b.beginFrame();  // idempotent no-op
  EXPECT_TRUE(b.build().ok());
}

TEST(SpriteBatcherEdges, CreateValidation) {
  SpriteBatcherOptions o;
  o.maxSprites = 0;
  auto r = SpriteBatcher::create(o);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
}

TEST(SpriteBatcherEdges, FrameProtocol) {
  SpriteBatcherOptions o;
  o.maxSprites = 4;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  // The window opens at create: declare without an explicit
  // beginFrame (the pipeline may rely on either).
  ASSERT_TRUE(b.add(makeItem(0, 0, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.build().ok());
  // add() after build: the window is closed (InvalidArgument).
  EXPECT_FALSE(b.add(makeItem(1, 0, 0, BlendMode::Alpha, false)).ok());
  EXPECT_EQ(b.add(makeItem(1, 0, 0, BlendMode::Alpha, false)).error(),
            laige::ErrorCode::InvalidArgument);
  // build() twice: double build (InvalidArgument).
  EXPECT_FALSE(b.build().ok());
  EXPECT_EQ(b.build().error(), laige::ErrorCode::InvalidArgument);
  // beginFrame reopens the window.
  b.beginFrame();
  ASSERT_TRUE(b.add(makeItem(2, 0, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.build().ok());

  // The exact-capacity frame (n == capacity) is legal, no drops.
  b.beginFrame();
  for (std::uint32_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(b.add(makeItem(i, 0, 0, BlendMode::Alpha, false)).ok());
  }
  EXPECT_EQ(b.droppedTotal(), 0u);
  ASSERT_TRUE(b.build().ok());
  EXPECT_EQ(b.batchCount(), 1u);

  // The empty frame builds to zero batches.
  b.beginFrame();
  ASSERT_TRUE(b.build().ok());
  EXPECT_EQ(b.batchCount(), 0u);
}

TEST(SpriteBatcherEdges, SlotAccess) {
  SpriteBatcherOptions o;
  o.maxSprites = 3;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();

  b.beginFrame();
  ASSERT_TRUE(b.add(makeItem(0, 0, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.add(makeItem(1, 0, 0, BlendMode::Alpha, false)).ok());
  ASSERT_TRUE(b.add(makeItem(2, 0, 0, BlendMode::Alpha, false)).ok());
  EXPECT_NE(b.get(0u), nullptr);
  EXPECT_EQ(b.at(1u).depthKey, 1u);
  EXPECT_NE(b.get(2u), nullptr);
  // One more: the ring overwrites the oldest (slot 0), head -> 1.
  // Window: slots 1, 2, 0 (oldest -> newest).
  ASSERT_TRUE(b.add(makeItem(3, 0, 0, BlendMode::Alpha, false)).ok());
  EXPECT_EQ(b.at(1u).depthKey, 1u);  // now the oldest
  EXPECT_EQ(b.at(2u).depthKey, 2u);
  EXPECT_EQ(b.at(0u).depthKey, 3u);  // the newest (reused slot)
  EXPECT_EQ(b.droppedTotal(), 1u);
  // beginFrame releases everything.
  b.beginFrame();
  for (std::uint32_t s = 0; s < 3; ++s) EXPECT_EQ(b.get(s), nullptr);
}

// ---------------------------------------------------------------------------
// The determinism property (RENDER-003, ARCH-009/010 scope): same
// declaration sequence -> bit-identical batches, on every platform
// and build. The per-group order is oracle-checked against the
// stable sort of the group's (key, position) pairs.
// ---------------------------------------------------------------------------

TEST(SpriteBatcherDeterminism, SameDeclarationsGiveIdenticalBatches) {
  constexpr std::uint32_t kSprites = 3000;
  laige::Prng prng = laige::testing::TestPrng(kDeterminismSubstreamId);
  // The declaration sequence, precomputed OUTSIDE any window (the
  // test discipline): a realistic isometric key range (the M2-ISO-01
  // 22-bit fine-depth field — ~2.4 equal-key partners per key on
  // average in a 3000-sprite scene), 4 atlases x 3 materials x 2
  // blends, occasional overrides (the warn path stays out of the
  // determinism question — the count is a counter, not order state).
  std::vector<SpriteItem> items;
  items.reserve(kSprites);
  for (std::uint32_t i = 0; i < kSprites; ++i) {
    items.push_back(makeItem(
        mixKey(i, 0x3FFFFFu),
        prng.next_range(0, 4), prng.next_range(0, 3),
        prng.next_range(0, 2) == 0 ? BlendMode::Alpha : BlendMode::Additive,
        prng.next_range(0, 4) == 0));
  }
  auto declare = [](SpriteBatcher& b, const std::vector<SpriteItem>& its) {
    b.beginFrame();
    for (const auto& it : its) {
      const auto r = b.add(it);
      if (!r.ok()) return false;
    }
    return b.build().ok();
  };

  SpriteBatcherOptions o;
  o.maxSprites = kSprites;  // the exact-capacity frame (no overflow)
  auto ra = SpriteBatcher::create(o);
  auto rb = SpriteBatcher::create(o);
  ASSERT_TRUE(ra.ok());
  ASSERT_TRUE(rb.ok());
  SpriteBatcher a = std::move(ra).takeValue();
  SpriteBatcher b = std::move(rb).takeValue();
  ASSERT_TRUE(declare(a, items));
  ASSERT_TRUE(declare(b, items));

  // Bit-identical batches: group count, group keys, group order, and
  // the per-group instance sequences.
  EXPECT_EQ(a.batchCount(), b.batchCount());
  const auto ba = a.batches();
  const auto bb = b.batches();
  for (std::size_t g = 0; g < ba.size(); ++g) {
    EXPECT_EQ(ba[g].atlasId, bb[g].atlasId) << "group " << g;
    EXPECT_EQ(ba[g].materialId, bb[g].materialId) << "group " << g;
    EXPECT_EQ(ba[g].blend, bb[g].blend) << "group " << g;
    EXPECT_EQ(ba[g].instances.size(), bb[g].instances.size()) << "group " << g;
    for (std::size_t i = 0; i < ba[g].instances.size(); ++i) {
      EXPECT_EQ(ba[g].instances[i], bb[g].instances[i])
          << "group " << g << " instance " << i;
    }
  }

  // The per-group ORACLE: each group's instances are the STABLE sort
  // of the group's (key, declaration position) pairs — non-decreasing
  // keys, equal keys in declaration order (the M2-SORT-01 contract,
  // the (key, entity id) total order of iso_depth_key.h).
  std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> groups(
      ba.size());
  for (std::uint32_t pos = 0; pos < kSprites; ++pos) {
    const SpriteItem& it = a.at(pos);
    // The group's index: the batches are in sorted (atlas, material,
    // blend) order — a linear scan (the test's oracle, O(G) per
    // position: 3000 x <= 24, cheap).
    for (std::size_t g = 0; g < ba.size(); ++g) {
      if (ba[g].atlasId == it.atlasId && ba[g].materialId == it.materialId &&
          ba[g].blend == it.blend) {
        groups[g].emplace_back(it.depthKey, pos);
        break;
      }
    }
  }
  for (std::size_t g = 0; g < ba.size(); ++g) {
    std::stable_sort(groups[g].begin(), groups[g].end(),
                     [](const auto& x, const auto& y) {
                       return x.first < y.first;
                     });
    ASSERT_EQ(groups[g].size(), ba[g].instances.size()) << "group " << g;
    for (std::size_t i = 0; i < groups[g].size(); ++i) {
      EXPECT_EQ(groups[g][i].second, ba[g].instances[i])
          << "group " << g << " instance " << i;
    }
  }
}

// ---------------------------------------------------------------------------
// The zero-allocation proof (PERF-003, the iso_picking / depth_sort
// precedent): 1 000 full frames of the beginFrame/add/build protocol
// allocate nothing — the per-frame paths are pure bookkeeping over the
// pre-allocated storage (the logging facade's own event memory is
// attributed to the diagnostic subsystem, not this window, and no
// warn fires here: no overflow, no overrides).
// ---------------------------------------------------------------------------

TEST(SpriteBatcherZeroAlloc, NoAllocationPerFrame) {
  if (!laige::allocWatchLive()) return;  // sanitizer trees: no-op (the
                                          // leak-free run covers it)
  constexpr std::uint32_t kCapacity = 1024;
  constexpr std::uint32_t kPerFrame = 512;
  constexpr std::uint32_t kFrames = 1000;
  laige::Prng prng = laige::testing::TestPrng(kZeroAllocSubstreamId);
  // The declaration data, precomputed OUTSIDE the measured window:
  // the group fields are fixed (a stable G of <= 16 distinct groups),
  // the keys vary per frame (the mix is cheap integer arithmetic in
  // the measured path — no RNG, no division, the test discipline).
  std::vector<SpriteItem> items;
  items.reserve(kPerFrame);
  for (std::uint32_t i = 0; i < kPerFrame; ++i) {
    items.push_back(makeItem(0, prng.next_range(0, 2), prng.next_range(0, 2),
                             prng.next_range(0, 2) == 0 ? BlendMode::Alpha
                                                        : BlendMode::Additive,
                             false));
  }
  SpriteBatcherOptions o;
  o.maxSprites = kCapacity;
  auto r = SpriteBatcher::create(o);
  ASSERT_TRUE(r.ok());
  SpriteBatcher b = std::move(r).takeValue();
  // One warm frame BEFORE the armed window: the pool's first-fill
  // creates happen here (the setup path), not in the measured run.
  b.beginFrame();
  for (const auto& it : items) {
    ASSERT_TRUE(b.add(it).ok());
  }
  ASSERT_TRUE(b.build().ok());

  laige::allocWatchArm();
  for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
    b.beginFrame();
    for (std::uint32_t i = 0; i < kPerFrame; ++i) {
      SpriteItem it = items[i];
      it.depthKey = mixKey(i) ^ frame;
      ASSERT_TRUE(b.add(it).ok());
    }
    ASSERT_TRUE(b.build().ok());
  }
  const laige::AllocWatchReading reading = laige::allocWatchRead();
  EXPECT_EQ(reading.allocs, 0u)
      << kFrames << " frames allocated " << reading.allocs
      << " heap blocks (first site: "
      << reinterpret_cast<std::uintptr_t>(reading.firstSite) << ")";
}

}  // namespace
