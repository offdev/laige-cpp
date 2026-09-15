// laige-sim presentation snapshot + interpolation state (M1-LOOP-02).
//
// FR-1.1 (render interpolation of positions — the 2D-aware half; the
// projection-correct rendering is M2); ARCH-009 (presentation state
// is separated from authoritative simulation state — interpolation
// MUST NOT mutate authoritative results); PRD §4 (depth is
// presentation-only: the logical simulation is 2D). This header ships
// the M1 presentation-state half of the game loop:
//
//   Position2D<Backend>
//                     The FIRST built-in component: the entity's 2D
//                     simulation-space position as the selected
//                     SimMath backend's Vec2 (ADR 0002 — one template
//                     instantiation per backend, factory-selected at
//                     engine init). The two LAIGE_COMPONENT marks
//                     below register both instantiations (the default
//                     fpx16_16 for deterministic/lockstep zones,
//                     fp32_pinned for opt-in IEEE-float zones).
//   PresentationSnapshot<Backend>
//                     The per-tick presentation state: for every live
//                     entity with a Position2D, the `prev`/`curr`
//                     position pair (the world's state at the end of
//                     the last two completed ticks) plus the frame's
//                     interpolation alpha:
//
//                         alpha = (render_time − last_tick) / tick_dt
//
//                     clamped to [0, 1] — never extrapolates (see the
//                     "alpha contract" below). `sample_position(e)`
//                     returns the interpolated 2D position — pure 2D
//                     math here (projection-correct rendering is
//                     M2's job).
//
// ---------------------------------------------------------------------------
// The per-tick snapshot production (FR-1.1, ARCH-009)
// ---------------------------------------------------------------------------
//
// The snapshot is refreshed ONCE PER COMPLETED TICK:
//
//   onTick(world, tick)   for every live entity with a Position2D:
//                          prev ← curr (the end-of-tick T−1 value),
//                          curr ← the world's current value (the
//                          end-of-tick T value).
//
// The GameLoop drives it: Options::onTick (game_loop.h) fires after
// every COMPLETED tick (a failed tick is not counted and does not
// fire — the state it would have captured never happened). Headless
// tests (and the M1-HEAD-01 engine) may drive onTick manually with
// the same world and tick numbers.
//
// **New entities snap to curr** (documented scope behavior): an
// entity first seen at a refresh — created before the first tick, or
// added BETWEEN ticks (after the last onTick, before the next one) —
// has no end-of-tick T−1 state, so both prev and curr are set to its
// current value. It renders at its spawn position (no phantom
// interpolation from an older state), and from the second tick after
// its creation it interpolates normally. The same rule re-applies
// after a slot recycle: destroy() bumps the slot's generation, so a
// new occupant of a recycled slot is a new entity (snaps) — the
// record's stored generation is checked against the handle's on
// every refresh (the 2^16 wraparound case carries the same accepted
// caveat as the entity handles themselves, entity.h).
//
// The snapshot NEVER mutates the world: the prev/curr pair is a pure
// copy of authoritative state (ARCH-009).
//
// ---------------------------------------------------------------------------
// The alpha contract (FR-1.1: render_time − last_tick over tick_dt)
// ---------------------------------------------------------------------------
//
// The anchor of tick T is the clock time at which tick T is DUE —
// the same exact rational the GameLoop's due computation uses
// (game_loop.h "The exact due computation"):
//
//   A(T) = startNs + T × 10⁹ / rate          (nanoseconds, rational)
//
// where startNs is the loop's start reference (the clock reading of
// the loop's first frame — `GameLoop::startReferenceNs()`). At render
// time R, after lastTick = T completed ticks, the presented state
// lags the simulation by exactly one tick (the classic
// fixed-timestep interpolation — it never shows a state the
// simulation has not yet produced):
//
//   alpha = (R − A(T)) / tickDt = (R − A(T)) × rate / 10⁹  ∈ [0, 1)
//
//   alpha = 0  at R = A(T)  (render prev — the end-of-tick T−1 state)
//   alpha → 1 as R → A(T+1) (render curr — the end-of-tick T state)
//
// The arithmetic is EXACT integer math (ARCH-010: no floating
// accumulator): with elapsed = R − startNs split as seconds/remainder,
//
//   alpha × 10⁹ = (seconds × rate − T) × 10⁹ + remainder × rate
//
// the seconds/remainder split keeps every intermediate product
// overflow-free (the ticksDue precedent, game_loop.cpp). The result
// is CLAMPED to [0, 1] — **never extrapolates**:
//
//   - R below the anchor (a render reading earlier than the tick's
//     due time — a mismatched start reference or a non-monotonic
//     render clock): alpha clamps to 0 (render prev).
//   - R at or beyond the next anchor (a render reading a full tick or
//     more past the anchor — a clock jump): alpha clamps to 1 (render
//     curr).
//
// A clamped sample still lies on the segment between the two known
// states — interpolation of prev/curr never produces a position the
// simulation did not occupy. Before the first completed tick the
// alpha is 0 (and every sample snaps, since no refresh has run).
//
// **Time base:** R (onRenderFrame's argument) and startNs must be on
// the SAME monotonic epoch as the loop's clock (GameLoop::Options::
// nowNs). The engine passes the frame's clock reading; a backward or
// off-base reading is a wiring misuse — the clamp bounds the damage
// (a stale-but-bounded sample), never UB.
//
// **Storage:** the alpha is stored as the backend scalar (one
// documented rounding per backend — `detail::AlphaConversion`:
// Fp32Pinned, one binary32 division; Fpx16_16, one integer
// round-to-nearest into Q16.16). The alpha is a wall-clock fact (the
// render time): it is NON-deterministic by design and never part of
// replay state or the simulation state hash (ARCH-009/010).
//
// ---------------------------------------------------------------------------
// Sample semantics (sample_position)
// ---------------------------------------------------------------------------
//
//   live handle + synced record (refreshed after its last tick)
//       → lerp(prev, curr, alpha) — SimMath ops only (S-7, ADR 0002)
//   live handle, first seen since the last onTick (added between
//       ticks) → SNAP: the entity's current Position2D value
//       (documented: new entities snap to curr)
//   stale/invalid handle → ErrorCode::InvalidArgument + warn-once
//       (the World::check precedent, FR-12.3: never silent)
//   live handle WITHOUT a Position2D → ErrorCode::InvalidArgument
//       (a negative query, like has<T>() reading false — no warn)
//   moved-from snapshot → ErrorCode::InvalidArgument
//       (no world access, no logging — the GameLoop moved-out precedent)
//
// ---------------------------------------------------------------------------
// Storage and allocation (PERF-003, S-2)
// ---------------------------------------------------------------------------
//
// One SlotRecord per entity slot of the world (direct index by
// slot id — O(1), no hashing, no unordered containers, the World
// per-slot-table precedent, M1-ECS-01). The table is reserved ONCE at
// create() from the world's capacity (a setup path, never a hot path)
// — no per-tick or per-frame heap. sizeof(SlotRecord) is 24 bytes
// (two 8-byte Vec2s + generation + flag, both backends).
//
// ---------------------------------------------------------------------------
// Ownership, threading, lifetime
// ---------------------------------------------------------------------------
//
// The snapshot holds a NON-OWNING World view (the world outlives the
// snapshot — the engine owns both; the GameLoop's non-owning-view
// precedent). One owner thread (PRD §10.2: the simulation thread);
// not thread-safe, no synchronization. Move is an O(1) pointer swap;
// a moved-from snapshot is stopped (every operation fails with
// InvalidArgument, no world access, no logging). Copies are deleted.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - Options::tickRateHz must EQUAL the driven GameLoop's tick rate
//     (create() validates the shared 20–120 Hz range; equality is the
//     engine's wiring guarantee — a rate mismatch makes the alpha
//     wrong in a way the clamp cannot fix).
//   - create()'s startReferenceNs must be the loop's start reference
//     (`loop.startReferenceNs()` after the loop's first frame). A
//     mismatch shifts the anchor; the clamp bounds the result to a
//     stale-but-valid sample.
//   - onRenderFrame()'s renderNs must come from the same monotonic
//     clock the loop reads (the engine reads it once per frame and
//     passes it to both — M1-HEAD-01 wiring).
//   - One snapshot per world is the intended wiring; multiple
//     snapshots on one world are independent (each drives its own
//     refresh), not a failure.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/logging.h"
#include "laige/result.h"
#include "laige/sim_math.h"
#include "laige/sim/entity.h"
#include "laige/sim/game_loop.h"

