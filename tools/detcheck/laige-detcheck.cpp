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
//   laige-detcheck --compare-combined=<combined-stream-file>
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
// Modes 2+3 (--run-a/--run-b, then --compare-combined): the real mode
// from M1-DET-04 on — two builds of the same scenario source (two build
// configurations) are executed and their hash streams compared.
// Everything after a `--` separator is passed to both scenario binaries
// (scenario arguments that could look like tool flags are unambiguous
// because of the separator).
//
// The comparison is TWO-STAGE on every platform. The CI Windows runner
// does not deliver handles the checker process creates (pipes or files,
// even with the INHERIT bit set, even duplicated) to child processes
// through STARTUPINFO — measured in the M0-TEST-01 CI (runs 24/25):
// only handles the process itself inherited from its parent are
// delivered. A scenario child therefore cannot be given a capture pipe;
// its stdout must be the checker's own stdout, which the harness
// (the CTest check script's execute_process) captures:
//
//   phase 1 (--run-a/--run-b): spawn each binary WITHOUT redirecting its
//     stdout (plain inheritance). Each run's ticks land in the harness's
//     capture of this process's stdout, delimited by the marker lines
//       @@DETCHK-RUN-A-BEGIN@@ ... @@DETCHK-RUN-A-END <exitcode>@@
//       @@DETCHK-RUN-B-BEGIN@@ ... @@DETCHK-RUN-B-END <exitcode>@@
//     Phase 1 exits 0 when both scenario processes ran to completion,
//     2 on spawn failure or scenario failure (the reason on stderr).
//     It does NOT read or compare the ticks.
//   phase 2 (--compare-combined=<file>): the check script writes the
//     captured combined stream to a file and re-runs detcheck on it.
//     Phase 2 splits the stream at the markers, re-runs the stream
//     contract on each run, compares, and reports (the report below).
//
// Report (phase-2 stdout, stable and machine-greppable — LOG-001):
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
//   (--run-a/--run-b, phase 1: 0 when both scenario processes ran to
//    completion, 2 on any spawn or scenario failure; the 0/1 comparison
//    result comes from the --compare-combined phase)
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
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// --- Diagnostic sink (M0-TEST-01 Windows CI investigation) ---------------
// Every diagnostic line goes to stderr AND is accumulated in g_diag. The
// test harness sets LAIGE_DETCHECK_DIAG_FILE; main() flushes g_diag there
// on every exit path (RAII). ctest hides the output of passing tests, so
// this file is how a passing instance's diagnostics reach the CI log (the
// Windows job cats the files after ctest).
std::string g_diag;

void diagf(const char* fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    const std::size_t len = static_cast<std::size_t>(
        n < static_cast<int>(sizeof buf) ? n : static_cast<int>(sizeof buf) - 1);
    std::fprintf(stderr, "%s", buf);
    g_diag.append(buf, len);
  }
}

// Read an environment variable into a fixed buffer (portable: MSVC
// degrades getenv to C4996, so use getenv_s there).
bool readEnvVar(const char* name, char* buf, std::size_t size) {
#ifdef _WIN32
  std::size_t len = 0;
  return getenv_s(&len, buf, static_cast<std::size_t>(size), name) == 0 &&
         len > 0;
#else
  const char* v = std::getenv(name);
  if (v == nullptr) return false;
  const std::size_t n = std::strlen(v);
  if (n >= size) return false;
  std::memcpy(buf, v, n + 1);
  return true;
#endif
}

// Flush g_diag to $LAIGE_DETCHECK_DIAG_FILE (truncating stale content) on
// every exit path from main().
struct DiagFlush {
  ~DiagFlush() {
    char path[1024] = {};
    if (readEnvVar("LAIGE_DETCHECK_DIAG_FILE", path, sizeof path)) {
      std::FILE* f = nullptr;
#ifdef _WIN32
      if (fopen_s(&f, path, "w") != 0) f = nullptr;
#else
      f = std::fopen(path, "w");
#endif
      if (f != nullptr) {
        std::fwrite(g_diag.data(), 1, g_diag.size(), f);
        std::fclose(f);
      }
    }
  }
};

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

