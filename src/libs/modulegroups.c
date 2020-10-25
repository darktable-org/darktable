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
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

#define FALLBACK_PRESET_NAME "default"
// if a preset cannot be loaded or the current preset deleted, this is the fallabck preset

#define PADDING 2
#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

#include "modulegroups.h"

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
  GtkWidget *hbox_groups;
  GtkWidget *hbox_search_box;

  GList *groups;

  GList *edit_groups;
  gchar *edit_preset;

  // editor dialog
  GtkWidget *dialog;
  GtkWidget *presets_list, *preset_box;
  GtkWidget *preset_name, *preset_groups_box;
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
/* modulergroups proxy search text focus function
   \see dt_dev_modulegroups_search_text_focus()
*/
static void _lib_modulegroups_search_text_focus(dt_lib_module_t *self);

/* hook up with viewmanager view change to initialize modulegroup */
static void _lib_modulegroups_viewchanged_callback(gpointer instance, dt_view_t *old_view,
                                                   dt_view_t *new_view, gpointer data);

static void _manage_preset_update_list(dt_lib_module_t *self);
static void _manage_editor_load(char *preset, dt_lib_module_t *self);

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
      dt_lib_presets_apply(_(FALLBACK_PRESET_NAME), self->plugin_name, self->version());
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
  else if(g_strcmp0(icon, "grading") == 0)
    return dtgtk_cairo_paint_modulegroup_grading;
  else if(g_strcmp0(icon, "technical") == 0)
    return dtgtk_cairo_paint_modulegroup_technical;

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
    gtk_widget_set_tooltip_text(bt, gr->name);
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
  darktable.develop->proxy.modulegroups.test_visible = _lib_modulegroups_test_visible;

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

static gboolean _lib_modulegroups_test_visible(dt_lib_module_t *self, gchar *module)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  GList *l = d->groups;
  while(l)
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    if(g_list_find_custom(gr->modules, module, _iop_compare) != NULL)
    {
      return TRUE;
    }
    l = g_list_next(l);
  }
  return FALSE;
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
          if(_lib_modulegroups_test_visible(self, module->op) || module->enabled)
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

/* presets syntax :
  Layout presets are saved as string consisting of blocs separated by ꬹ
  ꬹBLOC_0ꬹBLOC_1ꬹBLOC_2....
  BLOC_0 : reserved for future use. Always 1
  BLOC_1.... : blocs describing each group, contains :
    name|icon_name||iop_name_0|iop_name_1....
*/

static gchar *_preset_retrieve_old_layout_updated()
{
  gchar *ret = NULL;

  // layout with "new" 3 groups
  for(int i = 0; i < 4; i++)
  {
    // group name and icon
    if(i == 0)
      ret = dt_util_dstrcat(ret, "ꬹ1ꬹfavorites|favorites|");
    else if(i == 1)
      ret = dt_util_dstrcat(ret, "ꬹtechnical|technical|");
    else if(i == 2)
      ret = dt_util_dstrcat(ret, "ꬹgrading|grading|");
    else if(i == 3)
      ret = dt_util_dstrcat(ret, "ꬹeffects|effect|");

    // list of modules
    GList *modules = g_list_first(darktable.iop);
    while(modules)
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);

      if(!dt_iop_so_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
      {
        // get previous visibility values
        const int group = module->default_group();
        gchar *key = dt_util_dstrcat(NULL, "plugins/darkroom/%s/visible", module->op);
        const gboolean visi = dt_conf_get_bool(key);
        g_free(key);
        key = dt_util_dstrcat(NULL, "plugins/darkroom/%s/favorite", module->op);
        const gboolean fav = dt_conf_get_bool(key);
        g_free(key);

        if((i == 0 && fav && visi) || (i == 1 && (group & IOP_GROUP_TECHNICAL) && visi)
           || (i == 2 && (group & IOP_GROUP_GRADING) && visi) || (i == 3 && (group & IOP_GROUP_EFFECTS) && visi))
        {
          ret = dt_util_dstrcat(ret, "|%s", module->op);
        }
      }
      modules = g_list_next(modules);
    }
  }
  return ret;
}

static gchar *_preset_retrieve_old_layout(char *list, char *list_fav)
{
  gchar *ret = NULL;

  // layout with "old" 5 groups
  for(int i = 0; i < 6; i++)
  {
    // group name and icon
    if(i == 0)
      ret = dt_util_dstrcat(ret, "ꬹ1ꬹfavorites|favorites|");
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
    GList *modules = g_list_first(darktable.iop);
    while(modules)
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);

      if(!dt_iop_so_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
      {
        gchar *search = dt_util_dstrcat(NULL, "|%s|", module->op);
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
          key = dt_util_dstrcat(NULL, "plugins/darkroom/%s/modulegroup", module->op);
          group = dt_conf_get_int(key);
          g_free(key);
        }

        gboolean visi = FALSE;
        if(list)
          visi = (strstr(list, search) != NULL);
        else
        {
          key = dt_util_dstrcat(NULL, "plugins/darkroom/%s/visible", module->op);
          visi = dt_conf_get_bool(key);
          g_free(key);
        }

        gboolean fav = FALSE;
        if(i == 0 && list_fav)
          fav = (strstr(list_fav, search) != NULL);
        else if(i == 0)
        {
          key = dt_util_dstrcat(NULL, "plugins/darkroom/%s/favorite", module->op);
          fav = dt_conf_get_bool(key);
          g_free(key);
        }

        if((i == 0 && fav && visi) || (i == group && visi))
        {
          ret = dt_util_dstrcat(ret, "|%s", module->op);
        }

        g_free(search);
      }
      modules = g_list_next(modules);
    }
  }
  return ret;
}