namespace laige {

// ---------------------------------------------------------------------------
// The per-backend alpha conversion (detail: not public API)
// ---------------------------------------------------------------------------

namespace detail {

// The exact alpha numerator (alpha × 10⁹, an integer in [0, 10⁹]) as
// the backend scalar — one documented rounding per backend:
//   Fp32Pinned: one binary32 division (the pinned IEEE op, ADR 0002).
//   Fpx16_16:   one integer round-to-nearest (half-up) into Q16.16
//               raw units — no float intermediate (the exact rational
//               alphaNum/10⁹ maps to ≤ 65536 raw = 1.0, in range).
template <typename Backend>
struct AlphaConversion {
  static sim::SimMath<Backend>::Scalar toScalar(std::uint32_t alphaNum) noexcept;
};

template <>
struct AlphaConversion<sim::Fp32Pinned> {
  static float toScalar(std::uint32_t alphaNum) noexcept {
    return static_cast<float>(alphaNum) / 1e9f;
  }
};

template <>
struct AlphaConversion<sim::Fpx16_16> {
  static fpx16_16 toScalar(std::uint32_t alphaNum) noexcept {
    const std::int64_t scaled = static_cast<std::int64_t>(alphaNum) << 16;
    const std::int64_t raw = (scaled + 500000000LL) / 1000000000LL;
    return fpx16_16{static_cast<std::int32_t>(raw)};
  }
};

// The per-slot record of the presentation snapshot (detail: the
// backing table's layout, not public API). One per entity slot of
// the world (direct index by slot id — O(1), no hashing, no
// unordered containers; the World per-slot-table precedent,
// M1-ECS-01). The table is reserved once at create() — no per-
// tick/per-frame heap (PERF-003).
template <typename Backend>
struct SlotRecord {
  sim::SimMath<Backend>::Vec2 prev{};  // the end-of-tick T−1 value
  sim::SimMath<Backend>::Vec2 curr{};  // the end-of-tick T value
  std::uint16_t generation{};  // the occupant's generation at activation
  bool active{};
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Position2D — the first built-in component (M1-LOOP-02)
// ---------------------------------------------------------------------------

// The entity's 2D simulation-space position (the ground plane —
// PRD §4; the axes/units contract lands with the concepts docs). The
// value is the selected SimMath backend's Vec2 (ADR 0002: one
// template instantiation per backend, factory-selected at engine
// init). A data carrier (S-8): trivially copyable, no behavior —
// the LAIGE_COMPONENT marks below register both instantiations in
// the same path as user components (M1-ECS-02).
template <typename Backend>
struct Position2D {
  sim::SimMath<Backend>::Vec2 pos{};
};

LAIGE_COMPONENT(Position2D<sim::Fpx16_16>);
LAIGE_COMPONENT(Position2D<sim::Fp32Pinned>);

// The two backend instantiations: a game registers the one matching
// its init-time backend selection (ADR 0002, `determinism.math`).
using Position2DFpx16 = Position2D<sim::Fpx16_16>;
using Position2DFp32 = Position2D<sim::Fp32Pinned>;

// ---------------------------------------------------------------------------
// PresentationSnapshot — the per-tick presentation state (M1-LOOP-02)
// ---------------------------------------------------------------------------

template <typename Backend>
class PresentationSnapshot {
 public:
  using Vec2 = sim::SimMath<Backend>::Vec2;
  using Scalar = sim::SimMath<Backend>::Scalar;

