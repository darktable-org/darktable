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

#include "gui/gtk.h"
#include "splash.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

static GtkWidget *splash_screen = NULL;

void darktable_splash_screen_create(GtkWindow *parent_window)
{
  if(splash_screen)
    return;
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
  splash_screen = gtk_message_dialog_new_with_markup(parent_window, flags, GTK_MESSAGE_INFO, GTK_BUTTONS_NONE,
                                                     _("<span size='200%%'>Starting darktable %.5s</span>"),
                                                     darktable_package_version);
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(splash_screen), " ");
  gtk_widget_show(splash_screen);
  // give Gtk a chance to update the screen; we need to let the event processing run a few dozen
  // times for the splash window to actually be fully displayed
  for(int i = 0; i < 50; i++)
    g_main_context_iteration(NULL, FALSE);
}

void darktable_splash_screen_set_progress(const char *msg)
{
  if(splash_screen)
  {
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(splash_screen), "%s", msg);
    gtk_widget_show(splash_screen);
    // give Gtk a chance to update the screen; we need to let the event processing run several
    // hundred times to ensure that the splash window is fully updated on screen, since by the
    // time we are called, other stuff may have been hooked into the Gtk event system and will
    // be receiving events
    for(int i = 0; i < 400; i++)
      g_main_context_iteration(NULL, FALSE);
  }
}

void darktable_splash_screen_destroy()
{
  if(splash_screen)
  {
    gtk_widget_destroy(splash_screen);
    splash_screen = NULL;
  }
}
