# Bounded JSON parser + serializer (`laige::JsonValue`, `parseJson`)

The engine's declarative-config format and the home of all engine JSON
(M0-CORE-07; ADR 0003, FR-1.5). Public header:
`src/laige-core/include/laige/json.h`; implementation:
`src/laige-core/json.cpp`. Unit suite: `ctest -R config_json`
(`tests/laige-core/config_json_tests.cpp`). Fuzz target: `json_parse`
(`tools/fuzz/laige-fuzz.cpp`; CTest entry `fuzz_json_parse`).

ADR 0003's decision: a hand-rolled, *bounded* parser + serializer in
`laige-core`, **no new dependency** (PRD §11 unchanged). The needed
subset — objects, arrays, strings, numbers, booleans, null — is exactly
what this API covers.

## Quick start

```cpp
#include <laige/json.h>

// Parse a config document (whole view must be consumed).
auto doc = laige::parseJson(text);            // Result<JsonValue>
if (doc.isError()) {
  // Log the NFR-13.3 line (LOG-002) — the error is always
  // ErrorCode::MalformedInput (3); no other code is reachable.
  engineLog(laige::ErrorCode::MalformedInput, doc.errorText());
  return;
}

const laige::JsonValue& root = doc.value();

// Read a member with a default (findMember is null-safe on any value).
const laige::JsonValue* tick = root.findMember("tick_rate");
double tickRate = (tick != nullptr && tick->isNumber()) ? tick->asNumber()
                                                         : 60.0;

// Build a document (API manifest tooling, budget files) and serialize.
laige::JsonValue budgets = laige::JsonValue::makeObject();
budgets.setMember("update", laige::JsonValue::fromNumber(1.5));
const std::string text2 = laige::serializeJson(budgets);
// parse(serialize(v)) == v always (see Round-trip below).
```

## Accepted grammar (RFC 8259, strict)

Whitespace = `SPACE` / `TAB` / `LF` / `CR`, between tokens only. Values =
object / array / string / number / `true` / `false` / `null`. Strings
accept the six two-character escapes, `\/`, and `\uXXXX`. Numbers follow
RFC 8259 §6 exactly: `[ - ] int [ frac ] [ exp ]` with no leading zeros,
no leading `+`, and at least one digit after `.` and after `e`/`E`.

Strictness choices above the RFC floor — each returned as
`ErrorCode::MalformedInput` (never a crash, never silent, CORE-008):

| Rule | Rationale |
|---|---|
| Object member names must be unique (duplicates rejected) | Config hygiene: silent last-wins is a footgun for dev-authored documents. |
| Raw control characters `U+0000..U+001F` in strings rejected | The JSON grammar requires them escaped; `\u0000` escapes are accepted and stored as-is. |
| UTF-8 validated strictly | No overlong encodings, no raw surrogate codepoints, nothing above `U+10FFFF`. |
| Surrogate halves only as `\uD800..\uDBFF` + `\uDC00..\uDFFF` pairs | A lone half has no UTF-8 encoding (ADR 0003: "UTF-8 validated"). |

Anything else — trailing data after the top-level value, unterminated
values, bad escapes, bad numbers, invalid bytes — is `MalformedInput`.
A failed parse produces **no** `JsonValue` (all-or-nothing).

## Bounds (ADR 0003)

`parseJson(input, options)` is bounded by `laige::JsonOptions`:

| Option | Default | Meaning |
|---|---|---|
| `maxDocumentBytes` | `1 MiB` (`1 << 20`) | the raw input must not exceed this (inclusive) |
| `maxDepth` | `32` | maximum nested container depth (`maxDepth <= 0` rejects all containers) |

The parser is recursive descent, but recursion depth is bounded by
`maxDepth`, so **no input causes recursion blowup**. Every failure path
is a bounded return — no crash, no partial document. The bounds are
engine-configurable per call (API-006); the defaults are the documented
ones.

## Number semantics

- **JSON number → `double`** (ADR 0003). Config values that need exact
  integers must lie in `±2^53`, where doubles are exact; beyond that,
  integer literals lose low-order bits (e.g. `9007199254740993` stores
  `2^53`). That is a documented precision policy, not an error — the
  config validation layer (M1) owns range checks.
- **Overflow stores `±inf`** (IEEE 754): a well-formed token that does
  not fit a double (e.g. `1e999`) parses to `±inf`. That is a *valid*
  parse result; callers consuming or serializing the value must reject
  non-finite numbers (see Performance and failure below). NaN is
  unreachable from a well-formed document (JSON has no NaN literal).
- **Serializer numbers** are the *shortest correctly rounded decimal*:
  the least precision `p` in `1..17` whose `%g` rendering round-trips to
  the bit-identical double (`1.5` → `1.5`, `2.0` → `2`, `1e21` →
  `1e+21`, `123456.75` → `123456.75`). `-0.0` serializes as `0` (negative
  zero is not preserved). The output is deterministic on every platform
  (correctly-rounded decimal conversion; round-trip equality is
  bit-exact IEEE 754).

## `JsonValue`

A parsed or hand-built document. Value semantics: **copy is deep**
(`O(size)`, allocates), **move is `O(1)`**; a moved-from value is `Null`.
The value always owns exactly the payload its kind names — every kind
change releases the old payload (no stale state).

