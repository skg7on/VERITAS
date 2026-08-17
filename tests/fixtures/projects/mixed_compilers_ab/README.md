## mixed_compilers_ab fixture

Same project content as `mixed_compilers_ba`, but the compile-commands entries
appear in `(clang++, gcc)` order. Together the two fixtures pin the
regression test that reordered `compile_commands.json` entries produce
identical `compiler_id`, `build_variant_id`, and `repository_id`.
