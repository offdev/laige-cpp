// laige-sim deterministic state hash (M1-DET-03).
//
// Implementation of World::stateHash declared in
// include/laige/sim/entity.h — see that header for the canonical
// encoding, the scope (exactly what enters the hash and what is
// deliberately excluded — history and non-authoritative state), and
// the performance contract, and docs/api/entity.md for the API
// document.
//
// House hash conventions (docs/testing.md, determinism_tests,
// laige-detcheck): FNV-1a 64 — offset basis 0xcbf29ce484222325, prime
// 0x100000001b3 (fnv.org). Word-stream values are fed big-endian byte
// order per u64; raw component bytes are fed in memory order (every P0
// target is little-endian — PRD §6). One streaming FNV state covers
// the whole stream (FNV-1a is a streaming hash, replay.cpp trailer
// precedent), so no intermediate buffer and no allocation.

#include "laige/sim/entity.h"  // the contract (this header)

#include <cstddef>
#include <cstdint>

namespace laige {

namespace {

// FNV-1a 64 constants (fnv.org — the house convention).
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// One streaming FNV-1a 64 state (stack value; no allocation).
struct Fnv1a64 {
  std::uint64_t h = kFnvOffsetBasis;

  // Feed one word, big-endian byte order (the house word-stream
  // convention).
  void word(std::uint64_t v) noexcept {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (v >> shift) & 0xFFull;
      h *= kFnvPrime;
    }
  }

  // Feed raw bytes in memory order (the component column bytes).
  void bytes(const std::uint8_t* p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      h ^= static_cast<std::uint64_t>(p[i]);
      h *= kFnvPrime;
    }
  }
};

// Lexicographic compare of two 0-terminated sorted signatures
// (entity.h "Canonical encoding" step 4): -1 when a < b, 0 when equal,
// 1 when a > b; a proper prefix sorts first.
int compareSig(const std::uint32_t* a, std::uint16_t countA,
               const std::uint32_t* b, std::uint16_t countB) noexcept {
  for (std::uint16_t i = 0; i < countA && i < countB; ++i) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  if (countA != countB) return countA < countB ? -1 : 1;
  return 0;
}

}  // namespace

std::uint64_t World::stateHash(std::uint64_t tick) const noexcept {
  Fnv1a64 h;
  // Step 1: the completed tick count.
  h.word(tick);
  // Step 2: the live entity count.
  h.word(inUse_);
  // Step 3: the live handles, ascending slot order (dead slots and
  // their generations are history — entity.h scope).
  for (std::uint32_t s = 0; s < capacity_; ++s) {
    if (alive_[s] != 0) {
      h.word(static_cast<std::uint64_t>(s));
      h.word(static_cast<std::uint64_t>(generations_[s]));
    }
  }
  // Step 4: per distinct live component set (every NON-EMPTY
  // archetype), ascending lexicographic signature order — the
  // canonical, history-independent order of the set (the assigned
  // archetype ids are first-seen history and are never hashed).
  std::uint32_t order[kMaxArchetypes]{};
  std::uint32_t nonEmpty = 0;
  for (std::uint32_t a = 0; a < archetypeCount_; ++a) {
    if (archetypes_[a].size > 0) order[nonEmpty++] = a;
  }
  // Insertion sort over the non-empty archetypes by signature (at most
  // kMaxArchetypes entries comparing ≤ kMaxArchetypeComponents ids —
  // a cold path, not the per-tick hot path).
  for (std::uint32_t i = 1; i < nonEmpty; ++i) {
    const std::uint32_t key = order[i];
    const detail::ArchetypeRecord& keyRec = archetypes_[key];
    std::uint32_t j = i;
    while (j > 0 &&
           compareSig(archetypes_[order[j - 1]].sig,
                      archetypes_[order[j - 1]].sigCount, keyRec.sig,
                      keyRec.sigCount) > 0) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = key;
  }
  for (std::uint32_t i = 0; i < nonEmpty; ++i) {
    const detail::ArchetypeRecord& arch = archetypes_[order[i]];
    h.word(arch.sigCount);
    for (std::uint16_t c = 0; c < arch.sigCount; ++c) {
      h.word(arch.sig[c]);
    }
    h.word(arch.size);
    // The raw component bytes: rows in ascending slot order (the
    // dense-id row order, archetype.h invariant I2), columns in
    // signature order.
    for (std::uint32_t row = 0; row < arch.size; ++row) {
      for (std::uint16_t c = 0; c < arch.sigCount; ++c) {
        const detail::ArchetypeColumn& col = arch.columns[c];
        h.bytes(reinterpret_cast<const std::uint8_t*>(
                    col.base + static_cast<std::size_t>(row) * col.size),
                col.size);
      }
    }
  }
  // Step 5: the per-system PRNG state, ascending system id (the draw
  // position is the replay state — PRD §10.3; systems without a
  // substream hash their absence, not nothing).
  for (std::uint32_t i = 0; i < systemCount_; ++i) {
    h.word(static_cast<std::uint64_t>(i + 1));
    const detail::SystemRecord& rec = systems_[i];
    if (rec.rng.has_value()) {
      h.word(1);
      h.word(rec.rng->seed());
      h.word(rec.rng->statePart1());
      h.word(rec.rng->statePart2());
    } else {
      h.word(0);
    }
  }
  return h.h;
}

}  // namespace laige
