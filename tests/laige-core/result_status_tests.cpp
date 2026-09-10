// laige-core Result<T,E>/Status + error registry suite (M0-CORE-01).
//
// Step Verify scope:
//   - construction: success and failure, implicit and factory forms
//   - error propagation through a call chain
//   - no exceptions (linker level): this translation unit compiles with
//     -fno-exceptions -fno-rtti (NFR-8.10, via laige_apply_engine_policy);
//     the static_asserts below make a policy violation fail the build
//     instead of passing silently (CORE-008)
//   - error strings follow the 5-field NFR-13.3 grammar, asserted per
//     registered code
//
// Runs as CTest `result_status` (the step's Verify command is
// `ctest -R result_status`): a filtered view of this shared
// laige-core_tests executable, selecting exactly the suites below.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"
#include "laige/errors.h"
#include "laige/result.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "result_status_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "result_status_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "result_status_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

// MSVC never updates __cplusplus from /std (it stays 199711L, a legacy
// compatibility value); the active standard is reported by _MSVC_LANG.
// Every other supported compiler (NFR-8.10) sets __cplusplus from -std.
#if defined(_MSC_VER)
#  define RESULT_STATUS_TESTS_ACTIVE_CPLUSPLUS _MSVC_LANG
#else
#  define RESULT_STATUS_TESTS_ACTIVE_CPLUSPLUS __cplusplus
#endif

#if RESULT_STATUS_TESTS_ACTIVE_CPLUSPLUS < 202002L
static_assert(false,
              "result_status_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

namespace {

// The registered codes, in integer order. The values are pinned by the
// stability contract (PRD §9.4); this list is the registry test oracle.
const laige::ErrorCode kRegistered[] = {
    laige::ErrorCode::Unknown,
    laige::ErrorCode::InvalidArgument,
    laige::ErrorCode::MalformedInput,
    laige::ErrorCode::BudgetExhausted,
    laige::ErrorCode::IoError,
};

// NFR-13.3 grammar check on a rendered error line: exactly 5 fields
// joined by " | ", no empty field, no '|' inside a field, no leading or
// trailing spaces. Hand-rolled (not std::regex) so this suite stays free
// of the exception machinery it is testing the engine against.
bool isValidErrorLine(std::string_view line) {
  if (line.empty()) return false;
  int fields = 0;
  std::size_t start = 0;
  while (true) {
    const std::size_t sep = line.find(" | ", start);
    const std::size_t end =
        (sep == std::string_view::npos) ? line.size() : sep;
    const std::string_view field = line.substr(start, end - start);
    if (field.empty() || field.front() == ' ' || field.back() == ' ') {
      return false;
    }
    for (const char c : field) {
      if (c == '|') return false;
    }
    ++fields;
    if (sep == std::string_view::npos) return fields == 5;
    start = sep + 3;
  }
}

// Error-propagation fixtures: each function either returns its value or
// propagates an ErrorCode — the pattern every no-exception laige-core
// function follows (FR-12.1).
laige::Result<int, laige::ErrorCode> clampNonNegative(int x) {
  if (x < 0) return laige::ErrorCode::InvalidArgument;  // implicit E→Result
  return x;                                              // implicit T→Result
}

laige::Result<int, laige::ErrorCode> doubleValue(int x) {
  const laige::Result<int, laige::ErrorCode> r = clampNonNegative(x);
  if (r.isError()) return r.error();  // propagate the original code
  return r.value() * 2;
}

laige::Status validateRange(int x, int lo, int hi) {
  if (x < lo || x > hi) return laige::ErrorCode::InvalidArgument;
  return {};  // success (default-constructed Status)
}

}  // namespace

// ---------------------------------------------------------------------------
// Result<T,E> — construction
// ---------------------------------------------------------------------------

TEST(ResultStatus, SuccessConstructionImplicitAndFactory) {
  const laige::Result<int> r = laige::Result<int>::success(42);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.isError());
  EXPECT_EQ(r.value(), 42);
  EXPECT_NE(r.valueIfOk(), nullptr);
  EXPECT_EQ(r.errorIfError(), nullptr);

  const laige::Result<int> r2 = 42;  // implicit T→Result
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(r2.value(), 42);
}

