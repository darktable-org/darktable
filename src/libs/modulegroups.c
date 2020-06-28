/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "common/image_cache.h"
#include "common/iop_group.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define PADDING 2
#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

#include "modulegroups.h"

typedef struct dt_lib_modulegroups_group_t
{
  gchar *name;
  GtkWidget *button;
  gchar *icon;
  // default
  GList *modules;
} dt_lib_modulegroups_group_t;

typedef struct dt_lib_modulegroups_t
{
  uint32_t current;
  GtkWidget *text_entry;
  GtkWidget *hbox_buttons;
  GtkWidget *active_btn;
  GtkWidget *hbox_groups;
  GtkWidget *hbox_search_box;

  GList *groups;

  GList *edit_groups;
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
/* modulegroups proxy switch group function.
   sets the active group which module belongs too.
*/
static void _lib_modulegroups_switch_group(dt_lib_module_t *self, dt_iop_module_t *module);
/* modulergroups proxy search text focus function
   \see dt_dev_modulegroups_search_text_focus()
*/
static void _lib_modulegroups_search_text_focus(dt_lib_module_t *self);

/* hook up with viewmanager view change to initialize modulegroup */
static void _lib_modulegroups_viewchanged_callback(gpointer instance, dt_view_t *old_view,
                                                   dt_view_t *new_view, gpointer data);

static void _manage_update_presets_list(dt_lib_module_t *self, GtkWidget *vb);

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

int position()
{
  return 999;
}

static GtkWidget *_buttons_get_from_pos(dt_lib_module_t *self, const int pos)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(pos == 0) return d->active_btn;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_list_nth_data(d->groups, pos - 1);
  if(gr) return gr->button;
  return NULL;
}

static dt_lib_modulegroup_iop_visibility_type_t _get_search_iop_visibility()
{
  dt_lib_modulegroup_iop_visibility_type_t ret = DT_MODULEGROUP_SEARCH_IOP_TEXT_GROUPS_VISIBLE;
  const gchar *show_text_entry = dt_conf_get_string("plugins/darkroom/search_iop_by_text");

  if(strcmp(show_text_entry, "show search text") == 0)
    ret = DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE;
  else if(strcmp(show_text_entry, "show groups") == 0)
    ret = DT_MODULEGROUP_SEARCH_IOP_GROUPS_VISIBLE;
  else if(strcmp(show_text_entry, "show both") == 0)
    ret = DT_MODULEGROUP_SEARCH_IOP_TEXT_GROUPS_VISIBLE;

  return ret;
}

static void _text_entry_changed_callback(GtkEntry *entry, dt_lib_module_t *self)
{
  _lib_modulegroups_update_iop_visibility(self);
}

static gboolean _text_entry_icon_press_callback(GtkEntry *entry, GtkEntryIconPosition icon_pos, GdkEvent *event,
                                                dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  gtk_entry_set_text(GTK_ENTRY(d->text_entry), "");

  return TRUE;
}

static gboolean _text_entry_key_press_callback(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{

  if(event->keyval == GDK_KEY_Escape)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), "");
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
    return TRUE;
  }

  return FALSE;
}

void view_leave(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  if(!strcmp(old_view->module_name, "darkroom"))
  {
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
    dt_gui_key_accel_block_on_focus_disconnect(d->text_entry);
  }
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  if(!strcmp(new_view->module_name, "darkroom"))
  {
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
    dt_gui_key_accel_block_on_focus_connect(d->text_entry);

    // and we initialize the buttons too
    gchar *preset = dt_conf_get_string("plugins/darkroom/modulegroups_preset");
    if(!dt_lib_presets_apply(preset, self->plugin_name, self->version()))
      dt_lib_presets_apply(_("default"), self->plugin_name, self->version());
    g_free(preset);
  }
}

static DTGTKCairoPaintIconFunc _buttons_get_icon_fct(gchar *icon)
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

  return dtgtk_cairo_paint_modulegroup_basic;
}

