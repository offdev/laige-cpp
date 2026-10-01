// laige-render per-scene-chunk isometric depth key table tests
// (M2-ISO-02): the precomputed, incrementally-updated tile-grid ->
// depth-key map in laige/render/iso_depth_table.h.
//
// Pure data — no GL context, no GL environment needed: every suite
// runs in every local tree and in CI. The goldens are HAND-COMPUTED
// from the documented formula (tile centers (gx + 0.5, gy + 0.5), the
// M2-ISO-01 key); the property test pins the roadmap's "rebuild-from-
// scratch == incremental result" against an independent table built
// through rebuild() (the full isoDepthKey path); the budget suite
// gates the PRD §8.1 `iso_depthkey_rebuild` entry (10k dirty cells
// <= 0.2 ms mean) on the non-instrumented trees (methodology §4:
// instrumentation inflates absolute cost — the sanitizer trees run
// the same workload shapes leak-free under ASan/TSan instead).
//
// Seed: the repo-wide documented default seed via
// tests/support/laige_test_seed.h (docs/testing.md §4), one named
// substream per randomized suite.

#include "laige/render/iso_depth_key.h"
#include "laige/render/iso_depth_table.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/sim_math.h"
#include "laige_test_seed.h"

namespace {

using laige::fpx16_16;
using laige::render::IsoDepthKeyTable;
using laige::sim::Fp32Pinned;
using laige::sim::Fpx16_16;

// One named substream id per randomized suite (docs/testing.md §4).
constexpr std::uint32_t kPropertySubstreamId = 0x49534F52;  // "ISOR"

// ---------------------------------------------------------------------------
// The independent oracle: the M2-ISO-01 function on a tile center —
// the table's keys must equal it cell by cell (bit-identity contract).
// The test builds its own centers (never reusing the table's code).
// ---------------------------------------------------------------------------

// A tile center (gx + 0.5, gy + 0.5) in the backend Vec2 (the
// iso_depth_key_tests BackendPos pattern — dyadic, exact on both
// backends).
template <typename Backend>
struct TileCenter {
  static laige::sim::SimMath<Backend>::Vec2 make(std::int32_t gx,
                                                 std::int32_t gy) noexcept;
};
template <>
struct TileCenter<Fp32Pinned> {
  static laige::sim::SimMathFp32::Vec2 make(std::int32_t gx,
                                            std::int32_t gy) noexcept {
    return laige::sim::SimMathFp32::Vec2{static_cast<float>(gx) + 0.5f,
                                         static_cast<float>(gy) + 0.5f};
  }
};
template <>
struct TileCenter<Fpx16_16> {
  static laige::sim::SimMathFpx16::Vec2 make(std::int32_t gx,
                                             std::int32_t gy) noexcept {
    return laige::sim::SimMathFpx16::Vec2{
        fpx16_16::fromFloat(static_cast<float>(gx) + 0.5f),
        fpx16_16::fromFloat(static_cast<float>(gy) + 0.5f)};
  }
};

template <typename Backend>
std::uint32_t independentKey(std::int32_t gx, std::int32_t gy,
                             std::int32_t height, std::int32_t layer) {
  return laige::render::isoDepthKey<Backend>(
      TileCenter<Backend>::make(gx, gy), height, layer);
}

// ---------------------------------------------------------------------------
// Log capture (the render_thread_tests MemorySink pattern)
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

// Installs a fresh capture sink with rate limiting OFF (the tests
// assert per-event counts, not the facade's LOG-004 window). The
// default levels (global minimum Debug) keep the Debug chunk-creation
// events visible (the "growth ... logged" contract).
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

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

std::size_t countEvents(const MemorySink& sink, std::string_view subsystem,
                        std::string_view event) {
  std::size_t n = 0;
  for (const auto& e : sink.entries) {
    if (e.subsystem == subsystem && e.event == event) ++n;
  }
  return n;
}

bool hasField(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return true;
  }
  return false;
}

std::string fieldOf(const MemorySink::Entry& e, std::string_view key) {
  for (const auto& [k, v] : e.fields) {
    if (k == key) return v;
  }
  return std::string();
}

// The budgets.json path (the repo convention): LAIGE_BUDGETS_PATH,
// then "budgets.json" in the working directory (the ctest gate sets
// the env var to the repo root — tests/laige-render/CMakeLists.txt).
// Platform boundary (the budget_harness_tests.cpp pattern): MSVC
// deprecates plain getenv (C4996, fatal under /WX). Gated builds only
// (the ungated trees never load the table — the helper is defined
// only where it is used, -Wunused-function under -Werror):
#if defined(LAIGE_ISO_DEPTH_BUDGET)
std::string BudgetsFilePath() {
#if defined(_MSC_VER)
  constexpr std::size_t kMax = 4096;
  char buf[kMax];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "LAIGE_BUDGETS_PATH") != 0) {
    return std::string("budgets.json");
  }
  if (len == 0) return std::string("budgets.json");
  return std::string(buf, len);
#else
  const char* env = std::getenv("LAIGE_BUDGETS_PATH");
  return (env != nullptr && *env != '\0') ? std::string(env)
                                          : std::string("budgets.json");
#endif
}
#endif  // LAIGE_ISO_DEPTH_BUDGET

