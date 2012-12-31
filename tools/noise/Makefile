PROGS = noiseprofile floatdump

CFLAGS = -g -std=c99 -Wall

all: $(PROGS)

clean:
	rm -f $(PROGS)

noiseprofile: noiseprofile.c
	$(CC) $(CFLAGS) noiseprofile.c -lm -o $@

floatdump: floatdump.c
	$(CC) $(CFLAGS) -fno-strict-aliasing floatdump.c -o $@

.PHONY: all clean
