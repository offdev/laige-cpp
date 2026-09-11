// laige-core bounded JSON parser + serializer suite (M0-CORE-07).
//
// Step Verify scope (roadmap/M0-foundations.md):
//   - `ctest -R config_json` green (suites: ConfigJsonValid,
//     ConfigJsonInvalid, ConfigJsonRoundTrip, ConfigJsonValue,
//     ConfigJsonOptions)
//   - `laige-fuzz json_parse --runs=1000` clean under ASan — the
//     `fuzz_json_parse` CTest entry (tools/fuzz), which the ASan tree
//     runs instrumented.
//
// Coverage per the step: the valid / invalid / malformed corpus —
// nested depth limits, huge numbers, truncated input, encoding edge
// cases (strict UTF-8, surrogate pairs, control characters, escapes) —
// plus serializer round-trip, canonical forms, and JsonValue value
// semantics. Non-ASCII and control bytes are built explicitly with the
// Raw() helper: narrow-literal \x escapes greedily consume hex digits
// and are range-checked against `char` (both rejected under -Wall), and
// explicit bytes keep the test platform-portable (the corpus in
// tools/fuzz follows the same convention).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/json.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "config_json_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "config_json_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "config_json_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

namespace {

using laige::ErrorCode;
using laige::JsonOptions;
using laige::JsonValue;
using laige::Result;

Result<JsonValue> Parse(std::string_view text,
                        const JsonOptions& options = {}) {
  return laige::parseJson(text, options);
}

// Every parse failure must be MalformedInput: the registry entry for
// code 3 names the ADR 0003 size/depth bounds, so no other code is
// reachable from parseJson (CORE-004).
void ExpectMalformed(std::string_view text, const JsonOptions& options = {}) {
  const Result<JsonValue> r = Parse(text, options);
  EXPECT_TRUE(r.isError()) << "expected a parse failure for: " << text;
  if (r.isError()) {
    EXPECT_EQ(ErrorCode::MalformedInput, r.error()) << "for: " << text;
  }
}

// `depth` nested arrays around a null leaf ("[[[null]]]" at depth 3).
std::string NestedArray(int depth) {
  std::string s(static_cast<std::size_t>(depth), '[');
  s += "null";
  s.append(static_cast<std::size_t>(depth), ']');
  return s;
}

// Explicit raw bytes (see the file header for why \x escapes are not
// used for these).
std::string Raw(std::initializer_list<std::uint16_t> bytes) {
  std::string s;
  for (const std::uint16_t b : bytes) s.push_back(static_cast<char>(b));
  return s;
}

constexpr std::size_t kMiB = static_cast<std::size_t>(1u) << 20;

}  // namespace

// ---------------------------------------------------------------------------
// ConfigJsonValid — the parser accepts exactly the documented grammar
// ---------------------------------------------------------------------------

TEST(ConfigJsonValid, Scalars) {
  {
    const auto r = Parse("null");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().isNull());
  }
  {
    const auto r = Parse("true");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().isBool());
    EXPECT_TRUE(r.value().asBool());
  }
  {
    const auto r = Parse("false");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().asBool() == false);
  }
  {
    const auto r = Parse("42");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(42.0, r.value().asNumber());
  }
  {
    const auto r = Parse("-0");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(-0.0, r.value().asNumber());
  }
  {
    const auto r = Parse("1.5");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(1.5, r.value().asNumber());
  }
  {
    const auto r = Parse("-2.5e-7");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(-2.5e-7, r.value().asNumber());
  }
  {
    const auto r = Parse("1E+3");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(1000.0, r.value().asNumber());
  }
}

TEST(ConfigJsonValid, HugeNumbers) {
  // DBL_MAX / denormal minimum: exact boundary values (KAT).
  {
    const auto r = Parse("1.7976931348623157e308");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::numeric_limits<double>::max(), r.value().asNumber());
  }
  {
    const auto r = Parse("5e-324");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::numeric_limits<double>::denorm_min(), r.value().asNumber());
  }
  // Overflow: a well-formed token that does not fit a double stores
  // +/-inf (documented IEEE 754 behavior, json.h preamble).
  {
    const auto r = Parse("1e999");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::numeric_limits<double>::infinity(), r.value().asNumber());
  }
  {
    const auto r = Parse("-1e999999999999999999999999");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(-std::numeric_limits<double>::infinity(),
              r.value().asNumber());
  }
  // Integer exactness policy (json.h): beyond +/-2^53 integer literals
  // lose low-order bits; 2^53 + 1 stores 2^53 (round to even).
  {
    const auto r = Parse("9007199254740993");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(static_cast<double>(1ull << 53), r.value().asNumber());
  }
}

