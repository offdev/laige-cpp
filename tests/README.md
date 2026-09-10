# tests/

Unit and integration tests, one directory per engine module mirroring
`src/`; test executables are named `<module>_tests` (convention finalized
in M0-TEST-01). GoogleTest is a dev-only dependency (PRD §11), vendored in
M0-DEP-01, and never linked into engine libraries.

No tests yet: engine code lands from M0-BUILD-01 onward.
