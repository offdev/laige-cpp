// Test-only seed handling for randomized tests (M0-TEST-01).
//
// Normative convention: docs/testing.md §4. Every randomized test in the
// repository MUST draw from laige::Prng (M0-CORE-06) seeded through this
// helper — never from wall-clock time, std::random_device, or any other
// nondeterministic source. CI runs must be deterministic and reproducible
// (roadmap M0-TEST-01, PRD §14), and a randomized test must produce the
// same values on every CI run of the same commit.
//
// Seed source:
//   - Default: kDefaultTestSeed = 0x1F055EED ("one-fuzz-seed") — the same
//     fixed default the laige-fuzz runner uses (tools/fuzz/laige-fuzz.cpp),
//     so one documented default seed covers the test suites and the fuzz
//     lane.
//   - Override: the LAIGE_TEST_SEED environment variable (0x-prefixed hex
//     or decimal), read at call time. A set-but-unparseable value records
//     a test failure with an actionable message (CORE-008: an explicit
//     misconfiguration is not silently ignored) and the default seed is
//     returned so the calling test can finish its remaining assertions.
//
// Stream isolation: a test that draws randomness derives its own substream
// with a stable, named id — TestPrng(kMyId) — so two tests never share a
// stream position (the Prng contract: a copy shares the position;
// interleaved draws are a bug at the call site, not an error the type can
// detect). Each test file defines its own id constant (CORE-005).
//
// Cross-run identity: the SeededRandom suite (tests/testing) prints a
// machine-greppable `test-seed-check` line (seed, substream id, draw
// count, FNV-1a hash of a fixed draw count) before asserting it, so the
// line lands in the ctest output and the CI job logs (and the archived
// Testing/Temporary/LastTest.log) even when a KAT mismatches; two CI runs
// of the same commit must show identical lines (M0-TEST-01 Verify).
//
// Usage (inside a TEST body only — the failure path reports to the current
// test):
//
//   laige::Prng rng = laige::testing::TestPrng(kMySubstreamId);
//
// This header is test-only: it is never included from src/ (the
// include-graph lint covers src/** only) and defines no symbols of its
// own (header-only). It is compiled with the NFR-8.10 policy like every
// test TU (laige_apply_engine_policy).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gtest/gtest.h"
#include "laige/prng.h"

namespace laige::testing {

// The value of an environment variable; the empty string when unset.
// Platform boundary (CPP-009, the pattern of
// tests/laige-core/budget_harness_tests.cpp and tools/bench/laige-bench.cpp):
// MSVC deprecates plain getenv (C4996, fatal under /WX); getenv_s has the
// same lookup semantics. Largest value any caller reads here is a 16-hex
// seed string; 4096 is far beyond it, and a longer value is treated as
// unset (the documented fallback applies). Named per CORE-005.
inline std::string ReadEnvVar(const char* name) {
#if defined(_MSC_VER)
  constexpr std::size_t kEnvValueMaxBytes = 4096;
  char buf[kEnvValueMaxBytes];
  std::size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), name) != 0) return {};
  return std::string(buf, len);
#else
  const char* v = std::getenv(name);
  return (v != nullptr) ? std::string(v) : std::string();
#endif
}

// The fixed default test seed ("one-fuzz-seed"). Identical to
// laige-fuzz's kDefaultSeed (tools/fuzz/laige-fuzz.cpp) and never
// changed: committed known-answer constants in the test suites are
// pinned against it, so a default-seed change would fail every KAT
// loudly instead of silently reshuffling the random tests.
constexpr std::uint64_t kDefaultTestSeed = 0x1F055EEDull;

// The environment variable that overrides the default seed.
constexpr const char* kTestSeedEnvVar = "LAIGE_TEST_SEED";

// Parse a seed string the way TestSeed() reads LAIGE_TEST_SEED: 0x-prefixed
// hex or decimal. Returns false (with no output write) when the value is
// empty, partially numeric, or negative.
inline bool ParseTestSeed(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0' || text[0] == '-') {
    return false;
  }
  char* end = nullptr;
  out = std::strtoull(text, &end, 0);
  return end != text && *end == '\0';
}

// The seed the active test run uses: kDefaultTestSeed unless LAIGE_TEST_SEED
// is set, in which case its parsed value.
//
// Contract: call inside a TEST body. A set-but-unparseable LAIGE_TEST_SEED
// records a non-fatal failure (ADD_FAILURE — GTEST_FAIL is void-return
// only, and this helper returns a value) with the offending value and the
// accepted forms, then returns kDefaultTestSeed so the calling test can
// complete. The test has already failed, so the ctest entry goes red
// (CORE-008: no silent fallback for an explicit misconfiguration).
inline std::uint64_t TestSeed() {
  const std::string env = ReadEnvVar(kTestSeedEnvVar);
  if (env.empty()) {
    return kDefaultTestSeed;
  }
  std::uint64_t seed = 0;
  if (!ParseTestSeed(env.c_str(), seed)) {
    std::fprintf(stderr,
                 "laige test seed: invalid %s='%s' (use 0x-hex or decimal); "
                 "using the default 0x%llx — the test is already failing\n",
                 kTestSeedEnvVar, env.c_str(),
                 static_cast<unsigned long long>(kDefaultTestSeed));
    ADD_FAILURE() << kTestSeedEnvVar << "='" << env.c_str()
                  << "' is not a valid seed (0x-prefixed hex or decimal); "
                  << "falling back to the default 0x" << kDefaultTestSeed;
    return kDefaultTestSeed;
  }
  return seed;
}

// A Prng substream for a test: TestSeed() derived by the caller's stable
// substream id (docs/testing.md §4). The id is a test identity, not a
// magic number: each randomized test file declares its own named
// constant so substreams cannot collide by accident.
inline laige::Prng TestPrng(std::uint32_t substreamId) {
  return laige::Prng::deriveSubstream(TestSeed(), substreamId);
}

}  // namespace laige::testing
