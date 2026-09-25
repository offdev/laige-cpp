# Baselines

Recorded baseline reports — the durable before/after history of the
`budgets.json` budgets (convention:
[../methodology.md](../methodology.md) §4). One file per recorded
measurement:

```text
<milestone>-<workload>.md        e.g. m0-synthetic.md (M0-EXIT-01)
```

Each baseline file contains the full AGENTS §12 metadata (hardware, OS,
compiler + version, build type, flags, workload, warm-up, sample count,
summary statistics), the stable 4-line `budgetCheck` report verbatim,
and the exact command + commit that produced it. Baseline files are
**immutable**: superseding a baseline adds a new file and updates
`measured` in `budgets.json` — it never edits an existing baseline.

Recorded so far:

- [m0-synthetic.md](m0-synthetic.md) (M0-EXIT-01, 2026-09-13) — the
  synthetic harness workload; proves the measurement pipeline end to
  end but does not measure any real budget.
- [m1-ecs-stress.md](m1-ecs-stress.md) (M1-ECS-07, 2026-09-14) — the
  M1 ECS stress workload (10k entities × 6 component types × 10k frames
  of add/remove churn): no leaks (ASan), pool high-water stable,
  iteration within the documented cost, accounted ECS storage bytes.
  Not a `budgets.json` workload — no `measured` field updated.
- [m1-profiler-cost.md](m1-profiler-cost.md) (M1-PROF-01,
  2026-09-21) — the enabled-profiler cost gate (ON vs OFF on 10k-entity
  ticks, bounded at 1%): measured +0.28% on the canonical Debug tree.
  Not a `budgets.json` workload — the FR-11.1 counters are diagnostics,
  not a budgeted subsystem.
- [m1-sim-tick.md](m1-sim-tick.md) (M1-BENCH-01, 2026-09-25) — the
  PRD §8.1 simulation-tick workload (10k entities, 2k dynamic bodies,
  60 Hz, movement + per-tick state hash, both SimMath backends):
  0.59 ms mean / 0.62 ms p99 on the canonical Debug tree, both
  backends PASS. **The first baseline to update `budgets.json`
  `measured`** (`sim_tick_avg`, `sim_tick_p99` — the worse of the two
  backends). Every other `budgets.json` entry still has
  `measured: 0` (its subsystem lands in a later milestone).
