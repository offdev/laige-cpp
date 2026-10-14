# Baseline: `m2-ac4` — first measurement of the PRD §4 AC-4.3
# 60 FPS frame budget (`sprites_10k_frame`) + the AC-4.x acceptance record

Recorded by the **M2-AC-01** step (2026-10-14). This is the **thirteenth**
baseline file; it is immutable (methodology §4 — superseding it later adds a
new file, it is never edited). It is the **first recording** of the
`sprites_10k_frame` budget (`measured` was 0 = "not yet measured" per
methodology §4) and **supersedes nothing**; it records the AC-4.x
acceptance checklist required by the step's Verify ("AC checklist
recorded in `docs/benchmarks/baselines/m2-ac4.md`").

## Why this baseline exists

M2-AC-01 lands the PRD §4 AC-4.x acceptance criteria in executable form.
The `ac4` suite of `laige-bench` (the alias `render-sort-10k` is the same
workload under the dedicated suite name the AC-4.3 scope uses) sorts and
draws 10 000 overlapping sprites in one 60 FPS frame for 4000 frames
(1000 warm-up + 3000 measured, both SimMath backends, offscreen 1280×720,
software GL): the workload is a 100 × 10 cell grid (1000 cells at their
centers, x in −49.5..49.5, y in −4.5..4.5), 10 sprites stacked per cell at
the SAME position and step height `(gx >> 4) + (gy >> 4)` (0..6) — the
sort's equal-key ties (10-way, broken by declaration order per RENDER-003)
and the frame's 10× self-overdraw both exercised — scale 1.0, two atlases
× two blends = four (atlas, material, blend) groups, the engine-owned
`isoDepthKey<Backend>` per item (backend-quantized), the Iso engine default
(the ADR 0005 preset) at the grid-fit scale k = 1/128. One measured sample
is one whole frame (the AC-4.3 "sorted + drawn at 60 FPS" — the batch stage
beginFrame + 10 000 adds + build, with the M1-ALLOC-01 zero-allocation watch
armed around it — a violating frame aborts the run, exit 1, CORE-008 —
plus the submit stage's offscreen render). The `sprites_10k_frame` sample
is the whole frame's CPU ms (mean ≤ 16.7 ms, the §8.1 60 FPS frame budget);
the frame's draw-call count is asserted to be exactly 4 in teardown
(RENDER-001's one-draw-per-group contract). The suite's setup additionally
asserts the AC-4.4 engine default: `ProjectionView{}.mode == Iso` (ADR
0005) — a default drift is a loud failure, not a baseline shift.

## The AC-4.x acceptance checklist

| AC | What it requires | Where it is proven | Status (this PR) |
|---|---|---|---|
| AC-4.1 | One engine build runs a side-view and an isometric scene, differing only in config | The `ac4_iso` / `ac4_side_view` golden fixtures (the SAME 96-sprite world + 3-widget UI pass, one binary, config-only difference) + the GL-free `GoldenFixtures.Ac4*` tests (scene shape, config-only diff, bit-exact camera matrices) | `ctest -R golden` green (Linux llvmpipe lanes; the two new references committed in this PR, bootstrapped two-run: run 38526109942's `golden-references` job rendered both, in-run determinism compare all four fixtures failing=0 max_diff=0 budget=9; the verification run compares all four against the committed references) + `golden_compare` green in every tree |
| AC-4.2 | No sim/physics/pathing/network code depends on the projection mode | `tools/laige-surface-lint`: no `ProjectionMode` mention in the `laige-sim` / `laige-net` public surface (exit 0 OK / 1 violation / 2 structural), CTest entries `ac4_surface*` (fixture trees for each exit path + the real tree) in every PR and merge run | `ctest -R ac4` green (all four entries, every tree) |
| AC-4.3 | 10k overlapping sprites sorted + drawn at 60 FPS on CI reference hardware | The `ac4` / `render-sort-10k` bench suites: `laige_bench_ac4_fpx16` / `_fp32` CTest entries (Linux, canonical 3000/1000, `--budget=sprites_10k_frame`), in the `ci:slow:bench-50k` label-gated jobs | All four entries PASS the 16.7 ms mean budget (worst mean 12.70 ms, g++ fp32) — the first CI measurement table below |
| AC-4.4 | Iso picking exact at all zooms; depth-key rebuild within §8.1; isometric is the template default | The existing M2-ISO-03 picking tests (`iso_picking`) and M2-ISO-02 depth-table tests (`iso_depth_table`, the `iso_depthkey_rebuild` budget entry) — both green in this PR's runs; the engine default (`ProjectionView{}.mode == Iso`) asserted in the ac4 suite's setup (the identity line's `default=Iso` field) | `iso_picking` + `iso_depth_table` green; the setup assertion runs on every ac4 entry (a drift fails the run) |

## First CI measurement (run 38526109942, commit e387be3, 2026-10-14)

Reference environment: ubuntu-24.04, CMake Debug, g++ (the linux-gcc job)
and clang++ (the linux-clang job), Mesa surfaceless EGL + llvmpipe software
GL, offscreen 1280×720, 4000 frames per entry (1000 warm-up + 3000
measured), the canonical 3000/1000 shape.

| Entry (job) | backend | frame mean (ms) | frame min (ms) | frame max (ms) | draw_calls (max) | budget (16.7 ms mean) |
|---|---|---|---|---|---|---|
| laige_bench_ac4_fpx16 (g++) | fixed_point_16_16 | 11.1168 | 7.5567 | 26.3255 | 4 | PASS |
| laige_bench_ac4_fp32 (g++) | float_pinned_32 | 12.6959 | 8.7657 | 30.7213 | 4 | PASS |
| laige_bench_ac4_fpx16 (clang) | fixed_point_16_16 | 11.5452 | 7.1321 | 19.2588 | 4 | PASS |
| laige_bench_ac4_fp32 (clang) | float_pinned_32 | 12.1453 | 8.1677 | 24.9973 | 4 | PASS |

Zero-allocation: no abort on any of the 16 000 frames (4 entries × 4000
frames) — the batch stage allocated nothing per frame (FR-2.2/PERF-003).
AC-4.4 assertion: the identity line's `default=Iso` printed on every entry
(a default drift would have failed the run in setup).

A later pre-fix run (38543888405) recorded the same shape on different
runners (clang fpx16 mean 11.9165, clang fp32 10.1502, g++ fp32 9.85123 —
all PASS); its numbers are not part of the reference table above (the
table records the first complete measurement, per the M2-PERF-01
convention).

## The exit-time segfault (root-caused and fixed in this PR)

During the first measurement runs (38526109942 through 38540997015) every
`laige_bench_ac4_*` entry SEGFAULTED on the CI Linux lanes **after** the
full report printed — in the exit path, not the measured region.
Investigation (this PR's history):

- The crash is frame-count-dependent (a 20-frame run exits cleanly on CI;
  3000 frames crashes) and a Heisenbug (clean under gdb, under ASan, and on
  the developer machine).
- An in-process SIGSEGV/SIGBUS capture (temporary, removed with the fix)
  showed the crash hits **three worker threads, not the main thread**
  (crashed tids 4209/4211/4212 vs main 4208), with no usable backtrace.
- A temporary `/proc/self/task` census named the workers:
  **`llvmpipe-0..3`** — Mesa's software-rasterizer worker pool, spawned by
  the EGL context creation. The render-50k entries (same driver, same
  process shape) exit cleanly — the race is timing-luck, which is why the
  crash does not reproduce deterministically.
- Root cause: the driver executes submitted GL work on that pool, and the
  engine tore the context down (and `dlclose`d the driver library) while
  pool threads could still be inside it. Two fixes, both in
  `GlContext::Impl::destroy()`: (1) `glFinish()` — make the context
  current and drain all pending work before `eglDestroyContext` (GL-spec
  guarantee: after `glFinish` returns, all previously issued work is
  complete); (2) **stop `dlclose`ing the dlopened libEGL handle** — a
  library with live, never-joined threads must not be closed (dlclose'ing
  under live threads is undefined behavior; the driver's fini could wake
  the workers into unmapped pages). The handle is kept for the process's
  remaining lifetime (one per created context; the kernel reclaims it at
  exit). Both backends (EGL and GLFW windowed) get the drain.
- Verification: run 38552829408 (commit 1bcf3a1) — **all eight bench
  entries (4 ac4 + 4 render-50k) PASS on all four Linux lanes**, no
  segfault. The render-50k budget numbers are unchanged by the fix (the
  drain is one-shot teardown work, never on the frame loop).

## The `measured` value

`budgets.json`: `sprites_10k_frame` `measured` 0 → 12.1453 — the latest
recorded value, the worse of the two backends on the reference (Clang)
lane (fp32 12.1453 vs fpx16 11.5452), per the M2-PERF-01 convention.
`target` 16.7 unchanged (the PRD §8.1 60 FPS frame budget — the first
recording of this budget, not a calibration of a pre-existing one).

## Margin

The target (16.7 ms mean) sits 37.5% above the worst recorded mean on the
reference (Clang) lane (12.1453 ms) and 31.5% above the worst across both
lanes (12.6959 ms, g++ fp32). Per the runner-degradation policy (methodology
§5): a future gate failure above 16.7 ms is evidence of reference-runner or
toolchain degradation (or a real regression, disambiguated by the identity
line), the remedy is a budget revision through the same process, never a
silent target move.

## Job durations

The four slow-bench jobs of the first measurement run (each runs one
render-50k entry plus one ac4 entry) finished in 586 s (g++ fpx16), 597 s
(g++ fp32), 595 s (clang fpx16) and 611 s (clang fp32); the verification
run's four jobs finished in 614 s, 661 s, 301 s and 559 s — all well inside
the 30-min cap. The ac4 entry itself is ≈25–50 s per entry on the CI
runners, so it stays on the gated side of the 30 s threshold until a
master run re-derives the split.

## Open items

- The side-view fixture's world pass is degenerate by documented geometry
  (`SpriteItem` carries the 2D ground-plane position, no elevation field
  — no new engine API added, CORE-004): all z = 0 sprites project to
  zero-height quads under the z = 0 reference plane. The AC-4.1
  "differing only in config" is asserted on the scene (the
  `GoldenFixtures.Ac4ConfigsOnlyDiffer` test), not on the world raster; a
  platformer vertical scene needs the M3 elevation work.
- If the gate ever fails a P0 Linux job above 16.7 ms, the evidence
  indicates reference-runner or toolchain degradation (or a real engine
  regression — disambiguated by the identity line and the zero-allocation
  record) — the remedy is another budget revision through this same
  process (new baseline file + `budgets.json` + PRD revision + change
  log), not a silent target move.
