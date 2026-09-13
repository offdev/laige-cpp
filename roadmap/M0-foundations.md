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

- [x] **M0-REPO-01 · Repository skeleton**
  - **Refs:** PRD §10.1 (module map), NFR-8.8, NFR-8.10; AGENTS §13 (docs layout)
  - **Depends:** M0-DEC-01
  - **Scope:**
    - Top-level `README.md` (project overview, build pointer), `LICENSE` (per M0-DEC-01), `.gitignore`.
    - Root `CMakeLists.txt`: CMake ≥ 3.22, C++20, `-Wall -Werror`, exceptions/RTTI disabled for engine targets, static+shared option placeholders.
    - Directory layout per PRD §10.1: `src/` (one dir per module, only `laige-core` populated now), `deps/`, `tests/`, `tools/`, `docs/`, `samples/`, `third_party` placeholder for vendored code.
  - **Verify:** `cmake -S . -B build` configures cleanly; `cmake --build build` succeeds (empty target).
  - **Size:** ~100 lines (mostly CMake/docs)

- [x] **M0-BUILD-01 · `laige-core` target + canonical commands**
  - **Refs:** NFR-8.8, NFR-8.9, NFR-8.10; README §1 canonical commands
  - **Depends:** M0-REPO-01
  - **Scope:**
    - `laige-core` library target: static and shared builds both work (`LAIGE_BUILD_SHARED` option); no transitive dep leakage (CPP-010).
    - Document the canonical configure/build/test/sanitizer/fuzz/bench commands exactly (README §1 table becomes the source of truth); `docs/getting-started/building.md`.
    - CMake options used by later steps: `LAIGE_ASAN`, `LAIGE_TSAN`, `LAIGE_SCRIPT` (reserved), `LAIGE_BUILD_TESTS` (default ON).
  - **Verify:** static and shared builds both configure+build with zero warnings; `docs/getting-started/building.md` matches the actual commands (spot-checked).
  - **Size:** ~150 lines CMake + docs

- [x] **M0-DEP-01 · Dependency lock + vendored GoogleTest**
  - **Refs:** PRD §11 (dep table, `deps.lock`, NFR-8.6), DEP-005
  - **Depends:** M0-REPO-01
  - **Scope:**
    - `deps.lock`: JSON file listing each vendored dependency with name, version, source commit/URL, SHA-256 of the vendored tree, license, justification ref (PRD §11 row).
    - Vendor GoogleTest (dev-only, per PRD §11) under `deps/googletest/`; wire it into tests only, never linked into engine libs.
    - CMake check that `deps.lock` hashes match the vendored trees (fails loudly on mismatch).
  - **Decision (2026-09-10):** vendored **GoogleTest v1.18.0** (commit
    `063de7e9…`, complete tagged tree, BSD-3-Clause) in
    `deps/googletest`; `deps.lock` (repo root, one entry per dep, tree
    SHA-256); configure-time verification in
    `cmake/laige-deps-lock.cmake` (re-hashes every tree, also rejects
    unlisted `deps/` subdirectories); `gtest_main` wired into `tests/` only
    (`BUILD_GMOCK`/`INSTALL_GTEST` off; no-exceptions/no-rtti flags match the
    engine test TUs for ODR safety); `laige-core_tests` converted to the
    first GTest suite. ADR 0004 written.
  - **Verify:** hash check passes; tampering a vendored file makes the CMake
    configure/test fail; `ctest` runs one trivial test via GTest.
    (Verified 2026-09-10: fresh + shared + ASan + Clang trees configure,
    build warning-clean, `ctest` 1/1; tampered vendored file fails the
    configure with expected-vs-actual hashes; restored tree passes again.)
  - **Size:** ~150 lines (scripts + lock) + vendored tree

## CI

- [x] **M0-CI-01 · CI matrix: all P0 platforms**
  - **Refs:** PRD §6 (AC-6.1), §14; NFR-8.8
  - **Depends:** M0-BUILD-01
  - **Scope:**
    - CI workflow with one job per P0 OS: Linux (g++ and clang++), Windows (MSVC 2022), macOS (arm64 + Intel).
    - Each job: configure → build → `ctest` (unit tests only so far).
    - Cadence per PRD §14: one P0 OS per PR, all three per merge (implement as labels or merge-gate).
  - **Verify:** pushing a trivial change runs all jobs and they are green; matrix runs in < 10 min (PRD §8.1 build budget starts counting here).
    (Verified 2026-09-10: full 5-job matrix — linux-gcc, linux-clang,
    windows-msvc, macos-arm64, macos-intel — green after pushing
    `a90191c..7ec98b9`; each job is capped at `timeout-minutes: 10` and
    jobs run in parallel, so the < 10 min budget is enforced by the
    workflow itself.)
  - **Size:** workflow files only

