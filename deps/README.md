# deps/

Vendored third-party dependencies, each tracked in `deps.lock` (repo root;
PRD §11, NFR-8.6, DEP-005). This directory landed with **M0-DEP-01**: the
lock file, the configure-time hash check, and **GoogleTest** (dev-only);
**M2-GL-01** adds **GLFW** (windowing) and the **GLAD-generated GL 3.3
loader** (GL function access), **M2-GL-03** adds **GLM** (rendering-side
math), and **M2-TEXT-01** adds **stb_truetype** (font rasterization) —
all for `src/laige-render`.

## deps.lock

`deps.lock` is the machine-readable record of every vendored dependency.
Each entry carries: `name`, `version`, `path` (the vendored tree relative to
the repo root), `owner` (the module that owns/wraps the dependency — `tests`
for dev-only deps, or `src/laige-<module>` for engine modules; AGENTS
DEP-004), `source_url`, `source_commit`, `sha256` (SHA-256 of the vendored
tree), `license`, and `justification` (the PRD §11 row).

The tree hash is defined and computed by `cmake/laige-deps-lock.cmake`
(`laige_deps_tree_sha256`): for every regular file under the tree, sorted by
relative path, the hash accumulates
`hex(sha256(content)) + "\n" + relpath + "\n"`, and the tree SHA-256 is the
SHA-256 of that blob. It is content-addressed and cross-platform (hidden
files included, symlinks ignored).

## Integrity check (every configure)

The root `CMakeLists.txt` calls `laige_deps_verify_lock()` on **every
configure**. It re-hashes each vendored tree and fails loudly
(`FATAL_ERROR`) on:

- a mismatch between the lock's `sha256` and the on-disk tree;
- a missing or non-directory `path`;
- a vendored subdirectory of `deps/` that is **not** listed in the lock
  (so no code can exist outside the lock — DEP-005);
- a malformed or truncated `deps.lock`.

This is what makes "tampering with a vendored file fails the build" true:
re-running `cmake -S . -B build` after any edit under `deps/<name>/`
recomputes the tree hash and rejects the configure.

## Current dependencies

| Dependency | Version | Tree | Owner (`deps.lock`) | License | Justification |
|---|---|---|---|---|---|
| GoogleTest | 1.18.0 | `googletest/` | `tests` (`tests/` only) | BSD-3-Clause | PRD §11 dev-only row (unit/integration tests; never shipped) |
| GLFW | 3.5.1 | `glfw/` | `src/laige-render` | zlib | PRD §11 windowing row (M2-GL-01: windowed + hidden-window context creation on all P0 OSes) |
| GLAD (generated) | 2.0.8 | `glad/` | `src/laige-render` | WTFPL OR CC0-1.0 AND Apache-2.0 | PRD §11 GL-loader row (M2-GL-01: GL 3.3 core function access; the vendored artifact is the reproducible `gladv2 --api gl:core=3.3` output, not a source checkout) |
| GLM | 1.0.3 | `glm/` | `src/laige-render` | MIT | PRD §11 math-foundation row (M2-GL-03: rendering-side value types for cameras/projection matrices; the full `glm/` tree of the pinned tag) |
| stb (stb_truetype) | 2026-08-01 | `stb/` | `src/laige-render` | MIT-like (stb license) | PRD §11 font-rasterization row (M2-TEXT-01: bitmap glyph atlas rasterization for FR-2.8 P0; the single header `stb_truetype.h` + `LICENSE`, pinned commit — stb publishes no release tags) |

See [ADR 0004](../docs/decisions/0004-google-test-vendoring.md) (GoogleTest),
[ADR 0007](../docs/decisions/0007-glfw-glad-vendoring.md) (GLFW + GLAD),
[ADR 0008](../docs/decisions/0008-glm-vendoring.md) (GLM), and
[ADR 0009](../docs/decisions/0009-stb-truetype-vendoring.md) (stb_truetype)
for the full DEP-003 justifications and the upgrade/removal strategy.

## Adding or updating a dependency

1. Vendor the pinned source into `deps/<name>/` (a complete tagged tree).
2. Add/update the entry in `deps.lock` (recompute the tree hash; set
   `owner` to the module that will wrap the dependency).
3. Document it per AGENTS DEP-003; the PRD §11 table must already list it —
   a brand-new dependency first needs a PRD revision.
4. Reconfigure; the lock check verifies the result.

Vendored code is wrapped at a narrow module boundary (AGENTS DEP-004): only
`tests/` links GoogleTest (`gtest`/`gtest_main`); it is never linked into
engine libraries (dev-only deps never link into engine libraries at all).
The include side of that boundary is machine-checked by
`tools/laige-include-lint` (M0-CI-03): a vendored dependency may only be
`#include`d from its `deps.lock` `owner`, in CI on every PR and locally.
