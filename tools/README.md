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
  windows, world fields, per-system timings — at run end, EVERY
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
