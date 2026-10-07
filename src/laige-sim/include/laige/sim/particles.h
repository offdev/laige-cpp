// laige-sim particle simulation (M2-PART-01).
//
// FR-2.7: "Lightweight GPU particle system (CPU-simulated, 2D +
// depth), budgeted, pooled." This step is the CPU-simulation half:
// a bounded, pooled particle system updated in the simulation tick
// (deterministic, engine math only — ADR 0002). The rendering half
// (particles as batched sprites, one draw call per emitter set,
// depth from the particle depth value) is M2-PART-02, which consumes
// this header's read-only surface.
//
//   ParticleEmitterDef<Backend>   The emitter definition (spawn
//                                 domain + continuous rate)
//   ParticleSystem<Backend>       The bounded pool + emitter registry
//                                 + per-tick update
//
// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
//
// A particle is a simulation-space 2D position plus a CONSTANT depth
// value (the 2.5D height M2-PART-02 feeds the M2-ISO-01 depth key),
// a per-tick velocity, a life in simulation ticks, a size, a base
// RGBA tint (u8 channels), and a fade window:
//
//   Particle { pos, depth, vel, age, life, size, tint[4], fadeTicks }
//
// (the `ParticleSystem::Particle` struct — 40 bytes per particle on
// both backends). An emitter is a named, validated spawn domain:
// origin, depth, velocity box [velMin, velMax] (per tick), life box
// [lifeMin, lifeMax] (ticks), size box [sizeMin, sizeMax], tint,
// fade window, and `continuousRate` (particles per simulation tick;
// 0 = burst-only emitter). The system holds at most `maxEmitters`
// emitters with dense ids 1..N in registration order (id 0 is the
// unused sentinel — the M2-TILE-02 convention).
//
// ---------------------------------------------------------------------------
// The per-tick contract (ARCH-002)
// ---------------------------------------------------------------------------
//
// `update()` runs EXACTLY ONCE per completed simulation tick, driven
// by the game's tick path (a game system, or the loop's onTick hook —
// the TileMap::advanceAnimations pattern; the simulation never
// depends on the presentation frame rate). In-tick order:
//
//   1. Advance every currently live particle (live-array order):
//      age += 1; pos += vel (one SimMath vector add — ADR 0002);
//      age >= life kills (swap-removed with the last live particle;
//      the swapped-in particle — not yet advanced this tick — is
//      re-examined at the same index).
//   2. Emit from every emitter in registration order
//      (continuousRate particles each).
//   3. Report the tick's overflow: at most ONE rate-limited warn.
//
// A particle spawned during tick T's update is live (in the live set)
// after ticks T..T+life-1 (age 0..life-1) and dies during tick
// T+life's update — visible for EXACTLY `life` renders. Velocity is
// constant (no acceleration — out of scope for this step); depth is
// constant over the particle's life (the emitter's depth).
//
// ---------------------------------------------------------------------------
// Determinism (ARCH-010, ADR 0002)
// ---------------------------------------------------------------------------
//
// The state after N updates is a pure function of (seed, emitter
// definitions, operation sequence), per backend:
//
//   - The system owns one Prng (Options::seed). Each SUCCESSFUL spawn
//     consumes exactly FOUR draws in a fixed order: vx, vy, life,
//     size. Each uniform scalar is lerp(min, max, u / 2^24) where
//     u = next_range(0, 2^24) — the Prng's 24-bit tap resolution
//     (prng.h) — one documented rounding per backend (the ADR 0002
//     lerp contract); life is an integer next_range draw.
//   - A DROPPED spawn (pool full) consumes NO draws: the pool check
//     precedes the draws, so a drop never perturbs the stream.
//   - Motion is SimMath ops only and the fade is exact integer
//     arithmetic (below). Nothing here reads platform intrinsics,
//     addresses, or wall-clock time.
//
// Backend scope (ARCH-010): fpx16_16 — bit-identical across all
// builds, platforms, ISAs, and compilers (the language-standard
// guarantee); fp32_pinned — bit-identical across runs of the same
// build on the same platform/ISA (the detcheck matrix owns
// cross-target claims). The Prng state is part of the deterministic
// state: `prngSeed()`/`prngStatePart1()`/`prngStatePart2()` expose it
// for the World stateHash / replay identity (the M1-DET-03 pattern).
//
// ---------------------------------------------------------------------------
// The pool budget (FR-2.7 "budgeted, pooled", PERF-008)
// ---------------------------------------------------------------------------
//
// The pool is `maxParticles` PRE-ALLOCATED slots (the setup path —
// with the emitter table, the system's only allocations; PERF-003:
// nothing after). Live particles occupy the FIRST `liveCount` slots
// (compact: a death swap-removes the last live particle into the dead
// slot, so the live order is spawn order with swap removal —
// deterministic).
//
// Overflow: a spawn into a full pool is DROPPED — counted
// (`droppedTotal` + per-tick bookkeeping) and reported with at most
// ONE `particles/pool_overflow` warn per tick (fields
// dropped/live/capacity; the LOG-004 logger rate window is a second
// layer). A drop never mutates live state, never throws, and never
// grows the pool — the pool is bounded by construction.
//
// ---------------------------------------------------------------------------
// The color fade (exact integer arithmetic)
// ---------------------------------------------------------------------------
//
// `fadeTicks == 0`: no fade (the tint alpha is constant). Otherwise:
// remaining = life - age (live particles always have age < life — no
// underflow), and
//
//   alpha = remaining >= fadeTicks ? tint.a
//         : tint.a * remaining / fadeTicks     (u32 integer math)
//
// — the alpha ramps linearly from the fade window's start to 0 at
// death, one exact multiply + divide per read (no floating point —
// bit-identical on every platform). The fade touches the alpha
// channel only; the RGB channels are constant. `fadeAlpha(p)`
// exposes the computed value to the render pass.
//
// ---------------------------------------------------------------------------
// Failure (CORE-008, API-008) — first failure wins
// ---------------------------------------------------------------------------
//
//   create:     maxParticles outside [1, kParticlePoolMaxParticles]
//               or maxEmitters outside [1, kParticleEmitterMaxEmitters]
//               -> InvalidArgument (no log — the M2-SPRITE-01 create
//               precedent).
//   stopped:    the default-constructed / moved-from / failed-create
//               state: `valid()` false; `addEmitter`/`burst` ->
//               InvalidArgument (no log — the stopped-state
//               precedent); `update()` is a no-op; `liveParticles()`
//               is empty; `stats()` reads zero.
//   addEmitter: stopped -> InvalidArgument (no log); registry full ->
//               BudgetExhausted + warn `particles/emitters_exhausted`
//               (field capacity); a def field out of domain ->
//               InvalidArgument + one rate-limited warn
//               `particles/emitter_invalid` (fields emitter, field) —
//               validation order: origin -> depth -> vel_min ->
//               vel_max -> vel_box -> life_min -> life_box ->
//               life_max -> size_min -> size_max -> size_box ->
//               fade_ticks. A rejected def leaves the registry
//               unchanged.
//   burst:      stopped or unknown emitter id -> InvalidArgument (no
//               log — the M2-PAR-01 setUvOffset precedent); count 0 is
//               a no-op success.
//
// The happy paths log nothing (LOG-003).
//
// ---------------------------------------------------------------------------
// Ownership, threading (CORE-009, CONC-001)
// ---------------------------------------------------------------------------
//
// One owner thread (the sim thread). `create` is the setup path (two
// pre-allocations: the pool, the emitter table); nothing allocates
// afterward (the zero-allocation proof: the tests' 500-tick
// update/burst window). Move-only (O(1) pointer swap; the moved-from
// system is stopped — its Prng copy shares the stream position with
// the live system, which is safe because the moved-from system is
// stopped and never draws again). The render pass reads
// `liveParticles()` (a NON-OWNING span — PERF-005) after the sim
// phase (the M2-GL-02 cull/batch stage): presentation reads
// authoritative state read-only (ARCH-009); nothing in the render
// pass writes here.
//
// Canonical narrative: docs/api/particles.md (the full contract);
// the particle state's place in the 2.5D model:
// docs/concepts/coordinates.md §4.11 + the §5 conversion row.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include "laige/errors.h"
#include "laige/logging.h"
#include "laige/prng.h"
#include "laige/result.h"
#include "laige/sim_math.h"

