# tests/

Unit and integration tests, one directory per engine module mirroring
`src/`; test executables are named `<module>_tests` (convention finalized
in M0-TEST-01). GoogleTest is a dev-only dependency (PRD §11), vendored in
M0-DEP-01, and never linked into engine libraries.

`laige-core_tests` (build smoke test, M0-BUILD-01) is the first test; module
unit suites land with each M0-CORE-xx step. GoogleTest arrives in M0-DEP-01 —
until then tests are plain C++ executables registered with CTest.