static void _preset_retrieve_old_presets(dt_lib_module_t *self)
{
  // we retrieve old modulelist presets
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params"
                              " FROM data.presets"
                              " WHERE operation = 'modulelist' AND op_version = 1 AND writeprotect = 0",
                              -1, &stmt, NULL);

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
      int op_len = strlen(op);
      dt_iop_module_state_t state = p[pos + op_len + 1];

      if(state == dt_iop_state_ACTIVE)
        list = dt_util_dstrcat(list, "|%s", op);
      else if(state == dt_iop_state_FAVORITE)
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
  }
  sqlite3_finalize(stmt);

  // and we remove all existing modulelist presets
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM data.presets"
                        " WHERE operation = 'modulelist' AND op_version = 1",
                        NULL, NULL, NULL);
}

static gchar *_preset_to_string(GList *groups)
{
  gchar *res = NULL;
  GList *l = groups;
  res = dt_util_dstrcat(res, "ꬹ1");
  while(l)
  {
    dt_lib_modulegroups_group_t *g = (dt_lib_modulegroups_group_t *)l->data;
    res = dt_util_dstrcat(res, "ꬹ%s|%s|", g->name, g->icon);
    GList *ll = g->modules;
    while(ll)
    {
      gchar *m = (gchar *)ll->data;
      res = dt_util_dstrcat(res, "|%s", m);
      ll = g_list_next(ll);
    }
    l = g_list_next(l);
  }
  if(!res) res = g_strdup(" ");
  return res;
}

static GList *_preset_from_string(gchar *txt)
{
  GList *res = NULL;
  if(!txt) return res;

  gchar **gr = g_strsplit(txt, "ꬹ", -1);
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
  /*
    For the record, one can create the preset list by using the following code:

    $ cat <( git grep "return.*IOP_GROUP_TONE" -- src/iop/ | cut -d':' -f1 ) \
          <( git grep IOP_FLAGS_DEPRECATED -- src/iop/ | cut -d':' -f1 ) | \
          grep -E -v "useless|mask_manager|gamma" | sort | uniq --unique | \
          while read file; do BN=$(basename $(basename $file .cc) .c); \
            echo ${BN:0:16} ; done | xargs echo | sed 's/ /|/g'
  */

  // all modules
  gchar *tx = NULL;
  tx = dt_util_dstrcat(tx, "ꬹ1ꬹ%s|%s||%s", C_("modulegroup", "base"), "basic",
                       "basecurve|basicadj|clipping|colisa|colorreconstruct|demosaic|exposure|finalscale"
                       "|flip|highlights|invert|negadoctor|overexposed|rawoverexposed|rawprepare"
                       "|shadhi|temperature|toneequal");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "tone"),
                       "tone", "bilat|filmicrgb|globaltonemap|levels"
                       "|relight|rgbcurve|rgblevels|tonecurve|tonemap|zonesystem");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "color"), "color",
                       "channelmixer|colorbalance|colorchecker|colorcontrast|colorcorrection"
                       "|colorin|colorout|colorzones|lut3d|monochrome|profile_gamma|velvia|vibrance");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "correct"), "correct",
                       "ashift|atrous|bilateral|cacorrect|defringe|denoiseprofile|dither"
                       "|hazeremoval|hotpixels|lens|liquify|nlmeans|rawdenoise|retouch|rotatepixels"
                       "|scalepixels|sharpen|spots");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "effect"), "effect",
                       "bloom|borders|colorize|colormapping|graduatednd|grain|highpass|lowlight"
                       "|lowpass|soften|splittoning|vignette|watermark");
  dt_lib_presets_add(_("all modules"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);
  g_free(tx);

  // minimal / 3 tabs
  tx = NULL;
  tx = dt_util_dstrcat(tx, "ꬹ1ꬹ%s|%s||%s", C_("modulegroup", "base"), "basic",
                       "basicadj|ashift|basecurve|clipping"
                       "|denoiseprofile|exposure|flip|lens|temperature");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "grading"), "grading",
                       "channelmixer|colorbalance|colorzones|graduatednd|rgbcurve"
                       "|rgblevels|splittoning");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "effects"), "effect",
                       "bordersmonochrome|retouch|sharpen|vignette|watermark");
  dt_lib_presets_add(_("minimal"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);
  g_free(tx);

  // display referred
  tx = NULL;
  tx = dt_util_dstrcat(tx, "ꬹ1ꬹ%s|%s||%s", C_("modulegroup", "base"), "basic",
                       "basecurve|toneequal|clipping|flip|exposure|temperature"
                       "|rgbcurve|rgblevels|bilat|shadhi|highlights");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "color"), "color",
                       "channelmixer|colorbalance|colorcorrection|colorzones|monochrome|velvia|vibrance");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "correct"), "correct",
                       "ashift|cacorrect|defringe|denoiseprofile|hazeremoval|hotpixels"
                       "|lens|retouch|liquify|sharpen|nlmeans");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "effect"), "effect",
                       "borders|colorize|graduatednd|grain|splittoning|vignette|watermark");
  dt_lib_presets_add(_("display referred"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);
  g_free(tx);

  // scene referred
  tx = NULL;
  tx = dt_util_dstrcat(tx, "ꬹ1ꬹ%s|%s||%s", C_("modulegroup", "base"), "basic",
                       "filmicrgb|toneequal|clipping|flip|exposure|temperature|rgbcurve|rgblevels|bilat");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "color"), "color",
                       "channelmixer|colorbalance|colorzones|vibrance");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "correct"), "correct",
                       "ashift|cacorrect|defringe|denoiseprofile|hazeremoval|hotpixels"
                       "|lens|retouch|liquify|sharpen|nlmeans");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "effect"), "effect",
                       "borders|colorize|graduatednd|grain|splittoning|vignette|watermark");
  dt_lib_presets_add(_("scene referred"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);
  g_free(tx);

  // default / 3 tabs based on Aurélien's proposal
  tx = NULL;
  tx = dt_util_dstrcat(tx, "ꬹ1ꬹ%s|%s||%s", C_("modulegroup", "technical"), "technical",
                       "ashift|basecurve|bilateral|cacorrect|clipping|colorchecker|colorin|colorout"
                       "|colorreconstruct|defringe|demosaic|denoiseprofile|dither|exposure"
                       "|filmicrgb|finalscale|flip|hazeremoval|highlights|hotpixels|invert|lens"
                       "|lut3d|negadoctor|nlmeans|overexposed|rawdenoise"
                       "|rawoverexposed|rotatepixels|scalepixels");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "grading"), "grading",
                       "basicadj|channelmixer|colisa|colorbalance|colorcontrast|colorcorrection"
                       "|colorize|colorzones|globaltonemap|graduatednd|levels|relight|rgbcurve"
                       "|rgblevels|shadhi|splittoning|temperature|tonecurve|toneequal|tonemap"
                       "|velvia|vibrance|zonesystem");
  tx = dt_util_dstrcat(tx, "ꬹ%s|%s||%s", C_("modulegroup", "effects"), "effect",
                       "atrous|bilat|bloom|borders|clahe|colormapping"
                       "|grain|highpass|liquify|lowlight|lowpass|monochrome|retouch|sharpen"
                       "|soften|spots|vignette|watermark");
  dt_lib_presets_add(_("default"), self->plugin_name, self->version(), tx, strlen(tx), TRUE);
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

