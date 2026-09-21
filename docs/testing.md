# Testing conventions

Source of truth for how Laige tests are laid out, named, seeded, and
fuzzed (finalized by M0-TEST-01). Normative for every later step; the
normative rule IDs are AGENTS.md §12 (TEST-001…TEST-010), PRD §14 (quality
and verification cadence), and NFR-8.7 (parsers are fuzzed).

## 1. Test layout (GTest integration)

- **One directory per engine module under `tests/`, mirroring `src/`:**
  `tests/laige-core` ↔ `src/laige-core`, and later `tests/laige-sim` ↔
  `src/laige-sim`, etc.
- **One test executable per module, named `<module>_tests`:**
  `tests/laige-core/CMakeLists.txt` builds `laige-core_tests`. All of a
  module's unit suites are separate source files in that one executable
  (one `TEST` suite per concern), linked against `gtest_main` (which
  provides `main()`) and the module library under test.
- **Module suites are exposed as their own CTest entries** with an
  unquoted `--gtest_filter` selecting exactly the step's suites; the
  unfiltered entry runs the whole module. Each roadmap step names its
  Verify command as `ctest -R <entry>` (pattern: M0-CORE-01…08). The
  gtest filter must be one *unquoted* argument — CTest passes quoted
  arguments through with the literal quote characters, which silently
  under-runs the suite (see `tests/laige-core/CMakeLists.txt`).
- **GoogleTest is a dev-only dependency** (PRD §11): vendored in
  `deps/googletest`, integrity-locked in `deps.lock` (M0-DEP-01), and
  linked into test executables only — **never into engine libraries**
  ([ADR 0004](decisions/0004-google-test-vendoring.md)).
- **Non-module test directories** (no `src/` counterpart):
  - `tests/tools`, `tests/api`, `tests/detcheck` — checks that exercise
    the tools in `tools/` (include-graph lint, API manifest scanner,
    determinism checker); CTest entries named after the tool.
  - `tests/support/` — shared test-only headers (header-only, no build
    targets). Currently `laige_test_seed.h` (this step).
  - `tests/testing/` — the test-infrastructure checks; executable
    `test_infra_tests`, CTest entry `test_infra`.
- **Every test TU is compiled with the NFR-8.10 policy**
  (`laige_apply_engine_policy`: `-Wall -Werror -fno-exceptions
  -fno-rtti` / the MSVC equivalent), and the module test executables
  self-check that policy with `static_assert`s (a policy violation
  fails the build loudly).
- **Test executables are single-owner and run single-threaded per CTest
  entry** (CONC-001); concurrency under test gets its own suites
  (pattern: `LogConcurrency`) and the TSan tree (NFR-8.2).

## 2. Regression tests (AGENTS TEST-003)

- Every bug fix ships a regression test **named `regress_<short-id>`**
  inside the existing module suite (the suite name is unchanged; the
  short-id names the defect).
- The test **must fail before the fix and pass after it** (TEST-003).
  Demonstrate that in the fix step: write the test first, record the red
  run, apply the fix, record the green run — both belong in the step's
  Verify note.
- The fix is incomplete without its `regress_` test in the same change
  (CORE-007, TEST-003).
- First test under the convention (renamed by this step):
  `ConfigJsonValid.regress_json_object_member_ws` in
  `tests/laige-core/config_json_tests.cpp` — the M0-CORE-08 finding that
  the JSON parser rejected object members separated by `", "` (the
  hand-formatted repo-root `budgets.json` demonstrated it).

## 3. Fuzz runner (`laige-fuzz`; PRD §14, NFR-8.7)

