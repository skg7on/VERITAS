# src/core/CMakeLists.txt
#
# Builds `veritas_core`, the foundation library every other VERITAS
# component links against. At M0 it exposes Status / StatusOr and Version,
# nothing more.

# Capture the short git SHA of HEAD at configure time. Falls back to
# "unknown" when configuring from an archive or before any commit exists,
# so no downstream code has to special-case the empty string.
find_package(Git QUIET)
set(VERITAS_GIT_REVISION "unknown")
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _veritas_git_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _veritas_git_result
  )
  if(_veritas_git_result EQUAL 0 AND _veritas_git_sha)
    set(VERITAS_GIT_REVISION "${_veritas_git_sha}")
  endif()
endif()
message(STATUS "VERITAS: Git revision      : ${VERITAS_GIT_REVISION}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/Version.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/Version.cpp"
  @ONLY
)

add_library(veritas_core
  Status.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/Version.cpp"
)

target_include_directories(veritas_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(veritas_core PUBLIC cxx_std_20)

veritas_add_warnings(veritas_core)

add_library(veritas::core ALIAS veritas_core)
