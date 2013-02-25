/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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
#include <glib.h>
#include "lua/preferences.h"
#include "lua/dt_lua.h"
#include <stdlib.h>
#include <string.h>
#include "control/conf.h"

static const char *pref_type_name[] = {
"string",
  NULL
};

typedef enum {
  pref_string,
} pref_type;


typedef struct pref_element{
  char*script;
  char*name;
  char* label;
  char* tooltip;
  pref_type type;
  union {
    struct {
      char *default_value;
    } string_data;
  } ;
  struct pref_element* next;
  // ugly hack, written every time the dialog is displayed and unused when not displayed 
  // value is set when window is created, it is correct when window is destroyed, but is invalid out of that context
  GtkWidget * widget;

} pref_element;

static void destroy_pref_element(pref_element*elt) {
  free(elt->script);
  free(elt->name);
  free(elt->label);
  free(elt->tooltip);
  // don't free the widget field
  switch(elt->type) {
    case pref_string:
      free(elt->string_data.default_value);
      break;
    default:
      break;
  }
  free(elt);
}

static pref_element* pref_list=NULL;

static void get_pref_name(char *tgt,size_t size,const char*script,const char*name) {
    snprintf(tgt,size,"lua/%s/%s",script,name);
}

static int register_pref(lua_State*L){
  // to avoid leaks, we need to first get all params (which could raise errors) then alloc and fill the struct
  // this is complicated, but needed
  pref_element * built_elt = malloc(sizeof(pref_element));
  memset(built_elt,0,sizeof(pref_element));
  int cur_param =1;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->script = strdup(lua_tostring(L,cur_param));
  cur_param++;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->name = strdup(lua_tostring(L,cur_param));
  cur_param++;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->label = strdup(lua_tostring(L,cur_param));
  cur_param++;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->tooltip = strdup(lua_tostring(L,cur_param));
  cur_param++;

  int i;
  const char* type_name = lua_tostring(L,cur_param);
  printf("aaa %s\n",type_name);
  if(!type_name) goto error;
  printf("aaa %s\n",type_name);
  for (i=0; pref_type_name[i]; i++)
    if (strcmp(pref_type_name[i], type_name) == 0){
      built_elt->type =i;
      break;
    }
  if(!pref_type_name[i]) goto error;
  cur_param++;

  char pref_name[1024];
  get_pref_name(pref_name,1024,built_elt->script,built_elt->name);
  switch(built_elt->type) {
    case pref_string:
      if(!lua_isstring(L,cur_param)) goto error;
      built_elt->string_data.default_value = strdup(lua_tostring(L,cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name)) dt_conf_set_string(pref_name,built_elt->string_data.default_value);
      break;
    default:
      // can't happen, checkoption would have raised an error
      break;
  }


  built_elt->next = pref_list;
  pref_list = built_elt;
  return 0;
error:
  destroy_pref_element(built_elt);
  return luaL_argerror(L,cur_param,NULL);
}


static void callback_string(GtkWidget *widget, pref_element*cur_elt ) {
  char pref_name[1024];
  get_pref_name(pref_name,1024,cur_elt->script,cur_elt->name);
  dt_conf_set_string(pref_name,gtk_entry_get_text(GTK_ENTRY(widget)));
}
  
static void response_callback_string(GtkDialog *dialog, gint response_id, pref_element* cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name,1024,cur_elt->script,cur_elt->name);
    dt_conf_set_string(pref_name,gtk_entry_get_text(GTK_ENTRY(cur_elt->widget)));
  }
}

static gboolean reset_widget_string (GtkWidget *label, GdkEventButton *event, pref_element*cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    char pref_name[1024];
    get_pref_name(pref_name,1024,cur_elt->script,cur_elt->name);
    gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), cur_elt->string_data.default_value);
    return TRUE;
  }
  return FALSE;
}
void init_tab_lua (GtkWidget *dialog, GtkWidget *tab)
{
  if(!pref_list) return; // no option registered => don't create the tab
  GtkWidget  *label, *labelev;
  GtkWidget *hbox = gtk_hbox_new(5, FALSE);
  GtkWidget *vbox1 = gtk_vbox_new(5, TRUE);
  GtkWidget *vbox2 = gtk_vbox_new(5, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, FALSE, FALSE, 0);
  GtkWidget *alignment = gtk_alignment_new(0.5, 0.0, 1.0, 0.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 20, 20, 20, 20);
  gtk_container_add(GTK_CONTAINER(alignment), hbox);
  gtk_notebook_append_page(GTK_NOTEBOOK(tab), alignment, gtk_label_new(_("lua options")));

  pref_element* cur_elt = pref_list;
  while(cur_elt) {
    char pref_name[1024];
    get_pref_name(pref_name,1024,cur_elt->script,cur_elt->name);
    label = gtk_label_new(cur_elt->label);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    cur_elt->widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), dt_conf_get_string(pref_name));
    g_signal_connect(G_OBJECT(cur_elt->widget), "activate", G_CALLBACK(callback_string), cur_elt);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_string), cur_elt);
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", cur_elt->tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), cur_elt->widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_string), cur_elt);
    cur_elt = cur_elt->next;
  }
