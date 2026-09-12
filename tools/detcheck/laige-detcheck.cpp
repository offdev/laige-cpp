// laige-detcheck (M0-TOOL-02) — determinism checker skeleton (FR-11.5).
//
// Canonical command (docs/getting-started/building.md is the source of
// truth):
//
//   ./build/bin/laige-detcheck --scenario=<name>
//
// The step's scope: run a named scenario in two build configurations
// (e.g. Debug+ASan vs Release, or two compiler builds) and compare
// per-tick state hashes. The engine state-hash API arrives with
// M1-DET-03; this skeleton accepts the hash-file output contract — a
// stream of `<tick> <hash>` lines — and compares two such streams.
//
// ============================================================================
// SCENARIO CONTRACT (documented here; also in docs/api/detcheck.md)
// ============================================================================
// A scenario is a deterministic program (the M1 form is M1-SAMPLE-01's
// hello.laige, run headless with a fixed seed). It MUST:
//
//   stdout: exactly one line per simulated tick, in order:
//
//       <tick> <hash>
//
//     - <tick>: non-negative decimal integer, no padding or leading
//       zeros; the first line is tick 0 and each later tick is exactly
//       one higher (no gaps, no duplicates).
//     - <hash>: exactly 16 lowercase hex digits — the canonical text
//       form of a 64-bit state hash (M1-DET-03's world.state_hash).
//       The hash algorithm is NOT part of the contract: detcheck
//       compares the lines byte-for-byte, so only run-to-run identity
//       matters.
//     - One line separator per line; a trailing newline on the final
//       line is optional (an optional trailing \r is tolerated —
//       Windows CRLF).
//   stderr: ignored by detcheck (it remains visible in the CI job log).
//   exit code: 0 when the scenario completes; any other value is a
//     scenario failure.
//
// detcheck enforces the contract strictly (CORE-008: a malformed
// scenario is a loud exit-2 error, never a silent mismatch) and bounds
// the output (kMaxTicks lines, kMaxLineBytes per line — a scenario that
// runs away is a broken harness, not a determinism result).
//
// ============================================================================
// Usage
// ============================================================================
//
//   laige-detcheck --scenario=<name> [--ticks=N] [--seed=HEX|DEC]
//   laige-detcheck --run-a=<scenario-bin-A> --run-b=<scenario-bin-B>
//                  [-- scenario-args...]
//
// Mode 1 (--scenario): a built-in scenario (M0 stand-in for the M1
// scenarios):
//
//   synthetic             32 fpx16_16 bodies + seeded Prng input, run
//                         twice in-process (two identical configurations)
//   synthetic-perturbed   the same, but run-b adds 1 unit to body 3's x
//                         at tick 7 — the perturbation fixture that
//                         proves the failure path (the step's Verify
//                         clause)
//
// Mode 2 (--run-a/--run-b): the real mode from M1-DET-04 on — two builds
// of the same scenario source (two build configurations) are executed
// and their hash streams compared. Everything after a `--` separator is
// passed to both scenario binaries (scenario arguments that could look
// like tool flags are unambiguous because of the separator).
//
// Report (stdout, stable and machine-greppable — LOG-001):
//
//   match:
//     detcheck scenario=<name> result=OK ticks=<n>
//       run-a: <label-or-path>
//       run-b: <label-or-path>
//   divergence:
//     detcheck scenario=<name> result=DIVERGED first_diff_tick=<t>
//       run-a: <t> <hashA>
//       run-b: <t> <hashB>
//     (when one stream ends early the pair of tick lines becomes a
//      stream-length note instead)
//
// Exit codes:
//   0  the two runs agree on every tick (deterministic — the OK result)
//   1  divergence detected (a determinism failure — loud, CORE-008)
//   2  usage error, unknown scenario, a scenario run failed (non-zero
//      exit or spawn failure), or a scenario violated the output
//      contract (malformed line / tick gap / unbounded output)
//
// Windows only: a scenario run that produced no output and exited with an
// OS image-load failure code (see isTransientSpawnFailure — e.g. 259
// ERROR_FILE_NOT_FOUND right after a fresh build while a file filter
// scans the new .exe) is retried exactly once before being reported.
//
// ============================================================================
// Built-in synthetic workload
// ============================================================================
//
// 32 bodies of Q16.16 position/velocity (the default deterministic
// backend, M0-CORE-04 / ADR 0002). Each tick: fixed-order integration
// (x += vx, y += vy), wrap into a 64-unit box, then one seeded input
// event — the Prng (M0-CORE-06) picks the body index and a nudge in
// [-4, 3] added to its x. The synthetic-perturbed run-b additionally
// adds 1 unit to body 3's x at tick kPerturbTick (7).
//
// Per-tick hash: FNV-1a 64 (the same constants as the math_fixed
// known-answer test) over (tick, seed, every body's four raw words),
// big-endian per word (endianness-independent). M0 hash scope
// (documented, ARCH-010): tick counter + seed + body words. The Prng
// position is a pure function of (seed, nudge history) in this
// workload; the definitive scope — including PRNG state and the exact
// hash function — is defined by M1-DET-03's world.state_hash.
//
// Determinism scope of the built-in scenario: pure unsigned-integer
// arithmetic (fpx16_16 ops + xorshift128+) — bit-exact across build,
// platform, ISA, and compiler (ADR 0002; the language standard
// guarantees it). No float anywhere in the workload.
//
// Performance (this is a CI tool, not a hot path): one scenario run is
// O(ticks × kBodies); the two captured streams are bounded by
// kMaxTicks lines of ≤ kMaxLineBytes each (~1.5 MiB worst case).

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "laige/fpx16_16.h"
#include "laige/prng.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// windows.h's WinDef.h defines the min/max macros, which collide with
// std::min/std::max (MSVC C2589 in compareStreams); NOMINMAX is the
// documented opt-out (CPP-009: compile-time platform boundary).
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// --- Named constants (CORE-005) ------------------------------------------

