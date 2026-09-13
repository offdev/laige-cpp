# laige-sim

Module per PRD §10.1: game loop, physics, input state, animation state,
pathing/steering, AI hooks.

Status: M1 in progress. M1-ECS-01 landed the ECS foundation — the
32-bit `laige::Entity` handle and the `laige::World` entity storage
(`include/laige/sim/entity.h`, `entity.cpp`; API contract in
[docs/api/entity.md](../docs/api/entity.md), tests under
[tests/laige-sim](../tests/laige-sim), CTest entry `entity`).
M1-ECS-02 landed the component type registry — `ComponentTypeId`, the
`LAIGE_COMPONENT` macro, and `World::registerComponent<T>()`
(`include/laige/sim/component.h`; API contract in
[docs/api/component_registry.md](../docs/api/component_registry.md),
tests under [tests/laige-sim](../tests/laige-sim), CTest entry
`component_registry`). The archetype storage, iteration, and the game
loop land in the remaining M1-ECS / M1-SYS / M1-LOOP steps; physics,
input, and animation in M3.
