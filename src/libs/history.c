/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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
#include "develop/masks.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "common/history.h"
#include <complex.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_undo_history_t
{
  GList *before_snapshot, *after_snapshot;
  int before_end, after_end;
  GList *before_iop_order_list, *after_iop_order_list;
  dt_masks_edit_mode_t mask_edit_mode;
  dt_dev_pixelpipe_display_mask_t request_mask_display;
} dt_undo_history_t;

typedef struct dt_lib_history_t
{
  /* vbox with managed history items */
  GtkWidget *history_box;
  GtkWidget *create_button;
  GtkWidget *compress_button;
  gboolean record_undo;
  int record_history_level; // set to +1 in signal DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE
                            // and back to -1 in DT_SIGNAL_DEVELOP_HISTORY_CHANGE. We want
                            // to avoid multiple will-change before a change cb.
  // previous_* below store values sent by signal DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE
  GList *previous_snapshot;
  int previous_history_end;
  GList *previous_iop_order_list;
} dt_lib_history_t;

/* 3 widgets in each history line */
#define HIST_WIDGET_NUMBER 0
#define HIST_WIDGET_MODULE 1
#define HIST_WIDGET_STATUS 2

/* compress history stack */
static void _lib_history_compress_clicked_callback(GtkButton *widget, gpointer user_data);

static gboolean _lib_history_compress_pressed_callback(GtkWidget *widget,
                                                       GdkEventButton *e,
                                                       gpointer user_data);

static gboolean _lib_history_button_clicked_callback(GtkWidget *widget,
                                                     GdkEventButton *e,
                                                     gpointer user_data);

static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget,
                                                              gpointer user_data);
/* signal callback for history change */
static void _lib_history_will_change_callback(gpointer instance,
                                              GList *history,
                                              const int history_end,
                                              GList *iop_order_list,
                                              gpointer user_data);

static void _lib_history_change_callback(gpointer instance, gpointer user_data);

static void _lib_history_module_remove_callback(gpointer instance,
                                                dt_iop_module_t *module,
                                                gpointer user_data);

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

int position(const dt_lib_module_t *self)
{
  return 900;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_history_t *d = (dt_lib_history_t *)g_malloc0(sizeof(dt_lib_history_t));
  self->data = (void *)d;

  d->record_undo = TRUE;
  d->record_history_level = 0;
  d->previous_snapshot = NULL;
  d->previous_history_end = 0;
  d->previous_iop_order_list = NULL;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "history-ui");

  d->history_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  d->compress_button = dt_action_button_new
    (self, N_("compress history stack"), _lib_history_compress_clicked_callback, self,
     _("create a minimal history stack which produces the same image\n"
       "ctrl+click to truncate history to the selected item"), 0, 0);
  g_signal_connect(G_OBJECT(d->compress_button), "button-press-event",
                   G_CALLBACK(_lib_history_compress_pressed_callback), self);

  /* add toolbar button for creating style */
  d->create_button = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_NONE, NULL);
  g_signal_connect(G_OBJECT(d->create_button), "clicked",
                   G_CALLBACK(_lib_history_create_style_button_clicked_callback), NULL);
  gtk_widget_set_name(d->create_button, "non-flat");
  gtk_widget_set_tooltip_text(d->create_button,
                              _("create a style from the current history stack"));
  dt_action_define(DT_ACTION(self), NULL,
                   N_("create style from history"),
                   d->create_button, &dt_action_def_button);

  /* add buttons to buttonbox */
  gtk_box_pack_start(GTK_BOX(hhbox), d->compress_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hhbox), d->create_button, FALSE, FALSE, 0);

  /* add history list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_resize_wrap(d->history_box, 1,
                                       "plugins/darkroom/history/windowheight"),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hhbox, FALSE, FALSE, 0);

  gtk_widget_show_all(self->widget);

  /* connect to history change signal for updating the history view */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,
                            G_CALLBACK(_lib_history_will_change_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_lib_history_change_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_REMOVE,
                            G_CALLBACK(_lib_history_module_remove_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_lib_history_change_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_lib_history_will_change_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_lib_history_module_remove_callback), self);
  g_free(self->data);
  self->data = NULL;
}

