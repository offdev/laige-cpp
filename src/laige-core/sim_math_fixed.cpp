// laige-core SimMath — fpx16_16 backend instantiation (M0-CORE-04).
//
// This translation unit pins the fpx16_16 backend to the library the
// same way sim_math.cpp pins fp32_pinned: one template instantiation
// per backend (ADR 0002, PERF-006) emitted into a linkable translation
// unit (M0-BUILD-01 pattern), exercised by the link smoke test in
// tests/laige-core/ for both the static and shared variants (NFR-8.9).
//
// The fpx16_16 ops are integer arithmetic with defined behavior for
// every input (fpx16_16.h), so no pinned floating-point flags are
// needed for this backend (ADR 0002); consumers that instantiate
// SimMath<Fpx16_16> need no special compiler flags either.

#include "laige/sim_math.h"

namespace laige::sim {

// One template instantiation per backend (ADR 0002, PERF-006): emits
// every SimMath<Fpx16_16> op into this translation unit.
template struct SimMath<Fpx16_16>;

}  // namespace laige::sim