static void _buttons_update(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // first, we destroy all existing buttons except active one an preset one
  GList *l = gtk_container_get_children(GTK_CONTAINER(d->hbox_groups));
  if(l) l = g_list_next(l);
  while(l)
  {
    GtkWidget *bt = (GtkWidget *)l->data;
    gtk_widget_destroy(bt);
    l = g_list_next(l);
  }

  // then we repopulate the box with new buttons
  l = d->groups;
  while(l)
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    GtkWidget *bt = dtgtk_togglebutton_new(_buttons_get_icon_fct(gr->icon), CPF_STYLE_FLAT, NULL);
    g_signal_connect(bt, "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
    gr->button = bt;
    gtk_box_pack_start(GTK_BOX(d->hbox_groups), bt, TRUE, TRUE, 0);
    gtk_widget_show(bt);
    l = g_list_next(l);
  }

  // last, if d->current still valid, we select it otherwise the first one
  int cur = d->current;
  d->current = -1;
  if(cur > g_list_length(d->groups)) cur = 0;
  if(cur == 0)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->active_btn), TRUE);
  else
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_list_nth_data(d->groups, cur - 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gr->button), TRUE);
  }
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)g_malloc0(sizeof(dt_lib_modulegroups_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_widget_set_name(self->widget, "modules-tabs");

  dtgtk_cairo_paint_flags_t pf = CPF_STYLE_FLAT;

  d->hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->hbox_search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  // groups
  d->hbox_groups = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(d->hbox_buttons), d->hbox_groups, TRUE, TRUE, 0);

  // active group button
  d->active_btn = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_active, pf, NULL);
  g_signal_connect(d->active_btn, "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->active_btn, _("show only active modules"));
  gtk_box_pack_start(GTK_BOX(d->hbox_groups), d->active_btn, TRUE, TRUE, 0);

  // we load now the presets btn
  self->presets_button = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(self->presets_button, _("presets"));
  gtk_box_pack_start(GTK_BOX(d->hbox_buttons), self->presets_button, FALSE, FALSE, 0);

  /* search box */
  GtkWidget *label = gtk_label_new(_("search module"));
  gtk_box_pack_start(GTK_BOX(d->hbox_search_box), label, FALSE, TRUE, 0);

  d->text_entry = gtk_entry_new();
  gtk_widget_add_events(d->text_entry, GDK_FOCUS_CHANGE_MASK);

  gtk_widget_set_tooltip_text(d->text_entry, _("search modules by name or tag"));
  gtk_widget_add_events(d->text_entry, GDK_KEY_PRESS_MASK);
  g_signal_connect(G_OBJECT(d->text_entry), "changed", G_CALLBACK(_text_entry_changed_callback), self);
  g_signal_connect(G_OBJECT(d->text_entry), "icon-press", G_CALLBACK(_text_entry_icon_press_callback), self);
  g_signal_connect(G_OBJECT(d->text_entry), "key-press-event", G_CALLBACK(_text_entry_key_press_callback), self);
  gtk_box_pack_start(GTK_BOX(d->hbox_search_box), d->text_entry, TRUE, TRUE, 0);
  gtk_entry_set_width_chars(GTK_ENTRY(d->text_entry), 0);
  gtk_entry_set_icon_from_icon_name(GTK_ENTRY(d->text_entry), GTK_ENTRY_ICON_SECONDARY, "edit-clear");
  gtk_entry_set_icon_tooltip_text(GTK_ENTRY(d->text_entry), GTK_ENTRY_ICON_SECONDARY, _("clear text"));
  gtk_widget_set_name(GTK_WIDGET(d->hbox_search_box), "search-box");


  gtk_box_pack_start(GTK_BOX(self->widget), d->hbox_buttons, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->hbox_search_box, TRUE, TRUE, 0);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->active_btn), TRUE);
  if(d->current == DT_MODULEGROUP_NONE) _lib_modulegroups_update_iop_visibility(self);
  gtk_widget_show_all(self->widget);
  gtk_widget_show_all(d->hbox_buttons);
  gtk_widget_set_no_show_all(d->hbox_buttons, TRUE);
  gtk_widget_show_all(d->hbox_search_box);
  gtk_widget_set_no_show_all(d->hbox_search_box, TRUE);

  dt_lib_modulegroup_iop_visibility_type_t show_text_entry = _get_search_iop_visibility();
  if(show_text_entry == DT_MODULEGROUP_SEARCH_IOP_GROUPS_VISIBLE)
  {
    gtk_widget_hide(d->hbox_search_box);
  }
  else if(show_text_entry == DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE)
  {
    gtk_widget_hide(d->hbox_buttons);
  }

  /*
   * set the proxy functions
   */
  darktable.develop->proxy.modulegroups.module = self;
  darktable.develop->proxy.modulegroups.set = _lib_modulegroups_set;
  darktable.develop->proxy.modulegroups.update_visibility = _lib_modulegroups_update_visibility_proxy;
  darktable.develop->proxy.modulegroups.get = _lib_modulegroups_get;
  darktable.develop->proxy.modulegroups.test = _lib_modulegroups_test;
  darktable.develop->proxy.modulegroups.switch_group = _lib_modulegroups_switch_group;
  darktable.develop->proxy.modulegroups.search_text_focus = _lib_modulegroups_search_text_focus;

  /* let's connect to view changed signal to set default group */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(_lib_modulegroups_viewchanged_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  dt_gui_key_accel_block_on_focus_disconnect(d->text_entry);

  /* let's not listen to signals anymore.. */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_modulegroups_viewchanged_callback), self);

  darktable.develop->proxy.modulegroups.module = NULL;
  darktable.develop->proxy.modulegroups.set = NULL;
  darktable.develop->proxy.modulegroups.get = NULL;
  darktable.develop->proxy.modulegroups.test = NULL;
  darktable.develop->proxy.modulegroups.switch_group = NULL;

  g_free(self->data);
  self->data = NULL;
}

