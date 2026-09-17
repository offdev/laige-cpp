// hello scenario baseline check — implementation (M1-DET-04).
//
// The comparison mirrors tools/replay/laige-replay.cpp's --expect path
// line-for-line (the bounds, the line split, the strict
// '<tick> <hash>' parse, and the report texts — with "run" in place of
// "replay"): a mismatch is exit 1 with the first-divergence report, a
// baseline read/contract error is exit 2, and the run's own stream is
// the compared artifact (the scenario's stdout, the detcheck scenario
// contract). See hello-baseline.h for the contract.

#include "hello-baseline.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_MSC_VER)
#include <share.h>  // _SH_DENYNO: plain-fopen sharing for the _fsopen below
#endif

namespace hello {

namespace {

// The baseline bounds (the laige-replay --expect bounds, CORE-005): at
// most kMaxBaselineLines hash lines (the detcheck 65536-tick
// contract) at at most kMaxBaselineLineBytes each; the file read cap
// is the round bound above (laige-replay's kMaxBaselineBytes).
inline constexpr std::size_t kMaxBaselineLines = 65536;
inline constexpr std::size_t kMaxBaselineLineBytes = 64;
inline constexpr std::size_t kMaxBaselineBytes = 8u << 20;

enum class ReadResult { Ok, Io, TooLarge };

// Portable file open (the laige-run / laige-replay precedent: MSVC's
// CRT deprecates plain fopen; _fsopen(_SH_DENYNO) is the plain-fopen
// sharing semantics every other compiler provides).
#if defined(_MSC_VER)
inline std::FILE* openFile(const char* path) {
  return ::_fsopen(path, "rb", _SH_DENYNO);
}
#else
inline std::FILE* openFile(const char* path) { return std::fopen(path, "rb"); }
#endif

// Reads a file into a bounded buffer (the laige-replay readFileBounded
// contract: over the cap is a contract error, never a silent
// truncation).
ReadResult readFileBounded(const std::string& path, std::size_t cap,
                           std::string* out) {
  std::FILE* file = openFile(path.c_str());
  if (file == nullptr) return ReadResult::Io;
  out->clear();
  char chunk[8192];
  for (;;) {
    const std::size_t n = std::fread(chunk, 1, sizeof(chunk), file);
    if (n == 0) {
      if (std::ferror(file)) {
        std::fclose(file);
        return ReadResult::Io;
      }
      break;  // clean EOF
    }
    out->append(chunk, n);
    if (out->size() > cap) {
      std::fclose(file);
      return ReadResult::TooLarge;
    }
  }
  std::fclose(file);
  return ReadResult::Ok;
}

// One baseline line (the hash-line contract, the detcheck scenario
// form): '<tick> <hash>' — the tick is the exact decimal of the line
// index (no padding or leading zeros), one space, 16 lowercase hex
// digits (laige-replay's parseBaselineLine check, mirrored).
bool parseBaselineLine(const std::string& line, std::size_t lineIndex) {
  if (line.size() > kMaxBaselineLineBytes) return false;
  const std::string expectedTick = std::to_string(lineIndex);
  if (line.compare(0, expectedTick.size(), expectedTick) != 0) return false;
  std::size_t pos = expectedTick.size();
  if (pos >= line.size() || line[pos] != ' ') return false;
  ++pos;
  if (line.size() - pos != 16) return false;
  for (std::size_t i = 0; i < 16; ++i) {
    const char c = line[pos + i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

BaselineCheck::BaselineCheck(std::string_view path) {
  if (path.empty()) return;  // a plain run: no check (inactive)
  std::string file;
  const ReadResult read = readFileBounded(std::string(path),
                                          kMaxBaselineBytes, &file);
  if (read == ReadResult::Io) {
    error_ = "cannot read the baseline file '" + std::string(path) +
             "' (io error)";
    return;
  }
  if (read == ReadResult::TooLarge) {
    error_ = "baseline file exceeds the " + std::to_string(kMaxBaselineBytes) +
             "-byte read cap (the laige-replay --expect bound)";
    return;
  }
  // Split into lines (one separator per line; an optional trailing
  // newline and an optional trailing \r are tolerated — the detcheck
  // contract, the laige-replay split mirrored line-for-line).
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (;;) {
    const std::size_t nl = file.find('\n', start);
    if (nl == std::string::npos) {
      lines.push_back(file.substr(start));
      break;
    }
    std::string line = file.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    start = nl + 1;
    if (start >= file.size()) break;  // trailing newline
  }
  if (!lines.empty() && lines.back().empty() && !file.empty() &&
      file.back() == '\n') {
    lines.pop_back();  // the optional trailing newline
  }
  if (lines.size() > kMaxBaselineLines) {
    error_ = "baseline has " + std::to_string(lines.size()) +
             " lines (the contract bound is " +
             std::to_string(kMaxBaselineLines) + ")";
    return;
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!parseBaselineLine(lines[i], i)) {
      error_ = "malformed baseline line " + std::to_string(i) + ": '" +
               lines[i] +
               "' (the contract is '<tick> <hash>': the line index, one "
               "space, 16 lowercase hex digits)";
      return;
    }
  }
  baseline_ = std::move(lines);
}

void BaselineCheck::emit(std::uint64_t tick, std::uint64_t hash) {
  char line[40];
  std::snprintf(line, sizeof(line), "%llu %016llx",
                static_cast<unsigned long long>(tick),
                static_cast<unsigned long long>(hash));
  std::printf("%s\n", line);
  if (baseline_.empty() || firstDiff_ != std::uint64_t(-1)) return;
  if (tick >= baseline_.size()) {
    firstDiff_ = baseline_.size();  // the first extra line
    lineA_ = "";
    lineB_ = line;
    return;
  }
  if (std::strcmp(line, baseline_[tick].c_str()) != 0) {
    firstDiff_ = tick;
    lineA_ = baseline_[tick];
    lineB_ = line;
  }
}

std::string BaselineCheck::failText(std::uint64_t emitted) const {
  if (baseline_.empty()) return "";  // no check (inactive or load failed)
  if (firstDiff_ != std::uint64_t(-1) && firstDiff_ < baseline_.size()) {
    return "hash mismatch at tick " + std::to_string(firstDiff_) +
           " (first divergence)\n  baseline: " + lineA_ +
           "\n  run:      " + lineB_;
  }
  if (emitted != baseline_.size()) {
    const std::uint64_t at = std::min(emitted, baseline_.size());
    return "baseline stream length mismatch: the baseline has " +
           std::to_string(baseline_.size()) + " lines, the run produces " +
           std::to_string(emitted) + " (first missing/extra at tick " +
           std::to_string(at) + ")";
  }
  return "";
}

}  // namespace hello
