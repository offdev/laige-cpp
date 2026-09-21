// laige-sim declarative game config (M1-CFG-01).
//
// FR-1.5 (declarative game config in JSON: tick rate, budgets, camera
// defaults, asset roots; runtime config overrides; hot-reload of
// non-simulation config in debug); ARCH-007 (persistent data is
// versioned; readers reject unsupported versions explicitly); ADR 0003
// (the bounded JSON parser + the JSON-number policy); ADR 0002 (the
// determinism block: the mode flag + the SimMath backend ids, consumed
// by M1-DET-01); PRD §7.1, §10.2 (single-threaded sim), §10.3.
//
// This header is the FINAL config surface. The M1-HEAD-01 provisional
// `parseEngineConfig` surface (declared in engine.h) is folded into it:
// the provisional keys carried over unchanged, and the schema gains the
// REQUIRED "version" key plus the budgets / camera / asset_roots
// blocks. `EngineConfig` itself moved from engine.h to this header
// (engine.h now includes it); the five original members keep their
// order, so existing aggregate initializers still compile.
//
// This header carries:
//
//   EngineConfig           The typed configuration the engine consumes
//                          (sim-affecting fields + the declared
//                          presentation blocks below).
//   BudgetsConfig          The declared budget block (the per-system
//                          default time budget + the draw/particle
//                          budgets, declared before their consumers
//                          exist).
//   CameraConfig           The camera defaults (stored now, consumed
//                          in M2).
//   kSupportedConfigVersion
//                          The only config-document version this build
//                          accepts (ARCH-007).
//   parseEngineConfig      The versioned JSON-document -> EngineConfig
//                          loader (the rejection table below).
//   loadGameConfig         The file -> EngineConfig loader (bounded
//                          read + parse + version + schema validation).
//   EngineConfigOverride + applyConfigOverride
//                          The programmatic override-of-a-subset merge
//                          API (FR-1.5 "runtime config overrides").
//   ConfigHotReloader      The debug-build-only file-watch hot reload
//                          of non-simulation keys (FR-1.5).
//
// ---------------------------------------------------------------------------
// The version 1 schema (the full key table: docs/api/config.md)
// ---------------------------------------------------------------------------
//
//   Top level: a JSON OBJECT. The "version" key is REQUIRED and must be
//   the exact integer 1 (kSupportedConfigVersion); it is checked before
//   any other key. Every other key is optional (missing keys take the
//   defaults below); an unknown key is WARNED (one config/unknown_key
//   per key, forward-compat) and ignored. First failure wins: the
//   loader stops at the first rejected key in document order and
//   returns the error (no config is produced on failure, CORE-008).
//
//   key                           type / range               default
//   "version"                     exact integer == 1         REQUIRED
//   "tick_rate_hz"                exact integer, 20..120     kDefaultTickRateHz (60)
//   "entity_budget"               exact integer, 0..65536    0 (empty scene)
//   "churn_per_frame_budget"      exact integer, >= 0        kDefaultChurnPerFrameBudget (256)
//   "seed"                        exact integer, 0..2^53     kDefaultSimulationSeed (0)
//   "determinism"                 object (below)             {}
//     "enabled"                   bool                       true
//     "math"                      "fixed_point_16_16" |
//                                 "float_pinned_32"          "fixed_point_16_16"
//   "budgets"                     object (below)             {}
//     "system_time_default_ms"    number, finite, > 0        kDefaultSystemTimeBudgetMs (1.5)
//     "draw_calls_per_frame"      exact integer, >= 0        kDefaultDrawCallsPerFrame (30)
//     "particles_per_frame"       exact integer, >= 0        kDefaultParticlesPerFrame (4096)
//   "camera"                      object (below)             {}
//     "fov_degrees"               number, finite, 0 < v <= 180  kDefaultCameraFovDegrees (45)
//     "zoom"                      number, finite, > 0        kDefaultCameraZoom (1)
//     "follow_lerp_per_sec"       number, finite, >= 0       kDefaultCameraFollowLerpPerSec (8)
//   "asset_roots"                 array of non-empty strings []
//
//   The budgets and camera blocks are DECLARED values in M1: the scene
//   entity budget already has its consumer (World capacity, G-R3);
//   system_time_default_ms (the per-system default time budget for the
//   M1-PROF-02 budget report), the draw/particle budgets (the M2
//   render/particle consumers), and the camera defaults (the M2 camera
//   work) are stored and validated now, consumed later (FR-1.5: values
//   stored; the consumers land in M2).
//
//   Number policy (ADR 0003): JSON numbers are doubles; the exact-
//   integer keys are validated to be integral and in-range, and the
//   bounded 2^53 exactness of doubles is the seed's upper bound.
//   Non-finite values (a literal beyond double range parses to +/-inf)
//   are rejected by every numeric check.
//
// ---------------------------------------------------------------------------
// Versioning (ARCH-007)
// ---------------------------------------------------------------------------
//
//   The config document is persistent data and is VERSIONED. This
//   build accepts exactly version 1; rejection is explicit, never a
//   silent migration:
//
//     "version" absent              InvalidArgument + warn
//                                    config/version_missing
//     "version" not an exact
//       non-negative integer        InvalidArgument + warn
//                                    config/version_invalid
//     "version" != 1                InvalidArgument + warn
//                                    config/version_unsupported
//
//   Documents from the M1-HEAD-01 provisional surface (no "version"
//   key) are rejected by the version_missing rule; the migration is to
//   add "version": 1 (every other key is unchanged — see
//   docs/api/config.md, "Migration from the provisional surface").
//
//   The replay identity's configHash (replay.h, ADR 0002) covers ONLY
//   the simulation-affecting fields — tick rate, entity budget, churn
//   budget, seed, and the determinism block. The declared presentation
//   values (the budgets block, the camera block, the asset roots) never
//   touch simulation and are deliberately EXCLUDED from the identity:
//   a replay recorded under one set of those values replays bit-exact
//   under another. The hashed field encoding (its tag word 1 included)
//   therefore does not change with this step, and every committed
//   baseline/replay log stays valid.
//
// ---------------------------------------------------------------------------
// Loading and overrides (cold path, PERF-002/003)
// ---------------------------------------------------------------------------
//
//   loadGameConfig(path) reads the file (bounded: kMaxConfigDocumentBytes
//   = 1 MiB, the ADR 0003 bound; an oversized file is MalformedInput,
//   not a truncated parse), parses it (laige-core JSON), and runs the
//   schema validation above. It allocates (a document-sized buffer, the
//   config's string storage) — config loading is a startup/dev-time
//   operation, never on the sim tick path.
//
//   applyConfigOverride(base, ov) merges a SUBSET override (FR-1.5):
//   a std::nullopt field is kept from the base; a set field replaces it
//   and is validated in its documented domain (a set field outside the
//   domain is InvalidArgument — the same events as the JSON path).
//   First set field that fails wins (the struct's declaration order).
//
//   ConfigHotReloader (debug builds only; release builds reject with
//   config/hot_reload_disabled — the replay/record_disabled pattern)
//   watches the config file: a poll re-reads the file and compares the
//   bytes to the baseline. A change whose simulation-affecting fields
//   are all unchanged applies the non-simulation fields to the caller's
//   EngineConfig in place (the file becomes authoritative for them — a
//   programmatic override of those fields is replaced) and logs
//   config/hot_reload_applied. A change of ANY simulation-affecting
//   field is REFUSED with InvalidArgument + the config/hot_reload_rejected
//   ERROR naming the first differing key; the config and the baseline
//   are left untouched (restart the engine to apply it, FR-1.5).
//
//   The simulation-affecting set (restart-required on hot reload):
//   version, tick_rate_hz, entity_budget, churn_per_frame_budget,
//   seed, determinism.enabled, determinism.math, and
//   budgets.system_time_default_ms (it governs the G-R5 budget
//   enforcement during ticks). The non-simulation set (hot-reloadable):
//   budgets.draw_calls_per_frame, budgets.particles_per_frame,
//   camera.fov_degrees, camera.zoom, camera.follow_lerp_per_sec, and
//   asset_roots.
//
// ---------------------------------------------------------------------------
// Ownership, threading, performance
// ---------------------------------------------------------------------------
//
//   EngineConfig is a plain value (copied into Engine::create; the
//   assetRoots vector is its only heap member). The loader functions are
//   free functions over values — no hidden state.
//
//   ConfigHotReloader is MOVE-ONLY (it owns the baseline bytes and the
//   baseline config), single-owner-thread (the game's loop thread — the
//   sim is single-threaded, PRD §10.2), and creates no thread
//   (CONC-005): the caller drives the poll cadence (typically once per
//   frame in a dev loop). A moved-from reloader's poll fails with
//   InvalidArgument and no log (the stopped-state precedent).
//
//   Performance (the cold paths): loadGameConfig is O(file bytes) once
//   at startup. applyConfigOverride is O(1) plus the assetRoots copy
//   when that field is set. A hot-reload poll is O(file bytes) per call
//   (one bounded read + one byte comparison; a detected change adds one
//   parse + one field comparison) — debug-only and opt-in (the
//   reloader exists only when constructed, so the disabled cost is
//   zero, DBG-004), and it never runs on the sim tick path.
//
// ---------------------------------------------------------------------------
// Error events (LOG-001 stable names; LOG-002 5-field messages;
// docs/api/config.md carries the full table)
// ---------------------------------------------------------------------------
//
//   config/version_missing (Warn)         config/version_invalid (Warn)
//   config/version_unsupported (Warn)     config/not_an_object (Warn)
//   config/tick_rate_invalid (Warn)       config/entity_budget_invalid (Warn)
//   config/churn_budget_invalid (Warn)    config/seed_invalid (Warn)
//   config/determinism_invalid (Warn)     config/determinism_enabled_invalid (Warn)
//   config/determinism_math_invalid (Warn)
//   config/budgets_invalid (Warn)         config/system_time_budget_invalid (Warn)
//   config/draw_calls_budget_invalid (Warn)
//   config/particles_budget_invalid (Warn)
//   config/camera_invalid (Warn)          config/camera_fov_invalid (Warn)
//   config/camera_zoom_invalid (Warn)     config/camera_follow_lerp_invalid (Warn)
//   config/asset_roots_invalid (Warn)     config/unknown_key (Warn)
//   config/file_read_failed (Warn)        config/hot_reload_rejected (Error)
//   config/hot_reload_applied (Info)      config/hot_reload_read_failed (Warn)
//   config/hot_reload_disabled (Warn)     (release builds only)
//
//   Every rejection is a `Status`/`Result` error RETURNED to the caller
//   (never silent, CORE-008); the warns carry the NFR-13.3 5-field
//   message (build-stable text; the dynamic values are structured
//   fields, never message text).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "laige/errors.h"
#include "laige/result.h"
#include "laige/sim/determinism.h"  // DeterminismConfig, SimMathBackend
#include "laige/sim/entity.h"  // Entity::kMaxEntities, kDefaultChurnPerFrameBudget
#include "laige/sim/game_loop.h"  // kMinTickRateHz, kDefaultTickRateHz, kMaxTickRateHz