TEST(ResultStatus, FailureConstructionImplicitAndFactory) {
  const laige::Result<int> r =
      laige::Result<int>::failure(laige::ErrorCode::BudgetExhausted);
  ASSERT_TRUE(r.isError());
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_EQ(r.valueIfOk(), nullptr);
  EXPECT_NE(r.errorIfError(), nullptr);

  const laige::Result<int> r2 = laige::ErrorCode::InvalidArgument;
  ASSERT_TRUE(r2.isError());
  EXPECT_EQ(r2.error(), laige::ErrorCode::InvalidArgument);
}

TEST(ResultStatus, SameTypePayloadAndErrorUsesFactories) {
  // T and E are identical (hence mutually convertible): the converting
  // constructors are unavailable; the factories are the only path.
  const laige::Result<laige::ErrorCode> r =
      laige::Result<laige::ErrorCode>::success(laige::ErrorCode::Unknown);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), laige::ErrorCode::Unknown);

  const laige::Result<laige::ErrorCode> f = laige::Result<laige::ErrorCode>::
      failure(laige::ErrorCode::BudgetExhausted);
  ASSERT_TRUE(f.isError());
  EXPECT_EQ(f.error(), laige::ErrorCode::BudgetExhausted);
}

TEST(ResultStatus, MoveOnlyPayload) {
  auto makePtr = [](int x) { return std::make_unique<int>(x); };
  laige::Result<std::unique_ptr<int>> r =
      laige::Result<std::unique_ptr<int>>::success(makePtr(7));
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r.value(), 7);

  laige::Result<std::unique_ptr<int>> r2 = std::move(r);
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(*r2.value(), 7);
  // r is in a valid but unspecified state after the move; it is used
  // only for destruction here.
}

TEST(ResultStatus, TakeValueMovesOutSuccessValue) {
  // Ownership transfer path (rvalue results only): used e.g. to hand a
  // freshly created resource (FileSink, M0-CORE-02) to a container.
  auto makePtr = [](int x) { return std::make_unique<int>(x); };
  auto r = laige::Result<std::unique_ptr<int>>::success(makePtr(9));
  std::unique_ptr<int> moved = std::move(r).takeValue();
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(*moved, 9);

  // An error result has no value to take.
  auto e = laige::Result<int>::failure(laige::ErrorCode::InvalidArgument);
  EXPECT_TRUE(e.isError());
}

TEST(ResultStatus, CopySemantics) {
  const laige::Result<int> a = laige::Result<int>::success(5);
  const laige::Result<int> b = a;
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value(), 5);

  const laige::Result<int> c =
      laige::Result<int>::failure(laige::ErrorCode::MalformedInput);
  const laige::Result<int> d = c;
  ASSERT_TRUE(d.isError());
  EXPECT_EQ(d.error(), laige::ErrorCode::MalformedInput);
}

// ---------------------------------------------------------------------------
// Error propagation
// ---------------------------------------------------------------------------

TEST(ResultStatus, ErrorPropagatesThroughCallChain) {
  const laige::Result<int, laige::ErrorCode> r = doubleValue(-1);
  ASSERT_TRUE(r.isError());
  EXPECT_EQ(r.error(), laige::ErrorCode::InvalidArgument);
  EXPECT_EQ(r.valueIfOk(), nullptr);
}

TEST(ResultStatus, ValuePropagatesThroughCallChain) {
  const laige::Result<int, laige::ErrorCode> r = doubleValue(21);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 42);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

TEST(Status, DefaultConstructedIsSuccess) {
  const laige::Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_FALSE(s.isError());
}

TEST(Status, FailureCarriesCode) {
  const laige::Status s =
      laige::Status::failure(laige::ErrorCode::BudgetExhausted);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), laige::ErrorCode::BudgetExhausted);
  EXPECT_STREQ(s.errorText(),
               laige::errorText(laige::ErrorCode::BudgetExhausted));
}

