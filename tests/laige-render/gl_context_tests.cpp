// laige-render tests (M2-GL-01): the GL context contract.
//
// Suite map (the `gl_context` CTest entry selects exactly these):
//   GlContextGate   the pure 3.3 core gate (no GL required) — the
//                   "missing GL 3.3 -> clean error" decision;
//   GlContextArgs   creation argument validation (no GL required —
//                   validation happens before any GL call) and the
//                   stopped-context state;
//   GlContextSmoke  the offscreen render smoke (requires a usable
//                   OpenGL 3.3 environment — always present on the P0
//                   CI runners, where it must pass; on a local machine
//                   without a GL driver the suite GTEST_SKIPs with the
//                   clean Status reason, which is the documented
//                   environment contract, not an engine failure).

#include <cstdint>
#include <iostream>
#include <utility>

#include <gtest/gtest.h>

#include "laige/render/gl_context.h"

using laige::ErrorCode;
using laige::Result;
using laige::Status;
using laige::render::GlContext;
using laige::render::kMaxContextDimension;

namespace {

// The documented RGBA8 encoding of the smoke-test clear color
// (GL 3.3 §8.3: the clear color is converted to the buffer format —
// 1.0 -> 255, 0.25 -> 63.75 -> 64, 0.75 -> 191.25 -> 191, 1.0 -> 255).
const std::uint8_t kExpectedPixel[4] = {255, 64, 191, 255};

}  // namespace

// ------------------------------------------------------------------------
// GlContextGate — the pure 3.3 core requirement (PRD §6)
// ------------------------------------------------------------------------

TEST(GlContextGate, Accepts_3_3_Core) {
  EXPECT_TRUE(laige::render::checkGlVersion(3, 3, /*coreProfile=*/true).ok());
}

TEST(GlContextGate, Accepts_4_6_Core) {
  EXPECT_TRUE(laige::render::checkGlVersion(4, 6, /*coreProfile=*/true).ok());
}

TEST(GlContextGate, Rejects_3_2_Core) {
  const Status s =
      laige::render::checkGlVersion(3, 2, /*coreProfile=*/true);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), ErrorCode::GlVersionUnsupported);
}

TEST(GlContextGate, Rejects_3_3_Compat) {
  const Status s =
      laige::render::checkGlVersion(3, 3, /*coreProfile=*/false);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), ErrorCode::GlVersionUnsupported);
}

TEST(GlContextGate, Rejects_4_0_Compat) {
  const Status s =
      laige::render::checkGlVersion(4, 0, /*coreProfile=*/false);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), ErrorCode::GlVersionUnsupported);
}

TEST(GlContextGate, Rejects_2_1_Core) {
  const Status s =
      laige::render::checkGlVersion(2, 1, /*coreProfile=*/true);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), ErrorCode::GlVersionUnsupported);
}

TEST(GlContextGate, Rejects_Zero_Zero) {
  const Status s =
      laige::render::checkGlVersion(0, 0, /*coreProfile=*/false);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), ErrorCode::GlVersionUnsupported);
}

// ------------------------------------------------------------------------
// GlContextArgs — boundary validation before any GL call (API-008)
// ------------------------------------------------------------------------

TEST(GlContextArgs, Headless_RejectsZeroWidth) {
  const Result<GlContext> r = GlContext::createHeadless(0, 16);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
}

TEST(GlContextArgs, Headless_RejectsZeroHeight) {
  const Result<GlContext> r = GlContext::createHeadless(16, 0);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
}

TEST(GlContextArgs, Headless_RejectsOversizedWidth) {
  const Result<GlContext> r =
      GlContext::createHeadless(kMaxContextDimension + 1, 16);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
}

TEST(GlContextArgs, Headless_RejectsOversizedHeight) {
  const Result<GlContext> r =
      GlContext::createHeadless(16, kMaxContextDimension + 1);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
}

TEST(GlContextArgs, Windowed_RejectsZeroWidth) {
  const Result<GlContext> r = GlContext::createWindowed(0, 16, "laige");
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
}

TEST(GlContextArgs, Windowed_RejectsNullTitle) {
  const Result<GlContext> r = GlContext::createWindowed(16, 16, nullptr);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
}

TEST(GlContextArgs, Windowed_RejectsEmptyTitle) {
  const Result<GlContext> r = GlContext::createWindowed(16, 16, "");
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), ErrorCode::InvalidArgument);
}