// Environment diagnostic (CI comparison between failing and passing
// process instances): the CWD, every environment variable name, and the
// full values of the test-wiring variables (LAIGE_/CTEST_ prefixes) and
// PATH. GetEnvironmentStringsW returns a double-NUL-terminated block of
// "NAME=VALUE" entries.
void dumpEnvironmentDiagnostics() {
  wchar_t cwd[1024] = {};
  const DWORD cwdLen = GetCurrentDirectoryW(1024, cwd);
  std::string cwdUtf8;
  if (cwdLen > 0) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, cwd, -1, nullptr, 0,
                                       nullptr, nullptr);
    if (n > 0) {
      cwdUtf8.resize(static_cast<std::size_t>(n - 1));
      WideCharToMultiByte(CP_UTF8, 0, cwd, -1, cwdUtf8.data(), n, nullptr,
                          nullptr);
    }
  }
  diagf("laige-detcheck: env cwd=%s\n", cwdUtf8.c_str());
  LPWCH env = GetEnvironmentStringsW();
  if (env == nullptr) {
    diagf("laige-detcheck: env: unavailable\n");
    return;
  }
  // Only the test-wiring variables (compact; the full name dump was log
  // noise and has served its purpose — the wiring is intact).
  int testVars = 0;
  for (const wchar_t* block = env; *block != L'\0';) {
    const size_t len = wcslen(block);
    const wchar_t* eq = wcschr(block, L'=');
    const std::wstring name(block, eq ? static_cast<size_t>(eq - block)
                                      : len);
    const std::wstring value(eq ? eq + 1 : L"");
    const bool isTestVar = name.rfind(L"LAIGE_", 0) == 0 ||
                           name.rfind(L"CTEST_", 0) == 0 ||
                           name == L"PATH";
    if (isTestVar) {
      std::string nameUtf8, valueUtf8;
      {
        const int n = WideCharToMultiByte(CP_UTF8, 0, name.data(),
                                           static_cast<int>(name.size()),
                                           nullptr, 0, nullptr, nullptr);
        nameUtf8.resize(static_cast<std::size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, name.data(),
                            static_cast<int>(name.size()), nameUtf8.data(), n,
                            nullptr, nullptr);
        const int m = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
        valueUtf8.resize(static_cast<std::size_t>(m));
        WideCharToMultiByte(CP_UTF8, 0, value.data(),
                            static_cast<int>(value.size()), valueUtf8.data(),
                            m, nullptr, nullptr);
      }
      diagf("laige-detcheck: env %s=%s\n", nameUtf8.c_str(),
            valueUtf8.c_str());
      ++testVars;
    }
    block += len + 1;
  }
  FreeEnvironmentStringsW(env);
  diagf("laige-detcheck: env: %d test-wiring variables present\n", testVars);
}

// Handle flag bits for the diagnostics. GetHandleInformation uses
// HANDLE_FLAG_INHERIT = 0x1 and HANDLE_FLAG_PROTECT_FROM_CLOSE = 0x2
// (NOT 0x80/0x100 - those values belong to no handle API).
std::string handleBits(HANDLE h) {
  DWORD info = 0;
  GetHandleInformation(h, &info);
  char b[96];
  std::snprintf(b, sizeof b, "inherit=%d protect=%d (raw=0x%lx)",
                (info & HANDLE_FLAG_INHERIT) ? 1 : 0,
                (info & HANDLE_FLAG_PROTECT_FROM_CLOSE) ? 1 : 0, info);
  return std::string(b);
}