constexpr int kDefaultTicks = 256;   // built-in scenario default run length
constexpr int kMaxTicks = 65536;     // bounded scenario output (CORE-008)
constexpr std::uint64_t kDefaultSeed = 0x1DE7C0DEull;  // "detcheck" seed
constexpr int kBodies = 32;          // built-in scenario body count
constexpr int kPerturbTick = 7;      // synthetic-perturbed: run-b perturbation
constexpr int kBoxUnits = 64;        // the built-in world's wrap box (units)
constexpr std::size_t kMaxLineBytes = 64;  // one hash line is ~27 bytes
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// The build type is stamped by CMake (tools/detcheck/CMakeLists.txt) so
// the report records which configuration produced this run (AGENTS 12).
#ifndef LAIGE_DETCHECK_BUILD_TYPE
#define LAIGE_DETCHECK_BUILD_TYPE "unknown"
#endif

// --- Hash-line format (the scenario contract) ----------------------------

std::string formatHashLine(std::uint64_t tick, std::uint64_t hash) {
  char buf[kMaxLineBytes];
  std::snprintf(buf, sizeof buf, "%llu %016llx",
                static_cast<unsigned long long>(tick),
                static_cast<unsigned long long>(hash));
  return buf;
}

// Strict parse of one contract line: <decimal> ' ' <16 lowercase hex>.
// Leading zeros are rejected (the contract forbids padding).
bool parseHashLine(const std::string& line, std::uint64_t& tick,
                   std::uint64_t& hash) {
  const auto sp = line.find(' ');
  if (sp == std::string::npos) return false;
  if (sp == 0) return false;
  tick = 0;
  for (std::size_t i = 0; i < sp; ++i) {
    const char c = line[i];
    if (c < '0' || c > '9') return false;
    // Bounded parse: ticks never exceed kMaxTicks in a contract-valid
    // stream, so this cannot overflow (CPP-004).
    tick = tick * 10 + std::uint64_t(c - '0');
  }
  if (line[0] == '0' && sp > 1) return false;  // leading zero = padding
  const std::size_t hexLen = line.size() - sp - 1;
  if (hexLen != 16) return false;
  hash = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    const char c = line[sp + 1 + i];
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      return false;
    }
    hash = hash * 16 + std::uint64_t(digit);
  }
  return true;
}

