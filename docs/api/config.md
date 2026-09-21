# Declarative game config (`EngineConfig`, M1-CFG-01)

The declarative game config surface (M1-CFG-01; PRD FR-1.5, ARCH-007,
ADR 0002, ADR 0003, PRD §7.1/§10.2/§10.3; AGENTS API-006/008,
CORE-005/008, PERF-002/003, LOG-002): the versioned `config.json`
schema, the typed `EngineConfig` value the engine consumes, the file
loader, the programmatic override merge, and the debug-build-only
hot reload of non-simulation keys.

Public header: `src/laige-sim/include/laige/sim/config.h`
(`EngineConfig`, `BudgetsConfig`, `CameraConfig`,
`kSupportedConfigVersion`, the range/default constants,
`parseEngineConfig`, `loadGameConfig`, `EngineConfigOverride`,
`applyConfigOverride`, `ConfigHotReloader`, the rejection-message
constants); implementation: `src/laige-sim/config.cpp`. Unit suite:
`ctest -R config` (the `game_config` entry —
`tests/laige-sim/game_config_tests.cpp`; the `config_json` entry is
the M0-CORE-07 parser it builds on).

```cpp
// Load the game config from the version 1 JSON file (cold path):
const laige::Result<laige::EngineConfig, laige::ErrorCode> loaded =
    laige::loadGameConfig("config.json");
// ... on success:
laige::EngineConfig config = std::move(loaded).takeValue();

// Programmatic override of a subset (FR-1.5):
laige::EngineConfigOverride override;
override.tickRateHz = 120;  // the rest is kept from the loaded config
laige::EngineConfig merged = laige::applyConfigOverride(config, override).value();

laige::Engine engine = laige::Engine::create(merged).value();
```

## The version 1 schema

The document is a top-level **JSON object**. The `version` key is
**required** and must be the exact integer `1`
(`laige::kSupportedConfigVersion`); it is checked **before any other
key** (ARCH-007: readers reject or migrate unsupported data
explicitly — never a silent migration). Every other key is optional
(missing keys take the defaults below); an **unknown key at any level
is warned** (one `config/unknown_key` per key — forward-compatible
with future schema versions) and ignored. **First failure wins**: the
loader stops at the first rejected key in document order and returns
the error; no config is produced on failure (CORE-008).

| key | type / range | default | hot-reloadable |
|---|---|---|---|
| `version` | exact integer, `== 1` | **required** | no |
| `tick_rate_hz` | exact integer, 20–120 | `kDefaultTickRateHz` (60) | no |
| `entity_budget` | exact integer, 0–65536 | `0` (empty scene — a valid world that creates no entities; entity creation on it fails `BudgetExhausted`) | no |
| `churn_per_frame_budget` | exact integer, 0–4294967295 | `kDefaultChurnPerFrameBudget` (256; `0` disables the G-R4 guardrail) | no |
| `seed` | exact integer, 0–2^53 | `kDefaultSimulationSeed` (0) | no |
| `determinism` | object (below) | `{enabled: true, math: "fixed_point_16_16"}` | no |
| `determinism.enabled` | bool | `true` | no |
| `determinism.math` | `"fixed_point_16_16"` \| `"float_pinned_32"` | `"fixed_point_16_16"` | no |
| `budgets` | object (below) | `{system_time_default_ms: 1.5, draw_calls_per_frame: 30, particles_per_frame: 4096}` | — |
| `budgets.system_time_default_ms` | finite number, > 0 (ms) | `kDefaultSystemTimeBudgetMs` (1.5) | no |
| `budgets.draw_calls_per_frame` | exact integer, 0–4294967295 | `kDefaultDrawCallsPerFrame` (30) | **yes** |
| `budgets.particles_per_frame` | exact integer, 0–4294967295 | `kDefaultParticlesPerFrame` (4096) | **yes** |
| `camera` | object (below) | `{fov_degrees: 45, zoom: 1, follow_lerp_per_sec: 8}` | — |
| `camera.fov_degrees` | finite number, 0 < v ≤ 180 | `kDefaultCameraFovDegrees` (45) | **yes** |
| `camera.zoom` | finite number, > 0 | `kDefaultCameraZoom` (1) | **yes** |
| `camera.follow_lerp_per_sec` | finite number, ≥ 0 | `kDefaultCameraFollowLerpPerSec` (8; `0` disables smoothing) | **yes** |
| `asset_roots` | array of non-empty strings | `[]` | **yes** |

