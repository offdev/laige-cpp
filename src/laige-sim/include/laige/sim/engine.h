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
//        config), constructs the always-on profiler (M1-PROF-01 —
//        one object + its two fixed window storages, setup path), and
//        registers the built-in component matching the configured
//        SimMath backend (Position2DFpx16 by default,
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
//                type-key index), the presentation snapshot's
//                per-slot record table is released, and the profiler
//                (M1-PROF-01) is released. A profile report that was
//                started but never finalized (a pre-run teardown, or
//                a run that failed before it could be written) is
//                abandoned with the structured profiler/report_aborted
//                warn (the replay/record_aborted precedent — CORE-008).
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
// The config surface (M1-CFG-01: the final versioned schema)
// ---------------------------------------------------------------------------
//
// The declarative game config (FR-1.5) is owned by laige/sim/config.h:
// EngineConfig (the typed configuration the engine consumes — moved
// there by M1-CFG-01 from this header; the five M1-HEAD-01 members
// keep their order), the version 1 JSON schema (the REQUIRED "version"
// key, the tick rate, the scene/churn budgets, the seed, the
// determinism block, the declared budgets/camera/asset_roots blocks),
// parseEngineConfig (the versioned document loader), loadGameConfig
// (the file loader), EngineConfigOverride + applyConfigOverride (the
// programmatic override merge), and ConfigHotReloader (the
// debug-build-only hot reload of non-simulation keys). This header
// includes config.h; the full key table, the versioning rules, the
// rejection table, and the hot-reload contract are in config.h and
// docs/api/config.md.
//
// Engine::create re-validates the typed config's tick rate (the single
// warn there is config/tick_rate_invalid, the config surface's
// rejection — the JSON/file loaders emit the same event for the same
// domain).
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
// Replay recording (M1-DET-02; laige/sim/replay.h)
// ---------------------------------------------------------------------------
//
// Opt-in, DEBUG-BUILDS-ONLY recording of the run's replay log (FR-1.4,
// PRD Appendix A: replay = the input log + the seed). The engine owns
// at most one ReplayRecorder per run:
//
//   startReplayRecording(path, maxBytes)  called after all
//     component/system registration (the component schema hash is
//     part of the replay identity — replay.h) and before
//     run_headless. Captures the identity (ADR 0002) from
//     (world, config) and opens the recorder's temp file.
//
//   run_headless  one recorded input frame per COMPLETED tick,
//     written from the GameLoop's onTick hook (game_loop.h): M1
//     frames are zero-length (no input system exists yet — M3-INPUT-03
//     defines the payload shape and source). A recording failure
//     (size limit reached, write error) STOPS THE RUN: run_headless
//     returns the failure Status, and the ordered shutdown still
//     happens (CONC-006).
//
//   a successful run finalizes the recorder (flush + trailer +
//     atomic temp-to-final rename) before the shutdown; the log
//     appears at `path` only then — a failed run or a pre-run
//     teardown leaves NO partial log at the final path (the temp
//     file is removed; the engine emits replay/record_aborted).
//
// The engine emits the structured replay/* events (LOG-001/002):
// record_started / record_finished (Info), record_failed (Error),
// record_aborted and record_already_started (Warn), record_disabled
// (Warn — release builds only). The recorder itself logs nothing.
//
// ---------------------------------------------------------------------------
// The profiler (M1-PROF-01; laige/sim/profiler.h)
// ---------------------------------------------------------------------------
//
// The engine owns exactly one always-on Profiler (FR-11.1), created
// in Engine::create and released in the ordered shutdown ("pools"
// step — the profiler holds no world reference: it pulls the world's
// entity / alloc / per-system data cold, on demand). It is ON by
// default (Options::enabled); the measured cost of the enabled
// instrumentation is bounded at 1% of a 10k-entity tick (the
// m1-profiler-cost baseline, CORE-001/DBG-004).
//
// The engine wires the profiler's two time feeds:
//
//   - tick time: the GameLoop is created with Options::profiler =
//     the engine's profiler (game_loop.h: per-completed-tick TimeIt,
//     handed to Profiler::recordTick — a failed tick is not recorded)
//   - frame time: runFrames times each frame's sim work plus the
//     presentation refresh (excluding the pacing sleep) and hands it
//     to Profiler::recordFrame; the first frame (start reference,
//     zero ticks) is not a runFrames frame and is not recorded
//
// The per-run profile report (the CLI's --prof-out, FR-11.1 file
// export) is opt-in and available in EVERY build (unlike replay
// recording — the report is diagnostics, not replay state):
//
//   startProfileReport(path)  called after all registration and
//     before run_headless (like startReplayRecording — one report per
//     run). It stores the path; the report is written at the END of
//     the run (JSON, version 1 schema — profiler.h / docs/api/
//     profiler.md), from the live state (the profiler's counters,
//     the world's entity/alloc fields, every system's M1-SYS-03
//     window) before the shutdown.
//
//   run_headless  finalizes the report (when started) on EVERY path —
//     success, failed frame, and failed start alike: the per-run
//     summary describes what actually happened (a zero-tick run
//     writes a zero-tick report). A write failure does NOT fail the
//     run (diagnostics never gate the simulation — CORE-002's
//     priority order): it is recorded in the sticky
//     profileReportStatus(), logged (profiler/report_write_failed,
//     Error), and left for the caller — laige-run maps it to its
//     exit-2 IO class.
//
// The last run's snapshot is cached in the engine (profileStats())
// because the world — and with it the world-pulled fields' source —
// is released in the shutdown: the CLI reads the cache, not the live
// state.
//
// The engine emits the structured profiler/* events (LOG-001/002):
// report_written (Info), report_write_failed (Error), report_aborted
// and report_already_started and report_path_invalid (Warn).
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
// Replay recording (M1-DET-02, opt-in debug feature): the DISABLED
// path pays one null check per completed tick and nothing else
// (DBG-004); the ENABLED path adds one bounded stdio write per
// completed tick (the 12-byte frame record into stdio's 8 KiB buffer
// — a flush to the OS only every ~680 zero-length frames) and the
// cold finish (flush + trailer + rename). Recording is never on the
// default run path.
// Profiler (M1-PROF-01, always-on by default): the ENABLED frame
// path adds two steady_clock reads (the TimeIt around the frame's
// sim work + presentation refresh) and one O(1) ring write
// (Profiler::recordFrame); the tick path adds two clock reads + one
// ring write per completed tick (the GameLoop's runOneTick —
// game_loop.h). No allocation. DISABLED (Profiler::setEnabled(false)):
// one branch each — the m1-profiler-cost baseline bounds the enabled
// cost at 1% of a 10k-entity tick (CORE-001, DBG-004).
// The run's setup path allocates exactly three times, all one-shot
// (verified per-frame-zero by the M1-HEAD-01 zero-allocation test):
// the GameLoop object, the PresentationSnapshot object, and the
// presentation slot record table (24 B/entity slot, sized by the
// scene budget — the presentation.h storage contract). The profiler
// is created in Engine::create (the engine's setup, not the run's) —
// one object + its two fixed window storages. The drop path is cold
// (one rate-limited warn per overload frame — the M1-LOOP-01
// contract); the report finalization (startProfileReport) is cold
// too (one format pass + one file write, once per run).
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
//   - Replay recording is opt-in and debug-builds-only: call
//     startReplayRecording ONCE, after all component/system
//     registration and before run_headless (the component schema hash
//     in the identity is captured at recording start). A second call
//     fails (replay/record_already_started); release builds reject
//     the call entirely (replay/record_disabled).
//   - The profile report is opt-in (startProfileReport) and ONE per
//     run: call it after all registration and before run_headless
//     (like replay recording). A second call fails
//     (profiler/report_already_started). A report write failure does
//     NOT fail the run — check profileReportStatus() (laige-run maps
//     it to exit 2).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "laige/errors.h"
#include "laige/fpx16_16.h"
#include "laige/result.h"
#include "laige/sim/config.h"         // EngineConfig, the version 1 schema (M1-CFG-01)
#include "laige/sim/determinism.h"  // SimMathBackend, DeterminismConfig (M1-DET-01)
#include "laige/sim/entity.h"       // World, kDefaultChurnPerFrameBudget
#include "laige/sim/game_loop.h"    // GameLoop, GameLoopStats, tick-rate constants
#include "laige/sim/presentation.h" // Position2D, PresentationSnapshot
#include "laige/sim/profiler.h"     // Profiler, ProfilerStats (M1-PROF-01)
#include "laige/sim/replay.h"       // ReplayRecorder (M1-DET-02)

