
#  For more information, please see: http://software.sci.utah.edu
#
#  The MIT License
#
#  Copyright (c) 2007
#  Scientific Computing and Imaging Institute, University of Utah
#
#  License for the specific language governing rights and limitations under
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  the rights to use, copy, modify, merge, publish, distribute, sublicense,
#  and/or sell copies of the Software, and to permit persons to whom the
#  Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included
#  in all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
#  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
#  DEALINGS IN THE SOFTWARE.

# This code is based on the Manta swig/python wrapper dependency checking code.
# -- Abe Stephens

#####################################################################
## CUDA_INCLUDE_NVCC_DEPENDENCIES
##

# So we want to try and include the dependency file if it exists.  If
# it doesn't exist then we need to create an empty one, so we can
# include it.

# If it does exist, then we need to check to see if all the files it
# depends on exist.  If they don't then we should clear the dependency
# file and regenerate it later.  This covers the case where a header
# file has disappeared or moved.

MACRO(CUDA_INCLUDE_NVCC_DEPENDENCIES dependency_file)
  SET(CUDA_NVCC_DEPEND)
  SET(CUDA_NVCC_DEPEND_REGENERATE)

  # Include the dependency file.  Create it first if it doesn't exist
  # for make files except for IDEs (see below).  The INCLUDE puts a
  # dependency that will force CMake to rerun and bring in the new info
  # when it changes.  DO NOT REMOVE THIS (as I did and spent a few hours
  # figuring out why it didn't work.
  IF(${CMAKE_MAKE_PROGRAM} MATCHES "make")
    IF(NOT EXISTS ${dependency_file})
      CONFIGURE_FILE(
        ${CMAKE_SOURCE_DIR}/CMake/cuda/empty.depend.in
        ${dependency_file} IMMEDIATE)
    ENDIF(NOT EXISTS ${dependency_file})
    # Always include this file to force CMake to run again next
    # invocation and rebuild the dependencies.
    INCLUDE(${dependency_file})
  ELSE(${CMAKE_MAKE_PROGRAM} MATCHES "make")
    # for IDE generators like MS dev only include the depend files
    # if they exist.   This is to prevent ecessive reloading of
    # workspaces after each build.   This also means
    # that the depends will not be correct until cmake
    # is run once after the build has completed once.
    # the depend files are created in the wrap tcl/python sections
    # when the .xml file is parsed.
    INCLUDE(${dependency_file} OPTIONAL)
  ENDIF(${CMAKE_MAKE_PROGRAM} MATCHES "make")

  # Now we need to verify the existence of all the included files
  # here.  If they aren't there we need to just blank this variable and
  # make the file regenerate again.
  IF(CUDA_NVCC_DEPEND)
    FOREACH(f ${CUDA_NVCC_DEPEND})
      IF(EXISTS ${f})
      ELSE(EXISTS ${f})
        SET(CUDA_NVCC_DEPEND_REGENERATE 1)
      ENDIF(EXISTS ${f})
    ENDFOREACH(f)
  ELSE(CUDA_NVCC_DEPEND)
    # No dependencies, so regenerate the file.
    SET(CABLE_NVCC_DEPEND_REGENERATE 1)
  ENDIF(CUDA_NVCC_DEPEND)

  # No incoming dependencies, so we need to generate them.  Make the
  # output depend on the dependency file itself, which should cause the
  # rule to re-run.
  IF(CUDA_NVCC_DEPEND_REGENERATE)
    SET(CUDA_NVCC_DEPEND ${dependency_file})
    # Force CMake to run again next build
    CONFIGURE_FILE(
      ${CMAKE_SOURCE_DIR}/CMake/cuda/empty.depend.in
      ${dependency_file} IMMEDIATE)
  ENDIF(CUDA_NVCC_DEPEND_REGENERATE)
      
ENDMACRO(CUDA_INCLUDE_NVCC_DEPENDENCIES)