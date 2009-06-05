include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/..
LTADD=$(GEGL_LIBS) $(LIBEXIF_LIBS) $(MAGICK_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) libdarktable.a LibRaw/libraw_r.a -lstdc++
libtonecurve_la_SOURCES=tonecurve.c
# libtonecurve_la_LDFLAGS= -module -avoid-version
libgamma_la_SOURCES=gamma.c
# libgamma_la_LDFLAGS= -module -avoid-version
# libtonecurve_la_LTADD=$(GEGL_LIBS) $(LIBEXIF_LIBS) $(MAGICK_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) libdarktable.a LibRaw/libraw_r.a -lstdc++
# libtonecurve_la_CFLAGS=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/..
noinst_HEADERS=tonecurve.h gamma.h
dtpluginsdir=$(datarootdir)/darktable/plugins
dtplugins_LTLIBRARIES=libtonecurve.la libgamma.la
