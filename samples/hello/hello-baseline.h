// hello scenario baseline check (M1-DET-04).
//
// The scenario's committed per-tick hash stream (the reference build —
// the canonical Debug g++; the samples/hello/baselines/ README) is
// compared line-by-line against a run's stdout stream with the
// `laige-replay --expect` contract (tools/replay/laige-replay.cpp):
//
//   - exit 0: every emitted line matches the baseline
//   - exit 1: a per-tick hash mismatch — the first divergence is
//     reported (laige-replay report format, "first divergence")
//   - exit 2: a baseline read / contract error (the loud CORE-008
//     failure before any tick is run)
//
// The scenario's own binary carries the check because a game
// scenario's log cannot be replayed by `laige-replay` (it registers
// only the engine's built-ins — the replay identity, ADR 0002); the
// split is documented in docs/api/replay.md. This file is the CI
// machinery for the M1-DET-04 determinism matrix — it is deliberately
// a separate translation unit so the template game (hello.cpp) stays
// inside the PRD §9.4 line budget.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hello {

// One loaded baseline plus the comparison state of the run that emits
// into it. Cold path (one load, one compare per emitted line); no
// allocation after the load (the compare records first-divergence
// strings only once).
class BaselineCheck {
 public:
  // Load the baseline from `path` (empty path: a plain run, no check —
  // the object stays inactive). On a read or contract failure,
  // errorText() is non-empty and the object is inactive; the caller
  // reports and exits 2. Bounds: the laige-replay --expect bounds
  // (kMaxBaselineBytes read cap, 64 bytes/line, 65536 lines — the
  // detcheck scenario contract).
  explicit BaselineCheck(std::string_view path);

  // Print one `<tick> <hash>` line to stdout (the detcheck scenario
  // contract — 16 lowercase hex digits, tick 0 first, step 1) and,
  // when active, compare it against the baseline's line at the same
  // tick (O(1); the first divergence is recorded, the run continues —
  // the laige-replay --expect semantics).
  void emit(std::uint64_t tick, std::uint64_t hash);

  // The failure report text (laige-replay --expect format) when the
  // run diverged from the baseline; "" when it matched (or no check
  // was active). `emitted` is the number of lines emitted (the tick-0
  // line plus one per completed tick). O(1); no side effects.
  [[nodiscard]] std::string failText(std::uint64_t emitted) const;

  // Non-empty when the baseline failed to load (the caller prints
  // "hello: baseline: <text>" and exits 2). "" when inactive or
  // loaded. No side effects.
  [[nodiscard]] const std::string& errorText() const { return error_; }

 private:
  std::string error_{};              // non-empty: a load/contract failure
  std::vector<std::string> baseline_{};  // the loaded lines (empty: inactive)
  std::uint64_t firstDiff_ = std::uint64_t(-1);  // npos: no divergence yet
  std::string lineA_{};              // the baseline's line at firstDiff_
  std::string lineB_{};              // the run's line at firstDiff_
};

}  // namespace hello
