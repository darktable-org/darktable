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

#include "control/conf.h"
#include "gui/gtk.h"
#include "splash.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

static GtkWidget *splash_screen = NULL;
static GtkWidget *progress_text = NULL;
static GtkWidget *remaining_text = NULL;
static gboolean showing_remaining = FALSE;

void darktable_splash_screen_create(GtkWindow *parent_window, gboolean force)
{
  // no-op if the splash has already been created; if not, only run if the
  // splash screen is enabled in the config or we are told to create it regardless
  if(splash_screen || (!dt_conf_get_bool("show_splash_screen") && !force))
    return;
  // a simple gtk_dialog_new() leaves us unable to setup the header bar, so use .._with_buttons
  // and just specify a NULL strings to have no buttons.  We need to pretend to actually have
  // one button, though, to keep the compiler happy
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
  splash_screen = gtk_dialog_new_with_buttons(_("darktable starting"), parent_window, flags,
                                              NULL, GTK_RESPONSE_NONE,  // <-- fake button list for compiler
                                              NULL);
  gtk_widget_set_name(splash_screen,"splashscreen");
  progress_text = gtk_label_new("initializing");
  gtk_widget_set_name(progress_text,"splashscreen-progress");
  remaining_text = gtk_label_new("");
  gtk_widget_set_name(remaining_text,"splashscreen-remaining");
  GtkHeaderBar *header = GTK_HEADER_BAR(gtk_dialog_get_header_bar(GTK_DIALOG(splash_screen)));
  gtk_widget_set_name(GTK_WIDGET(header),"splashscreen-header");
  char *title_str = g_strdup_printf(_("Starting darktable %.5s"), darktable_package_version);
  GtkWidget *title = gtk_label_new(NULL);
  gtk_label_set_text(GTK_LABEL(title),title_str);
  g_free(title_str);
  gtk_header_bar_set_custom_title(header,title);
  gtk_header_bar_set_has_subtitle(header, FALSE);
  gtk_header_bar_set_show_close_button(header, FALSE);
  GtkWidget *icon = gtk_image_new_from_icon_name("darktable", GTK_ICON_SIZE_DIALOG);
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 240);
  gtk_widget_set_name(GTK_WIDGET(icon),"splashscreen-icon");
  GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(splash_screen)));
  gtk_box_pack_start(content, icon, FALSE, FALSE, 0);
  gtk_box_pack_start(content, progress_text, FALSE, FALSE, 0);
  gtk_box_pack_start(content, remaining_text, FALSE, FALSE, 0);
  gtk_window_set_keep_above(GTK_WINDOW(splash_screen), TRUE);
  gtk_widget_show_all(splash_screen);
  // give Gtk a chance to update the screen; we need to let the event processing run several
  // times for the splash window to actually be fully displayed
  for(int i = 0; i < 5; i++)
    dt_gui_process_events();
}

void darktable_splash_screen_set_progress(const char *msg)
{
  if(splash_screen)
  {
    gtk_label_set_text(GTK_LABEL(progress_text), msg);
    gtk_widget_show(progress_text);
    if(showing_remaining)
    {
      gtk_label_set_text(GTK_LABEL(remaining_text), "");
      showing_remaining = FALSE;
    }
    // give Gtk a chance to update the screen
    dt_gui_process_events();
  }
}

void darktable_splash_screen_set_progress_percent(const char *msg, double fraction, double elapsed)
{
  if(splash_screen)
  {
    int percent = round(100.0 * fraction);
    char *text = g_strdup_printf(msg, percent);
    gtk_label_set_text(GTK_LABEL(progress_text), text);
    g_free(text);
    gtk_widget_show(progress_text);
    if(elapsed >= 2.0 && fraction > 0.001)
    {
      double total = elapsed / fraction;
      double remain = total - elapsed;
      int minutes = remain / 60;
      int seconds = remain - (60 * minutes);
      char *rem_text = g_strdup_printf("⏲%4d:%02d",minutes,seconds);
      gtk_label_set_text(GTK_LABEL(remaining_text), rem_text);
      g_free(rem_text);
    }
    else
    {
      gtk_label_set_text(GTK_LABEL(remaining_text), "⏲  --:--");
    }
//    gtk_widget_show(remaining_text);
    gtk_widget_show_all(splash_screen);
    showing_remaining = TRUE;
    // give Gtk a chance to update the screen
    dt_gui_process_events();
  }
}

void darktable_splash_screen_destroy()
{
  if(splash_screen)
  {
    gtk_widget_destroy(progress_text);
    progress_text = NULL;
    gtk_widget_destroy(splash_screen);
    splash_screen = NULL;
  }
}
