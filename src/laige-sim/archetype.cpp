// laige-sim World archetype SoA storage (M1-ECS-03).
//
// Implementation of the archetype helpers declared in
// include/laige/sim/entity.h — see that header and
// include/laige/sim/archetype.h for the full contracts (layout,
// reserve policy, budgets, complexity, determinism, failure table).

#include "laige/sim/entity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "laige/logging.h"

namespace laige {

namespace {

// The stable subsystem name for ECS events (LOG-001).
inline constexpr const char* kEcsSubsystem = "ecs";

// FNV-1a 32-bit parameters (CORE-005; the standard FNV-1a constants —
// fnv.org). The fingerprint only short-circuits the signature scan;
// correctness always rests on the lexicographic verify, so a
// fingerprint collision is harmless.
inline constexpr std::uint32_t kFnvBasis = 2166136261u;
inline constexpr std::uint32_t kFnvPrime = 16777619u;

// The FNV-1a 32-bit fingerprint of a sorted signature.
inline std::uint32_t sigFingerprint(const std::uint32_t* sig,
                                    std::uint16_t count) noexcept {
  std::uint32_t h = kFnvBasis;
  for (std::uint16_t i = 0; i < count; ++i) {
    h ^= sig[i];
    h *= kFnvPrime;
  }
  return h;
}

// splitmix64 (Seiler, 2018; the canonical 64-bit integer mixer,
// splitmix13.org): a deterministic bijection used to hash the type-key
// address value into the key-index slot. The input value differs per
// process (ASLR) — that is fine: the table is never iterated and
// correctness rests on key equality only (componentIdOfKey contract).
inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// Align `raw` up to kArchetypeColumnAlignment (the raw block is
// over-allocated by kArchetypeColumnPad, so the aligned address always
// stays inside it).
inline std::byte* alignBlock(std::byte* raw) noexcept {
  const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(raw);
  const std::uintptr_t mask = kArchetypeColumnAlignment - 1;
  const std::uintptr_t aligned = (addr + mask) & ~mask;
  return raw + static_cast<std::ptrdiff_t>(aligned - addr);
}

}  // namespace

std::uint32_t World::componentIdOfKey(const void* key) const noexcept {
  // Open-addressing probe: deterministic splitmix64 slot + linear
  // probing until an empty slot (absent) or the exact key (present).
  // Never iterated — lookup only (PERF-006; the PRD §10.3 "deterministic
  // hash + fixed iteration, or banned" allowance, fixed form).
  std::uint32_t slot = static_cast<std::uint32_t>(
      splitmix64(reinterpret_cast<std::uint64_t>(key)) &
      (detail::kComponentKeyIndexSize - 1));
  for (std::uint32_t probe = 0; probe < detail::kComponentKeyIndexSize;
       ++probe) {
    const detail::ComponentKeySlot& entry = keyIndex_[slot];
    if (entry.key == nullptr) return 0;  // empty slot: absent
    if (entry.key == key) return entry.id;
    slot = (slot + 1) & (detail::kComponentKeyIndexSize - 1);
  }
  return 0;  // unreachable: load factor <= 1/2
}

void World::noteComponentKey(const void* key, std::uint32_t id) noexcept {
  // Setup path only (registerComponent): insert at the first empty
  // slot on the probe path. A duplicate key cannot reach here —
  // duplicate registration is an error (M1-ECS-02).
  std::uint32_t slot = static_cast<std::uint32_t>(
      splitmix64(reinterpret_cast<std::uint64_t>(key)) &
      (detail::kComponentKeyIndexSize - 1));
  for (std::uint32_t probe = 0; probe < detail::kComponentKeyIndexSize;
       ++probe) {
    if (keyIndex_[slot].key == nullptr) {
      keyIndex_[slot] = detail::ComponentKeySlot{key, id};
      return;
    }
    slot = (slot + 1) & (detail::kComponentKeyIndexSize - 1);
  }
  // Unreachable: load factor <= 1/2.
}

detail::ArchetypeRecord* World::findArchetype(const std::uint32_t* sig,
                                              std::uint16_t count) noexcept {
  const std::uint32_t fp = sigFingerprint(sig, count);
  // Bounded scan over the created archetypes (id order — creation
  // order; O(kMaxArchetypes), no allocation, PERF-007 documented).
  for (std::uint32_t i = 0; i < archetypeCount_; ++i) {
    const detail::ArchetypeRecord& rec = archetypes_[i];
    if (rec.fingerprint != fp || rec.sigCount != count) continue;
    bool equal = true;
    for (std::uint16_t j = 0; j < count; ++j) {
      if (rec.sig[j] != sig[j]) {
        equal = false;
        break;
      }
    }
    if (equal) return const_cast<detail::ArchetypeRecord*>(&rec);
  }
  return nullptr;
}

detail::ArchetypeRecord* World::createArchetype(const std::uint32_t* sig,
                                                std::uint16_t count) noexcept {
  // The archetype table is pre-allocated (kMaxArchetypes records,
  // value-initialized) at World::create — this is the first creation
  // of a component set: assign the next id (creation order —
  // deterministic for a fixed operation sequence, ARCH-010).
  detail::ArchetypeRecord& rec = archetypes_[archetypeCount_];
  for (std::uint16_t i = 0; i < count; ++i) rec.sig[i] = sig[i];
  for (std::uint16_t i = count; i < kMaxArchetypeComponents; ++i) {
    rec.sig[i] = 0;  // keep the 0-termination explicit
  }
  rec.sigCount = count;
  rec.fingerprint = sigFingerprint(sig, count);
  // The reserve policy (archetype.h): start at kInitialArchetypeRows,
  // or the world capacity when smaller (a small world never outgrows
  // the initial reserve — growth would be a no-op there).
  rec.rowCapacity =
      capacity_ < kInitialArchetypeRows ? capacity_ : kInitialArchetypeRows;
  if (rec.rowCapacity == 0) rec.rowCapacity = 1;  // defensive: no live entity
  rec.size = 0;
  rec.slotCol = std::make_unique<std::uint16_t[]>(rec.rowCapacity);
  rec.columns = std::make_unique<detail::ArchetypeColumn[]>(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    // Column geometry comes from the registry record (M1-ECS-02):
    // packed rows of sizeof(T), aligned to alignof(T) — which the
    // registerComponent static_assert bounded to <= kArchetypeColumnAlignment.
    const detail::ComponentRecord& comp = components_[sig[i] - 1];
    detail::ArchetypeColumn& column = rec.columns[i];
    column.block = std::make_unique<std::byte[]>(
        static_cast<std::size_t>(rec.rowCapacity) * comp.size +
        kArchetypeColumnPad);
    column.base = alignBlock(column.block.get());
    column.size = comp.size;
  }
  ++archetypeCount_;
  totalReservations_ += 1 + count;  // slot column + one block per component
  LAIGE_LOG_INFO(kEcsSubsystem, "archetype_created",
                 "New archetype for a component set seen for the first time",
                 laige::log::field("archetype_id", archetypeCount_),
                 laige::log::field("components", count));
  return &rec;
}

std::uint32_t World::columnIndexOf(const detail::ArchetypeRecord& arch,
                                   std::uint32_t componentId) const noexcept {
  // Binary search over the sorted signature (component ids are
  // registration-order dense, so sorted = registration order within
  // the set). O(log kMaxArchetypeComponents); no allocation.
  std::uint32_t lo = 0, hi = arch.sigCount;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (arch.sig[mid] < componentId) {
      lo = mid + 1;
    } else if (arch.sig[mid] > componentId) {
      hi = mid;
    } else {
      return mid;
    }
  }
  return kInvalidColumnIndex;
}