// One probe launch. With redirectStdio the child's stdout (and usually
// stderr) are set through STARTUPINFO; outMode selects the stdout source:
//   0 = a fresh CreatePipe write end we drain (baseline);
//   1 = detcheck's OWN stdout/stderr handles (kernel-assigned at process
//      start) - the child's marker then lands in detcheck's own streams
//      (harness-captured), so capturedOut/capturedErr stay empty;
//   2 = a fresh CreatePipe whose write end is first re-created through
//      DuplicateHandle(dwInheritable=TRUE) before use - tests whether a
//      freshly duplicated inheritable handle is delivered where the
//      original pipe handle is not;
//   3 = a self-created FILE handle (a temp file) as the child's stdout -
//      run 24 showed STARTUPINFO delivers detcheck's kernel-assigned
//      handles but NOT self-created pipes (even duplicated); if a
//      self-created FILE handle delivers, file capture is the scenario
//      path. The file is read back after the child exits (capturedOut
//      carries the marker; capturedErr still comes from a fresh pipe).
// Without redirectStdio the child inherits detcheck's own standard
// handles (the plain-inheritance control). Returns false only when the
// spawn itself failed (errOut).
bool probeAttempt(const wchar_t* appname, std::wstring cmd,
                  bool redirectStdio, int outMode, DWORD& exitCode,
                  std::string& capturedOut, std::string& capturedErr,
                  std::string& errOut) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  HANDLE outR = INVALID_HANDLE_VALUE, outW = INVALID_HANDLE_VALUE;
  HANDLE errR = INVALID_HANDLE_VALUE, errW = INVALID_HANDLE_VALUE;
  HANDLE dupOut = INVALID_HANDLE_VALUE;
  HANDLE outTarget = INVALID_HANDLE_VALUE;
  std::wstring outFile;  // mode 3 only: the child's stdout file
  bool outDrained = false, errDrained = false;
  auto cleanup = [&]() {
    if (outR != INVALID_HANDLE_VALUE) CloseHandle(outR);
    if (outW != INVALID_HANDLE_VALUE) CloseHandle(outW);
    if (dupOut != INVALID_HANDLE_VALUE) CloseHandle(dupOut);
    if (errR != INVALID_HANDLE_VALUE) CloseHandle(errR);
    if (errW != INVALID_HANDLE_VALUE) CloseHandle(errW);
  };
  if (redirectStdio) {
    if (outMode == 1) {
      // Kernel-assigned: detcheck's own stdout; the child's stderr goes
      // to detcheck's own stderr (both harness-captured).
      outTarget = GetStdHandle(STD_OUTPUT_HANDLE);
    } else if (outMode == 3) {
      // Self-created FILE handle (temp file) as the child's stdout:
      // if this delivers where self-created pipes do not, file capture
      // is the scenario path. Read back after the child exits.
      wchar_t tmp[512] = {};
      const DWORD tmpLen = GetTempPathW(512, tmp);
      if (tmpLen == 0 || tmpLen >= 512) {
        errOut = "GetTempPathW failed (lastError=" +
                 std::to_string(GetLastError()) + ")";
        cleanup();
        return false;
      }
      outFile = std::wstring(tmp, static_cast<std::size_t>(tmpLen)) +
                L"laige-detcheck-probe-" +
                std::to_wstring(GetCurrentProcessId()) + L".tmp";
      const HANDLE f = CreateFileW(outFile.c_str(), GENERIC_WRITE,
                                   FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE) {
        errOut = "CreateFileW failed (lastError=" +
                 std::to_string(GetLastError()) + ")";
        cleanup();
        return false;
      }
      outTarget = f;
    } else {
      if (!CreatePipe(&outR, &outW, &sa, 0)) {
        errOut = "CreatePipe failed (lastError=" +
                 std::to_string(GetLastError()) + ")";
        cleanup();
        return false;
      }
      outDrained = true;
      if (outMode == 2) {
        // Re-create the write end as a fresh inheritable duplicate.
        if (!DuplicateHandle(GetCurrentProcess(), outW, GetCurrentProcess(),
                             &dupOut, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
          errOut = "DuplicateHandle failed (lastError=" +
                   std::to_string(GetLastError()) + ")";
          cleanup();
          return false;
        }
        outTarget = dupOut;
      } else {
        outTarget = outW;
      }
    }
    if (outMode != 1 && !CreatePipe(&errR, &errW, &sa, 0)) {
      errOut = "CreatePipe(err) failed (lastError=" +
               std::to_string(GetLastError()) + ")";
      cleanup();
      return false;
    }
    errDrained = (outMode != 1);
    if (outDrained) {
      SetHandleInformation(outW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    if (errDrained) {
      SetHandleInformation(errW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    if (outMode == 1) {
      diagf("laige-detcheck: probe handles out=kernel-fd1 (0x%p %s) "
            "err=kernel-fd2 (0x%p %s)\n",
            static_cast<void*>(outTarget), handleBits(outTarget).c_str(),
            static_cast<void*>(GetStdHandle(STD_ERROR_HANDLE)),
            handleBits(GetStdHandle(STD_ERROR_HANDLE)).c_str());
    } else if (outMode == 3) {
      diagf("laige-detcheck: probe handles out=file 0x%p %s"
            " | err r=0x%p w=0x%p type=%lu %s\n",
            static_cast<void*>(outTarget), handleBits(outTarget).c_str(),
            static_cast<void*>(errR), static_cast<void*>(errW),
            GetFileType(errW), handleBits(errW).c_str());
    } else {
      std::string dupNote;
      if (outMode == 2) {
        char nb[128];
        std::snprintf(nb, sizeof nb, " | wdup=0x%p %s",
                      static_cast<void*>(dupOut),
                      handleBits(dupOut).c_str());
        dupNote = nb;
      }
      diagf("laige-detcheck: probe handles out r=0x%p w=0x%p type=%lu %s%s"
            "| err r=0x%p w=0x%p type=%lu %s\n",
            static_cast<void*>(outR), static_cast<void*>(outW),
            GetFileType(outW), handleBits(outTarget).c_str(),
            dupNote.c_str(), static_cast<void*>(errR),
            static_cast<void*>(errW), GetFileType(errW),
            handleBits(errW).c_str());
    }
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = redirectStdio ? STARTF_USESTDHANDLES : 0;
  if (redirectStdio) {
    si.hStdOutput = outTarget;
    si.hStdError = outMode == 1 ? GetStdHandle(STD_ERROR_HANDLE) : errW;
  }
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(appname, cmd.data(), nullptr, nullptr, TRUE, 0,
                       nullptr, nullptr, &si, &pi)) {
    errOut = "CreateProcessW failed (lastError=" +
             std::to_string(GetLastError()) + ")";
    cleanup();
    return false;
  }
  // The child now owns the write ends; drop our copies.
  if (outW != INVALID_HANDLE_VALUE) CloseHandle(outW);
  if (dupOut != INVALID_HANDLE_VALUE) CloseHandle(dupOut);
  if (errW != INVALID_HANDLE_VALUE) CloseHandle(errW);
  if (redirectStdio && (outDrained || errDrained)) {
    auto drain = [](HANDLE r, std::string& out) {
      char buf[256];
      for (;;) {
        if (WaitForSingleObject(r, INFINITE) != WAIT_OBJECT_0) break;
        DWORD n = 0;
        if (!PeekNamedPipe(r, buf, sizeof buf, &n, nullptr, nullptr)) break;
        if (n == 0) break;
        DWORD got = 0;
        if (!ReadFile(r, buf, n, &got, nullptr)) break;
        out.append(buf, got);
      }
      CloseHandle(r);
    };
    if (outDrained) drain(outR, capturedOut);
    if (errDrained) drain(errR, capturedErr);
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  // Mode 3: the child's stdout landed in a file; read it back now that
  // the child has exited (all of its writes are complete).
  if (!outFile.empty()) {
    const HANDLE rf = CreateFileW(outFile.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rf != INVALID_HANDLE_VALUE) {
      char buf[4096];
      for (;;) {
        DWORD n = 0;
        if (!ReadFile(rf, buf, sizeof buf, &n, nullptr) || n == 0) break;
        capturedOut.append(buf, n);
      }
      CloseHandle(rf);
    }
    DeleteFileW(outFile.c_str());
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

// Control probe (diagnostic): in this process instance, launch known-good
// children through the exact machinery used for scenario runs and record
// what each one received:
//
//   attempt 1: laige-detcheck-probe (the diagnostic helper from our own
//              module directory) — its stderr self-reports the std
//              handles the kernel assigned to it (value/type/flags),
//              captured in capturedErr; its stdout marker is captured in
//              capturedOut.
//   attempt 2: same helper, explicit lpApplicationName.
//   attempt 3: cmd.exe /c echo marker — the classic control.
//   attempt 4: same helper, NO STARTUPINFO redirection — plain handle
//              inheritance only (stdout follows detcheck's own fd1).
//   attempt 5: same helper, STARTUPINFO with detcheck's OWN (kernel-
//              assigned) fd1/fd2 — if this delivers while 1-3 do not,
//              the failure is specific to handles this process created.
//              (Success shows up as an extra PROBE_HELPER_OK line in
//              detcheck's own stdout and an extra self-report line in
//              its stderr; capturedOut/capturedErr stay empty by design.)
//   attempt 6: same helper, STARTUPINFO with a fresh pipe whose write
//              end was re-created via DuplicateHandle(dwInheritable) —
//              tests whether a freshly duplicated inheritable handle is
//              delivered where the original pipe handle is not.
//   attempt 7: same helper, STARTUPINFO with a self-created FILE handle
//              (temp file) as the child's stdout — if this delivers
//              where self-created pipes do not (run 24: kernel-assigned
//              handles delivered, self-created pipes not, even when
//              duplicated), file capture is the scenario path. Success
//              shows up as PROBE_HELPER_OK inside capturedOut.
void probeControlSpawn() {
  // The helper lives next to us in the build bin directory.
  wchar_t mod[1024] = {};
  const DWORD modLen = GetModuleFileNameW(nullptr, mod, 1024);
  std::wstring helper;
  if (modLen > 0 && modLen < 1024) {
    const std::wstring m(mod, modLen);
    const std::size_t slash = m.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
      helper = m.substr(0, slash + 1) + L"laige-detcheck-probe.exe";
    }
  }
  // Non-const so .data() yields wchar_t* for CreateProcessW (C++20).
  const std::wstring helperCmd = L"\"" + helper + L"\"";
  const std::wstring controlCmd = L"cmd.exe /c echo LAIGE_DETCHECK_CONTROL_OK";
  auto runAttempt = [&](int n, const wchar_t* app, const std::wstring& cmd,
                         bool redirect, int outMode, const char* desc) {
    DWORD code = 0;
    std::string out, err, why;
    const bool ok = probeAttempt(app, cmd, redirect, outMode, code, out, err,
                                 why);
    diagf("laige-detcheck: control probe attempt %d (%s): spawned=%d "
          "exit=%lu out='%s' err='%s' why=%s\n",
          n, desc, ok ? 1 : 0, static_cast<unsigned long>(code), out.c_str(),
          err.c_str(), why.c_str());
  };
  if (!helper.empty()) {
    runAttempt(1, nullptr, helperCmd, true, 0, "helper, cmdline path");
    runAttempt(2, helper.c_str(), helperCmd, true, 0, "helper, explicit app");
    // Kernel-assigned handles through STARTUPINFO: if this one delivers
    // while the fresh-pipe ones do not, the failure is specific to
    // handles this process created itself.
    runAttempt(5, nullptr, helperCmd, true, 1,
               "helper, kernel fd1/fd2 via STARTUPINFO");
    // Fresh pipe whose write end is re-created as an inheritable
    // duplicate: if this delivers where the original did not, use the
    // DuplicateHandle path for scenario capture.
    runAttempt(6, nullptr, helperCmd, true, 2,
               "helper, dup handle via STARTUPINFO");
    // Self-created FILE handle as the child's stdout (temp file, read
    // back after the child exits): if this delivers where self-created
    // pipes do not, file capture is the scenario path.
    runAttempt(7, nullptr, helperCmd, true, 3,
               "helper, file via STARTUPINFO");
  }
  runAttempt(3, nullptr, controlCmd, true, 0, "cmd marker");
  runAttempt(4, nullptr, helperCmd, false, 0, "helper, no redirect (control)");
}

// Best-effort parent-process identification (M0-TEST-01 Windows CI
// diagnosis): a diag file can be written by a detcheck invocation OTHER
// than the check script's; the parent's pid and command line say who
// launched this process. NtQueryInformationProcess is resolved through
// GetProcAddress (no new import; both process-info classes are stable on
// x64). Every step is best-effort: any failure yields (unavailable).
std::string parentProcessInfo() {
  struct Pbi {
    long exitStatus;
    void* peb;
    unsigned long long affinity;
    unsigned char priority;
    unsigned long pid;
    unsigned long long inheritedFrom;
  } pbi = {};
  typedef long(* NtQIP_t)(HANDLE, int, void*, unsigned long, unsigned long*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return "parent=(unavailable)";
  const NtQIP_t ntq = reinterpret_cast<NtQIP_t>(
      GetProcAddress(ntdll, "NtQueryInformationProcess"));
  if (!ntq) return "parent=(unavailable)";
  if (ntq(GetCurrentProcess(), 0, &pbi, sizeof pbi, nullptr) != 0) {
    return "parent=(unavailable)";
  }
  std::string s = "parent=" +
                  std::to_string(static_cast<unsigned long>(pbi.inheritedFrom));
  std::string cmd;
  HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                          static_cast<DWORD>(pbi.inheritedFrom));
  if (ph) {
    struct Us {
      unsigned short length;
      unsigned short maximumLength;
      wchar_t* buffer;
    } us = {};
    unsigned long need = 0;
    if (ntq(ph, 60, &us, sizeof us, &need) != 0 && need > sizeof us) {
      std::vector<wchar_t> buf(need / 2 + 1, 0);
      us.buffer = buf.data();
      us.maximumLength = static_cast<unsigned short>(need);
      if (ntq(ph, 60, &us, sizeof us, nullptr) == 0 && us.length > 0) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf.data(),
                                           static_cast<int>(us.length / 2),
                                           nullptr, 0, nullptr, nullptr);
        if (n > 0) {
          cmd.resize(static_cast<std::size_t>(n));
          WideCharToMultiByte(CP_UTF8, 0, buf.data(),
                              static_cast<int>(us.length / 2), cmd.data(), n,
                              nullptr, nullptr);
        }
      }
    }
    CloseHandle(ph);
  }
  s += cmd.empty() ? " parent-cmd=(unavailable)"
                  : " parent-cmd=\"" + cmd + "\"";
  return s;
}

#else  // POSIX (Linux, macOS)

// POSIX counterpart of the Windows parent-process identification: the
// parent pid plus its /proc command line (Linux; macOS has no /proc, so
// only the pid is reported there).
std::string parentProcessInfo() {
  const int pp = static_cast<int>(::getppid());
  std::string s = "parent=" + std::to_string(pp);
  char path[128];
  std::snprintf(path, sizeof path, "/proc/%d/cmdline", pp);
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    return s + " parent-cmd=(unavailable)";
  }
  char buf[1024];
  const std::size_t n = std::fread(buf, 1, sizeof buf, f);
  std::fclose(f);
  std::string cmd(buf, n);
  for (char& c : cmd) {
    if (c == '\0') c = ' ';
  }
  while (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
  s += cmd.empty() ? " parent-cmd=(unavailable)"
                  : " parent-cmd=\"" + cmd + "\"";
  return s;
}

#endif  // _WIN32

// --- Scenario spawn with markers (mode 2, phase 1) -------------------------
//
// Two-stage capture, all platforms:
//
// The CI Windows runner does NOT deliver handles this process creates
// (pipes or files, even with the INHERIT bit confirmed set and even
// duplicated) to child processes through STARTUPINFO - measured in the
// M0-TEST-01 CI (runs 24/25): only handles this process itself
// inherited from its parent (its kernel-assigned fd0/1/2) are delivered.
// A scenario child therefore cannot be given a capture pipe it inherits;
// its stdout must BE this process's stdout (plain inheritance), which the
// harness (the CTest check script's execute_process) captures.
//
// So mode 2 runs in two phases:
//   phase 1 (this function, per run): emit `@@DETCHK-RUN-<L>-BEGIN@@`,
//     spawn the scenario child WITHOUT redirecting its stdout (it
//     inherits this process's stdout), wait for it, emit
//     `@@DETCHK-RUN-<L>-END <exitcode>@@`. The child's ticks land between
//     the markers in the harness's capture of this process's stdout.
//   phase 2 (--compare-combined): the check script hands the combined
//     stream back to detcheck, which splits it at the markers, re-runs
//     the stream contract on each run, and compares.
//
// This function does not read the ticks. It returns the child's exit
// code, or -1 when the spawn itself failed (error set).
int spawnScenarioWithMarkers(const std::string& exe,
                             const std::vector<std::string>& args,
                             const std::string& label, std::string& error) {
  // The env dump and control probes describe THIS process instance; run
  // them once per process and BEFORE the first BEGIN marker, so their
  // (probe) stdout, if any, precedes the marker windows and never lands
  // inside an extracted run stream.
  static bool probed = false;
  if (!probed) {
#ifdef _WIN32
    dumpEnvironmentDiagnostics();
    probeControlSpawn();
#endif
    probed = true;
  }
  // The BEGIN marker must reach the capture BEFORE the child is born, so
  // the child's ticks (on the same stream) cannot precede it.
  std::printf("@@DETCHK-RUN-%s-BEGIN@@\n", label.c_str());
  std::fflush(stdout);
  int result = -1;
#ifdef _WIN32
  {
    const std::wstring exeW = toWide(exe);
    const DWORD attrs = GetFileAttributesW(exeW.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      error = "scenario executable not found (lastError=" +
              std::to_string(GetLastError()) + "): " + exe;
    } else {
      std::wstring cmd = exeW;
      for (const std::string& a : args) cmd += L" " + quoteArg(a);
      PROCESS_INFORMATION pi{};
      // No STARTUPINFO: the child inherits this process's standard handles
      // (the harness's capture). No self-created handle is involved - the
      // kind the CI Windows runner never delivers (see above).
      if (!CreateProcessW(exeW.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                          0, nullptr, nullptr, nullptr, &pi)) {
        error = "CreateProcessW failed (is the path correct?)";
      } else {
        diagf("laige-detcheck: spawned run-%s child pid=%lu (inherit stdio)\n",
              label.c_str(), static_cast<unsigned long>(pi.dwProcessId));
        // Wait for termination BEFORE reading the exit code
        // (GetExitCodeProcess on a live process returns STILL_ACTIVE).
        if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0) {
          error = "WaitForSingleObject(process) failed";
        } else {
          DWORD raw = 0;
          GetExitCodeProcess(pi.hProcess, &raw);
          result = static_cast<int>(raw);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
      }
    }
  }
#else
  {
    if (::access(exe.c_str(), X_OK) != 0) {
      error = "scenario executable not found: " + exe;
    } else {
      const pid_t pid = fork();
      if (pid < 0) {
        error = "fork() failed";
      } else if (pid == 0) {
        // Child: NO stdout redirect - it inherits this process's stdout
        // (the harness's capture), so its ticks land between the parent's
        // markers.
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& a : args)
          argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);  // execv failed: the path is wrong or not executable
      } else {
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
          error = "waitpid() failed";
        } else if (WIFEXITED(status)) {
          result = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          // Conventional "exit code" for a signalled child; main() reports
          // it as a scenario process failure (exit 2), same as the old
          // single-shot path.
          result = 128 + WTERMSIG(status);
        }
      }
    }
  }
#endif
  // The END marker carries the child's exit code (or -1 when the spawn
  // itself failed, in which case the check script stops at phase 1; the
  // marker keeps the captured stream parseable either way).
  std::printf("@@DETCHK-RUN-%s-END %d@@\n", label.c_str(), result);
  std::fflush(stdout);
  return result;
}

// Read a whole file, bounded to maxBytes (bounded work, CORE-003).
bool readFileBounded(const std::string& path, std::string& out,
                     std::size_t maxBytes) {
  std::FILE* f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path.c_str(), "rb") != 0) return false;
