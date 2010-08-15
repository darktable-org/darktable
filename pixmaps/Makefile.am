SUBDIRS = plugins 16x16 22x22 32x32 48x48 64x64 scalable

theme=hicolor
themedir=$(datadir)/icons/$(theme)

gtk_update_icon_cache = gtk-update-icon-cache -f -t $(themedir)

install-data-hook: update-icon-cache
uninstall-hook: update-icon-cache
update-icon-cache:
	@-if test -z "$(DESTDIR)"; then \
		echo "Updating Gtk icon cache."; \
		$(gtk_update_icon_cache); \
	else \
		echo "*** Icon cache not updated.  After (un)install, run this:"; \
		echo "***   $(gtk_update_icon_cache)"; \
	fi