namespace laige {

// ---------------------------------------------------------------------------
// Named constants (CORE-005)
// ---------------------------------------------------------------------------

// The pool capacity domain: [1, kParticlePoolMaxParticles]. 2^16 =
// 65 536 — headroom over the PRD §8.1 worst-case 50k-sprite scene
// (particle overlays on top); at 40 B/particle the worst-case pool
// is 2.5 MB (the setup path).
inline constexpr std::uint32_t kParticlePoolMaxParticles = 1u << 16;

// The default pool capacity (a typical single-screen particle
// budget; the reference scene's emitters fit with margin).
inline constexpr std::uint32_t kParticlePoolDefaultMaxParticles = 4096;

// The emitter registry capacity domain: [1,
// kParticleEmitterMaxEmitters].
inline constexpr std::uint32_t kParticleEmitterMaxEmitters = 256;

// The default emitter registry capacity.
inline constexpr std::uint32_t kParticleEmitterDefaultMaxEmitters = 32;

// The particle life domain: [1, kParticleMaxLifeTicks] simulation
// ticks. 2^20 is roughly 4.9 h at 60 Hz — far beyond any game
// particle life.
inline constexpr std::uint32_t kParticleMaxLifeTicks = 1u << 20;

// The uniform-sample resolution: the Prng's 24-bit tap (one draw
// resolves a [0, 1) sample to 2^-24 — the next_float01 resolution,
// prng.h).
inline constexpr std::uint32_t kParticleSampleDenominator = 1u << 24;

// The emitter id: dense from 1 (registration order); 0 is the
// unused sentinel.
using ParticleEmitterId = std::uint32_t;

// The particle system's profiler feed (DBG-008 — the M2-SPRITE-04
// pattern): the gauges (live/capacity) + the counters
// (spawnedTotal/droppedTotal — u64: a long-running session's spawn
// count is not bounded by 2^32).
struct ParticleStats {
  // The live particle count (gauge).
  std::uint32_t live{};
  // The pool capacity (gauge — 0 in the stopped state).
  std::uint32_t capacity{};
  // Total successful spawns since create (counter).
  std::uint64_t spawnedTotal{};
  // Total dropped spawns (pool full) since create (counter).
  std::uint64_t droppedTotal{};
};

// ---------------------------------------------------------------------------
// The emitter definition (the validated spawn domain)
// ---------------------------------------------------------------------------

// One emitter's spawn domain + rate (the header's model section).
// A plain value (no ownership); VALIDATED AT USE TIME by
// ParticleSystem::addEmitter (the header's failure section — first
// failure wins). Templated over the SimMath backends (the
// presentation.h pattern).
template <typename Backend>
struct ParticleEmitterDef {
  using Scalar = typename sim::SimMath<Backend>::Scalar;
  using Vec2 = typename sim::SimMath<Backend>::Vec2;

