// laige-render atlas frame layout (M2-SPRITE-03): the atlas UV frame
// animation hook.
//
// FR-2.1: "Batched textured quads; ... supports rotation, scale, tint,
// per-sprite depth, UV sub-rect, atlas UV animation (sheet frames)."
// This step ships the DATA-DRIVEN half of "atlas UV animation": the
// atlas sheet frame LAYOUT (frame size, row/col, margins — documented
// below) and the pure frame-index → UV sub-rect computation that turns
// a caller-set animation frame index into the `SpriteItem.uv` the
// M2-SPRITE-02 shader consumes. This is the hook M3 animation drives:
// for now the frame index is set by the caller per frame (the game's
// declaration pipeline); M3 will own the frame-advance policy on top
// of this same layout.
//
//   SpriteFrameLayout  The atlas sheet's frame layout (texels)
//   spriteFrameUv      The frame index → UV sub-rect function
//
// ---------------------------------------------------------------------------
// The sheet model (the documented layout)
// ---------------------------------------------------------------------------
//
// A sprite sheet is a rows × columns grid of frames, ROW-MAJOR, frame
// 0 at the top-left:
//
//   frame i:  col = i % columns,  row = i / columns
//
// Each frame is `frameWidth` × `frameHeight` texels. `frameSpacing` is
// the gap BETWEEN adjacent frames (texels); `sheetBorder` is the margin
// between the sheet edge and the first/last frame on every side
// (texels). Frame (col, row) occupies the texel rect
//
//   x: [border + col·(fw + spacing), border + col·(fw + spacing) + fw)
//   y: [border + row·(fh + spacing), border + row·(fh + spacing) + fh)
//
// — the gap lives to the RIGHT of each frame and BELOW each row, and a
// TIGHT sheet is exactly
//
//   width  = 2·border + columns·fw + (columns − 1)·spacing
//   height = 2·border + rows·fh    + (rows − 1)·spacing
//
// (the last column's/row's gap is absorbed into the border). A WIDER
// atlas is fine — the extra slack is just margin; the fit check in
// `spriteFrameUv` (every frame's rect inside the atlas) is the
// authoritative validity rule.
//
// The UV rect is the frame's texel rect normalized into the
// `atlasWidth × atlasHeight` atlas: `u0 = x0 / W`, `v0 = y0 / H`,
// `u1 = x1 / W`, `v1 = y1 / H` — the `SpriteUvRect` of
// `sprite_batcher.h` (u1 > u0, v1 > v0). V-axis convention: v = 0 is
// the FIRST texel row of the uploaded RGBA array (the sheet's TOP row —
// the M2-SPRITE-02 `GL_NEAREST` "texel row = floor(v·h)" contract), so
// frame row 0 (the sheet's top row) carries the smallest v, and a
// frame's v rect is measured from the top, not the bottom.
//
// ---------------------------------------------------------------------------
// Exactness (the float-exact domain)
// ---------------------------------------------------------------------------
//
// All texel coordinates are exact in float — and the invariant
// u1 > u0, v1 > v0 holds EXACTLY (two distinct k/W values never round
// to the same float) — when the atlas dimensions are within
// [1, kSpriteFrameMaxAtlasTexels] = [1, 2^24] (16 777 216 — far beyond
// the practical driver maxTextureSize of 16K–32K): every pixel
// coordinate is then < 2^24, exactly representable in float, and the
// UV corners are the EXACTLY-rounded k/W values. Outside the domain the
// call fails `InvalidArgument` (the domain is part of the layout
// contract — a sheet that does not fit a float-exact atlas is an
// invalid layout, not a degraded one).
//
// The conversion is a PURE function (no allocation, no logging, no
// state, no GL): a handful of integer checks + 4 float divisions,
// bit-identical on every platform/build (render-side float — the
// ARCH-009/010 presentation-only scope; never in the sim state hash).
//
// ---------------------------------------------------------------------------
// Failure (CORE-008, API-008) — first failure wins
// ---------------------------------------------------------------------------
//
// The layout is CALLER-OWNED asset metadata (the game's import pipeline
// for now, the asset system from M3 — untrusted input, SCALE-004), so
// every field is validated at the use boundary:
//
//   1. frameWidth, frameHeight, columns, rows must be >= 1
//      (InvalidArgument — a zero extent is an invalid layout, not an
//      "empty" one);
//   2. atlasWidth, atlasHeight must be within [1, 2^24]
//      (InvalidArgument — the float-exact domain above);
//   3. frameIndex < columns·rows (InvalidArgument — the documented
//      OUT-OF-RANGE contract: the engine never wraps silently; a
//      wrapping game computes the index itself);
//   4. the frame's texel rect fits the atlas (InvalidArgument — the
//      layout does not describe the sheet it claims to).
//
// A failed call returns the error; it never produces a UV rect.
//
// ---------------------------------------------------------------------------
// Ownership, threading (CORE-009, CONC-001)
// ---------------------------------------------------------------------------
//
// Stateless: `SpriteFrameLayout` is a plain value (no ownership,
// nothing to release) and `spriteFrameUv` is a free function — no
// owner, no locks, callable from any thread (the declaration pipeline
// calls it once per animated sprite per frame). The `SpriteItem`
// carries the result: `frameIndex` (the declared animation frame —
// carried through the batcher untouched) and `uv` (the frame's UV
// rect — what the renderer draws). The caller's invariant: `uv` is
// this item's `frameIndex` under its atlas's layout (M3 keeps it).
//
// Canonical narrative: docs/api/sprite_frames.md (the full contract);
// the sheet model's place in the sprite pipeline:
// docs/concepts/coordinates.md §4.8.

