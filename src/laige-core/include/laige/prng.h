// laige-core deterministic PRNG (M0-CORE-06).
//
// PRD §10.3: gameplay randomness is *seeded and per-substream* — every
// subsystem draws from a substream derived from the master seed, and the
// whole simulation is reproducible from (master seed, substream ids, call
// order). AGENTS ARCH-010: the determinism scope this type promises is
// *cross-platform bit-exact* (any build, platform, architecture, or
// compiler that targets the documented algorithm below).
//
// ---------------------------------------------------------------------------
// Algorithm (the determinism contract — changing it breaks the golden
// vectors on purpose; see docs/api/prng.md)
// ---------------------------------------------------------------------------
//
// Core: xorshift128+ (Markus Johnson, 2009; reference implementation:
// https://github.com/vigna/lemire/blob/master/xorshift128plus.c), transcribed
// and verified against that reference for 10^4 consecutive outputs in the
// `PrngGolden` suite (transcription check, §1 of the test file).
//
//   State: two 64-bit words (part1, part2); the all-zero state is excluded
//   (the transition is a bijection of the nonzero states).
//
//     o0 = part1; o1 = part2;
//     part1 = o1;
//     t     = o0 ^ (o0 << 23);
//     part2 = t ^ o1 ^ (t >> 18) ^ (o1 >> 5);
//     out   = part2 + o1;                    // unsigned 64-bit wraparound
//
//   The transition is a bijection of GF(2)^128 \ {0}, and the
//   state map's characteristic polynomial over GF(2) is *primitive* of
//   degree 128, so every nonzero state has period exactly 2^128 - 1
//   (the period proof is committed in the `PrngPeriod` suite, which
//   re-derives the characteristic polynomial from a probe orbit with
//   Berlekamp-Massey and checks primitivity directly).
//
// Seeding: the state is the first two outputs of splitmix64 (David
// Stafford, 2018) advanced from `seed + K`, with K the splitmix64
// increment:
//
//     z     = seed + K;
//     part1 = splitmix64(z);
//     part2 = splitmix64(z + K);
//
//   splitmix64 is a bijection and its two inputs differ by the nonzero K,
//   so the all-zero state is unreachable for every 64-bit seed.
//
// Substreams: `deriveSubstream(seed, id) == Prng(seed + id * K)` — a
// documented hash of (seed, id). Id 0 is the master stream, and
// derivation composes: derive(derive(seed, i), j) == derive(seed, i + j)
// (unsigned wraparound of the id sum).
//
// Output taps:
//
//   next_u64()      the xorshift128+ output (64 bits).
//   next_range(lo, hi)  uniform in [lo, hi), Lemire unbiased reduction
//                       (rejection of the low-bias residue of 2^64 mod n).
//   next_float01()  24-bit resolution: next_u64() >> 40 scaled by 2^-24,
//                   i.e. exactly k * 2^-24 for k in [0, 2^24) — a dyadic
//                   float, bit-exact on every platform (ADR 0002: integer
//                   arithmetic + one exactly-representable power-of-two
//                   scale; no libm, no rounding policy).
//
// ---------------------------------------------------------------------------
// Common contracts
// ---------------------------------------------------------------------------
//
// Determinism (ARCH-010, ADR 0002, PRD §10.3): the entire API is pure
// unsigned integer arithmetic plus one multiply by the exactly
// representable constant 2^-24. There are no floats in the state, no
// platform intrinsics, no libm calls, and no ordering that depends on
// anything but the call sequence. The same seed + call sequence produces
// bit-identical output on every supported platform and compiler. The
// golden vectors in the `prng` CTest suite are the replay fixtures: they
// are *intended* to fail if the algorithm changes (the algorithm is part
// of the determinism contract, not an implementation detail).
//
// Allocation (PERF-003, PERF-002): nothing. Construction, every draw,
// and substream derivation are a handful of integer operations — no
// allocation, no I/O, no synchronization, no global state. O(1) per draw
// with no amortization (the next_range rejection loop has an expected
// length < 2 draws).
//
// Ownership and lifetime (CPP-002, CPP-009, CONC-001): a `Prng` is a
// value type owning exactly three u64 words (seed + state). It is
// copyable and assignable (value semantics, O(1)); a copy *shares the
// stream position* — drawing from a copy interleaves with drawing from
// the original, which is a bug at the call site, not an error this type
// can detect. It has exactly one owner thread: it is NOT thread-safe.
// Sharing across threads requires an explicit engine synchronization
// boundary (CONC-002) or, preferably, one substream per owner.
//
// Errors (CORE-008, FR-12.1): the only misuse is `next_range(lo, hi)` with
// `lo >= hi` — a debug assert; in release builds it is documented
// undefined behavior (the span `hi - lo` underflows). There are no
// runtime error codes to return: every well-formed call is total.
//
// ---------------------------------------------------------------------------
// Misuse warnings
// ---------------------------------------------------------------------------
//   - Copying a Prng to "share a substream" across systems interleaves the
//     two copies' draws; give each subsystem its own derived substream
//     (that is what substream ids are for) and never hand out copies of a
//     live master.
//   - Feeding next_u64() into anything that assumes 64-bit randomness for
//     cryptography: this is a game PRNG, not a CSPRNG (DEP-002: never
//     use it for tokens, keys, or anything security-sensitive).
//   - next_float01() has 24-bit resolution (2^24 distinct values, not the
//     2^24 mantissa+denormals of a float); it never returns exactly 1.0,
//     and exactly 0.0 occurs with probability 2^-24 per draw.
//   - The id argument of substream()/deriveSubstream() is a *stable*
//     subsystem identity (PRD §10.3): reusing an id for a different
//     subsystem, or changing ids between sessions, changes the streams.

