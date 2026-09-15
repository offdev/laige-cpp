// M1-DET-01 compile check, NEGATIVE case (must FAIL to compile).
//
// A component whose storage contains a raw `double` is NOT
// determinism-safe: no SimMath backend uses `double` (ADR 0002), and
// the component carries no LAIGE_DETERMINISM_SAFE mark, so the
// primary trait is false. World::registerSystem's G-R8 static_assert
// (entity.h) must fire here with the actionable message pointing at
// docs/concepts/determinism.md. Compiled (not linked) by the
// trait_compile_reject_double CTest check.

#include "laige/result.h"
#include "laige/sim/determinism.h"
#include "laige/sim/entity.h"
#include "laige/sim/system.h"

struct DetBad {
  double v{};  // raw double — never determinism-safe (G-R8)
};
LAIGE_COMPONENT(DetBad);
// Intentionally NO LAIGE_DETERMINISM_SAFE mark: the primary trait
// (false) applies to the struct.

LAIGE_SYSTEM(DetBadSys, 1)
void DetBadSys(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

int main() {
  laige::World::Options opts;
  opts.capacity = 8;
  laige::Result<laige::World, laige::ErrorCode> w =
      laige::World::create(opts);
  if (!w.ok()) return 1;
  const laige::Result<laige::SystemId, laige::ErrorCode> reg =
      std::move(w).takeValue().registerSystem(
          DetBadSys_Def, laige::Io<DetBad, laige::Access::Read>{});
  return reg.ok() ? 0 : 1;
}
