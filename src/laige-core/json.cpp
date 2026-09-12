// laige-core bounded JSON parser + serializer (M0-CORE-07).
//
// Implementation of include/laige/json.h. The parser is a strict,
// depth-bounded recursive descent over RFC 8259 (grammar, bounds, and
// strictness choices are documented in the header preamble); every
// failure is a bounded return of ErrorCode::MalformedInput (CORE-008:
// never a crash, never silent). The serializer emits the canonical
// compact form documented there. Fuzz target: `json_parse`
// (tools/fuzz/laige-fuzz.cpp, NFR-8.7).

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "laige/json.h"

namespace laige {

namespace {

// ---------------------------------------------------------------------------
// Strict UTF-8 decoding (shared by the parser, the serializer, and the
// string factories). Validates: no overlong encodings, no raw surrogate
// codepoints, nothing above U+10FFFF. On success advances `pos` past the
// sequence and sets `cp`.
// ---------------------------------------------------------------------------
bool decodeUtf8(std::string_view in, std::size_t& pos, std::uint32_t& cp) {
  const std::size_t n = in.size();
  const auto byteAt = [&](std::size_t i) -> std::uint32_t {
    return static_cast<std::uint8_t>(in[i]);
  };
  const std::size_t p0 = pos;
  if (p0 >= n) return false;
  const std::uint32_t b0 = byteAt(p0);
  if (b0 < 0x80u) {
    cp = b0;
    pos = p0 + 1;
    return true;
  }
  if (b0 >= 0xC2u && b0 <= 0xDFu) {
    if (p0 + 1 >= n) return false;
    const std::uint32_t b1 = byteAt(p0 + 1);
    if ((b1 & 0xC0u) != 0x80u) return false;
    cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
    pos = p0 + 2;
    return true;
  }
  if (b0 >= 0xE0u && b0 <= 0xEFu) {
    if (p0 + 2 >= n) return false;
    const std::uint32_t b1 = byteAt(p0 + 1);
    const std::uint32_t b2 = byteAt(p0 + 2);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) return false;
    cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
    if (cp < 0x800u) return false;                     // overlong
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;  // raw surrogate
    pos = p0 + 3;
    return true;
  }
  if (b0 >= 0xF0u && b0 <= 0xF4u) {
    if (p0 + 3 >= n) return false;
    const std::uint32_t b1 = byteAt(p0 + 1);
    const std::uint32_t b2 = byteAt(p0 + 2);
    const std::uint32_t b3 = byteAt(p0 + 3);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u ||
        (b3 & 0xC0u) != 0x80u) {
      return false;
    }
    cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
         ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
    if (cp < 0x10000u) return false;  // overlong (b0 <= 0xF4 => cp <= 0x10FFFF)
    pos = p0 + 4;
    return true;
  }
  return false;  // 0x80-0xC1 and 0xF5-0xFF: never valid
}

// [[maybe_unused]]: referenced only from the debug asserts below (the
// fromString / setString UTF-8 preconditions); with NDEBUG (release
// builds) those asserts are compiled out and the helper would be unused
// (CORE-010: no new warnings under -Werror).
[[maybe_unused]] bool isValidUtf8(std::string_view s) {
  std::size_t pos = 0;
  while (pos < s.size()) {
    std::uint32_t cp = 0;
    if (!decodeUtf8(s, pos, cp)) return false;
  }
  return true;
}

// UTF-8 encode a codepoint (precondition: cp <= 0x10FFFF, never a
// surrogate).
void appendUtf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80u) {
    out.push_back(static_cast<char>(cp));
    return;
  }
  if (cp < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return;
  }
  if (cp < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return;
  }
  out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
  out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
}

// ---------------------------------------------------------------------------
// Parser state. All helpers below take this by reference; every failure
// sets `failed` (idempotent) and returns false.
// ---------------------------------------------------------------------------
struct ParserState {
  std::string_view in;
  std::size_t pos = 0;
  int maxDepth = 0;
  int depth = 0;  // currently open containers (0 at top level)
  bool failed = false;
};