  // The spawn position (sim space; a particle spawns EXACTLY here —
  // no position jitter in M2-PART-01).
  Vec2 origin{};
  // The spawn depth value (constant over the particle's life — the
  // M2-ISO-01 depth-key input M2-PART-02 consumes).
  Scalar depth{};
  // The velocity box lower bound (world units per tick; component-
  // wise).
  Vec2 velMin{};
  // The velocity box upper bound.
  Vec2 velMax{};
  // The life box lower bound (simulation ticks; >= 1).
  std::uint32_t lifeMin{1};
  // The life box upper bound (<= lifeMin.. kParticleMaxLifeTicks).
  std::uint32_t lifeMax{1};
  // The size box lower bound (world units; >= 0).
  Scalar sizeMin{};
  // The size box upper bound (>= sizeMin).
  Scalar sizeMax{};
  // The base tint RGBA (0..255 per channel).
  std::uint8_t tint[4]{255, 255, 255, 255};
  // The fade window (ticks; 0 = no fade; <= lifeMax — the header's
  // color-fade section).
  std::uint32_t fadeTicks{0};
  // The continuous emission rate (particles PER SIMULATION TICK; 0 =
  // burst-only emitter). Unbounded here — the pool budget bounds the
  // work (overflow drops).
  std::uint32_t continuousRate{0};
};

// ---------------------------------------------------------------------------
// The particle system (the bounded pool + emitter registry + update)
// ---------------------------------------------------------------------------

// The bounded, pooled, deterministic particle system of FR-2.7
// (header preamble). Templated over the SimMath backends (the
// M2-TILE-01/presentation.h pattern — one instantiation per
// backend, factory-selected at engine init, ADR 0002). Header-only:
// pure engine math + pool bookkeeping, no GL, no allocation in the
// per-tick path.
template <typename Backend>
class ParticleSystem {
  static_assert(
      std::is_same_v<Backend, sim::Fpx16_16> ||
          std::is_same_v<Backend, sim::Fp32Pinned>,
      "ParticleSystem is templated over the SimMath backends");

