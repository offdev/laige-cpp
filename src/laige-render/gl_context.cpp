// laige-render GL context implementation (M2-GL-01).
//
// Platform mechanism (docs/api/gl_context.md, ADR 0007):
//   windowed (all P0 OSes): GLFW window + 3.3 core context (PRD §11 —
//     the engine does not reinvent windowing).
//   headless, Linux:        EGL surfaceless platform — an EGL display is
//     created for the EGL_MESA_platform_surfaceless platform and the
//     context is created with no config and no surface (EGL 1.5 +
//     the Mesa surfaceless platform; P0 CI runners provide it via
//     Mesa). libEGL is loaded at runtime (dlopen) so the engine has no
//     build-time EGL dependency (AC-6.1 "one source tree": the same
//     sources compile on a P0 machine without Mesa dev packages; a
//     runtime failure is a clean Status, not a build failure).
//   headless, Windows/     : a never-shown GLFW window (the GLFW_VISIBLE
//   macOS:                 = false hint) + 3.3 core context on it.
// In every case the render target is either the window frame buffer
// (windowed) or an offscreen RGBA8 FBO (headless).
//
// Failure contract: creation never falls back to an older GL version or
// the compatibility profile (PRD §6) — it returns GlUnavailable (no
// usable context: no windowing backend, EGL unavailable, context
// creation failed, the GL library cannot be loaded, the FBO failed its
// completeness check) or GlVersionUnsupported (a context exists but is
// not 3.3 core), plus one structured Error event per failure
// (gl/context_creation_failed, LOG-001/002).
//
// Include order: glad/gl.h BEFORE GLFW/glfw3.h. glad's header defines
// __gl_h_/__gl3_h_ (the guards glfw3.h checks), so glfw3.h skips its
// own GL-header include and the two headers never collide.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#if !defined(__APPLE__) && !defined(_WIN32)
#include <dlfcn.h>  // Linux EGL runtime load (the only platform include)
#endif

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include "laige/render/gl_context.h"
#include "laige/logging.h"

