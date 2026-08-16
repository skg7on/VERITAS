# src/tools/CMakeLists.txt
#
# Four VERITAS CLI binaries. At M0 each one only implements `--version`; the
# real ingest / query / diff / explain flows arrive with later milestones.
# Every CLI links veritas_core and picks up the shared warning contract.

foreach(_veritas_cli veritas-build veritas-query veritas-diff veritas-explain)
  add_executable(${_veritas_cli} "${_veritas_cli}.cpp")
  target_link_libraries(${_veritas_cli} PRIVATE veritas_core)
  veritas_add_warnings(${_veritas_cli})
endforeach()
