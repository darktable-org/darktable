AC_INIT([darktable], [0.2_alpha], [hanatos@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign dist-bzip2])
AC_PREREQ(2.63)
AC_PROG_CC
dnl AC_PROG_CC_C99
dnl if test "$ac_cv_prog_cc_c99" = no; then
dnl   AC_MSG_FAILURE([Your c compiler does not support c99!])
dnl fi
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
AC_OPENMP
AC_PROG_RANLIB
PKG_CHECK_MODULES(GTK, gtk+-2.0 >= 2.14)
PKG_CHECK_MODULES(CAIRO, [cairo])
PKG_CHECK_MODULES(GTHREAD, [gthread-2.0])
PKG_CHECK_MODULES(GLADE, [libglade-2.0])
PKG_CHECK_MODULES(SQLITE, [sqlite3])
PKG_CHECK_MODULES(LIBEXIF, [libexif])
PKG_CHECK_MODULES(GEGL, gegl >= 0.0.22)
PKG_CHECK_MODULES(MAGICK, MagickCore >= 6.4)
 
dnl *** check for MagickCore ***
dnl ** AC_CHECK_PROGS(MAGICKCONFIG, [MagickCore-config])
dnl ** if test -n "$MAGICKCONFIG"; then
dnl **    ac_save_CFLAGS="$CFLAGS"
dnl **    ac_save_LIBS="$LIBS"
dnl **    MAGICK_CFLAGS="`$MAGICKCONFIG --cflags`"
dnl **    MAGICK_LIBS="`$MAGICKCONFIG --libs`"
dnl **    CFLAGS="$CFLAGS $MAGICK_CFLAGS"
dnl **    LIBS="$MAGICK_LIBS $LIBS"
dnl **    AC_CHECK_FUNC(ExportImagePixels,
dnl **         AC_DEFINE(HAVE_MAGICK, 1, [Whether the ImageMagick library is available]),
dnl **         AC_MSG_NOTICE([ImageMagick does not support the function
dnl **         ExportImagePixels. Please upgrade to ImageMagick 6.4 or newer]))
dnl **    CFLAGS="$ac_save_CFLAGS"
dnl **    LIBS="$ac_save_LIBS"
dnl ** fi
dnl **  
dnl ** AC_SUBST(MAGICK_CFLAGS)
dnl ** AC_SUBST(MAGICK_LIBS)

dnl *** Check for LittleCMS ***
AC_CHECK_LIB(lcms, cmsOpenProfileFromFile,
   [AC_DEFINE(HAVE_LCMS, 1, [Define if you have the LittleCMS development environment])
    LCMS_LIBS="-llcms"])
AC_SUBST(LCMS_LIBS)

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
    Makefile
    src/Makefile
    src/LibRaw/Makefile
    ])
AC_OUTPUT
