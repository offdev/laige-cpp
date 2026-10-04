// laige-render sprite submit stage (M2-SPRITE-02): the GPU instanced
// draw calls + the minimal GLSL 3.30 sprite shader.
//
// FR-2.1: "Batched textured quads; one draw call per (atlas, material,
// blend) group per frame; GPU-instanced; supports rotation, scale,
// tint, per-sprite depth, UV sub-rect". RENDER-001: batch + instance;
// state changes and draw submissions observable (the counters below —
// the M2-SPRITE-04 profiler feed). This is the frame pipeline's
// SUBMIT stage (M2-GL-02): it consumes the M2-SPRITE-01 batcher's
// built frame and makes exactly ONE instanced draw call per
// (atlas, material, blend) group — the per-group state (one texture
// bind, one blend function) is set once, so state changes are
// O(group count), not O(sprite count).
//
//   SpriteDrawStats    The per-frame counters (the last successful
//                      submit)
//   SpriteDrawTotals   The since-construction counters
//   SpriteRenderer     The submit stage: one program, one quad VBO,
//                      one per-frame instance buffer, the atlas
//                      registry; one owner (the render thread)
//
// ---------------------------------------------------------------------------
// The frame protocol (the submit stage's half)
// ---------------------------------------------------------------------------
//
// The render thread (M2-GL-02) owns one SpriteRenderer per sprite
// pass. Per frame, AFTER the batch stage's beginFrame()/add()/build():
//
//   submit(batcher, worldToNdc)
//
// where worldToNdc is the frame's combined world -> NDC matrix
// (IsoCamera::matrix() — M2-CAM-02 — or the M2-PROJ-01 view's
// combined matrix; RENDER-006: the conversion boundary is the matrix,
// nothing render-side leaks into the sim). submit() validates the
// frame (the batcher built for this frame — frameBuilt(), the
// instance budget, every group's atlas bound), packs the frame's
// instances into the pre-allocated instance buffer (52 B per
// instance — no per-frame allocation, FR-2.2/PERF-003), uploads it
// (ONE glBufferSubData), and draws: one glDrawArraysInstanced per
// group, in the batcher's published group order (ascending
// (atlas, material, blend) — RENDER-003), each group's instances in
// the batcher's in-group back-to-front order.
//
// The submit stage owns no frame state of its own: the frame's
// content is the batcher's built batches; the only per-frame GL work
// is the one upload + the per-group state/draw. The per-frame
// counters (SpriteDrawStats) are reset at the start of every submit —
// a FAILED submit zeroes them (read frameStats() for the last
// successful submit).
//
// ---------------------------------------------------------------------------
// The shader (minimal GLSL 3.30 — the whole M2 sprite feature)
// ---------------------------------------------------------------------------
//
// Vertex: the unit quad (4-vertex strip, per-vertex aCorner in
// [-0.5, 0.5]^2) is scaled by the per-instance WORLD-unit scale and
// translated to the per-instance world position, projected through
// uWorldToNdc (2D ground plane: z = 0), and the projected offset is
// rotated by the per-instance rotation IN SCREEN SPACE (NDC; the
// SpriteItem.rotation contract — M2-SPRITE-01). The per-vertex UV is
// the per-instance UV sub-rect mapped onto the quad. Fragment:
// texture(uAtlas, uv) * tint, blended per the per-group BlendMode
// (Alpha: standard SRC_ALPHA/ONE_MINUS_SRC_ALPHA; Additive:
// ONE/ONE). Sprites are painted back-to-front (the batcher's order)
// with the DEPTH TEST DISABLED for the sprite pass — the 2.5D depth
// is engine-owned (FR-2.2 / M2-ISO-01), never derived from the
// projection.
//
// The GPU's cos/sin (the rotation) is driver-float: bit-exact across
// runs of the same driver, not across drivers — the golden-image
// contract (M2-GOLD-01) pins the environment; this step's tests
// verify a frame against a CPU reference with a ±1-byte tolerance
// (the GPU float32 vs the reference double) plus exact probes.
//
// ---------------------------------------------------------------------------
// The pass's GL state model (what submit establishes per frame)
// ---------------------------------------------------------------------------
//
// submit() establishes, in order: the render-target frame buffer
// (GlContext::frameBuffer() — the offscreen FBO on headless contexts;
// the surfaceless default frame buffer is not a valid draw target),
// the viewport matched to the render-target size (the driver default
// is 0x0 — every draw would clip to nothing), the DEPTH TEST OFF
// (the painter's order is the batcher's — the 2.5D depth is
// engine-owned, FR-2.2), the BLEND ENABLED, the sprite program + the
// per-frame uWorldToNdc uniform, and each group's atlas on texture
// unit 0. After the pass (success or failure): the program and the VAO
// are restored to 0 (the pass owns only its own program/VAO); the
// blend function, the texture bind, the frame buffer, and the viewport
// PERSIST (the next pass re-establishes what it needs — the state
// changes are counted only when they actually happen: textureBinds /
// blendChanges carry across frames).
//
// ---------------------------------------------------------------------------
// Ownership, threading, lifetime (CONC-001, ARCH-009)
// ---------------------------------------------------------------------------
//
// Move-only. The default state is stopped (the failed create /
// moved-from — every operation fails InvalidArgument, no logging).
// The renderer is created on ONE GlContext and holds a NON-OWNING
// pointer to it: the context MUST outlive the renderer (the engine
// creates the context first, destroys it last; destroying the
// renderer first is the required shutdown order — the GPU resources
// are deleted through the context). ONE owner thread (the render
// thread's submit stage — CONC-001); no locks, no atomics: the
// counters are plain reads/writes on the owner thread (DBG-005: the
// profiler reads them on the owner thread or from a snapshot taken
// there). Presentation-only (ARCH-009): submit() reads the batcher's
// built frame (presentation state) and the given matrix; it never
// mutates sim state.
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003, RENDER-001) — the submit hot path
// ---------------------------------------------------------------------------
//
// Per frame: O(G * 5 + n) CPU work (G = group count, n = frame
// instance count): the per-group instance attribute-pointer setup
// (5 calls), the state checks, one upload of n * 52 B, G draw calls. Zero
// allocation (the staging buffer and the GL buffers are sized at
// create, the frame budget maxInstances). The state changes are
// observable: textureBinds counts the glBindTexture calls in the
// pass, blendChanges the blend-function changes, drawCalls the
// instanced draws, instances the drawn instances — one of each per
// group at minimum (RENDER-001). The composite 50k render-CPU budget
// (PRD §8.1, M2-PERF-01) is measured with this stage; no standalone
// budgets.json entry here (the sort cost is the depth_sort_10k
// budget — M2-SORT-01).
//
// Misuse warnings:
//   - create the renderer with maxInstances >= the batcher's
//     maxSprites — a frame above the renderer's budget fails
//     BudgetExhausted (the batcher's own overflow policy bounds the
//     frame to ITS budget first);
//   - bindAtlas is the SETUP/asset path (one call per atlas per
//     scene load — the asset system streams them later, M3), not a
//     per-frame call: it creates a GL texture;
//   - submit requires the batcher BUILT for the current frame — an
//     open window with declared items fails InvalidArgument, it is
//     never drawn as an empty frame (CORE-008);
//   - the context must be valid; submit makes it current on the
//     calling thread (idempotent — the M2-GL-02 onStart hook does it
//     once, submit re-affirms it), a cross-thread live takeover
//     fails GlUnavailable (the P0 EGL stack contract).

