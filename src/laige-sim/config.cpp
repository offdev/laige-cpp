// laige-sim declarative game config (M1-CFG-01).
//
// Implementation of the EngineConfig loaders (parseEngineConfig,
// loadGameConfig), the subset-override merge (applyConfigOverride),
// and the debug-only hot reloader (ConfigHotReloader) declared in
// include/laige/sim/config.h — see that header for the full contract
// (the version 1 schema, versioning, loading and overrides, ownership
// and performance, the error events) and docs/api/config.md for the
// API document.
//
// Hot-path cost: none — every function here is a cold path (startup,
// dev-time, or explicit dev-loop polling); the sim tick path is never
// touched (PERF-002/003, DBG-004).

#include "laige/sim/config.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for the _fsopen below
#endif

#include "laige/json.h"
#include "laige/logging.h"

namespace laige {

namespace {

// The 1 MiB config-document bound (ADR 0003 / the JsonOptions default):
// the read stops one byte past the bound so an oversized file is a
// MalformedInput, not a truncated parse.
inline constexpr std::size_t kMaxConfigBytes = 1u << 20;

// The ADR 0003 JSON number bound: 2^53 (doubles are exact up to
// here) — the upper bound of the JSON "seed" key (and of "version").
inline constexpr std::uint64_t kMaxJsonSeed = 9007199254740992ull;

// Portable file open (CPP-009 platform boundary; the laige-run.cpp /
// replay.cpp precedent): MSVC's CRT deprecates plain fopen (C4996,
// fatal under the engine's /WX policy), so the MSVC path uses
// _fsopen with _SH_DENYNO — plain-fopen sharing semantics.
#if defined(_MSC_VER)
std::FILE* openConfigFile(const char* path) {
  return ::_fsopen(path, "rb", _SH_DENYNO);
}
#else
std::FILE* openConfigFile(const char* path) {
  return std::fopen(path, "rb");
}
#endif

// Reads the file at `path` fully into `out` (bounded, CORE-008):
//   open/read failure  -> IoError          (config/file_read_failed)
//   over the bound     -> MalformedInput   (config/file_read_failed)
// On success `out` holds the exact file bytes (a failed read leaves
// `out` untouched). Cold path: one file open + bounded reads.
Status readFileBounded(std::string_view path, std::string* out) noexcept {
  std::FILE* file = openConfigFile(std::string(path).c_str());
  if (file == nullptr) {
    LAIGE_LOG_WARN(kConfigSubsystem, "file_read_failed",
                   kConfigFileReadFailedMessage,
                   laige::log::field("path", path));
    return ErrorCode::IoError;
  }
  std::string bytes;
  char buffer[8192];
  for (;;) {
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer), file);
    if (n > 0) {
      bytes.append(buffer, n);
      if (bytes.size() > kMaxConfigBytes) {
        std::fclose(file);
        LAIGE_LOG_WARN(kConfigSubsystem, "file_read_failed",
                       kConfigFileReadFailedMessage,
                       laige::log::field("path", path),
                       laige::log::field("bound_bytes", kMaxConfigBytes));
        return ErrorCode::MalformedInput;
      }
    } else {
      if (!std::feof(file)) {
        // A short read that is not EOF: a read error (a locked or
        // vanishing file — the transient states the reloader retries).
        std::fclose(file);
        LAIGE_LOG_WARN(kConfigSubsystem, "file_read_failed",
                       kConfigFileReadFailedMessage,
                       laige::log::field("path", path));
        return ErrorCode::IoError;
      }
      break;  // clean EOF
    }
  }
  std::fclose(file);
  *out = std::move(bytes);
  return Status{};
}