// Full-stream contract check: line count ≤ maxTicks, every line well
// formed, and the tick of line i is exactly i (start at 0, step 1, no
// duplicates). Returns "" when valid, otherwise the error message.
std::string validateStream(const std::vector<std::string>& lines,
                           std::size_t maxTicks) {
  if (lines.empty()) return "no ticks emitted";
  if (lines.size() > maxTicks) {
    return "more than " + std::to_string(maxTicks) +
           " ticks emitted (unbounded scenario output)";
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    std::uint64_t tick = 0;
    std::uint64_t hash = 0;
    if (!parseHashLine(lines[i], tick, hash)) {
      return "malformed line " + std::to_string(i + 1) + ": '" + lines[i] +
             "' (expected '<tick> <16 lowercase hex digits>')";
    }
    if (tick != i) {
      return "tick sequence violation at line " + std::to_string(i + 1) +
             ": tick " + std::to_string(tick) + " (expected " +
             std::to_string(i) + ")";
    }
  }
  return "";
}

// --- Scenario execution (two build configurations) ------------------------

struct RunResult {
  bool ok = false;          // process ran to completion without an error
  int exitCode = 0;
  std::vector<std::string> lines;
  std::string error;        // non-empty -> report + exit 2
};

// Append raw stdout bytes to the line vector: split on '\n', strip one
// optional trailing '\r' (Windows CRLF), and enforce the per-line and
// per-stream bounds. Returns false when the output is unbounded.
bool appendChunk(std::vector<std::string>& lines, std::string& pending,
                 const char* data, std::size_t n, std::size_t maxTicks) {
  for (std::size_t i = 0; i < n; ++i) {
    const char c = data[i];
    if (c == '\n') {
      if (!pending.empty() && pending.back() == '\r') pending.pop_back();
      if (pending.size() > kMaxLineBytes) return false;
      lines.push_back(std::move(pending));
      pending.clear();
      if (lines.size() > maxTicks) return false;
    } else {
      pending.push_back(c);
      if (pending.size() > kMaxLineBytes) return false;
    }
  }
  return true;
}

// The final unterminated line (the trailing-newline-optional contract).
void finishPending(std::vector<std::string>& lines, std::string& pending,
                   RunResult& r) {
  if (pending.empty()) return;
  if (pending.back() == '\r') pending.pop_back();
  if (pending.size() > kMaxLineBytes || lines.size() > kMaxTicks) {
    if (r.error.empty()) {
      r.error = "scenario output is unbounded (line or tick count exceeds "
                "the contract)";
    }
    return;
  }
  lines.push_back(std::move(pending));
}

#if defined(_WIN32)

std::wstring toWide(std::string_view s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      w.data(), n);
  return w;
}