#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "laige/render/gl_context.h"
#include "laige/render/matrices.h"
#include "laige/render/sprite_batcher.h"
#include "laige/result.h"

namespace laige::render {

// The per-frame counters of one successful submit (RENDER-001 — the
// state changes and draw submissions made observable; the
// M2-SPRITE-04 profiler feed). Read-only published state. `primitives`
// is 0 unless the opt-in primitive query (Options::primitiveQuery) is
// enabled: GL 3.3 core has no draw-call query primitive, so the
// dispatch count is the engine's own bookkeeping (drawCalls — what the
// profiler ships), and the GL-side cross-check is the
// PRIMITIVES_GENERATED count of the pass (2 per instance of the
// 4-vertex strip).
struct SpriteDrawStats {
  std::uint32_t drawCalls{0};     // instanced draw calls (== group count)
  std::uint32_t textureBinds{0};  // texture binds in the sprite pass
  std::uint32_t blendChanges{0};  // blend-function changes in the pass
  std::uint32_t instances{0};     // instances drawn this frame
  std::uint32_t primitives{0};    // PRIMITIVES_GENERATED (query on only)
};

// The since-construction totals (the frames counter counts successful
// submits only). The M2-SPRITE-04 profiler reads these on the owner
// thread (DBG-005).
struct SpriteDrawTotals {
  std::uint64_t frames{0};
  std::uint64_t drawCalls{0};
  std::uint64_t textureBinds{0};
  std::uint64_t blendChanges{0};
  std::uint64_t instances{0};
  std::uint64_t primitives{0};
};

// The submit stage of the frame pipeline (M2-SPRITE-02): one instanced
// draw call per (atlas, material, blend) group per frame (FR-2.1) +
// the minimal GLSL 3.30 sprite shader (the header preamble).
class SpriteRenderer {
 public:
  // The submit stage's configuration (API-006): both fields are
  // validated at create (the first failure wins, one Warn).
  struct Options {
    // The per-frame instance budget: the instance buffer is sized to
    // this at create (52 B per instance). Must be >= the batcher's
    // maxSprites. Domain [1, kSpriteRendererMaxInstances]; the
    // practical ceiling is the driver's GL buffer size — an
    // allocation that large fails create GlUnavailable (the clean
    // failure, CORE-008).
    std::uint32_t maxInstances{0};
    // The atlas-id domain: atlas ids are [0, maxAtlases) — the atlas
    // registry is a flat table (8 B per slot). Domain
    // [1, kSpriteRendererMaxAtlases]: 4096 slots = 32 KB — far beyond
    // the PRD's 50k-sprite scene (<= 30 draw calls = tens of
    // atlases at most).
    std::uint32_t maxAtlases{0};
    // The opt-in GL PRIMITIVES_GENERATED query around every submit
    // (the GL-side dispatch cross-check + the M2-SPRITE-04 primitive
    // feed). OFF by default: zero GL work, zero cost (the hot path,
    // PERF-002). ON: one query object per submit + a glFinish — a
    // CPU/GPU sync, a DIAGNOSTIC mode for the offscreen test/CI path
    // and the profiler, never the shipping frame loop (RENDER-005).
    bool primitiveQuery{false};
  };

