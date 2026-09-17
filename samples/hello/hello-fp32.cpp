// The float_pinned_32 build variant of the hello scenario
// (M1-DET-04).
//
// The SAME game source with the SimMath backend swapped by the build
// (ADR 0002: one op surface, two backends — the game code is written
// once against the interface). The canonical variant (hello.cpp, no
// defines) runs fixed_point_16_16; this TU runs float_pinned_32. The
// embedded config (hello.cpp) then carries the matching SimMathBackend,
// so the replay identity's math_backend_id (ADR 0002) records the
// backend the run was actually computed in.
//
// Its committed baseline: samples/hello/baselines/float_pinned_32/
// (generated the same way as the canonical baseline — the canonical
// Debug g++ build; see the baselines/README.md).
//
// The CI role: every P0 job compares this variant against its baseline
// (tests/sample `hello_baseline_fp32`), and the CI `detcheck` job
// pairs it with its other-compiler / sanitizer / Release builds
// (docs/benchmarks/determinism-matrix.md).

#define LAIGE_HELLO_BACKEND laige::sim::SimMathFp32
#define LAIGE_HELLO_BACKEND_ID laige::SimMathBackend::FloatPinned32
// The scenario's constants are small exact integers (|v| ≤ 32): the
// int → float conversion is exact for them (2^24 exact-int mantissa).
#define LAIGE_HELLO_FROM_INT32(V) static_cast<float>(V)

#include "hello.cpp"
