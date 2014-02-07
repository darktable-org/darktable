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

void dt_film_import1_init(dt_job_t *job, dt_film_t *film)
{
  dt_control_job_init(job, "cache load raw images for preview");
  job->execute = &dt_film_import1_run;
  dt_film_import1_t *t = (dt_film_import1_t *)job->param;
  t->film = film;
  dt_pthread_mutex_lock(&film->images_mutex);
  film->ref++;
  dt_pthread_mutex_unlock(&film->images_mutex);
}

int32_t dt_film_import1_run(dt_job_t *job)
{
  dt_film_import1_t *t = (dt_film_import1_t *)job->param;
  dt_film_import1(t->film);
  dt_pthread_mutex_lock(&t->film->images_mutex);
  t->film->ref--;
  dt_pthread_mutex_unlock(&t->film->images_mutex);
  if(t->film->ref <= 0)
  {
    if(dt_film_is_empty(t->film->id))
    {
      dt_film_remove(t->film->id);
    }
    dt_film_cleanup(t->film);
    free(t->film);
  }
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