static GtkWidget *_lib_history_create_button(dt_lib_module_t *self,
                                             const int num,
                                             const char *label,
                                             const gboolean enabled,
                                             const gboolean default_enabled,
                                             const gboolean always_on,
                                             const gboolean selected,
                                             const gboolean deprecated)
{
  /* create label */
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gchar numlab[10];

  g_snprintf(numlab, sizeof(numlab), "%2d", num + 1);
  GtkWidget *numwidget = gtk_label_new(numlab);
  gtk_widget_set_name(numwidget, "history-number");
  dt_gui_add_class(numwidget, "dt_history_items");
  dt_gui_add_class(numwidget, "dt_monospace");

  GtkWidget *onoff = NULL;

  /* create toggle button */
  GtkWidget *widget = gtk_toggle_button_new_with_label("");
  dt_gui_add_class(widget, "dt_transparent_background");
  GtkWidget *lab = gtk_bin_get_child(GTK_BIN(widget));
  gtk_widget_set_halign(lab, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(lab), 0);
  gtk_label_set_ellipsize(GTK_LABEL(lab), PANGO_ELLIPSIZE_END);
  gtk_label_set_markup (GTK_LABEL (lab), label);
  if(always_on)
  {
    onoff = dtgtk_button_new(dtgtk_cairo_paint_switch_on, 0, NULL);
    dtgtk_button_set_active(DTGTK_BUTTON(onoff), TRUE);
    gtk_widget_set_tooltip_text(onoff, _("always-on module"));
  }
  else if(default_enabled)
  {
    onoff = dtgtk_button_new(dtgtk_cairo_paint_switch, 0, NULL);
    dtgtk_button_set_active(DTGTK_BUTTON(onoff), enabled);
    gtk_widget_set_tooltip_text(onoff, _("default enabled module"));
  }
  else
  {
    if(deprecated)
    {
      onoff = dtgtk_button_new(dtgtk_cairo_paint_switch_deprecated, 0, NULL);
      gtk_widget_set_tooltip_text(onoff, _("deprecated module"));
    }
    else
    {
      onoff = dtgtk_button_new(dtgtk_cairo_paint_switch, 0, NULL);
      dt_gui_add_class(onoff, enabled ? "" : "dt_history_switch_off");
    }
    dt_gui_add_class(lab, enabled ? "" : "dt_history_switch_off");
    dtgtk_button_set_active(DTGTK_BUTTON(onoff), enabled);
  }
  dt_gui_add_class(widget, "dt_history_items");
  dt_gui_add_class(onoff, "dt_history_switch");

  gtk_widget_set_sensitive(onoff, FALSE);

  g_object_set_data(G_OBJECT(widget), "history_number", GINT_TO_POINTER(num + 1));
  g_object_set_data(G_OBJECT(widget), "label", (gpointer)label);
  if(selected) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);

  /* set callback when clicked */
  g_signal_connect(G_OBJECT(widget), "button-press-event",
                   G_CALLBACK(_lib_history_button_clicked_callback), self);

  /* associate the history number */
  g_object_set_data(G_OBJECT(widget), "history-number", GINT_TO_POINTER(num + 1));

  gtk_box_pack_start(GTK_BOX(hbox), numwidget, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(hbox), onoff, FALSE, FALSE, 0);

  return hbox;
}

static void _reset_module_instance(GList *hist,
                                   dt_iop_module_t *module,
                                   const int multi_priority)
{
  for(; hist; hist = g_list_next(hist))
  {
    dt_dev_history_item_t *hit = (dt_dev_history_item_t *)hist->data;

    if(!hit->module
       && strcmp(hit->op_name, module->op) == 0
       && hit->multi_priority == multi_priority)
    {
      hit->module = module;
    }
  }
}

struct _cb_data
{
  dt_iop_module_t *module;
  int multi_priority;
};

static void _undo_items_cb(gpointer user_data,
                           const dt_undo_type_t type,
                           dt_undo_data_t data)
{
  struct _cb_data *udata = (struct _cb_data *)user_data;
  dt_undo_history_t *hdata = (dt_undo_history_t *)data;
  _reset_module_instance(hdata->after_snapshot, udata->module, udata->multi_priority);
}

static void _history_invalidate_cb(gpointer user_data,
                                   const dt_undo_type_t type,
                                   dt_undo_data_t data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  dt_dev_invalidate_history_module(hist->after_snapshot, module);
}

static void _add_module_expander(GList *iop_list, dt_iop_module_t *module)
{
  // dt_dev_reload_history_items won't do this for base instances
  // and it will call gui_init() for the rest
  // so we do it here
  if(!dt_iop_is_hidden(module) && !module->expander)
  {
      /* add module to right panel */
      dt_iop_gui_set_expander(module);
      dt_iop_gui_set_expanded(module, TRUE, FALSE);
      dt_iop_gui_update_blending(module);
  }
}

// return the 1st history entry that matches module
static dt_dev_history_item_t *_search_history_by_module(GList *history_list,
                                                        dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_ret = NULL;

  for(GList *history = history_list; history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist_item = (dt_dev_history_item_t *)history->data;

    if(hist_item->module == module)
    {
      hist_ret = hist_item;
      break;
    }
  }
  return hist_ret;
}

