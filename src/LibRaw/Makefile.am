noinst_LTLIBRARIES=libraw_r.la
libraw_r_la_SOURCES=internal/dcraw_common.cpp src/libraw_cxx.cpp internal/dcraw_fileio.cpp src/libraw_c_api.cpp
libraw_r_la_CPPFLAGS=-I$(srcdir) -Wall $(OPENMP_CFLAGS) -fPIC -g
libraw_r_la_CFLAGS=-I$(srcdir) -Wall $(OPENMP_CFLAGS) -g
# libraw_r_a_LIBADD=$(LCMS_LIBS)
noinst_HEADERS=internal/defines.h internal/libraw_internal_funcs.h internal/var_defines.h libraw/libraw_internal.h libraw/libraw_alloc.h libraw/libraw_types.h libraw/libraw.h libraw/libraw_version.h libraw/libraw_const.h libraw/libraw_datastream.h internal/libraw_internal_funcs_dcb.h internal/libraw_internal_funcs_vcd.h internal/libraw_internal_funcs_amaze.h internal/ahd_interpolate_mod.c internal/ahd_partial_interpolate.c internal/amaze_ca_correct.c internal/amaze_demosaicing.c internal/dcb_demosaicing.c internal/es_median_filter.c internal/median_filter_new.c internal/refinement.c internal/vcd_interpolate.c 
EXTRA_DIST=dcraw/dcraw.c