#pragma once

#include <cassert>
#include <cstdint>
#include <limits>

namespace laige {

// ---------------------------------------------------------------------------
// Prng — deterministic xorshift128+ with seeded, per-substream derivation
// (M0-CORE-06; contract preamble above; full API doc: docs/api/prng.md)
// ---------------------------------------------------------------------------
class Prng {
 public:
  // splitmix64 increment (Marsaglia's golden-ratio odd constant): the
  // step between successive splitmix64 inputs and between substream seeds.
  static constexpr std::uint64_t kSplitmix64Increment = 0x9E3779B97F4A7C15ull;

  // splitmix64 mixing multipliers (Stafford 2018).
  static constexpr std::uint64_t kMixMultiplierA = 0xBF58476D1CE4E5B9ull;
  static constexpr std::uint64_t kMixMultiplierB = 0x94D049BB133111EBull;

  // next_float01() resolution: one value is 2^-24 (24 mantissa bits).
  static constexpr float kFloat01Unit = 0x1.0p-24f;

  // Construct the master stream for `seed`. The state is nonzero for
  // every seed (splitmix64 is a bijection; see the preamble).
  explicit Prng(std::uint64_t seed)
      : seed_(seed), s0_(0), s1_(0) {
    seedState(seed, s0_, s1_);
  }

  // One stream draw: the xorshift128+ output; advances the state.
  std::uint64_t next_u64();

  // Uniform value in [min, max) (max - min must be in [1, 2^32 - 1]).
  // Unbiased (Lemire reduction with rejection); expected < 2 draws.
  // `min >= max` is a debug assert (documented UB in release).
  std::uint32_t next_range(std::uint32_t min, std::uint32_t max);

  // Uniform value in [0, 1) at 24-bit resolution: exactly k * 2^-24 for an
  // integer k in [0, 2^24). Never 1.0; 0.0 with probability 2^-24.
  float next_float01();

  // The seed this stream was constructed from (save/replay identity,
  // PRD §10.3; M1-DET-03 hashes this together with the substream id).
  std::uint64_t seed() const { return seed_; }

  // The stream's state word 1 (the save/replay identity's part1; PRD
  // §10.3 — a saved stream is (seed, part1, part2)). Read-only; O(1),
  // no side effects. M1-DET-03: World::stateHash folds these into the
  // deterministic state hash with the substream id.
  std::uint64_t statePart1() const { return s0_; }

  // The stream's state word 2 (the save/replay identity's part2).
  // Read-only; O(1), no side effects. See statePart1().
  std::uint64_t statePart2() const { return s1_; }

