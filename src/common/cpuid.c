/*
    This file is part of darktable,
    Copyright (C) 2014-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.

    Original sources:
    LensFun library,
    original file lensfun/libs/lensfun/cpuid.cpp, licensed under LGPL-3
        Copyright (C) 2010 by Andrew Zabolotny
    which was taken from RawStudio,
    original file rawstudio/librawstudio/rs-utils.c, licensed under GPL-2+
        Copyright (C) 2006-2011 Anders Brander <anders@brander.dk>,
        Anders Kvist <akv@lnxbx.dk> and Klaus Post <klauspost@gmail.com>
 */

#include "config.h"
#include "cpuid.h"
#include "common/darktable.h"
#include <glib.h>

#ifdef HAVE_CPUID_H
#include <cpuid.h>
#endif

#if defined(HAVE___GET_CPUID)
dt_cpu_flags_t dt_detect_cpu_features()
{
  guint32 ax, bx, cx, dx;
  static dt_cpu_flags_t cpuflags = 0;
  static GMutex lock;

  g_mutex_lock(&lock);
  if(__get_cpuid(0x00000000,&ax,&bx,&cx,&dx))
  {
    /* Request for standard features */
    if(__get_cpuid(0x00000001,&ax,&bx,&cx,&dx))
    {
      if(dx & 0x00800000) cpuflags |= CPU_FLAG_MMX;
      if(dx & 0x02000000) cpuflags |= CPU_FLAG_SSE;
      if(dx & 0x04000000) cpuflags |= CPU_FLAG_SSE2;
      if(dx & 0x00008000) cpuflags |= CPU_FLAG_CMOV;

      if(cx & 0x00000001) cpuflags |= CPU_FLAG_SSE3;
      if(cx & 0x00000200) cpuflags |= CPU_FLAG_SSSE3;
      if(cx & 0x00040000) cpuflags |= CPU_FLAG_SSE4_1;
      if(cx & 0x00080000) cpuflags |= CPU_FLAG_SSE4_2;

      if(cx & 0x08000000) cpuflags |= CPU_FLAG_AVX;
    }

    /* Are there extensions? */
    if(__get_cpuid(0x80000000,&ax,&bx,&cx,&dx))
    {
      /* Ask extensions */
      if(__get_cpuid(0x80000001,&ax,&bx,&cx,&dx))
      {
        if(dx & 0x80000000) cpuflags |= CPU_FLAG_3DNOW;
        if(dx & 0x40000000) cpuflags |= CPU_FLAG_3DNOW_EXT;
        if(dx & 0x00400000) cpuflags |= CPU_FLAG_AMD_ISSE;
      }
    }
    fprintf(stderr,"\nfound cpuid instruction, dtflags %x",cpuflags);
  }
  g_mutex_unlock(&lock);
  return cpuflags;
}
#else
dt_cpu_flags_t dt_detect_cpu_features()
{
  static dt_cpu_flags_t cpuflags = 0;

  fprintf(stderr, "[dt_detect_cpu_features] Not implemented for this architecture.\n");
  fprintf(stderr, "[dt_detect_cpu_features] Please contribute a patch.\n");

  return cpuflags;
}
#endif /* defined(HAVE___GET_CPUID) */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

