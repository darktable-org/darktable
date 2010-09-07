TEMPLATE=darktable-usermanual.pot
TRANSLATIONS=\
	sv \
	ru

all: $(LINGUAS)

update-template:
	@xml2po -e -o $(TEMPLATE) ../darktable.xml

$(LINGUAS): update-template
	@if test ! -f $@.po; then \
		cp $(TEMPLATE) $@.po; \
	fi
	@xml2po -a -e -u $@.po ../darktable.xml