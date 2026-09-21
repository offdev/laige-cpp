// laige-sim declarative game config suite (M1-CFG-01).
//
// Step Verify scope (roadmap/M1-heartbeat.md, `ctest -R config`):
//   GameConfigParse   the version 1 document -> EngineConfig loader
//                     (parseEngineConfig): the key table, the version
//                     gate (missing / non-integer / unsupported —
//                     ARCH-007), every rejection domain, first-failure-
//                     wins, unknown-key forward-compat (the config
//                     surface's rule)
//   GameConfigFile    loadGameConfig: the bounded file read, the
//                     missing file (IoError), the truncated file
//                     (MalformedInput), the over-bound file
//                     (MalformedInput), the versioned file
//   ConfigOverride    applyConfigOverride: the programmatic override-
//                     of-a-subset merge (FR-1.5), the per-field
//                     validation, the first-set-field-wins rule
//   ConfigHotReload   ConfigHotReloader: the debug-build-only hot
//                     reload of non-simulation keys (FR-1.5) — a
//                     camera-default change reloads without a restart;
//                     a tick-rate change is refused with the
//                     actionable hot_reload_rejected error; read
//                     failures keep the previous config; release
//                     builds reject the feature (hot_reload_disabled)
//
// The underlying JSON parser is M0-CORE-07 (`ctest -R config_json`);
// this suite covers the schema layer above it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/json.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim/config.h"
#include "laige/sim/entity.h"
#include "laige/sim/game_loop.h"

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for the _fsopen below
#endif

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the
// build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "game_config_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "game_config_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "game_config_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// ---------------------------------------------------------------------------
// Test fixtures (the MemorySink capture pattern — engine_tests.cpp;
// Info and up, because hot_reload_applied is an Info event)
// ---------------------------------------------------------------------------

namespace {

// One log event captured from the facade.
class MemorySink : public laige::log::Sink {
 public:
  struct Entry {
    laige::log::Severity severity{};
    std::string subsystem;
    std::string event;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
  };

  void emit(const laige::log::LogRecord& record) override {
    if (record.severity < laige::log::Severity::Info) return;
    Entry e;
    e.severity = record.severity;
    e.subsystem = record.subsystem;
    e.event = record.event;
    e.message = record.message;
    for (const auto& f : record.fields) {
      e.fields.emplace_back(std::string(f.name), f.value);
    }
    entries.push_back(std::move(e));
  }
  void flush() override {}

  std::vector<Entry> entries;
};

// Installs a fresh capture sink with rate limiting OFF: the tests
// assert the source-level behavior, not the facade's LOG-004 window.
// (Re-)initializes the facade — required because an Engine shutdown
// retires it (CONC-006/LOG-007).
MemorySink* installCaptureSink() {
  auto sink = std::make_unique<MemorySink>();
  MemorySink* ptr = sink.get();
  laige::log::LoggerOptions opts;
  opts.sink = std::move(sink);
  opts.rateLimiting = false;
  if (!laige::log::Logger::instance().init(std::move(opts)).ok()) {
    ADD_FAILURE() << "Logger::init (capture sink) failed";
    abort();
  }
  return ptr;
}

void restoreLogger() {
  laige::log::LoggerOptions defaults;
  if (!laige::log::Logger::instance().init(std::move(defaults)).ok()) {
    ADD_FAILURE() << "Logger::init (restore default sink) failed";
  }
}

std::size_t countEvents(const MemorySink& sink, std::string_view event) {
  std::size_t n = 0;
  for (const auto& entry : sink.entries) {
    if (entry.event == event) ++n;
  }
  return n;
}

const MemorySink::Entry* findEvent(const MemorySink& sink,
                                   std::string_view event) {
  for (const auto& entry : sink.entries) {
    if (entry.event == event) return &entry;
  }
  return nullptr;
}

std::string fieldValue(const MemorySink::Entry& entry,
                       std::string_view name) {
  for (const auto& [key, value] : entry.fields) {
    if (key == name) return value;
  }
  return "";
}

// Parses a test-fixture document (a parse failure is a test bug).
laige::JsonValue parseDoc(const char* text) {
  const laige::Result<laige::JsonValue> r = laige::parseJson(text);
  if (r.isError()) {
    ADD_FAILURE() << "test fixture JSON failed to parse: " << text;
    abort();
  }
  return r.value();
}

std::string tempPath(const char* name) {
  return std::string(::testing::TempDir()) + name;
}

// Test-fixture text surgery: replace the first / every occurrence of a
// literal substring (a parse failure or a missing marker is a test
// bug).
std::string replaceFirst(const std::string& text, const char* old,
                         const char* rep) {
  const std::size_t pos = text.find(old);
  if (pos == std::string::npos) {
    ADD_FAILURE() << "fixture text not found in: " << text;
    abort();
  }
  std::string out = text;
  out.replace(pos, std::strlen(old), rep);
  return out;
}

std::string replaceAll(const std::string& text, const char* old,
                       const char* rep) {
  const std::size_t oldLen = std::strlen(old);
  std::string out;
  out.reserve(text.size());
  std::size_t pos = 0;
  for (;;) {
    const std::size_t found = text.find(old, pos);
    if (found == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    out.append(text, pos, found - pos);
    out.append(rep);
    pos = found + oldLen;
  }
  return out;
}

// Portable file write (CPP-009 platform boundary; the
// replay_record_tests.cpp precedent): MSVC's CRT deprecates plain
// fopen (C4996, fatal under the engine's /WX policy).
#if defined(_MSC_VER)
std::FILE* openTestFile(const char* path, const char* mode) {
  return ::_fsopen(path, mode, _SH_DENYNO);
}
#else
std::FILE* openTestFile(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
#endif

bool writeConfigFile(const std::string& path, std::string_view content) {
  std::FILE* file = openTestFile(path.c_str(), "wb");
  if (file == nullptr) return false;
  const std::size_t written =
      std::fwrite(content.data(), 1, content.size(), file);
  std::fclose(file);
  return written == content.size();
}

// The canonical test document: every key at a non-default value.
const char* kFullDocument = R"({
  "version": 1,
  "tick_rate_hz": 120,
  "entity_budget": 65536,
  "churn_per_frame_budget": 0,
  "seed": 9007199254740992,
  "determinism": { "enabled": false, "math": "float_pinned_32" },
  "budgets": {
    "system_time_default_ms": 2.5,
    "draw_calls_per_frame": 12,
    "particles_per_frame": 8192
  },
  "camera": { "fov_degrees": 60, "zoom": 2.5, "follow_lerp_per_sec": 0 },
  "asset_roots": ["assets/", "shaders/"]
})";

}  // namespace

