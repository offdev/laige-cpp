// laige-golden (M2-GOLD-01): the golden image test tool.
//
// Renders the committed golden scene fixtures (tests/golden/
// golden_fixtures.h: the M2-SPRITE-02 sprite scene and the M2-UI-02
// UI scene) offscreen at the canonical golden frame (1280x720, fixed
// camera, no PRNG), captures the frame (GlContext::readPixels: one
// glReadPixels, rows flipped to the top-origin / y-down screen
// convention), and compares it against the committed reference PNG
// (tools/golden/references) with the documented tolerance: max
// channel diff 2 (inclusive, on the 0..255 scale) + a 0.001% pixel
// budget (floor of the product: 9 of 921 600 pixels on the canonical
// frame). The tolerance is explicit: the ctest entry `golden` passes
// the same numbers as flags (TEST-008), and docs/testing.md states
// the regeneration contract (a driver or engine change that breaks
// the tolerance regenerates the references through the
// human-approved golden-update workflow; it never widens the
// tolerance silently).
//
// Modes:
//   (default)  compare every selected fixture against its reference
//              and enumerate the failing pixels (bounded samples);
//   --update   re-render and rewrite the references (atomic: a temp
//              file in the target directory, then rename). This is
//              the human-approved regeneration path; it is never run
//              under ctest (the ctest entry is compare-only).
//
// Exit codes (the CTest SKIP_RETURN_CODE contract for a plain
// executable, the GTEST_SKIP analog):
//   0  compare ok (every selected fixture within the tolerance)
//   1  compare failed (at least one fixture beyond the tolerance)
//   2  usage, configuration, or setup error (bad flags, missing
//      font, missing or wrong-sized reference, render failure)
//   77 no usable OpenGL 3.3 environment (the documented environment
//      contract; the macOS CI runners have no usable GL, so the
//      entry skips there rather than fails)
//
// Output is machine-greppable: one `golden-compare` or `golden-update`
// summary line per fixture, one `golden-diff` line per sampled
// failing pixel, one final `golden:` line. The CI job log (and the
// archived LastTest.log) carries the full comparison record.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "golden_compare.h"
#include "golden_fixtures.h"
#include "laige/errors.h"
#include "laige/render/gl_context.h"
#include "laige/render/sprite_batcher.h"
#include "laige/render/sprite_renderer.h"
#include "laige/render/ui_render.h"
#include "laige/result.h"
#include "laige_test_seed.h"
#include "stb_image.h"
#include "stb_image_write.h"

namespace {

using laige::render::GlContext;
using laige::render::Mat4;
using laige::render::SpriteBatcher;
using laige::render::SpriteItem;
using laige::render::SpriteRenderer;
using laige::testing::golden::Comparison;
using laige::testing::golden::SpriteScene;
using laige::testing::golden::Tolerance;
using laige::testing::golden::UiScene;
using laige::testing::golden::buildSpriteScene;
using laige::testing::golden::buildUiScene;
using laige::testing::golden::compareFrames;
using laige::testing::golden::kGoldenHeight;
using laige::testing::golden::kGoldenWidth;

constexpr int kExitOk = 0;
constexpr int kExitCompareFailed = 1;
constexpr int kExitConfigError = 2;
constexpr int kExitNoGl = 77;

// The fixture names (the reference file is <references>/<name>.png).
constexpr char const* kSpriteFixtureName = "sprite_iso_1000";
constexpr char const* kUiFixtureName = "ui_smoke";

// The per-fixture renderer/batcher budgets (the fixtures are tiny:
// 1000 sprites, 7 quads; the ceilings are headroom, not tuning).
constexpr std::uint32_t kMaxInstances = 1024;
constexpr std::uint32_t kMaxAtlases = 16;

struct Config {
  bool update{false};
  std::vector<std::string> fixtures;  // empty == every registered fixture
  std::string references{"tools/golden/references"};
  Tolerance tolerance;
};

void usage(char const* prog) {
  std::printf(
      "usage: %s [--update] [--all | --fixture NAME ...]\n"
      "           [--references DIR] [--max-channel-diff N]\n"
      "           [--pixel-budget-pct P]\n"
      "\n"
      "Fixtures: %s, %s (tests/golden/golden_fixtures.h).\n"
      "Exit codes: 0 ok, 1 compare failed, 2 usage/configuration\n"
      "error, 77 no usable OpenGL 3.3 environment (the documented\n"
      "environment contract).\n",
      prog, kSpriteFixtureName, kUiFixtureName);
}

int parseArgs(int argc, char** argv, Config& cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--update") {
      cfg.update = true;
    } else if (a == "--all") {
      cfg.fixtures.clear();
    } else if (a == "--fixture") {
      if (i + 1 >= argc) {
        return kExitConfigError;
      }
      cfg.fixtures.push_back(argv[++i]);
    } else if (a == "--references") {
      if (i + 1 >= argc) {
        return kExitConfigError;
      }
      cfg.references = argv[++i];
    } else if (a == "--max-channel-diff") {
      if (i + 1 >= argc) {
        return kExitConfigError;
      }
      cfg.tolerance.maxChannelDiff = std::atoi(argv[++i]);
    } else if (a == "--pixel-budget-pct") {
      if (i + 1 >= argc) {
        return kExitConfigError;
      }
      cfg.tolerance.pixelBudgetPct = std::atof(argv[++i]);
    } else {
      return kExitConfigError;
    }
  }
  if (cfg.tolerance.maxChannelDiff < 0) {
    return kExitConfigError;
  }
  if (cfg.tolerance.pixelBudgetPct < 0.0) {
    return kExitConfigError;
  }
  return kExitOk;
}