void fail(ParserState& p) { p.failed = true; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

void skipWhitespace(ParserState& p) {
  while (p.pos < p.in.size()) {
    const char c = p.in[p.pos];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    ++p.pos;
  }
}

bool consume(ParserState& p, char c) {
  if (p.pos < p.in.size() && p.in[p.pos] == c) {
    ++p.pos;
    return true;
  }
  return false;
}

// Bounded container-depth increment (ADR 0003: no recursion blowup).
struct DepthGuard {
  int& depth;
  bool ok;
  explicit DepthGuard(int& d, int max) : depth(d) {
    ok = (d + 1 <= max);
    if (ok) ++d;
  }
  ~DepthGuard() {
    if (ok) --depth;
  }
  explicit operator bool() const { return ok; }
};

bool parseLiteral(ParserState& p, const char* lit, JsonValue& out) {
  // out is always a fresh (Null) local at the call site.
  const std::size_t n = std::char_traits<char>::length(lit);
  if (p.in.compare(p.pos, n, lit) != 0) {
    fail(p);
    return false;
  }
  p.pos += n;
  if (lit[0] == 't') {
    out = JsonValue::fromBool(true);
  } else if (lit[0] == 'f') {
    out = JsonValue::fromBool(false);
  } else {
    out = JsonValue();  // "null"
  }
  return true;
}

// RFC 8259 §6 number grammar: [ '-' ] int [ frac ] [ exp ], with the
// strict sub-rules (no leading zeros, at least one digit after '.' and
// after 'e'/'E'). The token is converted with std::strtod; an overflow
// token (e.g. 1e999) stores +/-inf (documented in the header).
bool parseNumber(ParserState& p, JsonValue& out) {
  const std::size_t start = p.pos;
  if (p.in[p.pos] == '-') ++p.pos;
  if (p.pos >= p.in.size() || !isDigit(p.in[p.pos])) {
    fail(p);
    return false;
  }
  if (p.in[p.pos] == '0') {
    ++p.pos;
    if (p.pos < p.in.size() && isDigit(p.in[p.pos])) {
      fail(p);  // leading zero ("01")
      return false;
    }
  } else {
    while (p.pos < p.in.size() && isDigit(p.in[p.pos])) ++p.pos;
  }
  if (p.pos < p.in.size() && p.in[p.pos] == '.') {
    ++p.pos;
    if (p.pos >= p.in.size() || !isDigit(p.in[p.pos])) {
      fail(p);  // "." with no fractional digits
      return false;
    }
    while (p.pos < p.in.size() && isDigit(p.in[p.pos])) ++p.pos;
  }
  if (p.pos < p.in.size() && (p.in[p.pos] == 'e' || p.in[p.pos] == 'E')) {
    ++p.pos;
    if (p.pos < p.in.size() && (p.in[p.pos] == '+' || p.in[p.pos] == '-')) {
      ++p.pos;
    }
    if (p.pos >= p.in.size() || !isDigit(p.in[p.pos])) {
      fail(p);  // 'e' with no exponent digits
      return false;
    }
    while (p.pos < p.in.size() && isDigit(p.in[p.pos])) ++p.pos;
  }
  const std::string token(p.in.data() + start, p.in.data() + p.pos);
  out = JsonValue::fromNumber(std::strtod(token.c_str(), nullptr));
  return true;
}

bool parseHex4(ParserState& p, std::uint32_t& cp) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    if (p.pos >= p.in.size()) {
      fail(p);
      return false;
    }
    const char c = p.in[p.pos++];
    std::uint32_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<std::uint32_t>(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<std::uint32_t>(c - 'A') + 10u;
    } else {
      fail(p);
      return false;
    }
    v = (v << 4) | d;
  }
  cp = v;
  return true;
}