static int _check_deleted_instances(dt_develop_t *dev,
                                    GList **_iop_list,
                                    GList *history_list)
{
  GList *iop_list = *_iop_list;
  int deleted_module_found = 0;

  // we will check on dev->iop if there's a module that is not in history
  GList *modules = iop_list;
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
          const int mod_in_history = _search_history_by_module(history_list, mod) != NULL;
          const int mod_next_in_history =
            _search_history_by_module(history_list, mod_next) != NULL;

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
              dt_print(DT_DEBUG_ALWAYS,
                  "[_check_deleted_instances] found duplicate module"
                  " %s %s (%i) and %s %s (%i) both in history\n",
                  mod->op, mod->multi_name, mod->multi_priority,
                  mod_next->op, mod_next->multi_name,
                  mod_next->multi_priority);
            else
              dt_print(DT_DEBUG_ALWAYS,
                  "[_check_deleted_instances] found duplicate module"
                  " %s %s (%i) and %s %s (%i) none in history\n",
                  mod->op, mod->multi_name, mod->multi_priority,
                  mod_next->op, mod_next->multi_name,
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

      ++darktable.gui->reset;

      // we remove the plugin effectively
      if(!dt_iop_is_hidden(mod))
      {
        // we just hide the module to avoid lots of gtk critical warnings
        gtk_widget_hide(mod->expander);

        // this is copied from dt_iop_gui_delete_callback(), not sure
        // why the above sentence...
        dt_iop_gui_cleanup_module(mod);
        gtk_widget_destroy(mod->widget);
      }

      iop_list = g_list_remove_link(iop_list, modules);

      // remove it from all snapshots
      dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY,
                               mod, &_history_invalidate_cb);

      // we cleanup the module
      dt_action_cleanup_instance_iop(mod);

      // don't delete the module, a pipe may still need it
      dev->alliop = g_list_append(dev->alliop, mod);

      --darktable.gui->reset;

      // and reset the list
      modules = iop_list;
      continue;
    }

    modules = g_list_next(modules);
  }
  if(deleted_module_found) iop_list = g_list_sort(iop_list, dt_sort_iop_by_order);

  *_iop_list = iop_list;

  return deleted_module_found;
}

static void _reorder_gui_module_list(dt_develop_t *dev)
{
  int pos_module = 0;
  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    GtkWidget *expander = module->expander;
    if(expander)
    {
      gtk_box_reorder_child
        (dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
         expander,
         pos_module++);
    }
  }
}

static int _rebuild_multi_priority(GList *history_list)
{
  int changed = 0;
  for(const GList *history = history_list; history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)history->data;

    // if multi_priority is different in history and dev->iop
    // we keep the history version
    if(hitem->module && hitem->module->multi_priority != hitem->multi_priority)
    {
      dt_iop_update_multi_priority(hitem->module, hitem->multi_priority);
      changed = 1;
    }
  }
  return changed;
}

static int _create_deleted_modules(GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int changed = 0;
  gboolean done = FALSE;

  GList *l = history_list;
  while(l)
  {
    GList *next = g_list_next(l);
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)l->data;

    // this fixes the duplicate module when undo: hitem->multi_priority = 0;
    if(hitem->module == NULL)
    {
      changed = 1;

      const dt_iop_module_t *base_module =
        dt_iop_get_module_from_list(iop_list, hitem->op_name);
      if(base_module == NULL)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[_create_deleted_modules] can't find base module for %s\n",
                 hitem->op_name);
        return changed;
      }

      // from there we create a new module for this base instance. The
      // goal is to do a very minimal setup of the new module to be
      // able to write the history items. From there we reload the
      // whole history back and this will recreate the proper module
      // instances.
      dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module(module, base_module->so, base_module->dev))
      {
        return changed;
      }
      module->instance = base_module->instance;

      if(!dt_iop_is_hidden(module))
      {
        ++darktable.gui->reset;
        module->gui_init(module);
        --darktable.gui->reset;
      }

      // adjust the multi_name of the new module
      g_strlcpy(module->multi_name, hitem->multi_name, sizeof(module->multi_name));
      dt_iop_update_multi_priority(module, hitem->multi_priority);
      module->iop_order = hitem->iop_order;

      // we insert this module into dev->iop
      iop_list = g_list_insert_sorted(iop_list, module, dt_sort_iop_by_order);

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

static void _pop_undo(gpointer user_data, dt_undo_type_t type,
                      dt_undo_data_t data,
                      dt_undo_action_t action,
                      GList **imgs)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  if(type == DT_UNDO_HISTORY)
  {
    dt_lib_history_t *d = (dt_lib_history_t *)self->data;
    dt_undo_history_t *hist = (dt_undo_history_t *)data;
    dt_develop_t *dev = darktable.develop;

    // we will work on a copy of history and modules
    // when we're done we'll replace dev->history and dev->iop
    GList *history_temp = NULL;
    int hist_end = 0;

    if(action == DT_ACTION_UNDO)
    {
      history_temp = dt_history_duplicate(hist->before_snapshot);
      hist_end = hist->before_end;
      dev->iop_order_list = dt_ioppr_iop_order_copy_deep(hist->before_iop_order_list);
    }
    else
    {
      history_temp = dt_history_duplicate(hist->after_snapshot);
      hist_end = hist->after_end;
      dev->iop_order_list = dt_ioppr_iop_order_copy_deep(hist->after_iop_order_list);
    }

    GList *iop_temp = g_list_copy(dev->iop);

    // topology has changed?
    int pipe_remove = 0;

    // we have to check if multi_priority has changed since history was saved
    // we will adjust it here
    if(_rebuild_multi_priority(history_temp))
    {
      pipe_remove = 1;
      iop_temp = g_list_sort(iop_temp, dt_sort_iop_by_order);
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

    // disable recording undo as the _lib_history_change_callback will
    // be triggered by the calls below
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
      dt_dev_pixelpipe_rebuild(dev);
    }

    dt_pthread_mutex_unlock(&dev->history_mutex);

    // if dev->iop has changed reflect that on module list
    if(pipe_remove) _reorder_gui_module_list(dev);

    // write new history and reload
    dt_dev_write_history(dev);
    dt_dev_reload_history_items(dev);

    dt_ioppr_resync_modules_order(dev);

    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));

    if(dev->gui_module)
    {
      dt_masks_set_edit_mode(dev->gui_module, hist->mask_edit_mode);
      darktable.develop->gui_module->request_mask_display = hist->request_mask_display;
      dt_iop_gui_update_blendif(darktable.develop->gui_module);
      dt_iop_gui_blend_data_t *bd =
        (dt_iop_gui_blend_data_t *)(dev->gui_module->blend_data);
      if(bd)
        gtk_toggle_button_set_active
          (GTK_TOGGLE_BUTTON(bd->showmask),
           hist->request_mask_display == DT_DEV_PIXELPIPE_DISPLAY_MASK);
    }
  }
}

