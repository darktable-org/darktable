/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2010--2013 henrik andersson.
    Copyright (c) 2012 James C. McPherson
    copyright (c) 2014 tobias ellinghaus.

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

#include "control/jobs.h"
#include "control/control.h"

#define DT_CONTROL_FG_PRIORITY 4
#define DT_CONTROL_MAX_JOBS 30

/* the queue can have scheduled jobs but all
    the workers are sleeping, so this kicks the workers
    on timed interval.
*/
typedef struct worker_thread_parameters_t
{
  dt_control_t *self;
  int32_t threadid;
} worker_thread_parameters_t;

typedef struct _dt_job_t
{
  dt_job_execute_callback execute;
  void *params;
  int32_t result;

  dt_pthread_mutex_t state_mutex;
  dt_pthread_mutex_t wait_mutex;

  dt_job_state_t state;
  unsigned char priority;
  dt_job_queue_t queue;

  dt_job_state_change_callback state_changed_cb;

  char description[DT_CONTROL_DESCRIPTION_LEN];
} _dt_job_t;

/** check if two jobs are to be considered equal. a simple memcmp won't work since the mutexes probably won't
   match
    we don't want to compare result, priority or state since these will change during the course of
   processing.
    TODO: somehow compare params. maybe we have to pass the sizeof(params) when setting the params to do a
   memcmp, or maybe even
          allow to pass a comparator for that.
 */
static inline int dt_control_job_equal(_dt_job_t *j1, _dt_job_t *j2)
{
  return (j1->execute == j2->execute && j1->state_changed_cb == j2->state_changed_cb && j1->queue == j2->queue
          && g_strcmp0(j1->description, j2->description));
}

static void dt_control_job_set_state(_dt_job_t *job, dt_job_state_t state)
{
  if(!job) return;
  dt_pthread_mutex_lock(&job->state_mutex);
  job->state = state;
  /* pass state change to callback */
  if(job->state_changed_cb) job->state_changed_cb(job, state);
  dt_pthread_mutex_unlock(&job->state_mutex);
}

dt_job_state_t dt_control_job_get_state(_dt_job_t *job)
{
  if(!job) return DT_JOB_STATE_DISPOSED;
  dt_pthread_mutex_lock(&job->state_mutex);
  dt_job_state_t state = job->state;
  dt_pthread_mutex_unlock(&job->state_mutex);
  return state;
}

void dt_control_job_set_params(_dt_job_t *job, void *params)
{
  if(!job || dt_control_job_get_state(job) != DT_JOB_STATE_INITIALIZED) return;
  job->params = params;
}

void *dt_control_job_get_params(const _dt_job_t *job)
{
  if(!job) return NULL;
  return job->params;
}

dt_job_t *dt_control_job_create(dt_job_execute_callback execute, const char *msg, ...)
{
  _dt_job_t *job = (_dt_job_t *)calloc(1, sizeof(_dt_job_t));
  if(!job) return NULL;

  va_list ap;
  va_start(ap, msg);
  vsnprintf(job->description, DT_CONTROL_DESCRIPTION_LEN, msg, ap);
  va_end(ap);

  job->execute = execute;
  job->state = DT_JOB_STATE_INITIALIZED;
  dt_pthread_mutex_init(&job->state_mutex, NULL);
  dt_pthread_mutex_init(&job->wait_mutex, NULL);
  return job;
}

void dt_control_job_dispose(_dt_job_t *job)
{
  if(!job) return;
  dt_control_job_set_state(job, DT_JOB_STATE_DISPOSED);
  dt_pthread_mutex_destroy(&job->state_mutex);
  dt_pthread_mutex_destroy(&job->wait_mutex);
  free(job);
}

void dt_control_job_set_state_callback(_dt_job_t *job, dt_job_state_change_callback cb)
{
  // once the job got added to the queue it may not be changed from the outside
  if(dt_control_job_get_state(job) != DT_JOB_STATE_INITIALIZED)
    return; // get_state returns DISPOSED when job == NULL
  job->state_changed_cb = cb;
}


static void dt_control_job_print(_dt_job_t *job)
{
  if(!job) return;
  dt_print(DT_DEBUG_CONTROL, "%s | queue: %d | priority: %d", job->description, job->queue, job->priority);
}

void dt_control_job_cancel(_dt_job_t *job)
{
  dt_control_job_set_state(job, DT_JOB_STATE_CANCELLED);
}

