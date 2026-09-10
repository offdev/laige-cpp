// laige-core build smoke test (M0-BUILD-01; GoogleTest-ified in M0-DEP-01).
//
// laige-core carries no functional engine code yet (Result/Status lands in
// M0-CORE-01), so this suite verifies what M0-BUILD-01 delivers:
//
//   1. The laige-core library links into the test executable in whichever
//      variant the build tree selected: static (default) or shared
//      (LAIGE_BUILD_SHARED=ON). NFR-8.9.
//   2. The NFR-8.10 language policy was actually applied to this
//      translation unit: C++20, exceptions disabled, RTTI disabled.
//      Check 2 uses static_assert, so a policy violation fails the build
//      loudly instead of passing silently (CORE-008).
//
// This is the first suite running on the GoogleTest framework wired in by
// M0-DEP-01 (deps/googletest, locked in deps.lock): a green
// `ctest -R laige-core_tests` proves the dev-only dependency plumbing end
// to end (configure → build → ctest).

#include <cstdio>
#include <cstring>

#include "gtest/gtest.h"
#include "laige/core/version.h"

// ---------------------------------------------------------------------------
// NFR-8.10 policy self-checks (compile-time; a violation fails the build)
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions)
static_assert(false,
              "laige-core_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#elif defined(__EXCEPTIONS) && __EXCEPTIONS
static_assert(false,
              "laige-core_tests must be built with exceptions disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if defined(__cpp_rtti) && __cpp_rtti
static_assert(false,
              "laige-core_tests must be built with RTTI disabled "
              "(NFR-8.10); see laige_apply_engine_policy().");
#endif

#if __cplusplus < 202002L
static_assert(false,
              "laige-core_tests must be built as C++20 (NFR-8.10); "
              "see laige_apply_engine_policy().");
#endif

namespace {

// The variant actually linked, as stamped by CMake: LAIGE_CORE_IS_SHARED is
// defined only for shared builds (tests/laige-core/CMakeLists.txt).
#if defined(LAIGE_CORE_IS_SHARED)
constexpr int kLinkedShared = 1;
#else
constexpr int kLinkedShared = 0;
#endif

}  // namespace

// NFR-8.9: the variant actually linked must be the variant the build tree
// selected. A mismatch means this smoke test no longer reflects the tree it
// was built in.
TEST(LaigeCoreBuild, LinksTheVariantTheBuildTreeSelected) {
  static_assert(kLinkedShared == LAIGE_EXPECT_SHARED,
                "linked laige-core variant does not match "
                "LAIGE_BUILD_SHARED (NFR-8.9); see "
                "tests/laige-core/CMakeLists.txt.");
  std::printf("laige-core_tests: linked against %s laige-core; OK\n",
              kLinkedShared == 1 ? "shared" : "static");
  SUCCEED();
}

// M0-BUILD-01: the module carries the minimal version identifier only.
TEST(LaigeCoreBuild, VersionIsZeroInM0) {
  using namespace laige::core;
  EXPECT_EQ(kMajor, 0);
  EXPECT_EQ(kMinor, 0);
  EXPECT_EQ(kPatch, 0);
  EXPECT_STREQ(versionString(), "0.0.0");
}
