include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS+=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/..  -I$(srcdir)/dtgtk -Werror -g $(RSVG_CFLAGS)
CXXFLAGS+=$(AM_CPPFLAGS) -I$(srcdir) -I$(srcdir)/..  -I$(srcdir)/dtgtk -Werror -g
LTADD=$(GEGL_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) -fPIC
libtonecurve_la_SOURCES=tonecurve.c
libgamma_la_SOURCES=gamma.c
libtemperature_la_SOURCES=temperature.c
libtemperature_la_CFLAGS=-I$(srcdir)/../LibRaw
libcolorcorrection_la_SOURCES=colorcorrection.c
libexposure_la_SOURCES=exposure.c
libexposure_la_CFLAGS=-I$(srcdir)/../LibRaw
libequalizer_la_SOURCES=equalizer.c
libcolorin_la_SOURCES=colorin.c
libcolorin_la_CFLAGS=-I$(srcdir)/../LibRaw
libcolorout_la_SOURCES=colorout.c
libclipping_la_SOURCES=clipping.c
libsharpen_la_SOURCES=sharpen.c
libmonochrome_la_SOURCES=monochrome.c
librawimport_la_SOURCES=rawimport.c
libcolorzones_la_SOURCES=colorzones.c
libbasecurve_la_SOURCES=basecurve.c
libhighlights_la_SOURCES=highlights.c
libvelvia_la_SOURCES=velvia.c
libvignette_la_SOURCES=vignette.c
libsplittoning_la_SOURCES=splittoning.c
libgrain_la_SOURCES=grain.c
libbilateral_la_SOURCES=bilateral.cc
libprofile_gamma_la_SOURCES=profile_gamma.c
libprofile_gamma_la_CFLAGS=-I$(srcdir)/../LibRaw
libcolortransfer_la_SOURCES=colortransfer.c
libchannelmixer_la_SOURCES=channelmixer.c
libgraduatednd_la_SOURCES=graduatednd.c
librelight_la_SOURCES=relight.c
libzonesystem_la_SOURCES=zonesystem.c
noinst_HEADERS=tonecurve.h gamma.h temperature.h colorcorrection.h exposure.h equalizer.h equalizer_eaw.h colorin.h colorout.h lens.h wb_presets.c Permutohedral.h profile_gamma.h
dtpluginsdir=$(libdir)/darktable/plugins
dtplugins_LTLIBRARIES=libtonecurve.la\
                      libgamma.la \
                      libtemperature.la\
                      libcolorcorrection.la\
                      libexposure.la\
                      libequalizer.la\
                      libcolorin.la\
                      libcolorout.la\
                      libclipping.la\
                      libsharpen.la\
                      libmonochrome.la\
                      librawimport.la\
                      libcolorzones.la\
                      libbasecurve.la\
                      libhighlights.la\
                      libvelvia.la\
                      libvignette.la\
                      libsplittoning.la\
                      libgrain.la\
                      libbilateral.la\
                      libprofile_gamma.la\
                      libcolortransfer.la\
                      libchannelmixer.la\
                      libgraduatednd.la\
                      librelight.la\
                      libzonesystem.la
if HAVE_LENSFUN
  liblens_la_SOURCES=lens.c
  liblens_la_CFLAGS=$(LENSFUN_CFLAGS)
  liblens_la_LIBADD=$(LENSFUN_LIBS)
  dtplugins_LTLIBRARIES+=liblens.la
endif

if HAVE_RSVG
  libwatermark_la_SOURCES=watermark.c
  libwatermark_la_CFLAGS=$(RSVG_CFLAGS)
  libwatermark_la_LIBADD=$(RSVG_LIBS)
  dtplugins_LTLIBRARIES+=libwatermark.la
endif
  
