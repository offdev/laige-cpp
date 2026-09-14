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

Every `budgets.json` entry still has `measured: 0` (their subsystems land
in M1+). Recorded so far:

- [m0-synthetic.md](m0-synthetic.md) (M0-EXIT-01, 2026-09-13) — the
  synthetic harness workload; proves the measurement pipeline end to
  end but does not measure any real budget.
- [m1-ecs-stress.md](m1-ecs-stress.md) (M1-ECS-07, 2026-09-14) — the
  M1 ECS stress workload (10k entities × 6 component types × 10k frames
  of add/remove churn): no leaks (ASan), pool high-water stable,
  iteration within the documented cost, accounted ECS storage bytes.
  Not a `budgets.json` workload — no `measured` field updated.
