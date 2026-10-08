// M2-TEXT-01 (ADR 0009): the single translation unit that compiles
// stb_truetype's implementation (the pinned vendored header,
// deps/stb/stb_truetype.h, deps.lock).
//
// Pinned vendor code (DEP-005) keeps the plain compiler policy — the
// laige-glad precedent (ADR 0007): the engine's -Wall -Werror policy
// does not gate upstream code, and the vendor tree stays byte-pinned
// by the deps.lock hash. The engine's boundary is font.cpp (which
// includes the same header for its DECLARATIONS and links this
// target); the public header names no stb type (CPP-010, DEP-004).

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