namespace laige {

// The full JsonValue definition is laige/json.h; only a const reference
// is used here (CPP-010: no transitive include).
class JsonValue;

// The only config-document version this build accepts (ARCH-007:
// readers reject or migrate unsupported data explicitly — the
// rejection table in the header preamble).
inline constexpr std::uint32_t kSupportedConfigVersion = 1;

// The default master simulation seed (M1-DET-01: the PRNG substreams
// derive from it; it is part of the replay identity, ADR 0002).
inline constexpr std::uint64_t kDefaultSimulationSeed = 0;

// The per-system default time budget in milliseconds (G-R5). 1.5 ms is
// half of the 3.0 ms sim_tick_avg budget (budgets.json, PRD §8.1): at
// 60 Hz, a scene whose systems all take their defaults still fits the
// tick budget with margin. Declared value in M1 (no scheduler consumer
// yet — the per-system default lands with M1-PROF-02's budget report).
inline constexpr double kDefaultSystemTimeBudgetMs = 1.5;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time constant: a declared budget value, never sim state or math

// The declared per-frame draw-call budget. 30 is the PRD §8.1 budget
// for the worst-case busy scene (50k visible sprites, 3 parallax
// layers, UI — budgets.json `sprites_50k_draw_calls`). Declared value
// in M1; the M2 batcher consumes it.
inline constexpr std::uint32_t kDefaultDrawCallsPerFrame = 30;

// The declared per-frame particle budget (FR-2.7: budgeted, pooled).
// No measured M1 consumer; 4096 is a power-of-two default sized for a
// 60 fps pool (the M2 particle system validates it against the
// platform).
inline constexpr std::uint32_t kDefaultParticlesPerFrame = 4096;

// The default perspective field of view in degrees (FR-2.4/FR-2.5).
// 45 degrees is the 2.5D default for the perspective projection modes;
// the isometric ortho presets (the PRD primary) use `zoom` instead.
// Declared value in M1; the M2 camera work consumes it.
inline constexpr double kDefaultCameraFovDegrees = 45.0;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time constant: a declared default, never sim state or math

// The default camera zoom (world-to-screen scale factor).
inline constexpr double kDefaultCameraZoom = 1.0;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time constant: a declared default, never sim state or math

// The default smooth-follow rate in 1/s (FR-2.4 "smooth follow"): the
// per-frame follow factor is 1 - exp(-8*dt), about a 125 ms time
// constant at 60 Hz. Declared value in M1; the M2 camera work
// consumes it.
inline constexpr double kDefaultCameraFollowLerpPerSec = 8.0;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time constant: a declared default, never sim state or math

// The declared budget block (M1-CFG-01; FR-1.5 "budgets").
//
// All three fields are validated at load time; only the scene entity
// budget (a sibling key, World capacity) has an M1 consumer.
struct BudgetsConfig {
  // The per-system default time budget in milliseconds (G-R5).
  double systemTimeDefaultMs{kDefaultSystemTimeBudgetMs};  // LAIGE-DETERM-EXCEPTION: G-R8 a declared budget value, not sim math
  // The declared per-frame draw-call budget (the M2 batcher's budget).
  std::uint32_t drawCallsPerFrame{kDefaultDrawCallsPerFrame};
  // The declared per-frame particle budget (the M2 particle system's
  // budget).
  std::uint32_t particlesPerFrame{kDefaultParticlesPerFrame};
};

// The camera defaults (M1-CFG-01; FR-1.5 "camera defaults"). Stored now,
// consumed in M2 (the camera work) — see the header preamble.
struct CameraConfig {
  // The perspective field of view in degrees (0 < fov <= 180).
  double fovDegrees{kDefaultCameraFovDegrees};  // LAIGE-DETERM-EXCEPTION: G-R8 a declared default, not sim math
  // The world-to-screen zoom factor (> 0).
  double zoom{kDefaultCameraZoom};  // LAIGE-DETERM-EXCEPTION: G-R8 a declared default, not sim math
  // The smooth-follow rate in 1/s (>= 0; 0 disables smoothing).
  double followLerpPerSec{kDefaultCameraFollowLerpPerSec};  // LAIGE-DETERM-EXCEPTION: G-R8 a declared default, not sim math
};

// The typed game configuration the engine consumes (M1-CFG-01, the
// final schema; the five M1-HEAD-01 members keep their order — existing
// aggregate initializers compile unchanged).
//
// Simulation-affecting fields (part of the replay identity's
// configHash, replay.h; hot-reload-refused): tickRateHz,
// entityCapacity, churnPerFrameBudget, seed, determinism.
//
// Declared presentation fields (validated, stored, no M1 consumer;
// hot-reloadable in debug): budgets, camera, assetRoots.
struct EngineConfig {
  // The simulation tick rate in Hz (kMinTickRateHz..kMaxTickRateHz —
  // 20..120; the GameLoop re-validates the same range, game_loop.h).
  std::uint32_t tickRateHz{kDefaultTickRateHz};
  // The scene's entity budget in entities (0..Entity::kMaxEntities —
  // the G-R3 scene budget; 0 is the empty scene).
  std::uint32_t entityCapacity{0};
  // The per-frame component-churn budget (the G-R4 guardrail; 0
  // disables it).
  std::uint32_t churnPerFrameBudget{kDefaultChurnPerFrameBudget};
  // The master simulation seed (the PRNG substreams derive from it,
  // M1-DET-01; part of the replay identity, ADR 0002). The programmatic
  // surface accepts the full 64 bits (the JSON surface is bounded to
  // 2^53 by the ADR 0003 number policy — see the parse table).
  std::uint64_t seed{kDefaultSimulationSeed};
  // The determinism block (M1-DET-01; ADR 0002).
  DeterminismConfig determinism{};
  // The declared budget block (FR-1.5).
  BudgetsConfig budgets{};
  // The camera defaults (FR-1.5; consumed in M2).
  CameraConfig camera{};
  // The asset root paths (FR-1.5; the asset pipeline — M2 — resolves
  // them). Existence is NOT checked at config load (a root may be
  // created later; the pipeline owns the check).
  std::vector<std::string> assetRoots;
};

// The NFR-13.3 5-field rejection messages (build-stable; the doc
// anchor is docs/api/config.md — the schema's API document).
// engine.cpp's Engine::create tick-rate re-validation shares
// kConfigTickRateInvalidMessage (one text for one rejection, LOG-002).
inline constexpr const char* kConfigSubsystem = "config";
inline constexpr const char* kConfigNotAnObjectMessage =
    "not_an_object | the config document is not a JSON object | the "
    "game config must be a top-level object | wrap the config in a "
    "top-level object ({} plus \"version\": 1 for all defaults) | "
    "docs/api/config.md";
inline constexpr const char* kConfigVersionMissingMessage =
    "version_missing | the config document has no version key | the "
    "document predates the versioned schema (the M1-HEAD-01 provisional "
    "surface) or the key was removed | add \"version\": 1 to the config "
    "(docs/api/config.md) | docs/api/config.md";
inline constexpr const char* kConfigVersionInvalidMessage =
    "version_invalid | the config version key is not an exact "
    "non-negative integer | version must be a JSON number holding an "
    "exact integer | set version to 1 | docs/api/config.md";
inline constexpr const char* kConfigVersionUnsupportedMessage =
    "version_unsupported | the config document version is not "
    "supported by this build | this build accepts version 1 only "
    "(ARCH-007) | rebuild with the engine version matching the "
    "document, or downgrade the document to version 1 | "
    "docs/api/config.md";
inline constexpr const char* kConfigTickRateInvalidMessage =
    "tick_rate_invalid | the configured tick_rate_hz is invalid | the "
    "value must be an exact integer in the 20-120 Hz range | set "
    "tick_rate_hz to a value in 20-120 (the default is 60) | "
    "docs/api/config.md";
inline constexpr const char* kConfigEntityBudgetInvalidMessage =
    "entity_budget_invalid | the configured entity_budget is invalid | "
    "the value must be an exact integer in 0-65536 (the 16-bit entity "
    "id space) | set entity_budget to a value in 0-65536 (the scene's "
    "declared budget, G-R3) | docs/api/config.md";
inline constexpr const char* kConfigChurnBudgetInvalidMessage =
    "churn_budget_invalid | the configured churn_per_frame_budget is "
    "invalid | the value must be a non-negative exact integer | set "
    "churn_per_frame_budget to a non-negative integer (the default is "
    "256; 0 disables the G-R4 guardrail) | docs/api/config.md";
inline constexpr const char* kConfigSeedInvalidMessage =
    "seed_invalid | the configured seed is invalid | the value must be "
    "an exact integer in 0-2^53 (the ADR 0003 JSON bound: doubles are "
    "exact to 2^53; the programmatic EngineConfig.seed accepts the "
    "full 64 bits) | set seed to an integer in 0-2^53 (the default is "
    "0) | docs/api/config.md";
inline constexpr const char* kConfigDeterminismInvalidMessage =
    "determinism_invalid | the configured determinism block is invalid "
    "| the block must be a JSON object ({} for all defaults); the "
    "nested keys enabled (bool) and math (string) have their own "
    "checks | wrap the determinism block in an object | "
    "docs/api/config.md";
inline constexpr const char* kConfigDeterminismEnabledInvalidMessage =
    "determinism_enabled_invalid | the configured determinism.enabled "
    "is invalid | the value must be a JSON boolean (the default is "
    "true — deterministic by default, S-7) | set determinism.enabled to "
    "true or false | docs/api/config.md";
inline constexpr const char* kConfigDeterminismMathInvalidMessage =
    "determinism_math_invalid | the configured determinism.math is "
    "invalid | the value must be the string \"fixed_point_16_16\" "
    "(default) or \"float_pinned_32\" (the ADR 0002 backend ids) | set "
    "determinism.math to one of the two backend ids | "
    "docs/api/config.md";
inline constexpr const char* kConfigBudgetsInvalidMessage =
    "budgets_invalid | the configured budgets block is invalid | the "
    "block must be a JSON object ({} for all defaults); the nested "
    "keys have their own checks | wrap the budgets block in an object "
    "| docs/api/config.md";
inline constexpr const char* kConfigSystemTimeBudgetInvalidMessage =
    "system_time_budget_invalid | the configured budgets."
    "system_time_default_ms is invalid | the value must be a finite "
    "number > 0 (milliseconds; the per-system default time budget, "
    "G-R5) | set system_time_default_ms to a positive finite value "
    "(the default is 1.5) | docs/api/config.md";
inline constexpr const char* kConfigDrawCallsBudgetInvalidMessage =
    "draw_calls_budget_invalid | the configured budgets."
    "draw_calls_per_frame is invalid | the value must be a non-"
    "negative exact integer (the declared per-frame draw-call budget; "
    "the M2 batcher consumes it) | set draw_calls_per_frame to a non-"
    "negative integer (the default is 30) | docs/api/config.md";
inline constexpr const char* kConfigParticlesBudgetInvalidMessage =
    "particles_budget_invalid | the configured budgets."
    "particles_per_frame is invalid | the value must be a non-"
    "negative exact integer (the declared per-frame particle budget; "
    "the M2 particle system consumes it) | set particles_per_frame to "
    "a non-negative integer (the default is 4096) | "
    "docs/api/config.md";
inline constexpr const char* kConfigCameraInvalidMessage =
    "camera_invalid | the configured camera block is invalid | the "
    "block must be a JSON object ({} for all defaults); the nested "
    "keys have their own checks | wrap the camera block in an object | "
    "docs/api/config.md";
inline constexpr const char* kConfigCameraFovInvalidMessage =
    "camera_fov_invalid | the configured camera.fov_degrees is invalid "
    "| the value must be a finite number in (0, 180] degrees | set "
    "fov_degrees to a finite value in (0, 180] (the default is 45) | "
    "docs/api/config.md";
inline constexpr const char* kConfigCameraZoomInvalidMessage =
    "camera_zoom_invalid | the configured camera.zoom is invalid | the "
    "value must be a finite number > 0 | set zoom to a finite value > 0 "
    "(the default is 1) | docs/api/config.md";
inline constexpr const char* kConfigCameraFollowLerpInvalidMessage =
    "camera_follow_lerp_invalid | the configured camera."
    "follow_lerp_per_sec is invalid | the value must be a finite "
    "number >= 0 (the smooth-follow rate in 1/s, FR-2.4) | set "
    "follow_lerp_per_sec to a finite value >= 0 (the default is 8) | "
    "docs/api/config.md";
inline constexpr const char* kConfigAssetRootsInvalidMessage =
    "asset_roots_invalid | the configured asset_roots value is invalid "
    "| asset_roots must be a JSON array of non-empty strings | set "
    "asset_roots to an array of path strings ([] for none) | "
    "docs/api/config.md";
inline constexpr const char* kConfigUnknownKeyMessage =
    "unknown_key | the config key is not part of the version 1 config "
    "schema | the key is not (yet) consumed by this build | remove the "
    "key, or wait for a later schema version (forward-compat) | "
    "docs/api/config.md";
inline constexpr const char* kConfigFileReadFailedMessage =
    "file_read_failed | the config file could not be read | the path "
    "does not exist, is not readable, or is over the 1 MiB bound (ADR "
    "0003) | check the path, permissions, and file size | "
    "docs/api/config.md";
// The hot-reload messages (debug builds except the _disabled one, which
// is release-only — the guards keep both trees warning-free).
#ifndef NDEBUG
inline constexpr const char* kConfigHotReloadRejectedMessage =
    "hot_reload_rejected | the config hot reload was refused: a "
    "simulation-affecting key changed | sim-affecting keys (tick rate, "
    "budgets, seed, determinism) change the simulation and require a "
    "restart (FR-1.5) | restart the engine with the new config; only "
    "non-simulation keys are hot-reloadable | docs/api/config.md";
inline constexpr const char* kConfigHotReloadAppliedMessage =
    "hot_reload_applied | the config file changed and its "
    "non-simulation keys were reloaded | the file's camera defaults, "
    "presentation budgets, and asset roots now apply to the live "
    "config | none (informational) | docs/api/config.md";
inline constexpr const char* kConfigHotReloadReadFailedMessage =
    "hot_reload_read_failed | the config file could not be re-read "
    "during a hot-reload poll | the file was locked, deleted, or is "
    "over the 1 MiB bound (a transient editor state) | the previous "
    "config stays in effect; retry on the next poll | "
    "docs/api/config.md";
#endif
#if defined(NDEBUG)
inline constexpr const char* kConfigHotReloadDisabledMessage =
    "hot_reload_disabled | config hot reload is a debug-build feature "
    "| this binary was built without debug support (NDEBUG defined — "
    "release build) | rebuild with CMAKE_BUILD_TYPE=Debug, or restart "
    "the engine with the new config | docs/api/config.md";
#endif

// Parses a version 1 config document into the typed config (the
// schema + rejection table in the header preamble; the full key table
// in docs/api/config.md).
//
//   doc not an object            -> InvalidArgument (config/not_an_object)
//   "version" missing            -> InvalidArgument (config/version_missing)
//   "version" not an exact       -> InvalidArgument (config/version_invalid)
//     non-negative integer
//   "version" != 1               -> InvalidArgument (config/version_unsupported)
//   any known key outside its    -> InvalidArgument (config/<key>_invalid)
//     documented domain
//   unknown key (any level)      -> WARN (config/unknown_key) and ignore
//
// First failure wins (document order); a failed parse produces no
// EngineConfig (CORE-008). Cold path: one O(document) walk.
[[nodiscard]]
Result<EngineConfig, ErrorCode> parseEngineConfig(const JsonValue& doc) noexcept;

// Reads the config file at `path`, parses it, and validates it against
// the version 1 schema. The read is bounded (kMaxConfigDocumentBytes =
// 1 MiB; an oversized file is MalformedInput, not a truncated parse).
//
//   file missing/unreadable      -> IoError (config/file_read_failed)
//   file over the bound          -> MalformedInput (config/file_read_failed)
//   JSON grammar violation       -> MalformedInput (the ADR 0003 parser)
//   schema rejection             -> InvalidArgument (the parse table)
//
// Cold path (startup/dev-time): O(file bytes) + the parse.
[[nodiscard]]
Result<EngineConfig, ErrorCode> loadGameConfig(std::string_view path) noexcept;

// A programmatic override of a SUBSET of an EngineConfig (FR-1.5
// "runtime config overrides"). A std::nullopt field is kept from the
// base config; a set field replaces it and is validated in its
// documented domain (a set field outside the domain is InvalidArgument
// — the same config/<key>_invalid events as the JSON path). First set
// field that fails wins (the declaration order below).
//
// Cold path: O(1) plus the assetRoots copy when that field is set.
struct EngineConfigOverride {
  // The simulation-affecting fields (the same domains as the JSON
  // schema): 20..120 Hz; 0..65536 entities; >= 0; full 64-bit seed
  // (no 2^53 JSON bound on the programmatic surface); the two backend
  // enums.
  std::optional<std::uint32_t> tickRateHz;
  std::optional<std::uint32_t> entityCapacity;
  std::optional<std::uint32_t> churnPerFrameBudget;
  std::optional<std::uint64_t> seed;
  std::optional<bool> determinismEnabled;
  std::optional<SimMathBackend> determinismMath;
  // The declared presentation fields: finite > 0 ms; >= 0; finite
  // (0, 180] degrees; finite > 0; finite >= 0; non-empty strings.
  std::optional<double> systemTimeDefaultMs;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time optional: a declared value, never sim math
  std::optional<std::uint32_t> drawCallsPerFrame;
  std::optional<std::uint32_t> particlesPerFrame;
  std::optional<double> fovDegrees;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time optional: a declared value, never sim math
  std::optional<double> zoom;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time optional: a declared value, never sim math
  std::optional<double> followLerpPerSec;  // LAIGE-DETERM-EXCEPTION: G-R8 config-time optional: a declared value, never sim math
  std::optional<std::vector<std::string>> assetRoots;
};

// Merges `override` into a copy of `base` (value semantics: the base
// is untouched) and validates every set field (the header preamble,
// Loading and overrides). Returns the merged config, or the first
// rejected set field's error (no config on failure).
[[nodiscard]]
Result<EngineConfig, ErrorCode> applyConfigOverride(
    const EngineConfig& base, const EngineConfigOverride& override) noexcept;

// The debug-build-only config hot reloader (FR-1.5; the contract in
// the header preamble, Loading and overrides).
//
//   create(path)  binds to the file and takes the baseline: the current
//                 bytes + a full validated parse. Read/parse/schema
//                 failure -> the error, no reloader (CORE-008).
//   poll(config)  re-reads the file and compares the bytes to the
//                 baseline:
//                   unchanged                     -> ok (no event)
//                   changed, sim-affecting
//                     fields all equal the
//                     baseline                    -> the non-sim fields
//                                                    (camera.*, the
//                                                    draw/particle
//                                                    budgets,
//                                                    asset_roots) are
//                                                    applied to `config`
//                                                    IN PLACE, the
//                                                    baseline advances,
//                                                    config/hot_reload_applied (Info)
//                                                    names the changed
//                                                    keys
//                   changed, any sim-affecting
//                     field differs               -> InvalidArgument +
//                                                    config/hot_reload_rejected
//                                                    (Error) naming the
//                                                    first differing key;
//                                                    `config` and the
//                                                    baseline are
//                                                    UNCHANGED (restart
//                                                    to apply)
//                   file no longer reads          -> IoError +
//                                                    config/hot_reload_read_failed
//                                                    (Warn); `config`
//                                                    unchanged (retry
//                                                    on the next poll)
//                   file no longer validates      -> the loader's error;
//                                                    `config` unchanged
//   Release builds (NDEBUG): create and poll both reject with
//   InvalidArgument + config/hot_reload_disabled (the
//   replay/record_disabled pattern — hot reload is dev tooling).
//
// Move-only, single-owner-thread, no internal thread (CONC-005); the
// caller drives the poll cadence (typically once per frame in a dev
// loop — never on the sim tick path). A moved-from reloader's poll
// fails with InvalidArgument and no log (the stopped-state
// precedent). Poll cost: one bounded file read + one byte comparison
// per call (O(file bytes)); a detected change adds one parse + one
// field comparison (debug-only, opt-in — the disabled cost is zero,
// DBG-004).
class ConfigHotReloader {
 public:
  // Binds to the config file at `path` and takes the baseline. Cold
  // path: one bounded file read + one parse + one schema validation.
  [[nodiscard]] static Result<ConfigHotReloader, ErrorCode> create(
      std::string_view path) noexcept;

  // Polls the file once (see the class contract above). `config` is
  // updated in place only on a successful non-sim reload.
  [[nodiscard]] Status poll(EngineConfig& config) noexcept;

  ConfigHotReloader(ConfigHotReloader&&) noexcept;
  ConfigHotReloader& operator=(ConfigHotReloader&&) noexcept;
  ConfigHotReloader(const ConfigHotReloader&) = delete;
  ConfigHotReloader& operator=(const ConfigHotReloader&) = delete;

 private:
  ConfigHotReloader() = default;
  // The watched path (the poll re-reads it; the file is not kept open —
  // an editor may rewrite it between polls).
  std::string path_{};
  // The baseline document bytes (the poll's compare target).
  std::string baselineBytes_{};
  // The baseline's validated config (the sim-field compare target).
  EngineConfig baselineConfig_{};
  // True after the move-out (the stopped-state precedent: poll fails
  // without logging).
  bool movedFrom_ = false;
};

}  // namespace laige