 public:
  using Scalar = typename sim::SimMath<Backend>::Scalar;
  using Vec2 = typename sim::SimMath<Backend>::Vec2;
  using M = sim::SimMath<Backend>;

  // The per-particle state (40 B, both backends — the header's model
  // section). `age`/`life` are simulation ticks; `tint` is the base
  // RGBA; `fadeTicks` is the emitter's fade window (0 = no fade).
  // The render path reads these read-only (M2-PART-02).
  struct Particle {
    Vec2 pos{};
    Scalar depth{};
    Vec2 vel{};
    std::uint32_t age{};
    std::uint32_t life{};
    Scalar size{};
    std::uint8_t tint[4]{};
    std::uint32_t fadeTicks{};
  };

  // The create options (API-006): `maxParticles` in [1,
  // kParticlePoolMaxParticles] (default
  // kParticlePoolDefaultMaxParticles); `maxEmitters` in [1,
  // kParticleEmitterMaxEmitters] (default
  // kParticleEmitterDefaultMaxEmitters); `seed` (any u64 — 0 is a
  // valid seed; the system's Prng substream, the M0-CORE-06 pattern).
  struct Options {
    std::uint32_t maxParticles{kParticlePoolDefaultMaxParticles};
    std::uint32_t maxEmitters{kParticleEmitterDefaultMaxEmitters};
    std::uint64_t seed{0};
  };

  // Creates the system (setup path — the only allocations: the pool
  // and the emitter table). maxParticles/maxEmitters outside their
  // domains -> InvalidArgument (no log — the M2-SPRITE-01 create
  // precedent). The stopped state (failed create / default)
  // follows the RenderThread precedent: `valid()` false, every
  // operation fails or no-ops, no log.
  [[nodiscard]] static laige::Result<ParticleSystem, laige::ErrorCode>
  create(Options options) noexcept;

  // The stopped state (default / failed create): nothing owned
  // (pool_ null, maxParticles_ 0); the Prng is seed-0 (a stopped
  // system never draws — the seed is inert).
  ParticleSystem() noexcept : rng_(0) {}

  // Move-only: O(1) pointer swap; the moved-from system is stopped
  // (its Prng copy shares the stream position with the live system —
  // safe: the moved-from system never draws again).
  ParticleSystem(ParticleSystem&& other) noexcept;
  ParticleSystem& operator=(ParticleSystem&& other) noexcept;
  ParticleSystem(const ParticleSystem&) = delete;
  ParticleSystem& operator=(const ParticleSystem&) = delete;

  // True iff the system is live (create succeeded).
  [[nodiscard]] bool valid() const noexcept { return maxParticles_ > 0; }

  // The pool capacity (0 in the stopped state).
  [[nodiscard]] std::uint32_t maxParticles() const noexcept {
    return maxParticles_;
  }

  // The emitter registry capacity (0 in the stopped state).
  [[nodiscard]] std::uint32_t maxEmitters() const noexcept {
    return maxEmitters_;
  }

  // The system's Prng seed (the create Options::seed; the replay
  // identity's part, M1-DET-03 pattern).
  [[nodiscard]] std::uint64_t seed() const noexcept { return rng_.seed(); }

