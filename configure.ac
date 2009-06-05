AC_INIT([darktable], [0.2_alpha], [hanatos@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign dist-bzip2])
AC_PREREQ(2.63)
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
dnl ***  disabled because of strange libgomp.so crashes: :(
dnl AC_OPENMP
AC_PROG_LIBTOOL
PKG_CHECK_MODULES(GTK, gtk+-2.0 >= 2.14)
PKG_CHECK_MODULES(CAIRO, [cairo])
PKG_CHECK_MODULES(GTHREAD, [gthread-2.0])
PKG_CHECK_MODULES(GLADE, [libglade-2.0])
PKG_CHECK_MODULES(SQLITE, [sqlite3])
PKG_CHECK_MODULES(LIBEXIF, [libexif])
PKG_CHECK_MODULES(GEGL, gegl >= 0.0.22)
PKG_CHECK_MODULES(MAGICK, MagickCore >= 6.4)
AC_DEFINE(HAVE_MAGICK, 1, [whether the ImageMagick library is available])
 
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
    src/gegl-operations/Makefile
    ])
AC_OUTPUT
