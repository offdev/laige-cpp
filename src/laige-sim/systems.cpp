// laige-sim system registry (M1-SYS-01) + system scheduler (M1-SYS-02).
//
// The non-template World methods of the system framework:
//   - systemCount, system, the SystemInfo snapshot queries
//     (M1-SYS-01)
//   - the depends_on spec parse (detail::parseDepSpec /
//     detail::depSpecErrorName — declared in system.h) and the
//     scheduler (scheduleSystems, runSystems) — M1-SYS-02. The
//     registration template (registerSystem, with the resolveIoEntry
//     helper) is header-defined in entity.h (the M1-ECS-02 pattern).
// See include/laige/sim/system.h for the full contract (the id and
// determinism contracts, the validation orders, the I/O sets, the
// scheduler section) and docs/api/scheduler.md for the API contract.

#include "laige/sim/entity.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "laige/budget_harness.h"  // M1-SYS-03: TimeIt (the scope timer)
#include "laige/logging.h"

namespace laige {

std::uint32_t World::systemCount() const noexcept {
  return systemCount_;
}

Result<SystemInfo, ErrorCode> World::system(SystemId id) const noexcept {
  // Ids are dense from 1, so a valid registered id is exactly the
  // range [1, systemCount_] (system.h contract; the componentInfo
  // precedent: an invalid id is a pure-query failure, no warn).
  if (id.value == 0 || id.value > systemCount_ || systems_ == nullptr) {
    return ErrorCode::InvalidArgument;
  }
  const detail::SystemRecord& rec = systems_[id.value - 1];
  SystemInfo info;
  info.def = rec.def;  // value copy: the snapshot owns its def
  info.id = id;
  info.readComponents_ = rec.readComponents;
  info.writeComponents_ = rec.writeComponents;
  return info;
}

bool SystemInfo::declaresRead(ComponentTypeId componentId) const noexcept {
  // The id is 1..kMaxComponentTypes by the component.h contract
  // (IdSet256::contains treats 0 as "not a member" — a pure query).
  return readComponents_.contains(componentId.value);
}

bool SystemInfo::declaresWrite(ComponentTypeId componentId) const noexcept {
  return writeComponents_.contains(componentId.value);
}

namespace detail {

const char* depSpecErrorName(DepSpecError error) noexcept {
  switch (error) {
    case DepSpecError::EmptyToken: return "empty_token";
    case DepSpecError::DuplicateToken: return "duplicate_token";
    case DepSpecError::TooMany: return "too_many";
    case DepSpecError::Ok: return "ok";
  }
  return "ok";  // unreachable: the enum is exhaustive
}

DepSpecError parseDepSpec(const char* spec, DepSpecParse* out) noexcept {
  // No partial parse: the out parameter is empty on every error (the
  // callers treat a non-Ok outcome as "no usable dependencies").
  out->count = 0;
  if (spec == nullptr) return DepSpecError::Ok;
  bool afterComma = false;
  for (;;) {
    // Trim leading ASCII whitespace (space, tab, CR, LF).
    while (*spec != '\0' &&
           (*spec == ' ' || *spec == '\t' || *spec == '\r' || *spec == '\n')) {
      ++spec;
    }
    const char* start = spec;
    while (*spec != '\0' && *spec != ',') ++spec;
    char* end = const_cast<char*>(spec);
    // Trim trailing ASCII whitespace.
    while (end > start &&
           (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\r' ||
            *(end - 1) == '\n')) {
      --end;
    }
    if (end == start) {
      // An empty token. Legal only as the whole spec (nullptr/""/
      // whitespace = no dependencies); after a comma it is a
      // malformed spec (a trailing comma is a typo).
      if (!afterComma && *spec == '\0') break;
      return DepSpecError::EmptyToken;
    }
    const std::uint32_t len = static_cast<std::uint32_t>(end - start);
    // The dependency list is a set, not a multiset (the declared-I/O
    // precedent, system.h).
    for (std::uint32_t i = 0; i < out->count; ++i) {
      if (out->lengths[i] == len &&
          std::memcmp(out->names[i], start, len) == 0) {
        return DepSpecError::DuplicateToken;
      }
    }
    if (out->count >= kMaxSystemDependencies) {
      return DepSpecError::TooMany;
    }
    out->names[out->count] = start;  // points into the spec (stable)
    out->lengths[out->count] = len;
    ++out->count;
    if (*spec == '\0') break;
    ++spec;  // consume the comma
    afterComma = true;
  }
  return DepSpecError::Ok;
}

}  // namespace detail

namespace {

// True when the (not necessarily NUL-terminated) token `token` of
// length `len` equals the NUL-terminated registered name `name`.
// A prefix of a longer name is NOT a match ("A" != "AB") — the
// length must agree exactly.
bool tokenMatchesName(const char* token, std::uint32_t len,
                      const char* name) noexcept {
  for (std::uint32_t i = 0; i < len; ++i) {
    if (token[i] != name[i]) return false;
  }
  return name[len] == '\0';
}

// Append the NUL-terminated `s` to `buf` (capacity `cap` including
// the NUL), truncating beyond it (LOG-005: user-controlled text is
// bounded — the names are compile-time string literals, but the log
// fields stay small).
void appendBounded(char* buf, std::size_t cap, const char* s) noexcept {
  std::size_t len = 0;
  while (buf[len] != '\0' && len + 1 < cap) ++len;
  while (*s != '\0' && len + 1 < cap) {
    buf[len++] = *s++;
  }
  buf[len] = '\0';
}

// Copy the first `len` characters of the (not NUL-terminated) token
// into `buf` (capacity `cap` including the NUL), truncating beyond it.
void copyTokenBounded(char* buf, std::size_t cap, const char* token,
                      std::uint32_t len) noexcept {
  std::size_t i = 0;
  while (i < len && i + 1 < cap) {
    buf[i] = token[i];
    ++i;
  }
  buf[i] = '\0';
}

}  // namespace

// ---------------------------------------------------------------------------
// M1-SYS-02: the system scheduler.
//
// scheduleSystems: the stable topological sort of the registration
// order plus the declared depends_on edges (Kahn, min-id tie-break)
// plus the pre-run validation (system.h "Scheduler"; the normative
// order is documented there). All state is fixed-size and stack/
// world-scoped: no allocation (setup path, PERF-003).
// ---------------------------------------------------------------------------

Status World::scheduleSystems(SystemSchedule& out) const noexcept {
  out = SystemSchedule{};  // zeroed: an error leaves a well-defined out
  const std::uint32_t n = systemCount_;
  out.systemCount = n;
  if (n == 0) return Status{};  // the empty world schedules empty

  // 1) Resolve every system's dependency names (system id ascending;
  //    the first failure in ascending (system id, spec position)
  //    order wins — system/dep_missing).
  std::uint16_t depIds[kMaxSystems][kMaxSystemDependencies];
  std::uint8_t depCount[kMaxSystems]{};
  for (std::uint32_t i = 1; i <= n; ++i) {
    detail::DepSpecParse parsed;
    // The spec was validated at registration (registerSystem) and the
    // registry is immutable after setup: the parse cannot fail here.
    // Fail loudly anyway (CORE-008: never trust).
    const detail::DepSpecError err =
        detail::parseDepSpec(systems_[i - 1].def.dependsOn, &parsed);
    if (err != detail::DepSpecError::Ok) {
      LAIGE_LOG_WARN("system", "dep_spec_invalid",
                     "System depends_on spec failed to re-parse during "
                     "scheduling (should be unreachable — the spec was "
                     "validated at registration); rebuild the def",
                     laige::log::field("name", systems_[i - 1].def.name),
                     laige::log::field("error",
                                       detail::depSpecErrorName(err)));
      return ErrorCode::InvalidArgument;
    }
    depCount[i - 1] = static_cast<std::uint8_t>(parsed.count);
    for (std::uint32_t d = 0; d < parsed.count; ++d) {
      std::uint32_t resolved = 0;
      for (std::uint32_t j = 1; j <= n; ++j) {
        if (tokenMatchesName(parsed.names[d], parsed.lengths[d],
                             systems_[j - 1].def.name)) {
          resolved = j;
          break;
        }
      }
      if (resolved == 0) {
        // The dependency names a system that is not registered in
        // this world (a name from another world, or a typo).
        char missing[64];  // LOG-005 bound on the logged token
        copyTokenBounded(missing, sizeof(missing), parsed.names[d],
                         parsed.lengths[d]);
        LAIGE_LOG_WARN("system", "dep_missing",
                       "System depends on a name that is not registered "
                       "in this world; register the dependency in this "
                       "world or remove it from the depends_on list",
                       laige::log::field("system",
                                         systems_[i - 1].def.name),
                       laige::log::field("missing_dep", missing),
                       laige::log::field("position", d));
        return ErrorCode::InvalidArgument;
      }
      depIds[i - 1][d] = static_cast<std::uint16_t>(resolved);
    }
  }

  // 2) The stable topological sort (Kahn with a min-id tie-break):
  //    repeatedly place the smallest unrun system whose dependencies
  //    are all placed. A system only moves later, behind its
  //    dependencies; with no dependencies the order is exactly the
  //    registration order (system.h "Scheduler").
  std::uint16_t pending[kMaxSystems];
  std::uint8_t scheduled[kMaxSystems]{};
  for (std::uint32_t i = 0; i < n; ++i) {
    pending[i] = static_cast<std::uint16_t>(depCount[i]);
  }
  std::uint32_t placed = 0;
  while (placed < n) {
    std::uint32_t picked = 0;
    for (std::uint32_t i = 1; i <= n; ++i) {
      if (scheduled[i - 1] == 0 && pending[i - 1] == 0) {
        picked = i;
        break;
      }
    }
    if (picked == 0) break;  // the remainder is a cycle (below)
    out.order[placed++] = picked;
    scheduled[picked - 1] = 1;
    for (std::uint32_t j = 1; j <= n; ++j) {
      if (scheduled[j - 1] != 0) continue;
      for (std::uint32_t d = 0; d < depCount[j - 1]; ++d) {
        if (depIds[j - 1][d] == picked) {
          --pending[j - 1];
          break;  // the dep lists are sets (registration-validated)
        }
      }
    }
  }
  if (placed < n) {
    // 2b) Report one concrete cycle: the deterministic walk from the
    //     smallest remaining id, following each system's first
    //     spec-listed dependency that is still remaining (a remaining
    //     system always has at least one remaining dependency — the
    //     Kahn invariant).
    std::uint16_t path[kMaxSystems]{};
    std::uint32_t pathLen = 0;
    std::uint32_t cycleStart = 0;
    std::uint32_t cur = 0;
    for (std::uint32_t i = 1; i <= n; ++i) {
      if (scheduled[i - 1] == 0) {
        cur = i;
        break;
      }
    }
    for (;;) {
      for (std::uint32_t k = 0; k < pathLen; ++k) {
        if (path[k] == cur) {
          cycleStart = k;
          cur = 0;  // signal: cycle closed
          break;
        }
      }
      if (cur == 0) break;
      path[pathLen++] = static_cast<std::uint16_t>(cur);
      std::uint32_t next = 0;
      for (std::uint32_t d = 0; d < depCount[cur - 1]; ++d) {
        const std::uint16_t dep = depIds[cur - 1][d];
        if (scheduled[dep - 1] == 0) {
          next = dep;
          break;
        }
      }
      cur = next;  // nonzero by the Kahn invariant
    }
    // Join the cycle's systems (walk order, starting at the cycle's
    // smallest entry) into one bounded log field.
    char cycleBuf[256];
    cycleBuf[0] = '\0';
    for (std::uint32_t k = cycleStart; k < pathLen; ++k) {
      if (k > cycleStart) appendBounded(cycleBuf, sizeof(cycleBuf), ",");
      appendBounded(cycleBuf, sizeof(cycleBuf),
                    systems_[path[k] - 1].def.name);
    }
    LAIGE_LOG_WARN("system", "dependency_cycle",
                   "The depends_on graph has a cycle; no execution "
                   "order can satisfy the dependencies — break the "
                   "cycle (the listed systems, in walk order)",
                   laige::log::field("cycle", cycleBuf));
    return ErrorCode::InvalidArgument;
  }

  // 3) The double-writer check: two systems both declaring Write of
  //    the same component in one tick — order-independent (the last
  //    write would silently win). First conflict in ascending
  //    component-id, then ascending writer-id order.
  for (std::uint32_t c = 1; c <= kMaxComponentTypes; ++c) {
    std::uint32_t firstWriter = 0;
    std::uint32_t secondWriter = 0;
    for (std::uint32_t i = 1; i <= n; ++i) {
      if (systems_[i - 1].writeComponents.contains(c)) {
        if (firstWriter == 0) {
          firstWriter = i;
        } else {
          secondWriter = i;
          break;
        }
      }
    }
    if (secondWriter != 0) {
      LAIGE_LOG_WARN("system", "double_writer",
                     "Two systems both declare Write of the same "
                     "component in one tick; the last write would "
                     "silently win the component — split the write "
                     "between the systems or merge them",
                     laige::log::field("component_id", c),
                     laige::log::field("first_writer",
                                       systems_[firstWriter - 1].def.name),
                     laige::log::field("second_writer",
                                       systems_[secondWriter - 1].def.name));
      return ErrorCode::InvalidArgument;
    }
  }

  // 4) (Warn only — scheduling succeeds) a declared read that the
  //    computed order places BEFORE a declared write of the same
  //    component: the read observes the previous tick's value. Each
  //    (reader, writer, component) triple warns once, in ascending
  //    component-id, reader-id, writer-id order.
  std::uint32_t pos[kMaxSystems]{};  // system id -> execution index
  for (std::uint32_t k = 0; k < n; ++k) {
    pos[out.order[k] - 1] = k;
  }
  for (std::uint32_t c = 1; c <= kMaxComponentTypes; ++c) {
    for (std::uint32_t r = 1; r <= n; ++r) {
      if (!systems_[r - 1].readComponents.contains(c)) continue;
      for (std::uint32_t w = 1; w <= n; ++w) {
        if (w == r) continue;
        if (!systems_[w - 1].writeComponents.contains(c)) continue;
        if (pos[r - 1] < pos[w - 1]) {
          LAIGE_LOG_WARN("system", "read_before_write",
                         "System reads a component that another system "
                         "writes later in the same tick; the read sees "
                         "the previous tick's value, not this tick's "
                         "write — declare depends_on (or register the "
                         "writer earlier) when the read must see the "
                         "write",
                         laige::log::field("reader",
                                           systems_[r - 1].def.name),
                         laige::log::field("writer",
                                           systems_[w - 1].def.name),
                         laige::log::field("component_id", c));
        }
      }
    }
  }
  return Status{};
}

// runSystems: one sim tick's system phase (M1-LOOP-01 calls it once
// per tick). Strictly one system at a time, in schedule order, on the
// world's single owner thread; a fresh non-owning SystemContext per
// system. O(n) dispatch plus the M1-SYS-03 per-system measurement (two
// steady_clock reads, one O(1) ring write, two comparisons per
// system); no allocation, no logging on success.
Status World::runSystems(const SystemSchedule& schedule) noexcept {
  // The schedule must describe the CURRENT registry: a systemCount
  // mismatch means systems were registered after the schedule was
  // computed (or it belongs to another world) — recompute it.
  if (schedule.systemCount != systemCount_) {
    LAIGE_LOG_WARN("system", "schedule_stale",
                   "The schedule was computed for a different registry; "
                   "the world's system count changed after "
                   "scheduleSystems — recompute the schedule",
                   laige::log::field("scheduled_systems",
                                     schedule.systemCount),
                   laige::log::field("current_systems", systemCount_));
    return ErrorCode::InvalidArgument;
  }
  const std::uint32_t count = schedule.systemCount;
  if (count == 0) return Status{};  // the empty world runs nothing
  // The order entries must be unique ids in 1..count (a hand-built
  // malformed schedule is a caller bug — reject it loudly, API-008).
  detail::IdSet256 seen{};
  for (std::uint32_t k = 0; k < count; ++k) {
    const std::uint32_t id = schedule.order[k];
    if (id == 0 || id > count || seen.contains(id)) {
      LAIGE_LOG_WARN("system", "schedule_invalid",
                     "The schedule order contains a missing or "
                     "duplicate system id; rebuild it with "
                     "scheduleSystems",
                     laige::log::field("slot", k),
                     laige::log::field("id", id));
      return ErrorCode::InvalidArgument;
    }
    seen.set(id);
  }
  for (std::uint32_t k = 0; k < count; ++k) {
    const std::uint32_t id = schedule.order[k];
    const detail::SystemRecord& rec = systems_[id - 1];
    SystemContext ctx{*this};  // per-tick, per-system, non-owning
    // M1-SYS-03: the system's own run time (the context is built
    // outside the window — the measurement is the run function
    // itself, not the dispatch bookkeeping).
    TimeIt timer;
    rec.def.run(*this, ctx);
    checkSystemBudget(id, timer.elapsedMs());
  }
  return Status{};
}

}  // namespace laige