// CreateProcessW command-line quoting (Microsoft quoting rules): quote
// an argument that contains whitespace or a double quote; double the
// quotes inside.
std::wstring quoteArg(std::string_view arg) {
  if (arg.find_first_of(" \t\"") == std::string_view::npos) {
    return toWide(arg);
  }
  std::wstring q = L"\"";
  for (const char c : arg) {
    if (c == '"') {
      q += L"\"\"";
    } else {
      q += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
  }
  q += L"\"";
  return q;
}

// Windows image-load failure codes as a child process exit code. A freshly
// written .exe can fail its FIRST process start while a file filter (e.g.
// Windows Defender real-time scanning) still holds the file; the failure
// surfaces as the child's exit status instead of a CreateProcessW error.
// These are OS error/status codes, not scenario exit values (the scenarios
// in this repo exit 0/1/2/3, and a deterministic scenario exits with the
// same code on the retry — see runScenario below).
bool isTransientSpawnFailure(std::uint32_t code) {
  // 259 ERROR_FILE_NOT_FOUND, 32 ERROR_SHARING_VIOLATION,
  // 126 ERROR_MOD_NOT_FOUND, 142 0xC0000142 STATUS_FATAL_APP_EXIT,
  // 193 ERROR_BAD_EXE_FORMAT, 1422 ERROR_APP_INIT_FAILURE.
  return code == 259u || code == 32u || code == 126u || code == 142u ||
         code == 193u || code == 1422u;
}

// One spawn+capture of a scenario binary. Returns true when the run failed
// transiently (no output captured and the exit code is an OS image-load
// failure), meaning the caller may retry once.
bool runScenarioOnce(const std::string& exe,
                     const std::vector<std::string>& args, RunResult& r) {
  r = RunResult{};
  const std::wstring exeW = toWide(exe);
  // Diagnostics (LOG-002): a failed scenario run must say WHICH binary was
  // attempted and whether it exists, not just a numeric exit code.
  const DWORD attrs = GetFileAttributesW(exeW.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    r.error = "scenario executable not found (lastError=" +
              std::to_string(GetLastError()) + "): " + exe;
    return false;
  }
  std::wstring cmd = exeW;
  for (const std::string& a : args) cmd += L" " + quoteArg(a);

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  HANDLE readH = INVALID_HANDLE_VALUE;
  HANDLE writeH = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&readH, &writeH, &sa, 0)) {
    r.error = "CreatePipe failed";
    return false;
  }
  // The child inherits the write end of the pipe.
  if (!SetHandleInformation(writeH, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    r.error = "SetHandleInformation failed";
    CloseHandle(readH);
    CloseHandle(writeH);
    return false;
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writeH;  // capture stdout
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);  // stays visible in the log
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                      nullptr, &si, &pi)) {
    r.error = "CreateProcessW failed (is the path correct?)";
    CloseHandle(readH);
    CloseHandle(writeH);
    return false;
  }
  CloseHandle(writeH);

  char buf[65536];
  std::string pending;
  for (;;) {
    DWORD n = 0;
    if (!PeekNamedPipe(readH, buf, sizeof buf, &n, nullptr, nullptr)) {
      r.error = "PeekNamedPipe failed";
      break;
    }
    if (n == 0) break;  // the scenario closed the pipe
    if (n > sizeof buf) n = sizeof buf;
    DWORD got = 0;
    if (!ReadFile(readH, buf, n, &got, nullptr)) {
      r.error = "ReadFile failed";
      break;
    }
    if (!appendChunk(r.lines, pending, buf, got, kMaxTicks)) {
      r.error = "scenario output is unbounded (line or tick count exceeds "
                "the contract)";
      break;
    }
  }
  CloseHandle(readH);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  r.exitCode = static_cast<int>(code);
  finishPending(r.lines, pending, r);
  if (r.exitCode != 0 && r.error.empty()) {
    r.error = "scenario process exited with code " + std::to_string(code) +
              " (command: " + exe + ")";
  }
  r.ok = r.error.empty();
  return !r.ok && r.lines.empty() &&
         isTransientSpawnFailure(static_cast<std::uint32_t>(r.exitCode));
}