static void _history_undo_data_free(gpointer data)
{
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  g_list_free_full(hist->before_snapshot, dt_dev_free_history_item);
  g_list_free_full(hist->after_snapshot, dt_dev_free_history_item);
  g_list_free_full(hist->before_iop_order_list, free);
  g_list_free_full(hist->after_iop_order_list, free);
  free(data);
}

static void _lib_history_module_remove_callback(gpointer instance,
                                                dt_iop_module_t *module,
                                                gpointer user_data)
{
  dt_undo_iterate(darktable.undo, DT_UNDO_HISTORY, module, &_history_invalidate_cb);
}

static void _lib_history_will_change_callback(gpointer instance,
                                              GList *history,
                                              const int history_end,
                                              GList *iop_order_list,
                                              gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *lib = (dt_lib_history_t *)self->data;

  if(lib->record_undo && (lib->record_history_level == 0))
  {
    // history is about to change, here we want to record as snapshot
    // of the history for the undo record previous history
    g_list_free_full(lib->previous_snapshot, free);
    g_list_free_full(lib->previous_iop_order_list, free);
    lib->previous_snapshot = history;
    lib->previous_history_end = history_end;
    lib->previous_iop_order_list = iop_order_list;
  }

  lib->record_history_level += 1;
}

static gchar *_lib_history_change_text(dt_introspection_field_t *field,
                                       const char *d,
                                       gpointer params,
                                       gpointer oldpar)
{
  dt_iop_params_t *p = (dt_iop_params_t *)((uint8_t *)params + field->header.offset);
  dt_iop_params_t *o = (dt_iop_params_t *)((uint8_t *)oldpar + field->header.offset);

  switch(field->header.type)
  {
  case DT_INTROSPECTION_TYPE_STRUCT:
  case DT_INTROSPECTION_TYPE_UNION:
    {
      gchar **change_parts = g_malloc0_n(field->Struct.entries + 1, sizeof(char*));
      int num_parts = 0;

      for(int i = 0; i < field->Struct.entries; i++)
      {
        dt_introspection_field_t *entry = field->Struct.fields[i];

        gchar *description = _(*entry->header.description ?
                                entry->header.description :
                                entry->header.field_name);

        if(d) description = g_strdup_printf("%s.%s", d, description);

        gchar *part, *sect = NULL;
        if((part = _lib_history_change_text(entry, description, params, oldpar)))
        {
          GHashTable *sections = field->header.so->get_introspection()->sections;
          if(sections
             && (sect = g_hash_table_lookup(sections,
                                            GINT_TO_POINTER(entry->header.offset))))
          {
            sect = g_strdup_printf("%s/%s", Q_(sect), part);
            g_free(part);
            part = sect;
          }

          change_parts[num_parts++] = part;
        }

        if(d) g_free(description);
      }

      gchar *struct_text = num_parts ? g_strjoinv("\n", change_parts) : NULL;
      g_strfreev(change_parts);

      return struct_text;
    }
    break;
  case DT_INTROSPECTION_TYPE_ARRAY:
    if(field->Array.type == DT_INTROSPECTION_TYPE_CHAR)
    {
      const gboolean is_valid =
        g_utf8_validate((char *)o, -1, NULL)
        && g_utf8_validate((char *)p, -1, NULL);

      if(is_valid && strncmp((char*)o, (char*)p, field->Array.count))
        return g_strdup_printf("%s\t\"%s\"\t\u2192\t\"%s\"", d, (char*)o, (char*)p);
    }
    else
    {
      const int max_elements = 4;
      gchar **change_parts = g_malloc0_n(max_elements + 1, sizeof(char*));
      int num_parts = 0;

      for(int i = 0, item_offset = 0;
          i < field->Array.count;
          i++, item_offset += field->Array.field->header.size)
      {
        char *description = g_strdup_printf("%s[%d]", d, i);
        char *element_text =
          _lib_history_change_text(field->Array.field, description,
                                   (uint8_t *)params + item_offset,
                                   (uint8_t *)oldpar + item_offset);
        g_free(description);

        if(element_text && ++num_parts <= max_elements)
          change_parts[num_parts - 1] = element_text;
        else
          g_free(element_text);
      }

      gchar *array_text = NULL;
      if(num_parts > max_elements)
        array_text = g_strdup_printf("%s\t%d changes", d, num_parts);
      else if(num_parts > 0)
        array_text = g_strjoinv("\n", change_parts);

      g_strfreev(change_parts);

      return array_text;
    }
    break;
  case DT_INTROSPECTION_TYPE_FLOAT:
    if(*(float*)o != *(float*)p && (isfinite(*(float*)o) || isfinite(*(float*)p)))
      return g_strdup_printf("%s\t%.4f\t\u2192\t%.4f", d, *(float*)o, *(float*)p);
    break;
  case DT_INTROSPECTION_TYPE_INT:
    if(*(int*)o != *(int*)p)
      return g_strdup_printf("%s\t%d\t\u2192\t%d", d, *(int*)o, *(int*)p);
    break;
  case DT_INTROSPECTION_TYPE_UINT:
    if(*(unsigned int*)o != *(unsigned int*)p)
      return g_strdup_printf("%s\t%u\t\u2192\t%u", d, *(unsigned int*)o,
                             *(unsigned int*)p);
    break;
  case DT_INTROSPECTION_TYPE_USHORT:
    if(*(unsigned short int*)o != *(unsigned short int*)p)
      return g_strdup_printf("%s\t%hu\t\u2192\t%hu", d, *(unsigned short int*)o,
                             *(unsigned short int*)p);
    break;
  case DT_INTROSPECTION_TYPE_INT8:
    if(*(uint8_t*)o != *(uint8_t*)p)
      return g_strdup_printf("%s\t%d\t\u2192\t%d", d, *(uint8_t*)o, *(uint8_t*)p);
    break;
  case DT_INTROSPECTION_TYPE_CHAR:
    if(*(char*)o != *(char*)p)
      return g_strdup_printf("%s\t'%c'\t\u2192\t'%c'", d, *(char *)o, *(char *)p);
    break;
  case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
    if(*(float complex*)o != *(float complex*)p)
      return g_strdup_printf("%s\t%.4f + %.4fi\t\u2192\t%.4f + %.4fi", d,
                             creal(*(float complex*)o), cimag(*(float complex*)o),
                             creal(*(float complex*)p), cimag(*(float complex*)p));
    break;
  case DT_INTROSPECTION_TYPE_ENUM:
    if(*(int*)o != *(int*)p)
    {
      const char *old_str = N_("unknown"), *new_str = N_("unknown");
      for(dt_introspection_type_enum_tuple_t *i = field->Enum.values; i && i->name; i++)
      {
        if(i->value == *(int*)o)
        {
          old_str = i->description;
          if(!*old_str) old_str = i->name;
        }
        if(i->value == *(int*)p)
        {
          new_str = i->description;
          if(!*new_str) new_str = i->name;
        }
      }

      return g_strdup_printf("%s\t%s\t\u2192\t%s", d, _(old_str), _(new_str));
    }
    break;
  case DT_INTROSPECTION_TYPE_BOOL:
    if(*(gboolean*)o != *(gboolean*)p)
    {
      char *old_str = *(gboolean*)o ? "on" : "off";
      char *new_str = *(gboolean*)p ? "on" : "off";
      return g_strdup_printf("%s\t%s\t\u2192\t%s", d, _(old_str), _(new_str));
    }
    break;
  case DT_INTROSPECTION_TYPE_OPAQUE:
    {
      // TODO: special case float2
    }
    break;
  default:
    dt_print(DT_DEBUG_ALWAYS, "unsupported introspection type \"%s\" encountered"
             " in _lib_history_change_text (field %s)\n",
             field->header.type_name, field->header.field_name);
    break;
  }

  return NULL;
}

