# AGENTS.md — 2.5D Game Engine Engineering Contract

## 1. Scope and intent

This file governs the entire repository. More-specific `AGENTS.md` files may add
constraints for a subtree, but they MUST NOT weaken this contract without an
approved rule exception as defined in section 4.

This engine exists to make 2.5D games easy to build, from small single-player
games to large, long-running MMOs. It MUST remain fast, stable, observable,
portable, and difficult to misuse accidentally.

Normative words are used deliberately:

- **MUST / MUST NOT**: required for every change.
- **SHOULD / SHOULD NOT**: expected unless measured evidence justifies an
  exception.
- **MAY**: optional.

If a user request conflicts with this file, identify the conflict before
implementing it. Do not silently relax a rule.

## 2. Priority order

Use this order when requirements compete:

1. Correctness, memory safety, data integrity, and security.
2. Measured runtime performance, scalability, and predictable latency.
3. Stability, deterministic behavior where promised, and recoverability.
4. API safety: make the efficient path the obvious path and expensive behavior
   explicit.
5. Maintainability, readability, testability, and diagnostics.
6. Minimal dependency and build complexity.
7. Developer convenience and feature breadth.

Performance is the dominant choice between otherwise correct designs. Never
invoke undefined behavior, weaken validation at trust boundaries, risk corrupt
state, or remove necessary error handling merely to make a benchmark faster.

## 3. Non-negotiable principles

- **CORE-001 — Measure first.** Performance claims MUST include a reproducible
  benchmark, profile, trace, or before/after metric on a representative workload.
- **CORE-002 — Budget hot paths.** Frame, simulation-tick, render-submission,
  networking, physics, animation, visibility, and entity-update paths MUST have
  an explicit time and allocation budget.
- **CORE-003 — Safe fast path.** Public APIs MUST encourage batching, bounded
  work, explicit lifetimes, and cache-friendly access. A convenient API MUST NOT
  hide unbounded work, blocking I/O, synchronization, or repeated allocation.
- **CORE-004 — Smallest complete change.** Prefer the smallest readable design
  that satisfies current measured requirements. Do not add speculative systems,
  abstraction layers, or configuration points.
- **CORE-005 — No unexplained constants.** Domain values MUST use named constants,
  typed configuration, units, or documented formulas. Obvious structural values
  such as `0`, `1`, array extents, and bit widths do not need ceremonial names.
- **CORE-006 — Documentation is part of the implementation.** Code, tests,
  documentation, examples, diagnostics, and benchmarks affected by a change MUST
  be updated together.
- **CORE-007 — Test all behavior.** Every engine subsystem and every bug fix MUST
  have automated coverage at the lowest useful level. Architectural and
  performance changes MUST also have integration and performance validation.
- **CORE-008 — No silent failure.** Errors MUST be returned, handled, asserted as
  impossible, or logged with actionable context. Never ignore a failure by
  default.
- **CORE-009 — Explicit ownership.** Memory, resources, threads, jobs, callbacks,
  and cross-system references MUST have a clear owner and documented lifetime.
- **CORE-010 — Keep builds clean.** New code MUST compile without new warnings.
  Do not suppress a warning globally to hide a local problem.

## 4. Rule exceptions

If satisfying one rule requires breaking another, document the exception at the
exact implementation site. Place this block immediately above the affected code;
do not hide it only in a commit message, issue, or distant document.

```cpp
// ENGINE-RULE-EXCEPTION: PERF-004
// Reason: <why the rule cannot be satisfied here>
// Tradeoff: <cost introduced and rule or goal protected>
// Evidence: <benchmark/test/ADR/link or measured result>
// Scope: <why the exception is narrowly contained>
// Review: <condition or date that should trigger reconsideration>
```

Requirements:

- **EXC-001:** Exceptions MUST name a rule ID from this file.
- **EXC-002:** `Reason`, `Tradeoff`, `Evidence`, `Scope`, and `Review` MUST be
  concrete. “For performance,” “temporary,” and similar claims are insufficient.
