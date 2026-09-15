# Building Laige

This document is the **source of truth** for the canonical build commands.
The canonical-commands table in [roadmap/README.md](../../roadmap/README.md)
§1 mirrors it; later roadmap steps MUST use these exact forms.

## Prerequisites

- CMake ≥ 3.22 (NFR-8.8): single configure, no autotools, no network access
  needed to build.
- A C++20 compiler (NFR-8.10) for one of the P0 platforms (PRD §6):

  | P0 platform | Toolchain |
  |---|---|
  | Linux (x64/arm64) | GCC or Clang |
  | Windows (x64) | MSVC 2022 (clang-cl secondary) |
  | macOS (arm64/Intel) | AppleClang |

- All dependencies are vendored in-tree (NFR-8.8): no network access is
  needed to build. The only vendored dependency so far is GoogleTest
  (dev-only, PRD §11), used by `tests/` only and integrity-checked against
  `deps.lock` on every configure (M0-DEP-01).

## Canonical commands

| Purpose | Command |
|---|---|
| Configure | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` |
| Build | `cmake --build build -j` |
| Test | `ctest --test-dir build --output-on-failure` |
| ASan/UBSan build | `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_ASAN=ON` |
| Test (ASan/UBSan tree) | `ctest --test-dir build-asan --output-on-failure` |
| TSan build | `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_TSAN=ON` |
| Test (TSan tree) | `ctest --test-dir build-tsan --output-on-failure` |
| Headless run | `./build/bin/laige-run --headless CONFIG.json [--ticks N]` |
| Fuzz (bounded) | `./build/bin/laige-fuzz <target> --runs=1000` |
| Fuzz (long, nightly form) | `./build/bin/laige-fuzz <target> --runs=1000000` |
| Benchmarks | `./build/bin/laige-bench --suite=<name>` |
| Determinism check | `./build/bin/laige-detcheck --scenario=<name>` |
| API manifest | `cmake --build build --target laige-api` |
| Include-graph lint + dependency count | `python3 tools/laige-include-lint` |

Notes:

- `Debug` is the canonical `CMAKE_BUILD_TYPE`; `Release` is supported.
- The tool rows above the lint row are live targets now: `laige-run`
  (M1-HEAD-01: `--headless CONFIG.json [--ticks N] [--replay LOG]`,
  exit codes 0/1/2, the `laige_run_smoke` CTest entry is its CI form —
  contract in [docs/api/engine.md](../api/engine.md)), `laige-fuzz`
  (M0-CORE-07: the `json_parse` target and deterministic bounded runs;
  M0-TEST-01 documents the CI lane semantics — bounded `--runs=1000` in
  every P0 job's `ctest`, the nightly long-run form above — and the
  seed-handling rules), `laige-bench` (M0-CORE-08), `laige-detcheck`
  (M0-TOOL-02), and target `laige-api` (M0-TOOL-01). Their command forms
  were fixed here when they were reserved, so no step can drift them.
  Fuzz and randomized-test seeds: fixed default `0x1F055EED`,
  overridable (`laige-fuzz --seed=…`; tests via the `LAIGE_TEST_SEED`
  environment variable) — see [docs/testing.md](../testing.md).
- Include-graph lint (M0-CI-03): platform-independent (Python 3 stdlib
  only, no setup). It parses the `#include` edges of `src/**` and enforces
  the PRD §10.1 rules (laige-core includes nothing internal; arrows only
  downward in the module stack; vendored deps only from their `deps.lock`
  `owner`), then prints the vendored-dependency list and fails above the
  PRD §11 budget of 10. CI runs it on every PR and merge (job
  `include-lint` in `ci-pull.yml`/`ci.yml`), and the CTest suite runs it
  against the real tree in every build job (`tests/tools`). On Windows use
  `python tools\laige-include-lint`.
- Test (TSan tree): registered tests automatically run with
  `TSAN_OPTIONS=halt_on_error=1` (wired in `tests/<module>/CMakeLists.txt`
  when `LAIGE_TSAN=ON`), so a data race makes `ctest` fail with a non-zero
  exit — no extra environment setup needed.

