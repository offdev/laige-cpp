// M1-DET-01 compile check, NEGATIVE case (must FAIL to compile).
//
// A component whose members are individually determinism-safe (an
// integer) but which carries NO LAIGE_DETERMINISM_SAFE mark: the
// mark is the declaration that the member list IS the type's
// storage, so an unmarked user struct is never safe by default —
// the primary trait is false and World::registerSystem's G-R8
// static_assert (entity.h) must fire. Compiled (not linked) by the
// trait_compile_reject_unmarked CTest check.

#include <cstdint>

#include "laige/result.h"
#include "laige/sim/determinism.h"
#include "laige/sim/entity.h"
#include "laige/sim/system.h"

struct DetUnmarked {
  std::int32_t a{};  // safe member type — but the struct is unmarked
};
LAIGE_COMPONENT(DetUnmarked);
// Intentionally NO LAIGE_DETERMINISM_SAFE mark.

LAIGE_SYSTEM(DetUnmarkedSys, 1)
void DetUnmarkedSys(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  ctx.each<DetUnmarked>(
      [](laige::Entity e) { static_cast<void>(e); }, laige::Read{});
}

int main() {
  laige::World::Options opts;
  opts.capacity = 8;
  laige::Result<laige::World, laige::ErrorCode> w =
      laige::World::create(opts);
  if (!w.ok()) return 1;
  const laige::Result<laige::SystemId, laige::ErrorCode> reg =
      std::move(w).takeValue().registerSystem(
          DetUnmarkedSys_Def,
          laige::Io<DetUnmarked, laige::Access::Read>{});
  return reg.ok() ? 0 : 1;
}
