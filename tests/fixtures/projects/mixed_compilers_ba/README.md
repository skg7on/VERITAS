## mixed_compilers_ba fixture

Same project content as `mixed_compilers_ab`, but the compile-commands entries
appear in `(gcc, clang++)` order. Together the two fixtures pin the
regression test that reordered `compile_commands.json` entries produce
identical `compiler_id`, `build_variant_id`, and `repository_id`.