TEST(ConfigJsonValid, Strings) {
  {
    const auto r = Parse("\"\"");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().asString().empty());
  }
  {
    const auto r = Parse("\"hello world\"");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ("hello world", r.value().asString());
  }
  // The two-character escapes plus \uXXXX for quote, backslash, and
  // slash.
  {
    const auto r = Parse(R"("a\"b\\c\/d")");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ("a\"b\\c/d", r.value().asString());
  }
  {
    const auto r = Parse(R"("Ae\u00e9")");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(Raw({0x41, 0x65, 0xC3, 0xA9}), r.value().asString());
  }
  // Surrogate pair -> codepoint above U+FFFF.
  {
    const auto r = Parse(R"("\ud83d\ude00")");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(Raw({0xF0, 0x9F, 0x98, 0x80}), r.value().asString());
  }
  // Escaped control characters are stored as-is.
  {
    const auto r = Parse(R"("a\u0000b\u001fc")");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(Raw({0x61, 0x00, 0x62, 0x1F, 0x63}), r.value().asString());
  }
  // Raw UTF-8 bytes in strings (validated strictly).
  {
    const std::string hello =
        Raw({0x68, 0xC3, 0xA9, 0x6C, 0x6C, 0x6F, 0x20, 0xF0, 0x9F, 0x8E, 0xAE});
    const auto r = Parse("\"" + hello + "\"");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::string_view(hello), r.value().asString());
  }
  // DEL (U+007F) is not a control character: allowed raw.
  {
    const auto r = Parse("\"" + Raw({0x61, 0x7F, 0x62}) + "\"");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::string_view(Raw({0x61, 0x7F, 0x62})), r.value().asString());
  }
  // Whitespace inside a string key.
  {
    const auto r = Parse("{\"  \": 1}");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().hasMember("  "));
  }
}

TEST(ConfigJsonValid, Containers) {
  {
    const auto r = Parse("[]");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().isArray());
    EXPECT_TRUE(r.value().asArray().empty());
  }
  {
    const auto r = Parse("{}");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().isObject());
    EXPECT_TRUE(r.value().asObject().empty());
  }
  {
    const auto r = Parse("[1,2,3]");
    ASSERT_TRUE(r.ok());
    const auto& a = r.value().asArray();
    ASSERT_EQ(3u, a.size());
    EXPECT_EQ(1.0, a[0].asNumber());
    EXPECT_EQ(3.0, a[2].asNumber());
  }
  {
    const auto r = Parse(R"({"a":1,"b":{"c":[true,null]}})");
    ASSERT_TRUE(r.ok());
    const JsonValue* b = r.value().findMember("b");
    ASSERT_NE(nullptr, b);
    const JsonValue* c = b->findMember("c");
    ASSERT_NE(nullptr, c);
    ASSERT_EQ(2u, c->asArray().size());
    EXPECT_TRUE(c->asArray()[0].asBool());
    EXPECT_TRUE(c->asArray()[1].isNull());
  }
  // Whitespace between every token; document order is preserved.
  {
    const auto r = Parse(" \t\r\n [ 1 , 2 ] \r\n ");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(2u, r.value().asArray().size());
  }
  {
    const auto r = Parse(R"({"a":1,"b":2,"c":3})");
    ASSERT_TRUE(r.ok());
    const auto& members = r.value().asObject();
    ASSERT_EQ(3u, members.size());
    EXPECT_EQ("a", members[0].first);
    EXPECT_EQ("b", members[1].first);
    EXPECT_EQ("c", members[2].first);
  }
}

