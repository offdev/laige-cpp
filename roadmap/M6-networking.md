# M6 — Networking

**PRD:** §15 M6, §7.10, §12 · **Duration:** 6–8 weeks
**Scope (PRD):** Transport, protocol, replication, grid-aligned AOI,
prediction/reconciliation, lockstep mode, `laige-server` v1.
**Exit criteria (PRD):** Lockstep bit-exact; 100-player zone at 20 Hz; bandwidth
budgets met.

Rules specific to this milestone: the network is the untrusted boundary (SCALE-004)
— every parser is fuzzed, every field validated, no authoritative mutation from
unvalidated data. The deterministic sim (M1/M3) is the server's engine; networking
wraps it, never replaces it. Bandwidth budgets (AC-10.2) are measured, not assumed
(CORE-001).

---

## Decisions

- [ ] **M6-DEC-01 · NAT traversal scope (D-NAT)**
  - **Refs:** PRD §18.7 (STUN-only vs STUN+TURN)
  - **Depends:** —
  - **Scope:**
    - Decide M6 scope: STUN-style discovery + relay endpoint support (recommended) vs full TURN in-engine (rejected: TURN is a service, not an engine concern — per PRD §5 non-goals: "we provide the networking stack, not the service").
    - ADR `docs/decisions/000X-nat-traversal.md`.
  - **Verify:** ADR exists; decision matches the scope implemented in M6-NET-11.
  - **Size:** docs only

## Transport & protocol

- [ ] **M6-NET-01 · Vendor ENet + transport layer**
  - **Refs:** FR-10.2 (UDP; reliable-ordered + unreliable; retransmission; congestion-aware), PRD §11 (ENet row)
  - **Depends:** M0-DEP-01
  - **Scope:**
    - Vendor ENet (deps.lock); transport module: channel model (reliable-ordered for chat/UI/commands; unreliable for movement/state — channel map documented per message type); retransmission via ENet channels; congestion-aware send rate (ENet throttle + engine-level per-player send budget, G-R7).
    - Transport is thread-isolated: dedicated I/O thread (PRD §10.2), state handoff to sim at tick boundaries (one copy, no locks in hot path — document the handoff).
    - Failure modes: connection loss, timeout, half-open — all → structured events (no silent drops of *reliable* data; unreliable loss is counted + telemetry).
    - Unit/integration tests (loopback): reliable channel reorders/loses injected packets (packet-loss injection harness) → delivered in order, no loss; unreliable drops counted exactly; reconnect semantics.
  - **Verify:** `ctest -R net_transport` green under TSan; packet-loss harness green.
  - **Size:** ~300 lines + tests

- [ ] **M6-NET-02 · Versioned binary protocol framing**
  - **Refs:** FR-10.11 (versioned, forward-compat negotiation, no reflection over the wire, packet budget telemetry)
    - **Depends:** M6-NET-01
    - **Scope:**
    - Packet framing: header (protocol version, session id, channel, sequence, payload length); length-bounded parsing (all reads checked — SCALE-005); version negotiation on connect (forward-compat: older client + newer server = negotiated min version, documented; incompatible → clean reject with version info).
    - No reflection over the wire: every message type is an explicit enum with generated dispatch (no dynamic lookup by name).
    - Packet budget telemetry: per-player per-tick bytes sent/received (feeds G-R7 + FR-10.5 degradation).
    - Malformed packet handling: every truncation/corruption → drop + count + telemetry, never crash (fuzzed in M6-NET-12).
    - Unit tests: negotiation matrix (3×3 version pairs), length overflow attempts, unknown message id → documented drop (forward-compat: skip-padding rule documented).
  - **Verify:** `ctest -R net_protocol` green.
  - **Size:** ~250 lines + tests

- [ ] **M6-NET-03 · Generated serialization (bitpacked)**
  - **Refs:** FR-10.3 (generated from component traits, bitpacked; hand-written serialization not in the safe API), S-3
  - **Depends:** M4-TRAIT-01, M6-NET-02
  - **Scope:**
    - Codegen: component trait declarations (replicated: rate, compression hints, bit widths) → bitpacked serialize/deserialize functions + a protocol manifest (field order, widths, version — the single source of truth for wire format).
    - Codegen output is deterministic and versioned (manifest version bumps on any wire-affecting trait change; readers reject unsupported versions explicitly — ARCH-007).
    - No hand-written serialization in the safe API: user components with replication traits *must* go through the generator (trait without a generated serializer → registration error, documented).
    - Round-trip tests: every replicated built-in + the Appendix B `Health` component: value → bytes → value exact; bit-width reductions (degradation ladder) round-trip within documented precision loss.
  - **Verify:** `ctest -R net_codegen` green; manifest↔trait parity checked in CI (like M4 bindings parity).
  - **Size:** ~350 lines + tests