static gboolean _changes_tooltip_callback(GtkWidget *widget,
                                          const gint x,
                                          const gint y,
                                          const gboolean keyboard_mode,
                                          GtkTooltip *tooltip,
                                          const dt_dev_history_item_t *hitem)
{
  dt_iop_params_t *old_params = hitem->module->default_params;
  dt_develop_blend_params_t *old_blend = hitem->module->default_blendop_params;

  for(const GList *find_old = darktable.develop->history;
      find_old && find_old->data != hitem;
      find_old = g_list_next(find_old))
  {
    const dt_dev_history_item_t *hiprev = (dt_dev_history_item_t *)(find_old->data);

    if(hiprev->module == hitem->module)
    {
      old_params = hiprev->params;
      old_blend = hiprev->blend_params;
    }
  }

  gchar **change_parts = g_malloc0_n(sizeof(dt_develop_blend_params_t)
                                     / (sizeof(float)) + 10, sizeof(char*));

  if(hitem->module->have_introspection)
    change_parts[0] = _lib_history_change_text(hitem->module->get_introspection()->field,
                                               NULL,
                                               hitem->params, old_params);
  int num_parts = change_parts[0] ? 1 : 0;

  if(hitem->module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    #define add_blend_history_change(field, format, label)                             \
      if((hitem->blend_params->field) != (old_blend->field))                           \
      {                                                                                \
        gchar *full_format = g_strconcat("%s\t", format, "\t\u2192\t", format, NULL);  \
        change_parts[num_parts++] = g_strdup_printf(full_format, label,                \
                                    (old_blend->field), (hitem->blend_params->field)); \
        g_free(full_format);                                                           \
      }

    #define add_blend_history_change_enum(field, label, list)                          \
      if((hitem->blend_params->field) != (old_blend->field))                           \
      {                                                                                \
        const char *old_str = NULL, *new_str = NULL;                                   \
        for(const dt_introspection_type_enum_tuple_t *i = list; i->name; i++)          \
        {                                                                              \
          if(i->value == (old_blend->field)) old_str = i->name;                        \
          if(i->value == (hitem->blend_params->field)) new_str = i->name;              \
        }                                                                              \
                                                                                       \
        change_parts[num_parts++] = (!old_str || !new_str)                             \
              ? g_strdup_printf("%s\t%d\t\u2192\t%d", label,                           \
                                old_blend->field, hitem->blend_params->field)          \
                                  : g_strdup_printf("%s\t%s\t\u2192\t%s", label,       \
                                                    Q_(old_str),                       \
                                                    Q_(new_str));                      \
      }

    add_blend_history_change_enum(blend_cst, _("colorspace"),
                                  dt_develop_blend_colorspace_names);
    add_blend_history_change_enum(mask_mode, _("mask mode"),
                                  dt_develop_mask_mode_names);
    add_blend_history_change_enum(blend_mode & DEVELOP_BLEND_MODE_MASK, _("blend mode"),
                                  dt_develop_blend_mode_names);
    add_blend_history_change_enum(blend_mode & DEVELOP_BLEND_REVERSE, _("blend operation"),
                                  dt_develop_blend_mode_flag_names);
    add_blend_history_change(blend_parameter, _("%.2f EV"), _("blend fulcrum"));
    add_blend_history_change(opacity, "%.4f", _("mask opacity"));
    add_blend_history_change_enum
      (mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL), _("combine masks"),
       dt_develop_combine_masks_names);
    add_blend_history_change(feathering_radius, "%.4f", _("feathering radius"));
    add_blend_history_change_enum(feathering_guide, _("feathering guide"),
                                  dt_develop_feathering_guide_names);
    add_blend_history_change(blur_radius, "%.4f", _("mask blur"));
    add_blend_history_change(contrast, "%.4f", _("mask contrast"));
    add_blend_history_change(brightness, "%.4f", _("brightness"));
    add_blend_history_change(raster_mask_instance, "%d", _("raster mask instance"));
    add_blend_history_change(raster_mask_id, "%d", _("raster mask id"));
    add_blend_history_change_enum(raster_mask_invert, _("invert mask"),
                                  dt_develop_invert_mask_names);

    add_blend_history_change(mask_combine & DEVELOP_COMBINE_MASKS_POS
                             ? '-'
                             : '+', "%c", _("drawn mask polarity"));

    if(hitem->blend_params->mask_id != old_blend->mask_id)
      change_parts[num_parts++] = old_blend->mask_id == 0
                                ? g_strdup_printf(_("a drawn mask was added"))
                                : hitem->blend_params->mask_id == 0
                                ? g_strdup_printf(_("the drawn mask was removed"))
                                : g_strdup_printf(_("the drawn mask was changed"));

    dt_iop_gui_blend_data_t *bd = hitem->module->blend_data;

    for(int in_out = 1; in_out >= 0; in_out--)
    {
      gboolean first = TRUE;

      for(const dt_iop_gui_blendif_channel_t *b = bd ? bd->channel : NULL;
          b && b->label != NULL;
          b++)
      {
        const dt_develop_blendif_channels_t ch = b->param_channels[in_out];

        const int oactive = old_blend->blendif & (1 << ch);
        const int nactive = hitem->blend_params->blendif & (1 << ch);

        const int opolarity = old_blend->blendif & (1 << (ch + 16));
        const int npolarity = hitem->blend_params->blendif & (1 << (ch + 16));

        float *of = &old_blend->blendif_parameters[4 * ch];
        float *nf = &hitem->blend_params->blendif_parameters[4 * ch];

        const float oboost = exp2f(old_blend->blendif_boost_factors[ch]);
        const float nboost = exp2f(hitem->blend_params->blendif_boost_factors[ch]);

        if((oactive || nactive)
           && (memcmp(of, nf, sizeof(float) * 4) || opolarity != npolarity))
        {
          if(first)
          {
            change_parts[num_parts++] = g_strdup(in_out
                                                 ? _("parametric output mask:")
                                                 : _("parametric input mask:"));
            first = FALSE;
          }
          char s[4][2][25];
          for(int k = 0; k < 4; k++)
          {
            b->scale_print(of[k], oboost, s[k][0], sizeof(s[k][0]));
            b->scale_print(nf[k], nboost, s[k][1], sizeof(s[k][1]));
          }

          char *opol = !oactive ? "" : (opolarity ? "(-)" : "(+)");
          char *npol = !nactive ? "" : (npolarity ? "(-)" : "(+)");

          change_parts[num_parts++] =
            g_strdup_printf("%s\t%s| %s- %s| %s%s\t\u2192\t%s| %s- %s| %s%s",
                            _(b->name),
                            s[0][0], s[1][0], s[2][0], s[3][0], opol,
                            s[0][1], s[1][1], s[2][1], s[3][1], npol);
        }
      }
    }
  }

  gchar *tooltip_text = g_strjoinv("\n", change_parts);
  g_strfreev(change_parts);

  gboolean show_tooltip = *tooltip_text;

  if(show_tooltip)
  {
    static GtkWidget *view = NULL;
    if(!view)
    {
      view = gtk_text_view_new();
      dt_gui_add_class(view, "dt_transparent_background");
      dt_gui_add_class(view, "dt_monospace");
      g_signal_connect(G_OBJECT(view), "destroy", G_CALLBACK(gtk_widget_destroyed), &view);
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, tooltip_text, -1);
    gtk_tooltip_set_custom(tooltip, view);
    gtk_widget_map(view); // FIXME: workaround added in order to fix
                          // #9908, probably a Gtk issue, remove when
                          // fixed upstream

    int count_column1 = 0, count_column2 = 0;
    for(gchar *line = tooltip_text; *line; )
    {
      gchar *endline = g_strstr_len(line, -1, "\n");
      if(!endline) endline = line + strlen(line);

      gchar *found_tab1 = g_strstr_len(line, endline - line, "\t");
      if(found_tab1)
      {
        if(found_tab1 - line >= count_column1) count_column1 = found_tab1 - line + 1;

        gchar *found_tab2 = g_strstr_len(found_tab1 + 1, endline - found_tab1 - 1, "\t");
        if(found_tab2 - found_tab1 > count_column2)
          count_column2 = found_tab2 - found_tab1;
      }

      line = endline;
      if(*line) line++;
    }

    PangoLayout *layout = gtk_widget_create_pango_layout(view, " ");
    int char_width;
    pango_layout_get_size(layout, &char_width, NULL);
    g_object_unref(layout);
    PangoTabArray *tabs = pango_tab_array_new_with_positions
      (3, FALSE,
       PANGO_TAB_LEFT, (count_column1) * char_width,
       PANGO_TAB_LEFT, (count_column1 + count_column2) * char_width,
       PANGO_TAB_LEFT, (count_column1 + count_column2 + 2) * char_width);
    gtk_text_view_set_tabs(GTK_TEXT_VIEW(view), tabs);
    pango_tab_array_free(tabs);
  }

  g_free(tooltip_text);

  return show_tooltip;
}

