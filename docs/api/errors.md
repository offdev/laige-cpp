# Error registry (`laige::ErrorCode`)

The central error-code registry for `laige::Result<T,E>` / `laige::Status`
(M0-CORE-01). Public header: `src/laige-core/include/laige/errors.h`;
implementation: `src/laige-core/errors.cpp`.

## Grammar (NFR-13.3)

Every engine error renders as one line of exactly **5 fields** joined by
`" | "`:

```text
{code} | {what} | {why} | {fix} | {doc_anchor}
```

| Field | Meaning |
|---|---|
| `{code}` | The stable snake_case identifier of the `ErrorCode` (integer value listed per code below). |
| `{what}` | What failed — no call-site detail. |
| `{why}` | The most likely reason, phrased so a reader can act on it. |
| `{fix}` | What the caller should do. |
| `{doc_anchor}` | This file, the section for that code. |

Rendering: `laige::errorText(code)` returns the pre-rendered line —
O(1), no allocation, thread-safe, safe to hand to the logging facade
(LOG-002). The 5-field format, the field ↔ rendered-text equivalence,
the anchor format, and the pinned integer values are asserted by the
`result_status` CTest suite (`ErrorCodeRegistry.*` suites in
`tests/laige-core/result_status_tests.cpp`).

## Stability rules (PRD §9.4)

- **Values are stable.** Once shipped, an integer value never means
  something else and is never reused; minor versions are additive-only.
- **0 is not a code.** It is the no-error sentinel: an ok `Status`
  carries no code. Lookups of `0` or of unregistered values render
  `unknown` — never a crash, never a null pointer (CORE-008).
- **Fields must not contain `|`**, and no field may be empty; that is
  what makes the line machine-parseable (NFR-13.3).

### Adding a code

The step that needs a new code performs, in one change:

1. take the next free integer value in `errors.h` (values are
   additive-only);
2. add the table entry in `errors.cpp` — the fields **and** the
   pre-rendered `text` (the `ErrorCodeRegistry` suites keep them in
   sync);
3. add the section below — heading exactly the code id with underscores
   as dashes, so the GitHub anchor matches `docAnchor`;
4. add the code to `kRegistered` in `result_status_tests.cpp`.

## Codes

### unknown

- **Integer value:** `1` — also the lookup result for `0` and for
  unregistered values.

| Field | Text |
|---|---|
| what | an engine operation failed but no registered code applies |
| why | the failure was not mapped to an ErrorCode, or the code value was corrupted in transit |
| fix | report the numeric code and call site; map the failure to a registered code (see *Adding a code*) |
| doc anchor | `docs/api/errors.md#unknown` |

### invalid-argument

- **Integer value:** `2`

| Field | Text |
|---|---|
| what | a caller passed a value outside the operation's documented domain |
| why | boundary validation rejected the input (API-008: invalid input is validated at the boundary) |
| fix | pass a value within the documented range and units; the log context names the failing parameter |
| doc anchor | `docs/api/errors.md#invalid-argument` |

### malformed-input

- **Integer value:** `3`

| Field | Text |
|---|---|
| what | a parser or decoder rejected structurally invalid input |
| why | the input violated the format's grammar or its size/depth limits (ADR 0003 config-JSON bounds) |
| fix | validate the input against the format spec before use; treat untrusted input as hostile (SCALE-004) |
| doc anchor | `docs/api/errors.md#malformed-input` |

### budget-exhausted

- **Integer value:** `4`

| Field | Text |
|---|---|
| what | a budgeted resource ran out before the requested work completed |
| why | the caller requested more work or capacity than the configured budget allows (PERF-008, SCALE-003) |
| fix | reduce per-call work or raise the budget through typed configuration (API-006); budgeted resources must never grow silently |
| doc anchor | `docs/api/errors.md#budget-exhausted` |

### io-error

- **Integer value:** `5` (added by M0-CORE-02: file sink creation)

| Field | Text |
|---|---|
| what | an I/O operation (a file, or a system interface) failed |
| why | the file could not be opened, written, or flushed (missing path, permissions, full disk), or a system interface call (e.g. signal registration for crash handling) was rejected |
| fix | check the path, permissions, and disk space; for logging, fall back to the current console sink (LOG-007 minimal fallback, `docs/api/logging.md`) |
| doc anchor | `docs/api/errors.md#io-error` |