// M0-CORE-08 regression: whitespace after the ',' of an object member
// must be accepted (the grammar allows whitespace between tokens). The
// object key goes through parseString directly (not parseValue, which
// does the skipping), so the key needs its own whitespace skip — the old
// parseObjectMembers rejected {"a": 1, "b": 2}. Found when the
// hand-formatted repo-root budgets.json was rejected (M0-CORE-08).
TEST(ConfigJsonValid, ObjectMemberWhitespace) {
  {
    const auto r = Parse(R"({"a": 1, "b": 2})");
    ASSERT_TRUE(r.ok());
    const JsonValue* a = r.value().findMember("a");
    ASSERT_NE(nullptr, a);
    EXPECT_EQ(1.0, a->asNumber());
    const JsonValue* b = r.value().findMember("b");
    ASSERT_NE(nullptr, b);
    EXPECT_EQ(2.0, b->asNumber());
  }
  // Hand-formatted (multi-line) document — the shape of budgets.json:
  // newlines + indentation between members.
  {
    const auto r = Parse("{\n  \"version\": 1,\n  \"budgets\": []\n}");
    ASSERT_TRUE(r.ok());
    const JsonValue* v = r.value().findMember("version");
    ASSERT_NE(nullptr, v);
    EXPECT_EQ(1.0, v->asNumber());
    const JsonValue* budgets = r.value().findMember("budgets");
    ASSERT_NE(nullptr, budgets);
    EXPECT_TRUE(budgets->isArray());
    EXPECT_TRUE(budgets->asArray().empty());
  }
  // Whitespace before the closing brace after the last member.
  {
    const auto r = Parse(R"({"a": 1 })");
    ASSERT_TRUE(r.ok());
    const JsonValue* a = r.value().findMember("a");
    ASSERT_NE(nullptr, a);
    EXPECT_EQ(1.0, a->asNumber());
  }
}

TEST(ConfigJsonValid, DepthAtLimit) {
  // Default maxDepth is 32: exactly 32 nested containers parse, and the
  // walk reaches the null leaf at level 32.
  const auto r = Parse(NestedArray(32));
  ASSERT_TRUE(r.ok());
  const JsonValue* node = &r.value();
  for (int i = 0; i < 32; ++i) {
    ASSERT_TRUE(node->isArray()) << "level " << i;
    ASSERT_EQ(1u, node->asArray().size());
    node = &node->asArray()[0];
  }
  EXPECT_TRUE(node->isNull());
}

TEST(ConfigJsonValid, SizeAtLimit) {
  // A document of exactly kMiB bytes parses (the bound is inclusive).
  std::string doc;
  doc.reserve(kMiB);
  doc.push_back('"');
  doc.append(kMiB - 2, 'a');
  doc.push_back('"');
  ASSERT_EQ(kMiB, doc.size());
  const auto r = Parse(doc);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(kMiB - 2, r.value().asString().size());
}

// ---------------------------------------------------------------------------
// ConfigJsonInvalid — the malformed corpus (all MalformedInput)
// ---------------------------------------------------------------------------

TEST(ConfigJsonInvalid, EmptyAndTrailing) {
  ExpectMalformed("");
  ExpectMalformed(" \t\r\n");
  ExpectMalformed("1 2");
  ExpectMalformed("[] x");
  ExpectMalformed("1]");
  ExpectMalformed("\"a\"b");
  ExpectMalformed("tru");
  ExpectMalformed("fals");
  ExpectMalformed("nul");
  ExpectMalformed("truex");
  ExpectMalformed("1e");
  ExpectMalformed("-1.");
  ExpectMalformed(Raw({0xFF}));  // bare invalid byte outside a string
}

TEST(ConfigJsonInvalid, Truncated) {
  ExpectMalformed("[");
  ExpectMalformed("[1");
  ExpectMalformed("[1,");
  ExpectMalformed("{");
  ExpectMalformed("{\"a\"");
  ExpectMalformed("{\"a\":");
  ExpectMalformed("\"abc");
  ExpectMalformed("[[");
  ExpectMalformed("1.5e");
  ExpectMalformed("-1e+");
  ExpectMalformed("01");
  ExpectMalformed("-");
}

TEST(ConfigJsonInvalid, BadNumbers) {
  ExpectMalformed("+1");
  ExpectMalformed(".5");
  ExpectMalformed("1.");
  ExpectMalformed("1e");
  ExpectMalformed("1e+");
  ExpectMalformed("1e-");
  ExpectMalformed("1.2.3");
  ExpectMalformed("0x1");
  ExpectMalformed("--1");
  ExpectMalformed("1 2");
  ExpectMalformed("1e5.5");
  ExpectMalformed("1.5e5e5");
}