#else
  f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
#endif
  out.clear();
  char buf[65536];
  for (;;) {
    const std::size_t n = std::fread(buf, 1, sizeof buf, f);
    if (n == 0) break;
    if (out.size() + n > maxBytes) {
      std::fclose(f);
      return false;
    }
    out.append(buf, n);
  }
  std::fclose(f);
  return true;
}

// Extract one run's tick lines from the combined stream: the lines
// strictly between `@@DETCHK-RUN-<L>-BEGIN@@` and the
// `@@DETCHK-RUN-<L>-END <code>@@` line. Returns false when either marker
// is missing (a truncated capture is a contract failure, CORE-008).
bool extractRunStream(const std::vector<std::string>& lines,
                      const std::string& label,
                      std::vector<std::string>& out) {
  const std::string begin = "@@DETCHK-RUN-" + label + "-BEGIN@@";
  const std::string endPrefix = "@@DETCHK-RUN-" + label + "-END ";
  bool inStream = false;
  for (const std::string& ln : lines) {
    if (ln == begin) {
      inStream = true;
      continue;
    }
    if (inStream) {
      if (ln.rfind(endPrefix, 0) == 0) return true;
      out.push_back(ln);
    }
  }
  return false;
}

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

void report(std::string_view scenarioName, const CompareResult& c,
            std::size_t ticks, std::string_view labelA,
            std::string_view labelB) {
  auto emit = [](const char* line) {
    std::printf("%s\n", line);
    g_diag.append(line);
    g_diag += "\n";
  };
  if (c.ok) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "detcheck scenario=%s result=OK ticks=%llu",
                  std::string(scenarioName).c_str(),
                  static_cast<unsigned long long>(ticks));
    emit(buf);
    std::snprintf(buf, sizeof buf, "  run-a: %s",
                  std::string(labelA).c_str());
    emit(buf);
    std::snprintf(buf, sizeof buf, "  run-b: %s",
                  std::string(labelB).c_str());
    emit(buf);
  } else {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "detcheck scenario=%s result=DIVERGED "
                  "first_diff_tick=%llu",
                  std::string(scenarioName).c_str(),
                  static_cast<unsigned long long>(c.firstDiffTick));
    emit(buf);
    std::snprintf(buf, sizeof buf, "  run-a: %s", c.lineA.c_str());
    emit(buf);
    std::snprintf(buf, sizeof buf, "  run-b: %s", c.lineB.c_str());
    emit(buf);
  }
}

