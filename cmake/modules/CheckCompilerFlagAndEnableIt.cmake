include(CheckCCompilerFlagAndEnableIt)
include(CheckCXXCompilerFlagAndEnableIt)

macro (CHECK_COMPILER_FLAG_AND_ENABLE_IT _FLAG)
  CHECK_C_COMPILER_FLAG_AND_ENABLE_IT(${_FLAG})
  CHECK_CXX_COMPILER_FLAG_AND_ENABLE_IT(${_FLAG})
endmacro ()