TEST(ConfigJsonInvalid, BadEscapes) {
  ExpectMalformed(R"("a\z")");
  ExpectMalformed(R"("a\")");
  ExpectMalformed(R"("a\u12")");       // fewer than 4 hex digits
  ExpectMalformed(R"("a\uD800")");     // lone high surrogate
  ExpectMalformed(R"("a\uDC00")");     // lone low surrogate
  ExpectMalformed(R"("\ud83d")");      // high surrogate at end of string
  ExpectMalformed(R"("\ud83d\u0041")");  // high not followed by a low
}

TEST(ConfigJsonInvalid, ControlAndEncoding) {
  // Raw control characters are rejected inside strings.
  ExpectMalformed("\"" + Raw({0x61, 0x01, 0x62}) + "\"");
  ExpectMalformed("\"" + Raw({0x61, 0x1F, 0x62}) + "\"");
  ExpectMalformed("\"a\nb\"");
  // Strict UTF-8: overlong, surrogate, out-of-range, and truncated.
  ExpectMalformed("\"" + Raw({0xFF}) + "\"");
  ExpectMalformed("\"" + Raw({0xC0, 0x80}) + "\"");              // overlong NUL
  ExpectMalformed("\"" + Raw({0xC1, 0xBF}) + "\"");              // overlong
  ExpectMalformed("\"" + Raw({0xE0, 0x9F, 0xBF}) + "\"");        // overlong U+007F
  ExpectMalformed("\"" + Raw({0xED, 0xA0, 0x80}) + "\"");        // raw surrogate
  ExpectMalformed("\"" + Raw({0xF5, 0x80, 0x80, 0x80}) + "\"");  // above U+10FFFF
  ExpectMalformed("\"" + Raw({0xF0, 0x80, 0x80, 0x80}) + "\"");  // overlong NUL
  ExpectMalformed("\"" + Raw({0x61, 0x62, 0x63, 0xE2, 0x82}) +
                  "\"");  // truncated sequence at end
  ExpectMalformed("\"" + Raw({0x80}) + "\"");  // lone continuation byte
}

TEST(ConfigJsonInvalid, DuplicateKeys) {
  ExpectMalformed(R"({"a":1,"a":2})");
  ExpectMalformed(R"({"a":{"b":1},"a":{"b":2}})");  // top-level duplicate
}

TEST(ConfigJsonInvalid, DepthOverLimit) {
  ExpectMalformed(NestedArray(33));  // 33 nested containers > default 32
}

TEST(ConfigJsonInvalid, SizeOverLimit) {
  // kMiB + 2 bytes: over the inclusive document-size bound.
  std::string doc;
  doc.reserve(kMiB + 2);
  doc.push_back('"');
  doc.append(kMiB, 'a');
  doc.push_back('"');
  ASSERT_EQ(kMiB + 2, doc.size());
  ExpectMalformed(doc);
}

// ---------------------------------------------------------------------------
// ConfigJsonRoundTrip — parse -> serialize -> parse is stable
// ---------------------------------------------------------------------------

TEST(ConfigJsonRoundTrip, ValueRoundTrip) {
  // Note: documents whose numbers overflow to +/-inf (e.g. "1e999")
  // parse but are deliberately excluded here — serializing a non-finite
  // number is a documented precondition violation, not a supported
  // form. Every other parsed document round-trips.
  const std::string deepNested = NestedArray(32);
  const std::string utf8Doc = "\"" +
      Raw({0x68, 0xC3, 0xA9, 0x6C, 0x6C, 0x6F, 0x20, 0xF0, 0x9F, 0x8E, 0xAE}) +
      "\"";
  const std::vector<std::string> roundTripCorpus = {
      "null",
      "true",
      "false",
      "0",
      "-0",
      "1.5",
      "-2.5e-7",
      utf8Doc,
      R"("a\u0001b")",
      "[1,[2,3],{\"k\":\"v\"}]",
      R"({"a":1,"b":[true,null],"c":"x","d":[]})",
      deepNested,
  };
  for (const std::string& text : roundTripCorpus) {
    const auto r1 = Parse(text);
    ASSERT_TRUE(r1.ok()) << text;
    const std::string s1 = laige::serializeJson(r1.value());
    const auto r2 = Parse(s1);
    ASSERT_TRUE(r2.ok()) << "re-parse of: " << s1;
    EXPECT_TRUE(r1.value() == r2.value()) << "round trip of: " << text;
    const std::string s2 = laige::serializeJson(r2.value());
    EXPECT_EQ(s1, s2) << "serializer idempotence for: " << text;
  }
}

