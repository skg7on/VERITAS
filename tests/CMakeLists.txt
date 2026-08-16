find_program(VERITAS_BASH_EXECUTABLE bash REQUIRED)

add_test(
  NAME veritas_pre_commit_hook
  COMMAND
    "${VERITAS_BASH_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks/pre-commit-hook-test.sh"
    "${PROJECT_SOURCE_DIR}/.githooks/pre-commit"
)

set_tests_properties(veritas_pre_commit_hook PROPERTIES TIMEOUT 30)
