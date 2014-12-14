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
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"

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

const char *name()
{
  return _("modulegroups");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}


/* this module should always be shown without expander */
int expandable()
{
  return 0;
}

int position()
{
  return 999;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)g_malloc0(sizeof(dt_lib_modulegroups_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  dtgtk_cairo_paint_flags_t pf = CPF_STYLE_FLAT;

  /* favorites */
  d->buttons[DT_MODULEGROUP_FAVORITES] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_favorites, pf);
  g_signal_connect(d->buttons[DT_MODULEGROUP_FAVORITES], "toggled", G_CALLBACK(_lib_modulegroups_toggle),
                   self);
  g_object_set(d->buttons[DT_MODULEGROUP_FAVORITES], "tooltip-text",
               _("show only your favourite modules (selected in `more modules' below)"), (char *)NULL);

  /* active */
  d->buttons[DT_MODULEGROUP_ACTIVE_PIPE] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_active, pf);
  g_signal_connect(d->buttons[DT_MODULEGROUP_ACTIVE_PIPE], "toggled", G_CALLBACK(_lib_modulegroups_toggle),
                   self);
  g_object_set(d->buttons[DT_MODULEGROUP_ACTIVE_PIPE], "tooltip-text", _("show only active modules"),
               (char *)NULL);

  /* basic */
  d->buttons[DT_MODULEGROUP_BASIC] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_basic, pf);
  g_signal_connect(d->buttons[DT_MODULEGROUP_BASIC], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  g_object_set(d->buttons[DT_MODULEGROUP_BASIC], "tooltip-text", _("basic group"), (char *)NULL);

  /* correct */
  d->buttons[DT_MODULEGROUP_CORRECT] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_correct, pf);
  g_signal_connect(d->buttons[DT_MODULEGROUP_CORRECT], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  g_object_set(d->buttons[DT_MODULEGROUP_CORRECT], "tooltip-text", _("correction group"), (char *)NULL);

  /* color */
  d->buttons[DT_MODULEGROUP_COLOR] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_color, pf);
  g_signal_connect(d->buttons[DT_MODULEGROUP_COLOR], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  g_object_set(d->buttons[DT_MODULEGROUP_COLOR], "tooltip-text", _("color group"), (char *)NULL);

  /* tone */
  d->buttons[DT_MODULEGROUP_TONE] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_tone, pf);
  g_signal_connect(d->buttons[DT_MODULEGROUP_TONE], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  g_object_set(d->buttons[DT_MODULEGROUP_TONE], "tooltip-text", _("tone group"), (char *)NULL);

  /* effect */
  d->buttons[DT_MODULEGROUP_EFFECT] = dtgtk_togglebutton_new(dtgtk_cairo_paint_modulegroup_effect, pf);
  g_signal_connect(d->buttons[DT_MODULEGROUP_EFFECT], "toggled", G_CALLBACK(_lib_modulegroups_toggle), self);
  g_object_set(d->buttons[DT_MODULEGROUP_EFFECT], "tooltip-text", _("effects group"), (char *)NULL);

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

static gboolean _lib_modulegroups_test(dt_lib_module_t *self, uint32_t group, uint32_t iop_group)
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
            gtk_widget_show(w);
          else
          {
            if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
            gtk_widget_hide(w);
          }
        }
        break;

        case DT_MODULEGROUP_FAVORITES:
        {
          if(module->state == dt_iop_state_FAVORITE)
            gtk_widget_show(w);
          else
          {
            if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
            gtk_widget_hide(w);
          }
        }
        break;

        case DT_MODULEGROUP_NONE:
        {
          /* show all except hidden ones */
          if((module->state != dt_iop_state_HIDDEN || module->enabled)
             && (!(module->flags() & IOP_FLAGS_DEPRECATED)))
            gtk_widget_show(w);
          else
          {
            if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
            gtk_widget_hide(w);
          }
        }
        break;

        default:
        {
          if(_lib_modulegroups_test(self, d->current, module->groups())
             && module->state != dt_iop_state_HIDDEN
             && (!(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled))
            gtk_widget_show(w);
          else
          {
            if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
            gtk_widget_hide(w);
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
  for(int k = 0; k < DT_MODULEGROUP_SIZE; k++)
  {
    /* store toggled modulegroup */
    if(d->buttons[k] == button) cb = k;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->buttons[k]), FALSE);
  }


  if(d->current == cb)
    d->current = DT_MODULEGROUP_NONE;
  else
  {
    d->current = cb;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->buttons[cb]), TRUE);
  }

  /* unblock all button callbacks */
  for(int k = 0; k < DT_MODULEGROUP_SIZE; k++)
    g_signal_handlers_unblock_matched(d->buttons[k], G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                      _lib_modulegroups_toggle, NULL);

  /* update visibility */
  _lib_modulegroups_update_iop_visibility(self);
}

static void _lib_modulegroups_set(dt_lib_module_t *self, uint32_t group)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  /* this is a proxy function so it might be called from another thread */
  gboolean i_own_lock = dt_control_gdk_lock();

  /* if no change just update visibility */
  if(d->current == group)
  {
    _lib_modulegroups_update_iop_visibility(self);
    return;
  }

  /* set current group */
  if(group < DT_MODULEGROUP_SIZE && GTK_IS_TOGGLE_BUTTON(d->buttons[group]))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->buttons[group]), TRUE);

  if(i_own_lock) dt_control_gdk_unlock();
}

static void _lib_modulegroups_switch_group(dt_lib_module_t *self, dt_iop_module_t *module)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  /* do nothing if module is member of current group */
  if(_lib_modulegroups_test(self, d->current, module->groups())) return;

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
  return d->current;
}

#undef PADDING
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
