// laige-core error-code registry (M0-CORE-01).
//
// FR-12.1: the engine core and public API use no exceptions — errors are
// ErrorCode values carried by laige::Result / laige::Status (result.h).
// NFR-13.3: every engine error follows the 5-field grammar
//
//     {code} | {what} | {why} | {fix} | {doc_anchor}
//
// parseable by both machines and humans.
//
// Stability contract (PRD §9.4): ErrorCode integer values are stable —
// once shipped, a value never means something else, and new codes are
// additive-only. 0 is NOT a code: it is the no-error sentinel (an ok
// Status carries no code). Lookups of 0 or of values that are not
// registered render the `unknown` entry instead of failing (CORE-008:
// no silent failure, not even for a corrupted code value).
//
// Ownership/lifetime: the registry is process-lifetime, immutable, and
// safe to read from any thread. All accessors are O(1) and never
// allocate. `docAnchor` points at docs/api/errors.md, one section per
// code; that document is the human-readable registry.

#pragma once

#include <cstdint>

namespace laige {

// Stable error codes. The enumerator values are part of the stability
// contract (PRD §9.4): never renumber, never reuse.
enum class ErrorCode : std::uint32_t {
  Unknown = 1,
  InvalidArgument = 2,
  MalformedInput = 3,
  BudgetExhausted = 4,
};

// One registry entry per stable code. `text` is the pre-rendered
// NFR-13.3 line; it MUST be exactly "codeId | whatFailed | why | fix |
// docAnchor". The result_status suite cross-checks `text` against the
// other fields and re-parses it, so drift or a broken grammar fails the
// test run loudly (CORE-006: docs, tests, and code ship together).
struct ErrorEntry {
  ErrorCode code;
  const char* codeId;     // field 1: stable snake_case identifier
  const char* whatFailed;  // field 2: what failed
  const char* why;           // field 3: most likely reason
  const char* fix;           // field 4: what the caller should do
  const char* docAnchor;     // field 5: section in docs/api/errors.md
  const char* text;          // pre-rendered "f1 | f2 | f3 | f4 | f5"
};

// The registry entry for `code`. Unregistered values — including 0, the
// no-error sentinel — map to the `unknown` entry.
// Complexity O(1); no allocation; thread-safe.
[[nodiscard]] const ErrorEntry& errorInfo(ErrorCode code);

// The stable identifier (grammar field 1) for `code`.
// Complexity O(1); no allocation; thread-safe.
[[nodiscard]] const char* errorName(ErrorCode code);

// The pre-rendered NFR-13.3 line for `code`:
// "{codeId} | {whatFailed} | {why} | {fix} | {docAnchor}".
// Complexity O(1); no allocation; thread-safe; safe to hand to the
// logging facade (LOG-002).
[[nodiscard]] const char* errorText(ErrorCode code);

}  // namespace laige
