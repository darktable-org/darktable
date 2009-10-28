AC_INIT([darktable], [git], [hanatos@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign dist-bzip2])
AC_PREREQ(2.63)
AC_CONFIG_MACRO_DIR([m4])
AC_PROG_CC
AC_PROG_CXX
AX_CC_MAXOPT
AC_MSG_CHECKING([whether to enable debugging])
AC_ARG_ENABLE(debug,
	      AC_HELP_STRING([--enable-debug], [build debug executable]))
if test "$enable_debug" = "yes"; then
   CFLAGS="-Wall -O0 -g -D_DEBUG"
   AC_MSG_RESULT([yes])
else
   AC_MSG_RESULT([no])
fi
dnl ***  disabled because of strange libgomp.so crashes for gcc < 4.4: :(
AX_GCC_VERSION([4], [4], [0], try_openmp="yes", try_openmp="no")
AC_ARG_ENABLE(openmp,
	      AC_HELP_STRING([--enable-openmp], [use multiple threads with openmp]))
if test "$enable_openmp" = "no"; then
   try_openmp="no"
   AC_MSG_RESULT([no])
else
   AC_MSG_RESULT([yes])
fi
if test "$try_openmp" = "yes"; then
    AC_OPENMP
fi
AC_PROG_LIBTOOL
PKG_PROG_PKG_CONFIG([0.22])
AC_MSG_CHECKING([whether to use gegl for rendering])
AC_ARG_ENABLE(gegl,
	      AC_HELP_STRING([--enable-gegl], [use gegl for rendering (don't do this)]),
        [
          if test "$enable_gegl" = "yes"; then
             AC_DEFINE(HAVE_GEGL, 1, [whether to use gegl for rendering])
             AC_MSG_RESULT([yes])
             PKG_CHECK_MODULES(GEGL, gegl >= 0.0.22)
          else
             AC_MSG_RESULT([no])
          fi
        ],
        [
             AC_DEFINE(USE_GEGL_CURVE, 1, [whether to use gegl for rendering])
             AC_MSG_RESULT([only for curve])
             PKG_CHECK_MODULES(GEGL, gegl >= 0.0.22)
        ]
        )
PKG_CHECK_MODULES(GTK, gtk+-2.0 >= 2.14)
PKG_CHECK_MODULES(CAIRO, [cairo])
PKG_CHECK_MODULES(GTHREAD, [gthread-2.0])
PKG_CHECK_MODULES(GLADE, [libglade-2.0])
PKG_CHECK_MODULES(SQLITE, [sqlite3])
PKG_CHECK_MODULES(EXIV2, exiv2 >= 0.18)
PKG_CHECK_MODULES(PNG, libpng12)
 
AC_CHECK_LIB(jpeg, jpeg_destroy_decompress,
   [JPEG_LIBS="-ljpeg"])
AC_SUBST(JPEG_LIBS)

dnl *** Check for LittleCMS ***
AC_CHECK_LIB(lcms, cmsOpenProfileFromFile,
   [AC_DEFINE(HAVE_LCMS, 1, [whether the LittleCMS development environment is available])
    LCMS_LIBS="-llcms"])
AC_SUBST(LCMS_LIBS)

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
    Makefile
    src/Makefile
    src/LibRaw/Makefile
    src/iop/Makefile
    src/views/Makefile
    pixmaps/Makefile
    ])
dnl    src/gegl-operations/Makefile
AC_OUTPUT
