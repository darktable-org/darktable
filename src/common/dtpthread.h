/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

#include "external/ThreadSafetyAnalysis.h"
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _DEBUG

// copied from darktable.h so we don't need to include the header
#include <sys/time.h>
static inline double dt_pthread_get_wtime()
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec - 1290608000 + (1.0 / 1000000.0) * time.tv_usec;
}


#define TOPN 3
typedef struct CAPABILITY("mutex") dt_pthread_mutex_t
{
  pthread_mutex_t mutex;
  char name[256];
  double time_locked;
  double time_sum_wait;
  double time_sum_locked;
  char top_locked_name[TOPN][256];
  double top_locked_sum[TOPN];
  char top_wait_name[TOPN][256];
  double top_wait_sum[TOPN];
} CAPABILITY("mutex") dt_pthread_mutex_t;

typedef struct dt_pthread_rwlock_t
{
  pthread_rwlock_t lock;
  int cnt;
  pthread_t writer;
  char name[256];
} dt_pthread_rwlock_t;

static inline int dt_pthread_mutex_destroy(dt_pthread_mutex_t *mutex)
{
  const int ret = pthread_mutex_destroy(&(mutex->mutex));
  assert(!ret);

#if 0
  printf("\n[mutex] stats for mutex `%s':\n", mutex->name);
  printf("[mutex] total time locked: %.3f secs\n", mutex->time_sum_locked);
  printf("[mutex] total wait time  : %.3f secs\n", mutex->time_sum_wait);
  printf("[mutex] top %d lockers   :\n", TOPN);
  for(int k=0; k<TOPN; k++) printf("[mutex]  %.3f secs : `%s'\n", mutex->top_locked_sum[k],
  mutex->top_locked_name[k]);
  printf("[mutex] top %d waiters   :\n", TOPN);
  for(int k=0; k<TOPN; k++) printf("[mutex]  %.3f secs : `%s'\n", mutex->top_wait_sum[k],
  mutex->top_wait_name[k]);
#endif

  return ret;
}

#define dt_pthread_mutex_init(A, B) dt_pthread_mutex_init_with_caller(A, B, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_init_with_caller(dt_pthread_mutex_t *mutex,
                                                    const pthread_mutexattr_t *attr, const char *file,
                                                    const int line, const char *function)
{
  memset(mutex, 0x0, sizeof(dt_pthread_mutex_t));
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
#if defined(__OpenBSD__)
  if(attr == NULL)
  {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_NORMAL);
    const int ret = pthread_mutex_init(&(mutex->mutex), &a);
    pthread_mutexattr_destroy(&a);
    return ret;
  }
#endif
  const int ret = pthread_mutex_init(&(mutex->mutex), attr);
  assert(!ret);
  return ret;
}

#define dt_pthread_mutex_lock(A) dt_pthread_mutex_lock_with_caller(A, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_lock_with_caller(dt_pthread_mutex_t *mutex, const char *file,
                                                    const int line, const char *function)
  ACQUIRE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  const double t0 = dt_pthread_get_wtime();
  const int ret = pthread_mutex_lock(&(mutex->mutex));
  assert(!ret);
  mutex->time_locked = dt_pthread_get_wtime();
  double wait = mutex->time_locked - t0;
  mutex->time_sum_wait += wait;
  char *name = mutex->name;
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
  int min_wait_slot = 0;
  for(int k = 0; k < TOPN; k++)
  {
    if(mutex->top_wait_sum[k] < mutex->top_wait_sum[min_wait_slot]) min_wait_slot = k;
    if(!strncmp(name, mutex->top_wait_name[k], 256))
    {
      mutex->top_wait_sum[k] += wait;
      return ret;
    }
  }
  g_strlcpy(mutex->top_wait_name[min_wait_slot], name, sizeof(mutex->top_wait_name[min_wait_slot]));
  mutex->top_wait_sum[min_wait_slot] = wait;
  return ret;
}

