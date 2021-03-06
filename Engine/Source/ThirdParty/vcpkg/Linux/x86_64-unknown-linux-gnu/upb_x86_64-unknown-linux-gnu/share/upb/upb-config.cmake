# Generated by CMake

if("${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}" LESS 2.5)
   message(FATAL_ERROR "CMake >= 2.6.0 required")
endif()
cmake_policy(PUSH)
cmake_policy(VERSION 2.6...3.18)
#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Protect against multiple inclusion, which would fail when already imported targets are added once more.
set(_targetsDefined)
set(_targetsNotDefined)
set(_expectedTargets)
foreach(_expectedTarget upb::upb upb::fastdecode upb::upb_json upb::upb_pb upb::port upb::table upb::descriptor_upb_proto upb::handlers upb::reflection upb::textformat upb::all_libs)
  list(APPEND _expectedTargets ${_expectedTarget})
  if(NOT TARGET ${_expectedTarget})
    list(APPEND _targetsNotDefined ${_expectedTarget})
  endif()
  if(TARGET ${_expectedTarget})
    list(APPEND _targetsDefined ${_expectedTarget})
  endif()
endforeach()
if("${_targetsDefined}" STREQUAL "${_expectedTargets}")
  unset(_targetsDefined)
  unset(_targetsNotDefined)
  unset(_expectedTargets)
  set(CMAKE_IMPORT_FILE_VERSION)
  cmake_policy(POP)
  return()
endif()
if(NOT "${_targetsDefined}" STREQUAL "")
  message(FATAL_ERROR "Some (but not all) targets in this export set were already defined.\nTargets Defined: ${_targetsDefined}\nTargets not yet defined: ${_targetsNotDefined}\n")
endif()
unset(_targetsDefined)
unset(_targetsNotDefined)
unset(_expectedTargets)


# Compute the installation prefix relative to this file.
get_filename_component(_IMPORT_PREFIX "${CMAKE_CURRENT_LIST_FILE}" PATH)
get_filename_component(_IMPORT_PREFIX "${_IMPORT_PREFIX}" PATH)
get_filename_component(_IMPORT_PREFIX "${_IMPORT_PREFIX}" PATH)
if(_IMPORT_PREFIX STREQUAL "/")
  set(_IMPORT_PREFIX "")
endif()

# Create imported target upb::upb
add_library(upb::upb STATIC IMPORTED)

set_target_properties(upb::upb PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"
  INTERFACE_LINK_LIBRARIES "upb::fastdecode;upb::port"
)

# Create imported target upb::fastdecode
add_library(upb::fastdecode STATIC IMPORTED)

set_target_properties(upb::fastdecode PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::port;upb::table"
)

# Create imported target upb::upb_json
add_library(upb::upb_json STATIC IMPORTED)

set_target_properties(upb::upb_json PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::upb;upb::upb_pb"
)

# Create imported target upb::upb_pb
add_library(upb::upb_pb STATIC IMPORTED)

set_target_properties(upb::upb_pb PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::descriptor_upb_proto;upb::handlers;upb::port;upb::reflection;upb::table;upb::upb"
)

# Create imported target upb::port
add_library(upb::port INTERFACE IMPORTED)

# Create imported target upb::table
add_library(upb::table INTERFACE IMPORTED)

set_target_properties(upb::table PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::port"
)

# Create imported target upb::descriptor_upb_proto
add_library(upb::descriptor_upb_proto INTERFACE IMPORTED)

# Create imported target upb::handlers
add_library(upb::handlers STATIC IMPORTED)

set_target_properties(upb::handlers PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::port;upb::reflection;upb::table;upb::upb"
)

# Create imported target upb::reflection
add_library(upb::reflection STATIC IMPORTED)

set_target_properties(upb::reflection PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::descriptor_upb_proto;upb::port;upb::table;upb::upb"
)

# Create imported target upb::textformat
add_library(upb::textformat STATIC IMPORTED)

set_target_properties(upb::textformat PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::port;upb::reflection"
)

# Create imported target upb::all_libs
add_library(upb::all_libs INTERFACE IMPORTED)

set_target_properties(upb::all_libs PROPERTIES
  INTERFACE_LINK_LIBRARIES "upb::upb;upb::fastdecode;upb::upb_json;upb::upb_pb;upb::port;upb::table;upb::descriptor_upb_proto;upb::handlers;upb::reflection;upb::textformat"
)

if(CMAKE_VERSION VERSION_LESS 3.0.0)
  message(FATAL_ERROR "This file relies on consumers using CMake 3.0.0 or greater.")
endif()

# Load information for each installed configuration.
get_filename_component(_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
file(GLOB CONFIG_FILES "${_DIR}/upb-config-*.cmake")
foreach(f ${CONFIG_FILES})
  include(${f})
endforeach()

# Cleanup temporary variables.
set(_IMPORT_PREFIX)

# Loop over all imported files and verify that they actually exist
foreach(target ${_IMPORT_CHECK_TARGETS} )
  foreach(file ${_IMPORT_CHECK_FILES_FOR_${target}} )
    if(NOT EXISTS "${file}" )
      message(FATAL_ERROR "The imported target \"${target}\" references the file
   \"${file}\"
but this file does not exist.  Possible reasons include:
* The file was deleted, renamed, or moved to another location.
* An install or uninstall procedure did not complete successfully.
* The installation package was faulty and contained
   \"${CMAKE_CURRENT_LIST_FILE}\"
but not all the files it references.
")
    endif()
  endforeach()
  unset(_IMPORT_CHECK_FILES_FOR_${target})
endforeach()
unset(_IMPORT_CHECK_TARGETS)

# This file does not depend on other imported targets which have
# been exported from the same project but in a separate export set.

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
cmake_policy(POP)
