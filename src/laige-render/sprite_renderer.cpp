// laige-render sprite submit stage implementation (M2-SPRITE-02).
//
// The header (include/laige/render/sprite_renderer.h) carries the
// contract: one instanced draw call per (atlas, material, blend)
// group per frame (FR-2.1), the minimal GLSL 3.30 sprite shader, the
// observable state changes (RENDER-001), zero per-frame allocation
// (FR-2.2), the ownership/lifetime and the failure contract.
//
// GL state model (documented per RENDER-001): the sprite pass sets,
// per frame — depth test OFF (the painter's order is the batcher's,
// the 2.5D depth is engine-owned, FR-2.2), blend ENABLED, one texture
// bind per group whose atlas changed, one blend-function change per
// group whose blend changed, one instanced draw per group — and
// restores the program + VAO bindings (glUseProgram(0),
// glBindVertexArray(0)) so the pass leaves no hidden GL state for the
// engine's other passes. The per-group attribute-pointer setup is
// the only other per-frame GL work (6 glVertexAttribPointer per
// group, CPU-side, no GPU synchronization).

#include <glad/gl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "laige/render/sprite_renderer.h"
#include "laige/logging.h"

namespace laige::render {

namespace {


// ------------------------------------------------------------------------
// The minimal GLSL 3.30 sprite shader (the header preamble): one unit
// quad (per-vertex), the per-instance attributes (world pos, scale,
// UV sub-rect, tint, rotation), the combined world -> NDC matrix
// uniform. GLSL 3.30 layout qualifiers pin the attribute locations —
// no glGetAttribLocation round trip.
// ------------------------------------------------------------------------
constexpr const char* kVertexShader =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aCorner;\n"
    "layout(location = 1) in vec2 aPos;\n"
    "layout(location = 2) in vec2 aScale;\n"
    "layout(location = 3) in vec4 aUv;\n"
    "layout(location = 4) in vec4 aTint;\n"
    "layout(location = 5) in float aRot;\n"
    "uniform mat4 uWorldToNdc;\n"
    "out vec2 vUv;\n"
    "out vec4 vTint;\n"
    "void main() {\n"
    "  // The world-space quad corner: the world-unit scale is applied\n"
    "  // BEFORE the projection (the SpriteItem.scale contract).\n"
    "  vec2 world = aPos + aCorner * aScale;\n"
    "  vec4 center = uWorldToNdc * vec4(aPos, 0.0, 1.0);\n"
    "  vec4 p = uWorldToNdc * vec4(world, 0.0, 1.0);\n"
    "  // The screen-space rotation (NDC) of the projected offset\n"
    "  // (the SpriteItem.rotation contract — applied in screen space,\n"
    "  // not world space).\n"
    "  vec2 off = p.xy - center.xy;\n"
    "  float c = cos(aRot);\n"
    "  float s = sin(aRot);\n"
    "  off = vec2(c * off.x - s * off.y, s * off.x + c * off.y);\n"
    "  gl_Position = vec4(center.xy + off, 0.0, 1.0);\n"
    "  // The per-vertex UV: the quad corner maps onto the UV sub-rect\n"
    "  // ((-0.5, -0.5) -> u0/v0, (0.5, 0.5) -> u1/v1).\n"
    "  vUv = vec2(aUv.x + (aCorner.x + 0.5) * (aUv.z - aUv.x),\n"
    "             aUv.y + (aCorner.y + 0.5) * (aUv.w - aUv.y));\n"
    "  vTint = aTint;\n"
    "}\n";

constexpr const char* kFragmentShader =
    "#version 330 core\n"
    "uniform sampler2D uAtlas;\n"
    "in vec2 vUv;\n"
    "in vec4 vTint;\n"
    "out vec4 outColor;\n"
    "void main() {\n"
    "  outColor = texture(uAtlas, vUv) * vTint;\n"
    "}\n";

// One float in the per-instance staging layout (52 B per instance):
//   0..1   aPos    2..3   aScale    4..7   aUv (u0, v0, u1, v1)
//   8..11  aTint   12     aRot
constexpr std::size_t kInstanceFloats = 13;
constexpr std::size_t kInstanceBytes = kInstanceFloats * 4u;

// The unit quad strip (4 vertices, [-0.5, 0.5]^2, CCW): the per-vertex
// aCorner attribute (the VBO content — set once at create).
constexpr float kQuadVerts[8] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f,
                                 0.5f, 0.5f};

// The glVertexAttribPointer pointer parameter is an OFFSET (bytes)
// from the start of the bound buffer (the GL 3.3 contract — the CPU
// staging address is irrelevant on the GPU): the standard integer ->
// pointer cast idiom.
const void* bufferOffset(std::size_t bytes) {
  return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(bytes));
}

