/*
    This file is part of darktable,
    Copyright (C) 2024-2026 darktable developers.

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
#include "dtgtk/button.h"
#include "splash.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

// include a featured image on the splash screen?

//#define USE_FEATURED_IMAGE

// number of featured images from which to randomly select when
// USE_SPLASHSCREEN_IMAGE not defined
#define MAX_IMAGES 4

#define ICON_SIZE 150

#ifdef USE_FEATURED_IMAGE
#define PROGNAME_SIZE 300
#else
#define PROGNAME_SIZE 320
#endif

static void _process_all_gui_events()
{
  // give Gtk a chance to update the screen; we need to let the event
  // processing run several times for the splash window to actually be
  // fully displayed/updated
  for(int i = 0; i < 5; i++)
  {
    g_usleep(1000);
    dt_gui_process_events();
  }
}

static GtkWidget *_get_logo()
{
  // get the darktable logo, including seasonal variants as
  // appropriate.
  const dt_logo_season_t season = dt_util_get_logo_season();

  GtkWidget *logo = NULL;

  gchar *image_file =
    season == DT_LOGO_SEASON_NONE
    ? g_strdup_printf("%s/pixmaps/idbutton.svg", darktable.datadir)
    : g_strdup_printf("%s/pixmaps/idbutton-%d.svg", darktable.datadir, season);
  GdkPixbuf *logo_image =
    gdk_pixbuf_new_from_file_at_size(image_file, ICON_SIZE, -1, NULL);
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
  // get the darktable name in special font
  GtkWidget *program_name = NULL;
  gchar *image_file =
    g_strdup_printf("%s/pixmaps/darktable.svg", darktable.datadir);
  GdkPixbuf *prog_name_image =
    gdk_pixbuf_new_from_file_at_size(image_file, PROGNAME_SIZE, -1, NULL);
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

void dt_splash_screen_allow_create(const gboolean allow_create)
{
  darktable.splash.create_if_needed = allow_create;
}

void dt_splash_screen_create(const gboolean force)
{
  // no-op if the splash has already been created; if not, only run if
  // the splash screen is enabled in the config or we are told to
  // create it regardless.
  if(darktable.splash.start_screen
     || dt_check_gimpmode("file")
     || dt_check_gimpmode("thumb")
     || (!dt_conf_get_bool("show_splash_screen") && !force))
  {
    return;
  }

  darktable.splash.start_screen = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated(GTK_WINDOW(darktable.splash.start_screen), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(darktable.splash.start_screen), FALSE);
  gtk_window_set_position(GTK_WINDOW(darktable.splash.start_screen), GTK_WIN_POS_CENTER);

  gtk_widget_set_name(darktable.splash.start_screen, "splashscreen");
  darktable.splash.progress_text = gtk_label_new(_("initializing"));
  gtk_widget_set_name(darktable.splash.progress_text, "splashscreen-progress");
  darktable.splash.remaining_text = gtk_label_new("");
  gtk_widget_set_name(darktable.splash.remaining_text, "splashscreen-remaining");
  int version_len = strlen(darktable_package_version);
  char *delim = strchr(darktable_package_version, '~');
  if(delim)
    version_len = delim - darktable_package_version;
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
  GtkBox *content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

#ifdef USE_FEATURED_IMAGE
  // make a random selection of featured image based on the current
  // time
  const int imgnum = (int)(1 + (clock()%MAX_IMAGES));
  //FIXME: if user overrides --datadir, we won't find the image...
  gchar *image_file = g_strdup_printf("%s/pixmaps/splashscreen-%02d.jpg",
                                      darktable.datadir, imgnum);
  GtkWidget *image = gtk_image_new_from_file(image_file);
  g_free(image_file);
  gtk_widget_set_name(GTK_WIDGET(image), "splashscreen-image");
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 180);
  GtkWidget *program_desc =
    GTK_WIDGET(gtk_label_new(_("Photography workflow application\nand RAW developer")));
  gtk_label_set_justify(GTK_LABEL(program_desc), GTK_JUSTIFY_CENTER);
  gtk_widget_set_name(program_desc, "splashscreen-description");

  dt_gui_box_add(content,
                 dt_gui_hbox(dt_gui_vbox(logo, version, program_name, program_desc),
                             image));
#else
  gtk_image_set_pixel_size(GTK_IMAGE(logo), ICON_SIZE);
  gtk_label_set_justify(GTK_LABEL(version), GTK_JUSTIFY_LEFT);

  GtkWidget *program_desc = GTK_WIDGET(gtk_label_new(_("Photography workflow application\nand RAW developer")));
  gtk_label_set_justify(GTK_LABEL(program_desc), GTK_JUSTIFY_LEFT);
  gtk_widget_set_halign(program_desc, GTK_ALIGN_START);
  gtk_widget_set_name(program_desc, "splashscreen-description");

  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_name(sep, "splashscreen-separator");
  gtk_widget_set_hexpand(sep, TRUE);

  GtkWidget *title_col = dt_gui_vbox(program_name);
  gtk_box_set_spacing(GTK_BOX(title_col), 4);
  dt_gui_box_add(GTK_BOX(title_col), version);
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

  dt_gui_box_add(content,
                 dt_gui_vbox(dt_gui_hbox(logo_col, title_col),
                             program_desc, sep, darktable.splash.progress_text));
#endif

  gtk_widget_set_halign(darktable.splash.progress_text, GTK_ALIGN_START);

  darktable.splash.remaining_box =
    dt_gui_hbox(dtgtk_button_new(dtgtk_cairo_paint_clock, 0, 0),
                darktable.splash.remaining_text);
  gtk_widget_set_halign(GTK_WIDGET(darktable.splash.remaining_box), GTK_ALIGN_START);

  dt_gui_box_add(content, darktable.splash.remaining_box);
  gtk_container_add(GTK_CONTAINER(darktable.splash.start_screen), GTK_WIDGET(content));

  gtk_window_set_default_size(GTK_WINDOW(darktable.splash.start_screen), 700, -1);
  gtk_widget_show_all(darktable.splash.start_screen);
  gtk_widget_hide(darktable.splash.remaining_box);
  _process_all_gui_events();
}

void dt_splash_screen_set_progress(const char *msg)
{
  if(!darktable.splash.start_screen && darktable.splash.create_if_needed)
    dt_splash_screen_create(TRUE);

  if(darktable.splash.start_screen)
  {
    gtk_label_set_text(GTK_LABEL(darktable.splash.progress_text), msg);
    gtk_widget_show(darktable.splash.progress_text);
    gtk_widget_hide(darktable.splash.remaining_box);
    _process_all_gui_events();
    gdk_display_sync(gdk_display_get_default());
  }
}

void dt_splash_screen_set_progress_percent(const char *msg,
                                           const double fraction,
                                           const double elapsed)
{
  if(!darktable.splash.start_screen && darktable.splash.create_if_needed)
    dt_splash_screen_create(TRUE);

  if(darktable.splash.start_screen)
  {
    const int percent = round(100.0 * fraction);
    char *text = g_strdup_printf(msg, percent);
    gtk_label_set_text(GTK_LABEL(darktable.splash.progress_text), text);
    g_free(text);

    if(elapsed >= 2.0 || fraction > 0.01)
    {
      const double total = elapsed / fraction;
      const double remain = (total - elapsed) + 0.5;  // round to full seconds rather than truncating
      const int minutes = remain / 60;
      const int seconds = remain - (60 * minutes);
      char *rem_text = g_strdup_printf(" %4d:%02d", minutes, seconds);
      gtk_label_set_text(GTK_LABEL(darktable.splash.remaining_text), rem_text);
      g_free(rem_text);
      gtk_widget_queue_draw(darktable.splash.remaining_box);
    }
    else
    {
      gtk_label_set_text(GTK_LABEL(darktable.splash.remaining_text), "   --:--");
    }
    gtk_widget_show_all(darktable.splash.start_screen);
    _process_all_gui_events();
  }
}

void dt_splash_screen_destroy()
{
  if(darktable.splash.start_screen)
  {
    gtk_widget_destroy(darktable.splash.progress_text);
    darktable.splash.progress_text = NULL;
    gtk_widget_destroy(darktable.splash.start_screen);
    darktable.splash.start_screen = NULL;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
