# No longer needed as we have OpenCL headers in our source tree. OpenCL runtime
# library is searched and loaded at runtime.
#
# This file only kept as reference
#
#set(ENV_OPENCL_DIR $ENV{OPENCL_DIR})
#if(ENV_OPENCL_DIR)
#  find_path(
#        OPENCL_INCLUDE_DIR
#        NAMES CL/cl.h
#        PATHS $ENV{OPENCL_DIR}/include
#        NO_DEFAULT_PATH
#        )
#
#  if("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
#        if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
#          set(
#                OPENCL_LIB_SEARCH_PATH
#                ${OPENCL_LIB_SEARCH_PATH}
#                $ENV{OPENCL_DIR}/lib/x86_64
#                )
#        elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
#          set(
#                OPENCL_LIB_SEARCH_PATH
#                ${OPENCL_LIB_SEARCH_PATH}
#                $ENV{OPENCL_DIR}/lib/x86
#                )
#        endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
#  endif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
#  find_library(
#        OPENCL_LIBRARY
#        NAMES OpenCL
#        PATHS ${OPENCL_LIB_SEARCH_PATH}
#        NO_DEFAULT_PATH
#        )
#else(ENV_OPENCL_DIR)
#  find_path(
#        OPENCL_INCLUDE_DIR
#        NAMES CL/cl.h
#        )
#
#  find_library(
#        OPENCL_LIBRARY
#        NAMES OpenCL
#        )
#endif(ENV_OPENCL_DIR)
#
#include(FindPackageHandleStandardArgs)
#find_package_handle_standard_args(
#  OPENCL
#  DEFAULT_MSG
#  OPENCL_INCLUDE_DIR
#  )
#
#if(OPENCL_FOUND)
#  set(OPENCL_LIBRARIES ${OPENCL_LIBRARY})
#else(OPENCL_FOUND)
#  set(OPENCL_LIBRARIES)
#endif(OPENCL_FOUND)
#
#mark_as_advanced(
#  OPENCL_INCLUDE_DIR
#  OPENCL_LIBRARY
#  )
