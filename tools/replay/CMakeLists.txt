# laige-replay (M1-DET-03): the headless replay runner.
#
# Canonical command (docs/getting-started/building.md):
#
#   ./build/bin/laige-replay --log <log> --config <config.json>
#                            [--expect <baseline>]
#
# Loads a recorded replay log (M1-DET-02 format), checks its replay
# identity against (the engine's world, the config) — a mismatch is a
# rejected replay (ADR 0002) — replays the log headlessly (one tick
# per frame, the deterministic runReplay driver), and prints the
# per-tick state hashes (World::stateHash) as the hash line contract
# (tick 0 first; the same contract laige-detcheck enforces on
# scenario binaries). With --expect it also compares against a
# baseline hash stream and exits 1 on the first divergence. Exit
# codes 0/1/2 are documented in the source header and
# docs/api/replay.md.
#
# Gated with the test suite like the other tools: a library-only
# build does not need it. The tool's CTest suite (record → replay →
# --expect, the perturbed-baseline divergence, the identity-mismatch
# rejection) lives in tests/replay.
#
# Headless invariants (ARCH-003, NFR-8.11): the include-graph lint
# guarantees this target's include closure (laige-sim + laige-core)
# touches no GL/window/audio/input API.

add_executable(laige-replay laige-replay.cpp)
laige_apply_engine_policy(laige-replay)
# laige-sim is the only internal dependency (PRD §10.1: arrows only
# downward); laige-core's public headers come through it (CPP-010).
target_link_libraries(laige-replay PRIVATE laige-sim)