// Last-write time of the scenario executable, as Unix-epoch seconds, for
// the failure diagnostic: distinguishes a file that is still being written
// (write time after the launch) from a scan/contention window on a
// finished file. 0 when it cannot be determined.
std::uint64_t lastWriteUnixSeconds(const std::wstring& exeW) {
  const HANDLE h = CreateFileW(
      exeW.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 0;
  BY_HANDLE_FILE_INFORMATION info{};
  uint64_t epoch = 0;
  if (GetFileInformationByHandle(h, &info)) {
    // FILETIME is 100-ns ticks since 1601-01-01; Unix epoch is 1970-01-01,
    // 11644473600 s later.
    const ULARGE_INTEGER ft{info.ftLastWriteTime.dwLowDateTime,
                            info.ftLastWriteTime.dwHighDateTime};
    epoch = (ft.QuadPart - 116444736000000000ull) / 10000000ull;
  }
  CloseHandle(h);
  return epoch;
}

// Bounded first-launch retry (one attempt, short delay): absorbs the
// Windows first-launch image-load race described above. The retry CANNOT
// mask scenario behavior: only a run that produced no output and died with
// an OS image-load code is retried, and a deterministic scenario fails
// identically on the retry, so the failure is still reported.
RunResult runScenario(const std::string& exe,
                      const std::vector<std::string>& args) {
  RunResult r;
  const bool transient = runScenarioOnce(exe, args, r);
  int attempts = 1;
  if (transient) {
    std::fprintf(stderr,
                 "laige-detcheck: first launch transiently failed (exit %d); "
                 "retrying once after 250 ms\n",
                 r.exitCode);
    Sleep(250);  // let the file filter settle before the single retry
    attempts = 2;
    runScenarioOnce(exe, args, r);
  }
  if (!r.ok) {
    const std::uint64_t wrote = lastWriteUnixSeconds(toWide(exe));
    std::fprintf(stderr,
                 "laige-detcheck: scenario run failed after %d attempt(s); "
                 "executable last written at unix %llu\n",
                 attempts, static_cast<unsigned long long>(wrote));
  }
  return r;
}

#else  // POSIX (Linux, macOS)

RunResult runScenario(const std::string& exe,
                      const std::vector<std::string>& args) {
  RunResult r;
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    r.error = "pipe() failed";
    return r;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    r.error = "fork() failed";
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return r;
  }
  if (pid == 0) {
    // Child: stdout goes to the pipe; stderr is inherited, so a scenario
    // crash report still reaches the CI log.
    if (::dup2(pipefd[1], 1) < 0) _exit(127);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(exe.c_str(), argv.data());
    _exit(127);  // execv failed: the path is wrong or not executable
  }
  ::close(pipefd[1]);
  char buf[65536];
  std::string pending;
  for (;;) {
    const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      r.error = "read() of the scenario stdout failed";
      break;
    }
    if (n == 0) break;  // EOF: the scenario finished
    if (!appendChunk(r.lines, pending, buf, static_cast<std::size_t>(n),
                     kMaxTicks)) {
      r.error = "scenario output is unbounded (line or tick count exceeds "
                "the contract)";
      break;
    }
  }
  ::close(pipefd[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    r.error = "waitpid() failed";
  }
  if (WIFEXITED(status)) {
    r.exitCode = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.exitCode = 128 + WTERMSIG(status);
    if (r.error.empty()) {
      r.error = "scenario process was killed by signal " +
                std::to_string(WTERMSIG(status));
    }
  }
  finishPending(r.lines, pending, r);
  if (r.exitCode != 0 && r.error.empty()) {
    r.error = "scenario process exited with code " + std::to_string(r.exitCode);
  }
  r.ok = r.error.empty();
  return r;
}

#endif  // _WIN32

// --- The built-in synthetic scenario (M0 stand-in) -------------------------

struct SyntheticBody {
  laige::fpx16_16 x{};
  laige::fpx16_16 y{};
  laige::fpx16_16 vx{};
  laige::fpx16_16 vy{};
};
using SyntheticWorld = std::array<SyntheticBody, kBodies>;

void initWorld(SyntheticWorld& w, laige::Prng& prng) {
  for (int i = 0; i < kBodies; ++i) {
    // Deterministic seed layout: body i starts at (8*(i/8), 8*(i%8)) —
    // an 8x8 grid spanning the whole 64-unit box, so both wrap branches
    // (x and y, low and high) are exercised within the default tick
    // budget — with a seeded velocity in [-2, 2].
    w[i].x = laige::fpx16_16::fromInt32((i / 8) * 8);
    w[i].y = laige::fpx16_16::fromInt32((i % 8) * 8);
    const int dvx = static_cast<int>(prng.next_range(0, 5)) - 2;
    const int dvy = static_cast<int>(prng.next_range(0, 5)) - 2;
    w[i].vx = laige::fpx16_16::fromInt32(dvx);
    w[i].vy = laige::fpx16_16::fromInt32(dvy);
  }
}