bool World::growArchetype(detail::ArchetypeRecord& arch,
                          std::uint32_t archetypeId) noexcept {
  // The reserve policy (archetype.h): double the row capacity, capped
  // at the world's entity capacity — one bounded reservation per
  // column, then move the live rows. Bounded per archetype by
  // log2(worldCapacity / kInitialArchetypeRows) + 1 growth events.
  const std::uint32_t newCapacity =
      std::min(capacity_, arch.rowCapacity * 2);
  if (newCapacity <= arch.rowCapacity) {
    // The archetype already spans the whole world: no further row can
    // exist (every live entity would have to leave it to free one —
    // unreachable for the caller's add). The caller degrades to
    // BudgetExhausted.
    return false;
  }
  // All new blocks are reserved before any old block is released, so a
  // failure mid-growth cannot corrupt live rows (allocation failure
  // itself terminates under the no-exceptions policy, like every other
  // engine allocation).
  auto newSlotCol = std::make_unique<std::uint16_t[]>(newCapacity);
  for (std::uint16_t i = 0; i < arch.sigCount; ++i) {
    detail::ArchetypeColumn& column = arch.columns[i];
    auto block = std::make_unique<std::byte[]>(
        static_cast<std::size_t>(newCapacity) * column.size +
        kArchetypeColumnPad);
    std::byte* base = alignBlock(block.get());
    std::memcpy(base, column.base,
                static_cast<std::size_t>(arch.size) * column.size);
    column.block = std::move(block);  // releases the old block
    column.base = base;
  }
  std::memcpy(newSlotCol.get(), arch.slotCol.get(),
              static_cast<std::size_t>(arch.size) * sizeof(std::uint16_t));
  arch.slotCol = std::move(newSlotCol);
  arch.rowCapacity = newCapacity;
  ++totalArchetypeGrowth_;
  totalReservations_ += 1 + arch.sigCount;
  LAIGE_LOG_INFO(kEcsSubsystem, "archetype_grow",
                 "Archetype row capacity doubled (bounded reservation)",
                 laige::log::field("archetype_id", archetypeId),
                 laige::log::field("rows", newCapacity),
                 laige::log::field("components", arch.sigCount));
  return true;
}

