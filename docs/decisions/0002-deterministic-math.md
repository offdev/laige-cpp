# ADR 0002 — Deterministic math strategy (SimMath)

- **Status:** Accepted
- **Date:** 2026-09-10
- **Decider:** Project owner (roadmap step M0-DEC-02)
- **Refs:** PRD §10.3, §12.3, FR-1.4, FR-1.5, FR-3.3, NFR-8.3, AC-10.3;
  AGENTS.md ARCH-010, CORE-004, PERF-006; roadmap S-7, G-R8

## Context

NFR-8.3 / FR-1.4 require deterministic-mode simulation state to be
**bit-identical across all P0 platforms and compilers, verified in CI every
merge**. P0 spans **two ISAs** (x86-64: Windows/Linux/Intel macOS; arm64:
Apple Silicon/macOS/Linux) and three compiler families (MSVC, g++, clang++).
Lockstep (AC-10.3) and authoritative MMO simulation (PRD §12) require
bit-identity across arbitrary client/server hardware. ARCH-010 requires the
determinism scope to be stated explicitly.

Why the two candidate strategies differ:

- **Pinned IEEE float** is bit-identical between x86-64 SSE2 and arm64 NEON
  *only if every compiler emits the same operation sequence*. FMA contraction
  (`a*b+c` fusing into one operation with a single rounding), vectorized
  reassociation of sums, and per-compiler default flags silently break
  bit-identity. "Pinned float" is therefore a guarantee that must be
  *defended forever*: pinned flags (no FMA in sim translation units,
  `-ffp-contract=off` or equivalent), a ban on floating-point intrinsics in
  sim code, and re-audits at every toolchain upgrade. PRD §10.3's own wording
  ("x86-64 `float` semantics pinned") does not honestly cover the arm64 P0
  platforms.
- **Q16.16 fixed-point** integer arithmetic is bit-identical on every
  ISA/compiler *by language standard* (C++20 two's complement; no fused
  integer operations). Determinism is guaranteed by construction, not by flag
  archaeology — the established approach of lockstep engines. Costs:
  multiply/divide run on 64-bit intermediates (~2× float cost in tight loops,
  budgeted and measured in M1) and the range/precision of ±32768 units at
  1/65536 resolution — ample for tile-based 2D worlds (~65k sub-tile steps,
  ±16k tile world bounds). Bonus for the MMO goal: the PRD §12.3 bandwidth
  degradation ladder ("coarser precision — fixed-point bit-count reduction")
  falls out naturally: fewer fraction bits can be emitted to distant clients.

## Decision

Deterministic simulation code uses **one math interface, SimMath** (S-7 /
G-R8: engine math ops are the only math allowed in deterministic systems).
SimMath has **two backends**, selected **once per engine/zone init** from game
config (`determinism.math`):

| Backend | Type | Determinism scope (ARCH-010) | Used for |
|---|---|---|---|
| `fpx16_16` | Q16.16 in `int32_t` storage, `int64_t` intermediates | Bit-exact across build, platform, ISA, and compiler (guaranteed by the language standard) | **Default** deterministic backend; **required** for lockstep and authoritative MMO |
| `fp32_pinned` | IEEE `float` with pinned semantics (documented per-compiler flag set: no FMA in sim translation units, `-ffp-contract=off`/equivalent, no reassociation, no floating-point intrinsics) | Bit-exact across runs of the same build on the same platform/ISA; cross-ISA **not promised** until the CI detcheck matrix proves it — any desyncing pair is declared unsupported for this backend | Opt-in for single-player / non-lockstep games that want IEEE float semantics |

Mechanics:

- **One op surface** (add/sub/mul/div, compare, clamp, lerp, normalize,
  length, …), **two implementations**. Game and system code is written once
  against the interface — there is no per-game double code path.
- Backend selection is **compile-time dispatch** (one template instantiation
  per backend, factory-selected at init) — no per-call indirection in the
  hot loop (PERF-006).
- The **backend id is part of replay identity**: replay = inputs + seed + math
  backend + config hash. Cross-backend replays are not bit-exact and are not
  supported (documented).
- The PRD §12.3 bandwidth degradation ladder (fixed-point bit-count
  reduction) is defined on the `fpx16_16` backend; `fp32_pinned` zones do not
  participate in the ladder (they are single-ISA by definition).
- **Both backends must meet the same sim budget** (PRD §8.1: 10k entities
  @ 60 Hz ≤ 3 ms); measured in M1 (CORE-001).
- Determinism test matrix (M1-DET): **both backends × the CI build matrix**;
  `fp32_pinned` additionally carries a per-platform support list generated
  from detcheck results.

## Alternatives considered

- **Q16.16 only** (the roadmap register default) — strongest determinism
  story with one code path. Rejected in favor of flexibility: single-player
  games keep access to IEEE float semantics, and the fixed-point
  range/precision ceiling (±32768 units, 1/65536 resolution) is avoided for
  large or fine-grained worlds.
- **Pinned float only** — faster with finer precision, but cross-ISA
  determinism becomes a flag-pinning promise that requires re-audits at every
  toolchain upgrade and does not honestly cover arm64 P0 platforms.
- **SimMath (chosen)** — costs two backends to implement, budget, and
  detcheck. Mitigations: shared op surface, shared test matrix, config gate,
  trait enforcement (G-R8). This ADR is the explicit record of the extra
  backend against CORE-004 (smallest complete change) — a deliberate,
  owner-approved flexibility trade.

## Evidence

- IEEE-754/FMA semantics analysis (Context above); C++20 two's-complement
  integer identity across ISAs.
- PRD §10.3 ("fixed-point is the default for lockstep/MMO"), §12.3
  (degradation ladder), AC-10.3 (lockstep bit-determinism), NFR-8.3
  (cross-platform bit-identity verified in CI).
- No performance measurement yet: both backends' budgets are established in
  M1 (this is a design decision, not a measured performance claim).

## Consequences

- **M0-CORE-03** implements the SimMath interface + the `fp32_pinned` backend
  and the pinned flag set; **M0-CORE-04** implements `fpx16_16` as the default
  backend.
- **M1-DET** adds SimMath trait enforcement (G-R8), the detcheck matrix for
  both backends, and the per-backend platform support lists.
- Config surface: `determinism.math` (`"fixed_point_16_16" |
  "float_pinned_32"`), P0 via FR-1.5.
- The deterministic test matrix doubles; both backends are subject to the
  §8.1 budget gate.

## Review conditions

- If `fp32_pinned` fails any cross-ISA detcheck pair, that pair is declared
  unsupported for it (automated by CI). If it misses the §8.1 sim budget on
  mid-range P0 hardware, it is dropped (PRD revision).
- If `fpx16_16` misses the §8.1 sim budget, revisit precision (e.g. Q12.20)
  or the default choice (PRD revision).
- Revisit if a third backend (e.g. Q8.24 for larger worlds) is requested —
  SimMath is designed so that adding one is an additive change.
