# laige-api-scanner (M0-TOOL-01): the public API manifest generator.
#
# Scans the public headers (src/<module>/include for the PRD §10.1
# modules) and emits the deterministic machine-readable manifest
# laige-api.json. The full manifest contract — symbol kinds, doc
# association, unsupported constructs, exit codes — is in the header
# comment of laige-api.cpp.
#
# Gated with the test suite like tools/fuzz and tools/bench: a
# library-only build does not need it.

add_executable(laige-api-scanner laige-api.cpp)
laige_apply_engine_policy(laige-api-scanner)
target_link_libraries(laige-api-scanner PRIVATE laige-core)

# Canonical regeneration target (PRD §9.4, NFR-13.1):
#
#   cmake --build build --target laige-api
#
# rewrites the checked-in ${CMAKE_SOURCE_DIR}/laige-api.json from the
# current public headers. CI regenerates it in a temporary workdir and
# fails on any drift against the checked-in copy (the checked-in file is
# never rewritten by CI — it is a reviewable diff).
add_custom_target(laige-api
  COMMAND $<TARGET_FILE:laige-api-scanner>
          --root ${CMAKE_SOURCE_DIR}
          --out ${CMAKE_SOURCE_DIR}/laige-api.json
  COMMENT "Regenerating laige-api.json (public API manifest)"
  VERBATIM)
add_dependencies(laige-api laige-api-scanner)
