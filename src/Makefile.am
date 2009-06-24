SUBDIRS=LibRaw iop gegl-operations
include shared.am
noinst_LIBRARIES=libdarktable.a
libdarktable_a_SOURCES=control/jobs.c library/film.c library/library.c gui/gtk.c gui/navigation.c gui/histogram.c develop/develop.c develop/imageop.c common/imageio.c common/darktable.c common/nikon_curve.c control/control.c gui/develop_view.c common/image.c common/image_cache.c common/image_compression.c gui/metadata.c develop/pixelpipe.c
bin_PROGRAMS=darktable
darktable_SOURCES=main.c
darktable_LDADD=$(GEGL_LIBS) $(LIBEXIF_LIBS) $(MAGICK_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) libdarktable.a LibRaw/libraw_r.a -lstdc++
noinst_HEADERS=library/library.h gui/gtk.h gui/histogram.h gui/navigation.h develop/develop.h develop/imageop.h common/nikon_curve.h common/darktable.h common/imageio.h control/settings.h control/control.h control/jobs.h common/image.h common/image_cache.h common/image_compression.h gui/metadata.h develop/pixelpipe.h
