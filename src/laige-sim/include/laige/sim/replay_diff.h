// laige-sim replay diff (M1-DET-05).
//
// FR-11.3 (replay: diff two replays by frame/state), FR-1.4
// (deterministic replays must be replayable and diffable), PRD
// Appendix A (replay = the input log + the seed), ADR 0002 (the
// replay identity — the two logs a diff compares may legitimately
// differ in seed and, from M3 on, in the recorded input frames),
// CORE-008 (a divergence is always reported — never silent),
// CORE-005 (the report bounds below are named constants),
// PERF-002/003 (cold path: a tool, never the engine's per-tick hot
// path).
//
// This header carries the FULL diff contract:
//
//   StateDiffItem       One (entity, component) state difference
//                       between two worlds (declared in entity.h, the
//                       World home — see there for the field table).
//   ReplayDiffResult    The result of one diff: the first divergent
//                       tick, the divergence kind, and the bounded
//                       state-difference report.
//   diffReplays         The diff driver: identity-check both logs,
//                       replay them in lock step on two worlds, align
//                       by tick, and report the first divergence plus
//                       the bounded state diff at that tick.
//
// The World-side state access the driver builds on:
//
//   World::componentStateHash(tick)  the component-state part of the
//                                    state hash (entity.h steps 1-4:
//                                    tick, live handles, archetype
//                                    assignment, component bytes — the
//                                    PRNG substream state EXCLUDED);
//                                    the tick-alignment key.
//   World::stateDiff(other, max, fn) the bounded per-(entity,
//                                    component) comparison in
//                                    canonical order (entity.h).
//
// ---------------------------------------------------------------------------
// Why the tick alignment uses componentStateHash, not stateHash
// ---------------------------------------------------------------------------
//
// Two replays being diffed typically differ in their master seed and,
// from M3 on, in their recorded input frames. The per-system PRNG
// substream state (stateHash step 5) is a pure function of (master
// seed, draws so far) — so two different-seed replays diverge in the
// substream state from tick 0 WITHOUT any component-state difference.
// Aligning the two replays on the FULL stateHash would therefore
// report tick 0 for every different-seed diff and hide the first
// real component divergence — the FR-11.3 use case (the editor replay
// viewer, M5-ED-15, asks "where did the two runs' worlds first
// differ?"). The diff aligns on World::componentStateHash instead,
// and reports the full-state divergence HONESTLY as a separate flag
// (ReplayDiffResult::fullStateDivergent — CORE-008: "the component
// state is identical" is never reported for two replays whose
// authoritative state diverged in the draw position).
//
// Scope statement (ARCH-010 style): the comparison is over the
// entity/component part of the authoritative state — the tick, the
// live entity handles, the archetype assignment, and every live
// component's bytes (stateHash steps 1-4; entity.h scope). The PRNG
// substream state is the one deliberately excluded piece; everything
// else stateHash excludes (dead-slot generations, the free list,
// empty archetypes, presentation, timing, guardrails) is excluded
// here too.
//
// ---------------------------------------------------------------------------
// diffReplays contract
// ---------------------------------------------------------------------------
//
//   diffReplays(logA, logB, worldA, worldB, configA, configB,
//               maxEntries = kReplayDiffDefaultEntries)
//
// The two worlds must be FRESH (initial state, nothing run yet) and
// registered IDENTICALLY (the caller's responsibility — the same
// registration order the recording runs used, ADR 0002/ARCH-010);
// configX is the EngineConfig of log X's recording run. A log is
// REPLAYED UNDER ITS OWN CONFIG: the caller derives configX from the
// base config by copying log X's header seed into configX.seed (the
// other config fields stay as passed) — the seed is the one identity
// field the diff legitimately varies, and configX must pass the full
// replay-identity check against (worldX, logX) below.
//
// Steps (first failure wins):
//
//   1. Identity check, per log: replayIdentityDiff(logX, worldX,
//      configX) must be empty (the M1-DET-03 check, replay.h). A
//      mismatch is a REJECTED DIFF — ErrorCode::InvalidArgument + one
//      structured warn (replay/diff_identity_mismatch, naming the log
//      "A"/"B" and exactly which fields differ). BOTH logs are
//      checked before the error is returned (the report names every
//      failing log, not just the first). The two logs may differ in
//      seed and configHash (the diff inputs) but must share the
//      component schema, tick rate, and math backend — both matching
//      their (world, config) values enforces that.
//   2. Determinism check, per config: configX.determinism.enabled
//      must be true (a log recorded with determinism disabled is not
//      replayable — determinism.h). Failure: ErrorCode::InvalidArgument
//      + one structured warn (replay/diff_determinism_disabled,
//      naming the log).
//   3. Schedules: worldX.scheduleSystems once, before the loop (a
//      failure returns the world's Status — already logged by the
//      world; no partial result).
//   4. Tick walk, in lock step:
//        - tick 0 (the initial state): compare componentStateHash(0)
//          on both worlds;
//        - for each completed tick t = 1 .. min(framesA, framesB):
//          one beginFrame() + one runSystems(schedule) on EACH world
//          (the M1-DET-03 frame discipline; the recorded frame bytes
//          are opaque in M1 — no input system consumes them yet,
//          M3-INPUT-03 — and are accepted and ignored), then compare
//          componentStateHash(t).
//        - the FIRST tick whose component-state hash differs is the
//          divergence: the walk stops there and both worlds are left
//          at that tick's state (the caller may inspect them — the
//          stateDiff report below is taken at exactly that state).
//        - every COMPARED tick (0 .. firstDivergentTick, or all
//          aligned ticks when none differ) also has its FULL
//          stateHash compared: fullStateDivergent records whether any
//          of them differed (the honest PRNG-draw-position report).
//   5. State diff at the divergence (hash divergence only — not for
//      a length divergence, where no shared state exists at the
//      reported tick): worldA.stateDiff(worldB, maxEntries, ...)
//      collects the bounded report (see below).
//   6. Result (below).
//
// A failed tick on either world returns that world's Status (the
// world already logged the failed system — CORE-008; no partial
// result). The worlds are NOT wound back: after a failure they hold
// the state of the last successful tick.
//
// Result (ReplayDiffResult):
//
//   identical            true iff every aligned tick's component-
//                        state hash matched AND the frame counts are
//                        equal (the replays' component states are
//                        identical end to end; firstDivergentTick is
//                        0, a meaningless placeholder in that case)
//   firstDivergentTick   the first tick whose component-state hash
//                        differs (0 = the initial state already
//                        differs); when the frame counts differ but
//                        every shared tick matched, this is
//                        min(framesA, framesB) + 1 — the first tick
//                        only one replay has
//   lengthDivergence     true in the case above (the replays' frame
//                        counts differ; the component states match
//                        over the shared ticks — the reported tick
//                        has no state in the shorter replay, so
//                        differingItems/entries are 0/empty)
//   fullStateDivergent   true when any COMPARED tick's full stateHash
//                        (stateHash steps 1-5, including the PRNG
//                        substream state) differed — the two replays
//                        use different seeds (or, from M3 on,
//                        different inputs) even where the component
//                        state is identical
//   framesA / framesB    the two logs' frame counts (replayed ticks)
//   differingItems       the TOTAL number of (entity, component)
//                        differences at firstDivergentTick (0 for
//                        identical/lengthDivergence); may exceed
//                        entries.size()
//   entries              the first maxEntries differences (canonical
//                        order: slots ascending, presence items
//                        before component items, components ascending
//                        — World::stateDiff); each StateDiffItem's
//                        bytesA/bytesB point into worldA/worldB's
//                        component columns and stay valid until the
//                        next mutation of the involved entities
//                        (non-owning views, PERF-005)
//
// maxEntries: 0 = COUNT ONLY (differingItems is still exact; entries
// is empty); 1 .. kMaxReplayDiffEntries = the report bound; a value
// above kMaxReplayDiffEntries is CLAMPED to it (a report beyond 64
// items is a full dump — the bounded-report scope). The default
// kReplayDiffDefaultEntries (16) is the CLI's default.
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003; cold path)
// ---------------------------------------------------------------------------
//
// One diff costs: two scheduleSystems (setup), then per aligned tick
// two beginFrame() + two runSystems (the sim work — the diff replays
// the sim, it does not add a second engine) plus two componentStateHash
// and two stateHash (each O(capacity + live bytes +
// kMaxArchetypes²), allocation-free), then one stateDiff at the
// divergence (O(capacity × components), bounded). The only allocation
// is the result's entries vector (≤ maxEntries items). Never on the
// engine's per-tick hot path; the laige-replay --diff CLI is the M1
// consumer (the editor replay viewer, M5-ED-15, is the next one).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - Pass FRESH, identically-registered worlds. A world that already
//     ran ticks (or registered differently) makes the diff's
//     component-state comparison meaningless — the replay-identity
//     check catches a registration mismatch (the component schema
//     hash), but not a pre-run world.
//   - Derive configX's seed from log X's header (above). A config
//     whose seed is not the log's seed fails the identity check's
//     configHash field — the actionable report names it.
//   - The entries' byte pointers are views: format or copy them out
//     before mutating either world.
//   - A log recorded with game-scenario registrations cannot be
//     diffed by a tool that only registers the engine built-ins
//     (the laige-replay binary's scope — the same constraint as
//     M1-DET-03's --log mode): the component schema hash mismatch is
//     a rejected diff. Scenario logs are diffed from the scenario's
//     own binary, which registers the scenario's components.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "laige/errors.h"
#include "laige/result.h"
#include "laige/sim/entity.h"    // World, World::StateDiffItem
#include "laige/sim/replay.h"    // ReplayLog, ReplayIdentity, replayIdentityDiff

