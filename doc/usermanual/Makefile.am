SUBDIRS = 

@AMDEP_TRUE@@am__include@

translations = \
		sv \
		ru

STYLESHEET = xsl/$(PACKAGE).xsl

pdfusermanual = $(PACKAGE)-usermanual.pdf 
pdfusermanual_final = $(PACKAGE)-usermanual-final.pdf 
pdfusermanualdir = $(datadir)/doc
pdfusermanual_DATA = $(pdfusermanual)

EXTRA_DIST = $(PACKAGE)-usermanual.pdf 

DISTCLEANFILES = Makefile

check_can_build_usermanual:
	@if test x$(can_build_usermanual) != "xyes"; then \
		if test -z x$(xsltproc); then \
			echo "Missing libxslt."; \
		else if test -z x$(xml2po); then \
				echo "Missing xml2po."; \
			else \
				echo "Missing fop."; \
			fi; \
		fi; \
	exit 1 ; fi
	
all: $(pdfusermanual)

force-update-doc: doc-clean $(pdfusermanual)

$(pdfusermanual): 
	@ $(MAKE) check_can_build_usermanual
	$(MAKE) doc-clean
	$(xsltproc) --output build/$(PACKAGE)-usermanual.fo  $(STYLESHEET) $(PACKAGE).xml 
	$(fop) build/$(PACKAGE)-usermanual.fo -pdf $(PACKAGE)-usermanual.pdf
	
$(pdfusermanual_final): 
	@ $(MAKE) check_can_build_usermanual
	$(MAKE) doc-clean
	$(xsltproc) --output build/$(PACKAGE)-usermanual_profiled_final-profiled.fo --stringparam profile.status "final" $(STYLESHEET) $(PACKAGE).xml 
	$(fop) build/$(PACKAGE)-usermanual_profiled_final-profiled.fo -pdf $(PACKAGE)-usermanual-final.pdf
	
$(translations): 
	@ $(MAKE) check_can_build_usermanual
	$(MAKE) doc-clean
	$(xml2po) -a -e -p po/$@.po -l $@ $(PACKAGE).xml > build/$(PACKAGE)-usermanual-$@.xml
	$(xsltproc) --output build/$(PACKAGE)-usermanual-$@.fo $(STYLESHEET) build/$(PACKAGE)-usermanual-$@.xml
	$(fop) build/$(PACKAGE)-usermanual-$@.fo -pdf $(PACKAGE)-usermanual-$@.pdf
	
maintainer-clean-local: doc-clean maintainer-clean-recursive

doc-clean:
	-rm -f build/*
	-rm -f *.pdf
	-rm -f *.fo

.PHONY: check_can_build_usermanual force-update-doc doc-clean