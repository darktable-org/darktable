/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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

#include <glib.h>
#include <inttypes.h>
#include <stddef.h>

#define DT_CONTROL_DESCRIPTION_LEN 256
// reserved workers
#define DT_CTL_WORKER_RESERVED 3
#define DT_CTL_WORKER_ZOOM_1 0    // dev zoom 1
#define DT_CTL_WORKER_ZOOM_FILL 1 // dev zoom fill
#define DT_CTL_WORKER_ZOOM_2 2    // dev zoom for preview2

typedef enum dt_job_state_t
{
  DT_JOB_STATE_INITIALIZED = 0,
  DT_JOB_STATE_QUEUED,
  DT_JOB_STATE_RUNNING,
  DT_JOB_STATE_FINISHED,
  DT_JOB_STATE_CANCELLED,
  DT_JOB_STATE_DISCARDED,
  DT_JOB_STATE_DISPOSED
} dt_job_state_t;

typedef enum dt_job_queue_t
{
  DT_JOB_QUEUE_USER_FG = 0,     // gui actions, ...
  DT_JOB_QUEUE_SYSTEM_FG = 1,   // thumbnail creation, ..., may be pushed out of the queue
  DT_JOB_QUEUE_USER_BG = 2,     // imports, ...
  DT_JOB_QUEUE_USER_EXPORT = 3, // exports. only one of these jobs will ever be scheduled at a time
  DT_JOB_QUEUE_SYSTEM_BG = 4,   // some lua stuff that may not be pushed out of the queue, ...
  DT_JOB_QUEUE_MAX = 5
} dt_job_queue_t;

typedef struct _dt_job_t dt_job_t;

typedef int32_t (*dt_job_execute_callback)(dt_job_t *);
typedef void (*dt_job_state_change_callback)(dt_job_t *, dt_job_state_t state);
typedef void (*dt_job_destroy_callback)(void *data);

/** create a new initialized job */
dt_job_t *dt_control_job_create(dt_job_execute_callback execute, const char *msg, ...) __attribute__((format(printf, 2, 3)));
/** destroy a job object and free its memory. this does NOT remove it from any job queues! */
void dt_control_job_dispose(dt_job_t *job);
/** setup a state callback for job. */
void dt_control_job_set_state_callback(dt_job_t *job, dt_job_state_change_callback cb);
/** cancel a job, running or in queue. */
void dt_control_job_cancel(dt_job_t *job);
dt_job_state_t dt_control_job_get_state(dt_job_t *job);
/** wait for a job to finish execution. */
void dt_control_job_wait(dt_job_t *job);
/** set job params and a callback to destroy those params */
void dt_control_job_set_params(dt_job_t *job, void *params, dt_job_destroy_callback callback);
/** set job params (with size params_size) and a callback to destroy those params.
  * NOTE: in most cases you want dt_control_job_set_params() */
void dt_control_job_set_params_with_size(dt_job_t *job, void *params, size_t params_size,
                                         dt_job_destroy_callback callback);
/** get job params. WARNING: you must not free them. dt_control_job_dispose() will take care of that */
void *dt_control_job_get_params(const dt_job_t *job);

void dt_control_job_add_progress(dt_job_t *job, const char *message, gboolean cancellable);
void dt_control_job_set_progress_message(dt_job_t *job, const char *message);
void dt_control_job_set_progress(dt_job_t *job, double value);
double dt_control_job_get_progress(dt_job_t *job);

struct dt_control_t;
void dt_control_jobs_init(struct dt_control_t *control);
void dt_control_jobs_cleanup(struct dt_control_t *control);

int dt_control_add_job(struct dt_control_t *control, dt_job_queue_t queue_id, dt_job_t *job);
int32_t dt_control_add_job_res(struct dt_control_t *s, dt_job_t *job, int32_t res);

int32_t dt_control_get_threadid();

#ifdef HAVE_GPHOTO2
#include "control/jobs/camera_jobs.h"
#include "common/camera_control.h"
#endif
#include "control/jobs/control_jobs.h"
#include "control/jobs/develop_jobs.h"
#include "control/jobs/film_jobs.h"
#include "control/jobs/image_jobs.h"

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
