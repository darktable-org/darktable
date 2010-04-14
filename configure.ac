AC_INIT([darktable], [0.5+git], [hanatos@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign dist-bzip2])
AC_PREREQ(2.63)
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_HOST
AC_PROG_CC
AC_PROG_CXX
IT_PROG_INTLTOOL([0.40])
ALL_LINGUAS="fr nl ru sv"
AM_GLIB_GNU_GETTEXT
AM_GLIB_DEFINE_LOCALEDIR(DARKTABLE_LOCALEDIR)
GETTEXT_PACKAGE=darktable
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, ["$GETTEXT_PACKAGE"], [translation domain])

AC_PATH_PROG(GCONFTOOL, gconftool-2)
AM_GCONF_SOURCE_2

dnl *** Check for inotify ***
AC_CHECK_HEADER([sys/inotify.h],
	[AC_CHECK_FUNC([inotify_init],[have_inotify="yes"],[have_inotify="no"])],
	[have_inotify="no"])
AC_ARG_WITH([inotify],
	[AS_HELP_STRING([--with-inotify],[Enable inotify support (default=auto)])],
		[want_inotify=${withval}],
		[want_inotify=${have_inotify}])
	if test "x$want_inotify" = "xyes" ; then
		if test "x$have_inotify" = "xyes"; then
			AC_DEFINE(HAVE_INOTIFY, 1, [wheter to use inotify filesystem change events] )
		else
			AC_MSG_ERROR("inotify not found!")
	fi
fi 

# Enable silent build rules by default, requires at least
# Automake-1.11. Disable by either passing --disable-silent-rules to
# configure or passing V=1 to make
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AX_CC_MAXOPT
AC_MSG_CHECKING([whether to enable debugging])
AC_ARG_ENABLE(debug,
	      AC_HELP_STRING([--enable-debug], [build debug executable]))
if test "$enable_debug" = "yes"; then
   CFLAGS="-Wall -O0 -g -D_DEBUG"
   CXXFLAGS="-Wall -O0 -g -D_DEBUG"
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
	      AC_HELP_STRING([--enable-gegl], [use gegl for rendering (dont do this)]),
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
             AC_MSG_RESULT([no])
        ]
        )
AC_MSG_CHECKING([whether to use gconf to store settings])
AC_ARG_ENABLE(gconf,
	      AC_HELP_STRING([--enable-gconf], [use gconf to store settings (default under linux)]),
        [
          if test "$enable_gconf" = "yes"; then
             AC_DEFINE(HAVE_GCONF, 1, [whether to use gconf to store settings])
             AC_MSG_RESULT([yes])
             PKG_CHECK_MODULES(GCONF, gconf-2.0 >= 2.26)
             AM_CONDITIONAL(HAVE_GCONF, true)
          else
             AC_MSG_RESULT([no])
             AM_CONDITIONAL(HAVE_GCONF, false)
          fi
        ],
        [
             AC_DEFINE(HAVE_GCONF, 1, [whether to use gconf to store settings])
             AC_MSG_RESULT([yes])
             PKG_CHECK_MODULES(GCONF, gconf-2.0 >= 2.26)
             AM_CONDITIONAL(HAVE_GCONF, true)
        ]
        )
PKG_CHECK_MODULES(GTK, gtk+-2.0 >= 2.18)
PKG_CHECK_MODULES(CAIRO, [cairo])
PKG_CHECK_MODULES(GTHREAD, [gthread-2.0])
PKG_CHECK_MODULES(GLADE, [libglade-2.0])
PKG_CHECK_MODULES(SQLITE, [sqlite3])
PKG_CHECK_MODULES(EXIV2, exiv2 >= 0.18)
PKG_CHECK_MODULES(PNG, libpng >= 1.2)
PKG_CHECK_MODULES(OPENEXR, OpenEXR >= 1.6)
PKG_CHECK_MODULES(GPHOTO2, [libgphoto2 >= 2.4.5])

AC_ARG_ENABLE(lensfun,
	      AC_HELP_STRING([--enable-lensfun], [build the lens correction plugin]),
        [
          if test "$enable_lensfun" = "yes"; then
             PKG_CHECK_MODULES(LENSFUN, lensfun >= 0.2.3)
             AC_DEFINE(HAVE_LENSFUN, 1, [whether build the lensfun plugin])
          fi
          AM_CONDITIONAL([HAVE_LENSFUN], [test "$enable_lensfun" = yes])
        ],
        [
          PKG_CHECK_MODULES(LENSFUN, lensfun >= 0.2.3)
          AC_DEFINE(HAVE_LENSFUN, 1, [whether build the lensfun plugin])
          AM_CONDITIONAL([HAVE_LENSFUN], [true])
        ]
        )
 
AC_CHECK_LIB(jpeg, jpeg_destroy_decompress,
   [JPEG_LIBS="-ljpeg"], AC_MSG_FAILURE([libjpeg is required!]))
AC_SUBST(JPEG_LIBS)

AC_CHECK_LIB(tiff, TIFFOpen,
   [TIFF_LIBS="-ltiff"], AC_MSG_FAILURE([libtiff is required!]))
AC_SUBST(TIFF_LIBS)


dnl *** Check for LittleCMS ***
AC_CHECK_LIB(lcms, cmsOpenProfileFromFile,
   [AC_DEFINE(HAVE_LCMS, 1, [whether the LittleCMS development environment is available])
    LCMS_LIBS="-llcms"], AC_MSG_FAILURE([lcms is required!]))
AC_SUBST(LCMS_LIBS)

dnl *** mac os troubles ***
case $host_os in
  *mingw* )
    win32=yes
    darwin=no
    ;;
  *darwin* )
    win32=no
    darwin=yes
    ;;
  * )
    win32=no
    darwin=no
    ;;
esac

dnl *** Check for the Apple Carbon framework (needed for GDK_WINDOWING_QUARTZ). 
CARBON_LIBS=
if test $darwin = yes; then
  carbon_ok=no
  AC_MSG_CHECKING([for Mac OS X Carbon support])
  AC_TRY_CPP([
  #include <Carbon/Carbon.h>
  #include <ApplicationServices/ApplicationServices.h>
  #include <CoreServices/CoreServices.h>
  ], carbon_ok=yes)
  AC_MSG_RESULT($carbon_ok)
  if test "x$carbon_ok" = "xyes"; then
    CARBON_LIBS="-framework Carbon -framework Cocoa"
  fi
fi
AC_SUBST(CARBON_LIBS)



AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
    Makefile
    po/Makefile.in
    src/Makefile
    src/LibRaw/Makefile
    src/iop/Makefile
    src/views/Makefile
    src/libs/Makefile
    pixmaps/Makefile
    pixmaps/plugins/Makefile
    pixmaps/plugins/darkroom/Makefile
    color/Makefile
    color/in/Makefile
    color/out/Makefile
    ])
dnl    src/gegl-operations/Makefile
AC_OUTPUT
