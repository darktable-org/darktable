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
#include "dtgtk/tristatebutton.h"
#include "gui/draw.h"

DT_MODULE(1)

#define DT_MODULE_LIST_SPACING 2

typedef struct dt_lib_modulelist_t
{
}
dt_lib_modulelist_t;

/* handle iop module click */
static void _lib_modulelist_tristate_changed_callback(GtkWidget *,gint state, gpointer user_data);
/* callback for iop modules loaded signal */
static void _lib_modulelist_populate_callback(gpointer instance, gpointer user_data);

const char* name()
{
  return _("more plugins");
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

  self->widget = gtk_table_new(2, 6, TRUE);
  gtk_table_set_row_spacings(GTK_TABLE(self->widget), DT_MODULE_LIST_SPACING);
  gtk_table_set_col_spacings(GTK_TABLE(self->widget), DT_MODULE_LIST_SPACING);

  /* connect to signal for darktable.develop initialization */
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_DEVELOP_INITIALIZE,G_CALLBACK(_lib_modulelist_populate_callback),self);

}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals,G_CALLBACK(_lib_modulelist_populate_callback),self);
  g_free(self->data);
  self->data = NULL;
}

static gboolean _lib_modulelist_tristate_set_state(GtkWidget *w,gint state,dt_iop_module_t *module)
{
  char option[1024];
  gboolean expand = FALSE;
  if(state==0)
  {
    /* module is hidden lets set gconf values */
    gtk_widget_hide(GTK_WIDGET(module->topwidget));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, FALSE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, FALSE);
    
    /* construct tooltip text into option */
    snprintf(option, 512, _("show %s"), module->name());
  }
  else if(state==1)
  {
    /* module is shown lets set gconf values */
    // FIXME
    // dt_gui_iop_modulegroups_switch(module->groups());
    gtk_widget_show(GTK_WIDGET(module->topwidget));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, TRUE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, FALSE);

    expand = TRUE;

    /* construct tooltip text into option */
    snprintf(option, 512, _("%s as favorite"), module->name());
  }
  else if(state==2)
  {
    /* module is shown and favorite lets set gconf values */
    // FIXME
    // dt_gui_iop_modulegroups_switch(module->groups());
    gtk_widget_show(GTK_WIDGET(module->topwidget));
    snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool (option, TRUE);
    snprintf(option, 512, "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool (option, TRUE);

    expand = TRUE;
    
    /* construct tooltip text into option */
    snprintf(option, 512, _("hide %s"), module->name());
  }

  g_object_set(G_OBJECT(w), "tooltip-text", option, (char *)NULL);
  return expand;
}

static void _lib_modulelist_populate_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  /* go thru list of iop modules and add tp table */
  GList *modules = g_list_last(darktable.develop->iop);
  int ti = 0, tj = 0;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(strcmp(module->op, "gamma") && !(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      module->showhide = dtgtk_tristatebutton_new(NULL,0);
      char filename[1024], datadir[1024];
      dt_util_get_datadir(datadir, 1024);
      snprintf(filename, 1024, "%s/pixmaps/plugins/darkroom/%s.png", datadir, module->op);
      if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
	snprintf(filename, 1024, "%s/pixmaps/plugins/darkroom/template.png", datadir);
      GtkWidget *image = gtk_image_new_from_file(filename);
      gtk_button_set_image(GTK_BUTTON(module->showhide), image);

      /* set button state */
      char option[1024];
      snprintf(option, 1024, "plugins/darkroom/%s/visible", module->op);
      gboolean active = dt_conf_get_bool (option);
      snprintf(option, 1024, "plugins/darkroom/%s/favorite", module->op);
      gboolean favorite = dt_conf_get_bool (option);
      gint state=0;
      if(active)
      {
	state++;
	if(favorite) state++;
      }
      _lib_modulelist_tristate_set_state(module->showhide,state,module);
      dtgtk_tristatebutton_set_state(DTGTK_TRISTATEBUTTON(module->showhide), state);

      /* connect tristate button callback*/
      g_signal_connect(G_OBJECT(module->showhide), "tristate-changed",
		       G_CALLBACK(_lib_modulelist_tristate_changed_callback), module);
      gtk_table_attach(GTK_TABLE(self->widget), module->showhide, ti, ti+1, tj, tj+1,
		       GTK_FILL | GTK_EXPAND | GTK_SHRINK,
		       GTK_SHRINK,
		       0, 0);
      if(ti < 5) ti++;
      else { ti = 0; tj ++; }
    }
    else
    {
      gtk_widget_hide_all(GTK_WIDGET(module->topwidget));
    }

    modules = g_list_previous(modules);
  }

}



static void _lib_modulelist_tristate_changed_callback(GtkWidget *w,gint state, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  
  /* set state of tristate button and expand iop if wanted */
  gboolean expanded = _lib_modulelist_tristate_set_state(w,state,module);
  dt_dev_modulegroups_switch(module->dev, module);
  dt_iop_gui_set_expanded(module, expanded);

}




