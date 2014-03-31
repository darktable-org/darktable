PROGS = dt-curve-tool

ifeq ($(BUILD_TYPE),Debug)
CCFLAGS += -O0
else
CCFLAGS += -O3
endif

CCFLAGS += -g -Wall

CFLAGS = $(CCFLAGS) -std=c99 -pedantic
CXXFLAGS=$(CFLAGS)

DT_CURVE_TOOL_OBJS += exif-wrapper.o
DT_CURVE_TOOL_OBJS += dt-curve-tool.o

all: $(PROGS)

clean:
	rm -f $(PROGS) $(DT_CURVE_TOOL_OBJS)


dt-curve-tool: $(DT_CURVE_TOOL_OBJS)
	$(CXX) $(CXXFLAGS) $^ -lm -lexiv2 -o $@

.PHONY: all clean
