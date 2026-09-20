// laige-sim replay diff (M1-DET-05).
//
// Implementation of diffReplays declared in
// include/laige/sim/replay_diff.h — see that header for the FULL
// contract: why the tick alignment uses World::componentStateHash
// (the PRNG substream state excluded — it is a function of the master
// seed, a replay-identity field the diff legitimately varies) rather
// than World::stateHash, the diff steps, the result field table, the
// report bounds, the performance notes, and the misuse warnings.
//
// This file carries the tick walk: the identity + determinism checks
// (replay.h / determinism.h — the M1-DET-03 precedents), the
// lock-step beginFrame()/runSystems() drive (the runReplay frame
// discipline, replay.cpp), the per-tick component-state alignment
// (World::componentStateHash, state_hash.cpp), the honest full-state
// tracking (World::stateHash), and the bounded state-difference report
// at the first divergence (World::stateDiff, entity.h).

#include "laige/sim/replay_diff.h"  // the contract (this header)

#include <cstddef>
#include <cstdint>
#include <vector>

#include "laige/logging.h"    // the structured replay/diff_* events
#include "laige/sim/engine.h" // EngineConfig (the determinism check)

namespace laige {

namespace {

// Check log X's identity against (worldX, configX); when it mismatches,
// emit the structured warn naming the log and the differing fields and
// return true (the caller rejects the diff — CORE-008: a mismatch is a
// rejected diff, never a silent divergence, ADR 0002).
bool checkDiffIdentity(const char* logName, const ReplayIdentityDiff& diff) noexcept {
  if (diff.empty()) return false;
  LAIGE_LOG_WARN("replay", "diff_identity_mismatch",
                 "replay diff identity mismatch — the log was recorded "
                 "under a different identity; a mismatch is a rejected "
                 "diff, never a silent divergence (ADR 0002)",
                 laige::log::field("log", logName),
                 laige::log::field("seed", diff.seed),
                 laige::log::field("tick_rate", diff.tickRateHz),
                 laige::log::field("component_schema", diff.componentSchemaHash),
                 laige::log::field("math_backend", diff.mathBackendId),
                 laige::log::field("config", diff.configHash));
  return true;
}

// The lock-step tick compare at completed tick `tick`: record whether
// the FULL state hashes differ (the honest PRNG report — the out
// parameter), and return whether the component-state hashes match
// (the alignment key — the return value).
bool compareTick(const World& a, const World& b, std::uint64_t tick,
                 bool& fullStateDivergent) noexcept {
  fullStateDivergent |= (a.stateHash(tick) != b.stateHash(tick));
  return a.componentStateHash(tick) == b.componentStateHash(tick);
}

}  // namespace

Result<ReplayDiffResult, ErrorCode>
diffReplays(const ReplayLog& logA, const ReplayLog& logB,
            World& worldA, World& worldB,
            const EngineConfig& configA, const EngineConfig& configB,
            std::uint32_t maxEntries) noexcept {
  // The bounded report: 0 = count only (no entries), above the hard
  // cap = clamped to it (the bounded-report scope — the preamble).
  const std::uint32_t reportBound =
      (maxEntries == 0) ? 0
                        : (maxEntries > kMaxReplayDiffEntries
                              ? kMaxReplayDiffEntries
                              : maxEntries);

  // Step 1: the identity check, per log — against THAT log's own
  // (world, config) (the M1-DET-03 check, replay.h). BOTH logs are
  // checked before the error is returned, so the report names every
  // failing log, not just the first.
  bool identityFailed = false;
  identityFailed |= checkDiffIdentity("A", replayIdentityDiff(logA, worldA, configA));
  identityFailed |= checkDiffIdentity("B", replayIdentityDiff(logB, worldB, configB));
  if (identityFailed) {
    return Result<ReplayDiffResult, ErrorCode>(ErrorCode::InvalidArgument);
  }

  // Step 2: the determinism check, per config — a log recorded with
  // determinism disabled is not replayable (determinism.h).
  const char* nonDeterministicLog =
      !configA.determinism.enabled ? "A"
      : (!configB.determinism.enabled ? "B" : nullptr);
  if (nonDeterministicLog != nullptr) {
    LAIGE_LOG_WARN("replay", "diff_determinism_disabled",
                   "replay diff rejected — the config's determinism "
                   "mode is disabled; deterministic replays only "
                   "(the master seed and per-system PRNG substreams "
                   "are undefined with it off — determinism.h)",
                   laige::log::field("log", nonDeterministicLog),
                   laige::log::field("seed", nonDeterministicLog[0] == 'A'
                                          ? configA.seed
                                          : configB.seed));
    return Result<ReplayDiffResult, ErrorCode>(ErrorCode::InvalidArgument);
  }

  // Step 3: the schedules, once per world, before the loop (a failure
  // returns the world's Status — already logged by the world; no
  // partial result, CORE-008).
  SystemSchedule scheduleA;
  const Status scheduleStatusA = worldA.scheduleSystems(scheduleA);
  if (!scheduleStatusA.ok()) {
    return Result<ReplayDiffResult, ErrorCode>(scheduleStatusA.error());
  }
  SystemSchedule scheduleB;
  const Status scheduleStatusB = worldB.scheduleSystems(scheduleB);
  if (!scheduleStatusB.ok()) {
    return Result<ReplayDiffResult, ErrorCode>(scheduleStatusB.error());
  }

  const std::uint64_t framesA = logA.frames.size();
  const std::uint64_t framesB = logB.frames.size();
  const std::uint64_t alignedTicks = framesA < framesB ? framesA : framesB;

  ReplayDiffResult result;
  result.framesA = framesA;
  result.framesB = framesB;

  // The divergence state: -1 = none yet, 0 = the initial state, or the
  // first completed tick whose component-state hash differed.
  long long divergedAt = -1;

  // Step 4: the tick walk, in lock step (the runReplay frame
  // discipline, replay.cpp — one beginFrame() + one runSystems per
  // completed tick on each world; the recorded frame bytes are
  // opaque in M1 — no input system consumes them yet, M3-INPUT-03 —
  // and are accepted and ignored).
  if (!compareTick(worldA, worldB, 0, result.fullStateDivergent)) {
    divergedAt = 0;
  }
  for (std::uint64_t tick = 1;
       divergedAt < 0 && tick <= alignedTicks; ++tick) {
    worldA.beginFrame();
    worldB.beginFrame();
    const Status statusA = worldA.runSystems(scheduleA);
    if (!statusA.ok()) {
      // The world already logged the failed system; the diff stops —
      // no partial result (CORE-008). The worlds hold the state of
      // the last successful tick.
      return Result<ReplayDiffResult, ErrorCode>(statusA.error());
    }
    const Status statusB = worldB.runSystems(scheduleB);
    if (!statusB.ok()) {
      return Result<ReplayDiffResult, ErrorCode>(statusB.error());
    }
    if (!compareTick(worldA, worldB, tick, result.fullStateDivergent)) {
      divergedAt = static_cast<long long>(tick);
    }
  }

  if (divergedAt >= 0) {
    // Step 5: the bounded state diff at the first divergence — both
    // worlds sit at that tick's state (the walk stopped there).
    result.firstDivergentTick = static_cast<std::uint64_t>(divergedAt);
    result.entries.reserve(reportBound);
    result.differingItems = worldA.stateDiff(worldB, reportBound,
                                             [&result](const World::StateDiffItem& item) {
                                               result.entries.push_back(item);
                                             });
  } else if (framesA != framesB) {
    // No shared-tick divergence, but the frame counts differ: the
    // first tick only one replay has is alignedTicks + 1 (there is
    // no shared state at that tick — the report is empty).
    result.lengthDivergence = true;
    result.firstDivergentTick = alignedTicks + 1;
  } else {
    result.identical = true;
    result.firstDivergentTick = 0;  // the documented placeholder
  }
  return Result<ReplayDiffResult, ErrorCode>::success(std::move(result));
}

}  // namespace laige
