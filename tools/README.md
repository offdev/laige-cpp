# tools/

Engine tools and CI scripts, each landing with its roadmap step:

- `laige-run` — the headless run binary (M1-HEAD-01, in `tools/run`):
  `laige-run --headless CONFIG.json [--ticks N] [--replay LOG]
  [--prof-out REPORT]` — config → world → systems → loop, the
  bounded run (default 0 = the server form), and the ordered
  idempotent shutdown. Exit codes: `0` ok · `1` engine run failure ·
  `2` usage/IO/config/profile-report-write error; one machine-
  greppable summary line on stdout (`laige-run headless ticks=…
  status=…`) followed by the profiler's one-line summary
  (`laige-run profile: ticks=… tick_ms: … sim_allocs=…`, always
  printed — M1-PROF-01). `--replay` records the run (M1-DET-02:
  opt-in, debug builds only, atomic publish, 128 MiB default cap);
  `--prof-out` writes the run's profile report (M1-PROF-01, FR-11.1
  file export: the version-1 JSON schema — counters, tick/frame time
  windows, world fields, render timing (M2-PROF-01), per-system
  timings — at run end, EVERY
  build; a write failure does not fail the run — it exits `2` with
  the run status `ok`). Full contract in
  [docs/api/engine.md](../docs/api/engine.md),
  [docs/api/replay.md](../docs/api/replay.md), and
  [docs/api/profiler.md](../docs/api/profiler.md); the
  `laige_run_smoke` CTest entry (1000 ticks @ 60 Hz, every P0 OS
  job) is its CI form.
- `laige-fuzz` — deterministic bounded fuzz runner (minimal form from
  M0-CORE-07, in `tools/fuzz`: the `json_parse` and `replay_parse`
  targets (M1-DET-02 added the replay log parser), `--runs`/`--seed`,
  built with `LAIGE_BUILD_TESTS=ON`, registered as the `fuzz_json_parse`
  and `fuzz_replay_parse` CTest entries — bounded fuzz in every
  commit, PRD §14; M0-TEST-01 extends it: CI lane semantics, nightly
  long runs, seed documentation)
- `laige-include-lint` — include-graph lint + vendored-dependency-count
  metric over `src/**` (M0-CI-03). Pure Python 3 stdlib; run it as
  `python3 tools/laige-include-lint [--root REPO_ROOT]`. Enforces the PRD
  §10.1 include rules (laige-core is the leaf; arrows only downward;
  vendored deps only from their `deps.lock` owner) and fails above the PRD
  §11 dependency budget of 10. Runs in CI on every PR and merge
  (job `include-lint`), and as CTest checks in `tests/tools`.
- `laige-determinism-lint` — sim-source determinism scan (M1-DET-01;
  the second half of the G-R8 guarantee — the first is the compile-time
  trait in `World::registerSystem`). Pure Python 3 stdlib; run it as
  `python3 tools/laige-determinism-lint [--root REPO_ROOT]`. Scans
  `src/laige-sim/**` for raw `float`/`double` (type tokens, float and
  double literals) and `unordered_*` containers, with same-line
  `// LAIGE-DETERM-EXCEPTION: G-R8 <reason>` markers as the documented
  false-positive policy (every suppressed line is counted and printed).
  Exit codes: `0` pass · `1` violation · `2` structural. Runs in CI on
  every PR and merge (job `determinism-lint`), and as CTest checks in
  `tests/tools` (fixture trees + the real tree). Scope, rules, and the
  exception policy:
  [docs/concepts/determinism.md](../docs/concepts/determinism.md).
- `laige-surface-lint` — projection-surface lint (M2-AC-01 AC-4.2).
  Pure Python 3 stdlib; run it as `python3 tools/laige-surface-lint
  [--root REPO_ROOT]`. The AC-4.2 proof that no simulation, physics,
  pathing, or network code depends on the projection mode: the
  public surface of `laige-sim` and `laige-net` (their `include/`
  trees) MUST NOT mention `ProjectionMode` at all (RENDER-006).
  Exit codes: `0` OK (prints the scanned header count) · `1` a
  `ProjectionMode` mention in a public header (each hit printed as
  `rel/path:line: text`) · `2` structural (a module has source but
  no public-header root). Runs in CI on every PR and merge as the
  CTest entries `ac4_surface*` in `tests/tools` (fixture trees for
  each exit path + the real tree — the `laige-include-lint` CTest
  pattern).
- `laige-api` — public API manifest generator (M0-TOOL-01)
- `laige-detcheck` — determinism checker skeleton (M0-TOOL-02, in
  `tools/detcheck`): runs a named scenario in two build configurations
  and compares the per-tick state-hash streams (scenario contract:
  `<tick> <hash>` lines — 16 lowercase hex hash digits; full contract in
  [docs/api/detcheck.md](../docs/api/detcheck.md)). The built-in
  `synthetic` scenario is the M0 self-check, tested in `tests/detcheck`;
  the CI `detcheck` job runs it on every PR and merge and skips the
  real-scenario comparison (M1-SAMPLE-01) until M1-DET-04 activates it.
- `laige-bench` — budget/benchmark harness (M0-CORE-08)
- `laige-scene-iso` — the M2-SCENE-01 reference-scene tool (in
  `tools/scene-iso`): writes/reads the v1 LE reference-scene file and
  runs the CLI determinism gate (ctest entry `scene_iso_cli`)
- `laige-golden` — the golden image test tool (M2-GOLD-01, in
  `tools/golden`): renders the committed golden scene fixtures
  (the M2-SPRITE-02 sprite scene, the M2-UI-02 UI scene, and the
  M2-AC-01 `ac4_iso` / `ac4_side_view` acceptance scenes — one
  engine build, config-only difference) offscreen at the canonical
  1280x720 frame (fixed camera, no PRNG), captures the frame (one `GlContext::readPixels`), and
  compares it against the committed reference PNG
  (`tools/golden/references`) with the documented tolerance
  (max channel diff 2 + 0.001% pixel budget — passed as explicit
  flags by the ctest entry `golden`, TEST-008). `--update`
  re-renders and rewrites the references (atomic; the
  human-approved regeneration path, never run under ctest — the
  CI forms are the label-gated `golden-references` job of
  `.github/workflows/ci-pull.yml` (PR lane, label
  `ci:golden-update`) and the `workflow_dispatch`-only
  `.github/workflows/golden-update.yml` (post-merge master)).
  Exit codes: `0` ok · `1`
  compare failed · `2` usage/config/render error · `77` no usable
  OpenGL 3.3 environment (the ctest entry skips via
  SKIP_RETURN_CODE). Full contract:
  [docs/testing.md](../docs/testing.md) (section 6) and
  [tools/golden/references/README.md](golden/references/README.md).
