// laige-sim system registry (M1-SYS-01).
//
// FR-1.3 (plain registered functions with declared time budgets and
// declared component I/O): this header ships the system framework's
// public types; the World methods that use them (registerSystem,
// systemCount, system) are declared in entity.h (the World home) and
// the registration template is defined there (the M1-ECS-02 pattern:
// public types in the subsystem header, World methods in its home):
//
//   SystemId        The stable per-world system id: a 32-bit dense
//                   value assigned in registration order (0 is
//                   reserved and never assigned).
//   SystemDef       The static declaration of a system: name, the
//                   plain run function, and the declared time budget
//                   in milliseconds (fpx16_16, exact — ADR 0002).
//   SystemContext   The per-tick context handed to a system: the world
//                   view plus the delegated World::each (the PRD
//                   Appendix B sketch's `ctx.each<...>()`).
//   Io<T, Access>   One declared component I/O entry of a system:
//                   component type T and its declared access.
//   SystemInfo      A registered system's snapshot (def + the declared
//                   I/O membership queries) for the scheduler
//                   (M1-SYS-02) and the profiler (M1-PROF-01).
//   SystemSchedule  The computed execution order of a world's systems
//                   (M1-SYS-02): the systemCount plus the SystemId
//                   values in execution order.
//   SystemTimingStats
//                   One system's measured-run scalars (runs, last ms,
//                   warn/error counts) for the profiler (M1-SYS-03,
//                   M1-PROF-01/02).
//   LAIGE_SYSTEM    The one-line declaration of a system: the plain
//                   function declaration plus the SystemDef, at
//                   namespace scope directly above the function;
//                   optional trailing depends_on names (M1-SYS-02).
//
// ---------------------------------------------------------------------------
// The system shape (FR-1.3: plain functions, no inheritance)
// ---------------------------------------------------------------------------
//
// A system is a plain free function with the signature SystemFn:
//
//   LAIGE_SYSTEM(Movement, 1)
//   void Movement(laige::World& world, laige::SystemContext& ctx) {
//     world.each<SimVel>(
//         [](laige::Entity e, SimVel& v) { /* ... */ }, laige::Write{});
//   }
//
// No class, no inheritance, no state object: the function plus its
// SystemDef (the `Movement_Def` variable the macro builds) IS the
// system. `world` is the world the system runs on (one world, one
// owner thread — PRD §10.2); `ctx` is that tick's SystemContext, a
// view of the same world that delegates iteration to World::each
// (query.h). Systems are deterministic when the engine runs in
// deterministic mode (M1-DET-01) and must stay within their declared
// budget (M1-SYS-03 measures it).
//
// The LAIGE_SYSTEM macro expands at namespace scope to
//
//   void Movement(laige::World&, laige::SystemContext&);
//   inline const laige::SystemDef Movement_Def = laige::SystemDef{
//       "Movement", &Movement, laige::fpx16_16::fromFloat(1), ""};
//
// so `Name` is both the C++ function name and the system's
// registration name (stringified), and the def variable is `Name##Def`.
// The macro and the function definition live in the same translation
// unit (the macro takes the function's address). `budget_ms` is a
// numeric literal in milliseconds (1, 0.5, ...); the conversion to
// the exact fpx16_16 happens once, at program start (a setup path,
// never a hot path).
//
// M1-SYS-02 adds the optional trailing `depends_on` names:
//
//   LAIGE_SYSTEM(Health, 1, Spawner)
//
// The names after `budget_ms` are the registration names of the
// systems `Health` must run after — stringified verbatim into the
// def's `dependsOn` spec ("Spawner"). They may name systems
// registered LATER in the same world (forward dependencies: the spec
// is validated against the world at scheduling time, not at
// registration). The empty list (no trailing names) is the no-
// dependencies spec: "" where the preprocessor stringizes the empty
// variadic pack (GCC/Clang, [cpp.stringize]) and nullptr where it
// leaves the pack empty and the 4th initializer value-initializes
// (MSVC 2022) — both are the documented no-deps form
// (SystemDef::dependsOn), and parseDepSpec treats them alike. See
// the "Scheduler" section below for the spec format and the ordering
// semantics.
//
// ---------------------------------------------------------------------------
// Registration and the id contract (FR-1.3, component.h precedent)
// ---------------------------------------------------------------------------
//
// A system is registered into a world at world setup (before the
// loop), like a component type:
//
//   auto r = world.registerSystem(Movement_Def,
//       laige::Io<SimVel, laige::Access::Write>{});
//
//   SystemId is assigned in registration order, densely from 1, per
//   world (0 is reserved); kMaxSystems (256) is the engine-level cap.
//   Determinism (ARCH-010): assignment and the name-uniqueness check
//   are pure integer/string bookkeeping — no addresses, hashes, or
//   platform state enter the id. Two worlds, two process runs, or
//   two builds that register the same systems in the same order
//   produce bit-identical id sequences, so ids are replay state from
//   M1 on (M1-DET-01/02). Ids are per-world, like ComponentTypeIds.
//
// The declared component I/O is part of the REGISTRATION, not the
// def: component ids are per-world runtime values (component.h id
// contract) and cannot be baked into a compile-time def. The Io<...>
// pack is resolved against the world at registration into the
// disjoint read/write id sets stored in the world's record, and read
// back through SystemInfo::declaresRead/declaresWrite. The documented
// I/O list order is ascending ComponentTypeId (a pure function of
// the sets).
//
// Validation (FR-12.1, CORE-008 — never silent; order is normative,
// the first failure wins):
//
//   moved-from world (no registry)    -> ErrorCode::InvalidArgument
//   def.name null or empty             -> InvalidArgument + warn
//                                          (system/name_invalid)
//   def.run nullptr                    -> InvalidArgument + warn
//                                          (system/run_invalid)
//   def.budgetMs <= 0                  -> InvalidArgument + warn
//                                          (system/budget_invalid) —
//                                          the budget must be explicit
//                                          and positive (FR-1.3)
//   duplicate name in this world       -> InvalidArgument + warn
//                                          (system/duplicate)
//   Io<T> T not a Laige component      -> compile error (static_assert)
//   Io<T> T not registered in this
//       world                          -> InvalidArgument + warn
//                                          (system/io_unregistered)
//   the same component declared twice  -> InvalidArgument + warn
//       by one system (any access      (system/io_duplicate) — a
//       combination: read+read,        system's I/O is a set, not a
//       read+write, write+read)        multiset
//   more than kMaxSystems              -> BudgetExhausted + warn
//                                          (system/budget_exhausted)
//
// On success the def is COPIED by value into the world's fixed record
// table (kMaxSystems records, a setup-path allocation like the
// component registry — the user's def may be a stack variable), and
// the I/O sets are written in place: no allocation at registration
// and none per tick (the registry is read-only during the loop).
//
// ---------------------------------------------------------------------------
// Scheduler (M1-SYS-02): execution order, depends_on, validation
// ---------------------------------------------------------------------------
//
// The scheduler turns the registry (registration order + declared
// depends_on + declared component I/O) into the per-tick execution
// order, and runs the systems in that order:
//
//   SystemSchedule sched;
//   Status s = world.scheduleSystems(sched);   // setup phase, once
//   ...                                        // before the loop
//   for (tick) {                               // M1-LOOP-01 owns this
//     world.beginFrame();
//     world.runSystems(sched);
//   }
//
// Execution order:
//
//   - The BASE order is the registration order (ascending SystemId).
//     Without depends_on, the computed order is exactly that.
//   - A depends_on edge (system X lists system Y in its spec) means
//     X runs AFTER Y. Forward edges are legal (Y may be registered
//     later: the spec is resolved against the world at scheduling
//     time, not at registration).
//   - The computed order is the STABLE topological sort: repeatedly
//     pick the smallest unrun SystemId whose dependencies are all
//     already placed (Kahn's algorithm with a min-id tie-break). The
//     order is a pure function of the registration order and the
//     declared edges — deterministic (ARCH-010) — and it never moves a
//     system earlier than registration order would place it; a
//     system only moves later, behind its dependencies.
//
// The depends_on spec (the def's `dependsOn` string; the macro builds
// it from the trailing names): a comma-separated list of registration
// names. Each token is trimmed of ASCII whitespace, must be non-empty,
// and must not repeat (the dependency list is a set, not a multiset —
// the declared-I/O precedent). At most kMaxSystemDependencies (16)
// direct dependencies: a longer list means the registration order
// should carry the ordering (a barrier is registration position, not a
// dependency list). nullptr or "" means no dependencies.
//
// Pre-run validation — World::scheduleSystems (normative order, first
// failure wins; every failure is one rate-limited structured warn,
// subsystem "system", LOG-004, plus a Status — FR-12.3: never silent):
//
//   1. A dependency name that is not registered in this world
//      (first in ascending (system id, spec position) order)
//                                          -> InvalidArgument
//                                             (system/dep_missing)
//   2. A dependency cycle: Kahn's leaves systems with unsatisfied
//      dependencies (one concrete cycle is reported — the
//      deterministic walk from the smallest remaining id, following
//      each system's first spec-listed dependency that is still
//      remaining)
//                                          -> InvalidArgument
//                                             (system/dependency_cycle)
//   3. Two systems both declaring Write of the same component type in
//      one tick (first conflict in ascending component-id, then
//      ascending writer-id order; order-independent: the last write
//      would silently win)
//                                          -> InvalidArgument
//                                             (system/double_writer)
//   4. (WARN ONLY — scheduling succeeds) a declared read that the
//      computed order places BEFORE a declared write of the same
//      component: the reader observes the previous tick's value, not
//      this tick's write (each such (reader, writer, component) triple
//      warns once, in ascending component-id, reader-id, writer-id
//      order)
//                                          -> Warn
//                                             (system/read_before_write)
//
// On success `out` is fully populated and nothing is logged
// (LOG-003: the success path has no diagnostics).
//
// Running — World::runSystems(schedule):
//
//   - The schedule must describe the CURRENT registry: a systemCount
//     mismatch (systems registered after the schedule was computed)
//     is rejected (system/schedule_stale); the order entries must be
//     unique ids in 1..systemCount (a hand-built malformed schedule is
//     rejected, system/schedule_invalid). Both are InvalidArgument +
//     one rate-limited warn.
//   - The systems run STRICTLY ONE AT A TIME, in schedule order, on
//     the world's single owner thread (PRD §10.2: simulation is
//     single-threaded; API-004: the system phase is the mutation
//     phase). The per-tick SystemContext is built per system (a
//     non-owning view — never stored). The M1-ECS-04 iteration
//     legality guard (query.h) applies inside every system exactly as
//     for a direct World::each: nested each() and illegal mutations
//     are rejected per system, and the declared-I/O validation above
//     is the cross-system complement (one writer per component,
//     read-before-write surfaced).
//   - A system's run function is void: per-entity Status results from
//     its own each() calls are the system's to handle (check them,
//     CORE-008). runSystems itself reports only schedule-level
//     failures. Calling runSystems from inside a system (nesting
//     system phases) is misuse — the declared order contract no
//     longer holds (the one-writer-per-component invariant still
//     prevents state corruption; see the misuse warnings).
//
// Determinism (ARCH-010): scheduling is pure integer/string
// bookkeeping — id scans, string comparisons over the registration
// names, Kahn's with a min-id rule. No floating point, no randomness,
// no addresses enter the order or the warning set. Two worlds (two
// process runs, two builds) with the same registration order, specs,
// and declared I/O produce bit-identical schedules and identical
// warning sequences.
//
// ---------------------------------------------------------------------------
// Timing and budget enforcement (M1-SYS-03; PRD §9.3 G-R5)
// ---------------------------------------------------------------------------
//
// World::runSystems measures each system's own run time per tick
// (TimeIt — the M0-CORE-08 steady_clock scope timer, ms as a double)
// and hands the measurement to the system's rolling window and the
// budget check:
//
//   - Rolling window — one fixed-capacity Histogram per system
//     (kSystemTimingWindowSamples = 64 samples, ~1.1 s at the default
//     60 Hz). record() is O(1) and allocates nothing (PERF-003);
//     recording beyond the capacity drops the OLDEST sample, and
//     totalRecorded() keeps counting every sample ever recorded
//     (silent truncation is not allowed — the M0-CORE-08 contract).
//     The window rolls across TICKS (it is not a per-frame window):
//     beginFrame() does not touch it.
//   - Budget enforcement — measured vs the declared SystemDef
//     budget (fpx16_16 ms, converted to double exactly — raw/2^16 is
//     a power-of-two scale):
//       measured >  1 × budget  -> one system/budget_overrun WARN
//                                   (the rolling window p99 is carried
//                                   as a field)
//       measured >= 3 × budget  -> one system/budget_critical ERROR
//                                   (kBudgetCriticalMultiplier; PRD
//                                   §9.3 G-R5: "over 3× → error event")
//     Both events follow the NFR-13.3 5-field message grammar
//     (build-stable text; the dynamic values are structured fields)
//     and are rate-limited per (subsystem, event, severity) (LOG-004);
//     the warn and the error count separately in SystemTimingStats.
//     An over-budget system is STILL RUN — the timing is observation
//     and reporting, never an execution gate (the engine does not skip
//     or cancel a system; FR-12.3: the breach is surfaced, not hidden).
//   - Profiler feed — World::systemTimingStats(id) (the cheap scalars,
//     O(1), per frame) and World::systemTimingWindow(id) (the rolling
//     window itself, cold path: the M1-PROF-02 frame graph's
//     budgetCheck consumes it).
//
// Determinism (ARCH-009/ARCH-010): the measured times are DIAGNOSTIC
// only — they never enter authoritative simulation state, state
// hashes, or replays (wall-clock readings are platform-sensitive).
// The only state the timing adds to a world is the window contents
// and the counters: diagnostics, not sim state.
//
// ---------------------------------------------------------------------------
// Threading and failure
// ---------------------------------------------------------------------------
//
// Registration, scheduling, and running are all setup-phase or
// sim-thread operations on the world's single owner thread (CONC-001;
// API-004: mutations in explicit phases). scheduleSystems is a pure
// read of the registry (const); runSystems is the per-tick mutation
// phase (it also owns the per-system timing state — written strictly
// on the owner thread); the run functions are sim-thread code (PRD
// §10.2: simulation is single-threaded). All failures are Result/
// Status values with one rate-limited structured warn each (LOG-004);
// no exceptions (FR-12.1, NFR-8.10).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - A budget is a declaration, not a knob: 0, negative, or a
//     missing budget is a registration error (FR-1.3: "declared time
//     budgets" — an undeclared budget is an undeclared system).
//   - Two systems must never write the same component type in one
//     tick: the M1-SYS-02 scheduler rejects such a registration pair
//     at scheduling time (FR-12.3). Declare the I/O the system
//     really uses — an undeclared write is invisible to the
//     scheduler's conflict check.
//   - A system's I/O may declare a component it does not touch (a
//     conservative declaration), never one it touches without
//     declaring.
//   - depends_on names registration names, not function addresses or
//     SystemIds (ids are per-world runtime values, names are the
//     stable identity). A dependency on a name that is never
//     registered in this world fails at scheduling time — a
//     dependency on a system registered in ANOTHER world is always
//     such a failure (systems never cross worlds).
//   - A schedule is computed for the registry it was computed with:
//     registering systems after scheduleSystems() and then
//     runSystems() with the old schedule is rejected
//     (system/schedule_stale). Recompute the schedule after any
//     registration change.
//   - A system that reads a component written by a LATER system in
//     the computed order reads the previous tick's value: the
//     scheduler warns (system/read_before_write). If the read must
//     see this tick's write, declare `depends_on` (or register the
//     writer earlier); a deliberate cross-tick read is declared by
//     not declaring the write at all (undeclared I/O is invisible to
//     the check — use it consciously).
//   - The LAIGE_SYSTEM macro is namespace-scope only: the def
//     variable it builds is `inline const` (block scope is
//     ill-formed) and the function it declares must match the
//     definition's linkage.
//   - SystemContext is a non-owning per-tick view: never store it
//     (the world owns the storage; the context outlives nothing).
//   - A moved-from world has no system registry: registerSystem on
//     one returns InvalidArgument (the same "valid empty world"
//     contract as the component registry).

