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

No baselines are recorded yet (every `budgets.json` entry has
`measured: 0`); the first lands with M0-EXIT-01.
