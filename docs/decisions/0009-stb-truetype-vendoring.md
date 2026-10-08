# ADR 0009 — Vendoring stb_truetype as the font rasterizer

- **Status:** Accepted
- **Date:** 2026-10-08
- **Decider:** Roadmap step M2-TEXT-01
- **Refs:** PRD §11 (dependency table, "font rasterization: stb_truetype"),
  FR-2.8 (bitmap text P0), AGENTS DEP-003/DEP-004/DEP-005, roadmap
  M2-TEXT-01, ADR 0004 (the vendoring machinery this reuses), ADR 0007
  (the sibling vendor: same `deps.lock` mechanism), ADR 0008 (the
  header-only contrast documented in §Consequences)

## Context

M2-TEXT-01 lands bitmap font rasterization (FR-2.8 P0): the configured
font + glyph set is rasterized at scene set-up into a fixed-size bitmap
glyph atlas with per-glyph metrics (advance, bearing, line height). The
PRD pre-approves **stb_truetype** as the font rasterizer (PRD §11), so
no PRD revision is required — only the DEP-003 documentation and the
DEP-005 pinning, which this ADR and `deps.lock` provide.

Two constraints shape the decision:

1. **NFR-8.8 forbids network access at build time**, so the header is
   vendored in-tree and verified by the `deps.lock` tree hash on every
   configure (the ADR 0004 machinery, the same treatment as GLFW/GLAD in
   ADR 0007 and GLM in ADR 0008).
2. **The public API must not name vendor types** (CPP-010, DEP-004):
   unlike GLM (ADR 0008), the font API exposes engine-owned metric
   structs and atlas bytes, never `stbtt_fontinfo` or other vendor
   types — the rasterizer sits behind an implementation boundary.

## Decision

- Vendor **stb_truetype** from upstream commit
  `2c980bb59875b0d32144a71867fbdebb2f77cd20` (2026-08-01; stb publishes
  no release tags, so the pin is the commit, recorded in `deps.lock`
  `source_commit`) under `deps/stb/` — exactly two files:
  `stb_truetype.h` (the complete single-header library) and `LICENSE`
  (the upstream license text). No other stb headers are vendored:
  M2-TEXT-01/02/03 need only font rasterization, and the tree hash
  covers what is actually consumed (CORE-004: no speculative surface).
- stb_truetype is a **single-header library with a one-translation-unit
  implementation**: the engine's `laige-stb` static target compiles the
  pinned header once with `STB_TRUETYPE_IMPLEMENTATION`
  (`src/laige-render/stb_truetype_impl.cpp`) under the **plain compiler
  policy** — pinned vendor code, DEP-005, the `laige-glad` precedent
  (ADR 0007): the engine's `-Wall -Werror` policy does not gate upstream
  code, and the vendor tree stays byte-pinned by the lock.
- The engine boundary is `src/laige-render/font.cpp` (+ the public
  header `src/laige-render/include/laige/render/font.h`): it includes
  the vendor header's **declarations** privately and links `laige-stb`
  PRIVATE. The public header names no stb type, so consumers never see
  vendor code (CPP-010) and no other module may include `deps/stb`
  (the include-graph lint R3 owner rule, `owner: src/laige-render`).
- Pin in `deps.lock` (tree SHA-256 `8f065846…`, source URL + commit,
  stb license, justification); the root `CMakeLists.txt` runs
  `laige_deps_verify_lock()` on every configure and fails loudly on any
  mismatch (CORE-008, DEP-005).
