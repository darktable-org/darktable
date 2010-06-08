include $(srcdir)/../../shared.am
LDFLAGS= -module -avoid-version
CFLAGS+=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/../.. -Werror -g -rdynamic
CXXFLAGS+=$(AM_CXXFLAGS) $(OPENEXR_CFLAGS) -I$(srcdir) -I$(srcdir)/../.. -Werror -g -rdynamic
LTADD=$(OPENMP_CFLAGS) $(GEGL_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) -fPIC
libjpeg_la_SOURCES=jpeg.c
libpng_la_SOURCES=png.c
libppm_la_SOURCES=ppm.c
libexr_la_SOURCES=exr.cc
libexr_la_LIBADD=$(OPENEXR_LIBS)
dtimageioformatpluginsdir=$(libdir)/darktable/plugins/imageio/format
dtimageioformatplugins_LTLIBRARIES=libjpeg.la libpng.la libexr.la libppm.la