- **EXC-003:** The smallest practical region MUST carry the exception. Repeat or
  reference it where another implementation site independently violates the rule.
- **EXC-004:** A public API, architectural, security, or persistent file-format
  exception MUST also have an Architecture Decision Record in `docs/decisions/`.
- **EXC-005:** Tests MUST preserve the behavior or performance property that
  justified the exception.
- **EXC-006:** Search for `ENGINE-RULE-EXCEPTION` during review. New or changed
  exceptions require explicit human attention.

## 5. Required agent workflow

Before editing:

1. Read this file and any more-specific `AGENTS.md` in the target subtree.
2. Inspect the relevant implementation, tests, documentation, build configuration,
   and recent architectural decisions. Do not guess repository conventions.
3. State the intended behavior, invariants, failure modes, affected hot paths,
   and validation plan.
4. Establish a baseline before attempting an optimization or architectural change.

While editing:

1. Keep the patch focused; do not refactor unrelated code.
2. Preserve public compatibility unless the task explicitly authorizes a breaking
   change. Document migrations for approved breaking changes.
3. Add or update tests with the implementation, not afterward.
4. Update public documentation and performance notes in the same change.
5. Add diagnostics for new runtime systems and important failure states.

Before finishing:

1. Run formatting, static analysis, the relevant test suites, and sanitizer builds
   available for the changed code.
2. Run affected benchmarks for performance or architectural changes and compare
   against the recorded baseline.
3. Check that disabled diagnostics add negligible work to hot paths.
4. Report commands run, results, untested paths, compatibility impact, and any
   `ENGINE-RULE-EXCEPTION` added or retained.
5. Do not claim success when required validation was skipped. Explain exactly what
   remains and why.

## 6. Architecture boundaries

- **ARCH-001:** Keep simulation, rendering, platform, asset, audio, networking,
  persistence, tooling, and game-specific policy behind explicit boundaries.
- **ARCH-002:** The simulation MUST NOT depend on render frame rate. Use a fixed or
  otherwise explicitly controlled simulation timestep with documented overload
  behavior.
- **ARCH-003:** Headless builds MUST be possible without initializing graphics,
  audio, input, or windowing. Server code MUST NOT transitively depend on client UI.
- **ARCH-004:** Core abstractions MUST NOT assume a single player, one process, one
  world, or that every entity is locally visible or simulated.
- **ARCH-005:** Game-specific rules MUST live outside engine primitives. Extension
  points MUST have ownership, threading, lifetime, and cost contracts.
- **ARCH-006:** Cross-subsystem communication SHOULD use explicit data transfer,
  commands, events, or stable handles. Avoid hidden global state and unrestricted
  service locators.
- **ARCH-007:** Persistent, networked, and replay data MUST be versioned. Readers
  MUST reject or migrate unsupported data explicitly.
- **ARCH-008:** Define and document the engine's world axes, handedness, units,
  depth convention, render ordering, and conversion rules in
  `docs/concepts/coordinates.md`.
- **ARCH-009:** Separate authoritative simulation state from presentation state.
  Interpolation, particles, camera effects, and other presentation-only behavior
  MUST NOT mutate authoritative results.
- **ARCH-010:** Any guarantee of determinism MUST state its scope: same build,
  platform, architecture, compiler, or cross-platform. Add replay/hash tests at
  the promised scope.

## 7. Performance and scalability

### 7.1 General hot-path rules

- **PERF-001:** Mark important hot paths in nearby documentation and benchmarks.
  Optimize from profiles rather than intuition.
- **PERF-002:** Hot paths MUST avoid unbounded loops, blocking calls, filesystem
  access, synchronous resource loads, and implicit GPU/CPU synchronization.
- **PERF-003:** Hot paths MUST NOT allocate by default. Reuse storage, reserve
  capacity, batch operations, or use a measured arena/pool with a clear reset
  lifetime.
