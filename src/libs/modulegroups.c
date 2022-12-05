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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/iop_group.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

// the T_ macros are for the translation engine to take them into account
#define FALLBACK_PRESET_NAME     "modules: default"
#define T_FALLBACK_PRESET_NAME _("modules: default")

#define DEPRECATED_PRESET_NAME     "modules: deprecated"
#define T_DEPRECATED_PRESET_NAME _("modules: deprecated")

#define CURRENT_PRESET_NAME "last modified layout"
#define T_CURRENT_PRESET_NAME _("last modified layout")

// list of recommended basics widgets
#define RECOMMENDED_BASICS                                                                                        \
  "|exposure/exposure|temperature/temperature|temperature/tint|colorbalancergb/contrast|colorbalancergb/global "        \
  "vibrance|colorbalancergb/global chroma|colorbalancergb/global saturation|ashift/roration|denoiseprofile|lens|bilat|"

// if a preset cannot be loaded or the current preset deleted, this is the fallback preset

#define PADDING 2
#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

#include "modulegroups.h"

typedef enum dt_lib_modulegroups_basic_item_position_t
{
  NORMAL,
  NEW_MODULE,
  FIRST_MODULE
} dt_lib_modulegroups_basic_item_position_t;

typedef enum dt_lib_modulegroups_basic_item_type_t
{
  WIDGET_TYPE_NONE = 0,
  WIDGET_TYPE_BAUHAUS_SLIDER,
  WIDGET_TYPE_BAUHAUS_COMBO,
  WIDGET_TYPE_ACTIVATE_BTN,
  WIDGET_TYPE_MISC
} dt_lib_modulegroups_basic_item_type_t;

typedef struct dt_lib_modulegroups_basic_item_t
{
  gchar *id;
  gchar *module_op;
  gchar *widget_name; // translated
  GtkWidget *widget;
  GtkWidget *temp_widget;
  GtkWidget *old_parent;
  dt_lib_modulegroups_basic_item_type_t widget_type;

  int old_pos;
  gboolean expand;
  gboolean fill;
  guint padding;
  GtkPackType packtype;
  gchar *tooltip;
  int grid_x, grid_y, grid_w, grid_h;

  GtkWidget *box;
  dt_iop_module_t *module;
} dt_lib_modulegroups_basic_item_t;

typedef struct dt_lib_modulegroups_group_t
{
  gchar *name;
  GtkWidget *button;
  gchar *icon;
  GtkWidget *iop_box;
  // default
  GList *modules;
} dt_lib_modulegroups_group_t;

typedef struct dt_lib_modulegroups_t
{
  uint32_t current;
  GtkWidget *text_entry;
  GtkWidget *hbox_buttons;
  GtkWidget *active_btn;
  GtkWidget *basic_btn;
  GtkWidget *hbox_groups;
  GtkWidget *hbox_search_box;
  GtkWidget *deprecated;
  gboolean force_deprecated_message;
  GList *groups;
  gboolean show_search;
  gboolean full_active;

  GList *edit_groups;
  gboolean edit_show_search;
  gboolean edit_full_active;
  gchar *edit_preset;
  gboolean edit_ro;
  gboolean edit_basics_show;
  GList *edit_basics;

  // editor dialog
  GtkWidget *dialog;
  gboolean editor_reset;
  GtkWidget *presets_combo, *presets_btn_remove, *presets_btn_dup, *presets_btn_rename, *presets_btn_new;
  GtkWidget *preset_groups_box, *preset_btn_add_group, *preset_read_only_label, *preset_reset_btn;
  GtkWidget *edit_search_cb, *edit_full_active_cb;
  GtkWidget *basics_chkbox, *edit_basics_groupbox, *edit_basics_box;
  GtkWidget *edit_autoapply_chkbox, *edit_autoapply_btn;

  gboolean basics_show;
  GList *basics;
  GtkWidget *vbox_basic;
  GtkWidget *mod_vbox_basic;

  dt_iop_module_t *force_show_module;
} dt_lib_modulegroups_t;

typedef enum dt_lib_modulegroup_iop_visibility_type_t
{
  DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE,
  DT_MODULEGROUP_SEARCH_IOP_GROUPS_VISIBLE,
  DT_MODULEGROUP_SEARCH_IOP_TEXT_GROUPS_VISIBLE
} dt_lib_modulegroup_iop_visibility_type_t;

/* toggle button callback */
static void _lib_modulegroups_toggle(GtkWidget *button, gpointer data);
/* helper function to update iop module view depending on group */
static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self);

/* modulergroups proxy set group function
   \see dt_dev_modulegroups_set()
*/
static void _lib_modulegroups_set(dt_lib_module_t *self, uint32_t group);
/* modulegroups proxy update visibility function
*/
static void _lib_modulegroups_update_visibility_proxy(dt_lib_module_t *self);
/* modulegroups proxy get group function
  \see dt_dev_modulegroups_get()
*/
static uint32_t _lib_modulegroups_get(dt_lib_module_t *self);
/* modulegroups proxy test function.
   tests if iop module group flags matches modulegroup.
*/
static gboolean _lib_modulegroups_test(dt_lib_module_t *self, uint32_t group, dt_iop_module_t *module);
/* modulegroups proxy test visibility function.
   tests if iop module is preset in one groups for current layout.
*/
static gboolean _lib_modulegroups_test_visible(dt_lib_module_t *self, gchar *module);
/* modulegroups proxy switch group function.
   sets the active group which module belongs too.
*/
static void _lib_modulegroups_switch_group(dt_lib_module_t *self, dt_iop_module_t *module);

static void _manage_preset_update_list(dt_lib_module_t *self);
static void _manage_editor_load(const char *preset, dt_lib_module_t *self);

static void _buttons_update(dt_lib_module_t *self);

const char *name(dt_lib_module_t *self)
{
  return _("modulegroups");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}


/* this module should always be shown without expander */
int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 999;
}

static GtkWidget *_buttons_get_from_pos(dt_lib_module_t *self, const int pos)
{
  const dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(pos == DT_MODULEGROUP_ACTIVE_PIPE) return d->active_btn;
  if(pos == DT_MODULEGROUP_BASICS) return d->basic_btn;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_list_nth_data(d->groups, pos - 1);
  if(gr) return gr->button;
  return NULL;
}

static void _text_entry_changed_callback(GtkEntry *entry, dt_lib_module_t *self)
{
  _lib_modulegroups_update_iop_visibility(self);
}

static DTGTKCairoPaintIconFunc _buttons_get_icon_fct(const gchar *icon)
{
  if(g_strcmp0(icon, "active") == 0)
    return dtgtk_cairo_paint_modulegroup_active;
  else if(g_strcmp0(icon, "favorites") == 0)
    return dtgtk_cairo_paint_modulegroup_favorites;
  else if(g_strcmp0(icon, "tone") == 0)
    return dtgtk_cairo_paint_modulegroup_tone;
  else if(g_strcmp0(icon, "color") == 0)
    return dtgtk_cairo_paint_modulegroup_color;
  else if(g_strcmp0(icon, "correct") == 0)
    return dtgtk_cairo_paint_modulegroup_correct;
  else if(g_strcmp0(icon, "effect") == 0)
    return dtgtk_cairo_paint_modulegroup_effect;
  else if(g_strcmp0(icon, "grading") == 0)
    return dtgtk_cairo_paint_modulegroup_grading;
  else if(g_strcmp0(icon, "technical") == 0)
    return dtgtk_cairo_paint_modulegroup_technical;

  return dtgtk_cairo_paint_modulegroup_basic;
}

static gint _iop_compare(gconstpointer a, gconstpointer b)
{
  return g_strcmp0((gchar *)a, (gchar *)b);
}
static gboolean _lib_modulegroups_test_internal(dt_lib_module_t *self, uint32_t group, dt_iop_module_t *module)
{
  if(group == DT_MODULEGROUP_ACTIVE_PIPE) return module->enabled;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_list_nth_data(d->groups, group - 1);
  if(gr)
  {
    return (g_list_find_custom(gr->modules, module->so->op, _iop_compare) != NULL);
  }
  return FALSE;
}

static gboolean _lib_modulegroups_test(dt_lib_module_t *self, uint32_t group, dt_iop_module_t *module)
{
  return _lib_modulegroups_test_internal(self, group, module);
}

static gboolean _lib_modulegroups_test_visible(dt_lib_module_t *self, gchar *module)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  for(const GList *l = d->groups; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    if(g_list_find_custom(gr->modules, module, _iop_compare) != NULL)
    {
      return TRUE;
    }
  }
  return FALSE;
}

static void _basics_init_item(dt_lib_modulegroups_basic_item_t *item)
{
  if(!item->id) return;

  gchar **elems = g_strsplit(item->id, "/", -1);
  if(g_strv_length(elems) > 0)
  {
    item->module_op = g_strdup(elems[0]);
    if(item->widget && DT_IS_BAUHAUS_WIDGET(item->widget))
    {
      DtBauhausWidget *bw = DT_BAUHAUS_WIDGET(item->widget);
      if(g_strv_length(elems) > 2)
        item->widget_name = g_strdup_printf("%s - %s", _(elems[1]), bw->label);
      else if(g_strv_length(elems) > 1)
        item->widget_name = g_strdup(bw->label);
      else
      {
        item->widget_name = g_strdup(_("on-off"));
        item->widget_type = WIDGET_TYPE_ACTIVATE_BTN;
      }
    }
    else
    {
      if(g_strv_length(elems) > 2)
        item->widget_name = g_strdup_printf("%s - %s", _(elems[1]), _(elems[2]));
      else if(g_strv_length(elems) > 1)
        item->widget_name = g_strdup(_(elems[1]));
      else
      {
        item->widget_name = g_strdup(_("on-off"));
        item->widget_type = WIDGET_TYPE_ACTIVATE_BTN;
      }
    }
  }
  g_strfreev(elems);
}

static void _basics_free_item(dt_lib_modulegroups_basic_item_t *item)
{
  g_free(item->id);
  g_free(item->module_op);
  if(item->tooltip) g_free(item->tooltip);
  g_free(item->widget_name);
}

static void _basics_remove_widget(dt_lib_modulegroups_basic_item_t *item)
{
  if(item->widget && item->widget_type != WIDGET_TYPE_ACTIVATE_BTN && item->temp_widget)
  {
    g_signal_handlers_disconnect_by_data(item->widget, item);
    g_signal_handlers_disconnect_by_data(item->old_parent, item);

    if(GTK_IS_CONTAINER(item->old_parent) && gtk_widget_get_parent(item->widget) == item->box)
    {
      g_object_ref(item->widget);
      gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(item->widget)), item->widget);

      if(GTK_IS_BOX(item->old_parent))
      {
        if(item->packtype == GTK_PACK_START)
          gtk_box_pack_start(GTK_BOX(item->old_parent), item->widget, item->expand, item->fill, item->padding);
        else
          gtk_box_pack_end(GTK_BOX(item->old_parent), item->widget, item->expand, item->fill, item->padding);

        gtk_box_reorder_child(GTK_BOX(item->old_parent), item->widget, item->old_pos);
      }
      else if(GTK_IS_GRID(item->old_parent))
      {
        gtk_grid_attach(GTK_GRID(item->old_parent), item->widget, item->grid_x, item->grid_y, item->grid_w,
                        item->grid_h);
      }

      g_object_unref(item->widget);
    }
    // put back tooltip
    if(GTK_IS_WIDGET(item->widget))
    {
      gtk_widget_set_tooltip_text(item->widget, item->tooltip);
      gtk_widget_set_has_tooltip(item->widget, TRUE); // even if no tip, to show shortcuts
    }
    // put back label
    if(DT_IS_BAUHAUS_WIDGET(item->widget))
    {
      DtBauhausWidget *bw = DT_BAUHAUS_WIDGET(item->widget);
      bw->show_extended_label = FALSE;
    }
  }
  // cleanup item
  if(item->box) gtk_widget_destroy(item->box);
  if(item->temp_widget) gtk_widget_destroy(item->temp_widget);
  item->box = NULL;
  item->temp_widget = NULL;
  item->widget = NULL;
  item->old_parent = NULL;
  item->module = NULL;
  if(item->tooltip)
  {
    g_free(item->tooltip);
    item->tooltip = NULL;
  }
}

static void _basics_hide(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(!d->vbox_basic) return;
  gtk_widget_hide(d->vbox_basic);

  for(const GList *l = d->basics; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;
    _basics_remove_widget(item);
  }
  gtk_widget_destroy(d->vbox_basic);
  d->vbox_basic = NULL;
}

static gboolean _basics_goto_module(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)(user_data);
  dt_dev_modulegroups_switch(darktable.develop, module);
  dt_iop_gui_set_expanded(module, TRUE, TRUE);
  dt_iop_gui_set_expanded(module, TRUE, FALSE);
  return TRUE;
}

static void _basics_on_off_callback(GtkWidget *btn, dt_lib_modulegroups_basic_item_t *item)
{
  // we switch the "real" button accordingly
  if(darktable.gui->reset) return;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item->module->off),
                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn)));
}
static void _basics_on_off_callback2(GtkWidget *widget, GdkEventButton *e, dt_lib_modulegroups_basic_item_t *item)
{
  // we get the button and change its state
  GtkToggleButton *btn = (GtkToggleButton *)dt_gui_container_first_child(GTK_CONTAINER(item->box));
  if(btn)
  {
    darktable.gui->reset++;
    gtk_toggle_button_set_active(btn, !gtk_toggle_button_get_active(btn));
    darktable.gui->reset--;
    gtk_toggle_button_toggled(btn);
  }
}

static void _sync_visibility(GtkWidget *widget, dt_lib_modulegroups_basic_item_t *item)
{
  if(widget == item->temp_widget)
    gtk_widget_set_visible(item->widget, gtk_widget_get_visible(item->temp_widget));
  if(widget == item->widget)
    gtk_widget_set_visible(item->temp_widget, gtk_widget_get_visible(item->widget));

  gtk_widget_set_visible(item->box, !dt_action_widget_invisible(item->temp_widget));
}

static gboolean _manage_direct_module_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self);