  // A substream of this stream's seed: deriveSubstream(seed(), id).
  // Independent stream position; id 0 == the master stream.
  Prng substream(std::uint32_t id) const;

  // Substream derivation (documented hash, see the preamble):
  // Prng(seed + id * kSplitmix64Increment). Composes:
  // deriveSubstream(deriveSubstream(seed, i), j) == deriveSubstream(seed, i+j).
  static Prng deriveSubstream(std::uint64_t seed, std::uint32_t id);

  // Seed-to-state mapping (documented in the preamble). Exposed for
  // determinism verification and M1 save/replay: a saved stream is
  // (seed, part1, part2) and restores by seedState + stepState calls.
  static void seedState(std::uint64_t seed, std::uint64_t& part1,
                        std::uint64_t& part2);

  // One transition step on a raw state (see the preamble). Exposed for
  // determinism verification (the PrngPeriod suite reconstructs the
  // state map over GF(2) from this) and M1 save/replay.
  static void stepState(std::uint64_t& part1, std::uint64_t& part2);

 private:
  std::uint64_t seed_;
  std::uint64_t s0_;  // state word 1 (reference: s[0])
  std::uint64_t s1_;  // state word 2 (reference: s[1])
};

// ---------------------------------------------------------------------------
// splitmix64 (Stafford 2018) — seeding-only; a bijection of u64.
// ---------------------------------------------------------------------------

inline std::uint64_t splitMix64(std::uint64_t z) {
  z = (z ^ (z >> 30)) * Prng::kMixMultiplierA;
  z = (z ^ (z >> 27)) * Prng::kMixMultiplierB;
  return z ^ (z >> 31);
}

inline void Prng::seedState(std::uint64_t seed, std::uint64_t& part1,
                            std::uint64_t& part2) {
  const std::uint64_t z = seed + kSplitmix64Increment;
  part1 = splitMix64(z);
  part2 = splitMix64(z + kSplitmix64Increment);
}

inline void Prng::stepState(std::uint64_t& part1, std::uint64_t& part2) {
  const std::uint64_t o0 = part1;
  const std::uint64_t o1 = part2;
  part1 = o1;
  const std::uint64_t t = o0 ^ (o0 << 23);
  part2 = t ^ o1 ^ (t >> 18) ^ (o1 >> 5);
}

inline std::uint64_t Prng::next_u64() {
  const std::uint64_t o0 = s0_;
  const std::uint64_t o1 = s1_;
  s0_ = o1;
  const std::uint64_t t = o0 ^ (o0 << 23);
  s1_ = t ^ o1 ^ (t >> 18) ^ (o1 >> 5);
  return s1_ + o1;  // unsigned wraparound (documented)
}

inline std::uint32_t Prng::next_range(std::uint32_t min, std::uint32_t max) {
  assert(min < max);  // documented UB in release (span underflow)
  const std::uint64_t n = static_cast<std::uint64_t>(max - min);
  // rem = 2^64 mod n (via the max-value form, which avoids 128-bit math).
  const std::uint64_t max64 = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t rem = (max64 % n + 1) % n;
  if (rem == 0) {
    // n divides 2^64: the low bits are already uniform.
    return static_cast<std::uint32_t>(min + next_u64() % n);
  }
  // Lemire unbiased reduction: reject draws in [2^64 - rem, 2^64).
  const std::uint64_t threshold = max64 - rem + 1;
  for (;;) {
    const std::uint64_t r = next_u64();
    if (r < threshold) return static_cast<std::uint32_t>(min + r % n);
  }
}

inline float Prng::next_float01() {
  // 24 mantissa bits, scaled by the exactly representable 2^-24: the result
  // is bit-exact on every platform (ADR 0002 integer-exactness scope).
  return static_cast<float>(next_u64() >> 40) * kFloat01Unit;
}

inline Prng Prng::substream(std::uint32_t id) const {
  return deriveSubstream(seed_, id);
}

inline Prng Prng::deriveSubstream(std::uint64_t seed, std::uint32_t id) {
  return Prng(
      seed + static_cast<std::uint64_t>(id) * kSplitmix64Increment);
}

}  // namespace laige
