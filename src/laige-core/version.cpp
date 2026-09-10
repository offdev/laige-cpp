#include "laige/core/version.h"

#include <cstdio>

namespace laige::core {

const char* versionString() {
  // Rendered exactly once into a static buffer: no steady-state allocation,
  // no leak (PERF-003, CORE-004).
  static const char* const kRendered = [] {
    static char kStore[16];  // "d.dd.dd" + NUL fits with wide margin.
    std::snprintf(kStore, sizeof(kStore), "%d.%d.%d", kMajor, kMinor, kPatch);
    return kStore;
  }();
  return kRendered;
}

}  // namespace laige::core