TEST(GlContextArgs, StoppedContext_OperationsFail) {
  // The default-constructed state is stopped (moved-from equivalent):
  // every operation fails with InvalidArgument — no engine failure, no
  // log, no GL call.
  GlContext stopped;
  EXPECT_FALSE(stopped.valid());
  EXPECT_EQ(stopped.makeCurrent().error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(stopped.clear(0.0f, 0.0f, 0.0f, 0.0f).error(),
            ErrorCode::InvalidArgument);
  std::uint8_t px[4] = {0, 0, 0, 0};
  EXPECT_EQ(stopped.readPixel(0, 0, px).error(), ErrorCode::InvalidArgument);
}

// ------------------------------------------------------------------------
// GlContextSmoke — the offscreen render smoke (GL required)
// ------------------------------------------------------------------------

TEST(GlContextSmoke, Headless_OffscreenFrame) {
  // The smoke needs a usable OpenGL 3.3 environment — always present on
  // the P0 CI runners, where this test must pass; on a local machine
  // without a GL driver it is a documented environment skip, not an
  // engine failure.
  Result<GlContext> r = GlContext::createHeadless(32, 32);
  if (r.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment here: "
                 << laige::errorText(r.error());
  }
  GlContext ctx = std::move(r).takeValue();
  ASSERT_TRUE(ctx.valid());

  // The 3.3 core gate + the capability query (PRD §6).
  const laige::render::GlVersion v = ctx.version();
  EXPECT_TRUE(v.major > 3 || (v.major == 3 && v.minor >= 3));
  const laige::render::GlCapabilities caps = ctx.capabilities();
  EXPECT_TRUE(caps.coreProfile);
  EXPECT_EQ(caps.version.major, v.major);
  EXPECT_EQ(caps.version.minor, v.minor);
  // GL 3.3 core guarantees maxTextureSize >= 4096.
  EXPECT_GE(caps.maxTextureSize, 4096);
  EXPECT_NE(caps.vendor[0], '\0');
  EXPECT_NE(caps.renderer[0], '\0');
  EXPECT_EQ(ctx.width(), 32);
  EXPECT_EQ(ctx.height(), 32);

  // The creating thread owns the context: a frame is clearable without an
  // explicit makeCurrent (the render thread takes over later, M2-GL-02).
  EXPECT_TRUE(ctx.clear(1.0f, 0.25f, 0.75f, 1.0f).ok());
  EXPECT_TRUE(ctx.makeCurrent().ok());  // idempotent on the current thread

  // The offscreen FBO round trip: the cleared color comes back exactly
  // (kExpectedPixel — the RGBA8 encoding of the clear color).
  std::uint8_t px[4] = {0, 0, 0, 0};
  ASSERT_TRUE(ctx.readPixel(0, 0, px).ok());
  EXPECT_EQ(px[0], kExpectedPixel[0]);
  EXPECT_EQ(px[1], kExpectedPixel[1]);
  EXPECT_EQ(px[2], kExpectedPixel[2]);
  EXPECT_EQ(px[3], kExpectedPixel[3]);

  // Out-of-bounds reads are a clean InvalidArgument.
  EXPECT_EQ(ctx.readPixel(-1, 0, px).error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(ctx.readPixel(32, 0, px).error(), ErrorCode::InvalidArgument);
  EXPECT_EQ(ctx.readPixel(0, 32, px).error(), ErrorCode::InvalidArgument);

  // Machine-greppable smoke line (the docs/testing.md convention; the
  // exact format is in docs/api/gl_context.md).
  std::cout << "gl-smoke: kind=headless size=32x32 gl=" << v.major << "."
            << v.minor << " core=" << (caps.coreProfile ? 1 : 0)
            << " max_texture_size=" << caps.maxTextureSize
            << " clear_readback=ok\n";
}

TEST(GlContextSmoke, Headless_MoveStopsSource) {
  Result<GlContext> r = GlContext::createHeadless(8, 8);
  if (r.isError()) {
    GTEST_SKIP() << "no usable OpenGL 3.3 environment here: "
                 << laige::errorText(r.error());
  }
  GlContext ctx = std::move(r).takeValue();
  GlContext moved(std::move(ctx));
  EXPECT_FALSE(ctx.valid());  // the source is stopped
  EXPECT_TRUE(moved.valid());
  EXPECT_EQ(moved.width(), 8);
  EXPECT_EQ(moved.height(), 8);
  EXPECT_TRUE(moved.makeCurrent().ok());
  std::uint8_t px[4] = {0, 0, 0, 0};
  EXPECT_TRUE(moved.readPixel(0, 0, px).ok());
}

TEST(GlContextSmoke, Windowed_CleanOutcomeOnHeadlessHost) {
  // The windowed path needs a display. On a headless host (the P0 CI
  // Linux runner) it must fail with a clean Status — never crash, never
  // silently fall back. On a host with a display it succeeds and passes
  // the same 3.3 core gate.
  Result<GlContext> r = GlContext::createWindowed(16, 16, "laige test");
  if (r.isError()) {
    const ErrorCode code = r.error();
    EXPECT_TRUE(code == ErrorCode::GlUnavailable ||
                code == ErrorCode::GlVersionUnsupported)
        << "windowed creation failed with an unexpected code: "
        << laige::errorText(code);
    EXPECT_NE(laige::errorText(code), nullptr);
    return;
  }
  GlContext ctx = std::move(r).takeValue();
  EXPECT_TRUE(ctx.valid());
  const laige::render::GlVersion v = ctx.version();
  EXPECT_TRUE(v.major > 3 || (v.major == 3 && v.minor >= 3));
  EXPECT_TRUE(ctx.capabilities().coreProfile);
  EXPECT_EQ(ctx.width(), 16);
  EXPECT_EQ(ctx.height(), 16);
}