  // The live particle count (O(1); 0 in the stopped state).
  [[nodiscard]] std::uint32_t liveCount() const noexcept {
    return liveCount_;
  }

  // Total successful spawns since create (O(1); u64 counter).
  [[nodiscard]] std::uint64_t spawnedTotal() const noexcept {
    return spawnedTotal_;
  }

  // Total dropped spawns (pool full) since create (O(1); u64 counter).
  [[nodiscard]] std::uint64_t droppedTotal() const noexcept {
    return droppedTotal_;
  }

  // The profiler feed (DBG-008): the gauges + the counters (O(1)).
  [[nodiscard]] ParticleStats stats() const noexcept {
    return ParticleStats{liveCount_, maxParticles_, spawnedTotal_,
                         droppedTotal_};
  }

  // Registers an emitter (setup phase). Validates `def` in the
  // header's documented order (first failure wins; a rejection leaves
  // the registry unchanged + one rate-limited warn). Returns the
  // dense id (1-based, registration order). Stopped ->
  // InvalidArgument (no log); registry full -> BudgetExhausted +
  // warn. O(1).
  [[nodiscard]] laige::Result<ParticleEmitterId, laige::ErrorCode>
  addEmitter(const ParticleEmitterDef<Backend>& def) noexcept;

  // The number of registered emitters (O(1)).
  [[nodiscard]] std::uint32_t emitterCount() const noexcept {
    return emitterCount_;
  }

  // True iff `id` is a registered emitter (O(1)).
  [[nodiscard]] bool hasEmitter(ParticleEmitterId id) const noexcept {
    return maxParticles_ > 0 && id >= 1 && id <= emitterCount_;
  }

  // The registered emitter's definition (O(1)); nullptr when the id
  // is not registered (or the system is stopped).
  [[nodiscard]] const ParticleEmitterDef<Backend>* emitterAt(
      ParticleEmitterId id) const noexcept {
    return hasEmitter(id) ? &emitters_[id - 1] : nullptr;
  }

  // Spawns `count` particles from emitter `id` NOW (sim phase —
  // the game's spawn policy; the continuous rate is the automatic
  // half, the burst is the explicit one). Stopped or unknown id ->
  // InvalidArgument (no log); count 0 is a no-op success.
  // O(count): one pool check + (on success) 4 Prng draws per particle
  // — see the header's determinism section.
  [[nodiscard]] laige::Status burst(ParticleEmitterId id,
                                    std::uint32_t count) noexcept;

  // Advances the simulation by ONE tick (the header's per-tick
  // contract: advance -> emit -> overflow report). The game's tick
  // driver calls this exactly once per completed tick (ARCH-002).
  // O(live + sum of rates); no allocation; no logging on the happy
  // path. No-op in the stopped state.
  void update() noexcept;

  // The render read path (M2-PART-02): the live particles as a
  // non-owning span (PERF-005) — the compact live prefix
  // [0, liveCount) of the pool (spawn order with swap removal).
  // Read it after the sim phase (the M2-GL-02 cull/batch stage).
  // O(1) (the span); never writes.
  [[nodiscard]] std::span<const Particle> liveParticles() const noexcept {
    return std::span<const Particle>(pool_.get(), liveCount_);
  }

