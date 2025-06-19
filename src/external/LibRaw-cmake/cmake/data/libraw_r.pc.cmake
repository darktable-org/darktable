prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${exec_prefix}/@CMAKE_INSTALL_LIBDIR@
includedir=${prefix}/@CMAKE_INSTALL_INCLUDEDIR@

Name: @PROJECT_NAME@
Description: Raw image decoder library (thread-safe)
URL: http://www.libraw.org
Requires:
Version: @RAW_LIB_VERSION_STRING@
Libs: -L${libdir} -lraw_r
Cflags: -I${includedir} -I${includedir}/libraw
Libs.private: @LIBRAW_PC_LIBS_PRIVATE@
Requires.private: @LIBRAW_PC_REQUIRES_PRIVATE@