**Number policy (ADR 0003).** JSON numbers are `double`s. The exact-
integer keys are validated to be integral and in range; the bounded
2^53 exactness of doubles is the `seed` upper bound (a seed above
2^53 is not exactly representable and is rejected). Non-finite values
(a literal beyond double range parses to ±inf) are rejected by every
numeric check.

**Declared presentation values.** The `budgets` and `camera` blocks
and `asset_roots` are **declared values in M1**: the scene entity
budget already has its consumer (the `World` capacity, G-R3) and
`budgets.system_time_default_ms` will govern the M1-PROF-02 per-system
budget report, but the draw/particle budgets (the M2
render/particle consumers) and the camera defaults (the M2 camera
work) are stored and validated now and consumed later (FR-1.5: values
stored; the consumers land in M2). `asset_roots` is likewise declared
now; the asset pipeline (M2) resolves them — **existence is not
checked at config load** (a root may be created later; the pipeline
owns the check).

## Versioning (ARCH-007)

The config document is persistent data and is **versioned**. This
build accepts exactly version 1; rejection is explicit:

| condition | error | event |
|---|---|---|
| `version` absent | `InvalidArgument` | `config/version_missing` (Warn) |
| `version` not an exact non-negative integer | `InvalidArgument` | `config/version_invalid` (Warn) |
| `version` ≠ 1 | `InvalidArgument` | `config/version_unsupported` (Warn) |

Documents from the M1-HEAD-01 provisional surface (no `version` key)
are rejected by the `version_missing` rule. **Migration from the
provisional surface: add `"version": 1`** — every other key is
unchanged. The `laige-run` CLI surfaces a rejection as exit code 2
with the NFR-13.3 5-field message on stderr (e.g.
`version_missing | the config document has no version key | ... |
add "version": 1 to the config (docs/api/config.md) |
docs/api/config.md`).

**Replay identity (ADR 0002).** The replay identity's `configHash`
([api/replay.md](replay.md)) covers **only the simulation-affecting
fields** — `tick_rate_hz`, `entity_budget`, `churn_per_frame_budget`,
`seed`, and the `determinism` block. The declared presentation values
(budgets block, camera block, asset roots) never touch simulation and
are deliberately **excluded** from the identity: a replay recorded
under one set of those values replays bit-exact under another. The
hashed field encoding (its tag word `1` included) therefore does not
change with this step, and **every committed baseline and replay log
stays valid**.

## Loading (`loadGameConfig`)

