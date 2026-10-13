# Golden reference frames (M2-GOLD-01)

The committed reference PNGs of the golden image test infrastructure:
one 1280x720 RGBA8 PNG per fixture, the offscreen render of the
committed golden scene fixtures (tests/golden/golden_fixtures.h)
through the real engine path (`GlContext::createHeadless` +
`SpriteRenderer::submit` + one `GlContext::readPixels` capture, rows
flipped to the top-origin / y-down screen convention). The ctest
entry `golden` (tools/golden) compares every lane's render against
these frames with the documented tolerance (max channel diff 2,
0.001% pixel budget — explicit flags, never hidden;
docs/testing.md).

## The reference environment

- Runner: ubuntu-24.04 (the P0 first lane).
- Build: CMake Debug, g++ (the canonical linux-gcc configuration).
- GL: Mesa surfaceless EGL + llvmpipe software GL (the CI runtime
  stack of the Linux jobs).
- Reference generator: `laige-golden --update`, invoked by either
  generation path (both verify in-run determinism before printing
  the frames): the label-gated `golden-references` job of
  `.github/workflows/ci-pull.yml` (PR lane, label `ci:golden-update`)
  or the `workflow_dispatch`-only workflow
  `.github/workflows/golden-update.yml` (post-merge master path —
  workflow_dispatch only sees workflows on the default branch, so a
  PR branch cannot be dispatched).

The provenance of the PNGs currently committed lives in the commit
that added them (the golden-update run id and its environment are
stated there and in the PR/roadmap record). A reference change is a
human decision: regenerate, review, commit.

## Regeneration (human-approved path)

1. Trigger the generation on the branch that carries the rendering
   change: on a PR branch, add the `ci:golden-update` label to the
   PR (the label-gated `golden-references` job of
   `.github/workflows/ci-pull.yml` runs with the next run — the label
   must be on the PR before the run starts); on master after the
   change merged, dispatch `.github/workflows/golden-update.yml`
   (workflow_dispatch).
2. Read the job log; each reference is printed as single-line
   base64 between `GOLDEN_REFERENCE_BEGIN <fixture>` /
   `GOLDEN_REFERENCE_END <fixture>` markers (the PR lane cannot
   upload artifacts — the pull_request token is read-only — so the
   log is the transfer channel).
3. Decode each block to `tools/golden/references/<fixture>.png`,
   review the frames against the old references (the tolerance is
   what ctest applies; the review is of the rendered scene, not a
   byte diff), and commit the PNGs with the change that caused them.

Local form (a machine with a usable OpenGL 3.3 environment — never
on the agents' GPU-restricted machines):

```
LAIGE_TEST_FONT_PATH=$PWD/tests/laige-render/assets/vera.ttf \
  ./build/tools/golden/laige-golden --update --references tools/golden/references
```

`--update` writes atomically (a `.tmp` file in this directory, then
rename), so a crash mid-write never leaves a torn reference.

## The no-widening rule

A driver, engine, or fixture change that pushes a lane's render
beyond the tolerance fails `golden`. The fix is a reviewed
regeneration (above) when the new frame is the correct one, or a
code fix when it is not — never a silent change of the tolerance
flags (TEST-008). If a driver breaks determinism, the affected CI
job is pinned to the failing environment and the step reopened
(roadmap M2-GOLD-01), not the tolerance widened.
