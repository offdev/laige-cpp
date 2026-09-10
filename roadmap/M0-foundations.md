# M0 — Foundations

**PRD:** §15 M0 · **Duration:** 2–3 weeks
**Scope (PRD):** Repo, CI (all P0), build system, `laige-core` (math, pools, alloc,
Result, logging, config), dep lock, API manifest generator.
**Exit criteria (PRD):** CI green on 3 OSes; core unit-tested; budget harness wired.

Rules specific to this milestone: every step here produces code that compiles with
`-Wall -Werror -fno-exceptions -fno-rtti` (NFR-8.10) and unit tests in the same change.
No rendering, no physics, no networking yet — `laige-core` only.

---

## Decisions

- [x] **M0-DEC-01 · Resolve engine name and license**
  - **Refs:** PRD §18.1, §15 (working name "Laige")
  - **Depends:** —
  - **Scope:**
    - Decide final name (or confirm "Laige") and license (MIT proposed; assets/samples separately licensed).
    - Write ADR `docs/decisions/0001-name-and-license.md`.
  - **Decision (2026-09-10):** keep **Laige** (*Legendary AI Game Engine*);
    engine **MIT**, samples/assets separately licensed. ADR 0001 written.
  - **Verify:** ADR file exists and is linked from `docs/decisions/` index.
  - **Size:** docs only

- [x] **M0-DEC-02 · Decide deterministic math strategy (D-MATH)**
  - **Refs:** PRD §18.2, §10.3, FR-3.3; AGENTS ARCH-010
  - **Depends:** —
  - **Scope:**
    - Decide: Q16.16 fixed-point as the default deterministic math, vs float-pinned with fixed-point only for lockstep.
    - ADR `docs/decisions/0002-deterministic-math.md` stating the determinism scope (same build/platform/ISA vs cross-ISA) and which paths use which type.
  - **Decision (2026-09-10):** config-selectable **SimMath** — one op
    interface, two backends: `fpx16_16` (**default**; required for
    lockstep/MMO; bit-exact across build/platform/ISA/compiler) and
    `fp32_pinned` (opt-in; bit-exact per platform/ISA until CI proves
    cross-ISA). Backend selected once at engine/zone init (template dispatch,
    no per-call indirection); backend id is part of replay identity. ADR 0002
    written.
  - **Verify:** ADR exists; decision matrix (float path, fixed-point path, lockstep path) explicit.
  - **Size:** docs only

- [x] **M0-DEC-03 · Decide config JSON strategy (D-JSON)**
  - **Refs:** PRD §7.1 (FR-1.5), §11 (dep policy)
  - **Depends:** —
  - **Scope:**
    - Decide: in-engine bounded JSON parser (recommended; no new dependency) vs vendored JSON library (would need PRD §11 revision).
    - ADR `docs/decisions/0003-config-json.md`.
  - **Decision (2026-09-10):** in-engine bounded parser in `laige-core`
    (defaults: depth ≤ 32, size ≤ 1 MiB; malformed input → `Status`; fuzz
    target `json_parse`). **No new dependency** — PRD §11 table unchanged.
    ADR 0003 written.
  - **Verify:** ADR exists; if a new dependency is chosen, the PRD §11 table is updated in the same change.
  - **Size:** docs only

## Repository & build

- [ ] **M0-REPO-01 · Repository skeleton**
  - **Refs:** PRD §10.1 (module map), NFR-8.8, NFR-8.10; AGENTS §13 (docs layout)
  - **Depends:** M0-DEC-01
  - **Scope:**
    - Top-level `README.md` (project overview, build pointer), `LICENSE` (per M0-DEC-01), `.gitignore`.
    - Root `CMakeLists.txt`: CMake ≥ 3.22, C++20, `-Wall -Werror`, exceptions/RTTI disabled for engine targets, static+shared option placeholders.
    - Directory layout per PRD §10.1: `src/` (one dir per module, only `laige-core` populated now), `deps/`, `tests/`, `tools/`, `docs/`, `samples/`, `third_party` placeholder for vendored code.
  - **Verify:** `cmake -S . -B build` configures cleanly; `cmake --build build` succeeds (empty target).
  - **Size:** ~100 lines (mostly CMake/docs)