  // The typed configuration (API-006): the tick rate, validated to
  // the loop's documented 20–120 Hz range at create() — it must
  // EQUAL the driven GameLoop's rate (the preamble "Misuse
  // warnings").
  struct Options {
    std::uint32_t tickRateHz{kDefaultTickRateHz};
  };

  // Setup path (the only backing allocation: the per-slot record
  // table, sized by world.capacity()). The world outlives the
  // snapshot. startReferenceNs is the driven GameLoop's start
  // reference (0 before the loop's first frame; the preamble "alpha
  // contract"). Rejection: tickRateHz outside 20–120 →
  // ErrorCode::InvalidArgument + one rate-limited warn
  // (presentation/tick_rate_invalid) — FR-12.3/CORE-008, never
  // silent.
  [[nodiscard]] static Result<PresentationSnapshot, ErrorCode>
  create(World& world, std::int64_t startReferenceNs,
         Options options) noexcept;

  // One COMPLETED tick (the GameLoop's onTick hook fires this after
  // every completed tick; tests may drive it manually): rolls prev
  // ← curr and refreshes curr from the world's current Position2D
  // values (the preamble "per-tick snapshot production"). Cost:
  // O(bounded archetype scan + matching live entities), no
  // allocation, no logging (LOG-003). A rejected refresh (a nested
  // iteration — a caller misuse) leaves the previous tick's
  // prev/curr in place (the guard's event carries the failure);
  // lastTick still records the tick.
  void onTick(std::uint64_t tick) noexcept;