- [x] **M0-CI-02 · Sanitizer CI jobs**
  - **Refs:** NFR-8.2 (ASan+UBSan, TSan); AGENTS TEST-006
  - **Depends:** M0-CI-01
  - **Scope:**
    - Linux jobs building with `LAIGE_ASAN=ON` (ASan+UBSan) and `LAIGE_TSAN=ON`, running the unit suites.
    - Fail build on any sanitizer report; reports archived as CI artifacts.
  - **Verify:** introducing a deliberate OOB read in a scratch test fails the ASan job (test removed afterwards); TSan job green on clean code.
    (Mechanics verified locally 2026-09-10, Clang 22.1.8, and the full
    CI Verify cycle completed the same day on the pushed commits —
    see the end of this note.
    `ci.yml` gains two lanes on merge and `ci-pull.yml` on every PR not
    selecting another P0 OS (Linux default), each a canonical
    configure → build → ctest with `timeout-minutes: 10`:
    - `linux-asan`: `LAIGE_ASAN=ON`, canonical `build-asan` tree,
      clang++; `ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:
      detect_leaks=1:log_path=…/asan-reports/asan` on the test step
      (UBSan is already fatal via `-fno-sanitize-recover=all`).
    - `linux-tsan`: `LAIGE_TSAN=ON`, canonical `build-tsan` tree,
      clang++; `TSAN_OPTIONS=halt_on_error=1` per test as wired by
      `tests/` (M0-BUILD-01).
    - Reports archived as artifacts on every run, green or red: tee'd
      ctest output (carries reports on stderr), ASan per-process report
      files under `asan-reports/`, and `<tree>/Testing/Temporary/
      LastTest.log`; uploads run under job-scoped `actions: write`
      (GitHub Actions has no step-level permissions — the first CI run
      rejected the workflow file for exactly that, fixed before the run
      executed) and `continue-on-error: true` in `ci-pull.yml` (fork-PR
      read-only token).
    Local verification of the exact Verify scenario: on a scratch OOB
    read (`tests/laige-core/sanitizer-scratch.cpp`, runtime-volatile
    index so `-Wall -Werror` stays clean) the ASan lane's ctest failed
    non-zero with a fatal report ("index 16 out of bounds for type
    'int[4]'", report file written via `log_path`) while the same
    scratch kept the TSan lane green (100% passed, exit 0); a separate
    deliberate data race failed the TSan lane with a
    `WARNING: ThreadSanitizer: data race` report in ctest output
    (exit 8). Scratch removed afterwards; both lanes re-ran green on
    the clean tree. CI verification of the exact Verify scenario:
    scratch OOB read pushed as `6331a40`; the first `ci.yml` run
    rejected the workflow file (step-level `permissions:` is not a
    valid schema — fixed in `f75ed0d` by moving `actions: write` to
    job scope); with the fix, `linux-asan` failed with the UBSan
    "index 16 out of bounds for type 'int[4]'" report (archived in
    the `linux-asan-reports` artifact) while `linux-tsan` and the
    other five jobs stayed green; scratch removed in `25b57b9` and
    the full 7-job matrix ran green.)
  - **Size:** workflow changes only

- [x] **M0-CI-03 · Include-graph lint + dependency-count metric**
  - **Refs:** NFR-8.11, NFR-8.13; PRD §10.1 dependency rule
  - **Depends:** M0-BUILD-01
  - **Scope:**
    - Script (in `tools/`) that parses `#include` edges of `src/**` and enforces: `laige-core` includes nothing internal; arrows only downward; no include of `deps/` outside the owning module's boundary (DEP-004).
    - Same script reports the vendored-dependency count; CI asserts ≤ 10 (PRD §11) and prints the list.
    - Both run in CI on every PR.
  - **Decision (2026-09-10):** `tools/laige-include-lint` (Python 3,
    stdlib only — runs on every P0 runner without setup). Parses the
    `#include` edges of `src/**` (textual; backslash continuations handled)
    and enforces: **R1** `laige-core` includes nothing internal;
    **R2** arrows only downward in the PRD §10.1 stack (`MODULE_STACK` in
    the script mirrors the PRD order; cross-module includes must go
    through the target's public include root `src/<module>/include` —
    CPP-010); **R3** a vendored dep is includable only from the module
    recorded as its `owner` in `deps.lock` (both relative paths into
    `deps/` and angle-bracket includes of vendored header paths are
    caught via a vendored-header map); **R4** engine code includes only
    `src/**` or `deps/**`. Structural problems (unknown `src/` module,
    source outside a module, malformed/missing `deps.lock` or `owner`,
    missing vendored tree, ambiguous include paths) exit 2; include
    violations exit 1; pass exits 0. Ownership needed a machine-readable
    home, so `deps.lock` gains a required `owner` field (`tests` or
    `src/laige-<module>`), validated by `cmake/laige-deps-lock.cmake`
    (9 fields/entry) and by the lint. The metric reads `deps.lock`, prints
    the list (name, version, owner, license, justification) and fails
    above the PRD §11 budget of 10. CI: a new `include-lint` job in
    `ci-pull.yml` (every PR, independent of the ci:* label selector) and
    `ci.yml` (every merge) — platform-independent, one ubuntu runner,
    `timeout-minutes: 5`. CTest coverage in `tests/tools`: four fixture
    trees (clean graph; one violation per rule R1–R4; 11-dep budget
    overrun; unknown `src/` module) plus a real-tree check that runs in
    every P0 job. Expected-failure tests assert exit code **and** output
    content via a generated `cmake -P` check script — CTest inverts
    `PASS_REGULAR_EXPRESSION` under `WILL_FAIL` (verified on CMake 4.4.3),
    so content cannot be asserted with CTest properties.
  - **Verify:** (verified locally 2026-09-10, GCC 16.2.1 + Clang 22.1.8;
    pushed as `785af81`; CI observed 2026-09-10 via the GitHub API —
    the `ci.yml` (8-job) run on push `741c163` (run 34518244428) is
    green, including the new `include-lint` job, whose job log shows
    the live report: `count: 1 (budget: 10, PRD §11)` and `OK — 2
    source file(s) scanned, 1 allowed internal include edge(s)`; the
    `ci-pull.yml` include-lint job was exercised by this step's own PR
    #1 (run 34521473503 on `5b66cca`, green — the 5 relevant jobs incl.
    `include-lint`, same live report in its job log); the post-merge
    `ci.yml` (8-job) run 34521722826 on `a74b65a` is green):
    (a) an illegal include fails the lint — a future `laige-render` header
    stub plus a `laige-core` file including it → `R1`, exit 1; the same
    stub from `laige-sim` → `R2` (upward); a `src/` module including
    `<gtest/gtest.h>` → `R3` (owner: tests); a legitimate downward
    `laige-sim → laige-core` include passes; (b) dep count prints and
    passes: `count: 1 (budget: 10, PRD §11)` with the googletest entry;
    (c) fixtures: clean → exit 0; violations → exit 1 with exactly 6
    violations (one per rule file); 11-dep lock → exit 2 “exceed the PRD
    §11 budget of 10”; `src/laige-audio` → exit 2 “not a PRD §10.1
    module”; (d) scratch removed, clean tree green; `ctest` 6/6 on the
    fresh g++ tree, the shared tree, the ASan+UBSan tree, and a fresh
    Clang tree.
  - **Size:** 439 lines script + 233 lines CTest fixtures/template + ~140
    lines workflow/lock/docs (over the ~200-line estimate: the script's
    doc header carries the rule contract, and the milestone rule's
    "unit tests in the same change" is satisfied by the fixture CTest
    suite — cohesive, not split)

## laige-core

- [x] **M0-CORE-01 · `Result<T,E>` / `Status` + error registry**
  - **Refs:** FR-12.1, NFR-13.3; AGENTS CORE-008
  - **Depends:** M0-BUILD-01
  - **Scope:**
    - `laige::Result<T, E>` and `laige::Status` (no exceptions): success/value or error code.
    - Central error-code registry: stable integer codes, each with `{code} | {what} | {why} | {fix} | {doc_anchor}` template text (NFR-13.3 grammar).
    - Unit tests: construction, error propagation, no exceptions raised (linker-level: build with `-fno-exceptions`).
  - **Decision (2026-09-10):** `laige::Result<T, E = ErrorCode>` with
    inline `std::optional<T>` storage (no heap, no allocation, O(1)
    accessors); `Result()` deleted (API-008: an empty result is
    unrepresentable); SFINAE-guarded implicit constructors from `T`/`E`
    make error propagation natural (`return e;` / `return v;`), and the
    unambiguous `success()`/`failure()` factories are the only path when
    `T` and `E` are mutually convertible; `value()`/`error()` assert the
    state in debug (undefined in release, documented) while
    `valueIfOk()`/`errorIfError()` are the null-safe pointer reads.
    `laige::Status` is the value-less result: success by default,
    implicit from `ErrorCode`. Registry: flat value-indexed table in
    `errors.cpp` — 4 codes (`unknown` 1, `invalid_argument` 2,
    `malformed_input` 3, `budget_exhausted` 4); 0 is reserved as the
    no-error sentinel and unregistered values render `unknown` (CORE-008).
    Each entry carries its pre-rendered NFR-13.3 line
    `{codeId} | {what} | {why} | {fix} | {docAnchor}`; the human-readable
    registry is `docs/api/errors.md`.
  - **Verify:** `ctest -R result_status` green — 16 GTest cases: success/
    failure construction (implicit + factory forms), error and value
    propagation through call chains, copy/move (incl. move-only payload),
    5-field grammar per registered code, field↔rendered-text equivalence,
    anchor format, pinned integer values, unregistered → `unknown`.
    "No exceptions raised" at the linker level: the test TU compiles with
    `-fno-exceptions -fno-rtti` (NFR-8.10) and self-checks the policy with
    `static_assert`s (a violation fails the build). Verified locally
    2026-09-10 (GCC 16.2.1: static, shared, ASan/UBSan, TSan trees; fresh
    Clang 22.1.8 tree — all 7/7 ctest, zero warnings). CI: the first push
    failed on Windows x64 (MSVC 2022) with C2535 — the two converting
    constructors have identical parameter lists when `T == E` (MSVC rejects
    duplicate member declarations; GCC/Clang accept the SFINAE-guarded
    pair) — fixed in `6fa1414` by taking the failure value by `const E&`,
    which keeps the signatures distinct for every `(T, E)`. `ci.yml` run 34525402022 on
    `6fa1414`: full 8-job matrix green (linux-gcc, linux-clang,
    windows-msvc, macos-arm64, macos-intel, linux-asan, linux-tsan,
    include-lint), the Windows job compiled `result_status_tests.cpp` and
    passed `result_status`; every job ran in under a minute (well inside
    the 10-minute budget).
  - **Size:** 339 lines implementation (`errors.h` 68, `result.h` 157,
    `errors.cpp` 114) + 309 lines tests (over the ~250-lines estimate: the
    headers carry the full AGENTS §9 API contracts and the registry
    pre-renders its grammar line next to its fields — cohesive, not split)

