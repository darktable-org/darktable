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

#include "common/darktable.h"
#include "common/image_cache.h"
#include "control/jobs/image_jobs.h"

void dt_image_load_job_init(dt_job_t *job, int32_t id, dt_mipmap_size_t mip)
{
  dt_control_job_init(job, "load image %d mip %d", id, mip);
  job->execute = &dt_image_load_job_run;
  dt_image_load_t *t = (dt_image_load_t *)job->param;
  t->imgid = id;
  t->mip = mip;
}

int32_t dt_image_load_job_run(dt_job_t *job)
{
  dt_image_load_t *t = (dt_image_load_t *)job->param;

  // hook back into mipmap_cache:
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_read_get(
    darktable.mipmap_cache,
    &buf,
    t->imgid,
    t->mip,
    DT_MIPMAP_BLOCKING);

  // drop read lock, as this is only speculative async loading.
  if(buf.buf)
    dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
  return 0;
}

int32_t dt_image_import_job_run(dt_job_t *job)
{
  int id;
  char message[512];
  dt_image_import_t *t;
  const guint *jid;

  t = (dt_image_import_t *)job->param;
  message[0] = 0;

  snprintf(message, 512, _("importing image %s"), t->filename);
  jid = dt_control_backgroundjobs_create(darktable.control, 0, message );

  id = dt_image_import(t->film_id, t->filename, TRUE);
  if(id)
  {
    dt_view_filmstrip_set_active_image(darktable.view_manager, id);
    dt_control_queue_redraw();
  }

  dt_control_backgroundjobs_progress(darktable.control, jid, 1.0);
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  return 0;
}

void dt_image_import_job_init(dt_job_t *job, uint32_t filmid, const char *filename)
{
  dt_image_import_t *t;
  dt_control_job_init(job, "import image");
  job->execute = &dt_image_import_job_run;
  t = (dt_image_import_t *)job->param;
  t->filename = g_strdup(filename);
  t->film_id = filmid;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