// A JSON string: strict UTF-8, control characters rejected raw,
// surrogate pairs only. Builds a local decoded buffer; on success the
// result goes to `out` via setString (out is a fresh local at the call
// site).
bool parseString(ParserState& p, JsonValue& out) {
  consume(p, '"');
  std::string decoded;
  for (;;) {
    if (p.pos >= p.in.size()) {
      fail(p);  // unterminated
      return false;
    }
    const char c = p.in[p.pos];
    if (c == '"') {
      ++p.pos;
      out.setString(std::string_view(decoded));
      return true;
    }
    if (c == '\\') {
      ++p.pos;
      if (p.pos >= p.in.size()) {
        fail(p);
        return false;
      }
      const char e = p.in[p.pos];
      switch (e) {
        case '"':
          decoded.push_back('"');
          ++p.pos;
          continue;
        case '\\':
          decoded.push_back('\\');
          ++p.pos;
          continue;
        case '/':
          decoded.push_back('/');
          ++p.pos;
          continue;
        case 'b':
          decoded.push_back('\b');
          ++p.pos;
          continue;
        case 'f':
          decoded.push_back('\f');
          ++p.pos;
          continue;
        case 'n':
          decoded.push_back('\n');
          ++p.pos;
          continue;
        case 'r':
          decoded.push_back('\r');
          ++p.pos;
          continue;
        case 't':
          decoded.push_back('\t');
          ++p.pos;
          continue;
        case 'u': {
          ++p.pos;
          std::uint32_t hi = 0;
          if (!parseHex4(p, hi)) return false;
          if (hi >= 0xD800u && hi <= 0xDBFFu) {
            // High surrogate: a low surrogate half must immediately follow.
            if (p.pos + 1 >= p.in.size() || p.in[p.pos] != '\\' ||
                p.in[p.pos + 1] != 'u') {
              fail(p);
              return false;  // lone high surrogate
            }
            p.pos += 2;
            std::uint32_t lo = 0;
            if (!parseHex4(p, lo)) return false;
            if (lo < 0xDC00u || lo > 0xDFFFu) {
              fail(p);
              return false;  // not a low surrogate
            }
            appendUtf8(decoded,
                       0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u));
          } else if (hi >= 0xDC00u && hi <= 0xDFFFu) {
            fail(p);
            return false;  // lone low surrogate
          } else {
            appendUtf8(decoded, hi);
          }
          continue;
        }
        default:
          fail(p);
          return false;
      }
    }
    // Raw UTF-8 sequence (strict decode); control characters rejected.
    const std::size_t start = p.pos;
    std::uint32_t cp = 0;
    if (!decodeUtf8(p.in, p.pos, cp)) {
      fail(p);
      return false;
    }
    if (cp < 0x20u) {
      fail(p);  // raw control character (JSON grammar requires \uXXXX)
      return false;
    }
    decoded.append(p.in.data() + start, p.pos - start);
  }
}

bool parseObjectMembers(ParserState& p, JsonValue& obj);
bool parseValue(ParserState& p, JsonValue& out);

bool parseObject(ParserState& p, JsonValue& out) {
  consume(p, '{');  // guaranteed by the caller
  out = JsonValue::makeObject();
  DepthGuard guard(p.depth, p.maxDepth);
  if (!guard) {
    fail(p);  // depth limit
    return false;
  }
  skipWhitespace(p);
  if (consume(p, '}')) return true;
  return parseObjectMembers(p, out);
}

bool parseObjectMembers(ParserState& p, JsonValue& obj) {
  for (;;) {
    // Whitespace is legal between tokens (the header grammar), including
    // after the ',' of the previous member: the key goes through
    // parseString directly (not parseValue), so it needs its own skip.
    // (Found by M0-CORE-08: the repo-root budgets.json — a hand-formatted
    // document with ", " between members — was rejected; regression tests
    // in the ConfigJsonInvalid/Valid suites.)
    skipWhitespace(p);
    JsonValue key;
    if (!parseString(p, key)) return false;  // keys must be quoted strings
    skipWhitespace(p);
    if (!consume(p, ':')) {
      fail(p);
      return false;
    }
    JsonValue value;
    if (!parseValue(p, value)) return false;
    if (obj.findMember(key.asString()) != nullptr) {
      fail(p);  // duplicate key (strictness choice, see the header)
      return false;
    }
    obj.setMember(key.asString(), std::move(value));
    skipWhitespace(p);
    if (consume(p, ',')) continue;
    if (consume(p, '}')) return true;
    fail(p);
    return false;
  }
}

