// laige-golden (M2-GOLD-01): the frame comparison core of the golden
// image test infrastructure.
//
// TEST-008 (rendering changes require image/golden tests where stable,
// plus explicit tolerances): this header is the pure, GL-free
// comparison that both consumers share — the `laige-golden` tool
// (tools/golden, the ctest entry `golden`) and the unit suite
// (`GoldenCompare*`, the ctest entry `golden_compare`).
//
// The model: two frames of equal size (the canonical golden frame is
// 1280x720, top-origin / y-down — the screen-px convention of the UI
// pass; the capture side flips the GL bottom-left origin itself, see
// the tool) are compared per pixel. A PIXEL FAILS when any of the
// four channels differs by more than the tolerance's maxChannelDiff
// (inclusive). The FRAME FAILS when the count of failing pixels
// exceeds the budget: floor(total_pixels * pixelBudgetPct / 100).
// The defaults (maxChannelDiff = 2, pixelBudgetPct = 0.001 — 9 of
// 921 600 pixels on the canonical frame) are the documented
// tolerance of M2-GOLD-01; they are explicit in the ctest entry's
// flags and in docs/testing.md, never hidden. A driver or engine
// change that breaks the tolerance is a golden failure: the
// reference is regenerated through the documented human-approved
// path (the golden-update workflow), never by widening the tolerance
// silently.
//
// This header is test-only infrastructure: it is never included from
// src/ (the include-graph lint covers src/** only) and defines no
// symbols of its own. It is compiled with the NFR-8.10 policy like
// every test TU.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace laige::testing::golden {

// The documented golden tolerance (M2-GOLD-01 scope; the ctest entry
// `golden` passes the same numbers explicitly — they are config, not
// a hidden constant). maxChannelDiff: the per-channel absolute byte
// difference a pixel may have and still pass (inclusive, on the 0..255
// scale). pixelBudgetPct: the percentage of the frame's pixels
// allowed to fail before the frame fails (floor of the product).
struct Tolerance {
  std::int32_t maxChannelDiff{2};
  double pixelBudgetPct{0.001};
};

// One failing pixel (top-origin / y-down coordinates, matching the
// frame convention above).
struct FailingPixel {
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint8_t expected[4]{};
  std::uint8_t actual[4]{};
  std::int32_t maxDiff{0};  // the max over the four channels
};

// The comparison result. `failingPixels` is exact (every failing pixel
// is counted); `samples` carries the FIRST `maxSamples` failing pixels
// in scan order (the tool prints them — the roadmap's "enumerate
// failing pixels").
struct Comparison {
  std::uint64_t totalPixels{0};
  std::uint64_t failingPixels{0};
  std::uint64_t budget{0};  // the max failing pixels the frame may have
  std::int32_t maxObservedDiff{0};  // over all pixels, all channels
  std::vector<FailingPixel> samples;

  // The frame passes when the failing pixels stay within the budget.
  [[nodiscard]] bool pass() const noexcept {
    return failingPixels <= budget;
  }
};

// Compares two equal-sized RGBA8 frames (top-origin / y-down). Both
// spans must hold width * height * 4 bytes — the caller's precondition
// (a size mismatch is a caller bug, not an input error: UB by contract,
// like every span pair in this codebase's value types). O(w * h)
// reads, one `samples` allocation bounded by `maxSamples`.
[[nodiscard]] inline Comparison compareFrames(
    std::span<const std::uint8_t> expected,
    std::span<const std::uint8_t> actual, std::uint32_t width,
    std::uint32_t height, const Tolerance& tolerance,
    std::size_t maxSamples = 32) {
  Comparison c;
  const std::size_t pixels =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  c.totalPixels = static_cast<std::uint64_t>(pixels);
  c.budget =
      static_cast<std::uint64_t>(static_cast<double>(pixels) *
                                 tolerance.pixelBudgetPct / 100.0);
  for (std::size_t p = 0; p < pixels; ++p) {
    const std::uint8_t* e = expected.data() + p * 4u;
    const std::uint8_t* a = actual.data() + p * 4u;
    std::int32_t maxDiff = 0;
    for (std::size_t ch = 0; ch < 4u; ++ch) {
      const std::int32_t d = std::abs(static_cast<std::int32_t>(e[ch]) -
                                      static_cast<std::int32_t>(a[ch]));
      if (d > maxDiff) {
        maxDiff = d;
      }
    }
    if (maxDiff > c.maxObservedDiff) {
      c.maxObservedDiff = maxDiff;
    }
    if (maxDiff > tolerance.maxChannelDiff) {
      ++c.failingPixels;
      if (c.samples.size() < maxSamples) {
        FailingPixel fp;
        fp.x = static_cast<std::int32_t>(p % static_cast<std::size_t>(width));
        fp.y = static_cast<std::int32_t>(p / static_cast<std::size_t>(width));
        for (std::size_t ch = 0; ch < 4u; ++ch) {
          fp.expected[ch] = e[ch];
          fp.actual[ch] = a[ch];
        }
        fp.maxDiff = maxDiff;
        c.samples.push_back(fp);
      }
    }
  }
  return c;
}

}  // namespace laige::testing::golden
