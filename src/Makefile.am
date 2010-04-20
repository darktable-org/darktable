SUBDIRS=LibRaw iop views libs # gegl-operations
include shared.am
AM_CFLAGS+=-Werror -rdynamic -Wno-deprecated-declarations
AM_CPPFLAGS=$(OPENEXR_CFLAGS)
lib_LTLIBRARIES=libdarktable.la
libdarktable_la_LDFLAGS=-avoid-version -fPIC
libdarktable_la_LIBADD=LibRaw/libraw_r.la
libdarktable_la_SOURCES=control/jobs.c gui/gtk.c gui/capture.c gui/camera_import_dialog.c gui/navigation.c gui/histogram.c gui/filmview.c develop/develop.c develop/imageop.c common/imageio.c common/variables.c common/darktable.c control/control.c common/film.c common/image.c common/image_cache.c common/image_compression.c gui/metadata.c develop/pixelpipe.c common/exif.cc common/imageio_jpeg.c common/imageio_png.c common/imageio_pfm.c common/imageio_rgbe.c common/imageio_tiff.c common/imageio_exr.cc common/imageio_ppm.c views/view.c libs/lib.c dtgtk/label.c dtgtk/togglebutton.c dtgtk/gradientslider.c dtgtk/slider.c dtgtk/button.c dtgtk/paint.c common/fswatch.c common/camera_control.c common/colorspaces.c common/colorlabels.c
bin_PROGRAMS=darktable
darktable_SOURCES=main.c
darktable_LDADD=$(CARBON_LIBS) $(OPENMP_CFLAGS) $(GEGL_LIBS) $(GPHOTO2_LIBS) $(OPENEXR_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(TIFF_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) $(GCONF_LIBS) libdarktable.la -lstdc++ -lm
noinst_HEADERS=gui/gtk.h gui/capture.h gui/camera_import_dialog.h gui/histogram.h gui/navigation.h gui/filmview.h gui/draw.h develop/develop.h develop/imageop.h common/nikon_curve.h common/variables.h common/darktable.h common/imageio.h common/film.h control/settings.h control/control.h control/jobs.h common/image.h common/image_cache.h common/image_compression.h gui/metadata.h develop/pixelpipe.h develop/pixelpipe_hb.h develop/pixelpipe_cache.h develop/pixelpipe_hb.c develop/pixelpipe_gegl.c develop/pixelpipe_cache.c common/exif.h common/imageio_jpeg.h common/imageio_png.h common/imageio_pfm.h common/imageio_rgbe.h common/imageio_tiff.h common/imageio_ppm.h common/imageio_exr.h common/nikon_curve.c views/view.h control/conf.h dtgtk/gradientslider.h dtgtk/slider.h dtgtk/label.h dtgtk/button.h dtgtk/togglebutton.h dtgtk/paint.h common/fswatch.h common/camera_control.h common/colorspaces.h common/colorlabels.h