namespace laige {

struct EngineConfig;  // declared in laige/sim/engine.h; only const
                     // references are used (configX.determinism.enabled)

// -----------------------------------------------------------------------
// Report bounds (CORE-005)
// -----------------------------------------------------------------------

// The default bounded-report size (differences reported in
// ReplayDiffResult::entries when maxEntries is left at its default).
// 16 items names the divergence without dumping a 10k-entity world.
inline constexpr std::uint32_t kReplayDiffDefaultEntries = 16;

// The hard cap on the bounded report: a diff report beyond 64
// differing (entity, component) pairs is a full dump, not a report —
// diffReplays clamps maxEntries to this, and the laige-replay CLI
// rejects --entries values above it at the argument boundary.
inline constexpr std::uint32_t kMaxReplayDiffEntries = 64;

// -----------------------------------------------------------------------
// The diff result
// -----------------------------------------------------------------------

// The result of one diffReplays call (field table in the header
// preamble "diffReplays contract"). A plain value; the entries' byte
// pointers are non-owning views into the two worlds' component
// columns (World::StateDiffItem — valid until the next mutation of
// the involved entities' components).
struct ReplayDiffResult {
  // True iff every aligned tick's component-state hash matched and
  // the frame counts are equal (firstDivergentTick is 0 in that
  // case — a meaningless placeholder).
  bool identical{};
  // The first tick whose component-state hash differs (0 = the
  // initial state); min(framesA, framesB) + 1 when lengthDivergence
  // (the first tick only one replay has).
  std::uint64_t firstDivergentTick{};
  // True when the frame counts differ and every shared tick matched
  // (the reported tick has no state in the shorter replay —
  // differingItems/entries are 0/empty in that case).
  bool lengthDivergence{};
  // True when any compared tick's FULL state hash (stateHash steps
  // 1-5, including the PRNG substream state) differed — the honest
  // report for different-seed (or, from M3 on, different-input)
  // replays whose component state is identical.
  bool fullStateDivergent{};
  // The two logs' frame counts (the replayed completed-tick counts).
  std::uint64_t framesA{};
  std::uint64_t framesB{};
  // The TOTAL number of (entity, component) differences at
  // firstDivergentTick (0 for identical/lengthDivergence); may
  // exceed entries.size() (the bounded report).
  std::uint32_t differingItems{};
  // The first maxEntries differences, canonical order
  // (World::stateDiff). Empty when maxEntries is 0 or nothing
  // differed.
  std::vector<World::StateDiffItem> entries;
};

// Replay log A on world A (config A) and log B on world B (config B)
// in lock step, aligned by tick; report the first divergent tick and
// the bounded state diff at that tick (full contract, error table,
// result fields, and performance notes in the header preamble above).
//
//   both logs' identity vs (worldX, configX)  -> InvalidArgument + one
//                                              structured warn per
//                                              failing log (replay/
//                                              diff_identity_mismatch)
//   either config determinism disabled       -> InvalidArgument + one
//                                              structured warn (replay/
//                                              diff_determinism_disabled)
//   a scheduleSystems failure                -> the world's Status
//   a tick's runSystems failure              -> the world's Status
//   success                                  -> ReplayDiffResult
//
// Cold path (a tool, never the engine's per-tick hot path); see the
// header preamble "Performance".
// @budget O(aligned ticks × (per-tick system work + 2 × state hash) +
// the divergence state diff); one allocation (the entries vector,
// bounded by maxEntries).
[[nodiscard]] Result<ReplayDiffResult, ErrorCode>
diffReplays(const ReplayLog& logA, const ReplayLog& logB,
            World& worldA, World& worldB,
            const EngineConfig& configA, const EngineConfig& configB,
            std::uint32_t maxEntries = kReplayDiffDefaultEntries) noexcept;

}  // namespace laige
