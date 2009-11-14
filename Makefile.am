ACLOCAL_AMFLAGS=-I m4
SUBDIRS=src pixmaps po
INTLTOOL_FILES=intltool-extract.in intltool-merge.in intltool-update.in
EXTRA_DIST=darktable.gtkrc darktable.glade darktable.desktop $(INTLTOOL_FILES)
DISTCLEANFILES=intltool-extract intltool-merge intltool-update po/.intltool-merge-cache
dtdatarootdir=$(datarootdir)/darktable
dtdataroot_DATA=darktable.gtkrc darktable.glade
dtapprootdir=$(datarootdir)/applications
dtapproot_DATA=darktable.desktop
schemadir=$(datadir)/gconf/schemas
#@GCONF_SCHEMA_FILE_DIR@
schema_DATA=darktable.schemas

#install-data-local:
#	GCONF_CONFIG_SOURCE=$(GCONF_SCHEMA_CONFIG_SOURCE) $(GCONFTOOL) --makefile-install-rule $(srcdir)/$(schema_DATA)


