/*
    This file is part of darktable,
    copyright (c) 2009--2010 henrik andersson

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
#ifndef DARKTABLE_BACKGROUND_JOBS_H
#define DARKTABLE_BACKGROUND_JOBS_H

#include <gtk/gtk.h>
#include <inttypes.h>

typedef enum dt_gui_job_type_t {
	/** Single job ... */
	DT_JOB_SINGLE,
	/** Progress job ... */
	DT_JOB_PROGRESS
} dt_gui_job_type_t;

typedef struct dt_gui_job_t {
	dt_gui_job_type_t type;
	GtkWidget *widget;
	
	/** One liner of message */
	gchar *message;
	
	/** Progress of job 0.0 - 1.0 */
	double progress;
	
} dt_gui_job_t;

void dt_gui_background_jobs_init();

/** initializes a new background job to display */
const dt_gui_job_t *dt_gui_background_jobs_new(dt_gui_job_type_t type, const gchar *message);
/** Set's the message to display of the current job. */
void dt_gui_background_jobs_set_message(const dt_gui_job_t *j,const gchar *message);
/** set's the progress of job, if progress>=1.0 the job is removed from displayed jobs and, job is an invalid handle */
void dt_gui_background_jobs_set_progress(const dt_gui_job_t *j, double progress);

#endif