## Build trees and artifacts

| Tree | Configure flags | Contents |
|---|---|---|
| `build/` | (default) | Engine libraries (static), tests, tools |
| `build-shared/` | `-DLAIGE_BUILD_SHARED=ON` | Same, engine libraries shared (NFR-8.9) |
| `build-asan/` | `-DLAIGE_ASAN=ON` | Same, whole tree instrumented with ASan+UBSan |
| `build-tsan/` | `-DLAIGE_TSAN=ON` | Same, whole tree instrumented with TSan |

Runtime artifacts (test binaries, future tools) land in `<tree>/bin/`,
libraries in `<tree>/lib/` — the canonical commands above reference
`build/bin/` accordingly.

## Build options

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` | Build type (canonical: `Debug`). |
| `LAIGE_BUILD_SHARED` | `OFF` | `OFF`: engine libraries are static; `ON`: shared (NFR-8.9). Both variants are built with position-independent code so they link identically. |
| `LAIGE_BUILD_TESTS` | `ON` | Build `tests/` and register it with CTest. |
| `LAIGE_ASAN` | `OFF` | Instrument the whole tree with AddressSanitizer + UBSan (NFR-8.2). UBSan reports are fatal: any UB aborts the process. |
| `LAIGE_TSAN` | `OFF` | Instrument the whole tree with ThreadSanitizer (NFR-8.2); registered tests run with `TSAN_OPTIONS=halt_on_error=1`. Mutually exclusive with `LAIGE_ASAN` — configuring both fails loudly. |
| `LAIGE_SCRIPT` | `OFF` | Reserved for the scripting module (M4); no effect in M0. |

## Sanitizer builds (NFR-8.2)

- **ASan/UBSan** (`LAIGE_ASAN=ON`): every target — engine libraries, tests,
  future tools — compiles and links with
  `-fsanitize=address,undefined -fno-sanitize-recover=all
  -fno-omit-frame-pointer` (GCC/Clang/AppleClang). MSVC 2022 uses the
  documented equivalent `/fsanitize=address` (plus the MSVC-supported UBSan
  subset where available).
- **TSan** (`LAIGE_TSAN=ON`): `-fsanitize=thread` (GCC/Clang/AppleClang),
  `/fsanitize=thread` (MSVC 2022).
- Any sanitizer report therefore fails the build/test loudly: ASan aborts
  on the first error, UBSan is made fatal by `-fno-sanitize-recover=all`,
  and TSan failures are fatal to the test process via
  `TSAN_OPTIONS=halt_on_error=1`. CI (M0-CI-02) archives the reports as
  artifacts.
- Sanitizer builds are separate build trees (`build-asan`, `build-tsan`);
  the options are mutually exclusive and configuring both is rejected.
- MSVC sanitizer flags are wired but verified on the Windows CI job
  (M0-CI-01); the Linux flag set is the reference implementation.

## Engine compiler policy (NFR-8.10)

Every engine target is passed through `laige_apply_engine_policy()`
(root `CMakeLists.txt`):

- GCC/Clang/AppleClang: `-Wall -Werror -fno-exceptions -fno-rtti`
- MSVC 2022: `/W4 /WX /EHs- /EHc- /GR-` plus `-D_HAS_EXCEPTIONS=0`
  (the documented equivalent; the define switches the MS STL to its
  no-exception code paths, because the STL gates its own `try/catch` on
  `_HAS_EXCEPTIONS`, not on `/EHs-`)
- C++20 required. The requirement propagates to consumers; the warning
  flags do not (CPP-010 — a game linking the engine keeps its own compiler
  policy).

## SimMath pinned-math policy (M0-CORE-03, ADR 0002)

Every target that carries deterministic sim math is passed through
`laige_apply_simmath_policy()` (root `CMakeLists.txt`) — in M0 that is
`laige-core` and the `laige-core_tests` executable; from M1 on, every
sim module joins the list (e.g. `laige-sim`). The flags pin the
IEEE float semantics of the `fp32_pinned` backend
(`src/laige-core/include/laige/sim_math.h` is the source of truth for the
pinned set and the NaN/Inf policy):

- GCC/Clang/AppleClang: `-ffp-contract=off -fno-associative-math`
  (no FMA contraction of `a*b+c`, no reassociation — the pinned set is
  visible on every compile line).
- MSVC 2022: `/fp:precise` (MSVC does not FMA-contract C expressions and
  never reassociates at this setting).
- Banned in sim translation units: `-ffast-math` /
  `-funsafe-math-optimizations` / `/fp:fast`, floating-point
  intrinsics, rounding-mode changes, FP exception modes (re-audited at
  every toolchain upgrade, ADR 0002).

## Current status (M0)

- `laige-core` builds as a static library (default) or a shared library
  (`-DLAIGE_BUILD_SHARED=ON`). It carries the version/build identifier
  (`include/laige/core/version.h`, `version.cpp`) plus the first functional
  engine code from M0-CORE-01: `laige::Result<T,E>` / `laige::Status` and
  the error-code registry (`include/laige/result.h`,
  `include/laige/errors.h`, `errors.cpp`; error text follows the NFR-13.3
  5-field grammar — see [docs/api/errors.md](../api/errors.md)), the
  structured logging facade from M0-CORE-02 (`include/laige/logging.h`,
  `logging.cpp`; API contract in
  [docs/api/logging.md](../api/logging.md)), and the SimMath
  deterministic-math interface with both backends — `fp32_pinned` from
  M0-CORE-03 and the default `fpx16_16` from M0-CORE-04
  (`include/laige/sim_math.h`, `include/laige/fpx16_16.h`,
  `sim_math.cpp`, `sim_math_fixed.cpp`; API contract, NaN/Inf policy,
  and the fpx16_16 rounding/saturation policy in
  [docs/api/sim_math.md](../api/sim_math.md), pinned flags via
  `laige_apply_simmath_policy()`), and the memory pools from M0-CORE-05
  (`include/laige/pools.h`: `laige::ArenaPool<T>` and `laige::Pool<T>`  with `laige::PoolStats` accounting; API contract in  [docs/api/pools.md](../api/pools.md)), and the bounded JSON parser +  serializer from M0-CORE-07 (`include/laige/json.h`, `json.cpp`:  `laige::JsonValue`, `parseJson`, `serializeJson`, `JsonOptions`;  API contract in [docs/api/json.md](../api/json.md)).
- `tests/laige-core/laige-core_tests` is a CTest link smoke test (a
  GoogleTest suite since M0-DEP-01) that runs in every build tree above: it
  verifies the static/shared link and checks the NFR-8.10 policy flags with
  `static_assert` (a policy violation fails the build).
- `result_status` is the M0-CORE-01 CTest entry: a filtered view of the
  same `laige-core_tests` executable covering the `ResultStatus`, `Status`,
  and `ErrorCodeRegistry` suites — the step's Verify command is
  `ctest -R result_status`.
- `logging` is the M0-CORE-02 CTest entry: a filtered view of the same
  executable covering the `LogGate`, `LogRecord`, `LogSinks`,
  `LogRateLimit`, `LogFatal`, `LogCrash`, `LogConcurrency`, and
  `LogPerformance` suites — the step's Verify command is
  `ctest -R logging`.
- `math_float` is the M0-CORE-03 CTest entry: a filtered view of the same
  executable covering the `SimMathBasics`, `SimMathNanInf`,
  `SimMathProperties`, and `SimMathDispatch` suites — the step's Verify
  command is `ctest -R math_float`.
- `math_fixed` is the M0-CORE-04 CTest entry: a filtered view of the same
  executable covering the `FixedPointBasics`, `FixedPointRounding`,
  `FixedPointSaturation`, `FixedPointConversions`, `FixedPointSimMath`,
  `FixedPointDispatch`, and `FixedPointDeterminism` suites — the step's
  Verify command is `ctest -R math_fixed` (verified under ASan+UBSan).
- `pools` is the M0-CORE-05 CTest entry: a filtered view of the same
  executable covering the `ArenaPoolBasics`, `ArenaPoolBudget`,
  `PoolBasics`, `PoolBudget`, `PoolStale`, `PoolDestruction`,
  `PoolStats`, and `PoolMove` suites — the step's Verify command is
  `ctest -R pools` (budget exhaustion, reset semantics, and
  generation-checked stale handles; the stale-handle assert runs in a
  forked child on the POSIX jobs).
- `config_json` is the M0-CORE-07 CTest entry: a filtered view of the
  same `laige-core_tests` executable covering the `ConfigJsonValid`,
  `ConfigJsonInvalid`, `ConfigJsonRoundTrip`, `ConfigJsonValue`, and
  `ConfigJsonOptions` suites — the step's Verify command is
  `ctest -R config_json`.
- `fuzz_json_parse` is the M0-CORE-07 bounded-fuzz CTest entry
  (`laige-fuzz json_parse --runs=1000`, registered in `tools/fuzz`): it
  runs in every build tree — in the ASan tree it is instrumented and is
  the step's sanitizer gate (NFR-8.7; PRD §14: fuzz "every commit
  (bounded), nightly (long)").
- `api-fixture-scan`, `api-check-fresh`, `api-check-stale`,
  `api-unsupported-construct`, `api-root-error`, and `api-real-tree`
  are the M0-TOOL-01 CTest entries (`tests/api`): the public API
  manifest scanner (`tools/api/laige-api-scanner`) runs against a
  synthetic fixture tree (the exact manifest bytes are asserted) and
  against the real repository tree (`--check laige-api.json`), so a
  public-header change that misses the manifest fails in every P0 job
  — and in the dedicated `api-manifest` CI job, which regenerates the
  manifest and fails on any drift (PRD §9.4, NFR-13.1). The manifest
  contract (symbol kinds, doc association, exit codes, unsupported
  constructs) is the header comment of `tools/api/laige-api.cpp`.
- `detcheck-synthetic`, `detcheck-synthetic-perturbed`,
  `detcheck-bin-identical`, `detcheck-bin-identical-args`,
  `detcheck-bin-diverged`, `detcheck-bin-malformed`,
  `detcheck-scenario-failure`, `detcheck-stream-mismatch`, and
  `detcheck-unknown-scenario` are the M0-TOOL-02 CTest entries
  (`tests/detcheck`): the determinism checker
  (`tools/detcheck/laige-detcheck`) runs the synthetic two-run scenario —
  the built-in `synthetic` self-check, the built-in perturbation fixture,
  and the cross-binary mode against fixture scenario binaries (one source,
  five compiled variants) — asserting both the exit code and the required
  output fragments per test (`ctest -R detcheck`). The scenario contract
  (`<tick> <hash>` lines, 16 lowercase hex hash digits) and the tool's
  report/exit-code grammar are in
  [docs/api/detcheck.md](../api/detcheck.md); the CI `detcheck` job runs
  the self-check on every PR and merge, with the real-scenario comparison
  (two build configurations) skipped until M1-SAMPLE-01 (M1-DET-04
  activates it).
- `test_infra` is the M0-TEST-01 CTest entry (`tests/testing`): the
  `SeededRandom` suite of the `test_infra_tests` executable pins
  known-answer hashes for the seed-handling convention (the default seed
  `0x1F055EED`, the `LAIGE_TEST_SEED` override, the loud-failure path of
  an invalid seed, and substream isolation) and prints the
  machine-greppable `test-seed-check` lines that let two CI runs of the
  same commit be compared byte-for-byte (docs/testing.md §4).
- Every configure verifies the vendored dependency lock
  (`cmake/laige-deps-lock.cmake` against `deps.lock`); a tampered or
  unlisted file under `deps/` fails the configure loudly. GoogleTest is the
  only vendored dependency and is linked into tests only (M0-DEP-01,
  [ADR 0004](../decisions/0004-google-test-vendoring.md)).
