# missing_compile_database fixture

Deliberately omits `compile_commands.json` so that M1 tests can assert that
`veritas::build::ResolveProjectInput` rejects a project directory without a
compilation database with `StatusCode::kFailedPrecondition`.

The directory is intentionally empty apart from this README.
