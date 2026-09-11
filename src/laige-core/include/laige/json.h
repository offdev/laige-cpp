// laige-core bounded JSON parser + serializer (M0-CORE-07).
//
// FR-1.5: declarative game config in JSON. ADR 0003 (config JSON
// strategy): a hand-rolled *bounded* parser + serializer in laige-core,
// no new dependency (PRD §11), malformed input -> Status error
// (CORE-008), UTF-8 validated, a serializer for round-trip (used by the
// API manifest tooling and budget files), and a fuzz target `json_parse`
// (NFR-8.7, TEST-005). Full API contract: docs/api/json.md.
//
// ---------------------------------------------------------------------------
// Accepted grammar (RFC 8259, strict)
// ---------------------------------------------------------------------------
//
//   whitespace  SPACE / TAB / LF / CR, only *between* tokens
//   value       object / array / string / number / true / false / null
//   object      '{' [member (',' member)*] '}'     member = string ':' value
//   array       '[' [value (',' value)*] ']'
//   string      '"' (unescaped-char | escape)* '"'
//   escape      '"' '\' '/' 'b' 'f' 'n' 'r' 't' | '\u' HEXDIGIT{4}
//   number      [ '-' ] int [ frac ] [ exp ]
//               int  = '0' | [1-9] DIGIT*          (no leading zeros,
//                                                            no '+')
//               frac = '.' DIGIT+
//               exp  = ('e'|'E') ['+'|'-'] DIGIT+
//
//   Strictness choices above the RFC 8259 floor (each is a documented,
//   actionable MalformedInput — never a crash, never silent, CORE-008):
//
//     - Object member names must be unique; a duplicate key is rejected.
//       (Config hygiene: silent last-wins is a footgun for the
//       dev-authored documents this parser exists for.)
//     - Raw control characters U+0000..U+001F in strings are rejected
//       (the JSON grammar requires them escaped); the "\u0000" escape is
//       accepted and stored as-is.
//     - UTF-8 is validated strictly: no overlong encodings, no raw
//       surrogate codepoints, nothing above U+10FFFF.
//     - A "\uD800" high-surrogate escape is valid only when immediately
//       followed by a "\uDC00" low-surrogate escape, together forming a
//       codepoint above U+FFFF; lone surrogate halves are rejected (they
//       have no UTF-8 encoding — ADR 0003 "UTF-8 validated").
//
//   Anything else — trailing data after the top-level value, unterminated
//   values, bad escapes, bad numbers, invalid bytes — is MalformedInput.
//   A failed parse produces no JsonValue (all-or-nothing, CORE-008).
//
// ---------------------------------------------------------------------------
// Bounds (ADR 0003)
// ---------------------------------------------------------------------------
//
//   parseJson is bounded on raw input size and structural depth
//   (JsonOptions; the documented defaults are engine-configurable):
//
//     maxDocumentBytes  1 MiB (1 << 20) — the raw input must not exceed
//     maxDepth          32         — nested container depth
//
//   The parser is recursive descent, but recursion depth is bounded by
//   maxDepth, so no input causes recursion blowup. Every failure path is
//   a bounded return — no crash, no partial document.
//
// ---------------------------------------------------------------------------
// Number semantics (ADR 0003: "JSON number -> double")
// ---------------------------------------------------------------------------
//
//   - A JSON number is stored as a `double`. Config values that need
//     exact integers must lie in +/-2^53, where doubles are exact;
//     beyond that, integer literals silently lose low-order bits. That
//     is a *documented precision policy* (this header), not an error —
//     the config validation layer (M1) owns range checks.
//   - A number token that overflows double converts to +/-inf (IEEE 754;
//     e.g. 1e999 -> +inf). That is a *valid parse result* — the token is
//     well-formed JSON — and callers serializing or consuming the value
//     must reject non-finite numbers (see Misuse warnings). NaN is
//     unreachable from a well-formed document (JSON has no NaN literal).
//   - serializeJson emits the *shortest correctly rounded decimal* for
//     each finite number (the least precision p in 1..17 whose %g
//     rendering round-trips to the bit-identical double), so
//     parse(serialize(v)) == v always: serialized text re-parses to the
//     same value. -0.0 serializes as "0" (negative zero is not
//     preserved).
//
// ---------------------------------------------------------------------------
// JsonValue contract
// ---------------------------------------------------------------------------
//
// Ownership (CPP-002, CPP-009): a JsonValue owns its payload (string
// bytes, element vector, or member vector). Value semantics: copy is
// *deep* (O(size), allocates), move is O(1); a moved-from value is Null.
// The value always owns exactly the payload its kind names — every kind
// change releases the old payload (CPP-004: no stale active member).
//
// Threading (CONC-001): a JsonValue has exactly one owner thread while
// mutable and is NOT thread-safe. A fully constructed value is safe to
// read from any thread (no internal synchronization — the same publish
// contract as Result/Status, M0-CORE-01).
//
// Equality (operator==): deep and structural. Objects compare
// independent of member order ({"a":1,"b":2} == {"b":2,"a":1}); arrays
// are order-sensitive. A Number holding NaN compares unequal to itself
// (IEEE 754 ==).
//
// Performance (PERF-003/005, PERF-007): parsing and serialization are
// *cold paths* — config load, manifest generation, budget files — never
// frame or tick loops. Time and space are O(n) in document bytes (object
// member insertion is O(members) per insert, O(m^2) per object —
// negligible at config scale). serializeJson recurses to the value's
// nesting depth: bounded by JsonOptions::maxDepth for parsed documents;
// a hand-built value of pathological depth risks stack overflow (see
// Misuse warnings).
//
// Errors: parseJson returns Result<JsonValue>; every failure is
// ErrorCode::MalformedInput — the registry entry for code 3 already
// names the ADR 0003 size/depth bounds, so no new codes (CORE-004).
// Status::errorText() renders the NFR-13.3 line for logging (LOG-002).
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//   - fromString(s) / setString(s) require s to be valid UTF-8 (debug
//     assert; documented undefined behavior in release). The parser
//     never produces invalid UTF-8, so parser-built values are always
//     safe to serialize.
//   - serializeJson(v) requires v to contain no Number holding NaN or
//     +/-inf (debug assert; documented undefined behavior in release).
//     Parsed documents can carry +/-inf (number overflow); a config
//     loader must reject non-finite values before serializing or
//     consuming them.
//   - The asX() accessors require kind() == X (debug assert; documented
//     undefined behavior in release). findMember/hasMember are the
//     null-safe forms for object lookups: nullptr / false when the value
//     is not an object or the member is absent (total on any value).
//   - append()/setMember() require the matching container kind (debug
//     assert; documented undefined behavior in release).
//   - Discarding the Result from parseJson is a likely logic bug
//     (CORE-008); the API carries no other failure channel.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "laige/result.h"