static void _manage_editor_groups_cleanup(GList **groups)
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
  _manage_editor_groups_cleanup(&d->groups);

  d->groups = _preset_from_string((char *)params);

  gchar *tx = dt_util_dstrcat(NULL, "plugins/darkroom/%s/last_preset", self->plugin_name);
  dt_conf_set_string("plugins/darkroom/modulegroups_preset", dt_conf_get_string(tx));
  g_free(tx);

  _buttons_update(self);
  return 0;
}

static void _manage_editor_save(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(!d->edit_groups || !d->edit_preset) return;

  // get all the values
  gchar *params = _preset_to_string(d->edit_groups);
  gchar *newname = g_strdup(gtk_entry_get_text(GTK_ENTRY(d->preset_name)));

  // update the preset in the database
  dt_lib_presets_update(d->edit_preset, self->plugin_name, self->version(), newname, "", params, strlen(params));
  g_free(params);

  // if name has changed, we need to reflect the change on the presets list too
  _manage_preset_update_list(self);

  // update groups
  gchar *preset = dt_conf_get_string("plugins/darkroom/modulegroups_preset");
  if(g_strcmp0(preset, newname) == 0)
  {
    // if name has changed, let's update it
    if(g_strcmp0(d->edit_preset, newname) != 0)
      dt_conf_set_string("plugins/darkroom/modulegroups_preset", newname);
    // and we update the gui
    if(!dt_lib_presets_apply(newname, self->plugin_name, self->version()))
      dt_lib_presets_apply((gchar *)C_("modulegroup", FALLBACK_PRESET_NAME),
                           self->plugin_name, self->version());
  }
  g_free(preset);
  g_free(newname);
}