// A JsonValue holds an exact integer in [lo, hi] (ADR 0003 JSON number
// policy — config parsing, not sim math).
bool parseIntInRange(const JsonValue& value, std::uint32_t lo,
                     std::uint32_t hi, std::uint32_t* out) noexcept {
  if (!value.isNumber()) return false;
  const double d = value.asNumber();  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
  if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(hi) ||  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
      d != std::floor(d)) {
    return false;
  }
  const std::uint32_t u = static_cast<std::uint32_t>(d);
  if (u < lo) return false;
  *out = u;
  return true;
}

// A JsonValue holds an exact integer in [lo, hi] (u64 range; the
// ADR 0003 JSON number policy — config parsing, not sim math).
bool parseUint64InRange(const JsonValue& value, std::uint64_t lo,
                        std::uint64_t hi, std::uint64_t* out) noexcept {
  if (!value.isNumber()) return false;
  const double d = value.asNumber();  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
  if (!std::isfinite(d) || d < 0.0 || d > static_cast<double>(hi) ||  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
      d != std::floor(d)) {
    return false;
  }
  const std::uint64_t u = static_cast<std::uint64_t>(d);
  if (u < lo) return false;
  *out = u;
  return true;
}

// A JsonValue holds a finite double (the ADR 0003 JSON number policy —
// config parsing, not sim math).
bool parseFiniteDouble(const JsonValue& value, double* out) noexcept {  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003): the config number is a double, not sim math
  if (!value.isNumber()) return false;
  const double d = value.asNumber();  // LAIGE-DETERM-EXCEPTION: G-R8 JSON number policy (ADR 0003); config parsing, not sim math
  if (!std::isfinite(d)) return false;
  *out = d;
  return true;
}

// A stable machine-searchable name for a JsonValue's kind (the
// `value_kind` field of the rejection warns; LOG-001).
const char* jsonKindName(const JsonValue& value) noexcept {
  switch (value.kind()) {
    case JsonKind::Null: return "null";
    case JsonKind::Bool: return "bool";
    case JsonKind::Number: return "number";
    case JsonKind::String: return "string";
    case JsonKind::Array: return "array";
    case JsonKind::Object: return "object";
  }
  return "unknown";
}

}  // namespace

// ---------------------------------------------------------------------------
// parseEngineConfig (the version 1 schema — the rejection table in
// config.h; the full key table in docs/api/config.md)
// ---------------------------------------------------------------------------

