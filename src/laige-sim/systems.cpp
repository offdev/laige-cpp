// laige-sim system registry (M1-SYS-01).
//
// The non-template World methods of the system registry (systemCount,
// system) and the SystemInfo snapshot queries. The registration
// template (registerSystem, with the resolveIoEntry helper) is
// header-defined in entity.h (the M1-ECS-02/03 template precedent).
// See include/laige/sim/system.h for the full contract (the id and
// determinism contract, the validation order, the I/O sets).

#include "laige/sim/entity.h"

#include <cstdint>

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

}  // namespace laige
