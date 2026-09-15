# Concepts

Architecture, coordinates, lifecycle, and threading concepts
(AGENTS §13). The topics below land with the milestone that defines
them; until then the interim homes apply:

| Topic | Document | Status |
|---|---|---|
| Determinism scope (ARCH-010, S-7, G-R8) | [determinism.md](determinism.md) | **Written** (M1-DET-01): the same-build guarantee, the two-layer G-R8 enforcement (the compile-time trait + the CI source scan), the exception policy, the PRNG substreams, the config surface |
| World axes, handedness, units, depth convention, render ordering, conversion rules (ARCH-008) | `coordinates.md` | Not yet written — interim home: the `Vec2`/`Vec3` comments in `src/laige-core/include/laige/sim_math.h` ((x, y) is the ground plane, z is depth/height) and the SimMath API contract in [api/sim_math.md](../api/sim_math.md) |
| Engine / scene / entity lifecycle; the fixed-timestep rule (ARCH-002) | `lifecycle.md` | Not yet written (the M1 loop steps define it) |
| Threading and ownership model (CONC-001…CONC-007) | `threading.md` | Not yet written — interim home: the per-API contracts in [api/](../api/) — each document states its threading, lifetime, and phase rules |
| Module architecture (PRD §10.1 stack) | `architecture.md` | Not yet written — interim home: the PRD §10.1 module map, enforced by the include-graph lint (`tools/laige-include-lint`, M0-CI-03) |

Normative decisions about these topics live as ADRs in
[../decisions/](../decisions/README.md); this section explains the
chosen designs as they exist.
