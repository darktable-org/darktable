include(CheckCompilerFlagAndEnableIt)
include(CheckCCompilerFlagAndEnableIt)
include(CheckCXXCompilerFlagAndEnableIt)

CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wall)

if(WIN32)
  # MSYS2 gcc compiler gives false positive warnings for (format (printf, 1, 2) - need to turn off for the time being
  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-format)
else()
  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wformat)
  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wformat-security)
endif()

CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wshadow)

CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wtype-limits)

CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wvla)

CHECK_C_COMPILER_FLAG_AND_ENABLE_IT(-Wold-style-declaration)

CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wthread-safety)

CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wmaybe-uninitialized)

# since checking if defined(__GNUC__) is not enough to prevent Clang from using GCC-specific pragmas
# (so Clang defines __GNUC__ ???) we need to disable the warnings about unknown pragmas
CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-unknown-pragmas)

# may be our bug :(
CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-error=varargs)

# need proper gcc7 to try to fix all the warnings.
# so just disable for now.
CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-format-truncation)

# clang-4.0 bug https://llvm.org/bugs/show_bug.cgi?id=28115#c7
CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-error=address-of-packed-member)

# minimal main thread's stack/frame stack size.
# 2 MiB seems to work.
# 1 MiB does NOT work with gmic support enabled.
# MUST be a multiple of the system page size !!!
# see  $ getconf PAGESIZE
math(EXPR WANTED_STACK_SIZE 512*4*1024)

# minimal pthread stack/frame stack size.
# 2 MiB seems to work and is default on Linux.
# 1 MiB does NOT work with gmic support enabled.
# MUST be a multiple of the system page size !!!
# see  $ getconf PAGESIZE
math(EXPR WANTED_THREADS_STACK_SIZE 512*4*1024)

if(SOURCE_PACKAGE)
  add_definitions(-D_RELEASE)
endif()

###### GTK+3 ######
#
#  Do not include individual headers
#
add_definitions(-DGTK_DISABLE_SINGLE_INCLUDES)

#
# Dirty hack to enforce GTK3 behaviour in GTK2: "Replace GDK_<keyname> with GDK_KEY_<keyname>"
#
add_definitions(-D__GDK_KEYSYMS_COMPAT_H__)

#
#  Do not use deprecated symbols
#
add_definitions(-DGDK_DISABLE_DEPRECATED)
add_definitions(-DGTK_DISABLE_DEPRECATED)
###### GTK+3 port ######