static gchar *_lib_history_button_label(const dt_dev_history_item_t *item)
{
  gchar *label = NULL;
  if(!item)
    label = g_strdup("");
  else if(!item->multi_name[0] || strcmp(item->multi_name, "0") == 0)
    label = g_strdup(item->module->name());
  else
    label = g_markup_printf_escaped("%s • <small>%s</small>",
                                    item->module->name(), item->multi_name);

  return label;
}

static void _lib_history_change_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;

  /* first destroy all buttons in list */
  dt_gui_container_destroy_children(GTK_CONTAINER(d->history_box));

  /* add default which always should be */
  int num = -1;
  GtkWidget *widget =
    _lib_history_create_button(self, num, _("original"),
                               FALSE, FALSE, TRUE,
                               darktable.develop->history_end == 0, FALSE);
  gtk_box_pack_start(GTK_BOX(d->history_box), widget, FALSE, FALSE, 0);
  num++;

  d->record_history_level -= 1;

  if(d->record_undo == TRUE && (d->record_history_level == 0))
  {
    /* record undo/redo history snapshot */
    dt_undo_history_t *hist = malloc(sizeof(dt_undo_history_t));
    hist->before_snapshot = dt_history_duplicate(d->previous_snapshot);
    hist->before_end = d->previous_history_end;
    hist->before_iop_order_list = dt_ioppr_iop_order_copy_deep(d->previous_iop_order_list);

    hist->after_snapshot = dt_history_duplicate(darktable.develop->history);
    hist->after_end = darktable.develop->history_end;
    hist->after_iop_order_list =
      dt_ioppr_iop_order_copy_deep(darktable.develop->iop_order_list);

    if(darktable.develop->gui_module)
    {
      hist->mask_edit_mode = dt_masks_get_edit_mode(darktable.develop->gui_module);
      hist->request_mask_display = darktable.develop->gui_module->request_mask_display;
    }
    else
    {
      hist->mask_edit_mode = DT_MASKS_EDIT_OFF;
      hist->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    }

    dt_undo_record(darktable.undo, self, DT_UNDO_HISTORY, (dt_undo_data_t)hist,
                   _pop_undo, _history_undo_data_free);
  }
  else
    d->record_undo = TRUE;

  /* lock history mutex */
  dt_pthread_mutex_lock(&darktable.develop->history_mutex);

  /* iterate over history items and add them to list*/
  for(const GList *history = darktable.develop->history;
      history;
      history = g_list_next(history))
  {
    const dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)(history->data);
    gchar *label = _lib_history_button_label(hitem);

    const gboolean selected = (num == darktable.develop->history_end - 1);
    widget =
      _lib_history_create_button
      (self, num, label,
       (hitem->enabled || (strcmp(hitem->op_name, "mask_manager") == 0)),
       hitem->module->default_enabled, hitem->module->hide_enable_button, selected,
       hitem->module->flags() & IOP_FLAGS_DEPRECATED);

    g_free(label);

    gtk_widget_set_has_tooltip(widget, TRUE);
    g_signal_connect(G_OBJECT(widget), "query-tooltip",
                     G_CALLBACK(_changes_tooltip_callback), (void *)hitem);

    gtk_box_pack_start(GTK_BOX(d->history_box), widget, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(d->history_box), widget, 0);
    num++;
  }

  /* show all widgets */
  gtk_widget_show_all(d->history_box);

  dt_pthread_mutex_unlock(&darktable.develop->history_mutex);
}

