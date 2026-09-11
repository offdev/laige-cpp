# Deterministic PRNG (`laige::Prng`)

The engine's deterministic, seeded, per-substream random number generator
(M0-CORE-06; PRD §10.3, AGENTS ARCH-010). Public header:
`src/laige-core/include/laige/prng.h` (header-only; no implementation file).

## Quick start

```cpp
#include <laige/prng.h>

// Master stream from the session seed.
laige::Prng master(sessionSeed);

// One subsystem owns one derived substream (stable id, e.g. an
// subsystem id): deterministic for the seed, independent of every
// other substream's position.
laige::Prng physics = master.substream(kPhysicsSubstreamId);

std::uint32_t roll   = physics.next_range(0, 100);   // [0, 100)
float     chance     = physics.next_float01();      // [0, 1), 24-bit
std::uint64_t raw    = physics.next_u64();          // 64-bit
```

## Algorithm (the determinism contract)

Changing any of this breaks the committed golden vectors on purpose —
the algorithm *is* the determinism contract (ARCH-010, ADR 0002), not an
implementation detail. The `prng` CTest suite fails if it drifts.

- **Core:** xorshift128+ (Markus Johnson, 2009), transcribed from and
  verified against the reference implementation (lemire/SIMDxorshift,
  `xorshift128plus.c`; 10^4-step transcription check in the
  `PrngGolden.ReferenceMatch` test). State = two u64 words `(part1,
  part2)`; the all-zero state is excluded and unreachable:

  ```text
  o0 = part1;  o1 = part2;
  part1 = o1;
  t     = o0 ^ (o0 << 23);
  part2 = t ^ o1 ^ (t >> 18) ^ (o1 >> 5);
  out   = part2 + o1;              // unsigned 64-bit wraparound
  ```

- **Seeding:** the state is the first two outputs of splitmix64
  (Stafford 2018) advanced from `seed + K`, with
  `K = 0x9E3779B97F4A7C15` (the splitmix64 increment,
  `Prng::kSplitmix64Increment`):

  ```text
  z     = seed + K;
  part1 = splitmix64(z);
  part2 = splitmix64(z + K);
  ```

  splitmix64 is a bijection of u64 and its two inputs differ by the
  nonzero K, so no 64-bit seed reaches the excluded all-zero state
  (spot-checked for 100k seeds in the suite).

- **Substreams:** `deriveSubstream(seed, id) == Prng(seed + id * K)` —
  the documented derivation hash of (master seed, substream id). Id 0 is
  the master stream. Derivation composes:
  `deriveSubstream(deriveSubstream(seed, i), j) == deriveSubstream(seed,
  i + j)` (unsigned wraparound of the id sum).

- **Output taps:**

  | Tap | Value |
  |---|---|
  | `next_u64()` | the xorshift128+ output (64 bits). |
  | `next_range(min, max)` | uniform in `[min, max)`, Lemire unbiased reduction: `n = max - min`, reject draws `r >= 2^64 - (2^64 mod n)` when `2^64 mod n != 0`; the `n | 2^64` fast path returns `min + r mod n` directly. |
  | `next_float01()` | `next_u64() >> 40` scaled by `2^-24` (`Prng::kFloat01Unit`): exactly `k * 2^-24` for `k in [0, 2^24)` — 24-bit resolution, `[0, 1)`, bit-exact everywhere. |

## Determinism scope (ARCH-010, ADR 0002)

The whole API is pure unsigned integer arithmetic plus one multiply by
the exactly representable constant `2^-24`. There are no floats in the
state, no libm, no platform intrinsics, and no ordering that depends on
anything but the call sequence. The guarantee is therefore
**cross-platform bit-exact**: the same (seed, substream id, call
sequence) produces bit-identical output on every supported platform and
compiler (same build / any build — the scope is as wide as the language
guarantees for unsigned arithmetic; replay/hash tests at the promised
scope land with M1-DET-04).

The committed golden vectors (`PrngGolden.First32Match`,
`PrngGolden.FNV1aOfFirst4096`) are the replay fixtures: seed
`0x1234567890ABCDEFull`, first 32 master-stream draws, and the FNV-1a
big-endian hash of the first 4096 draws (`0xB64E76173859B6D8`). A change
to the algorithm, taps, seeding, or derivation **must** fail them — that
is intended.

## Period

Every nonzero state has period exactly **2^128 - 1**. This is proven in
the committed suite (`PrngPeriod.StateMapHasPrimitiveCharacteristicPolynomial`),
not sampled: the state map's characteristic polynomial over GF(2) is
reconstructed from a probe orbit with Berlekamp-Massey, checked to
annihilate the map on all 128 basis states, and verified irreducible and
primitive (including the full prime factorization of `2^128 - 1` with a
portable 128-bit multiply and Miller-Rabin). A primitive characteristic
polynomial of an invertible linear map over GF(2) is exactly "every
nonzero state has maximal period". The `PrngPeriod.OutputStreamHasNoShortCycles`
test adds an empirical screen (no duplicate in 4M draws; no period-q
pattern for the small prime divisors of `2^128 - 1`).

