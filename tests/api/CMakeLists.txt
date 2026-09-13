# laige-api manifest tests (M0-TOOL-01)
#
# tools/api/laige-api-scanner is a C++ tool, so it gets a CTest suite of
# its own: the scanner runs against synthetic fixture trees (generated
# here at configure time) and against the real repository tree:
#
#   api-fixture-scan        the fixture tree            -> exact manifest
#   api-check-fresh         --check on a current copy   -> exit 0
#   api-check-stale         --check on a stale copy     -> exit 1 + diff
#   api-unsupported-construct  a block comment         -> exit 2
#   api-root-error          a nonexistent root          -> exit 2
#   api-real-tree           the live repository         -> exit 0 (drift)
#
# The real-tree test is the same check the CI drift job runs, so a public
# header change that misses the manifest lands red in every P0 job.
#
# Each test is a generated `cmake -P` check script (see below) asserting
# the exit code, required output fragments, and — for the scan test — the
# exact manifest bytes (tests/api/expected-fixture-api.json). The fixture
# and its expected manifest are a contract pair: a scanner change that
# alters either one MUST update both in the same change (CORE-007).

set(FX "${CMAKE_BINARY_DIR}/api-fixtures")
file(REMOVE_RECURSE "${FX}")

# --- fixture: one header exercising every manifest rule -------------------
# detail-namespace exclusion, private-member exclusion, @budget and
# @experimental tags, multi-paragraph docs (first paragraph wins),
# enumerators with/without values, alias, function-like macro, and
# constructor / destructor / method classification.
file(MAKE_DIRECTORY "${FX}/api/src/laige-core/include/laige")
file(WRITE "${FX}/api/src/laige-core/include/laige/fixt.h" "#pragma once

#include <cstdint>

namespace laige {

namespace detail {
struct Hidden {
  int x;
};
}  // namespace detail

// A public function with a budget tag.
// @budget 20 us
[[nodiscard]] int fnA() noexcept;

// Tag two.
// @experimental
//
// Second paragraph ignored.
int fnB();

enum class E : std::uint8_t {
  A = 0,
  B,
};

struct S {
  int pub;
 private:
  int sec;
 public:
  int pub2;
};

// Alias doc.
using AliasT = S;

#define FIXT_MACRO(x) ((x) + 1)

class C {
 public:
  C();
  ~C();
  int m();
};

}  // namespace laige
")

# --- expected manifest for the fixture (exact serializer bytes) ------------
# Must stay in lockstep with the fixture above and with the scanner's
# serializer (see serializeManifest in tools/api/laige-api.cpp).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/expected-fixture-api.json" "{
  \"version\": 1,
  \"generatedBy\": \"laige-api\",
  \"headers\": [
  \"src/laige-core/include/laige/fixt.h\"
  ],
  \"symbols\": [
    {\"name\": \"laige::fnA\", \"kind\": \"function\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 15, \"signature\": \"[[nodiscard]] int fnA() noexcept\", \"summary\": \"A public function with a budget tag.\", \"budget\": \"20 us\", \"experimental\": false},
    {\"name\": \"laige::fnB\", \"kind\": \"function\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 21, \"signature\": \"int fnB()\", \"summary\": \"Tag two.\", \"budget\": null, \"experimental\": true},
    {\"name\": \"laige::E\", \"kind\": \"enum\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 23, \"signature\": \"enum class E : std::uint8_t\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::E::A\", \"kind\": \"enumerator\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 24, \"signature\": \"A = 0\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::E::B\", \"kind\": \"enumerator\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 25, \"signature\": \"B\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::S\", \"kind\": \"struct\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 28, \"signature\": \"struct S\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::S::pub\", \"kind\": \"variable\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 29, \"signature\": \"int pub\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::S::pub2\", \"kind\": \"variable\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 33, \"signature\": \"int pub2\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::AliasT\", \"kind\": \"alias\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 37, \"signature\": \"using AliasT = S\", \"summary\": \"Alias doc.\", \"budget\": null, \"experimental\": false},
    {\"name\": \"FIXT_MACRO\", \"kind\": \"macro\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 39, \"signature\": \"#define FIXT_MACRO(x)\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C\", \"kind\": \"class\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 41, \"signature\": \"class C\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C::C\", \"kind\": \"constructor\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 43, \"signature\": \"C()\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C::~C\", \"kind\": \"destructor\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 44, \"signature\": \"~C()\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C::m\", \"kind\": \"method\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 45, \"signature\": \"int m()\", \"summary\": null, \"budget\": null, \"experimental\": false}
  ]
}
")

# --- stale manifest for the fixture (two deliberate differences) -----------
# Same manifest as the expected one, minus laige::E::B, with laige::S's
# summary tampered: --check must report exactly one addition and one
# change (the S entry keeps its name/header/line, so it is a change, not
# a removal).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stale-fixture-api.json" "{
  \"version\": 1,
  \"generatedBy\": \"laige-api\",
  \"headers\": [
  \"src/laige-core/include/laige/fixt.h\"
  ],
  \"symbols\": [
    {\"name\": \"laige::fnA\", \"kind\": \"function\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 15, \"signature\": \"[[nodiscard]] int fnA() noexcept\", \"summary\": \"A public function with a budget tag.\", \"budget\": \"20 us\", \"experimental\": false},
    {\"name\": \"laige::fnB\", \"kind\": \"function\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 21, \"signature\": \"int fnB()\", \"summary\": \"Tag two.\", \"budget\": null, \"experimental\": true},
    {\"name\": \"laige::E\", \"kind\": \"enum\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 23, \"signature\": \"enum class E : std::uint8_t\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::E::A\", \"kind\": \"enumerator\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 24, \"signature\": \"A = 0\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::S\", \"kind\": \"struct\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 28, \"signature\": \"struct S\", \"summary\": \"tampered summary\", \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::S::pub\", \"kind\": \"variable\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 29, \"signature\": \"int pub\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::S::pub2\", \"kind\": \"variable\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 33, \"signature\": \"int pub2\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::AliasT\", \"kind\": \"alias\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 37, \"signature\": \"using AliasT = S\", \"summary\": \"Alias doc.\", \"budget\": null, \"experimental\": false},
    {\"name\": \"FIXT_MACRO\", \"kind\": \"macro\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 39, \"signature\": \"#define FIXT_MACRO(x)\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C\", \"kind\": \"class\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 41, \"signature\": \"class C\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C::C\", \"kind\": \"constructor\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 43, \"signature\": \"C()\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C::~C\", \"kind\": \"destructor\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 44, \"signature\": \"~C()\", \"summary\": null, \"budget\": null, \"experimental\": false},
    {\"name\": \"laige::C::m\", \"kind\": \"method\", \"header\": \"src/laige-core/include/laige/fixt.h\", \"line\": 45, \"signature\": \"int m()\", \"summary\": null, \"budget\": null, \"experimental\": false}
  ]
}
")

# --- fixture: an unsupported construct the scanner must reject -------------
file(MAKE_DIRECTORY "${FX}/blockcomment/src/laige-core/include/laige")
file(WRITE "${FX}/blockcomment/src/laige-core/include/laige/bad.h" "#pragma once

namespace laige {

/* a block comment: unsupported by the scanner */
struct S {
  int x;
};

}  // namespace laige
")

# --- the tests ---------------------------------------------------------------
# Each test runs a generated `cmake -P` check script
# (expect-api-result.cmake.in) that asserts the scanner's exit code AND
# the required output fragments (and, for the scan test, the exact output
# bytes). CTest properties cannot express this safely: CTest inverts
# PASS_REGULAR_EXPRESSION when WILL_FAIL is set (verified on CMake 4.4.3),
# and a crash would exit non-zero like a scan failure (CORE-008).
function(laige_add_api_test name expect_exit root mode file)
  # Space-separated quoted list (semicolon lists confuse the CMake
  # script lexer in generated files — verified on CMake 4.4.3).
  set(_cmd "\"--root\" \"${root}\" \"--${mode}\" \"${file}\"")
  # The template's @CHECKS@ placeholder is substituted from this variable,
  # so it must be named exactly CHECKS (a misspelled local like _checks
  # silently expands to nothing and the fragment checks never exist).
  set(CHECKS "")
  foreach(_needle IN LISTS ARGN)
    # The generated script is itself a CMake file, and its string literals
    # strip one level of backslashes on parse (\\ -> \, \X -> X), so each
    # regex escape must reach the file DOUBLED: \\( in the file becomes \(
    # for the regex engine (a single \X would be stripped and the bare
    # metacharacter would reach the regex).
    set(_bs2 "\\\\")
    string(REPLACE "\\" "\\\\" _esc "${_needle}")
    string(REPLACE "\"" "\\\"" _esc "${_esc}")
    string(REPLACE "(" "${_bs2}(" _esc "${_esc}")
    string(REPLACE ")" "${_bs2})" _esc "${_esc}")
    string(REPLACE "+" "${_bs2}+" _esc "${_esc}")
    string(REPLACE "." "${_bs2}." _esc "${_esc}")
    string(APPEND CHECKS
      "if(NOT _text MATCHES \"${_esc}\")\n"
      "  string(APPEND _problems \"output missing '<${_esc}'>; \")\n"
      "endif()\n")
  endforeach()
  set(CMD "${_cmd}")
  set(EXPECT_EXIT "${expect_exit}")
  set(OUT_FILE "")
  set(EXPECTED_FILE "")
  if(mode STREQUAL "out")
    set(OUT_FILE "${file}")
  endif()
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/expect-api-result.cmake.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake" @ONLY)
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -P
           "${CMAKE_CURRENT_BINARY_DIR}/${name}.cmake")
  # The scanner path is carried in the test environment, not in the
  # command or baked into the script: $<TARGET_FILE> resolves per
  # configuration under multi-config generators (the Windows VS layout
  # puts the executable in bin/<Config>/), while a configure-time path
  # only matches the single-config layout.
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "LAIGE_API_SCANNER=$<TARGET_FILE:laige-api-scanner>")
endfunction()

# The scan test also compares the written manifest byte-for-byte against
# the expected fixture manifest (the template's EXPECTED_FILE hook).
set(_scan_out "${CMAKE_CURRENT_BINARY_DIR}/fixture-api.json")
set(_scan_root "${FX}/api")
set(_scan_expected "${CMAKE_CURRENT_BINARY_DIR}/expected-fixture-api.json")
set(_cmd "\"--root\" \"${_scan_root}\" \"--out\" \"${_scan_out}\"")
set(CMD "${_cmd}")
set(EXPECT_EXIT "0")
set(OUT_FILE "${_scan_out}")
set(EXPECTED_FILE "${_scan_expected}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/expect-api-result.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/api-fixture-scan.cmake" @ONLY)
add_test(NAME api-fixture-scan COMMAND ${CMAKE_COMMAND} -P
           "${CMAKE_CURRENT_BINARY_DIR}/api-fixture-scan.cmake")
# Same ENVIRONMENT wiring as laige_add_api_test (see its comment).
set_tests_properties(api-fixture-scan PROPERTIES
  ENVIRONMENT "LAIGE_API_SCANNER=$<TARGET_FILE:laige-api-scanner>")

laige_add_api_test(api-check-fresh 0 "${FX}/api" check
  "${CMAKE_CURRENT_BINARY_DIR}/expected-fixture-api.json"
  "laige-api: OK"
  "is up to date (14 symbol(s) from the current public headers)"
  )

laige_add_api_test(api-check-stale 1 "${FX}/api" check
  "${CMAKE_CURRENT_BINARY_DIR}/stale-fixture-api.json"
  "is stale (2 difference(s) vs the regenerated manifest)"
  "+ added: laige::E::B"
  "~ changed: laige::S (summary)"
  "regenerate with:"
  )

laige_add_api_test(api-unsupported-construct 2 "${FX}/blockcomment" out
  "${CMAKE_CURRENT_BINARY_DIR}/fixture-bad.json"
  "block comments are not supported"
  )

laige_add_api_test(api-root-error 2 "${CMAKE_SOURCE_DIR}/no-such-root" out
  "${CMAKE_CURRENT_BINARY_DIR}/fixture-bad.json"
  "root is not a directory"
  )

# The live-tree drift check: the checked-in manifest must match the
# current public headers exactly (the CI drift job runs the same check).
laige_add_api_test(api-real-tree 0 "${CMAKE_SOURCE_DIR}" check
  "${CMAKE_SOURCE_DIR}/laige-api.json"
  "laige-api: OK"
  "is up to date"
  )
