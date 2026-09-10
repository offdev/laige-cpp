// laige-core Result<T,E> / Status (M0-CORE-01).
//
// FR-12.1: the engine core and public API use no exceptions, no RTTI, and
// no dynamic_cast. Errors are Result<T,E> / Status plus the stable
// ErrorCode registry (errors.h, NFR-13.3). This translation unit compiles
// with -fno-exceptions / -fno-rtti (NFR-8.10, laige_apply_engine_policy);
// any exception use here would fail the build.
//
// Performance contract (PERF-003/005): both types are value types with
// inline storage (std::optional; no heap, no allocation on any operation).
// All accessors are O(1). Both types are immutable after construction
// (const accessors only) and safe to read from any thread once published.
//
// Misuse warnings:
//   - value() / error() require the state check (ok() / isError()) first:
//     debug builds assert, release builds treat an unchecked call as
//     undefined behavior. For a branchless, always-safe read use
//     valueIfOk() / errorIfError() (nullptr when the state does not match).
//   - Discarding a Result or Status is a likely logic bug (CORE-008): all
//     constructors, factories, and accessors are [[nodiscard]].
//   - A moved-from Result is in a valid but unspecified state; use it
//     only for destruction or assignment (as with std::unique_ptr).

#pragma once

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>

#include "laige/errors.h"

namespace laige {

// A result carrying either a success value of type T or a failure of
// type E (default: laige::ErrorCode).
//
// Constraints: T must not be void (use Status); E must be
// default-constructible and move-constructible (ErrorCode satisfies both).
// When T and E are mutually convertible the converting constructors are
// unavailable (they would be ambiguous) — use success()/failure().
template <typename T, typename E = ErrorCode>
class Result {
  static_assert(!std::is_void_v<T>,
                "Result<void> is unrepresentable; use laige::Status");

 public:
  // An empty Result has no defined state and is therefore
  // unrepresentable (API-008).
  Result() = delete;

  Result(const Result&) = default;
  Result(Result&&) = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) = default;

  // Success carrying `value` (copied or moved in).
  [[nodiscard]]
  Result(T value)
      requires(!std::is_convertible_v<E, T>)
      : value_(std::move(value)), error_(E{}) {}

  // Failure carrying `error`.
  [[nodiscard]]
  Result(E error)
      requires(!std::is_convertible_v<T, E>)
      : value_(), error_(std::move(error)) {}

  // Unambiguous factory forms (always available, including when T and E
  // are mutually convertible).
  [[nodiscard]] static Result success(T value) {
    Result r(FactoryTag{});
    r.value_ = std::move(value);
    return r;
  }
  [[nodiscard]] static Result failure(E error) {
    Result r(FactoryTag{});
    r.error_ = std::move(error);
    return r;
  }

  // True when the result carries a success value.
  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool isError() const noexcept { return !ok(); }

  // The success value. Precondition: ok(). Debug builds assert; in
  // release builds an unchecked call is undefined behavior.
  [[nodiscard]] const T& value() const noexcept {
    assert(ok() && "Result::value() called on an error result");
    return *value_;
  }

  // The failure value. Precondition: isError(). Debug builds assert; in
  // release builds an unchecked call is undefined behavior.
  [[nodiscard]] const E& error() const noexcept {
    assert(isError() && "Result::error() called on a success result");
    return error_;
  }

  // Null-safe accessors (no precondition): nullptr when the result does
  // not carry the requested state.
  [[nodiscard]] const T* valueIfOk() const noexcept {
    return value_.has_value() ? &*value_ : nullptr;
  }
  [[nodiscard]] const E* errorIfError() const noexcept {
    return ok() ? nullptr : &error_;
  }

 private:
  // Tag for the factory-internal constructor. Private, so an empty
  // Result cannot be constructed from outside the class (API-008: the
  // public Result() stays deleted).
  struct FactoryTag {};

  explicit Result(FactoryTag) : value_(), error_(E{}) {}

  std::optional<T> value_;  // set  <=> success; empty <=> error state
  E error_;                 // meaningful only while value_ is empty
};

// A Result without a success value: an operation outcome only.
//
// A default-constructed Status is success — the no-error sentinel, which
// carries no code. A Status constructed from an ErrorCode (implicitly or
// via failure()) is a failure carrying that code.
class Status {
 public:
  Status() noexcept = default;
  Status(ErrorCode code) noexcept : ok_(false), code_(code) {}

  [[nodiscard]] static Status failure(ErrorCode code) noexcept {
    return Status(code);
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool isError() const noexcept { return !ok_; }

  // The failure code. Precondition: isError(). Debug builds assert; in
  // release builds an unchecked call is undefined behavior.
  [[nodiscard]] ErrorCode error() const noexcept {
    assert(isError() && "Status::error() called on an ok Status");
    return code_;
  }

  // The pre-rendered NFR-13.3 line for the failure code.
  // Precondition: isError().
  [[nodiscard]] const char* errorText() const noexcept {
    assert(isError() && "Status::errorText() called on an ok Status");
    return laige::errorText(code_);
  }

 private:
  bool ok_ = true;
  ErrorCode code_ = ErrorCode::Unknown;  // meaningful only while !ok_
};

}  // namespace laige