static void _basics_add_widget(dt_lib_module_t *self, dt_lib_modulegroups_basic_item_t *item, GtkWidget *w,
                               dt_lib_modulegroups_basic_item_position_t item_pos)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // if widget already exists, let's remove it and read it correctly
  if(item->widget)
  {
    _basics_remove_widget(item);
    if(item->widget) return; // we shouldn't arrive here !
  }

  // what type of ui we have ?
  const gboolean compact_ui = !dt_conf_get_bool("plugins/darkroom/modulegroups_basics_sections_labels");

  // we retrieve parents, positions, etc... so we can put the widget back in its module
  if(item->widget_type == WIDGET_TYPE_ACTIVATE_BTN)
  {
    // we only show the on-off widget for compact ui. otherwise the button is included in the header
    if(compact_ui)
    {
      // on-off widgets
      item->widget = GTK_WIDGET(item->module->off);
      item->tooltip = gtk_widget_get_tooltip_text(item->widget); // no need to copy, returns a newly-alloced string

      // create new basic widget
      item->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_name(item->box, "basics-widget");

      // we create a new button linked with the real one
      // because it create too much pb to remove the button from the expander
      GtkWidget *btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, 0, item->module);

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn),
                                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(item->widget)));
      g_signal_connect(G_OBJECT(btn), "toggled", G_CALLBACK(_basics_on_off_callback), item);
      gtk_box_pack_start(GTK_BOX(item->box), btn, FALSE, FALSE, 0);
      GtkWidget *evb = gtk_event_box_new();
      GtkWidget *lb = gtk_label_new(item->module->name());
      gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
      gtk_widget_set_name(lb, "basics-iop_name");
      gtk_container_add(GTK_CONTAINER(evb), lb);
      g_signal_connect(G_OBJECT(evb), "button-press-event", G_CALLBACK(_basics_on_off_callback2), item);
      gtk_box_pack_start(GTK_BOX(item->box), evb, FALSE, TRUE, 0);

      // disable widget if needed (multiinstance)
      if(dt_iop_count_instances(item->module->so) > 1)
      {
        gtk_widget_set_sensitive(evb, FALSE);
        gtk_widget_set_sensitive(btn, FALSE);
        gtk_widget_set_tooltip_text(
            lb, _("this quick access widget is disabled as there are multiple instances "
                  "of this module present. Please use the full module to access this widget..."));
        gtk_widget_set_tooltip_text(
            btn, _("this quick access widget is disabled as there are multiple instances "
                   "of this module present. Please use the full module to access this widget..."));
      }
      else
      {
        GtkWidget *orig_label = dt_gui_container_nth_child(GTK_CONTAINER(item->module->header), IOP_MODULE_LABEL);
        gchar *tooltip = gtk_widget_get_tooltip_text(orig_label);
        gtk_widget_set_tooltip_text(lb, tooltip);
        gtk_widget_set_tooltip_text(btn, tooltip);
        g_free(tooltip);
      }

      gtk_widget_show_all(item->box);
    }
  }
  else
  {
    // classic widgets (sliders and combobox)
    if(!w || !GTK_IS_WIDGET(w)) return;

    if(GTK_IS_BOX(gtk_widget_get_parent(w)))
    {
      item->widget = w;
      item->old_parent = gtk_widget_get_parent(item->widget);
      // we retrieve current positions, etc...
      gtk_box_query_child_packing(GTK_BOX(item->old_parent), item->widget, &item->expand, &item->fill,
                                  &item->padding, &item->packtype);
      gtk_container_child_get(GTK_CONTAINER(item->old_parent), item->widget, "position", &item->old_pos, NULL);
    }
    else if(GTK_IS_GRID(gtk_widget_get_parent(w)))
    {
      item->widget = w;
      item->old_parent = gtk_widget_get_parent(item->widget);

      gtk_container_child_get(GTK_CONTAINER(item->old_parent), item->widget,
                              "left-attach", &item->grid_x, "top-attach", &item->grid_y,
                              "width", &item->grid_w, "height", &item->grid_h, NULL);
    }
    else
    {
      // we don't allow other parents at the moment
      return;
    }

    // save old tooltip
    item->tooltip = gtk_widget_get_tooltip_text(item->widget);

    // create new quick access widget
    item->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(item->box, "basics-widget");
    gtk_widget_show(item->box);

    // we reparent the iop widget here
    g_object_ref(item->widget);
    gtk_container_remove(GTK_CONTAINER(item->old_parent), item->widget);
    gtk_box_pack_start(GTK_BOX(item->box), item->widget, TRUE, TRUE, 0);
    gtk_widget_set_hexpand(item->widget, FALSE);
    g_object_unref(item->widget);

    // change the widget label to integrate section name
    if(DT_IS_BAUHAUS_WIDGET(w))
    {
      DtBauhausWidget *bw = DT_BAUHAUS_WIDGET(w);
      bw->show_extended_label = TRUE;
      item->module = (dt_iop_module_t *)bw->module;
    }

    // we put the temporary widget at the place of the real widget in the module
    // this avoid order mismatch when putting back the real widget
    item->temp_widget = gtk_label_new("temp widget");
    if(GTK_IS_CONTAINER(item->old_parent))
    {
      if(GTK_IS_BOX(item->old_parent))
      {
        if(item->packtype == GTK_PACK_START)
          gtk_box_pack_start(GTK_BOX(item->old_parent), item->temp_widget, item->expand, item->fill, item->padding);
        else
          gtk_box_pack_end(GTK_BOX(item->old_parent), item->temp_widget, item->expand, item->fill, item->padding);

        gtk_box_reorder_child(GTK_BOX(item->old_parent), item->temp_widget, item->old_pos);
      }
      else if(GTK_IS_GRID(item->old_parent))
      {
        gtk_grid_attach(GTK_GRID(item->old_parent), item->temp_widget, item->grid_x, item->grid_y, item->grid_w,
                        item->grid_h);
      }
    }

    gchar *txt = g_strdup_printf("%s (%s)\n\n%s%s%s", item->widget_name, item->module->name(),
                                  item->tooltip ? item->tooltip : "", item->tooltip ? "\n\n" : "",
                                  _("(some features may only be available in the full module interface)"));
    gtk_widget_set_tooltip_text(item->widget, txt);
    g_free(txt);

    gtk_widget_add_events(item->widget     , GDK_VISIBILITY_NOTIFY_MASK);
    g_signal_connect(item->widget     , "show", G_CALLBACK(_sync_visibility), item);
    g_signal_connect(item->widget     , "hide", G_CALLBACK(_sync_visibility), item);
    gtk_widget_add_events(item->old_parent , GDK_VISIBILITY_NOTIFY_MASK);
    g_signal_connect(item->old_parent , "show", G_CALLBACK(_sync_visibility), item);
    g_signal_connect(item->old_parent , "hide", G_CALLBACK(_sync_visibility), item);
    gtk_widget_add_events(item->temp_widget, GDK_VISIBILITY_NOTIFY_MASK);
    g_signal_connect(item->temp_widget, "show", G_CALLBACK(_sync_visibility), item);
    g_signal_connect(item->temp_widget, "hide", G_CALLBACK(_sync_visibility), item);
    g_signal_connect(G_OBJECT(item->temp_widget), "destroy", G_CALLBACK(gtk_widget_destroyed), &item->temp_widget);

    _sync_visibility(item->widget, item);
  }

  // if it's the first widget of a module, we need to create the module box structure
  if(item_pos != NORMAL)
  {
    // we create the module header box
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *evb = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(evb), header_box);
    gtk_widget_show_all(evb);
    g_object_set_data(G_OBJECT(evb), "module", item->module->so);
    g_signal_connect(evb, "button-press-event", G_CALLBACK(_manage_direct_module_popup), self);
    gtk_widget_set_name(header_box, "basics-header-box");
    dt_gui_add_class(header_box, "dt_big_btn_canvas");
    gtk_box_pack_start(GTK_BOX(d->vbox_basic), evb, FALSE, FALSE, 0);

    // we create the module box structure
    GtkWidget *hbox_basic = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(hbox_basic, "basics-module-hbox");
    gtk_box_pack_start(GTK_BOX(d->vbox_basic), hbox_basic, TRUE, TRUE, 0);
    d->mod_vbox_basic = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox_basic), d->mod_vbox_basic, TRUE, TRUE, 0);
    gtk_widget_show_all(hbox_basic);

    // we create the link to the full iop
    GtkWidget *wbt = dtgtk_button_new(dtgtk_cairo_paint_link, 0, NULL);
    gtk_widget_show(wbt);
    gchar *tt = g_strdup_printf(_("go to the full version of the %s module"), item->module->name());
    gtk_widget_set_tooltip_text(wbt, tt);
    gtk_widget_set_name(wbt, "basics-link");
    gtk_widget_set_valign(wbt, GTK_ALIGN_CENTER);
    g_free(tt);
    g_signal_connect(G_OBJECT(wbt), "button-press-event", G_CALLBACK(_basics_goto_module), item->module);

    if(!compact_ui)
    {
      // we add the on-off button
      GtkWidget *btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, 0, item->module);

      gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn),
                                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(item->module->off)));
      g_signal_connect(G_OBJECT(btn), "toggled", G_CALLBACK(_basics_on_off_callback), item);
      gtk_widget_show(btn);
      dt_gui_add_class(btn, "dt_transparent_background");
      gtk_box_pack_start(GTK_BOX(header_box), btn, FALSE, FALSE, 0);
      // we add to the module header the section label and the link to the full iop
      GtkWidget *sect = dt_ui_section_label_new(item->module->name());
      gtk_label_set_xalign(GTK_LABEL(sect), 0.5); // we center the module name
      gtk_widget_show(sect);
      gtk_box_pack_start(GTK_BOX(header_box), sect, TRUE, TRUE, 0);

      gtk_box_pack_end(GTK_BOX(header_box), wbt, FALSE, FALSE, 0);
    }
    else
    {
      // if there is no section label, we add the link to the module hbox
      gtk_box_pack_end(GTK_BOX(hbox_basic), wbt, FALSE, FALSE, 0);

      // if there is no label, we handle separately in css the first module header
      if(item_pos == FIRST_MODULE) gtk_widget_set_name(header_box, "basics-header-box-first");
    }
  }

  if(item->box) gtk_box_pack_start(GTK_BOX(d->mod_vbox_basic), item->box, FALSE, FALSE, 0);
}

static gchar *_action_id(dt_action_t *action)
{
  if(action->type != DT_ACTION_TYPE_IOP && action->owner)
  {
    gchar *owner_id = _action_id(action->owner);
    gchar *combined_id = g_strdup_printf("%s/%s", owner_id, action->id);
    g_free(owner_id);
    return combined_id;
  }
  else
    return g_strdup(action->id);
}

static dt_lib_modulegroups_basic_item_position_t
_basics_add_items_from_module_widget(dt_lib_module_t *self, dt_iop_module_t *module, GtkWidget *w,
                                     dt_lib_modulegroups_basic_item_position_t item_pos)
{
  if(!w) return item_pos;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // search for a corresponding basic item
  dt_action_t *ac = module->so->actions.target;
  while(ac)
  {
    if(ac->type >= DT_ACTION_TYPE_WIDGET && ac->target == w)
    {
      gchar *action_id = _action_id(ac);

      for(const GList *l = d->basics; l; l = g_list_next(l))
      {
        dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;
        if(!item->module && g_strcmp0(item->module_op, module->op) == 0
           && item->widget_type != WIDGET_TYPE_ACTIVATE_BTN)
        {
          if(!strcmp(item->id, action_id))
          {
            item->module = module;
            _basics_add_widget(self, item, ac->target, item_pos);
            // we have found and added the widget. no need to go further
            g_free(action_id);
            return NORMAL;
          }
        }
      }
      g_free(action_id);
    }

    if(ac->type == DT_ACTION_TYPE_SECTION)
      ac = ac->target;
    else if(!ac->next && ac->owner->type == DT_ACTION_TYPE_SECTION)
      ac = ac->owner->next;
    else
      ac = ac->next;
  }

  // if w is a container, test all subwidgets
  if(GTK_IS_CONTAINER(w))
  {
    GList *ll = gtk_container_get_children(GTK_CONTAINER(w));
    for(const GList *l = ll; l; l = g_list_next(l))
    {
      item_pos = _basics_add_items_from_module_widget(self, module, l->data, item_pos);
    }
    g_list_free(ll);
  }

  return item_pos;
}

static void _basics_show(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  if(d->vbox_basic && gtk_widget_get_visible(d->vbox_basic)) return;

  if(!d->vbox_basic)
  {
    d->vbox_basic = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, d->vbox_basic);
  }
  if(dt_conf_get_bool("plugins/darkroom/modulegroups_basics_sections_labels"))
    gtk_widget_set_name(d->vbox_basic, "basics-box-labels");
  else
    gtk_widget_set_name(d->vbox_basic, "basics-box");
  dt_gui_add_class(d->vbox_basic,"dt_plugin_ui");

  dt_lib_modulegroups_basic_item_position_t item_pos = FIRST_MODULE;
  for(GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    // we record if it's a new module or not to set css class and box structure
    if(item_pos != FIRST_MODULE) item_pos = NEW_MODULE;

    if(!dt_iop_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED) && module->iop_order != INT_MAX)
    {
      // first, we add on-off buttons if any
      for(const GList *l = d->basics; l; l = g_list_next(l))
      {
        dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;
        if(!item->module && g_strcmp0(item->module_op, module->op) == 0)
        {
          if(item->widget_type == WIDGET_TYPE_ACTIVATE_BTN)
          {
            item->module = module;
            _basics_add_widget(self, item, NULL, item_pos);
            item_pos = NORMAL;
          }
        }
      }

      // for the other items, we want them in same order as the module gui
      _basics_add_items_from_module_widget(self, module, module->widget, item_pos);
    }
  }

  gtk_widget_show(d->vbox_basic);
}

static uint32_t _lib_modulegroups_get_activated(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we get the current group and verify that it is effectively activated
  // this can not be the case if we are in search mode
  GtkWidget *bt = _buttons_get_from_pos(self, d->current);
  if(bt && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bt))) return d->current;
  return DT_MODULEGROUP_NONE;
}

static gboolean _is_module_in_history(dt_iop_module_t *module)
{
  for(const GList *hists = darktable.develop->history; hists; hists = g_list_next(hists))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)hists->data;
    if(hist->module == module) return TRUE;
  }
  return FALSE;
}

static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we hide eventual basic panel
  if(d->current == DT_MODULEGROUP_BASICS && !d->basics_show) d->current = DT_MODULEGROUP_ACTIVE_PIPE;
  _basics_hide(self);

  // if we have a module to force, set d-current to active pipe
  if(d->current == DT_MODULEGROUP_INVALID) d->current = DT_MODULEGROUP_ACTIVE_PIPE;

  const gchar *text_entered = (gtk_widget_is_visible(GTK_WIDGET(d->hbox_search_box)))
                                  ? gtk_entry_get_text(GTK_ENTRY(d->text_entry))
                                  : NULL;

  if(DT_IOP_ORDER_INFO)
    fprintf(stderr,"\n^^^^^ modulegroups");

  // update basic button selection too
  g_signal_handlers_block_matched(d->basic_btn, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->basic_btn), d->current == DT_MODULEGROUP_BASICS);
  g_signal_handlers_unblock_matched(d->basic_btn, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);

  /* only show module group as selected if not currently searching */
  if((d->show_search || d->force_show_module) && d->current != DT_MODULEGROUP_NONE)
  {
    GtkWidget *bt = _buttons_get_from_pos(self, d->current);
    if(bt)
    {
      /* toggle button visibility without executing callback */
      g_signal_handlers_block_matched(bt, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);

      if((text_entered && text_entered[0] != '\0') || d->force_show_module)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), FALSE);
      else
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), TRUE);

      g_signal_handlers_unblock_matched(bt, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);
    }
  }

  // hide deprectade message. it will be shown after if needed
  gtk_widget_set_visible(d->deprecated, FALSE);

  for(const GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
  {
    /*
     * iterate over iop modules and do various test to
     * detect if the modules should be shown or not.
     */
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    GtkWidget *w = module->expander;

    if((DT_IOP_ORDER_INFO) && (module->enabled))
    {
      fprintf(stderr, "\n%20s %d", module->op, module->iop_order);
      if(dt_iop_is_hidden(module)) fprintf(stderr, ", hidden");
    }

      /* skip modules without an gui */
      if(dt_iop_is_hidden(module)) continue;

      // do not show non-active modules
      // we don't want the user to mess with those
      if(module->iop_order == INT_MAX)
      {
        if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
        if(w) gtk_widget_hide(w);
        continue;
      }

      // if we have a module that should be forced shown
      if(d->force_show_module)
      {
        if(d->force_show_module == module && w)
          gtk_widget_show(w);
        else
          gtk_widget_hide(w);

        continue;
      }

      // if there's some search text show matching modules only
      if(text_entered && text_entered[0] != '\0')
      {
        /* don't show deprecated ones unless they are enabled */
        if(module->flags() & IOP_FLAGS_DEPRECATED && !(module->enabled))
        {
          if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
          if(w) gtk_widget_hide(w);
        }
        else
        {
           const int is_match = (g_strstr_len(g_utf8_casefold(dt_iop_get_localized_name(module->op), -1), -1,
                                              g_utf8_casefold(text_entered, -1))
                                 != NULL) ||
                                 (g_strstr_len(g_utf8_casefold(dt_iop_get_localized_aliases(module->op), -1), -1,
                                               g_utf8_casefold(text_entered, -1))
                                 != NULL) ||
                                 (g_strstr_len(g_utf8_casefold(module->multi_name, -1), -1,
                                               g_utf8_casefold(text_entered, -1))
                                 != NULL);


          if(is_match)
            gtk_widget_show(w);
          else
            gtk_widget_hide(w);
        }
        continue;
      }

      /* lets show/hide modules dependent on current group*/
      const gboolean show_deprecated =
        dt_conf_is_equal("plugins/darkroom/modulegroups_preset", _(DEPRECATED_PRESET_NAME));
      gboolean show_module = TRUE;
      switch(d->current)
      {
        case DT_MODULEGROUP_BASICS:
        {
          show_module = FALSE;
        }
        break;

        case DT_MODULEGROUP_ACTIVE_PIPE:
        {
          if(d->full_active)
            show_module = _is_module_in_history(module);
          else
            show_module = module->enabled;
        }
        break;

        case DT_MODULEGROUP_NONE:
        {
          /* show all except hidden ones */
          show_module = (((!(module->flags() & IOP_FLAGS_DEPRECATED) || show_deprecated)
                          && _lib_modulegroups_test_visible(self, module->op))
                         || module->enabled);
        }
        break;

        default:
        {
          // show deprecated module in specific group deprecated
          gtk_widget_set_visible(d->deprecated, show_deprecated || d->force_deprecated_message);

          show_module = (_lib_modulegroups_test_internal(self, d->current, module)
                         && (!(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled || show_deprecated));
        }
      }

      if(show_module)
      {
        if(darktable.develop->gui_module == module && !module->expanded) dt_iop_request_focus(NULL);
        if(w) gtk_widget_show(w);
      }
      else
      {
        if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
        if(w) gtk_widget_hide(w);
      }

  }
  if(DT_IOP_ORDER_INFO) fprintf(stderr,"\nvvvvv\n");
  // now that visibility has been updated set multi-show
  dt_dev_modules_update_multishow(darktable.develop);

  // we show eventual basic panel but only if no text in the search box
  if(d->current == DT_MODULEGROUP_BASICS && !(text_entered && text_entered[0] != '\0')) _basics_show(self);
}

static void _lib_modulegroups_toggle(GtkWidget *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  const gchar *text_entered = (gtk_widget_is_visible(GTK_WIDGET(d->hbox_search_box)))
                                  ? gtk_entry_get_text(GTK_ENTRY(d->text_entry))
                                  : NULL;

  /* block all button callbacks */
  const int ngroups = g_list_length(d->groups);
  for(int k = 0; k <= ngroups; k++)
    g_signal_handlers_block_matched(_buttons_get_from_pos(self, k), G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                    _lib_modulegroups_toggle, NULL);
  g_signal_handlers_block_matched(d->basic_btn, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);

  /* deactivate all buttons */
  int gid = 0;
  for(int k = 0; k <= ngroups; k++)
  {
    const GtkWidget *bt = _buttons_get_from_pos(self, k);
    /* store toggled modulegroup */
    if(bt == button) gid = k;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), FALSE);
  }
  if(button == d->basic_btn) gid = DT_MODULEGROUP_BASICS;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->basic_btn), FALSE);

  /* only deselect button if not currently searching else re-enable module */
  if(d->current == gid && !(text_entered && text_entered[0] != '\0'))
    d->current = DT_MODULEGROUP_NONE;
  else
  {
    d->current = gid;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_buttons_get_from_pos(self, gid)), TRUE);
  }

  /* unblock all button callbacks */
  for(int k = 0; k <= ngroups; k++)
    g_signal_handlers_unblock_matched(_buttons_get_from_pos(self, k), G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                      _lib_modulegroups_toggle, NULL);
  g_signal_handlers_unblock_matched(d->basic_btn, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);

  /* clear search text */
  if(gtk_widget_is_visible(GTK_WIDGET(d->hbox_search_box)))
  {
    g_signal_handlers_block_matched(d->text_entry, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _text_entry_changed_callback, NULL);
    gtk_entry_set_text(GTK_ENTRY(d->text_entry), "");
    g_signal_handlers_unblock_matched(d->text_entry, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _text_entry_changed_callback, NULL);
  }

  /* update visibility */
  d->force_show_module = NULL;
  _lib_modulegroups_update_iop_visibility(self);
}

