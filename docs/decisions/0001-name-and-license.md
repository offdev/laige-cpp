# ADR 0001 — Engine name and license

- **Status:** Accepted
- **Date:** 2026-09-10
- **Decider:** Project owner (roadmap step M0-DEC-01)
- **Refs:** PRD §18.1, §10.1, §13.5 (NFR-13.5)

## Context

PRD §18.1 leaves the engine name tentative ("Laige", an acronym for *Legendary
AI Game Engine*) and proposes MIT for the engine with assets/samples separately
licensed. The name is already embedded in
the documents: module names (`laige-core`, …, PRD §10.1), binary names
(`laige-server`, `laige-fuzz`, `laige-detcheck`), the project format
(`.laige`), and template names (`hello.laige`, NFR-13.5). No code exists yet, so
the rename cost is minimal now and grows monotonically with every M0–M1 step
(PRD revision, roadmap edits, user-facing files).

A name-collision check was performed on 2026-09-10 (GitHub search + general
web): no existing game engine or C++ project named "Laige" was found in the
software domain. ("laige" is also a French word meaning stone/brick surface —
no conflict there either.)

The license binds every future game project built on the engine, so it must
fit the commercial single-player and MMO use cases (PRD §3).

## Decision

- **Name: Laige** — *Legendary AI Game Engine* — (confirmed). Namespace
  `laige::`, module/binary prefix `laige-`, project extension `.laige`.
- **License: MIT** for all engine code. Samples, assets, and reference games
  carry their own per-project licenses (each ships its own `LICENSE`); the
  engine license imposes no copyleft on game projects.

## Alternatives considered

- **Rename** — cheapest right now (zero code), but requires a document-wide
  rename pass (PRD, roadmap, binary/format names) before M0-REPO-01. No known
  conflict motivated it.
- **Apache-2.0** — same permissiveness plus an explicit patent grant and a
  NOTICE file; not required by the current user base. MIT → Apache-2.0 is a
  one-way, cheap upgrade path if IP risk materializes.
- **GPL / AGPL** — copyleft would force game projects (and, under AGPL, their
  servers) to open-source; incompatible with the commercial targets (PRD §3).

## Evidence

- Name-collision search, 2026-09-10: no engine/brand named "Laige" found in
  the software domain.
- License precedent: Godot (MIT), Bevy (MIT), raylib (Zlib).

## Consequences

- PRD §18.1 resolved exactly as proposed — no PRD revision required.
- M0-REPO-01 creates the repo under the name "Laige" with an MIT `LICENSE`.
- Every sample game in `samples/` ships its own `LICENSE`.

## Review conditions

- Revisit immediately if a trademark conflict surfaces.
- A license upgrade (MIT → Apache-2.0) may be executed via one PRD revision
  at any point before the 1.0 tag.