  // One presentation frame: recomputes the stored alpha from
  // renderNs (the preamble "alpha contract"): exact integer anchor
  // arithmetic, clamped to [0, 1] (never extrapolates). A few
  // integer ops; no allocation, no logging.
  void onRenderFrame(std::int64_t renderNs) noexcept;

  // The frame's interpolation alpha (the backend scalar in [0, 1];
  // 0 before the first completed tick). The M1-PROF-01 / debug
  // overlay feed.
  [[nodiscard]] Scalar alpha() const noexcept;

  // The number of completed ticks the snapshot has seen (0 before
  // the first onTick). The M1-PROF-01 feed; the engine's sync check.
  [[nodiscard]] std::uint64_t lastTick() const noexcept;

  // The interpolated 2D position of `e` (the preamble "Sample
  // semantics"): lerp(prev, curr, alpha) for a synced entity, the
  // current value for one added between ticks (snaps), Invalid-
  // Argument for stale handles (warn-once) and live handles without
  // a Position2D. Cost: O(1) (handle check + component lookup + one
  // 2D lerp); no allocation, no logging (LOG-003).
  [[nodiscard]] Result<Vec2, ErrorCode> sample_position(Entity e) const noexcept;

  // Move: O(1) pointer swap; the moved-from snapshot is stopped
  // (every operation fails with InvalidArgument; no world access,
  // no logging — the GameLoop moved-out precedent).
  PresentationSnapshot(PresentationSnapshot&& other) noexcept;
  PresentationSnapshot& operator=(PresentationSnapshot&& other) noexcept;

  // No copies (the unique backing table).
  PresentationSnapshot(const PresentationSnapshot&) = delete;
  PresentationSnapshot& operator=(const PresentationSnapshot&) = delete;

 private:
  PresentationSnapshot() noexcept = default;

