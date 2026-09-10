# tests/

Unit and integration tests, one directory per engine module mirroring
`src/`; test executables are named `<module>_tests` (convention finalized in
M0-TEST-01).

**Test framework: GoogleTest** — a dev-only dependency (PRD §11) vendored in
`deps/googletest` and locked in `deps.lock` (M0-DEP-01). Test executables
link `gtest_main` (which provides `main()`); GoogleTest is **never** linked
into engine libraries. See
[ADR 0004](../docs/decisions/0004-google-test-vendoring.md).

`laige-core_tests` (build smoke test, M0-BUILD-01; converted to a GoogleTest
suite in M0-DEP-01) is the first suite: it verifies the static/shared link
variant (NFR-8.9) and the NFR-8.10 language policy, and proves the
GoogleTest plumbing end to end (configure → build → `ctest`). Module unit
suites land with each M0-CORE-xx step, each as a separate source file in
the module's single test executable, exposed as its own CTest entry —
M0-CORE-01's Result/Status/error-registry suites run as
`ctest -R result_status` (filtering the shared executable to the
`ResultStatus`, `Status`, and `ErrorCodeRegistry` suites).
