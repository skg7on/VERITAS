#
#  FindZ3.cmake - Locate Z3 either via a packaged config or by header+lib
#

include(GNUInstallDirs)

# Try upstream/system CONFIG package first; use it if found (sets required variables):
find_package(Z3 CONFIG QUIET HINTS ${Z3_HOME} $ENV{Z3_HOME} ${Z3_DIR} $ENV{Z3_DIR})
if(Z3_FOUND)
  message(STATUS "Found upstream/system Z3 package (Z3Config.cmake)")
  message(STATUS "Z3_INCLUDE_DIR from CONFIG: ${Z3_INCLUDE_DIR}")
  message(STATUS "Z3_INCLUDE_DIRS from CONFIG: ${Z3_INCLUDE_DIRS}")
  # Ensure Z3_CXX_INCLUDE_DIRS and Z3_C_INCLUDE_DIRS are set for consistency
  # Try multiple variables that might be set by the CONFIG package
  if(NOT DEFINED Z3_CXX_INCLUDE_DIRS)
    if(DEFINED Z3_INCLUDE_DIR)
      set(Z3_CXX_INCLUDE_DIRS ${Z3_INCLUDE_DIR})
      set(Z3_C_INCLUDE_DIRS ${Z3_INCLUDE_DIR})
    elseif(DEFINED Z3_INCLUDE_DIRS)
      set(Z3_CXX_INCLUDE_DIRS ${Z3_INCLUDE_DIRS})
      set(Z3_C_INCLUDE_DIRS ${Z3_INCLUDE_DIRS})
    elseif(TARGET z3::libz3)
      # Try to get include dirs from the imported target
      get_target_property(_z3_includes z3::libz3 INTERFACE_INCLUDE_DIRECTORIES)
      if(_z3_includes)
        set(Z3_CXX_INCLUDE_DIRS ${_z3_includes})
        set(Z3_C_INCLUDE_DIRS ${_z3_includes})
        set(Z3_INCLUDE_DIR ${_z3_includes})
      endif()
    endif()
  endif()
  message(STATUS "Final Z3_CXX_INCLUDE_DIRS: ${Z3_CXX_INCLUDE_DIRS}")
else()
  if(NOT Z3_FIND_QUIETLY)
    message(STATUS "Failed to find upstream/system Z3 package; reverting to manual search")
  endif()

  # Fall back to explicit manual header + lib search (prioritise searching $Z3_DIR)
  find_library(
    Z3_LIBRARY_DIR
    NAMES z3 libz3
    HINTS ${Z3_HOME} $ENV{Z3_HOME} ${Z3_DIR} $ENV{Z3_DIR}
    PATH_SUFFIXES bin lib ${CMAKE_INSTALL_BINDIR} ${CMAKE_INSTALL_LIBDIR}
  )
  find_path(
    Z3_INCLUDE_DIR
    NAMES z3++.h
    HINTS ${Z3_HOME} $ENV{Z3_HOME} ${Z3_DIR} $ENV{Z3_DIR}
    PATH_SUFFIXES include ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # If the headers were found, find & extract the Z3 version number
  set(_ver_h "${Z3_INCLUDE_DIR}/z3_version.h")
  if(EXISTS "${_ver_h}")
    file(READ "${_ver_h}" _z3_ver_h)
    message(STATUS "Extracted Z3 version information from ${_ver_h}")

    # Match each component explicitly
    string(REGEX MATCH "#define Z3_MAJOR_VERSION[ \t]+([0-9]+)" _major "${_z3_ver_h}")
    set(Z3_VERSION_MAJOR "${CMAKE_MATCH_1}")

    string(REGEX MATCH "#define Z3_MINOR_VERSION[ \t]+([0-9]+)" _minor "${_z3_ver_h}")
    set(Z3_VERSION_MINOR "${CMAKE_MATCH_1}")

    string(REGEX MATCH "#define Z3_BUILD_NUMBER[ \t]+([0-9]+)" _patch "${_z3_ver_h}")
    set(Z3_VERSION_PATCH "${CMAKE_MATCH_1}")

    string(REGEX MATCH "#define Z3_REVISION_NUMBER[ \t]+([0-9]+)" _tweak "${_z3_ver_h}")
    set(Z3_VERSION_TWEAK "${CMAKE_MATCH_1}")

    # Fallback if any of the components are unset
    foreach(_part MAJOR MINOR PATCH TWEAK)
      if(NOT Z3_VERSION_${_part})
        set(Z3_VERSION_${_part} 0)
      endif()
    endforeach()

    set(Z3_VERSION_STRING "${Z3_VERSION_MAJOR}.${Z3_VERSION_MINOR}.${Z3_VERSION_PATCH}.${Z3_VERSION_TWEAK}")
    message(STATUS "Parsed Z3 version: ${Z3_VERSION_STRING}")
  else()
    message(WARNING "Z3 version header not found: ${_ver_h}")
    set(Z3_VERSION_MAJOR 0)
    set(Z3_VERSION_MINOR 0)
    set(Z3_VERSION_PATCH 0)
    set(Z3_VERSION_TWEAK 0)
    set(Z3_VERSION_STRING "0.0.0.0")
  endif()

  # Validate that both the public headers & the library files were found
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(
    Z3
    REQUIRED_VARS Z3_INCLUDE_DIR Z3_LIBRARY_DIR
    VERSION_VAR Z3_VERSION_STRING
  )

  # Create an imported target if all required variables were found
  if(Z3_FOUND)
    add_library(z3::libz3 UNKNOWN IMPORTED)
    set_target_properties(z3::libz3 PROPERTIES IMPORTED_LOCATION "${Z3_LIBRARY_DIR}")
    set_target_properties(z3::libz3 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Z3_INCLUDE_DIR}")
    message(STATUS "Manually found Z3 (libs: ${Z3_LIBRARY_DIR}; public headers: ${Z3_INCLUDE_DIR})")

    # Expose other equivalents for variables exported by the regular Z3 CMake package
    set(Z3_CXX_INCLUDE_DIRS ${Z3_INCLUDE_DIR})
    set(Z3_C_INCLUDE_DIRS ${Z3_INCLUDE_DIR})
    set(Z3_LIBRARIES z3::libz3)
  elseif(Z3_FIND_REQUIRED)
    message(FATAL_ERROR "Failed to find Z3 library files/public headers!")
  elseif(NOT Z3_FIND_QUIETLY)
    message(WARNING "Failed to manually find Z3 libraries/headers")
  endif()
endif()
