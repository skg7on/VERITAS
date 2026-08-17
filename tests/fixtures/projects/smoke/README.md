# smoke fixture

Minimal C++ translation unit reserved for integration tests that ingest a
project's `compile_commands.json`. M0 only establishes the layout; the
first consumer is M1 (project ingestion).

`compile_commands.json` uses a `@PROJECT_ROOT@` placeholder in the
`directory` field. Consumers copy the fixture to a temporary directory
and substitute the placeholder with the canonical temporary path before
handing the file to the compilation database loader; see
`tests/support/ProjectFixture.h`.
