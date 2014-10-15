-include config.mk

PLATFORM?= $(shell uname)

CC?=gcc
AR?=ar

LAC=lautoc.c
LAC_OBJ=lautoc.o
LAC_CPPFLAGS= -I./include $(LUA_INCLUDE_DIR)
LAC_CFLAGS= -std=gnu99 -Wall -Werror -Wno-unused -O3 -g
LAC_LDFLAGS= $(LUA_LIBRARY_DIR)
LAC_LIBS= $(LUA_LIBRARY)

EXAMPLES_SRC= $(wildcard examples/*.c)
EXAMPLES_OUT= $(EXAMPLES_SRC:%.c=%$(EXE_SUFFIX))

SHARED_LIB= $(SHARED_LIB_PREFIX)lautoc$(SHARED_LIB_SUFFIX)
STATIC_LIB= $(STATIC_LIB_PREFIX)lautoc$(STATIC_LIB_SUFFIX)

ifeq ($(findstring Linux,$(PLATFORM)),Linux)
	LUA_INCLUDE_DIR?= -I/usr/include/lua5.2/
	LUA_LIBRARY?= -llua5.2
	LAC_CFLAGS+= -fPIC
	LAC_LDFLAGS+= -fPIC
	SHARED_LIB_PREFIX:=lib
	SHARED_LIB_SUFFIX:=.so
	STATIC_LIB_PREFIX:=lib
	STATIC_LIB_SUFFIX:=.a
	EXE_SUFFIX:=
else ifeq ($(findstring Darwin,$(PLATFORM)),Darwin)
	LUA_INCLUDE_DIR?= -I/usr/include/lua5.2/
	LUA_LIBRARY?= -llua5.2
	LAC_CFLAGS+= -fPIC
	LAC_LDFLAGS+= -fPIC
	SHARED_LIB_PREFIX:=lib
	SHARED_LIB_SUFFIX:=.so
	STATIC_LIB_PREFIX:=lib
	STATIC_LIB_SUFFIX:=.a
	EXE_SUFFIX:=
else ifeq ($(findstring MINGW,$(PLATFORM)),MINGW)
	LUA_LIBRARY?= -llua
	LAC_LIBS+= -lmingw32
	SHARED_LIB_PREFIX:=
	SHARED_LIB_SUFFIX:=.dll
	STATIC_LIB_PREFIX:=
	STATIC_LIB_SUFFIX:=.lib
	EXE_SUFFIX:=.exe
endif

# Library

all: $(SHARED_LIB) $(STATIC_LIB)

$(SHARED_LIB): $(LAC)
	$(CC) $^ $(CFLAGS) $(LAC_CFLAGS) $(LAC_CPPFLAGS) $(LDFLAGS) $(LAC_LDFLAGS) $(LAC_LIBS) -shared -o $@  

$(STATIC_LIB): $(LAC)
	$(CC) $^ $(CFLAGS) $(LAC_CFLAGS) $(LAC_CPPFLAGS) -c
	$(AR) rcs $@ $(LAC_OBJ)

# Examples

examples: $(EXAMPLES_OUT)

examples/example_%$(EXE_SUFFIX): examples/example_%.c $(LAC)
	$(CC) $^ $(CFLAGS) $(LAC_CFLAGS) $(LAC_CPPFLAGS) $(LDFLAGS) $(LAC_LDFLAGS) $(LAC_LIBS) -o $@  

# Clean

clean:
	$(RM) $(LAC_OBJ) $(EXAMPLES_OUT) $(SHARED_LIB) $(STATIC_LIB)

