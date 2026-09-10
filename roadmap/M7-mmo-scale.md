# M7 — MMO Scale

**PRD:** §15 M7, §12, §7.10 (sharding, persistence, anti-cheat) · **Duration:** 8–12 weeks
**Scope (PRD):** Sharding, world registry, persistence seam, deterministic parallel
sim (optional), load harness, anti-cheat review, 2k-player scenario.
**Exit criteria (PRD):** AC-10.1/10.2/10.3 met; nightly load green 2 weeks straight.

Rules specific to this milestone: scale is achieved by replicating **zones**, not
state (PRD §12.7) — the deterministic sim stays single-process per zone unless
M7-SIM-01's optional parallel path is enabled (and it must stay bit-exact when on).
Every scale claim here is measured on the pinned CI server-class machine (CORE-001).

P1 items FR-10.7 (chat/UI sync), FR-10.9 (hot zone load/unload), FR-12.4 (crash
reports), FR-12.5 (telemetry) are placed here because the server owns their
surfaces; each is marked P1 and descopable by decision if M7 slips (PRD §17: MMO
scale is the top-impact risk — the degradation ladder must keep demos working at
1k players if 2k slips).

---

## Decisions

- [ ] **M7-DEC-01 · Persistence seam scope (D-PERSIST)**
  - **Refs:** PRD §18.6 (external store only vs built-in SQLite), §12.5
  - **Depends:** —
  - **Scope:**
    - Confirm: the engine exposes `EntitySnapshot` export/import + command event stream and nothing more (no built-in database); games plug in SQLite/Postgres/external services behind the seam.
    - ADR `docs/decisions/000X-persistence-seam.md` (seam contract: snapshot schema, event stream format, versioning).
  - **Verify:** ADR exists; seam contract matches M7-PERSIST-01's implementation.
  - **Size:** docs only

## Sharding & persistence

- [ ] **M7-SHARD-01 · World registry (file-backed)**
  - **Refs:** PRD §12.1/§12.7 (world registry for zones/instances), FR-10.9
  - **Depends:** M6-NET-09, M7-DEC-01
  - **Scope:**
    - Registry: zones + instances (dungeons, lobbies) registered with (zone id, host address, capacity, state: open/full/maintenance); file-backed (JSON, atomic writes, crash-safe per NFR-8.6 reproducible-ops) — external DB is a *game* concern, not the engine's (§5 non-goal).
    - Registry operations: register/unregister/list/lookup, TTL-based stale-entry eviction (documented TTL), watch API (change notifications, bounded queue).
    - No registry = no cross-zone travel, but single-zone play unaffected (SCALE-007: single-player/small games never pay for sharding).
    - Unit tests: concurrent register/lookup (TSan), crash mid-write → clean recovery (file corruption test), stale eviction exact.
  - **Verify:** `ctest -R shard_registry` green under TSan.
  - **Size:** ~250 lines + tests

