noinst_LIBRARIES=libraw_r.a
libraw_r_a_SOURCES=internal/dcraw_common.cpp src/libraw_cxx.cpp internal/dcraw_fileio.cpp internal/foveon.cpp src/libraw_c_api.cpp
libraw_r_a_CPPFLAGS=-O4 -I$(srcdir) -Wall $(OPENMP_CFLAGS)
libraw_r_a_CFLAGS=-O4 -I$(srcdir) -Wall $(OPENMP_CFLAGS)
# libraw_r_a_LIBADD=$(LCMS_LIBS)
noinst_HEADERS=internal/defines.h internal/libraw_internal_funcs.h internal/var_defines.h libraw/libraw_internal.h libraw/libraw_alloc.h libraw/libraw_types.h libraw/libraw.h libraw/libraw_version.h libraw/libraw_const.h libraw/libraw_datastream.h