typedef struct _set_gui_thread_t
{
  dt_lib_module_t *self;
  uint32_t group;
} _set_gui_thread_t;

static gboolean _lib_modulegroups_set_gui_thread(gpointer user_data)
{
  _set_gui_thread_t *params = (_set_gui_thread_t *)user_data;

  /* set current group and update visibility */
  GtkWidget *bt = _buttons_get_from_pos(params->self, params->group);
  if(bt) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), TRUE);
  _lib_modulegroups_update_iop_visibility(params->self);

  free(params);
  return FALSE;
}

static gboolean _lib_modulegroups_upd_gui_thread(gpointer user_data)
{
  _set_gui_thread_t *params = (_set_gui_thread_t *)user_data;

  _lib_modulegroups_update_iop_visibility(params->self);

  free(params);
  return FALSE;
}

/* this is a proxy function so it might be called from another thread */
static void _lib_modulegroups_set(dt_lib_module_t *self, uint32_t group)
{
  _set_gui_thread_t *params = (_set_gui_thread_t *)malloc(sizeof(_set_gui_thread_t));
  if(!params) return;
  params->self = self;
  params->group = group;
  g_main_context_invoke(NULL, _lib_modulegroups_set_gui_thread, params);
}

/* this is a proxy function so it might be called from another thread */
static void _lib_modulegroups_update_visibility_proxy(dt_lib_module_t *self)
{
  _set_gui_thread_t *params = (_set_gui_thread_t *)malloc(sizeof(_set_gui_thread_t));
  if(!params) return;
  params->self = self;
  g_main_context_invoke(NULL, _lib_modulegroups_upd_gui_thread, params);
}

static void _lib_modulegroups_switch_group(dt_lib_module_t *self, dt_iop_module_t *module)
{
  /* lets find the group which is not active pipe */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  const int ngroups = g_list_length(d->groups);
  for(int k = 1; k <= ngroups; k++)
  {
    if(_lib_modulegroups_test(self, k, module))
    {
      d->force_show_module = NULL;
      _lib_modulegroups_set(self, k);
      return;
    }
  }
  // if we arrive here, that means the module is not part of any group
  // so we force it to be shown outside any group
  d->force_show_module = module;
  d->current = DT_MODULEGROUP_INVALID;
  _lib_modulegroups_set(self, DT_MODULEGROUP_INVALID);
}

static uint32_t _lib_modulegroups_get(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  return d->current;
}

static dt_lib_modulegroup_iop_visibility_type_t _preset_retrieve_old_search_pref(gchar **ret)
{
  // show the search box ?
  const char *show_text_entry = dt_conf_get_string_const("plugins/darkroom/search_iop_by_text");
  dt_lib_modulegroup_iop_visibility_type_t val = DT_MODULEGROUP_SEARCH_IOP_TEXT_GROUPS_VISIBLE;

  if(strcmp(show_text_entry, "show search text") == 0)
  {
    // we only show the search box. no groups
    *ret = dt_util_dstrcat(*ret, "1ꬹ1");
    val = DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE;
  }
  else if(strcmp(show_text_entry, "show groups") == 0)
  {
    // we don't show the search box
    *ret = dt_util_dstrcat(*ret, "0");
    val = DT_MODULEGROUP_SEARCH_IOP_GROUPS_VISIBLE;
  }
  else
  {
    // we show both
    *ret = dt_util_dstrcat(*ret, "1");
    val = DT_MODULEGROUP_SEARCH_IOP_TEXT_GROUPS_VISIBLE;
  }
  return val;
}

/* presets syntax :
  Layout presets are saved as string consisting of blocs separated by ꬹ
  OPTIONSꬹBLOC_0ꬹBLOC_1ꬹBLOC_2....
  OPTION : show_search(0-1) | active == history (0-1)
  BLOC_0 : reserved for future use. Always 1
  BLOC_1.... : blocs describing each group, contains :
    name|icon_name||iop_name_0|iop_name_1....
*/

static gchar *_preset_retrieve_old_layout_updated()
{
  gchar *ret = NULL;

  // show the search box ?
  if(_preset_retrieve_old_search_pref(&ret) == DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE) return ret;

  // layout with "new" 3 groups
  for(int i = 0; i < 4; i++)
  {
    // group name and icon
    if(i == 0)
    {
      ret = dt_util_dstrcat(
          ret, "1|0ꬹ1|||%s",
          "exposure/exposure|temperature/temperature|temperature/tint|colorbalancergb/contrast"
          "|colorbalancergb/global vibrance|colorbalancergb/global chroma|colorbalancergb/global saturation"
          "|ashift/rotation|denoiseprofile|lens|bilat");
      ret = dt_util_dstrcat(ret, "ꬹfavorites|favorites|");
    }
    else if(i == 1)
      ret = dt_util_dstrcat(ret, "ꬹtechnical|technical|");
    else if(i == 2)
      ret = dt_util_dstrcat(ret, "ꬹgrading|grading|");
    else if(i == 3)
      ret = dt_util_dstrcat(ret, "ꬹeffects|effect|");

    // list of modules
    for(const GList *modules = darktable.iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);

      if(!dt_iop_so_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
      {
        // get previous visibility values
        const int group = module->default_group();
        gchar *key = g_strdup_printf("plugins/darkroom/%s/visible", module->op);
        const gboolean visi = dt_conf_get_bool(key);
        g_free(key);
        key = g_strdup_printf("plugins/darkroom/%s/favorite", module->op);
        const gboolean fav = dt_conf_get_bool(key);
        g_free(key);

        if((i == 0 && fav && visi) || (i == 1 && (group & IOP_GROUP_TECHNICAL) && visi)
           || (i == 2 && (group & IOP_GROUP_GRADING) && visi) || (i == 3 && (group & IOP_GROUP_EFFECTS) && visi))
        {
          ret = dt_util_dstrcat(ret, "|%s", module->op);
        }
      }
    }
  }
  return ret;
}

static gchar *_preset_retrieve_old_layout(const char *list, const char *list_fav)
{
  gchar *ret = NULL;

  // show the search box ?
  if(_preset_retrieve_old_search_pref(&ret) == DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE) return ret;

  // layout with "old" 5 groups
  for(int i = 0; i < 6; i++)
  {
    // group name and icon
    if(i == 0)
    {
      // we don't have to care about "modern" workflow for temperature as it's more recent than this layout
      ret = dt_util_dstrcat(
          ret, "1|0ꬹ1|||%s",
          "exposure/exposure|temperature/temperature|temperature/tint|colorbalancergb/contrast"
          "|colorbalancergb/global vibrance|colorbalancergb/global chroma|colorbalancergb/global saturation"
          "|ashift/rotation|denoiseprofile|lens|bilat");
      ret = dt_util_dstrcat(ret, "ꬹfavorites|favorites|");
    }
    else if(i == 1)
      ret = dt_util_dstrcat(ret, "ꬹbase|basic|");
    else if(i == 2)
      ret = dt_util_dstrcat(ret, "ꬹtone|tone|");
    else if(i == 3)
      ret = dt_util_dstrcat(ret, "ꬹcolor|color|");
    else if(i == 4)
      ret = dt_util_dstrcat(ret, "ꬹcorrect|correct|");
    else if(i == 5)
      ret = dt_util_dstrcat(ret, "ꬹeffect|effect|");

    // list of modules
    for(const GList *modules = darktable.iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);

      if(!dt_iop_so_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
      {
        gchar *search = g_strdup_printf("|%s|", module->op);
        gchar *key;

        // get previous visibility values
        int group = -1;
        if(i > 0 && list)
        {
          // we retrieve the group from hardcoded one
          const int gr = module->default_group();
          if(gr & IOP_GROUP_BASIC)
            group = 1;
          else if(gr & IOP_GROUP_TONE)
            group = 2;
          else if(gr & IOP_GROUP_COLOR)
            group = 3;
          else if(gr & IOP_GROUP_CORRECT)
            group = 4;
          else if(gr & IOP_GROUP_EFFECT)
            group = 5;
        }
        else if(i > 0)
        {
          key = g_strdup_printf("plugins/darkroom/%s/modulegroup", module->op);
          group = dt_conf_get_int(key);
          g_free(key);
        }

        gboolean visi = FALSE;
        if(list)
          visi = (strstr(list, search) != NULL);
        else
        {
          key = g_strdup_printf("plugins/darkroom/%s/visible", module->op);
          visi = dt_conf_get_bool(key);
          g_free(key);
        }

        gboolean fav = FALSE;
        if(i == 0 && list_fav)
          fav = (strstr(list_fav, search) != NULL);
        else if(i == 0)
        {
          key = g_strdup_printf("plugins/darkroom/%s/favorite", module->op);
          fav = dt_conf_get_bool(key);
          g_free(key);
        }

        if((i == 0 && fav && visi) || (i == group && visi))
        {
          ret = dt_util_dstrcat(ret, "|%s", module->op);
        }

        g_free(search);
      }
    }
  }
  return ret;
}

static void _preset_retrieve_old_presets(dt_lib_module_t *self)
{
  // we retrieve old modulelist presets
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params"
                              " FROM data.presets"
                              " WHERE operation = 'modulelist' AND op_version = 1 AND writeprotect = 0",
                              -1, &stmt, NULL);
  // clang-format on

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *pname = (char *)sqlite3_column_text(stmt, 0);
    const char *p = (char *)sqlite3_column_blob(stmt, 1);
    const int size = sqlite3_column_bytes(stmt, 1);

    gchar *list = NULL;
    gchar *fav = NULL;
    int pos = 0;
    while(pos < size)
    {
      const char *op = p + pos;
      const int op_len = strlen(op);
      dt_iop_module_state_t state = p[pos + op_len + 1];

      if(state == IOP_STATE_ACTIVE)
        list = dt_util_dstrcat(list, "|%s", op);
      else if(state == IOP_STATE_FAVORITE)
      {
        fav = dt_util_dstrcat(fav, "|%s", op);
        list = dt_util_dstrcat(list, "|%s", op);
      }
      pos += op_len + 2;
    }
    list = dt_util_dstrcat(list, "|");
    fav = dt_util_dstrcat(fav, "|");

    gchar *tx = _preset_retrieve_old_layout(list, fav);
    dt_lib_presets_add(pname, self->plugin_name, self->version(), tx, strlen(tx), FALSE);
    g_free(tx);
    g_free(list);
    g_free(fav);
  }
  sqlite3_finalize(stmt);

  // and we remove all existing modulelist presets
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM data.presets"
                        " WHERE operation = 'modulelist' AND op_version = 1",
                        NULL, NULL, NULL);
}

static gchar *_preset_to_string(dt_lib_module_t *self, gboolean edition)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  gchar *res = NULL;
  const gboolean show_search = edition ? d->edit_show_search : d->show_search;
  const gboolean full_active = edition ? d->edit_full_active : d->full_active;
  res = dt_util_dstrcat(res, "%d|%d", show_search ? 1 : 0, full_active ? 1 : 0);

  const gboolean basics_show = edition ? d->edit_basics_show : d->basics_show;
  GList *basics = edition ? d->edit_basics : d->basics;
  GList *groups = edition ? d->edit_groups : d->groups;

  // basics widgets
  res = dt_util_dstrcat(res, "ꬹ%d||", basics_show ? 1 : 0);
  for(const GList *l = basics; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;
    res = dt_util_dstrcat(res, "|%s", item->id);
  }

  for(const GList *l = groups; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_group_t *g = (dt_lib_modulegroups_group_t *)l->data;
    res = dt_util_dstrcat(res, "ꬹ%s|%s|", g->name, g->icon);
    for(const GList *ll = g->modules; ll; ll = g_list_next(ll))
    {
      gchar *m = (gchar *)ll->data;
      res = dt_util_dstrcat(res, "|%s", m);
    }
  }

  return res;
}

static void _preset_from_string(dt_lib_module_t *self, gchar *txt, gboolean edition)
{
  if(!txt) return;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  gboolean show_search = TRUE;
  gboolean full_active = FALSE;

  gchar **gr = g_strsplit(txt, "ꬹ", -1);

  // read the general options
  if(g_strv_length(gr) > 0)
  {
    gchar **gr2 = g_strsplit(gr[0], "|", -1);
    // do we show the search bar
    if(!g_strcmp0(gr2[0], "0")) show_search = FALSE;
    // do we show all history module in active group
    if(g_strv_length(gr2) > 1 && (g_strcmp0(gr2[1], "1") == 0)) full_active = TRUE;

    g_strfreev(gr2);
  }

  // read the basics widgets
  if(g_strv_length(gr) > 1)
  {
    if(gr[1])
    {
      gchar **gr2 = g_strsplit(gr[1], "|", -1);
      gboolean basics_show = FALSE;
      if(g_strv_length(gr2) > 3 && (g_strcmp0(gr2[0], "1") == 0)) basics_show = TRUE;
      if(edition)
        d->edit_basics_show = basics_show;
      else
        d->basics_show = basics_show;

      for(int j = 3; j < g_strv_length(gr2); j++)
      {
        dt_lib_modulegroups_basic_item_t *item
            = (dt_lib_modulegroups_basic_item_t *)g_malloc0(sizeof(dt_lib_modulegroups_basic_item_t));
        item->id = g_strdup(gr2[j]);
        _basics_init_item(item);

        if(edition)
          d->edit_basics = g_list_append(d->edit_basics, item);
        else
          d->basics = g_list_append(d->basics, item);
      }
      g_strfreev(gr2);
    }
  }

  GList *res = NULL;
  // read the groups
  for(int i = 2; i < g_strv_length(gr); i++)
  {
    gchar *tx = gr[i];
    if(tx)
    {
      gchar **gr2 = g_strsplit(tx, "|", -1);
      const int nb = g_strv_length(gr2);
      if(nb > 2)
      {
        dt_lib_modulegroups_group_t *group
            = (dt_lib_modulegroups_group_t *)g_malloc0(sizeof(dt_lib_modulegroups_group_t));
        group->name = g_strdup(gr2[0]);
        group->icon = g_strdup(gr2[1]);
        // gr2[2] is reserved for eventual future use
        for(int j = 3; j < nb; j++)
        {
          group->modules = g_list_append(group->modules, g_strdup(gr2[j]));
        }
        res = g_list_prepend(res, group);
      }
      g_strfreev(gr2);
    }
  }
  g_strfreev(gr);
  res = g_list_reverse(res);  // list was built in reverse order, so un-reverse it

  // and we set the values
  if(edition)
  {
    d->edit_show_search = show_search;
    d->edit_full_active = full_active;
    d->edit_groups = res;
  }
  else
  {
    d->show_search = show_search;
    d->full_active = full_active;
    d->groups = res;
  }
}

