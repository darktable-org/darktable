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

enum dt_ioporder_t
{
  DT_IOP_ORDER_UNSAFE      = 0,
  DT_IOP_ORDER_CUSTOM      = 1,
  DT_IOP_ORDER_LEGACY      = 2,
  DT_IOP_ORDER_RECOMMENDED = 3,
  DT_IOP_ORDER_LAST
} dt_ioporder_t;

typedef struct dt_lib_ioporder_t
{
  int current_mode;
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
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 880;
}

void update(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  const int32_t imgid = darktable.develop->image_storage.id;

  int current_iop_order_version = dt_image_get_iop_order_version(imgid);
  GList *iop_order_list = dt_ioppr_get_iop_order_list(&current_iop_order_version, TRUE);

  int mode = DT_IOP_ORDER_UNSAFE;

  if (current_iop_order_version == 2)
    mode = DT_IOP_ORDER_LEGACY;
  else if(current_iop_order_version == 5)
    mode = DT_IOP_ORDER_RECOMMENDED;

  /*
     Check if user has changed the order (custom order).
     We do not check for iop-order but the actual order of modules
     compared to the canonical order of modules for the given iop-order version.
  */
  GList *modules = g_list_first(darktable.develop->iop);
  GList *iop = iop_order_list;

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(strcmp(mod->op, "mask_manager"))
    {
      // check module in iop_order_list
      if(iop && !strcmp(((dt_iop_order_entry_t *)iop->data)->operation, "mask_manager")) iop = g_list_next(iop);

      if(iop)
      {
        dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)iop->data;
        if(strcmp(entry->operation, mod->op))
        {
          mode = DT_IOP_ORDER_CUSTOM;
          break;
        }
        iop = g_list_next(iop);
      }
    }

    // skip all same modules (duplicate instances) if any
    while(modules && !strcmp(((dt_iop_module_t *)modules->data)->op, mod->op)) modules = g_list_next(modules);
  }

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_bauhaus_combobox_set(d->widget, mode);
  d->current_mode = mode;

  darktable.gui->reset = reset;

  if(mode == DT_IOP_ORDER_UNSAFE)
    dt_control_log("this picture is using an unsafe iop-order, please select a proper one");
}

static void change_order_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  const int mode = dt_bauhaus_combobox_get(widget);
  const int32_t imgid = darktable.develop->image_storage.id;
  const int current_iop_order_version = dt_image_get_iop_order_version(imgid);
  int new_iop_order_version = DT_IOP_ORDER_UNSAFE;

  if(mode <= DT_IOP_ORDER_CUSTOM)
  {
    dt_bauhaus_combobox_set(widget, d->current_mode);
    return;
  }

  if(mode == DT_IOP_ORDER_LEGACY)
    new_iop_order_version = 2;
  else if(mode == DT_IOP_ORDER_RECOMMENDED)
    new_iop_order_version = 5;

  if(current_iop_order_version != new_iop_order_version
     || d->current_mode == DT_IOP_ORDER_CUSTOM)
  {
    dt_dev_write_history(darktable.develop);

    dt_ioppr_migrate_iop_order(darktable.develop, imgid, current_iop_order_version, new_iop_order_version);

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(darktable.develop);
  }

  d->current_mode = mode;
}

static void _image_loaded_callback(gpointer instace, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  update(self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)malloc(sizeof(dt_lib_ioporder_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  d->widget = dt_bauhaus_combobox_new(NULL);

  dt_bauhaus_combobox_add(d->widget, _("unsafe, select one below"));
  dt_bauhaus_combobox_add(d->widget, _("custom order"));
  dt_bauhaus_combobox_add(d->widget, _("legacy"));
  dt_bauhaus_combobox_add(d->widget, _("recommended"));
  dt_bauhaus_combobox_set(d->widget, 0);
  gtk_widget_set_tooltip_text
    (d->widget,
     _("information:\n"
       "  unsafe\t\t: an unsafe/broken iop-order, select one below\n"
       "  custom\t\t: a customr iop-order\n"
       "or select an iop-order version either:\n"
       "  legacy\t\t\t: legacy iop order used prior to 3.0\n"
       "  recommended\t: newly iop-order introduced in v3.0"));
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
  // the module reset is use to select the recommended iop-order
  dt_bauhaus_combobox_set(d->widget, DT_IOP_ORDER_RECOMMENDED);
}

void init_presets(dt_lib_module_t *self)
{
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  const int32_t imgid = darktable.develop->image_storage.id;

  // get the parameters buffer
  const char *buf = (char *)params;

  // load all params and create the iop-list

  const int current_iop_order = dt_image_get_iop_order_version(imgid);

  int32_t iop_order_version = 0;

  GList *iop_order_list = dt_ioppr_deserialize_iop_order_list(buf, size, &iop_order_version);

  // set pipe iop order

  dt_dev_write_history(darktable.develop);

  dt_ioppr_migrate_iop_order(darktable.develop, imgid, current_iop_order, iop_order_version);

  // invalidate buffers and force redraw of darkroom

  dt_dev_invalidate_all(darktable.develop);

  update(self);

  if(iop_order_list) g_list_free(iop_order_list);

  return 0;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  // do not allow recording unsafe or built-in iop-order
  // only custom order can be recorded.
  if(d->current_mode != DT_IOP_ORDER_CUSTOM) return NULL;

  GList *modules = g_list_first(darktable.develop->iop);

  // compute size of all modules
  *size = sizeof(int32_t);

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    *size += strlen(mod->op) + sizeof(int32_t);
    modules = g_list_next(modules);
  }

  // allocate the parameter buffer
  char *params = (char *)malloc(*size);

  // store all modules in proper order
  modules = g_list_first(darktable.develop->iop);

  int pos = 0;

  int count = DT_IOP_ORDER_PRESETS_START_ID + 1;

  // add the count of all current ioporder presets

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*)"
                              " FROM data.presets "
                              " WHERE operation='ioporder'", -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    count += sqlite3_column_int(stmt, 0);
  }

  sqlite3_finalize(stmt);

  // set set preset iop-order version

  memcpy(params+pos, &count, sizeof(int32_t));
  pos += sizeof(int32_t);

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    const int32_t len = strlen(mod->op);

    memcpy(params+pos, &len, sizeof(int32_t));
    pos += sizeof(int32_t);
    memcpy(params+pos, mod->op, len);
    pos += len;

    modules = g_list_next(modules);
  }

  return params;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
