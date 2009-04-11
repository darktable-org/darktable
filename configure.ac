AC_INIT([darktable], [0.1-svn], [hanatos@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign dist-bzip2])
AC_PROG_CC
dnl AC_PROG_CC_C99
dnl if test "$ac_cv_prog_cc_c99" = no; then
dnl   AC_MSG_FAILURE([Your c compiler does not support c99!])
dnl fi
AX_CC_MAXOPT
AC_PROG_CXX
AC_PROG_RANLIB
AC_OPENMP
PKG_CHECK_MODULES(GTK, gtk+-2.0 >= 2.14)
PKG_CHECK_MODULES(CAIRO, [cairo])
PKG_CHECK_MODULES(GTHREAD, [gthread-2.0])
PKG_CHECK_MODULES(GLADE, [libglade-2.0])
PKG_CHECK_MODULES(SQLITE, [sqlite3])
 
dnl *** check for MagickCore ***
AC_CHECK_PROGS(MAGICKCONFIG, [MagickCore-config])
if test -n "$MAGICKCONFIG"; then
   ac_save_CPPFLAGS="$CPPFLAGS"
   ac_save_LIBS="$LIBS"
   MAGICK_CPPFLAGS="`$MAGICKCONFIG --cppflags`"
   MAGICK_LIBS="`$MAGICKCONFIG --ldflags` `$MAGICKCONFIG --libs`"
   CPPFLAGS="$CPPFLAGS $MAGICK_CPPFLAGS"
   LIBS="$MAGICK_LIBS $LIBS"
   AC_CHECK_FUNC(ExportImagePixels,
        AC_DEFINE(HAVE_MAGICK, 1, [Whether the ImageMagick library is available]),
        AC_MSG_NOTICE([ImageMagick does not support the function
        ExportImagePixels. Please upgrade to ImageMagick 6.4 or newer]))
   CPPFLAGS="$ac_save_CPPFLAGS"
   LIBS="$ac_save_LIBS"
fi
 
AC_SUBST(MAGICK_CPPFLAGS)
AC_SUBST(MAGICK_LIBS)

dnl *** Check for LittleCMS ***
AC_CHECK_LIB(lcms, cmsOpenProfileFromFile,
   [AC_DEFINE(HAVE_LCMS, 1, [Define if you have the LittleCMS development environment])
    LCMS_LIBS="-llcms"])
AC_SUBST(LCMS_LIBS)

dnl *** check for lamer keyboard *** 
AC_ARG_ENABLE(qwerty, [  --enable-qwerty            Enable QWERTY key accels], [if test "$enableval" = yes; then 
    AC_DEFINE(HAVE_QWERTY, 1, [Use to switch from default dvorak key accels to qwerty])
  fi 
  ])

AC_MSG_CHECKING([whether to enable debugging])
AC_ARG_ENABLE(debug,
	      AC_HELP_STRING([--enable-debug], [build debug executable]))
if test "$enable_debug" = "yes"; then
   CFLAGS="$CFLAGS -Wall -O0 -g -D_DEBUG"
   AC_MSG_RESULT([yes])
else
   CFLAGS="$CFLAGS -Wall -O4"
   AC_MSG_RESULT([no])
fi

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
    Makefile
    src/Makefile
    src/LibRaw/Makefile
    ])
AC_OUTPUT
