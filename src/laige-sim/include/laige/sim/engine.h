// laige-sim headless engine run (M1-HEAD-01).
//
// FR-1.6 (the entire engine, except presentation, runs without a
// window/GPU — required for servers, CI, and replay tooling);
// ARCH-003 (headless builds without graphics, audio, input, or
// windowing; server code must not transitively depend on client UI);
// AC-6.2 (server mode builds headless on Linux and Windows); PRD §10.2
// (simulation is single-threaded); CONC-006 (ordered, testable,
// idempotent shutdown). This header ships the headless engine run:
//
//   EngineConfig      The typed configuration the engine consumes
//                     (tick rate, scene entity budget, the G-R4
//                     per-frame churn budget).
//   parseEngineConfig The JSON -> EngineConfig loader (the provisional
//                     M1-HEAD-01 config surface; M1-CFG-01 owns the
//                     full declarative config schema).
//   Engine            The headless engine object: config -> world ->
//                     systems -> loop. It owns the World, the
//                     PresentationSnapshot, and the GameLoop; it runs
//                     the loop in run_headless() and shuts everything
//                     down in a fixed order (shutdown()).
//
// ---------------------------------------------------------------------------
// Lifecycle (config -> world -> systems -> loop)
// ---------------------------------------------------------------------------
//
// The engine is the M1 owner of the wiring the M1 steps deferred to it
// (game_loop.h "Per-tick presentation hook": "M1-HEAD-01 owns the
// wiring"; presentation.h "Misuse warnings": "the engine reads [the
// frame clock] once per frame and passes it to both"):
//
//   1. Engine::create(config)
//        Validates the typed config, creates the World (the scene
//        budget, churn budget, seed, and determinism mode from the
//        config), and registers the built-in component matching the
//        configured SimMath backend (Position2DFpx16 by default,
//        Position2DFp32 for float_pinned_32 — M1-DET-01) FIRST (the
//        engine's built-ins always precede the game's components: a
//        stable registration order for the deterministic
//        ComponentTypeIds, ARCH-010).
//   2. Game setup (the game's setup phase, on the engine's world):
//        world->registerComponent<T>(), world->registerSystem(def,
//        Io<...>...) — the M1 systems are plain C++ functions
//        (system.h); a game binary registers its own before the run.
//   3. run_headless(maxTicks, frameBudgetTicks)
//        Computes the schedule (World::scheduleSystems), creates the
//        GameLoop (with the presentation onTick hook), runs one first
//        frame (it establishes the loop's start reference and runs
//        zero ticks — game_loop.h), creates the PresentationSnapshot
//        anchored on THAT start reference (exact, presentation.h
//        "alpha contract"), and then drives frames against the
//        headless monotonic clock until maxTicks ticks have completed
//        (or forever, maxTicks == 0 — the server form).
//   4. shutdown()
//        The ordered CONC-006 shutdown (below). run_headless() always
//        ends in a shutdown; shutdown() is public and idempotent so a
//        failed run or a pre-run teardown needs no special path.
//
// ---------------------------------------------------------------------------
// run_headless contract
// ---------------------------------------------------------------------------
//
//   maxTicks == 0   run until the process ends (the headless server
//                   form; bounded runs are the M1 form).
//   maxTicks  > 0   the run completes once the loop's completed tick
//                   count REACHES maxTicks. A healthy machine lands
//                   exactly on maxTicks; a frame running more than
//                   frameBudgetTicks of due ticks may overshoot by up
//                   to frameBudgetTicks - 1 (the GameLoop's bounded
//                   catch-up — game_loop.h), so the final count is
//                   maxTicks or a little above under overload.
//
// The frame cadence is driven by the headless monotonic clock
// (steady_clock — the M1-LOOP-01 clock source; the windowed clock
// arrives with M2-GL-02): each frame calls GameLoop::frame() (which
// reads the clock and runs the frame's due ticks, bounded by
// frameBudgetTicks, dropping and logging the excess — the M1-LOOP-01
// overload contract), then hands the SAME frame's clock reading to
// PresentationSnapshot::onRenderFrame (the presentation.h wiring
// guarantee), then sleeps until the next tick's due time (one bounded
// sleep per frame — the headless pacing; a server that must react
// faster than one tick raises the tick rate, it does not spin).
//
// Failure: a failed frame (a stale or malformed schedule — the loop's
// runSystems validation) stops the run: run_headless returns that
// Status and the engine is still shut down (CONC-006: shutdown always
// happens). The run's lifecycle events (engine/run_started,
// engine/run_finished, Info) bracket the run — low volume, one pair
// per run.
//
// ---------------------------------------------------------------------------
// The ordered shutdown (CONC-006: systems -> world -> pools ->
// logging flush)
// ---------------------------------------------------------------------------
//
//   1. systems   the loop is destroyed first: no frame can start
//                after the shutdown begins (the system phase stops).
//   2. world     world->clear() destroys every live entity (the
//                per-entity component data is released with its rows;
//                the registries survive, entity.h).
//   3. pools     the world's backing storage is released (per-slot
//                tables, the archetype table and column blocks, the
//                type-key index) and the presentation snapshot's
//                per-slot record table is released.
//   4. logging   the logging facade's controlled shutdown: the
//                pending rate-limit summaries drain, the sink
//                flushes, and the facade retires (LOG-007).
//
// shutdown() is IDEMPOTENT (CONC-006): a second call — and the
// destructor's call — is a no-op. It is safe before a run (nothing
// started: only the world is released), after a run (the normal
// path), and after a failed run.
//
// ---------------------------------------------------------------------------
// The config surface (PROVISIONAL — M1-CFG-01 owns the final schema)
// ---------------------------------------------------------------------------
//
// M1-CFG-01 (unchecked at this step's start) will land the full
// declarative config.json: versioned schema, unknown-key handling,
// budgets, camera defaults, asset roots, and the determinism block's
// final placement (M1-DET-01 lands the seed and determinism keys on
// this PROVISIONAL surface; M1-CFG-01 owns the final schema). Until
// then, parseEngineConfig reads the SUBSET the headless run consumes,
// from an unversioned top-level JSON object:
//
//   "tick_rate_hz"            integer, 20..120   (default 60)
//   "entity_budget"           integer, 0..65536  (default 0: no
//                              entities — every World::create fails
//                              with BudgetExhausted; set it to the
//                              scene's declared budget, G-R3)
//   "churn_per_frame_budget"  integer, >= 0      (default 256; 0
//                              disables the G-R4 guardrail)
//   "seed"                    integer, 0..2^53   (default 0 — the
//                              ADR 0003 JSON bound: exact doubles to
//                              2^53; the programmatic
//                              EngineConfig.seed accepts the full
//                              64 bits. M1-DET-01: the master
//                              simulation seed, part of the replay
//                              identity)
//   "determinism"             object (default {})
//     "enabled"               bool             (default true —
//                              deterministic by default, S-7)
//     "math"                  "fixed_point_16_16" | "float_pinned_32"
//                              (default "fixed_point_16_16" — the
//                              ADR 0002 backend ids)
//
// Missing keys take the defaults; unknown keys are WARNED (one
// config/unknown_key per key, forward-compat) and ignored; a wrong
// type, a non-integer, or an out-of-range value is one rate-limited
// warn (subsystem "config") plus InvalidArgument (FR-12.3, never
// silent). M1-CFG-01 replaces/extends this loader; the keys above are
// expected to carry over into the versioned schema unchanged.
//
// ---------------------------------------------------------------------------
// Built-in components and the determinism scope (ARCH-009/010)
// ---------------------------------------------------------------------------
//
// M1-DET-01: the engine runs the SimMath backend selected by
// config.determinism.math (ADR 0002, factory-selected at init):
// FixedPoint16_16 (default) registers Position2DFpx16 and owns a
// PresentationSnapshot<sim::Fpx16_16>; FloatPinned32 registers
// Position2DFp32 and owns a PresentationSnapshot<sim::Fp32Pinned>
// (presentation.h). The engine holds the snapshot through a
// type-erased handle (detail::PresentationHandle) — one code path,
// no virtual dispatch (PERF-006). The game registers the Position2D
// alias matching its backend for its own systems; registering the
// other alias in the same world is a duplicate-component rejection
// (one alias per world, the component.h contract).
//
// Determinism mode (S-7, PRD §10.3): with determinism.enabled (the
// default), every registered system receives its PRNG substream
// (SystemContext::rng; derived from (config.seed, system id) —
// determinism.h), and the simulation STATE after N completed ticks
// is a pure function of (the config, the seed, the registration
// order, the tick count, the inputs). With determinism.enabled =
// false, the substreams are not created (SystemContext::rng is
// nullptr) and the run is not replayable — see determinism.h
// "Determinism mode semantics" and docs/concepts/determinism.md.
//
// What is NOT deterministic: the completed tick count of a bounded
// run (the pacing is platform-sensitive — ARCH-009, the game_loop.h
// determinism scope), the presentation alpha (a wall-clock fact by
// design, presentation.h: never part of replay state or the state
// hash), and the timing diagnostics (system.h). No wall-clock values
// enter authoritative state.
//
// ---------------------------------------------------------------------------
// Ownership, threading
// ---------------------------------------------------------------------------
//
// The Engine is move-only and has exactly one owner thread (CONC-001;
// PRD §10.2): create, the game setup on world(), run_headless, and
// shutdown all run on that thread. The World, the PresentationSnapshot,
// and the GameLoop are owned by the engine (unique state; the loop and
// the snapshot hold non-owning world views — the world outlives them,
// the shutdown order above). A moved-from engine is STOPPED: world()
// is nullptr, run_headless() fails (InvalidArgument, no log — the
// stopped-state precedent), and shutdown() is a no-op.
//
// ---------------------------------------------------------------------------
// Performance (PERF-002/003)
// ---------------------------------------------------------------------------
//
// Per frame (the headless run loop): one clock read, one bounded
// GameLoop::frame() dispatch (itself one clock read + integer ops +
// up to frameBudgetTicks system dispatches — PERF-002 bounded), one
// snapshot onRenderFrame (a few integer ops), and one sleep. No
// allocation and no logging on the healthy path (PERF-003, LOG-003).
// The run's setup path allocates exactly three times, all one-shot
// (verified per-frame-zero by the M1-HEAD-01 zero-allocation test):
// the GameLoop object, the PresentationSnapshot object, and the
// presentation slot record table (24 B/entity slot, sized by the
// scene budget — the presentation.h storage contract). The drop
// path is cold (one rate-limited warn per overload frame — the
// M1-LOOP-01 contract).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - Register game components/systems on world() BEFORE
//     run_headless: the schedule is computed once, at the start of
//     the run. A registration after the schedule makes the schedule
//     stale — every frame fails with system/schedule_stale (the
//     GameLoop's documented behavior; the run stops and reports it).
//   - One live engine per world is not expressible: the engine OWNS
//     its world. Never drive the engine's world from outside
//     run_headless during a run (the owner thread, CONC-001).
//   - maxTicks == 0 is the server form: the run never returns on its
//     own. Stop the process (the OS signal form); a controlled stop()
//     arrives with the server work (M6), not here.
//   - The headless clock is steady_clock: it is monotonic by
//     definition, but it is a WALL-CLOCK base (ARCH-009) — the number
//     of wall-clock seconds a run of N ticks takes is not part of the
//     deterministic contract.
//   - run_headless after shutdown (or on a moved-from engine) is a
//     no-op failure: recreate the engine, do not reuse it.

