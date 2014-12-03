/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#include "common/darktable.h"
#include "common/film.h"
#include "control/jobs/film_jobs.h"
#include <stdlib.h>

typedef struct dt_film_import1_t
{
  dt_film_t *film;
} dt_film_import1_t;

static int32_t dt_film_import1_run(dt_job_t *job)
{
  dt_film_import1_t *params = dt_control_job_get_params(job);
  dt_film_import1(params->film);
  dt_pthread_mutex_lock(&params->film->images_mutex);
  params->film->ref--;
  dt_pthread_mutex_unlock(&params->film->images_mutex);
  if(params->film->ref <= 0)
  {
    if(dt_film_is_empty(params->film->id))
    {
      dt_film_remove(params->film->id);
    }
    dt_film_cleanup(params->film);
    free(params->film);
  }
  free(params);
  return 0;
}

dt_job_t *dt_film_import1_create(dt_film_t *film)
{
  dt_job_t *job = dt_control_job_create(&dt_film_import1_run, "cache load raw images for preview");
  if(!job) return NULL;
  dt_film_import1_t *params = (dt_film_import1_t *)calloc(1, sizeof(dt_film_import1_t));
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  dt_control_job_set_params(job, params);
  params->film = film;
  dt_pthread_mutex_lock(&film->images_mutex);
  film->ref++;
  dt_pthread_mutex_unlock(&film->images_mutex);
  return job;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