static void _lib_modulegroups_viewchanged_callback(gpointer instance, dt_view_t *old_view,
                                                   dt_view_t *new_view, gpointer data)
{
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

static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  const dt_lib_modulegroup_iop_visibility_type_t visibility = _get_search_iop_visibility();
  const gchar *text_entered = (gtk_widget_is_visible(GTK_WIDGET(d->hbox_search_box)))
                                  ? gtk_entry_get_text(GTK_ENTRY(d->text_entry))
                                  : NULL;

  if (DT_IOP_ORDER_INFO)
    fprintf(stderr,"\n^^^^^ modulegroups");

  /* only show module group as selected if not currently searching */
  if(visibility != DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE && d->current != DT_MODULEGROUP_NONE)
  {
    GtkWidget *bt = _buttons_get_from_pos(self, d->current);
    if(bt)
    {
      /* toggle button visibility without executing callback */
      g_signal_handlers_block_matched(bt, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);

      if(text_entered && text_entered[0] != '\0')
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), FALSE);
      else
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), TRUE);

      g_signal_handlers_unblock_matched(bt, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle, NULL);
    }
  }

  GList *modules = darktable.develop->iop;
  if(modules)
  {
    /*
     * iterate over ip modules and do various test to
     * detect if the modules should be shown or not.
     */
    do
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
      GtkWidget *w = module->expander;

      if ((DT_IOP_ORDER_INFO) && (module->enabled))
      {
        fprintf(stderr,"\n%20s %d",module->op, module->iop_order);
        if(dt_iop_is_hidden(module)) fprintf(stderr,", hidden");
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
                                != NULL);

          if(is_match)
            gtk_widget_show(w);
          else
            gtk_widget_hide(w);
        }
        continue;
      }
      // if only the search box is visible show the active pipe
      else if(visibility == DT_MODULEGROUP_SEARCH_IOP_TEXT_VISIBLE)
      {
        if(module->enabled)
        {
          if(w) gtk_widget_show(w);
        }
        else
        {
          if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
          if(w) gtk_widget_hide(w);
        }
        continue;
      }

      /* lets show/hide modules dependent on current group*/
      switch(d->current)
      {
        case DT_MODULEGROUP_ACTIVE_PIPE:
        {
          if(module->enabled)
          {
            if(w) gtk_widget_show(w);
          }
          else
          {
            if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
            if(w) gtk_widget_hide(w);
          }
        }
        break;

        case DT_MODULEGROUP_NONE:
        {
          /* show all except hidden ones */
          if((module->so->state != dt_iop_state_HIDDEN || module->enabled)
             && (!(module->flags() & IOP_FLAGS_DEPRECATED)))
          {
            if(w) gtk_widget_show(w);
          }
          else
          {
            if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
            if(w) gtk_widget_hide(w);
          }
        }
        break;

        default:
        {
          if(_lib_modulegroups_test_internal(self, d->current, module)
             && (!(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled))
          {
            if(w) gtk_widget_show(w);
          }
          else
          {
            if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
            if(w) gtk_widget_hide(w);
          }
        }
      }
    } while((modules = g_list_next(modules)) != NULL);
  }
  if (DT_IOP_ORDER_INFO) fprintf(stderr,"\nvvvvv\n");
  // now that visibility has been updated set multi-show
  dt_dev_modules_update_multishow(darktable.develop);
}