Result<EngineConfig, ErrorCode> parseEngineConfig(const JsonValue& doc) noexcept {
  if (!doc.isObject()) {
    LAIGE_LOG_WARN(kConfigSubsystem, "not_an_object",
                   kConfigNotAnObjectMessage);
    return ErrorCode::InvalidArgument;
  }
  // The version gate runs FIRST (ARCH-007): before any other key is
  // read, so a document from the wrong schema generation is rejected
  // on the oldest rule, whatever else it contains.
  const JsonValue* version = doc.findMember("version");
  if (version == nullptr) {
    LAIGE_LOG_WARN(kConfigSubsystem, "version_missing",
                   kConfigVersionMissingMessage);
    return ErrorCode::InvalidArgument;
  }
  std::uint64_t versionValue = 0;
  if (!parseUint64InRange(*version, 0, kMaxJsonSeed, &versionValue)) {
    if (version->isNumber()) {
      LAIGE_LOG_WARN(kConfigSubsystem, "version_invalid",
                     kConfigVersionInvalidMessage,
                     laige::log::field("value", version->asNumber()));
    } else {
      LAIGE_LOG_WARN(kConfigSubsystem, "version_invalid",
                     kConfigVersionInvalidMessage,
                     laige::log::field("value_kind",
                                       jsonKindName(*version)));
    }
    return ErrorCode::InvalidArgument;
  }
  if (versionValue != kSupportedConfigVersion) {
    LAIGE_LOG_WARN(kConfigSubsystem, "version_unsupported",
                   kConfigVersionUnsupportedMessage,
                   laige::log::field("version", versionValue),
                   laige::log::field("supported", kSupportedConfigVersion));
    return ErrorCode::InvalidArgument;
  }

  EngineConfig config;
  for (const auto& [key, value] : doc.asObject()) {
    if (key == "version") {
      // Validated by the gate above; the walk skips the gate key.
    } else if (key == "tick_rate_hz") {
      if (!parseIntInRange(value, kMinTickRateHz, kMaxTickRateHz,
                           &config.tickRateHz)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "tick_rate_invalid",
                         kConfigTickRateInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "tick_rate_invalid",
                         kConfigTickRateInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind",
                                           jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
    } else if (key == "entity_budget") {
      if (!parseIntInRange(value, 0, Entity::kMaxEntities,
                           &config.entityCapacity)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "entity_budget_invalid",
                         kConfigEntityBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "entity_budget_invalid",
                         kConfigEntityBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind",
                                           jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
    } else if (key == "churn_per_frame_budget") {
      const std::uint32_t kMaxUint32 =
          std::numeric_limits<std::uint32_t>::max();
      if (!parseIntInRange(value, 0, kMaxUint32,
                           &config.churnPerFrameBudget)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "churn_budget_invalid",
                         kConfigChurnBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "churn_budget_invalid",
                         kConfigChurnBudgetInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind",
                                           jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
    } else if (key == "seed") {
      std::uint64_t seed = 0;
      if (!parseUint64InRange(value, 0, kMaxJsonSeed, &seed)) {
        if (value.isNumber()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "seed_invalid",
                         kConfigSeedInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value", value.asNumber()));
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "seed_invalid",
                         kConfigSeedInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind",
                                           jsonKindName(value)));
        }
        return ErrorCode::InvalidArgument;
      }
      config.seed = seed;
    } else if (key == "determinism") {
      if (!value.isObject()) {
        LAIGE_LOG_WARN(kConfigSubsystem, "determinism_invalid",
                       kConfigDeterminismInvalidMessage,
                       laige::log::field("key", key),
                       laige::log::field("value_kind",
                                         jsonKindName(value)));
        return ErrorCode::InvalidArgument;
      }
      for (const auto& [dkey, dvalue] : value.asObject()) {
        if (dkey == "enabled") {
          if (!dvalue.isBool()) {
            LAIGE_LOG_WARN(kConfigSubsystem,
                           "determinism_enabled_invalid",
                           kConfigDeterminismEnabledInvalidMessage,
                           laige::log::field("key", dkey),
                           laige::log::field("value_kind",
                                             jsonKindName(dvalue)));
            return ErrorCode::InvalidArgument;
          }
          config.determinism.enabled = dvalue.asBool();
        } else if (dkey == "math") {
          if (!dvalue.isString()) {
            LAIGE_LOG_WARN(kConfigSubsystem, "determinism_math_invalid",
                           kConfigDeterminismMathInvalidMessage,
                           laige::log::field("key", dkey),
                           laige::log::field("value_kind",
                                             jsonKindName(dvalue)));
            return ErrorCode::InvalidArgument;
          }
          const std::string_view m = dvalue.asString();
          if (m == "fixed_point_16_16") {
            config.determinism.math = SimMathBackend::FixedPoint16_16;
          } else if (m == "float_pinned_32") {
            config.determinism.math = SimMathBackend::FloatPinned32;
          } else {
            LAIGE_LOG_WARN(kConfigSubsystem, "determinism_math_invalid",
                           kConfigDeterminismMathInvalidMessage,
                           laige::log::field("key", dkey),
                           laige::log::field("value", std::string{m}));
            return ErrorCode::InvalidArgument;
          }
        } else {
          // Unknown nested key: WARN (forward-compat) and ignore —
          // the schema rule applied inside the block (never silent).
          LAIGE_LOG_WARN(kConfigSubsystem, "unknown_key",
                         kConfigUnknownKeyMessage,
                         laige::log::field("key", dkey));
        }
      }
    } else if (key == "budgets") {
      if (!value.isObject()) {
        LAIGE_LOG_WARN(kConfigSubsystem, "budgets_invalid",
                       kConfigBudgetsInvalidMessage,
                       laige::log::field("key", key),
                       laige::log::field("value_kind",
                                         jsonKindName(value)));
        return ErrorCode::InvalidArgument;
      }
      const std::uint32_t kMaxUint32 =
          std::numeric_limits<std::uint32_t>::max();
      for (const auto& [bkey, bvalue] : value.asObject()) {
        if (bkey == "system_time_default_ms") {
          double ms = 0.0;  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
          if (!parseFiniteDouble(bvalue, &ms) || ms <= 0.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
            LAIGE_LOG_WARN(kConfigSubsystem, "system_time_budget_invalid",
                           kConfigSystemTimeBudgetInvalidMessage,
                           laige::log::field("key", bkey),
                           laige::log::field("value_kind",
                                             jsonKindName(bvalue)));
            return ErrorCode::InvalidArgument;
          }
          config.budgets.systemTimeDefaultMs = ms;
        } else if (bkey == "draw_calls_per_frame") {
          if (!parseIntInRange(bvalue, 0, kMaxUint32,
                               &config.budgets.drawCallsPerFrame)) {
            LAIGE_LOG_WARN(kConfigSubsystem, "draw_calls_budget_invalid",
                           kConfigDrawCallsBudgetInvalidMessage,
                           laige::log::field("key", bkey),
                           laige::log::field("value_kind",
                                             jsonKindName(bvalue)));
            return ErrorCode::InvalidArgument;
          }
        } else if (bkey == "particles_per_frame") {
          if (!parseIntInRange(bvalue, 0, kMaxUint32,
                               &config.budgets.particlesPerFrame)) {
            LAIGE_LOG_WARN(kConfigSubsystem, "particles_budget_invalid",
                           kConfigParticlesBudgetInvalidMessage,
                           laige::log::field("key", bkey),
                           laige::log::field("value_kind",
                                             jsonKindName(bvalue)));
            return ErrorCode::InvalidArgument;
          }
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "unknown_key",
                         kConfigUnknownKeyMessage,
                         laige::log::field("key", bkey));
        }
      }
    } else if (key == "camera") {
      if (!value.isObject()) {
        LAIGE_LOG_WARN(kConfigSubsystem, "camera_invalid",
                       kConfigCameraInvalidMessage,
                       laige::log::field("key", key),
                       laige::log::field("value_kind",
                                         jsonKindName(value)));
        return ErrorCode::InvalidArgument;
      }
      for (const auto& [ckey, cvalue] : value.asObject()) {
        if (ckey == "fov_degrees") {
          double fov = 0.0;  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
          if (!parseFiniteDouble(cvalue, &fov) || fov <= 0.0 ||  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
              fov > 180.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
            LAIGE_LOG_WARN(kConfigSubsystem, "camera_fov_invalid",
                           kConfigCameraFovInvalidMessage,
                           laige::log::field("key", ckey),
                           laige::log::field("value_kind",
                                             jsonKindName(cvalue)));
            return ErrorCode::InvalidArgument;
          }
          config.camera.fovDegrees = fov;
        } else if (ckey == "zoom") {
          double zoom = 0.0;  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
          if (!parseFiniteDouble(cvalue, &zoom) || zoom <= 0.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
            LAIGE_LOG_WARN(kConfigSubsystem, "camera_zoom_invalid",
                           kConfigCameraZoomInvalidMessage,
                           laige::log::field("key", ckey),
                           laige::log::field("value_kind",
                                             jsonKindName(cvalue)));
            return ErrorCode::InvalidArgument;
          }
          config.camera.zoom = zoom;
        } else if (ckey == "follow_lerp_per_sec") {
          double lerp = 0.0;  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
          if (!parseFiniteDouble(cvalue, &lerp) || lerp < 0.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config parsing (ADR 0003 JSON number policy), not sim math
            LAIGE_LOG_WARN(kConfigSubsystem, "camera_follow_lerp_invalid",
                           kConfigCameraFollowLerpInvalidMessage,
                           laige::log::field("key", ckey),
                           laige::log::field("value_kind",
                                             jsonKindName(cvalue)));
            return ErrorCode::InvalidArgument;
          }
          config.camera.followLerpPerSec = lerp;
        } else {
          LAIGE_LOG_WARN(kConfigSubsystem, "unknown_key",
                         kConfigUnknownKeyMessage,
                         laige::log::field("key", ckey));
        }
      }
    } else if (key == "asset_roots") {
      if (!value.isArray()) {
        LAIGE_LOG_WARN(kConfigSubsystem, "asset_roots_invalid",
                       kConfigAssetRootsInvalidMessage,
                       laige::log::field("key", key),
                       laige::log::field("value_kind",
                                         jsonKindName(value)));
        return ErrorCode::InvalidArgument;
      }
      for (const JsonValue& root : value.asArray()) {
        if (!root.isString() || root.asString().empty()) {
          LAIGE_LOG_WARN(kConfigSubsystem, "asset_roots_invalid",
                         kConfigAssetRootsInvalidMessage,
                         laige::log::field("key", key),
                         laige::log::field("value_kind",
                                           jsonKindName(root)));
          return ErrorCode::InvalidArgument;
        }
        config.assetRoots.emplace_back(root.asString());
      }
    } else {
      // Unknown top-level key: WARN (forward-compat) and ignore —
      // never silent (CORE-008).
      LAIGE_LOG_WARN(kConfigSubsystem, "unknown_key",
                     kConfigUnknownKeyMessage,
                     laige::log::field("key", key));
    }
  }
  return config;
}