- **PERF-004:** Prefer contiguous, compact data and predictable access over
  pointer-heavy object graphs. Validate data-oriented layouts with real workloads.
- **PERF-005:** Avoid accidental copies. Use values for small types and views such
  as `std::span` and `std::string_view` for non-owning access when lifetimes are
  unambiguous.
- **PERF-006:** Do not use exceptions, RTTI, virtual dispatch, `std::function`,
  shared ownership, hash maps, or locks in a hot loop without measured evidence
  and an exact-site exception. They remain valid outside hot paths when appropriate.
- **PERF-007:** Complexity MUST be documented for public operations whose cost can
  scale with entities, components, players, assets, chunks, or connections.
- **PERF-008:** Work that can spike MUST support budgeting, chunking, streaming,
  scheduling, or backpressure. Never defer an arbitrary backlog into one frame.
- **PERF-009:** Optimize tail latency as well as averages. Record useful percentiles
  and worst cases for frames, ticks, jobs, networking, and streaming.
- **PERF-010:** Performance tests MUST use representative small, medium, and stress
  workloads. A microbenchmark alone cannot validate an architectural change.

### 7.2 2.5D rendering and world organization

- **RENDER-001:** Batch and instance compatible draw work. State changes, draw
  submissions, overdraw, texture switches, and upload volume MUST be observable.
- **RENDER-002:** Use atlases, streaming, culling, spatial partitioning, level of
  detail, and occlusion techniques where measurement shows they help. Do not make
  every entity pay for features it does not use.
- **RENDER-003:** Visibility and render ordering MUST be deterministic within the
  documented contract. Tie-breaking MUST be explicit and stable.
- **RENDER-004:** Asset creation, decoding, compilation, and GPU upload MUST NOT
  occur unexpectedly during gameplay hot paths. Streaming work MUST be budgeted.
- **RENDER-005:** CPU and GPU timing MUST be measured independently. Avoid readbacks
  and synchronization that serialize them.
- **RENDER-006:** Simulation coordinates and render coordinates MUST have explicit
  conversion boundaries. Camera projection MUST NOT leak into game logic.

### 7.3 Large-world and MMO readiness

- **SCALE-001:** World processing MUST support spatial partitioning, sleeping or
  inactive regions, and bounded per-tick work.
- **SCALE-002:** Networking MUST support interest management and delta/batched
  replication; never assume broadcasting full world state is acceptable.
- **SCALE-003:** Servers MUST apply backpressure and explicit quotas for queues,
  connections, messages, persistence, and jobs. Growth MUST be bounded or monitored.
- **SCALE-004:** Network input is untrusted. Validate sizes, ranges, rates, identity,
  sequencing, and authorization before mutating authoritative state.
- **SCALE-005:** Serialization formats MUST specify byte order, bounds, version,
  compatibility, and malformed-input behavior. Parsers require fuzz tests.
- **SCALE-006:** Long-running server tests MUST check leaks, fragmentation, queue
  growth, handle exhaustion, drift, and recovery from dependency failures.
- **SCALE-007:** Do not sacrifice the small-game experience for distributed scale.
  Single-player MUST work without mandatory external services or a network server.

## 8. C++ implementation rules

- **CPP-001:** Use the C++ standard configured by the repository. Raising it is an
  architectural change and requires compatibility evidence and an ADR.
- **CPP-002:** Use RAII for ownership and cleanup. Raw pointers and references MAY
  express non-ownership only when the lifetime is locally clear.
- **CPP-003:** Prefer value semantics, `enum class`, strong domain types, and scoped
  resources. Use fixed-width integers where storage or protocol width matters.
- **CPP-004:** Never depend on signed overflow, invalid shifts, dangling views,
  strict-aliasing violations, uninitialized memory, or unspecified evaluation order.
- **CPP-005:** Avoid owning raw `new`/`delete`, C allocation, and manual cleanup in
  application code. Encapsulate platform or library APIs that require them.
