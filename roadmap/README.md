# Laige Roadmap — Implementation Checklist

This folder is the **working implementation plan** for the Laige engine
(*Legendary AI Game Engine*), derived from
[`../PRD.md`](../PRD.md) and governed by [`../AGENTS.md`](../AGENTS.md).

It is a checklist designed so that **one AI agent (or one human) can safely pick up
exactly one step at a time**, implement it in a small patch, verify it, and move on —
without ever facing "thousands of lines at once". Every step is small on purpose.

---

## 1. How to use this roadmap (agent contract)

Before picking work, an implementing agent MUST:

1. Read `../AGENTS.md` (the engineering contract) and the relevant `PRD.md` sections.
2. Pick the **lowest-ID unchecked step** whose `Depends` are all checked.
   Milestones are sequential (M0 before M1, etc.); within a milestone, follow file
   order unless `Depends` says otherwise.
3. Implement **exactly the Scope bullets of that step. Nothing else.**
   - No refactoring unrelated code.
   - No features from later steps.
   - No new dependencies, new public API, or new modules unless the step says so.
4. **Size limit:** a step should land in **≤ ~400 lines of code including tests**.
   If it clearly won't, **stop and split the step** (create `Mx-XXX-04a`, `-04b`, …,
   record the split in the Change Log below) instead of delivering a giant patch.
5. Verify before checking the box:
   - Run the step's `Verify` command(s).
   - Complete the applicable items of the AGENTS.md §16 acceptance checklist
     (tests, formatting, no new warnings, docs updated in the same change,
     logging/diagnostics where the step creates runtime behavior).
6. **Check the box in the same PR/commit** that implements the step. PR title MUST
   start with the step ID: `[M2-ISO-01] Isometric depth key computation`.
7. Update the **Progress Board** counts and add one line to the **Change Log**.
8. If a step's scope **conflicts with AGENTS.md or the PRD, stop and surface the
   conflict** (AGENTS.md §1). Do not silently relax a rule.
9. **Decision steps** (`Mx-DEC-…`) produce an ADR under `docs/decisions/`. No dependent
   step may start before its ADR exists.

### Step ID scheme

```
M<milestone>-<SUBSYSTEM>-<sequence>      e.g.  M2-ISO-03
```

| Tag | Subsystem | Tag | Subsystem |
|---|---|---|---|
| `DEC` | Decisions / ADRs | `INPUT` | Input |
| `REPO` | Repository layout | `AUDIO` | Audio |
| `BUILD` | Build system | `ANIM` | Animation |
| `CI` | CI pipelines | `ASSET` | Assets & import |
| `DEP` | Dependencies / vendoring | `SCRIPT` | Scripting |
| `CORE` | Core (Result, log, math, pools, PRNG, config) | `TRAIT` | Component traits |
| `TOOL` | Tooling (manifest, lints, checks) | `UNSAFE` | Unsafe API |
| `TEST` | Test infrastructure | `PASS` | Custom render passes |
| `DOC` | Documentation | `LIGHT` | 2D lighting |
| `ECS` | Entity-component system | `ED` | Editor |
| `SYS` | System framework | `NET` | Networking |
| `LOOP` | Game loop | `LOAD` | Load harness |
| `DET` | Determinism / replay | `SHARD` | Sharding / zones |
| `CFG` | Configuration | `PERSIST` | Persistence seam |
| `PROF` | Profiling / observability | `SIM` | Simulation scale-out |
| `ALLOC` | Allocation guardrails | `SEC` | Security / anti-cheat |
| `GL` | GL context / render infra | `OPS` | Server ops |
| `CAM` | Camera | `SOAK` | Soak testing |
| `PROJ` | Projection modes | `REL` | Releases |
| `ISO` | Isometric (depth keys, picking, presets) | `TAG` | Tagging / changelog |
| `SORT` | Depth sorting | `BENCH` | Benchmarks / budgets |
| `SPRITE` | Sprite batcher | `PERF` | Performance acceptance |
| `SCENE` | Reference scene / scene data | `AC` | Acceptance-criteria suites |
| `TILE` | Tilemaps | `SAMPLE` | Templates / sample games |
| `PAR` | Parallax layers | `WEBGL` / `VULKAN` / `MOBILE` / `MESH` / `TOUCH` / `SKELE` | M9 stretch |
| `TEXT` | Fonts / text | `EXIT` | Milestone gate |
| `UI` | UI widgets | | |
| `GOLD` | Golden/image tests | | |

### Status format