`tools/fuzz/laige-fuzz.cpp` (M0-CORE-07) is the deterministic bounded
runner: every input is generated from `laige::Prng` (M0-CORE-06), so a
given `(target, runs, seed)` reproduces the exact same input sequence on
every platform (the Prng's cross-platform bit-exactness, ARCH-010). A
target may return any `Status`; the run fails only on process death
(crash or sanitizer report), which ctest turns into a test failure
(CORE-008: no silent failure).

- **Registering a fuzz target:** add a
  `void target(const std::uint8_t*, std::size_t)` entry point and one
  `kTargets` row in `tools/fuzz/laige-fuzz.cpp`, then register a CTest
  entry `fuzz_<name>` next to it (`COMMAND laige-fuzz <name>
  --runs=1000`, `TIMEOUT`, and `TSAN_OPTIONS=halt_on_error=1` in the TSan
  tree — the shape of `fuzz_json_parse` in `tools/fuzz/CMakeLists.txt`).
- **Bounded runs in CI, every commit:** the `fuzz_<name>` CTest entries
  run inside every P0 job's `ctest` (both `ci.yml` and `ci-pull.yml`),
  1000 runs per target — the PRD §14 "every commit (bounded)" lane. In
  the ASan tree (`build-asan`) the runs are instrumented, so a crash or
  UB fails the job loudly (NFR-8.7).
- **Targets so far:** `json_parse` (the JSON parser, M0-CORE-07) and
  `replay_parse` (the replay log parser, M1-DET-02 — the
  malformed-input surface of the version 1 replay format; the corpus
  includes a valid v1 log as a mutate/truncate base). Both run the
  bounded lane above in every P0 job.
- **Nightly long runs (PRD §14 "nightly (long)"):** the canonical form is
  `./build/bin/laige-fuzz <target> --runs=1000000 [--seed=HEX]`. The
  scheduled nightly lane is documented here but not yet wired: it lands
  with the first M1 fuzz target whose long run protects more than the
  parser (asset import / network packets, PRD §14 fuzz row). Until then
  the bounded lane above is the complete fuzz cadence in M1.
- **Seed handling:** fixed default seed `0x1F055EED` ("one-fuzz-seed"),
  overridable with `--seed=` (0x-prefixed hex or decimal). This is the
  same default seed the test suites use (below) — one documented
  default seed repo-wide.

## 4. Seed handling for randomized tests

- **No nondeterministic sources in tests.** Every randomized test draws
  from `laige::Prng` (M0-CORE-06) — never from wall-clock time,
  `std::random_device`, or any other source that varies between runs.
  CI must be deterministic: the same commit produces the same test
  values on every CI run, on every P0 platform.
- **Seed source:** `tests/support/laige_test_seed.h` (this step).
  `laige::testing::TestSeed()` returns the fixed default
  `kDefaultTestSeed = 0x1F055EED` — identical to laige-fuzz's default —
  unless `LAIGE_TEST_SEED` is set (0x-prefixed hex or decimal, read at
  call time). A set-but-unparseable value records a test failure with
  the offending value and falls back to the default (CORE-008: an
  explicit misconfiguration is loud, not silent).
- **Stream isolation:** `laige::testing::TestPrng(id)` derives the
  substream `Prng::deriveSubstream(TestSeed(), id)`. Each randomized test
  file declares its own stable, named substream-id constant so two tests
  never share a stream position (the Prng contract: a copy shares the
  position; interleaved draws are a caller bug, not a detectable error).
- **CI determinism, checkable in the logs:** the `SeededRandom` suite
  (`tests/testing/`, CTest entry `test_infra`) pins known-answer FNV-1a
  hashes of 65536 draws under the default seed and under a documented
  override seed, and prints one machine-greppable line per KAT:

  ```
  test-seed-check default seed=0x000000001f055eed stream=1 draws=65536 fnv1a=0x7ea4049545656830
  test-seed-check override seed=0x2468acce01234567 stream=2 draws=65536 fnv1a=0x535d2ca741b61cbf
  ```

  The line lands in the ctest output, every CI job's log, and the
  archived `Testing/Temporary/LastTest.log` (linux-asan / linux-tsan
  artifacts) even when a KAT mismatches. Two CI runs of the same commit
  must show byte-identical `test-seed-check` lines — that is the step's
  cross-run identity check (M0-TEST-01 Verify).
- **Investigating a flaky randomized test:** set
  `LAIGE_TEST_SEED=<hex>` for the ctest run to reproduce the exact
  stream that produced the failure (the committed KATs pin the
  default-seed values, so the override never hides a KAT regression).

## 5. Determinism test entries (M1-DET-01)

The M1-DET-01 step adds three kinds of determinism checks, all part of
the standard ctest suite in every P0 job (and both sanitizer trees):

- **`ctest -R determinism_mode`** — the runtime determinism suites
  (`DeterminismMode.*`, `DeterminismEngine.*`, `DeterminismConfigParse.*`
  in `tests/laige-sim/determinism_tests.cpp`, part of the
  `laige-sim_tests` executable): a trivial moving-entity sim produces
  bit-identical per-tick FNV-1a state hashes in two consecutive runs
  (same build, same seed) over 256 ticks; a different seed diverges; the
  per-system PRNG substreams match `Prng::deriveSubstream` exactly and
  are independent; `deterministic == false` yields `SystemContext.rng ==
  nullptr`; the engine selects the configured SimMath backend (built-in
  component + presentation snapshot); and the `seed` / `determinism`
  config keys (defaults, valid values, the rejection table — the
  version 1 schema's determinism block, M1-CFG-01). The
  machine-greppable `determinism-tick-stream` line lands in the ctest
  output. (The full config schema — version gate, budgets, camera,
  asset roots, overrides, hot reload — is `ctest -R config`,
  `tests/laige-sim/game_config_tests.cpp`.)
- **`ctest -R trait_compile`** — the G-R8 trait compile-checks
  (`tests/laige-sim/compile_fail/`, generated `cmake -P` check scripts):
  one positive fixture (a marked determinism-safe component compiles)
  and three negative fixtures (a `double` member, an unmarked user
  struct, and a `double` in the mark's member list each fail to compile
  with the actionable G-R8 message). Each check asserts the exit code
  **and** a required stderr fragment, so an incidental compiler error
  cannot masquerade as the trait firing.
- **`ctest -R determinism-lint`** — the sim-source determinism scan
  (`tools/laige-determinism-lint`, `tests/tools`): fixture trees
  (clean tree with one marked exception → exit 0; one violation per rule
  D1a/D1b/D1c/D2/D3 → exit 1) and the real repository tree (→ exit 0).
  The same lint runs as the `determinism-lint` CI job in both
  `ci-pull.yml` and `ci.yml`.

These entries, together with the `prng` suite (M0-CORE-06) and the
`detcheck` matrix (M1-DET-04), implement TEST-004 (determinism tests
compare state hashes or replay outcomes wherever determinism is
promised) at the scope ARCH-010 requires.

## 6. Running this step's checks

| Purpose | Command |
|---|---|
| Seeded-random KAT suite | `ctest --test-dir build -R test_infra --output-on-failure` |
| Bounded fuzz, `json_parse` | `./build/bin/laige-fuzz json_parse --runs=1000` |
| Bounded fuzz, `replay_parse` | `./build/bin/laige-fuzz replay_parse --runs=1000` |
| Fuzz with an explicit seed | `./build/bin/laige-fuzz json_parse --runs=1000 --seed=0x12345678` |
| Nightly long run (documented form) | `./build/bin/laige-fuzz json_parse --runs=1000000` |
| Reproduce a randomized test's stream | `LAIGE_TEST_SEED=0x… ctest --test-dir build --output-on-failure` |

The full canonical command table (build trees, sanitizers, tools) stays
in [getting-started/building.md](getting-started/building.md).
