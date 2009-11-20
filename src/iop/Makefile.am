include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/.. -Werror -g
LTADD=$(GEGL_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) -fPIC
libtonecurve_la_SOURCES=tonecurve.c
libgamma_la_SOURCES=gamma.c
libtemperature_la_SOURCES=temperature.c
libcolorcorrection_la_SOURCES=colorcorrection.c
libexposure_la_SOURCES=exposure.c
libdenoise_la_SOURCES=denoise.c
libequalizer_la_SOURCES=equalizer.c
libcolorin_la_SOURCES=colorin.c
libcolorout_la_SOURCES=colorout.c
libclipping_la_SOURCES=clipping.c
libsharpen_la_SOURCES=sharpen.c
noinst_HEADERS=tonecurve.h gamma.h temperature.h colorcorrection.h exposure.h denoise.h equalizer.h equalizer_eaw.h colorin.h colorout.h lens.h
dtpluginsdir=$(datarootdir)/darktable/plugins
dtplugins_LTLIBRARIES=libtonecurve.la libgamma.la libtemperature.la libcolorcorrection.la libexposure.la libdenoise.la libequalizer.la libcolorin.la libcolorout.la libclipping.la libsharpen.la
if HAVE_LENSFUN
  liblens_la_SOURCES=lens.c
  liblens_la_CFLAGS=$(LENSFUN_CFLAGS)
  liblens_la_LIBADD=$(LENSFUN_LIBS)
  dtplugins_LTLIBRARIES+=liblens.la
endif
