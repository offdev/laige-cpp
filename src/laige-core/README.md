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
  leak to consumers). Contains the minimal version/build identifier
  (`include/laige/core/version.h`, `version.cpp`); link smoke test in
  `tests/laige-core/`.
- **M0-CORE-01 (done):** first functional engine code —
  `laige::Result<T,E>` / `laige::Status` (no exceptions, FR-12.1) plus the
  central error-code registry (`include/laige/result.h`,
  `include/laige/errors.h`, `errors.cpp`); rendered error text follows the
  NFR-13.3 5-field grammar (`docs/api/errors.md`); unit suite:
  `ctest -R result_status`.
- **M0-CORE-02 (done):** the one structured logging facade
  (`include/laige/logging.h`, `logging.cpp`): severity Trace…Fatal,
  stable subsystem/event names, lazy field/message evaluation (disabled
  events cost one atomic load + branch, no allocation), per-subsystem
  level filtering, replaceable sinks (console, file), rate limiting with
  `rate_limited` suppressed-count summaries, and flush-on-shutdown/crash
  (AGENTS §14). `Result::takeValue()` (rvalue move-out of the success
  value) and `ErrorCode::IoError` (`5`) were added as small additive
  extensions of M0-CORE-01 to support the FileSink→facade hand-off. API
  contract: `docs/api/logging.md`; unit suite: `ctest -R logging`.
- Further public headers land with each M0-CORE-xx step.

Canonical build commands:
[docs/getting-started/building.md](../../docs/getting-started/building.md).
