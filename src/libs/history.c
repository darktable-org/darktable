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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/styles.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)


typedef struct dt_undo_history_t
{
  GList *snapshot;
  int end;
} dt_undo_history_t;

typedef struct dt_lib_history_t
{
  /* vbox with managed history items */
  GtkWidget *history_box;
  GtkWidget *create_button;
//   GtkWidget *apply_button;
  GtkWidget *compress_button;
  gboolean record_undo;
} dt_lib_history_t;

/* compress history stack */
static void _lib_history_compress_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_history_button_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget, gpointer user_data);
/* signal callback for history change */
static void _lib_history_change_callback(gpointer instance, gpointer user_data);
static void _lib_history_module_remove_callback(gpointer instance, dt_iop_module_t *module, gpointer user_data);



const char *name(dt_lib_module_t *self)
{
  return _("history");
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
  return 900;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "create style from history"), 0, 0);
//   dt_accel_register_lib(self, NC_("accel", "apply style from popup menu"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "compress history stack"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;

  dt_accel_connect_button_lib(self, "create style from history", d->create_button);
//   dt_accel_connect_button_lib(self, "apply style from popup menu", d->apply_button);
  dt_accel_connect_button_lib(self, "compress history stack", d->compress_button);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_history_t *d = (dt_lib_history_t *)g_malloc0(sizeof(dt_lib_history_t));
  self->data = (void *)d;

  d->record_undo = TRUE;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5));
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_widget_set_name(self->widget, "history-ui");
  d->history_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5));

  d->compress_button = gtk_button_new_with_label(_("compress history stack"));
  gtk_label_set_xalign (GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->compress_button))), 0.0f);
  gtk_widget_set_tooltip_text(d->compress_button, _("create a minimal history stack which produces the same image"));
  g_signal_connect(G_OBJECT(d->compress_button), "clicked", G_CALLBACK(_lib_history_compress_clicked_callback), NULL);

  /* add toolbar button for creating style */
  d->create_button = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_size_request(d->create_button, DT_PIXEL_APPLY_DPI(24), -1);
  g_signal_connect(G_OBJECT(d->create_button), "clicked",
                   G_CALLBACK(_lib_history_create_style_button_clicked_callback), NULL);
  gtk_widget_set_tooltip_text(d->create_button, _("create a style from the current history stack"));

  /* add buttons to buttonbox */
  gtk_box_pack_start(GTK_BOX(hhbox), d->compress_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hhbox), d->create_button, FALSE, FALSE, 0);

  /* add history list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget), d->history_box, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hhbox, FALSE, FALSE, 0);


  gtk_widget_show_all(self->widget);

  /* connect to history change signal for updating the history view */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_lib_history_change_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_REMOVE,
                            G_CALLBACK(_lib_history_module_remove_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_history_change_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_history_module_remove_callback), self);
  g_free(self->data);
  self->data = NULL;
}

static GtkWidget *_lib_history_create_button(dt_lib_module_t *self, int num, const char *label,
                                             gboolean enabled, gboolean selected)
{
  /* create label */
  GtkWidget *widget = NULL;
  gchar numlabel[256];
  if(num == -1)
    g_snprintf(numlabel, sizeof(numlabel), "%d - %s", num + 1, label);
  else
  {
    if(enabled)
      g_snprintf(numlabel, sizeof(numlabel), "%d - %s", num + 1, label);
    else
      g_snprintf(numlabel, sizeof(numlabel), "%d - %s (%s)", num + 1, label, _("off"));
  }

  /* create toggle button */
  widget = gtk_toggle_button_new_with_label(numlabel);
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(widget)), GTK_ALIGN_START);
  g_object_set_data(G_OBJECT(widget), "history_number", GINT_TO_POINTER(num + 1));
  g_object_set_data(G_OBJECT(widget), "label", (gpointer)label);
  if(selected) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);

  /* set callback when clicked */
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_history_button_clicked_callback), self);

  /* associate the history number */
  g_object_set_data(G_OBJECT(widget), "history-number", GINT_TO_POINTER(num + 1));

  return widget;
}

