include(CheckCXXCompilerFlag)

macro (CHECK_CXX_COMPILER_FLAG_AND_ENABLE_IT _FLAG)
  set(_RESULT "CXX_COMPILER_UNDERSTANDS_${_FLAG}")

  CHECK_CXX_COMPILER_FLAG("${_FLAG}" ${_RESULT})

  if(${${_RESULT}})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_FLAG}")
  endif()
endmacro ()