namespace laige {

// The six JSON value kinds (RFC 8259).
enum class JsonKind : std::uint8_t {
  Null,
  Bool,
  Number,
  String,
  Array,
  Object,
};

// Parse bounds (ADR 0003). The defaults are the documented ones; engine
// code may tighten (or, in principle, loosen) them per parse call.
// maxDepth <= 0 rejects every container; maxDocumentBytes is a bound on
// the raw input bytes.
struct JsonOptions {
  std::size_t maxDocumentBytes = static_cast<std::size_t>(1u) << 20;  // 1 MiB
  int maxDepth = 32;
};

// A parsed (or hand-built) JSON document. See the preamble for the full
// contract: grammar, bounds, number semantics, ownership, threading,
// equality, performance, and error behavior.
class JsonValue {
 public:
  // The Null value.
  JsonValue() noexcept = default;

  // Value semantics (preamble): copy is deep (O(size), allocates); move is
  // O(1) and leaves the moved-from value Null. Copy assignment and move
  // assignment release this value's old payload first, so the "owns
  // exactly the payload its kind names" invariant holds across
  // assignment, not just across kind-changing mutation.
  JsonValue(const JsonValue& other) noexcept;
  JsonValue& operator=(const JsonValue& other) noexcept;
  JsonValue(JsonValue&& other) noexcept;
  JsonValue& operator=(JsonValue&& other) noexcept;