Practical consequence: no two distinct nonzero states ever produce the
same output again within one period, and the output stream cannot have
any short cycle. Substreams of one seed are different points on the same
single cycle — for any realistic number of draws they are independent in
every statistical sense, but they are *not* cryptographically independent
(see Misuse warnings).

## API reference

`laige::Prng` — value type, owns three u64 words (seed + state).

| Member | Contract |
|---|---|
| `explicit Prng(std::uint64_t seed)` | Master stream for `seed`. O(1); the state is nonzero for every seed. |
| `std::uint64_t next_u64()` | One draw; advances the state. O(1). |
| `std::uint32_t next_range(std::uint32_t min, std::uint32_t max)` | Uniform in `[min, max)`; `max - min in [1, 2^32 - 1]`. `min >= max`: debug assert, documented UB in release (span underflow). Expected < 2 draws; the rejection probability per draw is `< 1/2` (it is `(2^64 mod n) / 2^64`, which is 0 for power-of-two spans). |
| `float next_float01()` | Uniform in `[0, 1)`, 24-bit resolution. Never 1.0; 0.0 with probability 2^-24. |
| `std::uint64_t seed() const` | The construction seed (save/replay identity). |
| `Prng substream(std::uint32_t id) const` | `deriveSubstream(seed(), id)`; a fresh stream position, independent of this stream's current state. |
| `static Prng deriveSubstream(std::uint64_t seed, std::uint32_t id)` | The documented derivation: `Prng(seed + id * K)`. Composes (see Algorithm). |
| `static void seedState(std::uint64_t seed, std::uint64_t& part1, std::uint64_t& part2)` | Seed-to-state mapping; exposed for determinism verification and M1 save/replay (a saved stream is `(seed, part1, part2)`). |
| `static void stepState(std::uint64_t& part1, std::uint64_t& part2)` | One transition step on a raw state; same exposure. |
| `static constexpr std::uint64_t kSplitmix64Increment` | `0x9E3779B97F4A7C15` (named: seeding + substream derivation step). |
| `static constexpr std::uint64_t kMixMultiplierA / kMixMultiplierB` | splitmix64 mixing constants (named: Stafford 2018). |
| `static constexpr float kFloat01Unit` | `2^-24` (named: `next_float01` resolution). |

Value semantics: copy/assign are O(1); a copy *shares the stream
position* (draws from a copy interleave with the original — see Misuse
warnings). No allocation, no global state.

## Performance (DOC-004)

- **Hot-path cost:** `next_u64()` is 4 XOR/shift, 1 add (state update) +
  1 add (output) — a handful of integer ops, no branches. `next_range()`
  adds one divide and an expected `< 2` draws; `next_float01()` adds one
  64-bit shift and one float multiply by a power of two.
- **Allocations:** none, ever (construction, draws, substreams).
  **Synchronization:** none. **I/O:** none.
- **Batching:** draw in a loop; there is no cheaper bulk API and none is
  planned (the per-draw cost is at the floor of the algorithm).
- **Budget guidance (PERF-002):** a subsystem drawing `d` times per tick
  spends `~10d` integer ops — negligible against the tick budget; the
  only spike risk is `next_range` with a span barely dividing 2^64,
  which is bounded anyway (rejection probability `< 1/2`, expected
  length `< 2`).

## Threading and ownership (CONC-001)

A `Prng` has exactly one owner thread and is **not** thread-safe. Give
each subsystem its own derived substream (that is what the ids are for);
never hand out copies of a live master to multiple owners. Cross-thread
sharing requires an explicit engine synchronization boundary (CONC-002),
which M1 defines per subsystem.

## Save / replay (PRD §10.3, M1-DET)

A stream's complete deterministic identity is `(seed(), substream id,
draw count)`. For bit-exact restoration of an in-flight stream, save
`(seed, part1, part2)` via `seedState`/`stepState` (a fresh
`Prng(seed)` advanced to the saved state) — `stepState` is public exactly
for this and for the period proof's reconstruction of the state map.

## Misuse warnings

- **Copying to "share" a substream interleaves the copies' draws.** One
  subsystem, one derived substream, no copies of live masters.
- **Not a CSPRNG.** Never for tokens, keys, session material, or
  anything security-sensitive (DEP-002: use an approved crypto PRNG).
- **`next_float01()` has 2^24 distinct values**, not a full-precision
  float: do not use it for continuous physics parameters that need more
  than 24 bits of spread (re-derive at higher resolution from
  `next_u64()` if needed).
- **Substream ids are stable identities.** Reusing an id for a different
  subsystem, or changing ids between sessions, changes the streams (and
  breaks replay).
- **`next_range(min, max)` with `min >= max`** is a debug assert and
  documented UB in release.

## Verification

`ctest -R prng` (suites `PrngGolden`, `PrngRange`, `PrngFloat01`,
`PrngSubstreams`, `PrngPeriod` in `tests/laige-core/prng_tests.cpp`):
golden vectors + FNV-1a replay hash, 10^4-step reference transcription
check, nonzero-seeding spot check, KATs and unbiasedness of
`next_range`, dyadic exactness and bounds of `next_float01`, substream
derivation KATs + composition + overlap sanity, and the committed
period proof + output-stream short-cycle screen. Green under ASan/UBSan
and TSan in the standard build trees (TSan: the suite is single-threaded
and must stay race-free).
