// M1-DET-01 compile check, NEGATIVE case (must FAIL to compile).
//
// A LAIGE_DETERMINISM_SAFE mark whose member list contains a
// `double`: the mark verifies its member list at the mark site
// (determinism.h) — the static_assert in the specialization fires
// the first time the trait is instantiated (the registerSystem call
// below), BEFORE the component's I/O is checked by the system — the
// fail-early half of the G-R8 trait mechanism. Compiled (not linked)
// by the trait_compile_reject_bad_mark CTest check.

#include "laige/result.h"
#include "laige/sim/determinism.h"
#include "laige/sim/entity.h"
#include "laige/sim/system.h"

struct DetBadMark {
  double v{};
};
LAIGE_COMPONENT(DetBadMark);
// The mark lists the member honestly — and is therefore rejected at
// the mark site (a `double` member is never determinism-safe).
LAIGE_DETERMINISM_SAFE(DetBadMark, double);

LAIGE_SYSTEM(DetBadMarkSys, 1)
void DetBadMarkSys(laige::World& world, laige::SystemContext& ctx) {
  static_cast<void>(world);
  static_cast<void>(ctx);
}

int main() {
  // This call instantiates IsDeterminismSafe<DetBadMark> (through
  // the IoComponentSafety fold in registerSystem), which evaluates
  // the mark-site static_assert above.
  laige::World::Options opts;
  opts.capacity = 8;
  laige::Result<laige::World, laige::ErrorCode> w =
      laige::World::create(opts);
  if (!w.ok()) return 1;
  const laige::Result<laige::SystemId, laige::ErrorCode> reg =
      std::move(w).takeValue().registerSystem(
          DetBadMarkSys_Def,
          laige::Io<DetBadMark, laige::Access::Read>{});
  return reg.ok() ? 0 : 1;
}