- [ ] **M7-PERSIST-01 · Persistence seam (snapshot export/import + events)**
  - **Refs:** PRD §12.5, FR-10.x (state model), M7-DEC-01
  - **Depends:** M7-DEC-01, M1-ECS-03
  - **Scope:**
    - `EntitySnapshot`: versioned export/import of authoritative entity state (components per trait's serialization, M6-NET-03 codegen reused); batch export (bounded: max entities per call, documented); import validates version + integrity (hash per entity, mismatch → reject with the exact id).
    - Command event stream: game-affecting commands (the client→server inputs of record) emitted as a durable stream (bounded retention, config) so an external store can rebuild history/audit (FR-10.10).
    - Seam is engine-neutral: a sample in-process store adapter (memory/SQLite-style file, test-only) proves the contract — shipping adapter is the game's job.
    - Unit/integration tests: 10k-entity snapshot export → import → state hash equality; partial import (subset of entities) documented + tested; event stream round-trip.
  - **Verify:** `ctest -R persist_seam` green.
  - **Size:** ~300 lines + tests

- [ ] **M7-SHARD-02 · Cross-zone gateway (travel with state handoff)**
  - **Refs:** PRD §12.1/§12.7 (gateway for travel/teleport with state handoff)
  - **Depends:** M7-SHARD-01, M7-PERSIST-01
  - **Scope:**
    - Travel: client requests travel zone A → B; gateway coordinates: snapshot the player state (M7-PERSIST-01) → handoff to zone B (validated import) → player despawns from A **only after** B confirms (atomic handoff — no player lost on A-crash or B-crash, documented failure matrix).
    - Bounded: travel rate limit per player (config, anti-spam), handoff timeout (documented; timeout → rollback to A, error to client).
    - Unit/integration tests: happy path; A crashes mid-handoff → player recoverable in A (or documented B-side recovery); B rejects (version mismatch) → player stays in A; 100 concurrent travels bounded (no unbounded queue, SCALE-003).
  - **Verify:** `ctest -R shard_gateway` green; failure-matrix tests all green.
  - **Size:** ~300 lines + tests

- [ ] **M7-SHARD-03 · Zone process model (separate processes)**
  - **Refs:** FR-10.9 (zones in separate processes; registry = horizontal scale)
  - **Depends:** M7-SHARD-01, M6-NET-09
  - **Scope:**
    - Zone supervisor: launches/supervises zone processes (config-driven), restarts on crash (exit codes from M6-NET-09 watchdog), registers/unregisters with the registry, passes session handoff tokens for reconnects across zone restarts (documented: in-flight sessions during zone restart → client reconnect flow, bounded retry).
    - Resource isolation: per-zone memory cap (config; OS-level where portable — document the mechanism per platform), CPU affinity optional (documented, not magic).
    - Unit/integration tests: kill a zone process mid-session → supervisor restarts, registry updates, client reconnects within the documented budget; 4 zone processes coexist within the combined cap.
  - **Verify:** `ctest -R shard_processes` green.
  - **Size:** ~300 lines + tests

- [ ] **M7-SHARD-04 · Hot zone load/unload (P1)**
  - **Refs:** FR-10.9 (hot zone load/unload, P1), PRD §12.6 (ops surface)
  - **Depends:** M7-SHARD-03
  - **Scope:**
    - Runtime zone load/unload without server restart: load a zone (scene + config) while other zones run; unload a zone (drain players with the documented kick/teleport flow, flush snapshots, bounded drain time).
    - Budgets: zone load ≤ documented time (measured), unload drain ≤ documented time; both log + telemetry.
    - Unit/integration tests: load zone C into a running 2-zone server (zones A/B unaffected — tick times unchanged within tolerance); unload zone A with 50 players → all players teleported/kicked per config, snapshots flushed.
  - **Verify:** `ctest -R shard_hotload` green.
  - **Size:** ~250 lines + tests

## Simulation scale

- [ ] **M7-SIM-01 · Optional deterministic parallel sim**
  - **Refs:** PRD §10.2 (deterministic parallelism, fixed partitioning, off by default, bit-exact)
  - **Depends:** M1-DET-04, M3-PHYS-02
  - **Scope:**
    - Parallel path for large zones: sim partitioned into spatial regions (fixed partitioning — no dynamic work-stealing in the deterministic path), independent regions computed in worker jobs (CONC-002: partitioned ownership, no broad locking), cross-region interactions (bodies near boundaries) handled by a documented exchange step (ghost cells, documented).
    - **Off by default** (config flag); when on, output is bit-identical to the single-threaded path (the determinism claim, ARCH-010 — stated scope: same build/ISA).
    - Overhead when off: measured ≤ 0.5% (partition bookkeeping only when enabled — measured, DBG-004).
    - Determinism test: 2k-entity zone, 600 ticks, parallel on/off → identical state hashes (the acceptance for this feature).
    - If the partitioning cannot be made bit-exact within 2 attempts at a design change, this step is descoped (decision + ADR, PRD §17 risk: parallelism is the optional M7 item).
  - **Verify:** `ctest -R sim_parallel` green (on/off hash equality); off-mode overhead measured.
  - **Size:** ~400 lines + tests (the cap; split into partitioning / boundary-exchange if needed)

- [ ] **M7-CHAT-01 · Chat & basic UI state sync (FR-10.7, P1)**
  - **Refs:** FR-10.7 (reliable chat channel, emotes, basic UI state sync e.g. shop selection)
    - **Depends:** M6-NET-04 (reliable channel + replication rules)
    - **Scope:**
    - Chat: per-zone and per-player (whisper) channels over the reliable channel; message size cap (documented), rate limit per player (config, anti-spam; violation → documented mute window, telemetry); emotes as replicated component updates (existing replication path — no new protocol).
    - Basic UI state sync: a generic "UI state" replicated component (small map of key → bounded value, documented limits) for things like shop selection; per-player, not broadcast.
    - Unit/integration tests: chat under 100 players (delivered in order, no loss, rate limit fires at the documented count); UI state round-trip for a scripted shop flow.
  - **Verify:** `ctest -R net_chat` green.
  - **Size:** ~200 lines + tests

## Server ops & anti-cheat

- [ ] **M7-OPS-01 · Crash reports (minidump, P1)**
  - **Refs:** FR-12.4 (opt-in minidump + symbolication; deterministic-replay association)
  - **Depends:** M6-NET-09
  - **Scope:**
    - Opt-in crash capture (config flag, off by default in the safe sense: *opt-in* per FR-12.4): minidump on fatal (platform: Breakpad-style capture for Windows, signal handler + backtrace for Linux, mach exception for macOS — wrap at a narrow boundary, DEP-004).
    - Association: crash report carries the last N ticks' replay hashes + seed (M1-DET-03) so a crash is replayable to the failing tick (the PRD's association requirement); report written atomically to a documented path, symbolication is a separate tool (`laige-symbolicate`, offline).
    - No user/PII in reports (LOG-005); report size bounded.
    - Unit/integration tests: induced crash (fixture) → dump written, replay association present, engine's fatal path (controlled termination, LOG Fatal contract) completes flush before exit.
  - **Verify:** `ctest -R crash_reports` green per platform (Linux in CI P0; Windows/macOS in their CI jobs).
  - **Size:** ~250 lines + tests