#pragma once

#include <cstdint>
#include <memory>

#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/result.h"
#include "laige/sim/determinism.h"  // SimMathBackend, DeterminismConfig (M1-DET-01)
#include "laige/sim/entity.h"       // World, kDefaultChurnPerFrameBudget
#include "laige/sim/game_loop.h"    // GameLoop, GameLoopStats, tick-rate constants
#include "laige/sim/presentation.h" // Position2D, PresentationSnapshot

namespace laige {

class JsonValue;  // declared in laige/json.h; only a const reference is used

// The default master simulation seed (M1-DET-01; CORE-005). 0 is a
// valid master seed: the Prng's state is nonzero for every 64-bit
// seed (the xorshift128+ state transform — laige/prng.h), so no
// special invalid seed is needed.
inline constexpr std::uint64_t kDefaultSimulationSeed = 0;

// The typed headless-engine configuration (M1-HEAD-01; the provisional
// config surface — see the header preamble "The config surface").
// A plain value: the engine copies it into the EngineConfig echo read
// back through config().
struct EngineConfig {
  // The simulation tick rate in HERTZ (FR-1.1: 20-120 validated at
  // Engine::create; default kDefaultTickRateHz).
  std::uint32_t tickRateHz{kDefaultTickRateHz};
  // The declared scene budget (G-R3): the World's entity capacity.
  // 0 = an empty scene (a valid world that creates no entities —
  // entity creation on it fails with BudgetExhausted; the game
  // declares its budget, the engine does not guess one).
  std::uint32_t entityCapacity{0};
  // The G-R4 per-frame component-churn budget (0 disables the
  // guardrail; default kDefaultChurnPerFrameBudget).
  std::uint32_t churnPerFrameBudget{kDefaultChurnPerFrameBudget};
  // The master simulation seed (M1-DET-01; PRD §10.3: the seed is
  // part of the replay identity). Every system's PRNG substream is
  // derived from (seed, system id) — the Prng::deriveSubstream
  // contract (laige/prng.h). Default kDefaultSimulationSeed (0 — a
  // valid master seed: the Prng's state is nonzero for every 64-bit
  // seed, prng.h). The programmatic path accepts the full 64 bits;
  // the JSON config path is bounded to exact integers in 0..2^53
  // (the ADR 0003 number policy — parseEngineConfig's documented
  // limit).
  std::uint64_t seed{kDefaultSimulationSeed};
  // The determinism block (M1-DET-01; ADR 0002): the mode flag and
  // the selected SimMath backend. See the header preamble "The
  // config surface" for the JSON keys and "Built-in components and
  // the determinism scope" for the semantics; the full promised
  // scope is docs/concepts/determinism.md.
  DeterminismConfig determinism{};
};

// Load the headless-engine configuration from a parsed JSON document
// (the provisional M1-HEAD-01 config surface; M1-CFG-01 owns the full
// declarative schema — see the header preamble for the keys, the
// defaults, and the rejection table). The document must be a
// top-level object; every accepted key is optional (defaults above).
//
//   document not an object        -> InvalidArgument + warn
//                                     (config/not_an_object)
//   "tick_rate_hz" not a number, not an exact integer, or outside
//                               20..120                -> InvalidArgument + warn
//                                     (config/tick_rate_invalid)
//   "entity_budget" not a number, not an exact integer,
//                               or outside 0..65536    -> InvalidArgument + warn
//                                     (config/entity_budget_invalid)
//   "churn_per_frame_budget" not a number, not an exact
//                               integer, or < 0        -> InvalidArgument + warn
//                                     (config/churn_budget_invalid)
//   "seed" not a number, not an exact integer, or
//                               outside 0..2^53        -> InvalidArgument + warn
//                                     (config/seed_invalid)
//   "determinism" not an object   -> InvalidArgument + warn
//                                     (config/determinism_invalid)
//   "determinism.enabled" not a   -> InvalidArgument + warn
//       boolean                   (config/determinism_enabled_invalid)
//   "determinism.math" not a      -> InvalidArgument + warn
//       string, or not one of     (config/determinism_math_invalid)
//       the two ADR 0002 ids
//   unknown key (top-level or    -> Warn only (config/unknown_key,
//       inside "determinism")     forward-compat — M1-CFG-01's
//                                 rule); the key is ignored
//
// Cold path (config load); O(keys), allocates only for the warn
// fields. First failure wins; on failure the config is not returned.
// @budget O(document keys); cold path, warn fields allocate only when a key is rejected.
[[nodiscard]] Result<EngineConfig, ErrorCode>
parseEngineConfig(const JsonValue& doc) noexcept;

namespace detail {

// The engine's presentation snapshot, type-erased (M1-DET-01): one
// owned snapshot of the configured SimMath backend
// (PresentationSnapshot<sim::Fpx16_16> or
// PresentationSnapshot<sim::Fp32Pinned>) behind plain function
// pointers — no virtual dispatch (PERF-006), no std::function, no RTTI
// (the engine-policy laige_apply_engine_policy). The engine holds
// exactly one (a setup-path object — the third of the run's three
// setup allocations, engine.h "Performance"); the dispatch is a
// direct function-pointer call per completed tick / per frame.
//
// Empty (no snapshot) is the pre-run state; the engine's sequence
// guarantees the snapshot exists before onTickHookDispatch can fire
// and before runFrames runs — the checks below are the
// never-crash guards for the unreachable states (CORE-008: a null
// context is a no-op, never a crash).
// The dispatch function-pointer types (the game_loop.h TickFn
// pattern: an alias keeps the member declarations parseable and the
// handle's dispatch a plain function-pointer call).
using PresentationTickFn = void (*)(void* context, std::uint64_t tick);
using PresentationRenderFn = void (*)(void* context, std::int64_t nowNs);

// The deleter signature of PresentationHandle::storage: a function
// pointer capturing the concrete snapshot type (the factory template
// below provides it per backend; the logging.h stream_ precedent —
// unique_ptr<void, fnptr> is the engine's type-erasure idiom).
using SnapshotDeleteFn = void (*)(void*);

struct PresentationHandle {
  // The owned snapshot storage (one-shot setup allocation); the
  // deleter knows the concrete backend type (the factory template
  // below captures it).
  std::unique_ptr<void, SnapshotDeleteFn> storage;
  // The snapshot as a void pointer for the dispatch (== storage's
  // pointer, kept for the direct calls).
  void* context{};
  // The snapshot's onTick (world-independent: the snapshot owns its
  // non-owning world view, presentation.h).
  PresentationTickFn onTick{};
  // The snapshot's onRenderFrame (the frame clock, ns).
  PresentationRenderFn onRenderFrame{};