// start no quick access
#define SNQA()                                                                                                    \
  {                                                                                                               \
    g_free(tx);                                                                                                   \
    tx = g_strdup("1|0ꬹ0||");                                                                                   \
  }

// start quick access
#define SQA(is_modern, is_scene_referred)                                                                         \
  {                                                                                                               \
    g_free(tx);                                                                                                   \
    tx = g_strdup_printf("1|0ꬹ1||");                                                                            \
    if(is_scene_referred)                                                                                         \
    {                                                                                                             \
      AM("filmicrgb/white relative exposure");                                                                    \
      AM("filmicrgb/black relative exposure");                                                                    \
      AM("filmicrgb/contrast");                                                                                   \
    }                                                                                                             \
    if(is_modern)                                                                                                 \
    {                                                                                                             \
      AM("channelmixerrgb/temperature");                                                                          \
      AM("channelmixerrgb/chroma");                                                                               \
      AM("channelmixerrgb/hue");                                                                                  \
    }                                                                                                             \
    else                                                                                                          \
    {                                                                                                             \
      AM("temperature/temperature");                                                                              \
      AM("temperature/tint");                                                                                     \
    }                                                                                                             \
    AM("exposure/exposure");                                                                                      \
    if(!is_scene_referred) AM("colorbalancergb/contrast"); /* contrast is already in filmic */                    \
    AM("colorbalancergb/global chroma");                                                                          \
    AM("colorbalancergb/global vibrance");                                                                        \
    AM("colorbalancergb/global saturation");                                                                      \
    AM("ashift/rotation");                                                                                        \
    AM("denoiseprofile");                                                                                         \
    AM("lens");                                                                                                   \
    AM("bilat");                                                                                                  \
  }

// start module group
#define SMG(g,n) tx = dt_util_dstrcat(tx, "ꬹ%s|%s|", g, n)

// add module
#define AM(n)    tx = dt_util_dstrcat(tx, "|%s", n)

void init_presets(dt_lib_module_t *self)
{
  /*
    For the record, one can create the preset list by using the following code:

    $ cat <( git grep "return.*IOP_GROUP_TONE" -- src/iop/ | cut -d':' -f1 ) \
          <( git grep IOP_FLAGS_DEPRECATED -- src/iop/ | cut -d':' -f1 ) | \
          grep -E -v "useless|mask_manager|gamma" | sort | uniq --unique | \
          while read file; do BN=$(basename $(basename $file .cc) .c); \
            echo "AM(\"${BN:0:16}\");" ; done
  */

  const gboolean is_modern =
    dt_conf_is_equal("plugins/darkroom/chromatic-adaptation", "modern");
  const gboolean is_scene_referred =
    dt_conf_is_equal("plugins/darkroom/workflow", "scene-referred");

  // all modules
  gchar *tx = NULL;

  SQA(is_modern, is_scene_referred);

  SMG(C_("modulegroup", "base"), "basic");
  AM("basecurve");
  AM("crop");
  AM("ashift");
  AM("colisa");
  AM("colorreconstruct");
  AM("demosaic");
  AM("exposure");
  AM("finalscale");
  AM("flip");
  AM("highlights");
  AM("negadoctor");
  AM("overexposed");
  AM("rawoverexposed");
  AM("rawprepare");
  AM("shadhi");
  AM("temperature");
  AM("toneequal");

  SMG(C_("modulegroup", "tone"), "tone");
  AM("bilat");
  AM("filmicrgb");
  AM("levels");
  AM("rgbcurve");
  AM("rgblevels");
  AM("sigmoid");
  AM("tonecurve");

  SMG(C_("modulegroup", "color"), "color");
  AM("channelmixerrgb");
  AM("colorbalancergb");
  AM("colorchecker");
  AM("colorcontrast");
  AM("colorcorrection");
  AM("colorin");
  AM("colorout");
  AM("colorzones");
  AM("lut3d");
  AM("monochrome");
  AM("profile");
  AM("gamma");
  AM("velvia");

  SMG(C_("modulegroup", "correct"), "correct");
  AM("atrous");
  AM("bilateral");
  AM("cacorrect");
  AM("cacorrectrgb");
  AM("denoiseprofile");
  AM("dither");
  AM("hazeremoval");
  AM("hotpixels");
  AM("lens");
  AM("liquify");
  AM("nlmeans");
  AM("rawdenoise");
  AM("retouch");
  AM("rotatepixels");
  AM("scalepixels");
  AM("sharpen");

  SMG(C_("modulegroup", "effect"), "effect");
  AM("bloom");
  AM("borders");
  AM("colorize");
  AM("colormapping");
  AM("graduatednd");
  AM("grain");
  AM("highpass");
  AM("lowlight");
  AM("lowpass");
  AM("soften");
  AM("splittoning");
  AM("vignette");
  AM("watermark");
  AM("censorize");
  AM("blurs");
  AM("diffuse");

  dt_lib_presets_add(_("modules: all"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  // minimal / 3 tabs

  SQA(is_modern, is_scene_referred);

  SMG(C_("modulegroup", "base"), "basic");
  AM("ashift");

  if(is_scene_referred)
    AM("filmicrgb");
  else
    AM("basecurve");

  AM("crop");
  AM("denoiseprofile");
  AM("exposure");
  AM("flip");
  AM("lens");
  AM("temperature");

  SMG(C_("modulegroup", "grading"), "grading");
  AM("channelmixerrgb");
  AM("colorzones");
  AM("graduatednd");
  AM("rgbcurve");
  AM("rgblevels");
  AM("splittoning");

  SMG(C_("modulegroup", "effects"), "effect");
  AM("borders");
  AM("monochrome");
  AM("retouch");
  AM("sharpen");
  AM("vignette");
  AM("watermark");

  dt_lib_presets_add(_("workflow: beginner"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  // display referred
  SQA(is_modern, FALSE);

  SMG(C_("modulegroup", "base"), "basic");
  AM("basecurve");
  AM("toneequal");
  AM("crop");
  AM("ashift");
  AM("flip");
  AM("exposure");
  AM("temperature");
  AM("rgbcurve");
  AM("rgblevels");
  AM("bilat");
  AM("shadhi");
  AM("highlights");

  SMG(C_("modulegroup", "color"), "color");
  AM("channelmixerrgb");
  AM("colorbalancergb");
  AM("colorcorrection");
  AM("colorzones");
  AM("monochrome");
  AM("velvia");

  SMG(C_("modulegroup", "correct"), "correct");
  AM("cacorrect");
  AM("cacorrectrgb");
  AM("denoiseprofile");
  AM("hazeremoval");
  AM("hotpixels");
  AM("lens");
  AM("retouch");
  AM("liquify");
  AM("sharpen");
  AM("nlmeans");

  SMG(C_("modulegroup", "effect"), "effect");
  AM("borders");
  AM("colorize");
  AM("graduatednd");
  AM("grain");
  AM("splittoning");
  AM("vignette");
  AM("watermark");
  AM("censorize");

  dt_lib_presets_add(_("workflow: display-referred"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  // scene referred

  SQA(is_modern, TRUE);

  SMG(C_("modulegroup", "base"), "basic");
  AM("filmicrgb");
  AM("sigmoid");
  AM("toneequal");
  AM("crop");
  AM("ashift");
  AM("flip");
  AM("exposure");
  AM("temperature");
  AM("bilat");

  SMG(C_("modulegroup", "color"), "color");
  AM("channelmixerrgb");
  AM("colorbalancergb");
  AM("colorzones");

  SMG(C_("modulegroup", "correct"), "correct");
  AM("cacorrect");
  AM("cacorrectrgb");
  AM("denoiseprofile");
  AM("hazeremoval");
  AM("hotpixels");
  AM("lens");
  AM("retouch");
  AM("liquify");
  AM("sharpen");
  AM("nlmeans");

  SMG(C_("modulegroup", "effect"), "effect");
  AM("atrous");
  AM("borders");
  AM("graduatednd");
  AM("grain");
  AM("vignette");
  AM("watermark");
  AM("censorize");
  AM("blurs");
  AM("diffuse");

  dt_lib_presets_add(_("workflow: scene-referred"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  // default / 3 tabs based on Aurélien's proposal

  SQA(is_modern, is_scene_referred);

  SMG(C_("modulegroup", "technical"), "technical");
  AM("basecurve");
  AM("bilateral");
  AM("cacorrect");
  AM("crop");
  AM("ashift");
  AM("colorchecker");
  AM("colorin");
  AM("colorout");

  AM("colorreconstruct");
  AM("cacorrectrgb");
  AM("demosaic");
  AM("denoiseprofile");
  AM("dither");
  AM("exposure");
  AM("filmicrgb");
  AM("finalscale");
  AM("flip");
  AM("hazeremoval");
  AM("highlights");
  AM("hotpixels");
  AM("lens");
  AM("lut3d");
  AM("negadoctor");
  AM("nlmeans");
  AM("overexposed");
  AM("rawdenoise");
  AM("rawoverexposed");
  AM("rotatepixels");
  AM("temperature");
  AM("scalepixels");

  SMG(C_("modulegroup", "grading"), "grading");
  AM("channelmixerrgb");
  AM("colisa");
  AM("colorbalancergb");
  AM("colorcontrast");
  AM("colorcorrection");
  AM("colorize");
  AM("colorzones");
  AM("graduatednd");
  AM("levels");
  AM("rgbcurve");
  AM("rgblevels");
  AM("shadhi");
  AM("splittoning");
  AM("tonecurve");
  AM("toneequal");
  AM("velvia");

  SMG(C_("modulegroup", "effects"), "effect");
  AM("atrous");
  AM("bilat");
  AM("bloom");
  AM("borders");
  AM("colormapping");
  AM("grain");
  AM("highpass");
  AM("liquify");
  AM("lowlight");
  AM("lowpass");
  AM("monochrome");
  AM("retouch");
  AM("sharpen");
  AM("soften");
  AM("vignette");
  AM("watermark");
  AM("censorize");
  AM("blurs");
  AM("diffuse");

  dt_lib_presets_add(_(FALLBACK_PRESET_NAME), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  // search only (only active modules visible)
  SNQA();
  dt_lib_presets_add(_("search only"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  // this is a special preset for all newly deprecated modules
  // so users still have a chance to access them until next release (with warning messages)
  // this modules are deprecated in 3.4 and should be removed from this group in 3.8 (1 year later)
  SNQA();
  SMG(C_("modulegroup", "deprecated"), "basic");
  // these modules are deprecated in 3.6 and should be removed in 4.0 (1 year later)
  AM("spots");
  AM("defringe");
  // these modules are deprecated in 3.8 and should be removed in 4.1 (1 year later)
  AM("clipping");

  dt_lib_presets_add(_(DEPRECATED_PRESET_NAME), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  g_free(tx);

  // if needed, we add a new preset, based on last user config
  if(!dt_conf_key_exists("plugins/darkroom/modulegroups_preset"))
  {
    tx = _preset_retrieve_old_layout(NULL, NULL);
    dt_lib_presets_add(_("previous config"), self->plugin_name, self->version(), tx, strlen(tx), FALSE);
    dt_conf_set_string("plugins/darkroom/modulegroups_preset", _("previous layout"));
    g_free(tx);

    tx = _preset_retrieve_old_layout_updated();
    dt_lib_presets_add(_("previous config with new layout"), self->plugin_name, self->version(), tx,
                       strlen(tx), FALSE);
    g_free(tx);
  }
  // if they exists, we retrieve old user presets from old modulelist lib
  _preset_retrieve_old_presets(self);
}

static gchar *_presets_get_minimal(dt_lib_module_t *self)
{
  const gboolean is_modern = dt_conf_is_equal("plugins/darkroom/chromatic-adaptation", "modern");
  const gboolean is_scene_referred = dt_conf_is_equal("plugins/darkroom/workflow", "scene-referred");

  // all modules
  gchar *tx = NULL;

  SQA(is_modern, is_scene_referred);
  AM("exposure/exposure");
  AM("colorbalancergb/contrast");

  SMG(C_("modulegroup", "base"), "basic");
  if(is_scene_referred)
    AM("filmicrgb");
  else
    AM("basecurve");
  AM("exposure");
  AM("colorbalancergb");

  return tx;
}

#undef SNQA
#undef SQA
#undef SMG
#undef AM

void *legacy_params(dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size)
{
  return NULL;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  gchar *tx = _preset_to_string(self, FALSE);
  *size = strlen(tx);
  return tx;
}

static void _manage_editor_groups_cleanup(dt_lib_module_t *self, gboolean edition)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  GList *l = edition ? d->edit_groups : d->groups;

  for(; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    g_free(gr->name);
    g_free(gr->icon);
    g_list_free_full(gr->modules, g_free);
  }

  if(edition)
  {
    g_list_free_full(d->edit_groups, g_free);
    d->edit_groups = NULL;
  }
  else
  {
    g_list_free_full(d->groups, g_free);
    d->groups = NULL;
    _basics_hide(self);
  }

  l = edition ? d->edit_basics : d->basics;
  for(; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;
    _basics_free_item(item);
  }
  if(edition)
  {
    g_list_free_full(d->edit_basics, g_free);
    d->edit_basics = NULL;
  }
  else
  {
    g_list_free_full(d->basics, g_free);
    d->basics = NULL;
  }
}

static void _manage_editor_basics_remove(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  const char *id = (char *)g_object_get_data(G_OBJECT(widget), "widget_id");
  for(GList *l = d->edit_basics; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;
    if(g_strcmp0(item->id, id) == 0)
    {
      _basics_free_item(item);
      d->edit_basics = g_list_delete_link(d->edit_basics, l);
      gtk_widget_destroy(gtk_widget_get_parent(widget));
      break;
    }
  }
}

static int _manage_editor_module_find_multi(gconstpointer a, gconstpointer b)
{
  // we search for a other instance of module with lower priority
  dt_iop_module_t *ma = (dt_iop_module_t *)a;
  dt_iop_module_t *mb = (dt_iop_module_t *)b;
  if(g_strcmp0(ma->op, mb->op) != 0) return 1;
  if(ma->multi_priority >= mb->multi_priority) return 0;
  return 1;
}

static void _manage_editor_basics_update_list(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // first, we remove all existing modules
  dt_gui_container_destroy_children(GTK_CONTAINER(d->edit_basics_box));

  // and we add the ones from the list
  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    for(const GList *l = d->edit_basics; l; l = g_list_next(l))
    {
      dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;

      if(g_strcmp0(module->op, item->module_op) == 0 && !dt_iop_is_hidden(module))
      {
        // we want to avoid showing multiple instances of the same module
        if(module->multi_priority <= 0
           || g_list_find_custom(darktable.develop->iop, module, _manage_editor_module_find_multi) == NULL)
        {
          GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
          gtk_widget_set_name(hb, "modulegroups-iop-header");
          gchar *lbn = g_strdup_printf("%s\n    %s", module->name(), item->widget_name);
          GtkWidget *lb = gtk_label_new(lbn);
          gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_END);
          gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
          g_free(lbn);
          gtk_widget_set_name(lb, "iop-panel-label");
          gtk_box_pack_start(GTK_BOX(hb), lb, FALSE, TRUE, 0);
          if(!d->edit_ro)
          {
            GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);

            gtk_widget_set_tooltip_text(btn, _("remove this widget"));
            g_object_set_data(G_OBJECT(btn), "widget_id", item->id);
            g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_basics_remove), self);
            gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, TRUE, 0);
          }
          gtk_box_pack_start(GTK_BOX(d->edit_basics_box), hb, FALSE, TRUE, 0);
        }
      }
    }
  }

  gtk_widget_show_all(d->edit_basics_box);
}

static void _basics_cleanup_list(dt_lib_module_t *self, gboolean edition)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // ensure here that there's no basics widget of a module not present in one other group
  GList *l = edition ? d->edit_basics : d->basics;
  while(l)
  {
    dt_lib_modulegroups_basic_item_t *item = (dt_lib_modulegroups_basic_item_t *)l->data;
    gboolean exists = FALSE;
    for(GList *ll = edition ? d->edit_groups : d->groups; ll; ll = g_list_next(ll))
    {
      dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)ll->data;
      if(g_list_find_custom(gr->modules, item->module_op, _iop_compare))
      {
        exists = TRUE;
        break;
      }
    }
    // if the module doesn't exists, let's remove the widget
    if(!exists)
    {
      GList *ln = g_list_next(l);
      _basics_free_item(item);
      if(edition)
        d->edit_basics = g_list_delete_link(d->edit_basics, l);
      else
        d->basics = g_list_delete_link(d->basics, l);
      l = ln;
      continue;
    }
    l = g_list_next(l);
  }
  // if we are on edition mode, we need to update the box too
  if(edition && d->edit_basics_box && GTK_IS_BOX(d->edit_basics_box)) _manage_editor_basics_update_list(self);
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  // cleanup existing groups
  _manage_editor_groups_cleanup(self, FALSE);

  _preset_from_string(self, (char *)params, FALSE);

  gchar *tx = g_strdup_printf("plugins/darkroom/%s/last_preset", self->plugin_name);

  gchar *value = dt_conf_get_string(tx);
  dt_conf_set_string("plugins/darkroom/modulegroups_preset", value);
  g_free(value);
  g_free(tx);

  _buttons_update(self);
  return 0;
}

static void _manage_editor_save(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(!d->edit_preset) return;

  // get all the values
  d->edit_show_search = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->edit_search_cb));
  d->edit_full_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->edit_full_active_cb));
  gchar *params = _preset_to_string(self, TRUE);

  // update the preset in the database
  dt_lib_presets_update(d->edit_preset, self->plugin_name, self->version(), d->edit_preset, "", params,
                        strlen(params));
  g_free(params);

  // update groups
  const char *preset = dt_conf_get_string_const("plugins/darkroom/modulegroups_preset");
  if(g_strcmp0(preset, d->edit_preset) == 0)
  {
    const int cur = d->current;
    // we update the gui
    if(!dt_lib_presets_apply(d->edit_preset, self->plugin_name, self->version()))
      dt_lib_presets_apply((gchar *)C_("modulegroup", FALLBACK_PRESET_NAME),
                           self->plugin_name, self->version());

    // and we ensure the right group is selected
    d->current = cur;
    _lib_modulegroups_update_iop_visibility(self);
  }
}