// The machine line for the budget report context (the laige-bench
// operator convention: LAIGE_BENCH_MACHINE, empty when unset — the
// baseline document records the machine facts).
std::string MachineLine() {
#if defined(_MSC_VER)
  constexpr std::size_t kMax = 4096;
  char buf[kMax];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "LAIGE_BENCH_MACHINE") != 0) {
    return std::string();
  }
  return std::string(buf, len);
#else
  const char* env = std::getenv("LAIGE_BENCH_MACHINE");
  return (env != nullptr) ? std::string(env) : std::string();
#endif
}

// The compile-time build identity for the AGENTS §12 "build" context
// field (the laige-bench kCompilerId pattern): compiler + version,
// then the CMake build type stamped by
// tests/laige-render/CMakeLists.txt (LAIGE_ISO_DEPTH_BUILD_TYPE).
// __clang__ is checked BEFORE __GNUC__ (Clang defines the GCC-compat
// macros, and __VERSION__ carries a compiler-specific format that
// would be mis-attributed by the GCC branch — "Clang 22.1.8" on
// recent Clang, "16.2.1 20260810" on GCC). The Clang id is built from
// the version macros: stable across Clang versions regardless of the
// __VERSION__ spelling.
#define LAIGE_ISO_TABLE_STR2(x) #x
#define LAIGE_ISO_TABLE_STR(x) LAIGE_ISO_TABLE_STR2(x)
#if defined(__clang__)
constexpr char kCompilerId[] = "Clang " LAIGE_ISO_TABLE_STR(__clang_major__)
    "." LAIGE_ISO_TABLE_STR(__clang_minor__) "."
    LAIGE_ISO_TABLE_STR(__clang_patchlevel__);
#elif defined(__GNUC__)
constexpr char kCompilerId[] = "GCC " __VERSION__;
#elif defined(_MSC_VER)
// _MSC_FULL_VER is an INTEGER literal (e.g. 194434433), not a string —
// it must be stringified (the laige-bench kCompilerId pattern):
constexpr char kCompilerId[] = "MSVC " LAIGE_ISO_TABLE_STR(_MSC_FULL_VER);
#else
constexpr char kCompilerId[] = "unknown compiler";
#endif

}  // namespace

// ---------------------------------------------------------------------------
// IsoDepthTableCreate — option validation + the fresh-table contract
// ---------------------------------------------------------------------------

// Validation is backend-independent (plain option checks): one
// backend pins it (fpx16_16, the default backend).
TEST(IsoDepthTableCreate, RejectsInvalidOptions) {
  using Table = IsoDepthKeyTable<Fpx16_16>;
  auto base = [] {
    Table::Options o;
    o.widthTiles = 4;
    o.heightTiles = 4;
    return o;
  };
  auto expectReject = [](Table::Options o) {
    auto r = Table::create(o);
    ASSERT_TRUE(r.isError()) << "expected a rejected option set";
    EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  };

  auto o = base();
  o.widthTiles = 0;
  expectReject(o);  // grid extent
  o = base();
  o.heightTiles = 0;
  expectReject(o);  // grid extent
  o = base();
  o.chunkTiles = 0;
  expectReject(o);  // chunk side
  o = base();
  o.chunkTiles = 3;  // not a power of two
  expectReject(o);
  o = base();
  o.maxChunks = 0;
  expectReject(o);  // growth cap
  o = base();
  o.layer = 512;  // above the 10-bit layer field
  expectReject(o);
  o = base();
  o.layer = -513;  // below the 10-bit layer field
  expectReject(o);
  o = base();
  o.originTileX = 32767;  // tile center 32767.5 leaves the key domain
  expectReject(o);
  o = base();
  o.originTileX = -40000;  // below the tile-coordinate domain
  expectReject(o);
  o = base();
  o.originTileX = 30000;
  o.widthTiles = 4000;  // corner tile center 33999.5 > 32767
  expectReject(o);
  o = base();
  o.widthTiles = 128;   // 8 x 8 chunks = 64
  o.heightTiles = 128;  // exceeds the cap below
  o.maxChunks = 8;
  expectReject(o);
}

template <typename Backend>
void flatGroundAndIntrospection() {
  using Table = IsoDepthKeyTable<Backend>;
  typename Table::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;  // covered region: the chunk-aligned 16 x 16
  auto r = Table::create(o);
  ASSERT_TRUE(r.ok());
  const Table& t = r.value();
  EXPECT_EQ(t.chunkCount(), 1u);
  EXPECT_EQ(t.maxChunks(), static_cast<std::size_t>(laige::render::kIsoDepthTableDefaultMaxChunks));
  EXPECT_EQ(t.chunkTiles(), 16);
  EXPECT_EQ(t.layer(), 0);
  EXPECT_EQ(t.originTileX(), 0);
  EXPECT_EQ(t.originTileY(), 0);
  EXPECT_EQ(t.widthTiles(), 4);
  EXPECT_EQ(t.heightTiles(), 4);
  // The covered region is the chunk-aligned superset of the 4 x 4:
  EXPECT_EQ(t.coveredTileMinX(), 0);
  EXPECT_EQ(t.coveredTileMinY(), 0);
  EXPECT_EQ(t.coveredTileMaxX(), 16);
  EXPECT_EQ(t.coveredTileMaxY(), 16);
  EXPECT_EQ(t.coveredCellCount(), 256u);
  EXPECT_TRUE(t.covers(15, 15));   // the superset edge
  EXPECT_TRUE(t.covers(3, 3));     // inside the requested grid
  EXPECT_FALSE(t.covers(16, 0));   // outside the covered region
  EXPECT_FALSE(t.covers(-1, 0));   // outside the covered region
  // A fresh table models flat ground: every key equals the M2-ISO-01
  // key of height 0 on the tile center — cell by cell, against the
  // independent oracle:
  for (std::int32_t gy = 0; gy < 16; ++gy) {
    for (std::int32_t gx = 0; gx < 16; ++gx) {
      EXPECT_EQ(t.keyAt(gx, gy), independentKey<Backend>(gx, gy, 0, 0))
          << "cell (" << gx << ", " << gy << ")";
    }
  }
  // The last tile height read agrees with the flat-ground init:
  EXPECT_EQ(t.tileHeightAt(3, 3), 0);
}

