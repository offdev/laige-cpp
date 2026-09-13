# Concepts

Architecture, coordinates, lifecycle, and threading concepts
(AGENTS §13). **Nothing in this section is written yet** — M0 is
foundations only. The topics below are planned, and each document lands
with the milestone that defines it; until then the interim homes are:

| Topic | Planned document | Interim home (today) |
|---|---|---|
| World axes, handedness, units, depth convention, render ordering, conversion rules (ARCH-008) | `coordinates.md` | The `Vec2`/`Vec3` comments in `src/laige-core/include/laige/sim_math.h` ((x, y) is the ground plane, z is depth/height) and the SimMath API contract in [api/sim_math.md](../api/sim_math.md) |
| Determinism scope (ARCH-010) | `determinism.md` | [ADR 0002](../decisions/0002-deterministic-math.md) (which paths use which backend), [api/sim_math.md](../api/sim_math.md) (NaN/Inf policy, pinned flags), [api/detcheck.md](../api/detcheck.md) (replay comparison contract) |
| Engine / scene / entity lifecycle; the fixed-timestep rule (ARCH-002) | `lifecycle.md` | — (the M1 loop steps define it) |
| Threading and ownership model (CONC-001…CONC-007) | `threading.md` | The per-API contracts in [api/](../api/) — each document states its threading, lifetime, and phase rules |
| Module architecture (PRD §10.1 stack) | `architecture.md` | The PRD §10.1 module map, enforced by the include-graph lint (`tools/laige-include-lint`, M0-CI-03) |

Normative decisions about these topics live as ADRs in
[../decisions/](../decisions/README.md); this section will explain the
chosen designs once they exist.
