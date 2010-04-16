/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_CONTROL_JOBS_H
#define DT_CONTROL_JOBS_H

#include <glib.h>

#include "common/image.h"
#include "control/control.h"
#include "common/film.h"
#include "develop/develop.h"

typedef struct dt_image_load_t
{
  int32_t imgid;
  dt_image_buffer_t mip;
}
dt_image_load_t;

void dt_image_load_job_run(dt_job_t *job);
void dt_image_load_job_init(dt_job_t *job, int32_t imgid, dt_image_buffer_t mip);

typedef struct dt_captured_image_import_t
{
	const char *filename;
}
dt_captured_image_import_t;

void dt_captured_image_import_job_run(dt_job_t *job);
void dt_captured_image_import_job_init(dt_job_t *job, const char *filename);

typedef struct dt_camera_import_t
{
  GList *images;
  struct dt_camera_t *camera;
  char *import_path;
  dt_film_t *film;
}
dt_camera_import_t;
void dt_camera_import_job_run(dt_job_t *job);
void dt_camera_import_job_init(dt_job_t *job, char *path,GList *images, struct dt_camera_t *camera);


typedef struct dt_film_import1_t
{
  dt_film_t *film;
}
dt_film_import1_t;

void dt_film_import1_run(dt_job_t *job);
void dt_film_import1_init(dt_job_t *job, dt_film_t *film);


typedef struct dt_dev_raw_load_t
{
  dt_develop_t *dev;
  dt_image_t *image;
}
dt_dev_raw_load_t;

void dt_dev_raw_load_job_run(dt_job_t *job);
void dt_dev_raw_load_job_init(dt_job_t *job, dt_develop_t *dev, dt_image_t *image);

typedef struct dt_dev_process_t
{
  dt_develop_t *dev;
}
dt_dev_process_t;
void dt_dev_process_preview_job_run(dt_job_t *job);
void dt_dev_process_preview_job_init(dt_job_t *job, dt_develop_t *dev);
void dt_dev_process_image_job_run(dt_job_t *job);
void dt_dev_process_image_job_init(dt_job_t *job, dt_develop_t *dev);

void dt_dev_export_init(dt_job_t *job);

void dt_control_write_dt_files();
void dt_control_delete_images();
void dt_control_duplicate_images();
void dt_control_remove_images();
void dt_control_export();

#endif
