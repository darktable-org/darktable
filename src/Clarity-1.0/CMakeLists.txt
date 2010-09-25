CMAKE_MINIMUM_REQUIRED(VERSION 2.4)
PROJECT(Clarity)

OPTION(BUILD_SHARED_LIBS "Build Clarity with shared libraries." ON)
OPTION(BUILD_WITH_CUDA "Build with CUDA FFT acceleration." OFF)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake/")

IF(BUILD_WITH_CUDA)
  # Load CUDA macros
  INCLUDE(${CMAKE_SOURCE_DIR}/CMake/cuda/FindCuda.cmake)
ENDIF(BUILD_WITH_CUDA)

FIND_PACKAGE(FFTW)

SET(Clarity_MAJOR_VERSION "1")
SET(Clarity_MINOR_VERSION "0")
SET(Clarity_BUILD_VERSION "0")

SET( Clarity_VERSION_FLAT "${Clarity_MAJOR_VERSION}${Clarity_MINOR_VERSION}${Clarity_BUILD_VERSION}" )
SET( Clarity_VERSION "${Clarity_MAJOR_VERSION}.${Clarity_MINOR_VERSION}.${Clarity_BUILD_VERSION}" )

INCLUDE_DIRECTORIES(${Clarity_BINARY_DIR})
INCLUDE_DIRECTORIES(${Clarity_SOURCE_DIR} 
  ${Clarity_SOURCE_DIR}/include
  ${Clarity_SOURCE_DIR}/include/internal
  ${FFTW_INCLUDE_DIR}
  ${CUDA_INCLUDE}
  )

LINK_LIBRARIES(${FFTW_LIBRARIES})

SET( Clarity_SRCS
  src/BlindMaximumLikelihoodDeconvolve.cxx
  src/Clarity.cxx
  src/ComputePrimitives.cxx
  src/ComputePrimitivesGPU.cu
  src/Convolve.cxx
  src/FFT.cxx
  src/FFTGPU.cu
  src/IDivergenceDeconvolve.cxx
  src/ImageClip.cxx
  src/ImagePadSpatialShift.cxx
  src/JansenVanCittertDeconvolve.cxx
  src/JansenVanCittertDeconvolveGPU.cu
  src/Memory.cxx
  src/MaximumLikelihoodDeconvolve.cxx
  src/SmoothedJansenVanCittertDeconvolve.cxx
  src/Stopwatch.cxx
  src/WienerDeconvolve.cxx
  src/WienerDeconvolveGPU.cu
  )
  
CONFIGURE_FILE(
  ${Clarity_SOURCE_DIR}/Clarity.h.in
  ${Clarity_BINARY_DIR}/Clarity.h
)

# Enable linking with OpenMP when compiling with gcc
IF (${CMAKE_COMPILER_IS_GNUCXX})
  LINK_LIBRARIES(gomp)
ENDIF (${CMAKE_COMPILER_IS_GNUCXX})

IF(BUILD_WITH_CUDA)
  CUDA_INCLUDE_DIRECTORIES(${Clarity_SOURCE_DIR}/include)
  CUDA_INCLUDE_DIRECTORIES(${Clarity_SOURCE_DIR}/include/internal)
  CUDA_ADD_LIBRARY( Clarity ${Clarity_SRCS} ${FOUND_CUFFT})
ELSE(BUILD_WITH_CUDA)
  INCLUDE_DIRECTORIES(${Clarity_SOURCE_DIR}/include)
  ADD_LIBRARY( Clarity ${Clarity_SRCS})
ENDIF(BUILD_WITH_CUDA)

SET_TARGET_PROPERTIES(Clarity PROPERTIES 
  VERSION ${Clarity_VERSION}
  SOVERSION ${Clarity_MAJOR_VERSION}.${Clarity_MINOR_VERSION}
)

ADD_SUBDIRECTORY(tests)
