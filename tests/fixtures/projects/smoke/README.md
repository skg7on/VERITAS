# smoke fixture

Minimal C++ translation unit reserved for integration tests that ingest a
project's `compile_commands.json`. M0 only establishes the layout; the
first consumer is M1 (project ingestion).

`compile_commands.json` uses a `${VERITAS_SOURCE_DIR}` placeholder in the
`directory` field. Tests that consume the fixture are expected to
substitute the absolute path to their local checkout before handing the
file to the compilation database loader.
