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

void dt_dev_cache_load_job_run(dt_job_t *job)
{
  dt_dev_cache_load_t *t = (dt_dev_cache_load_t *)job->param;
  dt_dev_cache_load(t->dev, t->stackpos, t->zoom);
}

void dt_dev_cache_load_job_init(dt_job_t *job, dt_develop_t *dev, int32_t stackpos, dt_dev_zoom_t zoom)
{
  job->execute = &dt_dev_cache_load_job_run;
  dt_dev_cache_load_t *t = (dt_dev_cache_load_t *)job->param;
  t->dev = dev;
  t->stackpos = stackpos;
  t->zoom = zoom;
  dt_control_job_init(job, "develop load cache history %d for zoom level %d", stackpos, zoom);
}

void dt_dev_small_cache_load_run(dt_job_t *job)
{
  dt_dev_small_cache_load_t *t = (dt_dev_small_cache_load_t *)job->param;
  (void)dt_dev_small_cache_load(t->dev);
}

void dt_dev_small_cache_load_init(dt_job_t *job, dt_develop_t *dev)
{
  job->execute = &dt_dev_small_cache_load_run;
  dt_dev_small_cache_load_t *t = (dt_dev_small_cache_load_t *)job->param;
  t->dev = dev;
  dt_control_job_init(job, "develop load small cache");
}

void dt_dev_export_init(dt_job_t *job)
{
  job->execute = &dt_dev_export;
  dt_control_job_init(job, "develop export selected");
}