// One tick: fixed-order integration (PRD 10.3: deterministic iteration
// order), wrap into the 64-unit box (one wrap per axis per tick is
// enough: |v| ≤ 2), then one seeded input event (the Prng picks the
// body index and a nudge in [-4, 3] applied to x).
void stepWorld(SyntheticWorld& w, laige::Prng& prng) {
  using laige::fpx16_16;
  const fpx16_16 bound = fpx16_16::fromInt32(kBoxUnits);
  for (SyntheticBody& b : w) {
    b.x = fpx16_16::add(b.x, b.vx);
    b.y = fpx16_16::add(b.y, b.vy);
    if (b.x >= bound) {
      b.x = fpx16_16::sub(b.x, bound);
    } else if (b.x < fpx16_16{}) {
      b.x = fpx16_16::add(b.x, bound);
    }
    if (b.y >= bound) {
      b.y = fpx16_16::sub(b.y, bound);
    } else if (b.y < fpx16_16{}) {
      b.y = fpx16_16::add(b.y, bound);
    }
  }
  const int i = static_cast<int>(prng.next_range(0, kBodies));
  const int nudge = static_cast<int>(prng.next_range(0, 8)) - 4;
  w[i].x = fpx16_16::add(w[i].x, fpx16_16::fromInt32(nudge));
}

// FNV-1a 64 over (tick, seed, every body's four raw words); big-endian
// per word (endianness-independent) — the same constants as the
// math_fixed known-answer test (house FNV choice).
std::uint64_t hashWorld(std::uint64_t tick, std::uint64_t seed,
                        const SyntheticWorld& w) {
  std::uint64_t h = kFnvOffsetBasis;
  auto feed = [&h](std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      h ^= (v >> shift) & 0xFFull;
      h *= kFnvPrime;
    }
  };
  feed(tick);
  feed(seed);
  for (const SyntheticBody& b : w) {
    feed(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.x.raw)));
    feed(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.y.raw)));
    feed(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.vx.raw)));
    feed(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.vy.raw)));
  }
  return h;
}

struct RunSpec {
  std::string label;
  int ticks;
  std::uint64_t seed;
  bool perturb;  // run-b of synthetic-perturbed: the +1 nudge at tick 7
};

// One built-in run: the state after processing tick t is hashed and
// emitted as the line for tick t (the first line is tick 0).
std::vector<std::string> runBuiltIn(const RunSpec& spec) {
  laige::Prng prng(spec.seed);
  SyntheticWorld w;
  initWorld(w, prng);
  std::vector<std::string> lines;
  lines.reserve(static_cast<std::size_t>(spec.ticks));
  for (int t = 0; t < spec.ticks; ++t) {
    stepWorld(w, prng);
    if (spec.perturb && t == kPerturbTick) {
      w[3].x = laige::fpx16_16::add(w[3].x, laige::fpx16_16::fromInt32(1));
    }
    lines.push_back(formatHashLine(static_cast<std::uint64_t>(t),
                                   hashWorld(static_cast<std::uint64_t>(t),
                                             spec.seed, w)));
  }
  return lines;
}

std::string formatSeed(std::uint64_t seed) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx",
                static_cast<unsigned long long>(seed));
  return buf;
}

// --- Comparison and reporting ----------------------------------------------

struct CompareResult {
  bool ok = false;
  std::size_t firstDiffTick = 0;
  std::string lineA;
  std::string lineB;
};

CompareResult compareStreams(const std::vector<std::string>& a,
                             const std::vector<std::string>& b) {
  CompareResult c;
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      c.ok = false;
      c.firstDiffTick = i;
      c.lineA = a[i];
      c.lineB = b[i];
      return c;
    }
  }
  if (a.size() != b.size()) {
    // One stream ended early: a divergence at the boundary (a truncated
    // run is a determinism failure, not a match).
    c.ok = false;
    c.firstDiffTick = n;
    c.lineA = "stream ends: " + std::to_string(a.size()) + " ticks";
    c.lineB = "stream ends: " + std::to_string(b.size()) + " ticks";
    return c;
  }
  c.ok = true;
  return c;
}

std::string basenameOf(std::string_view path) {
  const auto pos = path.find_last_of("/\\");
  return pos == std::string_view::npos ? std::string(path)
                                       : std::string(path.substr(pos + 1));
}