#pragma once

#include <cstdint>
#include <memory>

#include "laige/budget_harness.h"  // M1-SYS-03: the rolling window (Histogram)
#include "laige/fpx16_16.h"
#include "laige/result.h"
#include "laige/sim/component.h"
#include "laige/sim/query.h"

namespace laige {

class World;  // the World home is entity.h; only the reference is used here
struct SystemContext;  // declared below; only the reference is used in SystemFn

// The stable per-world system id (FR-1.3): assigned in registration
// order, densely from 1. See the header preamble for the id and
// determinism contract.
struct SystemId {
  std::uint32_t value{};  // 0 is reserved: never a registered id
};

// The never-assigned id (API-008: the invalid state is representable
// and checkable; call sites never spell raw 0s).
inline constexpr SystemId kInvalidSystemId{0};

inline bool operator==(SystemId a, SystemId b) noexcept {
  return a.value == b.value;
}
inline bool operator!=(SystemId a, SystemId b) noexcept {
  return !(a == b);
}

// The engine-level cap on systems per world (CORE-005: a named engine
// constant, the kMaxComponentTypes precedent — a game's system count
// is orders of magnitude smaller than its entity count; raising it is
// an ADR, not a knob).
inline constexpr std::uint32_t kMaxSystems = 256;

// The bound on one system's direct depends_on list (CORE-005). A
// direct dependency list is a small hand-written declaration; beyond
// 16 the ordering should be carried by registration position (a
// barrier is registration order, not a dependency list). Raising it
// is an ADR.
inline constexpr std::uint32_t kMaxSystemDependencies = 16;

// The per-system rolling window capacity (M1-SYS-03; CORE-005): the
// number of measured run times (ms) kept per system in the rolling
// histogram — ~1.1 s of samples at the default 60 Hz tick rate. The
// window is fixed at world construction (a setup-path allocation);
// raising the capacity is an ADR, not a knob.
inline constexpr std::uint32_t kSystemTimingWindowSamples = 64;

// The over-budget multiplier that escalates the budget_overrun warn
// into a budget_critical error event (M1-SYS-03; PRD §9.3 G-R5:
// "over 3× → error event"; CORE-005). The warn fires strictly above
// 1× the declared budget; the error at 3× or more.
inline constexpr std::uint32_t kBudgetCriticalMultiplier = 3;

// The system function signature (FR-1.3): a plain free function — no
// class, no inheritance. `world` is the world the system runs on;
// `ctx` is that tick's SystemContext (one world, one owner thread,
// PRD §10.2).
using SystemFn = void (*)(World&, SystemContext&);

// The static declaration of a system (FR-1.3): one per system, built
// by the LAIGE_SYSTEM macro (see the header preamble for the shape).
// `name` is the stable registration name (unique per world); `run` is
// the plain system function; `budgetMs` is the declared per-tick time
// budget in MILLISECONDS (fpx16_16 — exact, no floating point; ADR
// 0002). `dependsOn` is the raw depends_on spec (M1-SYS-02): a
// comma-separated list of registration names — nullptr or "" means
// no dependencies (see the preamble "Scheduler" for the format and
// the validation). The declared component I/O is NOT part of the def
// (per-world runtime ids, see the preamble): it is declared at
// registration (the Io<...> pack of World::registerSystem) and stored
// in the world's record. The def is a small trivially-copyable value
// — registerSystem copies it, so a def on the stack is safe.
struct SystemDef {
  const char* name;
  SystemFn run;
  fpx16_16 budgetMs;
  const char* dependsOn;  // nullptr or "" = no dependencies
};

// The computed execution order of one world's systems (M1-SYS-02).
// A plain value: built by World::scheduleSystems (setup phase),
// consumed by World::runSystems once per tick, owned by the caller
// (the game's engine object — M1-HEAD-01). `systemCount` is the
// world's system count AT SCHEDULING TIME (runSystems' staleness
// check); `order[i]` is the SystemId of the system that runs i-th
// (order[0] first, order[systemCount - 1] last; no repeats, dense
// 1..systemCount).
struct SystemSchedule {
  std::uint32_t systemCount{};
  std::uint32_t order[kMaxSystems]{};
};

// The per-tick context handed to a system's run() (FR-1.3; the PRD
// Appendix B sketch's `ctx`). It names the world the system runs on
// and delegates iteration to World::each (query.h) — the sketch's
// `ctx.each<...>()`. The context is built per system per tick by the
// scheduler (M1-SYS-02); until then games and tests build it
// directly. It is a non-owning view (the world owns the storage):
// never store it across ticks.
struct SystemContext {
  // The world the system runs on (one world, one owner thread).
  World& world;