  // The empty state (no snapshot yet / after reset). User-provided:
  // the unique_ptr's own default constructor is deleted for a
  // function-pointer deleter ([unique.ptr.singlector]), so the
  // empty state is constructed here (the handle is therefore not an
  // aggregate — constructed only as Engine::snapshot_ and by the
  // factory below; moved, never copied).
  PresentationHandle() : storage(nullptr, nullptr) {}

  // True when a snapshot is owned. O(1).
  [[nodiscard]] bool hasSnapshot() const noexcept {
    return context != nullptr;
  }

  // Release the owned snapshot (no-op when empty); used by the
  // engine's ordered shutdown (the "pools" step).
  void reset() noexcept {
    storage.reset();
    context = nullptr;
    onTick = nullptr;
    onRenderFrame = nullptr;
  }
};

// Builds the PresentationHandle for one concrete backend
// (PresentationSnapshot<Backend>): creates the snapshot and wraps it.
// One allocation (the snapshot object — its slot table is the run's
// third setup allocation). The error path is a plain Result error
// (the snapshot's create contract, presentation.h).
template <typename Backend>
[[nodiscard]] Result<PresentationHandle, ErrorCode>
createPresentationHandle(World& world, std::int64_t startReferenceNs,
                         std::uint32_t tickRateHz) noexcept {
  using Snapshot = PresentationSnapshot<Backend>;
  const typename Snapshot::Options options{tickRateHz};
  Result<Snapshot, ErrorCode> created =
      Snapshot::create(world, startReferenceNs, options);
  if (created.isError()) return created.error();
  std::unique_ptr<Snapshot> snapshot =
      std::make_unique<Snapshot>(std::move(created).takeValue());
  PresentationHandle handle;
  // The deleter captures the concrete type (the one-shot setup
  // allocation's release); context is the same pointer for the
  // direct dispatch calls.
  handle.storage = std::unique_ptr<void, SnapshotDeleteFn>(
      snapshot.release(),
      [](void* p) { delete static_cast<Snapshot*>(p); });
  handle.context = handle.storage.get();
  handle.onTick = [](void* context, std::uint64_t tick) {
    static_cast<Snapshot*>(context)->onTick(tick);
  };
  handle.onRenderFrame = [](void* context, std::int64_t nowNs) {
    static_cast<Snapshot*>(context)->onRenderFrame(nowNs);
  };
  return handle;
}

}  // namespace detail

// The headless engine (M1-HEAD-01): config -> world -> systems ->
// loop, then the ordered CONC-006 shutdown. See the header preamble
// for the lifecycle, the run contract, the shutdown order, the config
// surface, the determinism scope, and the misuse warnings.
class Engine {
 public:
  // Setup phase (the engine's only backing allocations happen in the
  // World's create — the registry tables and, when capacity > 0, the
  // per-slot tables): validate the typed config, create the World
  // (entityCapacity, churnPerFrameBudget, seed, determinism mode),
  // and register the built-in component matching the configured
  // SimMath backend (Position2DFpx16 default, Position2DFp32 for
  // float_pinned_32 — M1-DET-01; the engine's built-ins always come
  // first — ARCH-010). O(1) beyond the World's setup allocations.
  //
  //   tickRateHz outside 20..120    -> InvalidArgument + warn
  //                                      (config/tick_rate_invalid)
  //   entityCapacity > 65536        -> InvalidArgument (the World's
  //                                      validation — no warn there,
  //                                      the World::create precedent)
  //   built-in registration failure -> propagated (unreachable on a
  //                                      fresh world; never silent)
  [[nodiscard]] static Result<Engine, ErrorCode> create(const EngineConfig& config) noexcept;

