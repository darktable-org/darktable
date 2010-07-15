include $(srcdir)/../../shared.am
LDFLAGS= -module -avoid-version
CFLAGS+=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/../.. -Werror -g -rdynamic
LDADD=$(OPENMP_CFLAGS) $(GEGL_LIBS) $(CURL_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) -fPIC
libdisk_la_SOURCES=disk.c
libpicasa_la_SOURCES=picasa.c
libemail_la_SOURCES=email.c
dtimageiostoragepluginsdir=$(libdir)/darktable/plugins/imageio/storage
dtimageiostorageplugins_LTLIBRARIES=libdisk.la libpicasa.la libemail.la