- [x] **M0-CORE-02 · Structured logging facade**
  - **Refs:** AGENTS.md §14 (LOG-001…LOG-007); FR-12.2
  - **Depends:** M0-CORE-01
  - **Scope:**
    - One logging facade: severity (Trace…Fatal per §14), stable subsystem+event names, lazy field/message evaluation (no formatting/allocation when disabled — LOG-003), per-subsystem level filters.
    - Sink interface with a console sink and a file sink; per-subsystem scopes; rate limiting with suppressed-count summary (LOG-004); crash/shutdown flush (LOG-007).
    - Unit tests: disabled levels allocate nothing (assert with allocation counter from M0-CORE-05 if available, else ASan leak-free + timing property test); rate-limit summary emitted after N repeats.
  - **Decision (2026-09-10):** single process-lifetime Meyers-singleton
    facade `laige::log::Logger` (thread-safe construction) owning the
    current `Sink` via `unique_ptr`. `LAIGE_LOG_*` macros put the level
    gate **before** argument evaluation — a disabled event costs one
    atomic load + branch: no message/field evaluation, no formatting,
    no allocation, no lock (LOG-003). Gating = atomic global minimum
    (checked first) + per-subsystem level table (one facade mutex, small
    linear scan; init-phase mutation only, CONC-001). Fields: scalar
    values render locale-free into a 64-byte stack buffer via
    `std::to_chars`; string-like values copy in full; field values must
    avoid spaces/`=` (the line is `| k=v` machine-parseable, LOG-001).
    Rate limiting (LOG-004) applies to Warn/Error/Fatal only, per
    `(subsystem, event, severity)` key: the first event is always
    recorded (an `everEmitted` flag — a `time_point::min()` sentinel
    overflowed the window subtraction, CPP-004), repeats are counted
    and reported as a stable `rate_limited` summary event
    (`event=<orig>`, `suppressed=N`) at window rollover and at shutdown
    (pending counts are drained, so a burst right before shutdown still
    reports). `Fatal` records, flushes, and terminates via
    `std::abort()` (AGENTS §14 controlled termination). Sinks:
    `ConsoleSink` (does not own the stream — `stderr` must outlive the
    process), `FileSink` (owns the `FILE*`; `create()` returns
    `Result<unique_ptr<FileSink>>`, open failure → `ErrorCode::IoError`;
    LOG-007 minimal fallback = keep the console sink and report). Crash
    handling: POSIX `sigaction` (SA_RESETHAND, one-shot) for
    SIGSEGV/ABRT/BUS/FPE/ILL + raw `write(2)` notice + allocation-free
    `try_lock` sink flush + re-raise; Windows vectored SEH; registration
    failure → `Status` (`IoError`), idempotent install. `shutdown()`
    idempotent (CONC-006): drain summaries → flush → retire (post-shutdown
    logs discarded). Timestamps: `system_clock`, UTC RFC 3339
    `YYYY-MM-DDTHH:MM:SS.ffffffZ` rendered with a vendored-in-code
    Hinnant `civil_from_days` (no libc date functions; thread-safe).
    Two small additive extensions of M0-CORE-01 shipped with this step:
    `ErrorCode::IoError` (`5`, `docs/api/errors.md#io-error`) and
    `Result::takeValue() &&` (move the success value out of an rvalue
    result — needed to hand a created `FileSink` into `LoggerOptions`);
    both tested in the `result_status` suite. Test-only process-wide
    allocation counter (strong global `operator new` overrides in a test
    TU) proves the zero-alloc property; it is excluded from the ASan/TSan
    trees (the sanitizer runtimes define their own `new`/`delete`) —
    there the same spam loop runs leak-free plus the timing property
    test, which is exactly the fallback the step names.
  - **Verify:** `ctest -R logging` green — 27 GTest cases across
    `LogGate` (default/per-subsystem/global gating, disabled event
    reaches no sink), `LogRecord` (scalar field formatting, full string
    copy, identity + timestamp format), `LogSinks` (exact console/file
    line format, dtor flush, create failure → `IoError`), `LogRateLimit`
    (suppression, summary after window with `suppressed=N`, window edge,
    key independence, Warn/Error/Fatal only, opt-out, shutdown drains
    pending summaries), `LogFatal` (gate; forked child emits + flushes +
    dies on SIGABRT with the line in the file), `LogCrash` (install
    idempotence, shutdown flush, post-shutdown discard),
    `LogConcurrency` (4 threads × 250 emits, all recorded), and
    `LogPerformance` (100k disabled Trace events → **0 allocations** via
    the allocation counter; disabled ≈46 ns/event vs ≈1207 ns/event
    enabled, N=200000). Verified locally 2026-09-10 (GCC 16.2.1: static,
    shared, ASan/UBSan, TSan trees — all 8/8 ctest, zero warnings; fresh
    Clang 22.1.8 static + shared trees — all 8/8 ctest, zero warnings;
    `ctest -R logging` green in every tree). CI (observed 2026-09-10
    via the GitHub API): `ci-pull.yml` run 34534697621 on `0d3ee28`
    green — all 5 jobs of the default-Linux lane passed (linux-gcc
    g++, linux-clang clang++, linux-asan+UBSan clang++, linux-tsan
    clang++, include-lint), finished in 50 s; macOS/Windows jobs
    skipped (label-gated).
  - **Size:** ~1034 lines implementation (`logging.h` 538 — full AGENTS
    §9 contracts, facade, macros — `logging.cpp` 496) + ~860 lines tests
    (`logging_tests.cpp` 743, allocation counter 115) + ~300 lines
    docs/CMake (over the ~350-lines estimate, same pattern as
    M0-CORE-01 and M0-CI-03: the header carries the API contract and the
    tests prove the step's Verify clauses — zero-alloc, rate-limit
    summary, Fatal termination — cohesive, not split)

- [x] **M0-CORE-03 · SimMath interface + `fp32_pinned` backend**
  - **Refs:** PRD §10.3, S-7; ADR 0002 (`fp32_pinned` backend); AGENTS CORE-005
  - **Depends:** M0-DEC-02, M0-CORE-01
  - **Scope:**
    - SimMath op interface (add/sub/mul/div, compare, clamp, lerp, normalize, length) plus the `fp32_pinned` backend: IEEE `float` ops with ADR 0002's pinned flag set (no FMA in sim translation units, `-ffp-contract=off`/equivalent per compiler, no reassociation, no floating-point intrinsics) applied and documented.
    - Ops are the *only* math allowed in deterministic sim code (enforcement comes later in M1-DET-01; here: provide the API + docs).
    - Unit tests: property tests (associativity guards, NaN/inf handling is *defined* and tested — a documented policy, not "whatever the CPU does").
  - **Verify:** `ctest -R math_float` green; documented NaN/Inf policy exists in header docs; pinned flag set documented and applied to sim targets.
  - **Size:** ~250 lines + tests

