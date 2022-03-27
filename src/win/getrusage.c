/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

/*
 * Copyright (c) 2014 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "win/win.h"
#include <config.h>
#include <stdio.h>
#include <errno.h>
#include <psapi.h>
#include <sys/time.h>
#include "getrusage.h"

static void usage_to_timeval(FILETIME *ft, struct timeval *tv)
{
  ULARGE_INTEGER time;
  time.LowPart = ft->dwLowDateTime;
  time.HighPart = ft->dwHighDateTime;

  tv->tv_sec = time.QuadPart / 10000000;
  tv->tv_usec = (time.QuadPart % 10000000) / 10;
}

int getrusage(int who, struct rusage *usage)
{
  FILETIME creation_time, exit_time, kernel_time, user_time;
  PROCESS_MEMORY_COUNTERS pmc;

  memset(usage, 0, sizeof(struct rusage));

  if(who == RUSAGE_SELF)
  {
    if(!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time))
    {
      fprintf(stdout, "failed at GetProcessTimes\n");
      return -1;
    }

    if(!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
      fprintf(stdout, "failed at GetProcessMemoryInfo\n");
      return -1;
    }

    usage_to_timeval(&kernel_time, &usage->ru_stime);
    usage_to_timeval(&user_time, &usage->ru_utime);
    usage->ru_majflt = pmc.PageFaultCount;
    usage->ru_maxrss = pmc.PeakWorkingSetSize / 1024;
    return 0;
  }
  else if(who == RUSAGE_THREAD)
  {
    if(!GetThreadTimes(GetCurrentThread(), &creation_time, &exit_time, &kernel_time, &user_time))
    {
      fprintf(stdout, "failed at GetThreadTimes\n");
      return -1;
    }
    usage_to_timeval(&kernel_time, &usage->ru_stime);
    usage_to_timeval(&user_time, &usage->ru_utime);
    return 0;
  }
  else
  {
    return -1;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

