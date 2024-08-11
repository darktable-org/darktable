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

#include "dtgtk/progress.h"
#include "gui/gtk.h"

dt_progressbar_params_t *dt_progressbar_create(const char *title, const char *message,
                                               unsigned total_items, gboolean can_cancel)
{
  dt_progressbar_params_t *params = calloc(sizeof(dt_progressbar_params_t),1);
  if(params)
  {
    params->title = g_strdup(title);
    params->message = g_strdup(message);
    params->total_items = total_items;
    params->can_cancel = can_cancel;
    params->min_for_dialog = 10;
  }
  return params;
}

static void _progress_callback(dt_progressbar_params_t *params)
{
  params->cancelled = TRUE;
}

gboolean dt_progressbar_start(dt_progressbar_params_t *prog)
{
  if(!prog)
  {
    dt_gui_cursor_set_busy();
    return FALSE;
  }
  prog->processed_items = 0;
  prog->cancelled = FALSE;
  if(prog->total_items >= prog->min_for_dialog || prog->total_items == 0)
  {
    // create a modal dialog with a progress bar
    GtkWindow *main_window = NULL; //FIXME
    GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
    gchar *title = g_strdup_printf(prog->title,prog->total_items);
    prog->dialog = gtk_dialog_new_with_buttons(title,
                                               main_window,
                                               flags,
                                               prog->can_cancel ? _("cancel") : NULL,
                                               GTK_RESPONSE_CANCEL,
                                               NULL);
    g_free(title);
    gtk_widget_set_name(prog->dialog, "progressmeter");
    prog->progress_bar = gtk_progress_bar_new();
    GtkProgressBar *progress_bar = GTK_PROGRESS_BAR(prog->progress_bar);
    gtk_progress_bar_set_show_text(progress_bar, TRUE);
    GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(prog->dialog)));
    gtk_box_pack_end(content, prog->progress_bar, FALSE, FALSE, 0);
    if(prog->message)
      gtk_progress_bar_set_text(progress_bar, prog->message);
    gtk_window_set_keep_above(GTK_WINDOW(prog->dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(prog->dialog),TRUE);
    g_signal_connect_swapped(G_OBJECT(prog->dialog), "response", G_CALLBACK(_progress_callback), prog);
    gtk_widget_show_all(prog->dialog);
    // give Gtk a chance to update the screen
    dt_gui_process_events();
  }
  else
  {
    dt_gui_cursor_set_busy();
  }
  return TRUE;
}

gboolean dt_progressbar_step(dt_progressbar_params_t *prog)
{
  if(!prog)
    return TRUE;	// if no progress bar requested, user should continue until items exhausted
  prog->processed_items++;
  if(prog->total_items >= prog->min_for_dialog)
  {
    // update the dialog
    GtkProgressBar *progress_bar = GTK_PROGRESS_BAR(prog->progress_bar);
    if(prog->total_items == 0)
      gtk_progress_bar_pulse(progress_bar);
    else
      gtk_progress_bar_set_fraction(progress_bar, prog->processed_items / (double)prog->total_items);
    dt_gui_process_events();
  }
  // indicate whether user should continue processing
  gboolean more = prog->total_items == 0 || prog->processed_items < prog->total_items;
  return more && !prog->cancelled;
}

gboolean dt_progressbar_done(dt_progressbar_params_t *prog)
{
  if(prog && (prog->total_items >= prog->min_for_dialog || prog->total_items == 0))
  {
    // close the dialog window
    g_signal_handlers_disconnect_by_func(G_OBJECT(prog->dialog), G_CALLBACK(_progress_callback), prog);
    gtk_widget_destroy(prog->dialog);
    prog->dialog = NULL;
    dt_gui_process_events();
  }
  else
  {
    // revert to non-busy cursor
    dt_gui_cursor_clear_busy();
  }
  return TRUE;
}

void dt_progressbar_destroy(dt_progressbar_params_t *params)
{
  if(params)
  {
    g_free(params->title);
    g_free(params->message);
    if(params->dialog)
    {
      g_signal_handlers_disconnect_by_func(G_OBJECT(params->dialog), G_CALLBACK(_progress_callback), params);
      gtk_widget_destroy(params->dialog);
      params->dialog = NULL;
    }
  }
  free(params);
}