static void _manage_editor_module_remove(GtkWidget *widget, GdkEventButton *event, dt_lib_modulegroups_group_t *gr)
{
  char *module = (char *)g_object_get_data(G_OBJECT(widget), "module_name");
  GList *l = gr->modules;
  while(l)
  {
    char *tx = (char *)l->data;
    if(g_strcmp0(tx, module) == 0)
    {
      g_free(l->data);
      gr->modules = g_list_delete_link(gr->modules, l);
      gtk_widget_destroy(gtk_widget_get_parent(widget));
      break;
    }
    l = g_list_next(l);
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
static void _manage_editor_module_update_list(dt_lib_modulegroups_group_t *gr, int ro)
{
  // first, we remove all existing modules
  GList *lw = gtk_container_get_children(GTK_CONTAINER(gr->iop_box));
  while(lw)
  {
    GtkWidget *w = (GtkWidget *)lw->data;
    gtk_widget_destroy(w);
    lw = g_list_next(lw);
  }

  // and we add the ones from the list
  GList *modules2 = g_list_last(darktable.develop->iop);
  while(modules2)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules2->data);
    if(g_list_find_custom(gr->modules, module->op, _iop_compare) && !dt_iop_is_hidden(module))
    {
      // we want to avoid showing multiple instances of the same module
      if(module->multi_priority <= 0
         || g_list_find_custom(darktable.develop->iop, module, _manage_editor_module_find_multi) == NULL)
      {
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_name(hb, "modulegroups-iop-header");
        GtkWidget *lb = gtk_label_new(module->name());
        gtk_widget_set_name(lb, "iop-panel-label");
        gtk_box_pack_start(GTK_BOX(hb), lb, FALSE, TRUE, 0);
        if(!ro)
        {
          GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT, NULL);
          gtk_widget_set_name(btn, "module-reset-button");
          gtk_widget_set_tooltip_text(btn, _("remove this module"));
          g_object_set_data(G_OBJECT(btn), "module_name", module->op);
          g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_module_remove), gr);
          gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, TRUE, 0);
        }
        gtk_box_pack_start(GTK_BOX(gr->iop_box), hb, FALSE, TRUE, 0);
      }
    }
    modules2 = g_list_previous(modules2);
  }

  gtk_widget_show_all(gr->iop_box);
}

static void _manage_editor_group_update_arrows(GtkWidget *box)
{
  // we go throw all group collumns
  GList *lw = gtk_container_get_children(GTK_CONTAINER(box));
  int pos = 0;
  const int max = g_list_length(lw) - 1;
  while(lw)
  {
    GtkWidget *w = (GtkWidget *)lw->data;
    GtkWidget *hb = (GtkWidget *)g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(w)), 0);
    if(hb)
    {
      GList *lw2 = gtk_container_get_children(GTK_CONTAINER(hb));
      if(g_list_length(lw2) > 2)
      {
        GtkWidget *left = (GtkWidget *)g_list_nth_data(lw2, 0);
        GtkWidget *right = (GtkWidget *)g_list_nth_data(lw2, 2);
        if(pos == 0)
          gtk_widget_hide(left);
        else
          gtk_widget_show(left);
        if(pos == max)
          gtk_widget_hide(right);
        else
          gtk_widget_show(right);
      }
    }
    lw = g_list_next(lw);
    pos++;
  }
}

static void _manage_editor_module_add(GtkWidget *widget, gpointer data)
{
  gchar *module = (gchar *)g_object_get_data(G_OBJECT(widget), "module_op");
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");
  if(g_strcmp0(module, "") == 0) return;

  if(!g_list_find_custom(gr->modules, module, _iop_compare))
  {
    gr->modules = g_list_append(gr->modules, g_strdup(module));
    _manage_editor_module_update_list(gr, 0);
    GtkWidget *pop = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "popup");
    gtk_widget_destroy(pop);
  }
}

static int _manage_editor_module_add_sort(gconstpointer a, gconstpointer b)
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
  return res;
}
static void _manage_editor_module_add_popup(GtkWidget *widget, gpointer data)
{
  dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)g_object_get_data(G_OBJECT(widget), "group");

  GtkWidget *pop = gtk_popover_new(widget);
  gtk_widget_set_name(pop, "modulegroups-iop-popup");
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *vb1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *vb2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *lb = NULL;

  int rec_nb = 0;

  GList *m2 = g_list_copy(g_list_first(darktable.iop));
  GList *modules = g_list_sort(m2, _manage_editor_module_add_sort);
  while(modules)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);

    if(!dt_iop_so_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      if(!g_list_find_custom(gr->modules, module->op, _iop_compare))
      {
        GtkWidget *bt = gtk_button_new_with_label(module->name());
        gtk_widget_set_name(bt, "modulegroups-iop-popup-name");
        g_object_set_data(G_OBJECT(bt), "module_op", g_strdup(module->op));
        g_object_set_data(G_OBJECT(bt), "group", gr);
        g_object_set_data(G_OBJECT(bt), "popup", pop);
        gtk_label_set_xalign(GTK_LABEL(gtk_bin_get_child(GTK_BIN(bt))), 1.0);
        g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_module_add), NULL);

        // does it belong to recommended modules ?
        GtkWidget *vbc = vb2;
        if(((module->default_group() & IOP_GROUP_BASIC) && g_strcmp0(gr->name, _("base")) == 0)
           || ((module->default_group() & IOP_GROUP_COLOR) && g_strcmp0(gr->name, _("color")) == 0)
           || ((module->default_group() & IOP_GROUP_CORRECT) && g_strcmp0(gr->name, _("correct")) == 0)
           || ((module->default_group() & IOP_GROUP_TONE) && g_strcmp0(gr->name, _("tone")) == 0)
           || ((module->default_group() & IOP_GROUP_EFFECT) && g_strcmp0(gr->name, _("effect")) == 0)
           || ((module->default_group() & IOP_GROUP_TECHNICAL) && g_strcmp0(gr->name, _("technical")) == 0)
           || ((module->default_group() & IOP_GROUP_GRADING) && g_strcmp0(gr->name, _("grading")) == 0)
           || ((module->default_group() & IOP_GROUP_EFFECTS) && g_strcmp0(gr->name, _("effects")) == 0))
        {
          vbc = vb1;
          rec_nb++;
        }
        // gtk_container_add(GTK_CONTAINER(ev), lb);
        gtk_box_pack_start(GTK_BOX(vbc), bt, FALSE, TRUE, 0);
      }
    }
    modules = g_list_next(modules);
  }
  g_list_free(m2);

  if(rec_nb > 0)
  {
    // we show the list of recommended modules
    lb = gtk_label_new(_("recommended"));
    gtk_label_set_xalign(GTK_LABEL(lb), 0);
    gtk_widget_set_name(lb, "modulegroups_iop_title");
    gtk_box_pack_start(GTK_BOX(vb), lb, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vb), vb1, FALSE, TRUE, 0);
    // and the title for the other modules
    lb = gtk_label_new(_("other"));
    gtk_label_set_xalign(GTK_LABEL(lb), 0);
    gtk_widget_set_name(lb, "modulegroups_iop_title");
    gtk_box_pack_start(GTK_BOX(vb), lb, FALSE, TRUE, 0);
  }
  // we now show all other modules
  gtk_box_pack_start(GTK_BOX(vb), vb2, FALSE, TRUE, 0);
  gtk_widget_set_size_request(sw, 250, 450);
  gtk_container_add(GTK_CONTAINER(sw), vb);
  gtk_container_add(GTK_CONTAINER(pop), sw);
  gtk_widget_show_all(pop);
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

  // we move the group in the ui
  gtk_box_reorder_child(GTK_BOX(gtk_widget_get_parent(vb)), vb, pos - 1);
  // and we update arrows
  _manage_editor_group_update_arrows(gtk_widget_get_parent(vb));
}

