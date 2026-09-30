# ADR 0008 — Vendoring GLM 1.0.3 as the rendering-side math library

- **Status:** Accepted
- **Date:** 2026-09-29
- **Decider:** Roadmap step M2-GL-03
- **Refs:** PRD §11 (dependency table, "math foundation for the rendering
  side"), §10.3 (sim math stays SimMath), §4 (2.5D depth is engine-owned),
  AGENTS DEP-003/DEP-004/DEP-005, roadmap M2-GL-03, ADR 0004 (the
  vendoring machinery this reuses), ADR 0005 (the isometric presets the
  matrix builders implement), ADR 0007 (the sibling vendor: same
  `deps.lock` mechanism, pimpl contrast documented in §Consequences)

## Context

M2-GL-03 lands the rendering-side camera/matrix utilities: `ortho`,
`perspective`, `lookAt`, the 2D-plane camera (`planeOrtho`, the
top_down/side_view modes of M2-PROJ-01), and the isometric camera matrix
builders (the ADR 0005 presets: 2:1 dimetric, true 30°/60°, custom shear).
The PRD pre-approves **GLM** as "the math foundation for the rendering
side" (PRD §11), so no PRD revision is required — only the DEP-003
documentation and the DEP-005 pinning, which this ADR and `deps.lock`
provide.

Two constraints shape the decision:

1. **The sim side must not use it.** PRD §10.3 keeps the simulation on
   `SimMath` (fixed-point, deterministic, ADR 0002); the render side is
   where float vector/matrix math belongs (presentation-only, ARCH-009).
   The boundary must be machine-enforced, not a convention.
2. **NFR-8.8 forbids network access at build time**, so GLM is vendored
   in-tree and verified by the `deps.lock` tree hash on every configure
   (the ADR 0004 machinery, the same treatment as GLFW/GLAD in ADR 0007).

## Decision

- Vendor **GLM 1.0.3** (upstream tag commit
  `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed`) under `deps/glm/` — the
  full `glm/` library tree of the pinned tag: 434 header files
  (3.2 MB), `glm/core`, `glm/detail`, `glm/ext`, `glm/gtc`, `glm/gtx`,
  `glm/simd`, plus `deps/glm/copying.txt` (the MIT license). Excluded:
  `doc/`, `test/`, `cmake/`, `util/`, `manual/`, `.github/` (dev-only
  upstream material; the header-only library does not need them to
  compile, and they would bloat the tree hash without buy-in).
- GLM is **header-only**: there is no GLM library target. The engine's
  `laige-render` target gains the GLM include root as a **PUBLIC** build
  interface (`target_include_directories(laige-render PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/deps/glm>`), because
  `laige/render/matrices.h` names GLM value types (`Vec2`/`Vec3`/`Mat4`)
  in its public API — see §Consequences for the deliberate contrast to
  the GLFW/GLAD pimpl treatment.
- The include edge is **module-owned**: `deps.lock` records
  `owner: src/laige-render`, and `tools/laige-include-lint` (R3) rejects
  any other module including `deps/glm` — in particular, no sim module
  ever sees a GLM type (PRD §10.3 is enforced by the include graph, not
  by discipline).
- Pin in `deps.lock` (tree SHA-256 `f4785f29…`, source URL + commit, MIT
  license, justification); the root `CMakeLists.txt` runs
  `laige_deps_verify_lock()` on every configure and fails loudly on any
  mismatch (CORE-008, DEP-005).
- The engine's own code uses only the **core** GLM surface
  (`glm/vec2.hpp`, `glm/vec3.hpp`, `glm/mat4x4.hpp`, `glm/geometric.hpp`,
  `glm/matrix.hpp`); the vendored `ext/`, `gtc/`, `gtx/` trees are
  present (they are part of the pinned tag's tree and keep the hash
  faithful to the upstream artifact) but no engine TU includes them —
  `gtx` in particular is upstream's experimental namespace and is not
  a supported surface for engine code.
- The matrix builders do **not** call GLM's own `ortho`/`perspective`/
  `lookAt` functions: they are implemented in `matrices.cpp` against the
  documented formulas in `matrices.h` (the engine's conventions are the
  contract — right-handed world, +z up, OpenGL NDC z ∈ [−1, +1],
  camera looks along its own −z — and the builders' unit tests pin them
  to hand-computed golden values). GLM provides the value types and the
  vector algebra primitives (`cross`, `dot`, `normalize`, mat4 `*`);
  this keeps the engine's matrix layer auditable against its own
  documentation and independent of GLM's configuration flags (GLM's
  `perspective`/`lookAt` dispatch on the `GLM_CONFIG_CLIP_CONTROL`
  compile-time selection, which the engine does not want to depend on).

