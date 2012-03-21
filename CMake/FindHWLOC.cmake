# Copyright (c) 2012 Marwan Abdellah <marwan.abdellah@epfl.ch>

find_package(PkgConfig)
if (PKG_CONFIG_FOUND)
  message(STATUS "Found PKG_CONFIG")
else()
  message(STATUS "Could NOT Find PKG_CONFIG")
endif()

# Search for package files 
pkg_search_module(HWLOC hwloc>=1.4.0)

# Other places via "ccmake .."
if(NOT HWLOC_FOUND)
  find_path(HWLOC_INCLUDE_DIR "hwloc.h")
  find_library(HWLOC_LIBRARIES NAMES hwloc)
  
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(HWLOC DEFAULT_MSG HWLOC_LIBRARIES HWLOC_INCLUDE_DIR)
endif()

if(HWLOC_FOUND)
  message(STATUS "Found HWLOC in ${HWLOC_INCLUDE_DIR};${HWLOC_LIBRARIES}")
endif(HWLOC_FOUND)