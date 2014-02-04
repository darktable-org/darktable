PROGS = basecurve

ifeq ($(BUILD_TYPE),Release)
CFLAGS += -O3
endif

CFLAGS += -g -std=c99 -Wall -pedantic

all: $(PROGS)

clean:
	rm -f $(PROGS)

basecurve: basecurve.c
	$(CC) $(CFLAGS) basecurve.c -lm -o $@

.PHONY: all clean
