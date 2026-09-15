# Per-tick presentation snapshot and interpolation state (`PresentationSnapshot`, M1-LOOP-02)

The M1 presentation-state half of the game loop (M1-LOOP-02; PRD
FR-1.1 — the 2D-aware half of render interpolation; ARCH-009 —
presentation state is separated from authoritative simulation state;
PRD §4 — depth is presentation-only, the logical simulation is 2D):
once per **completed** tick, the world's `Position2D` state is
captured as a `prev`/`curr` pair, and each render frame computes the
interpolation **alpha** between the last two tick states — clamped to
[0, 1], **never extrapolating**. Public header:
`src/laige-sim/include/laige/sim/presentation.h` (`Position2D`,
`PresentationSnapshot`, the aliases, the full contract). Header-only
(class template — one instantiation per SimMath backend, ADR 0002).
Unit suite: `ctest -R presentation`
(`tests/laige-sim/presentation_tests.cpp`).

```cpp
World world = ...;                       // M1-ECS
world.registerComponent<Position2DFpx16>();
Entity e = world.create().value();
world.addComponent<Position2DFpx16>(e, ...);

// The loop drives the refresh (the M1-HEAD-01 engine wiring):
GameLoop::Options lopts;
lopts.onTick = [](void* ctx, laige::World&, std::uint64_t tick) noexcept {
  static_cast<PresentationSnapshot<laige::sim::Fpx16_16>*>(ctx)->onTick(tick);
};
lopts.onTickContext = &snap;
GameLoop loop = GameLoop::create(world, sched, lopts).value();
loop.frame();                           // establishes the start reference

while (running) {
  const std::int64_t nowNs = clockNow(); // ONE reading, same clock as the loop
  loop.frame();
  snap.onRenderFrame(nowNs);            // alpha := (nowNs − anchor) / tick_dt
  auto pos = snap.sample_position(e);   // interpolated 2D position
  // ... render `pos` (projection-correct rendering is M2's job)
}
```

## The first built-in component: `Position2D`

`laige::Position2D<Backend>` — the entity's 2D simulation-space
position as the selected SimMath backend's `Vec2` (ADR 0002). The
two `LAIGE_COMPONENT` marks register both instantiations:

- `Position2DFpx16` — the default backend (fpx16_16, Q16.16):
  deterministic/lockstep zones;
- `Position2DFp32` — the opt-in IEEE-float backend (fp32_pinned) for
  float zones.

It is the first built-in component of `laige-sim`: a trivially
copyable 16-byte data carrier (one `Vec2`), registered per world like
any component (`World::registerComponent<T>`). The 2.5D depth axis
(`z`) stays presentation-side (PRD §4): the simulation logic is 2D,
and M2's camera/projection is the boundary that adds the third
dimension (RENDER-006).

## The per-tick snapshot production (FR-1.1, ARCH-009)

`PresentationSnapshot<Backend>::onTick(tick)` refreshes, for **every
live entity with a `Position2D`**:

- `prev ← curr` (the end-of-tick T−1 value),
- `curr ← the world's current value` (the end-of-tick T value),

and records `lastTick = T`. The refresh is O(entities with a
`Position2D`) over the bounded archetype scan (`World::each`) — one
visit per entity, no allocation (the `SlotRecord` table is pre-
reserved; see Performance).

The `GameLoop` drives it: `Options::onTick` (game_loop.md) fires
**after every completed tick** — a failed tick is not counted and
does not fire the hook (the state it would have captured never
happened; the previous `prev`/`curr` are retained). Headless tests
drive `onTick` manually with the same world and tick numbers.

The snapshot **never mutates the world**: `prev`/`curr` are pure
copies of authoritative state (ARCH-009). Interpolation reads
authoritative state; it never writes it.

## New entities snap to `curr`

An entity first seen at a refresh — created before the first tick, or
added **between** ticks (after the last `onTick`, before the next
one) — has no end-of-tick T−1 state: both `prev` and `curr` are set
to its **current** value. It renders at its spawn position (no
phantom interpolation from an older state), and from the second tick
after creation it interpolates normally (`sample_position` returns
the exact current value until then — the
`EntityAddedBetweenTicksSnapsToCurr` test pins it).

The same rule re-applies after a slot recycle: `destroy()` bumps the
slot's generation, so a new occupant of a recycled slot is a new
entity (it snaps). The record stores the occupant's generation at
activation and checks it on every refresh — slot recycling
self-heals. (The 2^16 generation wraparound carries the same
accepted caveat as the entity handles themselves, entity.md.)

## The alpha contract (exact integer math, clamped, ARCH-010)

The anchor of tick `T` is the clock time at which tick `T` is due —
the same exact rational the `GameLoop`'s due computation uses
(game_loop.md, "The exact due computation"):