#pragma once

#include <cstdint>

#include "laige/errors.h"
#include "laige/render/sprite_batcher.h"  // SpriteUvRect
#include "laige/result.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The float-exact atlas domain (the header's Exactness section):
// atlas dimensions are [1, kSpriteFrameMaxAtlasTexels]. 2^24 =
// 16 777 216 — every texel coordinate < 2^24 is exactly
// float-representable, and two distinct k/W values never round to the
// same float (the u1 > u0, v1 > v0 invariant, exact). Far beyond the
// practical driver maxTextureSize (16K–32K).
inline constexpr std::uint32_t kSpriteFrameMaxAtlasTexels = 1u << 24;

// ---------------------------------------------------------------------------
// The atlas sheet's frame layout (texel units)
// ---------------------------------------------------------------------------

// The layout of the frames in one atlas sheet (the header's sheet
// model). All fields are texels (not world units, not normalized UV —
// the layout is asset-side data, the import pipeline's output for now,
// the asset system's from M3). A plain value: no ownership, nothing to
// release; validated at use time by spriteFrameUv (the failure
// section).
struct SpriteFrameLayout {
  // The frame width in texels (>= 1).
  std::uint32_t frameWidth{};
  // The frame height in texels (>= 1).
  std::uint32_t frameHeight{};
  // The frame columns per row (>= 1).
  std::uint32_t columns{};
  // The frame rows (>= 1).
  std::uint32_t rows{};
  // The gap between adjacent frames, texels (0 = packed).
  std::uint32_t frameSpacing{};
  // The margin between the sheet edge and the first/last frame on
  // every side, texels (0 = flush to the edge).
  std::uint32_t sheetBorder{};
};

// ---------------------------------------------------------------------------
// The frame index → UV sub-rect computation
// ---------------------------------------------------------------------------