## DEP-003 justification

- **Capability needed:** a maintained, header-only, cross-platform
  float vector/matrix math library with the exact operations the
  camera/projection layer needs (vec2/vec3 algebra, 4×4 matrices,
  compose via multiply) and zero runtime footprint (no allocation, no
  state, no GL calls).
- **Alternatives considered:**
  - *Hand-written math (the SimMath style, float flavor)* — rejected:
    ADR 0002's fixed-point SimMath exists to make the **sim**
    deterministic; the render side is presentation-only and needs no
    fixed-point guarantee, while re-implementing vector/matrix algebra
    (and its edge cases: normalization of zero vectors, cross products,
    mat4 composition) is exactly the mature, boring, error-prone work
    DEP-002 says not to reinvent. GLM is PRD §11's named choice.
  - *Eigen* — viable C++ linear algebra, but heavier surface (dense
    solvers, block expressions, template depth), MIT-licensed but not
    header-only-by-default in the way needed, and not the PRD §11
    choice; buys nothing M2-GL-03 needs beyond what GLM provides.
  - *Direct GLM `lookAt`/`perspective` calls* — rejected (see
    §Decision): GLM 1.0.3's clip-space builders are configuration-
    dispatched (`GLM_CONFIG_CLIP_CONTROL`), and the engine wants its
    conventions documented and pinned by its own tests, not inherited
    from a compile-time default.
- **Transitive dependencies:** none (header-only; no link dependency,
  no generated code, no build-time tool). Build impact: compiling the
  engine's two matrix TUs pulls the core GLM headers — measured as
  seconds of extra compile time in the Debug trees (2026-09-29), no
  link-time change, no new system packages.