static void _manage_editor_group_remove(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
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
  char *ic = (char *)g_object_get_data(G_OBJECT(widget), "ic_name");
  g_free(gr->icon);
  gr->icon = g_strdup(ic);
  GtkWidget *pop = gtk_widget_get_parent(gtk_widget_get_parent(widget));
  GtkWidget *btn = gtk_popover_get_relative_to(GTK_POPOVER(pop));
  dtgtk_button_set_paint(DTGTK_BUTTON(btn), _buttons_get_icon_fct(ic), CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT,
                         NULL);
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
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_basic, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("basic icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "basic");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_active, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("active icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "active");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_color, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("color icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "color");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_correct, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("correct icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "correct");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_effect, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("effect icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "effect");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_favorites, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("favorites icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "favorites");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_tone, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("tone icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "tone");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_grading, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("grading icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "grading");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ic = dtgtk_button_new(dtgtk_cairo_paint_modulegroup_technical, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("technical icon")), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(eb), "ic_name", "technical");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_manage_editor_group_icon_changed), gr);
  gtk_container_add(GTK_CONTAINER(eb), hb);
  gtk_box_pack_start(GTK_BOX(vb), eb, FALSE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(pop), vb);
  gtk_widget_show_all(pop);
}

static GtkWidget *_manage_editor_group_init_modules_box(dt_lib_module_t *self, dt_lib_modulegroups_group_t *gr,
                                                        int ro)
{
  GtkWidget *vb2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(vb2, "modulegroups-groupbox");
  // line to edit the group
  GtkWidget *hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb2, "modulegroups-header");

  // left arrow (not if pos == 0 which means this is the first group)
  GtkWidget *btn = NULL;
  if(!ro)
  {
    btn = dtgtk_button_new(dtgtk_cairo_paint_arrow, CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_RIGHT | CPF_STYLE_FLAT,
                           NULL);
    gtk_widget_set_tooltip_text(btn, _("move group to the left"));
    g_object_set_data(G_OBJECT(btn), "group", gr);
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_move_left), self);
    gtk_box_pack_start(GTK_BOX(hb2), btn, FALSE, TRUE, 0);
  }

  GtkWidget *hb3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb3, "modulegroups-header-center");
  gtk_widget_set_hexpand(hb3, TRUE);

  btn = dtgtk_button_new(_buttons_get_icon_fct(gr->icon), CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_name(btn, "modulegroups-group-icon");
  gtk_widget_set_tooltip_text(btn, _("group icon"));
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_icon_popup), self);
  g_object_set_data(G_OBJECT(btn), "group", gr);
  gtk_box_pack_start(GTK_BOX(hb3), btn, FALSE, TRUE, 0);

  GtkWidget *tb = gtk_entry_new();
  gtk_widget_set_tooltip_text(tb, _("group name"));
  g_object_set_data(G_OBJECT(tb), "group", gr);
  g_signal_connect(G_OBJECT(tb), "changed", G_CALLBACK(_manage_editor_group_name_changed), self);
  gtk_entry_set_text(GTK_ENTRY(tb), gr->name);
  gtk_box_pack_start(GTK_BOX(hb3), tb, TRUE, TRUE, 0);

  if(!ro)
  {
    btn = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_DO_NOT_USE_BORDER | CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(btn, _("remove group"));
    g_object_set_data(G_OBJECT(btn), "group", gr);
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_remove), self);
    gtk_box_pack_end(GTK_BOX(hb3), btn, FALSE, TRUE, 0);
  }

  gtk_box_pack_start(GTK_BOX(hb2), hb3, FALSE, TRUE, 0);

  // right arrow (not if pos == -1 which means this is the last group)
  if(!ro)
  {
    btn = dtgtk_button_new(dtgtk_cairo_paint_arrow, CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_LEFT | CPF_STYLE_FLAT,
                           NULL);
    gtk_widget_set_tooltip_text(btn, _("move group to the right"));
    g_object_set_data(G_OBJECT(btn), "group", gr);
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_editor_group_move_right), self);
    gtk_box_pack_end(GTK_BOX(hb2), btn, FALSE, TRUE, 0);
  }

  gtk_box_pack_start(GTK_BOX(vb2), hb2, FALSE, TRUE, 0);

  // choosen modules
  GtkWidget *vb3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gr->iop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  _manage_editor_module_update_list(gr, ro);
  gtk_box_pack_start(GTK_BOX(vb3), gr->iop_box, FALSE, TRUE, 0);

  // '+' button to add new module
  if(!ro)
  {
    GtkWidget *hb4 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *bt = dtgtk_button_new(dtgtk_cairo_paint_plus_simple,
                                     CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_LEFT | CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(bt, _("add module to the list"));
    gtk_widget_set_name(bt, "modulegroups-add-module-btn");
    g_object_set_data(G_OBJECT(bt), "group", gr);
    g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_module_add_popup), gr->modules);
    gtk_widget_set_halign(hb4, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(hb4), bt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb3), hb4, FALSE, FALSE, 0);
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
  GtkWidget *vb2 = _manage_editor_group_init_modules_box(self, gr, 0);
  gtk_box_pack_start(GTK_BOX(d->preset_groups_box), vb2, FALSE, TRUE, 0);
  gtk_widget_show_all(vb2);

  // and we update arrows
  _manage_editor_group_update_arrows(d->preset_groups_box);
}