- [x] **M0-CORE-04 · SimMath `fpx16_16` backend (default)**
  - **Refs:** PRD §10.3, FR-3.3 (fixed-point option); ADR 0002 (`fpx16_16` backend); M0-DEC-02
  - **Depends:** M0-CORE-03
  - **Scope:**
    - `laige::fpx16_16`: signed Q16.16; add/sub/mul (rounded, documented), divide, negate, compare, convert from/to `int32_t`/`float`; overflow defined (saturate) and documented; no UB under any input (CPP-004).
    - Wire it in as the **default** SimMath backend (ADR 0002) with the same op surface as M0-CORE-03.
    - Unit tests including exhaustive edge cases (min/max, wrap candidates, rounding ties).
  - **Decision (2026-09-10):** `laige::fpx16_16` — `int32_t raw`,
    value = raw / 2^16, range [-32768, 32767.99998474], resolution 2^-16.
    All ops compute in `int64_t` and **saturate** (defined for every
    input — CPP-004, no UB); rounding is **round-to-nearest,
    ties-to-even** for mul/div/toInt32/fromFloat (the fixed-point
    analogue of IEEE round-to-nearest-even; no ties exist for
    sqrt-of-integer), divide by zero is defined (`x/0 → ±max`, sign of
    x; `0/0 → +0` — the saturation analogue of IEEE ±inf), and
    `negate(min) = max`. No NaN/Inf exist: comparisons are a total
    order, `isFinite` always true. The type has no implicit scalar
    constructors and no arithmetic operators (construct via
    `fromInt32`/`fromFloat`; compute via the SimMath ops). `Fpx16_16`
    is the backend (delegates to the type's static ops) and
    `SimMathFpx16` the alias — the DEFAULT backend per ADR 0002 (the
    `determinism.math` config plumbing lands with the config step,
    FR-1.5). The M0-CORE-03 template gained two backend-classification
    methods (`isNaN`/`isInf`, delegated — additive; fp32 semantics
    unchanged) and `Scalar{0.0}` → `Scalar{}` in `normalize`
    (identical for both backends). Documented accuracy bound:
    `length` is accurate while the sum of squares stays in the Q16.16
    range (|v| ≲ 181.02 per axis-aligned component), saturating
    beyond — local sim math stays inside; world-span distances belong
    to M1-DET-02.
  - **Verify:** `ctest -R math_fixed` green — 24 GTest cases across
    `FixedPointBasics` (exact values, identities, total order),
    `FixedPointRounding` (exhaustive ties-to-even cases for mul/div/
    toInt32/fromFloat, sqrt rounding — no ties possible),
    `FixedPointSaturation` (min/max, wrap candidates, divide-by-zero,
    conversion saturation), `FixedPointConversions` (int/float round
    trips, 20k-raw LCG scan), `FixedPointSimMath` (op surface,
    lerp/clamp/normalize, the length accuracy bound),
    `FixedPointDispatch` (stateless compile-time dispatch, `noexcept`
    contract, backend contract), and `FixedPointDeterminism` (a fixed
    4096-tick op sequence run through the SimMath ops and through an
    independent raw-int64 reference agree bit-for-bit, and the
    sequence's FNV-1a state hash equals the committed known-answer
    constant 0xF02728762777C581). Verified locally 2026-09-10: green
    under ASan+UBSan (`build-asan`, canonical `LAIGE_ASAN=ON`), and the
    known-answer hash is identical on **two different compiler builds**
    — g++ 16.2.1 (`build`) and clang++ 22.1.8 (`build-clang`), each
    10/10 ctest (the CI hookup for this property lands in M1-DET-04).
    Static + shared (NFR-8.9) and TSan trees also green.
  - **Size:** 251 lines `fpx16_16.h` + ~60 lines `sim_math.h` additions
    + 22 lines `sim_math_fixed.cpp` (implementation) + 677 lines tests
    + ~150 lines docs/CMake (over the ~350-line estimate: the header
    carries the full rounding/saturation/overflow policy next to the
    code, and the test suite proves the step's Verify clauses — ties,
    saturation, conversions, and the two-implementation determinism
    property — cohesive, not split)

- [x] **M0-CORE-05 · Pools: `ArenaPool<T>` and `Pool<T>`**
  - **Refs:** PRD §9.1 (S-2), §10.4; AGENTS PERF-003, CPP-002/007
  - **Depends:** M0-CORE-01
  - **Scope:**
    - `laige::ArenaPool<T>`: arena-scoped, budgeted, reset-per-frame lifetime, capacity accounting.
    - `laige::Pool<T>`: stable-handle pool (handle = index + generation, CPP-007), bounded, overflow → `Status::BudgetExhausted` (never grows silently).
    - Both report bytes/counts to a simple accounting sink (feeds the M1 profiler).
    - Unit tests: budget exhaustion returns error; reset semantics; generation-checked stale handle detection in debug.
  - **Decision (2026-09-11):** header-only `include/laige/pools.h`
    (no new .cpp: both pools are templates). `ArenaPool<T>`: one
    contiguous aligned block, bump cursor, `reset()` destroys the live
    prefix and rewinds (O(inUse), idempotent); slots are `uint32_t`,
    valid until the next `reset()`; no free list (in-frame recycling is
    not an arena property). `Pool<T>`: one element block + per-slot
    generation table (starts at 1; 0 reserved — the default handle is
    never valid) + LIFO free-list stack + alive flag; `destroy()`
    bumps the generation (a stale handle can never pass again, except a
    defined 2^32 unsigned wrap of one slot — documented as the one case
    the scheme does not rule out) and recycles the slot (O(1));
    `handle = {index, generation}` (CPP-007); `clear()` is O(capacity)
    (reset-per-frame workloads use the arena). Both: `Options{capacity}`
    (uint32, the declared budget, S-6/API-006) fixed at construction —
    the only backing allocation (setup path); every hot-path op O(1),
    allocation-free, lock-free, noexcept (PERF-003/006); overflow →
    `ErrorCode::BudgetExhausted` (4, already registered — no new code),
    `destroy()` on an invalid/stale handle → `ErrorCode::InvalidArgument`
    (2); the pool does not log — the owning system logs the failure
    under its subsystem name (LOG-002, G-R1), keeping a failing create
    a branch on the cold path. Move-only (O(1) pointer swap; a
    moved-from pool is a valid empty pool; move assignment destroys the
    destination's current elements first); the destructor destroys
    every live element (no leak). Accounting: `stats()` returns
    `laige::PoolStats` — capacity/inUse/peakInUse (FR-11.4 peak
    tracking), totalCreated churn (G-R4), bytesCapacity (element slots
    + per-slot bookkeeping) and bytesInUse — a plain value the M1
    profiler (FR-11.4) pulls; no registration/callback (CORE-004: the
    sink interface lands with the profiler). Element storage is a plain
    `ElementSlot<T>` struct (`alignas(T) std::byte[...]`) — not
    `std::aligned_storage`, which is deprecated since C++23 and whose
    layout changed across implementations (GCC 16's C++26-era
    libstdc++ makes the outer class an empty 1-byte wrapper — using it
    directly would silently allocate 1-byte slots; caught by this
    step's byte-accounting tests). Single-owner, not thread-safe
    (CONC-001; M1 defines the cross-thread boundaries, CONC-002);
    deterministic by construction (pure integer bookkeeping, ARCH-010).
    API contract: `docs/api/pools.md`.
  - **Verify:** `ctest -R pools` green — 25 GTest cases across
    `ArenaPoolBasics` (slots, read-back, reset semantics, peak/churn
    survival, zero budget, move), `ArenaPoolBudget`, `PoolBasics`
    (LIFO recycle, slot stability), `PoolBudget`, `PoolStale`
    (destroy/clear/reuse/default handles; generation bump `gen+1`;
    double-destroy; the stale-handle `at()` assert proven in a forked
    child dying on SIGABRT — POSIX jobs; Windows skips with a reason),
    `PoolDestruction` (destructor/clear/reset destroy counts balanced —
    leak-free), `PoolStats` (element/byte counts incl. the aligned-
    stride case), `PoolMove`. The exhaustion tests assert the `Status`
    error (`BudgetExhausted`), no crash, no growth, and reusability
    after reset/destroy. `Tracked` ctor/dtor counters prove every
    placement-new has a matching destroy (independently leak-free under
    ASan). Verified locally 2026-09-11: full 11/11 ctest (incl.
    `pools` and the real-tree include-lint) on GCC 16.2.1 (`build`
    static, `build-shared` shared, `build-asan` ASan+UBSan fatal,
    `build-tsan` TSan `halt_on_error=1`) and a Clang 22.1.8 tree
    (`build-clang`), zero warnings under the NFR-8.10 policy. CI
    observed 2026-09-11 via the GitHub API — PR #5 `ci-pull.yml` run
    34593510387 green (5/5 executed: linux-gcc, linux-clang,
    linux-asan, linux-tsan, include-lint; macOS/Windows are merge-lane
    jobs, skipped on PRs — the Windows job will additionally prove MSVC
    compilation of this header at merge).
  - **Size:** 458 lines header (`pools.h` — full AGENTS §9 contracts
    next to the code) + 584 lines tests + 228 lines docs + ~7 lines
    CMake (over the ~300-line estimate, same pattern as M0-CORE-01…04:
    the header carries the API contract and the tests prove the step's
    Verify clauses — exhaustion, reset, stale handles, accounting —
    cohesive, not split)

- [x] **M0-CORE-06 · Deterministic PRNG**
  - **Refs:** PRD §10.3 (seeded, per-substream); AGENTS ARCH-010
  - **Depends:** M0-CORE-01
  - **Scope:**
    - `laige::Prng`: xorshift128+ (or equivalent, documented), seed from a 64-bit value; per-substream derivation via documented hash (substream id, e.g. subsystem id).
    - API: `next_u64()`, `next_range(min, max)`, `next_float01()` — all deterministic, documented bit-exactness scope per ADR 0002.
    - Unit tests: golden vector (fixed seed → fixed first N outputs, checked in), substream independence sanity, period sanity (no short cycles).
  - **Decision (2026-09-11):** header-only
    `include/laige/prng.h`. Core: xorshift128+ transcribed from and
    verified against the reference implementation (lemire/SIMDxorshift
    `xorshift128plus.c`) — state `(part1, part2)`, step
    `part1=o1; t=o0^(o0<<23); part2=t^o1^(t>>18)^(o1>>5); out=part2+o1`
    (unsigned wrap), all-zero state excluded. Seeding: first two
    splitmix64 outputs from `seed + K` (`K = 0x9E3779B97F4A7C15`, named
    `kSplitmix64Increment`; mix constants named) — a bijection, so no
    seed reaches the zero state. Substreams: documented derivation
    `deriveSubstream(seed, id) == Prng(seed + id*K)` (id 0 == master;
    composes: `derive(derive(s,i),j) == derive(s,i+j)`). `next_range`:
    Lemire unbiased reduction (reject `r >= 2^64 - (2^64 mod n)`; fast
    path when `n | 2^64`); `min >= max` is a debug assert / documented
    UB. `next_float01`: `next_u64() >> 40 * 2^-24` — exactly
    `k*2^-24` (24-bit resolution, `[0,1)`, bit-exact; the only float in
    the API, multiplied by the exactly representable `kFloat01Unit`).
    `seedState`/`stepState` are public statics for determinism
    verification and M1 save/replay (a saved stream is
    `(seed, part1, part2)`). Value type (copy = shared stream position,
    documented), single-owner, not thread-safe (CONC-001), no
    allocation, no exceptions/RTTI (NFR-8.10). Determinism scope
    (ARCH-010/ADR 0002): pure unsigned integer arithmetic + one exact
    power-of-two scale => cross-platform bit-exact; the golden vectors
    are replay fixtures and intentionally fail on algorithm change.
    **Period: every nonzero state has period exactly 2^128 - 1 —
    proven, not sampled:** the test reconstructs the state map's
    characteristic polynomial over GF(2) from a 512-bit probe orbit via
    Berlekamp-Massey (C is the *reciprocal* of the characteristic
    polynomial — the recurrence relates `p[t]` to lower indices),
    checks `P(A)=0` on all 128 basis states, then verifies `P`
    irreducible (Ruffini: `x^(2^128)≡x` and
    `gcd(x^(2^d)-x,P)=1` for `d∈{1,2,4,8,16,32,64}`) and primitive
    (`P | x^(2^128-1)-1`, no `q`-th root for the 9 prime factors of
    `2^128-1 = 3·5·17·257·641·65537·6700417·274177·67280421310721`,
    primality by Miller-Rabin, factorization by portable 128-bit
    multiply — no `__int128`/builtins, MSVC-compatible). API contract:
    `docs/api/prng.md`.
  - **Verify:** `ctest -R prng` green — 18 GTest cases across
    `PrngGolden` (32-draw golden KAT on seed `0x1234567890ABCDEF`,
    FNV-1a-of-first-4096 replay hash `0xB64E76173859B6D8`, 10^4-step
    reference transcription check, zero-state seeding spot check over
    100k seeds), `PrngRange` (5 KATs incl. the power-of-two fast path,
    span-1 identity, 110k in-bounds draws over 10 spans, unbiasedness:
    2^18 draws over 8-way and 3-way spans within ~10 sigma),
    `PrngFloat01` (8-draw KAT, 2^16 draws: dyadic exactness, `[0,1)`
    bounds, mean 0.5 ± 14 sigma), `PrngSubstreams` (8 id KATs, id-0 ==
    master, derivation composition, same-id determinism, 2^16-draw
    cross-substream overlap check), `PrngPeriod` (the committed
    characteristic-polynomial proof above + empirical screen: no
    duplicate in 4M draws and no period-q window pattern for the 8 small
    prime factors of 2^128-1). The golden-vector KATs are intentionally
    algorithm-sensitive (the algorithm is the determinism contract).
    Verified locally 2026-09-11: full 12/12 ctest (incl. `prng`) on
    GCC 16.2.1 (`build` static, `build-shared` shared, `build-asan`
    ASan+UBSan fatal, `build-tsan` TSan `halt_on_error=1`) and Clang
    22.1.8 (`build-clang`) — the KAT constants are identical across the
    two compilers (local two-compiler run; CI hookup lands in
    M1-DET-04), zero warnings under the NFR-8.10 policy.
  - **Size:** 255 lines header (`prng.h` — full AGENTS §9 contracts
    next to the code) + 809 lines tests (incl. the self-contained
    portable GF(2)/BM/Miller-Rabin proof machinery) + 195 lines docs +
    ~12 lines CMake (over the ~150-line estimate, same pattern as
    M0-CORE-01…05: the header carries the API contract; the period
    proof — the step's "period sanity" clause — is a full algebraic
    proof rather than a sample check, which the GF(2) machinery costs)

- [x] **M0-CORE-07 · Config JSON: value type + parser**
  - **Refs:** FR-1.5 (M1 consumes it), M0-DEC-03; PRD §8.2 (NFR-8.7 fuzz), AGENTS TEST-005
  - **Depends:** M0-CORE-01, M0-DEC-03
  - **Scope:**
    - `laige::JsonValue` + parser per D-JSON decision: bounded (depth, size limits documented), no recursion blowup, malformed input → `Status` error (never crash, never silent).
    - Serializer for round-trip of simple structures.
    - Fuzz target `json_parse` registered with the fuzz runner.
    - Unit tests: valid/invalid/malformed corpus (nested depth limit, huge number, truncated input, encoding edge cases).
  - **Decision (2026-09-11):** hand-rolled bounded parser in
    `laige-core` — no dependency (ADR 0003). Recursive descent with an
    RAII depth guard; bounds from `JsonOptions` (defaults: 1 MiB document,
    depth 32, both inclusive; `maxDepth <= 0` rejects every container).
    **Every** parse failure — grammar, bad escapes, duplicate object
    keys, raw control characters, invalid UTF-8 (overlong / raw
    surrogate / > U+10FFFF), lone surrogate halves in `\uXXXX`, over
    size/depth — is `ErrorCode::MalformedInput` (3): no new codes,
    because the registry entry for code 3 already names the ADR 0003
    bounds (CORE-004). Numbers: JSON number → `double` via
    `std::strtod`; a well-formed overflow token (e.g. `1e999`) stores
    ±inf — a *valid parse result*, callers must reject non-finite
    (config validation, M1); integer literals beyond ±2^53 lose
    precision (documented policy). `JsonValue`: plain members; deep
    copy (O(size)), O(1) move with moved-from == Null (documented);
    the "owns exactly the payload its kind names" invariant is kept by
    `clearPayload()` on every kind change and every assignment.
    Equality: deep; objects order-insensitive, arrays order-sensitive,
    NaN != NaN. Serializer: canonical compact ASCII (control chars and
    every codepoint > 0x7F as `\uXXXX`, surrogate pairs above U+FFFF;
    numbers the shortest correctly rounded decimal via a `%.*g`
    search over p = 1..17 — deliberately *not* `std::to_chars`, which
    AppleClang 15 (macos-14 lane) lacks for floats; `-0.0` → `0`);
    `parse(serialize(v)) == v` for all finite values; serializing a
    non-finite number is a documented precondition violation.
    `laige-fuzz`: the *minimal* deterministic fuzz runner lands in this
    step because the Verify gate requires it (Prng-seeded, default
    seed `0x1F055EED`, `--runs`/`--seed`, three input modes — mutate /
    truncate / random bytes — over a 19-document ASCII corpus; any
    Status is acceptable, only a crash fails); target `json_parse`
    registered. M0-TEST-01 extends it (CI lane semantics, nightly long
    runs, seed documentation). API contract: `docs/api/json.md`.
  - **Verify:** `ctest -R config_json` green — 25 GTest cases across
    `ConfigJsonValid` (all six kinds; DBL_MAX / denorm_min / ±inf
    overflow / 2^53+1 rounding; escapes, surrogate pairs, strict
    UTF-8, DEL; containers, document order, depth-32 and 1 MiB
    boundary documents), `ConfigJsonInvalid` (empty/trailing data,
    truncation, bad numbers, bad escapes, lone surrogates, raw
    controls, invalid UTF-8, duplicate keys, depth 33, 1 MiB+2 —
    every case asserts `MalformedInput`, not merely an error),
    `ConfigJsonRoundTrip` (parse→serialize→parse value stability and
    serializer idempotence over a corpus incl. 32-deep nesting and
    escaped strings; canonical forms pinned), `ConfigJsonValue`
    (factories, deep copy, moved-from Null, in-place replacement, kind
    transitions, deep equality incl. NaN != NaN and object
    order-insensitivity, total `findMember`, churn), and
    `ConfigJsonOptions` (maxDepth 1/2; maxDocumentBytes inclusive bound
    and 0). `laige-fuzz json_parse --runs=1000` clean under ASan: the
    instrumented `fuzz_json_parse` CTest entry passed in the ASan tree
    and a direct `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` run is
    clean (1000 deterministic Prng-driven runs, no crash, no sanitizer
    report). Verified locally 2026-09-11: full 14/14 ctest on
    GCC 16.2.1 (`build` static, `build-shared` shared, `build-asan`
    ASan+UBSan fatal, `build-tsan` TSan `halt_on_error=1`) and a
    Clang 22.1.8 tree — zero warnings under the NFR-8.10 policy;
    CI will additionally prove MSVC (windows lane) and AppleClang
    (macos lane) compilation of the new sources.
  - **Size:** 270 lines header (`json.h` — full AGENTS §9 contracts
    next to the code) + 826 lines implementation + 620 lines tests +
    208 lines fuzz runner + ~22 lines fuzz CMake + ~12 lines CMake
    wiring (over the ~400-line estimate; kept cohesive rather than
    split into `M0-CORE-07a/07b`, same pattern as
    M0-CORE-01…06: the header carries the API contract and the tests
    prove the step's Verify clauses — depth/size bounds, the malformed
    corpus, round-trip, value semantics — in one suite)

- [x] **M0-CORE-08 · Budget harness (histogram + budget checks)**
  - **Refs:** PRD §8.1, CORE-001; AGENTS §12 (benchmark report requirements)
  - **Depends:** M0-CORE-01
  - **Scope:**
    - `laige::histogram`: fixed-capacity histogram with p50/p95/p99/mean/min/max (no allocation after construction).
    - `laige::timeit` scope timer; `laige::budget_check(name, histogram, budget.json entry)`: returns pass/fail with before/after numbers formatted per AGENTS §12 report fields (hardware/OS/compiler/build/workload recorded by the caller harness).
    - `budgets.json` schema at repo root with the PRD §8.1 numbers as named entries (values may start as `0` = "not yet measured" for M2+ budgets).
  - **Decision (2026-09-11):** three-piece harness in `laige-core` plus the
    `laige-bench` tool — `docs/getting-started/building.md` reserved the
    canonical command `./build/bin/laige-bench --suite=<name>` for this
    step, and M0-EXIT-01 needs a runnable end-to-end harness.
    `laige::Histogram`: fixed-capacity **rolling window** — `record()`
    is O(1) and allocation-free (one index arithmetic + one store); a
    full window drops the oldest sample and `totalRecorded()` exposes
    the truncation (CORE-008: no silent drop); `stats()` is a cold-path
    O(n log n) pass with **no allocation** (sorts a pre-allocated
    scratch buffer) and yields exact min/mean/p50/p95/p99/max over the
    stored window with **nearest-rank percentiles** (rank =
    ceil(p·n/100) in exact integer math — no fractional indices,
    bit-identical on every platform); an empty window is `n = 0` with
    NaN statistics. `laige::TimeIt`: `steady_clock` scope timer in
    milliseconds, no allocation, immutable start point (safe const
    reads). `loadBudgets`: file I/O + bounded parse (ADR 0003 — the 1 MiB
    bound is enforced before the read) + **strict schema v1 validation**
    (ARCH-007: unsupported version, unknown/missing field, bad
    name/metric/unit charset, duplicate name, negative or non-finite
    number all rejected — `MalformedInput`; unreadable file →
    `IoError`; no new error codes). `budgetCheck` is total (it cannot
    fail as an operation): empty histogram → `passed = false` + loud
    `NO_SAMPLES` (a workload that recorded nothing is a broken harness,
    CORE-008); `target > 0` → `measured <= target` (every PRD §8.1 budget
    is an at-most upper bound); `target == 0` is a **hard-zero budget**
    (e.g. `sim_heap_allocs`: zero steady-state heap allocations per
    frame), **not** "not set" — the "not yet measured" marker lives in
    `measured` (initial 0, the M0 convention). Report: stable 4-line
    machine-greppable text (LOG-001) —
    `budget=<name> result=<PASS|FAIL|NO_SAMPLES> metric=<m> unit=<u>` /
    `after=<v> before=<v> target=<v>` / stats line via
    `laige::formatStatsLine` (the single source of the stats text) /
    `context: workload=… build=… machine=… warmup=…` — the AGENTS §12
    machine/build facts are recorded by the caller's harness (the tool
    supplies compiler + build type; the operator supplies the machine via
    `LAIGE_BENCH_MACHINE`). `budgets.json` (repo root, schema v1):
    **all 15 PRD §8.1 targets** as named entries (sim tick, 50k-sprite
    scene, cold start, build time, and the zone server each carry two
    budgets); `measured: 0` = not yet measured until their subsystems
    land (M1+). `laige-bench`: `--suite` registry (M0: `synthetic` — a
    deterministic, allocation-free 4096-step LCG + double stand-in
    workload with the named Marsaglia LCG64 constants, no RNG),
    `--runs`/`--warmup` (defaults 1000/100), `--budget=<name>` check with
    `--budgets` path resolution (argument → `LAIGE_BUDGETS_PATH` env →
    `budgets.json`), `--report=<path>` append; exit codes 0 pass / 1
    usage or load failure / **2 budget check failure** (CI-gateable —
    PRD §8.1 policy). API contract: `docs/api/budget_harness.md`.
    **Bug fix found by this step's Verify run (M0-CORE-07):** the JSON
    parser rejected object members separated by `", "` — the object key
    goes through `parseString` directly (not `parseValue`, which does
    the whitespace skip), so `parseObjectMembers` was missing one
    `skipWhitespace`; the repo-root `budgets.json` (hand-formatted)
    demonstrated it. Fixed in `json.cpp` with a regression test
    (`ConfigJsonValid.ObjectMemberWhitespace`, fails pre-fix) — the
    documented grammar ("whitespace only between tokens") was already
    correct; the implementation did not match it.
  - **Verify:** `ctest -R budget_harness` green — **22 GTest cases**:
    `BudgetHarnessHistogram` (known percentiles on 1..100; single-sample
    window; full ring dropping the oldest with observable churn;
    capacity-0 drop-all; empty → NaN stats; ordering invariants over a
    20k-sample deterministic PRNG window; reset/churn semantics; stable
    `formatStatsLine` text), `BudgetHarnessTimeIt` (non-negative and
    monotonic; positive after bounded work; restart on reset),
    `BudgetHarnessCheck` (PASS under target; **loud FAIL when a
    threshold is exceeded** — the step's Verify clause, with a distinct
    structured `result=FAIL` report; loud `NO_SAMPLES` on an empty
    histogram; hard-zero-target budget semantics; named-metric dispatch;
    caller context in the report; stable first report line),
    `BudgetHarnessTable` (the repo-root `budgets.json` loads with 15
    well-formed entries — `frame_time_render` p95 8.3 ms,
    `sim_heap_allocs` target 0 — and a 13-case schema-rejection corpus:
    malformed JSON, bad version, root not an object, missing field,
    unknown field, bad metric, duplicate name, negative target,
    non-finite `1e999` target, empty name, bad name charset, bad unit,
    empty workload; missing file → `IoError`; an empty budgets array is
    valid). The **synthetic workload producing percentiles** is also
    exercised end-to-end by the `laige_bench_smoke` CTest entry
    (`laige-bench --suite=synthetic --runs=50`, output must carry
    `stats: n=50`). Verified locally 2026-09-11: full `ctest` (16/16
    entries, zero warnings under the NFR-8.10 policy) on GCC 16.2.1
    (`build` static, `build-shared` shared, `build-asan` ASan+UBSan
    fatal, `build-tsan` TSan `halt_on_error=1`) and Clang 22.1.8
    (`build-clang`). CI will additionally prove MSVC (windows lane) and
    AppleClang (macos lane) compilation of the new sources.
  - **Size:** ~350 lines header (`budget_harness.h`, full AGENTS §9
    contract) + ~330 lines `budget_harness.cpp` + ~250 lines
    `laige-bench.cpp` + CMake (~30) + ~560 lines tests + ~210 lines docs
    + ~110 lines `budgets.json` + a 6-line `json.cpp` fix and a 39-line
    regression test (over the ~250-line estimate: the header carries the
    API contract, the tests prove the Verify clauses — percentiles, loud
    FAIL on exceeded threshold, the schema corpus — and `laige-bench`
    implements the canonical command this step reserved in
    `building.md`, so M0-EXIT-01 has a runnable harness; cohesive, not
    split — same pattern as M0-CORE-01…07)

## Tooling

- [x] **M0-TOOL-01 · API manifest generator**
  - **Refs:** NFR-13.1; PRD §9.4
  - **Depends:** M0-BUILD-01, M0-CORE-01
  - **Scope:**
    - `laige-api` CMake target: a small parser over public headers (or a documented script) that emits `laige-api.json` — every public symbol: signature, doxygen summary, budget annotations, `@experimental` flag.
    - CI check: manifest regenerates identically (no stale manifest); every public symbol in headers appears in the manifest.
    - Keep the scanner deliberately narrow (doxygen-comment-driven) so it stays maintainable.
  - **Verify:** `cmake --build build --target laige-api` produces `laige-api.json`; adding a public symbol without regenerating fails CI; manifest for current (small) API is complete.
  - **Size:** ~400 lines (split if needed: scanner vs CI check)
  - **Decision (2026-09-12):** single C++20 tool
    `tools/api/laige-api-scanner` (~1,900 lines — over the ~400-line
    estimate: the scanner is a line-oriented state machine with loud
    failure on every unsupported construct (CORE-008), not a regex
    pass, and it reuses `laige::parseJson` for the `--check` diff, so
    one file carries scanner + CLI + manifest I/O; splitting adds a
    second target for no maintainability gain at this size) plus a
    `laige-api` custom target (the checked-in `laige-api.json` at the
    repo root is regenerated by `cmake --build build --target
    laige-api`). Manifest: `version: 1`, `generatedBy: "laige-api"`,
    `headers[]` (sorted, repo-relative), `symbols[]` with fixed keys
    `name/kind/header/line/signature/summary/budget/experimental` —
    deterministic (no timestamps), so byte-identical regeneration is
    the CI drift check. Symbols: `class`, `struct`, `enum`, `function`,
    `method`, `constructor`, `destructor`, `variable`, `alias`,
    `enumerator`, `macro`; fully qualified names (macros bare);
    public-only access, `namespace detail` excluded, out-of-line member
    definitions and forward declarations add no entries. Doc
    association: consecutive `//` blocks directly above the declaration
    (blank comment line = paragraph break; banner lines finalize;
    physical blank line / closing brace / non-transparent directive
    discard; `@budget <text>` and `@experimental` tags parsed).
    Unsupported constructs fail loudly (block comments, raw strings,
    lambdas at declaration level, function-pointer parameter types,
    `operator()`, `extern "C"` blocks, compound typedefs, `#define` in
    class bodies). `--check FILE`: byte compare; on mismatch a
    symbol-level diff (added/removed/changed, keyed by
    name+header+line, max 20 shown) via `laige::parseJson`. Exit
    0 OK / 1 stale / 2 error. CI: `api-manifest` job in
    `ci-pull.yml`/`ci.yml` regenerates and fails on drift; CTest
    (`tests/api`) covers the fixture tree (exact manifest bytes),
    fresh/stale checks, the unsupported-construct failure, the root
    error, and the real-tree drift check in every P0 job. Current
    manifest: 376 symbols from 10 headers (all in `laige-core`; the
    other M0 modules have no `include/` directories yet).

- [x] **M0-TOOL-02 · Determinism checker skeleton**
  - **Refs:** FR-11.5; AGENTS ARCH-010, TEST-004
  - **Depends:** M0-CORE-06, M0-CORE-08
  - **Scope:**
    - `laige-detcheck` binary: runs a named scenario binary in two build configurations (e.g. Debug+ASan vs Release; or two compiler builds) and compares per-tick state hashes (hash API arrives with M1-DET-03; for now accept a hash-file output contract).
    - Scenario contract documented: scenario binary prints `<tick> <hash>` lines.
    - Wire into CI as a job that is skipped until a real scenario exists (M1-SAMPLE-01), but the tool itself is tested with a synthetic two-run scenario.
  - **Verify:** `laige-detcheck --scenario=synthetic` passes on identical builds and fails when the synthetic scenario is perturbed (test fixture).
  - **Decision (2026-09-12):** `laige-detcheck` in `tools/detcheck`
    (single C++20 file, over the ~200-line estimate: the normative
    scenario contract is the file's header comment — same pattern as
    M0-TOOL-01 — and portable scenario execution needs both fork/exec
    + pipe capture (POSIX) and CreateProcessW + PeekNamedPipe (Windows)
    so the tool builds on every P0 platform). Two modes:
    `--scenario=synthetic|synthetic-perturbed` (built-in 32-body
    fpx16_16 + Prng workload, two in-process runs — pure integer
    arithmetic, bit-exact per ADR 0002) and the real M1-DET-04 mode
    `--run-a=<bin> --run-b=<bin> [-- scenario-args...]` (two builds of
    one scenario compared). Scenario contract (strict, enforced,
    bounded): one stdout line per tick `<tick> <hash>` — tick starts at
    0, step 1, no padding; hash = 16 lowercase hex digits (the 64-bit
    state hash's canonical text form; the algorithm is NOT part of the
    contract — lines compare byte-for-byte); trailing newline optional,
    trailing `\r` tolerated; stderr ignored; exit 0 on completion.
    Bounds: ≤ 65536 ticks, ≤ 64 bytes/line. Report: stable
    `detcheck scenario=<name> result=OK|DIVERGED [first_diff_tick=<t>]`
    + run-a/run-b lines (the diverging tick pair, or stream-length notes
    when one run ends early). Exit 0 = match · 1 = divergence (loud,
    CORE-008) · 2 = usage/unknown scenario/scenario failure/contract
    violation. Contract doc: `docs/api/detcheck.md` (normative text in
    the tool header). Tests: 9 CTest entries in `tests/detcheck`
    (synthetic self-check; perturbation fixture — built-in and as a
    fixture binary; identical/different cross-binary pairs; malformed
    output; scenario exit failure; stream-length mismatch; unknown
    scenario), each a generated `cmake -P` check script asserting exit
    code + output fragments (tests/api pattern; WILL_FAIL inversion and
    crash-vs-failure reasons as documented there). Fixture: one source,
    five compiled variants (clean/perturbed/bad/fail/short) — the
    synthetic two-run scenario. CI: `detcheck` job in ci-pull.yml/ci.yml
    (every PR and merge, no ci:* condition — tooling check, not a P0 OS
    build) runs the synthetic self-check and SKIPS the real-scenario
    step until M1-SAMPLE-01 exists (M1-DET-04 activates the
    two-configuration comparison). M1-DET-03's world.state_hash
    replaces the scenarios' ad-hoc FNV computation; the tool's
    line-comparison is unchanged.
  - **Size:** ~1,400 lines (over estimate: contract header + dual-
    platform process execution + 9-test suite + fixture variants +
    docs/api/detcheck.md; see Decision)

## Test infrastructure & docs

- [x] **M0-TEST-01 · Test infrastructure conventions**
  - **Refs:** AGENTS TEST-001/003/005; PRD §14
  - **Depends:** M0-DEP-01, M0-CORE-07
  - **Scope:**
    - GTest integration conventions: test dirs mirror `src/` modules; each module's tests named `<module>_tests`.
    - Regression-test convention documented: every bug fix test named `regress_<short-id>`, must fail before the fix (AGENTS TEST-003) — recorded in `docs/testing.md`.
    - Fuzz runner `laige-fuzz`: registers targets, bounded runs in CI (`--runs=1000`), nightly long runs documented.
    - Seed handling for all randomized tests (fixed default seed, overridable) so CI is deterministic.
  - **Decision (2026-09-12):** `docs/testing.md` is the normative
    conventions doc (layout, `regress_<short-id>`, fuzz lane semantics,
    seed handling; linked from `docs/README.md`, `tests/README.md`,
    `building.md`, and `README.md`). Seed handling:
    `tests/support/laige_test_seed.h` (test-only header) — `TestSeed()`
    returns the fixed default `kDefaultTestSeed = 0x1F055EED`, identical
    to laige-fuzz's `kDefaultSeed` (one documented default seed
    repo-wide) unless `LAIGE_TEST_SEED` is set (0x-hex or decimal, read
    at call time); a set-but-unparseable value records a loud test
    failure with the offending value and falls back to the default
    (CORE-008; `ADD_FAILURE` — `GTEST_FAIL` is void-return only);
    `TestPrng(id)` derives a per-test substream from a stable named id
    so two tests never share a stream position.
    `tests/testing/test_infra_tests` (CTest entry `test_infra`, suite
    `SeededRandom`, 6 cases): known-answer FNV-1a hashes of 65536 draws
    under the default seed (`0x7EA4049545656830`) and a documented
    override seed (`0x535D2CA741B61CBF`), first-8-draw KAT, the
    default/fuzz seed identity, the loud invalid-`LAIGE_TEST_SEED` path
    (`EXPECT_NONFATAL_FAILURE` from `gtest-spi.h`), the seed parser, and
    substream isolation — each KAT prints a machine-greppable
    `test-seed-check` line before asserting, so two CI runs of the same
    commit show identical lines in the job logs and the archived
    `Testing/Temporary/LastTest.log` (the step's cross-run Verify
    clause). First `regress_` test: the M0-CORE-08 bug-fix test renamed
    `ConfigJsonValid.ObjectMemberWhitespace` →
    `ConfigJsonValid.regress_json_object_member_ws` (suite unchanged, so
    `ctest -R config_json` still covers it; the historical M0-CORE-08
    step record is unchanged). Fuzz lane: no new CI job — the bounded
    run is the existing `fuzz_json_parse` ctest entry inside every P0
    job's ctest (PRD §14 "every commit (bounded)"); the nightly long
    form (`--runs=1000000`) is documented in `docs/testing.md` §3 and
    added to the canonical command table (`building.md`); the scheduled
    nightly lane lands with the first M1 fuzz target (asset import /
    network packets, PRD §14 fuzz row).
  - **Verify:** convention doc exists; fuzz runner runs the `json_parse`
    target (`ctest -R fuzz_json_parse` green in every local tree and
    inside every P0 job's ctest in CI); a seeded random test passes
    identically on two CI runs (byte-identical `test-seed-check` lines
    in the archived `Testing/Temporary/LastTest.log` of the Linux ASan
    job — verified across two CI runs of the same commit, and again
    across commits e2abce5 → c8b8221, in both cases
    byte-identical):

      ```text
      test-seed-check default seed=0x000000001f055eed stream=1 draws=65536 fnv1a=0x7ea4049545656830
      test-seed-check override seed=0x2468acce01234567 stream=2 draws=65536 fnv1a=0x535d2ca741b61cbf
      ```

    CI runs: 34713354925 / 34714039135 (same-commit pair, byte-identical
    lines) and 34728782624 (e2abce5) / 34746755055 (c8b8221, the final
    commit — byte-identical lines, all 10 jobs green including Windows
    32/32). Local (2026-09-12): 32/32 ctest with zero warnings under the
    NFR-8.10 policy on g++ 16.2.1 (`build` static, `build-shared`
    shared, `build-asan` ASan+UBSan fatal, `build-tsan` TSan
    `halt_on_error=1`) and Clang 22.1.8 (`build-clang`); the seeded KAT
    line is identical on the g++ and clang++ trees locally
    (cross-compiler identity, confirmed in CI above).
  - **Size:** ~1,000 lines (over the ~150 estimate: same pattern as
    M0-CORE-01…08 — `docs/testing.md` carries the conventions,
    `laige_test_seed.h` the seed contract next to the code, and the
    `SeededRandom` suite proves the step's Verify clauses — seeded KAT,
    override, loud failure, substream isolation — cohesively rather
    than split)

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
