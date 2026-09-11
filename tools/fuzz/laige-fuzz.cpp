// laige-fuzz — deterministic bounded fuzz runner (M0-CORE-07, minimal
// form; M0-TEST-01 extends it: CI lane semantics, nightly long runs,
// seed-handling documentation).
//
// Roadmap step M0-CORE-07 lands the *minimal* runner this Verify gate
// requires: `laige-fuzz json_parse --runs=1000` clean under ASan
// (NFR-8.7: parsers are fuzzed in CI, bounded every commit — PRD §14).
// The `json_parse` target is registered below; later steps add their
// targets to kTargets.
//
// Design (deterministic by construction):
//   - Every input is generated from laige::Prng (M0-CORE-06): a fixed
//     default seed (kDefaultSeed, overridable with --seed) drives the
//     whole run, so the same (target, runs, seed) reproduces the exact
//     same input sequence on every platform (Prng's cross-platform
//     bit-exact determinism, ARCH-010).
//   - Per input, one of three modes (Prng-selected):
//       mutate    pick a corpus document, apply 1..8 random byte edits
//                 (flip / insert / delete)
//       truncate  pick a corpus document, keep a random prefix
//       random    a pure random byte string, length 1..kMaxInputBytes
//   - The target is called with (bytes, size); any Status the target
//     returns is acceptable. The harness fails only on process death
//     (a crash or sanitizer report), which ctest turns into a test
//     failure — no silent failure (CORE-008).
//
// Usage:
//   laige-fuzz <target> [--runs=N] [--seed=HEX|DEC]
//
// Exit codes: 0 = all runs clean · 2 = usage error or unknown target.
// (A crash exits non-zero on its own before any summary is printed.)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "laige/json.h"
#include "laige/prng.h"

namespace {

constexpr std::uint64_t kDefaultSeed = 0x1F055EEDull;  // "one-fuzz-seed"
constexpr int kDefaultRuns = 1000;
constexpr std::size_t kMaxInputBytes = 64;

// Valid-document corpus: the base for mutate/truncate inputs. Kept ASCII
// (explicit bytes for the one non-ASCII document) so the corpus bytes are
// identical on every platform (narrow \x escapes are not portable).
const std::string kCorpusUtf8 =
    "\"\\ud83d\\ude00 raw " +
    std::string({static_cast<char>(0xE2), static_cast<char>(0x82),
                 static_cast<char>(0xAC)}) +
    "\"";
const std::vector<std::string> kCorpus = {
    "null",
    "true",
    "false",
    "0",
    "-1",
    "1.5",
    "1e999",
    "-2.5e-7",
    "\"\"",
    "\"hello\"",
    "\"\\u0041\\u00e9\"",
    kCorpusUtf8,
    "[]",
    "[1,2,3]",
    "[[1],[2,3]]",
    "{}",
    "{\"a\":1,\"b\":null}",
    "{\"a\":{\"b\":[1,{\"c\":\"x\"}]}}",
    " \t\r\n [ 1 , 2 ] \r\n ",
};

// ---------------------------------------------------------------------------
// Fuzz targets: (name, entry point). The entry point receives the raw
// input; any Status outcome is acceptable — only a crash fails the run.
// ---------------------------------------------------------------------------
struct FuzzTarget {
  const char* name;
  void (*run)(const std::uint8_t* data, std::size_t size);
};

void fuzzJsonParse(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const laige::Result<laige::JsonValue> result = laige::parseJson(input);
  (void)result;  // any Status is acceptable; only a crash fails the run
}

const FuzzTarget kTargets[] = {
    {"json_parse", &fuzzJsonParse},
};

void printUsage() {
  std::printf("usage: laige-fuzz <target> [--runs=N] [--seed=HEX|DEC]\n");
  std::printf("targets:\n");
  for (const FuzzTarget& target : kTargets) {
    std::printf("  %s\n", target.name);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* targetName = nullptr;
  int runs = kDefaultRuns;
  std::uint64_t seed = kDefaultSeed;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg.starts_with("--runs=")) {
      const int v = std::atoi(arg.data() + 7);
      if (v <= 0) {
        printUsage();
        return 2;
      }
      runs = v;
    } else if (arg.starts_with("--seed=")) {
      const char* value = arg.data() + 7;
      if (*value == '\0') {
        printUsage();
        return 2;
      }
      seed = std::strtoull(value, nullptr, 0);  // 0x-prefix or decimal
    } else if (arg.starts_with("-")) {
      printUsage();
      return 2;
    } else if (targetName != nullptr) {
      printUsage();
      return 2;
    } else {
      targetName = arg.data();
    }
  }

  if (targetName == nullptr) {
    printUsage();
    return 2;
  }
  const FuzzTarget* target = nullptr;
  for (const FuzzTarget& candidate : kTargets) {
    if (std::string_view(candidate.name) == targetName) {
      target = &candidate;
      break;
    }
  }
  if (target == nullptr) {
    printUsage();
    return 2;
  }

  laige::Prng rng(seed);
  for (int i = 0; i < runs; ++i) {
    std::vector<std::uint8_t> input;
    const int mode = rng.next_range(0, 10);  // 0..9
    if (mode < 5) {
      // ~50%: mutate a corpus document with 1..8 random byte edits.
      const std::string& doc =
          kCorpus[rng.next_range(0, static_cast<std::uint32_t>(kCorpus.size()))];
      input.assign(doc.begin(), doc.end());
      const int edits = 1 + static_cast<int>(rng.next_range(0, 8));
      for (int e = 0; e < edits; ++e) {
        // Position in [0, input.size()]: insert-at-end is a valid edit.
        const std::size_t where =
            rng.next_range(0, static_cast<std::uint32_t>(input.size() + 1));
        const int op = static_cast<int>(rng.next_range(0, 3));
        if (op == 0) {  // flip a byte
          if (!input.empty()) {
            input[where % input.size()] ^=
                static_cast<std::uint8_t>(1 + rng.next_range(0, 255));
          }
        } else if (op == 1) {  // insert a byte
          input.insert(input.begin() + where,
                       static_cast<std::uint8_t>(rng.next_u64()));
        } else {  // delete a byte
          if (!input.empty()) {
            input.erase(input.begin() + (where % input.size()));
          }
        }
      }
    } else if (mode < 8) {
      // ~30%: truncate a corpus document to a random prefix.
      const std::string& doc =
          kCorpus[rng.next_range(0, static_cast<std::uint32_t>(kCorpus.size()))];
      const std::size_t len = doc.size();
      const std::size_t keep =
          len == 0 ? 0 : rng.next_range(0, static_cast<std::uint32_t>(len + 1));
      input.assign(doc.begin(), doc.begin() + static_cast<std::ptrdiff_t>(keep));
    } else {
      // ~20%: pure random bytes.
      const std::size_t len =
          rng.next_range(1, static_cast<std::uint32_t>(kMaxInputBytes + 1));
      input.resize(len);
      for (std::uint8_t& byte : input) {
        byte = static_cast<std::uint8_t>(rng.next_u64());
      }
    }
    target->run(input.data(), input.size());
  }

  std::printf("laige-fuzz: target '%s' runs=%d seed=0x%016llx — ok\n",
              target->name, runs, static_cast<unsigned long long>(seed));
  return 0;
}