`laige::loadGameConfig(std::string_view path)` reads the file, parses
it (the M0-CORE-07 bounded JSON parser), and validates it against the
schema above. It is a **cold path** (startup/dev-time): one bounded
read + one parse + one O(document) schema walk; it allocates
(document-sized buffer, the config's string storage) and never runs on
the sim tick path (PERF-002/003).

| failure | `ErrorCode` | event |
|---|---|---|
| file missing / unreadable | `IoError` | `config/file_read_failed` (Warn) |
| file over the 1 MiB bound (ADR 0003) | `MalformedInput` | `config/file_read_failed` (Warn) |
| JSON grammar violation (truncated file, bad syntax) | `MalformedInput` | the ADR 0003 parser (no extra event) |
| schema rejection (any key above, version gate included) | `InvalidArgument` | the `config/<key>_invalid` event below |

The read is bounded at `1 MiB` (the ADR 0003 document bound): an
oversized file is a `MalformedInput`, not a truncated parse.

## Rejection table

First failure wins (document order); each rejection is one
rate-limited warn (LOG-004) with the NFR-13.3 5-field message (the
build-stable text is a named constant in `config.h`; dynamic values
are structured fields, never message text) plus the returned error.

| event (Warn) | condition |
|---|---|
| `config/version_missing` | `version` key absent |
| `config/version_invalid` | `version` not an exact non-negative integer (non-number, fractional, negative, wrong type) |
| `config/version_unsupported` | `version` ≠ 1 |
| `config/not_an_object` | document is not a JSON object |
| `config/tick_rate_invalid` | not a number / not an exact integer / outside 20–120 |
| `config/entity_budget_invalid` | not a number / not an exact integer / outside 0–65536 |
| `config/churn_budget_invalid` | not a number / not an exact integer / < 0 |
| `config/seed_invalid` | not a number / not an exact integer / outside 0–2^53 |
| `config/determinism_invalid` | the block is not an object |
| `config/determinism_enabled_invalid` | not a JSON boolean |
| `config/determinism_math_invalid` | not a string, or not one of the two ADR 0002 backend ids |
| `config/budgets_invalid` | the block is not an object |
| `config/system_time_budget_invalid` | not a number / not finite / ≤ 0 |
| `config/draw_calls_budget_invalid` | not a number / not an exact integer / < 0 |
| `config/particles_budget_invalid` | not a number / not an exact integer / < 0 |
| `config/camera_invalid` | the block is not an object |
| `config/camera_fov_invalid` | not a number / not finite / outside (0, 180] |
| `config/camera_zoom_invalid` | not a number / not finite / ≤ 0 |
| `config/camera_follow_lerp_invalid` | not a number / not finite / < 0 |
| `config/asset_roots_invalid` | not an array, or an element is not a non-empty string |
| `config/unknown_key` | any key not in the schema (any level) — **warn only, ignored** (forward-compat) |

All rejections map to `ErrorCode::InvalidArgument` (NFR-13.3 grammar:
`{codeId} | {what} | {why} | {fix} | {docAnchor}`, see
[errors.md](errors.md)).

## The typed config (`EngineConfig`)

`laige::EngineConfig` is a **plain value** (copied into
`Engine::create`; the `assetRoots` vector is its only heap member).
The five M1-HEAD-01 members keep their order — `{tickRateHz,
entityCapacity, churnPerFrameBudget, seed, determinism}` — followed
by the M1-CFG-01 additions `{budgets, camera, assetRoots}`, so
existing aggregate initializers compile unchanged.

```cpp
laige::EngineConfig config;  // every documented default
config.tickRateHz = 60;
config.entityCapacity = 4096;
config.churnPerFrameBudget = 256;
config.seed = 0x1F055EEDull;          // programmatic: full 64 bits (no 2^53 JSON bound)
config.determinism.enabled = true;
config.determinism.math = laige::SimMathBackend::FixedPoint16_16;
config.budgets.systemTimeDefaultMs = 1.5;
config.budgets.drawCallsPerFrame = 30;
config.budgets.particlesPerFrame = 4096;
config.camera.fovDegrees = 45.0;
config.camera.zoom = 1.0;
config.camera.followLerpPerSec = 8.0;
config.assetRoots = {"assets/"};
```

The **programmatic surface** accepts the full 64-bit `seed` (the JSON
surface is bounded to 2^53 by the ADR 0003 number policy).
`Engine::create` re-validates the typed config's tick rate (the struct
is public; the JSON path is not the only constructor) and reuses the
config surface's rejection (`config/tick_rate_invalid`, subsystem
`config`).

**Simulation-affecting fields** (part of the replay identity's
`configHash`; hot-reload-refused): `tickRateHz`, `entityCapacity`,
`churnPerFrameBudget`, `seed`, `determinism`, and
`budgets.systemTimeDefaultMs` (it governs the G-R5 budget enforcement
during ticks). **Declared presentation fields** (validated, stored, no
M1 consumer; hot-reloadable in debug): the draw/particle budgets,
`camera.*`, `assetRoots`.

## Overrides (`applyConfigOverride`)

