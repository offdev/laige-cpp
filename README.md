# Laige

**Laige** (*Legendary AI Game Engine*) is a C++20 2.5D game engine for
building everything from small single-player games to large, long-running
MMOs: 2D simulation with 3D presentation, deterministic by default,
isometric-first rendering, and a server-authoritative MMO path.

- **License:** MIT (engine code). Samples and assets are separately
  licensed; each sample ships its own `LICENSE`
  ([ADR 0001](docs/decisions/0001-name-and-license.md)).
- **Status:** **M0 — Foundations**, in progress. The repository skeleton,
  build system, and `laige-core` land over the M0 steps in
  [roadmap/M0-foundations.md](roadmap/M0-foundations.md). No engine features
  are buildable yet.

## Repository layout

| Path | Purpose |
|---|---|
| `src/laige-core` | Engine foundation: determinism, math, pools, alloc, config, logging, `Result` (PRD §10.1) |
| `src/laige-sim`, `src/laige-render`, … | One directory per engine module (PRD §10.1 module map); populated as milestones land |
| `deps/` | Vendored dependencies, tracked by `deps.lock` (PRD §11) — lands in M0-DEP-01 |
| `third_party/` | Reserved placeholder for vendored code outside `deps.lock` |
| `tests/` | Unit/integration tests, mirroring the `src/` module layout |
| `tools/` | Engine tools and CI scripts (fuzz runner, API manifest, lints) |
| `samples/` | Reference game projects (flagship isometric ARPG, platformer, lockstep arena, MMO demo zone) |
| `docs/` | Documentation; [decision index](docs/decisions/README.md) (full structure lands in M0-DOC-01) |

## Building

Requirements (PRD §6, §8.3): CMake ≥ 3.22 and a C++20 compiler (GCC,
Clang, MSVC 2022). No network access is needed to build (all dependencies
are vendored, NFR-8.8).

Canonical commands (the source of truth lands in
`docs/getting-started/building.md` at M0-BUILD-01):

| Purpose | Command |
|---|---|
| Configure | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` |
| Build | `cmake --build build -j` |
| Test | `ctest --test-dir build --output-on-failure` |

Until M0-BUILD-01 the project configures and builds an empty skeleton.
Engine targets compile with `-Wall -Werror` and with exceptions and RTTI
disabled (NFR-8.10); `LAIGE_BUILD_SHARED=ON` switches engine libraries from
static (default) to shared builds (NFR-8.9).

## Documentation

- [PRD](PRD.md) — product requirements
- [AGENTS.md](AGENTS.md) — engineering contract (normative)
- [Roadmap](roadmap/README.md) — implementation checklist, progress board,
  canonical commands
- [Architecture decisions](docs/decisions/README.md) — ADRs 0001–0003
