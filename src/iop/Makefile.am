include $(srcdir)/../shared.am
libtonecurve_la_SOURCES=tonecurve.c
libtonecurve_la_LDFLAGS= -module -avoid-version
libtonecurve_la_CFLAGS=$(AM_CFLAGS) -I$(srcdir) -I$(srcdir)/..
noinst_HEADERS=tonecurve.h
dtpluginsdir=$(datarootdir)/darktable/plugins
dtplugins_LTLIBRARIES=libtonecurve.la