namespace laige {

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

  // Start the opt-in replay recording of the upcoming run (M1-DET-02;
  // see the header preamble "Replay recording" and docs/api/replay.md
  // for the full contract). DEBUG BUILDS ONLY: a release build
  // rejects the call with InvalidArgument plus the structured
  // replay/record_disabled warn (CORE-008: never silent).
  //
  // Call after all component/system registration (the component
  // schema hash is part of the identity, captured at recording
  // start) and before run_headless.
  //
  //   path == the log's FINAL path (the recorder writes atomically:
  //           a temp file `path + ".tmp"` + rename — the final path
  //           appears only on a successful finish)
  //   maxBytes == the total log cap, header + frames + trailer
  //           (0 = kDefaultReplaySizeLimit; below
  //           kMinReplaySizeLimit the cap cannot hold a complete
  //           log)
  //
  //   stopped engine (already shut down) -> InvalidArgument (no log —
  //                                          the stopped-state
  //                                          precedent)
  //   already recording                 -> InvalidArgument + warn
  //                                          (replay/record_already_
  //                                          started)
  //   recorder start failure            -> the failure Status (the
  //                                          temp-open / header-write
  //                                          IoError) + warn (replay/
  //                                          record_start_failed)
  //
  // A recording failure mid-run stops the run: run_headless returns
  // the failure Status (BudgetExhausted at the size limit, IoError on
  // a write failure) and no partial log remains at `path`.
  // @budget cold path: one file open + one 40-byte header write; no per-tick cost while the engine is not recording.
  [[nodiscard]] Status startReplayRecording(std::string_view path,
                                             std::uint64_t maxBytes) noexcept;

