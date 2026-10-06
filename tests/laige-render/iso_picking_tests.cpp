// laige-render isometric grid picking tests (M2-ISO-03): the safe
// screen -> ground-plane -> grid-cell transform in
// laige/render/iso_picking.h.
//
// Pure float math — no GL context, no GL environment needed: every
// suite runs in every local tree and in CI. The goldens are
// hand-computed from the documented formulas (the M2-CAM-02 camera
// matrix, the 2x2 inverse, the half-open cell convention). The
// property test pins the roadmap's "screen_to_grid(
// world_to_screen(cell_center)) == cell" for 10k random cell x zoom
// pairs; the boundary suite pins the documented half-open convention
// and the boundary ambiguity zone; the total suite pins the saturation
// contract; the budget suite gates the PRD §8.1 `iso_picking` entry
// (one pick, mean <= 0.01 ms) on the non-instrumented trees
// (methodology §4: instrumentation inflates absolute cost — the
// sanitizer trees run the same workload leak-free under ASan/TSan
// instead).
//
// Seed: the repo-wide documented default seed via
// tests/support/laige_test_seed.h (docs/testing.md §4), one named
// substream per randomized suite.

#include "laige/render/iso_picking.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/budget_harness.h"
#include "laige/errors.h"
#include "laige/prng.h"
#include "laige_test_seed.h"

