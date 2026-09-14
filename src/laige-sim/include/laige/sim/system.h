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
//   LAIGE_SYSTEM    The one-line declaration of a system: the plain
//                   function declaration plus the SystemDef, at
//                   namespace scope directly above the function.
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
//       "Movement", &Movement, laige::fpx16_16::fromFloat(1)};
//
// so `Name` is both the C++ function name and the system's
// registration name (stringified), and the def variable is `Name##Def`.
// The macro and the function definition live in the same translation
// unit (the macro takes the function's address). `budget_ms` is a
// numeric literal in milliseconds (1, 0.5, ...); the conversion to
// the exact fpx16_16 happens once, at program start (a setup path,
// never a hot path).
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
// Threading and failure
// ---------------------------------------------------------------------------
//
// Registration is a setup-phase operation on the world's single
// owner thread (CONC-001; API-004: mutation in an explicit phase).
// The run function is sim-thread code (PRD §10.2: simulation is
// single-threaded). All failures are Result/Status values with one
// rate-limited structured warn each (LOG-004); no exceptions
// (FR-12.1, NFR-8.10).
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
// 0002). The declared component I/O is NOT part of the def (per-world
// runtime ids, see the preamble): it is declared at registration
// (the Io<...> pack of World::registerSystem) and stored in the
// world's record. The def is a small trivially-copyable value —
// registerSystem copies it, so a def on the stack is safe.
struct SystemDef {
  const char* name;
  SystemFn run;
  fpx16_16 budgetMs;
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

// Declare a system (FR-1.3): at namespace scope, directly above the
// plain system function's definition. `Name` is both the C++ function
// name and the system's registration name (stringified); `budget_ms`
// is the declared per-tick time budget in milliseconds (a numeric
// literal, e.g. 1 or 0.5 — converted to the exact fpx16_16 once, at
// program start). Expands to the function declaration plus
//
//   inline const laige::SystemDef Name##Def = laige::SystemDef{
//       #Name, &Name, laige::fpx16_16::fromFloat(budget_ms)};
//
// The def variable (e.g. `Movement_Def`) is what World::registerSystem
// takes, together with the Io<...> pack. The macro and the function
// definition live in the same translation unit.
#define LAIGE_SYSTEM(Name, budget_ms)                                   \
  void Name(laige::World&, laige::SystemContext&);                     \
  inline const laige::SystemDef Name##_Def = laige::SystemDef{        \
      #Name, &Name, laige::fpx16_16::fromFloat(budget_ms)};

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

}  // namespace detail

}  // namespace laige