  World* world_{nullptr};  // non-owning view (outlives the snapshot)
  std::unique_ptr<detail::SlotRecord<Backend>[]> records_;
  std::size_t capacity_{0};
  std::int64_t startNs_{0};
  std::uint32_t rate_{kDefaultTickRateHz};
  std::uint64_t lastTick_{0};
  Scalar alpha_{};
  // Cleared on move-out: a stopped snapshot samples nothing.
  bool valid_{true};
};

// ---------------------------------------------------------------------------
// Implementation (header-defined: class template — the M1-ECS-02
// pattern, like World::registerComponent)
// ---------------------------------------------------------------------------

namespace detail {

// Nanoseconds per second (the clock time base; CORE-005 named
// constant — same value and role as the GameLoop's, game_loop.cpp).
inline constexpr std::int64_t kPresentationNsPerSecond = 1000000000LL;

// The stable subsystem name for presentation events (LOG-001).
inline constexpr const char* kPresentationSubsystem = "presentation";

}  // namespace detail

template <typename Backend>
Result<PresentationSnapshot<Backend>, ErrorCode>
PresentationSnapshot<Backend>::create(World& world,
                                      std::int64_t startReferenceNs,
                                      Options options) noexcept {
  if (options.tickRateHz < kMinTickRateHz ||
      options.tickRateHz > kMaxTickRateHz) {
    // The tick rate must lie in the loop's documented range (FR-1.1)
    // and equal the driven loop's rate (the preamble "Misuse
    // warnings"). One rate-limited structured warn (LOG-004).
    LAIGE_LOG_WARN(detail::kPresentationSubsystem, "tick_rate_invalid",
                   "tick_rate_invalid | the presentation snapshot's tick "
                   "rate is outside the supported 20-120 Hz range | the "
                   "tick rate must match the driven GameLoop's rate | "
                   "pass the loop's tick rate (the default is 60) | "
                   "docs/api/presentation.md",
                   laige::log::field("tick_rate_hz", options.tickRateHz));
    return ErrorCode::InvalidArgument;
  }
  PresentationSnapshot snap;
  snap.world_ = &world;
  snap.capacity_ = world.capacity();
  // The setup-path allocation (PERF-003): one record per entity slot
  // of the world (a zero-size table for a zero-capacity world is
  // legal — C++20 [ptr.arith]).
  snap.records_ = std::make_unique<detail::SlotRecord<Backend>[]>(snap.capacity_);
  snap.startNs_ = startReferenceNs;
  snap.rate_ = options.tickRateHz;
  return snap;
}

template <typename Backend>
void PresentationSnapshot<Backend>::onTick(std::uint64_t tick) noexcept {
  if (!valid_) return;
  // Roll prev ← curr and refresh curr for every live entity with a
  // Position2D (superset match: an entity matches when its component
  // set CONTAINS Position2D — the snapshot tracks that component
  // only, whatever else the entity carries).
  // First sight (or a generation change after a slot recycle) snaps:
  // prev = curr = the current value (the preamble "per-tick snapshot
  // production").
  const Status s = world_->each<Position2D<Backend>>(
      [this](Entity e, const Position2D<Backend>& pos) {
        detail::SlotRecord<Backend>& rec = records_[e.id];
        if (rec.active && rec.generation == e.generation) {
          rec.prev = rec.curr;
        } else {
          rec.prev = pos.pos;  // new entity: snap to curr
        }
        rec.curr = pos.pos;
        rec.generation = e.generation;
        rec.active = true;
      },
      Read{});
  if (s.ok()) {
    lastTick_ = tick;
  } else {
    // A rejected refresh (a nested iteration — a caller misuse, e.g.
    // onTick driven from inside another each's callback): zero
    // entities were visited (the guard rejects before the first
    // callback, query.h) and it already logged the failure
    // (ecs/iteration_nested) — the previous tick's prev/curr and
    // lastTick stay in place (a stale-but-bounded sample; the guard's
    // event is the report, no duplicate logging, LOG-002).
  }
}

template <typename Backend>
void PresentationSnapshot<Backend>::onRenderFrame(std::int64_t renderNs) noexcept {
  if (!valid_) return;
  if (renderNs < startNs_) renderNs = startNs_;  // no time before the base
  const std::int64_t elapsedNs = renderNs - startNs_;
  // alpha × 10⁹ = elapsed × rate − lastTick × 10⁹, computed in EXACT
  // integer arithmetic with the seconds/remainder split (the ticksDue
  // precedent, game_loop.cpp): every intermediate product stays in
  // range (elapsed < 2^63 ns ≈ 292 years ⇒ seconds × rate ≤ ~1.1×10¹²;
  // lastTick × 10⁹ is never formed). The wiring-correct value lies
  // in [0, 10⁹); the clamps below bound every mismatch case to the
  // documentable range (the preamble "alpha contract" — never
  // extrapolates, never UB).
  std::int64_t num = 0;
  if (lastTick_ != 0) {
    const std::int64_t seconds = elapsedNs / detail::kPresentationNsPerSecond;
    const std::int64_t remainder = elapsedNs % detail::kPresentationNsPerSecond;
    const std::int64_t ticksElapsed = seconds * static_cast<std::int64_t>(rate_);
    const std::int64_t d = ticksElapsed - static_cast<std::int64_t>(lastTick_);
    // alpha × 10⁹ = d × 10⁹ + remainder × rate. Branch bounds keep
    // every product in range (overflow-free, CPP-004):
    //   d ≥ 10⁹            the render is a billion full ticks past the
    //                      anchor (absurd clock jump): clamp to 1.0
    //                      without forming the product.
    //   d < −(rate−1)      the sub-second remainder contributes at most
    //                      rate−1 full due ticks (floor((10⁹−1)×rate/10⁹)
    //                      = rate−1 for rate < 10⁹; worst case 119 over
    //                      the validated 20–120 Hz range), so d below
    //                      that is unambiguously before the anchor
    //                      (the exact value is negative for every
    //                      remainder): clamp to 0.0 without the product.
    //   otherwise          d ∈ [−119, 10⁹−1]: |d × 10⁹| < 10¹⁸ and the
    //                      remainder term < 1.2 × 10¹¹ — exact, then
    //                      clamped to [0, 10⁹] (the wiring-correct value
    //                      is already in range; the clamp bounds a
    //                      broken wiring — clock jump / mismatched base).
    if (d >= detail::kPresentationNsPerSecond) {
      num = detail::kPresentationNsPerSecond;
    } else if (d < -static_cast<std::int64_t>(kMaxTickRateHz - 1)) {
      num = 0;
    } else {
      num = d * detail::kPresentationNsPerSecond +
            remainder * static_cast<std::int64_t>(rate_);
      if (num < 0) num = 0;
      if (num > detail::kPresentationNsPerSecond) num = detail::kPresentationNsPerSecond;
    }
  }
  alpha_ = detail::AlphaConversion<Backend>::toScalar(
      static_cast<std::uint32_t>(num));
}

template <typename Backend>
typename PresentationSnapshot<Backend>::Scalar
PresentationSnapshot<Backend>::alpha() const noexcept {
  return valid_ ? alpha_ : Scalar{};
}

template <typename Backend>
std::uint64_t PresentationSnapshot<Backend>::lastTick() const noexcept {
  return valid_ ? lastTick_ : 0;
}

template <typename Backend>
Result<typename PresentationSnapshot<Backend>::Vec2, ErrorCode>
PresentationSnapshot<Backend>::sample_position(Entity e) const noexcept {
  if (!valid_) return ErrorCode::InvalidArgument;  // stopped: no world access
  if (!world_->isValid(e)) {
    // Stale/invalid handle: the World::check precedent (FR-12.3) —
    // warn-once through the facade's rate limiting, then fail.
    static_cast<void>(world_->check(e));
    return ErrorCode::InvalidArgument;
  }
  const Position2D<Backend>* pos = world_->get<Position2D<Backend>>(e);
  if (pos == nullptr) {
    // A live handle without the component: a negative query (like
    // has<T>() reading false — no warn; the Result IS the report).
    return ErrorCode::InvalidArgument;
  }
  const detail::SlotRecord<Backend>& rec = records_[e.id];
  if (rec.active && rec.generation == e.generation) {
    // Synced (refreshed after its last tick): the interpolated state.
    return sim::SimMath<Backend>::lerp(rec.prev, rec.curr, alpha_);
  }
  // First seen since the last onTick (added between ticks): snap to
  // the current authoritative value (documented scope behavior —
  // new entities snap to curr).
  return pos->pos;
}

template <typename Backend>
PresentationSnapshot<Backend>::PresentationSnapshot(
    PresentationSnapshot<Backend>&& other) noexcept
    : world_(other.world_),
      records_(std::move(other.records_)),
      capacity_(other.capacity_),
      startNs_(other.startNs_),
      rate_(other.rate_),
      lastTick_(other.lastTick_),
      alpha_(other.alpha_),
      valid_(other.valid_) {
  other.world_ = nullptr;
  other.capacity_ = 0;
  other.lastTick_ = 0;
  other.valid_ = false;
}

template <typename Backend>
PresentationSnapshot<Backend>&
PresentationSnapshot<Backend>::operator=(
    PresentationSnapshot<Backend>&& other) noexcept {
  if (this != &other) {
    world_ = other.world_;
    records_ = std::move(other.records_);
    capacity_ = other.capacity_;
    startNs_ = other.startNs_;
    rate_ = other.rate_;
    lastTick_ = other.lastTick_;
    alpha_ = other.alpha_;
    valid_ = other.valid_;
    other.world_ = nullptr;
    other.capacity_ = 0;
    other.lastTick_ = 0;
    other.valid_ = false;
  }
  return *this;
}

}  // namespace laige
