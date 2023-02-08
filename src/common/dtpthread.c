/*
    This file is part of darktable,
    Copyright (C) 2016-2023 darktable developers.

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

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#ifdef _WIN32
#include "win/dtwin.h"
#endif // _WIN32

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg)
{
  int ret;

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

  if(ret != 0 || stacksize < WANTED_THREADS_STACK_SIZE /*|| 1*/)
  {
    // looks like we need to bump/set it...
    ret = pthread_attr_setstacksize(&attr, WANTED_THREADS_STACK_SIZE);
    if(ret != 0)
    {
      fprintf(stderr, "[dt_pthread_create] error: pthread_attr_setstacksize() returned %i\n", ret);
    }
  }

  ret = pthread_create(thread, &attr, start_routine, arg);

  pthread_attr_destroy(&attr);

  return ret;
}

void dt_pthread_setname(const char *name)
{
#if defined __linux__
  pthread_setname_np(pthread_self(), name);
#elif defined __FreeBSD__ || defined __DragonFly__
  // TODO: is this the right syntax?
  // pthread_setname_np(pthread_self(), name, 0);
#elif defined __NetBSD__
  // TODO: is this the right syntax?
  // pthread_setname_np(pthread_self(), name, NULL);
#elif defined __OpenBSD__
  // TODO: find out if there is pthread_setname_np() on OpenBSD and how to call it
#elif defined __APPLE__
  pthread_setname_np(name);
#elif defined _WIN32
  dtwin_set_thread_name((DWORD)-1, name);
#endif
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

