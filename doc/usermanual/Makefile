#!/usr/bin/make -f
# -*- makefile -*-

FO_STYLESHEET = xsl/darktable_fo.xsl
PROFILE_STYLESHEET = xsl/darktable_profile.xsl
xsltproc=xsltproc
fop=fop

darktable-usermanual.pdf:
	$(xsltproc) --output build/darktable-usermanual.fo $(FO_STYLESHEET) darktable.xml
	$(fop) -c fopconfig.xml build/darktable-usermanual.fo -pdf darktable-usermanual.pdf