// ---------------------------------------------------------------------------
// GameConfigParse: the version 1 document -> EngineConfig loader
// ---------------------------------------------------------------------------

TEST(GameConfigParse, MinimalDocumentIsAllDefaults) {
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc(R"({"version":1})")).value();
  EXPECT_EQ(config.tickRateHz, laige::kDefaultTickRateHz);
  EXPECT_EQ(config.entityCapacity, 0u);
  EXPECT_EQ(config.churnPerFrameBudget, laige::kDefaultChurnPerFrameBudget);
  EXPECT_EQ(config.seed, laige::kDefaultSimulationSeed);
  EXPECT_TRUE(config.determinism.enabled);
  EXPECT_EQ(config.determinism.math, laige::SimMathBackend::FixedPoint16_16);
  EXPECT_DOUBLE_EQ(config.budgets.systemTimeDefaultMs,
                   laige::kDefaultSystemTimeBudgetMs);
  EXPECT_EQ(config.budgets.drawCallsPerFrame,
            laige::kDefaultDrawCallsPerFrame);
  EXPECT_EQ(config.budgets.particlesPerFrame,
            laige::kDefaultParticlesPerFrame);
  EXPECT_DOUBLE_EQ(config.camera.fovDegrees, laige::kDefaultCameraFovDegrees);
  EXPECT_DOUBLE_EQ(config.camera.zoom, laige::kDefaultCameraZoom);
  EXPECT_DOUBLE_EQ(config.camera.followLerpPerSec,
                   laige::kDefaultCameraFollowLerpPerSec);
  EXPECT_TRUE(config.assetRoots.empty());
}

TEST(GameConfigParse, FullValidDocument) {
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc(kFullDocument)).value();
  EXPECT_EQ(config.tickRateHz, 120u);
  EXPECT_EQ(config.entityCapacity, 65536u);
  EXPECT_EQ(config.churnPerFrameBudget, 0u);
  EXPECT_EQ(config.seed, 9007199254740992ull);
  EXPECT_FALSE(config.determinism.enabled);
  EXPECT_EQ(config.determinism.math, laige::SimMathBackend::FloatPinned32);
  EXPECT_DOUBLE_EQ(config.budgets.systemTimeDefaultMs, 2.5);
  EXPECT_EQ(config.budgets.drawCallsPerFrame, 12u);
  EXPECT_EQ(config.budgets.particlesPerFrame, 8192u);
  EXPECT_DOUBLE_EQ(config.camera.fovDegrees, 60.0);
  EXPECT_DOUBLE_EQ(config.camera.zoom, 2.5);
  EXPECT_DOUBLE_EQ(config.camera.followLerpPerSec, 0.0);
  ASSERT_EQ(config.assetRoots.size(), 2u);
  EXPECT_EQ(config.assetRoots[0], "assets/");
  EXPECT_EQ(config.assetRoots[1], "shaders/");
}