static void _manage_editor_module_remove(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  const char *module = (char *)g_object_get_data(G_OBJECT(widget), "module_name");
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");

  for(GList *l = gr->modules; l; l = g_list_next(l))
  {
    const char *tx = (char *)l->data;
    if(g_strcmp0(tx, module) == 0)
    {
      g_free(l->data);
      gr->modules = g_list_delete_link(gr->modules, l);
      gtk_widget_destroy(gtk_widget_get_parent(widget));
      break;
    }
  }
  // we also remove eventual widgets of this module in basics
  _basics_cleanup_list(self, TRUE);
}

static void _manage_editor_module_update_list(dt_lib_module_t *self, dt_lib_modulegroups_group_t *gr)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // first, we remove all existing modules
  dt_gui_container_destroy_children(GTK_CONTAINER(gr->iop_box));

  // and we add the ones from the list
  for(GList *modules2 = g_list_last(darktable.develop->iop); modules2; modules2 = g_list_previous(modules2))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules2->data);
    if((!(module->flags() & IOP_FLAGS_DEPRECATED) || !g_strcmp0(gr->name, C_("modulegroup", "deprecated")))
       && !dt_iop_is_hidden(module) && g_list_find_custom(gr->modules, module->op, _iop_compare))
    {
      // we want to avoid showing multiple instances of the same module
      if(module->multi_priority <= 0
         || g_list_find_custom(darktable.develop->iop, module, _manage_editor_module_find_multi) == NULL)
      {
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_name(hb, "modulegroups-iop-header");
        GtkWidget *lb = gtk_label_new(module->name());
        gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
        gtk_widget_set_name(lb, "iop-panel-label");
        gtk_box_pack_start(GTK_BOX(hb), lb, FALSE, TRUE, 0);
        if(!d->edit_ro)
        {
          GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
          gtk_widget_set_tooltip_text(btn, _("remove this module"));
          g_object_set_data(G_OBJECT(btn), "module_name", module->op);
          g_object_set_data(G_OBJECT(btn), "group", gr);
          g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_module_remove), self);
          gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, TRUE, 0);
        }
        gtk_box_pack_start(GTK_BOX(gr->iop_box), hb, FALSE, TRUE, 0);
      }
    }
  }

  gtk_widget_show_all(gr->iop_box);
}

static void _manage_editor_group_update_arrows(GtkWidget *box)
{
  // we go throw all group columns
  GList *lw = gtk_container_get_children(GTK_CONTAINER(box));
  int pos = 0;
  const int max = g_list_length(lw) - 1;
  for(const GList *lw_iter = lw; lw_iter; lw_iter = g_list_next(lw_iter))
  {
    GtkWidget *w = (GtkWidget *)lw_iter->data;
    GtkWidget *hb = dt_gui_container_nth_child(GTK_CONTAINER(w), 1);
    if(pos > 0 && hb) // we skip the first item as it's quick access panel
    {
      GList *lw2 = gtk_container_get_children(GTK_CONTAINER(hb));
      if(!g_list_shorter_than(lw2, 3)) //do we have at least three?
      {
        GtkWidget *left = (GtkWidget *)lw2->data;
        GtkWidget *right = (GtkWidget *)g_list_nth_data(lw2, 2);
        gtk_widget_set_sensitive(left, pos > 1);
        gtk_widget_set_sensitive(right, pos < max);
      }
      g_list_free(lw2);
    }
    pos++;
  }
  g_list_free(lw);
}

static void _manage_direct_save(dt_lib_module_t *self)
{
  // get all the values
  gchar *params = _preset_to_string(self, FALSE);
  // update the preset in the database
  dt_lib_presets_add(_(CURRENT_PRESET_NAME), self->plugin_name, self->version(), params, strlen(params), FALSE);
  g_free(params);

  // update the preset name
  dt_conf_set_string("plugins/darkroom/modulegroups_preset", _(CURRENT_PRESET_NAME));
  // and we update the gui
  if(!dt_lib_presets_apply(_(CURRENT_PRESET_NAME), self->plugin_name, self->version()))
    dt_lib_presets_apply((gchar *)C_("modulegroup", FALLBACK_PRESET_NAME), self->plugin_name, self->version());
}

static void _manage_direct_module_toggle(GtkWidget *widget, dt_lib_module_t *self)
{
  const gchar *module = (gchar *)g_object_get_data(G_OBJECT(widget), "module_op");
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  if(g_strcmp0(module, "") == 0) return;

  GList *found_item = g_list_find_custom(gr->modules, module, _iop_compare);
  if(!found_item)
  {
    gr->modules = g_list_append(gr->modules, g_strdup(module));
  }
  else
  {
    gr->modules = g_list_delete_link(gr->modules, found_item);
  }

  _manage_direct_save(self);
}

static gint _basics_item_find(gconstpointer a, gconstpointer b)
{
  dt_lib_modulegroups_basic_item_t *ia = (dt_lib_modulegroups_basic_item_t *)a;
  return g_strcmp0(ia->id, (char *)b);
}

static int _lib_modulegroups_basics_module_toggle_action(dt_lib_module_t *self, dt_action_t *action, gboolean doit)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  gchar *action_id = _action_id(action);
  GList *found_item = g_list_find_custom(d->basics, action_id, _basics_item_find);

  if(!doit)
    g_free(action_id);
  else
  {
    _basics_hide(self); // to be sure we put back all widget in their right modules

    if(!found_item)
    {
      dt_lib_modulegroups_basic_item_t *item
          = (dt_lib_modulegroups_basic_item_t *)g_malloc0(sizeof(dt_lib_modulegroups_basic_item_t));
      item->id = action_id;
      _basics_init_item(item);

      d->basics = g_list_append(d->basics, item);
    }
    else
    {
      _basics_free_item(found_item->data);
      d->basics = g_list_delete_link(d->basics, found_item);

      g_free(action_id);
    }

    _manage_direct_save(self);
  }

  return found_item ? -1 : 1;
}

static int _lib_modulegroups_basics_module_toggle(dt_lib_module_t *self, GtkWidget *widget, gboolean doit)
{
  if(GTK_IS_BUTTON(widget)) return 0;

  dt_action_t *action = g_hash_table_lookup(darktable.control->widgets, widget);

  dt_action_t *owner = action;
  while(owner && owner->type >= DT_ACTION_TYPE_SECTION) owner = owner->owner;
  if(!owner || owner->type != DT_ACTION_TYPE_IOP) return 0;

  return _lib_modulegroups_basics_module_toggle_action(self, action, doit);
}

static void _manage_direct_basics_module_toggle(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_action_t *action = g_object_get_data(G_OBJECT(widget), "widget_id");
  if(!action) return;

  _lib_modulegroups_basics_module_toggle_action(self, action, TRUE);
}


static void _manage_editor_basics_add(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  dt_action_t *action = g_object_get_data(G_OBJECT(widget), "widget_id");
  gchar *action_id = _action_id(action);

  if(g_list_find_custom(d->edit_basics, action_id, _basics_item_find))
    g_free(action_id);
  else
  {
    dt_lib_modulegroups_basic_item_t *item
        = (dt_lib_modulegroups_basic_item_t *)g_malloc0(sizeof(dt_lib_modulegroups_basic_item_t));
    item->id = action_id;
    _basics_init_item(item);

    d->edit_basics = g_list_append(d->edit_basics, item);
    _manage_editor_basics_update_list(self);
  }
}

static void _manage_editor_module_add(GtkWidget *widget, dt_lib_module_t *self)
{
  const gchar *module = (gchar *)g_object_get_data(G_OBJECT(widget), "module_op");
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  if(g_strcmp0(module, "") == 0) return;

  if(!g_list_find_custom(gr->modules, module, _iop_compare))
  {
    gr->modules = g_list_append(gr->modules, g_strdup(module));
    _manage_editor_module_update_list(self, gr);
  }
}

static int _manage_editor_module_so_add_sort(gconstpointer a, gconstpointer b)
{
  dt_iop_module_so_t *ma = (dt_iop_module_so_t *)a;
  dt_iop_module_so_t *mb = (dt_iop_module_so_t *)b;
  gchar *s1 = g_utf8_normalize(ma->name(), -1, G_NORMALIZE_ALL);
  gchar *sa = g_utf8_casefold(s1, -1);
  g_free(s1);
  s1 = g_utf8_normalize(mb->name(), -1, G_NORMALIZE_ALL);
  gchar *sb = g_utf8_casefold(s1, -1);
  g_free(s1);
  const int res = g_strcmp0(sa, sb);
  g_free(sa);
  g_free(sb);
  return -res;
}

static void _manage_module_add_popup(GtkWidget *widget, dt_lib_modulegroups_group_t *gr, GCallback callback,
                                     gpointer data, gboolean toggle)
{
  GtkWidget *pop = gtk_menu_new();
  gtk_widget_set_name(pop, "modulegroups-popup");

  int nba = 0; // nb of already present items

  GtkMenu *sm_all = (GtkMenu *)gtk_menu_new();

  GList *m2 = g_list_sort(g_list_copy(darktable.iop), _manage_editor_module_so_add_sort);
  for(const GList *modules = m2; modules; modules = g_list_next(modules))
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);

    if(!dt_iop_so_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      if(!g_list_find_custom(gr->modules, module->op, _iop_compare))
      {
        // does it belong to recommended modules ?
        if(((module->default_group() & IOP_GROUP_BASIC) && g_strcmp0(gr->name, _("base")) == 0)
           || ((module->default_group() & IOP_GROUP_COLOR) && g_strcmp0(gr->name, _("color")) == 0)
           || ((module->default_group() & IOP_GROUP_CORRECT) && g_strcmp0(gr->name, _("correct")) == 0)
           || ((module->default_group() & IOP_GROUP_TONE) && g_strcmp0(gr->name, _("tone")) == 0)
           || ((module->default_group() & IOP_GROUP_EFFECT)
               && g_strcmp0(gr->name, C_("modulegroup", "effect")) == 0)
           || ((module->default_group() & IOP_GROUP_TECHNICAL) && g_strcmp0(gr->name, _("technical")) == 0)
           || ((module->default_group() & IOP_GROUP_GRADING) && g_strcmp0(gr->name, _("grading")) == 0)
           || ((module->default_group() & IOP_GROUP_EFFECTS)
               && g_strcmp0(gr->name, C_("modulegroup", "effects")) == 0))
        {
          GtkMenuItem *smir = (GtkMenuItem *)gtk_menu_item_new_with_label(module->name());
          gtk_widget_set_name(GTK_WIDGET(smir), "modulegroups-popup-item");
          gtk_widget_set_tooltip_text(GTK_WIDGET(smir), _("add this module"));
          g_object_set_data(G_OBJECT(smir), "module_op", module->op);
          g_object_set_data(G_OBJECT(smir), "group", gr);
          g_signal_connect(G_OBJECT(smir), "activate", callback, data);
          gtk_menu_shell_insert(GTK_MENU_SHELL(pop), GTK_WIDGET(smir), nba);
        }
        GtkMenuItem *smi = (GtkMenuItem *)gtk_menu_item_new_with_label(module->name());
        gtk_widget_set_name(GTK_WIDGET(smi), "modulegroups-popup-item2");
        gtk_widget_set_tooltip_text(GTK_WIDGET(smi), _("add this module"));
        g_object_set_data(G_OBJECT(smi), "module_op", module->op);
        g_object_set_data(G_OBJECT(smi), "group", gr);
        g_signal_connect(G_OBJECT(smi), "activate", callback, data);
        gtk_menu_shell_prepend(GTK_MENU_SHELL(sm_all), GTK_WIDGET(smi));
      }
      else if(toggle)
      {
        GtkMenuItem *smi = (GtkMenuItem *)gtk_menu_item_new_with_label(module->name());
        gtk_widget_set_name(GTK_WIDGET(smi), "modulegroups-popup-item");
        gtk_widget_set_tooltip_text(GTK_WIDGET(smi), _("remove this module"));
        g_object_set_data(G_OBJECT(smi), "module_op", module->op);
        g_object_set_data(G_OBJECT(smi), "group", gr);
        g_signal_connect(G_OBJECT(smi), "activate", callback, data);
        gtk_menu_shell_insert(GTK_MENU_SHELL(pop), GTK_WIDGET(smi), 0);
        nba++;
      }
    }
  }
  g_list_free(m2);

  // show the submenu with all the modules
  GtkWidget *smt = gtk_menu_item_new_with_label(_("all available modules"));
  gtk_widget_set_name(smt, "modulegroups-popup-item-all");
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(smt), GTK_WIDGET(sm_all));
  gtk_menu_shell_append(GTK_MENU_SHELL(pop), smt);

  // show the add/remove sections titles if needed
  if(toggle && nba > 0)
  {
    smt = gtk_menu_item_new_with_label(_("add module"));
    gtk_widget_set_name(smt, "modulegroups-popup-title");
    gtk_widget_set_sensitive(smt, FALSE);
    gtk_menu_shell_insert(GTK_MENU_SHELL(pop), smt, nba);

    smt = gtk_menu_item_new_with_label(_("remove module"));
    gtk_widget_set_name(smt, "modulegroups-popup-title");
    gtk_widget_set_sensitive(smt, FALSE);
    gtk_menu_shell_prepend(GTK_MENU_SHELL(pop), smt);
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static gchar *_action_label(dt_action_t *action)
{
  if(action->type != DT_ACTION_TYPE_IOP && action->owner)
  {
    gchar *owner_id = _action_label(action->owner);
    gchar *combined_id = g_strdup_printf("%s - %s", owner_id, action->label);
    g_free(owner_id);
    return combined_id;
  }
  else
    return g_strdup(action->label);
}

static GtkWidget *_build_menu_from_actions(dt_action_t *actions, dt_lib_module_t *self, GtkWidget *on_off,
                                           GtkWidget *base_menu, gboolean full_menu, int *num_selected)
{
  GCallback callback = G_CALLBACK(full_menu ? _manage_direct_basics_module_toggle : _manage_editor_basics_add);

  GtkWidget *new_base = NULL;
  while(actions)
  {
    if(actions == &darktable.control->actions_focus ||
       actions == &darktable.control->actions_blend)
    {
        actions = actions->next;
        continue;
    }

    if(actions->type == DT_ACTION_TYPE_IOP)
    {
      dt_iop_module_so_t *so = (dt_iop_module_so_t *)actions;
      if(dt_iop_so_is_hidden(so) || so->flags() & IOP_FLAGS_DEPRECATED)
      {
        actions = actions->next;
        continue;
      }
    }

    gchar *action_label = NULL;
    GtkWidget *item = NULL, *new_sub = NULL;
    dt_action_t *action = NULL;

    if(actions->type >= DT_ACTION_TYPE_SECTION && !on_off) // not an iop module or blending (CATEGORY)
    {
      // FIXME don't check here if on/off is enabled, because it depends on image (reload_defaults)
      // respond later to image changed signal
      on_off = item = gtk_check_menu_item_new_with_label(_("on-off"));
      action = actions->owner;
      action_label = g_strdup_printf("%s - %s", actions->owner->label, _("on-off"));

      // in next loop deal with first actual widget or section
    }
    else
    {
      if(actions->type <= DT_ACTION_TYPE_SECTION)
        new_sub = _build_menu_from_actions(actions->target, self, on_off, base_menu, full_menu, num_selected);

      if(new_sub || (actions->type >= DT_ACTION_TYPE_WIDGET && actions->target && !GTK_IS_BUTTON(actions->target)))
      {
        item = new_sub ? gtk_menu_item_new_with_label(actions->label)
                       : gtk_check_menu_item_new_with_label(actions->label);
        action = actions;
        action_label = _action_label(actions);
      }

      actions = actions->next;
    }

    if(item)
    {
      gtk_widget_set_name(item, "modulegroups-popup-item2");

      if(!new_base)
        new_base = gtk_menu_new();
      gtk_menu_shell_append(GTK_MENU_SHELL(new_base), item);

      if(new_sub)
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), new_sub);
      else
      {
        dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

        GtkWidget *item_top = NULL;

        gchar *action_id = _action_id(action);
        if(g_list_find_custom(full_menu ? d->basics : d->edit_basics, action_id, _basics_item_find))
        {
          gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
          if(!full_menu)
            gtk_widget_set_sensitive(item, FALSE);
          else
            gtk_widget_set_tooltip_text(item, _("remove this widget"));

          const gboolean compact_ui = !dt_conf_get_bool("plugins/darkroom/modulegroups_basics_sections_labels");
          if(!compact_ui && item != on_off)
          {
            gtk_check_menu_item_set_inconsistent(GTK_CHECK_MENU_ITEM(on_off), TRUE);
            gtk_widget_set_tooltip_text(on_off, _("header needed for other widgets"));
          }

          if(full_menu)
          {
            item_top = gtk_menu_item_new_with_label(action_label);;
            gtk_widget_set_tooltip_text(item_top, _("remove this widget"));
            gtk_widget_set_name(item_top, "modulegroups-popup-item");
            g_object_set_data(G_OBJECT(item_top), "widget_id", action);
            g_signal_connect(G_OBJECT(item_top), "activate", callback, self);
            gtk_menu_shell_insert(GTK_MENU_SHELL(base_menu), item_top, *num_selected);
            ++*num_selected;
          }
        }
        else
        {
          gtk_widget_set_tooltip_text(item, _("add this widget"));

          gchar *delimited_id = g_strdup_printf("|%s|", action_id);

          if(strstr(RECOMMENDED_BASICS, delimited_id))
          {
            item_top = gtk_menu_item_new_with_label(action_label);;
            gtk_widget_set_tooltip_text(item_top, _("add this widget"));
            gtk_widget_set_name(item_top, "modulegroups-popup-item");
            g_object_set_data(G_OBJECT(item_top), "widget_id", action);
            g_signal_connect(G_OBJECT(item_top), "activate", callback, self);
            gtk_menu_shell_append(GTK_MENU_SHELL(base_menu), item_top);
          }
          g_free(delimited_id);
        }

        if(item != on_off && dt_action_widget_invisible(action->target))
        {
          gtk_check_menu_item_set_inconsistent(GTK_CHECK_MENU_ITEM(item), TRUE);
          gchar *toolmark = gtk_widget_get_tooltip_text(item);
          toolmark = dt_util_dstrcat(toolmark, " <i>(%s)</i>", _("currently invisible"));
          gtk_widget_set_tooltip_markup(item, toolmark);
          if(item_top)
            gtk_widget_set_tooltip_markup(item_top, toolmark);
          g_free(toolmark);
        }

        g_object_set_data(G_OBJECT(item), "widget_id", action);
        g_signal_connect(G_OBJECT(item), "activate", callback, self);
        g_free(action_id);
      }
      g_free(action_label);
    }
  }

  return new_base;
}