static dt_iop_module_t *get_base_module(GList *iop_list, const char *op)
{
  dt_iop_module_t *result = NULL;

  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(strcmp(mod->op, op) == 0)
    {
      result = mod;
      break;
    }
    modules = g_list_next(modules);
  }

  return result;
}

static GList *_duplicate_history(GList *hist)
{
  GList *result = NULL;

  GList *h = g_list_first(hist);
  while(h)
  {
    const dt_dev_history_item_t *old = (dt_dev_history_item_t *)(h->data);

    dt_dev_history_item_t *new = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));

    memcpy(new, old, sizeof(dt_dev_history_item_t));

    int32_t params_size = 0;
    if(old->module)
    {
      params_size = old->module->params_size;
    }
    else
    {
      dt_iop_module_t *base = get_base_module(darktable.develop->iop, old->op_name);
      if(base)
      {
        params_size = base->params_size;
      }
      else
      {
        // nothing else to do
        fprintf(stderr, "[_duplicate_history] can't find base module for %s\n", old->op_name);
      }
    }

    new->params = malloc(params_size);
    new->blend_params = malloc(sizeof(dt_develop_blend_params_t));

    memcpy(new->params, old->params, params_size);
    memcpy(new->blend_params, old->blend_params, sizeof(dt_develop_blend_params_t));

    result = g_list_append(result, new);

    h = g_list_next(h);
  }
  return result;
}

static void _reset_module_instance(GList *hist, dt_iop_module_t *module, int multi_priority)
{
  while (hist)
  {
    dt_dev_history_item_t *hit = (dt_dev_history_item_t *)hist->data;

    if(!hit->module && strcmp(hit->op_name, module->op) == 0 && hit->multi_priority == multi_priority)
    {
      hit->module = module;
    }
    hist = hist->next;
  }
}

struct _cb_data
{
  dt_iop_module_t *module;
  int multi_priority;
};

static void _undo_items_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *data)
{
  struct _cb_data *udata = (struct _cb_data *)user_data;
  dt_undo_history_t *hdata = (dt_undo_history_t *)data;
  _reset_module_instance(hdata->snapshot, udata->module, udata->multi_priority);
}

static void _history_invalidate_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_undo_history_t *hist = (dt_undo_history_t *)item;
  dt_dev_invalidate_history_module(hist->snapshot, module);
}

static void _add_module_expander(GList *iop_list, dt_iop_module_t *module)
{
  // dt_dev_reload_history_items won't do this for base instances
  // and it will call gui_init() for the rest
  // so we do it here
  if(!dt_iop_is_hidden(module) && !module->expander)
  {
    // since multi_priority is in reverse order, we want the last one
    // that is grather than this
    dt_iop_module_t *base = NULL;
    dt_iop_module_t *base_last = NULL;
    GList *mods = g_list_last(iop_list);
    while(mods)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)(mods->data);

      if(mod != module && mod->instance == module->instance)
      {
        // we save the last one in case module is the last one
        base_last = mod;
        if(mod->multi_priority > module->multi_priority)
        {
          base = mod;
          break;
        }
      }
      mods = g_list_previous(mods);
    }
    if(base == NULL)
    {
      base = base_last;
      base_last = NULL;
    }
    if(base)
    {
      /* add module to right panel */
      GtkWidget *expander = dt_iop_gui_get_expander(module);
      dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
      dt_iop_gui_set_expanded(module, TRUE, FALSE);
      dt_iop_gui_update_blending(module);
    }
    else
      fprintf(stderr, "[_add_module_expander] can't find base for module %s\n", module->op);
  }
}

// return the 1st history entry that matches module
static dt_dev_history_item_t *_search_history_by_module(GList *history_list, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_ret = NULL;

  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hist_item = (dt_dev_history_item_t *)history->data;

    if(hist_item->module == module)
    {
      hist_ret = hist_item;
      break;
    }

    history = g_list_next(history);
  }
  return hist_ret;
}