void dt_control_job_wait(_dt_job_t *job)
{
  if(!job) return;
  dt_job_state_t state = dt_control_job_get_state(job);

  /* if job execution is not finished let's wait for signal */
  if(state == DT_JOB_STATE_RUNNING || state == DT_JOB_STATE_CANCELLED)
  {
    dt_pthread_mutex_lock(&job->wait_mutex);
    dt_pthread_mutex_unlock(&job->wait_mutex);
  }
}

static int32_t dt_control_run_job_res(dt_control_t *control, int32_t res)
{
  if(((unsigned int)res) >= DT_CTL_WORKER_RESERVED) return -1;

  _dt_job_t *job = NULL;
  dt_pthread_mutex_lock(&control->queue_mutex);
  if(control->new_res[res])
  {
    job = control->job_res[res];
    control->job_res[res] = NULL; // this job belongs to us now, the queue may not touch it any longer
  }
  control->new_res[res] = 0;
  dt_pthread_mutex_unlock(&control->queue_mutex);
  if(!job) return -1;

  /* change state to running */
  dt_pthread_mutex_lock(&job->wait_mutex);
  if(dt_control_job_get_state(job) == DT_JOB_STATE_QUEUED)
  {
    dt_print(DT_DEBUG_CONTROL, "[run_job+] %02d %f ", res, dt_get_wtime());
    dt_control_job_print(job);
    dt_print(DT_DEBUG_CONTROL, "\n");

    dt_control_job_set_state(job, DT_JOB_STATE_RUNNING);

    /* execute job */
    job->result = job->execute(job);

    dt_control_job_set_state(job, DT_JOB_STATE_FINISHED);
    dt_print(DT_DEBUG_CONTROL, "[run_job-] %02d %f ", res, dt_get_wtime());
    dt_control_job_print(job);
    dt_print(DT_DEBUG_CONTROL, "\n");
  }
  dt_pthread_mutex_unlock(&job->wait_mutex);
  dt_control_job_dispose(job);
  return 0;
}

static _dt_job_t *dt_control_schedule_job(dt_control_t *control)
{
  /*
   * job scheduling works like this:
   * - when there is a single job in the queue head with a maximal priority -> pick it
   * - otherwise pick among the ones with the maximal priority in the following order:
   *   * user foreground
   *   * system foreground
   *   * user background
   *   * system background
   * - the jobs that didn't get picked this round get their priority incremented
   */

  dt_pthread_mutex_lock(&control->queue_mutex);

  // find the job
  _dt_job_t *job = NULL;
  int winner_queue = DT_JOB_QUEUE_MAX;
  int max_priority = -1;
  for(int i = 0; i < DT_JOB_QUEUE_MAX; i++)
  {
    if(control->queues[i] == NULL) continue;
    _dt_job_t *_job = (_dt_job_t *)control->queues[i]->data;
    if(_job->priority > max_priority)
    {
      max_priority = _job->priority;
      job = _job;
      winner_queue = i;
    }
  }

  if(!job)
  {
    dt_pthread_mutex_unlock(&control->queue_mutex);
    return NULL;
  }

  // the order of the queues in control->queues matches our priority, and we only update job when the priority
  // is strictly bigger
  // invariant -> job is the one we are looking for

  // remove the to be scheduled job from its queue
  GList **queue = &control->queues[winner_queue];
  *queue = g_list_delete_link(*queue, *queue);
  control->queue_length[winner_queue]--;

  // increment the priorities of the others
  for(int i = 0; i < DT_JOB_QUEUE_MAX; i++)
  {
    if(i == winner_queue || control->queues[i] == NULL) continue;
    ((_dt_job_t *)control->queues[i]->data)->priority++;
  }

  dt_pthread_mutex_unlock(&control->queue_mutex);

  return job;
}

