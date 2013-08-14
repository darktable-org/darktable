PROGS = basecurve

CFLAGS = -g -std=c99 -Wall

all: $(PROGS)

clean:
	rm -f $(PROGS)

basecurve: basecurve.c
	$(CC) $(CFLAGS) basecurve.c -lm -o $@

.PHONY: all clean
