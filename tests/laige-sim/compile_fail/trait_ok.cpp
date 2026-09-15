// M1-DET-01 compile check, POSITIVE case (must COMPILE — exit 0).
//
// A component whose storage is determinism-safe — integer and
// SimMath-registered scalar members, marked with
// LAIGE_DETERMINISM_SAFE at the mark site (determinism.h) — passes
// the G-R8 static_assert in World::registerSystem (entity.h). This
// fixture is compiled (not linked, not run) by the trait_compile_ok
// CTest check with the engine policy flags.

#include <cstdint>

#include "laige/result.h"
#include "laige/sim/determinism.h"
#include "laige/sim/entity.h"
#include "laige/sim/system.h"

struct DetGood {
  std::int32_t a{};
  laige::fpx16_16 b{};
};
LAIGE_COMPONENT(DetGood);
// M1-DET-01 (G-R8): the member list IS the type's storage.
LAIGE_DETERMINISM_SAFE(DetGood, std::int32_t, laige::fpx16_16);

LAIGE_SYSTEM(DetGoodSys, 1)
void DetGoodSys(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx.each<DetGood>(
      [](laige::Entity e, const DetGood& g) {
        static_cast<void>(e);
        static_cast<void>(g);
      },
      laige::Read{}));
}

int main() {
  laige::World::Options opts;
  opts.capacity = 8;
  laige::Result<laige::World, laige::ErrorCode> w =
      laige::World::create(opts);
  if (!w.ok()) return 1;
  const laige::Result<laige::SystemId, laige::ErrorCode> reg =
      std::move(w).takeValue().registerSystem(
          DetGoodSys_Def, laige::Io<DetGood, laige::Access::Read>{});
  return reg.ok() ? 0 : 1;
}