static void _lib_modulegroups_toggle(GtkWidget *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  const gchar *text_entered = (gtk_widget_is_visible(GTK_WIDGET(d->hbox_search_box)))
                                  ? gtk_entry_get_text(GTK_ENTRY(d->text_entry))
                                  : NULL;

  /* block all button callbacks */
  for(int k = 0; k <= g_list_length(d->groups); k++)
    g_signal_handlers_block_matched(_buttons_get_from_pos(self, k), G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                    _lib_modulegroups_toggle, NULL);

  /* deactivate all buttons */
  int gid = 0;
  for(int k = 0; k <= g_list_length(d->groups); k++)
  {
    const GtkWidget *bt = _buttons_get_from_pos(self, k);
    /* store toggled modulegroup */
    if(bt == button) gid = k;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), FALSE);
  }

  /* only deselect button if not currently searching else re-enable module */
  if(d->current == gid && !(text_entered && text_entered[0] != '\0'))
    d->current = DT_MODULEGROUP_NONE;
  else
  {
    d->current = gid;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_buttons_get_from_pos(self, gid)), TRUE);
  }

  /* unblock all button callbacks */
  for(int k = 0; k <= g_list_length(d->groups); k++)
    g_signal_handlers_unblock_matched(_buttons_get_from_pos(self, k), G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                      _lib_modulegroups_toggle, NULL);

  /* clear search text */
  if(gtk_widget_is_visible(GTK_WIDGET(d->hbox_search_box)))
  {
    g_signal_handlers_block_matched(d->text_entry, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _text_entry_changed_callback, NULL);
    gtk_entry_set_text(GTK_ENTRY(d->text_entry), "");
    g_signal_handlers_unblock_matched(d->text_entry, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _text_entry_changed_callback, NULL);
  }

  /* update visibility */
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

static gboolean _lib_modulegroups_search_text_focus_gui_thread(gpointer user_data)
{
  _set_gui_thread_t *params = (_set_gui_thread_t *)user_data;

  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)params->self->data;

  if(GTK_IS_ENTRY(d->text_entry))
  {
    if(!gtk_widget_is_visible(GTK_WIDGET(d->hbox_search_box))) gtk_widget_show(GTK_WIDGET(d->hbox_search_box));
    gtk_widget_grab_focus(GTK_WIDGET(d->text_entry));
  }

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

/* this is a proxy function so it might be called from another thread */
static void _lib_modulegroups_search_text_focus(dt_lib_module_t *self)
{
  _set_gui_thread_t *params = (_set_gui_thread_t *)malloc(sizeof(_set_gui_thread_t));
  if(!params) return;
  params->self = self;
  params->group = 0;
  g_main_context_invoke(NULL, _lib_modulegroups_search_text_focus_gui_thread, params);
}

static void _lib_modulegroups_switch_group(dt_lib_module_t *self, dt_iop_module_t *module)
{
  /* lets find the group which is not active pipe */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  for(int k = 1; k <= g_list_length(d->groups); k++)
  {
    if(_lib_modulegroups_test(self, k, module))
    {
      _lib_modulegroups_set(self, k);
      return;
    }
  }
}

static uint32_t _lib_modulegroups_get(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  return d->current;
}

static gchar *_preset_to_string(GList *groups)
{
  gchar *res = NULL;
  GList *l = groups;
  while(l)
  {
    dt_lib_modulegroups_group_t *g = (dt_lib_modulegroups_group_t *)l->data;
    if(res) res = dt_util_dstrcat(res, "ꬹ");
    res = dt_util_dstrcat(res, "%s|%s", g->name, g->icon);
    GList *ll = g->modules;
    while(ll)
    {
      gchar *m = (gchar *)ll->data;
      res = dt_util_dstrcat(res, "|%s", m);
      ll = g_list_next(ll);
    }
    l = g_list_next(l);
  }

  return res;
}

static GList *_preset_from_string(gchar *txt)
{
  GList *res = NULL;
  if(!txt) return res;

  gchar **gr = g_strsplit(txt, "ꬹ", -1);
  for(int i = 0; i < g_strv_length(gr); i++)
  {
    gchar *tx = gr[i];
    if(tx)
    {
      gchar **gr2 = g_strsplit(tx, "|", -1);
      const int nb = g_strv_length(gr2);
      if(nb > 1)
      {
        dt_lib_modulegroups_group_t *group
            = (dt_lib_modulegroups_group_t *)g_malloc0(sizeof(dt_lib_modulegroups_group_t));
        group->name = g_strdup(gr2[0]);
        group->icon = g_strdup(gr2[1]);
        for(int j = 2; j < nb; j++)
        {
          group->modules = g_list_append(group->modules, g_strdup(gr2[j]));
        }
        res = g_list_append(res, group);
      }
      g_strfreev(gr2);
    }
  }
  g_strfreev(gr);

  return res;
}

