prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${prefix}/lib@LIB_SUFFIX@
includedir=${prefix}/include/libraw

Name: @PROJECT_NAME@
Description: @PROJECT_NAME@ - Raw image decoder library (non-thread-safe)
URL: http://www.libraw.org
Requires:
Version: @RAW_LIB_VERSION_STRING@
Libs: -L${libdir} -lraw
Cflags: -I${includedir}
