// laige-render atlas frame tests (M2-SPRITE-03): the atlas UV frame
// animation hook in laige/render/sprite_frames.h.
//
// Pure float/integer math — no GL context, no GL environment needed:
// every suite runs in every local tree and in CI. The golden suite
// pins the roadmap's "frame index → UV rect exact for a documented
// atlas layout"; the errors suite pins the documented failure
// contract (out-of-range frame → InvalidArgument, never wrap; layout
// validation first-failure-wins); the property suite pins the sheet
// model against the documented formula + the float-exact domain; the
// pass-through suite pins the SpriteItem.frameIndex hook through the
// batcher (the M3 animation's data-driven entry point).
//
// Seed: the repo-wide documented default seed via
// tests/support/laige_test_seed.h (docs/testing.md §4), one named
// substream per randomized suite.

#include "laige/render/sprite_frames.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "gtest/gtest.h"
#include "laige/alloc_watch.h"
#include "laige/errors.h"
#include "laige/prng.h"
#include "laige/result.h"
#include "laige_test_seed.h"

#include "laige/render/sprite_batcher.h"

namespace {

using laige::render::SpriteBatcher;
using SpriteBatcherOptions = laige::render::SpriteBatcher::Options;
using laige::render::SpriteFrameLayout;
using laige::render::SpriteItem;
using laige::render::SpriteUvRect;
using laige::render::kSpriteFrameMaxAtlasTexels;
using laige::render::spriteFrameUv;

// The randomized suite's substream id (docs/testing.md §4 — stable
// named constants, CORE-005).
constexpr std::uint32_t kPropertySubstreamId = 0x5346524D;  // "SFRM"

// The documented sheet formula, recomputed independently of the
// implementation's expression (the property suite's oracle): frame i's
// texel rect under the layout.
struct PixelRect {
  std::uint32_t x0{};
  std::uint32_t y0{};
  std::uint32_t x1{};
  std::uint32_t y1{};
};

PixelRect framePixelRect(std::uint32_t frameIndex,
                         const SpriteFrameLayout& l) {
  const std::uint32_t col = frameIndex % l.columns;
  const std::uint32_t row = frameIndex / l.columns;
  const std::uint32_t x0 =
      l.sheetBorder + col * (l.frameWidth + l.frameSpacing);
  const std::uint32_t y0 =
      l.sheetBorder + row * (l.frameHeight + l.frameSpacing);
  return PixelRect{x0, y0, x0 + l.frameWidth, y0 + l.frameHeight};
}

// The UV rect the documented formula yields (the oracle's float step —
// the same single correctly-rounded division per corner).
SpriteUvRect expectUv(std::uint32_t frameIndex, const SpriteFrameLayout& l,
                      std::uint32_t w, std::uint32_t h) {
  const PixelRect r = framePixelRect(frameIndex, l);
  return SpriteUvRect{
      static_cast<float>(r.x0) / static_cast<float>(w),
      static_cast<float>(r.y0) / static_cast<float>(h),
      static_cast<float>(r.x1) / static_cast<float>(w),
      static_cast<float>(r.y1) / static_cast<float>(h)};
}

void expectUvExact(const char* what, std::uint32_t frameIndex,
                   const SpriteFrameLayout& l, std::uint32_t w,
                   std::uint32_t h, SpriteUvRect expected) {
  auto uv = spriteFrameUv(frameIndex, l, w, h);
  ASSERT_TRUE(uv.ok()) << what << ": " << frameIndex << " failed: "
                       << laige::errorText(uv.error());
  const SpriteUvRect got = std::move(uv).takeValue();
  EXPECT_EQ(got.u0, expected.u0) << what;
  EXPECT_EQ(got.v0, expected.v0) << what;
  EXPECT_EQ(got.u1, expected.u1) << what;
  EXPECT_EQ(got.v1, expected.v1) << what;
}

void expectInvalid(const char* what, std::uint32_t frameIndex,
                   const SpriteFrameLayout& l, std::uint32_t w,
                   std::uint32_t h) {
  const auto uv = spriteFrameUv(frameIndex, l, w, h);
  ASSERT_FALSE(uv.ok()) << what;
  EXPECT_EQ(uv.error(), laige::ErrorCode::InvalidArgument) << what;
}

}  // namespace

