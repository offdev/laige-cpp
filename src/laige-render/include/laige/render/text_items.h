// laige-render text items (M2-TEXT-02): the text half of FR-2.8 —
// screen-space text items for the UI pass.
//
// FR-2.8: "Text rendering: ... bitmap font atlas (P0) ...; UI is a
// separate render pass, screen-space." The P0 bitmap half (M2-TEXT-01)
// owns the font rasterization (`laige/render/font.h` — the
// `GlyphAtlas`). This header is the UI pass's text side: the
// `StringTable` (PRD §10.4 — strings live in an engine-owned table with
// `StringRef` handles; no per-frame `std::string` in the path), the
// `TextItem` value (string, font, scale, color, alignment, max-width
// wrap), the O(glyphs) width measurement, and the `declareText` pass
// that turns one `TextItem` into batched glyph quads in the UI pass's
// sprite batcher (S-5: rendering goes through the batcher — text is
// never drawn directly). The M2-UI-01 widget tree declares its text
// widgets through this same pass; the M2-UI-02 render pass submits the
// UI batcher through the M2-SPRITE-02 `SpriteRenderer` (identity
// screen-space matrix — RENDER-006).
//
//   StringRef               The string-table handle (PRD §10.4)
//   StringTable             The engine-owned interned string storage
//   TextAlignment           The per-line alignment (left/center/right)
//   TextItem                One declared text item (UI pass, screen px)
//   measureText             The O(glyphs) width measurement
//   declareText             The O(glyphs) text -> glyph-quads pass
//   expandGlyphAtlasRgba8   The 8-bit alpha atlas -> white-on-alpha
//                           RGBA8 upload bytes (the M2-SPRITE-02
//                           `bindAtlas` input)
//
// ---------------------------------------------------------------------------
// The model (documented units)
// ---------------------------------------------------------------------------
//
// All layout values are SCREEN PIXELS, screen space, y DOWN (the UI
// pass's coordinate system — the M2-UI-02 identity screen matrix maps
// (x, y) to NDC; nothing world-space leaks in, RENDER-006). The font's
// metrics are px at 1x (M2-TEXT-01); `TextItem.scale` is an INTEGER
// scale factor in [1, kTextMaxScale] (the M2-TEXT-01 "integer scale"
// convention: every metric is multiplied by the scale — exact, no
// fractional-pixel rasterization), so every layout coordinate is an
// exact integer before the one presentation-only float conversion to
// the `SpriteItem` fields (ARCH-009).
//
// The text is a sequence of UNICODE CODE POINTS (one u32 per code
// point — the `glyph(code)` domain, M2-TEXT-01; UTF-8/UTF-16 sources
// are converted to this representation once, at string load, never
// per frame — PRD §10.4). `StringTable` stores exactly that: u32 code
// points in a fixed pool.
//
// The text block's ANCHOR is `TextItem.x` / `TextItem.y`:
//
//   - `y` is always the FIRST LINE'S BASELINE (screen px, y down).
//   - `x` is, by `TextItem.alignment`: the text block's LEFT edge
//     (`Left`), its CENTER x (`Center`), or its RIGHT edge (`Right`).
//     Each line is aligned independently against that anchor (the
//     standard paragraph model — every line's own width is centered /
//     right-justified, not the longest line's).
//
// Line step: `fontSize() * scale` px (the M2-TEXT-01 line height —
// `lineHeight() == fontSize()` by the stbtt pixel-height convention).
//
// ---------------------------------------------------------------------------
// The glyph quads (the M2-SPRITE-02 quad model)
// ---------------------------------------------------------------------------
//
// One `SpriteItem` per GLYPH WITH INK (width > 0 && height > 0 at the
// scale — the empty glyph (the space, the zero-ink fallback) declares
// NOTHING but its advance still applies to the pen). The quad:
//
//   pen         walks the line in code-point order; a glyph's pen
//               advance is `glyph->advance * scale` (exact integer);
//   ink rect    [pen + bearingX·scale,  pen + bearingX·scale +
//               width·scale) × [baseline − bearingY·scale, baseline −
//               bearingY·scale + height·scale) — screen px, y down
//               (bearingY > 0 = the ink's top edge is ABOVE the
//               baseline — the M2-TEXT-01 bearing convention; a
//               descender has bearingY < 0 and hangs below it);
//   quad        the M2-SPRITE-02 centered-quad model:
//               `pos = (inkLeft + w/2, inkTop + h/2)`,
//               `scale = (w, h)` — the quad's ±0.5·scale spans exactly
//               the ink rect;
//   uv          the glyph cell's documented rect (M2-TEXT-01):
//               (atlasX/W, atlasY/H, (atlasX+w)/W, (atlasY+h)/H) — the
//               exact-float domain of M2-SPRITE-03 (atlas ≤ 2^24
//               texels; u1 > u0, v1 > v0 exact);
//   tint        the item's color (SpriteTint — the M2-SPRITE-02
//               multiplicative tint: the white-on-alpha upload makes
//               the drawn color = tint × glyph alpha, the standard
//               text coloring);
//   depthKey    the item's hand-tuned UI z (the G-R11 escape hatch:
//               `declareText` sets `depthOverride = true` — counted +
//               warned at build, the sprite_batcher.h G-R11 contract).
//               The UI pass is screen-space: there is no isoDepthKey
//               in this pass — manual UI z is the documented order
//               value, not a workaround for a world key.
//   rotation    0 (per-glyph rotation is not a text feature in M2),
//   frameIndex  0 (glyph animation is not a feature in M2 — the SDF
//               pass of M2-TEXT-03 changes the shader, not this hook).
//
// Declaration order is the ROW-MAJOR order (line by line, code point by
// code point): the UI pass's deterministic order (RENDER-003) — all
// glyphs of one item share the same depthKey, so the batcher's stable
// sort keeps this exact declaration order within the group.
//
// ---------------------------------------------------------------------------
// The wrap model (word wrap at max width)
// ---------------------------------------------------------------------------
//
// `TextItem.maxWidth` is 0 (NO wrap — the whole text is one line) or a
// width in [1, kTextMaxLineWidthPx] screen px. When wrapping is on:
//
//   - a WORD is a maximal run of non-space code points; U+0020 SPACE
//     is the ONLY word separator (U+00A0 no-break space and other
//     "whitespace" are ordinary characters — the Latin-1 convention;
//     the font's own space glyph is what breaks);
//   - a word is placed on the current line when it fits:
//     `lineWidth + spaceAdvance + wordWidth <= maxWidth` (a first word
//     fits when `wordWidth <= maxWidth`);
//   - consecutive spaces between two words on one line collapse to ONE
//     space advance in both the width and the pen (the extra spaces
//     occupy no ink — the standard layout behavior);
//   - a word WIDER than `maxWidth` (and a line that is still empty) is
//     SPLIT character-by-character: the greedy longest character prefix
//     that fits goes on the line, the rest continues on the next line;
//   - a SINGLE CHARACTER wider than `maxWidth` gets its own line whose
//     width exceeds `maxWidth` (the only way to draw it — never
//     dropped, never clipped — the documented oversized behavior);
//   - leading spaces of a line (after a wrap) and trailing spaces of
//     the text occupy no line and no width.
//
// The LINE WIDTH is the ink-block width: the sum of the line's glyph
// advances (words + one space advance per inter-word gap), excluding
// leading/trailing spaces. `measureText` returns exactly this width
// for the no-wrap text — the panel-sizing measurement (M2-UI-01).
//
// ---------------------------------------------------------------------------
// The screen domain (the layout's exact-float boundary)
// ---------------------------------------------------------------------------
//
// All layout arithmetic is done in EXACT integer (int64) math; the
// one conversion to render floats happens on the declared quad fields
// (the ARCH-009 presentation boundary). The domain guard: a line wider
// than `kTextMaxLineWidthPx` (= 2^20 px, a 20-bit screen dimension —
// far beyond any UI panel) is UNREPRESENTABLE: `declareText` fails
// `InvalidArgument` and declares NOTHING (the pre-check pass walks
// every line before the first add — a failed call leaves the frame
// untouched). Inside the domain every coordinate is < 2^24 px — exact
// in float (the M2-SPRITE-03 float-exact domain).
//
// ---------------------------------------------------------------------------
// The string table (PRD §10.4)
// ---------------------------------------------------------------------------
//
// `StringTable` is the engine-owned string storage: a fixed pool of u32
// code points + a header per string (offset, length). Strings are
// INTERNED (the setup/asset path — scene load, not the frame path):
// `intern` copies the code points into the pool once and returns a
// stable `StringRef` handle; the same bytes interned again return the
// SAME handle (dedup — the table's identity property); `text(ref)`
// hands back the span (no copy, no allocation). The frame path reads
// only the handle + the span (no per-frame `std::string` — PRD §10.4).
//
// Bounded by construction (PERF-008): `maxStrings` / `maxCodePoints`
// are fixed at `create`; an `intern` beyond either bound fails
// `BudgetExhausted` + one rate-limited Warn `string_table/table_full`
// (never grows, never throws — the pool's budget contract).
//
// ---------------------------------------------------------------------------
// Ownership, threading (CORE-009, CONC-001)
// ---------------------------------------------------------------------------
//
// `StringTable` is move-only (the `SpriteBatcher` precedent): one owner
// (the scene set-up thread); the render phase reads it (`declareText`
// / `measureText` read the spans) — the table is never shared with a
// concurrent writer (the single-owner pattern). `GlyphAtlas` is the
// scene's (M2-TEXT-01): a `TextItem.font` is a NON-OWNING pointer that
// MUST outlive every frame that declares through it (the scene owns
// the atlas, the frame owns the items). `TextItem` is a plain value
// (no ownership, nothing to release). Presentation-only (ARCH-009):
// none of this is sim state (no state-hash / replay involvement).
// No GL calls anywhere in this header (the GL half is the existing
// M2-SPRITE-02 `SpriteRenderer::submit` + `bindAtlas`).
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003/004, DOC-004)
// ---------------------------------------------------------------------------
//
//   StringTable::create  O(maxStrings + maxCodePoints): two
//                        allocations (the header table + the pool).
//                        Setup path only.
//   StringTable::intern  O(count · len) (the dedup scan — a linear
//                        memcmp over the live strings) + O(len) copy.
//                        SETUP PATH (scene load) — never per frame;
//                        the frame path is `text` (O(1)) +
//                        `declareText` (below).
//   StringTable::text    O(1); no allocation.
//   measureText          O(glyphs): one pass (the shared layout walk),
//                        no allocation, no logging, no GL.
//   declareText          O(glyphs): the line walk twice (the pre-check
//                        + the emit — both the same `nextLine`
//                        function) + one batcher add per ink glyph (the
//                        add is O(1)). No allocation, no logging on
//                        the happy path (the batcher's G-R11 warn at
//                        build is the batcher's own cold, rate-limited
//                        path — LOG-003/004), no GL.
//   expandGlyphAtlasRgba8 O(atlas W·H): four byte writes per texel.
//                        SETUP path (once per font per scene load —
//                        before the one-time `bindAtlas` upload —
//                        RENDER-004: no unexpected GPU work in the
//                        frame hot path).
//
// Call site: once per frame per text item, in the RENDER phase (the
// M2-GL-02 cull/batch stage of the UI pass) — never in the simulation
// tick (ARCH-002). The per-frame text cost is part of the composite
// 50k render-CPU budget (PRD §8.1 `sprites_50k_cpu` — measured with
// M2-PERF-01; no standalone budgets.json entry for this step).
//
// ---------------------------------------------------------------------------
// Failure (CORE-008, API-008 — first failure wins)
// ---------------------------------------------------------------------------
//
//   - StringTable::create: maxStrings / maxCodePoints outside
//     [1, 2^24] → InvalidArgument (no allocation, no log — the
//     create-validation precedent);
//   - StringTable::intern: stopped table → InvalidArgument (no log);
//     beyond either bound → BudgetExhausted + one rate-limited Warn
//     `string_table/table_full` (fields: `limit`, `capacity`);
//   - measureText: stopped/missing font, invalid scale, or an invalid
//     handle → InvalidArgument (no log — the precondition contract);
//   - declareText: stopped/missing font, invalid scale, an invalid
//     handle, an out-of-domain maxWidth, or a line wider than
//     kTextMaxLineWidthPx → InvalidArgument, NOTHING declared (the
//     pre-check runs before the first add — no partial text); a
//     failed batcher add (stopped batcher → BudgetExhausted, closed
//     window → InvalidArgument) fails the call with the batcher's
//     error — the items already declared by the call remain in the
//     frame (the batcher owns the frame state — the
//     `declareParticles` precedent); the batcher's own overflow policy
//     (drop oldest + warn) bounds a frame that declares too many
//     glyphs (PERF-008);
//   - expandGlyphAtlasRgba8: stopped atlas or a wrong-size span →
//     InvalidArgument (no GL work, no log).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
// - Do not intern per frame (PRD §10.4): `intern` is the setup path;
//   the frame path is the handle + `text` span + `declareText`.
// - Do not point `TextItem.font` / `TextItem.text` at dead storage:
//   the atlas and the table are scene-owned and must outlive every
//   frame that declares the item (the lifetime contract above).
// - Do not size a batcher for the world pass only: the UI pass is its
//   OWN batcher (one batcher per sprite pass, M2-SPRITE-01), but size
//   it for the frame's total glyph count (a 100-character label at
//   4x scale is ~100 quads).
// - Do not expect sub-pixel text: the scale is INTEGER (the
//   M2-TEXT-01 convention); fractional scales are out of scope in M2
//   (the SDF pass, M2-TEXT-03, is the zoom-crispness path).
//
// Canonical narrative: docs/concepts/coordinates.md §4.12 (ARCH-008);
// API contract: docs/api/text_items.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/render/font.h"
#include "laige/render/sprite_batcher.h"
#include "laige/result.h"