static int _check_deleted_instances(dt_develop_t *dev, GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int deleted_module_found = 0;

  // we will check on dev->iop if there's a module that is not in history
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    int delete_module = 0;

    // base modules are a special case
    // most base modules won't be in history and must not be deleted
    // but the user may have deleted a base instance of a multi-instance module
    // and then undo and redo, so we will end up with two entries in dev->iop
    // with multi_priority == 0, this can't happen and the extra one must be deleted
    // dev->iop is sorted by (priority, multi_priority DESC), so if the next one is
    // a base instance too, one must be deleted
    if(mod->multi_priority == 0)
    {
      GList *modules_next = g_list_next(modules);
      if(modules_next)
      {
        dt_iop_module_t *mod_next = (dt_iop_module_t *)modules_next->data;
        if(strcmp(mod_next->op, mod->op) == 0 && mod_next->multi_priority == 0)
        {
          // is the same one, check which one must be deleted
          const int mod_in_history = (_search_history_by_module(history_list, mod) != NULL);
          const int mod_next_in_history = (_search_history_by_module(history_list, mod_next) != NULL);

          // current is in history and next is not, delete next
          if(mod_in_history && !mod_next_in_history)
          {
            mod = mod_next;
            modules = modules_next;
            delete_module = 1;
          }
          // current is not in history and next is, delete current
          else if(!mod_in_history && mod_next_in_history)
          {
            delete_module = 1;
          }
          else
          {
            if(mod_in_history && mod_next_in_history)
              fprintf(
                  stderr,
                  "[_check_deleted_instances] found duplicate module %s %s (%i) and %s %s (%i) both in history\n",
                  mod->op, mod->multi_name, mod->multi_priority, mod_next->op, mod_next->multi_name,
                  mod_next->multi_priority);
            else
              fprintf(
                  stderr,
                  "[_check_deleted_instances] found duplicate module %s %s (%i) and %s %s (%i) none in history\n",
                  mod->op, mod->multi_name, mod->multi_priority, mod_next->op, mod_next->multi_name,
                  mod_next->multi_priority);
          }
        }
      }
    }
    // this is a regular multi-instance and must be in history
    else
    {
      delete_module = (_search_history_by_module(history_list, mod) == NULL);
    }

    // if module is not in history we delete it
    if(delete_module)
    {
      deleted_module_found = 1;

      if(darktable.develop->gui_module == mod) dt_iop_request_focus(NULL);

      const int reset = darktable.gui->reset;
      darktable.gui->reset = 1;

      // we remove the plugin effectively
      if(!dt_iop_is_hidden(mod))
      {
        // we just hide the module to avoid lots of gtk critical warnings
        gtk_widget_hide(mod->expander);

        // this is copied from dt_iop_gui_delete_callback(), not sure why the above sentence...
        gtk_widget_destroy(mod->widget);
        dt_iop_gui_cleanup_module(mod);
      }

      iop_list = g_list_remove_link(iop_list, modules);

      // remove it from all snapshots
      dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, mod, &_history_invalidate_cb);

      // we cleanup the module
      dt_accel_disconnect_list(mod->accel_closures);
      dt_accel_cleanup_locals_iop(mod);
      mod->accel_closures = NULL;
      // don't delete the module, a pipe may still need it
      dev->alliop = g_list_append(dev->alliop, mod);

      darktable.gui->reset = reset;

      // and reset the list
      modules = g_list_first(iop_list);
      continue;
    }

    modules = g_list_next(modules);
  }
  if(deleted_module_found) iop_list = g_list_sort(iop_list, sort_plugins);

  *_iop_list = iop_list;

  return deleted_module_found;
}

static void _reorder_gui_module_list(dt_develop_t *dev)
{
  int pos_module = 0;
  GList *modules = g_list_last(dev->iop);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    GtkWidget *expander = module->expander;
    if(expander)
    {
      gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER), expander,
                            pos_module++);
    }

    modules = g_list_previous(modules);
  }
}

static int _rebuild_multi_priority(GList *history_list)
{
  int changed = 0;
  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)history->data;

    // if multi_priority is different in history and dev->iop
    // we keep the history version
    if(hitem->module && hitem->module->multi_priority != hitem->multi_priority)
    {
      hitem->module->multi_priority = hitem->multi_priority;
      changed = 1;
    }

    history = g_list_next(history);
  }
  return changed;
}

