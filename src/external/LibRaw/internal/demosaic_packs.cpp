/* 
  Copyright 2008-2010 LibRaw LLC (info@libraw.org)

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of three licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

3. LibRaw Software License 27032010
   (See file LICENSE.LibRaw.pdf provided in LibRaw distribution archive for details).

   This file is generated from Dave Coffin's dcraw.c
   dcraw.c -- Dave Coffin's raw photo decoder
   Copyright 1997-2010 by Dave Coffin, dcoffin a cybercom o net

   Look into dcraw homepage (probably http://cybercom.net/~dcoffin/dcraw/)
   for more information
*/

#include <math.h>

#define CLASS LibRaw::

#include "libraw/libraw_types.h"
#define LIBRAW_LIBRARY_BUILD
#define LIBRAW_IO_REDEFINED
#include "libraw/libraw.h"
#include "internal/defines.h"
#define SRC_USES_SHRINK
#define SRC_USES_BLACK
#define SRC_USES_CURVE

#include "internal/var_defines.h"

// api stubs
void CLASS ahd_interpolate_mod() {}
void CLASS afd_interpolate_pl(int, int) {}
void CLASS vcd_interpolate(int) {}
void CLASS lmmse_interpolate(int) {}
void CLASS es_median_filter() {}
void CLASS median_filter_new() {}
void CLASS refinement() {}

void CLASS fbdd(int) {}
void CLASS dcb(int, int) {}

void CLASS CA_correct_RT(float,float) {}
void CLASS amaze_demosaic_RT() {}
void CLASS green_equilibrate(float thresh) {}
void CLASS cfa_linedn(float linenoise) {}
void CLASS cfa_impulse_gauss(float lclean, float cclean) {}

void CLASS foveon_interpolate() {}
void CLASS foveon_load_raw() {}
void CLASS parse_foveon() {}
void CLASS foveon_thumb_loader() {}
void CLASS foveon_thumb() {}