  // The particle's CURRENT fade alpha (0..255) — the header's
  // color-fade section (exact u32 integer arithmetic). Precondition:
  // `p` is live (age < life — no underflow). O(1).
  [[nodiscard]] static std::uint8_t fadeAlpha(const Particle& p) noexcept {
    const std::uint32_t remaining = p.life - p.age;
    if (p.fadeTicks == 0 || remaining >= p.fadeTicks) {
      return p.tint[3];
    }
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(p.tint[3]) * remaining) / p.fadeTicks);
  }

  // The system's Prng stream state (the replay identity's part —
  // M1-DET-03 pattern; the state after N successful spawns is 4N
  // draws from the seed, the header's determinism section). O(1).
  [[nodiscard]] std::uint64_t prngSeed() const noexcept { return rng_.seed(); }
  [[nodiscard]] std::uint64_t prngStatePart1() const noexcept {
    return rng_.statePart1();
  }
  [[nodiscard]] std::uint64_t prngStatePart2() const noexcept {
    return rng_.statePart2();
  }

 private:
  // create-only (the setup path: the two pre-allocations).
  explicit ParticleSystem(Options options) noexcept
      : pool_(new Particle[options.maxParticles]()),
        emitters_(new ParticleEmitterDef<Backend>[options.maxEmitters + 1]()),
        rng_(options.seed),
        maxParticles_(options.maxParticles),
        maxEmitters_(options.maxEmitters) {}

  // Emits `count` particles from the emitter at `index` (0-based,
  // emitters_ storage; called from burst/update in registration
  // order).
  void emitFrom(std::uint32_t index, std::uint32_t count) noexcept {
    const ParticleEmitterDef<Backend>& def = emitters_[index];
    for (std::uint32_t i = 0; i < count; ++i) {
      spawnOne(def);
    }
  }

  // One spawn attempt (the header's pool-budget + determinism
  // sections): a full pool drops (no draws); otherwise the fixed 4-
  // draw contract (vx, vy, life, size) fills the next live slot.
  void spawnOne(const ParticleEmitterDef<Backend>& def) noexcept {
    if (liveCount_ >= maxParticles_) {
      // The budget: the pool is full — drop. The check precedes the
      // Prng draws, so a drop never perturbs the stream.
      ++droppedTotal_;
      ++droppedSinceUpdate_;
      return;
    }
    Particle p;
    p.pos = def.origin;
    p.depth = def.depth;
    p.vel = Vec2{sampleScalar(def.velMin.x, def.velMax.x),
                 sampleScalar(def.velMin.y, def.velMax.y)};
    p.age = 0;
    p.life = rng_.next_range(def.lifeMin, def.lifeMax + 1);
    p.size = sampleScalar(def.sizeMin, def.sizeMax);
    for (int c = 0; c < 4; ++c) {
      p.tint[c] = def.tint[c];
    }
    p.fadeTicks = def.fadeTicks;
    pool_[liveCount_++] = p;
    ++spawnedTotal_;
  }

  // An integer v < 2^24 as a Scalar — exact on both backends (the fpx
  // raw units; the fp32 24-bit mantissa represents every integer < 2^24
  // exactly). Not a static_cast: the fpx16_16 aggregate's single-member
  // parenthesized cast is rejected by the older AppleClang of the
  // macOS P0 toolchain.
  static Scalar fromInt(std::uint32_t v) noexcept {
    if constexpr (std::is_same_v<Backend, laige::sim::Fpx16_16>) {
      return Scalar{static_cast<std::int32_t>(v)};
    } else {
      return static_cast<Scalar>(static_cast<std::int32_t>(v));
    }
  }

  // One uniform scalar in [lo, hi]: one 24-bit Prng tap resolved to
  // [0, 1) (u / 2^24 — the tap resolution, the header's
  // determinism section) and one SimMath lerp (the ADR 0002 rounding
  // contract). lo == hi returns exactly lo (no stream perturbation
  // beyond the documented draw).
  Scalar sampleScalar(Scalar lo, Scalar hi) noexcept {
    const std::uint32_t u = rng_.next_range(0, kParticleSampleDenominator);
    const Scalar t = M::div(fromInt(u), fromInt(kParticleSampleDenominator));
    return M::lerp(lo, hi, t);
  }

  // The per-tick overflow report: at most ONE warn per tick window
  // (the header's pool-budget section; the LOG-004 logger window is a
  // second layer).
  void maybeWarnOverflow() noexcept {
    if (droppedSinceUpdate_ == 0 || overflowWarnedSinceUpdate_) {
      return;
    }
    LAIGE_LOG_WARN("particles", "pool_overflow",
                   "Particle pool full; spawned particles dropped this tick",
                   laige::log::field("dropped", droppedSinceUpdate_),
                   laige::log::field("live", liveCount_),
                   laige::log::field("capacity", maxParticles_));
    overflowWarnedSinceUpdate_ = true;
  }

  std::unique_ptr<Particle[]> pool_;  // maxParticles_ slots (empty <=> stopped)
  // maxEmitters_ + 1 slots; index 0 unused (the id-0 sentinel).
  std::unique_ptr<ParticleEmitterDef<Backend>[]> emitters_;
  Prng rng_;
  std::uint32_t maxParticles_{0};
  std::uint32_t maxEmitters_{0};
  std::uint32_t emitterCount_{0};
  std::uint32_t liveCount_{0};
  std::uint64_t droppedTotal_{0};
  std::uint64_t spawnedTotal_{0};
  std::uint32_t droppedSinceUpdate_{0};
  bool overflowWarnedSinceUpdate_{false};
};

