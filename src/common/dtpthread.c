/*
    This file is part of darktable,
    Copyright (C) 2016-2024 darktable developers.

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
#include <stdlib.h>
#include <locale.h>
#include <inttypes.h>

#ifdef _WIN32
#include "win/dtwin.h"
#endif // _WIN32

#include "common/dtpthread.h"

void dt_pthread_setlocale(void)
{
#if !defined(_WIN32)
  // a private copy of the locale the process was started with; this thread's
  // gettext() reads it instead of the global locale, so it is immune to
  // setlocale() called concurrently on any other thread. Lives for the whole
  // thread and is intentionally never freed.
  const locale_t loc = newlocale(LC_ALL_MASK, "", (locale_t)0);
  if(loc) uselocale(loc);
#endif
}

// Trampoline used by dt_pthread_create() so that every darktable-spawned thread
// pins its locale before running its actual work (see dt_pthread_setlocale()).
typedef struct _dt_pthread_start_t
{
  void *(*start_routine)(void *);
  void *arg;
} _dt_pthread_start_t;

static void *_dt_pthread_start(void *arg)
{
  _dt_pthread_start_t *params = (_dt_pthread_start_t *)arg;
  void *(*start_routine)(void *) = params->start_routine;
  void *real_arg = params->arg;
  free(params);

  dt_pthread_setlocale();

  return start_routine(real_arg);
}

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg)
{
  int ret = 0;

  pthread_attr_t attr;
  ret = pthread_attr_init(&attr);
  if(ret != 0)
  {
    printf("[dt_pthread_create] error: pthread_attr_init() returned %s\n", _pthread_ret_mess(ret));
    fflush(stdout);
  }
  assert(ret == 0);

  size_t stacksize;
  ret = pthread_attr_getstacksize(&attr, &stacksize);
  if(ret != 0 || stacksize < WANTED_THREADS_STACK_SIZE)
  {
    // looks like we need to bump/set it...
    ret = pthread_attr_setstacksize(&attr, WANTED_THREADS_STACK_SIZE);
    if(ret != 0)
    {
      printf("[dt_pthread_create] error: pthread_attr_setstacksize() returned %s\n", _pthread_ret_mess(ret));
      fflush(stdout);
    }
  }
  assert(ret == 0);

  // wrap the thread entry so it pins a private locale before running (the
  // trampoline frees params; on failure we free it here since it won't run)
  _dt_pthread_start_t *params = malloc(sizeof(_dt_pthread_start_t));
  assert(params);
  params->start_routine = start_routine;
  params->arg = arg;

  ret = pthread_create(thread, &attr, _dt_pthread_start, params);
  if(ret != 0)
  {
    free(params);
    printf("[dt_pthread_create] error: pthread_create() returned %s\n", _pthread_ret_mess(ret));
    fflush(stdout);
  }

  pthread_attr_destroy(&attr);
  assert(ret == 0);
  return ret;
}

int dt_pthread_join(pthread_t thread)
{
#if defined(MUTEX_REPORTING) && ( defined(__linux__) || defined(__APPLE__) )
  char name[256] = { "???" };
  pthread_getname_np(thread, name, sizeof(name));
  const int ret = pthread_join(thread, NULL);
  printf("[dt_pthread_join] '%s' returned %s\n",
            name, _pthread_ret_mess(ret));
  fflush(stdout);
#else
  const int ret = pthread_join(thread, NULL);
#endif

  assert(ret == 0);
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

