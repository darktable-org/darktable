/*
    This file is part of darktable,
    Copyright (C) 2011-2022 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

typedef struct dt_lib_tool_filter_t
{
  GtkWidget *filter_box;
  GtkWidget *sort_box;
  GtkWidget *count;
} dt_lib_tool_filter_t;

const char *name(dt_lib_module_t *self)
{
  return _("filter");
}

const char **views(dt_lib_module_t *self)
{
  /* for now, show in all view due this affects filmroll too

     TODO: Consider to add flag for all views, which prevents
           unloading/loading a module while switching views.

   */
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 2001;
}

static GtkWidget *_lib_filter_get_filter_box(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->filter_box;
}
static GtkWidget *_lib_filter_get_sort_box(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->sort_box;
}

static GtkWidget *_lib_filter_get_count(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->count;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)g_malloc0(sizeof(dt_lib_tool_filter_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(self->widget, GTK_ALIGN_CENTER);

  d->filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->filter_box, "header-rule-box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->filter_box, FALSE, FALSE, 0);

  /* sort combobox */
  d->sort_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->sort_box, "header-sort-box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->sort_box, FALSE, FALSE, 0);
  GtkWidget *label = gtk_label_new(_("sort by"));
  gtk_box_pack_start(GTK_BOX(d->sort_box), label, TRUE, TRUE, 0);

  /* label to display selected count */
  d->count = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->count), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->count, TRUE, FALSE, 0);

  /* initialize proxy */
  darktable.view_manager->proxy.filter.module = self;
  darktable.view_manager->proxy.filter.get_filter_box = _lib_filter_get_filter_box;
  darktable.view_manager->proxy.filter.get_sort_box = _lib_filter_get_sort_box;
  darktable.view_manager->proxy.filter.get_count = _lib_filter_get_count;

  // test if the filtering module is already load and update its gui in this case
  // otherwise filtering module will do it in its gui_init()
  if(darktable.view_manager->proxy.module_filtering.module)
  {
    darktable.view_manager->proxy.module_filtering.update(darktable.view_manager->proxy.module_filtering.module);
  }
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