#define dt_pthread_mutex_trylock(A) dt_pthread_mutex_trylock_with_caller(A, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_trylock_with_caller(dt_pthread_mutex_t *mutex, const char *file,
                                                       const int line, const char *function)
  TRY_ACQUIRE(0, mutex)
{
  const double t0 = dt_pthread_get_wtime();
  const int ret = pthread_mutex_trylock(&(mutex->mutex));
  assert(!ret || (ret == EBUSY));
  if(ret) return ret;
  mutex->time_locked = dt_pthread_get_wtime();
  double wait = mutex->time_locked - t0;
  mutex->time_sum_wait += wait;
  char *name = mutex->name;
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
  int min_wait_slot = 0;
  for(int k = 0; k < TOPN; k++)
  {
    if(mutex->top_wait_sum[k] < mutex->top_wait_sum[min_wait_slot]) min_wait_slot = k;
    if(!strncmp(name, mutex->top_wait_name[k], 256))
    {
      mutex->top_wait_sum[k] += wait;
      return ret;
    }
  }
  g_strlcpy(mutex->top_wait_name[min_wait_slot], name, sizeof(mutex->top_wait_name[min_wait_slot]));
  mutex->top_wait_sum[min_wait_slot] = wait;
  return ret;
}

#define dt_pthread_mutex_unlock(A) dt_pthread_mutex_unlock_with_caller(A, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_unlock_with_caller(dt_pthread_mutex_t *mutex, const char *file,
                                                      const int line, const char *function)
  RELEASE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  const double t0 = dt_pthread_get_wtime();
  const double locked = t0 - mutex->time_locked;
  mutex->time_sum_locked += locked;

  char *name = mutex->name;
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
  int min_locked_slot = 0;
  for(int k = 0; k < TOPN; k++)
  {
    if(mutex->top_locked_sum[k] < mutex->top_locked_sum[min_locked_slot]) min_locked_slot = k;
    if(!strncmp(name, mutex->top_locked_name[k], 256))
    {
      mutex->top_locked_sum[k] += locked;
      min_locked_slot = -1;
      break;
    }
  }
  if(min_locked_slot >= 0)
  {
    g_strlcpy(mutex->top_locked_name[min_locked_slot], name, sizeof(mutex->top_locked_name[min_locked_slot]));
    mutex->top_locked_sum[min_locked_slot] = locked;
  }

  // need to unlock last, to shield our internal data.
  const int ret = pthread_mutex_unlock(&(mutex->mutex));
  assert(!ret);
  return ret;
}

static inline int dt_pthread_cond_wait(pthread_cond_t *cond, dt_pthread_mutex_t *mutex)
{
  return pthread_cond_wait(cond, &(mutex->mutex));
}


static inline int dt_pthread_rwlock_init(dt_pthread_rwlock_t *lock,
    const pthread_rwlockattr_t *attr)
{
  memset(lock, 0, sizeof(dt_pthread_rwlock_t));
  lock->cnt = 0;
  const int res = pthread_rwlock_init(&lock->lock, attr);
  assert(!res);
  return res;
}

static inline int dt_pthread_rwlock_destroy(dt_pthread_rwlock_t *lock)
{
  snprintf(lock->name, sizeof(lock->name), "destroyed with cnt %d", lock->cnt);
  const int res = pthread_rwlock_destroy(&lock->lock);
  assert(!res);
  return res;
}

static inline pthread_t dt_pthread_rwlock_get_writer(dt_pthread_rwlock_t *lock)
{
  return lock->writer;
}

#define dt_pthread_rwlock_unlock(A) dt_pthread_rwlock_unlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_unlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  const int res = pthread_rwlock_unlock(&rwlock->lock);

  assert(!res);

  __sync_fetch_and_sub(&(rwlock->cnt), 1);
  assert(rwlock->cnt >= 0);
  __sync_bool_compare_and_swap(&(rwlock->writer), pthread_self(), 0);
  if(!res) snprintf(rwlock->name, sizeof(rwlock->name), "u:%s:%d", file, line);
  return res;
}

