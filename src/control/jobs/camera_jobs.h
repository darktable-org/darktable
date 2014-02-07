/*
    This file is part of darktable,
    copyright (c) 2010 -- 2014 Henrik Andersson.

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
#ifndef DT_CONTROL_JOBS_CAMERA_H
#define DT_CONTROL_JOBS_CAMERA_H

#include <inttypes.h>
#include "common/film.h"
#include "common/variables.h"
#include "control/control.h"

typedef struct dt_camera_shared_t
{
  struct dt_import_session_t *session;
} dt_camera_shared_t;

/** Camera capture job */
typedef struct dt_camera_capture_t
{
  dt_camera_shared_t shared;
  int32_t total;
  pthread_mutex_t mutex;
  pthread_cond_t done;

  /** delay between each capture, 0 no delay */
  uint32_t delay;
  /** count of images to capture, 0==1 */
  uint32_t count;
  /** bracket capture, 0=no bracket */
  uint32_t brackets;

  /** steps for each bracket, only used ig bracket capture*/
  uint32_t steps;

}
dt_camera_capture_t;
int32_t dt_camera_capture_job_run(dt_job_t *job);
void dt_camera_capture_job_init(dt_job_t *job, const char *jobcode, uint32_t delay, uint32_t count, uint32_t brackets, uint32_t steps);

/** camera get previews job. */
typedef struct dt_camera_get_previews_t
{
  struct dt_camera_t *camera;
  uint32_t flags;
  struct dt_camctl_listener_t *listener;
}
dt_camera_get_previews_t;
int32_t dt_camera_get_previews_job_run(dt_job_t *job);
void dt_camera_get_previews_job_init(dt_job_t *job,struct dt_camera_t *camera,struct dt_camctl_listener_t *listener,uint32_t flags);

/** Camera import job */
typedef struct dt_camera_import_t
{
  dt_camera_shared_t shared;

  GList *images;
  struct dt_camera_t *camera;
  const guint *bgj;
  double fraction;
  uint32_t import_count;
}
dt_camera_import_t;
int32_t dt_camera_import_job_run(dt_job_t *job);
void dt_camera_import_job_init(dt_job_t *job, const char *jobcode, GList *images, struct dt_camera_t *camera, time_t time_override);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
