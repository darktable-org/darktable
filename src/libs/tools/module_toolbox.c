/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include "control/signal.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

/* proxy function, to add a widget to toolbox */
static void _lib_module_toolbox_add(dt_lib_module_t *self, GtkWidget *widget, dt_view_type_flags_t views);


typedef struct child_data_t
{
  GtkWidget * child;
  dt_view_type_flags_t views;

} child_data_t;

typedef struct dt_lib_module_toolbox_t
{
  GtkWidget *container;
  GList * child_views;
} dt_lib_module_toolbox_t;

const char *name(dt_lib_module_t *self)
{
  return _("Module toolbox");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", "lighttable", "tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT;
}

int expandable(dt_lib_module_t *self)
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
  d->container = self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  /* setup proxy */
  darktable.view_manager->proxy.module_toolbox.module = self;
  darktable.view_manager->proxy.module_toolbox.add = _lib_module_toolbox_add;
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)self->data;
  g_list_free_full(d->child_views,free);
  g_free(self->data);
  self->data = NULL;
}

void view_enter(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)self->data;
  dt_view_type_flags_t nv= new_view->view(new_view);
  for(const GList *child_elt = d->child_views; child_elt; child_elt = g_list_next(child_elt))
  {
    child_data_t* child_data = (child_data_t*)child_elt->data;
    if(child_data->views & nv)
    {
      gtk_widget_show_all(child_data->child);
    }
    else
    {
      gtk_widget_hide(child_data->child);
    }
  }
}

static void _lib_module_toolbox_add(dt_lib_module_t *self, GtkWidget *widget, dt_view_type_flags_t views)
{
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)self->data;
  gtk_box_pack_start(GTK_BOX(d->container), widget, TRUE, FALSE, 0);
  gtk_widget_show_all(widget);

  child_data_t *child_data = malloc(sizeof(child_data_t));
  child_data->child = widget;
  child_data->views = views;
  d->child_views = g_list_prepend(d->child_views,child_data);

}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