Each step is one top-level checklist item:

```markdown
- [ ] **Mx-XXX-nn · Title**
  - **Refs:** <PRD FR/AC/NFR ids, AGENTS.md rule ids>
  - **Depends:** <step ids, or —>
  - **Scope:** <exact deliverable, 1–4 bullets>
  - **Verify:** <command(s) + expected result>
  - **Size:** <rough LOC including tests — a sanity ceiling, not a target>
```

`- [ ]` = open, `- [x]` = done. A step stays open until its Verify command is green
**and** AGENTS.md §16 is satisfied for the change.

### Canonical commands (established by M0-BUILD-01; referenced throughout)

| Purpose | Command |
|---|---|
| Configure | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` |
| Build | `cmake --build build -j` |
| Test | `ctest --test-dir build --output-on-failure` |
| ASan/UBSan build | `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_ASAN=ON` |
| TSan build | `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_TSAN=ON` |
| Fuzz (bounded) | `./build/bin/laige-fuzz <target> --runs=1000` |
| Benchmarks | `./build/bin/laige-bench --suite=<name>` |
| Determinism check | `./build/bin/laige-detcheck --scenario=<name>` |
| API manifest | `cmake --build build --target laige-api` |
| Include-graph lint + dependency count | `python3 tools/laige-include-lint` |

Exact flags are fixed by `M0-BUILD-01`; later steps must use the documented form.

---

## 2. Milestone index

| File | Milestone (PRD §15) | Exit criteria (PRD) | Steps |
|---|---|---|---|
| [M0-foundations.md](M0-foundations.md) | **M0 — Foundations** (2–3 wks) | CI green on 3 OSes; core unit-tested; budget harness wired | 22 |
| [M1-heartbeat.md](M1-heartbeat.md) | **M1 — Heartbeat** (3–4 wks) | 10k entities @ 60 Hz ≤ 3 ms; replay bit-exact; zero-alloc assertion passes | 25 |
| [M2-rendering-2.5d.md](M2-rendering-2.5d.md) | **M2 — 2.5D Rendering** (4–6 wks) | 50k sprites ≤ 30 draw calls (worst-case iso); isometric is the default template; all AC-4.x pass | 32 |
| [M3-game-feel.md](M3-game-feel.md) | **M3 — Game Feel** (4–6 wks) | Physics budgets + determinism AC; playable isometric sample in-engine | 36 |
| [M4-scripting-customization.md](M4-scripting-customization.md) | **M4 — Scripting & Customization** (3–4 wks) | Script budget watchdog works; a custom-render-pass sample runs | 12 |
| [M5-editor-mvp.md](M5-editor-mvp.md) | **M5 — Editor MVP** (6–8 wks) | A dev can build a small isometric game *entirely* in the editor | 21 |
| [M6-networking.md](M6-networking.md) | **M6 — Networking** (6–8 wks) | Lockstep bit-exact; 100-player zone at 20 Hz; bandwidth budgets met | 16 |
| [M7-mmo-scale.md](M7-mmo-scale.md) | **M7 — MMO Scale** (8–12 wks) | AC-10.1/10.2/10.3 met; nightly load green 2 weeks straight | 15 |
| [M8-release-1.0.md](M8-release-1.0.md) | **M8 — Release 1.0** (4–6 wks) | All P0 FRs closed; budgets green; 3 reference games shipped | 8 |
| [M9-stretch.md](M9-stretch.md) | **M9+ — Stretch** | Per-feature proposals (each gated by its own ADR) | 6 |

P1 items (PRD §7: "M6–M7") that are rendering/editor/tooling in nature are placed in
the milestone that owns their subsystem (noted per step); the PRD's P1/M9 overlap is
flagged where it occurs rather than silently re-scoped.

---

## 3. Decision register (PRD §18 open questions)

Steps tagged `DEC` close these. ADRs live in `docs/decisions/`.

| ID | Question (PRD §18) | Blocking step(s) | Default if unresolved | Decision |
|---|---|---|---|---|
| D-NAME | Engine name & license (MIT proposed) | M0-REPO-01 | keep "Laige", MIT | **Decided 2026-09-10:** keep "Laige" (*Legendary AI Game Engine*); MIT (ADR 0001) |
| D-MATH | Fixed-point default for all deterministic paths, or float-pinned + FP only for lockstep | M1-DET-01, M3-PHYS-11 | Q16.16 default for deterministic mode (PRD §10.3 recommends it for lockstep/MMO) | **Decided 2026-09-10:** SimMath — config-selectable; `fpx16_16` default + `fp32_pinned` opt-in (ADR 0002) |
| D-JSON | Config JSON: tiny in-engine parser vs vendored library | M0-CORE-07 | in-engine bounded parser (no new dep) | **Decided 2026-09-10:** in-engine bounded parser, no new dep (ADR 0003) |
| D-ISO | 2:1 dimetric vs true iso as template default | M2-CAM-02 | 2:1 dimetric (PRD v0.2: pixel-art default) | — open |
| D-UI | Confirm minimal retained UI widget set for M2 | M2-UI-01 | the FR-2.8 P0 list (panel/button/text/image/list/slider/input) | — open |
| D-EDITOR | Editor embedded vs standalone | M5-ED-01 | standalone binary (FR-8.7) | — open |
| D-LUA | Confirm Lua 5.4 (vs no scripting in 1.0, vs WASM) | M4-SCRIPT-01 | Lua 5.4, optional module, off by default | — open |
| D-NAT | M6 NAT traversal scope: STUN-only vs STUN+TURN | M6-NET-11 | STUN-style + relay endpoint, no TURN server in-engine | — open |
| D-PERSIST | Persistence seam: external store only vs built-in SQLite | M7-PERSIST-01 | external store only (PRD §12.5) | — open |

---

## 4. Progress board

Updated in the same PR that closes steps. "Done" = box checked + Verify green.

| Milestone | Steps | Done | Status |
|---|---|---|---|
| M0 | 22 | 21 | ▶ in progress |
| M1 | 25 | 0 | ⬜ not started |
| M2 | 32 | 0 | ⬜ not started |
| M3 | 36 | 0 | ⬜ not started |
| M4 | 12 | 0 | ⬜ not started |
| M5 | 21 | 0 | ⬜ not started |
| M6 | 16 | 0 | ⬜ not started |
| M7 | 15 | 0 | ⬜ not started |
| M8 | 8 | 0 | ⬜ not started |
| M9 | 6 | 0 | ⬜ proposals only |
| **Total** | **193** | **21** | |

---

## 5. Change log

One line per completed (or split/renumbered) step.

| Date | Step | Commit | Note |
|---|---|---|---|
| 2026-09-10 | M0-DEC-01, M0-DEC-02, M0-DEC-03 | — | Decisions recorded by project owner; ADRs 0001–0003 written in `docs/decisions/`; docs only, no code |
| 2026-09-10 | M0-REPO-01 | — | Repo skeleton: top-level `README.md`/`LICENSE` (MIT, ADR 0001)/`.gitignore`; root `CMakeLists.txt` (CMake ≥ 3.22, C++20, `-Wall -Werror`, no exceptions/RTTI via `laige_apply_engine_policy`, `LAIGE_BUILD_SHARED` placeholder); PRD §10.1 module dirs with only `laige-core` populated; empty-target configure+build verified |
| 2026-09-10 | M0-BUILD-01 | — | `laige-core` CMake target (static default, shared via `LAIGE_BUILD_SHARED`; no transitive leakage — policy flags PRIVATE, CPP-010); options `LAIGE_ASAN`/`LAIGE_TSAN` (mutually exclusive, whole-tree instrumentation, fatal UBSan, TSan `halt_on_error=1`), `LAIGE_SCRIPT` reserved, `LAIGE_BUILD_TESTS` default ON; canonical commands fixed in `docs/getting-started/building.md` (source of truth); link smoke test `tests/laige-core` with NFR-8.10 `static_assert` policy self-checks; static+shared+ASan+TSan+Clang builds verified warning-free; fixed latent invalid `target_compile_features` call in M0-REPO-01 policy function |
| 2026-09-10 | M0-DEP-01 | — | `deps.lock` (repo root) + configure-time verification in `cmake/laige-deps-lock.cmake` (deterministic tree SHA-256; fails loudly on mismatch, missing tree, unlisted `deps/` dir, or malformed lock); vendored GoogleTest v1.18.0 (`deps/googletest`, 252 files, commit `063de7e9…`, BSD-3-Clause) wired into tests only (`gtest_main`, `BUILD_GMOCK`/`INSTALL_GTEST` off, no-exceptions/no-rtti flags match engine test TUs); `laige-core_tests` converted to the first GTest suite; ADR 0004 written; fresh+shared+ASan+Clang trees verified warning-free with `ctest` 1/1, tampered vendored file fails the configure |
| 2026-09-10 | M0-CI-01 | `7ec98b9` | CI matrix `.github/workflows/ci.yml` (all 5 P0 jobs on push to `master` + `workflow_dispatch`: linux-gcc, linux-clang, windows-msvc, macos-arm64, macos-intel) and `ci-pull.yml` (one P0 OS per PR, selected by `ci:linux`/`ci:windows`/`ci:macos` labels, default Linux — PRD §14 cadence); each job = canonical configure→build→ctest with `timeout-minutes: 10`; fixes landed in the same step: `.gitattributes` (`deps/** -text`) for byte-exact LF vendored checkouts (Windows CRLF broke the tree-hash lock), `_MSVC_LANG` for the C++20 self-check on MSVC, MSVC `/EH` conflict resolution (strip platform-default `/EHsc`; gtest rewritten to its documented no-exception set `/EHs-c- -D_HAS_EXCEPTIONS=0`), and `_HAS_EXCEPTIONS=0` in the engine policy for the MS STL (C4530 under `/WX`); verified: full matrix green after pushing `a90191c..7ec98b9` |
| 2026-09-10 | M0-CI-02 | `6573a64` | Sanitizer CI lanes `linux-asan` (LAIGE_ASAN=ON: ASan+UBSan, reports fatal via `-fno-sanitize-recover=all` + `ASAN_OPTIONS` abort/halt) and `linux-tsan` (LAIGE_TSAN=ON, per-test `TSAN_OPTIONS=halt_on_error=1`) in `ci.yml` (merge, all 7 jobs) and `ci-pull.yml` (PRs under the default-Linux P0 condition); sanitizer reports archived as artifacts (`linux-asan-reports`/`linux-tsan-reports`) on every run, green or red; the first CI run rejected the workflow file because step-level `permissions:` is not a valid schema — fixed in `f75ed0d` by moving `actions: write` to job scope (with `continue-on-error: true` uploads in `ci-pull.yml` for fork-PR read-only tokens); Verify cycle on CI: scratch OOB read (`6331a40`) failed `linux-asan` with the UBSan "index 16 out of bounds for type int[4]" report (archived) while `linux-tsan` and the other five jobs stayed green; scratch removed in `25b57b9`; full 7-job matrix green |
| 2026-09-10 | M0-CI-03 | `785af81` | `tools/laige-include-lint` (Python 3 stdlib): parses `#include` edges of `src/**`, enforces R1 (laige-core includes nothing internal), R2 (arrows only downward in the PRD §10.1 stack, via the target's public include root — CPP-010), R3 (vendored deps only from their `deps.lock` `owner` — new required lock field, validated by `cmake/laige-deps-lock.cmake`; angle-bracket vendored header paths caught via a vendored-header map), R4 (engine code includes only `src/**`/`deps/**`); reports the vendored-dependency list and fails above the PRD §11 budget of 10; `include-lint` CI job in `ci-pull.yml` (every PR, label-independent) and `ci.yml` (every merge, now 8 jobs); CTest coverage in `tests/tools` (4 fixture trees + real-tree check, expected failures asserted via generated `cmake -P` scripts because CTest inverts `PASS_REGULAR_EXPRESSION` under `WILL_FAIL`); local Verify: illegal `laige-core → laige-render` stub include fails with R1, all rule directions exercised, dep count prints (1/10), `ctest` 6/6 on g++/shared/ASan/Clang trees; pushed as `785af81` — CI observed via the GitHub API: `ci.yml` (8-job) run 34518244428 on `741c163` green, `include-lint` job log shows the live report (`count: 1 (budget: 10, PRD §11)`, `OK`); `ci-pull.yml` job exercised by PR #1 (run 34521473503, green incl. `include-lint`), squash-merged as `a74b65a` with the post-merge 8-job run 34521722826 green |
| 2026-09-10 | M0-CORE-01 | `6fa1414` | `laige::Result<T,E>`/`laige::Status` (no exceptions, FR-12.1; inline `std::optional` storage, SFINAE-guarded implicit constructors + `success()`/`failure()` factories, `valueIfOk()`/`errorIfError()` null-safe accessors) + central error registry (`errors.h`/`errors.cpp`: 4 pinned codes, 0 reserved, unregistered → `unknown`; pre-rendered NFR-13.3 5-field lines) with the human-readable registry in `docs/api/errors.md`; `result_status` CTest entry (16 cases: construction, propagation, copy/move, grammar per code, pinned values) in the shared `laige-core_tests` executable; local Verify: GCC static/shared/ASan/TSan + fresh Clang trees 7/7 ctest, zero warnings; first push `f96ce7d` failed 7/8 on windows-msvc (C2535: the `Result(T)`/`Result(E)` constructors have identical parameter lists when `T == E`) — fixed in `6fa1414` by taking the failure value by `const E&`; CI: `ci.yml` run 34525402022 on `6fa1414` (8-job matrix) green, Windows job compiles and passes `result_status`, every job under a minute |
| 2026-09-10 | M0-CORE-02 | `0d3ee28` | The one structured logging facade (AGENTS §14, FR-12.2): `laige::log::Logger` Meyers singleton + `LAIGE_LOG_*` macros (gate before argument evaluation — disabled event = one atomic load + branch, no allocation, LOG-003); per-subsystem level table + atomic global minimum; `Field` scalars render locale-free via `to_chars` into a 64-byte stack buffer; rate limiting per (subsystem, event, severity) for Warn/Error/Fatal with `rate_limited` suppressed-count summaries (first event always emitted, pending counts drained at shutdown); `ConsoleSink` (non-owning stream) + `FileSink` (owning, `create()` → `Result`, failure = new `ErrorCode::IoError` 5, LOG-007 console fallback); Fatal = emit + flush + `std::abort()`; crash handlers (POSIX `sigaction` SA_RESETHAND / Windows vectored SEH) with raw-`write` notice + allocation-free `try_lock` flush; idempotent `shutdown()` retires the facade; timestamps = system_clock UTC RFC 3339 (in-code Hinnant civil-from-days); additive M0-CORE-01 extensions `Result::takeValue() &&` + `IoError`; `logging` CTest entry (27 cases incl. zero-alloc proof via a test-only global `operator new` counter, excluded from sanitizer trees per the step's fallback: leak-free runs + timing property); API contract in `docs/api/logging.md`; local Verify: GCC static/shared/ASan/TSan + fresh Clang static/shared trees — `ctest` 8/8 and `ctest -R logging` green in every tree, zero warnings (disabled ≈46 ns/event vs ≈1207 ns/event enabled); CI (observed 2026-09-10 via the GitHub API): `ci-pull.yml` run 34534697621 on `0d3ee28` green — all 5 jobs of the default-Linux lane (linux-gcc g++, linux-clang clang++, linux-asan+UBSan clang++, linux-tsan clang++, include-lint) passed in 50 s, macOS/Windows skipped (label-gated) |
| 2026-09-11 | M0-CORE-03 | `0fd41f3` | SimMath op interface (`sim_math.h`; one op interface, template dispatch with no per-call indirection — ADR 0002) + `fp32_pinned` backend: pinned IEEE float semantics via the new `laige_apply_simmath_policy` (GCC/Clang: `-ffp-contract=off -fno-associative-math`; MSVC: `/fp:precise`) applied to laige-core + test targets; NaN/Inf policy (`isNaN`/`isInf`, no signaling); `math_float` CTest entry (10 SimMath* cases) (board/changelog row reconstructed 2026-09-11 from the step record) |
| 2026-09-11 | M0-CORE-04 | `5c76991` | `laige::fpx16_16` Q16.16 default SimMath backend (ADR 0002): int64 arithmetic, saturating ops (no UB — CPP-004), ties-to-even rounding, defined zero-division (x/0 → ±max, 0/0 → +0), `negate(min) = max`, no implicit scalar constructors and no arithmetic operators (API-008); `length` precision bound documented; `math_fixed` CTest entry (FixedPoint* suite incl. the 4096-tick determinism sequence with FNV-1a known-answer hash `0xF02728762777C581`, identical on g++ 16.2.1 and clang++ 22.1.8); CI fix `7538053` (macOS/Windows: missing `<compare>` include in `fpx16_16.h`) (board/changelog row reconstructed 2026-09-11 from the step record) |
| 2026-09-11 | M0-CORE-05 | `4a572af` | Header-only pools (`pools.h`): `laige::ArenaPool<T>` (contiguous bump arena, per-frame `reset()`, O(1) create) + `laige::Pool<T>` (stable handles = index + generation — CPP-007; LIFO free list; stale-handle destroy → `InvalidArgument`; capacity overrun → `BudgetExhausted`, no silent growth; `PoolStats` accounting: capacity/inUse/peakInUse/totalCreated/bytes); setup-only allocation, O(1) hot paths, single owner thread (CONC-001); `pools` CTest entry (25 cases incl. the stale-handle debug assert proven in a forked SIGABRT child); CI fix `ba4ffb2` (Windows C4324: `alignas(16)` padding in `pools_tests` fatal under `/WX`) (board/changelog row reconstructed 2026-09-11 from the step record) |
| 2026-09-11 | M0-CORE-06 | `a82de8e` | `laige::Prng`: xorshift128+ transcribed from and verified against the reference (all-zero state excluded), splitmix64 seeding (bijection), per-substream derivation (documented composition rule), Lemire unbiased `next_range`, `next_float01` = k·2^-24 exact; full period 2^128 − 1 for every nonzero state proven (characteristic polynomial over GF(2) via Berlekamp–Massey + irreducibility/primitivity checks; portable 128-bit arithmetic — no `__int128`, MSVC-safe); `prng` CTest entry (18 cases incl. the committed period proof and algorithm-sensitive golden KAT); API contract in `docs/api/prng.md` (board/changelog row reconstructed 2026-09-11 from the step record) |
| 2026-09-11 | M0-CORE-07 | `41938b0` | Bounded JSON in laige-core (ADR 0003, no new dependency): `laige::JsonValue` (deep copy, O(1) move, deep equality) + `parseJson` (1 MiB / depth-32 bounds, strict UTF-8, duplicate keys rejected, ±inf for overflow tokens — documented) + `serializeJson` (canonical ASCII; shortest-round-trip numbers; `std::to_chars` avoided for AppleClang 15 compatibility); all failures → `MalformedInput` (3); `laige-fuzz` minimal deterministic runner (Prng-seeded, `--runs`/`--seed`, 19-document corpus) + `json_parse` fuzz target; `config_json` CTest entry (25 cases) + `fuzz_json_parse` instrumented entry (ASan tree); API contract in `docs/api/json.md` (board/changelog row reconstructed 2026-09-11 from the step record) |
| 2026-09-11 | M0-CORE-08 | `810c251` | Budget harness: `laige::Histogram` (fixed-capacity rolling window; O(1) allocation-free `record()`; exact min/mean/p50/p95/p99/max over the stored window via nearest-rank percentiles; allocation-free O(n log n) `stats()`) + `laige::TimeIt` (`steady_clock` ms scope timer) + `loadBudgets`/`budgetCheck` (strict `budgets.json` schema v1 via the bounded JSON parser — ARCH-007; loud `NO_SAMPLES` failure on empty histograms; `target == 0` = hard-zero budget, not "not set"; AGENTS §12 report: stable 4-line text, before/after pair, caller context; `formatStatsLine` the single source of the stats text) + repo-root `budgets.json` (all 15 PRD §8.1 entries; `measured: 0` = not yet measured) + `laige-bench` tool (canonical command per `building.md`: `--suite=synthetic` deterministic 4096-step LCG+double stand-in workload, `--runs`/`--warmup`, `--budget=<name>` check, exit code 2 on budget failure, `--report` append); `budget_harness` CTest entry (22 cases) + `laige_bench_smoke` CTest entry; **bug fix (M0-CORE-07)** found by this step's Verify run: `json.cpp` `parseObjectMembers` missing `skipWhitespace` before the member key — object documents with `", "` between members (the hand-formatted `budgets.json`) were rejected; regression test `ConfigJsonValid.ObjectMemberWhitespace` (fails pre-fix); API contract in `docs/api/budget_harness.md`; local Verify: `ctest` 16/16, zero warnings on GCC static/shared/ASan/TSan + Clang trees; MSVC/AppleClang compile proof lands in CI |
| 2026-09-12 | M0-TOOL-01 | `937ac7c` | API manifest (NFR-13.1, PRD §9.4): `laige-api` target + `tools/api/laige-api-scanner` (line-oriented state machine — no regex pass; loud failure on every unsupported construct; doc association from consecutive `//` blocks with `@budget`/`@experimental` tags; `--check FILE` byte compare + symbol-level diff via `laige::parseJson`; exit 0 OK / 1 stale / 2 error) + checked-in `laige-api.json` (version 1, deterministic, no timestamps — byte-identical regeneration is the drift check) + `tests/api` CTest entries (fixture tree with exact manifest bytes, fresh/stale, unsupported-construct failure, real-tree `--check`) + the `api-manifest` CI job (every PR and merge, fails on drift); (board/changelog row retroactively added 2026-09-12 — the step merged as `937ac7c`/PR #10 without updating this board or log) |
| 2026-09-12 | M0-TOOL-02 | `fe4460c` | Determinism checker skeleton (FR-11.5, AGENTS ARCH-010, TEST-004): `laige-detcheck` (`tools/detcheck`) runs a named scenario in two build configurations and compares per-tick hash streams; scenario contract (normative in the tool header, mirrored in `docs/api/detcheck.md`): one stdout line per tick `<tick> <hash>` — 16 lowercase hex hash digits (algorithm NOT part of the contract — lines compare byte-for-byte), tick starts at 0 step 1 no padding, trailing newline optional / trailing `\r` tolerated, stderr ignored, exit 0 — enforced strictly and bounded (65536 ticks, 64 bytes/line; a violation is a loud exit 2, CORE-008); modes: `--scenario=synthetic\|synthetic-perturbed` (built-in 32-body `fpx16_16`+Prng workload, two in-process runs — pure integer arithmetic, bit-exact per ADR 0002; perturbation fixture: +1 unit to body 3's x at tick 7) and `--run-a=<bin> --run-b=<bin> [-- scenario-args...]` (the M1-DET-04 mode; `--` separator keeps scenario args unambiguous); stable report `detcheck scenario=<name> result=OK\|DIVERGED [first_diff_tick=<t>]` + run-a/run-b lines (LOG-001); exit 0 match / 1 divergence / 2 error; POSIX fork/exec + pipe capture, Windows CreateProcessW + PeekNamedPipe (no shell, bounded memory, scenario stderr stays on the CI log); 9 CTest entries (`tests/detcheck`, `ctest -R detcheck`): self-check, built-in perturbation, identical/diverged/malformed/failure/short-stream cross-binary pairs — one fixture source, five compiled variants — each a generated `cmake -P` script asserting exit code + output fragments (tests/api pattern); CI `detcheck` job in ci-pull.yml/ci.yml (every PR and merge, no ci:* condition — tooling check, not a P0 OS build): runs the synthetic self-check and SKIPS the real-scenario step (two build configurations of M1-SAMPLE-01's hello) until it lands — M1-DET-04 activates it; local Verify: ctest 31/31, zero warnings on g++ static/shared, clang++, ASan+UBSan, TSan trees; Windows path compile-verified by the CI MSVC job — but NOT runtime-verified (stale as of 2026-09-13: the mode-2 capture was broken on the Windows CI runner and the mode-2 fragment assertions were dead, so the Windows failures were silent; both were found and fixed inside M0-TEST-01 / PR #13 — see the 2026-09-13 row); CI (observed 2026-09-12 via the GitHub API): `ci-pull.yml` run 34697638239 on `82c548d` green - the default-Linux lane's four P0/sanitizer jobs (linux-gcc, linux-clang, linux-asan+UBSan, linux-tsan) plus all three tooling jobs (include-lint, api-manifest, detcheck) passed; Windows/macOS jobs label-skipped as expected; the new `detcheck` job's real-scenario step correctly reported `skipped: no real scenario yet (M1-SAMPLE-01)` |
| 2026-09-12 | M0-TEST-01 | `a292aef` | Test infrastructure conventions (docs/testing.md, source of truth, linked from docs/README.md, tests/README.md, building.md, README.md): tests/support/laige_test_seed.h (TestSeed()/TestPrng() — fixed default 0x1F055EED, identical to laige-fuzz's kDefaultSeed, one documented default seed repo-wide; LAIGE_TEST_SEED env override, 0x-hex/decimal, read at call time; set-but-unparseable value records a loud test failure and falls back to the default — CORE-008; per-test substream ids so streams never share position) + tests/testing (test_infra_tests, CTest entry `test_infra`, SeededRandom suite, 6 cases: 65536-draw FNV-1a KATs under the default seed 0x7EA4049545656830 and override seed 0x535D2CA741B61CBF, first-8-draw KAT, default/fuzz seed identity, loud invalid-env path via EXPECT_NONFATAL_FAILURE (gtest-spi.h), seed parser, substream isolation; machine-greppable `test-seed-check` line per KAT before asserting — the byte-identical-across-two-CI-runs identity check, M0-TEST-01 Verify) + first regress_ test (M0-CORE-08's ConfigJsonValid.ObjectMemberWhitespace renamed regress_json_object_member_ws; suite unchanged so `ctest -R config_json` still covers it; historical M0-CORE-08 record unchanged) + fuzz lane semantics (no new CI job — bounded --runs=1000 is the existing fuzz_json_parse ctest entry inside every P0 job's ctest, PRD §14 "every commit (bounded)"; nightly long form --runs=1000000 documented in docs/testing.md §3 + canonical command table; scheduled nightly lane lands with the first M1 fuzz target); CI workflow headers note the fuzz lane; local Verify: 32/32 ctest, zero warnings under NFR-8.10, on g++ static/shared, ASan+UBSan, TSan, and Clang trees; seeded KAT line identical on g++ and clang++ locally; cross-CI-run Verify: byte-identical `test-seed-check` lines in the archived Linux ASan `LastTest.log` across runs 34713354925/34714039135 (same-commit pair) and again across commits e2abce5→c8b8221 (runs 34728782624/34746755055); Windows CI fix chain landed inside this PR (M0-TOOL-02's Windows path, provenance per the established pattern — see the corrected M0-TOOL-02 row): MSVC portability (NOMINMAX, `getenv_s`/`fopen_s` C4996, C2664/C2440/C4457/C2660), `WaitForSingleObject` before `GetExitCodeProcess` (STILL_ACTIVE), then the root cause — the CI Windows runner (windows-2022) never delivers handles the process creates itself (pipes or files, INHERIT bit confirmed set, even duplicated) to children through `STARTUPINFO`; only parent-inherited (kernel-assigned) handles are delivered (measured runs 34728950395/34729337292) — and NULL `STARTUPINFO` is rejected by its CreateProcess machinery (run 34732057356); fixed by two-stage marker capture on all platforms (phase 1 `--run-a/--run-b` spawns with plain stdout inheritance + `@@DETCHK-RUN-A/B-BEGIN/END@@` markers; phase 2 `--compare-combined` splits/validates/compares) plus the zeroed-STARTUPINFO spawn; dead fragment assertions fixed in `7928379` (`@CHECKS@` variable-name misspelling + CMake single-backslash stripping → double-escaped regexes); final CI run 34746755055 (c8b8221): all 10 jobs green, Windows 32/32 |
| 2026-09-13 | M0-DOC-01 | `972090d` | Docs only: full AGENTS §13 `docs/` tree — `docs/README.md` rewritten as the single index (every section linked + honest "not yet written" list, DOC-001); new section indexes `concepts/` (planned docs + interim homes incl. the ARCH-008 coordinates topic), `guides/`, `debugging/` (usable today; AGENTS §15 debug-mode status), `compatibility/` (P0 platform/compiler table per PRD §6 + CI, current formats `budgets.json`/`laige-api.json`/`deps.lock`, no migration guides yet); `docs/benchmarks/` with `methodology.md` (normative AGENTS §12 report fields, `budgets.json` field→report mapping, immutable baseline-file convention, PRD §8.1 regression policy: >10% band, hard-zero budgets, loud `NO_SAMPLES`) and empty `baselines/` (first baseline `m0-synthetic.md` lands with M0-EXIT-01); `decisions/` index updated (stale M0-DOC-01 note replaced; ADRs 0001–0004 indexed); stale references fixed in the same change (root README docs pointer + ADR 0004; old index's non-existent "M0-DOC-02" reference dropped); dead-link check over all 49 repo `*.md` files (104 internal links, 0 dead — manual per the step's Verify clause; docs-only scope, no CI job added); `ctest` 32/32 green on the existing `build` tree |


---

## 6. Global invariants that apply to *every* step

These come from AGENTS.md / PRD and are restated here so no step can be done
"cheaper" by forgetting them:

- **Determinism scope is always stated** (ARCH-010); deterministic-mode code uses
  engine math ops only (PRD §10.3, S-7).
- **Zero steady-state allocation in sim/render hot paths** (PRD §8.1, G-R1, PERF-003);
  enforced by the M1-ALLOC-01 assertion once it exists, then asserted in every
  later hot-path step's Verify.
- **No exceptions / no RTTI / no `dynamic_cast`** in engine core or public API (FR-12.1,
  NFR-8.10). Errors are `Result<T,E>` / `Status` with the §9.4 error grammar.
- **Every new runtime subsystem registers its essential counters and one inspection
  view** before its step is checked (DBG-008 / FR-11.1).
- **Docs, tests, benchmarks ship in the same change as the code** (CORE-006, DOC-007).
- **New third-party code requires a PRD-revision entry in the decision register**
  first (PRD §11). Vendoring an already-listed dependency is fine (DEP steps say so).
- **Budgets are measured, never assumed** (CORE-001): any step that claims a budget
  number must run the harness and record the result in `docs/benchmarks/`.
