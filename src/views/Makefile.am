include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS+=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/.. -Werror -g -rdynamic
LTADD=$(OPENMP_CFLAGS) $(GEGL_LIBS) $(JPEG_LIBS) $(PNG_LIBS) $(LCMS_LIBS) $(EXIV2_LIBS) $(CAIRO_LIBS) $(GLADE_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) $(SQLITE_LIBS) -fPIC
libdarkroom_la_SOURCES=darkroom.c
liblighttable_la_SOURCES=lighttable.c
libcapture_la_SOURCES=capture.c
libfilmstrip_la_SOURCES=filmstrip.c
noinst_HEADERS=view.h capture.h
dtviewsdir=$(libdir)/darktable/views
dtviews_LTLIBRARIES=libdarkroom.la liblighttable.la libcapture.la libfilmstrip.la
