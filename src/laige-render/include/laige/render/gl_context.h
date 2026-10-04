// laige-render GL context (M2-GL-01).
//
// PRD §6: one renderer, OpenGL 3.3 core profile on the desktop P0
// platforms. GlContext owns one 3.3 core context and its render target:
//
//   - windowed (createWindowed): a GLFW window (PRD §11) — the window's
//     frame buffer is the render target (swap/present lands with
//     M2-GL-02);
//   - headless/offscreen (createHeadless): no display required — the
//     documented CI mechanism, an offscreen FBO (RGBA8) on
//     * Linux:     an EGL surfaceless context (libEGL loaded at runtime,
//     * Windows:   a never-shown GLFW window,
//     * macOS:     a never-shown GLFW window
//     see docs/api/gl_context.md for the exact per-platform mechanism.
//
// The 3.3 core gate: a driver that cannot deliver OpenGL 3.3 core is
// rejected with Status(ErrorCode::GlVersionUnsupported) — there is no
// silent fallback to an older version or the compatibility profile
// (PRD §6, CORE-008). Every other creation failure returns
// Status(ErrorCode::GlUnavailable).
//
// Ownership/lifetime: move-only. The default-constructed state is
// stopped (moved-from) — every operation on it fails (the
// `Engine`/`GameLoop` moved-out precedent). The destructor destroys the
// native context (GLFW window or EGL display/context); GPU objects
// created on the context die with it.
//
// Threading (CONC-001): one thread is current on a context at a time.
// The creating thread is the default owner; the render thread takes
// ownership with makeCurrent() (the frame pipeline, M2-GL-02). The
// handoff is RELEASE-THEN-BIND: the old owner calls release() on its
// thread first, then the new thread makes the context current — on
// the P0 EGL stack, take-overs while the context is still current on
// another LIVE thread fail with EGL_BAD_ACCESS (see
// docs/api/gl_context.md, Threading and phase).
// createWindowed/createHeadless are single-threaded startup calls — the
// engine creates its contexts before the frame pipeline starts.
//
// Performance (PERF-003/002): creation is one-time setup work (native
// window/EGL setup, one GLAD symbol-resolution pass over the 3.3 core
// entry points, one FBO + texture allocation); it never runs inside a
// frame. clear()/readPixel() are O(1), allocation-free, one
// glBindFramebuffer each. The per-frame render hot path (the sprite
// batcher, M2-SPRITE-02) does not go through these functions.
//
// Misuse warnings:
//   - do not destroy a context that is current on another thread (the
//     owner must makeCurrent() it first — M2-GL-02 formalizes this);
//   - clear()/readPixel() require the context to be current on the
//     calling thread (makeCurrent()); otherwise they fail with
//     ErrorCode::InvalidArgument (a precondition violation, not an
//     engine failure);
//   - width/height must be within [1, kMaxContextDimension] — larger
//     targets are a configuration error (API-008), not a valid request.
//   - vendor/renderer strings in GlCapabilities are driver-supplied:
//     they are bounded and NUL-terminated, but treat them as untrusted
//     text when they leave the engine (LOG-005).

#pragma once

#include <cstdint>
#include <memory>

#include "laige/result.h"

namespace laige::render {

// A GL version (the major.minor of GL_VERSION).
struct GlVersion {
  std::int32_t major;
  std::int32_t minor;
};

// Capability snapshot queried once, right after context creation (the
// PRD §6 "capability query"). Read-only after creation (published
// state — consumers read it, never take locks; DBG-005).
struct GlCapabilities {
  GlVersion version;          // GL_VERSION (gated: >= 3.3 core)
  bool coreProfile;           // GL_PROFILE reports the core profile
  std::int32_t maxTextureSize;  // GL_MAX_TEXTURE_SIZE (atlas ceiling,
                                // M2-TILE-01/M2-TEXT-01 plan against it)
  char vendor[128];           // GL_VENDOR, clamped, NUL-terminated
  char renderer[128];         // GL_RENDERER, clamped, NUL-terminated
};

// The engine's minimum: OpenGL 3.3 core (PRD §6). Named constants — the
// gate below and the context-creation hints both use them, so the
// requirement lives in one place (CORE-005).
constexpr std::int32_t kGlMajor = 3;
constexpr std::int32_t kGlMinor = 3;

// Documented dimension bound for created render targets (API-008):
// 16384x16384x4 bytes = 1 GiB of RGBA8 offscreen storage — a driver's
// maxTextureSize (queried after creation) is the real ceiling, and no
// P0 desktop configuration drives a per-context target above this.
constexpr std::int32_t kMaxContextDimension = 16384;

// Pure gate: ok iff (major, minor, coreProfile) satisfies the engine's
// OpenGL 3.3 core requirement (kGlMajor.kGlMinor, core profile).
// createWindowed/createHeadless apply it to the driver-reported values;
// it is public so the requirement is unit-testable without a GL context
// (the M2-GL-01 "missing GL 3.3 -> clean error" test).
[[nodiscard]] Status checkGlVersion(std::int32_t major, std::int32_t minor,
                                    bool coreProfile);

// One OpenGL 3.3 core context plus its render target (the window frame
// buffer, or the offscreen FBO). See the file preamble for the full
// contract; docs/api/gl_context.md is the normative API document.
class GlContext {
 public:
  // A default-constructed context is stopped — every operation fails
  // (no window, no context). The valid() check is the public query.
  // (Defined in the .cpp: the pimpl member needs Impl complete.)
  GlContext() noexcept;
  GlContext(const GlContext&) = delete;
  GlContext& operator=(const GlContext&) = delete;
  GlContext(GlContext&&) noexcept;
  GlContext& operator=(GlContext&&) noexcept;
  ~GlContext();

