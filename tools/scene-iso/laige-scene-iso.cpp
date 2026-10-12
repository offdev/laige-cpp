// laige-scene-iso (M2-SCENE-01): the reference-isometric-scene
// generator.
//
// The M2-SCENE-01 scope: deterministically generate the worst-case
// isometric reference scene from a seed (50k overlapping sprites on
// stepped terrain, 3 parallax layers, particle emitters, a UI panel)
// and emit it as data the engine can load (the versioned binary
// format, laige/render/iso_reference_scene.h — the in-memory
// builder + the simple binary serialization the tests use). The full
// scene FORMAT is M3; this tool is the M2 generator for that format.
//
// Usage:
//   laige-scene-iso --seed <u64> --variant <full|small> --out <path>
//
//   --seed <u64>           the scene's seed (any u64; 0 is valid):
//                         same seed, same bytes, every build and
//                         platform (ARCH-010 — the determinism the
//                         test fixtures rely on)
//   --variant <full|small> full = the 50k-sprite §8.1 worst-case
//                         scene; small = the 1k-sprite unit-test
//                         variant (the step scope's fast fixture)
//   --out <path>           the output file: written atomically (temp
//                         + rename in the same directory — a killed
//                         run never leaves a partial file)
//
// The tool builds the scene in memory, serializes it, writes it, then
// reads the file back and verifies the round trip (byte-identical + a
// clean parse) — no silent failure (CORE-008).
//
// Exit codes (stable for CI grepping):
//   0  the scene was written and verified (the summary line carries
//      the shape + the elapsed generation time, machine-greppable)
//   2  usage, IO, or round-trip failure
//
// Headless invariants (ARCH-003): this tool links laige-render for
// the scene builder/serializer only — it creates no GL context, no
// window, no sim world (the engine objects the scene builds are the
// loader's job, the test's).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "laige/errors.h"
#include "laige/render/iso_reference_scene.h"
#include "laige/result.h"

namespace {

void printUsage(std::FILE* out) {
  std::fprintf(out,
      "laige-scene-iso — the M2-SCENE-01 reference-scene generator\n"
      "\n"
      "Usage:\n"
      "  laige-scene-iso --seed <u64> --variant <full|small> --out "
      "<path>\n"
      "\n"
      "  --seed <u64>        the scene's seed (any u64; 0 is valid)\n"
      "  --variant <name>    full (50k sprites) | small (1k sprites)\n"
      "  --out <path>        the output file (atomic temp + rename)\n"
      "\n"
      "The tool builds the scene deterministically from the seed, "
      "serializes it (the versioned binary format, "
      "laige/render/iso_reference_scene.h), writes it, and reads it "
      "back to verify the round trip (byte-identical + a clean "
      "parse).\n"
      "\n"
      "Exit codes: 0 = written + verified, 2 = usage/IO/round-trip "
      "failure.\n");
}

std::uint64_t parseU64(std::string_view s, bool& ok) {
  ok = false;
  if (s.empty()) return 0;
  std::uint64_t v = 0;
  for (const char ch : s) {
    if (ch < '0' || ch > '9') return 0;
    v = v * 10u + static_cast<std::uint64_t>(static_cast<unsigned>(ch - '0'));
  }
  ok = true;
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  using laige::render::IsoRefSceneVariant;
  using laige::render::IsoReferenceScene;
  using laige::render::buildIsoReferenceScene;
  using laige::render::readIsoReferenceScene;
  using laige::render::serializeIsoReferenceScene;
  using laige::render::writeIsoReferenceScene;

  std::uint64_t seed{};
  bool hasSeed = false;
  IsoRefSceneVariant variant = IsoRefSceneVariant::Full;
  bool hasVariant = false;
  std::string outPath;
  bool hasOut = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--seed" || arg == "--variant" || arg == "--out") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "laige-scene-iso: %s needs a value\n",
                     std::string(arg).c_str());
        return 2;
      }
      const std::string value = argv[++i];
      if (arg == "--seed") {
        bool ok{};
        seed = parseU64(value, ok);
        if (!ok) {
          std::fprintf(stderr, "laige-scene-iso: bad --seed value\n");
          return 2;
        }
        hasSeed = true;
      } else if (arg == "--variant") {
        if (value == "full") {
          variant = IsoRefSceneVariant::Full;
        } else if (value == "small") {
          variant = IsoRefSceneVariant::Small;
        } else {
          std::fprintf(stderr,
                       "laige-scene-iso: --variant must be full or "
                       "small\n");
          return 2;
        }
        hasVariant = true;
      } else {
        outPath = value;
        hasOut = true;
      }
    } else if (arg == "--help" || arg == "-h") {
      printUsage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "laige-scene-iso: unknown argument '%.*s'\n",
                   static_cast<int>(arg.size()), arg.data());
      printUsage(stderr);
      return 2;
    }
  }
  if (!hasSeed || !hasVariant || !hasOut) {
    printUsage(stderr);
    return 2;
  }

  const auto t0 = std::chrono::steady_clock::now();
  const IsoReferenceScene scene = buildIsoReferenceScene(seed, variant);
  const std::vector<std::uint8_t> bytes = serializeIsoReferenceScene(scene);
  if (!writeIsoReferenceScene(scene, outPath).ok()) {
    std::fprintf(stderr, "laige-scene-iso: failed to write %s\n",
                 outPath.c_str());
    return 2;
  }

  // The round-trip self-check (CORE-008): read the file back, compare
  // it byte for byte, and re-parse it (a corrupt writer or a drifting
  // reader fails here, loudly).
  std::vector<std::uint8_t> back(bytes.size());
  std::FILE* f{};
#if defined(_MSC_VER)
  if (fopen_s(&f, outPath.c_str(), "rb") != 0) {
    std::fprintf(stderr, "laige-scene-iso: failed to read back %s\n",
                 outPath.c_str());
    return 2;
  }
#else
  f = std::fopen(outPath.c_str(), "rb");
#endif
  if (f == nullptr) {
    std::fprintf(stderr, "laige-scene-iso: failed to read back %s\n",
                 outPath.c_str());
    return 2;
  }
  const std::size_t got = std::fread(back.data(), 1, back.size(), f);
  std::fclose(f);
  if (got != back.size() || back != bytes) {
    std::fprintf(stderr,
                 "laige-scene-iso: round-trip mismatch (serialized %zu "
                 "bytes, read back %zu)\n",
                 bytes.size(), got);
    return 2;
  }
  const auto parsed =
      readIsoReferenceScene(std::span<const std::uint8_t>(back));
  if (parsed.isError()) {
    std::fprintf(stderr,
                 "laige-scene-iso: round-trip parse failed (%s)\n",
                 laige::errorName(parsed.error()));
    return 2;
  }

  const auto t1 = std::chrono::steady_clock::now();
  const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              t1 - t0)
                              .count();
  std::printf(
      "laige-scene-iso variant=%s seed=%llu bytes=%zu sprites=%u tiles=%u "
      "emitters=%u ui=%u elapsed_ms=%lld path=%s\n",
      variant == IsoRefSceneVariant::Full ? "full" : "small",
      static_cast<unsigned long long>(seed), bytes.size(),
      scene.spriteCount, scene.tileCount, scene.emitterCount,
      scene.uiWidgetCount, static_cast<long long>(ms), outPath.c_str());
  return 0;
}
