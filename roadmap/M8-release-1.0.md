# M8 — Release 1.0

**PRD:** §15 M8, §16 · **Duration:** 4–6 weeks
**Scope (PRD):** Docs pass, samples polish, reproducible releases, security review,
72 h soak, 1.0 tag.
**Exit criteria (PRD):** All P0 FRs closed; budgets green; 3 reference games shipped
with the engine (isometric flagship first).

Rules specific to this milestone: nothing new is built here except verification,
packaging, and documentation. Any feature found missing is a **PRD revision event**
(scope decision), not silent backfill (AGENTS.md §1: surface the conflict).

---

- [ ] **M8-DOC-01 · Documentation pass (100% public API)**
  - **Refs:** NFR-8.12 (100% doxygen coverage; example or `@experimental` per symbol), DOC-001…DOC-007, §9 public-API doc requirements
  - **Depends:** M4-EXIT-01 (API freeze)
  - **Scope:**
    - Every public symbol has doxygen docs: ownership/lifetime, thread-safety + phase, time complexity + allocation behavior, batching/caching, blocking/I/O/GPU-sync behavior, determinism/network implications, invalidation + failure behavior, **Performance** section for hot-path APIs (DOC-004), performant example + misuse warning (PRD §9).
    - API manifest (`laige-api.json`) at 100% symbol coverage (CI-checked, M0-TOOL-01); error strings all conform to `{code}|{what}|{why}|{fix}|{doc_anchor}` (NFR-13.3 — CI format check over the error registry).
    - `docs/README.md` index honest about experimental areas (DOC-001); all examples compiled in CI (DOC-006).
    - `docs/compatibility/`: supported platforms/compilers, format versions, migration notes (none breaking in 1.0 — verify against the M4 freeze).
  - **Verify:** CI checks green: doxygen coverage 100% (missing symbol = failure), manifest parity, error-grammar scan, example builds. Coverage report committed.
  - **Size:** docs + CI checks

- [ ] **M8-SAMPLE-01 · Three reference games shipped**
  - **Refs:** PRD §15 M8 exit ("3 reference games"), §3 (representative games), §16
  - **Depends:** M3-SAMPLE-01, M4-SAMPLE-01, M6-EXIT-01
  - **Scope:**
    - Ship, polished, and CI-verified: (1) **isometric ARPG** (flagship — extends M3-SAMPLE-01: combat, animations, camera, picking, audio; the §16 primary reference), (2) **platformer** (side-view, parallax, tilemaps — proves AC-4.1 from the user side), (3) **MMO demo zone** (one zone, `laige-server`, join/play/leave — the §12 demo).
    - Each: ≤ documented line budget for the *game* code (flagship ≤ 1500, platformer ≤ 800, MMO demo ≤ 1200 — recorded as the "shippable in a day" evidence for §16.1), builds from a fresh clone per `docs/getting-started/first-project.md`, runs headless in CI + windowed on all P0 (manual check recorded per OS).
    - §16.1 measurement: fresh clone → running `hello.laige` ≤ 10 min with 5 external devs (humans and agent-only builds) — record the actual times.
  - **Verify:** all three green in CI; line budgets enforced; §16.1 timing table committed.
  - **Size:** sample polish + docs