static void _manage_basics_add_popup(GtkWidget *widget, dt_lib_module_t *self, gboolean full_menu)
{
  int nba = 0; // nb of already present items
  GtkWidget *pop = gtk_menu_new();
  gtk_widget_set_name(pop, "modulegroups-popup");

  GtkWidget *all_modules = _build_menu_from_actions(darktable.control->actions_iops.target, self, NULL, pop, full_menu, &nba);

  // show the add/remove sections titles if needed
  if(full_menu && nba > 0)
  {
    GtkWidget *smt = gtk_menu_item_new_with_label(_("add widget"));
    gtk_widget_set_name(smt, "modulegroups-popup-title");
    gtk_widget_set_sensitive(smt, FALSE);
    gtk_menu_shell_insert(GTK_MENU_SHELL(pop), smt, nba);

    smt = gtk_menu_item_new_with_label(_("remove widget"));
    gtk_widget_set_name(smt, "modulegroups-popup-title");
    gtk_widget_set_sensitive(smt, FALSE);
    gtk_menu_shell_prepend(GTK_MENU_SHELL(pop), smt);
  }

  GList *children = gtk_container_get_children(GTK_CONTAINER(pop));
  if(children)
  {
    g_list_free(children);

    GtkWidget *smt = gtk_menu_item_new_with_label(_("all available modules"));
    gtk_widget_set_name(smt, "modulegroups-popup-item-all");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(smt), GTK_WIDGET(all_modules));
    gtk_menu_shell_append(GTK_MENU_SHELL(pop), smt);
  }
  else
  {
    gtk_widget_destroy(pop);
    pop = all_modules;
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static void _manage_editor_basics_add_popup(GtkWidget *widget, GdkEvent *event, dt_lib_module_t *self)
{
  _manage_basics_add_popup(widget, self, FALSE);
}

static void _manage_editor_module_add_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  _manage_module_add_popup(widget, gr, G_CALLBACK(_manage_editor_module_add), self, FALSE);
}

static gboolean _presets_pressed(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    manage_presets(self);
    return TRUE;
  }
  return FALSE;
}

static gboolean _manage_direct_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
    if(!g_strcmp0(gr->name, C_("modulegroup", "deprecated"))) return FALSE;
    _manage_module_add_popup(widget, gr, G_CALLBACK(_manage_direct_module_toggle), self, TRUE);
    return TRUE;
  }
  return FALSE;
}

static gboolean _manage_direct_basic_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    _manage_basics_add_popup(widget, self, TRUE);
    return TRUE;
  }
  return FALSE;
}

static gboolean _manage_direct_module_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_action_t *module = g_object_get_data(G_OBJECT(widget), "module");

  if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    int nba = 0; // nb of already present items
    GtkWidget *pop = gtk_menu_new();
    gtk_widget_set_name(pop, "modulegroups-popup");

    GtkWidget *this_module = _build_menu_from_actions(module->target, self, NULL, pop, TRUE, &nba);

    dt_gui_menu_popup(GTK_MENU(this_module), NULL, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);

    return TRUE;
  }
  return FALSE;
}

static void _manage_direct_full_active_toggled(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  d->full_active = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget));
  const int cur = d->current;
  _manage_direct_save(self);
  d->current = cur;
  _lib_modulegroups_update_iop_visibility(self);
}

static gboolean _manage_direct_active_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
    GtkWidget *pop = gtk_menu_new();
    gtk_widget_set_name(pop, "modulegroups-popup");

    GtkWidget *smt = gtk_check_menu_item_new_with_label(_("show all history modules"));
    gtk_widget_set_tooltip_text(
        smt,
        _("show modules that are present in the history stack, regardless of whether or not they are currently enabled"));
    gtk_widget_set_name(smt, "modulegroups-popup-item");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(smt), d->full_active);
    g_signal_connect(G_OBJECT(smt), "toggled", G_CALLBACK(_manage_direct_full_active_toggled), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(pop), smt);

    dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
    return TRUE;
  }
  return FALSE;
}

static void _dt_dev_image_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_develop_t *dev = darktable.develop;
  if(!dev || dev->image_storage.id <= 0) return;

  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, dev->image_storage.id, 'r');
  if(!image) return;

  char query[1024];
  // clang-format off
  snprintf(query, sizeof(query),
           "SELECT name"
           " FROM data.presets"
           " WHERE operation='modulegroups'"
           "       AND op_version=?1"
           "       AND autoapply=1"
           "       AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
           "       AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
           "       AND ?8 BETWEEN exposure_min AND exposure_max"
           "       AND ?9 BETWEEN aperture_min AND aperture_max"
           "       AND ?10 BETWEEN focal_length_min AND focal_length_max"
           "       AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))"
           " ORDER BY writeprotect DESC, name DESC"
           " LIMIT 1");
  // clang-format on

  int iformat = 0;
  if(dt_image_is_rawprepare_supported(image))
    iformat |= FOR_RAW;
  else
    iformat |= FOR_LDR;
  if(dt_image_is_hdr(image)) iformat |= FOR_HDR;

  int excluded = 0;
  if(dt_image_monochrome_flags(image))
    excluded |= FOR_NOT_MONO;
  else
    excluded |= FOR_NOT_COLOR;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, self->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);

  dt_image_cache_read_release(darktable.image_cache, image);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *preset = (char *)sqlite3_column_text(stmt, 0);
    dt_lib_presets_apply(preset, self->plugin_name, self->version());
  }
  sqlite3_finalize(stmt);

  // check for missing camera samples
  if(image->camera_missing_sample)
  {
    gchar *label = dt_image_camera_missing_sample_message(image, FALSE);
    d->force_deprecated_message = TRUE;
    gtk_label_set_markup(GTK_LABEL(d->deprecated), label);
    g_free(label);
    gtk_widget_set_visible(d->deprecated, TRUE);
  }
  else
  {
    d->force_deprecated_message = FALSE;
    gtk_label_set_markup
      (GTK_LABEL(d->deprecated),
       _("the following modules are deprecated because they have internal design mistakes"
         " that can't be corrected and alternative modules that correct them.\n"
         "they will be removed for new edits in the next release."));
  }

}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)g_malloc0(sizeof(dt_lib_modulegroups_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "modules-tabs");
  dt_gui_add_class(self->widget, "dt_big_btn_canvas");

  d->hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->hbox_search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  // groups
  d->hbox_groups = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(d->hbox_buttons), d->hbox_groups, TRUE, TRUE, 0);

  // basic group button
  d->basic_btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_basics, 0, NULL);
  g_signal_connect(d->basic_btn, "button-press-event", G_CALLBACK(_manage_direct_basic_popup), self);
  g_signal_connect(d->basic_btn, "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->basic_btn, _("quick access panel"));
  dt_action_define(DT_ACTION(self), NULL, N_("quick access panel"), d->basic_btn, &dt_action_def_toggle);
  gtk_box_pack_start(GTK_BOX(d->hbox_groups), d->basic_btn, TRUE, TRUE, 0);

  d->vbox_basic = NULL;
  d->basics = NULL;

  // active group button
  d->active_btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_active, 0, NULL);
  g_signal_connect(d->active_btn, "button-press-event", G_CALLBACK(_manage_direct_active_popup), self);
  g_signal_connect(d->active_btn, "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->active_btn, _("show only active modules"));
  dt_action_define(DT_ACTION(self), NULL, N_("active modules"), d->active_btn, &dt_action_def_toggle);
  gtk_box_pack_start(GTK_BOX(d->hbox_groups), d->active_btn, TRUE, TRUE, 0);

  // we load now the presets btn
  self->presets_button = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  gtk_widget_set_tooltip_text(self->presets_button, _("presets\nctrl+click to manage"));
  gtk_box_pack_start(GTK_BOX(d->hbox_buttons), self->presets_button, FALSE, FALSE, 0);
  g_signal_connect(self->presets_button, "button-press-event", G_CALLBACK(_presets_pressed), self);

  /* search box */
  d->text_entry = gtk_search_entry_new();
  dt_action_define(&darktable.view_manager->proxy.darkroom.view->actions, NULL, N_("search modules"), d->text_entry, &dt_action_def_entry);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->text_entry), _("search modules by name or tag"));
  g_signal_connect(G_OBJECT(d->text_entry), "search-changed", G_CALLBACK(_text_entry_changed_callback), self);
  g_signal_connect(G_OBJECT(d->text_entry), "stop-search", G_CALLBACK(dt_gui_search_stop), dt_ui_center(darktable.gui->ui));
  g_signal_connect_data(G_OBJECT(d->text_entry), "focus-in-event", G_CALLBACK(gtk_widget_show), d->hbox_search_box, NULL, G_CONNECT_AFTER | G_CONNECT_SWAPPED);

  GtkWidget *visibility_wrapper = gtk_event_box_new(); // extra layer prevents disabling shortcuts when hidden
  gtk_container_add(GTK_CONTAINER(visibility_wrapper), d->text_entry);
  gtk_box_pack_start(GTK_BOX(d->hbox_search_box), visibility_wrapper, TRUE, TRUE, 0);
  gtk_entry_set_width_chars(GTK_ENTRY(d->text_entry), 0);
  gtk_entry_set_max_width_chars(GTK_ENTRY(d->text_entry), 35);
  gtk_entry_set_icon_tooltip_text(GTK_ENTRY(d->text_entry), GTK_ENTRY_ICON_SECONDARY, _("clear text"));

  gtk_box_pack_start(GTK_BOX(self->widget), d->hbox_buttons, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->hbox_search_box, TRUE, TRUE, 0);

  // deprecated message
  d->deprecated
      = gtk_label_new(_("the following modules are deprecated because they have internal design mistakes"
                        " that can't be corrected and alternative modules that correct them.\n"
                        "they will be removed for new edits in the next release."));
  dt_gui_add_class(d->deprecated, "dt_warning");
  gtk_label_set_line_wrap(GTK_LABEL(d->deprecated), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->deprecated, TRUE, TRUE, 0);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->active_btn), TRUE);
  d->current = dt_conf_get_int("plugins/darkroom/groups");
  if(d->current == DT_MODULEGROUP_NONE) _lib_modulegroups_update_iop_visibility(self);
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(d->hbox_buttons, TRUE);
  gtk_widget_set_no_show_all(d->hbox_search_box, TRUE);

  /*
   * set the proxy functions
   */
  darktable.develop->proxy.modulegroups.module = self;
  darktable.develop->proxy.modulegroups.set = _lib_modulegroups_set;
  darktable.develop->proxy.modulegroups.update_visibility = _lib_modulegroups_update_visibility_proxy;
  darktable.develop->proxy.modulegroups.get = _lib_modulegroups_get;
  darktable.develop->proxy.modulegroups.get_activated = _lib_modulegroups_get_activated;
  darktable.develop->proxy.modulegroups.test = _lib_modulegroups_test;
  darktable.develop->proxy.modulegroups.switch_group = _lib_modulegroups_switch_group;
  darktable.develop->proxy.modulegroups.test_visible = _lib_modulegroups_test_visible;
  darktable.develop->proxy.modulegroups.basics_module_toggle = _lib_modulegroups_basics_module_toggle;

  // check for autoapplypresets on image change
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_dt_dev_image_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_dt_dev_image_changed_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_dev_image_changed_callback), self);

  darktable.develop->proxy.modulegroups.module = NULL;
  darktable.develop->proxy.modulegroups.set = NULL;
  darktable.develop->proxy.modulegroups.get = NULL;
  darktable.develop->proxy.modulegroups.get_activated = NULL;
  darktable.develop->proxy.modulegroups.test = NULL;
  darktable.develop->proxy.modulegroups.switch_group = NULL;

  g_free(self->data);
  self->data = NULL;
}

