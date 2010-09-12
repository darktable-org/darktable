/*
    This file is part of darktable,
    copyright (c) 2009--2010 Henrik Andersson.

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

#include <glade/glade.h>
#include  "control/conf.h"
#include  "common/darktable.h"
#include  "libs/lib.h"
#include "gui/gtk.h"
#include "gui/panel_sizegroup.h"

static GtkSizeGroup *_panel_size_group = NULL;

static const int scrollbarwidth = 14;

void 
dt_gui_panel_sizegroup_init()
{
  _panel_size_group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
}

void 
dt_gui_panel_sizegroup_add (GtkWidget *w) 
{
  gtk_size_group_add_widget(_panel_size_group, w);
}

void 
dt_gui_panel_sizegroup_remove (GtkWidget *w)
{
  gtk_size_group_remove_widget (_panel_size_group, w);
}