static void _manage_editor_load(char *preset, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // if we have a currently edited preset, we save it
  if(d->edit_preset && g_strcmp0(preset, d->edit_preset) != 0)
  {
    _manage_editor_save(self);
  }

  // we remove all widgets from the box
  GList *lw = gtk_container_get_children(GTK_CONTAINER(d->preset_box));
  while(lw)
  {
    GtkWidget *w = (GtkWidget *)lw->data;
    gtk_widget_destroy(w);
    lw = g_list_next(lw);
  }

  // we update all the preset lines
  lw = gtk_container_get_children(GTK_CONTAINER(d->presets_list));
  while(lw)
  {
    GtkWidget *w = (GtkWidget *)lw->data;
    char *pr_name = g_strdup((char *)g_object_get_data(G_OBJECT(w), "preset_name"));
    if(g_strcmp0(pr_name, preset) == 0)
      gtk_widget_set_name(w, "modulegroups-preset-activated");
    else if(pr_name)
      gtk_widget_set_name(w, "modulegroups-preset");
    lw = g_list_next(lw);
  }

  // get all presets groups
  if(d->edit_groups) _manage_editor_groups_cleanup(&d->edit_groups);
  if(d->edit_preset) g_free(d->edit_preset);
  d->edit_groups = NULL;
  d->edit_preset = NULL;
  int ro = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT writeprotect, op_params"
      " FROM data.presets"
      " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, preset, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    ro = sqlite3_column_int(stmt, 0);
    const void *blob = sqlite3_column_blob(stmt, 1);
    d->edit_groups = _preset_from_string((char *)blob);
    d->edit_preset = g_strdup(preset);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    return;
  }

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(vb, TRUE);

  // preset name
  GtkWidget *hb1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb1, "modulegroups-preset-name");
  gtk_box_pack_start(GTK_BOX(hb1), gtk_label_new(_("preset name : ")), FALSE, TRUE, 0);
  d->preset_name = gtk_entry_new();
  gtk_widget_set_tooltip_text(d->preset_name, _("preset name"));
  gtk_entry_set_text(GTK_ENTRY(d->preset_name), preset);
  if(ro) gtk_widget_set_sensitive(d->preset_name, FALSE);
  gtk_box_pack_start(GTK_BOX(hb1), d->preset_name, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vb), hb1, FALSE, TRUE, 0);

  hb1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->preset_groups_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hb1, "modulegroups-groups-title");
  gtk_box_pack_start(GTK_BOX(hb1), gtk_label_new(_("module groups")), FALSE, TRUE, 0);
  if(!ro)
  {
    GtkWidget *bt = dtgtk_button_new(dtgtk_cairo_paint_plus_simple,
                                     CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_LEFT | CPF_STYLE_FLAT, NULL);
    g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_group_add), self);
    gtk_box_pack_start(GTK_BOX(hb1), bt, FALSE, FALSE, 0);
  }
  gtk_widget_set_halign(hb1, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(vb), hb1, FALSE, TRUE, 0);

  gtk_widget_set_name(d->preset_groups_box, "modulegroups-groups-box");
  GList *l = d->edit_groups;
  while(l)
  {
    dt_lib_modulegroups_group_t *gr = (dt_lib_modulegroups_group_t *)l->data;
    GtkWidget *vb2 = _manage_editor_group_init_modules_box(self, gr, ro);
    gtk_box_pack_start(GTK_BOX(d->preset_groups_box), vb2, FALSE, TRUE, 0);
    l = g_list_next(l);
  }

  gtk_widget_set_halign(d->preset_groups_box, GTK_ALIGN_CENTER);
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_container_add(GTK_CONTAINER(sw), d->preset_groups_box);
  gtk_box_pack_start(GTK_BOX(vb), sw, TRUE, TRUE, 0);

  // read-only message
  if(ro)
  {
    GtkWidget *lb
        = gtk_label_new(_("this is a built-in read-only preset. duplicate it if you want to make changes"));
    gtk_widget_set_name(lb, "modulegroups-ro");
    gtk_box_pack_start(GTK_BOX(vb), lb, FALSE, TRUE, 0);
  }

  // reset button
  if(!ro)
  {
    hb1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *bt = gtk_button_new();
    gtk_widget_set_name(bt, "modulegroups-reset");
    gtk_button_set_label(GTK_BUTTON(bt), _("reset"));
    g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_editor_reset), self);
    gtk_box_pack_end(GTK_BOX(hb1), bt, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vb), hb1, FALSE, TRUE, 0);
  }

  gtk_container_add(GTK_CONTAINER(d->preset_box), vb);
  gtk_widget_show_all(d->preset_box);

  // and we update arrows
  if(!ro) _manage_editor_group_update_arrows(d->preset_groups_box);
}

