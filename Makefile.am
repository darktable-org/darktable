ACLOCAL_AMFLAGS=-I m4
SUBDIRS=src pixmaps po color doc
INTLTOOL_FILES=intltool-extract.in intltool-merge.in intltool-update.in
EXTRA_DIST=darktable.gtkrc darktable.glade darktable.desktop $(INTLTOOL_FILES) darktable.schemas.in darktable.1 TRANSLATORS LICENSE
DISTCLEANFILES=intltool-extract intltool-merge intltool-update po/.intltool-merge-cache
dtdatarootdir=$(datarootdir)/darktable
dtdataroot_DATA=darktable.gtkrc darktable.glade
dtapprootdir=$(datarootdir)/applications
dtapproot_DATA=darktable.desktop
dtdocdir=$(datarootdir)/doc/darktable
dtdoc_DATA=README AUTHORS TRANSLATORS LICENSE
man1_MANS=darktable.1
if HAVE_GCONF
schemadir=$(GCONF_SCHEMA_FILE_DIR)
schema_in_file=darktable.schemas.in
schema_DATA=$(schema_in_file:.schemas.in=.schemas)
@INTLTOOL_SCHEMAS_RULE@

CLEANFILES=$(schema_DATA)

if GCONF_SCHEMAS_INSTALL
install-data-local:
	if test -z "$(DESTDIR)" ; then \
		for p in $(schema_DATA) ; do \
			GCONF_CONFIG_SOURCE=$(GCONF_SCHEMA_CONFIG_SOURCE) $(GCONFTOOL) --makefile-install-rule $$p || \
      GCONF_CONFIG_SOURCE="xml:readwrite:${HOME}/.gconf" $(GCONFTOOL) --makefile-install-rule $$p ; \
		done; \
		killall gconfd-2 || true; \
	fi
else
install-data-local:
endif
else
EXTRA_DIST+=darktablerc
dtdataroot_DATA+=darktablerc
endif
