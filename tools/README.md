# tools/

Engine tools and CI scripts, each landing with its roadmap step:

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
- `laige-detcheck` — determinism checker (M0-TOOL-02)
- `laige-bench` — budget/benchmark harness (M0-CORE-08)