// ---------------------------------------------------------------------------
// SpriteFrameUvGolden — hand-computed UV rects for documented layouts
// (the roadmap's "frame index → UV rect exact for a documented atlas
// layout").
// ---------------------------------------------------------------------------

TEST(SpriteFrameUvGolden, PlainSheetNoMargins) {
  // 128x128 atlas, 16x16 frames, 4x4 grid, packed (the tight sheet is
  // 64x64 — the atlas is wider/taller than the tight sheet, the
  // slack is margin).
  const SpriteFrameLayout l{16, 16, 4, 4, 0, 0};
  expectUvExact("plain", 0, l, 128, 128,
                SpriteUvRect{0.0f, 0.0f, 16.0f / 128.0f, 16.0f / 128.0f});
  expectUvExact("plain", 3, l, 128, 128,
                SpriteUvRect{48.0f / 128.0f, 0.0f, 64.0f / 128.0f,
                             16.0f / 128.0f});
  expectUvExact("plain", 5, l, 128, 128,
                SpriteUvRect{16.0f / 128.0f, 16.0f / 128.0f, 32.0f / 128.0f,
                             32.0f / 128.0f});
  expectUvExact("plain", 15, l, 128, 128,
                SpriteUvRect{48.0f / 128.0f, 48.0f / 128.0f, 64.0f / 128.0f,
                             64.0f / 128.0f});
}

TEST(SpriteFrameUvGolden, TightSheetWithMargins) {
  // 82x82 atlas, 32x32 frames, 2x2 grid, spacing 2, border 8 — the
  // tight sheet: 2*8 + 2*32 + 1*2 = 82. Frame (c, r): origin
  // (8 + 34c, 8 + 34r).
  const SpriteFrameLayout l{32, 32, 2, 2, 2, 8};
  expectUvExact("margins", 0, l, 82, 82,
                SpriteUvRect{8.0f / 82.0f, 8.0f / 82.0f, 40.0f / 82.0f,
                             40.0f / 82.0f});
  expectUvExact("margins", 1, l, 82, 82,
                SpriteUvRect{42.0f / 82.0f, 8.0f / 82.0f, 74.0f / 82.0f,
                             40.0f / 82.0f});
  expectUvExact("margins", 2, l, 82, 82,
                SpriteUvRect{8.0f / 82.0f, 42.0f / 82.0f, 40.0f / 82.0f,
                             74.0f / 82.0f});
  expectUvExact("margins", 3, l, 82, 82,
                SpriteUvRect{42.0f / 82.0f, 42.0f / 82.0f, 74.0f / 82.0f,
                             74.0f / 82.0f});
}

TEST(SpriteFrameUvGolden, NonSquareFramesAndRows) {
  // 44x24 atlas, 12x8 frames, 3 columns x 2 rows, spacing 1, border 2
  // (tight: 2*2 + 3*12 + 2*1 = 42 — 2 px slack horizontally).
  const SpriteFrameLayout l{12, 8, 3, 2, 1, 2};
  expectUvExact("nonsquare", 0, l, 44, 24,
                SpriteUvRect{2.0f / 44.0f, 2.0f / 24.0f, 14.0f / 44.0f,
                             10.0f / 24.0f});
  expectUvExact("nonsquare", 4, l, 44, 24,
                SpriteUvRect{15.0f / 44.0f, 11.0f / 24.0f, 27.0f / 44.0f,
                             19.0f / 24.0f});
  expectUvExact("nonsquare", 5, l, 44, 24,
                SpriteUvRect{28.0f / 44.0f, 11.0f / 24.0f, 40.0f / 44.0f,
                             19.0f / 24.0f});
}

