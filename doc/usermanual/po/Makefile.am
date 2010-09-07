@AMDEP_TRUE@@am__include@

TEMPLATE=darktable-usermanual.pot
TRANSLATIONS=\
	sv \
	ru

all: $(TRANSLATIONS)

update-template:
	@xml2po -e -o $(TEMPLATE) ../darktable.xml

$(TRANSLATIONS): update-template
	@if test ! -f $@.po; then \
		cp $(TEMPLATE) $@.po; \
	fi
	@xml2po -a -e -u $@.po ../darktable.xml