# Debugging

Debug mode, logging, profiling, and troubleshooting (AGENTS §13).
The in-engine debug system (AGENTS §15: searchable overlay registry,
"Always"/"Debug" profiles) **does not exist yet** — its foundations
are landing with the profiling work: the M1-PROF-01 always-on
counters are live (below), the M2 render timing comes with the
editor. Until the overlay system lands, this section indexes what is
usable today:

| Need | Document |
|---|---|
| Profile a headless run: tick/frame time percentiles, entity counts, sim allocs, per-system timings; per-run JSON report (`laige-run --prof-out`) | [api/profiler.md](../api/profiler.md) (M1-PROF-01; FR-11.1 always-on counters) |
| Read and emit structured logs; severity contract, rate limiting, file sinks, crash handling | [api/logging.md](../api/logging.md) (M0-CORE-02; AGENTS §14) |
| Measure a suspected performance problem (rolling histograms, percentiles, budget checks) | [api/budget_harness.md](../api/budget_harness.md) + [benchmarks/methodology.md](../benchmarks/methodology.md) |
| Prove a determinism divergence (two builds, per-tick hash streams) | [api/detcheck.md](../api/detcheck.md) (M0-TOOL-02) |
| Find undefined behavior or data races locally | Sanitizer builds in [building.md](../getting-started/building.md) (`build-asan` / `build-tsan` trees; NFR-8.2) |
| Reproduce flaky behavior in tests (fixed seeds, KATs) | [testing.md](../testing.md) (seed convention, §4) |

When the debug mode lands, this section grows with the overlay-registry
documentation, the Always/Debug profiles, counter capture (DBG-007), and
troubleshooting guides.
