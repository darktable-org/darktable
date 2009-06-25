include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/..
LTADD=$(GEGL_LIBS) $(LIBEXIF_LIBS) $(MAGICK_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) libdarktable.a LibRaw/libraw_r.a -lstdc++
libtonecurve_la_SOURCES=tonecurve.c
libgamma_la_SOURCES=gamma.c
libtemperature_la_SOURCES=temperature.c
libcolorcorrection_la_SOURCES=colorcorrection.c
noinst_HEADERS=tonecurve.h gamma.h temperature.h colorcorrection.h
dtpluginsdir=$(datarootdir)/darktable/plugins
dtplugins_LTLIBRARIES=libtonecurve.la libgamma.la libtemperature.la libcolorcorrection.la
