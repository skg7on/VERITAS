## missing_source fixture

`compile_commands.json` references `missing.cpp`, which is deliberately not
present on disk. M1 tests use this fixture to assert that the manifest
loader fails the whole project load (`kFailedPrecondition`) instead of
silently dropping the translation unit.