  // True while a replay recording is active (started and not yet
  // finalized or abandoned). O(1), no side effects.
  [[nodiscard]] bool replayRecordingActive() const noexcept;

  // The bytes the active recording has written (header + frame
  // bytes; 0 when not recording). O(1), no side effects.
  [[nodiscard]] std::uint64_t replayBytesWritten() const noexcept;

  // The engine's always-on profiler (M1-PROF-01; the counters are
  // live while the engine runs). nullptr after shutdown or on a
  // moved-from engine (the world() nullability precedent). Use
  // profileStats() for the run's cached summary. O(1), no side
  // effects.
  [[nodiscard]] const Profiler* profiler() const noexcept;

  // The last run's profile snapshot (the profiler's counters plus
  // the world-pulled fields — entities, sim allocs, system count —
  // captured at the end of the run, BEFORE the shutdown releases the
  // world; all zeros before the first run). This is the feed the
  // laige-run CLI's one-line summary prints (FR-11.1 "exposed in the
  // CLI") and the M1-PROF-02 frame graph will consume per frame.
  // O(1), no allocation, no side effects.
  [[nodiscard]] ProfilerStats profileStats() const noexcept;

  // Start the opt-in per-run profile report (M1-PROF-01, FR-11.1
  // file export; see the header preamble "The profiler" for the
  // full contract). EVERY build (the report is diagnostics, not
  // replay state — unlike startReplayRecording's debug-only gate).
  //
  // Call after all component/system registration and before
  // run_headless. `path` is the report's FINAL path (the report is
  // written at the end of the run, JSON — profiler.h's version 1
  // schema; the file appears only when the write fully succeeds).
  //
  //   stopped engine (already shut down) -> InvalidArgument (no log —
  //                                          the stopped-state
  //                                          precedent)
  //   empty path                         -> InvalidArgument + warn
  //                                          (profiler/report_path_
  //                                          invalid)
  //   already started                    -> InvalidArgument + warn
  //                                          (profiler/report_
  //                                          already_started)
  //
  // A write failure at the end of the run does NOT fail the run —
  // it is sticky in profileReportStatus() (the laige-run CLI maps it
  // to exit 2).
  // @budget O(1); one string copy; no per-tick cost.
  [[nodiscard]] Status startProfileReport(std::string_view path) noexcept;

  // The sticky outcome of the last started report: ok when no report
  // was started or the write succeeded; the write error (IoError)
  // otherwise. O(1), no side effects.
  [[nodiscard]] Status profileReportStatus() const noexcept;

  // True while a report was started and its lifecycle has not ended
  // (between startProfileReport and the run's finalization, or a
  // shutdown's abandonment). O(1), no side effects.
  [[nodiscard]] bool profileReportActive() const noexcept;

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
  // The always-on profiler (M1-PROF-01): created in Engine::create
  // (one object + its two fixed window storages — setup path),
  // released in the shutdown's "pools" step. The GameLoop is created
  // with a non-owning view of it (the per-tick timing hook,
  // game_loop.h); runFrames drives the frame-time feed.
  std::unique_ptr<Profiler> profiler_;
  // The last run's profile snapshot (captured at the end of the run,
  // before the shutdown — the world-pulled fields' source is the
  // still-live world; the CLI reads this cache, the header preamble
  // "The profiler").
  ProfilerStats lastProfile_{};
  // Replay recording (M1-DET-02): the active recorder (nullptr when
  // not recording — the default path pays one null check per
  // completed tick and nothing else) and the sticky failure that
  // stopped the run (ok while nothing failed).
  std::unique_ptr<ReplayRecorder> replayRecorder_;
  Status replayFail_{};
  // Profile report (M1-PROF-01): the started report's final path
  // (empty = not started), its sticky write outcome (ok while
  // nothing failed), and whether the report's lifecycle has ended
  // (written at the run's end, or abandoned in a shutdown — a
  // pre-run teardown with a started report emits the profiler/
  // report_aborted warn and ends as abandoned).
  std::string profileReportPath_;
  Status profileReportStatus_{};
  bool profileReportFinalized_{false};
  // True after shutdown() has run (or on a moved-from engine).
  bool shutDown_{false};
};

}  // namespace laige
