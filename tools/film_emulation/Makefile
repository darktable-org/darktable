PROGS = fit identitylut

CFLAGS = -g -std=c99 -Wall -D_XOPEN_SOURCE
CC=gcc

all: $(PROGS)

clean:
	rm -f $(PROGS)

fit: fit.c template.h Makefile
	# $(CC) $(CFLAGS) fit.c levmar-2.6/liblevmar.a -lm -o $@
	# use this if you linked your levmar aginst lapack
	$(CC) -u MAIN__ $(CFLAGS) fit.c levmar-2.6/build/liblevmar.a -llapack -lblas -lf2c -lm -lc -o $@

identitylut: identitylut.c Makefile
	$(CC) $(CFLAGS) identitylut.c -o $@

template.h: template_head.xmp template_foot.xmp template_color_head.xmp
	xxd -i template_head.xmp > template.h
	xxd -i template_color_head.xmp >> template.h
	xxd -i template_foot.xmp >> template.h

.PHONY: all clean
