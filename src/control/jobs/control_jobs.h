/*
    This file is part of darktable,
    copyright (c) 2010--2011 Henrik Andersson.

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
#ifndef DT_CONTROL_JOBS_CONTROL_H
#define DT_CONTROL_JOBS_CONTROL_H

#include <inttypes.h>
#include "control/control.h"

typedef struct dt_control_image_enumerator_t
{
  GList *index;
  int flag;
  gpointer data;
}
dt_control_image_enumerator_t;

int32_t dt_control_write_sidecar_files_job_run(dt_job_t *job);
void dt_control_write_sidecar_files_job_init(dt_job_t *job);


void dt_control_duplicate_images_job_init(dt_job_t *job);
int32_t dt_control_duplicate_images_job_run(dt_job_t *job);

void dt_control_flip_images_job_init(dt_job_t *job, const int32_t cw);
int32_t dt_control_flip_images_job_run(dt_job_t *job);

void dt_control_image_enumerator_job_film_init(dt_control_image_enumerator_t *t, int32_t filmid);
void dt_control_image_enumerator_job_selected_init(dt_control_image_enumerator_t *t);

int32_t dt_control_remove_images_job_run(dt_job_t *job);
void dt_control_remove_images_job_init(dt_job_t *job);

void dt_control_delete_images_job_init(dt_job_t *job);
int32_t dt_control_delete_images_job_run(dt_job_t *job);

void dt_control_gpx_apply_job_init(dt_job_t *job, const gchar *filename, int32_t filmid);
int32_t dt_control_gpx_apply_job_run(dt_job_t *job);

void dt_control_export_job_init(dt_job_t *job);
int32_t dt_control_export_job_run(dt_job_t *job);


void dt_control_write_sidecar_files();
void dt_control_delete_images();
void dt_control_duplicate_images();
void dt_control_flip_images(const int32_t cw);
void dt_control_remove_images();
void dt_control_export();
void dt_control_merge_hdr();
void dt_control_gpx_apply(const gchar *filename, int32_t filmid);
#endif