TEST(ConfigJsonRoundTrip, CanonicalForms) {
  const auto r = Parse(R"({"a":1,"b":[true,null],"c":"x"})");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ("{\"a\":1,\"b\":[true,null],\"c\":\"x\"}",
            laige::serializeJson(r.value()));

  // Numbers: shortest round-trip decimal; -0.0 serializes as "0".
  EXPECT_EQ("1.5", laige::serializeJson(JsonValue::fromNumber(1.5)));
  EXPECT_EQ("2", laige::serializeJson(JsonValue::fromNumber(2.0)));
  EXPECT_EQ("0", laige::serializeJson(JsonValue::fromNumber(-0.0)));
  EXPECT_EQ("1e+21", laige::serializeJson(JsonValue::fromNumber(1e21)));
  EXPECT_EQ("1e-07", laige::serializeJson(JsonValue::fromNumber(1e-7)));
  EXPECT_EQ("1.1", laige::serializeJson(JsonValue::fromNumber(1.1)));
  EXPECT_EQ("123456.75",
            laige::serializeJson(JsonValue::fromNumber(123456.75)));
  EXPECT_EQ("0.1", laige::serializeJson(JsonValue::fromNumber(0.1)));

  // Strings: ASCII-safe canonical escaping.
  EXPECT_EQ("\"h\\u00e9llo \\ud83c\\udfae\"", laige::serializeJson(
              JsonValue::fromString(
                  Raw({0x68, 0xC3, 0xA9, 0x6C, 0x6C, 0x6F, 0x20,
                       0xF0, 0x9F, 0x8E, 0xAE}))));
  EXPECT_EQ("\"a\\u0001b\"",
            laige::serializeJson(JsonValue::fromString(Raw({0x61, 0x01, 0x62}))));
  EXPECT_EQ("\"q\\\"u\\\\ote\"",
            laige::serializeJson(JsonValue::fromString("q\"u\\ote")));
  EXPECT_EQ("\"/\"", laige::serializeJson(JsonValue::fromString("/")));
}

// ---------------------------------------------------------------------------
// ConfigJsonValue — the value type's mechanics
// ---------------------------------------------------------------------------

TEST(ConfigJsonValue, FactoriesAndKinds) {
  EXPECT_TRUE(JsonValue().isNull());
  EXPECT_TRUE(JsonValue::fromBool(true).isBool());
  EXPECT_TRUE(JsonValue::fromNumber(1.0).isNumber());
  EXPECT_TRUE(JsonValue::fromString("x").isString());
  EXPECT_TRUE(JsonValue::makeArray().isArray());
  EXPECT_TRUE(JsonValue::makeObject().isObject());
}

TEST(ConfigJsonValue, CopyIsDeep) {
  const auto r = Parse(R"({"k":[1,2]})");
  ASSERT_TRUE(r.ok());
  JsonValue doc = r.value();
  JsonValue copy = doc;
  doc.setMember("k", JsonValue::fromNumber(99.0));
  EXPECT_EQ(99.0, doc.findMember("k")->asNumber());
  const JsonValue* copiedKey = copy.findMember("k");
  ASSERT_NE(nullptr, copiedKey);
  EXPECT_TRUE(copiedKey->isArray());
  EXPECT_EQ(2u, copiedKey->asArray().size());
}

TEST(ConfigJsonValue, MoveSemantics) {
  const auto r = Parse("[1,2,3]");
  ASSERT_TRUE(r.ok());
  JsonValue source = r.value();
  JsonValue moved(std::move(source));
  EXPECT_TRUE(source.isNull());  // moved-from value is Null
  ASSERT_EQ(3u, moved.asArray().size());

  JsonValue target;
  target = std::move(moved);
  EXPECT_TRUE(moved.isNull());
  ASSERT_EQ(3u, target.asArray().size());
  EXPECT_EQ(3.0, target.asArray()[2].asNumber());
}

