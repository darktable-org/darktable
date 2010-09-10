SUBDIRS = po

if HAVE_DOCS
@AMDEP_TRUE@@am__include@

translations = \
		sv \
		po \
		ru

FO_STYLESHEET = xsl/$(PACKAGE)_fo.xsl
PROFILE_STYLESHEET = xsl/$(PACKAGE)_profile.xsl

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
	$(xsltproc) --output build/$(PACKAGE)-usermanual.fo  $(FO_STYLESHEET) $(PACKAGE).xml 
	$(fop) build/$(PACKAGE)-usermanual.fo -pdf $(PACKAGE)-usermanual.pdf
	
$(pdfusermanual_final): 
	@ $(MAKE) check_can_build_usermanual
	$(MAKE) doc-clean
	@echo "Profiling final xml document"
	$(xsltproc) --output build/$(PACKAGE)-usermanual_profiled_final.xml $(PROFILE_STYLESHEET) $(PACKAGE).xml 
	$(xsltproc) --output build/$(PACKAGE)-usermanual_profiled_final.fo $(FO_STYLESHEET)  build/$(PACKAGE)-usermanual_profiled_final.xml
	$(fop) build/$(PACKAGE)-usermanual_profiled_final.fo -pdf $(PACKAGE)-usermanual-final.pdf
	
$(translations): 
	@ $(MAKE) check_can_build_usermanual
	$(MAKE) doc-clean
	@echo "Profiling final xml document"
	$(xsltproc) --output build/$(PACKAGE)-usermanual_profiled_final.xml $(PROFILE_STYLESHEET) $(PACKAGE).xml 
	@echo "Translating final xml document"
	$(xml2po) -e -p po/$@.po -l $@ build/$(PACKAGE)-usermanual_profiled_final.xml > build/$(PACKAGE)-usermanual_profiled_final-$@.xml
	$(xsltproc) --output build/$(PACKAGE)-usermanual-$@.fo $(FO_STYLESHEET) build/$(PACKAGE)-usermanual_profiled_final-$@.xml
	$(fop) build/$(PACKAGE)-usermanual-$@.fo -pdf $(PACKAGE)-usermanual-$@.pdf
	
maintainer-clean-local: doc-clean maintainer-clean-recursive

doc-clean:
	-rm -f build/*
	-rm -f *.pdf
	-rm -f *.fo

.PHONY: check_can_build_usermanual force-update-doc doc-clean

endif
