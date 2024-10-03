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

#include "control/conf.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "splash.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

// override window manager's title bar?
#define USE_HEADER_BAR

// include a featured image on the splash screen?

//#define USE_FEATURED_IMAGE

// number of featured images from which to randomly select when
// USE_SPLASHSCREEN_IMAGE not defined
#define MAX_IMAGES 4

#define ICON_SIZE 250

#ifdef USE_FEATURED_IMAGE
#define PROGNAME_SIZE 300
#else
#define PROGNAME_SIZE 480
#endif

static GtkWidget *splash_screen = NULL;
static GtkWidget *progress_text = NULL;
static GtkWidget *remaining_text = NULL;
static gboolean showing_remaining = FALSE;
static GtkWidget *remaining_box = NULL;

static GtkWidget *exit_screen = NULL;

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

void darktable_splash_screen_create(GtkWindow *parent_window,
                                    const gboolean force)
{
  // no-op if the splash has already been created; if not, only run if
  // the splash screen is enabled in the config or we are told to
  // create it regardless.
  if(splash_screen
     || dt_check_gimpmode("file")
     || dt_check_gimpmode("thumb")
     || (!dt_conf_get_bool("show_splash_screen") && !force))
  {
    return;
  }

  // a simple gtk_dialog_new() leaves us unable to setup the header
  // bar, so use .._with_buttons and just specify a NULL strings to
  // have no buttons.  We need to pretend to actually have one button,
  // though, to keep the compiler happy
#ifdef USE_HEADER_BAR
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
#else
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
#endif
  splash_screen =
    gtk_dialog_new_with_buttons(_("darktable starting"),
                                parent_window, flags,
                                NULL,
                                GTK_RESPONSE_NONE, // <-- fake button list for compiler
                                NULL);
  gtk_window_set_position(GTK_WINDOW(splash_screen), GTK_WIN_POS_CENTER);
  gtk_widget_set_name(splash_screen, "splashscreen");
  progress_text = gtk_label_new(_("initializing"));
  gtk_widget_set_name(progress_text, "splashscreen-progress");
  remaining_text = gtk_label_new("");
  gtk_widget_set_name(remaining_text, "splashscreen-remaining");
  _set_header_bar(splash_screen);
  int version_len = strlen(darktable_package_version);
  char *delim = strchr(darktable_package_version, '~');
  if(delim)
    version_len = delim  - darktable_package_version;
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
  // make a random selection of featured image based on the current
  // time
  const int imgnum = (int)(1 + (clock()%MAX_IMAGES));
  //FIXME: if user overrides --datadir, we won't find the image...
  gchar *image_file = g_strdup_printf("%s/pixmaps/splashscreen-%02d.jpg",
                                      darktable.datadir, imgnum);
  GtkWidget *image = gtk_image_new_from_file(image_file);
  g_free(image_file);
  gtk_widget_set_name(GTK_WIDGET(image), "splashscreen-image");
  // make a vertical stack of the darktable logo, name, and description
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 180);
  GtkWidget *program_desc =
    GTK_WIDGET(gtk_label_new(_("Photography workflow\napplication and\nRAW developer")));
  gtk_label_set_justify(GTK_LABEL(program_desc), GTK_JUSTIFY_CENTER);
  gtk_widget_set_name(program_desc, "splashscreen-description");

  dt_gui_box_add(content, dt_gui_hbox(dt_gui_vbox(logo, version, program_name, program_desc), image));
#else
  // put the darktable logo and version number together
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 220);
  gtk_label_set_justify(GTK_LABEL(version), GTK_JUSTIFY_LEFT);

  // put the darktable wordmark and description in a vertical stack
  GtkWidget *program_desc = GTK_WIDGET(gtk_label_new(_("Photography workflow application\nand RAW developer")));
  gtk_label_set_justify(GTK_LABEL(program_desc), GTK_JUSTIFY_RIGHT);
  gtk_widget_set_halign(program_desc, GTK_ALIGN_END);
  gtk_widget_set_name(program_desc, "splashscreen-description");

  GtkWidget *prepare = gtk_label_new(_("get ready to unleash your creativity"));
  gtk_widget_set_name(prepare, "splashscreen-prepare");

  dt_gui_box_add(content,
                 dt_gui_hbox(dt_gui_vbox(logo, version, copyright),
                 dt_gui_vbox(gtk_label_new(NULL), program_name, program_desc, gtk_label_new(NULL), prepare)));
#endif

  GtkWidget *hbar = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_name(hbar, "splashscreen-separator");
  gtk_widget_show(hbar);

  remaining_box = dt_gui_hbox(dtgtk_button_new(dtgtk_cairo_paint_clock, 0, 0), remaining_text);
  gtk_widget_set_halign(GTK_WIDGET(remaining_box), GTK_ALIGN_CENTER);

  dt_gui_box_add(content, hbar, progress_text, remaining_box);

  gtk_window_set_decorated(GTK_WINDOW(splash_screen), FALSE);
  gtk_widget_show_all(splash_screen);
  gtk_widget_hide(remaining_box);
  _process_all_gui_events();
}

void darktable_splash_screen_set_progress(const char *msg)
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

void darktable_splash_screen_set_progress_percent(const char *msg,
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
      const double remain = (total - elapsed) + 0.5;  // round to full seconds rather than truncating
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

void darktable_exit_screen_create(GtkWindow *parent_window,
                                  const gboolean force)
{
  // no-op if the exit screen has already been created; if not, only
  // run if the splash screen is enabled in the config or we are told
  // to create it regardless
  if(exit_screen
     || dt_check_gimpmode("file")
     || dt_check_gimpmode("thumb")
     || (!dt_conf_get_bool("show_splash_screen") && !force))
  {
    return;
  }

  // a simple gtk_dialog_new() leaves us unable to setup the header
  // bar, so use .._with_buttons and just specify a NULL strings to
  // have no buttons.  We need to pretend to actually have one button,
  // though, to keep the compiler happy.

#ifdef USE_HEADER_BAR
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
#else
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
#endif

  exit_screen =
    gtk_dialog_new_with_buttons(_("darktable shutdown"), parent_window, flags,
                                NULL,
                                GTK_RESPONSE_NONE,  // <-- fake button list for compiler
                                NULL);
  gtk_window_set_position(GTK_WINDOW(exit_screen), GTK_WIN_POS_CENTER);
  gtk_widget_set_name(exit_screen, "splashscreen");
  _set_header_bar(exit_screen);
  GtkWidget *program_name = _get_program_name();
  GtkWidget *logo = _get_logo();
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 220);
  GtkBox *header_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(header_box, logo, FALSE, FALSE, 0);
  gtk_box_pack_start(header_box, program_name, FALSE, FALSE, 0);
  GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(exit_screen)));
  gtk_box_pack_start(content, GTK_WIDGET(header_box), FALSE, FALSE, 0);
  GtkWidget *message1 = gtk_label_new(_("darktable is now shutting down"));
  gtk_widget_set_name(message1, "exitscreen-message");
  GtkWidget *message2 = gtk_label_new(_("please wait while background jobs finish"));
  gtk_widget_set_name(message2, "exitscreen-message");
  gtk_box_pack_start(content, message1, FALSE, FALSE, 0);
  gtk_box_pack_start(content, message2, FALSE, FALSE, 0);
  gtk_widget_show_all(exit_screen);
  _process_all_gui_events();

  // allow it to be hidden by other windows:
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
