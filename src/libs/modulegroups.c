/*
    This file is part of darktable,
    copyright (c) 2011-2012 Henrik Andersson.

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
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define PADDING 2

#include "modulegroups.h"

typedef struct dt_lib_modulegroups_t
{
  uint32_t current;
  GtkWidget *buttons[DT_MODULEGROUP_SIZE];
} dt_lib_modulegroups_t;

/* toggle button callback */
static void _lib_modulegroups_toggle(GtkWidget *button, gpointer data);
/* helper function to update iop module view depending on group */
static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self);

/* modulergroups proxy set group function
   \see dt_dev_modulegroups_set()
*/
static void _lib_modulegroups_set(dt_lib_module_t *self, uint32_t group);
/* modulegroups proxy get group function
  \see dt_dev_modulegroups_get()
*/
static uint32_t _lib_modulegroups_get(dt_lib_module_t *self);
/* modulegroups proxy test function.
   tests if iop module group flags matches modulegroup.
*/
static gboolean _lib_modulegroups_test(dt_lib_module_t *self, uint32_t group, uint32_t iop_group);
/* modulegroups proxy switch group function.
   sets the active group which module belongs too.
*/
static void _lib_modulegroups_switch_group(dt_lib_module_t *self, dt_iop_module_t *module);

/* hook up with viewmanager view change to initialize modulegroup */
static void _lib_modulegroups_viewchanged_callback(gpointer instance, dt_view_t *old_view,
                                                   dt_view_t *new_view, gpointer data);

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