namespace laige::render {

namespace {

// ------------------------------------------------------------------------
// EGL declarations (Linux headless path).
//
// The engine dlopens libEGL and resolves the small function set below
// by name — no link-time or header-time dependency on EGL (see the
// file preamble). The values are the stable constants from the EGL 1.5
// spec and EGL_MESA_platform_surfaceless (verified against the system
// EGL headers on a P0 distro; API-stable, not driver-specific).
// ------------------------------------------------------------------------
namespace egl {

using Display = void*;
using Context = void*;
using Surface = void*;
using Config = void*;
using Int = std::int32_t;
using Bool = std::int32_t;
using Enum = std::int32_t;

// EGL_OPENGL_API — the eglBindAPI argument. Verified against the P0
// distro's EGL headers (noble libglvnd 1.7.0): the GLVND dispatcher
// accepts only EGL_OPENGL_API (0x30A2) and EGL_OPENGL_ES_API (0x30A0)
// and rejects every other value with EGL_BAD_PARAMETER (0x300C).
// Note the confusion this value caused: 0x0008 is EGL_OPENGL_BIT, a
// ClientAPIs mask bit, not an API enum. It is used from createHeadless
// (Linux only) AND from makeCurrent on every platform (the per-thread
// API selection below), so it lives outside the Linux guard.
constexpr Enum kOpenGlApi = 0x30A2;

// These remaining constants are referenced only from the Linux
// headless path (the #else createHeadless branch); without the guard,
// AppleClang's -Wunused-const-variable (-Werror, NFR-8.10) rejects
// them on macOS.
#if !defined(__APPLE__) && !defined(_WIN32)
constexpr Int kNone = 0x3038;
constexpr Int kContextMajorVersion = 0x3098;
constexpr Int kContextMinorVersion = 0x30FB;
constexpr Int kContextProfileMask = 0x30FD;
constexpr Int kCoreProfileBit = 0x00000001;
constexpr Enum kPlatformSurfacelessMesa = 0x31DD;  // EGL_MESA_platform_surfaceless
#endif

struct FnTable {
  Display (*getPlatformDisplay)(Enum platform, void* nativeDisplay,
                                const Int* attribs);
  Bool (*initialize)(Display display, Int* major, Int* minor);
  Bool (*bindApi)(Enum api);
  Context (*createContext)(Display display, Config config, Context share,
                           const Int* attribs);
  Bool (*makeCurrent)(Display display, Surface draw, Surface read,
                      Context context);
  // eglGetError — this thread's last EGL error (EGL 1.0 core; both the
  // libglvnd dispatcher and the vendor library export it): reported
  // with the make-current failure diagnostics (LOG-002).
  Int (*getError)();
  Context (*getCurrentContext)();
  Bool (*destroyContext)(Display display, Context context);
  Bool (*destroyDisplay)(Display display);
  // eglGetProcAddress — resolves GL entry points for the current
  // context (EGL 1.5). Required for the headless GLAD load below.
  void* (*getProcAddress)(const char* name);
};

}  // namespace egl

// GLAD's built-in loader (`gladLoaderLoadGL`) is GLX-flavored on Linux
// (it dlopens libGL.so.1 and resolves through glXGetProcAddressARB),
// which cannot serve an EGL surfaceless context: for the EGL backend
// the engine feeds GLAD its own userptr loader (gladLoadGLUserPtr)
// backed by the context's eglGetProcAddress — the EGL 1.5 mechanism for
// resolving a context's GL API. The GLFW backends keep GLAD's built-in
// loader (WGL/GLX/Cocoa), which is correct for their native interfaces.
// GLAD's userptr loader contract (gl.h): the loader takes a user
// pointer and a function name and returns GLADapiproc — a
// `void (*)(void)` function pointer.
void (*eglGladLoader(void* userptr, const char* name))(void) {
  const auto proc =
      *reinterpret_cast<void* (**)(const char*)>(userptr);
  if (proc == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<void (*)(void)>(proc(name));
}

// GL_CONTEXT_PROFILE_BIT (the core-profile bit of the
// GL_CONTEXT_PROFILE_MASK query): glad's core-3.3 generation emits the
// mask constant but not this bit; the value is stable in the OpenGL
// spec.
constexpr GLint kGlContextProfileBit = 0x00000001;

// The GLFW library is initialized once per process (idempotent by
// contract: glfwInit returns GLFW_TRUE immediately when already
// initialized) and never terminated — a game process owns its windowing
// lifetime (the test process exits; the M2-GL-02 render thread does not
// reinitialize it).
bool glfwInitialized() {
  static const bool kDone = glfwInit() != GLFW_FALSE;
  return kDone;
}

// Linux (the X11-only GLFW build, ADR 0007): an X server is reachable
// if and only if DISPLAY is set — the same probe GLFW's own diagnostic
// uses ("The DISPLAY environment variable is missing"). GLFW's X11
// backend dlopens X11 and initializes process-global Xlib state
// (XInitThreads/XrmInitialize) BEFORE XOpenDisplay, and its failure
// path (no display) frees the dlopen module but not that state — an
// upstream leak on the init-failure path that the ASan CI lane treats
// as fatal. The display-less hosts (the P0 CI runners) therefore never
// enter glfwInit for the windowed path: createWindowed returns the
// identical GlUnavailable/glfw_init result with no GLFW state created.
#if defined(__unix__) && !defined(__APPLE__)
bool x11DisplayAvailable() { return std::getenv("DISPLAY") != nullptr; }
#endif

// Parse "major.minor" from the GL_VERSION string ("4.6.0 ...", "3.3").
bool parseGlVersion(const char* s, std::int32_t& major, std::int32_t& minor) {
  if (s == nullptr) {
    return false;
  }
  auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
  std::int32_t maj = 0;
  int i = 0;
  while (isDigit(s[i])) {
    maj = maj * 10 + (s[i] - '0');
    ++i;
  }
  if (i == 0 || s[i] != '.') {
    return false;
  }
  ++i;
  std::int32_t min = 0;
  int n = 0;
  while (isDigit(s[i])) {
    min = min * 10 + (s[i] - '0');
    ++i;
    ++n;
  }
  if (n == 0) {
    return false;
  }
  major = maj;
  minor = min;
  return true;
}

// Clamp a driver string into a fixed buffer (bounded, NUL-terminated;
// driver text is untrusted — LOG-005).
void copyClamped(char* dst, std::size_t capacity, const char* src) {
  std::size_t i = 0;
  while (src[i] != '\0' && i + 1 < capacity) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

Status validateSize(std::int32_t width, std::int32_t height) {
  if (width < 1 || width > kMaxContextDimension || height < 1 ||
      height > kMaxContextDimension) {
    return Status(ErrorCode::InvalidArgument);
  }
  return Status();
}

}  // namespace

Status checkGlVersion(std::int32_t major, std::int32_t minor,
                      bool coreProfile) {
  const bool ok = coreProfile &&
                  (major > kGlMajor ||
                   (major == kGlMajor && minor >= kGlMinor));
  return ok ? Status() : Status(ErrorCode::GlVersionUnsupported);
}

// ------------------------------------------------------------------------
// Impl — one native context (GLFW window or EGL display/context) plus
// the headless FBO and the creation-time capability snapshot.
// ------------------------------------------------------------------------

struct GlContext::Impl {
  enum class Backend { glfw, egl };
  Backend backend = Backend::glfw;

  // GLFW path (windowed everywhere; headless on Windows/macOS).
  GLFWwindow* window = nullptr;

  // EGL path (headless on Linux).
  void* eglHandle = nullptr;  // dlopen handle for libEGL.so.1
  egl::FnTable egl{};
  egl::Display eglDisplay = nullptr;
  egl::Context eglContext = nullptr;

  std::int32_t width = 0;
  std::int32_t height = 0;
  std::uint32_t fbo = 0;        // 0 <=> windowed (the window frame buffer
                                // is the render target)
  std::uint32_t fboTexture = 0;
  GlCapabilities caps{};

  void destroy() {
    if (backend == Backend::egl) {
      if (egl.destroyContext != nullptr && eglContext != nullptr) {
        egl.destroyContext(eglDisplay, eglContext);
      }
      if (egl.destroyDisplay != nullptr && eglDisplay != nullptr) {
        egl.destroyDisplay(eglDisplay);
      }
      // libEGL is dlopened only on the Linux headless path (createHeadless
      // #else branch); on Windows/macOS eglHandle is always null and
      // <dlfcn.h> is not included (see the include guard above), so the
      // close is compiled only where the handle can exist.
#if !defined(__APPLE__) && !defined(_WIN32)
      if (eglHandle != nullptr) {
        dlclose(eglHandle);
      }
#endif
    } else {
      if (window != nullptr) {
        glfwDestroyWindow(window);  // destroys the context with it
      }
    }
  }
};

// ------------------------------------------------------------------------
// GlContext — creation-path helpers (static members: Impl is private)
// ------------------------------------------------------------------------

bool GlContext::isCurrentOnThisThread(const Impl* impl) {
  if (impl->backend == Impl::Backend::egl) {
    return impl->egl.getCurrentContext != nullptr &&
           impl->egl.getCurrentContext() == impl->eglContext;
  }
  return glfwGetCurrentContext() == impl->window;
}

// Query the capability snapshot and apply the 3.3 core gate. The
// context must be current on the calling thread.
Status GlContext::queryCapabilities(Impl* impl) {
  const char* versionString =
      reinterpret_cast<const char*>(glad_glGetString(GL_VERSION));
  if (!parseGlVersion(versionString, impl->caps.version.major,
                      impl->caps.version.minor)) {
    return Status(ErrorCode::GlVersionUnsupported);
  }
  GLint profileMask = 0;
  glad_glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
  impl->caps.coreProfile = (profileMask & kGlContextProfileBit) != 0;
  GLint maxTextureSize = 0;
  glad_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  impl->caps.maxTextureSize = static_cast<std::int32_t>(maxTextureSize);
  copyClamped(impl->caps.vendor, sizeof(impl->caps.vendor),
              reinterpret_cast<const char*>(glad_glGetString(GL_VENDOR)));
  copyClamped(impl->caps.renderer, sizeof(impl->caps.renderer),
              reinterpret_cast<const char*>(glad_glGetString(GL_RENDERER)));
  return checkGlVersion(impl->caps.version.major, impl->caps.version.minor,
                        impl->caps.coreProfile);
}

// The headless render target: an offscreen RGBA8 FBO, width x height.
// One setup-path allocation pair (texture + framebuffer); the FBO is
// left unbound — clear()/readPixel() bind it per call.
Status GlContext::createFbo(Impl* impl) {
  glad_glGenTextures(1, &impl->fboTexture);
  glad_glBindTexture(GL_TEXTURE_2D, impl->fboTexture);
  glad_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, impl->width, impl->height, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glad_glGenFramebuffers(1, &impl->fbo);
  glad_glBindFramebuffer(GL_FRAMEBUFFER, impl->fbo);
  glad_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, impl->fboTexture, 0);
  const GLenum status = glad_glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glad_glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    return Status(ErrorCode::GlUnavailable);
  }
  return Status();
}

// The tail shared by every successful native-context creation: GLAD
// load, capability query + gate, (headless) FBO, success event. The
// context is current on this thread (both creation paths make it
// current first), as GLAD's version detection requires.
Status GlContext::finishCreation(Impl* impl, const char* kind,
                                 bool headless) {
  const int gladVersion =
      (impl->backend == Impl::Backend::egl)
          ? gladLoadGLUserPtr(eglGladLoader, &impl->egl.getProcAddress)
          : gladLoaderLoadGL();
  if (gladVersion == 0) {
    return Status(ErrorCode::GlUnavailable);
  }
  const Status caps = queryCapabilities(impl);
  if (caps.isError()) {
    return caps;
  }
  if (headless && createFbo(impl).isError()) {
    return Status(ErrorCode::GlUnavailable);
  }
  LAIGE_LOG_INFO("gl", "context_created", "OpenGL 3.3 core context created",
                 laige::log::field("kind", std::string_view(kind)),
                 laige::log::field("version_major", impl->caps.version.major),
                 laige::log::field("version_minor", impl->caps.version.minor),
                 laige::log::field("width", impl->width),
                 laige::log::field("height", impl->height));
  return Status();
}

// One structured failure event per creation attempt (LOG-001/002): the
// stable event name, the attempt kind, the machine-stable reason, and
// the registry line for the returned code.
namespace {
void logCreationFailure(const char* kind, const char* reason,
                        ErrorCode code) {
  LAIGE_LOG_ERROR("gl", "context_creation_failed",
                  "OpenGL context creation failed",
                  laige::log::field("kind", std::string_view(kind)),
                  laige::log::field("reason", std::string_view(reason)),
                  laige::log::field("error", std::string_view(errorText(code))));
}
}  // namespace

// ------------------------------------------------------------------------
// GlContext — lifetime
// ------------------------------------------------------------------------

GlContext::GlContext() noexcept : impl_(nullptr) {}

GlContext::GlContext(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GlContext::GlContext(GlContext&& other) noexcept
    : impl_(std::move(other.impl_)) {}

GlContext& GlContext::operator=(GlContext&& other) noexcept {
  if (this != &other) {
    if (impl_) {
      impl_->destroy();
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

GlContext::~GlContext() {
  if (impl_) {
    impl_->destroy();
  }
}

bool GlContext::valid() const noexcept { return impl_ != nullptr; }

const GlVersion& GlContext::version() const noexcept {
  assert(impl_ != nullptr && "GlContext::version() on a stopped context");
  return impl_->caps.version;
}

const GlCapabilities& GlContext::capabilities() const noexcept {
  assert(impl_ != nullptr && "GlContext::capabilities() on a stopped context");
  return impl_->caps;
}

std::int32_t GlContext::width() const noexcept {
  assert(impl_ != nullptr && "GlContext::width() on a stopped context");
  return impl_->width;
}

std::int32_t GlContext::height() const noexcept {
  assert(impl_ != nullptr && "GlContext::height() on a stopped context");
  return impl_->height;
}

Status GlContext::makeCurrent() const {
  if (impl_ == nullptr) {
    return Status(ErrorCode::InvalidArgument);
  }
  bool ok;
  if (impl_->backend == Impl::Backend::egl) {
    // libglvnd's client-API selection is PER-THREAD: a thread that has
    // never selected an API cannot eglMakeCurrent an object created on
    // another thread — the call fails with EGL_BAD_ACCESS (0x3002;
    // the P0 CI log's egl_error=12290 on the render thread). The EGL
    // spec makes a new thread's default API OpenGL, so this is a
    // no-op success on stacks that honor the default (Mesa's native
    // libEGL) and the required step on the libglvnd dispatcher (the
    // P0 distro's libEGL.so.1). makeCurrent is the only cross-thread
    // entry point: createHeadless binds the API on its own thread, and
    // clear/readPixel require the context to be current ALREADY here.
    // Not a hot path (a takeover, not a per-frame call) — one plain
    // function-pointer call, no allocation.
    ok = impl_->egl.bindApi(egl::kOpenGlApi) != 0 &&
         impl_->egl.makeCurrent(impl_->eglDisplay, nullptr, nullptr,
                                impl_->eglContext) != 0;
  } else {
    // The window exists and GLFW is initialized (both guaranteed by
    // the creation path), so the bind cannot fail.
    glfwMakeContextCurrent(impl_->window);
    ok = true;
  }
  if (!ok) {
    // LOG-002: state the driver's reason when known — this thread's
    // last EGL error (the failure path is reachable only from the EGL
    // branch above; the GLFW bind sets ok unconditionally).
    const egl::Int eglError =
        (impl_->backend == Impl::Backend::egl) ? impl_->egl.getError() : 0;
    LAIGE_LOG_ERROR("gl", "context_make_current_failed",
                    "binding the GL context to the calling thread failed",
                    laige::log::field("error", std::string_view(
                        errorText(ErrorCode::GlUnavailable))),
                    laige::log::field("egl_error", eglError));
    return Status(ErrorCode::GlUnavailable);
  }
  return Status();
}

Status GlContext::release() const {
  if (impl_ == nullptr) {
    return Status(ErrorCode::InvalidArgument);
  }
  // The handoff protocol's first step (the class preamble): the old
  // owner unbinds the context from its own thread, so the new thread's
  // makeCurrent is a FRESH bind — a context no thread holds. (On the
  // P0 EGL stack, take-overs while the context is still current on
  // another live thread fail with EGL_BAD_ACCESS.)
  bool ok;
  if (impl_->backend == Impl::Backend::egl) {
    // EGL 1.5 §3.3.5: eglMakeCurrent with EGL_NO_CONTEXT releases the
    // calling thread's current context (a no-op success when this
    // thread holds none).
    ok = impl_->egl.makeCurrent(impl_->eglDisplay, nullptr, nullptr,
                                nullptr) != 0;
  } else {
    // The window exists and GLFW is initialized (both guaranteed by
    // the creation path), so the release cannot fail.
    glfwMakeContextCurrent(nullptr);
    ok = true;
  }
  if (!ok) {
    // LOG-002: state the driver's reason when known — this thread's
    // last EGL error (the failure path is reachable only from the EGL
    // branch above; the GLFW release sets ok unconditionally).
    const egl::Int eglError =
        (impl_->backend == Impl::Backend::egl) ? impl_->egl.getError() : 0;
    LAIGE_LOG_ERROR("gl", "context_release_failed",
                    "releasing the GL context from the calling thread failed",
                    laige::log::field("error", std::string_view(
                        errorText(ErrorCode::GlUnavailable))),
                    laige::log::field("egl_error", eglError));
    return Status(ErrorCode::GlUnavailable);
  }
  return Status();
}

Status GlContext::clear(float r, float g, float b, float a) const {
  if (impl_ == nullptr || !isCurrentOnThisThread(impl_.get())) {
    // Precondition violations (stopped, or the context is not current
    // on this thread), not engine failures: negative queries — no warn,
    // like a missing-component read on a live handle.
    return Status(ErrorCode::InvalidArgument);
  }
  // The render target: the FBO on headless contexts, the default
  // (window) frame buffer on windowed ones — one bind per call (the
  // per-frame draw path, M2-SPRITE-02, binds once per frame instead).
  glad_glBindFramebuffer(GL_FRAMEBUFFER, impl_->fbo);
  glad_glClearColor(clamp01(r), clamp01(g), clamp01(b), clamp01(a));
  glad_glClear(GL_COLOR_BUFFER_BIT);
  return Status();
}

Status GlContext::readPixel(std::int32_t x, std::int32_t y,
                            std::uint8_t rgba[4]) const {
  if (impl_ == nullptr || !isCurrentOnThisThread(impl_.get()) || x < 0 ||
      y < 0 || x >= impl_->width || y >= impl_->height) {
    return Status(ErrorCode::InvalidArgument);
  }
  glad_glBindFramebuffer(GL_FRAMEBUFFER, impl_->fbo);
  glad_glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  return Status();
}

std::uint32_t GlContext::refreshRateHz() const noexcept {
  // Stopped, or headless (the FBO is the render target — nothing is
  // ever presented, so no display rate applies): 0 (a negative query —
  // no log, no GL work).
  if (impl_ == nullptr || impl_->fbo != 0) {
    return 0;
  }
  // Windowed: the window's monitor's current video mode (a GLFW window
  // query — no GL call, no current thread required). Both query
  // failures (no attached monitor, no current video mode) yield 0 —
  // the frame clock's documented fallback (frame_pipeline.md).
  GLFWmonitor* monitor = glfwGetWindowMonitor(impl_->window);
  if (monitor == nullptr) {
    return 0;
  }
  const GLFWvidmode* mode = glfwGetVideoMode(monitor);
  if (mode == nullptr) {
    return 0;
  }
  return mode->refreshRate > 0
             ? static_cast<std::uint32_t>(mode->refreshRate)
             : 0;
}

// ------------------------------------------------------------------------
// Creation
// ------------------------------------------------------------------------

Result<GlContext> GlContext::createWindowed(std::int32_t width,
                                            std::int32_t height,
                                            const char* title) {
  const Status size = validateSize(width, height);
  if (size.isError()) {
    return Result<GlContext>::failure(size.error());
  }
  if (title == nullptr || title[0] == '\0') {
    return Result<GlContext>::failure(ErrorCode::InvalidArgument);
  }
#if defined(__unix__) && !defined(__APPLE__)
  // Display-less Linux host: fail fast with the identical
  // GlUnavailable/glfw_init result glfwInit would produce, without
  // entering its (leaky on failure) X11 init path — see
  // x11DisplayAvailable().
  if (!x11DisplayAvailable()) {
    logCreationFailure("windowed", "glfw_init", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
#endif
  auto impl = std::make_unique<Impl>();
  impl->width = width;
  impl->height = height;
  if (!glfwInitialized()) {
    logCreationFailure("windowed", "glfw_init", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, kGlMajor);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, kGlMinor);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  impl->window = glfwCreateWindow(width, height, title, nullptr, nullptr);
  if (impl->window == nullptr) {
    logCreationFailure("windowed", "window_create", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  glfwMakeContextCurrent(impl->window);
  const Status finish =
      finishCreation(impl.get(), "windowed", /*headless=*/false);
  if (finish.isError()) {
    impl->destroy();
    return Result<GlContext>::failure(finish.error());
  }
  return Result<GlContext>::success(GlContext(std::move(impl)));
}

#if defined(__APPLE__) || defined(_WIN32)
// Headless on Windows/macOS: a never-shown GLFW window carries the
// context (GLFW owns the platform windowing; the FBO is the render
// target — the window is never presented).
Result<GlContext> GlContext::createHeadless(std::int32_t width,
                                            std::int32_t height) {
  const Status size = validateSize(width, height);
  if (size.isError()) {
    return Result<GlContext>::failure(size.error());
  }
  auto impl = std::make_unique<Impl>();
  impl->width = width;
  impl->height = height;
  if (!glfwInitialized()) {
    logCreationFailure("headless", "glfw_init", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, kGlMajor);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, kGlMinor);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  impl->window =
      glfwCreateWindow(width, height, "laige (offscreen)", nullptr, nullptr);
  if (impl->window == nullptr) {
    logCreationFailure("headless", "window_create", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  glfwMakeContextCurrent(impl->window);
  const Status finish =
      finishCreation(impl.get(), "headless", /*headless=*/true);
  if (finish.isError()) {
    impl->destroy();
    return Result<GlContext>::failure(finish.error());
  }
  return Result<GlContext>::success(GlContext(std::move(impl)));
}
#else
// Headless on Linux: the EGL surfaceless platform (no display, no
// window — the documented CI mechanism). libEGL is loaded at runtime
// so the engine carries no build-time EGL dependency.
Result<GlContext> GlContext::createHeadless(std::int32_t width,
                                            std::int32_t height) {
  const Status size = validateSize(width, height);
  if (size.isError()) {
    return Result<GlContext>::failure(size.error());
  }
  auto impl = std::make_unique<Impl>();
  impl->width = width;
  impl->height = height;
  impl->backend = Impl::Backend::egl;

  impl->eglHandle = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (impl->eglHandle == nullptr) {
    logCreationFailure("headless", "egl_load", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  impl->egl.getPlatformDisplay =
      reinterpret_cast<egl::Display (*)(egl::Enum, void*, const egl::Int*)>(
          dlsym(impl->eglHandle, "eglGetPlatformDisplay"));
  impl->egl.initialize =
      reinterpret_cast<egl::Bool (*)(egl::Display, egl::Int*, egl::Int*)>(
          dlsym(impl->eglHandle, "eglInitialize"));
  impl->egl.bindApi = reinterpret_cast<egl::Bool (*)(egl::Enum)>(
      dlsym(impl->eglHandle, "eglBindAPI"));
  impl->egl.createContext =
      reinterpret_cast<egl::Context (*)(egl::Display, egl::Config,
                                        egl::Context, const egl::Int*)>(
          dlsym(impl->eglHandle, "eglCreateContext"));
  impl->egl.makeCurrent =
      reinterpret_cast<egl::Bool (*)(egl::Display, egl::Surface, egl::Surface,
                                     egl::Context)>(
          dlsym(impl->eglHandle, "eglMakeCurrent"));
  impl->egl.getError =
      reinterpret_cast<egl::Int (*)(void)>(
          dlsym(impl->eglHandle, "eglGetError"));
  impl->egl.getCurrentContext =
      reinterpret_cast<egl::Context (*)()>(
          dlsym(impl->eglHandle, "eglGetCurrentContext"));
  impl->egl.destroyContext =
      reinterpret_cast<egl::Bool (*)(egl::Display, egl::Context)>(
          dlsym(impl->eglHandle, "eglDestroyContext"));
  impl->egl.destroyDisplay =
      reinterpret_cast<egl::Bool (*)(egl::Display)>(
          dlsym(impl->eglHandle, "eglDestroyDisplay"));
  impl->egl.getProcAddress = reinterpret_cast<void* (*)(const char*)>(
      dlsym(impl->eglHandle, "eglGetProcAddress"));
  // A partial table is unusable: any missing symbol is a clean failure —
  // except eglDestroyDisplay, which is OPTIONAL: the libglvnd dispatcher
  // (libEGL.so.1 on P0 Ubuntu, what the engine dlopens) does not export
  // it (verified against the noble libegl1 1.7.0 symbol table), while
  // vendor libraries (e.g. Mesa's libEGL_mesa.so.0) do. When absent,
  // the display's resources are released at process termination — the
  // engine's one-display-per-process design (CORE-009: the display's
  // lifetime is the process's, owned by this context object until then).
  // destroy() already skips the call when the pointer is null.
  // eglGetProcAddress is REQUIRED: both the libglvnd dispatcher and
  // Mesa's vendor library export it (verified against the noble symbol
  // tables), and the headless GLAD load resolves the GL API through it.
  // eglGetError is EGL 1.0 core — both the libglvnd dispatcher and the
  // vendor libraries export it (verified against the noble symbol
  // tables) — so it is required, unlike eglDestroyDisplay.
  const bool complete =
      impl->egl.getPlatformDisplay != nullptr &&
      impl->egl.initialize != nullptr && impl->egl.bindApi != nullptr &&
      impl->egl.createContext != nullptr && impl->egl.makeCurrent != nullptr &&
      impl->egl.getError != nullptr &&
      impl->egl.getCurrentContext != nullptr &&
      impl->egl.destroyContext != nullptr &&
      impl->egl.getProcAddress != nullptr;
  if (!complete) {
    logCreationFailure("headless", "egl_load", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }

  impl->eglDisplay = impl->egl.getPlatformDisplay(egl::kPlatformSurfacelessMesa,
                                                  nullptr, nullptr);
  if (impl->eglDisplay == nullptr) {
    logCreationFailure("headless", "egl_surfaceless",
                       ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }

  // eglBindAPI selects the client API for this thread (the GLVND
  // dispatcher sets its thread state plus the vendor's; the spec
  // guarantees no failure for a valid API enum — the dispatcher
  // rejects an unknown enum with EGL_BAD_PARAMETER). Called before
  // eglInitialize per the spec's API-selection order; the result is
  // still checked (CORE-008): a failure means a broken EGL stack (an
  // ES-only or mismatched dispatcher), a clean GlUnavailable — never a
  // silent fallback.
  if (impl->egl.bindApi(egl::kOpenGlApi) == 0) {
    logCreationFailure("headless", "egl_context", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  egl::Int major = 0;
  egl::Int minor = 0;
  if (impl->egl.initialize(impl->eglDisplay, &major, &minor) == 0) {
    logCreationFailure("headless", "egl_init", ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  const egl::Int attrs[] = {
      egl::kContextMajorVersion, kGlMajor,
      egl::kContextMinorVersion, kGlMinor,
      egl::kContextProfileMask, egl::kCoreProfileBit,
      egl::kNone,
  };
  impl->eglContext =
      impl->egl.createContext(impl->eglDisplay, nullptr, nullptr, attrs);
  if (impl->eglContext == nullptr) {
    logCreationFailure("headless", "egl_context",
                       ErrorCode::GlUnavailable);
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  if (impl->egl.makeCurrent(impl->eglDisplay, nullptr, nullptr,
                            impl->eglContext) == 0) {
    // LOG-002: name the driver's error (the creation helper's fixed
    // field set cannot carry it).
    LAIGE_LOG_ERROR("gl", "context_creation_failed",
                    "OpenGL context creation failed",
                    laige::log::field("kind", std::string_view("headless")),
                    laige::log::field("reason",
                                      std::string_view("egl_make_current")),
                    laige::log::field("error", std::string_view(
                        errorText(ErrorCode::GlUnavailable))),
                    laige::log::field("egl_error", impl->egl.getError()));
    return Result<GlContext>::failure(ErrorCode::GlUnavailable);
  }
  const Status finish =
      finishCreation(impl.get(), "headless", /*headless=*/true);
  if (finish.isError()) {
    impl->destroy();
    return Result<GlContext>::failure(finish.error());
  }
  return Result<GlContext>::success(GlContext(std::move(impl)));
}
#endif

}  // namespace laige::render