static void _buttons_update(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // ensure we have no remaining force shown modules
  d->force_show_module = NULL;

  // first, we destroy all existing buttons except active one an preset one
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->hbox_groups));
  GList *l = children;
  if(l) l = g_list_next(l); // skip basics group
  if(l) l = g_list_next(l); // skip active group
  for(; l; l = g_list_next(l))
  {
    GtkWidget *bt = (GtkWidget *)l->data;
    gtk_widget_destroy(bt);
  }
  g_list_free(children);
  gtk_widget_set_visible(d->basic_btn, d->basics_show);

  // if there's no groups, we ensure that the preset button is on the search line and we hide the active button
  gtk_widget_set_visible(d->hbox_search_box, d->show_search);
  if(!d->groups && d->show_search)
  {
    if(gtk_widget_get_parent(self->presets_button) != d->hbox_search_box)
    {
      g_object_ref(self->presets_button);
      gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(self->presets_button)), self->presets_button);
      gtk_box_pack_start(GTK_BOX(d->hbox_search_box), self->presets_button, FALSE, FALSE, 0);
      g_object_unref(self->presets_button);
    }
    gtk_widget_hide(d->hbox_buttons);
    d->current = DT_MODULEGROUP_ACTIVE_PIPE;
    _lib_modulegroups_update_iop_visibility(self);
    return;
  }
  else
  {
    if(gtk_widget_get_parent(self->presets_button) != d->hbox_buttons)
    {
      g_object_ref(self->presets_button);
      gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(self->presets_button)), self->presets_button);
      gtk_box_pack_start(GTK_BOX(d->hbox_buttons), self->presets_button, FALSE, FALSE, 0);
      g_object_unref(self->presets_button);
    }
    gtk_widget_show(d->hbox_buttons);
    gtk_widget_show(d->hbox_groups);
  }

  // then we repopulate the box with new buttons
  for(l = d->groups; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    GtkWidget *bt = dtgtk_togglebutton_new(_buttons_get_icon_fct(gr->icon), 0, NULL);
    g_object_set_data(G_OBJECT(bt), "group", gr);
    g_signal_connect(bt, "button-press-event", G_CALLBACK(_manage_direct_popup), self);
    g_signal_connect(bt, "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
    gtk_widget_set_tooltip_text(bt, gr->name);
    gr->button = bt;
    gtk_box_pack_start(GTK_BOX(d->hbox_groups), bt, TRUE, TRUE, 0);
    gtk_widget_show(bt);
  }

  // last, if d->current still valid, we select it otherwise the first one
  int cur = d->current;
  d->current = DT_MODULEGROUP_NONE;
  if(cur > g_list_length(d->groups) && cur != DT_MODULEGROUP_BASICS) cur = DT_MODULEGROUP_ACTIVE_PIPE;
  if(cur == DT_MODULEGROUP_BASICS && !d->basics_show) cur = DT_MODULEGROUP_ACTIVE_PIPE;
  if(cur == DT_MODULEGROUP_ACTIVE_PIPE)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->active_btn), TRUE);
  else if(cur == DT_MODULEGROUP_BASICS)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->basic_btn)))
    {
      // we need to manually refresh the list
      d->current = DT_MODULEGROUP_BASICS;
      _lib_modulegroups_update_iop_visibility(self);
    }
    else
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->basic_btn), TRUE);
  }
  else
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_list_nth_data(d->groups, cur - 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gr->button), TRUE);
  }
}

static void _manage_editor_group_move_right(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  GtkWidget *vb = gtk_widget_get_parent(gtk_widget_get_parent(widget));

  // we move the group inside the list
  const int pos = g_list_index(d->edit_groups, gr);
  if(pos < 0 || pos >= g_list_length(d->edit_groups) - 1) return;
  d->edit_groups = g_list_remove(d->edit_groups, gr);
  d->edit_groups = g_list_insert(d->edit_groups, gr, pos + 1);

  // we move the group in the ui (the position need +1 due to the quick access panel)
  gtk_box_reorder_child(GTK_BOX(gtk_widget_get_parent(vb)), vb, pos + 2);
  // and we update arrows
  _manage_editor_group_update_arrows(gtk_widget_get_parent(vb));
}

static void _manage_editor_group_move_left(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  GtkWidget *vb = gtk_widget_get_parent(gtk_widget_get_parent(widget));

  // we move the group inside the list
  const int pos = g_list_index(d->edit_groups, gr);
  if(pos <= 0) return;
  d->edit_groups = g_list_remove(d->edit_groups, gr);
  d->edit_groups = g_list_insert(d->edit_groups, gr, pos - 1);

  // we move the group in the ui (the position need +1 due to the quick access panel)
  gtk_box_reorder_child(GTK_BOX(gtk_widget_get_parent(vb)), vb, pos);
  // and we update arrows
  _manage_editor_group_update_arrows(gtk_widget_get_parent(vb));
}

static void _manage_editor_group_remove(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  // we don't allow to remove the last group if no quick access or searchbox
  if(g_list_length(d->edit_groups) == 1 && !d->edit_basics_show && !d->edit_show_search)
  {
    return;
  }
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  GtkWidget *vb = gtk_widget_get_parent(gtk_widget_get_parent(gtk_widget_get_parent(widget)));
  GtkWidget *groups_box = gtk_widget_get_parent(vb);

  // we remove the group from the list and destroy it
  d->edit_groups = g_list_remove(d->edit_groups, gr);
  g_free(gr->name);
  g_free(gr->icon);
  g_list_free_full(gr->modules, g_free);
  g_free(gr);

  // we remove the group from the ui
  gtk_widget_destroy(vb);

  // and we update arrows
  _manage_editor_group_update_arrows(groups_box);

  // we also cleanup basics widgets list
  _basics_cleanup_list(self, TRUE);
}

static void _manage_editor_group_name_changed(GtkWidget *tb, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(tb), "group");
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(tb));
  g_free(gr->name);
  gr->name = g_strdup(txt);
}

static void _manage_editor_group_icon_changed(GtkWidget *widget, GdkEventButton *event,
                                              dt_lib_modulegroups_group_t *gr)
{
  const char *ic = (char *)g_object_get_data(G_OBJECT(widget), "ic_name");
  g_free(gr->icon);
  gr->icon = g_strdup(ic);
  GtkWidget *pop = gtk_widget_get_parent(gtk_widget_get_parent(widget));
  GtkWidget *btn = gtk_popover_get_relative_to(GTK_POPOVER(pop));
  dtgtk_button_set_paint(DTGTK_BUTTON(btn), _buttons_get_icon_fct(ic), 0, NULL);
  gtk_popover_popdown(GTK_POPOVER(pop));
}

static void _manage_editor_group_icon_popup(GtkWidget *btn, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(btn), "group");

  GtkWidget *pop = gtk_popover_new(btn);
  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(pop, "modulegroups-icons-popup");

  GtkWidget *eb, *hb, *ic;
  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_basic, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("basic icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "basic");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_active, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("active icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "active");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_color, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("color icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "color");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_correct, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("correct icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "correct");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_effect, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("effect icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "effect");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_favorites, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("favorites icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "favorites");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_tone, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("tone icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "tone");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_grading, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("grading icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "grading");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_technical, 0, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("technical icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "technical");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(pop), vb);
  gtk_widget_show_all(pop);
}

static GtkWidget *_manage_editor_group_init_basics_box(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  GtkWidget *vb2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(vb2, "modulegroups-groupbox");
  // line to edit the group
  GtkWidget *hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb2, "modulegroups-header");

  GtkWidget *btn = NULL;

  GtkWidget *hb3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb3, "modulegroups-header-center");
  gtk_widget_set_hexpand(hb3, TRUE);

  btn = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_basics, 0, NULL);
  gtk_widget_set_name(btn, "modulegroups-group-icon");
  gtk_widget_set_sensitive(btn, FALSE);
  gtk_box_pack_start(GTK_BOX(hb3), btn, FALSE, TRUE, 0);

  GtkWidget *tb = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(tb), 5);
  gtk_widget_set_tooltip_text(tb, _("quick access panel widgets"));
  gtk_widget_set_sensitive(tb, FALSE);
  gtk_entry_set_text(GTK_ENTRY(tb), _("quick access"));
  gtk_box_pack_start(GTK_BOX(hb3), tb, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(hb2), hb3, FALSE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vb2), hb2, FALSE, TRUE, 0);

  // chosen widgets
  GtkWidget *vb3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  d->edit_basics_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  _manage_editor_basics_update_list(self);
  gtk_box_pack_start(GTK_BOX(vb3), d->edit_basics_box, FALSE, TRUE, 0);

  // '+' button to add new widgets
  if(!d->edit_ro)
  {
    GtkWidget *hb4 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *bt = dtgtk_button_new(dtgtk_cairo_paint_square_plus, CPF_DIRECTION_LEFT, NULL);
    gtk_widget_set_tooltip_text(bt, _("add widget to the quick access panel"));
    gtk_widget_set_name(bt, "modulegroups-btn");
    g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_basics_add_popup), self);
    gtk_widget_set_halign(hb4, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(hb4), bt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb2), hb4, FALSE, FALSE, 0);
  }

  gtk_container_add(GTK_CONTAINER(sw), vb3);
  gtk_box_pack_start(GTK_BOX(vb2), sw, TRUE, TRUE, 0);

  return vb2;
}

static GtkWidget *_manage_editor_group_init_modules_box(dt_lib_module_t *self, dt_lib_modulegroups_group_t *gr)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  GtkWidget *vb2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(vb2, "modulegroups-groupbox");
  // line to edit the group
  GtkWidget *hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb2, "modulegroups-header");

  GtkWidget *btn = NULL;
  GtkWidget *hb3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb3, "modulegroups-header-center");
  gtk_widget_set_hexpand(hb3, TRUE);

  // icon
  btn = dtgtk_button_new(_buttons_get_icon_fct(gr->icon), 0, NULL);
  gtk_widget_set_name(btn, "modulegroups-group-icon");
  gtk_widget_set_tooltip_text(btn, _("group icon"));
  gtk_widget_set_sensitive(btn, !d->edit_ro);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_icon_popup), self);
  g_object_set_data(G_OBJECT(btn), "group", gr);
  gtk_box_pack_start(GTK_BOX(hb3), btn, FALSE, TRUE, 0);

  // entry for group name
  GtkWidget *tb = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(tb), 5);
  gtk_widget_set_tooltip_text(tb, _("group name"));
  g_object_set_data(G_OBJECT(tb), "group", gr);
  gtk_widget_set_sensitive(tb, !d->edit_ro);
  g_signal_connect(G_OBJECT(tb), "changed", G_CALLBACK(_manage_editor_group_name_changed), self);
  gtk_entry_set_text(GTK_ENTRY(tb), gr->name);
  gtk_box_pack_start(GTK_BOX(hb3), tb, TRUE, TRUE, 0);

  // remove button
  if(!d->edit_ro)
  {
    btn = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
    gtk_widget_set_tooltip_text(btn, _("remove group"));
    g_object_set_data(G_OBJECT(btn), "group", gr);
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_remove), self);
    gtk_box_pack_end(GTK_BOX(hb3), btn, FALSE, TRUE, 0);
  }

  gtk_box_pack_start(GTK_BOX(hb2), hb3, FALSE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vb2), hb2, FALSE, TRUE, 0);

  // chosen modules
  GtkWidget *vb3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gr->iop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  _manage_editor_module_update_list(self, gr);
  gtk_box_pack_start(GTK_BOX(vb3), gr->iop_box, FALSE, TRUE, 0);

  // '+' button to add new module
  if(!d->edit_ro)
  {
    GtkWidget *hb4 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    // left arrow
    btn = dtgtk_button_new(dtgtk_cairo_paint_line_arrow, CPF_DIRECTION_RIGHT, NULL);
    gtk_widget_set_name(btn, "modulegroups-btn");
    gtk_widget_set_tooltip_text(btn, _("move group to the left"));
    g_object_set_data(G_OBJECT(btn), "group", gr);
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_move_left), self);
    gtk_box_pack_start(GTK_BOX(hb4), btn, FALSE, FALSE, 2);

    // plus button
    GtkWidget *plusbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *bt = dtgtk_button_new(dtgtk_cairo_paint_square_plus, CPF_DIRECTION_LEFT, NULL);
    gtk_widget_set_tooltip_text(bt, _("add module to the group"));
    gtk_widget_set_name(bt, "modulegroups-btn");
    g_object_set_data(G_OBJECT(bt), "group", gr);
    g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_module_add_popup), self);
    gtk_widget_set_halign(plusbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(plusbox), bt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb4), plusbox, TRUE, TRUE, 0);

    //right arrow
    btn = dtgtk_button_new(dtgtk_cairo_paint_line_arrow, CPF_DIRECTION_LEFT, NULL);
    gtk_widget_set_name(btn, "modulegroups-btn");
    gtk_widget_set_tooltip_text(btn, _("move group to the right"));
    g_object_set_data(G_OBJECT(btn), "group", gr);
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_move_right), self);
    gtk_box_pack_end(GTK_BOX(hb4), btn, FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(vb2), hb4, FALSE, FALSE, 0);
  }

  gtk_container_add(GTK_CONTAINER(sw), vb3);
  gtk_box_pack_start(GTK_BOX(vb2), sw, TRUE, TRUE, 0);

  return vb2;
}

static void _manage_editor_reset(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  gchar *txt = g_strdup(d->edit_preset);
  _manage_editor_load(txt, self);
  g_free(txt);
}

static void _manage_editor_group_add(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_malloc0(sizeof(dt_lib_modulegroups_group_t));
  gr->name = g_strdup(_("new"));
  gr->icon = g_strdup("basic");
  d->edit_groups = g_list_append(d->edit_groups, gr);

  // we update the group list
  GtkWidget *vb2 = _manage_editor_group_init_modules_box(self, gr);
  gtk_box_pack_start(GTK_BOX(d->preset_groups_box), vb2, FALSE, TRUE, 0);
  gtk_widget_show_all(vb2);

  // and we update arrows
  _manage_editor_group_update_arrows(d->preset_groups_box);
}

static void _manage_editor_basics_toggle(GtkWidget *button, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(d->editor_reset) return;
  const gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  // we don't allow that to be false if there's no group or search
  if(!state && g_list_length(d->edit_groups) == 0 && !d->edit_show_search)
  {
    d->editor_reset = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
    d->editor_reset = FALSE;
  }
  d->edit_basics_show = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  gtk_widget_set_visible(d->edit_basics_groupbox, d->edit_basics_show);
}

static void _manage_editor_search_toggle(GtkWidget *button, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(d->editor_reset) return;
  const gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  // we don't allow that to be false if there's no group or quick access
  if(!state && g_list_length(d->edit_groups) == 0 && !d->edit_basics_show)
  {
    d->editor_reset = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
    d->editor_reset = FALSE;
  }
  d->edit_show_search = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
}

static void _manage_editor_full_active_toggle(GtkWidget *button, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(d->editor_reset) return;

  d->edit_full_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
}

static void _preset_autoapply_changed(dt_gui_presets_edit_dialog_t *g)
{
  dt_lib_module_t *self = g->data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we reread the presets autoapply values from the database
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT autoapply"
                              " FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, d->edit_preset, -1, SQLITE_TRANSIENT);

  int autoapply = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    autoapply = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    return;
  }

  // we refresh the checkbox
  d->editor_reset = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->edit_autoapply_chkbox), autoapply);
  d->editor_reset = FALSE;
}

static void _manage_editor_preset_name_verify(GtkWidget *tb, dt_lib_module_t *self)
{
  GList *names = (GList *)g_object_get_data(G_OBJECT(tb), "existing_names");
  GtkWidget *lb = (GtkWidget *)g_object_get_data(G_OBJECT(tb), "existing_label");
  GtkWidget *bt_ok = (GtkWidget *)g_object_get_data(G_OBJECT(tb), "ok_btn");
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(tb));

  // we don't want empty name
  if(!g_strcmp0(txt, ""))
  {
    gtk_widget_set_visible(lb, TRUE);
    gtk_widget_set_sensitive(bt_ok, FALSE);
    return;
  }

  for(const GList *l = names; l; l = g_list_next(l))
  {
    gchar *tx = (gchar *)l->data;
    if(!g_strcmp0(tx, txt))
    {
      gtk_widget_set_visible(lb, TRUE);
      gtk_widget_set_sensitive(bt_ok, FALSE);
      return;
    }
  }
  gtk_widget_set_visible(lb, FALSE);
  gtk_widget_set_sensitive(bt_ok, TRUE);
}

