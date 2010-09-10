/*
    This file is part of darktable,
    copyright (c) 2009--2010 Henrik Andersson.

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

#include <glade/glade.h>
#include  "control/conf.h"
#include  "common/darktable.h"
#include  "libs/lib.h"
#include "gui/gtk.h"
#include "gui/panel_sizegroup.h"

GSList *_panel_sizegroup_widgets = NULL;
int _panel_sizegroup_width=0;
int _panel_need_resize=0;

static const int scrollbarwidth = 14;

void 
dt_gui_panel_sizegroup_init()
{
  /* restore old panel width value */
  char *lang = getenv ("LANGUAGE");
  if (!lang || strlen(lang) == 0) lang = getenv("LANG");
  if (lang) 
  {
    char key[128]={0};
    strcat (key,"panel_width.");
    strcat (key,lang);
    _panel_need_resize=1;
    _panel_sizegroup_width = dt_conf_get_int(key);
  }
}

static void 
_panel_sizegroup_allocate(GtkWidget *w,GtkAllocation *a,gpointer data) 
{
  const int wdreq = a->width;
  if (_panel_sizegroup_width < wdreq)
  {
    fprintf(stderr,"Widget %s claims width %d.\n",gtk_widget_get_name(w),a->width);
    _panel_sizegroup_width = wdreq;
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "left"),_panel_sizegroup_width,-1);
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "right"),_panel_sizegroup_width,-1);
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"), MAX(-1, _panel_sizegroup_width-scrollbarwidth),-1);
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "left_scrolled"),MAX(-1, _panel_sizegroup_width-scrollbarwidth),-1);
    gtk_widget_queue_resize (glade_xml_get_widget (darktable.gui->main_window, "left"));
    gtk_widget_queue_resize (glade_xml_get_widget (darktable.gui->main_window, "right"));
    
    /* store panel width value */
    char *lang = getenv ("LANGUAGE");
    if (!lang || strlen(lang) == 0) lang=getenv("LANG");
    if (lang) 
    {
      char key[128]={0};
      strcat (key,"panel_width.");
      strcat (key,lang);
      
      dt_conf_set_int(key,_panel_sizegroup_width);
    } 
  } 
}

void 
dt_gui_panel_sizegroup_add (GtkWidget *w) 
{
  /* add widget to internal list of widgets */
  _panel_sizegroup_widgets = g_slist_append (_panel_sizegroup_widgets,w);
  
  /* add signal for size-allocate */
  g_signal_connect (G_OBJECT (w), "size-allocate",G_CALLBACK (_panel_sizegroup_allocate), 0);

  /* */
  if (_panel_need_resize)
  {
    _panel_need_resize=0;
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "left"),_panel_sizegroup_width,-1);
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "right"),_panel_sizegroup_width,-1);
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"), MAX(-1, _panel_sizegroup_width-scrollbarwidth),-1);
    gtk_widget_set_size_request (glade_xml_get_widget (darktable.gui->main_window, "left_scrolled"),MAX(-1, _panel_sizegroup_width-scrollbarwidth),-1);
    gtk_widget_queue_resize (glade_xml_get_widget (darktable.gui->main_window, "left"));
    gtk_widget_queue_resize (glade_xml_get_widget (darktable.gui->main_window, "right"));
  }
  
}

void 
dt_gui_panel_sizegroup_remove (GtkWidget *w)
{
  // gtk_size_group_remove_widget (_panel_sizegroup,w);
}

void
dt_gui_panel_sizegroup_modules () 
{
  /* now lets get expander widgetr for all modules... */
  GList *modules = g_list_last(darktable.lib->plugins);
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    /* init gui module */
    module->gui_init(module);
    /* get expander */
    dt_lib_gui_get_expander(module);
    /* module ui cleanup */
    module->gui_cleanup(module);
    modules = g_list_previous(modules);
  }
}