  // Delegate to World::each<T1..TN>(fn, Read/Write tags...) on the
  // same world: identical semantics, visit order, iteration-legality
  // behavior, and Status results (query.h). No allocation. The
  // definition is out-of-line in entity.h (the World home): World is
  // incomplete here, and the delegated call is checked at
  // instantiation — which needs the complete World.
  // @budget O(kMaxArchetypes * N) scan + one visit per matching entity; no allocation.
  template <typename... Ts, typename... Acc, typename F>
  [[nodiscard]] Status each(F&& fn, Acc... acc) noexcept;
};

// One declared component I/O entry of a system (FR-1.3): component
// type T and its declared access. Pass one value of this tag type per
// component in the Io<...> pack of World::registerSystem:
//
//   world.registerSystem(Movement_Def,
//       laige::Io<SimVel, laige::Access::Write>{},
//       laige::Io<SimPos, laige::Access::Read>{});
//
// T must be a Laige component (LAIGE_COMPONENT) registered on the
// world being registered to; a component may appear at most ONCE in
// one system's I/O (any access combination — the declaration is a
// set, not a multiset: M1-SYS-02 reads the disjoint read/write sets
// for its conflict checks).
template <typename T, Access kAccess>
struct Io {
  // The declared access of the entry (Read or Write).
  static constexpr Access access = kAccess;
};

// A registered system's snapshot (a plain value; the M1-SYS-02
// scheduler and the M1-PROF-01 profiler pull it). `def` is the value
// copy of the registered def; `id` is the world's SystemId. The
// declared component I/O list (FR-1.3) is stored as the disjoint
// read/write id sets and read back through the membership queries:
// the documented list order is ascending ComponentTypeId (component.h
// id contract — a pure function of the sets).
struct SystemInfo {
  SystemDef def{};
  SystemId id{};