int _iop_get_group_order(const int group_id, const int default_order)
{
  if (group_id < DT_MODULEGROUP_BASIC)
    return group_id;

  gchar *key = dt_util_dstrcat(NULL, "plugins/darkroom/group_order/%d", group_id - 1);
  int prefs = dt_conf_get_int(key);

  /* if zero, not found, record it */
  if (!prefs)
  {
    dt_conf_set_int(key, default_order - 1);
    prefs = default_order;
  }
  else
    prefs += 1;

  g_free(key);
  return prefs<1 ? 1 : (prefs>DT_MODULEGROUP_SIZE? DT_MODULEGROUP_SIZE: prefs);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)g_malloc0(sizeof(dt_lib_modulegroups_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  dtgtk_cairo_paint_flags_t pf = CPF_STYLE_FLAT;

  /* active */
  d->buttons[DT_MODULEGROUP_ACTIVE_PIPE] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_active, pf, NULL);
  g_signal_connect(d->buttons[DT_MODULEGROUP_ACTIVE_PIPE], "toggled", G_CALLBACK(_lib_modulegroups_toggle),
                   self);
  gtk_widget_set_tooltip_text(d->buttons[DT_MODULEGROUP_ACTIVE_PIPE], _("show only active modules"));

  /* favorites */
  d->buttons[DT_MODULEGROUP_FAVORITES] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_favorites, pf, NULL);
  g_signal_connect(d->buttons[DT_MODULEGROUP_FAVORITES], "toggled", G_CALLBACK(_lib_modulegroups_toggle),
                   self);
  gtk_widget_set_tooltip_text(d->buttons[DT_MODULEGROUP_FAVORITES],
                              _("show only your favourite modules (selected in `more modules' below)"));

  /* basic */
  int g_index = _iop_get_group_order(DT_MODULEGROUP_BASIC, DT_MODULEGROUP_BASIC);
  d->buttons[g_index] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_basic, pf, NULL);
  g_signal_connect(d->buttons[g_index], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->buttons[g_index], _("basic group"));

  /* tone */
  g_index = _iop_get_group_order(DT_MODULEGROUP_TONE, DT_MODULEGROUP_TONE);
  d->buttons[g_index] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_tone, pf, NULL);
  g_signal_connect(d->buttons[g_index], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->buttons[g_index], _("tone group"));

  /* color */
  g_index = _iop_get_group_order(DT_MODULEGROUP_COLOR, DT_MODULEGROUP_COLOR);
  d->buttons[g_index] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_color, pf, NULL);
  g_signal_connect(d->buttons[g_index], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->buttons[g_index], _("color group"));

  /* correct */
  g_index = _iop_get_group_order(DT_MODULEGROUP_CORRECT, DT_MODULEGROUP_CORRECT);
  d->buttons[g_index] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_correct, pf, NULL);
  g_signal_connect(d->buttons[g_index], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->buttons[g_index], _("correction group"));

  /* effect */
  g_index = _iop_get_group_order(DT_MODULEGROUP_EFFECT, DT_MODULEGROUP_EFFECT);
  d->buttons[g_index] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_effect, pf, NULL);
  g_signal_connect(d->buttons[g_index], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  gtk_widget_set_tooltip_text(d->buttons[g_index], _("effects group"));

  /* minimize table height before adding the buttons */
  gtk_widget_set_size_request(self->widget, -1, -1);

  /*
   * layout button row
   */
  int iconsize = DT_PIXEL_APPLY_DPI(28);
  GtkWidget *br = self->widget;
  for(int k = 0; k < DT_MODULEGROUP_SIZE; k++)
  {
    gtk_widget_set_size_request(d->buttons[k], iconsize, iconsize);
    gtk_box_pack_start(GTK_BOX(br), d->buttons[k], TRUE, TRUE, 0);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->buttons[d->current]), TRUE);
  gtk_widget_show_all(self->widget);

  /*
   * set the proxy functions
   */
  darktable.develop->proxy.modulegroups.module = self;
  darktable.develop->proxy.modulegroups.set = _lib_modulegroups_set;
  darktable.develop->proxy.modulegroups.get = _lib_modulegroups_get;
  darktable.develop->proxy.modulegroups.test = _lib_modulegroups_test;
  darktable.develop->proxy.modulegroups.switch_group = _lib_modulegroups_switch_group;

  /* let's connect to view changed signal to set default group */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(_lib_modulegroups_viewchanged_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* let's not listen to signals anymore.. */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_modulegroups_viewchanged_callback), self);

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

static gboolean _lib_modulegroups_test_internal(dt_lib_module_t *self, uint32_t group, uint32_t iop_group)
{
  if(iop_group & IOP_SPECIAL_GROUP_ACTIVE_PIPE && group == DT_MODULEGROUP_ACTIVE_PIPE)
    return TRUE;
  else if(iop_group & IOP_SPECIAL_GROUP_USER_DEFINED && group == DT_MODULEGROUP_FAVORITES)
    return TRUE;
  else if(iop_group & IOP_GROUP_BASIC && group == DT_MODULEGROUP_BASIC)
    return TRUE;
  else if(iop_group & IOP_GROUP_TONE && group == DT_MODULEGROUP_TONE)
    return TRUE;
  else if(iop_group & IOP_GROUP_COLOR && group == DT_MODULEGROUP_COLOR)
    return TRUE;
  else if(iop_group & IOP_GROUP_CORRECT && group == DT_MODULEGROUP_CORRECT)
    return TRUE;
  else if(iop_group & IOP_GROUP_EFFECT && group == DT_MODULEGROUP_EFFECT)
    return TRUE;
  return FALSE;
}

static gboolean _lib_modulegroups_test(dt_lib_module_t *self, uint32_t group, uint32_t iop_group)
{
  return _lib_modulegroups_test_internal(self, _iop_get_group_order(group, group), iop_group);
}

static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

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

      /* skip modules without an gui */
      if(dt_iop_is_hidden(module)) continue;

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

        case DT_MODULEGROUP_FAVORITES:
        {
          if(module->so->state == dt_iop_state_FAVORITE)
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
          if(_lib_modulegroups_test_internal(self, d->current, module->groups())
             && module->so->state != dt_iop_state_HIDDEN
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
}

static void _lib_modulegroups_toggle(GtkWidget *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  /* block all button callbacks */
  for(int k = 0; k < DT_MODULEGROUP_SIZE; k++)
    g_signal_handlers_block_matched(d->buttons[k], G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _lib_modulegroups_toggle,
                                    NULL);

  /* deactivate all buttons */
  uint32_t cb = 0;
  int gid = 0;
  for(int k = 0; k < DT_MODULEGROUP_SIZE; k++)
  {
    /* store toggled modulegroup */
    if(d->buttons[k] == button)
    {
      cb = k;
      gid = _iop_get_group_order(k, k);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->buttons[k]), FALSE);
  }

  if(d->current == gid)
    d->current = DT_MODULEGROUP_NONE;
  else
  {
    d->current = gid;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->buttons[cb]), TRUE);
  }

  /* unblock all button callbacks */
  for(int k = 0; k < DT_MODULEGROUP_SIZE; k++)
    g_signal_handlers_unblock_matched(d->buttons[k], G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                      _lib_modulegroups_toggle, NULL);

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

  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)params->self->data;

  const int group = _iop_get_group_order(params->group, params->group);

  /* if no change just update visibility */
  if(d->current == group)
  {
    _lib_modulegroups_update_iop_visibility(params->self);
    free(params);
    return FALSE;
  }

  /* set current group */
  if(params->group < DT_MODULEGROUP_SIZE && GTK_IS_TOGGLE_BUTTON(d->buttons[params->group]))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->buttons[params->group]), TRUE);

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

static void _lib_modulegroups_switch_group(dt_lib_module_t *self, dt_iop_module_t *module)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  /* do nothing if module is member of current group */
  if(_lib_modulegroups_test_internal(self, d->current, module->groups())) return;

  /* lets find the group which is not favorite/active pipe */
  for(int k = DT_MODULEGROUP_BASIC; k < DT_MODULEGROUP_SIZE; k++)
  {
    if(_lib_modulegroups_test(self, k, module->groups()))
    {
      _lib_modulegroups_set(self, k);
      return;
    }
  }
}

static uint32_t _lib_modulegroups_get(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  for(int k = 0; k < DT_MODULEGROUP_SIZE; k++)
  {
    if (d->current == _iop_get_group_order(k, k))
      return k;
  }
  return DT_MODULEGROUP_FAVORITES;
}

#undef PADDING
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
