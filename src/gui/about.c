/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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
#include "about.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

void darktable_show_about_dialog()
{
  GtkWidget *dialog = gtk_about_dialog_new();
  gtk_widget_set_name (dialog, "about-dialog");
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), darktable_package_version);
  char *copyright = g_strdup_printf(_("copyright (c) the authors 2009-%s"),
                                    darktable_last_commit_year);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), copyright);
  g_free(copyright);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                _("organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "https://www.darktable.org/");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog), "website");
  const dt_logo_season_t season = dt_util_get_logo_season();
  char *icon;
  if(season != DT_LOGO_SEASON_NONE)
    icon = g_strdup_printf("darktable-%d", (int)season);
  else
    icon = g_strdup("darktable");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), icon);
  g_free(icon);

  const char *str = _("all those of you that made previous releases possible");

#include "tools/darktable_authors.h"

  const char *final[] = { str, NULL };
  gtk_about_dialog_add_credit_section (GTK_ABOUT_DIALOG(dialog), _("and..."), final);

  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog),
                                          _("translator-credits"));

  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