  // True when the system declares `componentId` for reading.
  // @budget O(1); no allocation.
  [[nodiscard]] bool declaresRead(ComponentTypeId componentId) const noexcept;

  // True when the system declares `componentId` for writing.
  // @budget O(1); no allocation.
  [[nodiscard]] bool declaresWrite(ComponentTypeId componentId) const noexcept;

 private:
  // World::system (systems.cpp) builds the snapshot from the world's
  // record: the only outside access to the set members.
  friend class World;
  // The declared I/O sets (the storage form of the I/O list;
  // membership only — never iterated, PERF-006). Set in place at
  // registration (systems.cpp builds the snapshot from the world's
  // record).
  detail::IdSet256 readComponents_{};
  detail::IdSet256 writeComponents_{};
};

// One system's measured-run scalars (M1-SYS-03; PRD §9.3 G-R5): the
// cheap per-frame snapshot the profiler (M1-PROF-01) and the frame
// graph (M1-PROF-02) pull through World::systemTimingStats(id) —
// O(1), no allocation, no side effects. The window SAMPLES are not
// here (the rolling histogram is read cold through
// World::systemTimingWindow(id) — its stats() is O(n log n)). All
// counters are since-construction; the window rolls across ticks
// (kSystemTimingWindowSamples, not per-frame).
struct SystemTimingStats {
  // Measured runs of the system since world construction.
  std::uint64_t runs{};
  // The measured time (ms) of the most recent run (0 before the first
  // run). A run shorter than the platform's steady_clock tick
  // measures as exactly 0.0 ms — a legitimate sub-resolution reading
  // (wall-clock resolution is platform-sensitive; ARCH-009), not a
  // failure state.
  double lastMs{};
  // The system/budget_overrun warns issued since construction.
  std::uint32_t warns{};
  // The system/budget_critical error events issued since
  // construction.
  std::uint32_t errors{};
};

// Declare a system (FR-1.3): at namespace scope, directly above the
// plain system function's definition. `Name` is both the C++ function
// name and the system's registration name (stringified); `budget_ms`
// is the declared per-tick time budget in milliseconds (a numeric
// literal, e.g. 1 or 0.5 — converted to the exact fpx16_16 once, at
// program start); the optional trailing `Dep...` names are the
// depends_on spec (M1-SYS-02): the registration names of the systems
// `Name` must run after, stringified verbatim into the def's
// `dependsOn` field (comma-separated, as written). Expands to the
// function declaration plus
//
//   inline const laige::SystemDef Name##Def = laige::SystemDef{
//       #Name, &Name, laige::fpx16_16::fromFloat(budget_ms),
//       #__VA_ARGS__};
//
// The def variable (e.g. `Movement_Def`) is what World::registerSystem
// takes, together with the Io<...> pack. The macro and the function
// definition live in the same translation unit.
#define LAIGE_SYSTEM(Name, budget_ms, ...)                             \
  void Name(laige::World&, laige::SystemContext&);                    \
  inline const laige::SystemDef Name##_Def = laige::SystemDef{        \
      #Name, &Name, laige::fpx16_16::fromFloat(budget_ms), #__VA_ARGS__};

namespace detail {

// One registry record per registered system. World stores a dense
// array of these indexed by (system id - 1) — ids are dense by
// construction (the component.h ComponentRecord precedent). `def` is
// the value copy from registration; the I/O is the disjoint read/write
// id sets (the storage form of the declared I/O list — system.h
// preamble).
struct SystemRecord {
  SystemDef def{};
  IdSet256 readComponents;    // declared Read component ids (1..256)
  IdSet256 writeComponents;   // declared Write component ids (1..256)
};

// One per-system timing record (M1-SYS-03): the rolling window of
// measured run times (ms — the M0-CORE-08 Histogram, fixed capacity
// kSystemTimingWindowSamples) plus the cheap scalars exposed by
// SystemTimingStats. World stores a dense array of these indexed by
// (system id - 1) — parallel to the SystemRecord table (the M1-SYS-01
// precedent: fixed engine budget, setup-path allocation, moves with
// the world, survives clear()).
struct SystemTimingRecord {
  // The rolling window of measured run times (ms): the M0-CORE-08
  // Histogram, fixed capacity kSystemTimingWindowSamples. Built in
  // World::create (setup path; the Histogram has no default
  // constructor, so the record holds it as a unique_ptr — the one
  // level of indirection is bounded by kMaxSystems).
  std::unique_ptr<Histogram> window;
  double lastMs{};  // most recent measured run (0 before first; a run
                   // shorter than the steady_clock tick reads 0.0)
  std::uint64_t runs{};  // measured runs since construction
  std::uint32_t warns{};  // budget_overrun warns issued
  std::uint32_t errors{}; // budget_critical errors issued
};

// The outcome of one Io entry's resolution in World::registerSystem
// (the fold short-circuits on the first failure).
enum class IoResolution : std::uint8_t {
  Ok = 0,
  Unregistered = 1,  // T is not registered in this world
  Duplicate = 2,     // T is already declared by this system (any access)
};

// True when `T` is an Io<T, Access> tag (the registerSystem pack
// check, compile time; class form — the public-header convention for
// traits, the ComponentTraits precedent in component.h).
template <typename T>
struct IsIoTag {
  static constexpr bool value = false;
};
template <typename T, Access A>
struct IsIoTag<Io<T, A>> {
  static constexpr bool value = true;
};

// True when the component type of an Io tag is a Laige component
// (the second registerSystem pack check, compile time).
template <typename T>
struct IsIoComponent {
  static constexpr bool value = false;
};
template <typename T, Access A>
struct IsIoComponent<Io<T, A>> {
  static constexpr bool value = ComponentTraits<T>::isComponent;
};

// Extracts the component type and the declared access of an Io tag:
// IoComponent<Io<T, A>>::type == T and ::access == A.
template <typename Tag>
struct IoComponent;
template <typename T, Access A>
struct IoComponent<Io<T, A>> {
  using type = T;
  static constexpr Access access = A;
};

// The parsed form of a depends_on spec (M1-SYS-02). `names[i]`
// points into the spec string itself (tokens are substrings — the
// spec is a compile-time string literal owned by the program, so the
// pointers are stable for the process lifetime), with `lengths[i]`
// the token length (tokens are NOT NUL-terminated). Setup path only
// — registration validation (registerSystem) and resolution
// (scheduleSystems); never on a tick. No copy, no allocation.
struct DepSpecParse {
  std::uint32_t count{};
  const char* names[kMaxSystemDependencies]{};
  std::uint32_t lengths[kMaxSystemDependencies]{};
};

// The outcome of parseDepSpec (the first failure wins; on any error
// the out parameter is left empty — count 0, no partial parse).
enum class DepSpecError : std::uint8_t {
  Ok = 0,
  EmptyToken = 1,    // a token is empty or all whitespace
  DuplicateToken = 2,  // the same name listed twice (the list is a set)
  TooMany = 3,       // more than kMaxSystemDependencies dependencies
};

// Parse one depends_on spec (the raw SystemDef::dependsOn string):
// split on ',', trim ASCII whitespace (space, tab, CR, LF) around
// each token, and record the token pointers in `out`. nullptr or a
// whitespace-only spec is Ok with count 0 (no dependencies).
DepSpecError parseDepSpec(const char* spec, DepSpecParse* out) noexcept;

// The stable token for a DepSpecError (the `error` field of the
// system/dep_spec_invalid warn; LOG-001 machine-searchable).
const char* depSpecErrorName(DepSpecError error) noexcept;

}  // namespace detail

}  // namespace laige
