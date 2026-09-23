// laige-sim frame graph / budget report implementation (M1-PROF-02;
// PRD FR-11.2, §9.1 S-6, §9.3 G-R5).
//
// Implementation of the API declared in
// include/laige/sim/frame_budget.h — see that header (the declared
// budgets, the per-frame record, the report layout and pass/flag
// semantics) and docs/api/frame_budget.md for the full API contract.
//
// Hot-path cost: recordFrame is one ring write plus the engine's
// per-frame counter reads (engine.h — no allocation, no logging).
// Everything here is cold path: at() / reset() are O(1)/O(window)
// reads, and buildFrameBudgetReport formats the report (allocates —
// reporting is never a hot path, the profiler.cpp / budget_harness.cpp
// precedent).

#include "laige/sim/frame_budget.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "laige/budget_harness.h"  // budgetCheck, formatStatsLine
#include "laige/fpx16_16.h"        // fpx16_16::toFloat (the declared budgets)

namespace laige {

namespace {

// The three declared budgets.json entries the frame report evaluates
// (PRD §8.1 hard budgets — the stable names, LOG-001).
inline constexpr const char* kSimTickAvgBudget = "sim_tick_avg";
inline constexpr const char* kSimTickP99Budget = "sim_tick_p99";
inline constexpr const char* kSimHeapAllocsBudget = "sim_heap_allocs";

// Locale-free rendering of a double for the report: at most 6
// significant digits (%.6g, "C" locale); NaN/inf render as plain
// "nan"/"inf" text so the report stays greppable (the
// budget_harness.cpp formatDouble precedent).
void formatDouble(std::string& out, double value) {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: report rendering of measured time (M1-PROF-02, ARCH-009)
  char buf[32];
  if (std::isnan(value)) {
    std::snprintf(buf, sizeof(buf), "nan");
  } else if (std::isinf(value)) {
    std::snprintf(buf, sizeof(buf), value > 0.0 ? "inf" : "-inf");  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic sign test (M1-PROF-02, ARCH-009)
  } else {
    std::snprintf(buf, sizeof(buf), "%.6g", value);
  }
  out += buf;
}

// One string field (name + text + one separating space — the AGENTS
// §12 context line; the numeric fields use std::to_string inline).
void appendField(std::string& out, const char* name, std::string_view value) {
  out += name;
  out += value;
  out += ' ';
}

// The greppable stats line of one window: the formatStatsLine fields
// when non-empty (its stable `stats: ` prefix stripped — the report
// line carries its own context), the NaN-free "n=0" form when empty
// (the report must never emit NaN text into a machine-readable line —
// the profiler.cpp windowLine precedent, LOG-001).
std::string windowLine(const HistogramStats& s) {
  if (s.n == 0) return "n=0";
  const std::string line = formatStatsLine(s);
  static const std::string_view prefix = "stats: ";
  return line.substr(prefix.size());
}

}  // namespace

void FrameBudgetRecorder::recordFrame(FrameBudgetRecord record) noexcept {
  ring_[cursor_] = record;
  cursor_ = (cursor_ + 1) % kFrameBudgetWindow;
  if (count_ < kFrameBudgetWindow) ++count_;
  ++total_;
}

void FrameBudgetRecorder::reset() noexcept {
  for (FrameBudgetRecord& record : ring_) record = FrameBudgetRecord{};
  cursor_ = 0;
  count_ = 0;
  total_ = 0;
}

std::uint64_t FrameBudgetRecorder::totalFrames() const noexcept {
  return total_;
}

std::uint32_t FrameBudgetRecorder::count() const noexcept { return count_; }

const FrameBudgetRecord& FrameBudgetRecorder::at(std::uint32_t i) const noexcept {
  // The caller reads 0..count() (the header contract).
  assert(i < count_);
  // Oldest-first head (the header's private-storage note).
  const std::uint64_t head = (total_ - count_) % kFrameBudgetWindow;
  return ring_[(head + i) % kFrameBudgetWindow];
}

FrameBudgetReport buildFrameBudgetReport(
    const FrameBudgetRecorder& recorder, const Profiler& profiler,
    const World& world, const BudgetTable& budgets,
    const FrameBudgetReportOptions& options) {
  FrameBudgetReport out;
  std::string r;

  // Header (the stable version marker, LOG-001).
  r += "laige-budget-report version=1\n";

  // The AGENTS §12 caller context (the M0-CORE-08 report format).
  r += "laige-budget-report context: ";
  appendField(r, "workload=", options.context.workload);
  appendField(r, "build=", options.context.build);
  appendField(r, "machine=", options.context.machine);
  r += "warmup=";
  r += std::to_string(options.context.warmup);
  r += "\n";

  const std::uint32_t retained = recorder.count();
  const std::uint32_t lastN = (options.lastNFrames == 0)
      ? kFrameBudgetWindow
      : std::min(options.lastNFrames, kFrameBudgetWindow);
  const std::uint32_t shown = std::min(retained, lastN);

  r += "laige-budget-report frames: n=";
  r += std::to_string(shown);
  r += " total=";
  r += std::to_string(recorder.totalFrames());
  r += "\n";

  // Overall pass/flag (every section folds into it; NO_SAMPLES /
  // NO_ENTRY count as failures — a broken harness is loud, not
  // green — CORE-008).
  bool overall = true;

  // The per-frame lines (oldest → newest, the last `shown` frames).
  for (std::uint32_t i = 0; i < shown; ++i) {
    const FrameBudgetRecord& rec = recorder.at(retained - shown + i);
    const bool framePass =
        (rec.simAllocs == 0) && (rec.overrunWarns == 0) &&
        (rec.criticalErrors == 0);
    overall = overall && framePass;
    r += "laige-budget-report frame=";
    r += std::to_string(rec.frame);
    r += " ticks=";
    r += std::to_string(rec.ticks);
    r += " tick_after=";
    r += std::to_string(rec.tickAfter);
    r += " frame_ms=";
    formatDouble(r, rec.frameMs);
    r += " sim_allocs=";
    r += std::to_string(rec.simAllocs);
    r += " overrun_warns=";
    r += std::to_string(rec.overrunWarns);
    r += " critical_errors=";
    r += std::to_string(rec.criticalErrors);
    r += " result=";
    r += framePass ? "PASS" : "FAIL";
    r += "\n";
  }

  // The declared total-tick-time budgets (PRD §8.1 sim_tick_avg /
  // sim_tick_p99) over the Profiler's tick window — the M0-CORE-08
  // budgetCheck, whose AGENTS §12 report block is embedded verbatim.
  for (const char* name : {kSimTickAvgBudget, kSimTickP99Budget}) {
    const BudgetEntry* entry = budgets.find(name);
    if (entry == nullptr) {
      // A missing entry is a configuration error, not an empty
      // workload — loud, never silent (CORE-008).
      r += "budget=";
      r += name;
      r += " result=NO_ENTRY\n";
      overall = false;
      continue;
    }
    const BudgetCheckResult check =
        budgetCheck(*entry, profiler.tickWindow(), options.context);
    overall = overall && check.passed;
    r += check.report;
  }

  // The declared allocation-count budget (PRD §8.1 sim_heap_allocs,
  // target 0 — the hard-zero at-most semantics) over the per-frame
  // sim-alloc deltas of the retained window (a cold, local histogram —
  // reporting is never a hot path). An empty window (no completed
  // frames) is the loud NO_SAMPLES state.
  {
    const BudgetEntry* entry = budgets.find(kSimHeapAllocsBudget);
    if (entry == nullptr) {
      r += "budget=";
      r += kSimHeapAllocsBudget;
      r += " result=NO_ENTRY\n";
      overall = false;
    } else {
      Histogram allocs(Histogram::Options{static_cast<std::size_t>(shown)});
      for (std::uint32_t i = 0; i < shown; ++i) {
        // The per-frame sim-alloc delta as a measured sample.
        allocs.record(static_cast<double>(  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: allocation count rendered as a measured sample (M1-PROF-02, ARCH-009)
            recorder.at(retained - shown + i).simAllocs));
      }
      const BudgetCheckResult check =
          budgetCheck(*entry, allocs, options.context);
      overall = overall && check.passed;
      r += check.report;
    }
  }

  // The per-system section: each system's DECLARED budget
  // (SystemDef::budgetMs, M1-SYS-01) vs its M1-SYS-03 rolling window's
  // p99 (the statistic the G-R5 warn carries), plus the G-R5 event
  // counters (the per-frame deltas fold into the frame lines above).
  std::uint32_t overCount = 0;
  for (std::uint32_t id = 1; id <= world.systemCount(); ++id) {
    const SystemId sid{id};
    const Result<SystemInfo, ErrorCode> infoResult = world.system(sid);
    if (infoResult.isError()) {
      // Unreachable: the id comes from the world's own dense count.
      assert(!infoResult.isError() && "system(id) for a valid dense id");
      continue;
    }
    const SystemInfo& info = infoResult.value();
    const Result<SystemTimingStats, ErrorCode> timingResult =
        world.systemTimingStats(sid);
    assert(timingResult.ok() && "systemTimingStats for a valid dense id");
    const SystemTimingStats& timing = timingResult.value();
    const Histogram* window = world.systemTimingWindow(sid);
    const HistogramStats stats = (window != nullptr) ? window->stats()
                                                     : HistogramStats{};
    // The declared budget (fpx16_16 ms, exact — ADR 0002).
    const double budgetMs =  // LAIGE-DETERM-EXCEPTION: G-R8 declared budget value, not sim math (M1-PROF-02)
        fpx16_16::toFloat(info.def.budgetMs);
    // Measured vs declared (at-most): an empty window (the system
    // never ran) is NO_SAMPLES (loud — the budgetCheck precedent);
    // p99 > budget is a sustained overrun (FAIL).
    const char* result;
    if (stats.n == 0) {
      result = "NO_SAMPLES";
      overall = false;
    } else if (stats.p99 > budgetMs) {
      result = "FAIL";
      overall = false;
      ++overCount;
    } else {
      result = "PASS";
    }
    r += "laige-budget-report system id=";
    r += std::to_string(sid.value);
    r += " name=";
    r += (info.def.name != nullptr ? info.def.name : "");
    r += " budget_ms=";
    formatDouble(r, budgetMs);
    r += " runs=";
    r += std::to_string(timing.runs);
    r += " last_ms=";
    formatDouble(r, timing.lastMs);
    r += " measured_p99_ms=";
    if (stats.n > 0) {
      formatDouble(r, stats.p99);
    } else {
      r += "nan";
    }
    r += " result=";
    r += result;
    r += " warns=";
    r += std::to_string(timing.warns);
    r += " errors=";
    r += std::to_string(timing.errors);
    r += " window: ";
    r += windowLine(stats);
    r += "\n";
  }

  // The over-budget systems list (FR-11.2 "over-budget systems
  // flagged"): one line per FAIL system, ascending id, machine-
  // greppable; `none` when the section is clean.
  if (overCount == 0) {
    r += "laige-budget-report over_budget: none\n";
  } else {
    for (std::uint32_t id = 1; id <= world.systemCount(); ++id) {
      const SystemId sid{id};
      const Result<SystemInfo, ErrorCode> infoResult = world.system(sid);
      if (infoResult.isError()) {
        assert(!infoResult.isError() && "system(id) for a valid dense id");
        continue;
      }
      const SystemInfo& info = infoResult.value();
      const Histogram* window = world.systemTimingWindow(sid);
      if (window == nullptr) continue;
      const HistogramStats stats = window->stats();
      const double budgetMs =  // LAIGE-DETERM-EXCEPTION: G-R8 declared budget value, not sim math (M1-PROF-02)
          fpx16_16::toFloat(info.def.budgetMs);
      if (stats.n > 0 && stats.p99 > budgetMs) {
        r += "laige-budget-report over_budget: id=";
        r += std::to_string(sid.value);
        r += " name=";
        r += (info.def.name != nullptr ? info.def.name : "");
        r += " p99_ms=";
        formatDouble(r, stats.p99);
        r += " budget_ms=";
        formatDouble(r, budgetMs);
        r += "\n";
      }
    }
  }

  r += "laige-budget-report overall=";
  r += overall ? "PASS" : "FAIL";
  r += "\n";

  out.passed = overall;
  out.report = std::move(r);
  return out;
}

}  // namespace laige
