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
#include "dtgtk/tristatebutton.h"
#include "gui/gtk.h"
#include "gui/iop_modulegroups.h"

static GList *_iop_modulegroups_modules=NULL;

static GtkWidget *_iop_modulegroups_userdefined_widget=NULL;
static GtkWidget *_iop_modulegroups_activepipe_widget=NULL;
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
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_userdefined_widget)) == FALSE &&
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_activepipe_widget)) == FALSE &&
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
        if ( ( !module->showhide || (DTGTK_IS_TRISTATEBUTTON (module->showhide) && dtgtk_tristatebutton_get_state(DTGTK_TRISTATEBUTTON(module->showhide))>0) ) &&
             ((!(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled)))
          gtk_widget_show(GTK_WIDGET (module->topwidget));
      }

      /* step to next module */
      modules = g_list_next(modules);
    }
    return;
  }


  /* radiobutton behaviour */

  /* prevent toggled signal emittion */
  g_signal_handlers_block_matched ( _iop_modulegroups_userdefined_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_activepipe_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_basic_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_correct_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_color_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_effect_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);

  if( button != _iop_modulegroups_userdefined_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_userdefined_widget),FALSE);
  if( button != _iop_modulegroups_activepipe_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_activepipe_widget),FALSE);
  if( button != _iop_modulegroups_basic_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_basic_widget),FALSE);
  if( button != _iop_modulegroups_correct_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_correct_widget),FALSE);
  if( button != _iop_modulegroups_color_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_color_widget),FALSE);
  if( button != _iop_modulegroups_effect_widget) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_effect_widget),FALSE);

  gtk_widget_queue_draw (_iop_modulegroups_userdefined_widget);
  gtk_widget_queue_draw (_iop_modulegroups_activepipe_widget);
  gtk_widget_queue_draw (_iop_modulegroups_basic_widget);
  gtk_widget_queue_draw (_iop_modulegroups_correct_widget);
  gtk_widget_queue_draw (_iop_modulegroups_color_widget);
  gtk_widget_queue_draw (_iop_modulegroups_effect_widget);

  /* ublock toggled signal emittion */
  g_signal_handlers_unblock_matched ( _iop_modulegroups_userdefined_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_activepipe_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
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

        if(group == IOP_SPECIAL_GROUP_ACTIVE_PIPE )
        {
          /* handle special case group */
          if(module->enabled)
            gtk_widget_show(GTK_WIDGET (module->topwidget));
          else
            gtk_widget_hide(GTK_WIDGET (module->topwidget));
        }
        else if(group == IOP_SPECIAL_GROUP_USER_DEFINED )
        {
          /* handle special case group */
          if(module->showhide && dtgtk_tristatebutton_get_state (DTGTK_TRISTATEBUTTON(module->showhide))==2)
            gtk_widget_show(GTK_WIDGET (module->topwidget));
          else
            gtk_widget_hide(GTK_WIDGET (module->topwidget));

        }
        else if ( (module->groups () & group ) &&
                  ( !module->showhide || (module->showhide && dtgtk_tristatebutton_get_state (DTGTK_TRISTATEBUTTON(module->showhide))>0 )) &&
                  ((!(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled)))
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
  else if(group&IOP_SPECIAL_GROUP_ACTIVE_PIPE) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_activepipe_widget ),TRUE);
  else if(group&IOP_SPECIAL_GROUP_USER_DEFINED) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_userdefined_widget ),TRUE);
}

int
dt_gui_iop_modulegroups_get ()
{
  int group = 0;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_iop_modulegroups_basic_widget)))   group |= IOP_GROUP_BASIC;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_iop_modulegroups_correct_widget))) group |= IOP_GROUP_CORRECT;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_iop_modulegroups_color_widget)))   group |= IOP_GROUP_COLOR;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_iop_modulegroups_effect_widget)))  group |= IOP_GROUP_EFFECT;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_iop_modulegroups_activepipe_widget)))  group |= IOP_SPECIAL_GROUP_ACTIVE_PIPE;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_iop_modulegroups_userdefined_widget)))  group |= IOP_SPECIAL_GROUP_USER_DEFINED;
  return group;
}

