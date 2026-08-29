#[=======================================================================[.rst:
FindLibFFI
-----------

Find the FFI libraries

IMPORTED targets
^^^^^^^^^^^^^^^^

This module defines the following :prop_tgt:`IMPORTED` target:

``LibFFI::LibFFI``

Result variables
^^^^^^^^^^^^^^^^

This module will set the following variables if found:

``LIBFFI_INCLUDE_DIRS``
  where to find ffi.h, etc.
``LIBFFI_LIBRARIES``
  the libraries to link against to use libffi.
``LIBFFI_FOUND``
  TRUE if found

#]=======================================================================]

# Look for the necessary header
find_path(LIBFFI_INCLUDE_DIR NAMES ffi.h
          PATHS /usr /usr/local /opt/local /usr/include/ffi
          PATH_SUFFIXES include include/ffi include/x86_64-linux-gnu x86_64-linux-gnu)
mark_as_advanced(LIBFFI_INCLUDE_DIR)

# Look for the necessary library
find_library(LIBFFI_LIBRARY NAMES ffi)
mark_as_advanced(LIBFFI_LIBRARY)

find_package_handle_standard_args(LibFFI
    REQUIRED_VARS LIBFFI_INCLUDE_DIR LIBFFI_LIBRARY)

# Create the imported target
if(LIBFFI_FOUND)
    set(LIBFFI_INCLUDE_DIRS ${LIBFFI_INCLUDE_DIR})
    set(LIBFFI_LIBRARIES ${LIBFFI_LIBRARY})
    if(NOT TARGET LibFFI::LibFFI)
        add_library(LibFFI::LibFFI UNKNOWN IMPORTED)
        set_target_properties(LibFFI::LibFFI PROPERTIES
            IMPORTED_LOCATION             "${LIBFFI_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBFFI_INCLUDE_DIR}")
    endif()
endif()