- [ ] **M8-REL-01 · Reproducible releases**
  - **Refs:** NFR-8.6 (same sources + versions ⇒ same binaries; checksums), NFR-8.8
  - **Depends:** M0-DEP-01
  - **Scope:**
    - Release pipeline: pinned `deps.lock` hashes (no network fetch at build — all vendored, NFR-8.8), fixed flags (documented per platform), deterministic build options (timestamps stripped / pinned where the toolchain allows — document what *is* and *isn't* bit-reproducible and why).
    - Release artifacts: engine libs + headers, tools (`laige-run`, `laige-server`, `laige-asset`, `laige-replay`, `laige-bench`), 3 reference games, checksums per file; archive verification step (rebuild-from-pinned-sources reproduces the checksums — proven on at least Linux + Windows in the pipeline).
    - `docs/compatibility/releases.md`: 1.0 artifact list, checksums, platform matrix, known limitations (honest, DOC-001).
  - **Verify:** two independent builds from the pinned source produce identical checksums (recorded); archive verify step in the pipeline.
  - **Size:** pipeline + docs

- [ ] **M8-SEC-01 · Security review**
  - **Refs:** NFR-8.7 (fuzzed surfaces, no `system()`/`exec`), M7-SEC-01
  - **Depends:** M7-SEC-01
  - **Scope:**
    - Final security pass: (1) fuzz coverage report — every untrusted-input surface (protocol, assets, config, replay, registry, STUN/relay, telemetry) has a fuzz target + corpus (the M6/M3 lists, re-verified), (2) static check: no `system`/`exec`/`popen` anywhere in engine code (grep + review, committed result), (3) memory-safety summary: ASan/UBSan/TSan clean on all P0 (CI evidence links), (4) dependency security review: each vendored dep's CVE history checked, licenses verified (MIT/public-domain per PRD §11 table), (5) no credential/secret handling in engine (LOG-005 — verified in code review).
    - Findings: any P0/P1 finding blocks the tag (fix = separate step added here with its own ID); P2 findings recorded with rationale.
  - **Verify:** review doc `docs/security/1.0-review.md` committed with evidence links; static check in CI permanently.
  - **Size:** review + CI check

- [ ] **M8-SOAK-01 · Final 72 h soak (release candidate)**
  - **Refs:** NFR-8.4 (0 crashes per release candidate), PRD §14
  - **Depends:** M7-SOAK-01
  - **Scope:**
    - Re-run the M7 soak on the **release candidate** build (optimized, not debug): 72 h, same scenario schedule, same checks (crashes = 0, trends bounded, determinism spot-checks).
    - Also: 72 h **editor** soak (M5's nightly 4 h extended for the release: editor open, scripted editing, play/stop cycles — leak-free).
    - Soak report committed with the full timeline.
  - **Verify:** both soak reports: 0 crashes, trends within documented bounds.
  - **Size:** scenarios + reports

- [ ] **M8-BUDGET-01 · Full §8.1 budget suite green**
  - **Refs:** PRD §8.1 (all budgets), NFR-8.1 (PRD §8.1 policy: CI-gated)
  - **Depends:** M1-BENCH-01, M2-PERF-01, M3-PHYS-12, M6-TEST-01, M7-AC-01
  - **Scope:**
    - Run the complete budget suite on the pinned CI hardware: frame time p95 ≤ 8.3 ms @ 1080p; sim tick 10k/2k ≤ 3.0 ms avg / 5 ms p99; 50k sprites ≤ 30 draw calls / ≤ 2 ms CPU; iso depth rebuild ≤ 0.2 ms; iso picking ≤ 0.01 ms; sim allocs = 0 (asserted); base memory ≤ 100 MB; cold start ≤ 2 s SSD / 5 s cold; build time ≤ 10 min CI / 5 min local warm; zone 2k @ 20 Hz p95 ≤ 8 ms / ≤ 4 GB.
    - Every number recorded with full AGENTS §12 metadata; any breach = tag blocker (budget revision requires a PRD revision per §8.1 policy — no silent tolerance).
    - The perf-regression lane is permanent now: PRs that regress a budget > 10% fail CI (NFR-8.1 policy implemented).
  - **Verify:** all 10 budget lines green + baseline report `docs/benchmarks/baselines/m8-full.md`; CI regression lane demonstrated (a deliberate regression in a scratch PR fails CI, then reverted).
  - **Size:** report + CI lane

- [ ] **M8-TAG-01 · 1.0 tag + success-metrics report**
  - **Refs:** PRD §16 (all six success metrics), §15 M8
  - **Depends:** M8-DOC-01, M8-SAMPLE-01, M8-REL-01, M8-SEC-01, M8-SOAK-01, M8-BUDGET-01
  - **Scope:**
    - Success-metrics report (§16): (1) ease — §16.1 times from M8-SAMPLE-01, (2) performance — M8-BUDGET-01 links, (3) stability — 0 crash bugs open + determinism checker green 30 consecutive merges (CI history link), (4) scale — 2k demo 72 h healthy (M7-LOAD-01/soak links), (5) API health — 0 breaking changes since M4 freeze (manifest diff M4→1.0 = additive-only, checked) + 100% manifest coverage, (6) dependency discipline — ≤ 10 vendored deps (deps.lock count) + 0 new deps since M0 without PRD revision.
    - Git tag `v1.0.0` on the release commit; changelog (milestone-by-milestone, from the Change Log); `docs/compatibility/releases.md` final entry.
  - **Verify:** report committed with all six metrics evidenced; tag exists; changelog matches the roadmap Change Log.
  - **Size:** report + tag

- [ ] **M8-EXIT-01 · M8 exit gate**
  - **Refs:** PRD §15 M8 exit criteria
  - **Depends:** M8-TAG-01
  - **Scope:**
    - Confirm: all P0 FRs closed (checklist walk: FR-1.1…FR-12.5 P0 items each mapped to a green step in this roadmap — the mapping table is committed in `docs/compatibility/fr-closure-1.0.md`); budgets green; 3 reference games shipped; 1.0 tagged.
    - Final Progress Board update (all milestones done or explicitly descoped with ADRs).
  - **Verify:** FR-closure table committed (no P0 FR without evidence); Progress Board at 100% for M0–M8 P0.
  - **Size:** docs only