```
A(T) = startNs + T × 10⁹ / rate        (nanoseconds, rational)
```

where `startNs` is the loop's start reference (the clock reading of
the loop's first frame — `GameLoop::startReferenceNs()`). At render
time `R`, after `lastTick = T` completed ticks, the presented state
lags the simulation by exactly one tick (the classic fixed-timestep
interpolation — it never shows a state the simulation has not yet
produced):

```
alpha = (R − A(T)) / tick_dt = (R − A(T)) × rate / 10⁹   ∈ [0, 1)
```

- `alpha = 0` at `R = A(T)` (render `prev` — the end-of-tick T−1
  state);
- `alpha → 1` as `R → A(T+1)` (render `curr` — the end-of-tick T
  state).

The arithmetic is **exact integer math** — no floating accumulator
(ARCH-010): with `elapsed = R − startNs` split as
`seconds`/`remainder`,

```
alpha × 10⁹ = (seconds × rate − T) × 10⁹ + remainder × rate
```

and the seconds/remainder split keeps every intermediate product
overflow-free for any 64-bit clock reading (the `ticksDue`
precedent, game_loop.cpp). The result is **clamped to [0, 1] —
never extrapolates**:

| Render time `R` (with `lastTick = T`) | Result |
|---|---|
| `R < A(T)` (earlier than the tick's due time — mismatched start reference / non-monotonic render clock) | alpha clamps to **0** (render `prev`) |
| `A(T) ≤ R < A(T+1)` (the normal window) | the exact fraction — preserved |
| `R ≥ A(T+1)` (a full tick or more past the anchor — a clock jump) | alpha clamps to **1** (render `curr`) |
| before the first completed tick | alpha **0**, every sample snaps (no refresh has run) |

A clamped sample still lies on the segment between the two known
states: the lerp of `prev`/`curr` never produces a position the
simulation did not occupy.

**Time base:** `R` (the `onRenderFrame` argument) and `startNs` must
be on the **same monotonic epoch** as the loop's clock
(`GameLoop::Options::nowNs`). The engine reads the clock **once per
frame** and passes the same reading to both the loop and the
snapshot (the M1-HEAD-01 wiring). A backward or off-base reading is
a wiring misuse — the clamp bounds the damage to a stale-but-bounded
sample, never undefined behavior (a reading below `startNs` clamps to
the start).

**Storage:** the alpha is stored as the backend scalar — one
documented rounding per backend (`detail::AlphaConversion`):
Fp32Pinned, one binary32 division (the pinned IEEE op); Fpx16_16, one
integer round-to-nearest into Q16.16 raw units (no float
intermediate). The alpha is a **wall-clock fact** (the render time):
non-deterministic by design, and never part of replay state or the
simulation state hash (ARCH-009/010; M1-DET-03 excludes it).

## Sample semantics (`sample_position`)

`PresentationSnapshot::sample_position(e)` — the name is the
roadmap's exact API; the engine is otherwise camelCase:

| Input | Result |
|---|---|
| live handle + synced record (refreshed after its last tick) | `lerp(prev, curr, alpha)` — SimMath ops only (S-7, ADR 0002) |
| live handle, first seen since the last `onTick` (added between ticks) | **SNAP**: the entity's current `Position2D` value (the documented "new entities snap to `curr`") |
| stale/invalid handle | `InvalidArgument` + **warn-once** (`ecs/stale_entity_access`, the `World::check` precedent — FR-12.3: never silent) |
| live handle **without** a `Position2D` | `InvalidArgument` — a negative query, like `has<T>()` reading false: no warn |
| moved-from snapshot | `InvalidArgument` — no world access, no logging (the `GameLoop` moved-out precedent) |

The lerp is the SimMath backend's `lerp` (exactly
`a + (b − a) × t`, ADR 0002) — linear in `alpha` by construction;
the `LinearInterpolationBetweenTicks` test pins the exact Q16.16
results at alpha 0, 0.5, 0.75, and the single rounding of a near-1
alpha.

## Configuration and validation (create)

`PresentationSnapshot<Backend>::create(world, startReferenceNs,
options)`:

| Parameter | Contract | Reject |
|---|---|---|
| `world` | outlives the snapshot (non-owning view) | — |
| `startReferenceNs` | the loop's start reference after its first frame | a mismatch shifts the anchor; the clamp bounds it to a stale-but-valid sample (no reject) |
| `options.tickRateHz` | 20–120 Hz (`kMinTickRateHz`–`kMaxTickRateHz`, the loop's documented range, FR-1.1); must **equal** the driven loop's rate (the engine's wiring guarantee) | `InvalidArgument` + one rate-limited warn `presentation/tick_rate_invalid` (field `tick_rate_hz`) |

The allocation is one per-world `SlotRecord` table, reserved **once
at `create()`** from the world's capacity (a setup path, never a hot
path). A zero-capacity world yields a zero-size table (legal —
C++20 [ptr.arith]).

## Wiring with the `GameLoop` (M1-HEAD-01 shape)

- **`GameLoop::Options::onTick` / `onTickContext`** — a
  `noexcept` callback fired after every **completed** tick with the
  tick number; the engine wraps
  `PresentationSnapshot::onTick` in a static thunk behind the
  `void*` context (the headless engine does exactly this). The
  callback must not allocate or block (PERF-002/003) — the snapshot's
  `onTick` is bounded and allocation-free.
- **`GameLoop::startReferenceNs()`** — the loop's clock reading at
  its first frame: the anchor base for the snapshot's alpha. Read it
  **after** the loop's first frame (before, it is 0 and undefined-
  useful).
- **`onRenderFrame(nowNs)`** — called once per frame with the
  frame's clock reading, **after** `loop.frame()`.

## Ownership, threading, lifetime

The snapshot holds a **non-owning** `World` view (the world
outlives the snapshot — the engine owns both; the `GameLoop`
precedent). One owner thread (PRD §10.2: the simulation thread); not
thread-safe, no synchronization. Move is an O(1) pointer swap: a
moved-from snapshot is **stopped** — every operation fails with
`InvalidArgument`, no world access, no logging (the `GameLoop`
moved-out precedent). Copies are deleted (the unique backing table).

## Determinism scope (ARCH-009/010)

The `prev`/`curr` capture is a pure function of the tick sequence
(bit-exact under the backend's policy — ADR 0002): replaying the same
ticks reproduces the records exactly. The **alpha is not
deterministic** — it is the render-time fact (wall clock), and it is
never part of the simulation state hash or replay state
(M1-DET-03). `sample_position`'s lerp is deterministic **given** the
alpha (backend-exact, ADR 0002).

## Performance (DOC-004)

- **Per tick (`onTick`):** one `World::each<Position2D>` pass —
  O(1) bookkeeping per visiting entity (a `prev ← curr` copy, two
  store writes), plus the bounded archetype scan (the
  `World::each` cost, query.md). **No allocation, no logging**
  (PERF-003, LOG-003) — the table is pre-reserved at `create()`.
- **Per frame (`onRenderFrame`):** a few 64-bit integer ops (the
  exact alpha computation, the clamp) — O(1), no allocation, no
  logging.
- **Per sample (`sample_position`):** O(1) — the handle check
  (`World::check`, warn-once on stale), the component lookup
  (`World::get`, O(1) in the entity count), and one 2D lerp
  (two backend ops). No allocation; logging only on the stale-handle
  warn path (rate-limited).
- **Memory:** one `SlotRecord` per entity slot of the world's
  capacity — 24 bytes each (two 8-byte `Vec2`s + generation + flag,
  both backends), reserved once at setup. A 10k-entity world holds
  ~240 KB — bounded by the scene budget (G-R3), never grown per
  frame.
- **No per-frame heap** on any of the three paths: the
  `HealthyFramesAllocateNothing` test asserts `allocs == 0` over a
  500-entity × 100-frame refresh/sample window (test-only operator-
  new counter; the sanitizer trees prove the same loop leak-free).

## Threading (CONC-001, PRD §10.2)

The snapshot has exactly **one owner thread**: `onTick`/
`onRenderFrame`/`sample_position` run on the world's single owner
thread, strictly interleaved with the tick and frame phases —
`onTick` inside the tick (after the systems), `onRenderFrame` after
`loop.frame()`, `sample_position` in the render phase. The non-
owning world view points at single-owner state and is never
dereferenced off-thread.

## Misuse warnings

- **`tickRateHz` must equal the driven loop's rate.** `create()`
  validates the shared 20–120 Hz range; equality is the engine's
  wiring guarantee — a rate mismatch makes the alpha wrong in a way
  the clamp cannot fix (a stale-but-bounded sample).
- **`startReferenceNs` must be the loop's** —
  `loop.startReferenceNs()` read after the loop's first frame. A
  mismatch shifts the anchor; the clamp bounds the result (a
  stale-but-valid sample), never UB.
- **`onRenderFrame`'s reading must come from the same monotonic
  clock the loop reads** — the engine reads it once per frame and
  passes it to both. A backward reading (below the start reference)
  clamps to the start; an off-base reading yields a stale sample.
- **One snapshot per world is the intended wiring.** Multiple
  snapshots on one world are independent (each drives its own
  refresh) — not a failure, just redundant work.
- **`sample_position` is a render-phase query**: it reads the
  `prev`/`curr` pair and the alpha — call it after
  `onRenderFrame` for the frame's state, never to drive simulation
  (ARCH-009: presentation never feeds back into authoritative state).
