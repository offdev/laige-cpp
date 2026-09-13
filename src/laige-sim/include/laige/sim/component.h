// laige-sim component type registry (M1-ECS-02).
//
// PRD §9.1 S-8 (component traits are the customization surface) and
// FR-1.2 (components identified by compile-time trait types): this
// header ships the M1 data-only half of the component contract:
//
//   ComponentTypeId   The stable per-world component type id: a 32-bit
//                     dense value assigned in registration order (0 is
//                     reserved and never assigned).
//   LAIGE_COMPONENT   The compile-time registration mark: it
//                     specializes the component trait so
//                     World::registerComponent<T>() accepts T.
//                     Replication/inspector traits (the PRD Appendix B
//                     shape) extend this macro in M4; M1 components
//                     are data carriers only (S-8).
//   World::registerComponent<T>()
//                     The per-world runtime registration (declared in
//                     entity.h, the World home): records sizeof(T)/
//                     alignof(T) for the M1-ECS-03 SoA layout and
//                     assigns the next id. Built-in and user-defined
//                     components register through the same path — a
//                     user struct is legal (S-8 data-carrier case).
//
// ---------------------------------------------------------------------------
// The id contract (FR-1.2, ARCH-010)
// ---------------------------------------------------------------------------
//
// Ids are assigned in the order successful registerComponent() calls
// happen on a world, densely from 1:
//
//   first registered type  -> 1
//   second                 -> 2
//   ...
//
// kMaxComponentTypes (256) is the engine-level cap on the number of
// component types per world (CORE-005: a named engine constant — a
// game's component vocabulary is orders of magnitude smaller than
// its entity count; a project that outgrows it raises the constant
// through an ADR, not a per-world knob).
//
// Determinism (ARCH-010): assignment is pure integer bookkeeping — no
// addresses, hashes, or platform state enter the id. Two worlds, two
// process runs, or two builds that register the same types in the
// same order produce bit-identical id sequences, so ids are replay
// state from M1 on and are safe to replicate (M1-DET-01/02). The
// ordering contract is documented, not implied: a different
// registration order produces different ids — games register their
// component set once at world setup, in one documented place
// (M1-HEAD-01 wires the loop around it).
//
// Ids are per-world: a ComponentTypeId is meaningless in another
// world (the same cross-world caveat as Entity handles, entity.h).
//
// ---------------------------------------------------------------------------
// Type identity without RTTI (NFR-8.10, PERF-006)
// ---------------------------------------------------------------------------
//
// Duplicate detection and the type -> record lookup key on each
// component type's compile-time identity token: the address of
// ComponentTypeKey<T>::kMarker (one inline static per template
// specialization; distinct types have distinct addresses,
// [basic.stc]). The registry table therefore needs no
// std::type_info (RTTI is disabled, NFR-8.10) and no hash map
// (unordered containers are banned from sim paths, PERF-006;
// M1-ECS-05 iteration contract). The duplicate scan is linear over
// the registered records — a setup-path cost, never a hot path
// (PERF-007).
//
// ---------------------------------------------------------------------------
// Allocation, threading, failure
// ---------------------------------------------------------------------------
//
// registerComponent() is a setup-phase operation (world construction,
// before the loop): one linear scan plus one record write, no
// allocation, O(n) in the number of registered types (PERF-007). It
// runs on the world's single owner thread (CONC-001) and is not a
// per-tick operation (API-004: mutation in an explicit phase).
//
// Failures are Result values (FR-12.1, CORE-008 — never silent):
//
//   T already registered in this world -> ErrorCode::InvalidArgument
//                                          (+ one rate-limited
//                                          structured warn, event
//                                          ecs/component_duplicate)
//   more than kMaxComponentTypes       -> ErrorCode::BudgetExhausted
//   invalid/unregistered id in
//   componentInfo()                    -> ErrorCode::InvalidArgument
//
// Component types must be trivially copyable data carriers (S-8):
// registerComponent<T>() static-asserts this, so a non-trivial member
// (a string, a destructor, a vtable) is a compile error with an
// actionable message before it can break the M1-ECS-03 SoA layout.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//
//   - Marking a type twice (two full specializations of the trait) is
//     a compile error — the C++ standard rejects duplicate
//     specializations; mark each type once.
//   - Registering the same type twice on one world is an error, not a
//     no-op: read the Result (FR-12.3).
//   - Do not compare ComponentTypeId values across worlds (per-world
//     ids, above).
//   - A moved-from world has no registry: registerComponent() on one
//     returns InvalidArgument (the same "valid empty world" contract
//     as entity creation).

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace laige {

// The stable per-world component type id (FR-1.2). See the header
// preamble for the assignment and determinism contracts.
struct ComponentTypeId {
  std::uint32_t value{};  // 0 is reserved: never a registered id
};

// The never-assigned id (API-008: the invalid state is representable
// and checkable; call sites never spell raw 0s).
inline constexpr ComponentTypeId kInvalidComponentTypeId{0};

inline bool operator==(ComponentTypeId a, ComponentTypeId b) noexcept {
  return a.value == b.value;
}
inline bool operator!=(ComponentTypeId a, ComponentTypeId b) noexcept {
  return !(a == b);
}
// Ordering is registration order (M1-ECS-03's per-archetype component
// sets and M1-ECS-05's deterministic iteration order by this).
inline bool operator<(ComponentTypeId a, ComponentTypeId b) noexcept {
  return a.value < b.value;
}

// The size/alignment recorded for a registered component type. The
// M1-ECS-03 SoA layout reads these: one column is a contiguous T[] of
// `size` bytes aligned to `alignment`.
struct ComponentInfo {
  std::uint32_t size{};       // sizeof(T)
  std::uint32_t alignment{};  // alignof(T)
};

// The engine-level cap on component types per world (CORE-005). See
// the preamble for the rationale and the ADR path to raise it.
inline constexpr std::uint32_t kMaxComponentTypes = 256;

namespace detail {

// Compile-time component mark: the primary template says "not a
// component"; LAIGE_COMPONENT(T) (below) specializes it per type.
// detail: engine implementation — games use the macro, not this
// template (CPP-016).
template <typename T>
struct ComponentTraits {
  static constexpr bool isComponent = false;
};

// Compile-time identity token, one per component type (see the
// preamble "Type identity without RTTI"). The marker's VALUE is never
// read; only its address is used, so it stays zero-initialized.
template <typename T>
struct ComponentTypeKey {
  inline static const char kMarker{};
};

// One registry record per registered component type. World stores a
// dense array of these indexed by (id - 1) — ids are dense by
// construction, so no per-record id storage is needed.
struct ComponentRecord {
  const void* typeKey{};  // &ComponentTypeKey<T>::kMarker (identity)
  std::uint32_t size{};      // sizeof(T)
  std::uint32_t alignment{}; // alignof(T)
};

}  // namespace detail

// Mark T as a Laige component (FR-1.2; S-8 data-carrier case).
//
// Compile-time: this specializes the component trait for T, which is
// what World::registerComponent<T>() checks. It adds no runtime state
// by itself; the runtime registration (id + size/alignment) happens
// in World::registerComponent<T>() — the same path for built-in and
// user-defined types. Replication and inspector traits (the PRD
// Appendix B shape) extend this macro in M4.
//
// Write it once per type, at namespace scope, next to the type
// definition.
#define LAIGE_COMPONENT(Type) \
  template <> \
  struct laige::detail::ComponentTraits<Type> { \
    static constexpr bool isComponent = true; \
  };

}  // namespace laige