// The house font load (tests/support/laige_test_seed.h ReadEnvVar +
// the fopen_s pattern from ui_render_tests.cpp, no gtest).
std::vector<std::uint8_t> loadFont() {
  const std::string path = laige::testing::ReadEnvVar("LAIGE_TEST_FONT_PATH");
  if (path.empty()) {
    std::fprintf(stderr,
                 "laige-golden: LAIGE_TEST_FONT_PATH is not set (CTest "
                 "ENVIRONMENT contract; the ui_smoke fixture needs the "
                 "committed test font)\n");
    return {};
  }
  std::FILE* f = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&f, path.c_str(), "rb") != 0) {
#else
  f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
#endif
    std::fprintf(stderr,
                 "laige-golden: cannot open the test font: %s\n",
                 path.c_str());
    return {};
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (got != bytes.size()) {
    std::fprintf(stderr,
                 "laige-golden: short read of the test font: %s\n",
                 path.c_str());
    return {};
  }
  return bytes;
}

struct RenderedFrame {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba;  // top-origin / y-down (the golden
                                   // frame convention)
};

// The offscreen frame protocol of one fixture (the M2-SPRITE-02
// protocol): clear before the first draw, submit, then read back the
// whole frame (one glReadPixels; the 921 600-pixel 1x1 readPixel loop
// is what readPixels replaced) and flip the GL bottom-left origin to
// the top-origin / y-down screen convention. The setup and declare
// callbacks carry the fixture-specific work (the atlas binds, the item
// declares); they run on the creating thread, single-threaded. A
// missing GL environment exits with 77 (the documented contract);
// every other failure exits with 2.
RenderedFrame renderFrame(
    float clearR, float clearG, float clearB, float clearA,
    const Mat4& matrix,
    const std::function<void(SpriteRenderer&)>& setupAtlases,
    const std::function<void(SpriteBatcher&)>& declare) {
  RenderedFrame out;
  auto ctxR = GlContext::createHeadless(
      static_cast<std::int32_t>(kGoldenWidth),
      static_cast<std::int32_t>(kGoldenHeight));
  if (ctxR.isError()) {
    std::fprintf(stderr,
                 "laige-golden: no usable OpenGL 3.3 environment: %s "
                 "(the documented environment contract; exit %d, the "
                 "ctest entry skips)\n",
                 laige::errorText(ctxR.error()), kExitNoGl);
    std::exit(kExitNoGl);
  }
  GlContext ctx = std::move(ctxR).takeValue();
  auto rrR = SpriteRenderer::create(
      ctx, SpriteRenderer::Options{kMaxInstances, kMaxAtlases});
  if (rrR.isError()) {
    std::fprintf(stderr, "laige-golden: SpriteRenderer::create failed: "
                         "%s\n",
                 laige::errorText(rrR.error()));
    std::exit(kExitConfigError);
  }
  SpriteRenderer renderer = std::move(rrR).takeValue();
  setupAtlases(renderer);
  auto bcR = SpriteBatcher::create(SpriteBatcher::Options{kMaxInstances});
  if (bcR.isError()) {
    std::fprintf(stderr, "laige-golden: SpriteBatcher::create failed: "
                         "%s\n",
                 laige::errorText(bcR.error()));
    std::exit(kExitConfigError);
  }
  SpriteBatcher batcher = std::move(bcR).takeValue();
  batcher.beginFrame();
  declare(batcher);
  if (!batcher.build().ok()) {
    std::fprintf(stderr, "laige-golden: batcher.build() failed\n");
    std::exit(kExitConfigError);
  }
  if (!ctx.clear(clearR, clearG, clearB, clearA).ok()) {
    std::fprintf(stderr, "laige-golden: clear failed\n");
    std::exit(kExitConfigError);
  }
  if (!renderer.submit(batcher, matrix).ok()) {
    std::fprintf(stderr, "laige-golden: submit failed\n");
    std::exit(kExitConfigError);
  }
  const std::uint32_t w = kGoldenWidth;
  const std::uint32_t h = kGoldenHeight;
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(w) * h * 4u);
  if (!ctx.readPixels(0, 0, static_cast<std::int32_t>(w),
                      static_cast<std::int32_t>(h), raw.data())
          .ok()) {
    std::fprintf(stderr, "laige-golden: readPixels failed\n");
    std::exit(kExitConfigError);
  }
  out.width = w;
  out.height = h;
  out.rgba.resize(raw.size());
  // The GL readback's row 0 is the BOTTOM row (the GL convention); the
  // golden frame convention is top-origin / y-down (the screen-px
  // model of the UI pass, and the PNG row order). One memcpy per row.
  for (std::uint32_t r = 0; r < h; ++r) {
    std::memcpy(out.rgba.data() + static_cast<std::size_t>(r) * w * 4u,
                raw.data() + (static_cast<std::size_t>(h - 1u - r) * w) * 4u,
                static_cast<std::size_t>(w) * 4u);
  }
  return out;
}

