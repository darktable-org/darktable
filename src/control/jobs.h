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
#include "common/variables.h"
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

/** Tethered image import job */
typedef struct dt_captured_image_import_t
{
	const char *filename;
}
dt_captured_image_import_t;
void dt_captured_image_import_job_run(dt_job_t *job);
void dt_captured_image_import_job_init(dt_job_t *job, const char *filename);

/** Camera capture job */
typedef struct dt_camera_capture_t
{
	/** delay between each capture, 0 no delay */
	uint32_t delay;
	/** count of images to capture, 0==1 */
	uint32_t count;
	/** bracket capture, 0=no bracket */
	uint32_t brackets;
}
dt_camera_capture_t;
void dt_camera_capture_job_run(dt_job_t *job);
void dt_camera_capture_job_init(dt_job_t *job, uint32_t delay, uint32_t count, uint32_t brackets);

/** Camera import job */
typedef struct dt_camera_import_t
{
  GList *images;
  struct dt_camera_t *camera;
  dt_variables_params_t *vp;
  dt_film_t *film;
  gchar *path;
  gchar *filename;
  uint32_t import_count;
}
dt_camera_import_t;
void dt_camera_import_job_run(dt_job_t *job);
void dt_camera_import_job_init(dt_job_t *job,char *jobcode, char *path,char *filename,GList *images, struct dt_camera_t *camera);

/** Camera image import backup job initiated upon import job for each image*/
typedef struct dt_camera_import_backup_t
{
  gchar *sourcefile;
  gchar *destinationfile;
}
dt_camera_import_backup_t;
void dt_camera_import_backup_job_run(dt_job_t *job);
void dt_camera_import_backup_job_init(dt_job_t *job,const char *sourcefile,const char *destinationfile);

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