// The UV sub-rect of `frameIndex` in the `atlasWidth × atlasHeight`
// atlas, under `layout` (the header: the sheet model, the exactness
// domain, the failure contract). The frame index is 0-based row-major
// (frame 0 = top-left); an out-of-range index fails
// `InvalidArgument` — the engine NEVER wraps silently (CORE-008).
//
// @budget O(1): a handful of integer checks + 4 float divisions; zero
// allocation; no logging, no GL. One call per animated sprite per
// frame in the declaration pipeline — never per texel.
[[nodiscard]] inline Result<SpriteUvRect> spriteFrameUv(
    std::uint32_t frameIndex, const SpriteFrameLayout& layout,
    std::uint32_t atlasWidth, std::uint32_t atlasHeight) noexcept {
  // 1. The layout's extents (the header's failure section — first
  //    failure wins).
  if (layout.frameWidth == 0 || layout.frameHeight == 0 ||
      layout.columns == 0 || layout.rows == 0) {
    return Result<SpriteUvRect>::failure(ErrorCode::InvalidArgument);
  }
  // 2. The float-exact domain ([1, 2^24]).
  if (atlasWidth == 0 || atlasWidth > kSpriteFrameMaxAtlasTexels ||
      atlasHeight == 0 || atlasHeight > kSpriteFrameMaxAtlasTexels) {
    return Result<SpriteUvRect>::failure(ErrorCode::InvalidArgument);
  }
  // 3. The frame index: no silent wrap (CORE-008). The product is
  //    u64 (columns·rows can exceed 2^32 for adversarial layouts).
  const std::uint64_t frameCount =
      static_cast<std::uint64_t>(layout.columns) * layout.rows;
  if (frameIndex >= frameCount) {
    return Result<SpriteUvRect>::failure(ErrorCode::InvalidArgument);
  }
  const std::uint32_t col = frameIndex % layout.columns;
  const std::uint32_t row = frameIndex / layout.columns;
  // The frame stride (u64: frameWidth + frameSpacing can exceed 2^32
  // for adversarial layouts).
  const std::uint64_t strideX =
      static_cast<std::uint64_t>(layout.frameWidth) + layout.frameSpacing;
  const std::uint64_t strideY =
      static_cast<std::uint64_t>(layout.frameHeight) + layout.frameSpacing;
  // 4. The fit — and the overflow guard: with col > 0 the origin is at
  //    least strideX, so a stride beyond the atlas already fails the
  //    fit; reject BEFORE the col·stride multiplication (which can
  //    wrap u64 for adversarial strides — unsigned wrap is well-defined
  //    but a wrapped origin could pass the fit check with the WRONG
  //    frame, CPP-004 / SCALE-004: the layout is untrusted metadata).
  if (col > 0 && strideX > atlasWidth) {
    return Result<SpriteUvRect>::failure(ErrorCode::InvalidArgument);
  }
  if (row > 0 && strideY > atlasHeight) {
    return Result<SpriteUvRect>::failure(ErrorCode::InvalidArgument);
  }
  // The pixel origin (now < 2^56: col < 2^32, stride <= atlas <= 2^24
  // on this path — no u64 wrap).
  const std::uint64_t x0 =
      static_cast<std::uint64_t>(layout.sheetBorder) +
      static_cast<std::uint64_t>(col) * strideX;
  const std::uint64_t y0 =
      static_cast<std::uint64_t>(layout.sheetBorder) +
      static_cast<std::uint64_t>(row) * strideY;
  if (x0 + static_cast<std::uint64_t>(layout.frameWidth) > atlasWidth ||
      y0 + static_cast<std::uint64_t>(layout.frameHeight) > atlasHeight) {
    return Result<SpriteUvRect>::failure(ErrorCode::InvalidArgument);
  }
  // Past the fit: x0 + frameWidth <= atlasWidth <= 2^24, so the u32
  // casts are lossless and the float conversions exact (the header's
  // Exactness section).
  const std::uint32_t x1 = static_cast<std::uint32_t>(
      x0 + static_cast<std::uint64_t>(layout.frameWidth));
  const std::uint32_t y1 = static_cast<std::uint32_t>(
      y0 + static_cast<std::uint64_t>(layout.frameHeight));
  const float w = static_cast<float>(atlasWidth);
  const float h = static_cast<float>(atlasHeight);
  return Result<SpriteUvRect>::success(SpriteUvRect{
      static_cast<float>(static_cast<std::uint32_t>(x0)) / w,
      static_cast<float>(static_cast<std::uint32_t>(y0)) / h,
      static_cast<float>(x1) / w,
      static_cast<float>(y1) / h});
}

}  // namespace laige::render