- **CPP-006:** Use `std::unique_ptr` for unique dynamic ownership. Use
  `std::shared_ptr` only for genuinely shared lifetime, never as a substitute for
  deciding ownership.
- **CPP-007:** Use stable IDs or generation-checked handles for long-lived references
  into moving or pooled storage. Detect stale handles in debug builds.
- **CPP-008:** Public types MUST communicate units, ownership, nullability, lifetime,
  mutability, and error behavior. Do not encode booleans or modes as unexplained
  positional parameters.
- **CPP-009:** Macros SHOULD be restricted to compile-time platform boundaries,
  assertions, logging, tracing, and generated code. Prefer typed language features.
- **CPP-010:** Keep headers self-contained and minimize transitive includes. Prefer
  forward declarations only when they do not make ownership or completeness fragile.
- **CPP-011:** Do not add catch-all exception handling. If exceptions are enabled,
  define module boundaries, guarantees, and translation to engine errors.
- **CPP-012:** Assertions protect programmer invariants; recoverable runtime failures
  use explicit error paths. Assertions MUST NOT be required to prevent unsafe behavior
  in release builds.
- **CPP-013:** Formatting and naming MUST follow the repository configuration. Do
  not introduce a second style.
- **CPP-014:** Complex algorithms require a short explanation of invariants and a
  reference to the paper, standard, issue, or derivation that supports them.
- **CPP-015:** Keep functions focused and files cohesive. There is no arbitrary line
  limit, but split code when one unit owns unrelated responsibilities or cannot be
  understood without tracking excessive mutable state.
- **CPP-016:** Names MUST express domain intent. Avoid private abbreviations and
  vague containers such as `data`, `manager`, `util`, or `misc` when a precise name
  exists.
- **CPP-017:** Comments explain intent, invariants, non-obvious constraints, and
  tradeoffs; they SHOULD NOT narrate syntax. Delete stale code instead of commenting
  it out.
- **CPP-018:** Template metaprogramming, code generation, and clever low-level tricks
  require a demonstrated benefit, a readable interface, compile-time impact checks,
  and focused tests.

## 9. API design: flexible without easy performance failure

- **API-001:** Provide a safe, efficient default and an explicit advanced path.
  Customization MUST NOT require bypassing core invariants.
- **API-002:** Prefer bulk queries and batch mutation over per-object chatty calls.
  Per-item convenience wrappers MUST make their cost clear and MUST NOT be promoted
  for hot-loop use.
- **API-003:** Potentially expensive operations MUST be named and documented as such.
  Never hide scene traversal, synchronization, allocation, I/O, or decompression in
  a getter or trivial-looking constructor.
- **API-004:** Mutations SHOULD occur in explicit phases or command buffers where
  immediate mutation would invalidate iteration, defeat batching, or require locks.
- **API-005:** Extension callbacks MUST specify allowed operations, thread, phase,
  frequency, lifetime, reentrancy, and whether allocation or blocking is permitted.
- **API-006:** Configurable limits require validated defaults and documented memory,
  latency, and throughput effects. Avoid magic boolean combinations; use option
  structures or builders when configuration is non-trivial.
- **API-007:** Avoid making implementation details ABI promises. Public ABI and plugin
  boundaries require an explicit versioning and compatibility policy.
- **API-008:** Invalid states SHOULD be unrepresentable. Where that is impractical,
  validate at the boundary and return an actionable error.

Each public API document MUST include, where relevant:

- ownership and lifetime;
- thread-safety and permitted execution phase;
- time complexity and allocation behavior;
- batching and caching guidance;
- blocking, I/O, or GPU synchronization behavior;
- determinism and network-authority implications;
- invalidation rules and failure behavior;
- a performant example and a misuse warning.

## 10. Concurrency and scheduling

- **CONC-001:** Every mutable datum MUST have one clear owner or synchronization
  protocol. Document thread affinity and mutation phases.