  // The engine's world (the game setup phase: register components and
  // systems here, BEFORE run_headless). nullptr after shutdown or on a
  // moved-from engine (CPP-008 nullability; the stopped-state
  // precedent). O(1), no side effects.
  [[nodiscard]] World* world() noexcept;

  // The engine configuration echo (the validated values). O(1), no
  // side effects.
  [[nodiscard]] const EngineConfig& config() const noexcept;

  // Run the headless engine: compute the schedule, create the loop
  // (with the presentation onTick hook) and the snapshot, drive frames
  // until maxTicks ticks have completed (0 = the server form: run
  // until the process ends), then shut down (always — even on a
  // failed frame; CONC-006). One engine run per engine: a second call
  // (after any outcome) fails with InvalidArgument without logging
  // (the stopped-state precedent).
  //
  //   frameBudgetTicks == 0         -> InvalidArgument + warn
  //                                      (loop/catchup_invalid — the
  //                                      GameLoop's validation)
  //   schedule failure              -> the scheduleSystems Status
  //                                      (system/* — world-emitted);
  //                                      the engine is not started,
  //                                      but is still shut down
  //   a failed frame mid-run        -> that frame's Status
  //                                      (system/* — the loop's
  //                                      runSystems dispatch)
  //   success                       -> ok
  //
  // The run emits one engine/run_started (Info, before setup) and one
  // engine/run_finished (Info, after the last frame, before the
  // shutdown flush) — the lifecycle pair (AGENTS §14 Info contract).
  // @budget O(maxTicks x per-tick system work), bounded per frame by frameBudgetTicks (PERF-002); setup allocates three one-shot objects (the GameLoop, the PresentationSnapshot, and the snapshot slot table); the frame path allocates nothing.
  [[nodiscard]] Status run_headless(std::uint64_t maxTicks,
                                    std::uint32_t frameBudgetTicks = kDefaultMaxCatchUpTicks) noexcept;