- [ ] **M0-BUILD-01 · `laige-core` target + canonical commands**
  - **Refs:** NFR-8.8, NFR-8.9, NFR-8.10; README §1 canonical commands
  - **Depends:** M0-REPO-01
  - **Scope:**
    - `laige-core` library target: static and shared builds both work (`LAIGE_BUILD_SHARED` option); no transitive dep leakage (CPP-010).
    - Document the canonical configure/build/test/sanitizer/fuzz/bench commands exactly (README §1 table becomes the source of truth); `docs/getting-started/building.md`.
    - CMake options used by later steps: `LAIGE_ASAN`, `LAIGE_TSAN`, `LAIGE_SCRIPT` (reserved), `LAIGE_BUILD_TESTS` (default ON).
  - **Verify:** static and shared builds both configure+build with zero warnings; `docs/getting-started/building.md` matches the actual commands (spot-checked).
  - **Size:** ~150 lines CMake + docs

- [ ] **M0-DEP-01 · Dependency lock + vendored GoogleTest**
  - **Refs:** PRD §11 (dep table, `deps.lock`, NFR-8.6), DEP-005
  - **Depends:** M0-REPO-01
  - **Scope:**
    - `deps.lock`: JSON file listing each vendored dependency with name, version, source commit/URL, SHA-256 of the vendored tree, license, justification ref (PRD §11 row).
    - Vendor GoogleTest (dev-only, per PRD §11) under `deps/googletest/`; wire it into tests only, never linked into engine libs.
    - CMake check that `deps.lock` hashes match the vendored trees (fails loudly on mismatch).
  - **Verify:** hash check passes; tampering a vendored file makes the CMake configure/test fail; `ctest` runs one trivial test via GTest.
  - **Size:** ~150 lines (scripts + lock) + vendored tree

## CI

- [ ] **M0-CI-01 · CI matrix: all P0 platforms**
  - **Refs:** PRD §6 (AC-6.1), §14; NFR-8.8
  - **Depends:** M0-BUILD-01
  - **Scope:**
    - CI workflow with one job per P0 OS: Linux (g++ and clang++), Windows (MSVC 2022), macOS (arm64 + Intel).
    - Each job: configure → build → `ctest` (unit tests only so far).
    - Cadence per PRD §14: one P0 OS per PR, all three per merge (implement as labels or merge-gate).
  - **Verify:** pushing a trivial change runs all jobs and they are green; matrix runs in < 10 min (PRD §8.1 build budget starts counting here).
  - **Size:** workflow files only

- [ ] **M0-CI-02 · Sanitizer CI jobs**
  - **Refs:** NFR-8.2 (ASan+UBSan, TSan); AGENTS TEST-006
  - **Depends:** M0-CI-01
  - **Scope:**
    - Linux jobs building with `LAIGE_ASAN=ON` (ASan+UBSan) and `LAIGE_TSAN=ON`, running the unit suites.
    - Fail build on any sanitizer report; reports archived as CI artifacts.
  - **Verify:** introducing a deliberate OOB read in a scratch test fails the ASan job (test removed afterwards); TSan job green on clean code.
  - **Size:** workflow changes only

- [ ] **M0-CI-03 · Include-graph lint + dependency-count metric**
  - **Refs:** NFR-8.11, NFR-8.13; PRD §10.1 dependency rule
  - **Depends:** M0-BUILD-01
  - **Scope:**
    - Script (in `tools/`) that parses `#include` edges of `src/**` and enforces: `laige-core` includes nothing internal; arrows only downward; no include of `deps/` outside the owning module's boundary (DEP-004).
    - Same script reports the vendored-dependency count; CI asserts ≤ 10 (PRD §11) and prints the list.
    - Both run in CI on every PR.
  - **Verify:** an illegal include (e.g. `laige-core` including a future `laige-render` header stub) fails the lint; dep count prints and passes.
  - **Size:** ~200 lines script

