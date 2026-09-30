# Concepts

Architecture, coordinates, lifecycle, and threading concepts
(AGENTS §13). The topics below land with the milestone that defines
them; until then the interim homes apply:

| Topic | Document | Status |
|---|---|---|
| Determinism scope (ARCH-010, S-7, G-R8) | [determinism.md](determinism.md) | **Written** (M1-DET-01): the same-build guarantee, the two-layer G-R8 enforcement (the compile-time trait + the CI source scan), the exception policy, the PRNG substreams, the config surface |
| World axes, handedness, units, depth convention, render ordering, conversion rules (ARCH-008) | [coordinates.md](coordinates.md) | **Written** (M2-ISO-01): the world axes/handedness/units (right-handed, (x, y) ground plane, +z up, world units), the NDC conventions, the isometric projection family (ADR 0005), the depth key formula and 32-bit layout (M2-ISO-01), the (key, entity id) total render order (RENDER-003), the supported-iso-shear contract, and the sim↔render conversion rules (RENDER-006) |
| Engine / scene / entity lifecycle; the fixed-timestep rule (ARCH-002) | `lifecycle.md` | Not yet written (the M1 loop steps define it) |
| Threading and ownership model (CONC-001…CONC-007) | `threading.md` | Not yet written — interim home: the per-API contracts in [api/](../api/) — each document states its threading, lifetime, and phase rules |
| Module architecture (PRD §10.1 stack) | `architecture.md` | Not yet written — interim home: the PRD §10.1 module map, enforced by the include-graph lint (`tools/laige-include-lint`, M0-CI-03) |

Normative decisions about these topics live as ADRs in
[../decisions/](../decisions/README.md); this section explains the
chosen designs as they exist.
