// laige-core SimMath — fp32_pinned backend instantiation (M0-CORE-03).
//
// This translation unit pins the fp32_pinned backend in two ways:
//
//   1. It carries the pinned flag set of ADR 0002 (applied by
//      `laige_apply_simmath_policy()`: -ffp-contract=off
//      -fno-associative-math for GCC/Clang/AppleClang, /fp:precise for
//      MSVC), so the ops below compile exactly as written — never
//      FMA-contracted, never reassociated.
//   2. It gives the library a real, linkable instantiation of the
//      fp32_pinned backend (M0-BUILD-01 pattern: each functional step
//      carries a symbol in both the static and shared variants), which
//      the link smoke test in tests/laige-core/ exercises in both
//      variants (NFR-8.9).
//
// Consumers instantiate SimMath<Fp32Pinned> in their own translation
// units; those translation units must carry the same pinned flag set
// (PRD §10.3, ADR 0002 — see the header preamble).

#include "laige/sim_math.h"

namespace laige::sim {

// One template instantiation per backend (ADR 0002, PERF-006): emits
// every SimMath<Fp32Pinned> op into this pinned translation unit.
template struct SimMath<Fp32Pinned>;

}  // namespace laige::sim
