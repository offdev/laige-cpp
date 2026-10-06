# laige-render tests: the GL context contract (M2-GL-01) — the pure
# 3.3 core gate (GlContextGate), the creation argument validation and
# the stopped-context state (GlContextArgs, no GL required), and the
# offscreen render smoke (GlContextSmoke: the headless offscreen FBO
# frame round trip, the move semantics, and the windowed path's clean
# failure on a headless host); and the frame pipeline (M2-GL-02) — the
# vsync-paced frame clock (FrameClock, no GL), the single-slot lock-free
# handoff (RenderThreadHandoff, no GL), and the offscreen pipeline runs
# (RenderThreadOffscreen: the 3000-frame no-deadlock integration run and
# the slowed-submit drop path); and the matrix utilities (M2-GL-03) —
# the camera/projection builders pinned against hand-computed golden
# values (Matrices*, pure math: no GL environment needed); and the
# isometric depth key (M2-ISO-01) — the engine-owned 32-bit sortable
# depth key pinned against hand-computed golden keys for a small
# stepped-terrain scene (IsoDepthKeyGolden), the 10k random scene's
# back-to-front property against the iso projection matrices
# (IsoDepthKeyProperty), the (key, entity id) total order and
# determinism (IsoDepthKeyTieBreak), the domain/saturation contract
# (IsoDepthKeyDomain), and the supported-shear checker
# (IsoDepthKeyShears) — pure math: no GL environment needed; and the
# per-scene-chunk depth key table (M2-ISO-02) — the precomputed,
# incrementally-updated tile-grid -> depth-key map (IsoDepthTable*:
# option validation + the flat-ground contract, the hand-computed
# golden scene, the single-tile-edit / rejected-edit / zero-allocation
# update contract, the bounded + logged chunk growth, the rebuild ==
# incremental property, and the PRD §8.1 10k-dirty-cell budget gate)
# — pure data: no GL environment needed; and the 3D camera core
# (M2-CAM-01) — the presentation-side Camera (camera_tests.cpp's
# Camera* suites: the create validation and initial clamp, the zoom
# clamp exact at the bounds, the rectangular bounds under adversarial
# follow/shake input, the smooth follow's deterministic exact sequence,
# the shake's bounded decay to exactly zero within the documented
# tick count, and the matrix builds against the M2-GL-03 builders) —
# pure value math: no GL environment needed; and the projection modes
# + screen<->world transforms (M2-PROJ-01) — the ProjectionView
# (projection_tests.cpp's Projection* suites: the mode/matrix defaults,
# the hand-computed world->NDC goldens for every mode (2:1 dimetric,
# true 30°/60°, a custom invertible shear, the side_view/top_down plane
# cameras, the ortho and perspective cinematic cameras), the per-mode
# preimage-ray geometry (the ground-plane origin + the (1,1,2)-ish
# iso direction, the plane-mode reference-plane origins, the cinematic
# near-plane origin + away-from-camera direction), the 10k-point
# round trips against the documented precision per mode, and the
# documented Result failure paths (parallel plane, zero normal,
# intersection behind the camera)) — pure value math: no GL
# environment needed; and the isometric grid picking (M2-ISO-03) —
# the engine-owned safe screen -> ground -> grid-cell transform
# (iso_picking_tests.cpp's IsoPick* suites: the hand-computed golden
# cells at 4 zoom levels for the 2:1 dimetric and true 30/60 presets,
# the 10k-cell x 4-zoom center round-trip property (plus the
# zero-allocation proof), the documented half-open boundary rule and
# boundary zone, the supported-shear round trip, the total-function
# saturation contract, the stopped-camera degenerate pick, the
# ProjectionView overload parity, and the PRD §8.1 one-pick budget
# gate) — pure float math: no GL environment needed; and the
# deterministic depth sort (M2-SORT-01) — the stable, deterministic,
# pre-allocated 32-bit-key radix sort (depth_sort_tests.cpp's
# DepthSort* suites: the hand-computed sorted order + capacity edge
# behavior, the 0/1/all-equal/monotone edge cases, the determinism
# property on 64 shuffled inputs against the stable-sort oracle, the
# 10k-key oracle comparison + the zero-allocation proof, and the PRD
# §8.1 `depth_sort_10k` budget gate — 10 000 keys sorted per frame,
# 3 000 frames, mean <= 1.0 ms) — pure integer math: no GL
# environment needed; and the sprite item + batcher (M2-SPRITE-01) —
# the engine-owned "declare, don't draw" declaration window + batch
# builder (sprite_batcher_tests.cpp's SpriteBatcher* suites: the
# grouping correctness (N atlases x materials x blends -> the exact
# group count, the deterministic group order, the per-group
# membership), the in-group instance order against hand-computed
# expected orders (the M2-SORT-01 sorted order restricted to the
# group, stable on equal keys), the bounded drop-oldest + warn
# overflow policy, the G-R11 counted + warned depth-override escape
# hatch, the stopped state / create validation / frame protocol /
# slot access edges, the determinism property on 3 000-sprite frames
# against the stable-sort oracle, and the 1 000-frame zero-allocation
# proof) — pure integer bookkeeping: no GL environment needed; and
# the GPU instanced draw submits (M2-SPRITE-02) — the frame pipeline's
# submit stage (sprite_draw_tests.cpp's SpriteDraw* suites: the create
# validation + stopped state (no GL), the submit precondition contract
# on a live context — the built-frame gate (an unbuilt window is never
# drawn as an empty frame), the unbound-atlas rejection, the empty
# frame, the instance-budget BudgetExhausted + warn, the bindAtlas
# argument domain, the rebind-replaces semantics — the roadmap's
# 1000-sprite offscreen scene (one instanced draw per group: the
# dispatch count == the group count, cross-checked against the opt-in
# GL PRIMITIVES_GENERATED count, and the whole frame pixel-by-pixel
# against a CPU reference rasterizer), and the 100-frame integration
# run through the M2-GL-02 RenderThread with the GL handoff) — the
# GL suites need a usable OpenGL 3.3 environment (the documented
# environment contract: GTEST_SKIPs where absent); and the atlas
# frame layout (M2-SPRITE-03) — the atlas UV frame animation hook
# (sprite_frames_tests.cpp's SpriteFrame* suites: the hand-computed
# UV rects for documented layouts (tight sheets, margins, non-square
# frames, single row/column), the documented failure contract
# (out-of-range frame -> InvalidArgument, never wrap; layout
# validation first-failure-wins), the sheet model against the
# documented formula + the float-exact domain + the adjacency rule,
# and the SpriteItem.frameIndex hook through the batcher) — pure
# float/integer math: no GL environment needed; and the render
# observability + draw-call budget (M2-SPRITE-04) — the M2-SPRITE-04
# profiler fields + the G-R2 per-pass draw-call cap
# (render_counters_tests.cpp's RenderCounters* suites: the create
# validation of Options::maxDrawCalls (first failure wins, one Warn) +
# the stopped state reads zero (no GL), the known small scene
# (10 sprites, 2 atlases, 2 blends -> 3 groups) with the per-frame
# counters EXACT (frame 1 + frame 2, the cross-frame state
# persistence) + the since-construction totals + the empty frame
# counting nothing + the opt-in PRIMITIVES_GENERATED feed, the cap
# warn firing at the configured count (pinned fields) with the frame
# still drawn (observation, never an execution gate) + no warn at the
# cap, and the texture-memory VRAM estimate gauge (binds +
# re-bind replacements)) — the GL suites need a usable OpenGL 3.3
# environment (the documented environment contract: GTEST_SKIPs where
# absent); and the tilemap (M2-TILE-01) — the chunked tile grid data,
# the auto-depth wiring of the M2-ISO-02 depth key table, and the
# static tile-quad batch path into the sprite batcher
# (tilemap_tests.cpp's TileMap* suites: the grid options + the
# flat/empty contract, the per-tile data writes/rejected edits/scene
# load + the "rebuild from scratch == incremental" property, the
# height -> depth-table wiring (a height edit changes exactly the
# edited cell's key), the hand-computed 4 x 4 chunk's quad
# positions/depth goldens + the (texture, material, blend) grouping
# and cross-frame determinism, the frame protocol/failure paths/custom
# options/no-log happy path, and the 1000-frame zero-allocation declare
# loop) — pure data + batcher bookkeeping: no GL environment needed.
#
# One executable per module (tests/README.md): laige-render_tests links
# the module under test plus gtest_main. The unfiltered entry runs the
# whole module; the `gl_context` entry is the M2-GL-01 Verify command
# (`ctest -R gl_context`), the `render_thread` entry is the M2-GL-02
# Verify command (`ctest -R render_thread`), the `matrices` entry is
# the M2-GL-03 Verify command (`ctest -R matrices`), the
# `iso_depth_key` entry is the M2-ISO-01 Verify command
# (`ctest -R iso_depth_key`), the `iso_depth_table` entry is the
# M2-ISO-02 Verify command (`ctest -R iso_depth_table`), the `camera`
# entry is the M2-CAM-01 Verify command (`ctest -R camera`), the
# `iso_camera` entry is the M2-CAM-02 Verify command
# (`ctest -R iso_camera`), the `projection` entry is the M2-PROJ-01
# Verify command (`ctest -R projection`), the `iso_picking` entry is
# the M2-ISO-03 Verify command (`ctest -R iso_picking`), the
# `depth_sort` entry is the M2-SORT-01 Verify command
# (`ctest -R depth_sort`), the `batcher` entry is the M2-SPRITE-01
# Verify command (`ctest -R batcher`), the `sprite_draw` entry is the
# M2-SPRITE-02 Verify command (`ctest -R sprite_draw`), the
# `sprite_frames` entry is the M2-SPRITE-03 Verify command
# (`ctest -R sprite_frames`), the `render_counters` entry is the
# M2-SPRITE-04 Verify command (`ctest -R render_counters`), and the
# `tilemap` entry is the M2-TILE-01 Verify command (`ctest -R
# tilemap`), each selecting exactly its suites from the shared
# executable.
#
# Environment note: the GlContextSmoke, RenderThreadOffscreen,
# SpriteDraw{State,Smoke,Pipeline}, and RenderCounters{Scene,Cap,
# Memory} suites require a usable OpenGL 3.3 environment — always
# present on the P0 CI runners (Mesa/ANGLE/Apple/Windows software or
# hardware drivers), where they must pass. On a local machine without
# a GL driver the suites GTEST_SKIP with the clean Status reason — the
# documented environment contract (docs/api/gl_context.md), not an
# engine failure. The GlContextGate/GlContextArgs/FrameClock/
# RenderThreadHandoff/SpriteDrawCreate/RenderCountersCreate/TileMap*
# suites always run (they make no GL calls).

