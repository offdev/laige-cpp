// laige-core error registry (M0-CORE-01).
//
// A flat table indexed by the integer code: slot N holds the entry for
// ErrorCode N. Slot 0 doubles as the fallback for 0 (the no-error
// sentinel) and for values that are not registered: both render
// `unknown` instead of crashing or returning a null pointer (CORE-008).
//
// Each `text` field is the pre-rendered NFR-13.3 line for its entry. It
// is maintained by hand next to the fields it renders from; the
// result_status suite cross-checks it (RenderedTextMatchesRegistryFields)
// and re-parses it as 5 fields (EveryRegisteredCodeFollowsGrammar), so
// drift or a grammar break fails the test run.

#include "laige/errors.h"

#include <cstddef>

namespace laige {

namespace {

constexpr std::size_t kLastCode =
    static_cast<std::size_t>(ErrorCode::BudgetExhausted);

const ErrorEntry kErrorRegistry[kLastCode + 1] = {
    {   // slot 0: no-error sentinel + unregistered-value fallback
        ErrorCode::Unknown,
        "unknown",
        "an engine operation failed but no registered code applies",
        "the failure was not mapped to an ErrorCode, or the code value "
        "was corrupted in transit",
        "report the numeric code and call site; map the failure to a "
        "registered code (see docs/api/errors.md, 'Adding a code')",
        "docs/api/errors.md#unknown",
        "unknown | an engine operation failed but no registered code "
        "applies | the failure was not mapped to an ErrorCode, or the "
        "code value was corrupted in transit | report the numeric code "
        "and call site; map the failure to a registered code (see "
        "docs/api/errors.md, 'Adding a code') | docs/api/errors.md#unknown"},
    {   // slot 1
        ErrorCode::Unknown,
        "unknown",
        "an engine operation failed but no registered code applies",
        "the failure was not mapped to an ErrorCode, or the code value "
        "was corrupted in transit",
        "report the numeric code and call site; map the failure to a "
        "registered code (see docs/api/errors.md, 'Adding a code')",
        "docs/api/errors.md#unknown",
        "unknown | an engine operation failed but no registered code "
        "applies | the failure was not mapped to an ErrorCode, or the "
        "code value was corrupted in transit | report the numeric code "
        "and call site; map the failure to a registered code (see "
        "docs/api/errors.md, 'Adding a code') | docs/api/errors.md#unknown"},
    {   // slot 2
        ErrorCode::InvalidArgument,
        "invalid_argument",
        "a caller passed a value outside the operation's documented "
        "domain",
        "boundary validation rejected the input (API-008: invalid input "
        "is validated at the boundary)",
        "pass a value within the documented range and units; the log "
        "context names the failing parameter",
        "docs/api/errors.md#invalid-argument",
        "invalid_argument | a caller passed a value outside the "
        "operation's documented domain | boundary validation rejected "
        "the input (API-008: invalid input is validated at the "
        "boundary) | pass a value within the documented range and units; "
        "the log context names the failing parameter | "
        "docs/api/errors.md#invalid-argument"},
    {   // slot 3
        ErrorCode::MalformedInput,
        "malformed_input",
        "a parser or decoder rejected structurally invalid input",
        "the input violated the format's grammar or its size/depth "
        "limits (ADR 0003 config-JSON bounds)",
        "validate the input against the format spec before use; treat "
        "untrusted input as hostile (SCALE-004)",
        "docs/api/errors.md#malformed-input",
        "malformed_input | a parser or decoder rejected structurally "
        "invalid input | the input violated the format's grammar or its "
        "size/depth limits (ADR 0003 config-JSON bounds) | validate the "
        "input against the format spec before use; treat untrusted input "
        "as hostile (SCALE-004) | docs/api/errors.md#malformed-input"},
    {   // slot 4
        ErrorCode::BudgetExhausted,
        "budget_exhausted",
        "a budgeted resource ran out before the requested work "
        "completed",
        "the caller requested more work or capacity than the configured "
        "budget allows (PERF-008, SCALE-003)",
        "reduce per-call work or raise the budget through typed "
        "configuration (API-006); budgeted resources must never grow "
        "silently",
        "docs/api/errors.md#budget-exhausted",
        "budget_exhausted | a budgeted resource ran out before the "
        "requested work completed | the caller requested more work or "
        "capacity than the configured budget allows (PERF-008, "
        "SCALE-003) | reduce per-call work or raise the budget through "
        "typed configuration (API-006); budgeted resources must never "
        "grow silently | docs/api/errors.md#budget-exhausted"},
};

}  // namespace

const ErrorEntry& errorInfo(ErrorCode code) {
  const std::size_t v = static_cast<std::size_t>(code);
  return (v <= kLastCode) ? kErrorRegistry[v] : kErrorRegistry[0];
}

const char* errorName(ErrorCode code) { return errorInfo(code).codeId; }

const char* errorText(ErrorCode code) { return errorInfo(code).text; }

}  // namespace laige
