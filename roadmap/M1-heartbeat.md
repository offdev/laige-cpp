# M1 — Heartbeat

**PRD:** §15 M1 · **Duration:** 3–4 weeks
**Scope (PRD):** Game loop, ECS, systems, determinism, headless mode, profiler core,
`hello.laige` (headless).
**Exit criteria (PRD):** 10k entities @ 60 Hz ≤ 3 ms; replay bit-exact; zero-alloc
assertion passes.

Rules specific to this milestone: everything here is headless (no GL, no window —
ARCH-003). `laige-sim` is the first module beyond `laige-core`. Simulation is
single-threaded (PRD §10.2). Every step that touches sim hot paths must keep the
zero-allocation property (M1-ALLOC-01 enforces it once it exists; before that, ASan
+ pool accounting is the check).

---

## ECS

- [x] **M1-ECS-01 · Entity handle**
  - **Refs:** FR-1.2 (32-bit handle); AGENTS CPP-007
  - **Depends:** M0-CORE-05
  - **Scope:**
    - `laige::Entity`: 32-bit handle = entity id + generation; `laige::World` entity storage (create/destroy, pool-backed, no per-op heap).
    - Stale-handle detection in debug builds (use-after-free via stale handle asserts loudly); in release, stale-handle access returns `Status` (`InvalidArgument`) + warn-once — never silent (FR-12.3; M0-CORE-05 `Pool` precedent).
    - Entity capacity is a config value (scene budget, G-R3); overflow → `Status`.
    - Unit tests: reuse after destroy bumps generation; stale access detected; capacity limit honored.
  - **Verify:** `ctest -R entity` green under ASan; debug stale-handle test asserts.
  - **Size:** ~200 lines + tests

- [x] **M1-ECS-02 · Component registry + trait-based IDs**
  - **Refs:** FR-1.2 (compile-time trait types); PRD §9.1 S-8
  - **Depends:** M1-ECS-01
  - **Scope:**
    - Compile-time component type registration: a `LAIGE_COMPONENT(Type)` macro (initially data-only; replication/inspector traits are M4) producing a stable `ComponentTypeId` assigned in registration order (documented, deterministic).
    - `laige::World::register_component<T>()` with size/alignment recorded for SoA layout.
    - Custom (user) components register through the same path — a user-defined struct is legal (S-8 data-carrier case).
    - Unit tests: duplicate registration is an error; type ids stable across two runs with the same registration order.
  - **Verify:** `ctest -R component_registry` green.
  - **Size:** ~150 lines + tests

- [x] **M1-ECS-03 · Archetype SoA storage**
  - **Refs:** FR-1.2 (archetype/SoA, pool-backed add/remove); AGENTS PERF-003, PERF-004
  - **Depends:** M1-ECS-02
  - **Scope:**
    - Archetype = ordered set of component types; SoA columns (one `T[]` per component per archetype); entity→archetype map.
    - `add_component<T>(e, value)` / `remove_component<T>(e)`: pool-backed moves between archetypes, **no heap allocation per operation** (archetype growth is pre-reserved in bounded chunks; reserve policy documented).
    - Component access `world.get<T>(e)` is O(1) (archetype lookup + column index).
    - Unit tests: 10k entities × add/remove churn completes with zero pool-overflow and constant per-op cost (measured, not assumed — CORE-001); memory layout is contiguous per column (property test).
  - **Verify:** `ctest -R archetype` green; churn test reports allocs = 0 via pool accounting.
  - **Size:** ~400 lines + tests (the largest ECS step; split into storage-layout / move-if-exceeding)

- [x] **M1-ECS-04 · Query API + iteration legality**
  - **Refs:** FR-1.2/FR-1.3 (declared access, iteration legality); AGENTS CORE-008
  - **Depends:** M1-ECS-03
  - **Scope:**
    - `ctx.each<T1, T2, ...>(access_flags, fn)`: iterate entities having all listed components; access declared per component (`Read` / `Write`).
    - Iteration legality enforcement: mutating a storage currently being iterated (write during read-iteration, or add/remove during iteration of the affected archetype) → assert in debug, `Status`/skip-with-log in release (documented behavior).
    - No hidden allocations in the query path (iteration state is stack/pool scoped).
    - Unit tests: mixed-component queries return exact sets; illegal mutation caught in debug.
  - **Verify:** `ctest -R query` green; illegal-mutation test asserts in debug build.
  - **Size:** ~250 lines + tests

