# ADR 0003 — Config JSON strategy

- **Status:** Accepted
- **Date:** 2026-09-10
- **Decider:** Project owner (roadmap step M0-DEC-03)
- **Refs:** PRD FR-1.5, §11, NFR-8.7, NFR-13.1; AGENTS.md DEP-001, CORE-008,
  TEST-005; FR-12.1 / NFR-8.10

## Context

FR-1.5 requires declarative game configuration in JSON (tick rate, budgets,
camera defaults, asset roots; hot-reload of non-simulation config in debug).
JSON also appears in `laige-api.json` (NFR-13.1), `budgets.json`
(M0-CORE-08), and `deps.lock` (PRD §11). The engine's JSON needs are narrow:
objects, arrays, strings, numbers, booleans — small, dev-authored documents,
not a network surface (fuzzing is still required per NFR-8.7).

PRD §11 caps vendored dependencies at ≤ 10 (≤ 3 compiled) and requires a PRD
revision + DEP-003 justification for any new dependency. DEP-001 prefers the
standard library and in-house code when they meet the requirement. The engine
builds with `-fno-exceptions -fno-rtti` (FR-12.1, NFR-8.10).

## Decision

A hand-rolled **bounded JSON parser + serializer in `laige-core`**
(implemented by M0-CORE-07). **No new dependency.**

- Full JSON value type: null / bool / number / string / object / array.
- **Bounded:** documented defaults of max depth 32 and max document size
  1 MiB (engine-configurable); no recursion blowup (iterative or
  depth-bounded parse).
- **Malformed input → `Status` error** (CORE-008): never crashes, never
  silent; actionable error per NFR-13.3 grammar.
- Number semantics documented (JSON number → `double`; integer exactness
  policy for config values is documented in the header).
- UTF-8 validated; control characters rejected in strings (LOG-005 hygiene).
- Simple serializer for round-trip (used by the API manifest tooling and
  budget files).
- **Fuzz target `json_parse`** (NFR-8.7, TEST-005) registered with the
  `laige-fuzz` runner (M0-TEST-01).

## Alternatives considered

- **nlohmann/json** — most ergonomic, but exception-centric by default
  (conflicts with FR-12.1 / NFR-8.10 unless compiled with `JSON_NOEXCEPTION`,
  which degrades the API), ~25k lines, consumes one dependency slot, requires
  a PRD §11 revision.
- **RapidJSON** — header-only and exception-free, but a large C-style API
  surface for a narrow need; one dependency slot, PRD §11 revision.
- Both remain options under the review condition below.

## Evidence

- Dependency policy: DEP-001 (prefer standard library / in-house), PRD §11
  (≤ 10 deps, PRD-revision gate), G6 (minimal dependencies).
- Scope fit: the required subset is exactly what a ~200–400-line bounded
  parser covers — the size M0-CORE-07 already budgets (including tests and
  fuzz target).
- Security: config is dev-authored (low risk), but the fuzz target satisfies
  NFR-8.7 regardless of trust level.

## Consequences

- No new dependency; the PRD §11 table is **unchanged** (M0-DEC-03's "update
  the PRD §11 table" verify clause is not triggered).
- `deps.lock` counts are unaffected.
- M0-CORE-07's scope is unchanged — it already assumed this decision.
- The in-engine serializer must suffice for `laige-api.json` (M0-TOOL-01) and
  `budgets.json`; if it does not, see review condition.

## Review conditions

- Revisit with a DEP-003 record if the API manifest generator (M0-TOOL-01) or
  the asset pipeline (M3) needs general JSON tooling beyond a bounded parser
  (streaming, comments, very large documents, incremental patching).