void World::removeRow(detail::ArchetypeRecord& arch, std::uint32_t row) noexcept {
  // Shift the tail left by one row: the slot column (2 B/row) plus one
  // pass per live component column (size B/row) — (size - row - 1) rows
  // move — then re-sync the rowOf_ records of the shifted rows (the
  // dual representation of the slot column — the rowOf_ table must
  // agree with slotCol, invariant I2 below).
  // O((size - row) * (row-stride + 2 B)) moved + O(size - row) re-sync;
  // no allocation.
  // Components are trivially copyable, so the vacated row's bytes need
  // no destruction.
  const std::size_t tailRows = static_cast<std::size_t>(arch.size - row - 1);
  if (tailRows > 0) {
    std::memmove(arch.slotCol.get() + row, arch.slotCol.get() + row + 1,
                 tailRows * sizeof(std::uint16_t));
    for (std::uint16_t i = 0; i < arch.sigCount; ++i) {
      detail::ArchetypeColumn& column = arch.columns[i];
      std::memmove(column.base + static_cast<std::size_t>(row) * column.size,
                   column.base + static_cast<std::size_t>(row + 1) * column.size,
                   tailRows * column.size);
    }
    totalRowShifts_ += tailRows;  // accounted work (ArchetypeStats)
  }
  --arch.size;
  for (std::uint32_t r = row; r < arch.size; ++r) {
    rowOf_[arch.slotCol[r]] = r;  // the shifted rows' records follow
  }
}

std::uint32_t World::attachSlot(std::uint32_t slot,
                                detail::ArchetypeRecord& arch) noexcept {
  const std::uint32_t archId = static_cast<std::uint32_t>(
      std::distance(archetypes_.get(), &arch)) + 1;
  if (arch.size == arch.rowCapacity && !growArchetype(arch, archId)) {
    // The at-world-capacity edge (caller: BudgetExhausted). kInvalidRowIndex
    // (not 0): row 0 is a valid row index.
    return kInvalidRowIndex;
  }
  // Slot-ordered insertion position: the first row with a greater slot
  // id (the slot is absent from this archetype — a slot lives in
  // exactly one archetype, and this one is the destination).
  // O(log size) binary search over the strictly ascending slot column.
  std::uint32_t lo = 0, hi = arch.size;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (arch.slotCol[mid] <= slot) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const std::uint32_t row = lo;
  // Shift the tail RIGHT by one row: the existing rows [row, size-1]
  // (size - row of them) move to [row+1, size] — the mirror of
  // removeRow's left shift: memmove from the old position (row) to the
  // new one (row+1); memmove handles the overlap. 0 when appending at
  // the end (row == size).
  const std::size_t tailRows = static_cast<std::size_t>(arch.size - row);
  if (tailRows > 0) {
    std::memmove(arch.slotCol.get() + row + 1, arch.slotCol.get() + row,
                 tailRows * sizeof(std::uint16_t));
    for (std::uint16_t i = 0; i < arch.sigCount; ++i) {
      detail::ArchetypeColumn& column = arch.columns[i];
      std::memmove(column.base + static_cast<std::size_t>(row + 1) * column.size,
                   column.base + static_cast<std::size_t>(row) * column.size,
                   tailRows * column.size);
    }
    totalRowShifts_ += tailRows;  // accounted work (ArchetypeStats)
  }
  arch.slotCol[row] = static_cast<std::uint16_t>(slot);
  ++arch.size;
  for (std::uint32_t r = row; r < arch.size; ++r) {
    rowOf_[arch.slotCol[r]] = r;  // covers the new row and the tail
  }
  archetypeOf_[slot] = static_cast<std::uint16_t>(archId);
  return row;
}

std::uint32_t World::archetypeCount() const noexcept { return archetypeCount_; }

ArchetypeStats World::archetypeStats() const noexcept {
  ArchetypeStats s{};
  s.archetypeCount = archetypeCount_;
  // Bounded cold pass (O(kMaxArchetypes * kMaxArchetypeComponents));
  // no allocation. bytesReserved counts reserved row bytes: the slot
  // column (2 B/row) plus each component column (size B/row) —
  // alignment padding is excluded (it is over-allocation, not rows).
  for (std::uint32_t i = 0; i < archetypeCount_; ++i) {
    const detail::ArchetypeRecord& rec = archetypes_[i];
    s.rowsLive += rec.size;
    s.rowsReserved += rec.rowCapacity;
    std::uint64_t stride = sizeof(std::uint16_t);
    for (std::uint16_t c = 0; c < rec.sigCount; ++c) {
      stride += rec.columns[c].size;
    }
    s.bytesReserved += static_cast<std::uint64_t>(rec.rowCapacity) * stride;
  }
  s.totalAdds = totalAdds_;
  s.totalRemoves = totalRemoves_;
  s.totalArchetypeGrowth = totalArchetypeGrowth_;
  s.totalReservations = totalReservations_;
  s.totalRowShifts = totalRowShifts_;
  return s;
}

}  // namespace laige
