#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "ftpclient::ftpclient" for configuration "Release"
set_property(TARGET ftpclient::ftpclient APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(ftpclient::ftpclient PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libftpclient.so.1.0.0"
  IMPORTED_SONAME_RELEASE "libftpclient.so.1"
  )

list(APPEND _cmake_import_check_targets ftpclient::ftpclient )
list(APPEND _cmake_import_check_files_for_ftpclient::ftpclient "${_IMPORT_PREFIX}/lib/libftpclient.so.1.0.0" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