| Operation | Behavior | Cost |
|---|---|---|
| `kind()` / `isX()` | kind queries | `O(1)` |
| `asBool` / `asNumber` / `asString` / `asArray` / `asObject` | kind accessors; **precondition: matching kind** (debug assert; documented UB in release) | `O(1)` |
| `findMember(name)` | null-safe lookup: `nullptr` when not an object or absent (total on any value) | `O(members)` |
| `hasMember(name)` | `findMember(name) != nullptr` | `O(members)` |
| `setNull` / `setBool` / `setNumber` / `setString` | make this the given value, releasing the old payload | `O(1)` / `O(len)` |
| `append(element)` | array element; **precondition: `isArray()`** | `O(1)` amortized |
| `setMember(name, value)` | replace in place (position preserved) or append; **precondition: `isObject()`** | `O(members)` |
| `operator==` | deep structural equality | `O(size)` |

**Equality** is deep and structural: objects compare **independent of
member order** (`{"a":1,"b":2} == {"b":2,"a":1}` — configs in different
key order are equal), arrays are **order-sensitive**, and a `Number`
holding NaN compares unequal to itself (IEEE 754 `==`).

**Ownership and lifetime (CPP-002, CPP-009, CONC-001).** A `JsonValue`
owns its payload and has exactly one owner thread while mutable — it is
**not thread-safe**. A fully constructed value is safe to read from any
thread (no internal synchronization; the same publish contract as
`Result`/`Status`).

## Serializer

`serializeJson(value)` emits the canonical compact form: no insignificant
whitespace; strings ASCII-safe (`\b \f \n \r \t` for the printable
controls, `\uXXXX` for the rest, and `\uXXXX` — surrogate pairs above
`U+FFFF` — for every codepoint above `0x7F`); numbers as documented
above; objects and arrays in stored order (for parsed documents: document
order). The output is pure ASCII, so it is byte-identical on every
platform and re-parses to the bit-identical value.

## Round-trip

`parse(serialize(v)) == v` for every value whose numbers are finite, and
`serialize(parse(serialize(parse(s)))) == serialize(parse(s))` — the
serializer is idempotent on its own output. The `ConfigJsonRoundTrip`
suite pins both properties on a corpus that includes deep nesting (32),
escaped strings, surrogate pairs, and control characters.

## Performance

**Cold path, O(n).** Parsing and serialization are config-load,
manifest-generation, and budget-file work — **never frame or tick loops**
(PERF-003, PERF-007). Time and space are `O(n)` in document bytes;
object member insertion is `O(members)` per insert (`O(m^2)` per object —
negligible at config scale). One allocation per string value and per
array/object container; the serialized document is one `std::string`.

**Traps:**

- `serializeJson` **recurses** to the value's nesting depth: bounded by
  `JsonOptions::maxDepth` for parsed documents, but a hand-built value of
  pathological depth risks stack overflow (build documents at normal
  depths; the parser cannot produce deeper values than `maxDepth`).
- Serializing a value containing a `Number` holding NaN or `±inf` is a
  documented precondition violation (debug assert; UB in release) —
  non-finite values are not representable in JSON.
- `fromString`/`setString` with invalid UTF-8 is a documented
  precondition violation (debug assert; UB in release). The parser never
  produces invalid UTF-8, so parser-built values are always safe to
  serialize.

## Determinism

Parsing is a pure function of the input bytes: same input → same value,
same serialization, on every platform (no floating point beyond the
documented `double` number semantics, no platform intrinsics, no
ordering that depends on anything but the input). Object and array
iteration order is document order for parsed values and insertion order
for built values — deterministic in both cases.

## Errors

`parseJson` returns `laige::Result<JsonValue>`; **every failure is
`ErrorCode::MalformedInput` (3)** — the registry entry for code 3
already names the ADR 0003 size/depth bounds, so no new codes exist
(CORE-004). `Status::errorText()` / `errorText(code)` renders the
NFR-13.3 five-field line for logging (LOG-002). There is no other
failure channel: discarding the `Result` is a likely logic bug
(CORE-008).

| Condition | Code |
|---|---|
| Input over `maxDocumentBytes` | `ErrorCode::MalformedInput` (3) |
| Nesting over `maxDepth` | `ErrorCode::MalformedInput` (3) |
| Grammar violation (bad token, escape, number, key) | `ErrorCode::MalformedInput` (3) |
| Duplicate object key | `ErrorCode::MalformedInput` (3) |
| Invalid UTF-8 / lone surrogate / raw control character | `ErrorCode::MalformedInput` (3) |
| Trailing data after the top-level value | `ErrorCode::MalformedInput` (3) |

## Fuzzing (NFR-8.7, TEST-005, SCALE-005)

The parser is a malformed-input surface and is fuzzed in CI: the
`json_parse` target feeds `laige::parseJson` a deterministic,
Prng-seeded stream of mutated, truncated, and random byte inputs
(`tools/fuzz/laige-fuzz.cpp`). The bounded run — `laige-fuzz
json_parse --runs=1000` — is registered as the `fuzz_json_parse` CTest
entry and runs in **every build tree**, instrumented in the ASan tree
(the step's Verify gate; PRD §14: fuzz "every commit (bounded), nightly
(long)" — the nightly long-run lane lands with M0-TEST-01).