// The driver's info log, bounded + control-character-sanitized for
// logging (LOG-005): at most 256 printable characters.
std::string sanitizeInfoLog(const std::string& log) {
  std::string out;
  out.reserve(256);
  for (char ch : log) {
    if (out.size() >= 256) {
      out.append("...");
      break;
    }
    const unsigned char u = static_cast<unsigned char>(ch);
    out.push_back((u >= 0x20 && u <= 0x7e) ? static_cast<char>(u) : '?');
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------------------
// Impl
// ------------------------------------------------------------------------

struct SpriteRenderer::Impl {
  const GlContext* ctx{};
  std::uint32_t maxInstances{0};
  std::uint32_t maxAtlases{0};
  std::uint32_t maxDrawCalls{0};
  std::int32_t maxTextureSize{0};
  bool valid{false};

  // The GL resources (0 = not created; destroyed through the context
  // — the context outlives the renderer, the header's Ownership
  // section).
  std::uint32_t program{0};
  std::uint32_t quadVbo{0};
  std::uint32_t instanceVbo{0};
  std::uint32_t vao{0};
  struct Atlas {
    std::uint32_t texture{0};
    // The M2-SPRITE-04 VRAM estimate's bookkeeping: the uploaded
    // width/height (w * h * 4 bytes, GL_RGBA8).
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool bound{false};
  };
  std::unique_ptr<Atlas[]> atlases;
  // The VRAM estimate (M2-SPRITE-04): the bound atlases' w * h * 4
  // sum, kept current at bindAtlas (a re-bind replaces: subtract old,
  // add new). A gauge — it never changes per frame.
  std::uint64_t textureMemoryBytes{0};
  // The per-frame CPU staging (maxInstances * kInstanceFloats floats,
  // sized at create — FR-2.2).
  std::unique_ptr<float[]> staging;

  // The uniform locations (the instance attribute locations are
  // pinned by the GLSL layout qualifiers, 1..5).
  int locMatrix{-1};
  int locAtlas{-1};

  // The opt-in PRIMITIVES_GENERATED query (the header's Options
  // section: diagnostic mode, zero cost when off).
  bool primitiveQuery{false};

  // Per-frame state (reset at the start of every submit; a failed
  // submit leaves them ZERO).
  SpriteDrawStats frame{};
  std::uint32_t lastAtlas{0};
  bool haveLastAtlas{false};
  BlendMode lastBlend{BlendMode::Alpha};
  bool haveLastBlend{false};

  // Since construction (successful submits only).
  SpriteDrawTotals totals{};

  // Delete every created GL resource (no-op for the 0 handles; called
  // from the destructor and the create-failure cleanup — the context
  // is guaranteed alive by the header's Ownership section).
  void destroy() noexcept {
    if (atlases != nullptr) {
      for (std::uint32_t a = 0; a < maxAtlases; ++a) {
        if (atlases[a].bound) {
          glDeleteTextures(1, &atlases[a].texture);
          atlases[a].texture = 0;
          atlases[a].bound = false;
        }
      }
    }
    if (quadVbo != 0) {
      glDeleteBuffers(1, &quadVbo);
      quadVbo = 0;
    }
    if (instanceVbo != 0) {
      glDeleteBuffers(1, &instanceVbo);
      instanceVbo = 0;
    }
    if (vao != 0) {
      glDeleteVertexArrays(1, &vao);
      vao = 0;
    }
    if (program != 0) {
      glDeleteProgram(program);
      program = 0;
    }
  }

  ~Impl() { destroy(); }
};

// ------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------

SpriteRenderer::SpriteRenderer() noexcept : impl_(nullptr) {}

SpriteRenderer::SpriteRenderer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

SpriteRenderer::~SpriteRenderer() = default;

SpriteRenderer::SpriteRenderer(SpriteRenderer&&) noexcept = default;

SpriteRenderer& SpriteRenderer::operator=(SpriteRenderer&&) noexcept =
    default;

bool SpriteRenderer::valid() const noexcept {
  return impl_ != nullptr && impl_->valid;
}

std::uint32_t SpriteRenderer::maxInstances() const noexcept {
  return (impl_ != nullptr && impl_->valid) ? impl_->maxInstances : 0;
}

std::uint32_t SpriteRenderer::maxAtlases() const noexcept {
  return (impl_ != nullptr && impl_->valid) ? impl_->maxAtlases : 0;
}

std::uint32_t SpriteRenderer::maxDrawCalls() const noexcept {
  return (impl_ != nullptr && impl_->valid) ? impl_->maxDrawCalls : 0;
}

SpriteDrawStats SpriteRenderer::frameStats() const noexcept {
  return (impl_ != nullptr) ? impl_->frame : SpriteDrawStats{};
}

SpriteDrawTotals SpriteRenderer::totals() const noexcept {
  return (impl_ != nullptr) ? impl_->totals : SpriteDrawTotals{};
}

std::uint64_t SpriteRenderer::textureMemoryBytes() const noexcept {
  return (impl_ != nullptr && impl_->valid) ? impl_->textureMemoryBytes : 0;
}

// ------------------------------------------------------------------------
// create — the one-time setup (never inside a frame, PERF-002)
// ------------------------------------------------------------------------

laige::Result<SpriteRenderer, laige::ErrorCode> SpriteRenderer::create(
    const GlContext& ctx, Options options) noexcept {
  // 1. The options (first failure wins, one Warn — the Camera::create
  //    precedent). No GL work before the context check.
  const char* badOption = nullptr;
  std::uint32_t badValue = 0;
  if (options.maxInstances < 1 ||
      options.maxInstances > kSpriteRendererMaxInstances) {
    badOption = "maxInstances";
    badValue = options.maxInstances;
  } else if (options.maxAtlases < 1 ||
             options.maxAtlases > kSpriteRendererMaxAtlases) {
    badOption = "maxAtlases";
    badValue = options.maxAtlases;
  } else if (options.maxDrawCalls < 1 ||
             options.maxDrawCalls > kSpriteRendererMaxInstances) {
    badOption = "maxDrawCalls";
    badValue = options.maxDrawCalls;
  }
  if (badOption != nullptr) {
    LAIGE_LOG_WARN("sprite_renderer", "options_invalid",
                   "SpriteRenderer create rejected an option",
                   laige::log::field("option", badOption),
                   laige::log::field("value", badValue));
    return laige::Result<SpriteRenderer, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  // 2. The context (valid + current on the creating thread).
  if (!ctx.valid()) {
    return laige::Result<SpriteRenderer, laige::ErrorCode>::failure(
        laige::ErrorCode::GlUnavailable);
  }
  const laige::Status current = ctx.makeCurrent();
  if (current.isError()) {
    return laige::Result<SpriteRenderer, laige::ErrorCode>::failure(
        current.error());
  }

  // 3. The owned state (the CPU side — always constructible).
  auto impl = std::make_unique<Impl>();
  impl->ctx = &ctx;
  impl->maxInstances = options.maxInstances;
  impl->maxAtlases = options.maxAtlases;
  impl->maxDrawCalls = options.maxDrawCalls;
  impl->primitiveQuery = options.primitiveQuery;
  impl->maxTextureSize = ctx.capabilities().maxTextureSize;
  impl->atlases = std::make_unique<Impl::Atlas[]>(options.maxAtlases);
  impl->staging =
      std::make_unique<float[]>(static_cast<std::size_t>(
                                     options.maxInstances) * kInstanceFloats);

  // 4. The GL resources. Any failure: one structured Error (with the
  //    driver info log where relevant), the cleanup, GlUnavailable.
  const auto fail = [&](const char* stage, GLenum glError,
                        const std::string* infoLog) {
    LAIGE_LOG_ERROR("sprite_renderer", "resource_creation_failed",
                    "SpriteRenderer create failed on the GL stage",
                    laige::log::field("stage", stage),
                    laige::log::field("gl_error",
                                      static_cast<std::uint32_t>(glError)));
    if (infoLog != nullptr && !infoLog->empty()) {
      LAIGE_LOG_ERROR("sprite_renderer", "program_creation_failed",
                      "The driver info log for the failed stage",
                      laige::log::field("stage", stage),
                      laige::log::field("info_log", *infoLog));
    }
    impl->destroy();
    return laige::Result<SpriteRenderer, laige::ErrorCode>::failure(
        laige::ErrorCode::GlUnavailable);
  };

  // 4a. The program: compile both shaders, attach, link.
  const GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
  const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  if (vertex == 0 || fragment == 0) {
    if (vertex != 0) glDeleteShader(vertex);
    if (fragment != 0) glDeleteShader(fragment);
    return fail("shader_create", glGetError(), nullptr);
  }
  const GLchar* vs = kVertexShader;
  const GLchar* fs = kFragmentShader;
  glShaderSource(vertex, 1, &vs, nullptr);
  glShaderSource(fragment, 1, &fs, nullptr);
  glCompileShader(vertex);
  glCompileShader(fragment);
  std::string infoLog;
  const struct ShaderCheck {
    GLuint id;
    const char* stage;
  } shaders[2] = {
      {vertex, "vertex_shader"},
      {fragment, "fragment_shader"},
  };
  for (const ShaderCheck& shader : shaders) {
    GLint compiled = 0;
    glGetShaderiv(shader.id, GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
      GLint len = 0;
      glGetShaderiv(shader.id, GL_INFO_LOG_LENGTH, &len);
      infoLog.assign(static_cast<std::size_t>(std::max(len, 0)) + 1, '\0');
      glGetShaderInfoLog(shader.id, static_cast<GLsizei>(infoLog.size()),
                         nullptr, infoLog.data());
      infoLog = sanitizeInfoLog(infoLog);
      glDeleteShader(vertex);
      glDeleteShader(fragment);
      return fail(shader.stage, glGetError(), &infoLog);
    }
  }
  const GLuint program = glCreateProgram();
  if (program == 0) {
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return fail("program_create", glGetError(), nullptr);
  }
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glDeleteShader(vertex);  // the program owns the compiled stages now
  glDeleteShader(fragment);
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == 0) {
    GLint len = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    infoLog.assign(static_cast<std::size_t>(std::max(len, 0)) + 1, '\0');
    glGetProgramInfoLog(program, static_cast<GLsizei>(infoLog.size()),
                        nullptr, infoLog.data());
    infoLog = sanitizeInfoLog(infoLog);
    glDeleteProgram(program);
    return fail("program_link", glGetError(), &infoLog);
  }
  impl->program = program;
  impl->locMatrix = static_cast<int>(glGetUniformLocation(program,
                                                          "uWorldToNdc"));
  impl->locAtlas = static_cast<int>(
      glGetUniformLocation(program, "uAtlas"));
  if (impl->locMatrix < 0 || impl->locAtlas < 0) {
    return fail("uniform_lookup", glGetError(), nullptr);
  }

  // 4b. The quad VBO (set once).
  GLuint quadVbo = 0;
  glGenBuffers(1, &quadVbo);
  if (quadVbo == 0) {
    return fail("quad_vbo", glGetError(), nullptr);
  }
  glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(kQuadVerts)),
               kQuadVerts, GL_STATIC_DRAW);

  // 4c. The per-frame instance buffer (the frame budget, FR-2.2: sized
  //     once, reused per frame).
  GLuint instanceVbo = 0;
  glGenBuffers(1, &instanceVbo);
  if (instanceVbo == 0) {
    glDeleteBuffers(1, &quadVbo);
    return fail("instance_vbo", glGetError(), nullptr);
  }
  glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(
                   static_cast<std::size_t>(options.maxInstances) *
                   kInstanceBytes),
               nullptr, GL_DYNAMIC_DRAW);

  // 4d. The VAO: the quad attribute (divisor 0) + the instance
  //     attributes (divisor 1) — the per-group offsets are re-set in
  //     submit (they capture into the VAO's state).
  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  if (vao == 0) {
    glDeleteBuffers(1, &quadVbo);
    glDeleteBuffers(1, &instanceVbo);
    return fail("vao", glGetError(), nullptr);
  }
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                         nullptr);
  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
  // The initial offsets (0 = the first instance); submit() re-sets
  // them per group (they capture into the VAO's state).
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kInstanceBytes,
                         bufferOffset(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kInstanceBytes,
                         bufferOffset(2 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, kInstanceBytes,
                         bufferOffset(4 * sizeof(float)));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, kInstanceBytes,
                         bufferOffset(8 * sizeof(float)));
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, kInstanceBytes,
                         bufferOffset(12 * sizeof(float)));
  glEnableVertexAttribArray(5);
  glVertexAttribDivisor(1, 1);
  glVertexAttribDivisor(2, 1);
  glVertexAttribDivisor(3, 1);
  glVertexAttribDivisor(4, 1);
  glVertexAttribDivisor(5, 1);
  glBindVertexArray(0);

  const GLenum vaoError = glGetError();
  if (vaoError != GL_NO_ERROR) {
    glDeleteBuffers(1, &quadVbo);
    glDeleteBuffers(1, &instanceVbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    return fail("vao_setup", vaoError, nullptr);
  }

  impl->quadVbo = quadVbo;
  impl->instanceVbo = instanceVbo;
  impl->vao = vao;
  impl->valid = true;
  return laige::Result<SpriteRenderer, laige::ErrorCode>::success(
      SpriteRenderer(std::move(impl)));
}

