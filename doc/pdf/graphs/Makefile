
#
# this is the .dot to pdflatex Makefile.
# it generates graphs with dot and converts them
# to pdf with latex typeset fonts using xfig.
# you will need graphviz dot and fig2dev installed in your PATH.
#

DOT=dot
FIG2DEV=fig2dev
# put a list of your graphs here (with .pdftex_t extension, not .dot)
# GRAPHS=architecture.pdftex_t aabbtest.pdftex_t qbvh.pdftex_t triangular_bezier.pdftex_t bezier_2d.pdftex_t riot.pdftex_t chirkov_style_test.pdftex_t pt_ringbuffer.pdftex_t pt_ringbuffer2.pdftex_t
GRAPHS=$(shell ls *.fig | sed -e 's/fig/pdftex_t/')

.PHONY=all clean install

all: $(GRAPHS)

clean:
	rm -f *.pdftex_t *.pdf

install: all
	cp -f $(GRAPHS) ..
	cp -f *.pdf ..

%.fig: %.dot
	$(DOT) $< -Tfig -o $@

%.pdftex_t: %.fig
	$(FIG2DEV) -Lpdftex $< $*.pdf
	$(FIG2DEV) -Lpdftex_t -p graphs/$*.pdf $< $*.pdftex_t