TEST(Status, ImplicitConstructionFromErrorCode) {
  const laige::Status s = laige::ErrorCode::MalformedInput;
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), laige::ErrorCode::MalformedInput);
}

TEST(Status, ErrorPropagatesThroughCallChain) {
  const laige::Status s = validateRange(10, 0, 5);
  ASSERT_TRUE(s.isError());
  EXPECT_EQ(s.error(), laige::ErrorCode::InvalidArgument);

  const laige::Status ok = validateRange(3, 0, 5);
  EXPECT_TRUE(ok.ok());
}

// ---------------------------------------------------------------------------
// ErrorCode registry — NFR-13.3 grammar
// ---------------------------------------------------------------------------

TEST(ErrorCodeRegistry, EveryRegisteredCodeFollowsGrammar) {
  for (const laige::ErrorCode code : kRegistered) {
    const char* line = laige::errorText(code);
    ASSERT_NE(line, nullptr);
    EXPECT_TRUE(isValidErrorLine(line)) << line;
  }
}

TEST(ErrorCodeRegistry, RenderedTextMatchesRegistryFields) {
  for (const laige::ErrorCode code : kRegistered) {
    const laige::ErrorEntry& e = laige::errorInfo(code);
    EXPECT_EQ(e.code, code);
    EXPECT_STREQ(laige::errorName(code), e.codeId);

    // The pre-rendered line MUST be exactly the five fields joined by
    // " | " (NFR-13.3); this catches field/text drift (CORE-006).
    std::string expected = e.codeId;
    expected += " | ";
    expected += e.whatFailed;
    expected += " | ";
    expected += e.why;
    expected += " | ";
    expected += e.fix;
    expected += " | ";
    expected += e.docAnchor;
    EXPECT_STREQ(laige::errorText(code), expected.c_str());
  }
}

TEST(ErrorCodeRegistry, DocAnchorMatchesCodeId) {
  for (const laige::ErrorCode code : kRegistered) {
    const laige::ErrorEntry& e = laige::errorInfo(code);
    std::string anchor = "docs/api/errors.md#";
    for (const char c : std::string_view(e.codeId)) {
      anchor += (c == '_') ? '-' : c;
    }
    EXPECT_STREQ(e.docAnchor, anchor.c_str());
  }
}

TEST(ErrorCodeRegistry, IntegerValuesArePinned) {
  // PRD §9.4 stability: values are additive-only; a renumber here is an
  // API break that goes through a PRD revision, never silently.
  EXPECT_EQ(static_cast<std::uint32_t>(laige::ErrorCode::Unknown), 1u);
  EXPECT_EQ(static_cast<std::uint32_t>(laige::ErrorCode::InvalidArgument),
            2u);
  EXPECT_EQ(static_cast<std::uint32_t>(laige::ErrorCode::MalformedInput),
            3u);
  EXPECT_EQ(static_cast<std::uint32_t>(laige::ErrorCode::BudgetExhausted),
            4u);
  EXPECT_EQ(static_cast<std::uint32_t>(laige::ErrorCode::IoError), 5u);
}

TEST(ErrorCodeRegistry, UnregisteredValuesRenderUnknown) {
  // 0 is the no-error sentinel and 0xDEADBEEF is not registered; both
  // must render `unknown` instead of crashing (CORE-008).
  const laige::ErrorCode sentinel = static_cast<laige::ErrorCode>(0u);
  const laige::ErrorCode bogus = static_cast<laige::ErrorCode>(0xDEADBEEFu);
  for (const laige::ErrorCode code : {sentinel, bogus}) {
    const laige::ErrorEntry& e = laige::errorInfo(code);
    EXPECT_STREQ(e.codeId, "unknown");
    EXPECT_TRUE(isValidErrorLine(laige::errorText(code)))
        << laige::errorText(code);
  }
}
