SUBDIRS=dtgtk LibRaw iop views libs # gegl-operations
include shared.am
AM_CFLAGS+=-Werror -rdynamic -Wno-deprecated-declarations
noinst_LIBRARIES=libdarktable.a
libdarktable_a_SOURCES=control/jobs.c gui/gtk.c gui/navigation.c gui/histogram.c gui/filmview.c develop/develop.c develop/imageop.c common/imageio.c common/darktable.c control/control.c common/film.c common/image.c common/image_cache.c common/image_compression.c gui/metadata.c develop/pixelpipe.c common/exif.cc common/imageio_jpeg.c common/imageio_png.c common/imageio_pfm.c common/imageio_rgbe.c views/view.c libs/lib.c
bin_PROGRAMS=darktable
darktable_SOURCES=main.c
darktable_LDADD=$(CARBON_LIBS) $(OPENMP_CFLAGS) $(PHAT_LIBS) $(GEGL_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) $(GCONF_LIBS) libdarktable.a LibRaw/libraw_r.a -lstdc++ -lm -L./dtgtk/.libs -ldtgtk
noinst_HEADERS=gui/gtk.h gui/histogram.h gui/navigation.h gui/filmview.h gui/draw.h develop/develop.h develop/imageop.h common/nikon_curve.h common/darktable.h common/imageio.h common/film.h control/settings.h control/control.h control/jobs.h common/image.h common/image_cache.h common/image_compression.h gui/metadata.h develop/pixelpipe.h develop/pixelpipe_hb.h develop/pixelpipe_cache.h develop/pixelpipe_hb.c develop/pixelpipe_gegl.c develop/pixelpipe_cache.c common/exif.h common/imageio_jpeg.h common/imageio_png.h common/imageio_pfm.h common/imageio_rgbe.h common/nikon_curve.c views/view.h control/conf.h
