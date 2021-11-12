prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${prefix}/lib@LIB_SUFFIX@
includedir=${prefix}/include/libraw

Name: @PROJECT_NAME@
Description: @PROJECT_NAME@ - Raw image decoder library (thread-safe)
URL: http://www.libraw.org
Requires:
Version: @RAW_LIB_VERSION_STRING@
Libs: -L${libdir} -lraw_r
Cflags: -I${includedir}
