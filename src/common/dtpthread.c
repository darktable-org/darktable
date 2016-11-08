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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg)
{
  int ret;

  struct rlimit rlim = { 0 };

  ret = getrlimit(RLIMIT_STACK, &rlim);

  if(ret != 0)
  {
    const int errsv = errno;
    fprintf(stderr, "[dt_pthread_create] error: getrlimit(RLIMIT_STACK) returned %i: %i (%s)\n", ret, errsv,
            strerror(errsv));
  }

  assert((ret == 0 && WANTED_THREADS_STACK_SIZE <= rlim.rlim_max) || (ret != 0));

  if(ret != 0 || rlim.rlim_cur < WANTED_THREADS_STACK_SIZE /*|| 1*/)
  {
    // looks like we need to bump/set it...

    fprintf(stderr, "[dt_pthread_create] ERROR: RLIMIT_STACK rlim_cur is less than we thought it is: %ju < %i\n",
            (uintmax_t)rlim.rlim_cur, WANTED_THREADS_STACK_SIZE);

    rlim.rlim_cur = WANTED_THREADS_STACK_SIZE;
  }

  pthread_attr_t attr;

  ret = pthread_attr_init(&attr);
  if(ret != 0)
  {
    fprintf(stderr, "[dt_pthread_create] error: pthread_attr_init() returned %i\n", ret);
    return ret;
  }

  size_t stacksize;

  ret = pthread_attr_getstacksize(&attr, &stacksize);

  if(ret != 0)
  {
    fprintf(stderr, "[dt_pthread_create] error: pthread_attr_getstacksize() returned %i\n", ret);
  }

  if(ret != 0 || stacksize < rlim.rlim_cur)
  {
    // looks like we need to bump/set it...

    fprintf(stderr, "[dt_pthread_create] info: bumping pthread's stacksize from %zu to %ju\n", stacksize,
            (uintmax_t)rlim.rlim_cur);

    ret = pthread_attr_setstacksize(&attr, rlim.rlim_cur);
    if(ret != 0)
    {
      fprintf(stderr, "[dt_pthread_create] error: pthread_attr_setstacksize() returned %i\n", ret);
    }
  }

  ret = pthread_create(thread, &attr, start_routine, arg);

  pthread_attr_destroy(&attr);

  return ret;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