void dt_gui_iop_modulegroups_set_list (GList *modules)
{
  /* sets the list of iop modules to use, called when entering develop mode */
  if( !modules ) fprintf (stderr,"setting empty iop list\n");
  _iop_modulegroups_modules = modules;

  /* clear all */
  g_signal_handlers_block_matched ( _iop_modulegroups_basic_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_correct_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_color_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_effect_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_activepipe_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_block_matched ( _iop_modulegroups_userdefined_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_basic_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_correct_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_color_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_effect_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_activepipe_widget),FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_userdefined_widget),FALSE);

  /* ublock toggled signal emittion */
  g_signal_handlers_unblock_matched ( _iop_modulegroups_basic_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_correct_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_color_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_effect_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_activepipe_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);
  g_signal_handlers_unblock_matched ( _iop_modulegroups_userdefined_widget,G_SIGNAL_MATCH_FUNC,0,0,NULL,_iop_modulegroups_toggle,NULL);

  /* default behavior is to enable view of basic group when entering develop mode */
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (_iop_modulegroups_activepipe_widget),TRUE);

}
#define PADDING 2
void dt_gui_iop_modulegroups_init ()
{
  /* create the button box*/
  GtkWidget *table = gtk_table_new(2,4,TRUE);
// gtk_widget_set_size_request (bbox,-1,22);

  /* add buttong for active pipe */
  _iop_modulegroups_activepipe_widget = gtk_toggle_button_new_with_label(_("active"));
  g_signal_connect (_iop_modulegroups_activepipe_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_SPECIAL_GROUP_ACTIVE_PIPE);
  g_object_set (_iop_modulegroups_activepipe_widget,"tooltip-text",_("the modules used in active pipe"),(char *)NULL);
  //gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_activepipe_widget,TRUE,TRUE,0);

  /* add buttong for active pipe */
  _iop_modulegroups_userdefined_widget = gtk_toggle_button_new_with_label(_("favorite"));
  g_signal_connect (_iop_modulegroups_userdefined_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_SPECIAL_GROUP_USER_DEFINED);
  g_object_set (_iop_modulegroups_userdefined_widget,"tooltip-text",_("show modules explicit specified by user"),(char *)NULL);
  //gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_userdefined_widget,TRUE,TRUE,0);

  /* add button for basic plugins */
  //_iop_modulegroups_basic_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0);
  _iop_modulegroups_basic_widget = gtk_toggle_button_new_with_label(_("basic"));
  g_signal_connect (_iop_modulegroups_basic_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_BASIC);
  g_object_set (_iop_modulegroups_basic_widget,"tooltip-text",_("basic group"),(char *)NULL);
// gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_basic_widget,TRUE,TRUE,0);

  /* add button for color plugins */
  //_iop_modulegroups_color_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0);
  _iop_modulegroups_color_widget =gtk_toggle_button_new_with_label(_("color"));
  g_signal_connect (_iop_modulegroups_color_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_COLOR);
  g_object_set (_iop_modulegroups_color_widget,"tooltip-text",_("color group"),(char *)NULL);
  //gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_color_widget,TRUE,TRUE,0);

  /* add button for correction plugins */
  //_iop_modulegroups_correct_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0);
  _iop_modulegroups_correct_widget = gtk_toggle_button_new_with_label(_("correct"));
  g_signal_connect (_iop_modulegroups_correct_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_CORRECT);
  g_object_set (_iop_modulegroups_correct_widget,"tooltip-text",_("correction group"),(char *)NULL);
  //gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_correct_widget,TRUE,TRUE,0);

  /* add buttons for artistic plugins */
// _iop_modulegroups_effect_widget = dtgtk_togglebutton_new (dtgtk_cairo_paint_refresh,0	);
  _iop_modulegroups_effect_widget = gtk_toggle_button_new_with_label(_("effect"));
  g_signal_connect (_iop_modulegroups_effect_widget,"toggled",G_CALLBACK (_iop_modulegroups_toggle),(gpointer)IOP_GROUP_EFFECT);
  g_object_set (_iop_modulegroups_effect_widget,"tooltip-text",_("effect group"),(char *)NULL);
  //gtk_box_pack_start (GTK_BOX (bbox),_iop_modulegroups_effect_widget,TRUE,TRUE,0);


  /* top row */
  gtk_table_attach(GTK_TABLE(table),_iop_modulegroups_activepipe_widget,0,1,0,1,GTK_EXPAND|GTK_FILL,0,PADDING,PADDING);
  gtk_table_attach(GTK_TABLE(table),_iop_modulegroups_userdefined_widget,1,2,0,1,GTK_EXPAND|GTK_FILL,0,PADDING,PADDING);
  /* second row */
  gtk_table_attach(GTK_TABLE(table),_iop_modulegroups_basic_widget,0,1,1,2,GTK_EXPAND|GTK_FILL,0,PADDING,PADDING);
  gtk_table_attach(GTK_TABLE(table),_iop_modulegroups_color_widget,1,2,1,2,GTK_EXPAND|GTK_FILL,0,PADDING,PADDING);
  gtk_table_attach(GTK_TABLE(table),_iop_modulegroups_correct_widget,2,3,1,2,GTK_EXPAND|GTK_FILL,0,PADDING,PADDING);
  gtk_table_attach(GTK_TABLE(table),_iop_modulegroups_effect_widget,3,4,1,2,GTK_EXPAND|GTK_FILL,0,PADDING,PADDING);

  gtk_container_add (GTK_CONTAINER (darktable.gui->
                                    widgets.modulegroups_eventbox),table);
  gtk_widget_show_all(table);

}
