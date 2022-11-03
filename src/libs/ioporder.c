/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

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
#include "common/debug.h"
#include "control/signal.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <dtgtk/expander.h>

DT_MODULE(1)

typedef struct dt_lib_ioporder_t
{
  int current_mode;
  GList *last_custom_iop_order;
  GtkWidget *widget;
} dt_lib_ioporder_t;

const char *name(dt_lib_module_t *self)
{
  return _("module order");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM;
}

int position(const dt_lib_module_t *self)
{
  return 880;
}

void update(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  if(!d->widget)
  {
    if(!self->expander) return;

    d->widget = gtk_label_new("");
    g_signal_connect(G_OBJECT(d->widget), "destroy", G_CALLBACK(gtk_widget_destroyed), &d->widget);
    gtk_widget_show(d->widget);
    gtk_box_pack_start(GTK_BOX(dtgtk_expander_get_header(DTGTK_EXPANDER(self->expander))), d->widget, TRUE, TRUE, 0);

    gtk_widget_destroy(self->arrow);
    self->arrow = NULL;
  }

  const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(darktable.develop->iop_order_list);

  if(kind == DT_IOP_ORDER_CUSTOM)
  {
    gchar *iop_order_list = dt_ioppr_serialize_text_iop_order_list(darktable.develop->iop_order_list);
    gboolean found = FALSE;
    int index = 0;

    sqlite3_stmt *stmt;

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT op_params, name"
                                " FROM data.presets"
                                " WHERE operation='ioporder'"
                                " ORDER BY writeprotect DESC", -1, &stmt, NULL);
    // clang-format on

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const char *params = (char *)sqlite3_column_blob(stmt, 0);
      const int32_t params_len = sqlite3_column_bytes(stmt, 0);
      const char *name = (const char *)sqlite3_column_text(stmt, 1);
      GList *iop_list = dt_ioppr_deserialize_iop_order_list(params, params_len);
      gchar *iop_list_text = dt_ioppr_serialize_text_iop_order_list(iop_list);
      g_list_free(iop_list);
      index++;

      if(!strcmp(iop_order_list, iop_list_text))
      {
        gtk_label_set_text(GTK_LABEL(d->widget), name);
        d->current_mode = index;
        found = TRUE;
        g_free(iop_list_text);
        break;
    }

      g_free(iop_list_text);
    }

    sqlite3_finalize(stmt);

    g_free(iop_order_list);

    if(!found)
    {
      d->current_mode = DT_IOP_ORDER_CUSTOM;
      gtk_label_set_text(GTK_LABEL(d->widget), _(dt_iop_order_string(d->current_mode)));
    }
  }
  else
  {
    d->current_mode = kind;
    gtk_label_set_text(GTK_LABEL(d->widget), _(dt_iop_order_string(d->current_mode)));
  }
}

static void _image_loaded_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  update(self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)malloc(sizeof(dt_lib_ioporder_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  d->widget = NULL; // initialise in first update when header has been set up
  d->current_mode = -1;
  d->last_custom_iop_order = NULL;

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(_image_loaded_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_image_loaded_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_image_loaded_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  if(d->widget) gtk_widget_destroy(d->widget);
  free(self->data);
  self->data = NULL;
}

void gui_reset (dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  // the module reset is use to select the v3.0 iop-order

  GList *iop_order_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);

  if(iop_order_list)
  {
    const int32_t imgid = darktable.develop->image_storage.id;

    dt_ioppr_change_iop_order(darktable.develop, imgid, iop_order_list);

    dt_dev_pixelpipe_rebuild(darktable.develop);

    d->current_mode = DT_IOP_ORDER_V30;
    if(d->widget) gtk_label_set_text(GTK_LABEL(d->widget), _(dt_iop_order_string(d->current_mode)));

    g_list_free_full(iop_order_list, free);
  }
}

void init_presets(dt_lib_module_t *self)
{
  size_t size = 0;
  char *params = NULL;
  GList *list;

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("legacy"), self->plugin_name, self->version(), (const char *)params, (int32_t)size, TRUE);
  free(params);

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("v3.0 for RAW input (default)"), self->plugin_name, self->version(), (const char *)params, (int32_t)size,
                     TRUE);

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30_JPG);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("v3.0 for JPEG/non-RAW input"), self->plugin_name, self->version(), (const char *)params, (int32_t)size,
                     TRUE);
  free(params);
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  GList *iop_order_list = dt_ioppr_deserialize_iop_order_list(params, (size_t)size);

  if(iop_order_list)
  {
    const int32_t imgid = darktable.develop->image_storage.id;

    dt_ioppr_change_iop_order(darktable.develop, imgid, iop_order_list);

    dt_dev_pixelpipe_rebuild(darktable.develop);

    update(self);

    g_list_free_full(iop_order_list, free);
    return 0;
  }
  else
  {
    return 1;
  }
}

void *get_params(dt_lib_module_t *self, int *size)
{
  size_t p_size = 0;
  void *params = dt_ioppr_serialize_iop_order_list(darktable.develop->iop_order_list, &p_size);
  *size = (int)p_size;

  return params;
}

gboolean preset_autoapply(dt_lib_module_t *self)
{
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
