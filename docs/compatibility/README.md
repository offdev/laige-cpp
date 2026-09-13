# Compatibility

Platforms, compilers, formats, and migration guides (AGENTS §13).

## P0 platforms and compilers (PRD §6, NFR-8.8 / NFR-8.10)

| P0 platform | Toolchain | CI jobs |
|---|---|---|
| Linux x64 (arm64 planned) | GCC / Clang | `linux-gcc`, `linux-clang` |
| Windows x64 | MSVC 2022 (clang-cl secondary) | `windows-msvc` |
| macOS arm64 / Intel | AppleClang | `macos-arm64`, `macos-intel` |

- **Language:** C++20 (NFR-8.10). Engine targets compile with
  `-Wall -Werror` and with exceptions/RTTI disabled (the documented MSVC
  equivalent).
- **Build system:** CMake ≥ 3.22 (NFR-8.8), single configure, no network
  access; all dependencies vendored in-tree and integrity-locked in
  `deps.lock`.
- **Library flavors:** static (default) and shared (`LAIGE_BUILD_SHARED`,
  NFR-8.9) both supported.
- **Sanitizers:** ASan+UBSan and TSan build trees on Linux
  (NFR-8.2); see [building.md](../getting-started/building.md) — the
  source of truth for the canonical commands and the CI matrix
  (`.github/workflows/ci.yml` / `ci-pull.yml`).

## Formats

No persistent, networked, or replay data formats exist yet (M0 is
foundations only; they arrive with M1 replay and M6/M7 networking). The
machine-readable files that do exist, each with its schema documented:

| File | Schema | Document |
|---|---|---|
| `budgets.json` | version 1, strict validation (ARCH-007) | [api/budget_harness.md](../api/budget_harness.md) ("budgets.json schema") |
| `laige-api.json` | manifest version 1, deterministic (byte-identical regeneration is the CI drift check) | the `tools/api/laige-api.cpp` header (M0-TOOL-01) |
| `deps.lock` | one entry per vendored dependency, integrity-checked at configure time | [ADR 0004](../decisions/0004-google-test-vendoring.md) |

When a persistent format lands it MUST ship versioned, with a reader that
rejects or migrates unsupported data explicitly (ARCH-007) and a format
document added to this section in the same change (DOC-007).

## Migration guides

None yet: the engine is pre-1.0 and M0 has introduced no breaking public
changes. The first migration guide lands with the first breaking
public-API change; public API/ABI versioning is governed by the manifest
(NFR-13.1) and API-007.
