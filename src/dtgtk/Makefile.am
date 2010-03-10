include $(srcdir)/../shared.am
LDFLAGS= -module -avoid-version
CFLAGS+=$(AM_CFLAGS) -Werror -g
LTADD=$(CAIRO_LIBS) $(GTK_LIBS) $(GTHREAD_LIBS) -fPIC
libdtgtk_la_SOURCES=slider.c togglebutton.c paint.c
noinst_HEADERS=slider.h togglebutton.h paint.h
dtgtkdir=$(libdir)/darktable
dtgtk_LTLIBRARIES=libdtgtk.la