  // The frame-slot index width (the batcher's slot domain, the
  // instance buffer's index domain).
  static constexpr std::uint32_t kSpriteRendererMaxInstances =
      0xFFFFFFFFu;
  // The atlas registry cap (the Options comment: 32 KB at 4096 slots).
  static constexpr std::uint32_t kSpriteRendererMaxAtlases = 4096u;

  // The stopped state (the failed create / moved-from): valid() is
  // false, every operation fails InvalidArgument, the counters read
  // zero. No logging.
  // Defined in the .cpp (the defaulted form would instantiate the
  // unique_ptr destructor against the incomplete Impl — the GlContext
  // precedent).
  SpriteRenderer() noexcept;
  ~SpriteRenderer();
  // Defined in the .cpp, where Impl is complete (the unique_ptr move
  // operations need the complete pointee — the GlContext precedent).
  SpriteRenderer(SpriteRenderer&&) noexcept;
  SpriteRenderer& operator=(SpriteRenderer&&) noexcept;
  SpriteRenderer(const SpriteRenderer&) = delete;
  SpriteRenderer& operator=(const SpriteRenderer&) = delete;

  // One-time setup (the engine's renderer construction, before the
  // frame pipeline starts — never inside a frame, PERF-002):
  // validates the options (first failure wins, one Warn
  // options_invalid), requires a valid context (makeCurrent,
  // idempotent), then compiles the sprite shader, creates the quad
  // VBO, the per-frame instance buffer (maxInstances * 52 B), the VAO,
  // and the atlas registry. Any GL failure returns GlUnavailable with
  // one structured Error event (no partial renderer: the failure is
  // the stopped state).
  // @budget one-time: two shader compiles, one link, three GPU
  // allocations (52 * maxInstances B instance buffer), no loop.
  [[nodiscard]] static laige::Result<SpriteRenderer, laige::ErrorCode>
  create(const GlContext& ctx, Options options) noexcept;

