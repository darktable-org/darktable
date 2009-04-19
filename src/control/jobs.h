#ifndef DT_CONTROL_JOBS_H
#define DT_CONTROL_JOBS_H

#include "common/image.h"
#include "control/control.h"
#include "library/library.h"
#include "develop/develop.h"

typedef struct dt_image_load_t
{
  dt_image_t *image;
  dt_image_buffer_t mip;
}
dt_image_load_t;

void dt_image_load_job_run(dt_job_t *job);
void dt_image_load_job_init(dt_job_t *job, dt_image_t *image, dt_image_buffer_t mip);

typedef struct dt_film_import1_t
{
  dt_film_roll_t *film;
}
dt_film_import1_t;

void dt_film_import1_run(dt_job_t *job);
void dt_film_import1_init(dt_job_t *job, dt_film_roll_t *film);


typedef struct dt_dev_raw_load_t
{
  dt_develop_t *dev;
  dt_image_t *image;
}
dt_dev_raw_load_t;

void dt_dev_raw_load_job_run(dt_job_t *job);
void dt_dev_raw_load_job_init(dt_job_t *job, dt_develop_t *dev, dt_image_t *image);

#ifndef DT_USE_GEGL
enum dt_dev_zoom_t;
typedef struct dt_dev_cache_load_t
{
  dt_develop_t *dev;
  int32_t stackpos;
  enum dt_dev_zoom_t zoom;
}
dt_dev_cache_load_t;

void dt_dev_cache_load_job_run(dt_job_t *job);
void dt_dev_cache_load_job_init(dt_job_t *job, dt_develop_t *dev, int32_t stackpos, enum dt_dev_zoom_t zoom);

typedef struct dt_dev_small_cache_load_t
{
  dt_develop_t *dev;
}
dt_dev_small_cache_load_t;

void dt_dev_small_cache_load_run(dt_job_t *job);
void dt_dev_small_cache_load_init(dt_job_t *job, dt_develop_t *dev);
#else
typedef struct dt_dev_process_t
{
  dt_develop_t *dev;
}
void dt_dev_process_preview_job_run(dt_job_t *job);
void dt_dev_process_preview_job_init(dt_job_t *job, dt_develop_t *dev);
void dt_dev_process_image_job_run(dt_job_t *job);
void dt_dev_process_image_job_init(dt_job_t *job, dt_develop_t *dev);
#endif

void dt_dev_export_init(dt_job_t *job);

#endif
