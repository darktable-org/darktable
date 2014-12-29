/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"

DT_MODULE(1)

/* proxy function, to add a widget to toolbox */
static void _lib_module_toolbox_add(dt_lib_module_t *self, GtkWidget *widget);

typedef struct dt_lib_module_toolbox_t
{
  GtkWidget *container;
} dt_lib_module_toolbox_t;

const char *name()
{
  return _("module toolbox");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT;
}

int expandable()
{
  return 0;
}

int position()
{
  return 100;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)g_malloc0(sizeof(dt_lib_module_toolbox_t));
  self->data = (void *)d;

  /* the toolbar container */
  d->container = self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  /* setup proxy */
  darktable.view_manager->proxy.module_toolbox.module = self;
  darktable.view_manager->proxy.module_toolbox.add = _lib_module_toolbox_add;
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}


static void _lib_module_toolbox_add(dt_lib_module_t *self, GtkWidget *widget)
{
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)self->data;
  gtk_box_pack_start(GTK_BOX(d->container), widget, TRUE, FALSE, 0);
  gtk_widget_show_all(widget);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