// ---------------------------------------------------------------------------
// loadGameConfig (file -> document -> typed config)
// ---------------------------------------------------------------------------

Result<EngineConfig, ErrorCode> loadGameConfig(std::string_view path) noexcept {
  std::string bytes;
  const Status readStatus = readFileBounded(path, &bytes);
  if (readStatus.isError()) return readStatus.error();  // IoError / MalformedInput
  const Result<JsonValue, ErrorCode> parsed = parseJson(bytes);
  if (parsed.isError()) return parsed.error();  // MalformedInput (ADR 0003)
  return parseEngineConfig(parsed.value());
}

// ---------------------------------------------------------------------------
// applyConfigOverride (the programmatic override-of-a-subset merge)
// ---------------------------------------------------------------------------

Result<EngineConfig, ErrorCode> applyConfigOverride(
    const EngineConfig& base, const EngineConfigOverride& override) noexcept {
  EngineConfig config = base;  // value semantics: the base is untouched
  // Every set field is validated in its documented domain, first set
  // field that fails wins (the struct's declaration order).
  if (override.tickRateHz) {
    if (*override.tickRateHz < kMinTickRateHz ||
        *override.tickRateHz > kMaxTickRateHz) {
      LAIGE_LOG_WARN(kConfigSubsystem, "tick_rate_invalid",
                     kConfigTickRateInvalidMessage,
                     laige::log::field("value", *override.tickRateHz));
      return ErrorCode::InvalidArgument;
    }
    config.tickRateHz = *override.tickRateHz;
  }
  if (override.entityCapacity) {
    if (*override.entityCapacity > Entity::kMaxEntities) {
      LAIGE_LOG_WARN(kConfigSubsystem, "entity_budget_invalid",
                     kConfigEntityBudgetInvalidMessage,
                     laige::log::field("value", *override.entityCapacity));
      return ErrorCode::InvalidArgument;
    }
    config.entityCapacity = *override.entityCapacity;
  }
  if (override.churnPerFrameBudget) {
    // uint32: every value is in the >= 0 domain — no check needed.
    config.churnPerFrameBudget = *override.churnPerFrameBudget;
  }
  if (override.seed) {
    // uint64: the programmatic surface accepts the full 64 bits (no
    // 2^53 JSON bound — the config.h table).
    config.seed = *override.seed;
  }
  if (override.determinismEnabled) {
    config.determinism.enabled = *override.determinismEnabled;
  }
  if (override.determinismMath) {
    config.determinism.math = *override.determinismMath;
  }
  if (override.systemTimeDefaultMs) {
    const double ms = *override.systemTimeDefaultMs;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
    if (!std::isfinite(ms) || ms <= 0.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
      LAIGE_LOG_WARN(kConfigSubsystem, "system_time_budget_invalid",
                     kConfigSystemTimeBudgetInvalidMessage,
                     laige::log::field("value", ms));
      return ErrorCode::InvalidArgument;
    }
    config.budgets.systemTimeDefaultMs = ms;
  }
  if (override.drawCallsPerFrame) {
    config.budgets.drawCallsPerFrame = *override.drawCallsPerFrame;
  }
  if (override.particlesPerFrame) {
    config.budgets.particlesPerFrame = *override.particlesPerFrame;
  }
  if (override.fovDegrees) {
    const double fov = *override.fovDegrees;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
    if (!std::isfinite(fov) || fov <= 0.0 || fov > 180.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
      LAIGE_LOG_WARN(kConfigSubsystem, "camera_fov_invalid",
                     kConfigCameraFovInvalidMessage,
                     laige::log::field("value", fov));
      return ErrorCode::InvalidArgument;
    }
    config.camera.fovDegrees = fov;
  }
  if (override.zoom) {
    const double zoom = *override.zoom;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
    if (!std::isfinite(zoom) || zoom <= 0.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
      LAIGE_LOG_WARN(kConfigSubsystem, "camera_zoom_invalid",
                     kConfigCameraZoomInvalidMessage,
                     laige::log::field("value", zoom));
      return ErrorCode::InvalidArgument;
    }
    config.camera.zoom = zoom;
  }
  if (override.followLerpPerSec) {
    const double lerp = *override.followLerpPerSec;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
    if (!std::isfinite(lerp) || lerp < 0.0) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time value (ADR 0003 JSON number policy), not sim math
      LAIGE_LOG_WARN(kConfigSubsystem, "camera_follow_lerp_invalid",
                     kConfigCameraFollowLerpInvalidMessage,
                     laige::log::field("value", lerp));
      return ErrorCode::InvalidArgument;
    }
    config.camera.followLerpPerSec = lerp;
  }
  if (override.assetRoots) {
    for (const std::string& root : *override.assetRoots) {
      if (root.empty()) {
        LAIGE_LOG_WARN(kConfigSubsystem, "asset_roots_invalid",
                       kConfigAssetRootsInvalidMessage,
                       laige::log::field("key", "asset_roots"));
        return ErrorCode::InvalidArgument;
      }
    }
    config.assetRoots = *override.assetRoots;  // the cold-path copy
  }
  return config;
}

