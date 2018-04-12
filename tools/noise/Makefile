PROGS = noiseprofile

CFLAGS = -g -std=c99 -Wall

all: $(PROGS)

clean:
	rm -f $(PROGS)

noiseprofile: noiseprofile.c
	$(CC) $(CFLAGS) noiseprofile.c -lm -o $@

.PHONY: all clean
