# CFLAGS+=$(shell pkg-config glib-2.0 --cflags)
# LDFLAGS+=$(shell pkg-config glib-2.0 --libs)

cache: cache.c ../common/cache.h ../common/cache.c Makefile
	gcc -std=c99 -O0 -I.. -g -march=native -o cache cache.c -fopenmp ${CFLAGS} ${LDFLAGS}
