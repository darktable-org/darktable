/*
    This file is part of darktable,
    Copyright (C) 2024-2025 darktable developers.

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

#include "splash.h"
#include "control/conf.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#define USE_HEADER_BAR

// #define USE_FEATURED_IMAGE

#define MAX_IMAGES 4

#define ICON_SIZE 150

#ifdef USE_FEATURED_IMAGE
#define PROGNAME_SIZE 300
#else
#define PROGNAME_SIZE 320
#endif

static GtkWidget *splash_screen = NULL;
static GtkWidget *progress_text = NULL;
static GtkWidget *remaining_text = NULL;
static gboolean showing_remaining = FALSE;
static GtkWidget *remaining_box = NULL;

static GtkWidget *exit_screen = NULL;

static void _process_all_gui_events()
{
  for(int i = 0; i < 5; i++)
  {
    g_usleep(1000);
    dt_gui_process_events();
  }
}

static GtkWidget *_get_logo()
{
  const dt_logo_season_t season = dt_util_get_logo_season();

  GtkWidget *logo = NULL;

  gchar *image_file = season == DT_LOGO_SEASON_NONE
                          ? g_strdup_printf("%s/pixmaps/idbutton.svg", darktable.datadir)
                          : g_strdup_printf("%s/pixmaps/idbutton-%d.svg", darktable.datadir, season);
  GdkPixbuf *logo_image = gdk_pixbuf_new_from_file_at_size(image_file, ICON_SIZE, -1, NULL);
  g_free(image_file);

  if(logo_image)
  {
    logo = gtk_image_new_from_pixbuf(logo_image);
    g_object_unref(logo_image);
  }
  else
  {
    logo = GTK_WIDGET(gtk_label_new("logo"));
  }
  gtk_widget_set_name(GTK_WIDGET(logo), "splashscreen-logo");
  return logo;
}

static GtkWidget *_get_program_name()
{
  GtkWidget *program_name = NULL;
  gchar *image_file = g_strdup_printf("%s/pixmaps/darktable.svg", darktable.datadir);
  GdkPixbuf *prog_name_image = gdk_pixbuf_new_from_file_at_size(image_file, PROGNAME_SIZE, -1, NULL);
  g_free(image_file);

  if(prog_name_image)
  {
    program_name = gtk_image_new_from_pixbuf(prog_name_image);
    g_object_unref(prog_name_image);
  }
  else
    program_name = GTK_WIDGET(gtk_label_new("darktable"));

  gtk_widget_set_name(program_name, "splashscreen-program");
  return program_name;
}

static void _set_header_bar(GtkWidget *dialog)
{
#ifdef USE_HEADER_BAR
  GtkHeaderBar *header = GTK_HEADER_BAR(gtk_dialog_get_header_bar(GTK_DIALOG(dialog)));
  gtk_widget_set_name(GTK_WIDGET(header), "splashscreen-header");
  GtkWidget *title = gtk_label_new(NULL);
  gtk_header_bar_set_custom_title(header, title);
  gtk_header_bar_set_has_subtitle(header, FALSE);
  gtk_header_bar_set_show_close_button(header, FALSE);
#endif
}

void darktable_splash_screen_create(
    GtkWindow *parent_window,
    const gboolean force)
{
  if(splash_screen || dt_check_gimpmode("file") || dt_check_gimpmode("thumb")
     || (!dt_conf_get_bool("show_splash_screen") && !force))
  {
    return;
  }

#ifdef USE_HEADER_BAR
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
#else
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
#endif
  splash_screen
      = gtk_dialog_new_with_buttons(_("darktable starting"), parent_window, flags, NULL, GTK_RESPONSE_NONE, NULL);
  gtk_window_set_position(GTK_WINDOW(splash_screen), GTK_WIN_POS_CENTER);
  gtk_widget_set_name(splash_screen, "splashscreen");
  progress_text = gtk_label_new(_("initializing"));
  gtk_widget_set_name(progress_text, "splashscreen-progress");
  remaining_text = gtk_label_new("");
  gtk_widget_set_name(remaining_text, "splashscreen-remaining");
  _set_header_bar(splash_screen);
  int version_len = strlen(darktable_package_version);
  char *delim = strchr(darktable_package_version, '~');
  if(delim) version_len = delim - darktable_package_version;
  gchar *version_str = g_strdup_printf("%.*s", version_len, darktable_package_version);
  GtkWidget *version = GTK_WIDGET(gtk_label_new(version_str));
  g_free(version_str);
  gtk_widget_set_name(version, "splashscreen-version");
  gchar *years = g_strdup_printf("© 2009-%s", darktable_last_commit_year);
  GtkWidget *copyright = GTK_WIDGET(gtk_label_new(years));
  g_free(years);
  gtk_widget_set_name(copyright, "splashscreen-copyright");
  GtkWidget *logo = _get_logo();
  GtkWidget *program_name = _get_program_name();
  GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(splash_screen)));

#ifdef USE_FEATURED_IMAGE
  const int imgnum = (int)(1 + (clock() % MAX_IMAGES));
  gchar *image_file = g_strdup_printf("%s/pixmaps/splashscreen-%02d.jpg", darktable.datadir, imgnum);
  GtkWidget *image = gtk_image_new_from_file(image_file);
  g_free(image_file);
  gtk_widget_set_name(GTK_WIDGET(image), "splashscreen-image");
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 180);
  GtkWidget *program_desc = GTK_WIDGET(gtk_label_new(_("photography workflow application\nand RAW developer")));
  gtk_label_set_justify(GTK_LABEL(program_desc), GTK_JUSTIFY_CENTER);
  gtk_widget_set_name(program_desc, "splashscreen-description");
  dt_gui_box_add(content, dt_gui_hbox(dt_gui_vbox(logo, version, program_name, program_desc), image));
#else
  gtk_image_set_pixel_size(GTK_IMAGE(logo), ICON_SIZE);
  gtk_label_set_justify(GTK_LABEL(version), GTK_JUSTIFY_LEFT);

  GtkWidget *program_desc = GTK_WIDGET(gtk_label_new(_("photography workflow application\nand RAW developer")));
  gtk_label_set_justify(GTK_LABEL(program_desc), GTK_JUSTIFY_LEFT);
  gtk_widget_set_halign(program_desc, GTK_ALIGN_START);
  gtk_widget_set_name(program_desc, "splashscreen-description");

  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_name(sep, "splashscreen-separator");
  gtk_widget_set_hexpand(sep, TRUE);

  GtkWidget *title_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_set_spacing(GTK_BOX(title_col), 4);
  gtk_box_pack_start(GTK_BOX(title_col), program_name, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(title_col), version, FALSE, FALSE, 0);
  gtk_widget_set_halign(program_name, GTK_ALIGN_START);
  gtk_widget_set_halign(version, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(version), 0.0);
  gtk_widget_set_halign(title_col, GTK_ALIGN_START);
  gtk_widget_set_valign(title_col, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(logo, GTK_ALIGN_CENTER);

  GtkWidget *logo_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_pack_start(GTK_BOX(logo_col), logo, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(logo_col), copyright, FALSE, FALSE, 0);
  gtk_widget_set_halign(logo_col, GTK_ALIGN_START);
  gtk_widget_set_valign(logo_col, GTK_ALIGN_CENTER);

  dt_gui_box_add(content, dt_gui_vbox(dt_gui_hbox(logo_col, title_col), program_desc, sep, progress_text));
#endif

  gtk_widget_set_halign(progress_text, GTK_ALIGN_START);

  remaining_box = dt_gui_hbox(dtgtk_button_new(dtgtk_cairo_paint_clock, 0, 0), remaining_text);
  gtk_widget_set_halign(GTK_WIDGET(remaining_box), GTK_ALIGN_START);

  dt_gui_box_add(content, remaining_box);

  gtk_window_set_decorated(GTK_WINDOW(splash_screen), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(splash_screen), 700, -1);
  gtk_widget_show_all(splash_screen);
  gtk_widget_hide(remaining_box);
  _process_all_gui_events();
}

void darktable_splash_screen_set_progress(
    const char *msg)
{
  if(splash_screen)
  {
    gtk_label_set_text(GTK_LABEL(progress_text), msg);
    gtk_widget_show(progress_text);
    if(showing_remaining)
    {
      gtk_widget_hide(remaining_box);
      showing_remaining = FALSE;
    }
    _process_all_gui_events();
  }
}

void darktable_splash_screen_set_progress_percent(
    const char *msg,
    const double fraction,
    const double elapsed)
{
  if(splash_screen)
  {
    const int percent = round(100.0 * fraction);
    char *text = g_strdup_printf(msg, percent);
    gtk_label_set_text(GTK_LABEL(progress_text), text);
    g_free(text);

    if(elapsed >= 2.0 || fraction > 0.01)
    {
      const double total = elapsed / fraction;
      const double remain = (total - elapsed) + 0.5;
      const int minutes = remain / 60;
      const int seconds = remain - (60 * minutes);
      char *rem_text = g_strdup_printf(" %4d:%02d", minutes, seconds);
      gtk_label_set_text(GTK_LABEL(remaining_text), rem_text);
      g_free(rem_text);
      gtk_widget_queue_draw(remaining_box);
    }
    else
    {
      gtk_label_set_text(GTK_LABEL(remaining_text), "   --:--");
    }
    gtk_widget_show_all(splash_screen);
    showing_remaining = TRUE;
    _process_all_gui_events();
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

void darktable_exit_screen_create(
    GtkWindow *parent_window,
    const gboolean force)
{
  if(exit_screen || dt_check_gimpmode("file") || dt_check_gimpmode("thumb")
     || (!dt_conf_get_bool("show_splash_screen") && !force))
  {
    return;
  }

#ifdef USE_HEADER_BAR
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
#else
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
#endif

  exit_screen
      = gtk_dialog_new_with_buttons(_("darktable shutdown"), parent_window, flags, NULL, GTK_RESPONSE_NONE, NULL);
  gtk_window_set_position(GTK_WINDOW(exit_screen), GTK_WIN_POS_CENTER);
  gtk_widget_set_name(exit_screen, "splashscreen");
  _set_header_bar(exit_screen);
  GtkWidget *program_name = _get_program_name();
  GtkWidget *logo = _get_logo();
  gtk_image_set_pixel_size(GTK_IMAGE(logo), ICON_SIZE);
  GtkBox *header_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(header_box, logo, FALSE, FALSE, 0);
  gtk_box_pack_start(header_box, program_name, FALSE, FALSE, 0);
  GtkWidget *message1 = gtk_label_new(_("darktable is now shutting down"));
  gtk_widget_set_name(message1, "exitscreen-message");
  GtkWidget *message2 = gtk_label_new(_("please wait while background jobs finish"));
  gtk_widget_set_name(message2, "exitscreen-message");
  dt_gui_dialog_add(GTK_DIALOG(exit_screen), GTK_WIDGET(header_box), message1, message2);
  gtk_widget_show_all(exit_screen);
  _process_all_gui_events();
  gtk_window_set_keep_above(GTK_WINDOW(exit_screen), FALSE);
  dt_gui_process_events();
}

void darktable_exit_screen_destroy()
{
  if(exit_screen)
  {
    gtk_widget_destroy(exit_screen);
    exit_screen = NULL;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