- [x] **M1-ECS-05 · Deterministic iteration order**
  - **Refs:** PRD §10.3 (archetype order, entity id order); AGENTS ARCH-010
  - **Depends:** M1-ECS-04
  - **Scope:**
    - Documented iteration contract: archetypes in registration order; entities within an archetype in id order (dense id order survives moves via a documented scheme).
    - No unordered containers in the iteration path (banned in sim hot paths); the one internal hash structure (entity→archetype) uses deterministic hash + never iterated directly.
    - Property test: two worlds whose operation sequences interleave create/destroy differently but converge on the identical final state — including the entity→id assignment — iterate identically; the test also exercises component moves (add/remove) to pin the dense-id-order scheme.
  - **Verify:** `ctest -R iter_order` green (property test with fixed PRNG seed).
  - **Size:** ~100 lines + tests

- [x] **M1-ECS-06 · ECS guardrails (G-R3, G-R4)**
  - **Refs:** PRD §9.3 G-R3, G-R4; FR-12.3
  - **Depends:** M1-ECS-01, M0-CORE-02
  - **Scope:**
    - Entity-count guardrail: warns (structured log + counter) at 25%/50%/100% of declared scene budget, with actionable message (NFR-13.3 grammar).
    - Component-churn counter per frame (adds+removes); threshold breach → warn with "move to spawn/despawn system" advice (debug includes the advice text).
    - Both exposed to the profiler (M1-PROF-01).
    - Unit tests: thresholds fire exactly at the documented percentages (no duplicates: warn once per level per frame).
  - **Verify:** `ctest -R ecs_guardrails` green; log output matches the error grammar.
  - **Size:** ~150 lines + tests

- [x] **M1-ECS-07 · ECS stress + memory accounting test**
  - **Refs:** AGENTS TEST-001/007; PRD §8.1 (base memory)
  - **Depends:** M1-ECS-06
  - **Scope:**
    - Stress test: 10k entities, 6 component types, 10k frames of add/remove churn, verify: no leaks (ASan), pool high-water stable, iteration stays ≤ documented cost.
    - Record result in `docs/benchmarks/baselines/m1-ecs-stress.md` (AGENTS §12 fields).
  - **Verify:** `ctest -R ecs_stress` green under ASan; baseline file written with metadata.
  - **Size:** ~150 lines + baseline doc

## System framework

- [x] **M1-SYS-01 · System registry**
  - **Refs:** FR-1.3 (plain registered functions, declared budgets + I/O)
  - **Depends:** M1-ECS-04
  - **Scope:**
    - `laige::SystemDef`: name, function pointer `void run(World&, SystemContext&)`, declared time budget (ms), declared component I/O list.
    - `LAIGE_SYSTEM(Name, budget_ms)` macro registering into the world; systems are plain functions (no inheritance required).
    - Duplicate system names are an error; budgets ≤ 0 rejected at registration (must be explicit).
    - Unit tests: registration, validation errors, duplicate detection.
  - **Verify:** `ctest -R system_registry` green.
  - **Size:** ~150 lines + tests

- [x] **M1-SYS-02 · System scheduler**
  - **Refs:** FR-1.3 (engine enforces iteration legality); PRD §10.2 (sim thread)
  - **Depends:** M1-SYS-01
  - **Scope:**
    - Scheduler runs systems in explicit registration order; supports declared `depends_on` ordering constraints (topological, documented).
    - Pre-run validation: two systems both writing the same component type in one tick → error at scheduling time (fail loudly, FR-12.3); read-after-write ordering warnings where declared.
    - Unit tests: ordering respected; conflicting I/O rejected; dependency cycle rejected with actionable error.
  - **Verify:** `ctest -R scheduler` green.
  - **Size:** ~200 lines + tests