TEST(IsoDepthTableCreate, FlatGroundAndIntrospection) {
  flatGroundAndIntrospection<Fpx16_16>();
  flatGroundAndIntrospection<Fp32Pinned>();
}

// A non-ground layer stamps the layer field: tile (0, 0), height 0,
// layer 1: d = 16, layer field (1 + 512) << 22 = 0x80400000 ->
// key = 0x80400010 (hand-computed).
TEST(IsoDepthTableCreate, NonGroundLayer) {
  using Table = IsoDepthKeyTable<Fpx16_16>;
  Table::Options o;
  o.widthTiles = 2;
  o.heightTiles = 2;
  o.layer = 1;
  auto r = Table::create(o);
  ASSERT_TRUE(r.ok());
  // Hand computation: layer field (1 + 512) << 22 = 0x80400000; d = 16
  // -> d + 2^21 = 0x200010; key = 0x80400000 | 0x200010 = 0x80600010.
  EXPECT_EQ(r.value().keyAt(0, 0), 0x80600010u);
  EXPECT_EQ(r.value().keyAt(0, 0), independentKey<Fpx16_16>(0, 0, 0, 1));
}

// ---------------------------------------------------------------------------
// IsoDepthTableGolden — hand-computed keys for a stepped-terrain grid
// ---------------------------------------------------------------------------
//
// The 4 x 4 grid (tile coords), origin (0, 0), chunk 16, layer 0:
//
//   h(0,0)=0  h(1,0)=0  h(2,0)=0  h(3,0)=0
//   h(0,1)=0  h(1,1)=1  h(2,1)=0  h(3,1)=0
//   h(0,2)=0  h(1,2)=0  h(2,2)=2  h(3,2)=0
//   h(0,3)=0  h(1,3)=0  h(2,3)=0  h(3,3)=0
//
// Hand computation (tile center (gx + 0.5, gy + 0.5); sum = gx + gy + 1;
// q = 16 * sum; d = q - 16*h; key = 0x80000000 | (2^21 + d)):
//
//   (0,0) sum 1  q  16 d  16  -> 0x80200010
//   (1,0) sum 2  q  32 d  32  -> 0x80200020
//   (2,0) sum 3  q  48 d  48  -> 0x80200030
//   (3,0) sum 4  q  64 d  64  -> 0x80200040
//   (0,1) sum 2  q  32 d  32  -> 0x80200020
//   (1,1) sum 3  q  48 d  32  -> 0x80200020   (h = 1: the step UP moves
//   (2,1) sum 4  q  64 d  64  ->    0x80200040      the tile onto the row
//   (3,1) sum 5  q  80 d  80  ->    0x80200050      of its (1,0) neighbor)
//   (0,2) sum 3  q  48 d  48  -> 0x80200030
//   (1,2) sum 4  q  64 d  64  -> 0x80200040
//   (2,2) sum 5  q  80 d  48  -> 0x80200030   (h = 2: the 2-step plateau
//   (3,2) sum 6  q  96 d  96  ->    0x80200060      sits on the (0,2) row)
//   (0,3) sum 4  q  64 d  64  -> 0x80200040
//   (1,3) sum 5  q  80 d  80  -> 0x80200050
//   (2,3) sum 6  q  96 d  96  -> 0x80200060
//   (3,3) sum 7  q 112 d 112  -> 0x80200070

