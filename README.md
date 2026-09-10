# Laige

**Laige** (*Legendary AI Game Engine*) is a C++20 2.5D game engine for
building everything from small single-player games to large, long-running
MMOs: 2D simulation with 3D presentation, deterministic by default,
isometric-first rendering, and a server-authoritative MMO path.

- **License:** MIT (engine code). Samples and assets are separately
  licensed; each sample ships its own `LICENSE`
  ([ADR 0001](docs/decisions/0001-name-and-license.md)).
- **Status:** **M0 — Foundations**, in progress. The repository skeleton,
  build system, `laige-core` library target, and dependency lock
  (`deps.lock` with vendored GoogleTest) have landed; the functional core
  (math, pools, Result, logging, config) lands over the remaining M0 steps
  in [roadmap/M0-foundations.md](roadmap/M0-foundations.md). No engine
  features are buildable yet.

## Built by a local LLM

This project is being completely built by a local LLM. The engine is
designed and implemented by
**[Qwen3.8-27B-GSQ-RCO](https://huggingface.co/ISTA-DASLab/Qwen3.8-27B-GSQ-RCO-GGUF)**
(IQ3_S quant), served by the most recent builds of
[llama.cpp](https://llama.app/) and driven by the
[DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness/).
No cloud or hosted model API is involved.

A human supervises the project and every milestone step: the model drafts
and writes the work, and a human reviews and accepts each step before it
lands.

## System specs

The AI-relevant hardware the model runs on:

| Component | Spec |
|---|---|
| CPU | AMD Ryzen 9 7950X3D — 16 cores / 32 threads, up to 5.7 GHz, 3D V-Cache |
| Memory | 64 GB |
| GPU | NVIDIA GeForce RTX 4090 (24 GB) |
| OS | CachyOS (Arch-based), Linux 7.2.2, x86_64 |

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

Canonical commands (the full table — including sanitizer, fuzz, and
benchmark forms — is the source of truth in
[docs/getting-started/building.md](docs/getting-started/building.md)):

| Purpose | Command |
|---|---|
| Configure | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` |
| Build | `cmake --build build -j` |
| Test | `ctest --test-dir build --output-on-failure` |
| ASan/UBSan build | `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_ASAN=ON` |
| TSan build | `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLAIGE_TSAN=ON` |

The `laige-core` library builds now: static by default, shared with
`-DLAIGE_BUILD_SHARED=ON` (NFR-8.9), verified by a CTest link smoke test in
both variants. It carries no functional engine code yet — the first code
(`laige::Result<T,E>` / `laige::Status`) lands in M0-CORE-01. Engine targets
compile with `-Wall -Werror` and with exceptions and RTTI disabled
(NFR-8.10).

## Documentation

- [PRD](PRD.md) — product requirements
- [AGENTS.md](AGENTS.md) — engineering contract (normative)
- [Roadmap](roadmap/README.md) — implementation checklist, progress board,
  canonical commands
- [Architecture decisions](docs/decisions/README.md) — ADRs 0001–0003
