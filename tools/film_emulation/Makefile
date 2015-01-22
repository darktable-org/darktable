PROGS = fit unitylut

CFLAGS = -g -std=c99 -Wall -D_XOPEN_SOURCE

all: $(PROGS)

clean:
	rm -f $(PROGS)

fit: fit.c template.h Makefile
	$(CC) $(CFLAGS) fit.c levmar-2.6/liblevmar.a -lm -o $@
	# use this if you linked your levmar aginst lapack
	# $(CC) $(CFLAGS) fit.c levmar-2.6/liblevmar.a -llapack -lblas -lf2c -lm -o $@

unitylut: unitylut.c Makefile
	$(CC) $(CFLAGS) unitylut.c -o $@

template.h: template_head.xmp template_foot.xmp template_color_head.xmp
	xxd -i template_head.xmp > template.h
	xxd -i template_color_head.xmp >> template.h
	xxd -i template_foot.xmp >> template.h

.PHONY: all clean