static void _lib_history_truncate(gboolean compress)
{
  const int32_t imgid = darktable.develop->image_storage.id;
  if(!imgid) return;

  dt_dev_undo_start_record(darktable.develop);

  // As dt_history_compress_on_image does *not* use the history stack data at all
  // make sure the current stack is in the database
  dt_dev_write_history(darktable.develop);

  if(compress)
    dt_history_compress_on_image(imgid);
  else
    dt_history_truncate_on_image(imgid, darktable.develop->history_end);

  sqlite3_stmt *stmt;

  // load new history and write it back to ensure that all history are
  // properly numbered without a gap
  dt_dev_reload_history_items(darktable.develop);
  dt_dev_write_history(darktable.develop);
  dt_image_synch_xmp(imgid);

  // then we can get the item to select in the new clean-up history
  // retrieve the position of the module corresponding to the history
  // end.  clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT IFNULL(MAX(num)+1, 0)"
                              " FROM main.history"
                              " WHERE imgid=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
    darktable.develop->history_end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // select the new history end corresponding to the one before the history compression
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end=?2 WHERE id=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, darktable.develop->history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  darktable.develop->proxy.chroma_adaptation = NULL;
  dt_dev_reload_history_items(darktable.develop);
  dt_dev_undo_end_record(darktable.develop);

  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_INVALIDATED);
}