- **Platforms / health / license:** GLM — the de-facto standard C++
  math library (g-truc/glm, 17+ years of releases, header-only by
  design, the library most C++ rendering code already uses); actively
  maintained (1.0.x line). **MIT license** (permissive,
  MIT-compatible with the engine's license, NFR-1.2).
- **Security:** GLM parses no input and does no I/O; the security
  surface is the upstream code itself — the pin is re-audited on every
  upgrade (DEP-005).
- **Upgrade/removal strategy:** re-vendor the new tag's `glm/` tree +
  `copying.txt`, recompute the tree hash, update `deps.lock`, re-run CI
  (DEP-005). Upgrades can move public API types: the `Vec2`/`Vec3`/`Mat4`
  aliases are stable names, but GLM value types flow through
  `laige::render`'s public signatures, so an upgrade is a public-API
  review item (see §Review conditions). Removal would mean adopting the
  hand-written math alternative — the PRD §11 revision and the
  include-graph lint owner change would both be required.

## Evidence

- Vendored tree verified: the `deps.lock` hash check passes at configure
  time in every local tree (GCC/Clang/ASan, 2026-09-29); the
  include-graph lint reports the dependency correctly:
  `glm 1.0.3 — owner: src/laige-render — MIT`, total vendored deps 4
  (budget 10, PRD §11), no non-owner include edge.
- Warning-clean under the engine policy (`-Wall -Werror`) in the
  **GCC 16** and **Clang 22** Debug trees (2026-09-29): GLM's core
  headers compile with zero diagnostics inside the engine's TUs
  (`matrices.cpp`, `matrices_tests.cpp`), including under the engine's
  `-fno-exceptions -fno-rtti` flags (GLM's core headers are
  exception-free).
- The matrix builders are pinned against **hand-computed golden values**:
  CTest entry `matrices` (22 tests across `MatricesOrtho`,
  `MatricesPerspective`, `MatricesLookAt`, `MatricesPlaneOrtho`,
  `MatricesIso`) is green in `build`, `build-clang`, and `build-asan`
  (2026-09-29). The iso presets map known grid points to the expected
  NDC screen positions (the roadmap's Verify command), the preset
  builders equal `isoMatrix` with the ADR 0005 axis deltas, NDC-z is
  exactly 0 for every iso point (depth is engine-owned, PRD §4), and the
  ground-plane affine maps round-trip through their analytic inverse
  (the M2-ISO-03 picking requirement).
- GL-dependent verification is unaffected: these builders make no GL
  calls; the `matrices` suite runs in every local tree (no GPU
  required — the sandbox rule is not exercised).

## Consequences

- **GLM types are part of `laige-render`'s public API** — a deliberate
  contrast to ADR 0007, where GLFW/GLAD hide behind the `GlContext`
  pimpl. The difference: GLFW/GLAD expose *opaque native resources*
  (context handles) whose identity and layout are vendor-ABI, so
  naming them would make an ABI promise (API-007); GLM exposes *value
  types* (POD float vectors/matrices) with a stable, versioned,
  header-defined layout, and the render-side API is specified in terms
  of them (a camera matrix *is* a `Mat4`). Consumers therefore include
  GLM transitively through `laige-render`'s public include interface and
  never `#include <glm/...>` themselves (the lint R3 owner rule still
  forbids that from any other module).
- **NFR-8.9 (shared builds):** all consumers compile against the same
  pinned, in-tree header (one translation of GLM per binary by value),
  so there is no ODR drift risk between the engine library and game
  code — the same argument as ADR 0004's treatment of GoogleTest,
  applied to a shipped dependency.
- The include-graph lint (R3) now carries the "sim never includes GLM"
  rule: a sim module that includes `deps/glm` fails CI before review.
- The dependency count is 4/10 (googletest, glfw, glad, glm); GLM
  adds no transitive deps and no build-time tools.
- M2-CAM-01/02 will build on this: the camera objects own the build
  cadence (set-up phase) and M2-CAM-02's iso-preset selection maps
  preset id → `IsoAxes`/scale → the builders above; M2-PROJ-01's
  `world_to_screen` composes `planeOrtho`/iso matrices with the
  viewport transform. The builders' `scale` parameter is the zoom
  knob (the ADR 0005 tables are linear in the scale).

## Review conditions

- Upgrade GLM only via a DEP-005 change (new tag → new tree hash → lock
  update → CI green). Because GLM value types cross the public API
  boundary, an upgrade additionally requires reviewing the public
  `laige/render/matrices.h` signatures and regenerating `laige-api.json`
  (the API manifest check in CI will flag any symbol drift).
- Revisit the PUBLIC-include decision if the render API ever needs to
  expose GLM types that carry vendor ABI (e.g. simd-packed vectors
  exposed to games) — the value-type argument above does not extend to
  that.
- If the sim side ever needs float vector math (it currently must not —
  PRD §10.3, ADR 0002), the include-graph lint owner rule is the gate:
  changing `owner` in `deps.lock` is an architectural change requiring
  its own ADR and a PRD revision.
- If GLM's experimental `gtx` namespace ever becomes necessary for
  engine code, that is a review item: `gtx` is upstream's experimental
  surface (no stability promise), and engine use would need its own
  DEP-003 justification.