bool parseArray(ParserState& p, JsonValue& out) {
  consume(p, '[');  // guaranteed by the caller
  out = JsonValue::makeArray();
  DepthGuard guard(p.depth, p.maxDepth);
  if (!guard) {
    fail(p);  // depth limit
    return false;
  }
  skipWhitespace(p);
  if (consume(p, ']')) return true;
  for (;;) {
    JsonValue element;
    if (!parseValue(p, element)) return false;
    out.append(std::move(element));
    skipWhitespace(p);
    if (consume(p, ',')) continue;
    if (consume(p, ']')) return true;
    fail(p);
    return false;
  }
}

bool parseValue(ParserState& p, JsonValue& out) {
  skipWhitespace(p);
  if (p.pos >= p.in.size()) {
    fail(p);  // empty / truncated
    return false;
  }
  const char c = p.in[p.pos];
  switch (c) {
    case '{':
      return parseObject(p, out);
    case '[':
      return parseArray(p, out);
    case '"':
      return parseString(p, out);
    case 't':
      return parseLiteral(p, "true", out);
    case 'f':
      return parseLiteral(p, "false", out);
    case 'n':
      return parseLiteral(p, "null", out);
    default:
      if (c == '-' || isDigit(c)) return parseNumber(p, out);
      fail(p);
      return false;
  }
}

// ---------------------------------------------------------------------------
// Serializer (canonical compact form). ASCII-safe strings; shortest
// correctly rounded decimal numbers; stored order for arrays/objects.
// ---------------------------------------------------------------------------
constexpr char kHexDigits[] = "0123456789abcdef";

void appendHex4(std::string& out, std::uint32_t v) {
  out.push_back(kHexDigits[(v >> 12) & 0xFu]);
  out.push_back(kHexDigits[(v >> 8) & 0xFu]);
  out.push_back(kHexDigits[(v >> 4) & 0xFu]);
  out.push_back(kHexDigits[v & 0xFu]);
}

void appendEscapedString(std::string& out, std::string_view s) {
  out.push_back('"');
  std::size_t i = 0;
  while (i < s.size()) {
    const std::uint8_t b = static_cast<std::uint8_t>(s[i]);
    if (b < 0x20u) {
      switch (b) {
        case 0x08u:
          out += "\\b";
          break;
        case 0x0Cu:
          out += "\\f";
          break;
        case 0x0Au:
          out += "\\n";
          break;
        case 0x0Du:
          out += "\\r";
          break;
        case 0x09u:
          out += "\\t";
          break;
        default:
          out += "\\u00";
          out.push_back(kHexDigits[(b >> 4) & 0xFu]);
          out.push_back(kHexDigits[b & 0xFu]);
          break;
      }
      ++i;
      continue;
    }
    if (b >= 0x80u) {
      // Canonical form escapes all codepoints above 0x7F (\uXXXX,
      // surrogate pairs above U+FFFF) so the output is pure ASCII and
      // re-parses bit-identically on every platform.
      std::uint32_t cp = 0;
      if (!decodeUtf8(s, i, cp)) {
        // Unreachable: JsonValue strings are valid UTF-8 (the parser
        // guarantees it; fromString/setString assert it).
        assert(false && "serializeJson: string is not valid UTF-8");
        return;
      }
      if (cp > 0xFFFFu) {
        const std::uint32_t v = cp - 0x10000u;
        out += "\\u";
        appendHex4(out, 0xD800u + (v >> 10));
        out += "\\u";
        appendHex4(out, 0xDC00u + (v & 0x3FFu));
      } else {
        out += "\\u";
        appendHex4(out, cp);
      }
      continue;
    }
    if (b == '"' || b == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(b));
    } else {
      out.push_back(static_cast<char>(b));
    }
    ++i;
  }
  out.push_back('"');
}

// Shortest correctly rounded decimal: the least precision p in 1..17
// whose %g rendering round-trips to the bit-identical double. 17
// significant digits always suffice for a finite IEEE 754 double, so the
// loop cannot fall through; the tail is a defensive fallback only.
void appendNumber(std::string& out, double v) {
  assert(std::isfinite(v) &&
         "serializeJson: non-finite number is not serializable JSON");
  if (v == 0.0) {
    out += "0";  // -0.0 serializes as "0" (documented)
    return;
  }
  char buf[32];
  for (int precision = 1; precision <= 17; ++precision) {
    const int n = std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) continue;
    if (std::strtod(buf, nullptr) == v) {
      out.append(buf, static_cast<std::size_t>(n));
      return;
    }
  }
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  out.append(buf, n < 0 ? 0 : static_cast<std::size_t>(n));
}

