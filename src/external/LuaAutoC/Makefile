-include config.mk

PLATFORM?= $(shell uname)

CC?=gcc
AR?=ar

LAC_CPPFLAGS= -I./include $(LUA_INCLUDE_DIR)
LAC_CFLAGS= -std=gnu99 -Wall -Werror -Wno-unused -O3 -g
LAC_LDFLAGS= $(LUA_LIBRARY_DIR)
LAC_LIBS= $(LUA_LIBRARY)

LIB_C_FILES= $(wildcard src/*.c)
LIB_OBJ_FILES= $(addprefix obj/,$(notdir $(LIB_C_FILES:%.c=%.o)))

DEMO_C_FILES= $(wildcard demos/*.c)
DEMO_OBJ_FILES= $(addprefix obj/,$(notdir $(DEMO_C_FILES:%.c=%.o)))

DEMO_TARGETS= $(DEMO_C_FILES:%.c=%$(EXE_SUFFIX))
DEMOS= $(DEMO_C_FILES:demos/demo_%.c=%)

SHARED_LIB= $(SHARED_LIB_PREFIX)lautoc$(SHARED_LIB_SUFFIX)
STATIC_LIB= $(STATIC_LIB_PREFIX)lautoc$(STATIC_LIB_SUFFIX)

ifeq ($(findstring Linux,$(PLATFORM)),Linux)
	LUA_INCLUDE_DIR?= -I/usr/include/lua5.2
	LUA_LIBRARY?= -llua5.2
	LAC_CFLAGS+= -fPIC
	LAC_LDFLAGS+= -fPIC
	SHARED_LIB_PREFIX:=lib
	SHARED_LIB_SUFFIX:=.so
	STATIC_LIB_PREFIX:=lib
	STATIC_LIB_SUFFIX:=.a
	EXE_SUFFIX:=
else ifeq ($(findstring Darwin,$(PLATFORM)),Darwin)
	LUA_INCLUDE_DIR?= -I/usr/include/lua5.2
	LUA_LIBRARY?= -llua5.2
	LAC_CFLAGS+= -fPIC
	LAC_LDFLAGS+= -fPIC
	SHARED_LIB_PREFIX:=lib
	SHARED_LIB_SUFFIX:=.so
	STATIC_LIB_PREFIX:=lib
	STATIC_LIB_SUFFIX:=.a
else ifeq ($(findstring MINGW,$(PLATFORM)),MINGW)
	LUA_INCLUDE_DIR?= -I./lua52/include
	LUA_LIBRARY_DIR?= -L./lua52
	LUA_LIBRARY?= -llua52
	LAC_LDFLAGS+= -L.
	LAC_LIBS+= -lmingw32
	SHARED_LIB_PREFIX:=
	SHARED_LIB_SUFFIX:=.dll
	STATIC_LIB_PREFIX:=
	STATIC_LIB_SUFFIX:=.lib
	EXE_SUFFIX:=.exe
endif

# Library

all: $(SHARED_LIB) $(STATIC_LIB)

$(SHARED_LIB): $(LIB_OBJ_FILES)
	$(CC) $(LAC_LDFLAGS) $(LDFLAGS) -shared -o $@ $^ $(LAC_LIBS)

$(STATIC_LIB): $(LIB_OBJ_FILES)
	$(AR) rcs $@ $^
  
# Demos

demos: $(DEMO_TARGETS)

$(DEMO_TARGETS): demos/demo_%$(EXE_SUFFIX): obj/demo_%.o $(STATIC_LIB)
	$(CC) $(LAC_LDFLAGS) $(LDFLAGS) -o $@ $^ $(LAC_LIBS)
 
# Clean

clean:
	$(RM) $(LIB_OBJ_FILES) $(DEMO_OBJ_FILES) $(DEMO_TARGETS) $(SHARED_LIB) $(STATIC_LIB)

obj:
	mkdir obj

obj/%.o: src/%.c | obj
	$(CC) $(LAC_CPPFLAGS) $(LAC_CFLAGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

obj/%.o: demos/%.c | obj
	$(CC) $(LAC_CPPFLAGS) $(LAC_CFLAGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

