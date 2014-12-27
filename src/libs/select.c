/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2010--2011 henrik andersson.

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
#include "common/darktable.h"
#include "common/collection.h"
#include "common/selection.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>
#include "dtgtk/button.h"

DT_MODULE(1)

const char *name()
{
  return _("select");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_select_t
{
  GtkWidget *select_all_button, *select_none_button, *select_invert_button, *select_film_roll_button,
      *select_untouched_button;
} dt_lib_select_t;

static void button_clicked(GtkWidget *widget, gpointer user_data)
{
  switch(GPOINTER_TO_INT(user_data))
  {
    case 0: // all
      dt_selection_select_all(darktable.selection);
      break;
    case 1: // none
      dt_selection_clear(darktable.selection);
      break;
    case 2: // invert
      dt_selection_invert(darktable.selection);
      break;
    case 4: // untouched
      dt_selection_select_unaltered(darktable.selection);
      break;
    default: // case 3: same film roll
      dt_selection_select_filmroll(darktable.selection);
  }

  dt_control_queue_redraw_center();
}

int position()
{
  return 800;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_select_t *d = (dt_lib_select_t *)malloc(sizeof(dt_lib_select_t));
  self->data = d;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;
  GtkWidget *button;

  button = gtk_button_new_with_label(_("select all"));
  d->select_all_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("select all images in current collection (ctrl-a)"),
               (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(0));

  button = gtk_button_new_with_label(_("select none"));
  d->select_none_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("clear selection (ctrl-shift-a)"), (char *)NULL);
  gtk_grid_attach(grid, button, 1, line++, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(1));


  button = gtk_button_new_with_label(_("invert selection"));
  g_object_set(G_OBJECT(button), "tooltip-text",
               _("select unselected images\nin current collection (ctrl-!)"), (char *)NULL);
  d->select_invert_button = button;
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(2));

  button = gtk_button_new_with_label(_("select film roll"));
  d->select_film_roll_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text",
               _("select all images which are in the same\nfilm roll as the selected images"), (char *)NULL);
  gtk_grid_attach(grid, button, 1, line++, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(3));


  button = gtk_button_new_with_label(_("select untouched"));
  d->select_untouched_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("select untouched images in\ncurrent collection"),
               (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(4));
}

void gui_cleanup(dt_lib_module_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