- [ ] **M7-OPS-02 · Perf telemetry (opt-in, local)**
  - **Refs:** FR-12.5 (developer-facing, opt-in, local only; no user/PII telemetry in the engine)
    - **Depends:** M1-PROF-01
    - **Scope:**
    - Local telemetry sink: opt-in (config), writes structured perf events (tick/frame/bandwidth/queue metrics — the existing profiler fields, batched at a documented rate, bounded file size with rotation) to a local file/endpoint the *developer* configures.
    - Explicitly not a network service: no built-in remote endpoint, no PII, no account data (NFR-12.5 boundary); disabled telemetry adds negligible work (measured ≤ 0.1% per DBG-004).
    - Unit tests: disabled cost measured; enabled output schema validated (JSON lines, versioned); rotation at the documented size.
  - **Verify:** `ctest -R telemetry` green.
  - **Size:** ~150 lines + tests

## Scale acceptance

- [ ] **M7-LOAD-01 · 2k-player scenario (nightly)**
  - **Refs:** AC-10.1 (2,000 players @ 20 Hz, p95 tick ≤ 8 ms, 8-core), PRD §12.8 (nightly full)
  - **Depends:** M6-LOAD-01, M7-SHARD-03
  - **Scope:**
    - `idle_2000` + `combat_2000` scenarios (M6-LOAD-01 formats) run **nightly** on the pinned 8-core server-class CI machine (subset per merge: `combat_100` + `churn_100`).
    - Asserts (AC-10.1): p95 server tick ≤ 8 ms, RSS ≤ 4 GB, 72 h stability tracked (see M7-SOAK-01), client state latency budget ≤ 1 RTT (measured in the harness, documented method).
    - Degradation telemetry healthy: at 2k, the degradation ladder (M6-NET-06) engages exactly as designed (steps observable, no silent budget breach).
    - Report → `docs/benchmarks/baselines/m7-2k.md` (AGENTS §12 fields).
  - **Verify:** nightly scenario green for 2 consecutive weeks (exit gate requirement); baseline committed.
  - **Size:** scenarios + report

