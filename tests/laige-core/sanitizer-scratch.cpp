// M0-CI-02 Verify scratch (roadmap: "introducing a deliberate OOB read in a
// scratch test fails the ASan job (test removed afterwards)").
//
// This file is INTENTIONALLY BAD C++ and must be deleted after the CI cycle.
//
// What it does:
//   * Reads one element past the end of a 4-element stack array, through a
//     runtime-volatile index so that:
//       - -Wall -Werror stays clean (no constant-index OOB warning), and
//       - no optimizer can constant-fold the read away.
//   * In the linux-asan lane (LAIGE_ASAN=ON: -fsanitize=address,undefined
//     with -fno-sanitize-recover=all), UBSan's bounds check reports
//     "index 16 out of bounds for type 'int[4]'" and aborts the test
//     process; ctest then fails the job.
//   * In the linux-tsan lane (LAIGE_TSAN=ON) the read is not a data race and
//     the test takes no assertion on the garbage value, so the lane stays
//     green — the deliberate cross-lane control of the Verify scenario.
//
// Local evidence (2026-09-10, Clang 22.1.8): same file red on the ASan tree
// (ctest exit 8, fatal report + report file via log_path), green on the TSan
// tree (100% passed, exit 0).

#include <gtest/gtest.h>

namespace {

int oob_read_scratch() {
  int arr[4] = {0, 1, 2, 3};
  const volatile int idx = 16;  // runtime-volatile index, keeps -Werror clean
  return arr[idx];              // deliberate out-of-bounds read
}

}  // namespace

TEST(SanitizerScratch, oob_read_trips_asan_lane) {
  // No assertion on the value: in the TSan lane the read is the only
  // observable behavior and the test passes; in the ASan lane UBSan aborts
  // the process before any assertion could run.
  oob_read_scratch();
}
