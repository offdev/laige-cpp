// laige-core build/version identifier (M0-BUILD-01).
//
// Foundational build plumbing — NOT part of the functional engine API
// (Result/Status, math, pools, logging, config land in M0-CORE-xx). It
// exists so the laige-core library target has a real, linkable symbol in
// both the static and shared variants, and a unit test to assert against
// (M0 milestone rule: code that compiles with -Wall -Werror
// -fno-exceptions -fno-rtti, unit tests in the same change).

#pragma once

namespace laige::core {

// Semver components of the laige-core module (0 = not yet released).
inline constexpr int kMajor = 0;
inline constexpr int kMinor = 0;
inline constexpr int kPatch = 0;

// "MAJOR.MINOR.PATCH", rendered once on first call.
//
// Contract: O(1), no allocation after the first call, safe from any
// thread (magic-static initialization). Not for use in hot paths —
// intended for startup/diagnostic display.
[[nodiscard]] const char* versionString();

}  // namespace laige::core
