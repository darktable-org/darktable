/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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

#include "gui/preferences.h"

void dt_gui_preferences_show()
{
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      _("darktable preferences"), NULL, 
      GTK_DIALOG_MODAL,
      _("cancel"),
      GTK_RESPONSE_NONE,
      _("apply"),
      GTK_RESPONSE_ACCEPT,
      NULL);
  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *notebook = gtk_notebook_new();

  const int num_tabs = 2;
  const char *labels = {
    _("gui"),
    _("core"),
  };

  gtk_notebook_append_page(
  
}