- **CONC-002:** Prefer partitioned ownership, immutable snapshots, staged commands,
  and job dependencies over broad shared locking.
- **CONC-003:** Locks MUST NOT be held across callbacks, blocking I/O, waits, or
  unknown external code. Define lock ordering when multiple locks can be acquired.
- **CONC-004:** Do not add lock-free code without a demonstrated contention problem,
  a written memory-order argument, stress tests, and ThreadSanitizer coverage.
- **CONC-005:** Jobs MUST have bounded inputs, explicit dependencies, cancellation or
  shutdown behavior, and safe captured lifetimes. No detached threads.
- **CONC-006:** Engine shutdown MUST be ordered, testable, and idempotent. All owned
  work and resources MUST be joined, drained, canceled, or released.
- **CONC-007:** Randomized scheduling, long stress runs, and race detection MUST cover
  new concurrency primitives and cross-thread systems.

## 11. Dependencies

- **DEP-001:** Prefer the standard library and existing approved dependencies when
  they meet requirements without material performance or stability cost.
- **DEP-002:** Do not reinvent mature, security-sensitive, or format-heavy work such
  as cryptography, compression, image codecs, font shaping, window/input integration,
  or platform APIs merely to remove a dependency.
- **DEP-003:** Before adding a dependency, document:
  - the exact capability needed and alternatives considered;
  - measured performance and memory impact where relevant;
  - transitive dependencies and build impact;
  - supported platforms, maintenance health, license, and security history;
  - upgrade/removal strategy and whether it crosses a public API boundary.
- **DEP-004:** Wrap third-party APIs at a narrow engine boundary when that reduces
  coupling. Do not create a vague wrapper that merely duplicates the dependency.
- **DEP-005:** Pin or otherwise make versions reproducible. Dependency changes MUST
  pass tests, benchmarks, license checks, and supported-platform builds.

## 12. Testing and validation

Select the smallest tests that fully protect the behavior, plus broader validation
when architectural risk requires it.

- **TEST-001:** Unit tests cover algorithms, state transitions, invariants, limits,
  and error cases.
- **TEST-002:** Integration tests cover subsystem boundaries, lifetimes, shutdown,
  resource failure, save/load, and client/server interaction.
- **TEST-003:** Regression tests reproduce every fixed bug and fail before the fix.
- **TEST-004:** Determinism tests compare state hashes or replay outcomes wherever
  deterministic behavior is promised.
- **TEST-005:** Parsers, serializers, network protocols, asset loaders, and other
  untrusted-input surfaces require malformed-input tests and fuzz targets.
- **TEST-006:** Concurrency work requires stress tests and race/deadlock tooling.
- **TEST-007:** Long-running systems require soak tests with leak and resource-growth
  checks.
- **TEST-008:** Rendering changes require image/golden tests where stable, plus CPU/GPU
  performance metrics when they affect the render path. Make tolerances explicit.
- **TEST-009:** Architectural changes require representative end-to-end tests and a
  documented before/after benchmark. Preserve the benchmark for regressions.
- **TEST-010:** Performance improvements MUST demonstrate improvement without
  degrading correctness, tail latency, memory, loading, or another supported
  workload beyond an explicitly accepted threshold.

Benchmark reports MUST record hardware, OS, compiler and version, build type,
relevant flags, dataset/workload, warm-up, sample count, summary statistics, and
before/after results. Prefer repeatable automated benchmarks over ad hoc timings.
Never change a benchmark solely to make a regression disappear.

CI SHOULD include supported compiler/platform builds, formatting, static analysis,
unit and integration tests, sanitizer variants, fuzz smoke tests, and a stable
performance-regression lane. Keep slower soak and full benchmark suites schedulable.

## 13. Documentation

The repository MUST contain a top-level `docs/` directory. If it is missing, create
it before completing the next engine change. Keep this minimum structure:

```text
docs/
  README.md                 # Documentation index and navigation
  getting-started/          # Build, first project, examples
  concepts/                 # Architecture, coordinates, lifecycle, threading
  api/                      # Public API contracts and performance notes
  guides/                   # Task-oriented usage and optimization guides
  debugging/                # Debug mode, logging, profiling, troubleshooting
  benchmarks/               # Methods, baselines, results, regression policy
  decisions/                # Architecture Decision Records (ADRs)
  compatibility/            # Platforms, compilers, formats, migration guides
```

- **DOC-001:** `docs/README.md` MUST link to all documentation sections and identify
  incomplete or experimental areas honestly.
- **DOC-002:** Every public API and extension point MUST be documented before it is
  considered complete.
- **DOC-003:** Documentation MUST match shipped names, defaults, behavior, limits,
  errors, threading, and examples. Stale documentation is a defect.
- **DOC-004:** Hot-path APIs MUST include a clearly labeled **Performance** section
  describing complexity, allocations, batching, caching, budgets, and common traps.
- **DOC-005:** Significant architectural choices and rule exceptions MUST use ADRs
  containing context, decision, alternatives, evidence, consequences, and review
  conditions.
- **DOC-006:** Examples MUST be compiled or tested in CI where practical. Do not copy
  unverified snippets into public documentation.
- **DOC-007:** A behavior-changing patch is incomplete until its documentation and
  migration guidance are updated in the same patch.

## 14. Standardized logging

All engine logging MUST go through one logging facade with structured fields and
lazy message/field evaluation. The backend may remain replaceable.

Required event fields:

- timestamp using one documented clock and format;
- severity;
- stable subsystem and event name;
- concise human-readable message;
- thread or job identity when relevant;
- frame, simulation tick, world, entity, asset, connection, or request identifiers
  when they help correlate the event;
- error code/category and causal context for failures.

Severity contract:

- **Trace:** very high-volume diagnostic detail; disabled by default.
- **Debug:** developer-facing state useful during investigation.
- **Info:** low-volume lifecycle and significant state transitions.
- **Warn:** degraded or unexpected behavior from which the engine recovered.
- **Error:** an operation or subsystem failed and needs attention.
- **Fatal:** continued execution is unsafe or impossible; initiate controlled
  termination after preserving useful diagnostics.

Rules:

- **LOG-001:** Event names and field keys MUST be stable and machine searchable.
- **LOG-002:** An error log MUST state what failed, why if known, relevant identifiers,
  and the consequence or recovery action. Avoid duplicate logging at every layer.
- **LOG-003:** Hot-path logging MUST be compiled out, disabled through a cheap branch,
  sampled, aggregated, or rate-limited. Disabled logs MUST NOT format strings or
  allocate.
- **LOG-004:** Repeated failures MUST be rate-limited with a suppressed-count summary.
- **LOG-005:** Never log credentials, secrets, session tokens, personal data, or raw
  untrusted payloads. Sanitize control characters and bound user-controlled text.
- **LOG-006:** Assertions, metrics, and logs serve different purposes; do not replace
  one with another.
- **LOG-007:** Logging initialization failure MUST have a minimal safe fallback.
  Logging MUST flush appropriately during crashes and controlled shutdown without
  making normal hot paths synchronous.

Example shape; exact syntax depends on the engine facade:

```cpp
ENGINE_LOG_WARN("network", "packet_dropped",
                "Dropped packet outside receive window",
                field("connection_id", connection_id),
                field("sequence", sequence),
                field("window_end", window_end));
```

## 15. Runtime debug mode and observability