  // True when the create succeeded (the stopped state is false).
  [[nodiscard]] bool valid() const noexcept;

  // The validated configuration (the stopped state reads zero).
  // O(1), no allocation.
  [[nodiscard]] std::uint32_t maxInstances() const noexcept;
  [[nodiscard]] std::uint32_t maxAtlases() const noexcept;

  // Bind one atlas texture from CPU RGBA8 bytes — the SETUP/asset
  // path (one call per atlas per scene load; the asset system
  // re-uploads them later, M3 — RENDER-004: no unexpected GPU work
  // in the frame hot path). The context must be current on the
  // calling thread (makeCurrent, idempotent). The atlas id is the
  // batcher's SpriteItem.atlasId domain: [0, maxAtlases). width/height
  // are [1, the context's maxTextureSize]; rgba must be exactly
  // width * height * 4 bytes. The texture is uploaded GL_RGBA8,
  // GL_NEAREST, GL_CLAMP_TO_EDGE, no mipmaps (the sub-rects are
  // pixel-exact — the M2-GOLD-01 contract). Re-binding an id
  // REPLACES the texture (the old one is deleted — the last bind
  // wins). Validation failures return InvalidArgument (no GL work, no
  // logging — the precondition contract, the GlContext::readPixel
  // precedent); a GL failure returns GlUnavailable + one Error.
  // @budget one-time per atlas: one glTexImage2D (width*height*4 B
  // upload); no loop, no allocation beyond the texture object.
  [[nodiscard]] laige::Status bindAtlas(std::uint32_t atlasId,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        std::span<const std::uint8_t> rgba);

  // The submit stage: draw the batcher's BUILT frame (the frame
  // protocol, the header preamble). Preconditions (checked in order,
  // the first failure wins — a failed submit leaves the frame
  // counters ZERO and the since-construction totals UNCHANGED and
  // draws nothing; the pass re-establishes its own GL state at the
  // start of every successful submit, so a failure leaves no stale
  // sprite-pass state for the next frame):
  //   1. the renderer valid (stopped -> InvalidArgument, no logging);
  //   2. the context valid + current (makeCurrent — idempotent; a
  //      cross-thread live takeover -> GlUnavailable, the P0 EGL
  //      contract);
  //   3. the batcher built for the current frame
  //      (batcher.frameBuilt() — an open window is InvalidArgument:
  //      declared items are never silently dropped, CORE-008);
  //   4. the frame's instance count <= maxInstances
  //      (else BudgetExhausted + one rate-limited Warn
  //      instance_capacity — the budget, PERF-008);
  //   5. every group's atlas in the registry and bound (else
  //      InvalidArgument — the stateless pre-state validation, one
  //      failure per frame).
  // The frame's world -> NDC matrix (the IsoCamera / M2-PROJ-01
  // combined matrix — RENDER-006) is the per-frame uniform.
  // The batcher reference is non-const only because the batcher's
  // pool accessors are (the M2-SPRITE-01 house quirk); submit mutates
  // nothing on the batcher.
  // @budget O(G * 5 + n) CPU (G groups, n instances), one upload of
  // n * 52 B, G draw calls, zero allocation — the header Performance
  // section.
  [[nodiscard]] laige::Status submit(SpriteBatcher& batcher,
                                     const Mat4& worldToNdc);

  // The per-frame counters of the last SUCCESSFUL submit (a failed
  // submit reads zero — it zeroes them). O(1).
  [[nodiscard]] SpriteDrawStats frameStats() const noexcept;

  // The since-construction totals (successful submits only). O(1).
  [[nodiscard]] SpriteDrawTotals totals() const noexcept;

 private:
  struct Impl;
  // Construction goes through create() only; the GL resources are
  // born owned (deleted through the context in the destructor — the
  // context outlives the renderer, the Ownership section).
  explicit SpriteRenderer(std::unique_ptr<Impl>) noexcept;

  std::unique_ptr<Impl> impl_;  // empty <=> stopped
};

}  // namespace laige::render