## laige-core

- [ ] **M0-CORE-01 · `Result<T,E>` / `Status` + error registry**
  - **Refs:** FR-12.1, NFR-13.3; AGENTS CORE-008
  - **Depends:** M0-BUILD-01
  - **Scope:**
    - `laige::Result<T, E>` and `laige::Status` (no exceptions): success/value or error code.
    - Central error-code registry: stable integer codes, each with `{code} | {what} | {why} | {fix} | {doc_anchor}` template text (NFR-13.3 grammar).
    - Unit tests: construction, error propagation, no exceptions raised (linker-level: build with `-fno-exceptions`).
  - **Verify:** `ctest -R result_status` green; error strings follow the 5-field grammar (test asserts format).
  - **Size:** ~250 lines + tests

- [ ] **M0-CORE-02 · Structured logging facade**
  - **Refs:** AGENTS.md §14 (LOG-001…LOG-007); FR-12.2
  - **Depends:** M0-CORE-01
  - **Scope:**
    - One logging facade: severity (Trace…Fatal per §14), stable subsystem+event names, lazy field/message evaluation (no formatting/allocation when disabled — LOG-003), per-subsystem level filters.
    - Sink interface with a console sink and a file sink; per-subsystem scopes; rate limiting with suppressed-count summary (LOG-004); crash/shutdown flush (LOG-007).
    - Unit tests: disabled levels allocate nothing (assert with allocation counter from M0-CORE-05 if available, else ASan leak-free + timing property test); rate-limit summary emitted after N repeats.
  - **Verify:** `ctest -R logging` green; trace-level spam in a disabled-subsystem test shows zero allocations (ASan/alloc counter).
  - **Size:** ~350 lines + tests

- [ ] **M0-CORE-03 · SimMath interface + `fp32_pinned` backend**
  - **Refs:** PRD §10.3, S-7; ADR 0002 (`fp32_pinned` backend); AGENTS CORE-005
  - **Depends:** M0-DEC-02, M0-CORE-01
  - **Scope:**
    - SimMath op interface (add/sub/mul/div, compare, clamp, lerp, normalize, length) plus the `fp32_pinned` backend: IEEE `float` ops with ADR 0002's pinned flag set (no FMA in sim translation units, `-ffp-contract=off`/equivalent per compiler, no reassociation, no floating-point intrinsics) applied and documented.
    - Ops are the *only* math allowed in deterministic sim code (enforcement comes later in M1-DET-01; here: provide the API + docs).
    - Unit tests: property tests (associativity guards, NaN/inf handling is *defined* and tested — a documented policy, not "whatever the CPU does").
  - **Verify:** `ctest -R math_float` green; documented NaN/Inf policy exists in header docs; pinned flag set documented and applied to sim targets.
  - **Size:** ~250 lines + tests

- [ ] **M0-CORE-04 · SimMath `fpx16_16` backend (default)**
  - **Refs:** PRD §10.3, FR-3.3 (fixed-point option); ADR 0002 (`fpx16_16` backend); M0-DEC-02
  - **Depends:** M0-CORE-03
  - **Scope:**
    - `laige::fpx16_16`: signed Q16.16; add/sub/mul (rounded, documented), divide, negate, compare, convert from/to `int32_t`/`float`; overflow defined (saturate) and documented; no UB under any input (CPP-004).
    - Wire it in as the **default** SimMath backend (ADR 0002) with the same op surface as M0-CORE-03.
    - Unit tests including exhaustive edge cases (min/max, wrap candidates, rounding ties).
  - **Verify:** `ctest -R math_fixed` green under ASan+UBSan; property test: same op sequence on two different compiler builds produces identical results (run locally in M1-DET-04 CI hookup).
  - **Size:** ~350 lines + tests

