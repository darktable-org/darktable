SUBDIRS =

@AMDEP_TRUE@@am__include@

pdfusermanual = $(PACKAGE)-usermanual.pdf 
pdfusermanualdir = $(datadir)/doc
pdfusermanual_DATA = $(pdfusermanual)

EXTRA_DIST = $(PACKAGE)-usermanual.pdf 

DISTCLEANFILES = Makefile

check_can_build_usermanual:
	@if test x$(can_build_usermanual) != "xyes"; then \
		if test -z x$(xsltproc); then \
			echo "Missing libxslt."; \
		else \
			echo "Missing fop."; \
		fi; \
	exit 1 ; fi
	
all: $(pdfusermanual)

force-update-doc: doc-clean $(pdfusermanual)

$(pdfusermanual): 
	@ $(MAKE) check_can_build_usermanual
	$(MAKE) doc-clean
	$(xsltproc) --output $(PACKAGE)-usermanual.fo xsl/$(PACKAGE).xsl $(PACKAGE).xml 
	$(MAKE) pdfusermanual-render

pdfusermanual-render:
	$(fop) $(PACKAGE)-usermanual.fo -pdf $(PACKAGE)-usermanual.pdf
	
maintainer-clean-local: doc-clean maintainer-clean-recursive

doc-clean:
	-rm -f *.pdf
	-rm -f *.fo

.PHONY: check_can_build_usermanual force-update-doc doc-clean