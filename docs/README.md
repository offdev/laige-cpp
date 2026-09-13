# Laige documentation

Documentation index and navigation (DOC-001). The engine is at **M0**
(foundations): `laige-core` is the only populated module, and the
sections below mark what exists and what is still to land.

## Build & tools

- [Building Laige](getting-started/building.md) — the source of truth for
  the canonical build commands, build trees, options, compiler policy
  (NFR-8.10), sanitizer builds (NFR-8.2), and the current M0 status.
  Tool commands: `laige-fuzz`, `laige-bench`, `laige-detcheck`, the
  `laige-api` manifest target, and the include-graph lint.

## API contracts (per public header)

- [Result / Status / error codes](api/errors.md) — `laige::Result<T,E>`,
  `laige::Status`, the stable `ErrorCode` registry (M0-CORE-01).
- [Structured logging](api/logging.md) — the `laige::log` facade, sinks,
  rate limiting, crash handling (M0-CORE-02; AGENTS §14).
- [SimMath deterministic math](api/sim_math.md) — the op surface plus the
  `fp32_pinned` and `fpx16_16` backends, NaN/Inf policy, pinned-math
  flags (M0-CORE-03/04; ADR 0002).
- [Memory pools](api/pools.md) — `ArenaPool<T>` and `Pool<T>` with
  generation-checked handles and `PoolStats` accounting (M0-CORE-05).
- [Bounded JSON](api/json.md) — `laige::JsonValue`, `parseJson`,
  `serializeJson`, `JsonOptions` bounds (M0-CORE-07; ADR 0003).
- [Budget harness](api/budget_harness.md) — `Histogram`, `TimeIt`,
  `budgetCheck`, the AGENTS §12 report format, and the `budgets.json`
  schema (M0-CORE-08).
- [PRNG](api/prng.md) — `laige::Prng`: the splitmix64/LCG64 hybrid,
  period, and determinism contract (M0-CORE-06).
- [Determinism checker](api/detcheck.md) — the `laige-detcheck` tool and
  the scenario hash-line contract (`<tick> <hash>` lines, two build
  configurations) (M0-TOOL-02).

## Testing

- [Testing conventions](testing.md) — test layout (module dirs mirror
  `src/`, `<module>_tests` executables), the `regress_<short-id>`
  regression-test convention, `laige-fuzz` target registration and CI
  lane semantics, and the seed-handling convention for randomized tests
  (M0-TEST-01).

## Architecture decisions (ADRs)

- [ADR index](decisions/README.md) — 0001 (name and license), 0002
  (deterministic math), 0003 (config JSON), 0004 (GoogleTest
  vendoring).

## Not yet written (honest status)

- `concepts/` — architecture, coordinates (ARCH-008; lands as
  `docs/concepts/coordinates.md` with M0-DOC-02 — until then the
  coordinate system is documented in the `Vec2`/`Vec3` comments of
  `src/laige-core/include/laige/sim_math.h`), lifecycle, threading.
- `guides/` — task-oriented usage (first game, profiling, determinism).
- `debugging/` — debug mode (AGENTS §15 lands in M3), logging in
  production, troubleshooting.
- `benchmarks/` — method, baselines, and the regression policy
  (the harness exists — `laige-bench`, M0-CORE-08 — but the recorded
  baselines land with M1 workloads).
- `compatibility/` — platform/compilers/formats matrix (the P0 matrix
  is in [building.md](getting-started/building.md) for now).
- Per-module API docs for the M1+ modules (`laige-sim`, `laige-render`,
  `laige-assets`, `laige-net`, `laige-server`, `laige-script`,
  `laige-editor`) — they land with their modules.

## Related

- [Roadmap index](../roadmap/README.md) — the M0/M1/... step plan;
  [M0 foundations](../roadmap/M0-foundations.md) is the current
  milestone.
- `AGENTS.md` — the engineering contract this documentation implements.