TEST(SpriteFrameUvGolden, SingleColumnAndSingleRow) {
  // 8x48 atlas, 8x16 frames, 1 column x 3 rows, packed.
  const SpriteFrameLayout column{8, 16, 1, 3, 0, 0};
  expectUvExact("column", 0, column, 8, 48,
                SpriteUvRect{0.0f, 0.0f, 1.0f, 16.0f / 48.0f});
  expectUvExact("column", 2, column, 8, 48,
                SpriteUvRect{0.0f, 32.0f / 48.0f, 1.0f, 1.0f});
  // 64x8 atlas, 16x8 frames, 4 columns x 1 row, packed.
  const SpriteFrameLayout row{16, 8, 4, 1, 0, 0};
  expectUvExact("row", 2, row, 64, 8,
                SpriteUvRect{32.0f / 64.0f, 0.0f, 48.0f / 64.0f, 1.0f});
}

TEST(SpriteFrameUvGolden, DeterministicAndIdempotent) {
  // The pure function: the same call twice is bit-identical.
  const SpriteFrameLayout l{16, 16, 4, 4, 0, 0};
  const auto a = spriteFrameUv(5, l, 128, 128);
  const auto b = spriteFrameUv(5, l, 128, 128);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  const SpriteUvRect& ra = *a.valueIfOk();
  const SpriteUvRect& rb = *b.valueIfOk();
  EXPECT_EQ(ra.u0, rb.u0);
  EXPECT_EQ(ra.v0, rb.v0);
  EXPECT_EQ(ra.u1, rb.u1);
  EXPECT_EQ(ra.v1, rb.v1);
}

// ---------------------------------------------------------------------------
// SpriteFrameErrors — the documented failure contract (first failure
// wins; out-of-range frames fail, never wrap).
// ---------------------------------------------------------------------------

TEST(SpriteFrameErrors, OutOfRangeFrameNeverWraps) {
  const SpriteFrameLayout l{16, 16, 4, 4, 0, 0};  // 16 frames
  // Exactly at the count, beyond it, and at the u32 top:
  expectInvalid("out-of-range at count", 16, l, 128, 128);
  expectInvalid("out-of-range beyond", 17, l, 128, 128);
  expectInvalid("out-of-range u32 top", 0xFFFFFFFFu, l, 128, 128);
  // NO WRAP: frame 16 must NOT return frame 0's rect (the last frame
  // is frame 15; the in-range ends stay exact):
  expectUvExact("no-wrap frame 15", 15, l, 128, 128,
                SpriteUvRect{48.0f / 128.0f, 48.0f / 128.0f, 64.0f / 128.0f,
                             64.0f / 128.0f});
}

TEST(SpriteFrameErrors, ZeroExtentLayouts) {
  const std::uint32_t atlas = 128;
  expectInvalid("frameWidth 0", 0,
                SpriteFrameLayout{0, 16, 4, 4, 0, 0}, atlas, atlas);
  expectInvalid("frameHeight 0", 0,
                SpriteFrameLayout{16, 0, 4, 4, 0, 0}, atlas, atlas);
  expectInvalid("columns 0", 0, SpriteFrameLayout{16, 16, 0, 4, 0, 0}, atlas,
                atlas);
  expectInvalid("rows 0", 0, SpriteFrameLayout{16, 16, 4, 0, 0, 0}, atlas,
                atlas);
}

TEST(SpriteFrameErrors, AtlasDomain) {
  const SpriteFrameLayout l{1, 1, 1, 1, 0, 0};
  expectInvalid("atlasWidth 0", 0, l, 0, 1);
  expectInvalid("atlasHeight 0", 0, l, 1, 0);
  expectInvalid("atlasWidth above domain", 0, l, kSpriteFrameMaxAtlasTexels + 1,
                1);
  expectInvalid("atlasHeight above domain", 0, l, 1,
                kSpriteFrameMaxAtlasTexels + 1);
  // The domain top is exact (1x1 frame in a max-domain atlas):
  expectUvExact("domain top", 0, l, kSpriteFrameMaxAtlasTexels,
                kSpriteFrameMaxAtlasTexels,
                SpriteUvRect{0.0f, 0.0f,
                             1.0f / static_cast<float>(kSpriteFrameMaxAtlasTexels),
                             1.0f / static_cast<float>(kSpriteFrameMaxAtlasTexels)});
}