static void _manage_preset_change(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  char *preset = g_strdup((char *)g_object_get_data(G_OBJECT(widget), "preset_name"));
  _manage_editor_load(preset, self);
}

static void _manage_preset_add(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
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
        "SELECT name"
        " FROM data.presets"
        " WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, tx, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_ROW) ko = FALSE;
    sqlite3_finalize(stmt);
    g_free(tx);
  }
  gchar *nname = dt_util_dstrcat(NULL, "new_%d", i);

  // and create a new empty preset
  dt_lib_presets_add(nname, self->plugin_name, self->version(), " ", 1, FALSE);

  _manage_preset_update_list(self);

  // and we load the new preset
  _manage_editor_load(nname, self);
  g_free(nname);
}

static void _manage_preset_duplicate(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  char *preset = (char *)g_object_get_data(G_OBJECT(widget), "preset_name");
  gchar *nname = dt_lib_presets_duplicate(preset, self->plugin_name, self->version());

  // reload the window
  _manage_preset_update_list(self);
  // select the duplicated preset
  _manage_editor_load(nname, self);

  g_free(nname);
}

static void _manage_preset_delete(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
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

    // reload presets list
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
    _manage_preset_update_list(self);

    // we try to reload previous selected preset if it still exists
    gboolean sel_ok = FALSE;
    GList *l = gtk_container_get_children(GTK_CONTAINER(d->presets_list));
    while(l)
    {
      GtkWidget *ww = (GtkWidget *)l->data;
      char *tx = g_strdup((char *)g_object_get_data(G_OBJECT(ww), "preset_name"));
      if(g_strcmp0(tx, gtk_entry_get_text(GTK_ENTRY(d->preset_name))) == 0)
      {
        _manage_editor_load(tx, self);
        sel_ok = TRUE;
        break;
      }
      l = g_list_next(l);
    }
    // otherwise we load the first preset
    if(!sel_ok)
    {
      GtkWidget *ww = (GtkWidget *)g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(d->presets_list)), 0);
      if(ww)
      {
        char *firstn = g_strdup((char *)g_object_get_data(G_OBJECT(ww), "preset_name"));
        _manage_editor_load(firstn, self);
      }
    }

    // if the deleted preset was the one currently in use, load default preset
    if(dt_conf_key_exists("plugins/darkroom/modulegroups_preset"))
    {
      gchar *cur = dt_conf_get_string("plugins/darkroom/modulegroups_preset");
      if(g_strcmp0(cur, preset) == 0)
      {
        dt_conf_set_string("plugins/darkroom/modulegroups_preset", C_("modulegroup", FALLBACK_PRESET_NAME));
        dt_lib_presets_apply((gchar *)C_("modulegroup", FALLBACK_PRESET_NAME),
                             self->plugin_name, self->version());
      }
      g_free(cur);
    }
  }
}

static gboolean _manage_preset_hover_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  int flags = gtk_widget_get_state_flags(gtk_widget_get_parent(widget));

  if(event->type == GDK_ENTER_NOTIFY)
    flags |= GTK_STATE_FLAG_PRELIGHT;
  else
    flags &= ~GTK_STATE_FLAG_PRELIGHT;

  gtk_widget_set_state_flags(gtk_widget_get_parent(widget), flags, TRUE);
  return FALSE;
}

