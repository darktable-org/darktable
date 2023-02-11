/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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
*/

#pragma once

#include <stdio.h>

#define RLIMIT_CPU 0    /* limit on CPU time per process */
#define RLIMIT_FSIZE 1  /* limit on file size */
#define RLIMIT_DATA 2   /* limit on data segment size */
#define RLIMIT_STACK 3  /* limit on process stack size */
#define RLIMIT_CORE 4   /* limit on size of core dump file */
#define RLIMIT_NOFILE 5 /* limit on number of open files */
#define RLIMIT_AS 6     /* limit on process total address space size */
#define RLIMIT_VMEM RLIMIT_AS
#define RLIM_NLIMITS 7
#define RLIM_INFINITY (~0UL)

/*
 * process resource limits definitions
 */

struct rlimit
{
  //        LARGE_INTEGER  rlim_cur;
  //        LARGE_INTEGER  rlim_max;
  __int64 rlim_cur;
  __int64 rlim_max;
};

typedef struct rlimit rlimit_t;

/*
 * Prototypes
 */
int getrlimit(int resource, struct rlimit *);
int setrlimit(int resource, const struct rlimit *);

size_t rfwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int _rwrite(int handle, const void *buffer, unsigned int count);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