static void _manage_editor_preset_action(GtkWidget *btn, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we get the default name
  gchar *new_name = NULL;
  if(btn == d->presets_btn_rename)
    new_name = g_strdup(d->edit_preset);
  else if(btn == d->presets_btn_new)
    new_name = g_strdup(_("new"));
  else if(btn == d->presets_btn_dup)
    new_name = g_strdup_printf("%s_1", d->edit_preset);
  else
    return;

  // we first get the list of all the existing preset names
  GList *names = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name"
                              " FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    names = g_list_prepend(names, g_strdup(name));
  }
  sqlite3_finalize(stmt);

  gint res = GTK_RESPONSE_OK;
  GtkWidget *dialog
      = gtk_dialog_new_with_buttons(_("rename preset"), GTK_WINDOW(d->dialog), GTK_DIALOG_DESTROY_WITH_PARENT,
                                    _("cancel"), GTK_RESPONSE_CANCEL, _("rename"), GTK_RESPONSE_OK, NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  GtkWidget *bt_ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(content_area), gtk_label_new(_("new preset name:")), FALSE, TRUE, 0);
  GtkWidget *lb = gtk_label_new(_("a preset with this name already exists!"));
  GtkWidget *tb = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(tb), new_name);
  g_object_set_data(G_OBJECT(tb), "existing_names", names);
  g_object_set_data(G_OBJECT(tb), "existing_label", lb);
  g_object_set_data(G_OBJECT(tb), "ok_btn", bt_ok);
  g_signal_connect(G_OBJECT(tb), "changed", G_CALLBACK(_manage_editor_preset_name_verify), self);
  gtk_box_pack_start(GTK_BOX(content_area), tb, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(content_area), lb, FALSE, TRUE, 0);
  gtk_widget_show_all(content_area);
  _manage_editor_preset_name_verify(tb, self);
  res = gtk_dialog_run(GTK_DIALOG(dialog));

  g_free(new_name);

  if(res == GTK_RESPONSE_OK)
  {
    if(btn == d->presets_btn_rename)
    {
      // we update the database
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE data.presets"
                                  " SET name=?1"
                                  " WHERE name=?2 AND operation = ?3 AND op_version = ?4",
                                  -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, gtk_entry_get_text(GTK_ENTRY(tb)), -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, d->edit_preset, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, self->plugin_name, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, self->version());
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      // we update the presets list
      g_free(d->edit_preset);
      d->edit_preset = g_strdup(gtk_entry_get_text(GTK_ENTRY(tb)));
      d->editor_reset = TRUE;
      _manage_preset_update_list(self);
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(d->presets_combo), d->edit_preset);
      d->editor_reset = FALSE;
    }
    else if(btn == d->presets_btn_new)
    {
      // create a new minimal preset
      char *tx = _presets_get_minimal(self);
      dt_lib_presets_add(gtk_entry_get_text(GTK_ENTRY(tb)), self->plugin_name, self->version(), tx, strlen(tx),
                         FALSE);
      g_free(tx);
      // update the presets list
      d->editor_reset = TRUE;
      _manage_preset_update_list(self);
      d->editor_reset = FALSE;
      // select the new preset
      _manage_editor_load(gtk_entry_get_text(GTK_ENTRY(tb)), self);
    }
    else if(btn == d->presets_btn_dup)
    {
      char *tx = _preset_to_string(self, TRUE);
      dt_lib_presets_add(gtk_entry_get_text(GTK_ENTRY(tb)), self->plugin_name, self->version(), tx, strlen(tx),
                         FALSE);
      g_free(tx);
      // update the presets list
      d->editor_reset = TRUE;
      _manage_preset_update_list(self);
      d->editor_reset = FALSE;
      // select the new preset
      _manage_editor_load(gtk_entry_get_text(GTK_ENTRY(tb)), self);
    }
  }

  gtk_widget_destroy(dialog);
  g_list_free_full(names, g_free);
}

static void _preset_autoapply_edit(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(d->editor_reset) return;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT rowid"
                              " FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, d->edit_preset, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int rowid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    dt_gui_presets_show_edit_dialog(d->edit_preset, self->name(self), rowid, G_CALLBACK(_preset_autoapply_changed),
                                    self, FALSE, FALSE, FALSE, GTK_WINDOW(d->dialog));
  }
  else
    sqlite3_finalize(stmt);
}

static void _manage_editor_load(const char *preset, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // if we have a currently edited preset, we save it
  if(d->edit_preset && g_strcmp0(preset, d->edit_preset) != 0)
  {
    _manage_editor_save(self);
  }

  // we avoid widgets events to be executed
  d->editor_reset = TRUE;

  // we remove all widgets from the groups-box
  dt_gui_container_destroy_children(GTK_CONTAINER(d->preset_groups_box));
  gtk_box_set_homogeneous(GTK_BOX(d->preset_groups_box), TRUE);

  // we select the right preset in the combobox (or we select the first one)
  gboolean sel_ok = FALSE;
  if(preset) sel_ok = gtk_combo_box_set_active_id(GTK_COMBO_BOX(d->presets_combo), preset);
  if(!sel_ok) gtk_combo_box_set_active(GTK_COMBO_BOX(d->presets_combo), 0);
  const gchar *sel_preset = gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->presets_combo));

  // get all presets groups
  if(d->edit_groups) _manage_editor_groups_cleanup(self, TRUE);
  if(d->edit_preset) g_free(d->edit_preset);
  d->edit_groups = NULL;
  d->edit_preset = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT writeprotect, op_params, autoapply"
                              " FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, sel_preset, -1, SQLITE_TRANSIENT);

  int autoapply = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    d->edit_ro = sqlite3_column_int(stmt, 0);
    const void *blob = sqlite3_column_blob(stmt, 1);
    _preset_from_string(self, (char *)blob, TRUE);
    d->edit_basics_box = NULL;
    d->edit_basics_groupbox = NULL;
    _basics_cleanup_list(self, TRUE);
    d->edit_preset = g_strdup(sel_preset);

    autoapply = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);
  }
  else
  {
    d->editor_reset = FALSE;
    sqlite3_finalize(stmt);
    return;
  }

  // presets buttons
  gtk_widget_set_sensitive(d->presets_btn_rename, !d->edit_ro);
  gtk_widget_set_sensitive(d->presets_btn_remove, !d->edit_ro);
  gtk_widget_set_sensitive(d->presets_btn_dup, g_strcmp0(sel_preset, _(DEPRECATED_PRESET_NAME)));

  // search checkbox
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->edit_search_cb), d->edit_show_search);
  gtk_widget_set_sensitive(d->edit_search_cb, !d->edit_ro);

  // full_active checkbox
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->edit_full_active_cb), d->edit_full_active);
  gtk_widget_set_sensitive(d->edit_full_active_cb, !d->edit_ro);

  // basics checkbox
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->basics_chkbox), d->edit_basics_show);
  gtk_widget_set_sensitive(d->basics_chkbox, !d->edit_ro);

  // autoapply
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->edit_autoapply_chkbox), autoapply);
  gtk_widget_set_sensitive(d->edit_autoapply_btn, !d->edit_ro);

  // new group button
  gtk_widget_set_visible(d->preset_btn_add_group, !d->edit_ro);

  // set up basics widgets
  d->edit_basics_groupbox = _manage_editor_group_init_basics_box(self);
  gtk_box_pack_start(GTK_BOX(d->preset_groups_box), d->edit_basics_groupbox, FALSE, TRUE, 0);
  gtk_widget_show_all(d->edit_basics_groupbox);
  gtk_widget_set_no_show_all(d->edit_basics_groupbox, TRUE);
  gtk_widget_set_visible(d->edit_basics_groupbox, d->edit_basics_show);

  // other groups
  for(const GList *l = d->edit_groups; l; l = g_list_next(l))
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    GtkWidget *vb2 = _manage_editor_group_init_modules_box(self, gr);
    gtk_widget_show_all(vb2);
    gtk_box_pack_start(GTK_BOX(d->preset_groups_box), vb2, FALSE, TRUE, 0);
  }

  // read-only message
  gtk_widget_set_visible(d->preset_read_only_label, d->edit_ro);

  // reset button
  gtk_widget_set_visible(d->preset_reset_btn, !d->edit_ro);

  // and we update arrows
  if(!d->edit_ro) _manage_editor_group_update_arrows(d->preset_groups_box);

  d->editor_reset = FALSE;
  // set keyboard focus on the scrollable window (not on a widget)
  // gtk_widget_grab_focus(sw);
}

static void _manage_preset_change(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(d->editor_reset) return;
  const char *preset = gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->presets_combo));
  _manage_editor_load(preset, self);
}

static void _manage_preset_delete(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  if(!dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset")
     || dt_gui_show_yes_no_dialog(_("delete preset?"),
                                  _("do you really want to delete the preset `%s'?"), d->edit_preset))
  {
    dt_lib_presets_remove(d->edit_preset, self->plugin_name, self->version());

    // if the deleted preset was the one currently in use, load default preset
    if(dt_conf_key_exists("plugins/darkroom/modulegroups_preset"))
    {
      const char *cur = dt_conf_get_string_const("plugins/darkroom/modulegroups_preset");
      if(g_strcmp0(cur, d->edit_preset) == 0)
      {
        dt_conf_set_string("plugins/darkroom/modulegroups_preset", C_("modulegroup", FALLBACK_PRESET_NAME));
        dt_lib_presets_apply((gchar *)C_("modulegroup", FALLBACK_PRESET_NAME),
                             self->plugin_name, self->version());
      }
    }

    // reload presets list
    _manage_preset_update_list(self);
    // we fallback to the first preset
    _manage_editor_load(NULL, self);
  }
}

static void _manage_preset_update_list(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we first remove all existing entries from the combobox
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(d->presets_combo));

  // and we repopulate it
  sqlite3_stmt *stmt;
  // order: get shipped defaults first
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name"
                              " FROM data.presets"
                              " WHERE operation=?1 AND op_version=?2"
                              " ORDER BY writeprotect DESC, name, rowid",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->presets_combo), name, name);
  }
  sqlite3_finalize(stmt);
}

static void _manage_editor_destroy(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we save the last edited preset
  _manage_editor_save(self);

  // and we free editing data
  if(d->edit_groups) _manage_editor_groups_cleanup(self, TRUE);
  if(d->edit_preset) g_free(d->edit_preset);
  d->edit_groups = NULL;
  d->edit_preset = NULL;
}

static void _manage_editor_resize_dialog(GtkWidget *widget, dt_lib_module_t *self)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_conf_set_int("ui_last/modulegroups_dialog_width", allocation.width);
  dt_conf_set_int("ui_last/modulegroups_dialog_height", allocation.height);
}

static void _manage_show_window(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  d->dialog = gtk_dialog_new_with_buttons(_("manage module layouts"), win,
                                          GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, NULL, NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->dialog);
#endif
  gtk_window_set_default_size(GTK_WINDOW(d->dialog), dt_conf_get_int("ui_last/modulegroups_dialog_width"),
                              dt_conf_get_int("ui_last/modulegroups_dialog_height"));
  gtk_widget_set_name(d->dialog, "modulegroups-manager");
  gtk_window_set_title(GTK_WINDOW(d->dialog), _("manage module layouts"));
  g_signal_connect(d->dialog, "check-resize", G_CALLBACK(_manage_editor_resize_dialog), self);

  // remove the small border
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d->dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 0);

  GtkWidget *vb_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); // main box
  // preset combobox
  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb, "modulegroups-topbox");
  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(vb, "modulegroups-top-boxes");
  GtkWidget *hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hb2), gtk_label_new(_("preset: ")), FALSE, TRUE, 2);
  d->presets_combo = gtk_combo_box_text_new();
  g_signal_connect(G_OBJECT(d->presets_combo), "changed", G_CALLBACK(_manage_preset_change), self);
  gtk_box_pack_start(GTK_BOX(hb2), d->presets_combo, TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vb), hb2, FALSE, TRUE, 2);
  // presets buttons
  hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->presets_btn_remove = dt_action_button_new(NULL, N_("remove"), _manage_preset_delete, self,_("remove the preset"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hb2), d->presets_btn_remove, TRUE, TRUE, 2);
  d->presets_btn_dup = dt_action_button_new(NULL, N_("duplicate"), _manage_editor_preset_action, self,_("duplicate the preset"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hb2), d->presets_btn_dup, TRUE, TRUE, 2);
  d->presets_btn_rename = dt_action_button_new(NULL, N_("rename"), _manage_editor_preset_action, self,_("rename the preset"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hb2), d->presets_btn_rename, TRUE, TRUE, 2);
  d->presets_btn_new = dt_action_button_new(NULL, N_("new"), _manage_editor_preset_action, self,_("create a new empty preset"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hb2), d->presets_btn_new, TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vb), hb2, FALSE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, TRUE, 2);

  // presets settings (search + quick access + full active)
  vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(vb, "modulegroups-top-boxes");
  d->edit_search_cb = gtk_check_button_new_with_label(_("show search line"));
  g_signal_connect(G_OBJECT(d->edit_search_cb), "toggled", G_CALLBACK(_manage_editor_search_toggle), self);
  gtk_box_pack_start(GTK_BOX(vb), d->edit_search_cb, FALSE, TRUE, 0);
  d->basics_chkbox = gtk_check_button_new_with_label(_("show quick access panel"));
  g_signal_connect(G_OBJECT(d->basics_chkbox), "toggled", G_CALLBACK(_manage_editor_basics_toggle), self);
  gtk_box_pack_start(GTK_BOX(vb), d->basics_chkbox, FALSE, TRUE, 0);
  d->edit_full_active_cb = gtk_check_button_new_with_label(_("show all history modules in active group"));
  gtk_widget_set_tooltip_text(
      d->edit_full_active_cb,
      _("show modules that are present in the history stack, regardless of whether or not they are currently enabled"));
  g_signal_connect(G_OBJECT(d->edit_full_active_cb), "toggled", G_CALLBACK(_manage_editor_full_active_toggle),
                   self);
  gtk_box_pack_start(GTK_BOX(vb), d->edit_full_active_cb, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, TRUE, 0);

  // presets settings (autoapply)
  vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->edit_autoapply_chkbox = gtk_check_button_new_with_label(_("auto-apply this preset"));
  gtk_widget_set_sensitive(d->edit_autoapply_chkbox, FALSE); // always readonly. change are done with the button...
  gtk_box_pack_start(GTK_BOX(hb2), d->edit_autoapply_chkbox, FALSE, TRUE, 0);
  d->edit_autoapply_btn = dtgtk_button_new(dtgtk_cairo_paint_preferences, 0, NULL);
  g_signal_connect(G_OBJECT(d->edit_autoapply_btn), "clicked", G_CALLBACK(_preset_autoapply_edit), self);
  gtk_widget_set_name(d->edit_autoapply_btn, "modulegroups-autoapply-btn");
  gtk_box_pack_start(GTK_BOX(hb2), d->edit_autoapply_btn, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(vb), hb2, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vb_main), hb, FALSE, TRUE, 0);

  // groups title line
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb, "modulegroups-groups-title");
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("module groups")), FALSE, TRUE, 0);
  d->preset_btn_add_group = dtgtk_button_new(dtgtk_cairo_paint_square_plus, CPF_DIRECTION_LEFT, NULL);
  g_signal_connect(G_OBJECT(d->preset_btn_add_group), "button-press-event", G_CALLBACK(_manage_editor_group_add),
                   self);
  gtk_box_pack_start(GTK_BOX(hb), d->preset_btn_add_group, FALSE, FALSE, 0);
  gtk_widget_set_halign(hb, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(vb_main), hb, FALSE, TRUE, 0);

  // groups line
  d->preset_groups_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->preset_groups_box, "modulegroups-groups-box");
  gtk_widget_set_vexpand(d->preset_groups_box, TRUE);
  gtk_widget_set_halign(d->preset_groups_box, GTK_ALIGN_FILL);
  gtk_box_pack_start(GTK_BOX(vb_main), d->preset_groups_box, TRUE, TRUE, 0);

  // read only message
  d->preset_read_only_label
      = gtk_label_new(_("this is a built-in read-only preset. duplicate it if you want to make changes"));
  gtk_widget_set_name(d->preset_read_only_label, "modulegroups-ro");
  gtk_box_pack_start(GTK_BOX(vb_main), d->preset_read_only_label, FALSE, TRUE, 0);

  // reset button
  hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->preset_reset_btn = gtk_button_new_with_label(_("reset"));
  g_signal_connect(G_OBJECT(d->preset_reset_btn), "button-press-event", G_CALLBACK(_manage_editor_reset), self);
  gtk_box_pack_end(GTK_BOX(hb2), d->preset_reset_btn, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vb_main), hb2, FALSE, TRUE, 0);

  // we load the presets list
  _manage_preset_update_list(self);

  gtk_widget_show_all(vb_main);

  // and we select the current one
  const char *preset = dt_conf_get_string_const("plugins/darkroom/modulegroups_preset");
  _manage_editor_load(preset, self);

  gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d->dialog))), vb_main);

  g_signal_connect(d->dialog, "destroy", G_CALLBACK(_manage_editor_destroy), self);
  gtk_window_set_resizable(GTK_WINDOW(d->dialog), TRUE);

  gtk_window_set_position(GTK_WINDOW(d->dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show(d->dialog);
}


void manage_presets(dt_lib_module_t *self)
{
  _manage_show_window(self);
}

void view_leave(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  if(!strcmp(old_view->module_name, "darkroom"))
  {
    _basics_hide(self);
  }
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  if(!strcmp(new_view->module_name, "darkroom"))
  {
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

    // and we initialize the buttons too
    const char *preset = dt_conf_get_string_const("plugins/darkroom/modulegroups_preset");
    if(!dt_lib_presets_apply(preset, self->plugin_name, self->version()))
      dt_lib_presets_apply(_(FALLBACK_PRESET_NAME), self->plugin_name, self->version());

    // and set the current group
    d->current = dt_conf_get_int("plugins/darkroom/groups");
  }
}

gboolean preset_autoapply(dt_lib_module_t *self)
{
  return TRUE;
}
#undef PADDING
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