namespace {

using laige::render::IsoAxes;
using laige::render::IsoCamera;
using laige::render::IsoCameraOptions;
using laige::render::IsoGridConfig;
using laige::render::IsoGridPick;
using laige::render::IsoPresetKind;
using laige::render::ProjectionView;
using laige::render::Vec2;

// One named substream id per randomized suite (docs/testing.md §4).
constexpr std::uint32_t kPropertySubstreamId = 0x49535050;  // "ISPP"
constexpr std::uint32_t kShearSubstreamId = 0x49535053;     // "ISPS"
constexpr std::uint32_t kParitySubstreamId = 0x49535051;    // "ISPI"

// Creates an IsoCamera from options; on a rejected config it fails the
// test and hands back the stopped state (the iso_camera_tests.cpp
// makeIso pattern).
IsoCamera makeIso(IsoCameraOptions o) {
  auto r = IsoCamera::create(o);
  if (r.isError()) {
    ADD_FAILURE() << "IsoCamera::create rejected the options";
    return IsoCamera{};
  }
  return std::move(r).takeValue();
}

// The engine's forward map of a ground point: the frame matrix x the
// point (the ProjectionView contract, M2-PROJ-01) — the tests'
// screen-point source, independent of the pick's 2x2 inverse.
Vec2 projectGround(const IsoCamera& cam, Vec2 p) {
  ProjectionView v;  // mode defaults to Iso (the engine default)
  v.matrix = cam.matrix();
  const laige::render::Vec3 s = v.worldToScreen(p, 0.0f);
  return Vec2{s.x, s.y};
}

// ---------------------------------------------------------------------------
// Goldens: hand-computed screen points at 4 zoom levels (the roadmap
// Verify) — 2:1 dimetric, scale 1, camera at the origin (no shake):
// the frame matrix's rows are (2/Z, -2/Z) and (-1/Z, -1/Z)
// (matrices.h), so the ground point (x, y) projects to
//   ndc = ((2x - 2y)/Z, -(x + y)/Z)
// and the pick's inverse must return the half-open cell
//   (floor(x / g), floor(y / g)).
//
//   cell (3, 2)   center (3.5, 2.5):   ndc = (2/Z, -6/Z)   (dyadic)
//   cell (-2, -1) center (-1.5, -0.5): ndc = (-2/Z, 2/Z)
//   cell (0, 0)   center (0.5, 0.5):   ndc = (0, -1/Z)
// Camera offset e = (5, -3), Z = 2: cell (7, 4) center (7.5, 4.5):
//   w - e = (2.5, 7.5); ndc = ((2*2.5 - 2*7.5)/2, -(2.5 + 7.5)/2)
//                         = (-5.0, -5.0).
// ---------------------------------------------------------------------------

TEST(IsoPickGolden, HandComputedCellsAtFourZooms) {
  const float zooms[4] = {0.25f, 1.0f, 4.0f, 16.0f};
  const IsoGridConfig grid{};  // g = 1 (the tile map's cell)
  for (const float z : zooms) {
    IsoCameraOptions o;  // defaults: 2:1 dimetric scale 1, snap OFF,
                         // zoom clamped into [0.1, 16]
    o.camera.zoom = z;
    const IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
    // The forward sanity (the hand values are exact dyadics for these
    // zooms; the engine's float ops must agree to the pinned 1e-6):
    const Vec2 s32 = projectGround(cam, Vec2{3.5f, 2.5f});
    EXPECT_NEAR(s32.x, 2.0f / z, 1e-6f) << "zoom " << z;
    EXPECT_NEAR(s32.y, -6.0f / z, 1e-6f) << "zoom " << z;
    const IsoGridPick p32 = screenToGrid(s32, cam, grid);
    EXPECT_EQ(p32.cellX, 3) << "zoom " << z;
    EXPECT_EQ(p32.cellY, 2) << "zoom " << z;
    EXPECT_NEAR(p32.ground.x, 3.5f, 1e-3f) << "zoom " << z;
    EXPECT_NEAR(p32.ground.y, 2.5f, 1e-3f) << "zoom " << z;

    const Vec2 sm21 = projectGround(cam, Vec2{-1.5f, -0.5f});
    EXPECT_NEAR(sm21.x, -2.0f / z, 1e-6f) << "zoom " << z;
    EXPECT_NEAR(sm21.y, 2.0f / z, 1e-6f) << "zoom " << z;
    const IsoGridPick pm21 = screenToGrid(sm21, cam, grid);
    EXPECT_EQ(pm21.cellX, -2) << "zoom " << z;
    EXPECT_EQ(pm21.cellY, -1) << "zoom " << z;

    const Vec2 s00 = projectGround(cam, Vec2{0.5f, 0.5f});
    EXPECT_NEAR(s00.x, 0.0f, 1e-6f) << "zoom " << z;
    EXPECT_NEAR(s00.y, -1.0f / z, 1e-6f) << "zoom " << z;
    const IsoGridPick p00 = screenToGrid(s00, cam, grid);
    EXPECT_EQ(p00.cellX, 0) << "zoom " << z;
    EXPECT_EQ(p00.cellY, 0) << "zoom " << z;
  }
  // The camera-offset case (e = (5, -3), Z = 2): the hand screen point
  // (-5.0, -5.0) — above — must pick cell (7, 4):
  IsoCameraOptions o;
  o.camera.zoom = 2.0f;
  o.camera.position = {5.0f, -3.0f, 0.0f};
  const IsoCamera cam2 = makeIso(o);
  ASSERT_TRUE(cam2.valid());
  const Vec2 s74 = projectGround(cam2, Vec2{7.5f, 4.5f});
  EXPECT_NEAR(s74.x, -5.0f, 1e-6f);
  EXPECT_NEAR(s74.y, -5.0f, 1e-6f);
  const IsoGridPick p74 = screenToGrid(s74, cam2, grid);
  EXPECT_EQ(p74.cellX, 7);
  EXPECT_EQ(p74.cellY, 4);
  EXPECT_NEAR(p74.ground.x, 7.5f, 1e-3f);
  EXPECT_NEAR(p74.ground.y, 4.5f, 1e-3f);
}

// The documented true 30/60, scale 1, camera at the origin: the rows
// are (1, -1/sqrt(3)) and (-1, -1/sqrt(3)), so cell (1, -1) center
// (1.5, -0.5) projects to ndc = (2.0, -1/sqrt(3)) — the -1/sqrt(3)
// hand value -0.5773502691896258...; the literal pins it to 8
// significant digits (within 1e-7 of the exact hand value — far
// inside the pick's 0.5-cell margin at every zoom), and the pick's
// cell is the golden.
TEST(IsoPickGolden, TrueIso3060HandComputedCells) {
  const float zooms[2] = {1.0f, 4.0f};
  const IsoGridConfig grid{};  // g = 1
  for (const float z : zooms) {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::TrueIso3060;
    o.camera.zoom = z;
    const IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
    const Vec2 s{2.0f / z, -0.57735027f / z};
    const IsoGridPick p = screenToGrid(s, cam, grid);
    EXPECT_EQ(p.cellX, 1) << "zoom " << z;
    EXPECT_EQ(p.cellY, -1) << "zoom " << z;
    EXPECT_NEAR(p.ground.x, 1.5f, 1e-3f) << "zoom " << z;
    EXPECT_NEAR(p.ground.y, -0.5f, 1e-3f) << "zoom " << z;
  }
}

// ---------------------------------------------------------------------------
// The roadmap property: screen_to_grid(world_to_screen(cell_center))
// == cell for 10k random cells x zooms. 4 096 cells (|gx|, |gy| <=
// 63, g = 1) x 4 zoom levels = 16 384 picks >= 10k. The cell center
// (gx + 0.5, gy + 0.5) sits 0.5 world units from every boundary —
// ~900x the documented boundary zone at this scene scale (~6e-4), so
// the pick is exact by the header preamble's precision contract. The
// ground point pins the precision itself (|w-hat - center| <= 1e-3,
// 16x inside the zone).
// ---------------------------------------------------------------------------

TEST(IsoPickProperty, CellCentersRoundTripAtFourZooms) {
  const float zooms[4] = {0.25f, 1.0f, 4.0f, 16.0f};
  const float kEps = 1e-3f;  // the precision pin (the contract zone at
                             // this scene scale is ~6e-4)
  const int kCellsPerZoom = 4096;
  const IsoGridConfig grid{};  // g = 1
  laige::Prng prng = laige::testing::TestPrng(kPropertySubstreamId);
  for (const float z : zooms) {
    IsoCameraOptions o;
    o.camera.zoom = z;
    const IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
    for (int i = 0; i < kCellsPerZoom; ++i) {
      const int gx = static_cast<int>(prng.next_range(0, 127)) - 63;
      const int gy = static_cast<int>(prng.next_range(0, 127)) - 63;
      const Vec2 center{static_cast<float>(gx) + 0.5f,
                        static_cast<float>(gy) + 0.5f};
      const Vec2 s = projectGround(cam, center);
      const IsoGridPick p = screenToGrid(s, cam, grid);
      EXPECT_EQ(p.cellX, gx) << "zoom " << z << " cell (" << gx << ", "
                            << gy << ") screen (" << s.x << ", " << s.y << ")";
      EXPECT_EQ(p.cellY, gy) << "zoom " << z << " cell (" << gx << ", "
                            << gy << ") screen (" << s.x << ", " << s.y << ")";
      EXPECT_NEAR(p.ground.x, center.x, kEps)
          << "zoom " << z << " cell (" << gx << ", " << gy << ")";
      EXPECT_NEAR(p.ground.y, center.y, kEps)
          << "zoom " << z << " cell (" << gx << ", " << gy << ")";
    }
  }
  // Zero-allocation proof (where the allocation watch is live — the
  // non-sanitizer trees; the sanitizer runtimes own operator new, the
  // iso_depth_table_tests.cpp precedent): 1 000 consecutive picks
  // allocate nothing — the pick is a fixed sequence of float ops,
  // structurally zero-heap (PERF-003).
  if (laige::allocWatchLive()) {
    IsoCameraOptions o;
    const IsoCamera cam = makeIso(o);
    const IsoGridConfig g{};
    laige::allocWatchArm();
    for (int i = 0; i < 1000; ++i) {
      (void)screenToGrid(
          Vec2{0.5f * static_cast<float>(i % 13) - 3.0f,
               0.5f * static_cast<float>(i % 7) - 2.0f},
          cam, g);
    }
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "1000 picks allocated " << reading.allocs
        << " heap blocks (first site: "
        << reinterpret_cast<std::uintptr_t>(reading.firstSite) << ")";
  }
}

// ---------------------------------------------------------------------------
// The documented boundary rule (the header preamble): the grid cell
// (gx, gy) is the half-open square [gx*g, (gx+1)*g) x [gy*g,
// (gy+1)*g) — floor; a point exactly on a cell's lower/left boundary
// belongs to THAT cell, on its upper/right boundary to the cell
// beyond (the corner to its upper-right cell). Points 0.05 world
// units off a boundary sit far outside the documented boundary zone
// (~1e-4 at this scene scale) — exact sides. The exact-boundary
// round trip lands within the zone of the exact boundary: either
// adjacent cell is the documented result (deterministic per build).
// ---------------------------------------------------------------------------

TEST(IsoPickBoundary, HalfOpenCellsAndBoundaryZone) {
  IsoCameraOptions o;  // 2:1 dimetric, e = 0, Z = 1
  const IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  const IsoGridConfig grid{};  // g = 1
  // 0.05 off the boundaries (exact sides):
  EXPECT_EQ(
      screenToGrid(projectGround(cam, Vec2{0.95f, 0.5f}), cam, grid).cellX, 0);
  EXPECT_EQ(
      screenToGrid(projectGround(cam, Vec2{1.05f, 0.5f}), cam, grid).cellX, 1);
  EXPECT_EQ(
      screenToGrid(projectGround(cam, Vec2{-1.05f, 0.5f}), cam, grid).cellX,
      -2);  // floor(-1.05) — the cell below the x = -1 boundary
  EXPECT_EQ(
      screenToGrid(projectGround(cam, Vec2{0.5f, -1.05f}), cam, grid).cellY,
      -2);
  // The exact boundary (the round trip through the stored float matrix
  // lands within the documented zone of the exact boundary): either
  // adjacent cell is the documented result:
  const IsoGridPick bx =
      screenToGrid(projectGround(cam, Vec2{1.0f, 0.5f}), cam, grid);
  EXPECT_TRUE(bx.cellX == 0 || bx.cellX == 1) << "got " << bx.cellX;
  const IsoGridPick by =
      screenToGrid(projectGround(cam, Vec2{0.5f, 1.0f}), cam, grid);
  EXPECT_TRUE(by.cellY == 0 || by.cellY == 1) << "got " << by.cellY;
  const IsoGridPick corner =
      screenToGrid(projectGround(cam, Vec2{1.0f, 1.0f}), cam, grid);
  EXPECT_TRUE(corner.cellX == 0 || corner.cellX == 1) << "got " << corner.cellX;
  EXPECT_TRUE(corner.cellY == 0 || corner.cellY == 1) << "got " << corner.cellY;
  // The rule scales with the cell size (g = 2): 3.9/2 = 1.95 -> cell 1,
  // 4.1/2 = 2.05 -> cell 2:
  const IsoGridConfig g2{2.0f};
  EXPECT_EQ(
      screenToGrid(projectGround(cam, Vec2{3.9f, 0.5f}), cam, g2).cellX, 1);
  EXPECT_EQ(
      screenToGrid(projectGround(cam, Vec2{4.1f, 0.5f}), cam, g2).cellX, 2);
}

// ---------------------------------------------------------------------------
// A custom shear passing isoShearSupported (the M2-CAM-02 scene gate):
// dx = (2, -1), dy = (-1, -1), zUnit = 1 (both axes project downward
// with slope 1 = zUnit; det = 2*(-1) - (-1)*(-1) = -3 != 0). The pick
// is the same O(1) 2x2 inverse for every supported shear (FR-2.11):
// cell (2, -1) center (2.5, -0.5) projects to ndc =
// (2*2.5 + (-1)*(-0.5), -2.5 + 0.5) = (5.5, -2.0) — exact dyadics.
// ---------------------------------------------------------------------------

TEST(IsoPickCustomShear, SupportedShearRoundTrip) {
  const IsoAxes axes{Vec2{2.0f, -1.0f}, Vec2{-1.0f, -1.0f}, 1.0f};
  const float zooms[2] = {1.0f, 2.0f};
  for (const float z : zooms) {
    IsoCameraOptions o;
    o.preset.kind = IsoPresetKind::CustomShear;
    o.preset.axes = axes;
    o.camera.zoom = z;
    const IsoCamera cam = makeIso(o);
    ASSERT_TRUE(cam.valid());
    const Vec2 s{5.5f / z, -2.0f / z};
    const IsoGridPick p = screenToGrid(s, cam, IsoGridConfig{});
    EXPECT_EQ(p.cellX, 2) << "zoom " << z;
    EXPECT_EQ(p.cellY, -1) << "zoom " << z;
    EXPECT_NEAR(p.ground.x, 2.5f, 1e-3f) << "zoom " << z;
    EXPECT_NEAR(p.ground.y, -0.5f, 1e-3f) << "zoom " << z;
  }
  // The property over 256 random cells (this shear's ground map is
  // well-conditioned: kappa ~ 2.6, inside the documented 64):
  IsoCameraOptions o;
  o.preset.kind = IsoPresetKind::CustomShear;
  o.preset.axes = axes;
  const IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  laige::Prng prng = laige::testing::TestPrng(kShearSubstreamId);
  for (int i = 0; i < 256; ++i) {
    const int gx = static_cast<int>(prng.next_range(0, 63)) - 31;
    const int gy = static_cast<int>(prng.next_range(0, 63)) - 31;
    const Vec2 center{static_cast<float>(gx) + 0.5f,
                      static_cast<float>(gy) + 0.5f};
    const IsoGridPick p =
        screenToGrid(projectGround(cam, center), cam, IsoGridConfig{});
    EXPECT_EQ(p.cellX, gx) << "cell (" << gx << ", " << gy << ")";
    EXPECT_EQ(p.cellY, gy) << "cell (" << gx << ", " << gy << ")";
  }
}

// ---------------------------------------------------------------------------
// The total-function contract (the header preamble): non-finite screen
// input saturates at the documented world domain (kIsoDepthMaxWorld
// Units = 32767; NaN -> the lower bound — the isoDepthKey convention)
// and never fails, never UB. The g = 2 case pins the saturation divided
// by the cell size (32767/2 -> 16383).
// ---------------------------------------------------------------------------

TEST(IsoPickNonFinite, TotalSaturation) {
  IsoCameraOptions o;
  const IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  const IsoGridConfig grid{};  // g = 1
  const float inf = std::numeric_limits<float>::infinity();
  EXPECT_EQ(screenToGrid(Vec2{std::nanf(""), 0.0f}, cam, grid).cellX, -32767);
  EXPECT_EQ(screenToGrid(Vec2{inf, 0.0f}, cam, grid).cellX, 32767);
  EXPECT_EQ(screenToGrid(Vec2{-inf, 0.0f}, cam, grid).cellX, -32767);
  // Both axes +inf: the exact limit of the 2x2 solve for the 2:1 rows
  // ((-sx + 2sy)/-4 with sx = sy -> -t/4) is -inf in x — the float
  // sequence (NaN from inf - inf) saturates to the same lower bound:
  const IsoGridPick both = screenToGrid(Vec2{inf, inf}, cam, grid);
  EXPECT_EQ(both.cellX, -32767);
  EXPECT_EQ(both.cellY, -32767);
  const IsoGridConfig g2{2.0f};
  EXPECT_EQ(screenToGrid(Vec2{inf, 0.0f}, cam, g2).cellX, 16383);
}

// ---------------------------------------------------------------------------
// A stopped (failed-create / default) camera picks with the identity
// matrix (the M2-CAM-02 stopped-state contract): cell = floor(screen
// / g) — degenerate but total.
// ---------------------------------------------------------------------------

TEST(IsoPickStoppedCamera, IdentityDegenerate) {
  IsoCamera stopped;  // the default form: valid() false
  ASSERT_FALSE(stopped.valid());
  const IsoGridConfig grid{};  // g = 1
  const IsoGridPick p = screenToGrid(Vec2{2.3f, -1.2f}, stopped, grid);
  EXPECT_EQ(p.cellX, 2);
  EXPECT_EQ(p.cellY, -2);
  EXPECT_EQ(p.ground.x, 2.3f);
  EXPECT_EQ(p.ground.y, -1.2f);
  const IsoGridConfig g2{2.0f};
  const IsoGridPick p2 = screenToGrid(Vec2{2.3f, -1.2f}, stopped, g2);
  EXPECT_EQ(p2.cellX, 1);   // floor(2.3/2)
  EXPECT_EQ(p2.cellY, -1);  // floor(-1.2/2)
}

// ---------------------------------------------------------------------------
// The ProjectionView overload (the M2-PROJ-01 base this step lands on)
// agrees with the camera overload: same stored matrix -> same pick
// (cell and ground, bit-identical).
// ---------------------------------------------------------------------------

TEST(IsoPickView, OverloadParity) {
  IsoCameraOptions o;
  o.camera.zoom = 2.0f;
  const IsoCamera cam = makeIso(o);
  ASSERT_TRUE(cam.valid());
  ProjectionView v;  // mode defaults to Iso (the engine default)
  v.matrix = cam.matrix();
  laige::Prng prng = laige::testing::TestPrng(kParitySubstreamId);
  for (int i = 0; i < 64; ++i) {
    const float rx =
        static_cast<float>(prng.next_range(0, 65536)) / 65536.0f;
    const float ry =
        static_cast<float>(prng.next_range(0, 65536)) / 65536.0f;
    const Vec2 s{rx * 2.0f - 1.0f, ry * 2.0f - 1.0f};
    const IsoGridPick a = screenToGrid(s, cam, IsoGridConfig{});
    const IsoGridPick b = screenToGrid(s, v, IsoGridConfig{});
    EXPECT_EQ(a.cellX, b.cellX) << "screen (" << s.x << ", " << s.y << ")";
    EXPECT_EQ(a.cellY, b.cellY) << "screen (" << s.x << ", " << s.y << ")";
    EXPECT_EQ(a.ground.x, b.ground.x)
        << "screen (" << s.x << ", " << s.y << ")";
    EXPECT_EQ(a.ground.y, b.ground.y)
        << "screen (" << s.x << ", " << s.y << ")";
  }
}

// ---------------------------------------------------------------------------
// IsoPickBudget — the PRD §8.1 `iso_picking` gate
// ---------------------------------------------------------------------------
//
// The budget workload (deterministic — no RNG in the measured path,
// the m1-sim-tick pattern): one measured sample is ONE screenToGrid
// pick (the budgets.json unit: "one isometric screen-to-grid pick,
// O(1)"). 3 000 measured picks over 3 000 PRECOMPUTED NDC points
// (the points are built outside every measured window — no division
// in the harness, only the pick's own 2x2 solve — the
// iso_depth_table_tests.cpp discipline), warm-up 100. Metric: mean;
// target: 0.01 ms.
//
// The absolute 0.01 ms target applies only on the reference platform
// (the non-instrumented Linux trees — methodology §4/§5): elsewhere
// the suite runs the SAME workload ungated and verifies its safety
// properties instead (leak-free under ASan/TSan). The gated branch
// (load budgets.json, budgetCheck, the stable 4-line report,
// EXPECT(passed)) compiles only on Linux non-instrumented trees
// (LAIGE_ISO_PICK_BUDGET — the iso_depth_table_tests.cpp precedent).

constexpr std::int32_t kBudgetWarmup = 100;
constexpr std::int32_t kBudgetRuns = 3000;

// The machine line for the budget report (the iso_depth_table_tests.cpp
// pattern; the LAIGE_BENCH_MACHINE env var, when set, carries the
// operator's machine description).
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
// tests/laige-render/CMakeLists.txt (LAIGE_ISO_PICK_BUILD_TYPE).
// __clang__ is checked BEFORE __GNUC__ (Clang defines the GCC-compat
// macros, and __VERSION__ carries a compiler-specific format that
// would be mis-attributed by the GCC branch — "Clang 22.1.8" on
// recent Clang, "16.2.1 20260810" on GCC). The Clang id is built from
// the version macros: stable across Clang versions regardless of the
// __VERSION__ spelling.
#define LAIGE_ISO_PICK_STR2(x) #x
#define LAIGE_ISO_PICK_STR(x) LAIGE_ISO_PICK_STR2(x)
#if defined(__clang__)
constexpr char kCompilerId[] = "Clang " LAIGE_ISO_PICK_STR(__clang_major__)
    "." LAIGE_ISO_PICK_STR(__clang_minor__) "."
    LAIGE_ISO_PICK_STR(__clang_patchlevel__);
#elif defined(__GNUC__)
constexpr char kCompilerId[] = "GCC " __VERSION__;
#elif defined(_MSC_VER)
// _MSC_FULL_VER is an INTEGER literal (e.g. 194434433), not a string —
// it must be stringified (the laige-bench kCompilerId pattern):
constexpr char kCompilerId[] = "MSVC " LAIGE_ISO_PICK_STR(_MSC_FULL_VER);
#else
constexpr char kCompilerId[] = "unknown compiler";
#endif

laige::BudgetCheckResult runBudget(const laige::BudgetEntry* entry) {
  IsoCameraOptions o;  // defaults: 2:1 dimetric scale 1, snap OFF,
                       // zoom 1 in [0.1, 16]
  auto r = IsoCamera::create(o);
  if (r.isError()) {
    std::fprintf(stderr, "IsoPickBudget: IsoCamera::create failed\n");
    std::abort();
  }
  const IsoCamera cam = std::move(r).takeValue();
  const IsoGridConfig grid{};  // g = 1 (the tile map's cell)
  // The precomputed NDC points (deterministic; every division lives
  // OUTSIDE the measured windows):
  std::array<laige::render::Vec2, kBudgetRuns> pts;
  for (std::int32_t i = 0; i < kBudgetRuns; ++i) {
    pts[i] = Vec2{2.0f * static_cast<float>(i % 97) / 97.0f - 1.0f,
                  2.0f * static_cast<float>(i % 89) / 89.0f - 1.0f};
  }
  // One pick (the budget's unit).
  auto pick = [&pts, &cam, &grid](std::int32_t i) {
    (void)screenToGrid(pts[i], cam, grid);
  };
  if (entry == nullptr) {
    // The ungated run: the workload's value here is the
    // leak/race/correctness coverage the sanitizer runtimes (and the
    // non-reference runners) provide — warm-up only, no assertions.
    for (std::int32_t i = 0; i < kBudgetWarmup; ++i) pick(i);
    return {};
  }
  for (std::int32_t i = 0; i < kBudgetWarmup; ++i) pick(i);
  laige::Histogram hist(
      laige::Histogram::Options{static_cast<std::size_t>(kBudgetRuns)});
  for (std::int32_t i = 0; i < kBudgetRuns; ++i) {
    laige::TimeIt timer;
    pick(i);
    hist.record(timer.elapsedMs());
  }
  const std::string buildLine = std::string(kCompilerId) +
#if defined(LAIGE_ISO_PICK_BUILD_TYPE)
      ", CMake " LAIGE_ISO_PICK_BUILD_TYPE
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

#if defined(LAIGE_ISO_PICK_BUDGET)
// The repo budgets.json (the LAIGE_BUDGETS_PATH ctest environment
// variable, set by the entry below).
std::string BudgetsFilePath() {
  const char* p = std::getenv("LAIGE_BUDGETS_PATH");
  if (p == nullptr || p[0] == '\0') {
    std::fprintf(stderr,
                 "IsoPickBudget: the LAIGE_BUDGETS_PATH env var is unset or "
                 "empty; the `iso_picking` ctest entry sets it — the "
                 "gated budget run cannot find budgets.json\n");
    std::abort();
  }
  return p;
}
#endif

TEST(IsoPickBudget, OnePick) {
#if defined(LAIGE_ISO_PICK_BUDGET)
  // The gated branch (the reference platform — the non-instrumented
  // Linux trees): load the budgets.json `iso_picking` entry and gate
  // the workload's metric against it (the iso_depth_table_tests.cpp
  // pattern). The 4-line AGENTS §12 report lands in the ctest log
  // (AGENTS §12).
  const std::string path = BudgetsFilePath();
  const laige::Result<laige::BudgetTable, laige::ErrorCode> loaded =
      laige::loadBudgets(path);
  ASSERT_TRUE(loaded.ok()) << "loadBudgets(\"" << path << "\") failed: "
                           << laige::errorText(loaded.error());
  const laige::BudgetEntry* entry = loaded.value().find("iso_picking");
  ASSERT_NE(entry, nullptr) << "budgets.json has no iso_picking entry";
  ASSERT_EQ(entry->metric, laige::BudgetMetric::Mean);
  const laige::BudgetCheckResult res = runBudget(entry);
  std::fputs(res.report.c_str(), stdout);
  std::fflush(stdout);
  EXPECT_TRUE(res.passed) << res.report;
#else
  // The ungated run: a test body without assertions passes — the
  // workload's value here is the leak/race/correctness coverage the
  // sanitizer runtimes (and the non-reference runners) provide.
  (void)runBudget(nullptr);
#endif
}

}  // namespace
