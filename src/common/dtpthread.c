/*
    This file is part of darktable,
    copyright (c) 2009--2014 johannes hanika.

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

#include <sys/resource.h>

#include "common/dtpthread.h"
#include <sys/time.h>
#include <sys/resource.h>

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void*), void *arg)
{
  pthread_attr_t attr;
  size_t stacksize;
  int ret;
  struct rlimit rlim;

  pthread_attr_init(&attr);
  pthread_attr_getstacksize(&attr, &stacksize);

  ret = getrlimit(RLIMIT_STACK, &rlim);

  if(ret != 0)
  {
    rlim.rlim_cur = 8 * 1024 * 1024;
  }

  if(stacksize < rlim.rlim_cur)
  {
    pthread_attr_setstacksize(&attr, rlim.rlim_cur);
  }

  ret = pthread_create(thread, &attr, start_routine, arg);

  pthread_attr_destroy(&attr);
  
  return ret;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