- The engine uses only the **font-metrics + glyph-bitmap surface**:
  `stbtt_GetFontOffsetForIndex` (0 = the first font in the file, −1 =
  not a font), `stbtt_InitFont`, `stbtt_ScaleForPixelHeight`,
  `stbtt_FindGlyphIndex` (0 = `.notdef` — stbtt's unknown-codepoint
  contract), `stbtt_GetGlyphHMetrics` (unscaled integer advance),
  `stbtt_GetGlyphBitmapBox` (no allocation), `stbtt_GetGlyphBitmap`
  (the only allocating call — one transient `w × h` 8-bit buffer per
  glyph, freed by the engine's RAII wrapper). Nothing else of the
  40k-line header (outline parsing internals, subsetting, the
  "STB_truetype" visualization modes) is called by engine code. Note
  the pinned commit's API differs from older stb versions: there is no
  `stbtt_GetFontOffsetForData` and no per-codepoint convenience
  wrappers (the glyph index + metrics surface is the API).

## DEP-003 justification

- **Capability needed:** a maintained, dependency-free, cross-platform
  TTF/OTF rasterizer that turns a caller-supplied font byte buffer into
  8-bit alpha glyph bitmaps + exact metrics at a configured pixel
  height, with no runtime state (the atlas is built once at scene
  set-up — no runtime re-rasterization, M2-TEXT-01 scope) and no GL
  calls.
- **Alternatives considered:**
  - *FreeType* — the full-featured incumbent, but a compiled C library
    with a long build configuration (autoconf/meson), a large transitive
    surface, and far more than M2-TEXT-01 needs (it parses the entire
    OpenType spec for features this engine does not use). It is the
    PRD §11 escape hatch if bitmap text ever needs TrueType hinting or
    OpenType layout features; it is not the P0 path.
  - *Hand-written TTF outline rasterizer* — rejected: font-outline
    parsing + scanline fill is exactly the mature, error-prone,
    format-heavy work DEP-002 says not to reinvent.
  - *Pre-rasterized atlas files (no runtime rasterizer)* — rejected:
    FR-2.8 requires the CONFIGURED font (the game's font asset), not a
    baked-in one; and M2-TEXT-03 (SDF) needs the outline data again,
    which a baked atlas would not provide.
- **Transitive dependencies:** none (single header, C99, no submodules,
  no build-time tool). Build impact: one extra static target
  (`laige-stb`) + the vendor header compiled once — seconds of compile
  time in the Debug trees, no system packages.
- **Platforms / health / license:** stb — the de-facto standard
  single-header C library collection (nothings/stb, 15+ years, used
  across the C/C++ rendering ecosystem); `stb_truetype.h` is one of its
  most widely deployed headers. **License:** stb's no-warranty license
  (permissive, no copyleft, compatible with the engine's MIT license,
  NFR-1.2) — the text is committed at `deps/stb/LICENSE`.
- **Security:** the rasterizer parses caller-supplied font bytes
  (untrusted input, SCALE-004): `GlyphAtlas::create` validates the
  bytes at the boundary — the header + full table directory must be
  present (the engine-side guard: stbtt's table-directory scan is not
  bounds-checked in the pinned commit, so the guard runs before any
  stb call), and `stbtt_GetFontOffsetForIndex`/`stbtt_InitFont` reject
  non-fonts before any parsing — the glyph range and atlas capacity are
  bounded and validated, and the bitmap buffer size is derived from the
  font's own (validated) metrics — a malformed font yields an error
  Status, never a crash. Fuzz surface: the font loader is an
  untrusted-input surface; a `laige-fuzz` font target is the M3-asset
  step's concern (the loader consumes game assets after the M3 asset
  system lands).
- **Upgrade/removal strategy:** re-vendor the pinned commit's
  `stb_truetype.h` + `LICENSE`, recompute the tree hash, update
  `deps.lock`, re-run CI (DEP-005). The engine-facing surface is the
  engine's own `font.h` (stable names, metric structs); a stb upgrade
  cannot move it, so the upgrade is a lock + CI change only. Removal
  would mean adopting the FreeType alternative — a PRD §11 revision and
  a new ADR.

## Evidence

- Vendored tree verified: the `deps.lock` hash check passes at configure
  time in every local tree (all six, M2-TEXT-01 validation); the
  include-graph lint reports the dependency: `stb 2026-08-01 — owner:
  src/laige-render`, total vendored deps 5 (budget 10, PRD §11), no
  non-owner include edge.
- Warning-clean: `laige-stb` compiles the pinned header under the plain
  compiler policy in the GCC 16 and Clang 23 Debug trees (2026-10-08);
  `font.cpp` compiles under the engine policy (`-Wall -Werror`) in all
  six local trees with zero diagnostics.
- The M2-TEXT-01 unit suite pins the behavior: deterministic atlas
  generation (same font + size → identical atlas bytes), the exact
  metrics goldens, the missing-glyph fallback (never a crash), and the
  stopped/capacity failure paths — CTest entry `font`, green in all six
  local trees.

## Consequences

- **The vendor type stays behind the boundary** — a deliberate
  parallel to the GLFW/GLAD pimpl treatment (ADR 0007) and a contrast
  to ADR 0008: GLM value types are part of the public API because they
  are versioned PODs; `stbtt_fontinfo` is an opaque, version-pinned C
  struct whose layout is an upstream implementation detail, so it
  stays in `font.cpp`. The public API is `GlyphAtlas`/`GlyphMetrics`
  (engine-owned, stable).
- **One extra target, one extra lock entry:** the dependency count is
  5/10 (googletest, glfw, glad, glm, stb).
- **NFR-8.9 (shared builds):** `laige-stb` is a static archive linked
  PRIVATE into `laige-render` (static or shared) — no public link
  interface change, no ODR surface.
- **M2-TEXT-02/03 build on this:** M2-TEXT-02 (text items) consumes the
  metrics + 8-bit alpha atlas (uploading it as an RGBA8 white-on-alpha
  atlas to the M2-SPRITE-02 `bindAtlas` path); M2-TEXT-03 (SDF) reuses
  the SAME vendored header (outline → distance field) — no new
  dependency.
- **Determinism scope:** the atlas is presentation-only (ARCH-009):
  rasterization is a fixed float sequence per (font bytes, options) —
  bit-identical across runs of the same build (the in-process
  determinism test pins this); cross-platform bit-identity is NOT
  promised (render-side float, not SimMath — the same scope as the
  M2-PROJ-01/ISO-03 float transforms).

## Review conditions

- Upgrade stb only via a DEP-005 change (new commit → new tree hash →
  lock update → CI green); the engine-facing API (`font.h`) is
  unchanged by such an upgrade, and `laige-api.json` regeneration
  confirms the manifest.
- If FR-2.8 ever needs TrueType hinting, OpenType layout (kerning
  pairs, GSUB), or a non-Latin-1 default glyph set, revisit the
  FreeType alternative with its own ADR (PRD §11 row).
- If M2-TEXT-03's SDF path turns out to need stb internals beyond the
  outline surface already vendored, re-scope the vendored file set in
  the same PR (tree hash + this ADR).
