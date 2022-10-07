/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#pragma once

#include <glib.h>

struct dt_control_t;
struct _dt_job_t;

struct _dt_progress_t;
typedef struct _dt_progress_t dt_progress_t;

typedef void (*dt_progress_cancel_callback_t)(dt_progress_t *progress, void *data);

/* init the progress system, basically making sure that any global progress bar is hidden */
void dt_control_progress_init(struct dt_control_t *control);

/** create a new progress object and add it to the gui. pass it to dt_control_progress_destroy() to free the
 * resources. */
dt_progress_t *dt_control_progress_create(struct dt_control_t *control, gboolean has_progress_bar,
                                          const gchar *message);
/** free the resources and remove the gui. */
void dt_control_progress_destroy(struct dt_control_t *control, dt_progress_t *progress);

/** set a callback to be executed when the progress is being cancelled. */
void dt_control_progress_make_cancellable(struct dt_control_t *control, dt_progress_t *progress,
                                          dt_progress_cancel_callback_t cancel, void *data);
/** convenience function to cancel a job when the progress gets cancelled. */
void dt_control_progress_attach_job(struct dt_control_t *control, dt_progress_t *progress,
                                    struct _dt_job_t *job);
/** cancel the job linked to with dt_control_progress_attach_job(). don't forget to call
 * dt_control_progress_destroy() afterwards. */
void dt_control_progress_cancel(struct dt_control_t *control, dt_progress_t *progress);

/** update the progress of the progress object. the range should be [0.0, 1.0] to make progress bars work. */
void dt_control_progress_set_progress(struct dt_control_t *control, dt_progress_t *progress, double value);
/** return the last set progress value. */
double dt_control_progress_get_progress(dt_progress_t *progress);

/** get the message passed during construction. */
const gchar *dt_control_progress_get_message(dt_progress_t *progress);
/** update the message. */
void dt_control_progress_set_message(struct dt_control_t *control, dt_progress_t *progress, const char *message);

/** these functions are to be used by lib/backgroundjobs.c only. */
void dt_control_progress_set_gui_data(dt_progress_t *progress, void *data);
void *dt_control_progress_get_gui_data(dt_progress_t *progress);

/** does the progress object have a progress bar in its gui? */
gboolean dt_control_progress_has_progress_bar(dt_progress_t *progress);

/** has a job been linked to the progress object? */
gboolean dt_control_progress_cancellable(dt_progress_t *progress);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