// ---------------------------------------------------------------------------
// ConfigHotReloader (debug builds only — FR-1.5)
// ---------------------------------------------------------------------------

// Emits the hot_reload_rejected ERROR for one changed sim-affecting
// key and returns the failure Status (the caller's config and the
// reloader's baseline are untouched — restart to apply, FR-1.5).
// Debug-only (NDEBUG-guarded like the message constants it emits): the
// Release poll/create paths return early and never reach this helper,
// so compiling it out of Release keeps both trees warning-free
// (the unguarded function would name a constant the header does not
// declare under NDEBUG — an error — and be -Wunused-function if
// declared).
#ifndef NDEBUG
Status rejectSimKey(const char* key, const log::Field& oldField,
                    const log::Field& newField) noexcept {
  LAIGE_LOG_ERROR(kConfigSubsystem, "hot_reload_rejected",
                  kConfigHotReloadRejectedMessage,
                  laige::log::field("key", key), oldField, newField);
  return ErrorCode::InvalidArgument;
}
#endif

Result<ConfigHotReloader, ErrorCode> ConfigHotReloader::create(
    std::string_view path) noexcept {
#if defined(NDEBUG)
  // Hot reload is development tooling: release builds reject the
  // feature call (the replay/record_disabled pattern).
  LAIGE_LOG_WARN(kConfigSubsystem, "hot_reload_disabled",
                 kConfigHotReloadDisabledMessage);
  return ErrorCode::InvalidArgument;
#else
  std::string bytes;
  const Status readStatus = readFileBounded(path, &bytes);
  if (readStatus.isError()) return readStatus.error();  // IoError / MalformedInput
  const Result<JsonValue, ErrorCode> parsed = parseJson(bytes);
  if (parsed.isError()) return parsed.error();  // MalformedInput (ADR 0003)
  Result<EngineConfig, ErrorCode> validated =
      parseEngineConfig(parsed.value());
  if (validated.isError()) return validated.error();
  ConfigHotReloader reloader;
  reloader.path_ = std::string(path);
  reloader.baselineBytes_ = std::move(bytes);
  reloader.baselineConfig_ = std::move(validated).takeValue();
  return reloader;
#endif
}