#define dt_pthread_rwlock_rdlock(A) dt_pthread_rwlock_rdlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_rdlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  const int res = pthread_rwlock_rdlock(&rwlock->lock);
  assert(!res);
  assert(!(res && pthread_equal(rwlock->writer, pthread_self())));
  __sync_fetch_and_add(&(rwlock->cnt), 1);
  if(!res)
    snprintf(rwlock->name, sizeof(rwlock->name), "r:%s:%d", file, line);
  return res;
}
#define dt_pthread_rwlock_wrlock(A) dt_pthread_rwlock_wrlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_wrlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  const int res = pthread_rwlock_wrlock(&rwlock->lock);
  assert(!res);
  __sync_fetch_and_add(&(rwlock->cnt), 1);
  if(!res)
  {
    __sync_lock_test_and_set(&(rwlock->writer), pthread_self());
    snprintf(rwlock->name, sizeof(rwlock->name), "w:%s:%d", file, line);
  }
  return res;
}
#define dt_pthread_rwlock_tryrdlock(A) dt_pthread_rwlock_tryrdlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_tryrdlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  const int res = pthread_rwlock_tryrdlock(&rwlock->lock);
  assert(!res || (res == EBUSY));
  assert(!(res && pthread_equal(rwlock->writer, pthread_self())));
  if(!res)
  {
    __sync_fetch_and_add(&(rwlock->cnt), 1);
    snprintf(rwlock->name, sizeof(rwlock->name), "tr:%s:%d", file, line);
  }
  return res;
}
#define dt_pthread_rwlock_trywrlock(A) dt_pthread_rwlock_trywrlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_trywrlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  const int res = pthread_rwlock_trywrlock(&rwlock->lock);
  assert(!res || (res == EBUSY));
  if(!res)
  {
    __sync_fetch_and_add(&(rwlock->cnt), 1);
    __sync_lock_test_and_set(&(rwlock->writer), pthread_self());
    snprintf(rwlock->name, sizeof(rwlock->name), "tw:%s:%d", file, line);
  }
  return res;
}

#undef TOPN
#else

typedef struct CAPABILITY("mutex") dt_pthread_mutex_t
{
  pthread_mutex_t mutex;
} CAPABILITY("mutex") dt_pthread_mutex_t;

// *please* do use these;
static inline int dt_pthread_mutex_init(dt_pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr)
{
  return pthread_mutex_init(&mutex->mutex, mutexattr);
};

static inline int dt_pthread_mutex_lock(dt_pthread_mutex_t *mutex) ACQUIRE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  return pthread_mutex_lock(&mutex->mutex);
};

static inline int dt_pthread_mutex_trylock(dt_pthread_mutex_t *mutex) TRY_ACQUIRE(0, mutex)
{
  return pthread_mutex_trylock(&mutex->mutex);
};

static inline int dt_pthread_mutex_unlock(dt_pthread_mutex_t *mutex) RELEASE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  return pthread_mutex_unlock(&mutex->mutex);
};

static inline int dt_pthread_mutex_destroy(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_destroy(&mutex->mutex);
};

static inline int dt_pthread_cond_wait(pthread_cond_t *cond, dt_pthread_mutex_t *mutex)
{
  return pthread_cond_wait(cond, &mutex->mutex);
};

#define dt_pthread_rwlock_t pthread_rwlock_t
#define dt_pthread_rwlock_init pthread_rwlock_init
#define dt_pthread_rwlock_destroy pthread_rwlock_destroy
#define dt_pthread_rwlock_unlock pthread_rwlock_unlock
#define dt_pthread_rwlock_rdlock pthread_rwlock_rdlock
#define dt_pthread_rwlock_wrlock pthread_rwlock_wrlock
#define dt_pthread_rwlock_tryrdlock pthread_rwlock_tryrdlock
#define dt_pthread_rwlock_trywrlock pthread_rwlock_trywrlock

#define dt_pthread_rwlock_rdlock_with_caller(A,B,C) pthread_rwlock_rdlock(A)
#define dt_pthread_rwlock_wrlock_with_caller(A,B,C) pthread_rwlock_wrlock(A)
#define dt_pthread_rwlock_tryrdlock_with_caller(A,B,C) pthread_rwlock_tryrdlock(A)
#define dt_pthread_rwlock_trywrlock_with_caller(A,B,C) pthread_rwlock_trywrlock(A)

#endif

// if at all possible, do NOT use.
static inline int dt_pthread_mutex_BAD_lock(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_lock(&mutex->mutex);
};

static inline int dt_pthread_mutex_BAD_trylock(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_trylock(&mutex->mutex);
};

static inline int dt_pthread_mutex_BAD_unlock(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_unlock(&mutex->mutex);
};

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg);

void dt_pthread_setname(const char *name);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