  // Factories (the engine builds with -fno-exceptions: a failed
  // allocation terminates the process, it never throws).
  [[nodiscard]] static JsonValue fromBool(bool value) noexcept;
  [[nodiscard]] static JsonValue fromNumber(double value) noexcept;
  [[nodiscard]] static JsonValue fromString(std::string_view value) noexcept;
  [[nodiscard]] static JsonValue makeArray() noexcept;
  [[nodiscard]] static JsonValue makeObject() noexcept;

  // Kind queries.
  [[nodiscard]] JsonKind kind() const noexcept { return kind_; }
  [[nodiscard]] bool isNull() const noexcept { return kind_ == JsonKind::Null; }
  [[nodiscard]] bool isBool() const noexcept { return kind_ == JsonKind::Bool; }
  [[nodiscard]] bool isNumber() const noexcept { return kind_ == JsonKind::Number; }
  [[nodiscard]] bool isString() const noexcept { return kind_ == JsonKind::String; }
  [[nodiscard]] bool isArray() const noexcept { return kind_ == JsonKind::Array; }
  [[nodiscard]] bool isObject() const noexcept { return kind_ == JsonKind::Object; }

  // Kind accessors. Precondition: the matching kind (debug assert;
  // documented undefined behavior in release).
  [[nodiscard]] bool asBool() const noexcept;
  [[nodiscard]] double asNumber() const noexcept;
  [[nodiscard]] std::string_view asString() const noexcept;
  [[nodiscard]] const std::vector<JsonValue>& asArray() const noexcept;
  [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>&
  asObject() const noexcept;

  // Null-safe object lookups (total on any value): nullptr / false when
  // this is not an object or the member is absent.
  [[nodiscard]] const JsonValue* findMember(std::string_view name) const noexcept;
  [[nodiscard]] bool hasMember(std::string_view name) const noexcept;

  // Mutations. The setX() forms make this the given kind/value, releasing
  // the old payload first. append()/setMember() require the matching
  // container kind (debug assert; documented undefined behavior in
  // release). setMember() replaces an existing member in place (position
  // preserved) or appends it.
  void setNull() noexcept;
  void setBool(bool value) noexcept;
  void setNumber(double value) noexcept;
  void setString(std::string_view value) noexcept;
  void append(JsonValue element);
  void setMember(std::string_view name, JsonValue value);

  // Deep structural equality (see the preamble: objects order-insensitive,
  // arrays order-sensitive, NaN != NaN).
  [[nodiscard]] bool operator==(const JsonValue& other) const noexcept;
  [[nodiscard]] bool operator!=(const JsonValue& other) const noexcept {
    return !(*this == other);
  }

 private:
  // Releases the payload of the current kind (destroy + release), so the
  // value always owns exactly the payload its kind_ names (CPP-004).
  void clearPayload() noexcept;

  JsonKind kind_ = JsonKind::Null;
  bool boolean_ = false;                                    // Bool
  double number_ = 0.0;                                    // Number
  std::string str_;                                        // String: valid
                                                          // UTF-8, may hold NUL
  std::vector<JsonValue> arr_;                             // Array: document order
  std::vector<std::pair<std::string, JsonValue>> members_;  // Object: document
                                                          // order, unique keys
};

// Parses exactly one JSON document from `input` (the whole view must be
// consumed; trailing non-whitespace is MalformedInput). Bounded by
// `options` (defaults: 1 MiB, depth 32 — see the preamble). Every failure
// is ErrorCode::MalformedInput; a failed parse produces no value.
[[nodiscard]]
Result<JsonValue> parseJson(std::string_view input,
                            JsonOptions options = {});

// Serializes `value` to canonical compact JSON (no insignificant
// whitespace): strings ASCII-safe (\uXXXX for control characters and
// every codepoint above 0x7F; the two-character escapes for the six
// printable ones), numbers shortest-round-trip decimal (see the
// preamble), objects and arrays in stored order. Precondition: no Number
// holding NaN or +/-inf anywhere in the value (debug assert; documented
// undefined behavior in release). Cold path: allocates one output string
// plus recursive calls per nesting level.
[[nodiscard]]
std::string serializeJson(const JsonValue& value);

}  // namespace laige