void report(std::string_view scenarioName, const CompareResult& c,
            std::size_t ticks, std::string_view labelA,
            std::string_view labelB) {
  if (c.ok) {
    std::printf("detcheck scenario=%s result=OK ticks=%llu\n",
                std::string(scenarioName).c_str(),
                static_cast<unsigned long long>(ticks));
    std::printf("  run-a: %s\n", std::string(labelA).c_str());
    std::printf("  run-b: %s\n", std::string(labelB).c_str());
  } else {
    std::printf("detcheck scenario=%s result=DIVERGED first_diff_tick=%llu\n",
                std::string(scenarioName).c_str(),
                static_cast<unsigned long long>(c.firstDiffTick));
    std::printf("  run-a: %s\n", c.lineA.c_str());
    std::printf("  run-b: %s\n", c.lineB.c_str());
  }
}

// --- Argument parsing ------------------------------------------------------

struct Args {
  std::string scenario;  // mode 1
  std::string runA;      // mode 2
  std::string runB;      // mode 2
  bool ticksSet = false;
  int ticks = kDefaultTicks;
  bool seedSet = false;
  std::uint64_t seed = kDefaultSeed;
  std::vector<std::string> positionals;  // mode 2 pass-through
};

void printUsage(std::FILE* out) {
  std::fprintf(
      out,
      "usage: laige-detcheck --scenario=<name> [--ticks=N] "
      "[--seed=HEX|DEC]\n"
      "       laige-detcheck --run-a=<scenario-bin-A> --run-b="
      "<scenario-bin-B> [-- scenario-args...]\n"
      "       laige-detcheck --help\n"
      "\n"
      "  --scenario          built-in scenario: synthetic | "
      "synthetic-perturbed\n"
      "  --run-a/--run-b     two builds of the same scenario (two build\n"
      "                      configurations)\n"
      "  --ticks             built-in scenario tick count (1..%d, "
      "default %d)\n"
      "  --seed              built-in scenario seed (0xHEX or decimal)\n"
      "  --                  end of tool options; the remaining args\n"
      "                      are passed to both scenario binaries\n"
      "\n"
      "Exit codes: 0 = match, 1 = divergence, 2 = error (usage, unknown\n"
      "scenario, scenario failure, contract violation).\n",
      kMaxTicks, kDefaultTicks);
}

