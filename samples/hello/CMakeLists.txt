# hello.laige (M1-SAMPLE-01): the headless template game.
#
# The binary lands at the SOURCE-TREE path samples/hello/bin/hello
# (RUNTIME_OUTPUT_DIRECTORY override): the CI detcheck step
# (.github/workflows/ci.yml, "Real scenario (M1-SAMPLE-01)") invokes
# it from the repository root with no arguments — the canonical
# config is embedded in the source, so no CWD-relative file access.
# The build-tree convention (<tree>/bin/) is unchanged for every
# other target.
add_executable(hello hello.cpp)
laige_apply_engine_policy(hello)
laige_apply_simmath_policy(hello)
# laige-sim is the only internal dependency (PRD §10.1: arrows only
# downward); laige-core's public headers come through it (CPP-010).
target_link_libraries(hello PRIVATE laige-sim)
set_target_properties(hello PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