// Fills `keys` with the covered rectangle's keys (row-major) after the
// assertions (void — the gtest macros require a void function).
template <typename Backend>
void goldenScene(std::vector<std::uint32_t>& keys) {
  using Table = IsoDepthKeyTable<Backend>;
  typename Table::Options o;
  o.widthTiles = 4;
  o.heightTiles = 4;  // covered 16 x 16 (the superset)
  auto r = Table::create(o);
  ASSERT_TRUE(r.ok());
  Table t = std::move(r).takeValue();
  const std::int32_t h[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 2, 0},
                                {0, 0, 0, 0}};
  // The rebuild span: the covered 16 x 16 rectangle, row-major; the
  // 4 x 4 pattern inside, flat ground in the superset margin:
  std::vector<std::int32_t> heights(256, 0);
  for (std::int32_t gy = 0; gy < 4; ++gy) {
    for (std::int32_t gx = 0; gx < 4; ++gx) {
      heights[static_cast<std::size_t>(gy) * 16 + gx] = h[gy][gx];
    }
  }
  ASSERT_TRUE(t.rebuild(heights).ok());
  const std::uint32_t golden[4][4] = {{0x80200010, 0x80200020, 0x80200030,
                                       0x80200040},
                                      {0x80200020, 0x80200020, 0x80200040,
                                       0x80200050},
                                      {0x80200030, 0x80200040, 0x80200030,
                                       0x80200060},
                                      {0x80200040, 0x80200050, 0x80200060,
                                       0x80200070}};
  for (std::int32_t gy = 0; gy < 4; ++gy) {
    for (std::int32_t gx = 0; gx < 4; ++gx) {
      EXPECT_EQ(t.keyAt(gx, gy), golden[gy][gx])
          << "cell (" << gx << ", " << gy << ")";
    }
  }
  // The step pairs: the step-up tiles share their neighbor's row:
  EXPECT_EQ(t.keyAt(1, 0), t.keyAt(1, 1));
  EXPECT_EQ(t.keyAt(2, 2), t.keyAt(0, 2));
  // Every covered cell agrees with the independent oracle (the
  // superset margin is flat ground):
  keys.clear();
  keys.reserve(256);
  for (std::int32_t gy = 0; gy < 16; ++gy) {
    for (std::int32_t gx = 0; gx < 16; ++gx) {
      const std::int32_t expected =
          (gx < 4 && gy < 4) ? h[gy][gx] : 0;
      EXPECT_EQ(t.keyAt(gx, gy), independentKey<Backend>(gx, gy, expected, 0))
          << "cell (" << gx << ", " << gy << ")";
      keys.push_back(t.keyAt(gx, gy));
    }
  }
}

TEST(IsoDepthTableGolden, HandComputedSteppedTerrain) {
  std::vector<std::uint32_t> fpx16, fp32;
  goldenScene<Fpx16_16>(fpx16);
  goldenScene<Fp32Pinned>(fp32);
  // Cross-backend agreement: the tile centers are dyadic and inside
  // the exactness zone, so both backends' keys are bit-equal:
  ASSERT_EQ(fpx16.size(), fp32.size());
  for (std::size_t i = 0; i < fpx16.size(); ++i) {
    EXPECT_EQ(fpx16[i], fp32[i]) << "cell " << i;
  }
}

// ---------------------------------------------------------------------------
// IsoDepthTableUpdate — the incremental update contract (FR-2.2)
// ---------------------------------------------------------------------------

template <typename Backend>
void updateContract() {
  using Table = IsoDepthKeyTable<Backend>;
  typename Table::Options o;
  o.widthTiles = 8;
  o.heightTiles = 8;  // covered 16 x 16
  auto r = Table::create(o);
  ASSERT_TRUE(r.ok());
  Table t = std::move(r).takeValue();
  // A deterministic base terrain (inside the 8 x 8 grid), flat in the
  // superset margin:
  std::vector<std::int32_t> heights(256, 0);
  for (std::int32_t gy = 0; gy < 8; ++gy) {
    for (std::int32_t gx = 0; gx < 8; ++gx) {
      heights[static_cast<std::size_t>(gy) * 16 + gx] =
          (3 * gx + 5 * gy) % 4;
    }
  }
  ASSERT_TRUE(t.rebuild(heights).ok());
  std::vector<std::uint32_t> before(256);
  for (std::int32_t gy = 0; gy < 16; ++gy) {
    for (std::int32_t gx = 0; gx < 16; ++gx) {
      before[static_cast<std::size_t>(gy) * 16 + gx] = t.keyAt(gx, gy);
    }
  }

  // A single-tile edit changes ONLY the documented cells: the radius-
  // 0 neighborhood is the edited cell itself. Hand computation:
  // cell (3, 4): center (3.5, 4.5), sum 8, q 128, h 2 -> d 96 ->
  // key 0x80200060.
  ASSERT_TRUE(t.setTile(3, 4, 2).ok());
  EXPECT_EQ(t.keyAt(3, 4), 0x80200060u);
  EXPECT_EQ(t.keyAt(3, 4), independentKey<Backend>(3, 4, 2, 0));
  EXPECT_EQ(t.tileHeightAt(3, 4), 2);
  for (std::size_t i = 0; i < before.size(); ++i) {
    const std::int32_t gx = static_cast<std::int32_t>(i % 16);
    const std::int32_t gy = static_cast<std::int32_t>(i / 16);
    const bool edited = (gx == 3 && gy == 4);
    if (edited) {
      EXPECT_NE(t.keyAt(gx, gy), before[i]);  // the edited cell changed
    } else {
      EXPECT_EQ(t.keyAt(gx, gy), before[i])   // every other cell untouched
          << "cell (" << gx << ", " << gy << ")";
    }
  }

  // Last write wins: setting the cell back to its original height
  // (h0(3,4) = (9 + 20) % 4 = 1) restores the original key:
  ASSERT_TRUE(t.setTile(3, 4, 1).ok());
  EXPECT_EQ(t.keyAt(3, 4), before[4 * 16 + 3]);

  // Rejected edits leave the table UNCHANGED (the Status is the
  // failure channel — the caller handles/logs it, LOG-002):
  auto expectRejected = [](laige::Status s, laige::ErrorCode want) {
    ASSERT_TRUE(s.isError());
    EXPECT_EQ(s.error(), want);
  };
  expectRejected(t.setTile(16, 0, 1),   // not covered
                laige::ErrorCode::InvalidArgument);
  expectRejected(t.setTile(0, 16, 1),   // not covered
                laige::ErrorCode::InvalidArgument);
  expectRejected(t.setTile(-1, 0, 1),   // not covered
                laige::ErrorCode::InvalidArgument);
  expectRejected(t.setTile(3, 4, 2048), // above the key domain
                laige::ErrorCode::InvalidArgument);
  expectRejected(t.setTile(3, 4, -2048),  // below the key domain
                laige::ErrorCode::InvalidArgument);
  for (std::size_t i = 0; i < before.size(); ++i) {
    const std::int32_t gx = static_cast<std::int32_t>(i % 16);
    const std::int32_t gy = static_cast<std::int32_t>(i / 16);
    EXPECT_EQ(t.keyAt(gx, gy), before[i]) << "cell (" << gx << ", " << gy << ")";
  }
  // Boundary heights inside the domain are accepted (2047, -2047):
  ASSERT_TRUE(t.setTile(3, 4, 2047).ok());
  ASSERT_TRUE(t.setTile(3, 4, -2047).ok());
  ASSERT_TRUE(t.setTile(3, 4, 1).ok());  // back to the base height

  // Zero-allocation proof (where the watch is live — the non-
  // sanitizer trees; the sanitizer runtimes own operator new):
  // 100 edits allocate nothing (the roadmap's "zero allocation per
  // update" — the chunk storages are pre-sized).
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    for (std::int32_t i = 0; i < 100; ++i) {
      const std::int32_t gx = i % 8;
      const std::int32_t gy = (i / 8) % 8;
      auto s = t.setTile(gx, gy, (i % 5));
      ASSERT_TRUE(s.ok());
    }
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "100 setTile updates allocated " << reading.allocs
        << " heap blocks (first site: " << (void*)reading.firstSite << ")";
  }
}