- [x] **M1-SYS-03 · Per-system timing + budget enforcement (G-R5)**
  - **Refs:** PRD §9.3 G-R5; FR-11.1/11.2; FR-12.3
  - **Depends:** M1-SYS-02, M0-CORE-08
  - **Scope:**
    - Measure each system's run time per tick into a rolling histogram (fixed window, no alloc).
    - Over budget → structured warn (rolling p99 in message); > 3× budget → error event (both in NFR-13.3 grammar, actionable).
    - Data feeds the frame graph report (M1-PROF-02).
    - Unit tests: synthetic slow system triggers warn then error at the documented multipliers; histogram window resets correctly.
  - **Verify:** `ctest -R system_timing` green.
  - **Size:** ~200 lines + tests

## Game loop

- [x] **M1-LOOP-01 · Fixed-timestep core**
  - **Refs:** FR-1.1 (default 60 Hz, 20–120 Hz configurable); ARCH-002; PRD §10.2
  - **Depends:** M1-SYS-03, M0-CORE-08
  - **Scope:**
    - Accumulator loop: sim advances in integer ticks; render/presentation cadence decoupled.
    - Config: tick rate (20–120 Hz validated), max catch-up ticks per frame (documented default); overload behavior: exceed catch-up → drop ticks with a **structured log event** (count + dropped amount), never silently.
    - Headless clock source (monotonic clock) now; windowed clock arrives with M2 (M2-GL-02).
    - Unit tests: fixed 60 Hz over a synthetic 10 s clock → exact tick count; overload path drops exactly the documented amount and logs once per episode (rate-limited).
  - **Verify:** `ctest -R game_loop` green.
  - **Size:** ~200 lines + tests