template <typename Backend>
laige::Result<ParticleSystem<Backend>, laige::ErrorCode>
ParticleSystem<Backend>::create(Options options) noexcept {
  // First failure wins (the M2-SPRITE-01 create precedent; no log).
  if (options.maxParticles < 1 ||
      options.maxParticles > kParticlePoolMaxParticles) {
    return laige::Result<ParticleSystem<Backend>, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  if (options.maxEmitters < 1 ||
      options.maxEmitters > kParticleEmitterMaxEmitters) {
    return laige::Result<ParticleSystem<Backend>, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  return laige::Result<ParticleSystem<Backend>, laige::ErrorCode>::success(
      ParticleSystem<Backend>(std::move(options)));
}

template <typename Backend>
ParticleSystem<Backend>::ParticleSystem(ParticleSystem&& other) noexcept
    : pool_(std::move(other.pool_)),
      emitters_(std::move(other.emitters_)),
      rng_(other.rng_),
      maxParticles_(other.maxParticles_),
      maxEmitters_(other.maxEmitters_),
      emitterCount_(other.emitterCount_),
      liveCount_(other.liveCount_),
      droppedTotal_(other.droppedTotal_),
      spawnedTotal_(other.spawnedTotal_),
      droppedSinceUpdate_(other.droppedSinceUpdate_),
      overflowWarnedSinceUpdate_(other.overflowWarnedSinceUpdate_) {
  // Stop the source (the stopped-state precedent).
  other.maxParticles_ = 0;
  other.maxEmitters_ = 0;
  other.emitterCount_ = 0;
  other.liveCount_ = 0;
  other.droppedTotal_ = 0;
  other.spawnedTotal_ = 0;
  other.droppedSinceUpdate_ = 0;
  other.overflowWarnedSinceUpdate_ = false;
}

template <typename Backend>
ParticleSystem<Backend>&
ParticleSystem<Backend>::operator=(ParticleSystem&& other) noexcept {
  if (this != &other) {
    // Route through the move constructor (RAII cleanup of this
    // system's old storage happens on the member moves).
    ParticleSystem tmp(std::move(other));
    pool_ = std::move(tmp.pool_);
    emitters_ = std::move(tmp.emitters_);
    rng_ = tmp.rng_;
    maxParticles_ = tmp.maxParticles_;
    maxEmitters_ = tmp.maxEmitters_;
    emitterCount_ = tmp.emitterCount_;
    liveCount_ = tmp.liveCount_;
    droppedTotal_ = tmp.droppedTotal_;
    spawnedTotal_ = tmp.spawnedTotal_;
    droppedSinceUpdate_ = tmp.droppedSinceUpdate_;
    overflowWarnedSinceUpdate_ = tmp.overflowWarnedSinceUpdate_;
  }
  return *this;
}

template <typename Backend>
laige::Result<ParticleEmitterId, laige::ErrorCode>
ParticleSystem<Backend>::addEmitter(
    const ParticleEmitterDef<Backend>& def) noexcept {
  if (maxParticles_ == 0) {
    // The stopped state — no log (the stopped-state precedent).
    return laige::Result<ParticleEmitterId, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  if (emitterCount_ >= maxEmitters_) {
    LAIGE_LOG_WARN("particles", "emitters_exhausted",
                   "Particle emitter registry full",
                   laige::log::field("capacity", maxEmitters_));
    return laige::Result<ParticleEmitterId, laige::ErrorCode>::failure(
        laige::ErrorCode::BudgetExhausted);
  }
  // The documented validation order (first failure wins — the
  // M2-PAR-01 setLayer precedent). The def is caller-owned scene
  // metadata (untrusted input, SCALE-004): every field is checked.
  const Scalar kZero = Scalar{};
  const char* field = nullptr;
  if (!M::isFinite(def.origin.x) || !M::isFinite(def.origin.y)) {
    field = "origin";
  } else if (!M::isFinite(def.depth)) {
    field = "depth";
  } else if (!M::isFinite(def.velMin.x) || !M::isFinite(def.velMin.y)) {
    field = "vel_min";
  } else if (!M::isFinite(def.velMax.x) || !M::isFinite(def.velMax.y)) {
    field = "vel_max";
  } else if (M::greater(def.velMin.x, def.velMax.x) ||
             M::greater(def.velMin.y, def.velMax.y)) {
    field = "vel_box";
  } else if (def.lifeMin < 1) {
    field = "life_min";
  } else if (def.lifeMin > def.lifeMax) {
    field = "life_box";
  } else if (def.lifeMax > kParticleMaxLifeTicks) {
    field = "life_max";
  } else if (!M::isFinite(def.sizeMin) ||
             !M::greaterEqual(def.sizeMin, kZero)) {
    field = "size_min";
  } else if (!M::isFinite(def.sizeMax)) {
    field = "size_max";
  } else if (M::greater(def.sizeMin, def.sizeMax)) {
    field = "size_box";
  } else if (def.fadeTicks > def.lifeMax) {
    field = "fade_ticks";
  }
  if (field != nullptr) {
    LAIGE_LOG_WARN("particles", "emitter_invalid",
                   "Rejected particle emitter definition",
                   laige::log::field("emitter", emitterCount_ + 1),
                   laige::log::field("field", field));
    return laige::Result<ParticleEmitterId, laige::ErrorCode>::failure(
        laige::ErrorCode::InvalidArgument);
  }
  const ParticleEmitterId id = emitterCount_ + 1;
  emitters_[emitterCount_] = def;
  ++emitterCount_;
  return laige::Result<ParticleEmitterId, laige::ErrorCode>::success(id);
}

template <typename Backend>
laige::Status ParticleSystem<Backend>::burst(ParticleEmitterId id,
                                             std::uint32_t count) noexcept {
  if (maxParticles_ == 0) {
    // The stopped state — no log (the stopped-state precedent).
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  if (id < 1 || id > emitterCount_) {
    // Precondition failure (unknown emitter) — no log (the M2-PAR-01
    // setUvOffset precedent).
    return laige::Status(laige::ErrorCode::InvalidArgument);
  }
  emitFrom(id - 1, count);
  maybeWarnOverflow();
  return laige::Status{};
}

template <typename Backend>
void ParticleSystem<Backend>::update() noexcept {
  if (maxParticles_ == 0) {
    return;  // The stopped state — no-op (the stopped-state precedent).
  }
  // 1. Advance the currently live particles (the header's per-tick
  //    contract). The kill is a swap removal (the last live particle
  //    moves into the dead slot); the swapped-in particle — not yet
  //    advanced this tick — is re-examined at the same index.
  std::uint32_t i = 0;
  while (i < liveCount_) {
    Particle& p = pool_[i];
    ++p.age;
    p.pos = M::add(p.pos, p.vel);
    if (p.age >= p.life) {
      --liveCount_;
      pool_[i] = pool_[liveCount_];
    } else {
      ++i;
    }
  }
  // 2. Emit from every emitter in registration order.
  for (std::uint32_t e = 0; e < emitterCount_; ++e) {
    const std::uint32_t rate = emitters_[e].continuousRate;
    if (rate != 0) {
      emitFrom(e, rate);
    }
  }
  // 3. The overflow report (at most one warn per tick) + the window
  //    reset.
  maybeWarnOverflow();
  droppedSinceUpdate_ = 0;
  overflowWarnedSinceUpdate_ = false;
}

}  // namespace laige