void appendValue(std::string& out, const JsonValue& v) {
  switch (v.kind()) {
    case JsonKind::Null:
      out += "null";
      break;
    case JsonKind::Bool:
      out += v.asBool() ? "true" : "false";
      break;
    case JsonKind::Number:
      appendNumber(out, v.asNumber());
      break;
    case JsonKind::String:
      appendEscapedString(out, v.asString());
      break;
    case JsonKind::Array: {
      out.push_back('[');
      const std::vector<JsonValue>& elements = v.asArray();
      for (std::size_t i = 0; i < elements.size(); ++i) {
        if (i != 0) out.push_back(',');
        appendValue(out, elements[i]);
      }
      out.push_back(']');
      break;
    }
    case JsonKind::Object: {
      out.push_back('{');
      const std::vector<std::pair<std::string, JsonValue>>& members =
          v.asObject();
      for (std::size_t i = 0; i < members.size(); ++i) {
        if (i != 0) out.push_back(',');
        appendEscapedString(out, members[i].first);
        out.push_back(':');
        appendValue(out, members[i].second);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// JsonValue member implementations
// ---------------------------------------------------------------------------
void JsonValue::clearPayload() noexcept {
  str_ = std::string();
  arr_ = std::vector<JsonValue>();
  members_ = std::vector<std::pair<std::string, JsonValue>>();
}

JsonValue::JsonValue(const JsonValue& other) noexcept
    : kind_(other.kind_),
      boolean_(other.boolean_),
      number_(other.number_),
      str_(other.str_),
      arr_(other.arr_),
      members_(other.members_) {}

JsonValue& JsonValue::operator=(const JsonValue& other) noexcept {
  if (this != &other) {
    clearPayload();  // release the old kind's payload before the kind change
    kind_ = other.kind_;
    boolean_ = other.boolean_;
    number_ = other.number_;
    str_ = other.str_;
    arr_ = other.arr_;
    members_ = other.members_;
  }
  return *this;
}

JsonValue::JsonValue(JsonValue&& other) noexcept
    : kind_(other.kind_),
      boolean_(other.boolean_),
      number_(other.number_),
      str_(std::move(other.str_)),
      arr_(std::move(other.arr_)),
      members_(std::move(other.members_)) {
  // Moved-from value is Null (preamble); the moved payloads are already
  // empty (std::string/vector move leaves the source empty).
  other.kind_ = JsonKind::Null;
  other.boolean_ = false;
  other.number_ = 0.0;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
  if (this != &other) {
    clearPayload();
    kind_ = other.kind_;
    boolean_ = other.boolean_;
    number_ = other.number_;
    str_ = std::move(other.str_);
    arr_ = std::move(other.arr_);
    members_ = std::move(other.members_);
    other.kind_ = JsonKind::Null;
    other.boolean_ = false;
    other.number_ = 0.0;
  }
  return *this;
}

JsonValue JsonValue::fromBool(bool value) noexcept {
  JsonValue v;
  v.kind_ = JsonKind::Bool;
  v.boolean_ = value;
  return v;
}

JsonValue JsonValue::fromNumber(double value) noexcept {
  JsonValue v;
  v.kind_ = JsonKind::Number;
  v.number_ = value;
  return v;
}

JsonValue JsonValue::fromString(std::string_view value) noexcept {
  JsonValue v;
  v.kind_ = JsonKind::String;
  v.str_ = std::string(value);
  // Precondition (header): valid UTF-8; debug assert, documented UB in
  // release.
  assert(isValidUtf8(v.str_) &&
         "JsonValue::fromString: string is not valid UTF-8");
  return v;
}

JsonValue JsonValue::makeArray() noexcept {
  JsonValue v;
  v.kind_ = JsonKind::Array;
  return v;
}

JsonValue JsonValue::makeObject() noexcept {
  JsonValue v;
  v.kind_ = JsonKind::Object;
  return v;
}

bool JsonValue::asBool() const noexcept {
  assert(kind_ == JsonKind::Bool && "asBool() called on a non-bool value");
  return boolean_;
}

double JsonValue::asNumber() const noexcept {
  assert(kind_ == JsonKind::Number &&
         "asNumber() called on a non-number value");
  return number_;
}

std::string_view JsonValue::asString() const noexcept {
  assert(kind_ == JsonKind::String &&
         "asString() called on a non-string value");
  return std::string_view(str_);
}

const std::vector<JsonValue>& JsonValue::asArray() const noexcept {
  assert(kind_ == JsonKind::Array && "asArray() called on a non-array value");
  return arr_;
}

const std::vector<std::pair<std::string, JsonValue>>& JsonValue::asObject()
    const noexcept {
  assert(kind_ == JsonKind::Object &&
         "asObject() called on a non-object value");
  return members_;
}

const JsonValue* JsonValue::findMember(std::string_view name) const noexcept {
  if (kind_ != JsonKind::Object) return nullptr;
  for (const auto& [key, value] : members_) {
    if (key == name) return &value;
  }
  return nullptr;
}

bool JsonValue::hasMember(std::string_view name) const noexcept {
  return findMember(name) != nullptr;
}

void JsonValue::setNull() noexcept {
  clearPayload();
  kind_ = JsonKind::Null;
  boolean_ = false;
  number_ = 0.0;
}

void JsonValue::setBool(bool value) noexcept {
  clearPayload();
  kind_ = JsonKind::Bool;
  boolean_ = value;
}

void JsonValue::setNumber(double value) noexcept {
  clearPayload();
  kind_ = JsonKind::Number;
  number_ = value;
}

void JsonValue::setString(std::string_view value) noexcept {
  clearPayload();
  kind_ = JsonKind::String;
  str_ = std::string(value);
  // Precondition (header): valid UTF-8; debug assert, documented UB in
  // release.
  assert(isValidUtf8(str_) && "JsonValue::setString: not valid UTF-8");
}

void JsonValue::append(JsonValue element) {
  assert(kind_ == JsonKind::Array &&
         "append() called on a non-array value");
  arr_.push_back(std::move(element));
}

void JsonValue::setMember(std::string_view name, JsonValue value) {
  assert(kind_ == JsonKind::Object &&
         "setMember() called on a non-object value");
  for (auto& [key, memberValue] : members_) {
    if (key == name) {
      memberValue = std::move(value);  // replace in place
      return;
    }
  }
  members_.emplace_back(std::string(name), std::move(value));
}

bool JsonValue::operator==(const JsonValue& other) const noexcept {
  if (kind_ != other.kind_) return false;
  switch (kind_) {
    case JsonKind::Null:
      return true;
    case JsonKind::Bool:
      return boolean_ == other.boolean_;
    case JsonKind::Number:
      return number_ == other.number_;  // NaN != NaN (IEEE 754)
    case JsonKind::String:
      return str_ == other.str_;
    case JsonKind::Array:
      return arr_ == other.arr_;
    case JsonKind::Object: {
      if (members_.size() != other.members_.size()) return false;
      for (const auto& [key, value] : members_) {
        const JsonValue* otherValue = other.findMember(key);
        if (otherValue == nullptr || *otherValue != value) return false;
      }
      return true;
    }
  }
  return false;  // unreachable: every JsonKind is handled above
}

// ---------------------------------------------------------------------------
// parseJson / serializeJson
// ---------------------------------------------------------------------------
Result<JsonValue> parseJson(std::string_view input, JsonOptions options) {
  if (input.size() > options.maxDocumentBytes) {
    return Result<JsonValue>::failure(ErrorCode::MalformedInput);
  }
  ParserState p;
  p.in = input;
  p.maxDepth = options.maxDepth;
  JsonValue out;
  if (!parseValue(p, out)) {
    return Result<JsonValue>::failure(ErrorCode::MalformedInput);
  }
  skipWhitespace(p);
  if (p.pos != p.in.size()) {
    return Result<JsonValue>::failure(ErrorCode::MalformedInput);  // trailing
  }
  return Result<JsonValue>::success(std::move(out));
}

std::string serializeJson(const JsonValue& value) {
  std::string out;
  out.reserve(128);
  appendValue(out, value);
  return out;
}

}  // namespace laige
