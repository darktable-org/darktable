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

#include "control/jobs/develop_jobs.h"
#include "control/jobs/control_jobs.h"

int32_t dt_dev_process_preview_job_run(dt_job_t *job)
{
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  dt_dev_process_preview_job(t->dev);
  return 0;
}

void dt_dev_process_preview_job_init(dt_job_t *job, dt_develop_t *dev)
{
  dt_control_job_init(job, "develop process preview");
  job->execute = &dt_dev_process_preview_job_run;
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  t->dev = dev;
}

int32_t dt_dev_process_image_job_run(dt_job_t *job)
{
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  dt_dev_process_image_job(t->dev);
  return 0;
}

void dt_dev_process_image_job_init(dt_job_t *job, dt_develop_t *dev)
{
  dt_control_job_init(job, "develop process image");
  job->execute = &dt_dev_process_image_job_run;
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  t->dev = dev;
}

// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