void init_presets(dt_lib_module_t *self)
{
  gchar *tx = "test|basic|ashift|filmicrgb|exposureꬹcoucou|tone|clipping|vignette|watermarkꬹtruc|effect|"
              "clipping|filmicrgb|"
              "tonecurve|temperature";
  dt_lib_presets_add(_("default"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);

  gchar *tx2 = "test|color|filmicrgbꬹtruc|favourites|clipping|filmicrgb";
  dt_lib_presets_add(_("test"), self->plugin_name, self->version(), tx2, strlen(tx2), TRUE);
}

void *legacy_params(dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size)
{
  return NULL;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  gchar *tx = _preset_to_string(d->groups);
  *size = strlen(tx);
  return tx;
}

static void _manage_cleanup_groups(GList **groups)
{
  GList *l = *groups;
  while(l)
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    g_free(gr->name);
    g_free(gr->icon);
    g_list_free_full(gr->modules, g_free);
    l = g_list_next(l);
  }
  g_list_free_full(*groups, g_free);
  *groups = NULL;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // cleanup existing groups
  _manage_cleanup_groups(&d->groups);

  d->groups = _preset_from_string((char *)params);

  gchar *tx = dt_util_dstrcat(NULL, "plugins/darkroom/%s/last_preset", self->plugin_name);
  dt_conf_set_string("plugins/darkroom/modulegroups_preset", dt_conf_get_string(tx));
  g_free(tx);

  _buttons_update(self);
  return 0;
}

static GtkWidget *_manage_get_iop_combo(dt_lib_module_t *self, GList *exclude)
{
  GtkWidget *combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "", _("modules to add"));

  GList *modules = g_list_last(darktable.iop);
  while(modules)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);
    if(!dt_iop_so_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), module->op, module->name());
    }
    modules = g_list_previous(modules);
  }

  return combo;
}

static void _manage_duplicate_preset(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  char *preset = (char *)g_object_get_data(G_OBJECT(widget), "preset_name");
  dt_lib_presets_duplicate(preset, self->plugin_name, self->version());

  // reload the window
  GtkWidget *vb = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "presets_vbox");
  _manage_update_presets_list(self, vb);
}

static void _manage_delete_preset(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  char *preset = (char *)g_object_get_data(G_OBJECT(widget), "preset_name");

  gint res = GTK_RESPONSE_YES;
  GtkWidget *w = gtk_widget_get_toplevel(widget);

  if(dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset"))
  {
    GtkWidget *dialog
        = gtk_message_dialog_new(GTK_WINDOW(w), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                                 GTK_BUTTONS_YES_NO, _("do you really want to delete the preset `%s'?"), preset);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete preset?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  if(res == GTK_RESPONSE_YES)
  {
    dt_lib_presets_remove(preset, self->plugin_name, self->version());

    // remove the line
    gtk_widget_destroy(gtk_widget_get_parent(widget));
  }
}

static void _manage_editor_save(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  // get all the values
  GtkWidget *tb = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "name_entry");
  char *old_name = (char *)g_object_get_data(G_OBJECT(widget), "old_name");
  gchar *params = _preset_to_string(d->edit_groups);
  const gchar *newname = gtk_entry_get_text(GTK_ENTRY(tb));

  // update the preset in the database
  dt_lib_presets_update(old_name, self->plugin_name, self->version(), newname, "", params, strlen(params));

  // cleanup
  _manage_cleanup_groups(&d->edit_groups);
  gtk_widget_destroy(gtk_widget_get_toplevel(widget));

  // update groups
  gchar *preset = dt_conf_get_string("plugins/darkroom/modulegroups_preset");
  if(!dt_lib_presets_apply(preset, self->plugin_name, self->version()))
    dt_lib_presets_apply(_("default"), self->plugin_name, self->version());
}

static void _manage_editor_close(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  _manage_cleanup_groups(&d->edit_groups);
  gtk_widget_destroy(gtk_widget_get_toplevel(widget));
}

static void _manage_remove_module(GtkWidget *widget, GdkEventButton *event, GList **modules)
{
  char *module = (char *)g_object_get_data(G_OBJECT(widget), "module_name");
  GList *l = *modules;
  while(l)
  {
    char *tx = (char *)l->data;
    if(g_strcmp0(tx, module) == 0)
    {
      g_free(l->data);
      *modules = g_list_delete_link(*modules, l);
      gtk_widget_destroy(gtk_widget_get_parent(widget));
      break;
    }
    l = g_list_next(l);
  }
}

static void _manage_update_modules_list(GList **modules, GtkWidget *vb)
{
  // first, we remove all existing modules
  GList *lw = gtk_container_get_children(GTK_CONTAINER(vb));
  while(lw)
  {
    GtkWidget *w = (GtkWidget *)lw->data;
    gtk_widget_destroy(w);
    lw = g_list_next(lw);
  }

  // and we add the ones from the list
  GList *modules2 = g_list_last(darktable.iop);
  while(modules2)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules2->data);
    if(g_list_find_custom(*modules, module->op, _iop_compare))
    {
      GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(module->name()), FALSE, TRUE, 0);
      GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_DO_NOT_USE_BORDER, NULL);
      gtk_widget_set_tooltip_text(btn, _("remove this module"));
      g_object_set_data(G_OBJECT(btn), "module_name", module->op);
      g_object_set_data(G_OBJECT(btn), "modules_vbox", vb);
      g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_remove_module), modules);
      gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, TRUE, 0);
      gtk_box_pack_start(GTK_BOX(vb), hb, FALSE, TRUE, 0);
    }
    modules2 = g_list_previous(modules2);
  }
  gtk_widget_show_all(vb);
}

