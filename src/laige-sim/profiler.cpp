// laige-sim profiler core (M1-PROF-01; PRD FR-11.1).
//
// Implementation of the Profiler and the report surface declared in
// include/laige/sim/profiler.h — see that header (the counter model,
// the hot-path cost, the ownership contract) and docs/api/profiler.md
// for the full API contract, including the report schemas.
//
// Hot-path cost: recordTick / recordFrame are one branch plus one
// O(1) ring write each when enabled — no allocation, no logging
// (PERF-003, LOG-003; the M1-SYS-03 window-write precedent). The
// formatting and file-write surface is cold path only (reporting is
// never a hot path — the budget_harness.cpp precedent).

#include "laige/sim/profiler.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for _fsopen
#endif

#include "laige/budget_harness.h"  // formatStatsLine (the stats-line format)
#include "laige/fpx16_16.h"        // fpx16_16::toFloat (the system budgets)
#include "laige/json.h"            // JsonValue + serializeJson (the report)
#include "laige/sim/entity.h"      // World (the cold pull: stats and windows)
#include "laige/sim/system.h"      // SystemInfo, SystemTimingStats, SystemId

namespace laige {

namespace {

// Locale-free rendering of a double for the report: at most 6
// significant digits (%.6g, "C" locale); NaN/inf render as plain
// "nan"/"inf" text so the report stays greppable (the
// budget_harness.cpp formatDouble precedent).
void formatDouble(std::string& out, double value) {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: report rendering of measured time (M1-PROF-01, ARCH-009)
  char buf[32];
  if (std::isnan(value)) {
    std::snprintf(buf, sizeof(buf), "nan");
  } else if (std::isinf(value)) {
    std::snprintf(buf, sizeof(buf), value > 0.0 ? "inf" : "-inf");  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic sign test (M1-PROF-01, ARCH-009)
  } else {
    std::snprintf(buf, sizeof(buf), "%.6g", value);
  }
  out += buf;
}

// The greppable stats line of one window: the formatStatsLine fields
// when non-empty (its stable `stats: ` prefix stripped — the report
// line carries its own context, `tick_ms:` / `frame_ms:` / `window:`),
// the NaN-free "n=0" form when empty (the report must never emit NaN
// text into a machine-readable line — LOG-001).
std::string windowLine(const HistogramStats& s) {
  if (s.n == 0) return "n=0";
  const std::string line = formatStatsLine(s);
  static const std::string_view prefix = "stats: ";
  return line.substr(prefix.size());
}

// One u64 counter as a report field (std::to_string's stable decimal).
void appendCount(std::string& out, const char* name, std::uint64_t value) {
  out += name;
  out += std::to_string(value);
  out += ' ';
}

// Portable file open (CPP-009 platform boundary, the replay.cpp /
// logging.cpp precedent): MSVC's CRT deprecates plain `fopen` (C4996,
// fatal under the engine's /WX policy); the MSVC path uses
// `_fsopen(path, mode, _SH_DENYNO)` — plain-`fopen` sharing semantics
// every other supported compiler provides.
std::FILE* openProfileFile(const std::string& path, const char* mode) {
#if defined(_MSC_VER)
  return ::_fsopen(path.c_str(), mode, _SH_DENYNO);
#else
  return std::fopen(path.c_str(), mode);
#endif
}

// The JSON form of one window's stats (null when empty — the report
// never carries NaN, the serializeJson precondition).
JsonValue statsObject(const HistogramStats& s) {
  if (s.n == 0) return JsonValue();  // Null
  JsonValue o = JsonValue::makeObject();
  o.setMember("n", JsonValue::fromNumber(static_cast<double>(s.n)));  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic report field (M1-PROF-01, ARCH-009)
  o.setMember("min", JsonValue::fromNumber(s.min));
  o.setMember("mean", JsonValue::fromNumber(s.mean));
  o.setMember("p50", JsonValue::fromNumber(s.p50));
  o.setMember("p95", JsonValue::fromNumber(s.p95));
  o.setMember("p99", JsonValue::fromNumber(s.p99));
  o.setMember("max", JsonValue::fromNumber(s.max));
  return o;
}

}  // namespace

Profiler::Profiler(Options options)
    : tickWindow_(Histogram(Histogram::Options{
                      static_cast<std::size_t>(options.tickWindowSamples)})),
      frameWindow_(Histogram(Histogram::Options{
                       static_cast<std::size_t>(options.frameWindowSamples)})),
      enabled_(options.enabled) {}

Profiler::Profiler(Profiler&& other) noexcept
    : tickWindow_(std::move(other.tickWindow_)),
      frameWindow_(std::move(other.frameWindow_)),
      drawCalls_(other.drawCalls_),
      textureBinds_(other.textureBinds_),
      netBytes_(other.netBytes_),
      renderBatchMs_(other.renderBatchMs_),
      renderSortMs_(other.renderSortMs_),
      renderSubmitMs_(other.renderSubmitMs_),
      renderGpuUs_(other.renderGpuUs_),
      renderGpuFrames_(other.renderGpuFrames_),
      renderStallUs_(other.renderStallUs_),
      renderStallFrames_(other.renderStallFrames_),
      renderTextureMemoryBytes_(other.renderTextureMemoryBytes_),
      enabled_(other.enabled_),
      active_(other.active_) {
  other.active_ = false;  // the source becomes a stopped profiler
}

Profiler& Profiler::operator=(Profiler&& other) noexcept {
  if (this != &other) {
    tickWindow_ = std::move(other.tickWindow_);
    frameWindow_ = std::move(other.frameWindow_);
    drawCalls_ = other.drawCalls_;
    textureBinds_ = other.textureBinds_;
    netBytes_ = other.netBytes_;
    renderBatchMs_ = other.renderBatchMs_;
    renderSortMs_ = other.renderSortMs_;
    renderSubmitMs_ = other.renderSubmitMs_;
    renderGpuUs_ = other.renderGpuUs_;
    renderGpuFrames_ = other.renderGpuFrames_;
    renderStallUs_ = other.renderStallUs_;
    renderStallFrames_ = other.renderStallFrames_;
    renderTextureMemoryBytes_ = other.renderTextureMemoryBytes_;
    enabled_ = other.enabled_;
    active_ = other.active_;
    other.active_ = false;  // the source becomes a stopped profiler
  }
  return *this;
}

void Profiler::recordTick(double ms) noexcept {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured tick time never enters sim state, hashes, or replays (M1-PROF-01, ARCH-009)
  if (!active_ || !enabled_) return;  // one branch when off (DBG-004)
  tickWindow_.record(ms);
}

void Profiler::recordFrame(double ms) noexcept {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured frame time never enters sim state, hashes, or replays (M1-PROF-01, ARCH-009)
  if (!active_ || !enabled_) return;
  frameWindow_.record(ms);
}

void Profiler::addDrawCalls(std::uint64_t count) noexcept {
  if (!active_ || !enabled_) return;
  drawCalls_ += count;
}

void Profiler::addTextureBinds(std::uint64_t count) noexcept {
  if (!active_ || !enabled_) return;
  textureBinds_ += count;
}

void Profiler::addNetBytes(std::uint64_t count) noexcept {
  if (!active_ || !enabled_) return;
  netBytes_ += count;
}

void Profiler::addRenderBatchMs(double ms) noexcept {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured batch time never enters sim state, hashes, or replays (M2-PROF-01, ARCH-009)
  if (!active_ || !enabled_) return;
  renderBatchMs_ += ms;
}

void Profiler::addRenderSortMs(double ms) noexcept {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured sort time never enters sim state, hashes, or replays (M2-PROF-01, ARCH-009)
  if (!active_ || !enabled_) return;
  renderSortMs_ += ms;
}

void Profiler::addRenderSubmitMs(double ms) noexcept {  // LAIGE-DETERM-EXCEPTION: G-R8 wall-clock diagnostic: measured submit time never enters sim state, hashes, or replays (M2-PROF-01, ARCH-009)
  if (!active_ || !enabled_) return;
  renderSubmitMs_ += ms;
}

void Profiler::addRenderGpu(std::uint64_t us) noexcept {
  if (!active_ || !enabled_) return;
  renderGpuUs_ += us;
  renderGpuFrames_ += 1;
}

void Profiler::addRenderStall(std::uint64_t us) noexcept {
  if (!active_ || !enabled_) return;
  renderStallUs_ += us;
  renderStallFrames_ += 1;
}

void Profiler::setRenderTextureMemory(std::uint64_t bytes) noexcept {
  if (!active_ || !enabled_) return;
  renderTextureMemoryBytes_ = bytes;  // a gauge: the last value wins
}

HistogramStats Profiler::tickTime() const noexcept {
  return tickWindow_.stats();
}

HistogramStats Profiler::frameTime() const noexcept {
  return frameWindow_.stats();
}

ProfilerStats Profiler::snapshot() const noexcept {
  // A stopped profiler (moved from) reports empty values — the
  // GameLoop moved-out contract.
  if (!active_) return ProfilerStats{};
  ProfilerStats s;
  // The since-construction sample counts: the windows' own totals
  // (they keep counting every recorded sample even after the window
  // rolls — the M0-CORE-08 totalRecorded contract).
  s.ticks = tickWindow_.totalRecorded();
  s.frames = frameWindow_.totalRecorded();
  s.drawCalls = drawCalls_;
  s.textureBinds = textureBinds_;
  s.netBytes = netBytes_;
  s.renderBatchMs = renderBatchMs_;
  s.renderSortMs = renderSortMs_;
  s.renderSubmitMs = renderSubmitMs_;
  s.renderGpuUs = renderGpuUs_;
  s.renderGpuFrames = renderGpuFrames_;
  s.renderStallUs = renderStallUs_;
  s.renderStallFrames = renderStallFrames_;
  s.renderTextureMemoryBytes = renderTextureMemoryBytes_;
  s.tickTimeMs = tickWindow_.stats();
  s.frameTimeMs = frameWindow_.stats();
  return s;
}

ProfilerStats Profiler::snapshot(const World& world) const noexcept {
  ProfilerStats s = snapshot();
  if (!active_) return s;  // stopped: the own counters are already empty
  s.worldAvailable = true;
  const EntityStats e = world.stats();
  s.entitiesAlive = e.inUse;
  s.entitiesTotal = static_cast<std::uint32_t>(e.totalCreated);
  s.entityCapacity = e.capacity;
  // The sim alloc count (the header preamble): the sum of the pool
  // accounting since world construction — the pool-backed sim
  // storage's reserved column blocks (M1-ECS-03).
  s.simAllocs = world.archetypeStats().totalReservations;
  s.systems = world.systemCount();
  return s;
}

const Histogram& Profiler::tickWindow() const noexcept { return tickWindow_; }

bool Profiler::enabled() const noexcept { return enabled_; }

void Profiler::setEnabled(bool on) noexcept { enabled_ = on; }

std::string formatProfileSummaryLine(const ProfilerStats& stats) {
  std::string r = "laige-run profile: ticks=";
  r += std::to_string(stats.ticks);
  r += " frames=";
  r += std::to_string(stats.frames);
  r += " tick_ms: ";
  r += windowLine(stats.tickTimeMs);
  r += " frame_ms: ";
  r += windowLine(stats.frameTimeMs);
  appendCount(r, "entities_alive=", stats.entitiesAlive);
  appendCount(r, "entities_total=", stats.entitiesTotal);
  appendCount(r, "entity_capacity=", stats.entityCapacity);
  appendCount(r, "sim_allocs=", stats.simAllocs);
  appendCount(r, "draw_calls=", stats.drawCalls);
  appendCount(r, "texture_binds=", stats.textureBinds);
  appendCount(r, "net_bytes=", stats.netBytes);
  // The M2-PROF-01 render-timing fields (RENDER-005). The GPU time
  // renders "n/a" when no GPU measurement has completed (the
  // headless/offscreen fallback — never a fake number).
  r += "render_batch_ms=";
  formatDouble(r, stats.renderBatchMs);
  r += " render_sort_ms=";
  formatDouble(r, stats.renderSortMs);
  r += " render_submit_ms=";
  formatDouble(r, stats.renderSubmitMs);
  r += " render_gpu_us=";
  if (stats.renderGpuFrames == 0) {
    r += "n/a";
  } else {
    r += std::to_string(stats.renderGpuUs);
  }
  r += " render_stall_frames=";
  r += std::to_string(stats.renderStallFrames);
  appendCount(r, "render_texture_memory_bytes=", stats.renderTextureMemoryBytes);
  r.pop_back();  // the trailing space after the last field
  return r;
}

std::string formatProfileText(const Profiler& profiler, const World& world) {
  const ProfilerStats stats = profiler.snapshot(world);
  std::string r = "laige-profile version=1\n";
  r += "laige-profile counters: ";
  appendCount(r, "ticks=", stats.ticks);
  appendCount(r, "frames=", stats.frames);
  appendCount(r, "draw_calls=", stats.drawCalls);
  appendCount(r, "texture_binds=", stats.textureBinds);
  appendCount(r, "net_bytes=", stats.netBytes);
  r.pop_back();
  r += "\n";
  r += "laige-profile tick_ms: ";
  r += windowLine(stats.tickTimeMs);
  r += "\n";
  r += "laige-profile frame_ms: ";
  r += windowLine(stats.frameTimeMs);
  r += "\n";
  r += "laige-profile world: ";
  appendCount(r, "entities_alive=", stats.entitiesAlive);
  appendCount(r, "entities_total=", stats.entitiesTotal);
  appendCount(r, "entity_capacity=", stats.entityCapacity);
  appendCount(r, "sim_allocs=", stats.simAllocs);
  appendCount(r, "systems=", stats.systems);
  r.pop_back();
  r += "\n";
  // The M2-PROF-01 render-timing section (RENDER-005: CPU and GPU
  // measured independently). The GPU time renders "n/a" when no GPU
  // measurement has completed (the headless/offscreen fallback).
  r += "laige-profile render: ";
  r += "batch_ms=";
  formatDouble(r, stats.renderBatchMs);
  r += " sort_ms=";
  formatDouble(r, stats.renderSortMs);
  r += " submit_ms=";
  formatDouble(r, stats.renderSubmitMs);
  r += " gpu_us=";
  if (stats.renderGpuFrames == 0) {
    r += "n/a";
  } else {
    r += std::to_string(stats.renderGpuUs);
  }
  r += " gpu_frames=";
  r += std::to_string(stats.renderGpuFrames);
  r += " stall_us=";
  r += std::to_string(stats.renderStallUs);
  r += " stall_frames=";
  r += std::to_string(stats.renderStallFrames);
  r += " texture_memory_bytes=";
  r += std::to_string(stats.renderTextureMemoryBytes);
  r += "\n";
  // One line per registered system (M1-SYS-03 feed): the declared
  // budget, the run scalars, and the rolling window's stats.
  for (std::uint32_t i = 1; i <= stats.systems; ++i) {
    const SystemId id{i};
    const Result<SystemInfo, ErrorCode> infoResult = world.system(id);
    if (infoResult.isError()) {
      // Unreachable: the id comes from the world's own dense count.
      assert(!infoResult.isError() && "system(id) for a valid dense id");
      continue;
    }
    const SystemInfo& info = infoResult.value();
    const Result<SystemTimingStats, ErrorCode> timingResult =
        world.systemTimingStats(id);
    assert(timingResult.ok() && "systemTimingStats for a valid dense id");
    const SystemTimingStats& timing = timingResult.value();
    const Histogram* window = world.systemTimingWindow(id);
    r += "laige-profile system id=";
    r += std::to_string(id.value);
    r += " name=";
    r += (info.def.name != nullptr ? info.def.name : "");
    r += " budget_ms=";
    formatDouble(r, fpx16_16::toFloat(info.def.budgetMs));
    r += " runs=";
    r += std::to_string(timing.runs);
    r += " last_ms=";
    formatDouble(r, timing.lastMs);
    r += " warns=";
    r += std::to_string(timing.warns);
    r += " errors=";
    r += std::to_string(timing.errors);
    r += " window: ";
    r += (window != nullptr) ? windowLine(window->stats()) : std::string("n=0");
    r += "\n";
  }
  return r;
}

std::string formatProfileJson(const Profiler& profiler, const World& world) {
  const ProfilerStats stats = profiler.snapshot(world);
  JsonValue root = JsonValue::makeObject();
  root.setMember("version", JsonValue::fromNumber(1));

  JsonValue counters = JsonValue::makeObject();
  counters.setMember("ticks",
                     JsonValue::fromNumber(static_cast<double>(stats.ticks)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M1-PROF-01, ARCH-009)
  counters.setMember("frames",
                     JsonValue::fromNumber(static_cast<double>(stats.frames)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M1-PROF-01, ARCH-009)
  counters.setMember("draw_calls",
                     JsonValue::fromNumber(static_cast<double>(stats.drawCalls)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M1-PROF-01, ARCH-009)
  counters.setMember("texture_binds",
                     JsonValue::fromNumber(static_cast<double>(stats.textureBinds)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M1-PROF-01, ARCH-009)
  counters.setMember("net_bytes",
                     JsonValue::fromNumber(static_cast<double>(stats.netBytes)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M1-PROF-01, ARCH-009)
  root.setMember("counters", std::move(counters));

  root.setMember("tick_time_ms", statsObject(stats.tickTimeMs));
  root.setMember("frame_time_ms", statsObject(stats.frameTimeMs));

  JsonValue worldObj = JsonValue::makeObject();
  worldObj.setMember("entities_alive",
                     JsonValue::fromNumber(stats.entitiesAlive));
  worldObj.setMember("entities_total",
                     JsonValue::fromNumber(stats.entitiesTotal));
  worldObj.setMember("entity_capacity",
                     JsonValue::fromNumber(stats.entityCapacity));
  worldObj.setMember("sim_allocs",
                     JsonValue::fromNumber(static_cast<double>(stats.simAllocs)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M1-PROF-01, ARCH-009)
  worldObj.setMember("systems", JsonValue::fromNumber(stats.systems));
  root.setMember("world", std::move(worldObj));

  // The M2-PROF-01 render-timing object (RENDER-005). gpu_us is JSON
  // null when no GPU measurement has completed (the headless/offscreen
  // fallback — never a fake number).
  JsonValue renderObj = JsonValue::makeObject();
  renderObj.setMember("batch_ms", JsonValue::fromNumber(stats.renderBatchMs));
  renderObj.setMember("sort_ms", JsonValue::fromNumber(stats.renderSortMs));
  renderObj.setMember("submit_ms", JsonValue::fromNumber(stats.renderSubmitMs));
  if (stats.renderGpuFrames == 0) {
    renderObj.setMember("gpu_us", JsonValue());  // Null = "n/a"
  } else {
    renderObj.setMember("gpu_us",
                        JsonValue::fromNumber(
                            static_cast<double>(stats.renderGpuUs)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 GPU time to JSON number (M2-PROF-01, ARCH-009)
  }
  renderObj.setMember("gpu_frames",
                      JsonValue::fromNumber(
                          static_cast<double>(stats.renderGpuFrames)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M2-PROF-01, ARCH-009)
  renderObj.setMember("stall_us",
                      JsonValue::fromNumber(
                          static_cast<double>(stats.renderStallUs)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 stall wait to JSON number (M2-PROF-01, ARCH-009)
  renderObj.setMember("stall_frames",
                      JsonValue::fromNumber(
                          static_cast<double>(stats.renderStallFrames)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M2-PROF-01, ARCH-009)
  renderObj.setMember("texture_memory_bytes",
                      JsonValue::fromNumber(
                          static_cast<double>(stats.renderTextureMemoryBytes)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 VRAM estimate to JSON number (M2-PROF-01, ARCH-009)
  root.setMember("render", std::move(renderObj));

  JsonValue systems = JsonValue::makeArray();
  for (std::uint32_t i = 1; i <= stats.systems; ++i) {
    const SystemId id{i};
    const Result<SystemInfo, ErrorCode> infoResult = world.system(id);
    if (infoResult.isError()) {
      assert(!infoResult.isError() && "system(id) for a valid dense id");
      continue;
    }
    const SystemInfo& info = infoResult.value();
    const Result<SystemTimingStats, ErrorCode> timingResult =
        world.systemTimingStats(id);
    assert(timingResult.ok() && "systemTimingStats for a valid dense id");
    const SystemTimingStats& timing = timingResult.value();
    const Histogram* window = world.systemTimingWindow(id);

    JsonValue sys = JsonValue::makeObject();
    sys.setMember("id", JsonValue::fromNumber(id.value));
    sys.setMember("name", JsonValue::fromString(info.def.name != nullptr
                                                     ? info.def.name
                                                     : ""));
    sys.setMember("budget_ms",
                  JsonValue::fromNumber(fpx16_16::toFloat(info.def.budgetMs)));
    sys.setMember("runs",
                  JsonValue::fromNumber(static_cast<double>(timing.runs)));  // LAIGE-DETERM-EXCEPTION: G-R8 report field: u64 counter to JSON number (M1-PROF-01, ARCH-009)
    sys.setMember("last_ms", JsonValue::fromNumber(timing.lastMs));
    sys.setMember("warns", JsonValue::fromNumber(timing.warns));
    sys.setMember("errors", JsonValue::fromNumber(timing.errors));
    sys.setMember("window_ms",
                  (window != nullptr) ? statsObject(window->stats())
                                      : JsonValue());
    systems.append(std::move(sys));
  }
  root.setMember("systems", std::move(systems));

  return serializeJson(root);
}

Result<std::uint64_t, ErrorCode> writeProfile(const Profiler& profiler,
                                              const World& world,
                                              std::string_view path,
                                              ProfileFormat format) {
  const std::string pathCopy(path);  // fopen needs a C string
  const std::string text =
      (format == ProfileFormat::Json)
          ? formatProfileJson(profiler, world)
          : formatProfileText(profiler, world);
  std::FILE* f = openProfileFile(pathCopy, "wb");
  if (f == nullptr) {
    return ErrorCode::IoError;
  }
  const bool written =
      std::fwrite(text.data(), 1, text.size(), f) == text.size() &&
      std::fclose(f) == 0;
  if (!written) {
    // No partial report on disk (the replay recorder's no-partial-log
    // guarantee, the simple direct-write form).
    std::remove(pathCopy.data());
    return ErrorCode::IoError;
  }
  return static_cast<std::uint64_t>(text.size());
}

}  // namespace laige
