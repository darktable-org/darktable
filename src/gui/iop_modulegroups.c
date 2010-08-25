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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "common/darktable.h"
#include "control/control.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include "gui/iop_modulegroups.h"

static GList *_iop_modulegroups_modules=NULL;

static GtkWidget *_iop_modulegroups_basic_widget=NULL;
static GtkWidget *_iop_modulegroups_correct_widget=NULL;
static GtkWidget *_iop_modulegroups_color_widget=NULL;
static GtkWidget *_iop_modulegroups_effect_widget=NULL;

static void 
_iop_modulegroups_toggle(GtkWidget *button,gpointer data)
{
  if(!dt_control_running()) return;
  long group=(long)data;

  /* is none of the buttons on, let's show all enabled modules.. */
  
  if (  _iop_modulegroups_modules &&
              gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_basic_widget)) == FALSE &&
              gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_correct_widget)) == FALSE &&
              gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_color_widget)) == FALSE &&
              gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_effect_widget)) == FALSE )
  {
    GList *modules=_iop_modulegroups_modules;
    while (modules)
    {
      dt_iop_module_t *module=(dt_iop_module_t*)modules->data;
        
      /* ignore special module named gamma */
      if(strcmp(module->op, "gamma"))
      {
        if ( ( !module->showhide || (GTK_IS_TOGGLE_BUTTON (module->showhide) && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (module->showhide))==TRUE) ) )  
          gtk_widget_show(GTK_WIDGET (module->topwidget));
      }
      
      /* step to next module */
      modules = g_list_next(modules);
    }
    return;
  }

  
  /* radiobutton behaviour */
  
  /* prevent toggled signal emittion */
  g_signal_handlers_block_matched ( _iop_modulegroups_basic_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_correct_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_color_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_effect_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  
  if( button != _iop_modulegroups_basic_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_basic_widget),FALSE);
  if( button != _iop_modulegroups_correct_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_correct_widget),FALSE);
  if( button != _iop_modulegroups_color_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_color_widget),FALSE);
  if( button != _iop_modulegroups_effect_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_effect_widget),FALSE);
  
  gtk_widget_queue_draw (_iop_modulegroups_basic_widget);
  gtk_widget_queue_draw (_iop_modulegroups_correct_widget);
  gtk_widget_queue_draw (_iop_modulegroups_color_widget);
  gtk_widget_queue_draw (_iop_modulegroups_effect_widget);
  
  /* ublock toggled signal emittion */
  g_signal_handlers_unblock_matched ( _iop_modulegroups_basic_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_correct_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_color_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_effect_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
 
  
  /* let's update visibilities of modules for group */
  if (_iop_modulegroups_modules)
  {
    GList *modules=_iop_modulegroups_modules;
    while (modules)
    {
      
      dt_iop_module_t *module=(dt_iop_module_t*)modules->data;
      if(strcmp(module->op, "gamma"))
      {
       
        if ( (module->groups () & group ) && ( !module->showhide || (module->showhide && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (module->showhide))==TRUE) ) )  
          gtk_widget_show(GTK_WIDGET (module->topwidget));
        else
          gtk_widget_hide(GTK_WIDGET (module->topwidget));
      }
      
      /* step to next module */
      modules = g_list_next(modules);
    }
    return;
  }
  
}

void 
dt_gui_iop_modulegroups_switch(int group)
{
  if(group&IOP_GROUP_BASIC) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_basic_widget),TRUE);
  else if(group&IOP_GROUP_CORRECT) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_correct_widget),TRUE);
  else if(group&IOP_GROUP_COLOR) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_color_widget),TRUE);
  else if(group&IOP_GROUP_EFFECT) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_effect_widget ),TRUE);
}

void dt_gui_iop_modulegroups_set_list (GList *modules) {
  /* sets the list of iop modules to use, called when entering develop mode */
  if( !modules ) fprintf (stderr,"setting empty iop list\n");
    _iop_modulegroups_modules = modules;
  
  /* clear all */
  g_signal_handlers_block_matched ( _iop_modulegroups_basic_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_correct_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_color_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_effect_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_basic_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_correct_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_color_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_effect_widget),FALSE);
  
  /* ublock toggled signal emittion */
  g_signal_handlers_unblock_matched ( _iop_modulegroups_basic_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_correct_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_color_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_effect_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);

  /* default behavior is to enable view of basic group when entering develop mode */
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_basic_widget),TRUE);
  
}
  
void dt_gui_iop_modulegroups_init ()
{
  /* create the button box*/
  GtkWidget *bbox=gtk_hbox_new(TRUE,2);
  gtk_widget_set_size_request (bbox,-1,22);
  
  /* add button for basic plugins */
  //_iop_modulegroups_basic_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0);
  _iop_modulegroups_basic_widget = gtk_toggle_button_new_with_label(_("basic"));
  g_signal_connect (_iop_modulegroups_basic_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_BASIC);
  g_object_set (_iop_modulegroups_basic_widget,"tooltip-text",_("basic group"),NULL);
  gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_basic_widget,TRUE,TRUE,0);

  /* add button for color plugins */
  //_iop_modulegroups_color_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0);
  _iop_modulegroups_color_widget =gtk_toggle_button_new_with_label(_("color"));
  g_signal_connect (_iop_modulegroups_color_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_COLOR);
  g_object_set (_iop_modulegroups_color_widget,"tooltip-text",_("color group"),NULL);
  gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_color_widget,TRUE,TRUE,0);
  
  /* add button for correction plugins */
  //_iop_modulegroups_correct_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0);
  _iop_modulegroups_correct_widget = gtk_toggle_button_new_with_label(_("correct"));
  g_signal_connect (_iop_modulegroups_correct_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_CORRECT);
  g_object_set (_iop_modulegroups_correct_widget,"tooltip-text",_("correction group"),NULL);
  gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_correct_widget,TRUE,TRUE,0);
  
  /* add buttons for artistic plugins */
 // _iop_modulegroups_effect_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0	);
  _iop_modulegroups_effect_widget = gtk_toggle_button_new_with_label(_("effect"));
  g_signal_connect (_iop_modulegroups_effect_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_EFFECT);
  g_object_set (_iop_modulegroups_effect_widget,"tooltip-text",_("effect group"),NULL);
  gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_effect_widget,TRUE,TRUE,0);
  
  gtk_container_add (GTK_CONTAINER (glade_xml_get_widget (darktable.gui->main_window, "modulegroups_eventbox")),bbox);
  gtk_widget_show_all(bbox);

}