Status ConfigHotReloader::poll(EngineConfig& config) noexcept {
#if defined(NDEBUG)
  (void)config;
  LAIGE_LOG_WARN(kConfigSubsystem, "hot_reload_disabled",
                 kConfigHotReloadDisabledMessage);
  return ErrorCode::InvalidArgument;
#else
  if (movedFrom_) return ErrorCode::InvalidArgument;  // no log (stopped state)
  std::string bytes;
  const Status readStatus = readFileBounded(path_, &bytes);
  if (readStatus.isError()) {
    // Transient (locked/deleted/oversized file): the previous config
    // stays in effect; the next poll retries.
    LAIGE_LOG_WARN(kConfigSubsystem, "hot_reload_read_failed",
                   kConfigHotReloadReadFailedMessage,
                   laige::log::field("path", path_));
    return readStatus;
  }
  if (bytes == baselineBytes_) return Status{};  // unchanged: no event
  const Result<JsonValue, ErrorCode> parsed = parseJson(bytes);
  if (parsed.isError()) return parsed.error();  // MalformedInput; no change
  Result<EngineConfig, ErrorCode> freshResult =
      parseEngineConfig(parsed.value());
  if (freshResult.isError()) return freshResult.error();  // no change
  const EngineConfig& fresh = freshResult.value();
  const EngineConfig& base = baselineConfig_;
  // The simulation-affecting comparison (the documented key order; the
  // version cannot differ between two parseable documents — a
  // different version fails the gate above — so the set starts at the
  // tick rate).
  if (fresh.tickRateHz != base.tickRateHz) {
    return rejectSimKey("tick_rate_hz",
                        log::field("old", base.tickRateHz),
                        log::field("new", fresh.tickRateHz));
  }
  if (fresh.entityCapacity != base.entityCapacity) {
    return rejectSimKey("entity_budget",
                        log::field("old", base.entityCapacity),
                        log::field("new", fresh.entityCapacity));
  }
  if (fresh.churnPerFrameBudget != base.churnPerFrameBudget) {
    return rejectSimKey("churn_per_frame_budget",
                        log::field("old", base.churnPerFrameBudget),
                        log::field("new", fresh.churnPerFrameBudget));
  }
  if (fresh.seed != base.seed) {
    return rejectSimKey("seed", log::field("old", base.seed),
                        log::field("new", fresh.seed));
  }
  if (fresh.determinism.enabled != base.determinism.enabled) {
    return rejectSimKey("determinism.enabled",
                        log::field("old", base.determinism.enabled),
                        log::field("new", fresh.determinism.enabled));
  }
  if (fresh.determinism.math != base.determinism.math) {
    return rejectSimKey(
        "determinism.math",
        log::field("old", static_cast<std::uint32_t>(base.determinism.math)),
        log::field("new",
                   static_cast<std::uint32_t>(fresh.determinism.math)));
  }
  if (fresh.budgets.systemTimeDefaultMs !=
      base.budgets.systemTimeDefaultMs) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time comparison of declared budget values, not sim math
    return rejectSimKey(
        "budgets.system_time_default_ms",
        log::field("old", base.budgets.systemTimeDefaultMs),
        log::field("new", fresh.budgets.systemTimeDefaultMs));
  }
  // Only non-simulation keys changed: apply them in place (the file is
  // now authoritative for them — a programmatic override of those
  // fields is replaced, the config.h contract) and advance the
  // baseline.
  std::vector<std::string> changed;
  if (fresh.budgets.drawCallsPerFrame != base.budgets.drawCallsPerFrame) {
    config.budgets.drawCallsPerFrame = fresh.budgets.drawCallsPerFrame;
    changed.push_back("budgets.draw_calls_per_frame");
  }
  if (fresh.budgets.particlesPerFrame != base.budgets.particlesPerFrame) {
    config.budgets.particlesPerFrame = fresh.budgets.particlesPerFrame;
    changed.push_back("budgets.particles_per_frame");
  }
  if (fresh.camera.fovDegrees != base.camera.fovDegrees) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time comparison of declared camera defaults, not sim math
    config.camera.fovDegrees = fresh.camera.fovDegrees;
    changed.push_back("camera.fov_degrees");
  }
  if (fresh.camera.zoom != base.camera.zoom) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time comparison of declared camera defaults, not sim math
    config.camera.zoom = fresh.camera.zoom;
    changed.push_back("camera.zoom");
  }
  if (fresh.camera.followLerpPerSec != base.camera.followLerpPerSec) {  // LAIGE-DETERM-EXCEPTION: G-R8 config-time comparison of declared camera defaults, not sim math
    config.camera.followLerpPerSec = fresh.camera.followLerpPerSec;
    changed.push_back("camera.follow_lerp_per_sec");
  }
  if (fresh.assetRoots != base.assetRoots) {
    config.assetRoots = fresh.assetRoots;
    changed.push_back("asset_roots");
  }
  if (!changed.empty()) {
    // One Info event naming every changed key (bounded: <= 6 keys; the
    // joined string is a cold debug-path allocation).
    std::string keys;
    for (const std::string& key : changed) {
      if (!keys.empty()) keys.push_back(',');
      keys += key;
    }
    LAIGE_LOG_INFO(kConfigSubsystem, "hot_reload_applied",
                   kConfigHotReloadAppliedMessage,
                   laige::log::field("keys", keys),
                   laige::log::field("count", changed.size()));
  }
  baselineBytes_ = std::move(bytes);
  baselineConfig_ = std::move(freshResult).takeValue();
  return Status{};
#endif
}

ConfigHotReloader::ConfigHotReloader(ConfigHotReloader&& other) noexcept
    : path_(std::move(other.path_)),
      baselineBytes_(std::move(other.baselineBytes_)),
      baselineConfig_(std::move(other.baselineConfig_)),
      movedFrom_(other.movedFrom_) {
  other.movedFrom_ = true;
}

ConfigHotReloader& ConfigHotReloader::operator=(
    ConfigHotReloader&& other) noexcept {
  if (this != &other) {
    path_ = std::move(other.path_);
    baselineBytes_ = std::move(other.baselineBytes_);
    baselineConfig_ = std::move(other.baselineConfig_);
    movedFrom_ = other.movedFrom_;
    other.movedFrom_ = true;
  }
  return *this;
}

}  // namespace laige