static void _manage_preset_update_list(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we first remove all existing entries from the box
  GList *lw = gtk_container_get_children(GTK_CONTAINER(d->presets_list));
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
                              "SELECT name, writeprotect, description"
                              " FROM data.presets"
                              " WHERE operation=?1 AND op_version=?2"
                              " ORDER BY writeprotect DESC, name, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int ro = sqlite3_column_int(stmt, 1);
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(hb, "modulegroups-preset");
    g_object_set_data(G_OBJECT(hb), "preset_name", g_strdup(name));

    // preset label
    GtkWidget *evt = gtk_event_box_new();
    g_object_set_data(G_OBJECT(evt), "preset_name", g_strdup(name));
    g_signal_connect(G_OBJECT(evt), "button-press-event", G_CALLBACK(_manage_preset_change), self);
    g_signal_connect(G_OBJECT(evt), "enter-notify-event", G_CALLBACK(_manage_preset_hover_callback), self);
    g_signal_connect(G_OBJECT(evt), "leave-notify-event", G_CALLBACK(_manage_preset_hover_callback), self);
    GtkWidget *lbl = gtk_label_new(name);
    gtk_widget_set_tooltip_text(lbl, name);
    gtk_widget_set_size_request(lbl, 180, -1);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_container_add(GTK_CONTAINER(evt), lbl);
    gtk_box_pack_start(GTK_BOX(hb), evt, TRUE, TRUE, 0);

    // duplicate button
    GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(btn, _("duplicate this preset"));
    g_object_set_data(G_OBJECT(btn), "preset_name", g_strdup(name));
    g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_preset_duplicate), self);
    gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, FALSE, 0);

    // remove button (not for read-lony presets)
    if(!ro)
    {
      btn = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT, NULL);
      gtk_widget_set_tooltip_text(btn, _("delete this preset"));
      g_object_set_data(G_OBJECT(btn), "preset_name", g_strdup(name));
      g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_manage_preset_delete), self);
      gtk_box_pack_end(GTK_BOX(hb), btn, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(d->presets_list), hb, FALSE, TRUE, 0);
  }
  sqlite3_finalize(stmt);

  // and we finally add the "new preset" button
  GtkWidget *hb2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *bt = dtgtk_button_new(dtgtk_cairo_paint_plus_simple,
                                   CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_LEFT | CPF_STYLE_FLAT, NULL);
  g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_manage_preset_add), self);
  gtk_widget_set_name(bt, "modulegroups-preset-add-btn");
  gtk_widget_set_tooltip_text(bt, _("add new empty preset"));
  gtk_widget_set_halign(hb2, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(hb2), bt, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->presets_list), hb2, FALSE, FALSE, 0);

  gtk_widget_show_all(d->presets_list);
}

static void _manage_editor_destroy(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  // we save the last edited preset
  _manage_editor_save(self);

  // and we free editing data
  if(d->edit_groups) _manage_editor_groups_cleanup(&d->edit_groups);
  if(d->edit_preset) g_free(d->edit_preset);
  d->edit_groups = NULL;
  d->edit_preset = NULL;
}

static void _manage_show_window(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  d->dialog = gtk_dialog_new_with_buttons(_("manage module layouts"), win,
                                          GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, NULL, NULL);

  gtk_window_set_default_size(GTK_WINDOW(d->dialog), DT_PIXEL_APPLY_DPI(1100), DT_PIXEL_APPLY_DPI(700));

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->dialog);
#endif
  gtk_widget_set_name(d->dialog, "modulegroups_manager");
  gtk_window_set_title(GTK_WINDOW(d->dialog), _("manage module layouts"));

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(vb, "modulegroups-presets-list");
  d->preset_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(d->preset_box, "modulegroups_editor");

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  d->presets_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // we load the presets list
  _manage_preset_update_list(self);

  gtk_container_add(GTK_CONTAINER(sw), d->presets_list);
  gtk_box_pack_start(GTK_BOX(vb), sw, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hb), d->preset_box, TRUE, TRUE, 0);
  gtk_widget_show_all(hb);

  // and we select the current one
  gboolean sel_ok = FALSE;
  if(dt_conf_key_exists("plugins/darkroom/modulegroups_preset"))
  {
    gchar *preset = dt_conf_get_string("plugins/darkroom/modulegroups_preset");
    GList *l = gtk_container_get_children(GTK_CONTAINER(d->presets_list));
    while(l)
    {
      GtkWidget *w = (GtkWidget *)l->data;
      char *tx = g_strdup((char *)g_object_get_data(G_OBJECT(w), "preset_name"));
      if(g_strcmp0(tx, preset) == 0)
      {
        _manage_editor_load(preset, self);
        sel_ok = TRUE;
        break;
      }
      l = g_list_next(l);
    }
    g_free(preset);
  }
  // or the first one if no selection found
  if(!sel_ok)
  {
    GtkWidget *w = (GtkWidget *)g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(d->presets_list)), 0);
    if(w)
    {
      char *firstn = g_strdup((char *)g_object_get_data(G_OBJECT(w), "preset_name"));
      _manage_editor_load(firstn, self);
    }
  }

  gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d->dialog))), hb);

  g_signal_connect(d->dialog, "destroy", G_CALLBACK(_manage_editor_destroy), self);
  gtk_window_set_resizable(GTK_WINDOW(d->dialog), TRUE);

  gtk_window_set_position(GTK_WINDOW(d->dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show(d->dialog);
}


void manage_presets(dt_lib_module_t *self)
{
  _manage_show_window(self);
}
#undef PADDING
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
