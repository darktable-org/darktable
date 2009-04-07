AM_CFLAGS=-O4 -I$(srcdir) -Wall -fopenmp -pthread
LDADD=$(LCMS_LIBS)
noinst_LIBRARIES=libraw_r.a
libraw_r_a_SOURCES=internal/dcraw_common.cpp src/libraw_cxx.cpp internal/dcraw_fileio.cpp internal/foveon.cpp src/libraw_c_api.cpp
noinst_HEADERS=internal/defines.h internal/libraw_internal_funcs.h internal/var_defines.h libraw/libraw_internal.h libraw/libraw_alloc.h libraw/libraw_types.h libraw/libraw.h libraw/libraw_version.h libraw/libraw_const.h libraw/libraw_datastream.h
