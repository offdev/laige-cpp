# deps/

Vendored third-party dependencies, tracked by `deps.lock` (PRD §11,
NFR-8.6, NFR-8.13). The lock file, the hash-check wiring, and the first
dependency (GoogleTest, dev-only) land in **M0-DEP-01**.

Vendored code is wrapped at a narrow module boundary (AGENTS DEP-004) and
is never linked into engine libraries directly (dev-only deps never link
into engine libraries at all).
