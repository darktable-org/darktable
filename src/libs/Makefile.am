include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/.. -Werror -g -rdynamic
LTADD=$(OPENMP_CFLAGS) $(GEGL_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) -fPIC
libexport_la_SOURCES=export.c
libcopy_history_la_SOURCES=copy_history.c
libimage_la_SOURCES=image.c
libtagging_la_SOURCES=tagging.c
noinst_HEADERS=lib.h
dtlighttablepluginsdir=$(datarootdir)/darktable/plugins/lighttable
dtlighttableplugins_LTLIBRARIES=libexport.la libcopy_history.la libimage.la libtagging.la