- [x] **M1-LOOP-02 · Presentation snapshot + interpolation state**
  - **Refs:** FR-1.1 (render interpolation, 2D-aware); PRD §4 (depth is presentation-only)
  - **Depends:** M1-LOOP-01, M1-ECS-04
  - **Scope:**
    - Per tick, the world produces a presentation snapshot: for entities with a `Position2D` component (first built-in component defined here), store `prev` and `curr` + interpolation alpha = (render_time - last_tick)/tick_dt.
    - `PresentationSnapshot::sample_position(e)` returns the interpolated 2D position (projection-correct rendering is M2's job; here it is pure 2D math).
    - Snapshot is pool-backed, no per-frame heap; alpha range [0,1] clamped and documented (never extrapolates).
    - Unit tests: interpolation is linear between ticks; alpha clamping; entity added between ticks samples correctly (documented: new entities snap to curr).
  - **Verify:** `ctest -R presentation` green.
  - **Size:** ~200 lines + tests

- [x] **M1-HEAD-01 · Headless engine run**
  - **Refs:** FR-1.6, ARCH-003, AC-6.2
  - **Depends:** M1-LOOP-02, M1-CFG-01, M0-CORE-07
  - **Scope:**
    - `Engine` object: config (JSON) → world → systems → loop; `run_headless(frame_budget_ticks)` starts, ticks, shuts down cleanly (CONC-006 ordered shutdown: systems → world → pools → logging flush).
    - `laige-run` binary: `laige-run --headless config.json [--ticks N] [--replay log]` (replay flag lands with M1-DET-02; stub now).
    - CI runs `laige-run --headless` for 1000 ticks as a smoke test.
    - Unit/integration tests: shutdown idempotent (double shutdown safe); no GPU/window symbols referenced (include-graph lint already ensures).
  - **Verify:** `laige-run --headless <fixture config> --ticks 1000` exits 0 in CI on all P0 OSes; double-shutdown test green.
  - **Size:** ~250 lines + tests

## Determinism & replay

- [x] **M1-DET-01 · Deterministic mode + sim math rules**
  - **Refs:** FR-1.4, S-7, PRD §10.3; AGENTS ARCH-010
  - **Depends:** M0-DEC-02, M0-CORE-03, M0-CORE-04, M0-CORE-06, M1-SYS-01, M1-ECS-05
  - **Scope:**
    - `EngineConfig.determinism`: `{enabled: bool, default true; math: "fixed_point_16_16" | "float_pinned_32"}` (schema owned by M1-CFG-01; ADR 0002). When enabled, sim code must use engine math ops (SimMath) only — raw `float`/`double` outside SimMath is a compile-time error (G-R8, S-7), enforced by: (a) a compile-time trait constraint on components used in deterministic systems (member types must be SimMath-registered or integer; document the trait mechanism), and (b) a deterministic source scan over sim translation units in CI banning raw `float`/`double` and `unordered_*` (in the style of `tools/laige-include-lint`; false-positive policy documented).
    - PRNG substreams wired: each system gets a derived substream from (seed, system id) — seed is part of config and replay.
    - Docs: `docs/concepts/determinism.md` stating the promised scope per ADR 0002 (CORE-001/ARCH-010).
    - Tests: a trivial moving-entity sim produces identical per-tick hashes on two sequential runs (same build) — the cross-target check is M1-DET-04.
  - **Verify:** `ctest -R determinism_mode` green; determinism doc published; trait-check compiles-fail test (compile-check test) passes; sim-TU source scan green in CI.
  - **Size:** ~250 lines + tests

- [x] **M1-DET-02 · Replay recorder**
  - **Refs:** FR-1.4, FR-11.3; PRD Appendix A (replay = input log + seed)
  - **Depends:** M1-DET-01, M0-CORE-01
  - **Scope:**
    - Replay log format (versioned, ARCH-007): header (format version, seed, tick rate, component schema hash, math backend id, config hash — replay identity per ADR 0002) + per-tick input frames (input data shape lands with M3-INPUT-03; for now the frame is an opaque byte blob + length).
    - `ReplayRecorder`: every debug run can record (opt-in flag); writes atomically (temp+rename), bounded size, no unbounded growth (file size cap → error).
    - `laige-run --replay <path>` wired (fulfills the M1-HEAD-01 stub): opt-in recording of the current run in the format above; debug builds only; record failure → `Status`.
    - Round-trip test: record N ticks → parse back → identical bytes.
  - **Verify:** `ctest -R replay_record` green; malformed log file (truncated, bad version) → `Status` error, never crash.
  - **Size:** ~200 lines + tests

- [x] **M1-DET-03 · State hashing + replay runner**
  - **Refs:** FR-1.4 (replayable and diffable), FR-11.3; AGENTS TEST-004
  - **Depends:** M1-DET-02, M1-ECS-05
  - **Scope:**
    - `world.state_hash(tick)`: deterministic hash over all sim state (entity ids, archetype assignment, all component bytes in iteration order, PRNG state, tick counter); documented exact scope (what is included/excluded).
    - `laige-replay` binary: `laige-replay --log <log> --config <cfg>` replays the log headlessly, prints `<tick> <hash>` lines, and can `--expect <baseline file>` comparing per-tick hashes; exit code 1 + first-diff report on mismatch.
    - Integration test: 500-tick scenario recorded, replayed, hashes identical.
  - **Verify:** `ctest -R replay_replay` green; perturbed baseline makes `laige-replay --expect` fail at the correct tick with an actionable report.
  - **Size:** ~250 lines + tests

- [x] **M1-DET-04 · Bit-exactness CI (all P0 OS jobs, both SimMath backends)**
  - **Refs:** FR-1.4, NFR-8.3, FR-11.5; PRD §14 (determinism every merge)
  - **Depends:** M1-DET-03, M1-SAMPLE-01, M0-TOOL-02
  - **Scope:**
    - Activate `laige-detcheck` with a real scenario: commit a baseline per-tick hash stream of the M1-SAMPLE-01 hello scenario (reference build: canonical Debug g++); every P0 OS CI job (linux-gcc, linux-clang, windows-msvc, macos-arm64, macos-intel) runs the scenario headless and asserts per-tick identity against the baseline (`laige-replay --expect`); `laige-detcheck --run-a/--run-b` additionally pairs the two Linux compiler builds and the Debug+ASan vs Release configurations.
    - `fp32_pinned` backend: same matrix; per-platform support list generated from the detcheck results (desyncing pairs declared unsupported — ADR 0002).
    - CI job wired per PRD §14 cadence (every merge); result recorded in `docs/benchmarks/` as a determinism report (scope stated per ARCH-010).
  - **Verify:** determinism CI job green on a merge across all P0 OS jobs (both backends); an intentional perturbation (a changed SimMath constant in a scratch system) makes the job fail (then revert).
  - **Size:** CI wiring + ~100 lines

- [x] **M1-DET-05 · Replay diff tool**
  - **Refs:** FR-11.3 (diff two replays by frame/state)
  - **Depends:** M1-DET-03
  - **Scope:**
    - `laige-replay --diff <logA> <logB>`: replays both, aligns by tick, reports first divergent tick + the diffed state components (bounded report, not a full dump).
    - Used by the editor replay viewer later (M5-ED-15); for now CLI + structured output (machine-readable optional section).
    - Integration test: two replays differing at tick 37 report tick 37 and the diverging component.
  - **Verify:** `ctest -R replay_diff` green; diff output format tested.
  - **Size:** ~150 lines + tests

## Configuration

- [x] **M1-CFG-01 · Declarative game config**
  - **Refs:** FR-1.5; PRD §7.1
  - **Depends:** M0-CORE-07, M1-LOOP-01
  - **Scope:**
    - `config.json` schema (versioned, documented in `docs/api/config.md`): tick rate (20–120), budgets (per-system defaults, scene entity budget, draw/particle budgets as *declared* values even before their consumers exist), camera defaults (values stored; consumed in M2), asset roots, `determinism` (`{enabled: bool, math: "fixed_point_16_16" | "float_pinned_32"}` — ADR 0002; consumed by M1-DET-01).
    - Loading: missing file → error; unknown keys → warn (forward-compat); version mismatch → explicit reject (ARCH-007).
    - Runtime overrides: `EngineConfig` merge API (programmatic override of a subset); hot-reload of **non-simulation** keys in debug only (file watch; sim-affecting keys require restart — documented, FR-1.5).
    - Unit tests: full valid config; each invalid case (bad tick rate, unknown key, bad version, truncated file) → the documented error.
  - **Verify:** `ctest -R config` green; hot-reload test (debug build) reloads a changed camera-default value without restart; tick-rate change via hot-reload is refused with an actionable error.
  - **Size:** ~300 lines + tests

## Profiler & allocation guardrails

- [x] **M1-PROF-01 · Profiler core (cheap counters)**
  - **Refs:** FR-11.1, DBG-008; AGENTS §15.1
  - **Depends:** M1-SYS-03, M1-ECS-06
  - **Scope:**
    - Always-on counters (fixed storage, no allocation after init): per-system time histogram, entity counts (total/alive), sim allocation count (pool accounting sum, target 0), tick time p50/p95/p99/mean/min/max over a rolling window, frame time percentiles, draw calls (0 in headless, field present), texture binds (0), net bytes (0).
    - Output: `profiler.snapshot()` → structured text/JSON to file; `laige-run --prof-out path` writes per-run summary; CLI one-liner summary.
    - Disabled-cost check: profiler on vs off measured on the 10k-entity tick (must be ≤ 1% — measured, CORE-001, DBG-004).
  - **Verify:** `ctest -R profiler` green; disabled-cost measurement recorded in `docs/benchmarks/baselines/m1-profiler-cost.md`.
  - **Size:** ~300 lines + tests

- [ ] **M1-PROF-02 · Frame graph / budget report**
  - **Refs:** FR-11.2; PRD §9.1 S-6
  - **Depends:** M1-PROF-01, M0-CORE-08
  - **Scope:**
    - Per-frame budget report: for each declared budget (system time, total tick time, allocation count), measured vs declared, pass/flag; over-budget systems listed (feeds G-R5 events).
    - `laige-run --budget-report` prints the last N frames' report in the AGENTS §12 field format; over-budget → non-zero exit in CI when `--fail-on-budget` is set.
    - Unit test: synthetic over-budget system appears in the report with correct numbers.
  - **Verify:** `ctest -R budget_report` green; sample report file committed as fixture.
  - **Size:** ~200 lines + tests

- [ ] **M1-ALLOC-01 · Zero sim-loop allocation assertion (G-R1)**
  - **Refs:** PRD §9.3 G-R1, §8.1 (0 per frame in sim); PERF-003
  - **Depends:** M1-PROF-01, M1-ECS-03, M1-ECS-07, M1-LOOP-01
  - **Scope:**
    - Debug allocation counter hooking the engine allocators (core allocator + pools) around the sim tick; after each tick in debug: allocs > 0 → assert with the offending allocation's call site (actionable, FR-12.3).
    - Release behavior: pool overflow → logged degradation (already via pool accounting); no crash.
    - This assertion is the standing hot-path check for every later sim/render step (see README §6).
    - Test: the 10k-entity tick performs 0 allocations (the M1-ECS-07 workload through the loop).
  - **Verify:** `ctest -R zero_alloc` green; a deliberate `std::vector` in a scratch system fails the assertion (then revert).
  - **Size:** ~150 lines + tests

## Benchmark & sample

- [ ] **M1-BENCH-01 · 10k-entity tick benchmark**
  - **Refs:** PRD §8.1 (≤ 3.0 ms avg, ≤ 5 ms p99), §15 M1 exit
  - **Depends:** M1-ALLOC-01, M1-PROF-01
  - **Scope:**
    - `laige-bench --suite=sim-tick`: representative workload — 10k entities, 2k with a `Position2D`, a handful of systems (movement, hash), 60 Hz, N=3000 ticks warm-up excluded, sample count per AGENTS §12; run on both SimMath backends (`fpx16_16`, `fp32_pinned`) — both must meet the targets (ADR 0002).
    - Report written to `docs/benchmarks/baselines/m1-sim-tick.md` with full metadata (hardware, OS, compiler, flags, build type, workload).
    - Update `measured` on the existing `sim_tick_avg`/`sim_tick_p99` entries in `budgets.json`; the baseline records that M1 measures the ECS-only slice of their §8.1 workload (2k dynamic bodies land in M3); CI perf lane runs the subset per PRD §14.
  - **Verify:** `laige-bench --suite=sim-tick` avg ≤ 3.0 ms and p99 ≤ 5 ms on both backends on the CI reference machine (if local machine differs, record measured value + CI is the gate); baseline file exists.
  - **Size:** ~200 lines + baseline doc

- [x] **M1-SAMPLE-01 · `hello.laige` (headless template)**
  - **Refs:** NFR-13.5 (template < 100 lines), PRD §13
  - **Depends:** M1-HEAD-01, M1-DET-03
  - **Scope:**
    - `samples/hello/`: `hello.laige` project (manifest + `config.json` + one game source) — one component (`PlayerPos`), one system (moves it by a constant velocity per tick, wrapped in a bounded box), deterministic, records its own replay when `--replay` is passed.
    - **< 100 lines of game code** (PRD §9.4), heavily commented (canonical pattern for AI agents, NFR-13.5).
    - Built and run in CI (headless, 300 ticks, replay recorded + replayed + hash-compared — this is the scenario behind M1-DET-04).
  - **Verify:** CI builds `hello`, runs it, and its replay is bit-exact; game-code line count < 100 (checked in CI, PRD §9.4).
  - **Size:** ~120 lines (sample + wiring)

## Milestone gate

- [ ] **M1-EXIT-01 · M1 exit gate**
  - **Refs:** PRD §15 M1 exit criteria
  - **Depends:** all other M1 steps
  - **Scope:**
    - Confirm and record: (1) `sim-tick` budget green on both SimMath backends (link report), (2) replay bit-exact on CI across all P0 OS jobs and both backends (link run), (3) zero-alloc assertion green on the 10k workload (link test log).
    - Update Progress Board; note any deferred P1 items (there should be none in M1 — everything here is P0).
  - **Verify:** all three evidence links present; no open M1 step.
  - **Size:** docs only