static int _create_deleted_modules(GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int changed = 0;
  gboolean done = FALSE;

  GList *l = g_list_first(history_list);
  while(l)
  {
    GList *next = g_list_next(l);
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)l->data;

    // this fixes the duplicate module when undo: hitem->multi_priority = 0;
    if(hitem->module == NULL)
    {
      changed = 1;

      const dt_iop_module_t *base_module = get_base_module(iop_list, hitem->op_name);
      if(base_module == NULL)
      {
        fprintf(stderr, "[_create_deleted_modules] can't find base module for %s\n", hitem->op_name);
        return changed;
      }

      // from there we create a new module for this base instance. The goal is to do a very minimal setup of the
      // new module to be able to write the history items. From there we reload the whole history back and this
      // will recreate the proper module instances.
      dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module(module, base_module->so, base_module->dev))
      {
        return changed;
      }
      module->instance = base_module->instance;

      if(!dt_iop_is_hidden(module))
      {
        module->gui_init(module);
      }

      // adjust the multi_name of the new module
      g_strlcpy(module->multi_name, hitem->multi_name, sizeof(module->multi_name));
      module->multi_priority = hitem->multi_priority;

      // we insert this module into dev->iop
      iop_list = g_list_insert_sorted(iop_list, module, sort_plugins);

      // add the expander, dt_dev_reload_history_items() don't work well without one
      _add_module_expander(iop_list, module);

      // if not already done, set the module to all others same instance
      if(!done)
      {
        _reset_module_instance(history_list, module, hitem->multi_priority);

        // and do that also in the undo/redo lists
        struct _cb_data udata = { module, hitem->multi_priority };
        dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, &udata, &_undo_items_cb);
        done = TRUE;
      }

      hitem->module = module;
    }
    l = next;
  }

  *_iop_list = iop_list;

  return changed;
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_develop_t *dev = darktable.develop;

  if(type == DT_UNDO_HISTORY)
  {
    dt_lib_history_t *d = (dt_lib_history_t *)self->data;
    dt_undo_history_t *hist = (dt_undo_history_t *)data;

    // we will work on a copy of history and modules
    // when we're done we'll replace dev->history and dev->iop
    GList *history_temp = _duplicate_history(hist->snapshot);
    const int hist_end = hist->end;
    GList *iop_temp = g_list_copy(dev->iop);

    // topology has changed?
    int pipe_remove = 0;

    // we have to check if multi_priority has changed since history was saved
    // we will adjust it here
    if(_rebuild_multi_priority(history_temp))
    {
      pipe_remove = 1;
      iop_temp = g_list_sort(iop_temp, sort_plugins);
    }

    // check if this undo a delete module and re-create it
    if(_create_deleted_modules(&iop_temp, history_temp))
    {
      pipe_remove = 1;
    }

    // check if this is a redo of a delete module or an undo of an add module
    if(_check_deleted_instances(dev, &iop_temp, history_temp))
    {
      pipe_remove = 1;
    }

    // disable recording undo as the _lib_history_change_callback will be triggered by the calls below
    d->record_undo = FALSE;

    dt_pthread_mutex_lock(&dev->history_mutex);

    // set history and modules to dev
    GList *history_temp2 = dev->history;
    dev->history = history_temp;
    dev->history_end = hist_end;
    g_list_free_full(history_temp2, dt_dev_free_history_item);
    GList *iop_temp2 = dev->iop;
    dev->iop = iop_temp;
    g_list_free(iop_temp2);

    // topology has changed
    if(pipe_remove)
    {
      // we refresh the pipe
      dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
      dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
      dev->pipe->cache_obsolete = 1;
      dev->preview_pipe->cache_obsolete = 1;

      // invalidate buffers and force redraw of darkroom
      dt_dev_invalidate_all(dev);
    }

    dt_pthread_mutex_unlock(&dev->history_mutex);

    // if dev->iop has changed reflect that on module list
    if(pipe_remove) _reorder_gui_module_list(dev);

    // write new history and reload
    dt_dev_write_history(dev);
    dt_dev_reload_history_items(dev);
  }
}

static void _history_undo_data_free(gpointer data)
{
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  GList *snapshot = hist->snapshot;
  g_list_free_full(snapshot, dt_dev_free_history_item);
  free(data);
}