static void _lib_history_compress_clicked_callback(GtkButton *widget, gpointer user_data)
{
  _lib_history_truncate(TRUE);
}

static gboolean _lib_history_compress_pressed_callback(GtkWidget *widget,
                                                       GdkEventButton *e,
                                                       gpointer user_data)
{
  const gboolean compress = !dt_modifier_is(e->state, GDK_CONTROL_MASK);
  _lib_history_truncate(compress);

  return TRUE;
}

static gboolean _lib_history_button_clicked_callback(GtkWidget *widget,
                                                     GdkEventButton *e,
                                                     gpointer user_data)
{
  const int32_t imgid = darktable.develop->image_storage.id;
  static int reset = 0;
  if(reset) return FALSE;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return FALSE;

  // ctrl-click just show the corresponding module in modulegroups
  if(dt_modifier_is(e->state, GDK_SHIFT_MASK))
  {
    const int num = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history-number"));
    dt_dev_history_item_t *hist =
      (dt_dev_history_item_t *)g_list_nth_data(darktable.develop->history, num - 1);
    if(hist)
    {
      dt_dev_modulegroups_switch(darktable.develop, hist->module);
      dt_iop_gui_set_expanded(hist->module, TRUE, TRUE);
    }
    return TRUE;
  }

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;
  reset = 1;

  /* deactivate all toggle buttons */
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->history_box));
  for(GList *l = children; l != NULL; l = g_list_next(l))
  {
    GtkToggleButton *b = GTK_TOGGLE_BUTTON
      (dt_gui_container_nth_child(GTK_CONTAINER(l->data), HIST_WIDGET_MODULE));
    if(b != GTK_TOGGLE_BUTTON(widget))
      g_object_set(G_OBJECT(b), "active", FALSE, (gchar *)0);
  }
  g_list_free(children);

  reset = 0;
  if(darktable.gui->reset) return FALSE;

  dt_dev_undo_start_record(darktable.develop);

  /* revert to given history item. */
  const int num = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history-number"));
  dt_dev_pop_history_items(darktable.develop, num);
  // set the module list order
  dt_dev_reorder_gui_module_list(darktable.develop);
  dt_image_update_final_size(imgid);

  /* signal history changed */
  dt_dev_undo_end_record(darktable.develop);

  dt_iop_connect_accels_all();
  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  return FALSE;
}

static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget,
                                                              gpointer user_data)
{
  if(darktable.develop->image_storage.id)
  {
    dt_dev_write_history(darktable.develop);
    dt_gui_styles_dialog_new(darktable.develop->image_storage.id);
  }
}

void gui_reset(dt_lib_module_t *self)
{
  const int32_t imgid = darktable.develop->image_storage.id;
  if(!imgid) return;

  if(!dt_conf_get_bool("ask_before_discard")
     || dt_gui_show_yes_no_dialog
          (_("delete image's history?"),
           _("do you really want to clear history of current image?")))
  {
    dt_dev_undo_start_record(darktable.develop);

    dt_history_delete_on_image_ext(imgid, FALSE);

    dt_dev_undo_end_record(darktable.develop);

    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));

    dt_control_queue_redraw_center();
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
