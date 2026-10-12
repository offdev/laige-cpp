# scene_iso_determinism.cmake — the M2-SCENE-01 CLI determinism gate
# (roadmap Verify: "two same-seed invocations produce byte-identical
# output, tested").
#
# Usage:
#   cmake -D LAIGE_SCENE_ISO=<exe> -D WORKDIR=<dir> -P scene_iso_determinism.cmake
#
# Runs laige-scene-iso TWICE per variant (the same seed — 424242, an
# arbitrary fixed value), then compares the two output files byte for
# byte (file READ ... HEX + string compare — no shell, cross-platform).
# Any failure is a FATAL_ERROR (ctest red). The two variants cover the
# 50k worst-case scene and the 1k-sprite unit-test fixture; a third
# property (different seed => different bytes) is asserted in-process
# by the gtest suite (IsoReferenceSceneBuilder).

if(NOT DEFINED LAIGE_SCENE_ISO OR NOT DEFINED WORKDIR)
  message(FATAL_ERROR
    "scene_iso_determinism.cmake: missing LAIGE_SCENE_ISO and/or WORKDIR")
endif()

file(MAKE_DIRECTORY ${WORKDIR})

set(kSeed 424242)
foreach(VARIANT full small)
  set(A ${WORKDIR}/${VARIANT}-a.bin)
  set(B ${WORKDIR}/${VARIANT}-b.bin)
  execute_process(COMMAND ${LAIGE_SCENE_ISO}
                          --seed ${kSeed} --variant ${VARIANT} --out ${A}
                  RESULT_VARIABLE RA)
  execute_process(COMMAND ${LAIGE_SCENE_ISO}
                          --seed ${kSeed} --variant ${VARIANT} --out ${B}
                  RESULT_VARIABLE RB)
  if(NOT RA STREQUAL 0 OR NOT RB STREQUAL 0)
    message(FATAL_ERROR
      "laige-scene-iso failed for variant=${VARIANT} (exit codes ${RA} / ${RB})")
  endif()
  file(READ ${A} HA HEX)
  file(READ ${B} HB HEX)
  if(NOT HA STREQUAL HB)
    message(FATAL_ERROR
      "same-seed outputs differ for variant=${VARIANT} (seed ${kSeed}) — "
      "ARCH-010 determinism broken")
  endif()
  string(LENGTH HL ${HA})
  message(STATUS
    "scene-iso determinism: variant=${VARIANT} seed=${kSeed} OK "
    "(${HL} hex chars identical)")
endforeach()