// ------------------------------------------------------------------------
// bindAtlas — the setup/asset path (never the frame hot path)
// ------------------------------------------------------------------------

laige::Status SpriteRenderer::bindAtlas(std::uint32_t atlasId,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        std::span<const std::uint8_t> rgba) {
  if (!valid()) {  // the stopped state: impl_ is null
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  Impl& i = *impl_;
  // The argument domain (no GL work, no logging — the precondition
  // contract, the GlContext::readPixel precedent).
  if (atlasId >= i.maxAtlases) {
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  if (width < 1 || static_cast<std::uint32_t>(i.maxTextureSize) < width ||
      height < 1 || static_cast<std::uint32_t>(i.maxTextureSize) < height) {
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  if (rgba.size() !=
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u) {
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  const laige::Status current = i.ctx->makeCurrent();
  if (current.isError()) {
    return current;
  }
  GLuint texture = 0;
  glGenTextures(1, &texture);
  if (texture == 0) {
    LAIGE_LOG_ERROR("sprite_renderer", "atlas_upload_failed",
                    "The atlas texture object could not be created",
                    laige::log::field("atlas_id", atlasId),
                    laige::log::field("gl_error",
                                      static_cast<std::uint32_t>(glGetError())));
    return laige::Status(laige::ErrorCode::GlUnavailable);
  }
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
               static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               rgba.data());
  const GLenum uploadError = glGetError();
  if (uploadError != GL_NO_ERROR) {
    glDeleteTextures(1, &texture);
    LAIGE_LOG_ERROR("sprite_renderer", "atlas_upload_failed",
                    "The atlas texture upload failed",
                    laige::log::field("atlas_id", atlasId),
                    laige::log::field("width", width),
                    laige::log::field("height", height),
                    laige::log::field("gl_error",
                                      static_cast<std::uint32_t>(uploadError)));
    return laige::Status(laige::ErrorCode::GlUnavailable);
  }
  // The last bind wins: replace any previous texture on this id.
  // The M2-SPRITE-04 VRAM estimate tracks the replacement (subtract
  // the old upload, add the new — both exact GL_RGBA8 byte counts).
  Impl::Atlas& slot = i.atlases[atlasId];
  const std::uint64_t newBytes =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) *
      4u;
  if (slot.bound) {
    glDeleteTextures(1, &slot.texture);
    i.textureMemoryBytes -=
        static_cast<std::uint64_t>(slot.width) *
        static_cast<std::uint64_t>(slot.height) * 4u;
  }
  slot.texture = texture;
  slot.width = width;
  slot.height = height;
  slot.bound = true;
  i.textureMemoryBytes += newBytes;
  return laige::Status{};
}

// ------------------------------------------------------------------------
// submit — the frame pipeline's submit stage (the hot path)
// ------------------------------------------------------------------------

laige::Status SpriteRenderer::submit(SpriteBatcher& batcher,
                                     const Mat4& worldToNdc) {
  if (!valid()) {  // the stopped state: impl_ is null
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  Impl& i = *impl_;
  // A failed submit (any stage below) leaves the frame counters ZERO
  // and the since-construction totals UNCHANGED.
  i.frame = SpriteDrawStats{};
  // 2. The context current on this thread (idempotent makeCurrent —
  //    the M2-GL-02 onStart hook did it once; a cross-thread live
  //    takeover fails GlUnavailable, the P0 EGL contract).
  const laige::Status current = i.ctx->makeCurrent();
  if (current.isError()) {
    return current;
  }
  // 3. The batcher built for the current frame (an open window with
  //    declared items is NEVER drawn as an empty frame — CORE-008).
  if (!batcher.frameBuilt()) {
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  // 4. The frame budget (PERF-008: bounded, rate-limited Warn). The
  // frame count is always <= the batcher's u32 capacity, so the
  // narrowing is lossless (MSVC C4267).
  const std::uint32_t n = static_cast<std::uint32_t>(batcher.frameCount());
  if (n > i.maxInstances) {
    LAIGE_LOG_WARN("sprite_renderer", "instance_capacity",
                   "The frame exceeds the renderer instance budget; it is "
                   "not drawn",
                   laige::log::field("capacity", i.maxInstances),
                   laige::log::field("frame_count", n));
    return laige::Status(laige::ErrorCode::BudgetExhausted);
  }
  // 5. Every group's atlas in the registry AND bound (the stateless
  //    validation BEFORE any GL state change — one failure per frame,
  //    CORE-008). The batcher admits any u32 atlas id; the registry
  //    is a flat table — the range check precedes the read.
  for (const SpriteBatch& b : batcher.batches()) {
    if (b.atlasId >= i.maxAtlases || !i.atlases[b.atlasId].bound) {
      LAIGE_LOG_ERROR("sprite_renderer", "atlas_unbound",
                      "A batch group references an atlas that was never "
                      "bound; the frame is not drawn",
                      laige::log::field("atlas_id", b.atlasId));
      return laige::Status(laige::ErrorCode::InvalidArgument);
    }
  }
  // G-R2 (PRD §9.3): the per-pass draw-call cap. The frame's draw
  // calls == its group count (one instanced draw per group); above
  // the cap the frame is STILL drawn — observation, never an
  // execution gate (the G-R5 precedent): one rate-limited Warn
  // (LOG-004) + the frame-graph flag on the frame (M2-PROF-01 will
  // report it) + the since-construction total. The batcher's group
  // count is always <= its u32 capacity, so the narrowing is
  // lossless (MSVC C4267).
  const std::uint32_t groups = static_cast<std::uint32_t>(batcher.batchCount());
  if (groups > i.maxDrawCalls) {
    i.frame.drawCallCapExceeded = true;
    LAIGE_LOG_WARN("sprite_renderer", "draw_call_cap",
                   "The frame exceeds the per-pass draw-call cap; it is "
                   "still drawn",
                   laige::log::field("capacity", i.maxDrawCalls),
                   laige::log::field("draw_calls", groups));
  }
  // The empty frame: nothing to draw — count it, no GL state.
  if (n == 0) {
    i.totals.frames += 1;
    return laige::Status{};
  }

  // 6. The sprite-pass state (the header's GL state model): the
  //    render-target frame buffer (the offscreen FBO on headless —
  //    the surfaceless default frame buffer is not a valid draw
  //    target, the per-frame bind per the gl_context.md contract),
  //    the viewport matched to the render-target size (the default is
  //    0x0 — glClear ignores it, but every draw would clip to nothing),
  //    depth test OFF (the painter's order is the batcher's — the 2.5D
  //    depth is engine-owned, FR-2.2), blend ENABLED, the program +
  //    the per-frame matrix uniform.
  // The M2-SPRITE-04 render-target-use field: the size of the
  // render target this submit draws (width * height * 4, RGBA8).
  i.frame.renderTargetBytes =
      static_cast<std::uint64_t>(i.ctx->width()) *
      static_cast<std::uint64_t>(i.ctx->height()) * 4u;
  glBindFramebuffer(GL_FRAMEBUFFER, i.ctx->frameBuffer());
  glViewport(0, 0, i.ctx->width(), i.ctx->height());
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBindVertexArray(i.vao);
  glUseProgram(i.program);
  // The M2-SPRITE-04 program-change counter: the pass sets its own
  // program at the start (the previous pass's glUseProgram(0) makes
  // this a real change — exactly 1 per non-empty successful submit).
  i.frame.programChanges += 1;
  glUniformMatrix4fv(i.locMatrix, 1, GL_FALSE, &worldToNdc[0].x);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(i.locAtlas, 0);

  // 7. Pack the frame's instances (group order = the batcher's
  //    published order; in-group = the back-to-front order) into the
  //    staging — no allocation (FR-2.2), no float arithmetic (the
  //    items' floats are copied verbatim — the GPU owns the math).
  float* s = i.staging.get();
  for (const SpriteBatch& b : batcher.batches()) {
    for (const std::uint32_t slot : b.instances) {
      const SpriteItem& it = batcher.at(slot);
      s[0] = it.pos.x;
      s[1] = it.pos.y;
      s[2] = it.scale.x;
      s[3] = it.scale.y;
      s[4] = it.uv.u0;
      s[5] = it.uv.v0;
      s[6] = it.uv.u1;
      s[7] = it.uv.v1;
      s[8] = it.tint.r;
      s[9] = it.tint.g;
      s[10] = it.tint.b;
      s[11] = it.tint.a;
      s[12] = it.rotation;
      s += kInstanceFloats;
    }
  }
  // 8. The ONE per-frame upload (RENDER-004: the observable GPU work
  //    of the pass — n * 52 B; M2-SPRITE-04 meters it in
  //    uploadBytes).
  i.frame.uploadBytes =
      static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(kInstanceBytes);
  glBindBuffer(GL_ARRAY_BUFFER, i.instanceVbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0,
                  static_cast<GLsizeiptr>(static_cast<std::size_t>(n) *
                                          kInstanceBytes),
                  i.staging.get());
  const GLenum uploadError = glGetError();
  if (uploadError != GL_NO_ERROR) {
    glUseProgram(0);
    glBindVertexArray(0);
    i.frame = SpriteDrawStats{};  // a failed submit counts nothing
    LAIGE_LOG_ERROR("sprite_renderer", "submit_failed",
                    "The instance upload failed; the frame is not drawn",
                    laige::log::field("gl_error",
                                      static_cast<std::uint32_t>(uploadError)));
    return laige::Status(laige::ErrorCode::GlUnavailable);
  }

  // 9. The per-group state + draw (RENDER-001: one of each per group at
  //    minimum — the counters below are the observable truth). The
  //    opt-in PRIMITIVES_GENERATED query wraps the draws (the header's
  //    Options section — diagnostic mode, zero cost when off).
  GLuint query = 0;
  if (i.primitiveQuery) {
    glGenQueries(1, &query);
    if (query == 0) {
      glUseProgram(0);
      glBindVertexArray(0);
      i.frame = SpriteDrawStats{};  // a failed submit counts nothing
      LAIGE_LOG_ERROR("sprite_renderer", "submit_failed",
                      "The primitive query object could not be created; "
                      "the frame is not drawn",
                      laige::log::field("gl_error",
                                        static_cast<std::uint32_t>(
                                            glGetError())));
      return laige::Status(laige::ErrorCode::GlUnavailable);
    }
    glBeginQuery(GL_PRIMITIVES_GENERATED, query);
  }
  std::uint32_t base = 0;
  for (const SpriteBatch& b : batcher.batches()) {
    if (!i.haveLastAtlas || b.atlasId != i.lastAtlas) {
      glBindTexture(GL_TEXTURE_2D, i.atlases[b.atlasId].texture);
      i.lastAtlas = b.atlasId;
      i.haveLastAtlas = true;
      i.frame.textureBinds += 1;
    }
    if (!i.haveLastBlend || b.blend != i.lastBlend) {
      if (b.blend == BlendMode::Alpha) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      } else {
        glBlendFunc(GL_ONE, GL_ONE);
      }
      i.lastBlend = b.blend;
      i.haveLastBlend = true;
      i.frame.blendChanges += 1;
    }
    // The group's instance offset in the shared buffer: the pointer
    // parameters are BYTES-OFFSETS from the buffer start (the GL
    // contract) — base * 52 B + the field's in-instance offset. The
    // VAO captures the updated pointers (5 CPU-side state sets, no
    // GPU work).
    const std::size_t ob = static_cast<std::size_t>(base) * kInstanceBytes;
    const GLsizei stride = static_cast<GLsizei>(kInstanceBytes);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                           bufferOffset(ob + 0 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                           bufferOffset(ob + 2 * sizeof(float)));
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                           bufferOffset(ob + 4 * sizeof(float)));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                           bufferOffset(ob + 8 * sizeof(float)));
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride,
                           bufferOffset(ob + 12 * sizeof(float)));
    // ONE instanced draw per group (FR-2.1) — the 4-vertex strip is
    // drawn per instance (2 triangles per instance).
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(b.instances.size()));
    i.frame.drawCalls += 1;
    i.frame.instances += static_cast<std::uint32_t>(b.instances.size());
    base += static_cast<std::uint32_t>(b.instances.size());
  }
  if (i.primitiveQuery) {
    glEndQuery(GL_PRIMITIVES_GENERATED);
  }
  const GLenum drawError = glGetError();
  if (drawError != GL_NO_ERROR) {
    if (query != 0) {
      glDeleteQueries(1, &query);
    }
    glUseProgram(0);
    glBindVertexArray(0);
    i.frame = SpriteDrawStats{};
    LAIGE_LOG_ERROR("sprite_renderer", "submit_failed",
                    "A group draw failed; the frame is not counted",
                    laige::log::field("gl_error",
                                      static_cast<std::uint32_t>(drawError)));
    return laige::Status(laige::ErrorCode::GlUnavailable);
  }

  // The opt-in primitive read: the diagnostic sync makes the query
  // result available (RENDER-005 — this is why the mode is off by
  // default and never used in the shipping frame loop).
  //
  // ponytail: the 32-bit glGetQueryObjectuiv read caps the count at
  // 2^32-1 primitives — beyond the realistic frame of 2^31 instances
  // (2 per instance); the glad-generated glQueryCounter has a broken
  // 2-argument signature in the vendored 2.0.8 loader.
  if (i.primitiveQuery) {
    glFinish();
    GLuint primitives = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &primitives);
    glDeleteQueries(1, &query);
    i.frame.primitives = primitives;
  }

  // 10. Leave no hidden GL state (the pass's program/VAO are its own).
  glUseProgram(0);
  glBindVertexArray(0);

  i.totals.frames += 1;
  i.totals.drawCalls += i.frame.drawCalls;
  i.totals.textureBinds += i.frame.textureBinds;
  i.totals.blendChanges += i.frame.blendChanges;
  i.totals.programChanges += i.frame.programChanges;
  i.totals.instances += i.frame.instances;
  i.totals.primitives += i.frame.primitives;
  i.totals.uploadBytes += i.frame.uploadBytes;
  i.totals.renderTargetBytes += i.frame.renderTargetBytes;
  if (i.frame.drawCallCapExceeded) {
    i.totals.capExceededFrames += 1;
  }
  return laige::Status{};
}

}  // namespace laige::render