- [ ] **M0-CORE-05 · Pools: `ArenaPool<T>` and `Pool<T>`**
  - **Refs:** PRD §9.1 (S-2), §10.4; AGENTS PERF-003, CPP-002/007
  - **Depends:** M0-CORE-01
  - **Scope:**
    - `laige::ArenaPool<T>`: arena-scoped, budgeted, reset-per-frame lifetime, capacity accounting.
    - `laige::Pool<T>`: stable-handle pool (handle = index + generation, CPP-007), bounded, overflow → `Status::BudgetExhausted` (never grows silently).
    - Both report bytes/counts to a simple accounting sink (feeds the M1 profiler).
    - Unit tests: budget exhaustion returns error; reset semantics; generation-checked stale handle detection in debug.
  - **Verify:** `ctest -R pools` green; exhaustion test asserts `Status` error, no crash, no leak (ASan).
  - **Size:** ~300 lines + tests

- [ ] **M0-CORE-06 · Deterministic PRNG**
  - **Refs:** PRD §10.3 (seeded, per-substream); AGENTS ARCH-010
  - **Depends:** M0-CORE-01
  - **Scope:**
    - `laige::Prng`: xorshift128+ (or equivalent, documented), seed from a 64-bit value; per-substream derivation via documented hash (substream id, e.g. subsystem id).
    - API: `next_u64()`, `next_range(min, max)`, `next_float01()` — all deterministic, documented bit-exactness scope per ADR 0002.
    - Unit tests: golden vector (fixed seed → fixed first N outputs, checked in), substream independence sanity, period sanity (no short cycles).
  - **Verify:** `ctest -R prng` green; golden-vector test fails if the algorithm changes (intentional — algorithm is part of the determinism contract).
  - **Size:** ~150 lines + tests

- [ ] **M0-CORE-07 · Config JSON: value type + parser**
  - **Refs:** FR-1.5 (M1 consumes it), M0-DEC-03; PRD §8.2 (NFR-8.7 fuzz), AGENTS TEST-005
  - **Depends:** M0-CORE-01, M0-DEC-03
  - **Scope:**
    - `laige::JsonValue` + parser per D-JSON decision: bounded (depth, size limits documented), no recursion blowup, malformed input → `Status` error (never crash, never silent).
    - Serializer for round-trip of simple structures.
    - Fuzz target `json_parse` registered with the fuzz runner.
    - Unit tests: valid/invalid/malformed corpus (nested depth limit, huge number, truncated input, encoding edge cases).
  - **Verify:** `ctest -R config_json` green; `laige-fuzz json_parse --runs=1000` clean under ASan.
  - **Size:** ~400 lines + tests (split here if the parser exceeds budget: `M0-CORE-07a` parser, `M0-CORE-07b` serializer+fuzz)

- [ ] **M0-CORE-08 · Budget harness (histogram + budget checks)**
  - **Refs:** PRD §8.1, CORE-001; AGENTS §12 (benchmark report requirements)
  - **Depends:** M0-CORE-01
  - **Scope:**
    - `laige::histogram`: fixed-capacity histogram with p50/p95/p99/mean/min/max (no allocation after construction).
    - `laige::timeit` scope timer; `laige::budget_check(name, histogram, budget.json entry)`: returns pass/fail with before/after numbers formatted per AGENTS §12 report fields (hardware/OS/compiler/build/workload recorded by the caller harness).
    - `budgets.json` schema at repo root with the PRD §8.1 numbers as named entries (values may start as `0` = "not yet measured" for M2+ budgets).
  - **Verify:** `ctest -R budget_harness` green: synthetic workload produces percentiles; budget check fails loudly when a threshold is exceeded.
  - **Size:** ~250 lines + tests

## Tooling

