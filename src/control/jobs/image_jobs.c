/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include "control/jobs/image_jobs.h"
#include "common/darktable.h"
#include "common/image_cache.h"

typedef struct dt_image_load_t
{
  int32_t imgid;
  dt_mipmap_size_t mip;
} dt_image_load_t;

static int32_t dt_image_load_job_run(dt_job_t *job)
{
  dt_image_load_t *params = dt_control_job_get_params(job);

  // hook back into mipmap_cache:
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, params->imgid, params->mip, DT_MIPMAP_BLOCKING, 'r');

  if(buf.buf && buf.height && buf.width)
  {
    const double aspect_ratio = (double)buf.width / (double)buf.height;
    dt_image_set_aspect_ratio_if_different(params->imgid, aspect_ratio, FALSE);
  }

  // drop read lock, as this is only speculative async loading.
  // moved this after the if, because the if never worked because the cache was released.
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  return 0;
}

dt_job_t *dt_image_load_job_create(int32_t id, dt_mipmap_size_t mip)
{
  dt_job_t *job = dt_control_job_create(&dt_image_load_job_run, "load image %d mip %d", id, mip);
  if(!job) return NULL;
  dt_image_load_t *params = (dt_image_load_t *)calloc(1, sizeof(dt_image_load_t));
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  dt_control_job_set_params_with_size(job, params, sizeof(dt_image_load_t), free);
  params->imgid = id;
  params->mip = mip;
  return job;
}

typedef struct dt_image_import_t
{
  uint32_t film_id;
  gchar *filename;
} dt_image_import_t;

static int32_t dt_image_import_job_run(dt_job_t *job)
{
  char message[512] = { 0 };
  dt_image_import_t *params = dt_control_job_get_params(job);

  snprintf(message, sizeof(message), _("Importing image %s"), params->filename);
  dt_control_job_set_progress_message(job, message);

  const int id = dt_image_import(params->film_id, params->filename, TRUE, TRUE);
  if(id)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
    dt_control_queue_redraw();
  }

  dt_control_job_set_progress(job, 1.0);
  return 0;
}

static void dt_image_import_job_cleanup(void *p)
{
  dt_image_import_t *params = p;

  g_free(params->filename);

  free(params);
}

dt_job_t *dt_image_import_job_create(uint32_t filmid, const char *filename)
{
  dt_image_import_t *params;
  dt_job_t *job = dt_control_job_create(&dt_image_import_job_run, "import image");
  if(!job) return NULL;
  params = (dt_image_import_t *)calloc(1, sizeof(dt_image_import_t));
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  dt_control_job_add_progress(job, _("Import image"), FALSE);
  dt_control_job_set_params(job, params, dt_image_import_job_cleanup);
  params->filename = g_strdup(filename);
  params->film_id = filmid;
  return job;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
