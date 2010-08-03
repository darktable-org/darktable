SUBDIRS=plugins
theme=hicolor
themedir=$(datadir)/icons/$(theme)
theme_icons = \
apps,darktable.svg \
apps,darktable-16.png \
apps,darktable-22.png \
apps,darktable-32.png \
apps,darktable-48.png \
apps,darktable-64.png

install_icon_exec = $(top_srcdir)/pixmaps/icon-theme-installer -t $(theme) -s $(srcdir) -d "x$(DESTDIR)" -b $(themedir) -m "$(mkinstalldirs)" -x "$(INSTALL_DATA)"
install-data-local:
	$(install_icon_exec) -i $(theme_icons)

uninstall-hook:
	$(install_icon_exec) -u $(theme_icons)

MAINTAINERCLEANFILES = Makefile.in
EXTRA_DIST = $(wildcard *.svg *.png) icon-theme-installer