  // A GLFW window of width x height pixels with the given title, plus a
  // 3.3 core context on it (PRD §11 windowing). title must be a
  // non-empty NUL-terminated string (ErrorCode::InvalidArgument
  // otherwise). Width/height must be within [1, kMaxContextDimension].
  //
  // Cost: one-time setup (window + context + GLAD load + capability
  // query). Failures: GlUnavailable (no windowing backend, context
  // creation failed, the GL library cannot be loaded) or
  // GlVersionUnsupported (the driver's context is not 3.3 core) — never
  // a fallback profile.
  [[nodiscard]] static Result<GlContext> createWindowed(
      std::int32_t width, std::int32_t height, const char* title);

  // A display-less offscreen context of width x height pixels: on Linux
  // an EGL surfaceless context, on Windows/macOS a never-shown GLFW
  // window (docs/api/gl_context.md). The render target is an offscreen
  // FBO (RGBA8, width x height) owned by the context.
  //
  // Cost/failure: as createWindowed (GlUnavailable additionally covers
  // the EGL surfaceless platform being unavailable and the FBO
  // failing its completeness check).
  [[nodiscard]] static Result<GlContext> createHeadless(
      std::int32_t width, std::int32_t height);

  // False when stopped (default-constructed or moved-from).
  [[nodiscard]] bool valid() const noexcept;

  // The version the driver realized (the gate is >= 3.3 core).
  // Precondition: valid().
  [[nodiscard]] const GlVersion& version() const noexcept;

  // The creation-time capability snapshot. Precondition: valid().
  [[nodiscard]] const GlCapabilities& capabilities() const noexcept;

  // The render-target size (the window, or the FBO).
  // Precondition: valid().
  [[nodiscard]] std::int32_t width() const noexcept;
  [[nodiscard]] std::int32_t height() const noexcept;

  // The render-target frame buffer: the offscreen FBO on headless
  // contexts, 0 (the default frame buffer — the window) on windowed
  // ones. The per-frame draw path binds it once per frame (the
  // class's clear()/readPixel() bind it per call); a frame-pass
  // (M2-SPRITE-02) binds it at the frame's start. No GL call (a
  // creation-time handle); O(1). 0 when stopped.
  [[nodiscard]] std::uint32_t frameBuffer() const noexcept;

  // Bind this context to the calling thread (one thread current at a
  // time, CONC-001). The M2-GL-02 render thread calls this on takeover
  // AFTER the old owner has called release() (the P0 EGL stack rejects
  // a takeover while the context is still current on another live
  // thread); re-calling on the current thread is a no-op success.
  // Precondition: valid(). Failure: GlUnavailable.
  [[nodiscard]] Status makeCurrent() const;

  // Unbind this context from the calling thread (the context stays
  // valid and is no longer current on ANY thread). The first step of
  // a cross-thread handoff — release here, makeCurrent on the new
  // thread (the class preamble's handoff protocol). A no-op success
  // when the calling thread holds no context. Not a hot path (a
  // handoff/setup call, never per-frame).
  // Precondition: valid(). Failure: GlUnavailable.
  [[nodiscard]] Status release() const;

  // Clear the render target (the FBO on headless contexts, the window
  // frame buffer on windowed contexts) to an RGBA color; the float
  // components are clamped to [0, 1] before conversion to RGBA8. The
  // minimal frame primitive — the sprite draw path lands in
  // M2-SPRITE-02. O(1), no allocation. Precondition: valid(); the
  // context must be current on the calling thread (InvalidArgument
  // otherwise — a precondition violation, not an engine failure).
  [[nodiscard]] Status clear(float r, float g, float b, float a) const;

  // Read one RGBA8 pixel from the render target; (0, 0) is bottom-left
  // (the GL convention). O(1), no allocation. Precondition: valid();
  // the context must be current on the calling thread; (x, y) within
  // [0, width) x [0, height) (InvalidArgument otherwise).
  [[nodiscard]] Status readPixel(std::int32_t x, std::int32_t y,
                                 std::uint8_t rgba[4]) const;

  // The windowed context's display refresh rate (Hz); 0 when
  // unavailable (a headless context — the FBO is the render target and
  // nothing is ever presented — or a monitor/video-mode query failure).
  // No GL call (a GLFW window query): the context need not be current
  // on the calling thread. The M2-GL-02 frame clock uses this as the
  // vsync pace (docs/api/frame_pipeline.md); 0 → the caller's target
  // rate stands in. Precondition: valid(); O(1), no allocation.
  [[nodiscard]] std::uint32_t refreshRateHz() const noexcept;

 private:
  struct Impl;
  // Construction goes through createWindowed/createHeadless only; the
  // native context (and, headless, the FBO) is born owned.
  explicit GlContext(std::unique_ptr<Impl>) noexcept;

  // Creation-path helpers (impl detail; see gl_context.cpp).
  static bool isCurrentOnThisThread(const Impl* impl);
  static Status queryCapabilities(Impl* impl);
  static Status createFbo(Impl* impl);
  static Status finishCreation(Impl* impl, const char* kind, bool headless);

  std::unique_ptr<Impl> impl_;  // empty <=> stopped
};

}  // namespace laige::render
