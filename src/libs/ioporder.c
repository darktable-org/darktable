/*
    This file is part of darktable,
    copyright (c) 2019-2020 pascal obry.

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

DT_MODULE(1)

typedef struct dt_lib_ioporder_t
{
  int current_mode;
  GList *last_custom_iop_order;
  GtkWidget *widget;
} dt_lib_ioporder_t;

const char *name(dt_lib_module_t *self)
{
  return _("module order version");
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

int position()
{
  return 880;
}

void update(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  int mode = DT_IOP_ORDER_CUSTOM;

  const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(darktable.develop->iop_order_list);

  if(kind == DT_IOP_ORDER_CUSTOM)
  {
    gchar *iop_order_list = dt_ioppr_serialize_text_iop_order_list(darktable.develop->iop_order_list);

    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT op_params"
                                " FROM data.presets "
                                " WHERE operation='ioporder'"
                                " ORDER BY writeprotect DESC", -1, &stmt, NULL);

    int index = 1;
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const char *params = (char *)sqlite3_column_blob(stmt, 0);
      const int32_t params_len = sqlite3_column_bytes(stmt, 0);
      GList *iop_list = dt_ioppr_deserialize_iop_order_list(params, params_len);
      gchar *iop_list_text = dt_ioppr_serialize_text_iop_order_list(iop_list);
      g_list_free(iop_list);

      if(!strcmp(iop_order_list, iop_list_text))
      {
        mode = index;
        g_free(iop_list_text);
        break;
    }

      g_free(iop_list_text);
      index ++;
    }

    sqlite3_finalize(stmt);

    g_free(iop_order_list);
  }
  else
  {
    mode = kind;
  }

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_bauhaus_combobox_set(d->widget, mode);
  d->current_mode = mode;

  darktable.gui->reset = reset;
}

static void change_order_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  sqlite3_stmt *stmt;

  int mode = dt_bauhaus_combobox_get(widget);
  const int32_t imgid = darktable.develop->image_storage.id;

  if(d->current_mode == DT_IOP_ORDER_CUSTOM && mode != DT_IOP_ORDER_CUSTOM)
  {
    // changing from custom to something else, keep this custom iop-order to
    // restore it if needed.
    d->last_custom_iop_order = dt_ioppr_iop_order_list_duplicate(darktable.develop->iop_order_list);
  }

  if(mode == DT_IOP_ORDER_CUSTOM) // custom order
  {
    if(d->last_custom_iop_order)
    {
      // last custom is defined, restore it

      dt_ioppr_write_iop_order(DT_IOP_ORDER_CUSTOM, d->last_custom_iop_order, imgid);

      g_list_free_full(d->last_custom_iop_order, free);
      d->last_custom_iop_order = NULL;

      dt_ioppr_migrate_iop_order(darktable.develop, imgid);

      // invalidate buffers and force redraw of darkroom
      dt_dev_invalidate_all(darktable.develop);

      d->current_mode = DT_IOP_ORDER_CUSTOM;
    }
    else
    {
      // do not allow changing to the first mode, reset back to previous value
      const int reset = darktable.gui->reset;
      darktable.gui->reset = 1;
      dt_bauhaus_combobox_set(widget, d->current_mode);
      darktable.gui->reset = reset;
    }

    return;
  }

  const char *name = dt_bauhaus_combobox_get_text(widget);

  if(d->current_mode != mode
     || d->current_mode == DT_IOP_ORDER_CUSTOM)
  {
    // ensure current history is written
    dt_dev_write_history(darktable.develop);

    // if we have multi-instances, save them as we will need to add them back into the iop-list order

    GList *mi = dt_ioppr_extract_multi_instances_list(darktable.develop->iop_order_list);

    // this is a preset as all built-in orders are filed before (see _fill_iop_order)

    if(mode >= DT_IOP_ORDER_LAST)
    {
      const int preset_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), name));

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT op_params"
                                  " FROM data.presets "
                                  " WHERE operation='ioporder' AND rowid=?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, preset_id);

      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const char *params = (char *)sqlite3_column_blob(stmt, 0);
        const int32_t params_len = sqlite3_column_bytes(stmt, 0);
        GList *iop_list = dt_ioppr_deserialize_iop_order_list(params, params_len);

        sqlite3_finalize(stmt);

        if(mi) iop_list = dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi);

        dt_ioppr_write_iop_order(DT_IOP_ORDER_CUSTOM, iop_list, imgid);

        g_list_free_full(iop_list, free);
      }
    }
    else
    {
      if(mi)
      {
        GList *iop_list = dt_ioppr_get_iop_order_list_version(mode);
        iop_list = dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi);

        dt_ioppr_write_iop_order(DT_IOP_ORDER_CUSTOM, iop_list, imgid);

        g_list_free_full(iop_list, free);
      }
      else
        dt_ioppr_write_iop_order(mode, NULL, imgid);
    }

    g_list_free(mi);

    dt_ioppr_migrate_iop_order(darktable.develop, imgid);

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(darktable.develop);
  }

  d->current_mode = mode;
}

static void _fill_iop_order(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_bauhaus_combobox_clear(d->widget);

  dt_bauhaus_combobox_add(d->widget, _("custom order"));

  // fill preset iop-order

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT rowid, name"
                              " FROM data.presets "
                              " WHERE operation='ioporder'"
                              " ORDER BY writeprotect DESC, name", -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t rowid = sqlite3_column_int(stmt, 0);
    const char *name = (char *)sqlite3_column_text(stmt, 1);
    dt_bauhaus_combobox_add(d->widget, name);
    g_object_set_data(G_OBJECT(d->widget), name, GUINT_TO_POINTER(rowid));
  }

  sqlite3_finalize(stmt);

  darktable.gui->reset = reset;
}

static void _image_loaded_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  _fill_iop_order(self);
  update(self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)malloc(sizeof(dt_lib_ioporder_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  d->widget = dt_bauhaus_combobox_new(NULL);
  d->current_mode = -1;
  d->last_custom_iop_order = NULL;

  _fill_iop_order(self);

  gtk_widget_set_tooltip_text
    (d->widget,
     _("custom\t: a customr iop-order\n"
       "legacy\t\t: legacy iop order used prior to v3.0\n"
       "v3.0\t\t: iop-order introduced in v3.0"));
  g_signal_connect(G_OBJECT(d->widget), "value-changed",
                   G_CALLBACK(change_order_callback), (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), d->widget, TRUE, TRUE, 0);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(_image_loaded_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_image_loaded_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_image_loaded_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

void gui_reset (dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  // the module reset is use to select the v3.0 iop-order
  dt_bauhaus_combobox_set(d->widget, DT_IOP_ORDER_V30);
}

void init_presets(dt_lib_module_t *self)
{
  size_t size = 0;
  char *params = NULL;
  GList *list;

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("legacy"), self->plugin_name, self->version(), (const char *)params, (int32_t)size);
  free(params);

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("v3.0 (default)"), self->plugin_name, self->version(), (const char *)params, (int32_t)size);
  free(params);
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  GList *iop_order_list = dt_ioppr_deserialize_iop_order_list(params, (size_t)size);

  if(iop_order_list)
  {
    const int32_t imgid = darktable.develop->image_storage.id;

    dt_dev_write_history(darktable.develop);

    dt_ioppr_write_iop_order_list(iop_order_list, imgid);

    // invalidate dev and force redraw of darkroom

    dt_ioppr_migrate_iop_order(darktable.develop, imgid);

    dt_dev_invalidate_all(darktable.develop);

    _fill_iop_order(self);
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
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  void *params = NULL;

  // do not allow recording unsafe or built-in iop-order
  // only custom order can be recorded.
  if(d->current_mode == DT_IOP_ORDER_CUSTOM)
  {
    size_t p_size = 0;
    params = dt_ioppr_serialize_iop_order_list(darktable.develop->iop_order_list, &p_size);
    *size = (int)p_size;
  }

  return params;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
