# hello.laige (M1-SAMPLE-01): the headless template game.
#
# The binaries land at the SOURCE-TREE paths samples/hello/bin/hello
# and samples/hello/bin/hello-fp32 (RUNTIME_OUTPUT_DIRECTORY override):
# the CI detcheck job (.github/workflows/ci.yml, "Determinism check")
# invokes them from the repository root with no arguments — the
# canonical config is embedded in the source, so no CWD-relative file
# access. The build-tree convention (<tree>/bin/) is unchanged for
# every other target.
#
# Two build variants of the SAME game source (M1-DET-04; ADR 0002:
# one op surface, two backends, the game code written once):
#
#   hello      fixed_point_16_16 (the canonical backend — the default;
#              the config.json backend)
#   hello-fp32 float_pinned_32 (hello-fp32.cpp #defines the two
#              LAIGE_HELLO_BACKEND macros and includes hello.cpp)
#
# hello-baseline.cpp is the --expect baseline check (the M1-DET-04
# determinism-matrix machinery; kept out of hello.cpp so the template
# stays inside the PRD §9.4 line budget).
add_executable(hello hello.cpp hello-baseline.cpp)
laige_apply_engine_policy(hello)
laige_apply_simmath_policy(hello)
# laige-sim is the only internal dependency (PRD §10.1: arrows only
# downward); laige-core's public headers come through it (CPP-010).
target_link_libraries(hello PRIVATE laige-sim)
set_target_properties(hello PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

add_executable(hello-fp32 hello-fp32.cpp hello-baseline.cpp)
laige_apply_engine_policy(hello-fp32)
laige_apply_simmath_policy(hello-fp32)
target_link_libraries(hello-fp32 PRIVATE laige-sim)
set_target_properties(hello-fp32 PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
