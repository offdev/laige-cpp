# laige-core

Engine foundation module (PRD §10.1): determinism, math, allocators, pools,
ECS, systems, config, logging, `Result`. `laige-core` depends on nothing
internal (PRD §10.1 dependency rule) — the CMake target links no other
engine target and exposes no transitive includes or dependencies (CPP-010).

Status: M0 — Foundations.

- **M0-BUILD-01 (done):** CMake target `laige-core`, static by default,
  shared with `-DLAIGE_BUILD_SHARED=ON` (NFR-8.9); engine policy
  `-Wall -Werror -fno-exceptions -fno-rtti` applied via
  `laige_apply_engine_policy()` (NFR-8.10, flags kept PRIVATE so they do not
  leak to consumers). Contains only the minimal version/build identifier
  (`include/laige/core/version.h`, `version.cpp`); link smoke test in
  `tests/laige-core/`.
- **M0-CORE-01 (next):** first functional code (`laige::Result<T,E>` /
  `laige::Status` + error registry); further public headers land with each
  M0-CORE-xx step.

Canonical build commands:
[docs/getting-started/building.md](../../docs/getting-started/building.md).