`laige::EngineConfigOverride` is a per-leaf `std::optional` struct
(one member per config leaf — the split `determinismEnabled` /
`determinismMath` mirror the JSON block's two keys).
`laige::applyConfigOverride(base, override)` merges the **subset**:

- a `std::nullopt` field is kept from `base`;
- a set field replaces it and is **validated in its documented domain**
  (the same `config/<key>_invalid` events as the JSON path — e.g. a
  set `tickRateHz` of 15 is `InvalidArgument`, a set `fovDegrees` of
  0 is `InvalidArgument`);
- **first set field that fails wins** (the struct's declaration
  order);
- value semantics: `base` is untouched; a set `assetRoots` **replaces
  the vector**.

Cold path: O(1) plus the `assetRoots` copy when that field is set.
This is the FR-1.5 "runtime config overrides" merge — e.g. a test
harness that loads the shipped config and bumps one field.

## Hot reload (`ConfigHotReloader`, debug builds only)

`laige::ConfigHotReloader` watches the config file so a dev loop can
pick up **non-simulation** config changes without a restart (FR-1.5
"hot-reload of non-simulation config in debug"). It is **move-only**,
single-owner-thread (the game's loop thread — the sim is
single-threaded, PRD §10.2), and creates **no thread** (CONC-005):
the caller drives the poll cadence (typically once per frame in a dev
loop — never on the sim tick path).

```cpp
// In a dev loop (debug build):
auto reloader = laige::ConfigHotReloader::create("config.json");
// ... each frame, with the live engine config:
if (!reloader->poll(config).ok()) {
  // refused (sim-affecting key changed) or a transient read failure —
  // see the events below; the engine keeps running with the old values
}
```

`create(path)` binds to the file and takes the **baseline**: the
current bytes plus a full validated parse (a read/parse/schema failure
returns the error, no reloader — CORE-008). `poll(config)` re-reads
the file and compares the bytes to the baseline (a byte comparison per
poll — robust to same-size/same-mtime-second edits):

| poll outcome | returned | log |
|---|---|---|
| file unchanged | ok | — (no event) |
| changed, sim-affecting fields all equal to the baseline | ok | `config/hot_reload_applied` (**Info**) naming every changed key; the non-sim fields (camera.*, the draw/particle budgets, `asset_roots`) are applied to `config` **in place** and the baseline advances (the file is now authoritative for them — a programmatic override of those fields is replaced) |
| changed, any sim-affecting field differs | `InvalidArgument` | `config/hot_reload_rejected` (**Error**) naming the **first differing key** with its old/new values; `config` and the baseline are **untouched** — restart the engine to apply (FR-1.5) |
| file no longer reads (deleted, locked, over bound) | `IoError` | `config/hot_reload_read_failed` (Warn); `config` unchanged — retry on the next poll |
| file no longer validates (truncated mid-edit, bad version) | the loader's error (`MalformedInput` / `InvalidArgument`) | the loader's event; `config` unchanged |

A change of **any** simulation-affecting key (`version`,
`tick_rate_hz`, `entity_budget`, `churn_per_frame_budget`, `seed`,
`determinism.enabled`, `determinism.math`,
`budgets.system_time_default_ms`) is refused **atomically**: even if
the same write also changes camera defaults, none of it is applied —
the live config never corresponds to a partial file state.

A **moved-from** reloader's poll fails with `InvalidArgument` and no
log (the stopped-state precedent).

**Release builds (NDEBUG)**: `create` and `poll` both reject with
`InvalidArgument` + `config/hot_reload_disabled` (Warn) — hot reload
is development tooling, the replay/`record_disabled` pattern.

## Performance

All config work is a **cold path** (PERF-002/003):

- `loadGameConfig`: O(file bytes) once at startup + the parse.
- `parseEngineConfig`: O(document keys), one pass; no allocation except
  the warn fields on a rejection.
- `applyConfigOverride`: O(1) plus the `assetRoots` copy when set.
- `ConfigHotReloader::poll`: one bounded file read + one byte
  comparison per call (O(file bytes)); a detected change adds one
  parse + one field comparison. Debug-only and opt-in — the reloader
  exists only when constructed, so the disabled cost is zero
  (DBG-004) — and it never runs on the sim tick path.

## Misuse warnings

- **Do not edit sim-affecting keys expecting a live effect.** The hot
  reloader refuses them with `hot_reload_rejected`; the fix it names
  is a restart.
- **Do not expect `asset_roots` existence checks at load.** Missing
  roots are a config-time non-error; the M2 asset pipeline owns the
  check.
- **Do not build `EngineConfig` by hand with out-of-range values and
  expect the engine to correct them.** `Engine::create` validates the
  tick rate and rejects; the other domains are the caller's
  responsibility on the programmatic surface (the JSON/override paths
  validate everything).
- **Do not hold a `ConfigHotReloader` across a config file rename.**
  It watches a fixed path; create a new reloader after a rename
  (the poll of the new path takes a new baseline).
- **Do not poll from multiple threads.** The reloader is
  single-owner-thread (CONC-005).

## Tests and CI

`ctest -R config` (the step Verify command): the `game_config` entry
(this suite — the schema's every rejection domain, the version gate,
first-failure-wins, unknown-key forward-compat, the file loader's
bounded read, the override merge, and the hot-reload contract including
the release-disabled path) plus the `config_json` entry (the M0-CORE-07
bounded JSON parser the schema layer builds on). The malformed-input
surface is the ADR 0003 parser's, fuzzed by the laige-core `json_parse`
fuzz target (TEST-005); the schema layer only sees well-formed
`JsonValue`s, so it needs no separate fuzz target.
