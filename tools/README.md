# tools/

Engine tools and CI scripts, each landing with its roadmap step:

- `laige-run` — the headless run binary (M1-HEAD-01, in `tools/run`):
  `laige-run --headless CONFIG.json [--ticks N] [--replay LOG]` —
  config → world → systems → loop, the bounded run (default 0 = the
  server form), and the ordered idempotent shutdown. Exit codes:
  `0` ok · `1` engine run failure · `2` usage/IO/config error; one
  machine-greppable summary line on stdout (`laige-run headless
  ticks=… status=…`). `--replay` is the M1-DET-02 stub (accepted,
  warned, ignored). Full contract in
  [docs/api/engine.md](../docs/api/engine.md); the `laige_run_smoke`
  CTest entry (1000 ticks @ 60 Hz, every P0 OS job) is its CI form.
- `laige-fuzz` — deterministic bounded fuzz runner (minimal form from
  M0-CORE-07, in `tools/fuzz`: the `json_parse` target, `--runs`/`--seed`,
  built with `LAIGE_BUILD_TESTS=ON`, registered as the `fuzz_json_parse`
  CTest entry — bounded fuzz in every commit, PRD §14; M0-TEST-01
  extends it: CI lane semantics, nightly long runs, seed documentation)
- `laige-include-lint` — include-graph lint + vendored-dependency-count
  metric over `src/**` (M0-CI-03). Pure Python 3 stdlib; run it as
  `python3 tools/laige-include-lint [--root REPO_ROOT]`. Enforces the PRD
  §10.1 include rules (laige-core is the leaf; arrows only downward;
  vendored deps only from their `deps.lock` owner) and fails above the PRD
  §11 dependency budget of 10. Runs in CI on every PR and merge
  (job `include-lint`), and as CTest checks in `tests/tools`.
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
