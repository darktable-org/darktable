@AMDEP_TRUE@@am__include@

TEMPLATE=darktable-usermanual.pot
PROFILE_STYLESHEET = ../xsl/$(PACKAGE)_profile.xsl
TRANSLATIONS=\
	sv \
	ru

all: $(TRANSLATIONS)

update-template:
	$(xsltproc) --output ../build/$(PACKAGE)-usermanual_profiled_final.xml $(PROFILE_STYLESHEET) ../$(PACKAGE).xml 
	$(xml2po) -a -e -o $(TEMPLATE) ../build/$(PACKAGE)-usermanual_profiled_final.xml
	
$(TRANSLATIONS): update-template
	@if test ! -f $@.po; then \
		cp $(TEMPLATE) $@.po; \
	fi
	$(xml2po) -a -e -u $@.po ../build/$(PACKAGE)-usermanual_profiled_final.xml