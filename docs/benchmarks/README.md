# Benchmarks

Benchmark methodology, baselines, results, and the regression policy
(AGENTS §13). Performance claims are reproducible measurements
(CORE-001); this section is where they are recorded.

- [methodology.md](methodology.md) — the **normative methodology**
  (M0-DOC-01): the AGENTS §12 report fields, how `budgets.json` entries
  map onto them, the baseline-file convention, the PRD §8.1 regression
  policy, and workload discipline.
- [baselines/](baselines/README.md) — recorded baseline reports
  (`<milestone>-<workload>.md`). **Currently empty**: every
  `budgets.json` entry has `measured: 0`. The first baseline,
  `baselines/m0-synthetic.md`, lands with M0-EXIT-01, and
  `baselines/m1-profiler-cost.md` with M1-PROF-01.
- Per-milestone performance results (M1 10k-entity tick, M2 50k-sprite
  scene, M6/M7 zone server, …) land here as their milestones close —
  see the [roadmap progress board](../../roadmap/README.md).

The measurement tool is `laige-bench` (M0-CORE-08): API contract in
[api/budget_harness.md](../api/budget_harness.md), canonical command in
[building.md](../getting-started/building.md).
