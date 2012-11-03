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
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/icon.h"
#include "dtgtk/label.h"
#include "gui/draw.h"

DT_MODULE(1)

#define DT_MODULE_LIST_SPACING 2

typedef struct dt_lib_modulelist_t
{
  GtkList *list;
}
dt_lib_modulelist_t;

/* handle iop module click */
static void _lib_modulelist_row_changed_callback(GtkWidget *,GdkEvent *unused, gpointer user_data);
/* callback for iop modules loaded signal */
static void _lib_modulelist_populate_callback(gpointer instance, gpointer user_data);

const char* name()
{
  return _("more modules");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM;
}

int position()
{
  return 1;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulelist_t *d = (dt_lib_modulelist_t *)g_malloc(sizeof(dt_lib_modulelist_t));
  memset(d,0,sizeof(dt_lib_modulelist_t));
  self->data = (void *)d;
  self->widget = gtk_scrolled_window_new(NULL, NULL); //GTK_ADJUSTMENT(gtk_adjustment_new(200, 100, 200, 10, 100, 100))
  gtk_widget_set_size_request(self->widget, -1, 200);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->widget), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  d->list = GTK_LIST(gtk_list_new());
  gtk_widget_set_size_request(GTK_WIDGET(d->list), 50, -1);
  gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(self->widget), GTK_WIDGET(d->list));

  /* connect to signal for darktable.develop initialization */
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_DEVELOP_INITIALIZE,G_CALLBACK(_lib_modulelist_populate_callback),self);

}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals,G_CALLBACK(_lib_modulelist_populate_callback),self);
  g_free(self->data);
  self->data = NULL;
}

static gboolean _lib_modulelist_row_set_state(gint state,dt_iop_module_t *module)
{
  char option[1024];
  gboolean expand = FALSE;
  module->state = state%dt_iop_state_LAST;
  if(module->state==dt_iop_state_HIDDEN)
  {
    /* module is hidden lets set conf values */
    gtk_widget_hide(GTK_WIDGET(module->expander));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, FALSE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, FALSE);
    GtkWidget *icon = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->state_widget)),1);
    dtgtk_icon_set_paint(icon,dtgtk_cairo_paint_empty,CPF_STYLE_FLAT);
    icon = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->state_widget)),0);
    dtgtk_label_set_text(DTGTK_LABEL(icon),module->name(),DARKTABLE_LABEL_ALIGN_LEFT);

    /* construct tooltip text into option */
    snprintf(option, 512, _("show %s"), module->name());
  }
  else if(module->state==dt_iop_state_ACTIVE)
  {
    /* module is shown lets set conf values */
    // FIXME
    // dt_gui_iop_modulegroups_switch(module->groups());
    gtk_widget_show(GTK_WIDGET(module->expander));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, TRUE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, FALSE);
    GtkWidget *icon = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->state_widget)),1);
    dtgtk_icon_set_paint(icon,dtgtk_cairo_paint_empty,CPF_STYLE_FLAT | CPF_ACTIVE);
    icon = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->state_widget)),0);
    dtgtk_label_set_text(DTGTK_LABEL(icon),module->name(),DARKTABLE_LABEL_ALIGN_LEFT | DARKTABLE_LABEL_BACKFILLED);

    expand = TRUE;

    /* construct tooltip text into option */
    snprintf(option, 512, _("%s as favorite"), module->name());
  }
  else if(module->state==dt_iop_state_FAVORITE)
  {
    /* module is shown and favorite lets set conf values */
    // FIXME
    // dt_gui_iop_modulegroups_switch(module->groups());
    gtk_widget_show(GTK_WIDGET(module->expander));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, TRUE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, TRUE);
    GtkWidget *icon = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->state_widget)),1);
    dtgtk_icon_set_paint(icon,dtgtk_cairo_paint_modulegroup_favorites,CPF_STYLE_FLAT | CPF_ACTIVE);
    icon = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->state_widget)),0);
    dtgtk_label_set_text(DTGTK_LABEL(icon),module->name(),DARKTABLE_LABEL_ALIGN_LEFT | DARKTABLE_LABEL_BACKFILLED);

    expand = TRUE;

    /* construct tooltip text into option */
    snprintf(option, 512, _("hide %s"), module->name());
  }

  //g_object_set(G_OBJECT(module->state_widget), "tooltip-text", option, (char *)NULL);
  return expand;
}

static void _lib_modulelist_populate_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  /* go thru list of iop modules and add them to the list */
  GList *modules = g_list_last(darktable.develop->iop);
 
  GList *rows = NULL;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(!dt_iop_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      module->state_widget = gtk_table_new(1,3,FALSE);
      char filename[DT_MAX_PATH_LEN], datadir[DT_MAX_PATH_LEN];
      dt_loc_get_datadir(datadir, DT_MAX_PATH_LEN);
      snprintf(filename, DT_MAX_PATH_LEN, "%s/pixmaps/plugins/darkroom/%s.png", datadir, module->op);
      if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
        snprintf(filename, DT_MAX_PATH_LEN, "%s/pixmaps/plugins/darkroom/template.png", datadir);
      GtkWidget *tmp_widget = gtk_image_new_from_file(filename);
      gtk_table_attach(GTK_TABLE(module->state_widget),tmp_widget,0,1,0,1,GTK_FILL  | GTK_SHRINK,GTK_SHRINK,0,0);

      tmp_widget =dtgtk_icon_new(dtgtk_cairo_paint_modulegroup_favorites, CPF_STYLE_FLAT);
      gtk_table_attach(GTK_TABLE(module->state_widget),tmp_widget,1,2,0,1,GTK_FILL | GTK_SHRINK,GTK_SHRINK,0,0);

      tmp_widget = dtgtk_label_new(module->name(),DARKTABLE_LABEL_ALIGN_LEFT);
      gtk_table_attach(GTK_TABLE(module->state_widget),tmp_widget,2,3,0,1,GTK_FILL | GTK_EXPAND | GTK_SHRINK,GTK_EXPAND|GTK_FILL ,0,0);

      /* set button state */
      char option[1024];
      snprintf(option, 1024, "plugins/darkroom/%s/visible", module->op);
      gboolean active = dt_conf_get_bool (option);
      snprintf(option, 1024, "plugins/darkroom/%s/favorite", module->op);
      gboolean favorite = dt_conf_get_bool (option);
      module->state=0;
      if(active)
      {
        module->state++;
        if(favorite) module->state++;
      }
      _lib_modulelist_row_set_state(module->state,module);

      GtkEventBox * box = GTK_EVENT_BOX(gtk_event_box_new());
      gtk_event_box_set_above_child(box,TRUE);
      gtk_event_box_set_visible_window(box, FALSE);
      g_signal_connect(G_OBJECT(box), "button-press-event",
                      G_CALLBACK(_lib_modulelist_row_changed_callback), module);
      gtk_container_add(GTK_CONTAINER(box),module->state_widget);
      rows = g_list_append(rows, box);
      gtk_widget_show_all(GTK_WIDGET(box));
    }

    modules = g_list_previous(modules);
  }
  gtk_list_append_items(((dt_lib_modulelist_t*)self->data)->list, rows);
}



static void _lib_modulelist_row_changed_callback(GtkWidget *w,GdkEvent *unused, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  gboolean expanded = _lib_modulelist_row_set_state(module->state+1,module);
  dt_dev_modulegroups_switch(module->dev, module);
  dt_iop_gui_set_expanded(module, expanded);

}




// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
