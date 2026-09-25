# Benchmarks

Benchmark methodology, baselines, results, and the regression policy
(AGENTS §13). Performance claims are reproducible measurements
(CORE-001); this section is where they are recorded.

- [methodology.md](methodology.md) — the **normative methodology**
  (M0-DOC-01): the AGENTS §12 report fields, how `budgets.json` entries
  map onto them, the baseline-file convention, the PRD §8.1 regression
  policy, and workload discipline.
- [baselines/](baselines/README.md) — recorded baseline reports
  (`<milestone>-<workload>.md`). Four are recorded:
  `baselines/m0-synthetic.md` (M0-EXIT-01),
  `baselines/m1-ecs-stress.md` (M1-ECS-07),
  `baselines/m1-profiler-cost.md` (M1-PROF-01), and
  `baselines/m1-sim-tick.md` (M1-BENCH-01) — the first that updates a
  `budgets.json` `measured` field (`sim_tick_avg`, `sim_tick_p99`);
  every other entry still has `measured: 0` (its subsystem lands in a
  later milestone).
- [determinism-matrix.md](determinism-matrix.md) — the **determinism
  report** for the M1-SAMPLE-01 hello scenario (M1-DET-04): the
  committed per-tick hash baselines (both SimMath backends), the CI
  matrix (every P0 OS job's ctest baseline check + the merge detcheck
  job's cross-compiler / sanitizer / Release pairs), the ARCH-010
  scope statement, and the `float_pinned_32` per-platform support list.
- Per-milestone performance results (M1 10k-entity tick, M2 50k-sprite
  scene, M6/M7 zone server, …) land here as their milestones close —
  see the [roadmap progress board](../../roadmap/README.md).

The measurement tool is `laige-bench` (M0-CORE-08): API contract in
[api/budget_harness.md](../api/budget_harness.md), canonical command in
[building.md](../getting-started/building.md).