add_executable(laige-render_tests
  gl_context_tests.cpp render_thread_tests.cpp matrices_tests.cpp
  iso_depth_key_tests.cpp iso_depth_table_tests.cpp camera_tests.cpp
  iso_camera_tests.cpp projection_tests.cpp iso_picking_tests.cpp
  depth_sort_tests.cpp sprite_batcher_tests.cpp sprite_draw_tests.cpp
  sprite_frames_tests.cpp render_counters_tests.cpp tilemap_tests.cpp)
laige_apply_engine_policy(laige-render_tests)
target_link_libraries(laige-render_tests PRIVATE gtest_main laige-render)
# The randomized suites draw through the test-only seed helper
# (docs/testing.md §4; tests/support/laige_test_seed.h).
target_include_directories(laige-render_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/support)

add_test(NAME laige-render_tests COMMAND laige-render_tests)
# The unfiltered entry also runs the M2-ISO-02 budget suite (in the
# gated builds it checks the entry), so it carries the same budgets
# path:
set_tests_properties(laige-render_tests PROPERTIES
  ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")

# M2-GL-01: the step's Verify command is `ctest -R gl_context`. The
# gtest filter is one UNQUOTED argument (see
# tests/laige-core/CMakeLists.txt for the rationale).
add_test(NAME gl_context
         COMMAND laige-render_tests --gtest_filter=GlContextGate.*:GlContextArgs.*:GlContextSmoke.*)

# M2-GL-02: the step's Verify command is `ctest -R render_thread`.
# FrameClock + RenderThreadHandoff run without any GL environment;
# RenderThreadOffscreen needs the same usable OpenGL 3.3 environment as
# GlContextSmoke (GTEST_SKIPs where absent — the documented
# environment contract).
add_test(NAME render_thread
         COMMAND laige-render_tests
         --gtest_filter=FrameClock.*:RenderThreadHandoff.*:RenderThreadOffscreen.*)

# M2-GL-03: the step's Verify command is `ctest -R matrices`. Pure math
# (no GL calls) — runs in every local tree and CI.
add_test(NAME matrices
         COMMAND laige-render_tests --gtest_filter=Matrices*)

# M2-ISO-01: the step's Verify command is `ctest -R iso_depth_key`.
# Pure math (no GL calls) — runs in every local tree and CI.
add_test(NAME iso_depth_key
         COMMAND laige-render_tests --gtest_filter=IsoDepthKey*)

# M2-ISO-02: the step's Verify command is `ctest -R iso_depth_table`.
# Pure data (no GL calls) — runs in every local tree and CI. The
# LAIGE_BUDGETS_PATH env var points the budget suite at the repo
# budgets.json (the test falls back to the working directory when the
# var is unset — the budget_harness_tests.cpp convention). The budget
# suite runs ONLY on the non-instrumented trees (methodology §4: the
# budget gates measure the canonical Debug tree — the sanitizer
# runtimes inflate absolute cost and own operator new); it
# GTEST_SKIPs elsewhere. The absolute 0.2 ms target is reference-
# platform-scoped (methodology §5: the shared macOS/Windows runners
# are slower and noisier than the ubuntu-24.04 reference — the
# m1-sim-tick scoping), so the gate is compiled only on Linux in the
# non-instrumented trees (the LAIGE_ISO_DEPTH_BUDGET define marks
# them); other platforms still run the budget WORKLOAD (ungated —
# the suite's other suites cover the same shapes, and the workload
# itself runs leak-free under ASan/TSan):
add_test(NAME iso_depth_table
         COMMAND laige-render_tests --gtest_filter=IsoDepthTable*)
set_tests_properties(iso_depth_table PROPERTIES
  ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")

# M2-CAM-01: the step's Verify command is `ctest -R camera`. Pure value
# math (no GL calls) — runs in every local tree and in CI.
add_test(NAME camera
         COMMAND laige-render_tests --gtest_filter=Camera*)

# M2-CAM-02: the step's Verify command is `ctest -R iso_camera`. Pure
# value math over the M2-CAM-01 Camera + the M2-GL-03 preset matrices
# (no GL calls) — runs in every local tree and in CI.
add_test(NAME iso_camera
         COMMAND laige-render_tests --gtest_filter=IsoCamera*)

# M2-PROJ-01: the step's Verify command is `ctest -R projection`. Pure
# value math over the M2-GL-03 / M2-CAM-01 matrices (no GL calls) — runs
# in every local tree and in CI.
add_test(NAME projection
         COMMAND laige-render_tests --gtest_filter=Projection*)

# M2-ISO-03: the step's Verify command is `ctest -R iso_picking`. Pure
# float math (no GL calls) — runs in every local tree and in CI. The
# LAIGE_BUDGETS_PATH env var points the budget suite at the repo
# budgets.json. The budget gate compiles only on Linux in the
# non-instrumented trees (LAIGE_ISO_PICK_BUDGET — the
# iso_depth_table_tests.cpp precedent); other platforms run the budget
# WORKLOAD ungated.
add_test(NAME iso_picking
         COMMAND laige-render_tests --gtest_filter=IsoPick*)
set_tests_properties(iso_picking PROPERTIES
   ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")

# M2-SORT-01: the step's Verify command is `ctest -R depth_sort`. Pure
# integer math (no GL calls) — runs in every local tree and in CI.
# The LAIGE_BUDGETS_PATH env var points the budget suite at the repo
# budgets.json. The budget gate compiles only on Linux in the
# non-instrumented trees (LAIGE_DEPTH_SORT_BUDGET — the
# iso_depth_table / iso_picking precedent); other platforms run the
# budget WORKLOAD ungated.
add_test(NAME depth_sort
         COMMAND laige-render_tests --gtest_filter=DepthSort*)
set_tests_properties(depth_sort PROPERTIES
   ENVIRONMENT "LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")

# M2-SPRITE-01: the step's Verify command is `ctest -R batcher`. Pure
# integer bookkeeping over pre-allocated storage (no GL calls) —
# runs in every local tree and in CI. No budget gate: the step's
# roadmap scope has no standalone budget entry (the composite 50k
# render-CPU budget of PRD §8.1 is measured with the submit stage,
# M2-PERF-01).
add_test(NAME batcher
         COMMAND laige-render_tests --gtest_filter=SpriteBatcher*)

# M2-SPRITE-02: the step's Verify command is `ctest -R sprite_draw`.
# SpriteDrawCreate runs without any GL environment; SpriteDrawState /
# SpriteDrawSmoke / SpriteDrawPipeline need the same usable OpenGL 3.3
# environment as GlContextSmoke (GTEST_SKIPs where absent — the
# documented environment contract).
add_test(NAME sprite_draw
         COMMAND laige-render_tests
         --gtest_filter=SpriteDrawCreate.*:SpriteDrawState.*:SpriteDrawSmoke.*:SpriteDrawPipeline.*)

# M2-SPRITE-03: the step's Verify command is `ctest -R sprite_frames`.
# Pure float/integer math (no GL calls) — runs in every local tree and
# in CI. No budget gate: the step's roadmap scope has no standalone
# budget entry (the per-frame conversion cost is part of the composite
# 50k render-CPU budget, measured with M2-PERF-01).
add_test(NAME sprite_frames COMMAND laige-render_tests
         --gtest_filter=SpriteFrame*)

# M2-SPRITE-04: the step's Verify command is `ctest -R render_counters`.
# RenderCountersCreate runs without any GL environment;
# RenderCounters{Scene,Cap,Memory} need the same usable OpenGL 3.3
# environment as the SpriteDrawState/Smoke suites (GTEST_SKIPs where
# absent — the documented environment contract). No budget gate: the
# step's roadmap scope has no standalone budgets.json entry (the
# composite 50k render budget of PRD §8.1 is measured with M2-PERF-01).
add_test(NAME render_counters COMMAND laige-render_tests
         --gtest_filter=RenderCounters*)

# M2-TILE-01: the step's Verify command is `ctest -R tilemap`. Pure
# data + batcher bookkeeping (no GL calls) — runs in every local tree
# and in CI. No budget gate: the step's roadmap scope has no
# standalone budgets.json entry (the per-frame declare cost is part of
# the composite 50k render-CPU budget, measured with M2-PERF-01).
add_test(NAME tilemap COMMAND laige-render_tests
         --gtest_filter=TileMap*)

if(NOT LAIGE_ASAN AND NOT LAIGE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(laige-render_tests PRIVATE LAIGE_ISO_DEPTH_BUDGET=1)
  target_compile_definitions(laige-render_tests PRIVATE LAIGE_ISO_PICK_BUDGET=1)
  target_compile_definitions(laige-render_tests PRIVATE LAIGE_DEPTH_SORT_BUDGET=1)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "")
    target_compile_definitions(laige-render_tests PRIVATE
      LAIGE_ISO_DEPTH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
    target_compile_definitions(laige-render_tests PRIVATE
      LAIGE_ISO_PICK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
    target_compile_definitions(laige-render_tests PRIVATE
      LAIGE_DEPTH_SORT_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
  endif()
endif()

if(LAIGE_TSAN)
  # Make the first data race report fatal to the test process (NFR-8.2).
  # The M2-GL-02 `render_thread` entry is the step's TSan gate (the
  # lock-free handoff's race-freedom, the 3000-frame no-deadlock run);
  # the pure-math `matrices`, `iso_depth_key`, `iso_depth_table`,
  # `camera`, `iso_camera`, `projection`, `iso_picking`, `depth_sort`,
  # `batcher`, `sprite_draw`, and `sprite_frames` entries carry the
  # option for uniformity (the pure-math ones have no shared state, but
  # the flag is harmless).
  # The `sprite_draw` entry is a second TSan gate: the
  # SpriteDrawPipeline integration run drives the render thread's
  # stages against the renderer + batcher from two threads (the GL
  # handoff's race-freedom). The `render_counters` entry is a third:
  # the M2-SPRITE-04 counters are plain owner-thread state (no shared
  # state of the step's own) — the GL suites run single-threaded, so
  # the entry gates the engine's GL teardown ordering under TSan.
  # The test-level
  # ENVIRONMENT replaces the job-level one, so the budgets path is
  # restated here.
  set_tests_properties(laige-render_tests gl_context render_thread matrices
    iso_depth_key iso_depth_table camera iso_camera projection
    iso_picking depth_sort batcher sprite_draw sprite_frames
    render_counters tilemap PROPERTIES
    ENVIRONMENT
    "TSAN_OPTIONS=halt_on_error=1;LAIGE_BUDGETS_PATH=${CMAKE_SOURCE_DIR}/budgets.json")
endif()

if(LAIGE_ASAN)
  # The headless smoke's EGL display is process-lifetime by design
  # (docs/api/gl_context.md; CORE-009): the libglvnd dispatcher does not
  # export eglDestroyDisplay, so the Mesa driver state reachable through
  # the display is still live at process exit — LSan reports it as a leak
  # (2026-09-29 CI run 36501951395: ~1.88 MB across 17,781 allocations,
  # every stack allocated through createHeadless / createFbo /
  # finishCreation / readPixel). That is vendor state, not engine
  # memory: every engine-owned allocation in this module is freed by
  # RAII before exit. It cannot be suppressed surgically — LSan's
  # `leak:` suppression lines abort ASan's own suppression parser at
  # init (mutually exclusive formats in one file, verified against
  # compiler-rt), and the leak roots are libEGL-internal state objects
  # the engine never holds a pointer to. detect_leaks=0 for the render
  # entries instead (render_thread's RenderThreadOffscreen suite,
  # sprite_draw's SpriteDraw{State,Smoke,Pipeline} suites, and
  # render_counters's RenderCounters{Scene,Cap,Memory} suites create
  # the same headless context): in-run ASan/UBSan error detection
  # stays fully active (the smoke still performs its GL work under
  # the sanitizer). The test-level ENVIRONMENT replaces the job-level
  # ASAN_OPTIONS (GitHub workflow), so mirror its options here.
  set_tests_properties(laige-render_tests gl_context render_thread
    sprite_draw render_counters PROPERTIES
    ENVIRONMENT
    "ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0:log_path=${CMAKE_SOURCE_DIR}/asan-reports/asan")
endif()

# The offscreen smoke creates a real (software) context + FBO; give a
# slow headless runner generous headroom. The pure-math `matrices`,
# `iso_depth_key`, `camera`, `iso_camera`, `projection`, `iso_picking`,
# `batcher`, and `sprite_frames` entries are small (thousands of O(1)
# computations,
# comparisons, and O(1) camera / projection updates and picks at most,
# plus 10k-cell round trips and a few hundred-sprite batch builds) —
# 60s is generous headroom.
set_tests_properties(laige-render_tests gl_context PROPERTIES TIMEOUT 180)
set_tests_properties(render_thread PROPERTIES TIMEOUT 300)
# The `render_counters` entry creates the headless context + FBO and
# renders a few 10-sprite frames (no pixel comparison, no render
# thread) — generous headroom for a slow headless runner, between the
# small math entries and the 1000-sprite `sprite_draw` scene.
set_tests_properties(render_counters PROPERTIES TIMEOUT 120)
# The `sprite_draw` entry renders a 1000-sprite frame through a real
# (software) context + FBO, compares the whole 128x128 frame against a
# CPU reference rasterizer (16 384 pixels x 1000 sprites), and runs the
# 100-frame RenderThread integration — generous headroom for a slow
# headless runner, like the offscreen smoke.
set_tests_properties(sprite_draw PROPERTIES TIMEOUT 300)
set_tests_properties(matrices PROPERTIES TIMEOUT 60)
set_tests_properties(iso_depth_key PROPERTIES TIMEOUT 60)
set_tests_properties(camera PROPERTIES TIMEOUT 60)
set_tests_properties(iso_camera PROPERTIES TIMEOUT 60)
set_tests_properties(projection PROPERTIES TIMEOUT 60)
set_tests_properties(iso_picking PROPERTIES TIMEOUT 60)
set_tests_properties(batcher PROPERTIES TIMEOUT 60)
set_tests_properties(sprite_frames PROPERTIES TIMEOUT 60)
set_tests_properties(tilemap PROPERTIES TIMEOUT 60)
# The `iso_depth_table` and `depth_sort` entries include their budget
# gates: 2 backends x (100 warm-up + 3000 measured) iterations of 10 000
# setTile calls, and 3 000 sorts of 10 000 keys respectively — a few
# seconds each on the canonical Debug tree; 120s is generous headroom
# for a slow CI runner.
set_tests_properties(iso_depth_table PROPERTIES TIMEOUT 120)
set_tests_properties(depth_sort PROPERTIES TIMEOUT 120)
