/*
    This file is part of darktable,
    copyright (c) 2011 robert bieber.

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


#include <gtk/gtk.h>

#include "gui/accelerators.h"
#include "common/darktable.h"
#include "control/control.h"
#include "dtgtk/slider.h"

void dt_accel_path_global(char *s, size_t n, const char* path)
{
  snprintf(s, n, "<Darktable>/%s/%s",
           NC_("accel", "global"), path);
}

void dt_accel_path_view(char *s, size_t n, char *module,
                               const char* path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s",
           NC_("accel", "views"), module, path);
}

void dt_accel_path_iop(char *s, size_t n, char *module,
                              const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s",
           NC_("accel", "image operations"), module, path);
}

void dt_accel_path_lib(char *s, size_t n, char *module,
                              const char* path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s",
           NC_("accel", "plugins"), module, path);
}

void dt_accel_paths_slider_iop(char *s[], size_t n, char *module,
                               const char* path)
{
  snprintf(s[0], n, "<Darktable>/%s/%s/%s/%s",
           NC_("accel", "image operations"), module, path,
           NC_("accel", "increase"));
  snprintf(s[1], n, "<Darktable>/%s/%s/%s/%s",
           NC_("accel", "image operations"), module, path,
           NC_("accel", "decrease"));
  snprintf(s[2], n, "<Darktable>/%s/%s/%s/%s",
           NC_("accel", "image operations"), module, path,
           NC_("accel", "reset"));
  snprintf(s[3], n, "<Darktable>/%s/%s/%s/%s",
           NC_("accel", "image operations"), module, path,
           NC_("accel", "edit"));
}

static void dt_accel_path_global_translated(char *s, size_t n, const char* path)
{
  snprintf(s, n, "<Darktable>/%s/%s",
           C_("accel", "global"), g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_view_translated(char *s, size_t n, dt_view_t *module,
                                          const char* path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s",
           C_("accel", "views"), module->name(module),
           g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_iop_translated(char *s, size_t n,
                                         dt_iop_module_so_t *module,
                                         const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s",
           C_("accel", "image operations"), module->name(),
           g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_lib_translated(char *s, size_t n,
                                         dt_lib_module_t *module,
                                         const char* path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s",
           C_("accel", "plugins"), module->name(),
           g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_paths_slider_iop_translated(char *s[], size_t n,
                                                 dt_iop_module_so_t *module,
                                                 const char* path)
{
  snprintf(s[0], n, "<Darktable>/%s/%s/%s/%s",
           C_("accel", "image operations"),
           module->name(),
           g_dpgettext2(NULL, "accel", path),
           C_("accel", "increase"));
  snprintf(s[1], n, "<Darktable>/%s/%s/%s/%s",
           C_("accel", "image operations"),
           module->name(),
           g_dpgettext2(NULL, "accel", path),
           C_("accel", "decrease"));
  snprintf(s[2], n, "<Darktable>/%s/%s/%s/%s",
           C_("accel", "image operations"),
           module->name(),
           g_dpgettext2(NULL, "accel", path),
           C_("accel", "reset"));
  snprintf(s[3], n, "<Darktable>/%s/%s/%s/%s",
           C_("accel", "image operations"),
           module->name(),
           g_dpgettext2(NULL, "accel", path),
           C_("accel", "edit"));

}

void dt_accel_register_global(const gchar *path, guint accel_key,
                              GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t*)malloc(sizeof(dt_accel_t));

  dt_accel_path_global(accel_path, 256, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  strcpy(accel->path, accel_path);
  dt_accel_path_global_translated(accel_path, 256, path);
  strcpy(accel->translated_path, accel_path);

  *(accel->module) = '\0';
  accel->views = DT_LIGHTTABLE_VIEW | DT_DARKTABLE_VIEW | DT_CAPTURE_VIEW;
  accel->local = FALSE;
  darktable.control->accelerator_list =
      g_slist_prepend(darktable.control->accelerator_list, accel);

}

void dt_accel_register_view(dt_view_t *self, const gchar *path, guint accel_key,
                            GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t*)malloc(sizeof(dt_accel_t));

  dt_accel_path_view(accel_path, 256, self->module_name, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  strcpy(accel->path, accel_path);
  dt_accel_path_view_translated(accel_path, 256, self, path);
  strcpy(accel->translated_path, accel_path);

  strcpy(accel->module, self->module_name);
  if(!strcmp(self->module_name, "capture"))
    accel->views = DT_CAPTURE_VIEW;
  else if(!strcmp(self->module_name, "darkroom"))
    accel->views = DT_DARKTABLE_VIEW;
  else if(!strcmp(self->module_name, "filmstrip"))
    accel->views = DT_DARKTABLE_VIEW | DT_CAPTURE_VIEW;
  else if(!strcmp(self->module_name, "lighttable"))
    accel->views = DT_LIGHTTABLE_VIEW;
  else
    accel->views = 0;
  accel->local = FALSE;
  darktable.control->accelerator_list =
      g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local,
                           const gchar *path, guint accel_key,
                           GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t*)malloc(sizeof(dt_accel_t));

  dt_accel_path_iop(accel_path, 256, so->op, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  strcpy(accel->path, accel_path);
  dt_accel_path_iop_translated(accel_path, 256, so, path);
  strcpy(accel->translated_path, accel_path);

  strcpy(accel->module, so->op);
  accel->local = local;
  accel->views = DT_DARKTABLE_VIEW;
  darktable.control->accelerator_list =
      g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_lib(dt_lib_module_t *self, const gchar *path,
                           guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t*)malloc(sizeof(dt_accel_t));

  dt_accel_path_lib(accel_path, 256, self->plugin_name, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);
  strcpy(accel->path, accel_path);
  dt_accel_path_lib_translated(accel_path, 256, self, path);
  strcpy(accel->translated_path, accel_path);

  strcpy(accel->module, self->plugin_name);
  accel->local = FALSE;
  accel->views = self->views();
  darktable.control->accelerator_list =
      g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_slider_iop(dt_iop_module_so_t *so, gboolean local,
                           const gchar *path)
{
  gchar increase_path[256];
  gchar decrease_path[256];
  gchar reset_path[256];
  gchar edit_path[256];
  gchar increase_path_trans[256];
  gchar decrease_path_trans[256];
  gchar reset_path_trans[256];
  gchar edit_path_trans[256];

  char *paths[] = {increase_path, decrease_path, reset_path, edit_path};
  char *paths_trans[] = {increase_path_trans, decrease_path_trans,
                         reset_path_trans, edit_path_trans};

  int i = 0;
  dt_accel_t *accel = NULL;

  dt_accel_paths_slider_iop(paths, 256, so->op, path);
  dt_accel_paths_slider_iop_translated(paths_trans, 256, so, path);

  for(i = 0; i < 4; i++)
  {
    gtk_accel_map_add_entry(paths[i], 0, 0);
    accel = (dt_accel_t*)malloc(sizeof(dt_accel_t));

    strcpy(accel->path, paths[i]);
    strcpy(accel->translated_path, paths_trans[i]);
    strcpy(accel->module, so->op);
    accel->local = local;
    accel->views = DT_DARKTABLE_VIEW;

    darktable.control->accelerator_list =
        g_slist_prepend(darktable.control->accelerator_list, accel);
  }
}


void dt_accel_connect_global(const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_global(accel_path, 256, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path,
                                  closure);
}

void dt_accel_connect_view(dt_view_t *self, const gchar *path,
                           GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_view(accel_path, 256, self->module_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path,
                                  closure);
  self->accel_closures = g_slist_prepend(self->accel_closures, closure);
}

static void _connect_local_accel(dt_iop_module_t *module, dt_accel_t *accel,
                                 GClosure *closure)
{
  dt_accel_local_t *laccel =
      (dt_accel_local_t*)malloc(sizeof(dt_accel_local_t));
  laccel->accel = accel;
  laccel->closure = closure;
  g_closure_ref(closure);
  module->accel_closures_local =
      g_slist_prepend(module->accel_closures_local, laccel);
}

static dt_accel_t* _lookup_accel(gchar *path)
{
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t*)l->data;
    if(!strcmp(accel->path, path))
      return accel;
    l = g_slist_next(l);
  }
  return NULL;
}

void dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path,
                          GClosure *closure)
{
  dt_accel_t *accel = NULL;
  gchar accel_path[256];
  dt_accel_path_iop(accel_path, 256, module->op, path);

  // Looking up the entry in the global accelerators list
  accel = _lookup_accel(accel_path);

  if(accel && accel->local)
  {
    // Local accelerators don't actually get connected, just added to the list
    // They will be connected if/when the module gains focus
    _connect_local_accel(module, accel, closure);
  }
  else
  {
    gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path,
                                    closure);
    module->accel_closures = g_slist_prepend(module->accel_closures, closure);
  }
}

void dt_accel_connect_lib(dt_lib_module_t *module, const gchar *path,
                          GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_lib(accel_path, 256, module->plugin_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path,
                                  closure);

  module->accel_closures = g_slist_prepend(module->accel_closures, closure);
}

static void _press_button_callback(GtkAccelGroup *accel_group,
                                   GObject *acceleratable,
                                   guint keyval, GdkModifierType modifier,
                                   gpointer data)
{
  if(!(GTK_IS_BUTTON(data)))
    return;

  g_signal_emit_by_name(G_OBJECT(data),"activate");
}

void dt_accel_connect_button_iop(dt_iop_module_t *module, const gchar *path,
                                 GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback),
                                     button, NULL);
  dt_accel_connect_iop(module, path, closure);
}

void dt_accel_connect_button_lib(dt_lib_module_t *module, const gchar *path,
                                 GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback),
                                     (gpointer)button, NULL);
  dt_accel_connect_lib(module, path, closure);
}


static void slider_edit_callback(GtkAccelGroup *accel_group,
                                    GObject *acceleratable, guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
	GtkDarktableSlider *slider=DTGTK_SLIDER(data);
	char sv[32]= {0};
	slider->is_entry_active=TRUE;
	gdouble value = gtk_adjustment_get_value(slider->adjustment);
	sprintf(sv,"%.*f",slider->digits,value);
	gtk_entry_set_text (GTK_ENTRY(slider->entry),sv);
	gtk_widget_show (GTK_WIDGET(slider->entry));
	gtk_widget_grab_focus (GTK_WIDGET(slider->entry));
	gtk_widget_queue_draw (GTK_WIDGET(slider));
}
static void slider_increase_callback(GtkAccelGroup *accel_group,
                                    GObject *acceleratable, guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
	GtkDarktableSlider *slider=DTGTK_SLIDER(data);
	float value = gtk_adjustment_get_value(slider->adjustment);
	value += gtk_adjustment_get_step_increment(slider->adjustment);
	if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);

	gtk_adjustment_set_value(slider->adjustment, value);
	gtk_widget_draw(GTK_WIDGET(slider),NULL);
	g_signal_emit_by_name(G_OBJECT(slider),"value-changed");
}
static void slider_decrease_callback(GtkAccelGroup *accel_group,
                                    GObject *acceleratable, guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
	GtkDarktableSlider *slider=DTGTK_SLIDER(data);
	float value = gtk_adjustment_get_value(slider->adjustment);
	value -= gtk_adjustment_get_step_increment(slider->adjustment);
	if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);

	gtk_adjustment_set_value(slider->adjustment, value);
	gtk_widget_draw(GTK_WIDGET(slider),NULL);
	g_signal_emit_by_name(G_OBJECT(slider),"value-changed");
}

static void slider_reset_callback(GtkAccelGroup *accel_group,
                                    GObject *acceleratable, guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
	GtkDarktableSlider *slider=DTGTK_SLIDER(data);
	gtk_adjustment_set_value(slider->adjustment, slider->default_value);
	gtk_widget_draw(GTK_WIDGET(slider),NULL);
	g_signal_emit_by_name(G_OBJECT(slider),"value-changed");
}
void dt_accel_connect_slider_iop(dt_iop_module_t *module, const gchar *path,
                                 GtkWidget *slider)
{
  gchar increase_path[256];
  gchar decrease_path[256];
  gchar reset_path[256];
  gchar edit_path[256];
  dt_accel_t *accel = NULL;
  GClosure *closure;
  char *paths[] = {increase_path, decrease_path, reset_path, edit_path};
  dt_accel_paths_slider_iop(paths, 256, module->op, path);

  closure =  g_cclosure_new(G_CALLBACK(slider_increase_callback),
                            (gpointer)slider, NULL);
  accel = _lookup_accel(increase_path);
  if(accel && accel->local)
  {
    _connect_local_accel(module, accel, closure);
  }
  else
  {
    gtk_accel_group_connect_by_path(darktable.control->accelerators,
                                    increase_path, closure);
    module->accel_closures = g_slist_prepend(module->accel_closures, closure);
  }

  closure = g_cclosure_new(G_CALLBACK(slider_decrease_callback),
                           (gpointer)slider, NULL);
  accel = _lookup_accel(decrease_path);
  if(accel && accel->local)
  {
    _connect_local_accel(module, accel, closure);
  }
  else
  {
    gtk_accel_group_connect_by_path(darktable.control->accelerators,
                                    decrease_path, closure);
    module->accel_closures = g_slist_prepend(module->accel_closures, closure);
  }

  closure = g_cclosure_new(G_CALLBACK(slider_reset_callback),
                           (gpointer)slider, NULL);
  accel = _lookup_accel(reset_path);
  if(accel && accel->local)
  {
    _connect_local_accel(module, accel, closure);
  }
  else
  {
    gtk_accel_group_connect_by_path(darktable.control->accelerators,
                                    reset_path, closure);
    module->accel_closures = g_slist_prepend(module->accel_closures, closure);
  }

  closure = g_cclosure_new(G_CALLBACK(slider_edit_callback),
                           (gpointer)slider, NULL);
  accel = _lookup_accel(edit_path);
  if(accel && accel->local)
  {
    _connect_local_accel(module, accel, closure);
  }
  else
  {
    gtk_accel_group_connect_by_path(darktable.control->accelerators,
                                    edit_path, closure);
    module->accel_closures = g_slist_prepend(module->accel_closures, closure);
  }
}

void dt_accel_connect_locals_iop(dt_iop_module_t *module)
{
  dt_accel_local_t *accel;
  GSList *l = module->accel_closures_local;

  while(l)
  {
    accel = (dt_accel_local_t*)l->data;
    gtk_accel_group_connect_by_path(darktable.control->accelerators,
                                    accel->accel->path, accel->closure);
    l = g_slist_next(l);
  }

  module->local_closures_connected = TRUE;
}

void dt_accel_disconnect_list(GSList *list)
{
  GClosure *closure;
  while(list)
  {
    closure = (GClosure*)(list->data);
    gtk_accel_group_disconnect(darktable.control->accelerators, closure);
    list = g_slist_delete_link(list, list);
  }
}

void dt_accel_disconnect_locals_iop(dt_iop_module_t *module)
{
  dt_accel_local_t *accel;
  GSList *l = module->accel_closures_local;

  if(!module->local_closures_connected)
    return;

  while(l)
  {
    accel = (dt_accel_local_t*)l->data;
    gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    l = g_slist_next(l);
  }

  module->local_closures_connected = FALSE;
}

void dt_accel_cleanup_locals_iop(dt_iop_module_t *module)
{
  dt_accel_local_t *accel;
  GSList *l = module->accel_closures_local;
  while(l)
  {
    accel = (dt_accel_local_t*)l->data;
    if(module->local_closures_connected)
      gtk_accel_group_disconnect(darktable.control->accelerators,
                                 accel->closure);
    g_closure_unref(accel->closure);
    free(accel);
    l = g_slist_delete_link(l, l);
  }
  module->accel_closures_local = NULL;
}