namespace laige::render {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The string-table pool domains: [1, 2^24] each (16 777 216 strings /
// code points — 64 MiB of pool at the ceiling, far beyond any UI
// string budget; the 24-bit width is the index domain of the header
// table).
inline constexpr std::uint32_t kStringTableMaxStrings = 1u << 24;
inline constexpr std::uint32_t kStringTableMaxCodePoints = 1u << 24;
// The documented defaults (a typical UI scene: a few hundred distinct
// strings, a few thousand code points — 64 KiB of pool).
inline constexpr std::uint32_t kStringTableDefaultStrings = 4096;
inline constexpr std::uint32_t kStringTableDefaultCodePoints = 16384;

// The integer scale domain (the M2-TEXT-01 "integer scale" convention):
// [1, kTextMaxScale]. 16 × the 1024 px font ceiling is a 16 384 px
// line height — beyond any practical UI text (the layout math is
// int64; the 2^20 px line-width domain below is the representability
// guard).
inline constexpr std::uint32_t kTextMaxScale = 16;

// The line-width / max-width domain in screen px: [1, 2^20]. A 20-bit
// screen dimension — far beyond any UI panel, and < 2^24 so every
// layout coordinate inside the domain is exact in float (the
// M2-SPRITE-03 float-exact domain). A line wider than this is
// unrepresentable (the header's screen-domain section).
inline constexpr std::uint32_t kTextMaxLineWidthPx = 1u << 20;

// The ONLY word separator in the wrap model (U+0020 SPACE — the
// header's wrap section; U+00A0 and friends are ordinary characters).
inline constexpr std::uint32_t kTextSpaceCode = 0x20;

// ---------------------------------------------------------------------------
// The string-table handle (PRD §10.4)
// ---------------------------------------------------------------------------

// A stable handle into one `StringTable` (the engine-owned string
// storage). Plain value: the id is dense from 0, stable for the
// table's lifetime (the table never moves, frees, or reallocates
// after create — CORE-007). No generation counter is needed (nothing
// is ever destroyed — the pool is fixed at create).
struct StringRef {
  std::uint32_t id{0};
};

// ---------------------------------------------------------------------------
// The engine-owned string table (PRD §10.4)
// ---------------------------------------------------------------------------

// The fixed pool of interned u32 code-point strings (the header's
// model + failure + performance sections). Move-only (CORE-009); the
// default state is stopped (nothing owned; `intern` fails, `text`
// reads empty).
class StringTable {
 public:
  // The table budget (API-006): both fields in [1, kStringTableMax*]
  // (validated at create — first failure wins, InvalidArgument, no
  // allocation). `maxCodePoints` bounds the TOTAL code points stored
  // (across all strings); `maxStrings` the distinct string count.
  struct Options {
    std::uint32_t maxStrings{kStringTableDefaultStrings};
    std::uint32_t maxCodePoints{kStringTableDefaultCodePoints};
  };

  // The stopped state (default / moved-from): valid() false,
  // intern -> InvalidArgument, text reads empty, introspection zero.
  StringTable() noexcept = default;

  // The set-up path (scene load): two allocations (the 8 B header per
  // string + the 4 B pool per code point — the header's performance
  // section).
  [[nodiscard]] static laige::Result<StringTable, laige::ErrorCode>
  create(Options options) noexcept;

  StringTable(const StringTable&) = delete;
  StringTable& operator=(const StringTable&) = delete;
  // The move (the house pattern — the moved-from table is stopped:
  // its storage moves out with the target, and the scalars are
  // zeroed so the moved-from table reports the stopped state).
  StringTable(StringTable&& o) noexcept
      : maxStrings_(o.maxStrings_),
        maxCodePoints_(o.maxCodePoints_),
        count_(o.count_),
        stored_(o.stored_),
        headers_(std::move(o.headers_)),
        pool_(std::move(o.pool_)) {
    o.maxStrings_ = 0;
    o.maxCodePoints_ = 0;
    o.count_ = 0;
    o.stored_ = 0;
  }
  StringTable& operator=(StringTable&& o) noexcept {
    if (this != &o) {
      *this = StringTable(std::move(o));
    }
    return *this;
  }
  ~StringTable() noexcept = default;

  // True iff the table is live (create succeeded).
  [[nodiscard]] bool valid() const noexcept { return maxStrings_ != 0; }

  // Interns `codes` (u32 code points — the header's model section):
  // a copy lands in the pool, the handle is stable for the table's
  // lifetime. The same bytes already interned return the SAME handle
  // (the dedup — the identity property). An empty span interns the
  // empty string (a 0-width text).
  //
  // Fails (no partial state): stopped table → InvalidArgument (no
  // log); count at maxStrings or pool past maxCodePoints →
  // BudgetExhausted + one rate-limited Warn `string_table/table_full`
  // (fields `limit` = "strings" / "code_points", `capacity`).
  //
  // @budget O(count·len + len) (the dedup scan + the copy) — the
  // SETUP path (scene load), never per frame.
  [[nodiscard]] laige::Result<StringRef, laige::ErrorCode>
  intern(std::span<const std::uint32_t> codes) noexcept;

  // True iff `ref` names a live string (false in the stopped state or
  // for an out-of-range id). O(1).
  [[nodiscard]] bool isValid(StringRef ref) const noexcept {
    return ref.id < count_;
  }

  // The string's code points (a non-owning view into the pool — valid
  // until the table is destroyed). An invalid handle reads an empty
  // span (the total read — never UB). O(1); no allocation.
  [[nodiscard]] std::span<const std::uint32_t> text(
      StringRef ref) const noexcept {
    if (!isValid(ref)) return std::span<const std::uint32_t>{};
    const Header& h = headers_[ref.id];
    return std::span<const std::uint32_t>(pool_.get() + h.offset,
                                          h.length);
  }

  // The live string count (0 in the stopped state). O(1).
  [[nodiscard]] std::uint32_t count() const noexcept { return count_; }
  // The total code points stored (0 in the stopped state). O(1).
  [[nodiscard]] std::uint32_t storedCodePoints() const noexcept {
    return stored_;
  }
  // The create() budgets (0 in the stopped state). O(1).
  [[nodiscard]] std::uint32_t stringCapacity() const noexcept {
    return maxStrings_;
  }
  [[nodiscard]] std::uint32_t codePointCapacity() const noexcept {
    return maxCodePoints_;
  }

 private:
  // One interned string's pool slot (8 B — the header's performance
  // section).
  struct Header {
    std::uint32_t offset{};
    std::uint32_t length{};
  };

  explicit StringTable(std::uint32_t maxStrings,
                       std::uint32_t maxCodePoints) noexcept
      : maxStrings_(maxStrings),
        maxCodePoints_(maxCodePoints),
        headers_(std::make_unique<Header[]>(maxStrings)),
        pool_(std::make_unique<std::uint32_t[]>(maxCodePoints)) {}

  std::uint32_t maxStrings_{0};
  std::uint32_t maxCodePoints_{0};
  std::uint32_t count_{0};
  std::uint32_t stored_{0};
  std::unique_ptr<Header[]> headers_;
  std::unique_ptr<std::uint32_t[]> pool_;
};

inline laige::Result<StringTable, laige::ErrorCode>
StringTable::create(Options options) noexcept {
  // The create-validation (first failure wins, InvalidArgument, no
  // allocation, no log — the header's failure section).
  if (options.maxStrings < 1 ||
      options.maxStrings > kStringTableMaxStrings ||
      options.maxCodePoints < 1 ||
      options.maxCodePoints > kStringTableMaxCodePoints) {
    return laige::Result<StringTable, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  return laige::Result<StringTable, laige::ErrorCode>::success(
      StringTable(options.maxStrings, options.maxCodePoints));
}

inline laige::Result<StringRef, laige::ErrorCode>
StringTable::intern(std::span<const std::uint32_t> codes) noexcept {
  auto fail = [](laige::ErrorCode e, const char* limit,
                  std::uint32_t capacity) -> laige::Result<StringRef> {
    if (e == laige::ErrorCode::BudgetExhausted) {
      // One rate-limited warn per failure (LOG-003/004 — the facade
      // windows the repeats; the set-up path is low-volume anyway).
      LAIGE_LOG_WARN("string_table", "table_full",
                     "String table intern exceeded a pool bound",
                     laige::log::field("limit", std::string_view(limit)),
                     laige::log::field("capacity", capacity));
    }
    return laige::Result<StringRef>::failure(e);
  };
  if (maxStrings_ == 0) {  // the stopped state (no log)
    return laige::Result<StringRef>::failure(laige::ErrorCode::InvalidArgument);
  }
  // The dedup scan (the identity property; the header's performance
  // section: the setup path, never per frame).
  const std::size_t len = codes.size();
  for (std::uint32_t i = 0; i < count_; ++i) {
    const Header& h = headers_[i];
    if (h.length == len &&
        (len == 0 ||
         std::memcmp(pool_.get() + h.offset, codes.data(),
                     len * sizeof(std::uint32_t)) == 0)) {
      return laige::Result<StringRef>::success(StringRef{i});
    }
  }
  // The bounds (first failure wins — the string count, then the pool).
  if (count_ == maxStrings_) {
    return fail(laige::ErrorCode::BudgetExhausted, "strings", maxStrings_);
  }
  if (static_cast<std::uint64_t>(stored_) + static_cast<std::uint64_t>(len) >
      maxCodePoints_) {
    return fail(laige::ErrorCode::BudgetExhausted, "code_points",
                maxCodePoints_);
  }
  // The copy (len <= maxCodePoints fits the u32 header length exactly).
  const std::uint32_t length = static_cast<std::uint32_t>(len);
  if (len > 0) {
    std::memcpy(pool_.get() + stored_, codes.data(),
                len * sizeof(std::uint32_t));
  }
  headers_[count_] = Header{stored_, length};
  stored_ += length;
  return laige::Result<StringRef>::success(StringRef{count_++});
}

// ---------------------------------------------------------------------------
// The per-item presentation values (UI pass, screen px)
// ---------------------------------------------------------------------------

// The per-line alignment of the text block (the header's model
// section: the anchor semantics of TextItem.x).
enum class TextAlignment : std::uint8_t {
  // The anchor x is the block's LEFT edge (every line starts there).
  Left = 0,
  // The anchor x is the block's CENTER x (each line is centered).
  Center = 1,
  // The anchor x is the block's RIGHT edge (each line ends there).
  Right = 2,
};

// One declared text item (M2-TEXT-02): the value the game declares for
// one UI text widget (S-5: declaration, not draw). A plain value — no
// ownership (the `font` pointer and the `text` handle are NON-OWNING:
// the scene's GlyphAtlas and StringTable must outlive the frame, the
// header's ownership section). Screen pixels, y down (the header's
// model section).
struct TextItem {
  // The text's string-table handle (PRD §10.4 — no per-frame
  // std::string in the path).
  StringRef text{};
  // The glyph atlas (M2-TEXT-01) — non-owning (scene-owned; must
  // outlive the frame).
  const GlyphAtlas* font{};
  // The text block's anchor x, screen px (the header's model section:
  // left/center/right edge by `alignment`).
  float x{};
  // The FIRST line's baseline, screen px (y down).
  float y{};
  // The integer scale factor (1x = the font's rasterized size — the
  // M2-TEXT-01 convention). Domain [1, kTextMaxScale]; validated by
  // measureText / declareText (InvalidArgument, no log).
  std::uint32_t scale{1};
  // The text color (the M2-SPRITE-02 multiplicative tint over the
  // white-on-alpha upload — the drawn color = color × glyph alpha).
  // Default: white.
  SpriteTint color{};
  // The per-line alignment (default: left).
  TextAlignment alignment{TextAlignment::Left};
  // The max line width, screen px (0 = no wrap — one line). Domain 0
  // or [1, kTextMaxLineWidthPx]; validated by declareText
  // (InvalidArgument, no log).
  std::int32_t maxWidth{0};
  // The hand-tuned UI z (the G-R11 escape hatch — declareText sets
  // depthOverride = true; counted + warned at build, the
  // sprite_batcher.h G-R11 contract). The UI pass is screen-space:
  // this is the documented order value, not a world-key workaround.
  std::uint32_t depthKey{};
  // The glyph atlas's texture id (the scene's bindAtlas id —
  // M2-SPRITE-02).
  std::uint32_t atlasId{};
  // The material id (0 = the default material — the group key carries
  // it per FR-2.1).
  std::uint32_t materialId{};
  // The blend state (the group key's third field). Alpha is the text
  // convention (the standard alpha blend over the UI panels).
  BlendMode blend{BlendMode::Alpha};
};

// ---------------------------------------------------------------------------
// The shared layout walk (the pre-check and the emit use one code path)
// ---------------------------------------------------------------------------

namespace detail {

// One laid-out line (the header's wrap model): the line's code-point
// range in `text` (half-open) + its ink-block width (px, exact int).
struct TextLine {
  std::uint32_t start{};
  std::uint32_t end{};
  std::int64_t width{};
};

// The next line starting from code index `pos` (the header's wrap
// model — word wrap, the character split, the collapsed spaces). A
// TOTAL function on a live font: `false` when the remaining code
// points are all spaces (or exhausted) — no line is produced. All
// widths are in screen px at the item's `scale` (the integer-scale
// convention — every advance is multiplied by the scale). Pure
// (no allocation, no logging): a handful of integer sums over the
// line's codes.
[[nodiscard]] inline bool nextLine(const GlyphAtlas& font,
                                   std::span<const std::uint32_t> codes,
                                   std::uint32_t scale,
                                   std::int64_t maxWidth, std::uint32_t pos,
                                   TextLine& out) noexcept {
  const std::uint32_t n = static_cast<std::uint32_t>(codes.size());
  // The scaled advance of the code at index `idx` (the glyph is a
  // total function — M2-TEXT-01: a live font never returns null).
  auto advance = [&](std::uint32_t idx) {
    return static_cast<std::int64_t>(font.glyph(codes[idx])->advance) *
           static_cast<std::int64_t>(scale);
  };
  // Skip the leading spaces of the line (the header's wrap model).
  std::uint32_t p = pos;
  while (p < n && codes[p] == kTextSpaceCode) ++p;
  if (p == n) return false;
  // The first word.
  std::uint32_t q = p;
  while (q < n && codes[q] != kTextSpaceCode) ++q;
  std::int64_t wordW = 0;
  for (std::uint32_t i = p; i < q; ++i) wordW += advance(i);
  if (maxWidth > 0 && wordW > maxWidth) {
    // The character split (the header's wrap model): the greedy longest
    // prefix that fits (a single character always takes its own line —
    // the documented oversized behavior).
    std::int64_t cur = 0;
    std::uint32_t r = p;
    while (r < q) {
      const std::int64_t cw = advance(r);
      if (cur == 0) {
        cur = cw;
        ++r;
      } else if (cur + cw <= maxWidth) {
        cur += cw;
        ++r;
      } else {
        break;
      }
    }
    out = TextLine{p, r, cur};
    return true;
  }
  // The word fits: fill the line with the following words.
  std::int64_t cur = wordW;
  std::uint32_t r = q;
  const std::int64_t spaceAdv =
      static_cast<std::int64_t>(font.glyph(kTextSpaceCode)->advance) *
      static_cast<std::int64_t>(scale);
  while (r < n) {
    // The space run between this word and the next (collapsed to one
    // space advance — the header's wrap model).
    std::uint32_t r2 = r + 1;
    while (r2 < n && codes[r2] == kTextSpaceCode) ++r2;
    if (r2 == n) break;  // trailing spaces: the line ends here
    std::uint32_t q2 = r2;
    while (q2 < n && codes[q2] != kTextSpaceCode) ++q2;
    std::int64_t w2 = 0;
    for (std::uint32_t i = r2; i < q2; ++i) w2 += advance(i);
    if (maxWidth > 0 && cur + spaceAdv + w2 > maxWidth) break;
    cur += spaceAdv + w2;
    r = q2;
  }
  out = TextLine{p, r, cur};
  return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The width measurement (the M2-UI-01 panel-sizing read)
// ---------------------------------------------------------------------------

// The rendered ink-block width of `text` in screen px at `scale`
// (the header's wrap model, no-wrap: the sum of the glyph advances +
// one space advance per inter-word gap, leading/trailing spaces
// excluded — exactly what the declared quads span). O(glyphs); no
// allocation, no logging, no GL.
//
// Fails (no log — the precondition contract): a stopped/missing font,
// a scale outside [1, kTextMaxScale], or a handle the table does not
// own → InvalidArgument. An empty string (or an all-space one)
// measures 0.
[[nodiscard]] inline laige::Result<std::int64_t> measureText(
    const GlyphAtlas& font, const StringTable& table, StringRef text,
    std::uint32_t scale) noexcept {
  if (!font.valid() || scale < 1 || scale > kTextMaxScale ||
      !table.isValid(text)) {
    return laige::Result<std::int64_t>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  const std::span<const std::uint32_t> codes = table.text(text);
  detail::TextLine line;
  constexpr std::int64_t kNoWrap = 0;  // the no-wrap domain value
  if (!detail::nextLine(font, codes, scale, kNoWrap, 0, line)) {
    return laige::Result<std::int64_t>::success(0);  // empty / all spaces
  }
  return laige::Result<std::int64_t>::success(line.width);
}

// ---------------------------------------------------------------------------
// The text -> glyph-quads pass (the UI pass's declaration)
// ---------------------------------------------------------------------------

// Declares `item`'s glyphs into `batcher` under its (atlasId,
// materialId, blend) group (the header's model + wrap + domain
// sections): O(glyphs), no allocation, no logging on the happy path,
// no GL. The batcher must be live (created, nonzero capacity) and its
// frame window OPEN (the frame-protocol contract, the
// `declareParticles` precedent): a stopped batcher fails the first add
// with BudgetExhausted, a closed window with InvalidArgument. A line
// wider than kTextMaxLineWidthPx fails InvalidArgument BEFORE the
// first add (nothing declared — the pre-check). An empty text declares
// nothing and succeeds. The first failed add fails the call with the
// batcher's error (the items already declared remain in the frame —
// the batcher owns the frame state).
[[nodiscard]] inline laige::Status declareText(
    SpriteBatcher& batcher, const StringTable& table,
    const TextItem& item) noexcept {
  // The preconditions (first failure wins, InvalidArgument, no log —
  // the header's failure section): the scene-owned storage is live,
  // the item fields are in domain.
  if (item.font == nullptr || !item.font->valid() ||
      !table.isValid(item.text) || item.scale < 1 ||
      item.scale > kTextMaxScale ||
      (item.maxWidth != 0 &&
       (item.maxWidth < 1 ||
        item.maxWidth > static_cast<std::int32_t>(kTextMaxLineWidthPx)))) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  const GlyphAtlas& font = *item.font;
  const std::span<const std::uint32_t> codes = table.text(item.text);
  const std::int64_t maxWidth =
      static_cast<std::int64_t>(item.maxWidth);  // 0 = no wrap
  const std::int64_t lineStepPx =
      static_cast<std::int64_t>(font.lineHeight()) * item.scale;

  // 1. The pre-check (the header's screen-domain section): every line
  //    fits the domain, else nothing is declared.
  {
    std::uint32_t pos = 0;
    detail::TextLine line;
    while (detail::nextLine(font, codes, item.scale, maxWidth, pos, line)) {
      if (line.width > static_cast<std::int64_t>(kTextMaxLineWidthPx)) {
        return laige::Status::failure(laige::ErrorCode::InvalidArgument);
      }
      pos = line.end;
    }
  }

  // 2. The emit (the same walk — one code path, the header's layout
  //    walk): line by line, the per-glyph quads.
  std::uint32_t pos = 0;
  std::uint32_t lineIndex = 0;
  const std::uint32_t atlasW = font.atlasWidth();
  const std::uint32_t atlasH = font.atlasHeight();
  detail::TextLine line;
  while (detail::nextLine(font, codes, item.scale, maxWidth, pos, line)) {
    // The line's anchor (the header's model section: the alignment
    // offset from the ink-block width — one presentation-only float
    // division for the center alignment).
    float lineX0 = item.x;
    if (item.alignment == TextAlignment::Center) {
      lineX0 -= static_cast<float>(line.width) * 0.5f;
    } else if (item.alignment == TextAlignment::Right) {
      lineX0 -= static_cast<float>(line.width);
    }
    const float baselineY =
        item.y + static_cast<float>(static_cast<std::int64_t>(lineStepPx) *
                                    lineIndex);
    // The pen walk (the header's glyph-quad section).
    float pen = lineX0;
    for (std::uint32_t i = line.start; i < line.end; ++i) {
      const std::uint32_t code = codes[i];
      if (code == kTextSpaceCode) {
        // One space advance per inter-word gap (the consecutive spaces
        // collapse — the header's wrap model).
        pen += static_cast<float>(
            static_cast<std::int64_t>(font.glyph(code)->advance) *
            item.scale);
        while (i + 1 < line.end && codes[i + 1] == kTextSpaceCode) ++i;
        continue;
      }
      const GlyphMetrics& m = *font.glyph(code);
      if (m.width > 0 && m.height > 0) {
        // The ink rect (exact int math, one float conversion per
        // field — the header's screen-domain section).
        const float inkW = static_cast<float>(
            static_cast<std::int64_t>(m.width) * item.scale);
        const float inkH = static_cast<float>(
            static_cast<std::int64_t>(m.height) * item.scale);
        const float inkLeft =
            pen + static_cast<float>(static_cast<std::int64_t>(m.bearingX) *
                                     item.scale);
        const float inkTop =
            baselineY - static_cast<float>(static_cast<std::int64_t>(
                            m.bearingY) * item.scale);
        SpriteItem quad;
        quad.pos = Vec2{inkLeft + inkW * 0.5f, inkTop + inkH * 0.5f};
        quad.depthKey = item.depthKey;
        // The G-R11 escape hatch: the UI pass's manual z (the
        // sprite_batcher.h contract — counted + warned at build).
        quad.depthOverride = true;
        quad.uv = SpriteUvRect{
            static_cast<float>(m.atlasX) / static_cast<float>(atlasW),
            static_cast<float>(m.atlasY) / static_cast<float>(atlasH),
            static_cast<float>(m.atlasX +
                               static_cast<std::uint32_t>(m.width)) /
                static_cast<float>(atlasW),
            static_cast<float>(m.atlasY +
                               static_cast<std::uint32_t>(m.height)) /
                static_cast<float>(atlasH)};
        quad.frameIndex = 0;
        quad.rotation = 0.0f;
        quad.scale = Vec2{inkW, inkH};
        quad.tint = item.color;
        quad.atlasId = item.atlasId;
        quad.materialId = item.materialId;
        quad.blend = item.blend;
        const auto added = batcher.add(std::move(quad));
        if (!added.ok()) {
          // The first failed add fails the call (the declareParticles
          // precedent): the quads already declared remain in the frame.
          return laige::Status::failure(added.error());
        }
      }
      pen += static_cast<float>(static_cast<std::int64_t>(m.advance) *
                                item.scale);
    }
    pos = line.end;
    ++lineIndex;
  }
  return laige::Status{};
}

// ---------------------------------------------------------------------------
// The atlas upload bytes (the M2-SPRITE-02 bindAtlas input)
// ---------------------------------------------------------------------------

// Expands the 8-bit alpha glyph atlas into the white-on-alpha RGBA8
// bytes the M2-SPRITE-02 `bindAtlas` uploads (one call per font per
// scene load — the SETUP/asset path, before the one-time texture
// upload, RENDER-004). Every texel becomes (255, 255, 255, alpha):
// the M2-SPRITE-02 shader's multiplicative tint then makes the drawn
// color = tint × glyph alpha (the header's glyph-quad section — the
// standard text coloring). O(W·H); no allocation (the caller owns the
// span — one W·H·4-byte buffer at set-up), no logging, no GL.
//
// Fails (no GL work, no log — the precondition contract): a stopped
// atlas, or `rgba` not exactly `atlasWidth * atlasHeight * 4` bytes →
// InvalidArgument.
[[nodiscard]] inline laige::Status expandGlyphAtlasRgba8(
    const GlyphAtlas& atlas, std::span<std::uint8_t> rgba) noexcept {
  if (!atlas.valid()) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  const std::uint64_t need =
      static_cast<std::uint64_t>(atlas.atlasWidth()) *
      static_cast<std::uint64_t>(atlas.atlasHeight()) * 4u;
  if (rgba.size() != need) {
    return laige::Status::failure(laige::ErrorCode::InvalidArgument);
  }
  const std::span<const std::uint8_t> alpha = atlas.atlas();
  for (std::uint64_t i = 0; i < need / 4; ++i) {
    const std::uint8_t a = alpha[static_cast<std::size_t>(i)];
    std::uint8_t* px = rgba.data() + i * 4;
    px[0] = 255;
    px[1] = 255;
    px[2] = 255;
    px[3] = a;
  }
  return laige::Status{};
}

}  // namespace laige::render