// --- Argument parsing ------------------------------------------------------

struct Args {
  std::string scenario;  // mode 1
  std::string runA;      // mode 2
  std::string runB;      // mode 2
  bool cmpCombined = false;  // mode 3
  std::string cmpCombinedFile;  // mode 3 combined stream file
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
      "       laige-detcheck --compare-combined=<combined-stream-file>\n"
      "       laige-detcheck --help\n"
      "\n"
      "  --scenario          built-in scenario: synthetic | "
      "synthetic-perturbed\n"
      "  --run-a/--run-b     two builds of the same scenario (two build\n"
      "                      configurations). Two-stage capture: this\n"
      "                      invocation SPAWNS both binaries (each child's\n"
      "                      stdout is inherited from this process, so its\n"
      "                      ticks flow to this process's stdout, delimited\n"
      "                      by @@DETCHK-RUN-A/B-BEGIN/END@@ markers) and\n"
      "                      does NOT compare; the caller hands the\n"
      "                      combined stream to --compare-combined.\n"
      "  --compare-combined  read a combined stream (markers + both tick\n"
      "                      streams), validate both runs against the\n"
      "                      stream contract, compare, and report (phase 2\n"
      "                      of --run-a/--run-b)\n"
      "  --ticks             built-in scenario tick count (1..%d, "
      "default %d)\n"
      "  --seed              built-in scenario seed (0xHEX or decimal)\n"
      "  --                  end of tool options; the remaining args\n"
      "                      are passed to both scenario binaries\n"
      "\n"
      "Exit codes: 0 = match, 1 = divergence, 2 = error (usage, unknown\n"
      "scenario, spawn failure, scenario failure, contract violation).\n"
      "--run-a/--run-b exits 0 when both scenario processes ran to\n"
      "completion and 2 on any spawn or scenario failure (the comparison\n"
      "happens in the --compare-combined phase).\n",
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
    } else if (key == "--compare-combined") {
      if (!hasValue) return false;
      a.cmpCombined = true;
      a.cmpCombinedFile = value;
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
  // Flush the accumulated diagnostics to $LAIGE_DETCHECK_DIAG_FILE on
  // every exit path (ctest hides passing-test output; the file is how
  // those diagnostics reach the CI log).
  DiagFlush diagFlush;
  // Diagnostic begin marker (identifies the run and its writer in the
  // flushed file: a diag file can be overwritten by a later detcheck
  // invocation with the same LAIGE_DETCHECK_DIAG_FILE).
  {
    std::string cmdLine;
    for (int i = 0; i < argc; ++i) {
      if (i > 0) cmdLine += " ";
      cmdLine += argv[i];
    }
#ifdef _WIN32
    diagf("laige-detcheck: diag begin pid=%lu %s argv=%s\n",
          static_cast<unsigned long>(GetCurrentProcessId()),
          parentProcessInfo().c_str(), cmdLine.c_str());
#else
    diagf("laige-detcheck: diag begin pid=%d %s argv=%s\n",
          static_cast<int>(::getpid()), parentProcessInfo().c_str(),
          cmdLine.c_str());
#endif
  }
  Args a;
  if (!parseArgs(argc, argv, a)) {
    printUsage(stderr);
    return 2;
  }
  const bool mode1 = !a.scenario.empty();
  const bool mode2 = !a.runA.empty() || !a.runB.empty();
  const bool mode3 = a.cmpCombined;
  const int modeCount = (mode1 ? 1 : 0) + (mode2 ? 1 : 0) + (mode3 ? 1 : 0);
  if (modeCount != 1) {
    std::fprintf(stderr,
                 "laige-detcheck: exactly one of --scenario, "
                 "--run-a/--run-b, or --compare-combined is required\n");
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

  // --- Mode 3: compare a combined stream (phase 2) ------------------------
  if (mode3) {
    // The combined stream holds, per run: a BEGIN marker line, the tick
    // lines, and an END marker line. Its size is bounded by the contract
    // (two runs of kMaxTicks lines of kMaxLineBytes, plus the markers);
    // reading more is a contract failure, not an allocation (CORE-003).
    const std::size_t maxBytes =
        (2 * static_cast<std::size_t>(kMaxTicks) + 8) * (kMaxLineBytes + 1);
    std::string content;
    if (!readFileBounded(a.cmpCombinedFile, content, maxBytes)) {
      std::fprintf(stderr,
                   "laige-detcheck: cannot read combined stream file: %s\n",
                   a.cmpCombinedFile.c_str());
      return 2;
    }
    std::vector<std::string> lines;
    std::string pending;
    RunResult bounded;  // carries the unbounded-output error if any
    const bool okA = appendChunk(lines, pending, content.data(),
                                 content.size(), 2 * kMaxTicks + 8);
    if (okA) finishPending(lines, pending, bounded);
    if (!okA || !bounded.error.empty()) {
      std::fprintf(stderr,
                   "laige-detcheck: combined stream is unbounded (line or "
                   "tick count exceeds the contract)\n");
      return 2;
    }
    std::vector<std::string> linesA, linesB;
    if (!extractRunStream(lines, "A", linesA)) {
      std::fprintf(stderr,
                   "laige-detcheck: combined stream is missing the "
                   "run-A markers (@@DETCHK-RUN-A-BEGIN/END@@)\n");
      return 2;
    }
    if (!extractRunStream(lines, "B", linesB)) {
      std::fprintf(stderr,
                   "laige-detcheck: combined stream is missing the "
                   "run-B markers (@@DETCHK-RUN-B-BEGIN/END@@)\n");
      return 2;
    }
    const std::string errA = validateStream(linesA, kMaxTicks);
    if (!errA.empty()) {
      diagf("laige-detcheck: scenario run-a: %s\n", errA.c_str());
      return 2;
    }
    const std::string errB = validateStream(linesB, kMaxTicks);
    if (!errB.empty()) {
      diagf("laige-detcheck: scenario run-b: %s\n", errB.c_str());
      return 2;
    }
    const CompareResult c = compareStreams(linesA, linesB);
    report("combined", c, linesA.size(), "run-a", "run-b");
    return c.ok ? 0 : 1;
  }

  // --- Mode 2: two scenario binaries (phase 1 of two-stage capture) ------
  // Each scenario child inherits THIS process's stdout (no capture pipe -
  // see spawnScenarioWithMarkers), so its ticks land in the harness's
  // capture of this process's stdout, between the BEGIN/END markers. The
  // comparison happens in phase 2 (--compare-combined), invoked by the
  // check script over the combined stream.
  if (mode2) {
    std::string errA;
    const int codeA =
        spawnScenarioWithMarkers(a.runA, a.positionals, "A", errA);
    if (codeA < 0) {
      diagf("laige-detcheck: scenario run-a: %s\n", errA.c_str());
      return 2;
    }
    if (codeA != 0) {
      diagf("laige-detcheck: scenario run-a: scenario process exited with "
            "code %d (command: %s)\n",
            codeA, a.runA.c_str());
      return 2;
    }
    std::string errB;
    const int codeB =
        spawnScenarioWithMarkers(a.runB, a.positionals, "B", errB);
    if (codeB < 0) {
      diagf("laige-detcheck: scenario run-b: %s\n", errB.c_str());
      return 2;
    }
    if (codeB != 0) {
      diagf("laige-detcheck: scenario run-b: scenario process exited with "
            "code %d (command: %s)\n",
            codeB, a.runB.c_str());
      return 2;
    }
    return 0;
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