static int32_t dt_control_run_job(dt_control_t *control)
{
  _dt_job_t *job = dt_control_schedule_job(control);

  if(!job) return -1;

  /* change state to running */
  dt_pthread_mutex_lock(&job->wait_mutex);
  if(dt_control_job_get_state(job) == DT_JOB_STATE_QUEUED)
  {
    dt_print(DT_DEBUG_CONTROL, "[run_job+] %02d %f ", DT_CTL_WORKER_RESERVED + dt_control_get_threadid(),
             dt_get_wtime());
    dt_control_job_print(job);
    dt_print(DT_DEBUG_CONTROL, "\n");

    dt_control_job_set_state(job, DT_JOB_STATE_RUNNING);

    /* execute job */
    job->result = job->execute(job);

    dt_control_job_set_state(job, DT_JOB_STATE_FINISHED);

    dt_print(DT_DEBUG_CONTROL, "[run_job-] %02d %f ", DT_CTL_WORKER_RESERVED + dt_control_get_threadid(),
             dt_get_wtime());
    dt_control_job_print(job);
    dt_print(DT_DEBUG_CONTROL, "\n");
  }

  /* free job */
  dt_pthread_mutex_unlock(&job->wait_mutex);
  dt_control_job_dispose(job);

  return 0;
}

int32_t dt_control_add_job_res(dt_control_t *control, _dt_job_t *job, int32_t res)
{
  if(((unsigned int)res) >= DT_CTL_WORKER_RESERVED || !job)
  {
    dt_control_job_dispose(job);
    return 1;
  }

  // TODO: pthread cancel and restart in tough cases?
  dt_pthread_mutex_lock(&control->queue_mutex);

  // if there is a job in the queue we have to discard that first
  if(control->job_res[res])
  {
    dt_control_job_set_state(control->job_res[res], DT_JOB_STATE_DISCARDED);
    dt_control_job_dispose(control->job_res[res]);
  }

  dt_print(DT_DEBUG_CONTROL, "[add_job_res] %d | ", res);
  dt_control_job_print(job);
  dt_print(DT_DEBUG_CONTROL, "\n");

  dt_control_job_set_state(job, DT_JOB_STATE_QUEUED);
  control->job_res[res] = job;
  control->new_res[res] = 1;

  dt_pthread_mutex_unlock(&control->queue_mutex);

  dt_pthread_mutex_lock(&control->cond_mutex);
  pthread_cond_broadcast(&control->cond);
  dt_pthread_mutex_unlock(&control->cond_mutex);

  return 0;
}

int dt_control_add_job(dt_control_t *control, dt_job_queue_t queue_id, _dt_job_t *job)
{
  if(((unsigned int)queue_id) >= DT_JOB_QUEUE_MAX || !job)
  {
    dt_control_job_dispose(job);
    return 1;
  }

  job->queue = queue_id;

  dt_pthread_mutex_lock(&control->queue_mutex);

  GList **queue = &control->queues[queue_id];
  size_t length = control->queue_length[queue_id];

  dt_print(DT_DEBUG_CONTROL, "[add_job] %ld | ", length);
  dt_control_job_print(job);
  dt_print(DT_DEBUG_CONTROL, "\n");

  if(queue_id == DT_JOB_QUEUE_SYSTEM_FG)
  {
    // this is a stack with limited size and bubble up and all that stuff
    job->priority = DT_CONTROL_FG_PRIORITY;

    // if the job is already in the queue -> move it to the top
    for(GList *iter = *queue; iter; iter = g_list_next(iter))
    {
      _dt_job_t *other_job = (_dt_job_t *)iter->data;
      if(dt_control_job_equal(job, other_job))
      {
        dt_print(DT_DEBUG_CONTROL, "[add_job] found job already in queue: ");
        dt_control_job_print(job);
        dt_print(DT_DEBUG_CONTROL, "\n");

        *queue = g_list_delete_link(*queue, iter);
        length--;
        dt_control_job_set_state(job, DT_JOB_STATE_DISCARDED);
        dt_control_job_dispose(job);
        job = other_job;
        break; // there can't be any further copy in the list
      }
    }

    // now we can add the new job to the list
    *queue = g_list_prepend(*queue, job);
    length++;

    // and take care of the maximal queue size
    if(length > DT_CONTROL_MAX_JOBS)
    {
      GList *last = g_list_last(*queue);
      dt_control_job_set_state((_dt_job_t *)last->data, DT_JOB_STATE_DISCARDED);
      *queue = g_list_delete_link(*queue, last);
      length--;
    }

    control->queue_length[queue_id] = length;
  }
  else
  {
    // the rest are FIFOs
    if(queue_id == DT_JOB_QUEUE_USER_BG || queue_id == DT_JOB_QUEUE_SYSTEM_BG)
      job->priority = 0;
    else
      job->priority = DT_CONTROL_FG_PRIORITY;
    *queue = g_list_append(*queue, job);
    control->queue_length[queue_id]++;
  }
  dt_control_job_set_state(job, DT_JOB_STATE_QUEUED);
  dt_pthread_mutex_unlock(&control->queue_mutex);

  // notify workers
  dt_pthread_mutex_lock(&control->cond_mutex);
  pthread_cond_broadcast(&control->cond);
  dt_pthread_mutex_unlock(&control->cond_mutex);

  return 0;
}