- [ ] **M0-TOOL-01 · API manifest generator**
  - **Refs:** NFR-13.1; PRD §9.4
  - **Depends:** M0-BUILD-01, M0-CORE-01
  - **Scope:**
    - `laige-api` CMake target: a small parser over public headers (or a documented script) that emits `laige-api.json` — every public symbol: signature, doxygen summary, budget annotations, `@experimental` flag.
    - CI check: manifest regenerates identically (no stale manifest); every public symbol in headers appears in the manifest.
    - Keep the scanner deliberately narrow (doxygen-comment-driven) so it stays maintainable.
  - **Verify:** `cmake --build build --target laige-api` produces `laige-api.json`; adding a public symbol without regenerating fails CI; manifest for current (small) API is complete.
  - **Size:** ~400 lines (split if needed: scanner vs CI check)

- [ ] **M0-TOOL-02 · Determinism checker skeleton**
  - **Refs:** FR-11.5; AGENTS ARCH-010, TEST-004
  - **Depends:** M0-CORE-06, M0-CORE-08
  - **Scope:**
    - `laige-detcheck` binary: runs a named scenario binary in two build configurations (e.g. Debug+ASan vs Release; or two compiler builds) and compares per-tick state hashes (hash API arrives with M1-DET-03; for now accept a hash-file output contract).
    - Scenario contract documented: scenario binary prints `<tick> <hash>` lines.
    - Wire into CI as a job that is skipped until a real scenario exists (M1-SAMPLE-01), but the tool itself is tested with a synthetic two-run scenario.
  - **Verify:** `laige-detcheck --scenario=synthetic` passes on identical builds and fails when the synthetic scenario is perturbed (test fixture).
  - **Size:** ~200 lines + test

## Test infrastructure & docs

- [ ] **M0-TEST-01 · Test infrastructure conventions**
  - **Refs:** AGENTS TEST-001/003/005; PRD §14
  - **Depends:** M0-DEP-01, M0-CORE-07
  - **Scope:**
    - GTest integration conventions: test dirs mirror `src/` modules; each module's tests named `<module>_tests`.
    - Regression-test convention documented: every bug fix test named `regress_<short-id>`, must fail before the fix (AGENTS TEST-003) — recorded in `docs/testing.md`.
    - Fuzz runner `laige-fuzz`: registers targets, bounded runs in CI (`--runs=1000`), nightly long runs documented.
    - Seed handling for all randomized tests (fixed default seed, overridable) so CI is deterministic.
  - **Verify:** convention doc exists; fuzz runner runs the `json_parse` target; a seeded random test passes identically on two CI runs (checkable via artifact logs).
  - **Size:** ~150 lines + docs

- [ ] **M0-DOC-01 · `docs/` skeleton + index**
  - **Refs:** AGENTS §13 (DOC-001…DOC-007), PRD NFR-8.12
  - **Depends:** M0-DEC-01, M0-DEC-02, M0-DEC-03
  - **Scope:**
    - Create the full AGENTS §13 structure: `docs/README.md` (index linking every section, honest about incomplete areas), `getting-started/`, `concepts/`, `api/`, `guides/`, `debugging/`, `benchmarks/` (with an empty `baselines/`), `decisions/` (index + the three ADRs from M0-DEC), `compatibility/`.
    - `docs/benchmarks/methodology.md`: required report fields per AGENTS §12, and how budget entries in `budgets.json` map to them.
  - **Verify:** `docs/README.md` links every section; no dead links (checked in CI or manual); the three ADRs are referenced from the decisions index.
  - **Size:** docs only

## Milestone gate

- [ ] **M0-EXIT-01 · M0 exit gate**
  - **Refs:** PRD §15 M0 exit criteria
  - **Depends:** all other M0 steps
  - **Scope:**
    - Confirm and record: (1) CI green on all 3 P0 OSes (link CI run URLs), (2) all `laige-core` unit tests pass on all 3 OSes, (3) budget harness runs end-to-end on a synthetic workload and writes a baseline file to `docs/benchmarks/baselines/m0-synthetic.md` with full AGENTS §12 metadata.
    - Mark milestone complete in `roadmap/README.md` Progress Board.
  - **Verify:** gate checklist item all checked with evidence links; no open M0 step remains.
  - **Size:** docs only
