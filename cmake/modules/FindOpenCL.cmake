# - Try to find OpenCL
# Once done this will define
#  
#  OPENCL_FOUND        - system has OpenCL
#  OPENCL_INCLUDE_DIR  - the OpenCL include directory
#  OPENCL_LIBRARIES    - link these to use OpenCL
#
# WIN32 should work, but is untested

IF (WIN32)

    FIND_PATH(OPENCL_INCLUDE_DIR CL/cl.h )

    # TODO this is only a hack assuming the 64 bit library will
    # not be found on 32 bit system
    FIND_LIBRARY(OPENCL_LIBRARIES opencl64 )
    IF( OPENCL_LIBRARIES )
        FIND_LIBRARY(OPENCL_LIBRARIES opencl32 )
    ENDIF( OPENCL_LIBRARIES )

ELSE (WIN32)

    # Unix style platforms
    FIND_PATH(OPENCL_INCLUDE_DIR CL/opencl.h )
    FIND_LIBRARY(OPENCL_LIBRARIES OpenCL ENV LD_LIBRARY_PATH)

ENDIF (WIN32)

SET( OPENCL_FOUND "NO" )
IF(OPENCL_LIBRARIES AND OPENCL_INCLUDE_DIR)
    SET( OPENCL_FOUND "YES" )
ENDIF(OPENCL_LIBRARIES AND OPENCL_INCLUDE_DIR)

MARK_AS_ADVANCED(
  OPENCL_INCLUDE_DIR
)