static __thread int threadid = -1;

int32_t dt_control_get_threadid()
{
  if(threadid > -1) return threadid;
  return darktable.control->num_threads;
}

static int32_t dt_control_get_threadid_res()
{
  if(threadid > -1) return threadid;
  return DT_CTL_WORKER_RESERVED;
}

static void *dt_control_work_res(void *ptr)
{
#ifdef _OPENMP // need to do this in every thread
  omp_set_num_threads(darktable.num_openmp_threads);
#endif
  worker_thread_parameters_t *params = (worker_thread_parameters_t *)ptr;
  dt_control_t *s = params->self;
  threadid = params->threadid;
  free(params);
  int32_t threadid = dt_control_get_threadid_res();
  while(dt_control_running())
  {
    // dt_print(DT_DEBUG_CONTROL, "[control_work] %d\n", threadid);
    if(dt_control_run_job_res(s, threadid) < 0)
    {
      // wait for a new job.
      int old;
      pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
      dt_pthread_mutex_lock(&s->cond_mutex);
      dt_pthread_cond_wait(&s->cond, &s->cond_mutex);
      dt_pthread_mutex_unlock(&s->cond_mutex);
      pthread_setcancelstate(old, NULL);
    }
  }
  return NULL;
}

static void *dt_control_worker_kicker(void *ptr)
{
  dt_control_t *control = (dt_control_t *)ptr;
  while(dt_control_running())
  {
    sleep(2);
    dt_pthread_mutex_lock(&control->cond_mutex);
    pthread_cond_broadcast(&control->cond);
    dt_pthread_mutex_unlock(&control->cond_mutex);
  }
  return NULL;
}

static void *dt_control_work(void *ptr)
{
#ifdef _OPENMP // need to do this in every thread
  omp_set_num_threads(darktable.num_openmp_threads);
#endif
  worker_thread_parameters_t *params = (worker_thread_parameters_t *)ptr;
  dt_control_t *control = params->self;
  threadid = params->threadid;
  free(params);
  // int32_t threadid = dt_control_get_threadid();
  while(dt_control_running())
  {
    // dt_print(DT_DEBUG_CONTROL, "[control_work] %d\n", threadid);
    if(dt_control_run_job(control) < 0)
    {
      // wait for a new job.
      dt_pthread_mutex_lock(&control->cond_mutex);
      dt_pthread_cond_wait(&control->cond, &control->cond_mutex);
      dt_pthread_mutex_unlock(&control->cond_mutex);
    }
  }
  return NULL;
}

// moved out of control.c to be able to make some helper functions static
void dt_control_jobs_init(dt_control_t *control)
{
  // start threads
  control->num_threads = CLAMP(dt_conf_get_int("worker_threads"), 1, 8);
  control->thread = (pthread_t *)calloc(control->num_threads, sizeof(pthread_t));
  dt_pthread_mutex_lock(&control->run_mutex);
  control->running = 1;
  dt_pthread_mutex_unlock(&control->run_mutex);
  for(int k = 0; k < control->num_threads; k++)
  {
    worker_thread_parameters_t *params
        = (worker_thread_parameters_t *)calloc(1, sizeof(worker_thread_parameters_t));
    params->self = control;
    params->threadid = k;
    pthread_create(&control->thread[k], NULL, dt_control_work, params);
  }

  /* create queue kicker thread */
  pthread_create(&control->kick_on_workers_thread, NULL, dt_control_worker_kicker, control);

  for(int k = 0; k < DT_CTL_WORKER_RESERVED; k++)
  {
    control->job_res[k] = NULL;
    control->new_res[k] = 0;
    worker_thread_parameters_t *params
        = (worker_thread_parameters_t *)calloc(1, sizeof(worker_thread_parameters_t));
    params->self = control;
    params->threadid = k;
    pthread_create(&control->thread_res[k], NULL, dt_control_work_res, params);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