  // The ordered, IDEMPOTENT shutdown (the header preamble "The
  // ordered shutdown": loop -> world clear -> storage release ->
  // logging flush). Safe before a run, after a run, and after a
  // failed run; the destructor calls it. O(world clear cost); no
  // logging on the success path beyond the facade's own flush.
  void shutdown() noexcept;

  // True once shutdown() has completed (or on a moved-from engine).
  // O(1), no side effects.
  [[nodiscard]] bool isShutDown() const noexcept;

  // The last run's loop accounting (frames, ticks, droppedTicks,
  // droppedFrames — the GameLoopStats since the run's loop
  // construction; all zeros before the first run). O(1), no
  // allocation, no side effects (the profiler feed, M1-PROF-01).
  [[nodiscard]] GameLoopStats stats() const noexcept;

  // Move transfers the owned state; the source becomes a STOPPED
  // engine (world() nullptr, run_headless fails, shutdown is a no-op
  // — the GameLoop moved-out precedent).
  Engine(Engine&& other) noexcept;
  Engine& operator=(Engine&& other) noexcept;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // The destructor shuts down (CONC-006: owned work is released even
  // when the caller forgets shutdown()).
  ~Engine() noexcept;

 private:
  // The factory path (create).
  Engine();

  // The GameLoop's per-completed-tick hook (M1-LOOP-02 wiring): the
  // loop is created with this hook from its first frame, but the
  // first frame runs zero ticks (game_loop.h), so the snapshot exists
  // by the time the hook can fire; a null snapshot (a failed setup
  // between frames — unreachable in the engine's sequence) is a
  // no-op, never a crash.
  static void onTickHook(void* context, World& world,
                         std::uint64_t tick) noexcept;
  void onTickHookDispatch(std::uint64_t tick) noexcept;

  // The frame drive (the run_headless contract: one clock read, one
  // bounded loop frame, one snapshot refresh, one sleep per frame;
  // stops at maxTicks — 0 = run until the process ends).
  [[nodiscard]] Status runFrames(std::uint64_t maxTicks) noexcept;

  // The owned state (all released in shutdown, in the documented
  // order).
  std::unique_ptr<World> world_;
  std::unique_ptr<GameLoop> loop_;
  // The presentation snapshot, type-erased over the configured
  // SimMath backend (M1-DET-01; detail::PresentationHandle — no
  // virtual dispatch, PERF-006).
  detail::PresentationHandle snapshot_{};
  // The run's execution order (computed at the start of the run; a
  // plain value — the SystemSchedule ownership contract, system.h).
  SystemSchedule schedule_{};
  EngineConfig config_{};
  // The last run's loop accounting (set on every run completion,
  // including a failed one — before the loop is destroyed).
  GameLoopStats lastStats_{};
  // True after shutdown() has run (or on a moved-from engine).
  bool shutDown_{false};
};

}  // namespace laige
