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

#include "control/jobs/develop_jobs.h"
#include "control/jobs/control_jobs.h"

static int32_t dt_dev_process_preview_job_run(dt_job_t *job)
{
  dt_develop_t *dev = dt_control_job_get_params(job);
  dt_dev_process_preview_job(dev);
  return 0;
}

static int32_t dt_dev_process_preview2_job_run(dt_job_t *job)
{
  dt_develop_t *dev = dt_control_job_get_params(job);
  dt_dev_process_preview2_job(dev);
  return 0;
}

dt_job_t *dt_dev_process_preview_job_create(dt_develop_t *dev)
{
  dt_job_t *job = dt_control_job_create(&dt_dev_process_preview_job_run, "develop process preview");
  if(!job) return NULL;
  dt_control_job_set_params(job, dev, NULL);
  return job;
}

dt_job_t *dt_dev_process_preview2_job_create(dt_develop_t *dev)
{
  dt_job_t *job = dt_control_job_create(&dt_dev_process_preview2_job_run, "develop process preview");
  if(!job) return NULL;
  dt_control_job_set_params(job, dev, NULL);
  return job;
}

static int32_t dt_dev_process_image_job_run(dt_job_t *job)
{
  dt_develop_t *dev = dt_control_job_get_params(job);
  dt_dev_process_image_job(dev);
  return 0;
}

dt_job_t *dt_dev_process_image_job_create(dt_develop_t *dev)
{
  dt_job_t *job = dt_control_job_create(&dt_dev_process_image_job_run, "develop process image");
  if(!job) return NULL;
  dt_control_job_set_params(job, dev, NULL);
  return job;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