Provide an in-engine debug system inspired by
[Factorio's debug mode](https://wiki.factorio.com/Debug_mode): searchable,
individually toggleable overlays with small “Always” and opt-in “Debug” profiles.
It MUST be useful in a running game, editor, client, and headless server rather
than requiring a debugger for routine inspection.

### 15.1 Debug-system contract

- **DBG-001:** Register debug features in one searchable registry with stable names,
  categories, descriptions, and runtime toggles.
- **DBG-002:** Support at least two configurable profiles:
  - **Always:** a tiny persistent set of essential counters.
  - **Debug:** opt-in overlays and detailed instrumentation.
- **DBG-003:** Settings SHOULD be filterable, persist per developer, and resettable.
  Adding a diagnostic SHOULD NOT require building a one-off UI.
- **DBG-004:** Disabled instrumentation MUST have negligible CPU cost, no allocation,
  and no GPU work. Measure expensive probes and label their observer effect.
- **DBG-005:** Debug UI and overlays MUST consume published snapshots rather than
  taking broad locks or mutating authoritative simulation state.
- **DBG-006:** Diagnostics MUST be available in development builds. Shipping access
  MUST be separately configurable, permission-gated where it exposes server or
  anti-cheat-sensitive state, and safe against untrusted remote use.
- **DBG-007:** Important counters MUST be capturable to a timestamped report or trace
  so intermittent issues can be compared and shared.
- **DBG-008:** New runtime subsystems MUST register essential metrics, failure state,
  and at least one useful inspection view before they are complete.

### 15.2 Minimum built-in diagnostics

Provide only cheap essentials by default; make detailed views opt-in:

- FPS, simulation updates per second, frame/tick time, average, min, max, useful
  percentiles, hitch count, and configurable rolling window;
- CPU time by major subsystem and job, worker utilization, queue depth, steals,
  waits, lock contention, and overruns;
- GPU pass timings, draw/dispatch count, primitives, state changes, uploads, VRAM,
  render-target use, overdraw indicators, and CPU/GPU stalls where supported;
- entity/component counts, active/sleeping counts, creations/destructions, spatial
  partitions, visible/culled counts, and per-region update cost;
- world grid/chunk boundaries, coordinates, origins, depth/order, bounds, collision
  shapes, velocities, targets, paths, path requests, and navigation cache state;
- allocation counts/bytes, high-water marks, pools/arenas, fragmentation indicators,
  resource lifetime, and leak summaries;
- asset streaming queues, cache hits/misses, load/decode/upload timing, residency,
  and fallback/error state;
- network RTT, jitter, loss, bandwidth, message counts/sizes, queue depth, replication
  cost, interest-set size, prediction error, reconciliation, and server tick health;
- audio voices, virtualized voices, streaming buffers, and underruns;
- selected-object inspection with stable IDs and subsystem-specific details;
- logging filters, recent warnings/errors, profiler markers, trace capture, and an
  optional bounded frame/tick history.

Metrics MUST use documented units and distinguish counters, gauges, histograms,
and timings. Avoid vanity metrics that cannot guide a decision.

## 16. Change acceptance checklist

A change is complete only when all applicable statements are true:

- [ ] Behavior and scope are clear; unrelated code was not changed.
- [ ] Ownership, lifetimes, threading, failure modes, and shutdown are explicit.
- [ ] The efficient path is the natural API path; costly behavior is visible.
- [ ] Relevant unit, integration, regression, stress, fuzz, or soak tests pass.
- [ ] Performance/architecture work has reproducible before/after evidence.
- [ ] Supported builds are warning-clean; relevant analysis/sanitizers pass.
- [ ] Public docs, examples, performance notes, and migration guidance are current.
- [ ] Logging is structured, actionable, safe, and cheap when disabled.
- [ ] Runtime diagnostics cover the new system without perturbing hot paths.
- [ ] Dependencies were avoided or justified with lifecycle and license review.
- [ ] Magic values were removed or explained with names, types, units, or formulas.
- [ ] Every rule tradeoff is documented exactly where it occurs and, when required,
      in an ADR.
- [ ] Validation commands, results, limitations, and remaining risks are reported.

When in doubt, preserve correctness, collect evidence, choose the simplest bounded
design, and leave the code easier to measure than you found it.