TEST(SpriteFrameErrors, FrameDoesNotFit) {
  // The border alone pushes the frame past the edge:
  expectInvalid("border too big", 0, SpriteFrameLayout{32, 32, 1, 1, 0, 40},
                64, 64);
  // The spacing pushes the second column past the edge — one texel
  // short, then exact at the boundary (the fit check accepts a frame
  // whose x1 == atlas width):
  expectInvalid("spacing too big", 1, SpriteFrameLayout{32, 32, 2, 1, 32, 0},
                95, 32);
  expectUvExact("spacing at boundary", 1, SpriteFrameLayout{32, 32, 2, 1,
                                                            32, 0},
                96, 32, SpriteUvRect{64.0f / 96.0f, 0.0f, 1.0f, 1.0f});
  // Adversarial stride beyond the atlas (the header's overflow guard:
  // frameWidth + frameSpacing exceeds the u32-friendly domain and the
  // col·stride product would be beyond the atlas long before the fit
  // check — rejected, not a wrapped origin):
  expectInvalid("adversarial stride", 1,
                SpriteFrameLayout{1, 1, 8, 2, 0xFFFFFFFFu, 0}, 4, 4);
}

// ---------------------------------------------------------------------------
// SpriteFrameProperty — the sheet model against the documented formula,
// the float-exact invariants, the adjacency rule, and the
// zero-allocation proof (the iso_picking / depth_sort precedent —
// non-sanitizer trees).
// ---------------------------------------------------------------------------

