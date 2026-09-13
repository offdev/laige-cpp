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
in M1+); the first recorded baseline is
[m0-synthetic.md](m0-synthetic.md) (M0-EXIT-01, 2026-09-13) — the
synthetic harness workload, which proves the measurement pipeline end to
end but does not measure any real budget.
