include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/.. -Werror -g -rdynamic
LTADD=$(OPENMP_CFLAGS) $(GEGL_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) -fPIC
libexport_la_SOURCES=export.c
noinst_HEADERS=lib.h
dtlighttablepluginsdir=$(datarootdir)/darktable/plugins/lighttable
dtlighttableplugins_LTLIBRARIES=libexport.la
