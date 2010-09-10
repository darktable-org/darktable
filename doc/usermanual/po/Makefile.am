SUBDIRS=

if HAVE_DOCS
@AMDEP_TRUE@@am__include@

TEMPLATE=darktable-usermanual.pot
PROFILE_STYLESHEET = ../xsl/$(PACKAGE)_profile.xsl
PO_LINGUAS=$(shell if test -r $(srcdir)/LINGUAS; then grep -v "^\#" $(srcdir)/LINGUAS; fi)

update-template:
	$(xsltproc) --output ../build/$(PACKAGE)-usermanual_profiled_final.xml $(PROFILE_STYLESHEET) ../$(PACKAGE).xml 
	$(xml2po) -a -e -o $(TEMPLATE) ../build/$(PACKAGE)-usermanual_profiled_final.xml
	
$(PO_LINGUAS): update-template
	@if test ! -f $@.po; then \
		cp $(TEMPLATE) $@.po; \
	fi
	$(xml2po) -a -e -u $@.po ../build/$(PACKAGE)-usermanual_profiled_final.xml

endif