TEST(IsoDepthTableUpdate, SingleTileEditChangesOnlyDocumentedCells) {
  updateContract<Fpx16_16>();
  updateContract<Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// IsoDepthTableGrowth — bounded + logged chunk growth (ensureChunk)
// ---------------------------------------------------------------------------

template <typename Backend>
void growthContract() {
  // The capture sink is installed BEFORE the table exists, so the
  // create's Info event and per-chunk Debug events are visible (the
  // "growth bounded + logged" contract).
  auto* sink = installCaptureSink();
  using Table = IsoDepthKeyTable<Backend>;
  typename Table::Options o;
  o.widthTiles = 16;
  o.heightTiles = 16;  // exactly one 16 x 16 chunk
  o.maxChunks = 4;
  auto r = Table::create(o);
  ASSERT_TRUE(r.ok());
  Table t = std::move(r).takeValue();
  // The create logged one Info event + one chunk (Debug event):
  EXPECT_EQ(countEvents(*sink, "render", "iso_depth_table_created"), 1u);
  ASSERT_EQ(countEvents(*sink, "render", "iso_depth_table_chunk_created"),
            1u);
  // Already covered: idempotent no-op.
  ASSERT_TRUE(t.ensureChunk(5, 5).ok());
  EXPECT_EQ(t.chunkCount(), 1u);
  // A negative-side extension: chunk (-1, -1) — the covered region
  // grows to the chunk rectangle [-1, 0] x [-1, 0] (4 chunks = the
  // cap). Hand computation for the new corner cell (-4, -4):
  // center (-3.5, -3.5), sum -7, q -112, d -112 ->
  // key = 0x80000000 | (2^21 - 112) = 0x801FFF90.
  const std::uint32_t beforeKey = t.keyAt(3, 3);
  ASSERT_TRUE(t.ensureChunk(-4, -4).ok());
  EXPECT_EQ(t.chunkCount(), 4u);
  EXPECT_EQ(t.coveredTileMinX(), -16);
  EXPECT_EQ(t.coveredTileMinY(), -16);
  EXPECT_EQ(t.coveredTileMaxX(), 16);
  EXPECT_EQ(t.coveredTileMaxY(), 16);
  EXPECT_TRUE(t.covers(-16, -16));
  EXPECT_FALSE(t.covers(-17, -17));
  EXPECT_FALSE(t.covers(16, 16));
  EXPECT_TRUE(t.covers(15, 15));
  EXPECT_EQ(t.keyAt(-4, -4), 0x801FFF90u);
  EXPECT_EQ(t.keyAt(-4, -4), independentKey<Backend>(-4, -4, 0, 0));
  // The pre-existing cells are untouched by the re-slot:
  EXPECT_EQ(t.keyAt(3, 3), beforeKey);
  // Three more chunks were created (Debug events) — growth is logged:
  EXPECT_EQ(countEvents(*sink, "render", "iso_depth_table_chunk_created"),
            4u);
  // The cap: chunk (1, 0) would grow the region to 3 x 2 = 6 chunks
  // > maxChunks 4: BudgetExhausted + the rate-limited Warn event.
  {
    const laige::Status cap = t.ensureChunk(20, 0);
    ASSERT_TRUE(cap.isError());
    EXPECT_EQ(cap.error(), laige::ErrorCode::BudgetExhausted);
  }
  EXPECT_EQ(t.chunkCount(), 4u);
  EXPECT_EQ(countEvents(*sink, "render", "iso_depth_table_growth_cap"), 1u);
  bool sawCapEvent = false;
  for (const auto& e : sink->entries) {
    if (e.subsystem == "render" && e.event == "iso_depth_table_growth_cap") {
      sawCapEvent = true;
      EXPECT_EQ(e.severity, laige::log::Severity::Warn);
      EXPECT_TRUE(hasField(e, "max_chunks"));
      EXPECT_TRUE(hasField(e, "chunk_count"));
      EXPECT_EQ(fieldOf(e, "max_chunks"), "4");
    }
  }
  EXPECT_TRUE(sawCapEvent);
  // A chunk whose extreme tile centers leave the key domain is
  // rejected before any growth (InvalidArgument):
  {
    const laige::Status oob = t.ensureChunk(40000, 0);
    ASSERT_TRUE(oob.isError());
    EXPECT_EQ(oob.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(t.chunkCount(), 4u);
  restoreLogger();
}

TEST(IsoDepthTableGrowth, BoundedAndLogged) {
  growthContract<Fpx16_16>();
  growthContract<Fp32Pinned>();
}

// ---------------------------------------------------------------------------
// IsoDepthTableProperty — rebuild-from-scratch == incremental result
// ---------------------------------------------------------------------------
//
// A 64 x 64 grid (4096 cells, 16 chunks). Table A is filled PURELY
// incrementally: 4096 setTile calls from flat ground, then 2000
// seeded random edits (plus two forced same-cell edits — last write
// wins). Table B is a fresh table with ONE rebuild() of the final
// grid (the full isoDepthKey path — independent of A's qBase-based
// updates). The property: A == B cell by cell.

// Returns the table's keys (cell by cell, covered-rectangle row-major)
// through the out-param so the helper stays void (the gtest ASSERT/
// EXPECT macros require a void function).
template <typename Backend>
void propertyRun(std::vector<std::uint32_t>& keys) {
  using Table = IsoDepthKeyTable<Backend>;
  constexpr std::int32_t kGrid = 64;
  constexpr std::int32_t kCells = kGrid * kGrid;
  typename Table::Options o;
  o.widthTiles = kGrid;
  o.heightTiles = kGrid;  // exactly 16 chunks; covered 64 x 64
  auto a = Table::create(o);
  ASSERT_TRUE(a.ok());
  Table A = std::move(a).takeValue();
  // The deterministic base terrain:
  std::vector<std::int32_t> finalGrid(kCells);
  for (std::int32_t gy = 0; gy < kGrid; ++gy) {
    for (std::int32_t gx = 0; gx < kGrid; ++gx) {
      const std::int32_t h = (7 * gx + 11 * gy) % 5;
      finalGrid[static_cast<std::size_t>(gy) * kGrid + gx] = h;
      auto s = A.setTile(gx, gy, h);  // the pure-incremental fill
      ASSERT_TRUE(s.ok());
    }
  }
  // The edit sequence: two forced same-cell edits (the last-write-
  // wins case — only the LAST edit per cell is authoritative), then
  // 2000 seeded random edits (duplicates expected):
  {
    auto s = A.setTile(10, 10, 3);
    ASSERT_TRUE(s.ok());
    finalGrid[static_cast<std::size_t>(10) * kGrid + 10] = 3;
    s = A.setTile(10, 10, 1);  // overwrites the first
    ASSERT_TRUE(s.ok());
    finalGrid[static_cast<std::size_t>(10) * kGrid + 10] = 1;
  }
  laige::Prng prng = laige::testing::TestPrng(kPropertySubstreamId);
  for (int i = 0; i < 2000; ++i) {
    const std::int32_t gx = static_cast<std::int32_t>(prng.next_range(0, kGrid));
    const std::int32_t gy = static_cast<std::int32_t>(prng.next_range(0, kGrid));
    const std::int32_t h = static_cast<std::int32_t>(prng.next_range(0, 5u));
    finalGrid[static_cast<std::size_t>(gy) * kGrid + gx] = h;
    auto s = A.setTile(gx, gy, h);
    ASSERT_TRUE(s.ok());
  }
  // The from-scratch rebuild of the FINAL grid (the independent path):
  auto b = Table::create(o);
  ASSERT_TRUE(b.ok());
  Table B = std::move(b).takeValue();
  ASSERT_TRUE(B.rebuild(finalGrid).ok());
  // The property: A == B cell by cell (keys AND stored heights —
  // the heights are the update path's other stored state):
  keys.clear();
  keys.reserve(kCells);
  for (std::size_t i = 0; i < kCells; ++i) {
    const std::int32_t gx = static_cast<std::int32_t>(i % kGrid);
    const std::int32_t gy = static_cast<std::int32_t>(i / kGrid);
    EXPECT_EQ(A.keyAt(gx, gy), B.keyAt(gx, gy))
        << "cell (" << gx << ", " << gy << ")";
    EXPECT_EQ(A.tileHeightAt(gx, gy), B.tileHeightAt(gx, gy))
        << "cell (" << gx << ", " << gy << ")";
    keys.push_back(A.keyAt(gx, gy));
  }
  // Pin A against the independent M2-ISO-01 oracle (bit-identity with
  // isoDepthKey itself, not just A-vs-B agreement) — against the FINAL
  // grid state (only the last edit per cell is authoritative):
  for (std::size_t i = 0; i < kCells; ++i) {
    const std::int32_t gx = static_cast<std::int32_t>(i % kGrid);
    const std::int32_t gy = static_cast<std::int32_t>(i / kGrid);
    const std::int32_t fh = finalGrid[i];
    EXPECT_EQ(A.keyAt(gx, gy), independentKey<Backend>(gx, gy, fh, 0))
        << "cell (" << gx << ", " << gy << ")";
  }
}

TEST(IsoDepthTableProperty, RebuildEqualsIncremental) {
  std::vector<std::uint32_t> fpx16, fp32;
  propertyRun<Fpx16_16>(fpx16);
  propertyRun<Fp32Pinned>(fp32);
  // Cross-backend agreement (dyadic grid-locked centers in the
  // exactness zone):
  ASSERT_EQ(fpx16.size(), fp32.size());
  for (std::size_t i = 0; i < fpx16.size(); ++i) {
    EXPECT_EQ(fpx16[i], fp32[i]) << "cell " << i;
  }
}

// ---------------------------------------------------------------------------
// IsoDepthTableBudget — the PRD §8.1 iso_depthkey_rebuild gate
// ---------------------------------------------------------------------------
//
// The budget workload (deterministic — no RNG in the measured path,
// the m1-sim-tick pattern): a 128 x 128 tile grid (16 384 cells, 64
// chunks) built from a deterministic terrain; one measured iteration
// is a terrain edit that DIRTIES a 100 x 100 block of 10 000 cells
// (the block sits at (14, 14) inside the grid), applied as 10 000
// setTile calls — the "10k dirty cells after a terrain edit" of the
// budgets.json entry (metric: mean; target: 0.2 ms).
//
// The absolute 0.2 ms target applies only on the reference platform
// (the non-instrumented Linux trees — methodology §4/§5: the sanitizer
// runtimes inflate absolute cost and own operator new; the shared
// macOS/Windows runners are slower and noisier than the ubuntu-24.04
// reference). Everywhere else the suite runs the SAME workload
// ungated: leak-free under ASan/TSan, correctness-only on the other
// P0 platforms (the m1-sim-tick gated-vs-smoke precedent).

// The workload shape (named constants — CORE-005):
constexpr std::int32_t kBudgetGrid = 128;
constexpr std::int32_t kBudgetBlock = 100;        // 100 x 100 dirty cells
constexpr std::int32_t kBudgetBlockOrigin = 14;   // inside the grid
constexpr std::int32_t kBudgetWarmup = 100;
constexpr std::int32_t kBudgetRuns = 3000;

// One backend run of the budget workload. `entry == nullptr` selects
// the UNGATED run (the sanitizer and non-reference platforms): the
// workload runs for its leak/correctness value, no timing. A non-null
// `entry` selects the GATED run (the reference platform): warm-up +
// the measured window + budgetCheck; the result carries the report.
// The gtest expectations live in the TEST body (the gtest macros must
// not run from a helper outside a test).
template <typename Backend>
laige::BudgetCheckResult runBudgetBackend(const laige::BudgetEntry* entry) {
  using Table = IsoDepthKeyTable<Backend>;
  typename Table::Options o;
  o.widthTiles = kBudgetGrid;
  o.heightTiles = kBudgetGrid;  // exactly 8 x 8 chunks
  auto r = Table::create(o);
  if (!r.ok()) {
    std::fprintf(stderr, "runBudgetBackend: create failed: %s\n",
                 laige::errorText(r.error()));
    std::abort();
  }
  Table t = std::move(r).takeValue();
  // The deterministic terrain (rebuild span: the covered 128 x 128):
  std::vector<std::int32_t> heights(static_cast<std::size_t>(kBudgetGrid) *
                                    kBudgetGrid);
  for (std::int32_t gy = 0; gy < kBudgetGrid; ++gy) {
    for (std::int32_t gx = 0; gx < kBudgetGrid; ++gx) {
      heights[static_cast<std::size_t>(gy) * kBudgetGrid + gx] =
          (7 * gx + 11 * gy) % 5;
    }
  }
  if (!t.rebuild(heights).ok()) {
    std::fprintf(stderr, "runBudgetBackend: rebuild failed\n");
    std::abort();
  }
  // The edit sequence, precomputed once OUTSIDE the measured window:
  // the exact (tileX, tileY, height) triples the workload applies —
  // 10 000 column-major calls over the 100 x 100 block (tileX the outer
  // index, tileY the inner; the same order the original i-loop
  // produced), new height the fixed function (gx + gy) % 5 of the cell
  // (steady state — no cell is written twice within one iteration).
  // Precomputing keeps the MEASURED loop free of integer division: the
  // original formulation computed i / 100, i % 100 and (gx + gy) % 5
  // per call, and in CMake-Debug (-O0) builds that is three real
  // div/idiv instructions per iteration (each ~20-30 x86 cycles, on top
  // of the setTile call) — the gate was measuring the harness's
  // division codegen, not the engine's 10 000 setTile calls, which
  // pushed the CI clang-18 reference lane to 0.209 ms against the 0.2
  // ms budget (run 36885653175, job 110448173121, 2026-10-01). The
  // precomputation loop itself is outside every measured window (the
  // warm-up and the measured runs both start below it), and the
  // measured loop's setTile call sequence is byte-identical to the
  // original's (same order, same arguments):
  struct BudgetEdit {
    std::int32_t x;
    std::int32_t y;
    std::int32_t h;
  };
  constexpr std::size_t kBudgetEditCount =
      static_cast<std::size_t>(kBudgetBlock) * kBudgetBlock;
  std::array<BudgetEdit, kBudgetEditCount> edits;
  for (std::size_t i = 0; i < kBudgetEditCount; ++i) {
    const std::int32_t gx = kBudgetBlockOrigin +
        static_cast<std::int32_t>(
            i / static_cast<std::size_t>(kBudgetBlock));
    const std::int32_t gy = kBudgetBlockOrigin +
        static_cast<std::int32_t>(
            i % static_cast<std::size_t>(kBudgetBlock));
    edits[i] = BudgetEdit{gx, gy, (gx + gy) % 5};
  }
  // One measured iteration: the terrain edit applied as 10 000 setTile
  // calls in the precomputed order — the division-free measured loop:
  auto applyEdit = [&t, &edits]() {
    for (const BudgetEdit& e : edits) {
      if (!t.setTile(e.x, e.y, e.h).ok()) {
        std::fprintf(stderr,
                     "runBudgetBackend: setTile(%d, %d, %d) failed inside "
                     "the workload\n",
                     e.x, e.y, e.h);
        std::abort();  // a workload-shape failure is a test failure
      }
    }
  };
  // The ungated run (leak/correctness only, no timing):
  if (entry == nullptr) {
    for (std::int32_t i = 0; i < kBudgetWarmup; ++i) applyEdit();
    return {};  // an empty (unchecked) result — the caller does not gate
  }
  // The gated run: warm-up (first-touch costs) then the measured window:
  for (std::int32_t i = 0; i < kBudgetWarmup; ++i) applyEdit();
  laige::Histogram hist(
      laige::Histogram::Options{static_cast<std::size_t>(kBudgetRuns)});
  for (std::int32_t i = 0; i < kBudgetRuns; ++i) {
    laige::TimeIt timer;
    applyEdit();
    hist.record(timer.elapsedMs());
  }
  // The AGENTS §12 context (stable pointers — the ctx fields are
  // const char*; the strings live to the end of the call):
  const std::string buildLine = std::string(kCompilerId) +
#if defined(LAIGE_ISO_DEPTH_BUILD_TYPE)
      ", CMake " LAIGE_ISO_DEPTH_BUILD_TYPE
#else
      ", CMake build type unknown"
#endif
      ", engine policy (NFR-8.10)";
  const std::string machine = MachineLine();
  laige::BudgetReportContext ctx;
  ctx.workload = entry->workload.c_str();
  ctx.build = buildLine.c_str();
  ctx.machine = machine.c_str();
  ctx.warmup = static_cast<std::uint32_t>(kBudgetWarmup);
  return laige::budgetCheck(*entry, hist, ctx);
}

TEST(IsoDepthTableBudget, TenThousandDirtyCells) {
  // The gated run loads the entry (the reference platform); the
  // ungated run passes a null entry (the workload still runs):
  const laige::BudgetEntry* entry = nullptr;
#if defined(LAIGE_ISO_DEPTH_BUDGET)
  const std::string path = BudgetsFilePath();
  const laige::Result<laige::BudgetTable, laige::ErrorCode> loaded =
      laige::loadBudgets(path);
  ASSERT_TRUE(loaded.ok()) << "loadBudgets(\"" << path << "\") failed: "
                           << laige::errorText(loaded.error());
  entry = loaded.value().find("iso_depthkey_rebuild");
  ASSERT_NE(entry, nullptr)
      << "budgets.json has no iso_depthkey_rebuild entry";
  ASSERT_EQ(entry->metric, laige::BudgetMetric::Mean);
#endif
  // Both backends run the workload (ADR 0002: the gate is backend-
  // complete); the budgets.json `measured` records the worse of the
  // two when gated:
  const laige::BudgetCheckResult fpx16 =
      runBudgetBackend<Fpx16_16>(entry);
#if defined(LAIGE_ISO_DEPTH_BUDGET)
  // Land the stable report in the ctest log (AGENTS §12):
  std::fputs(fpx16.report.c_str(), stdout);
  std::fflush(stdout);
  EXPECT_TRUE(fpx16.passed) << "fpx16_16: " << fpx16.report;
#endif
  const laige::BudgetCheckResult fp32 =
      runBudgetBackend<Fp32Pinned>(entry);
#if defined(LAIGE_ISO_DEPTH_BUDGET)
  std::fputs(fp32.report.c_str(), stdout);
  std::fflush(stdout);
  EXPECT_TRUE(fp32.passed) << "fp32_pinned: " << fp32.report;
#else
  // The ungated run: a test body without assertions passes — the
  // workload's value here is the leak/race/correctness coverage the
  // sanitizer runtimes (and the non-reference runners) provide.
  (void)fpx16;
  (void)fp32;
#endif
}
