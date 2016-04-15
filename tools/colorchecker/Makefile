CC=gcc
CFLAGS=-Wall -I. -g
# OPTFLAGS=-O3 -march=native
OPTFLAGS=-ggdb3 -O0
LDFLAGS=-lm

all: test
test: main.c thinplate.h tonecurve.h ../../src/lut/deltaE.c
	$(CC) $(CFLAGS) $(OPTFLAGS) main.c ../../src/lut/deltaE.c -o test $(LDFLAGS)

clean:
	rm -f test
