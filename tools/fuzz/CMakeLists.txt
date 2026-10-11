# laige-fuzz — deterministic bounded fuzz runner (M0-CORE-07, minimal
# form; M0-TEST-01 extends it: CI lane semantics, nightly long runs,
# seed-handling documentation).
#
# Gated with LAIGE_BUILD_TESTS (root CMakeLists): a dev tool that links
# the engine library — a library-only build does not need it.

add_executable(laige-fuzz laige-fuzz.cpp)
laige_apply_engine_policy(laige-fuzz)
# M1-DET-02: the replay_parse target exercises laige-sim's replay log
# parser — laige-sim joins the link (arrows only downward, PRD
# §10.1); laige-core's public headers come through it (CPP-010).
# M2-SCENE-01: the scene_iso_parse target exercises laige-render's
# reference-scene file reader — laige-render joins the link (laige-sim
# comes through it, the M2-PART-02 public edge).
target_link_libraries(laige-fuzz PRIVATE laige-sim laige-render)

# Bounded fuzz in every commit (PRD §14: "every commit (bounded), nightly
# (long)"): 1000 deterministic inputs to the json_parse target (NFR-8.7,
# ADR 0003). In an ASan tree this entry is the M0-CORE-07 Verify gate —
# the run is instrumented, so any crash/UB fails ctest loudly.
add_test(NAME fuzz_json_parse COMMAND laige-fuzz json_parse --runs=1000)
set_tests_properties(fuzz_json_parse PROPERTIES TIMEOUT 120)
# M1-DET-02: the replay log parser's malformed-input surface (SCALE-005
# / ARCH-007) gets the same bounded-every-commit gate — 1000
# deterministic inputs; the corpus includes a valid v1 replay log as
# a mutate/truncate base.
add_test(NAME fuzz_replay_parse COMMAND laige-fuzz replay_parse --runs=1000)
set_tests_properties(fuzz_replay_parse PROPERTIES TIMEOUT 120)
# M2-SCENE-01: the reference-scene file reader's malformed-input
# surface (SCALE-005 / ARCH-007 — the versioned binary format,
# laige/render/iso_reference_scene.h) gets the same bounded-every-
# commit gate; the corpus includes valid v1 small + full scenes as
# mutate/truncate bases (the full scene's ~0.94 MB keeps the mutate
# positions meaningful against the 50k-sprite record array).
add_test(NAME fuzz_scene_iso_parse COMMAND laige-fuzz scene_iso_parse --runs=1000)
set_tests_properties(fuzz_scene_iso_parse PROPERTIES TIMEOUT 120)
if(LAIGE_TSAN)
  # Same fatal-race policy as the module test entries (NFR-8.2).
  set_tests_properties(fuzz_json_parse PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
  set_tests_properties(fuzz_replay_parse PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
  set_tests_properties(fuzz_scene_iso_parse PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