static void _lib_history_module_remove_callback(gpointer instance, dt_iop_module_t *module, gpointer user_data)
{
  dt_undo_iterate(darktable.undo, DT_UNDO_HISTORY, module, &_history_invalidate_cb);
}

static void _lib_history_change_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;

  /* first destroy all buttons in list */
  gtk_container_foreach(GTK_CONTAINER(d->history_box), (GtkCallback)gtk_widget_destroy, 0);

  /* add default which always should be */
  int num = -1;
  gtk_box_pack_start(GTK_BOX(d->history_box),
                     _lib_history_create_button(self, num, _("original"), FALSE, darktable.develop->history_end == 0),
                     TRUE, TRUE, 0);
  num++;

  if (d->record_undo == TRUE)
  {
    /* record undo/redo history snapshot */
    dt_undo_history_t *hist = malloc(sizeof(dt_undo_history_t));
    hist->snapshot = _duplicate_history(darktable.develop->history);
    hist->end = darktable.develop->history_end;

    dt_undo_record(darktable.undo, self, DT_UNDO_HISTORY, (dt_undo_data_t *)hist,
                   _pop_undo, _history_undo_data_free);
  }
  else
    d->record_undo = TRUE;

  /* lock history mutex */
  dt_pthread_mutex_lock(&darktable.develop->history_mutex);

  /* iterate over history items and add them to list*/
  GList *history = g_list_first(darktable.develop->history);
  while(history)
  {
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)(history->data);

    gchar *label;
    if(!hitem->multi_name[0] || strcmp(hitem->multi_name, "0") == 0)
      label = g_strdup_printf("%s", hitem->module->name());
    else
      label = g_strdup_printf("%s %s", hitem->module->name(), hitem->multi_name);

    gboolean selected = (num == darktable.develop->history_end - 1);
    GtkWidget *widget = _lib_history_create_button(self, num, label, hitem->enabled, selected);
    g_free(label);

    gtk_box_pack_start(GTK_BOX(d->history_box), widget, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(d->history_box), widget, 0);
    num++;

    history = g_list_next(history);
  }

  /* show all widgets */
  gtk_widget_show_all(d->history_box);

  dt_pthread_mutex_unlock(&darktable.develop->history_mutex);
}

static void _lib_history_compress_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  const int imgid = darktable.develop->image_storage.id;
  if(!imgid) return;
  // make sure the right history is in there:
  dt_dev_write_history(darktable.develop);
  sqlite3_stmt *stmt;

  // compress history
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.history WHERE imgid = ?1 AND num "
                                                             "NOT IN (SELECT MAX(num) FROM main.history WHERE "
                                                             "imgid = ?1 AND num < ?2 GROUP BY operation, "
                                                             "multi_priority)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, darktable.develop->history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // load new history and write it back to ensure that all history are properly numbered without a gap
  dt_dev_reload_history_items(darktable.develop);
  dt_dev_write_history(darktable.develop);

  // then we can get the item to select in the new clean-up history retrieve the position of the module
  // corresponding to the history end.
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT IFNULL(MAX(num)+1, 0) FROM main.history "
                                                             "WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if (sqlite3_step(stmt) == SQLITE_ROW)
    darktable.develop->history_end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // select the new history end corresponding to the one before the history compression
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE main.images SET history_end=?2 WHERE id=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, darktable.develop->history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_dev_reload_history_items(darktable.develop);
  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
}

static void _lib_history_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  static int reset = 0;
  if(reset) return;
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return;

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;
  reset = 1;

  /* inactivate all toggle buttons */
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->history_box));
  for(GList *l = children; l != NULL; l = g_list_next(l))
  {
    GtkToggleButton *b = GTK_TOGGLE_BUTTON(l->data);
    if(b != GTK_TOGGLE_BUTTON(widget)) g_object_set(G_OBJECT(b), "active", FALSE, (gchar *)0);
  }
  g_list_free(children);

  reset = 0;
  if(darktable.gui->reset) return;

  /* revert to given history item. */
  int num = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history-number"));
  dt_dev_pop_history_items(darktable.develop, num);
  /* signal history changed */
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
}

static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.develop->image_storage.id)
  {
    dt_dev_write_history(darktable.develop);
    dt_gui_styles_dialog_new(darktable.develop->image_storage.id);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