- [ ] **M6-NET-04 · Replication engine (per-component rules)**
  - **Refs:** FR-10.3 (replicated: server→all / client→server / both; rate)
  - **Depends:** M6-NET-03, M1-SYS-02
  - **Scope:**
    - Replication system: for each replicated component, per the trait: direction (server→all, client→server, both), update rate (Hz), delta-encoding (changed-fields only, documented field granularity).
    - Per-player outgoing queue (bounded — SCALE-003; overflow → degradation, M6-NET-06); inbound queue bounded with drop policy per direction (client→server input never silently dropped beyond the documented window → logged).
    - Determinism: server state only changes from validated inbound (SCALE-004) — architecture test: no render/transport state leaks into sim writes (declared I/O check pattern).
    - Unit/integration tests: a replicated component at 5 Hz updates exactly 5×/s (measured); direction matrix (all 4 combinations) behaves per trait; client→server spam (10k msgs/s) → bounded queue + documented drop events, no growth (memory-stable over 60 s).
  - **Verify:** `ctest -R net_replication` green; 60 s spam test memory-stable (ASan).
  - **Size:** ~350 lines + tests

- [ ] **M6-NET-05 · Interest management (AOI, grid-aligned for iso)**
  - **Refs:** FR-10.4 (spatial-hash interest regions; grid-aligned for isometric; enter/leave streaming; per-player entity budget), SCALE-002
  - **Depends:** M6-NET-04, M2-TILE-01 (grid config), M3-PHYS-02 (spatial hash pattern)
  - **Scope:**
    - AOI: per-player interest region = axis-aligned box in **simulation space** (for iso worlds: a rhombus on screen, an axis-aligned box in sim — the PRD's key insight), centered on the player, radius per zone config (documented units).
    - Enter/leave streaming: entity entering a player's AOI → full state snapshot sent once; leaving → despawn message; movement across AOI boundaries recomputed on a coarse tick (not per-tick O(players × entities) — document the batching: AOI delta computed per spatial-hash cell transition).
    - Per-player entity budget (config, named): over budget → farthest entities unstreamed (documented degradation, telemetry).
    - Deterministic: AOI membership for a given tick + player positions is a pure function (replay-able — test with recorded inputs).
    - Unit/integration tests: a player walking a grid crosses exactly the documented cell boundaries (enter/leave events golden); 200 entities in a 100-player zone → per-player streamed set exact; budget degradation verified.
  - **Verify:** `ctest -R net_aoi` green.
  - **Size:** ~350 lines + tests

- [ ] **M6-NET-06 · Snapshot + delta sync + bandwidth budgets**
  - **Refs:** FR-10.5 (20–100 Hz server tick, snapshot+delta, per-player per-tick budget, auto-degradation, client interpolation buffer 100–250 ms)
    - **Depends:** M6-NET-05, M6-NET-03
    - **Scope:**
    - Server: per-tick, per player: snapshot (first time / rejoin) or delta (changed components within AOI) — delta encoding via M6-NET-03 bitpacking; per-player per-tick bandwidth budget (config; PRD targets: idle ≤ 1 KB/s, combat ≤ 50 KB/s).
    - Degradation ladder (per component, declared in traits — PRD §12.3): lower update rate → coarser precision (fixed-point bit-count reduction) → region shrinkage; ladder applied automatically when the budget is exceeded; each step logs + telemetry (G-R7).
    - Client: interpolation buffer for remote entities (adaptive 100–250 ms, config; buffer state observable via profiler).
    - Unit/integration tests: idle player (200 in zone, 20 Hz) measures ≤ 1 KB/s steady-state; scripted combat burst measures ≤ 50 KB/s with degradation engaged; ladder steps fire in documented order (counter verified).
  - **Verify:** `ctest -R net_sync` green; bandwidth numbers recorded in `docs/benchmarks/baselines/` (these seed the AC-10.2 measurement).
  - **Size:** ~400 lines + tests (the largest net step; split snapshot/delta vs degradation if needed)

- [ ] **M6-NET-07 · Client-side prediction**
  - **Refs:** FR-10.6 (built-in prediction for player-controlled entities: movement, actions)
  - **Depends:** M6-NET-06, M3-PHYS-06
  - **Scope:**
    - Local player entity: client simulates its own entity locally (same physics, engine math — M1-DET-01 deterministic mode) using local input before the server responds; remote entities use the interpolation buffer.
    - Prediction state is presentation-layer bookkeeping around the replicated entity (documented: predicted vs authoritative state separated, ARCH-009 — the *authoritative* copy is the replicated one; prediction never changes what the server sees).
    - Prediction error observable (profiler field: predicted-vs-authoritative position error, p95).
    - Unit/integration tests (simulated RTT 50 ms): local player movement is smooth (no 50 ms lag visible in presentation), prediction error bounded, no divergence after reconciliation (M6-NET-08).
  - **Verify:** `ctest -R net_prediction` green; error metrics recorded.
  - **Size:** ~300 lines + tests

- [ ] **M6-NET-08 · Reconciliation + server rewind (lag compensation)**
  - **Refs:** FR-10.6 (server reconciliation; hit detection via deterministic replay rewind ≤ 500 ms)
  - **Depends:** M6-NET-07, M1-DET-03
  - **Scope:**
    - Reconciliation: on server state arrival for the local player, the client rewinds its prediction to the server tick, re-applies the local inputs the server already applied (documented input-window accounting), and resimulates — no visible snap when inputs were correct (golden: predicted vs reconciled position delta ≤ documented bound for standard inputs).
    - Server rewind: server keeps a bounded window of past state hashes (≤ 500 ms, config; window = ticks, memory documented) enabling hit-resolution at the *client-claimed* tick for lag compensation (attack at tick t-20 validated against state at t-20).
    - Window overflow: attack claiming older than the window → rejected with the documented error (anti-cheat, FR-10.10).
    - Determinism: rewind uses the M1-DET-03 replay machinery (input log → state at tick), not re-simulation with floats.
    - Unit/integration tests: attack timing under 100 ms RTT resolves at the correct tick (hit/miss golden); window-boundary rejection exact; no state corruption after 1000 reconciliation cycles (ASan + hash-stable).
  - **Verify:** `ctest -R net_reconcile` green.
  - **Size:** ~350 lines + tests

- [ ] **M6-NET-09 · `laige-server` v1 (zone runtime)**
  - **Refs:** FR-10.8 (multi-zone host; zone = one sim instance with own tick; sessions, reconnection, kick/leave; headless)
  - **Depends:** M6-NET-06, M1-HEAD-01
  - **Scope:**
    - `laige-server` binary: config file (zone list: name, scene, tick rate, capacity, AOI radius), one zone = one sim instance (single-threaded per zone, PRD §10.2), zones tick independently (different tick rates allowed, documented).
    - Player sessions: join (auth token — pluggable validator, no built-in accounts per §5 non-goals), leave, kick (admin command), reconnection (session token + state re-sync via snapshot, M6-NET-06).
    - Ops surface (PRD §12.6): log to stdout (structured), metrics endpoint (local HTTP, dev-mode flag, minimal), graceful shutdown (flush zone snapshots to a documented path, bounded time), watchdog hook (exit codes documented for restart supervisors).
    - Headless by definition (AC-6.2) — no GPU/window code linked (include-graph + link check).
    - Integration tests: 2 zones × 10 clients (simulated) join/leave/reconnect under a scripted churn; graceful shutdown flushes snapshots; watchdog exit codes exact.
  - **Verify:** `ctest -R net_server` green (headless CI); graceful-shutdown test green.
  - **Size:** ~350 lines + tests

- [ ] **M6-NET-10 · Lockstep mode**
  - **Refs:** FR-10.1 (b) (small deterministic multiplayer, client-side sim, ≤ N players, input exchange), AC-10.3
  - **Depends:** M6-NET-02, M1-DET-03
  - **Scope:**
    - Lockstep: all clients run the deterministic sim locally; exchange only per-tick input frames (the M3-INPUT-03 format); tick clocking: a client advances to tick t only when all players' inputs for t are in (or a documented timeout → pause + resync, no divergence); player count cap (config, ≤ 8 recommended, documented).
    - Resync: on desync detection (state hash mismatch at tick t — hashes exchanged every N ticks, N config) → rollback to last agreed hash + documented resync (full state from a designated referrer, no silent divergence).
    - AC-10.3 test: 3+ simulated clients, 600 ticks, identical state hashes on all clients (the bit-exactness claim).
    - Unit/integration tests: input ordering edge (one client 50 ms late → others pause, no divergence); desync injection (flip one bit in a client's input stream) → detected at the documented tick, resynced.
  - **Verify:** `ctest -R net_lockstep` green; AC-10.3 assertion recorded in baseline.
  - **Size:** ~300 lines + tests

- [ ] **M6-NET-11 · NAT traversal basics (per D-NAT)**
  - **Refs:** FR-10.2 (NAT traversal basics: STUN-style, relay support), D-NAT
  - **Depends:** M6-NET-01, M6-DEC-01
  - **Scope:**
    - Per the D-NAT decision (default: STUN-style + relay, no in-engine TURN): STUN-style bind/discover against a configurable STUN endpoint; relay: connect through a relay server (relay is an external service — the engine implements the *client* protocol only, documented); candidate gathering (host/served/relay) with a bounded timing budget (no unbounded connect loops).
    - All external responses are untrusted (SCALE-004): parsed with the same length-bounded parsers, fuzzed (M6-NET-12).
    - Unit/integration tests (mock STUN/relay): happy path + every failure mode (timeout, bad response, spoofed attributes) → documented behavior, no crash.
  - **Verify:** `ctest -R net_nat` green; fuzz targets for STUN/relay parsing clean.
  - **Size:** ~250 lines + tests

- [ ] **M6-NET-12 · Protocol fuzz + security validation**
  - **Refs:** NFR-8.7 (network protocol fuzzed in CI), SCALE-004, SCALE-005
  - **Depends:** M6-NET-02, M6-NET-11
  - **Scope:**
    - Fuzz targets: `net_packet` (any channel), `net_protocol_header`, `stun_response`, `relay_msg` — each: bounded per-commit (`--runs=1000`), nightly long; no crash/hang/leak; all failures → `Status` + telemetry only.
    - Security validation suite (beyond fuzz): oversized fields, out-of-range sequence numbers, spoofed session ids, replayed packets (sequence replay → rejected/deduped per documented window), auth token absence/misuse — each an explicit test with the documented response.
    - Baseline corpus committed (hand-broken + fuzzer-discovered seeds).
  - **Verify:** all fuzz targets clean; security suite green; corpus committed.
  - **Size:** ~200 lines + corpus

## Load & acceptance

- [ ] **M6-LOAD-01 · Load harness (bot generator)**
  - **Refs:** PRD §12.8 (CI-testable MMO scenarios), FR-10.x scale prep
  - **Depends:** M6-NET-09, M6-NET-06
  - **Scope:**
    - `laige-load` tool (headless): N simulated clients (bot logic: wander, combat bursts, chat, join/leave churn, partition/latency injection per scenario file — scenario format documented, versioned).
    - Scenarios: `idle_100`, `combat_100`, `churn_100` (100-player milestone set) + `idle_2000`/`combat_2000` stubs (runnable, used fully in M7).
    - Metrics collected per run: server tick p50/p95/p99, per-player bandwidth (idle/combat), AOI streaming cost, queue depths, memory (RSS over time), client-side prediction error.
    - Unit/integration test: `idle_100` runs 10 min headless in CI (subset cadence per PRD §14).
  - **Verify:** `idle_100` 10-min CI run green; metrics report format documented + committed sample.
  - **Size:** ~300 lines + scenarios

- [ ] **M6-TEST-01 · 100-player zone acceptance**
  - **Refs:** PRD §15 M6 exit (100-player zone at 20 Hz; bandwidth budgets met)
  - **Depends:** M6-LOAD-01, M6-NET-08
  - **Scope:**
    - Run the M6 milestone scenario set on the CI reference machine: `idle_100` (20 Hz), `combat_100`, `churn_100`, plus lockstep bit-exact (M6-NET-10) and reconciliation (M6-NET-08) under load.
    - Budgets asserted (not just measured): server tick p95 ≤ 8 ms (the AC-10.1 number, at 100 players — must hold well under it), idle bandwidth ≤ 1 KB/s/player, combat ≤ 50 KB/s/player with degradation engaged.
    - Report → `docs/benchmarks/baselines/m6-100p.md` (AGENTS §12 fields).
  - **Verify:** all assertions green on CI reference machine; baseline committed.
  - **Size:** scenarios + report

- [ ] **M6-EXIT-01 · M6 exit gate**
  - **Refs:** PRD §15 M6 exit criteria
  - **Depends:** all other M6 steps
  - **Scope:**
    - Confirm and record: (1) lockstep bit-exact across clients (AC-10.3, link), (2) 100-player zone at 20 Hz green (link), (3) bandwidth budgets met with degradation telemetry (link), (4) protocol fuzz clean (link).
    - Update Progress Board.
  - **Verify:** all evidence links present; no open M6 step.
  - **Size:** docs only