TEST(ConfigJsonValue, Mutation) {
  JsonValue obj = JsonValue::makeObject();
  obj.setMember("a", JsonValue::fromNumber(1.0));
  obj.setMember("b", JsonValue::fromBool(true));
  obj.setMember("a", JsonValue::fromNumber(2.0));  // replace in place
  ASSERT_EQ(2u, obj.asObject().size());
  EXPECT_EQ(2.0, obj.findMember("a")->asNumber());
  EXPECT_EQ("a", obj.asObject().front().first);  // position preserved

  JsonValue arr = JsonValue::makeArray();
  arr.append(JsonValue::fromNumber(1.0));
  arr.append(JsonValue::fromString("x"));
  ASSERT_EQ(2u, arr.asArray().size());
  EXPECT_EQ("x", arr.asArray()[1].asString());

  // Kind transitions release the old payload (the churn test below plus
  // the ASan tree verify this).
  JsonValue v = JsonValue::makeArray();
  v.append(JsonValue::fromNumber(1.0));
  v.setString("s");
  EXPECT_TRUE(v.isString());
  v.setNull();
  EXPECT_TRUE(v.isNull());
}

TEST(ConfigJsonValue, Equality) {
  EXPECT_TRUE(JsonValue() == JsonValue());
  EXPECT_TRUE(JsonValue::fromNumber(1.0) == JsonValue::fromNumber(1.0));
  EXPECT_TRUE(JsonValue::fromNumber(1.0) != JsonValue::fromNumber(2.0));
  // NaN is unequal to itself (IEEE 754 ==).
  const double nanValue = std::nan("");
  EXPECT_TRUE(JsonValue::fromNumber(nanValue) !=
              JsonValue::fromNumber(nanValue));

  // Objects compare independent of member order.
  JsonValue a = JsonValue::makeObject();
  a.setMember("x", JsonValue::fromNumber(1.0));
  a.setMember("y", JsonValue::fromNumber(2.0));
  JsonValue b = JsonValue::makeObject();
  b.setMember("y", JsonValue::fromNumber(2.0));
  b.setMember("x", JsonValue::fromNumber(1.0));
  EXPECT_TRUE(a == b);

  // Arrays are order-sensitive; kinds must match.
  EXPECT_TRUE(Parse("[1,2]").value() != Parse("[2,1]").value());
  EXPECT_TRUE(Parse("{\"a\":1}").value() == Parse("{\"a\":1}").value());
  EXPECT_TRUE(JsonValue::fromBool(true) != JsonValue::fromNumber(1.0));
}

TEST(ConfigJsonValue, FindMemberTotal) {
  // Total on any value: non-objects report absence, not an error.
  JsonValue arr = JsonValue::makeArray();
  EXPECT_TRUE(arr.findMember("a") == nullptr);
  EXPECT_FALSE(arr.hasMember("a"));

  JsonValue obj = JsonValue::makeObject();
  obj.setMember("a", JsonValue::fromNumber(1.0));
  EXPECT_TRUE(obj.findMember("a") != nullptr);
  EXPECT_TRUE(obj.findMember("b") == nullptr);
  EXPECT_FALSE(obj.hasMember("b"));
}

TEST(ConfigJsonValue, ChurnNoLeak) {
  // Construct/destroy churn: the ASan/LSan trees prove the leak-freeness;
  // on the plain tree this exercises the copy/move/dtor paths.
  for (int i = 0; i < 10000; ++i) {
    JsonValue v = JsonValue::makeObject();
    v.setMember("k", JsonValue::fromString("value"));
    v.setMember("n", JsonValue::fromNumber(static_cast<double>(i)));
    JsonValue copy = v;
    (void)copy;
  }
  SUCCEED();
}

// ---------------------------------------------------------------------------
// ConfigJsonOptions — the documented bounds are engine-configurable
// ---------------------------------------------------------------------------

TEST(ConfigJsonOptions, DepthLimit) {
  JsonOptions opts;
  opts.maxDepth = 1;
  EXPECT_TRUE(Parse("[1]", opts).ok());
  ExpectMalformed("{{}}", opts);  // inner object would be depth 2

  opts.maxDepth = 2;
  EXPECT_TRUE(Parse("[[1]]", opts).ok());
  ExpectMalformed("[[[1]]]", opts);  // depth 3
}

TEST(ConfigJsonOptions, SizeLimit) {
  JsonOptions opts;
  opts.maxDocumentBytes = 10;
  EXPECT_TRUE(Parse("[1,2,3,45]", opts).ok());  // exactly 10 bytes
  ExpectMalformed("[1,2,3,4,5]", opts);         // 11 bytes
  EXPECT_TRUE(Parse("1", opts).ok());
  ExpectMalformed("1 2", opts);                 // trailing data

  JsonOptions zero;
  zero.maxDocumentBytes = 0;
  ExpectMalformed("1", zero);  // any non-empty document
}
