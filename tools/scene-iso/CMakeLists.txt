# laige-scene-iso (M2-SCENE-01): the reference-isometric-scene
# generator.
#
# Gated with the test suite like tools/bench and tools/fuzz: a
# library-only build does not need it. The tool builds the worst-case
# isometric reference scene (50k sprites / small 1k variant) from a
# seed, serializes it (the versioned binary format,
# laige/render/iso_reference_scene.h), and writes it — headless, no
# GL (ARCH-003: the engine objects the scene builds are the loader's
# job, exercised by the tests).

add_executable(laige-scene-iso laige-scene-iso.cpp)
laige_apply_engine_policy(laige-scene-iso)
target_link_libraries(laige-scene-iso PRIVATE laige-render)

# The M2-SCENE-01 Verify clause in executable form (roadmap: "two
# same-seed invocations produce byte-identical output, tested"): the
# -P script runs laige-scene-iso twice per variant (the same seed)
# and compares the two output files byte for byte. CMake-level file
# compare (no shell — cross-platform, the P0 Windows/macOS/Linux jobs).
# The 60 s timeout bounds the step's "< 5 s" generation budget with
# wide margin (the absolute number is machine-dependent; the summary
# line records the measured time — CORE-001).
add_test(NAME scene_iso_cli
         COMMAND ${CMAKE_COMMAND}
                 -D LAIGE_SCENE_ISO=$<TARGET_FILE:laige-scene-iso>
                 -D WORKDIR=${CMAKE_BINARY_DIR}/scene-iso-check
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/scene_iso_determinism.cmake)
set_tests_properties(scene_iso_cli PROPERTIES TIMEOUT 60)

if(LAIGE_TSAN)
  # Same first-report-fatal policy as the other test entries (NFR-8.2).
  set_tests_properties(scene_iso_cli
    PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
