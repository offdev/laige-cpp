# ADR 0004 — Vendoring GoogleTest as the dev-only test dependency

- **Status:** Accepted
- **Date:** 2026-09-10
- **Decider:** Roadmap step M0-DEP-01
- **Refs:** PRD §11 (dependency table, dev-only row), §8.3 (NFR-8.6, NFR-8.8),
  AGENTS DEP-003/DEP-004/DEP-005, roadmap M0-DEP-01

## Context

M0 needs a unit-test framework: the milestone rule requires unit tests in the
same change as every implementation step, and later M0 steps (M0-CORE-xx,
M0-CI-01/02) assume a framework that supports death tests, parameterized
tests, and sanitizer-instrumented builds on all P0 platforms (PRD §6).

PRD §11 pre-approves **GoogleTest** as a *dev-only* dependency ("Unit/
integration tests; never shipped"), so no PRD revision is required — only the
DEP-003 documentation and the DEP-005 pinning machinery, which this ADR and
`deps.lock` provide.

NFR-8.8 forbids network access at build time, so the framework must be
vendored in-tree with a verifiable integrity check, not fetched at configure
time.

## Decision

- Vendor **GoogleTest v1.18.0** (upstream commit
  `063de7e9578f82b369302001269680b4b1553359`) under `deps/googletest/`,
  the complete tagged tree (252 files, BSD-3-Clause license file included).
- Pin it in `deps.lock` (repo root): name, version, source URL + commit,
  SHA-256 of the vendored tree, license, and the PRD §11 row. The root
  `CMakeLists.txt` runs `laige_deps_verify_lock()` on **every configure**
  (`cmake/laige-deps-lock.cmake`): it re-hashes the vendored tree and fails
  loudly on any mismatch, missing tree, or vendored directory not listed in
  the lock (CORE-008, DEP-005).
- Wire it into **tests only** (`tests/CMakeLists.txt`): test executables link
  `gtest_main`; it is never linked into engine libraries (DEP-004 boundary).
  `BUILD_GMOCK=OFF` (no mock-based suites yet, CORE-004) and
  `INSTALL_GTEST=OFF` (vendored, never installed, NFR-8.8).
- GTest is pinned third-party code, so it does not take the engine's
  `-Wall -Werror` policy; it **does** receive the same no-exceptions/no-rtti
  flags as the engine test TUs (required so `GTEST_HAS_EXCEPTIONS_` /
  `GTEST_HAS_RTTI_` resolve identically in the library and the test
  translation units — an ODR requirement, CPP-004). It also inherits the
  whole-tree sanitizer instrumentation (NFR-8.2).

## DEP-003 justification

- **Capability needed:** a mature C++20 unit-test framework with death tests,
  parameterized/typed tests, and clean sanitizer builds; no runtime
  dependency (tests must not change what ships).
- **Alternatives considered:** Catch2 (v3) and doctest — both viable, but
  PRD §11 already lists GoogleTest and it has the deepest integration-test
  features (death tests are needed for the no-exceptions contract, FR-12.1);
  a home-grown framework is rejected per DEP-002 (mature, security-adjacent
  test-infra work is not ours to reinvent).
- **Transitive dependencies:** none — pure C++ plus pthreads (auto-detected
  by CMake). Build impact: one static library pair (`gtest`,
  `gtest_main`), ≈250 files, tens of seconds of build time in a fresh tree.
- **Platforms / health / license:** maintained by Google for 15+ years,
  CMake builds on all P0 platforms (GCC, Clang/AppleClang, MSVC 2022);
  BSD-3-Clause (permissive, no copyleft, compatible with the MIT engine).
- **Security:** parses no untrusted input at runtime in our usage.
- **Upgrade/removal strategy:** re-vendor the new tag, recompute the tree
  hash, update `deps.lock`, re-run CI (DEP-005). It never crosses a public
  API boundary (dev-only, test executables only), so upgrades cannot break
  game consumers.

## Evidence

- Vendored tree verified: `deps.lock` hash check passes at configure time;
  tampering any vendored file makes the configure fail with the expected vs
  actual hash (repro: append a line to
  `deps/googletest/googletest/include/gtest/gtest.h`, reconfigure).
- Warning-clean under `-Wall -Werror -fno-exceptions -fno-rtti`:
  **GCC 16.2.1** and **Clang 22.1.8** (2026-09-10). MSVC 2022 compatibility is
  verified by the M0-CI-01 Windows job (same status as the M0-BUILD-01
  sanitizer flags).
- `ctest` runs the first GTest-based suite
  (`tests/laige-core/laige-core_tests`) in the default, shared, ASan, and
  Clang build trees — 1/1 passed in each, zero warnings.

## Consequences

- `deps/` + `deps.lock` + `cmake/laige-deps-lock.cmake` are the permanent
  dependency-pinning machinery; every future vendor (GLFW, miniz, …) reuses
  it without new build machinery.
- Every configure re-hashes the vendored trees: the cost is one SHA-256 pass
  over ≈250 small files per configure — negligible and offline.
- Test executables now link `gtest_main`; engine libraries are unchanged and
  still have zero runtime dependencies (NFR-8.8, ARCH-003 headless builds
  unaffected).

## Review conditions

- Upgrade GoogleTest only via a DEP-005 change (new tag → new tree hash →
  lock update → CI green); no public API impact is possible.
- Re-enable `BUILD_GMOCK` only when a suite actually requires mocks.
- If MSVC 2022 surfaces warnings under the language-consistency flags,
  extend the flag mapping in `tests/CMakeLists.txt` (no policy change).
