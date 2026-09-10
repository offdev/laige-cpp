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
| Fuzz (bounded) | `./build/bin/laige-fuzz <target> --runs=1000` |
| Benchmarks | `./build/bin/laige-bench --suite=<name>` |
| Determinism check | `./build/bin/laige-detcheck --scenario=<name>` |
| API manifest | `cmake --build build --target laige-api` |
| Include-graph lint + dependency count | `python3 tools/laige-include-lint` |

Notes:

- `Debug` is the canonical `CMAKE_BUILD_TYPE`; `Release` is supported.
- The four rows above the lint row name tools that land in later M0 steps —
  `laige-fuzz` (M0-TEST-01), `laige-bench` (M0-CORE-08), `laige-detcheck`
  (M0-TOOL-02), target `laige-api` (M0-TOOL-01). Their command forms are
  fixed here now so later steps cannot drift.
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

## Current status (M0)

- `laige-core` builds as a static library (default) or a shared library
  (`-DLAIGE_BUILD_SHARED=ON`). It carries only the minimal version/build
  identifier code (`include/laige/core/version.h`, `version.cpp`); the
  functional engine code (`laige::Result<T,E>` / `laige::Status`) lands in
  M0-CORE-01.
- `tests/laige-core/laige-core_tests` is a CTest link smoke test (a
  GoogleTest suite since M0-DEP-01) that runs in every build tree above: it
  verifies the static/shared link and checks the NFR-8.10 policy flags with
  `static_assert` (a policy violation fails the build).
- Every configure verifies the vendored dependency lock
  (`cmake/laige-deps-lock.cmake` against `deps.lock`); a tampered or
  unlisted file under `deps/` fails the configure loudly. GoogleTest is the
  only vendored dependency and is linked into tests only (M0-DEP-01,
  [ADR 0004](../docs/decisions/0004-google-test-vendoring.md)).