static void _manage_add_module(GtkWidget *widget, GList **modules)
{
  const char *module = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget));
  if(g_strcmp0(module, "") == 0) return;

  if(!g_list_find_custom(*modules, module, _iop_compare))
  {
    *modules = g_list_append(*modules, g_strdup(module));
    GtkWidget *vb = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "modules_vbox");
    _manage_update_modules_list(modules, vb);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), "");
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

  // we move the group in the ui
  gtk_box_reorder_child(GTK_BOX(gtk_widget_get_parent(vb)), vb, pos + 1);
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

  // we move the group in the ui
  gtk_box_reorder_child(GTK_BOX(gtk_widget_get_parent(vb)), vb, pos - 1);
}

static void _manage_editor_group_remove(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  GtkWidget *vb = gtk_widget_get_parent(gtk_widget_get_parent(widget));

  // we remove the group from the list and destroy it
  d->edit_groups = g_list_remove(d->edit_groups, gr);
  g_free(gr->name);
  g_free(gr->icon);
  g_list_free_full(gr->modules, g_free);
  g_free(gr);

  // we remove the group from the ui
  gtk_widget_destroy(vb);
}

static void _manage_editor_group_icon_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  GtkWidget *pop = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "popover");
  gtk_widget_show_all(pop);
}

static void _manage_editor_group_icon_changed(GtkWidget *widget, GdkEventButton *event,
                                              dt_lib_modulegroups_group_t *gr)
{
  char *ic = (char *)g_object_get_data(G_OBJECT(widget), "ic_name");
  g_free(gr->icon);
  gr->icon = g_strdup(ic);
  GtkWidget *pop = gtk_widget_get_parent(gtk_widget_get_parent(widget));
  GtkWidget *btn = gtk_popover_get_relative_to(GTK_POPOVER(pop));
  dtgtk_button_set_paint(DTGTK_BUTTON(btn), _buttons_get_icon_fct(ic), CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT,
                         NULL);
  gtk_popover_popdown(GTK_POPOVER(pop));
}