TEST(SpriteFrameProperty, LayoutModelAndZeroAlloc) {
  laige::Prng prng = laige::testing::TestPrng(kPropertySubstreamId);
  for (int i = 0; i < 2000; ++i) {
    const std::uint32_t fw = prng.next_range(1, 33);
    const std::uint32_t fh = prng.next_range(1, 33);
    const std::uint32_t columns = prng.next_range(1, 9);
    const std::uint32_t rows = prng.next_range(1, 9);
    const std::uint32_t spacing = prng.next_range(0, 9);
    const std::uint32_t border = prng.next_range(0, 17);
    // The tight sheet (the header's tight formula) — always fits its
    // own layout:
    const std::uint32_t w =
        2 * border + columns * fw + (columns - 1) * spacing;
    const std::uint32_t h =
        2 * border + rows * fh + (rows - 1) * spacing;
    const SpriteFrameLayout l{fw, fh, columns, rows, spacing, border};
    const std::uint32_t frameCount = columns * rows;
    const std::uint32_t frameIndex = prng.next_range(0, frameCount);
    auto uv = spriteFrameUv(frameIndex, l, w, h);
    ASSERT_TRUE(uv.ok()) << "i=" << i;
    const SpriteUvRect got = std::move(uv).takeValue();
    const SpriteUvRect expected = expectUv(frameIndex, l, w, h);
    EXPECT_EQ(got.u0, expected.u0) << "i=" << i;
    EXPECT_EQ(got.v0, expected.v0) << "i=" << i;
    EXPECT_EQ(got.u1, expected.u1) << "i=" << i;
    EXPECT_EQ(got.v1, expected.v1) << "i=" << i;
    // The float-exact invariants (the header's Exactness section):
    EXPECT_LT(got.u0, got.u1) << "i=" << i;
    EXPECT_LT(got.v0, got.v1) << "i=" << i;
    EXPECT_LE(got.u1, 1.0f) << "i=" << i;
    EXPECT_LE(got.v1, 1.0f) << "i=" << i;
    // The adjacency rule: the next frame in the row starts where this
    // frame's gap ends — exact touch when packed, a strict gap
    // otherwise. The frame after the last in the row is the next row
    // (checked on the row only).
    const std::uint32_t col = frameIndex % columns;
    if (col + 1 < columns) {
      const SpriteUvRect next = expectUv(frameIndex + 1, l, w, h);
      if (spacing == 0) {
        EXPECT_EQ(got.u1, next.u0) << "i=" << i;
      } else {
        EXPECT_LT(got.u1, next.u0) << "i=" << i;
      }
      EXPECT_EQ(got.v0, next.v0) << "i=" << i;
    }
    const std::uint32_t row = frameIndex / columns;
    if (row + 1 < rows) {
      const SpriteUvRect next = expectUv(frameIndex + columns, l, w, h);
      if (spacing == 0) {
        EXPECT_EQ(got.v1, next.v0) << "i=" << i;
      } else {
        EXPECT_LT(got.v1, next.v0) << "i=" << i;
      }
      EXPECT_EQ(got.u0, next.u0) << "i=" << i;
    }
  }

  // Zero-allocation proof (where the process-wide watch is live — the
  // non-sanitizer trees; the sanitizer runtimes own operator new, the
  // depth_sort test precedent): 1 000 consecutive conversions allocate
  // nothing — the function is fixed-size value traffic, structurally
  // zero-heap (PERF-003).
  const SpriteFrameLayout l{16, 16, 4, 4, 0, 0};
  if (laige::allocWatchLive()) {
    laige::allocWatchArm();
    for (int i = 0; i < 1000; ++i) {
      const auto uv = spriteFrameUv(static_cast<std::uint32_t>(i % 16), l, 128,
                                    128);
      if (!uv.ok()) {
        ADD_FAILURE() << "conversion " << i << " failed: "
                      << laige::errorText(uv.error());
        break;
      }
    }
    const laige::AllocWatchReading reading = laige::allocWatchRead();
    EXPECT_EQ(reading.allocs, 0u)
        << "1000 conversions allocated " << reading.allocs
        << " heap blocks (first site: "
        << reinterpret_cast<std::uintptr_t>(reading.firstSite) << ")";
  }
}

// ---------------------------------------------------------------------------
// SpriteFrameItemPassThrough — the SpriteItem.frameIndex hook through
// the batcher (the M3 animation's data-driven entry point: the caller
// sets frameIndex + uv, the batcher carries both untouched).
// ---------------------------------------------------------------------------

TEST(SpriteFrameItemPassThrough, ItemCarriesFrameIndexAndUv) {
  const SpriteFrameLayout l{16, 16, 4, 4, 0, 0};
  auto uv = spriteFrameUv(3, l, 128, 128);
  ASSERT_TRUE(uv.ok());
  const SpriteUvRect uvRect = std::move(uv).takeValue();
  SpriteItem item;
  item.pos = laige::render::Vec2{1.0f, 2.0f};
  item.depthKey = 42;
  item.uv = uvRect;
  item.frameIndex = 3;
  item.rotation = 0.25f;
  item.scale = laige::render::Vec2{1.0f, 1.0f};
  item.atlasId = 7;

  auto r = SpriteBatcher::create(SpriteBatcherOptions{8});
  ASSERT_TRUE(r.ok());
  SpriteBatcher batcher = std::move(r).takeValue();
  auto slot = batcher.add(item);
  ASSERT_TRUE(slot.ok());
  const auto build = batcher.build();
  ASSERT_TRUE(build.ok());
  const SpriteItem* got = batcher.get(std::move(slot).takeValue());
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->frameIndex, 3u);
  EXPECT_EQ(got->uv.u0, uvRect.u0);
  EXPECT_EQ(got->uv.v0, uvRect.v0);
  EXPECT_EQ(got->uv.u1, uvRect.u1);
  EXPECT_EQ(got->uv.v1, uvRect.v1);
  EXPECT_EQ(got->rotation, 0.25f);
}
