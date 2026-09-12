// laige-detcheck-probe (Windows only) — diagnostic child for
// laige-detcheck (M0-TEST-01 Windows CI investigation).
//
// laige-detcheck spawns this with its stdout and stderr each connected
// to a capture pipe (or with no redirection at all, as the control
// attempt). The helper reports the standard handles the kernel actually
// assigned to it — value, type, flags — and writes a marker line to
// stdout. The parent compares that report against the handles it asked
// for in its STARTUPINFO, which shows exactly what CreateProcessW
// delivered in the working versus the broken launch instances.
//
// fd1 types: 1 = character device (console/NUL), 2 = disk file,
// 3 = pipe, 0 = unknown (e.g. INVALID_HANDLE_VALUE).

#include <windows.h>

#include <cstdio>

static void report(const char* name, HANDLE h) {
  DWORD flags = 0;
  GetHandleInformation(h, &flags);
  const unsigned long long hv = reinterpret_cast<unsigned long long>(h);
  std::fprintf(stderr, " %s=0x%llx type=%lu flags=0x%lx", name, hv,
               GetFileType(h), flags);
}

int main() {
  std::fprintf(stderr, "probe-helper:");
  report("fd0", GetStdHandle(STD_INPUT_HANDLE));
  report("fd1", GetStdHandle(STD_OUTPUT_HANDLE));
  report("fd2", GetStdHandle(STD_ERROR_HANDLE));
  std::fprintf(stderr, "\n");
  std::printf("PROBE_HELPER_OK\n");
  return 0;
}
