# deps/

Vendored third-party dependencies, each tracked in `deps.lock` (repo root;
PRD §11, NFR-8.6, DEP-005). This directory lands with **M0-DEP-01**: the lock
file, the configure-time hash check, and the first (and only so far)
dependency, **GoogleTest** (dev-only).

## deps.lock

`deps.lock` is the machine-readable record of every vendored dependency.
Each entry carries: `name`, `version`, `path` (the vendored tree relative to
the repo root), `source_url`, `source_commit`, `sha256` (SHA-256 of the
vendored tree), `license`, and `justification` (the PRD §11 row).

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

| Dependency | Version | Tree | Used by | License | Justification |
|---|---|---|---|---|---|
| GoogleTest | 1.18.0 | `googletest/` | `tests/` only | BSD-3-Clause | PRD §11 dev-only row (unit/integration tests; never shipped) |

See [ADR 0004](../docs/decisions/0004-google-test-vendoring.md) for the full
DEP-003 justification and the upgrade/removal strategy.

## Adding or updating a dependency

1. Vendor the pinned source into `deps/<name>/` (a complete tagged tree).
2. Add/update the entry in `deps.lock` (recompute the tree hash).
3. Document it per AGENTS DEP-003; the PRD §11 table must already list it —
   a brand-new dependency first needs a PRD revision.
4. Reconfigure; the lock check verifies the result.

Vendored code is wrapped at a narrow module boundary (AGENTS DEP-004): only
`tests/` links GoogleTest (`gtest`/`gtest_main`); it is never linked into
engine libraries (dev-only deps never link into engine libraries at all).