bool parseArgs(int argc, char** argv, Args& a) {
  bool passThrough = false;  // everything after `--` is a scenario argument
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (passThrough) {
      a.positionals.push_back(arg);
      continue;
    }
    if (arg == "--") {
      passThrough = true;
      continue;
    }
    const auto eq = arg.find('=');
    const bool hasValue = eq != std::string::npos && eq + 1 < arg.size();
    const std::string key = hasValue ? arg.substr(0, eq) : arg;
    const std::string value = hasValue ? arg.substr(eq + 1) : "";

    if (key == "--scenario") {
      if (!hasValue) return false;
      a.scenario = value;
    } else if (key == "--run-a") {
      if (!hasValue) return false;
      a.runA = value;
    } else if (key == "--run-b") {
      if (!hasValue) return false;
      a.runB = value;
    } else if (key == "--ticks") {
      if (!hasValue || value.empty()) return false;
      std::uint64_t v = 0;
      for (const char c : value) {
        if (c < '0' || c > '9') return false;
        if (v > (static_cast<std::uint64_t>(kMaxTicks) -
                 static_cast<std::uint64_t>(c - '0')) / 10) {
          return false;  // beyond the contract bound
        }
        v = v * 10 + std::uint64_t(c - '0');
      }
      if (v < 1) return false;
      a.ticksSet = true;
      a.ticks = static_cast<int>(v);
    } else if (key == "--seed") {
      if (!hasValue) return false;
      a.seedSet = true;
      a.seed = std::strtoull(value.c_str(), nullptr, 0);  // 0x or decimal
    } else if (key == "--help" || key == "-h") {
      printUsage(stdout);
      std::exit(0);
    } else {
      // Unknown option, or a bare token before `--`: scenario arguments
      // must come after `--`, so this is always a usage error (strict
      // surface, API-008).
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parseArgs(argc, argv, a)) {
    printUsage(stderr);
    return 2;
  }
  const bool mode1 = !a.scenario.empty();
  const bool mode2 = !a.runA.empty() || !a.runB.empty();
  if (mode1 && mode2) {
    std::fprintf(stderr,
                 "laige-detcheck: --scenario and --run-a/--run-b are "
                 "mutually exclusive\n");
    printUsage(stderr);
    return 2;
  }
  if (mode1 && !a.positionals.empty()) {
    std::fprintf(stderr,
                 "laige-detcheck: scenario args (after --) are only "
                 "allowed with --run-a/--run-b\n");
    printUsage(stderr);
    return 2;
  }
  if (!mode1 && (a.ticksSet || a.seedSet)) {
    std::fprintf(stderr,
                 "laige-detcheck: --ticks/--seed apply to the built-in "
                 "scenario only\n");
    printUsage(stderr);
    return 2;
  }
  if (!mode1 && !mode2) {
    printUsage(stderr);
    return 2;
  }
  if (mode2 && (a.runA.empty() || a.runB.empty())) {
    std::fprintf(stderr,
                 "laige-detcheck: both --run-a and --run-b are required\n");
    printUsage(stderr);
    return 2;
  }
  if (mode1 && a.scenario != "synthetic" && a.scenario != "synthetic-perturbed") {
    std::fprintf(stderr,
                 "laige-detcheck: unknown scenario '%s' (built-in: "
                 "synthetic, synthetic-perturbed)\n",
                 a.scenario.c_str());
    return 2;
  }

  // --- Mode 2: two scenario binaries (two build configurations) ----------
  if (mode2) {
    const RunResult resA = runScenario(a.runA, a.positionals);
    if (!resA.ok) {
      std::fprintf(stderr, "laige-detcheck: scenario run-a: %s\n",
                   resA.error.c_str());
      return 2;
    }
    const std::string errA = validateStream(resA.lines, kMaxTicks);
    if (!errA.empty()) {
      std::fprintf(stderr, "laige-detcheck: scenario run-a: %s\n",
                   errA.c_str());
      return 2;
    }
    const RunResult resB = runScenario(a.runB, a.positionals);
    if (!resB.ok) {
      std::fprintf(stderr, "laige-detcheck: scenario run-b: %s\n",
                   resB.error.c_str());
      return 2;
    }
    const std::string errB = validateStream(resB.lines, kMaxTicks);
    if (!errB.empty()) {
      std::fprintf(stderr, "laige-detcheck: scenario run-b: %s\n",
                   errB.c_str());
      return 2;
    }
    const CompareResult c = compareStreams(resA.lines, resB.lines);
    report(basenameOf(a.runA) + " vs " + basenameOf(a.runB), c,
           resA.lines.size(), a.runA, a.runB);
    return c.ok ? 0 : 1;
  }

  // --- Mode 1: built-in scenario, two in-process runs --------------------
  const RunSpec specA{
      std::string(a.scenario) + "[seed=" + formatSeed(a.seed) +
          " ticks=" + std::to_string(a.ticks) +
          " build=" + LAIGE_DETCHECK_BUILD_TYPE + "]",
      a.ticks, a.seed, false};
  const RunSpec specB{
      std::string(a.scenario) + "[seed=" + formatSeed(a.seed) +
          " ticks=" + std::to_string(a.ticks) +
          " build=" + LAIGE_DETCHECK_BUILD_TYPE +
          (a.scenario == "synthetic-perturbed"
               ? " perturb_tick=" + std::to_string(kPerturbTick)
               : "") +
          "]",
      a.ticks, a.seed, a.scenario == "synthetic-perturbed"};
  const std::vector<std::string> linesA = runBuiltIn(specA);
  const std::vector<std::string> linesB = runBuiltIn(specB);
  const CompareResult c = compareStreams(linesA, linesB);
  report(a.scenario, c, linesA.size(), specA.label, specB.label);
  return c.ok ? 0 : 1;
}
