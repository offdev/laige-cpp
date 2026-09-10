// laige-core build smoke test (M0-BUILD-01).
//
// laige-core carries no functional engine code yet (Result/Status lands in
// M0-CORE-01), so this test verifies what M0-BUILD-01 delivers:
//
//   1. The laige-core library links into a test executable in whichever
//      variant the build tree selected: static (default) or shared
//      (LAIGE_BUILD_SHARED=ON). NFR-8.9.
//   2. The NFR-8.10 language policy was actually applied to this
//      translation unit: C++20, exceptions disabled, RTTI disabled.
//      Check 2 uses static_assert, so a policy violation fails the build
//      loudly instead of passing silently (CORE-008).
//
// Plain C++ on purpose: GoogleTest is vendored in M0-DEP-01; tests before
// that are CTest-registered executables with no third-party dependency.

#include <cstdio>
#include <cstring>

#include "laige/core/version.h"

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

int gFailures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    ++gFailures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

}  // namespace

int main() {
  using namespace laige::core;

  check(kMajor == 0 && kMinor == 0 && kPatch == 0,
        "version components are 0.0.0 in M0");

  const char* const version = versionString();
  check(version != nullptr, "versionString() returns non-null");
  check(version != nullptr && std::strcmp(version, "0.0.0") == 0,
        "versionString() renders \"0.0.0\"");

  if (gFailures != 0) {
    std::printf("laige-core_tests: %d failure(s)\n", gFailures);
    return 1;
  }

#if defined(LAIGE_CORE_IS_SHARED)
  std::puts("laige-core_tests: linked against shared laige-core; OK");
#else
  std::puts("laige-core_tests: linked against static laige-core; OK");
#endif
  return 0;
}