TEST(GameConfigParse, NotAnObjectRejected) {
  for (const char* doc : {"[]", "42", R"("config")", "null"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
}

// ---------------------------------------------------------------------------
// The version gate (ARCH-007)
// ---------------------------------------------------------------------------

TEST(GameConfigParse, VersionMissingRejected) {
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::parseEngineConfig(parseDoc("{}"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "version_missing"), 1u);
  restoreLogger();
}

TEST(GameConfigParse, VersionUnsupportedRejected) {
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::parseEngineConfig(parseDoc(R"({"version":2})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  const MemorySink::Entry* event = findEvent(*sink, "version_unsupported");
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(fieldValue(*event, "version"), "2");
  EXPECT_EQ(fieldValue(*event, "supported"),
            std::to_string(laige::kSupportedConfigVersion));
  restoreLogger();
}

TEST(GameConfigParse, VersionNotAnExactIntegerRejected) {
  for (const char* bad : {R"("1")", "1.5", "-1", "true", "null"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(
            parseDoc(std::string(R"({"version":)").append(bad).append("}")
                         .c_str()));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
}

TEST(GameConfigParse, VersionIsCheckedBeforeTheKeys) {
  // The document has a bad tick rate AND no version: the version gate
  // fires first (the oldest rule rejects the whole document).
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::parseEngineConfig(
          parseDoc(R"({"tick_rate_hz":999,"version":2})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "version_unsupported"), 1u);
  EXPECT_EQ(countEvents(*sink, "tick_rate_invalid"), 0u);
  restoreLogger();
}

// ---------------------------------------------------------------------------
// The key domains (the version 1 key table, config.h)
// ---------------------------------------------------------------------------

namespace {
laige::Result<laige::EngineConfig, laige::ErrorCode>
parseKey(const char* key, const char* value) {
  // Plain literals (a raw string here would swallow the closing quote
  // of the value: the document is assembled by concatenation). The
  // value carries its own JSON quotes when it is a string literal.
  const std::string doc =
      std::string("{\"version\":1,\"") + key + "\":" + value + "}";
  return laige::parseEngineConfig(parseDoc(doc.c_str()));
}
}  // namespace

TEST(GameConfigParse, TickRateRejects) {
  for (const char* bad : {"19", "121", "60.5", R"("60")", "true",
                         "1e999"}) {  // 1e999 parses to +inf (ADR 0003)
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        parseKey("tick_rate_hz", bad);
    ASSERT_TRUE(result.isError()) << bad;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  // The inclusive boundaries are accepted.
  for (const char* good : {"20", "120"}) {
    ASSERT_TRUE(parseKey("tick_rate_hz", good).ok()) << good;
  }
}

TEST(GameConfigParse, EntityBudgetRejects) {
  for (const char* bad : {"-1", "65537", "65536.5", R"("100")"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        parseKey("entity_budget", bad);
    ASSERT_TRUE(result.isError()) << bad;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(
      parseKey("entity_budget", "65536")
          .value()
          .entityCapacity,
      laige::Entity::kMaxEntities);
  EXPECT_EQ(parseKey("entity_budget", "0").value().entityCapacity, 0u);
}

TEST(GameConfigParse, ChurnBudgetRejects) {
  for (const char* bad : {"-1", R"("10")", "10.5"}) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        parseKey("churn_per_frame_budget", bad);
    ASSERT_TRUE(result.isError()) << bad;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(parseKey("churn_per_frame_budget", "4294967295")
                .value()
                .churnPerFrameBudget,
            4294967295u);
}

TEST(GameConfigParse, SeedRejects) {
  for (const char* bad : {"-1", "9007199254740994", "1.5", R"("7")"}) {
    // 9007199254740994 = 2^53 + 2: beyond the ADR 0003 exactness bound.
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        parseKey("seed", bad);
    ASSERT_TRUE(result.isError()) << bad;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  // The 2^53 bound is inclusive (doubles are exact to 2^53).
  EXPECT_EQ(parseKey("seed", "9007199254740992").value().seed,
            9007199254740992ull);
}

TEST(GameConfigParse, DeterminismRejects) {
  MemorySink* sink = installCaptureSink();
  const char* blocks[] = {
      R"({"version":1,"determinism":5})",
      R"({"version":1,"determinism":{"enabled":"yes"}})",
      R"({"version":1,"determinism":{"math":"float32"}})"};
  for (const char* doc : blocks) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(result.isError()) << doc;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(countEvents(*sink, "determinism_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "determinism_enabled_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "determinism_math_invalid"), 1u);
  restoreLogger();
}

TEST(GameConfigParse, BudgetsRejects) {
  MemorySink* sink = installCaptureSink();
  const char* docs[] = {
      R"({"version":1,"budgets":[]})",
      R"({"version":1,"budgets":{"system_time_default_ms":0}})",
      R"({"version":1,"budgets":{"system_time_default_ms":-1}})",
      R"({"version":1,"budgets":{"system_time_default_ms":1e999}})",
      R"({"version":1,"budgets":{"draw_calls_per_frame":-1}})",
      R"({"version":1,"budgets":{"draw_calls_per_frame":1.5}})",
      R"({"version":1,"budgets":{"particles_per_frame":-1}})"};
  for (const char* doc : docs) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(result.isError()) << doc;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(countEvents(*sink, "budgets_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "system_time_budget_invalid"), 3u);
  EXPECT_EQ(countEvents(*sink, "draw_calls_budget_invalid"), 2u);
  EXPECT_EQ(countEvents(*sink, "particles_budget_invalid"), 1u);
  restoreLogger();
}

TEST(GameConfigParse, CameraRejects) {
  MemorySink* sink = installCaptureSink();
  const char* docs[] = {
      R"({"version":1,"camera":"iso"})",
      R"({"version":1,"camera":{"fov_degrees":0}})",
      R"({"version":1,"camera":{"fov_degrees":181}})",
      R"({"version":1,"camera":{"fov_degrees":1e999}})",
      R"({"version":1,"camera":{"zoom":0}})",
      R"({"version":1,"camera":{"follow_lerp_per_sec":-1}})"};
  for (const char* doc : docs) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(result.isError()) << doc;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(countEvents(*sink, "camera_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "camera_fov_invalid"), 3u);
  EXPECT_EQ(countEvents(*sink, "camera_zoom_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "camera_follow_lerp_invalid"), 1u);
  // The inclusive fov upper bound is accepted.
  EXPECT_TRUE(
      laige::parseEngineConfig(
          parseDoc(R"({"version":1,"camera":{"fov_degrees":180}})"))
          .ok());
  restoreLogger();
}

TEST(GameConfigParse, AssetRootsRejects) {
  MemorySink* sink = installCaptureSink();
  const char* docs[] = {
      R"({"version":1,"asset_roots":"assets/"})",
      R"({"version":1,"asset_roots":[7]})",
      R"({"version":1,"asset_roots":["assets/",""]})"};
  for (const char* doc : docs) {
    const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
        laige::parseEngineConfig(parseDoc(doc));
    ASSERT_TRUE(result.isError()) << doc;
    EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(countEvents(*sink, "asset_roots_invalid"), 3u);
  restoreLogger();
}

// ---------------------------------------------------------------------------
// Unknown keys and first-failure-wins
// ---------------------------------------------------------------------------

TEST(GameConfigParse, UnknownKeysWarnAndAreIgnored) {
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::parseEngineConfig(parseDoc(
          R"({"version":1,"tick_rate_hz":60,"fog_density":0.1,"determinism":{"enabled":true,"future_flag":true},"budgets":{"legacy":1},"camera":{"shake":2}})"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().tickRateHz, 60u);
  // One unknown_key per unknown key: one top-level, three nested.
  EXPECT_EQ(countEvents(*sink, "unknown_key"), 4u);
  restoreLogger();
}

TEST(GameConfigParse, FirstFailureWins) {
  MemorySink* sink = installCaptureSink();
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::parseEngineConfig(
          parseDoc(R"({"version":1,"tick_rate_hz":999,"entity_budget":-5})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "tick_rate_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "entity_budget_invalid"), 0u);
  restoreLogger();
}

// ---------------------------------------------------------------------------
// GameConfigFile: loadGameConfig (the bounded file read + schema)
// ---------------------------------------------------------------------------

TEST(GameConfigFile, LoadsTheFullDocument) {
  const std::string path = tempPath("game_config_full.json");
  ASSERT_TRUE(writeConfigFile(path, kFullDocument));
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::loadGameConfig(path);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().tickRateHz, 120u);
  EXPECT_EQ(result.value().seed, 9007199254740992ull);
  EXPECT_DOUBLE_EQ(result.value().camera.zoom, 2.5);
  ASSERT_EQ(result.value().assetRoots.size(), 2u);
  std::remove(path.c_str());  // clean up (the gtest temp dir)
}

TEST(GameConfigFile, MissingFileIsAnIoError) {
  MemorySink* sink = installCaptureSink();
  const std::string path = tempPath("game_config_missing.json");
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::loadGameConfig(path);
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::IoError);
  const MemorySink::Entry* event = findEvent(*sink, "file_read_failed");
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(fieldValue(*event, "path"), path);
  restoreLogger();
}

TEST(GameConfigFile, TruncatedFileIsMalformedInput) {
  const std::string path = tempPath("game_config_truncated.json");
  // A full document minus its closing brace (and trailing newline):
  // a truncated file is a grammar violation (MalformedInput), not a
  // truncated parse.
  const std::string full = kFullDocument;
  const std::string truncated = full.substr(0, full.size() - 2);
  ASSERT_TRUE(writeConfigFile(path, truncated));
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::loadGameConfig(path);
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::MalformedInput);
  std::remove(path.c_str());
}

TEST(GameConfigFile, UnsupportedVersionFileIsRejected) {
  const std::string path = tempPath("game_config_bad_version.json");
  ASSERT_TRUE(writeConfigFile(path, R"({"version":2,"tick_rate_hz":60})"));
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::loadGameConfig(path);
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::InvalidArgument);
  std::remove(path.c_str());
}

TEST(GameConfigFile, OverBoundFileIsMalformedInput) {
  const std::string path = tempPath("game_config_overbound.json");
  // 1 MiB + 1 byte: over the ADR 0003 bound (the read stops one byte
  // past the bound — MalformedInput, not a truncated parse).
  std::string overbound(1u << 20, 'x');
  overbound.push_back('x');
  ASSERT_TRUE(writeConfigFile(path, overbound));
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::loadGameConfig(path);
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::MalformedInput);
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// ConfigOverride: applyConfigOverride (the programmatic merge, FR-1.5)
// ---------------------------------------------------------------------------

namespace {
laige::EngineConfig fullBaseConfig() {
  laige::EngineConfig config;
  config.tickRateHz = 90;
  config.entityCapacity = 2048;
  config.churnPerFrameBudget = 128;
  config.seed = 42;
  config.determinism.enabled = false;
  config.determinism.math = laige::SimMathBackend::FloatPinned32;
  config.budgets.systemTimeDefaultMs = 2.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared budget, not sim math
  config.budgets.drawCallsPerFrame = 16;
  config.budgets.particlesPerFrame = 1024;
  config.camera.fovDegrees = 50.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared camera default, not sim math
  config.camera.zoom = 2.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared camera default, not sim math
  config.camera.followLerpPerSec = 4.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared camera default, not sim math
  config.assetRoots = {"assets/"};
  return config;
}
}  // namespace

TEST(ConfigOverride, EmptyOverrideKeepsTheBase) {
  const laige::EngineConfig base = fullBaseConfig();
  const laige::EngineConfig merged =
      laige::applyConfigOverride(base, {}).value();
  EXPECT_EQ(merged.tickRateHz, base.tickRateHz);
  EXPECT_EQ(merged.entityCapacity, base.entityCapacity);
  EXPECT_EQ(merged.churnPerFrameBudget, base.churnPerFrameBudget);
  EXPECT_EQ(merged.seed, base.seed);
  EXPECT_EQ(merged.determinism.enabled, base.determinism.enabled);
  EXPECT_EQ(merged.determinism.math, base.determinism.math);
  EXPECT_DOUBLE_EQ(merged.budgets.systemTimeDefaultMs,
                   base.budgets.systemTimeDefaultMs);
  EXPECT_EQ(merged.budgets.drawCallsPerFrame, base.budgets.drawCallsPerFrame);
  EXPECT_EQ(merged.budgets.particlesPerFrame, base.budgets.particlesPerFrame);
  EXPECT_DOUBLE_EQ(merged.camera.fovDegrees, base.camera.fovDegrees);
  EXPECT_DOUBLE_EQ(merged.camera.zoom, base.camera.zoom);
  EXPECT_DOUBLE_EQ(merged.camera.followLerpPerSec,
                   base.camera.followLerpPerSec);
  EXPECT_EQ(merged.assetRoots, base.assetRoots);
}

TEST(ConfigOverride, SingleFieldOverridesLeaveTheRest) {
  const laige::EngineConfig base = fullBaseConfig();
  laige::EngineConfigOverride override;
  override.tickRateHz = 120;
  const laige::EngineConfig merged =
      laige::applyConfigOverride(base, override).value();
  EXPECT_EQ(merged.tickRateHz, 120u);
  EXPECT_EQ(merged.entityCapacity, base.entityCapacity);
  EXPECT_EQ(merged.seed, base.seed);
  EXPECT_EQ(merged.camera.zoom, base.camera.zoom);
  // The base is untouched (value semantics).
  EXPECT_EQ(base.tickRateHz, 90u);
}

TEST(ConfigOverride, InvalidFieldsAreRejected) {
  MemorySink* sink = installCaptureSink();
  const laige::EngineConfig base = fullBaseConfig();
  laige::EngineConfigOverride badTick;
  badTick.tickRateHz = 19;
  ASSERT_TRUE(laige::applyConfigOverride(base, badTick).isError());
  EXPECT_EQ(countEvents(*sink, "tick_rate_invalid"), 1u);

  laige::EngineConfigOverride badBudget;
  badBudget.entityCapacity = laige::Entity::kMaxEntities + 1;
  ASSERT_TRUE(laige::applyConfigOverride(base, badBudget).isError());
  EXPECT_EQ(countEvents(*sink, "entity_budget_invalid"), 1u);

  laige::EngineConfigOverride badFov;
  badFov.fovDegrees = 0.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared camera default, not sim math
  ASSERT_TRUE(laige::applyConfigOverride(base, badFov).isError());
  EXPECT_EQ(countEvents(*sink, "camera_fov_invalid"), 1u);

  laige::EngineConfigOverride badLerp;
  badLerp.followLerpPerSec = -1.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared camera default, not sim math
  ASSERT_TRUE(laige::applyConfigOverride(base, badLerp).isError());
  EXPECT_EQ(countEvents(*sink, "camera_follow_lerp_invalid"), 1u);

  laige::EngineConfigOverride badBudgetMs;
  badBudgetMs.systemTimeDefaultMs = 0.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared budget, not sim math
  ASSERT_TRUE(laige::applyConfigOverride(base, badBudgetMs).isError());
  EXPECT_EQ(countEvents(*sink, "system_time_budget_invalid"), 1u);

  laige::EngineConfigOverride badRoots;
  badRoots.assetRoots = {"assets/", ""};
  ASSERT_TRUE(laige::applyConfigOverride(base, badRoots).isError());
  EXPECT_EQ(countEvents(*sink, "asset_roots_invalid"), 1u);
  restoreLogger();
}

TEST(ConfigOverride, FirstSetFieldThatFailsWins) {
  MemorySink* sink = installCaptureSink();
  const laige::EngineConfig base = fullBaseConfig();
  laige::EngineConfigOverride override;
  override.fovDegrees = 181.0;  // LAIGE-DETERM-EXCEPTION: G-R8 test value: a declared camera default, not sim math
  override.tickRateHz = 19;
  // The struct's declaration order puts tickRateHz before fovDegrees.
  const laige::Result<laige::EngineConfig, laige::ErrorCode> result =
      laige::applyConfigOverride(base, override);
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(countEvents(*sink, "tick_rate_invalid"), 1u);
  EXPECT_EQ(countEvents(*sink, "camera_fov_invalid"), 0u);
  restoreLogger();
}

TEST(ConfigOverride, AssetRootsReplaceTheVector) {
  const laige::EngineConfig base = fullBaseConfig();
  laige::EngineConfigOverride override;
  override.assetRoots = {"a/", "b/"};
  const laige::EngineConfig merged =
      laige::applyConfigOverride(base, override).value();
  ASSERT_EQ(merged.assetRoots.size(), 2u);
  EXPECT_EQ(merged.assetRoots[0], "a/");
  EXPECT_EQ(merged.assetRoots[1], "b/");
  // Every other field survives the replacement.
  EXPECT_EQ(merged.tickRateHz, base.tickRateHz);
}

// ---------------------------------------------------------------------------
// ConfigHotReload: the debug-build-only hot reload (FR-1.5)
// ---------------------------------------------------------------------------

namespace {
// The reloader's baseline document: tick 60, camera fov 45 (the
// defaults), one asset root.
const char* kBaselineDocument = R"({
  "version": 1,
  "tick_rate_hz": 60,
  "camera": { "fov_degrees": 45, "zoom": 1, "follow_lerp_per_sec": 8 },
  "asset_roots": ["assets/"]
})";

laige::EngineConfig baselineConfig() {
  const laige::EngineConfig config =
      laige::parseEngineConfig(parseDoc(kBaselineDocument)).value();
  return config;
}
}  // namespace

TEST(ConfigHotReload, CreateFailsOnMissingFile) {
  MemorySink* sink = installCaptureSink();
  const std::string path = tempPath("hot_reload_missing.json");
  const laige::Result<laige::ConfigHotReloader, laige::ErrorCode> result =
      laige::ConfigHotReloader::create(path);
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error(), laige::ErrorCode::IoError);
  EXPECT_EQ(countEvents(*sink, "file_read_failed"), 1u);
  restoreLogger();
}

TEST(ConfigHotReload, UnchangedFileIsANoop) {
  const std::string path = tempPath("hot_reload_unchanged.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  EXPECT_TRUE(reloader.poll(config).ok());
  EXPECT_EQ(config.tickRateHz, 60u);
  std::remove(path.c_str());
}

TEST(ConfigHotReload, CameraDefaultReloadsWithoutRestart) {
  const std::string path = tempPath("hot_reload_camera.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  // The editor changes the camera fov (a non-simulation key): the
  // poll applies it to the live config without a restart (FR-1.5).
  const std::string changed =
      replaceFirst(kBaselineDocument, R"( "fov_degrees": 45)",
                   R"( "fov_degrees": 60)");
  ASSERT_NE(changed, kBaselineDocument);
  ASSERT_TRUE(writeConfigFile(path, changed));
  MemorySink* sink = installCaptureSink();
  EXPECT_TRUE(reloader.poll(config).ok());
  EXPECT_DOUBLE_EQ(config.camera.fovDegrees, 60.0);
  // The simulation-affecting fields are untouched.
  EXPECT_EQ(config.tickRateHz, 60u);
  EXPECT_EQ(config.seed, laige::kDefaultSimulationSeed);
  EXPECT_TRUE(config.determinism.enabled);
  const MemorySink::Entry* applied = findEvent(*sink, "hot_reload_applied");
  ASSERT_NE(applied, nullptr);
  EXPECT_EQ(fieldValue(*applied, "keys"), "camera.fov_degrees");
  restoreLogger();
  std::remove(path.c_str());
}

TEST(ConfigHotReload, TickRateChangeIsRefused) {
  const std::string path = tempPath("hot_reload_tick.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  // The editor changes the tick rate (a simulation-affecting key):
  // the poll is REFUSED with the actionable hot_reload_rejected error
  // and the config is left untouched (FR-1.5: restart to apply).
  const std::string changed = replaceFirst(kBaselineDocument,
                                           "\"tick_rate_hz\": 60",
                                           "\"tick_rate_hz\": 90");
  ASSERT_NE(changed, kBaselineDocument);
  ASSERT_TRUE(writeConfigFile(path, changed));
  MemorySink* sink = installCaptureSink();
  const laige::Status status = reloader.poll(config);
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::InvalidArgument);
  const MemorySink::Entry* rejected =
      findEvent(*sink, "hot_reload_rejected");
  ASSERT_NE(rejected, nullptr);
  EXPECT_EQ(fieldValue(*rejected, "key"), "tick_rate_hz");
  EXPECT_EQ(fieldValue(*rejected, "old"), "60");
  EXPECT_EQ(fieldValue(*rejected, "new"), "90");
  // The live config and the baseline are untouched: a further poll of
  // the (still refused) file reports the same refusal, and the engine
  // values are unchanged.
  EXPECT_EQ(config.tickRateHz, 60u);
  EXPECT_TRUE(reloader.poll(config).isError());
  restoreLogger();
  std::remove(path.c_str());
}

TEST(ConfigHotReload, MixedSimAndNonSimChangeIsRefusedAtomic) {
  const std::string path = tempPath("hot_reload_mixed.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  // One write changes the seed (sim-affecting) AND the zoom
  // (non-sim): the whole change is refused — the zoom is NOT applied
  // (the config never corresponds to a partial file state).
  std::string changed = replaceFirst(kBaselineDocument, R"("zoom": 1,)",
                                     R"("zoom": 9,)");
  changed = replaceFirst(changed, "\n}", ",\n  \"seed\": 7\n}");
  ASSERT_TRUE(writeConfigFile(path, changed));
  const laige::Status status = reloader.poll(config);
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_DOUBLE_EQ(config.camera.zoom, laige::kDefaultCameraZoom);
  EXPECT_EQ(config.seed, laige::kDefaultSimulationSeed);
  std::remove(path.c_str());
}

TEST(ConfigHotReload, SystemTimeBudgetChangeIsRefused) {
  const std::string path = tempPath("hot_reload_systime.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  // The per-system default time budget governs the G-R5 enforcement
  // during ticks: it is simulation-affecting (restart-required).
  const std::string changed =
      replaceFirst(kBaselineDocument, "\"asset_roots\"",
                   R"("budgets": {"system_time_default_ms": 9}, "asset_roots")");
  ASSERT_TRUE(writeConfigFile(path, changed));
  const laige::Status status = reloader.poll(config);
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_DOUBLE_EQ(config.budgets.systemTimeDefaultMs,
                   laige::kDefaultSystemTimeBudgetMs);
  std::remove(path.c_str());
}

TEST(ConfigHotReload, PresentationBudgetsAndAssetRootsReload) {
  const std::string path = tempPath("hot_reload_presentation.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  std::string changed =
      replaceFirst(kBaselineDocument, "\"asset_roots\"",
                   R"("budgets": {"draw_calls_per_frame": 24, "particles_per_frame": 2048}, "asset_roots")");
  changed = replaceFirst(changed, R"("assets/")", R"("assets/", "extra/")");
  ASSERT_TRUE(writeConfigFile(path, changed));
  MemorySink* sink = installCaptureSink();
  EXPECT_TRUE(reloader.poll(config).ok());
  EXPECT_EQ(config.budgets.drawCallsPerFrame, 24u);
  EXPECT_EQ(config.budgets.particlesPerFrame, 2048u);
  ASSERT_EQ(config.assetRoots.size(), 2u);
  EXPECT_EQ(config.assetRoots[1], "extra/");
  const MemorySink::Entry* applied = findEvent(*sink, "hot_reload_applied");
  ASSERT_NE(applied, nullptr);
  EXPECT_EQ(
      fieldValue(*applied, "keys"),
      "budgets.draw_calls_per_frame,budgets.particles_per_frame,"
      "asset_roots");
  restoreLogger();
  std::remove(path.c_str());
}

TEST(ConfigHotReload, WhitespaceOnlyChangeAdvancesTheBaseline) {
  const std::string path = tempPath("hot_reload_whitespace.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  // The editor reformats (same fields, different bytes): no semantic
  // change — no applied event, but the baseline advances so the NEXT
  // semantic change is measured against the new document.
  const std::string reformatted =
      replaceAll(kBaselineDocument, "\n", "\r\n");
  ASSERT_NE(reformatted, kBaselineDocument);
  ASSERT_TRUE(writeConfigFile(path, reformatted));
  MemorySink* sink = installCaptureSink();
  EXPECT_TRUE(reloader.poll(config).ok());
  EXPECT_EQ(countEvents(*sink, "hot_reload_applied"), 0u);
  // Now a real camera change against the reformatted baseline.
  const std::string changed =
      replaceFirst(reformatted, R"( "fov_degrees": 45)",
                   R"( "fov_degrees": 90)");
  ASSERT_TRUE(writeConfigFile(path, changed));
  EXPECT_TRUE(reloader.poll(config).ok());
  EXPECT_DOUBLE_EQ(config.camera.fovDegrees, 90.0);
  EXPECT_EQ(countEvents(*sink, "hot_reload_applied"), 1u);
  restoreLogger();
  std::remove(path.c_str());
}

TEST(ConfigHotReload, MalformedFileKeepsThePreviousConfig) {
  const std::string path = tempPath("hot_reload_malformed.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  // The file is truncated mid-edit (the closing brace is missing):
  // the poll fails with MalformedInput and the live config is
  // untouched.
  const std::string full = kBaselineDocument;
  const std::string truncated = full.substr(0, full.size() - 2);
  ASSERT_TRUE(writeConfigFile(path, truncated));
  const laige::Status status = reloader.poll(config);
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::MalformedInput);
  EXPECT_EQ(config.tickRateHz, 60u);
  // The editor finishes the save: the file matches the baseline again
  // — the poll is a clean no-op.
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  EXPECT_TRUE(reloader.poll(config).ok());
  std::remove(path.c_str());
}

TEST(ConfigHotReload, ReadFailureKeepsThePreviousConfig) {
  const std::string path = tempPath("hot_reload_readfail.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::EngineConfig config = baselineConfig();
  // The file vanishes (a transient editor state): the poll fails with
  // IoError + the retryable read-failed warn; the config stays.
  std::remove(path.c_str());
  MemorySink* sink = installCaptureSink();
  const laige::Status status = reloader.poll(config);
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::IoError);
  EXPECT_EQ(countEvents(*sink, "hot_reload_read_failed"), 1u);
  EXPECT_EQ(config.tickRateHz, 60u);
  // The file comes back with the original bytes: a clean no-op.
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  EXPECT_TRUE(reloader.poll(config).ok());
  restoreLogger();
  std::remove(path.c_str());
}

TEST(ConfigHotReload, MovedFromReloaderFailsWithoutLogging) {
  const std::string path = tempPath("hot_reload_moved.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  laige::ConfigHotReloader reloader =
      std::move(laige::ConfigHotReloader::create(path)).takeValue();
  laige::ConfigHotReloader moved = std::move(reloader);
  laige::EngineConfig config = baselineConfig();
  MemorySink* sink = installCaptureSink();
  // The moved-from reloader is stopped: poll fails without logging
  // (the stopped-state precedent).
  const laige::Status status = reloader.poll(config);
  ASSERT_TRUE(status.isError());
  EXPECT_EQ(status.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(sink->entries.size(), 0u);
  // The live reloader still works.
  EXPECT_TRUE(moved.poll(config).ok());
  restoreLogger();
  std::remove(path.c_str());
}

#if defined(NDEBUG)
// Release builds (NDEBUG): hot reload is a debug-build feature — the
// feature call is rejected (the replay/record_disabled pattern).
TEST(ConfigHotReload, ReleaseBuildsRejectTheFeature) {
  MemorySink* sink = installCaptureSink();
  const std::string path = tempPath("hot_reload_release.json");
  ASSERT_TRUE(writeConfigFile(path, kBaselineDocument));
  const laige::Result<laige::ConfigHotReloader, laige::ErrorCode> created =
      laige::ConfigHotReloader::create(path);
  ASSERT_TRUE(created.isError());
  EXPECT_EQ(created.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(countEvents(*sink, "hot_reload_disabled"), 1u);
  restoreLogger();
  std::remove(path.c_str());
}
#endif