/*
  {
    label = gtk_label_new(cur_elt->label);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    gint min = 0;
    gint max = G_MAXINT;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("panel_width"));
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_idp328384), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp328384), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "300");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _(" (needs a restart)"), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp328384), (gpointer)widget);
  }
  {
    label = gtk_label_new(_("don't use embedded preview jpeg but half-size raw"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("never_use_embedded_thumb"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp388384), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp388384), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "FALSE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("check this option to not use the embedded jpeg from the raw file but process the raw data. this is slower but gives you color managed thumbnails."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp388384), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("ask before removing images from database"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("ask_before_remove"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp404720), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp404720), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "TRUE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("always ask the user before any image is removed from db."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp404720), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("ask before erasing images from disk"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("ask_before_delete"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp407504), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp407504), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "TRUE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("always ask the user before any image file is deleted."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp407504), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("ask before moving images from film roll folder"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("ask_before_move"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp410256), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp410256), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "TRUE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("always ask the user before any image file is moved."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp410256), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("ask before copying images to new film roll folder"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("ask_before_copy"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp413024), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp413024), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "TRUE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("always ask the user before any image file is copied."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp413024), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("number of folder levels to show in lists"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    gint min = 0;
    gint max = G_MAXINT;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("show_folder_levels"));
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_idp415792), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp415792), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "1");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("the number of folder levels to show in film roll names, starting from the right"), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp415792), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("ignore jpeg images when importing film rolls"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("ui_last/import_ignore_jpegs"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp478400), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp478400), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "FALSE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("when having raw+jpeg images together in one directory it makes no sense to import both. with this flag one can ignore all jpegs found."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp478400), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("recursive directory traversal when importing filmrolls"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("ui_last/import_recursive"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp481264), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp481264), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "FALSE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp481264), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("creator to be applied when importing"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), dt_conf_get_string("ui_last/import_last_creator"));
    g_signal_connect(G_OBJECT(widget), "activate", G_CALLBACK(preferences_callback_idp483856), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp483856), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp483856), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("publisher to be applied when importing"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), dt_conf_get_string("ui_last/import_last_publisher"));
    g_signal_connect(G_OBJECT(widget), "activate", G_CALLBACK(preferences_callback_idp486272), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp486272), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp486272), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("rights to be applied when importing"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), dt_conf_get_string("ui_last/import_last_rights"));
    g_signal_connect(G_OBJECT(widget), "activate", G_CALLBACK(preferences_callback_idp488688), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp488688), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp488688), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("comma separated tags to be applied when importing"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), dt_conf_get_string("ui_last/import_last_tags"));
    g_signal_connect(G_OBJECT(widget), "activate", G_CALLBACK(preferences_callback_idp491104), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp491104), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp491104), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("initial import rating"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    gint min = 0;
    gint max = G_MAXINT;
    min = 0;
    max = 5;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("ui_last/import_initial_rating"));
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_idp495696), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp495696), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "1");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("initial star rating for all images when importing a filmroll"), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp495696), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("enable filmstrip"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("plugins/lighttable/filmstrip/visible"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp501168), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp501168), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "TRUE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("enable the filmstrip in darkroom and tethering modes."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp501168), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("maximum width of image drawing area"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    gint min = 0;
    gint max = G_MAXINT;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("plugins/lighttable/thumbnail_width"));
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_idp509520), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp509520), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "1300");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("maximum width of the image drawing area in darkroom mode. adjust to your screen\n(needs a restart and will invalidate current thumbnail caches)."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp509520), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("maximum height of image drawing area"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    gint min = 0;
    gint max = G_MAXINT;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("plugins/lighttable/thumbnail_height"));
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_idp512352), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp512352), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "1000");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("maximum height of the image drawing area in dakroom mode. adjust to your screen\n(needs a restart and will invalidate current thumbnail caches)."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp512352), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("compression of thumbnail images"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_combo_box_new_text();
    {
      gchar *str = dt_conf_get_string("cache_compression");
      gint pos = -1;
      gtk_combo_box_append_text(GTK_COMBO_BOX(widget), "off");
      if(pos == -1 && strcmp(str, "off") == 0)
        pos = 0;
      gtk_combo_box_append_text(GTK_COMBO_BOX(widget), "low quality (fast)");
      if(pos == -1 && strcmp(str, "low quality (fast)") == 0)
        pos = 1;
      gtk_combo_box_append_text(GTK_COMBO_BOX(widget), "high quality (slow)");
      if(pos == -1 && strcmp(str, "high quality (slow)") == 0)
        pos = 2;
      gtk_combo_box_set_active(GTK_COMBO_BOX(widget), pos);
    }
    g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(preferences_callback_idp515184), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp515184), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "off");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("off - no compression in memory, jpg on disk. low quality - dxt1 (fast). high quality - dxt1, same memory as low quality variant but slower."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp515184), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("ask before deleting a tag"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp639856), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp639856), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "TRUE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp639856), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("maximum number of images drawn on map"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    gint min = 0;
    gint max = G_MAXINT;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("plugins/map/max_images_drawn"));
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_idp644608), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp644608), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "100");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("the maximum number of geotagged images drawn on the map. increasing this number can slow drawing of the map down (needs a restart)."), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp644608), (gpointer)widget);
  }

  {
    label = gtk_label_new(_("pretty print the image location"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_idp647440), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_idp647440), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "TRUE");
    gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("show a more readable representation of the location in the image information module"), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_idp647440), (gpointer)widget);
  }
*/
}

int dt_lua_init_preferences(lua_State * L) {
	
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"preferences");
  lua_pushcfunction(L,register_pref);
  lua_setfield(L,-2,"register_preference");
  lua_pop(L,-1);
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

