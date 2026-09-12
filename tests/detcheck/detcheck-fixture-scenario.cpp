// detcheck-fixture-scenario — the fixture scenario for the laige-detcheck
// CTest tests (M0-TOOL-02).
//
// A self-contained deterministic scenario that implements the M0
// hash-line contract (the normative text is in the header of
// tools/detcheck/laige-detcheck.cpp and in docs/api/detcheck.md): one
// `<tick> <hash>` line per tick on stdout (16 lowercase hex hash
// digits), starting at tick 0, exit 0.
//
// It is deliberately NOT linked against laige-core: the fixture tests
// the contract plumbing (capture, strict parsing, comparison, reports),
// not the engine math — the built-in `synthetic` scenario (fpx16_16 +
// Prng) and the math_fixed known-answer test cover that.
//
// One source, five CMake-built variants (one flag each):
//
//   (no flag)                          clean: 32 ticks
//   DETCHK_FIXTURE_PERTURB_TICK=7      run-b stand-in: adds 1 to state[3]
//                                      at tick 7 (diverges from the clean
//                                      run exactly at tick 7)
//   DETCHK_FIXTURE_BAD_OUTPUT=1        prints a malformed line at tick 3
//                                      (detcheck must exit 2)
//   DETCHK_FIXTURE_FAIL_TICK=5         exits with DETCHK_FIXTURE_FAIL_EXIT
//   DETCHK_FIXTURE_FAIL_EXIT=3         at tick 5 (scenario failure;
//                                      detcheck must exit 2)
//   DETCHK_FIXTURE_TICKS=16            only 16 ticks (stream-length
//                                      mismatch vs the 32-tick clean run;
//                                      detcheck must report DIVERGED)
//
// State: 16 u32 words advanced by one named 64-bit LCG per word (the
// Marsaglia 64-bit LCG constants, the same house choice as
// laige-bench's synthetic workload — CPP-014); the per-tick hash is
// FNV-1a 64 over (tick, the state words), big-endian per word — the
// same FNV constants as laige-detcheck. Pure unsigned-integer
// arithmetic: bit-exact on every P0 platform (ARCH-010).

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr std::uint64_t kLcgMultiplier = 6364136223846793005ull;
constexpr std::uint64_t kLcgIncrement = 1442695040888963407ull;
constexpr std::uint64_t kSeed = 0x1234567890ABCDEFull;
constexpr int kStateWords = 16;
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

#ifndef DETCHK_FIXTURE_TICKS
#define DETCHK_FIXTURE_TICKS 32
#endif

// Strict decimal parse bounded to 1..4096 (fixture runs are tiny; the
// bound keeps the argument surface minimal).
int parseTicks(std::string_view value) {
  if (value.empty()) return -1;
  std::uint64_t v = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return -1;
    if (v > (4096 - static_cast<std::uint64_t>(c - '0')) / 10) return -1;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return static_cast<int>(v);
}

}  // namespace

int main(int argc, char** argv) {
  int ticks = DETCHK_FIXTURE_TICKS;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--ticks=", 0) == 0) {
      ticks = parseTicks(arg.substr(8));
      if (ticks < 1) return 2;
    } else {
      return 2;  // unknown argument: a scenario failure, not a crash
    }
  }

  std::uint64_t lcg = kSeed;
  std::uint32_t state[kStateWords] = {0};
  for (int t = 0; t < ticks; ++t) {
    // Fixed-order advance: one LCG step mixed into each state word.
    for (int i = 0; i < kStateWords; ++i) {
      lcg = lcg * kLcgMultiplier + kLcgIncrement;
      state[i] = static_cast<std::uint32_t>(lcg) ^
                 (static_cast<std::uint32_t>(lcg >> 32) + state[i]);
    }
#if defined(DETCHK_FIXTURE_PERTURB_TICK)
    if (t == DETCHK_FIXTURE_PERTURB_TICK) state[3] += 1;
#endif
#if defined(DETCHK_FIXTURE_BAD_OUTPUT)
    if (t == 3) {
      std::printf("3 not-a-hash\n");  // contract violation
      continue;
    }
#endif
#if defined(DETCHK_FIXTURE_FAIL_TICK)
    if (t == DETCHK_FIXTURE_FAIL_TICK) return DETCHK_FIXTURE_FAIL_EXIT;
#endif
    std::uint64_t h = kFnvOffsetBasis;
    auto feed = [&h](std::uint64_t v) {
      for (int shift = 56; shift >= 0; shift -= 8) {
        h ^= (v >> shift) & 0xFFull;
        h *= kFnvPrime;
      }
    };
    feed(static_cast<std::uint64_t>(static_cast<std::uint32_t>(t)));
    for (int i = 0; i < kStateWords; ++i) {
      feed(state[i]);
    }
    std::printf("%d %016llx\n", t, static_cast<unsigned long long>(h));
  }
  return 0;
}
