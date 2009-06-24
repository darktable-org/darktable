#include "control/jobs.h"

void dt_image_load_job_init(dt_job_t *job, dt_image_t *image, dt_image_buffer_t mip)
{
  job->execute = &dt_image_load_job_run;
  dt_image_load_t *t = (dt_image_load_t *)job->param;
  t->image = image;
  t->mip = mip;
  dt_control_job_init(job, "load image %d mip %d", image->id, mip);
}

void dt_image_load_job_run(dt_job_t *job)
{
  dt_image_load_t *t = (dt_image_load_t *)job->param;
  int ret = dt_image_load(t->image, t->mip);
  // drop read lock, as this is only speculative async loading.
  if(!ret) dt_image_release(t->image, t->mip, 'r');
}

void dt_film_import1_init(dt_job_t *job, dt_film_roll_t *film)
{
  job->execute = &dt_film_import1_run;
  dt_film_import1_t *t = (dt_film_import1_t *)job->param;
  t->film = film;
  dt_control_job_init(job, "cache load raw images for preview");
}

void dt_film_import1_run(dt_job_t *job)
{
  dt_film_import1_t *t = (dt_film_import1_t *)job->param;
  dt_film_import1(t->film);
}

// ====================
// develop:
// ====================

void dt_dev_raw_load_job_run(dt_job_t *job)
{
  dt_dev_raw_load_t *t = (dt_dev_raw_load_t *)job->param;
  dt_dev_raw_load(t->dev, t->image);
}

void dt_dev_raw_load_job_init(dt_job_t *job, dt_develop_t *dev, dt_image_t *image)
{
  job->execute =&dt_dev_raw_load_job_run;
  dt_dev_raw_load_t *t = (dt_dev_raw_load_t *)job->param;
  t->dev = dev;
  t->image = image;
  dt_control_job_init(job, "develop load raw image %s", image->filename);
}

void dt_dev_process_preview_job_run(dt_job_t *job)
{
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  dt_dev_process_preview_job(t->dev);
}

void dt_dev_process_preview_job_init(dt_job_t *job, dt_develop_t *dev)
{
  job->execute = &dt_dev_process_preview_job_run;
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  t->dev = dev;
  dt_control_job_init(job, "develop process preview");
}

void dt_dev_process_image_job_run(dt_job_t *job)
{
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  dt_dev_process_image_job(t->dev);
}

void dt_dev_process_image_job_init(dt_job_t *job, dt_develop_t *dev)
{
  job->execute = &dt_dev_process_image_job_run;
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  t->dev = dev;
  dt_control_job_init(job, "develop image preview");
}

void dt_dev_export_init(dt_job_t *job)
{
  job->execute = &dt_dev_export;
  dt_control_job_init(job, "develop export selected");
}
