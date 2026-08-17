## multiple_tus fixture

Two well-formed C++ translation units, `a.cpp` and `b.cpp`, listed in
`compile_commands.json` in reverse (b, a) order. M1 tests use this fixture to
assert that the manifest loader:

- loads every entry,
- resolves each source against the working directory,
- normalizes paths to repository-relative `TaggedPath` values, and
- produces byte-identical canonical bytes regardless of entry order.