static GtkWidget *_manage_editor_group_icon_get_popup(GtkWidget *btn, dt_lib_modulegroups_group_t *gr)
{
  GtkWidget *pop = gtk_popover_new(btn);
  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *eb, *hb, *ic;
  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_basic, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("basic icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "basic");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_active, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("active icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "active");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_color, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("color icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "color");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_correct, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("correct icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "correct");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_effect, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("effect icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "effect");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_favorites, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("favorites icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "favorites");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_tone, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("tone icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "tone");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(pop), vb);
  g_object_set_data(G_OBJECT(btn), "popover", pop);
  return pop;
}

static GtkWidget *_manage_editor_get_group_box(dt_lib_module_t *self, dt_lib_modulegroups_group_t *gr)
{
  GtkWidget *vb2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  // line to edit the group
  GtkWidget *hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *btn = dtgtk_button_new(_buttons_get_icon_fct(gr->icon), CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_tooltip_text(btn, _("group icon"));
  _manage_editor_group_icon_get_popup(btn, gr);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_icon_popup), self);
  g_object_set_data(G_OBJECT(btn), "group", gr);
  gtk_box_pack_start(GTK_BOX(hb2), btn, FALSE, TRUE, 0);
  GtkWidget *tb = gtk_entry_new();
  gtk_widget_set_tooltip_text(tb, _("group name"));
  gtk_entry_set_text(GTK_ENTRY(tb), gr->name);
  gtk_box_pack_start(GTK_BOX(hb2), tb, TRUE, TRUE, 0);

  btn = dtgtk_button_new(dtgtk_cairo_paint_arrow, CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_LEFT | CPF_STYLE_FLAT,
                         NULL);
  gtk_widget_set_tooltip_text(btn, _("move group to the right"));
  g_object_set_data(G_OBJECT(btn), "group", gr);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_move_right), self);
  gtk_box_pack_end(GTK_BOX(hb2), btn, FALSE, TRUE, 0);
  btn = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(btn, _("remove group"));
  g_object_set_data(G_OBJECT(btn), "group", gr);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_remove), self);
  gtk_box_pack_end(GTK_BOX(hb2), btn, FALSE, TRUE, 0);
  btn = dtgtk_button_new(dtgtk_cairo_paint_arrow, CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_RIGHT | CPF_STYLE_FLAT,
                         NULL);
  gtk_widget_set_tooltip_text(btn, _("move group to the left"));
  g_object_set_data(G_OBJECT(btn), "group", gr);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_move_left), self);
  gtk_box_pack_end(GTK_BOX(hb2), btn, FALSE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vb2), hb2, FALSE, TRUE, 0);

  // combo box to add new module
  GtkWidget *vb3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *combo = _manage_get_iop_combo(self, gr->modules);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "");
  g_object_set_data(G_OBJECT(combo), "modules_vbox", vb3);
  g_signal_connect(G_OBJECT(combo), "changed", G_CALLBACK(_manage_add_module), &gr->modules);
  gtk_box_pack_start(GTK_BOX(vb2), combo, FALSE, TRUE, 0);

  // choosen modules
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  _manage_update_modules_list(&gr->modules, vb3);
  gtk_container_add(GTK_CONTAINER(sw), vb3);
  gtk_box_pack_start(GTK_BOX(vb2), sw, TRUE, TRUE, 0);

  return vb2;
}

static void _manage_editor_add_group(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_malloc0(sizeof(dt_lib_modulegroups_group_t));
  gr->name = _("new");
  d->edit_groups = g_list_append(d->edit_groups, gr);

  // we update the group list : remove the button, add the vb and put the button back
  g_object_ref(widget);
  GtkWidget *hb = gtk_widget_get_parent(widget);
  gtk_container_remove(GTK_CONTAINER(hb), widget);

  GtkWidget *vb2 = _manage_editor_get_group_box(self, gr);
  gtk_box_pack_start(GTK_BOX(hb), vb2, FALSE, TRUE, 5);

  gtk_box_pack_start(GTK_BOX(hb), widget, FALSE, FALSE, 0);
  g_object_unref(widget);

  gtk_widget_show_all(hb);
}

static void _manage_edit_preset(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  char *preset = g_strdup((char *)g_object_get_data(G_OBJECT(widget), "preset_name"));

  // get all presets groups
  if(d->edit_groups) _manage_cleanup_groups(&d->edit_groups);
  d->edit_groups = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, preset, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    d->edit_groups = _preset_from_string((char *)blob);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    return;
  }

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif
  gtk_widget_set_name(window, "modulegroups_editor");

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hb1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hb1), gtk_label_new(_("preset name : ")), FALSE, TRUE, 0);
  GtkWidget *tb0 = gtk_entry_new();
  gtk_widget_set_tooltip_text(tb0, _("preset name"));
  gtk_entry_set_text(GTK_ENTRY(tb0), preset);
  gtk_box_pack_start(GTK_BOX(hb1), tb0, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vb), hb1, FALSE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vb), gtk_label_new(_("module groups")), FALSE, TRUE, 0);
  hb1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GList *l = d->edit_groups;
  while(l)
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    GtkWidget *vb2 = _manage_editor_get_group_box(self, gr);
    gtk_box_pack_start(GTK_BOX(hb1), vb2, FALSE, TRUE, 5);
    l = g_list_next(l);
  }

  GtkWidget *bt = gtk_button_new();
  gtk_button_set_label(GTK_BUTTON(bt), _("new group"));
  g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_add_group), self);
  gtk_box_pack_start(GTK_BOX(hb1), bt, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(vb), hb1, TRUE, TRUE, 0);

  // save and cancel buttons
  hb1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  bt = gtk_button_new();
  gtk_button_set_label(GTK_BUTTON(bt), _("cancel"));
  g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_close), self);
  gtk_box_pack_start(GTK_BOX(hb1), bt, FALSE, TRUE, 0);
  bt = gtk_button_new();
  gtk_button_set_label(GTK_BUTTON(bt), _("save"));
  g_object_set_data(G_OBJECT(bt), "name_entry", tb0);
  g_object_set_data(G_OBJECT(bt), "old_name", preset);
  g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_save), self);
  gtk_box_pack_start(GTK_BOX(hb1), bt, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vb), hb1, FALSE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(window), vb);

  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);

  gtk_window_set_gravity(GTK_WINDOW(window), GDK_GRAVITY_STATIC);
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show_all(window);
  gtk_widget_destroy(gtk_widget_get_toplevel(widget));
}

