# laige-render test assets

Committed test assets for `tests/laige-render`. Test assets are inputs,
not engine code: the test executables read them at run time (the path
is handed over the CTest `ENVIRONMENT`, e.g. `LAIGE_TEST_FONT_PATH` —
see `tests/laige-render/CMakeLists.txt`), never at configure time.

## vera.ttf

**Bitstream Vera Sans** (regular), 65 932 bytes — the committed test
font for the M2-TEXT-01 font atlas suites (`font_tests.cpp`).

- **License:** SIL Open Font License 1.1 (OFL 1.1) — the license under
  which Bitstream Inc. released the Vera font family in 2003; the
  canonical license text is at <https://openfontlicense.org> (OFL 1.1
  is a free license with no copyleft and no attribution condition at
  embedding; the font file itself carries the copyright line
  `Copyright (c) 2003 by Bitstream, Inc.`).
- **Why this font:** the engine's own test asset (no system font is
  trusted — P0 runners differ in their font packages, NFR-8.8: the
  build and the tests must be reproducible in-tree). It is small
  (66 KB), a complete TrueType font with the full default Latin-1
  glyph set (32..255), and stable byte-for-byte (committed under the
  repo's `text=auto eol=lf` attributes; the binary is never
  transformed).
- **Use:** the M2-TEXT-01 suites pin their metric goldens against THIS
  file at `fontSize = 16`; a replacement asset changes the goldens
  (the test file records the measured values — re-pin in the same
  change).
