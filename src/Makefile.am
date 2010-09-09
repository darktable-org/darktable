SUBDIRS=LibRaw iop views libs imageio # gegl-operations
include shared.am
AM_CFLAGS+=-Werror -rdynamic -Wno-deprecated-declarations
AM_CPPFLAGS=$(OPENEXR_CFLAGS)
libdarktable_LTLIBRARIES=libdarktable.la
libdarktabledir=$(libdir)/darktable
libdarktable_la_LDFLAGS=-avoid-version -fPIC
libdarktable_la_LIBADD=LibRaw/libraw_r.la $(CARBON_LIBS) $(GNOME_KEYRING_LIBS) $(OPENMP_LDFLAGS) $(CURL_LIBS) $(GEGL_LIBS) $(GPHOTO2_LIBS) $(OPENEXR_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(TIFF_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) $(GCONF_LIBS) $(DBUSGLIB_LIBS)
libdarktable_la_SOURCES= \
			common/camera_control.c \
			common/collection.c \
			common/colorspaces.c \
			common/colorlabels.c \
			common/colormatrices.c \
			common/darktable.c \
			common/exif.cc \
			common/film.c \
			common/fswatch.c \
			common/history.c \
			common/image.c \
			common/image_cache.c \
			common/image_compression.c \
			common/imageio.c \
			common/imageio_jpeg.c \
			common/imageio_module.c \
			common/imageio_pfm.c \
			common/imageio_rgbe.c \
			common/imageio_tiff.c \
			common/pwstorage/backend_gconf.c \
			common/pwstorage/backend_gkeyring.c \
			common/pwstorage/backend_kwallet.c \
			common/pwstorage/pwstorage.c \
			common/tags.c \
			common/variables.c \
			control/control.c \
			control/jobs/camera_jobs.c \
			control/jobs/control_jobs.c \
			control/jobs/develop_jobs.c \
			control/jobs/film_jobs.c \
			control/jobs/image_jobs.c \
			develop/develop.c \
			develop/imageop.c \
			develop/pixelpipe.c \
			dtgtk/button.c \
			dtgtk/gradientslider.c \
			dtgtk/label.c \
			dtgtk/paint.c \
			dtgtk/slider.c \
			dtgtk/togglebutton.c \
			gui/camera_import_dialog.c \
			gui/contrast.c \
			gui/background_jobs.c \
			gui/devices.c \
			gui/filmview.c \
			gui/gtk.c \
			gui/histogram.c \
			gui/iop_history.c \
			gui/iop_modulegroups.c \
			gui/metadata.c \
			gui/navigation.c \
			gui/panel_sizegroup.c \
			gui/presets.c \
			libs/lib.c \
			views/view.c 
			
bin_PROGRAMS=darktable
darktable_SOURCES=main.c
darktable_LDADD=libdarktable.la $(CARBON_LIBS) $(OPENMP_LDLAGS) $(CURL_LIBS) $(GEGL_LIBS) $(GPHOTO2_LIBS) $(OPENEXR_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(TIFF_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) $(GCONF_LIBS) $(DBUSGLIB_LIBS)  -lstdc++ -lm
noinst_HEADERS= \
			gui/gtk.h \
			gui/devices.h \
			gui/camera_import_dialog.h \
			gui/contrast.h \
			gui/histogram.h \
			gui/navigation.h \
			gui/filmview.h \
			gui/iop_history.h \
			gui/iop_modulegroups.h \
			gui/draw.h \
			gui/panel_sizegroup.h \
			gui/presets.h \
			gui/background_jobs.h \
			develop/develop.h \
			develop/imageop.h \
			common/collection.h \
			common/tags.h \
			common/nikon_curve.h \
			common/variables.h \
			common/camera_control.h \
			common/darktable.h \
			common/imageio.h \
			common/history.h \
			common/film.h \
			control/settings.h \
			control/control.h \
			control/jobs.h \
			control/jobs/camera_jobs.h \
			control/jobs/control_jobs.h \
			control/jobs/develop_jobs.h \
			control/jobs/film_jobs.h \
			control/jobs/image_jobs.h \
			common/image.h \
			common/imageio_module.h \
			common/image_cache.h \
			common/image_compression.h \
			gui/metadata.h develop/pixelpipe.h \
			develop/pixelpipe_hb.h \
			develop/pixelpipe_cache.h \
			develop/pixelpipe_hb.c \
			develop/pixelpipe_gegl.c \
			develop/pixelpipe_cache.c \
			common/exif.h \
			common/imageio_jpeg.h \
			common/imageio_tiff.h \
			common/imageio_pfm.h \
			common/imageio_rgbe.h \
			common/nikon_curve.c \
			views/view.h control/conf.h \
			dtgtk/gradientslider.h \
			dtgtk/slider.h dtgtk/label.h \
			dtgtk/button.h \
			dtgtk/togglebutton.h \
			dtgtk/paint.h \
			common/fswatch.h \
			common/colorspaces.h \
			common/colorlabels.h \
			common/points.h \
			common/pwstorage/pwstorage.h \
			common/pwstorage/backend_gconf.h \
			common/pwstorage/backend_gkeyring.h \
			common/pwstorage/backend_kwallet.h