static void _manage_update_presets_list(dt_lib_module_t *self, GtkWidget *vb)
{
  // we first remove all existing entries from the box
  GList *lw = gtk_container_get_children(GTK_CONTAINER(vb));
  while(lw)
  {
    GtkWidget *w = (GtkWidget *)lw->data;
    gtk_widget_destroy(w);
    lw = g_list_next(lw);
  }

  // and we repopulate it
  sqlite3_stmt *stmt;
  // order: get shipped defaults first
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, writeprotect, description FROM data.presets WHERE "
                              "operation=?1 AND op_version=?2 ORDER BY writeprotect DESC, name, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int ro = sqlite3_column_int(stmt, 1);
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *lbl = gtk_label_new(name);
    gtk_box_pack_start(GTK_BOX(hb), lbl, TRUE, TRUE, 0);
    GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(btn, _("duplicate this preset"));
    g_object_set_data(G_OBJECT(btn), "preset_name", g_strdup(name));
    g_object_set_data(G_OBJECT(btn), "presets_vbox", vb);
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_duplicate_preset), self);
    gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, FALSE, 0);
    btn = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(btn, _("edit this preset"));
    g_object_set_data(G_OBJECT(btn), "preset_name", g_strdup(name));
    if(!ro) g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_edit_preset), self);
    if(ro) gtk_widget_set_sensitive(btn, FALSE);
    gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, FALSE, 0);
    btn = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(btn, _("delete this preset"));
    g_object_set_data(G_OBJECT(btn), "preset_name", g_strdup(name));
    if(!ro) g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_delete_preset), self);
    if(ro) gtk_widget_set_sensitive(btn, FALSE);
    gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vb), hb, FALSE, TRUE, 0);
  }
  sqlite3_finalize(stmt);
  gtk_widget_show_all(vb);
}

static void _manage_add_preset(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  // find the new name
  sqlite3_stmt *stmt;
  int i = 0;
  gboolean ko = TRUE;
  while(ko)
  {
    i++;
    gchar *tx = dt_util_dstrcat(NULL, "new_%d", i);
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT name FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, tx, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_ROW) ko = FALSE;
    sqlite3_finalize(stmt);
    g_free(tx);
  }
  gchar *nname = dt_util_dstrcat(NULL, "new_%d", i);

  // and create a new empty preset
  dt_lib_presets_add(nname, self->plugin_name, self->version(), "", 0, FALSE);

  GtkWidget *vb = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "presets_vbox");
  _manage_update_presets_list(self, vb);
}

static void _manage_show_window(dt_lib_module_t *self)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif
  gtk_widget_set_name(window, "modulegroups_manager");

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_box_pack_start(GTK_BOX(vb), gtk_label_new(_("manage module layout presets")), FALSE, TRUE, 0);

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  GtkWidget *vb2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  _manage_update_presets_list(self, vb2);

  gtk_container_add(GTK_CONTAINER(sw), vb2);
  gtk_box_pack_start(GTK_BOX(vb), sw, TRUE, TRUE, 0);

  GtkWidget *bt = gtk_button_new();
  gtk_button_set_label(GTK_BUTTON(bt), _("new preset"));
  g_object_set_data(G_OBJECT(bt), "presets_vbox", vb2);
  g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_add_preset), self);
  gtk_box_pack_start(GTK_BOX(vb), bt, FALSE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(window), vb);

  gtk_window_set_default_size(GTK_WINDOW(window), 300, 400);
  gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);

  gtk_window_set_gravity(GTK_WINDOW(window), GDK_GRAVITY_STATIC);
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show_all(window);
}


void manage_presets(dt_lib_module_t *self)
{
  _manage_show_window(self);
}
#undef PADDING
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
