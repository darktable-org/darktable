/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include <gtk/gtk.h>

typedef struct {
  // items set up by caller before starting
  char *title;			// dialog title
  char *message;		// optional message in content area (NULL if none)
  unsigned total_items;		// the total number of items we'll be processsing
  unsigned min_for_dialog;	// only actually show the progress bar if at least this many
  gboolean can_cancel;		// is user allowed to cancel the processing?
  // items which get updated as we progress; user should treat these as read-only
  unsigned processed_items;
  gboolean cancelled;
  // internal use only
  GtkWidget *dialog;
  GtkWidget *progress_bar;
} dt_progressbar_params_t;


// allocate/free a parameter block for the progress bar dialog
dt_progressbar_params_t *dt_progressbar_create(const char *title,
                                               const char *message,
                                               unsigned total_items,
                                               gboolean can_cancel);
void dt_progressbar_destroy(dt_progressbar_params_t *params);

// initialize the progress bar and put up a modal dialog if total_items > min_for_dialog
// if the number of items is not large enough to warrant a dialog, turn on the busy cursor
gboolean dt_progressbar_start(dt_progressbar_params_t *prog);
// we have processed one item, so update the progress bar if it is being displayed
// returns TRUE if iteration should continue, FALSE if the user cancelled
gboolean dt_progressbar_step(dt_progressbar_params_t *prog);
// clean up: remove dialog or unset busy cursor, as appropriate
gboolean dt_progressbar_done(dt_progressbar_params_t *prog);