void printDiffSamples(char const* name, const Comparison& c) {
  for (const auto& s : c.samples) {
    std::printf("golden-diff: fixture=%s x=%d y=%d expected=(%u,%u,%u,%u) "
                "actual=(%u,%u,%u,%u) max_diff=%d\n",
                name, s.x, s.y, s.expected[0], s.expected[1],
                s.expected[2], s.expected[3], s.actual[0], s.actual[1],
                s.actual[2], s.actual[3], s.maxDiff);
  }
}

// One fixture: build, render, then compare (default) or update
// (--update). Returns one of kExitOk, kExitCompareFailed,
// kExitConfigError. (The no-GL case exits 77 inside renderFrame.)
int runFixture(const std::string& name, const Config& cfg,
               const std::vector<std::uint8_t>& font) {
  RenderedFrame frame;
  if (name == kSpriteFixtureName) {
    auto sceneR = buildSpriteScene();
    if (sceneR.isError()) {
      std::fprintf(stderr, "laige-golden: sprite fixture build failed: "
                           "%s\n",
                   laige::errorText(sceneR.error()));
      std::exit(kExitConfigError);
    }
    SpriteScene scene = std::move(sceneR).takeValue();
    frame = renderFrame(
        scene.clearR, scene.clearG, scene.clearB, scene.clearA,
        scene.matrix,
        [&scene](SpriteRenderer& rr) {
          if (!rr.bindAtlas(laige::testing::golden::kSpriteAtlas0Id, 4, 4,
                            std::span{scene.atlas0})
                   .ok()) {
            std::fprintf(stderr, "laige-golden: bindAtlas(0) failed\n");
            std::exit(kExitConfigError);
          }
          if (!rr.bindAtlas(laige::testing::golden::kSpriteAtlas1Id, 4, 4,
                            std::span{scene.atlas1})
                   .ok()) {
            std::fprintf(stderr, "laige-golden: bindAtlas(1) failed\n");
            std::exit(kExitConfigError);
          }
        },
        [&scene](SpriteBatcher& b) {
          for (const SpriteItem& it : scene.items) {
            if (!b.add(it).ok()) {
              std::fprintf(stderr, "laige-golden: batcher.add failed\n");
              std::exit(kExitConfigError);
            }
          }
        });
  } else {  // kUiFixtureName
    auto sceneR = buildUiScene(font);
    if (sceneR.isError()) {
      std::fprintf(stderr, "laige-golden: ui fixture build failed: %s\n",
                   laige::errorText(sceneR.error()));
      std::exit(kExitConfigError);
    }
    UiScene scene = std::move(sceneR).takeValue();
    frame = renderFrame(
        scene.clearR, scene.clearG, scene.clearB, scene.clearA,
        scene.matrix,
        [&scene](SpriteRenderer& rr) {
          if (!rr.bindAtlas(scene.uiAtlasId, 4, 4, std::span{scene.uiAtlas})
                   .ok()) {
            std::fprintf(stderr, "laige-golden: bindAtlas(ui) failed\n");
            std::exit(kExitConfigError);
          }
          if (!rr.bindAtlas(scene.fontAtlasId, 1024, 1024,
                            std::span{scene.fontRgba})
                   .ok()) {
            std::fprintf(stderr, "laige-golden: bindAtlas(font) failed\n");
            std::exit(kExitConfigError);
          }
        },
        [&scene](SpriteBatcher& b) {
          if (!laige::render::declareUi(b, scene.table, scene.tree,
                                        scene.declareOptions())
                   .ok()) {
            std::fprintf(stderr, "laige-golden: declareUi failed\n");
            std::exit(kExitConfigError);
          }
        });
  }

  const std::string path = cfg.references + "/" + name + ".png";
  if (cfg.update) {
    // The human-approved regeneration: atomic write (a temp file in
    // the target directory, then rename; a crash mid-write never
    // leaves a torn reference).
    const std::string tmp = path + ".tmp";
    if (!stbi_write_png(tmp.c_str(), static_cast<int>(frame.width),
                        static_cast<int>(frame.height), 4,
                        frame.rgba.data(),
                        static_cast<int>(static_cast<std::size_t>(frame.width) * 4u))) {
      std::fprintf(stderr,
                   "laige-golden: stbi_write_png failed for %s\n",
                   tmp.c_str());
      return kExitConfigError;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
      std::fprintf(stderr,
                   "laige-golden: rename %s -> %s failed\n", tmp.c_str(),
                   path.c_str());
      return kExitConfigError;
    }
    std::printf("golden-update: fixture=%s size=%ux%u path=%s status=ok\n",
                name.c_str(), frame.width, frame.height, path.c_str());
    return kExitOk;
  }

  // Compare mode: load the committed reference (4 channels; the
  // references are always RGBA8 PNGs of this tool's own output).
  int rw = 0;
  int rh = 0;
  int rc = 0;
  void* ref = stbi_load(path.c_str(), &rw, &rh, &rc, 4);
  if (ref == nullptr) {
    std::fprintf(stderr,
                 "golden: missing-reference fixture=%s path=%s (run "
                 "--update through the documented regeneration path; "
                 "docs/testing.md)\n",
                 name.c_str(), path.c_str());
    return kExitConfigError;
  }
  std::vector<std::uint8_t> refBytes(
      static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh) * 4u);
  std::memcpy(refBytes.data(), ref, refBytes.size());
  stbi_image_free(ref);
  if (static_cast<std::uint32_t>(rw) != frame.width ||
      static_cast<std::uint32_t>(rh) != frame.height) {
    std::fprintf(stderr,
                 "golden: wrong reference size fixture=%s path=%s "
                 "expected=%ux%u actual=%dx%d (re-run --update through "
                 "the documented regeneration path)\n",
                 name.c_str(), path.c_str(), frame.width, frame.height,
                 rw, rh);
    return kExitConfigError;
  }
  const Comparison c =
      compareFrames(refBytes, frame.rgba, frame.width, frame.height,
                    cfg.tolerance);
  std::printf(
      "golden-compare: fixture=%s size=%ux%u failing=%llu budget=%llu "
      "max_diff=%d status=%s\n",
      name.c_str(), frame.width, frame.height,
      static_cast<unsigned long long>(c.failingPixels),
      static_cast<unsigned long long>(c.budget), c.maxObservedDiff,
      c.pass() ? "ok" : "fail");
  if (!c.pass()) {
    printDiffSamples(name.c_str(), c);
  }
  return c.pass() ? kExitOk : kExitCompareFailed;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (parseArgs(argc, argv, cfg) != kExitOk) {
    usage(argv[0]);
    return kExitConfigError;
  }
  std::vector<std::string> names = cfg.fixtures;
  if (names.empty()) {
    names = {kSpriteFixtureName, kUiFixtureName};
  }
  for (const std::string& n : names) {
    if (n != kSpriteFixtureName && n != kUiFixtureName) {
      std::fprintf(stderr, "laige-golden: unknown fixture %s\n", n.c_str());
      usage(argv[0]);
      return kExitConfigError;
    }
  }
  // The ui_smoke fixture needs the committed test font; fail fast (2)
  // before any GL work.
  std::vector<std::uint8_t> font;
  for (const std::string& n : names) {
    if (n == kUiFixtureName) {
      font = loadFont();
      if (font.empty()) {
        return kExitConfigError;
      }
    }
  }
  std::printf("golden: mode=%s references=%s tolerance=(max_channel_diff=%d "
              "pixel_budget_pct=%g)\n",
              cfg.update ? "update" : "compare", cfg.references.c_str(),
              cfg.tolerance.maxChannelDiff, cfg.tolerance.pixelBudgetPct);
  int rc = kExitOk;
  std::size_t failed = 0;
  for (const std::string& n : names) {
    const int r = runFixture(n, cfg, font);
    if (r == kExitCompareFailed) {
      ++failed;
      rc = kExitCompareFailed;
    } else if (r != kExitOk) {
      return r;  // a configuration error aborts the run
    }
  }
  std::printf("golden: %s (%zu of %zu fixture(s) failed)\n",
              failed == 0 ? "ok" : "fail", failed, names.size());
  return rc;
}
