/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI.

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

#include "cpuid.h"
#include "common/darktable.h"
#include "config.h"
#include <glib.h>

#if defined(__i386__) || defined(__x86_64__)

#ifdef __x86_64__
#define R_AX "rax"
#define R_BX "rbx"
#define R_CX "rcx"
#define R_DX "rdx"
#else
#define R_AX "eax"
#define R_BX "ebx"
#define R_CX "ecx"
#define R_DX "edx"
#endif

dt_cpu_flags_t dt_detect_cpu_features()
{
#define cpuid(cmd) \
  __asm volatile("push %%" R_BX "\n"                                                                         \
                 "cpuid\n"                                                                                   \
                 "pop %%" R_BX "\n"                                                                          \
                 : "=a"(ax), "=c"(cx), "=d"(dx)                                                              \
                 : "0"(cmd))

#ifdef __x86_64__
  guint64 ax, cx, dx, tmp;
#else
  guint32 ax, cx, dx, tmp;
#endif

  static dt_cpu_flags_t cpuflags = -1;
  static GMutex lock;

  g_mutex_lock(&lock);
  if(cpuflags == (dt_cpu_flags_t)-1)
  {
    cpuflags = 0;

    /* Test cpuid presence by checking bit 21 of eflags */
    __asm volatile("pushf\n"
                   "pop     %0\n"
                   "mov     %0, %1\n"
                   "xor     $0x00200000, %0\n"
                   "push    %0\n"
                   "popf\n"
                   "pushf\n"
                   "pop     %0\n"
                   "cmp     %0, %1\n"
                   "setne   %%al\n"
                   "movzb   %%al, %0\n"
                   : "=r"(ax), "=r"(tmp));

    if(ax)
    {
      /* Get the standard level */
      cpuid(0x00000000);

      if(ax)
      {
        /* Request for standard features */
        cpuid(0x00000001);

        if(dx & 0x00800000) cpuflags |= CPU_FLAG_MMX;
        if(dx & 0x02000000) cpuflags |= CPU_FLAG_SSE;
        if(dx & 0x04000000) cpuflags |= CPU_FLAG_SSE2;
        if(dx & 0x00008000) cpuflags |= CPU_FLAG_CMOV;

        if(cx & 0x00000001) cpuflags |= CPU_FLAG_SSE3;
        if(cx & 0x00000200) cpuflags |= CPU_FLAG_SSSE3;
        if(cx & 0x00040000) cpuflags |= CPU_FLAG_SSE4_1;
        if(cx & 0x00080000) cpuflags |= CPU_FLAG_SSE4_2;
      }

      /* Are there extensions? */
      cpuid(0x80000000);

      if(ax)
      {
        /* Ask extensions */
        cpuid(0x80000001);

        if(dx & 0x80000000) cpuflags |= CPU_FLAG_3DNOW;
        if(dx & 0x40000000) cpuflags |= CPU_FLAG_3DNOW_EXT;
        if(dx & 0x00400000) cpuflags |= CPU_FLAG_AMD_ISSE;
      }
    }
  }
  g_mutex_unlock(&lock);

#if 0
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
#define report(a, x) dt_print(DT_DEBUG_PERF, "CPU Feature: " a " = %d\n", !!(cpuflags & x));
    report("MMX", CPU_FLAG_MMX);
    report("SSE", CPU_FLAG_SSE);
    report("CMOV", CPU_FLAG_CMOV);
    report("3DNOW", CPU_FLAG_3DNOW);
    report("3DNOW_EXT", CPU_FLAG_3DNOW_EXT);
    report("Integer SSE", CPU_FLAG_AMD_ISSE);
    report("SSE2", CPU_FLAG_SSE2);
    report("SSE3", CPU_FLAG_SSE3);
    report("SSSE3", CPU_FLAG_SSSE3);
    report("SSE4.1", CPU_FLAG_SSE4_1);
    report("SSE4.2", CPU_FLAG_SSE4_2);
    report("AVX", CPU_FLAG_AVX);
#undef report
  }
#endif

  return cpuflags;

#undef cpuid
}
#else
dt_cpu_flags_t dt_detect_cpu_features()
{
  static dt_cpu_flags_t cpuflags = 0;

  fprintf(stderr, "[dt_detect_cpu_features] Not implemented for this architecture.\n");
  fprintf(stderr, "[dt_detect_cpu_features] Please contribute a patch.\n");

  return cpuflags;
}
#endif /* __i386__ || __x86_64__ */

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