- [ ] **M7-AC-01 · AC-10.2 bandwidth at scale**
  - **Refs:** AC-10.2 (idle ≤ 1 KB/s/player; combat ≤ 50 KB/s/player, auto-degradation)
  - **Depends:** M7-LOAD-01
  - **Scope:**
    - Bandwidth assertions on the 2k scenarios: per-player idle and combat steady-state measured per player (not just aggregate — the PRD says *per player*); degradation ladder verified under pressure (coarse-precision step engages, idle stays ≤ 1 KB/s even under churn).
    - Worst-case check: 10% of players in simultaneous combat bursts (the `combat_2000` pattern) — per-player combat budget still met or degradation engaged (never silent breach).
  - **Verify:** assertions green in the nightly run; numbers in `m7-2k.md`.
  - **Size:** assertions + report section

- [ ] **M7-SEC-01 · Anti-cheat posture review**
  - **Refs:** FR-10.10 (server-authoritative; validated input; bounded replay audit)
  - **Depends:** M6-NET-08, M7-PERSIST-01
  - **Scope:**
    - Audit (checked, documented in `docs/security/anti-cheat.md`): (1) all game-affecting state computed server-side (grep + code review checklist over sim write sites — no client data applied without validation), (2) input validation rules enumerated (rate, range, sequence — the M6-NET-12 suite as the living proof), (3) bounded replay audit storage: last N ticks of inputs + state hashes retained per player (N config, memory documented) for dispute review, (4) no authoritative state reachable via unsafe API from client paths (link-graph check).
    - Red-team test: scripted malicious client (velocity spam, position teleport claims, sequence replay, oversized components) → all rejected/degraded with telemetry, no state corruption (hash-stable after the attack).
  - **Verify:** audit doc committed with evidence links; red-team test green.
  - **Size:** tests + docs

- [ ] **M7-SOAK-01 · 72 h soak (auto-gameplay + abuse)**
  - **Refs:** NFR-8.4 (0 crashes in 72 h soak), PRD §14 (per release candidate)
  - **Depends:** M7-LOAD-01, M7-SEC-01
  - **Scope:**
    - 72 h soak on the pinned machine: 2k bots (`idle`/`combat`/`churn` mixed per schedule file), zone restarts at random documented times, network partition injections (10-minute windows at random times), input floods (rate-limited by the scenario file), 10k-entity spawn/die storms in one zone.
    - Checks continuous: crash count (must be 0), RSS trend (no growth beyond documented bound), queue depths bounded, handle exhaustion counters zero, determinism hash spot-checks (replay a sampled tick from the soak log, hash matches).
    - Soak report: timeline, all degradation events, leak summary (ASan off for the run — documented; a 24 h ASan sub-run included for leak confidence).
  - **Verify:** soak report committed: 0 crashes, trends within bounds, spot-check hashes match.
  - **Size:** scenario + report

## Milestone gate

- [ ] **M7-EXIT-01 · M7 exit gate**
  - **Refs:** PRD §15 M7 exit criteria
  - **Depends:** all other M7 steps
  - **Scope:**
    - Confirm and record: (1) AC-10.1 met (link), (2) AC-10.2 met (link), (3) AC-10.3 still met (lockstep re-verified at this scale's build, link), (4) nightly load green **2 weeks straight** (CI links), (5) 72 h soak 0-crash (link).
    - P1 items status (SHARD-04, CHAT-01, OPS-01/02): done or descoped-with-ADR.
    - Update Progress Board.
  - **Verify:** all evidence links present; no open P0 M7 step.
  - **Size:** docs only
