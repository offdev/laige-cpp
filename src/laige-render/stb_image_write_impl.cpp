// M2-GOLD-01: the single translation unit that compiles
// stb_image_write's implementation (the pinned vendored header,
// deps/stb/stb_image_write.h, deps.lock).
//
// Pinned vendor code (DEP-005) keeps the plain compiler policy — the
// laige-glad and stb_truetype precedents (ADR 0007, ADR 0009): the
// engine's -Wall -Werror policy does not gate upstream code, and the
// vendor tree stays byte-pinned by the deps.lock hash. The engine
// boundary for the PNG encode is the laige-golden test tool
// (tools/golden), which includes the same header for its
// DECLARATIONS and links this target (DEP-004).

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
