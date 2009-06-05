# if OS_WIN32
# no_undefined = -no-undefined
# libgegl = $(top_builddir)/gegl/libgegl-$(GEGL_API_VERSION).la @BABL_LIBS@
# endif

# op_libs = @DEP_LIBS@ $(libgegl)

GEGLHEADERS     = $(wildcard $(top_srcdir)/gegl/*.h)\
                  $(wildcard $(top_srcdir)/gegl/buffer/*.h)

AM_CPPFLAGS = \
  @GEGL_CFLAGS@       \
 	-I$(srcdir)				  \
  -std=c99

# 	@DEP_CFLAGS@				\
# 	@BABL_CFLAGS@				\
# 	@CPPFLAGS@				  \
# 	-I$(top_srcdir)				\
# 	-I$(top_builddir)			\
# 	-I$(top_srcdir)/gegl			\
# 	-I$(top_srcdir)/gegl/buffer		\
# 	-I$(top_srcdir)/gegl/operation		\
# 	-I$(top_srcdir)/gegl/property-types	\
# 	-I$(top_srcdir)/gegl/module

AM_LDFLAGS = -avoid-version -export-dynamic -module $(no_undefined)
# TODO: find right directory how?
# ext_dir = $(DESTDIR)$(libdir)/gegl-@GEGL_API_VERSION@
ext_dir = $(libdir)/gegl-0.0.22

# LDADD           = $(op_libs)
LDADD           = @GEGL_LIBS@

CFILES          = $(wildcard $(srcdir)/*.c)
plugins           = $(subst $(srcdir)/,,$(CFILES:.c=.la))
INSTALLED_ITEMS = $(subst $(srcdir)/,$(ext_dir)/,$(CFILES:.c=$(SHREXT)))
EXTRA_DIST      = $(wildcard *.c) $(wildcard *.h)

all-local: $(plugins)

%.la: %.c $(GEGLHEADERS)
	$(LIBTOOL) --tag=CC --mode=compile $(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c -o $@.lo $<
	$(LIBTOOL) --tag=CC --mode=link $(CC) $(AM_LDFLAGS) $(LDFLAGS) $(CFLAGS) $(LDADD) -o $@ -rpath $(ext_dir) $@.lo


clean-local:
	rm -f *.la $(OFILES)

install-exec-local: all-local
	$(INSTALL) -d $(ext_dir)
	for i in $(plugins); do \
	  $(LIBTOOL) --mode=install $(INSTALL) $$i $(ext_dir) ; \
	done

uninstall-local:
	for i in $(plugins); do \
	  $(LIBTOOL) --mode=uninstall $(RM) $(ext_dir)/$$i ; \
	done

check-local: all-local
