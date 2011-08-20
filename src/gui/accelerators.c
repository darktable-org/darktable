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

void dt_accel_register_lib(dt_lib_module_t *self, gboolean local,
                           const gchar *path, guint accel_key,
                           GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t*)malloc(sizeof(dt_accel_t));

  dt_accel_path_lib(accel_path, 256, self->plugin_name, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);
  strcpy(accel->path, accel_path);
  dt_accel_path_lib_translated(accel_path, 256, self, path);
  strcpy(accel->translated_path, accel_path);

  strcpy(accel->module, self->plugin_name);
  accel->local = local;
  accel->views = self->views();
  darktable.control->accelerator_list =
      g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_slider_iop(dt_iop_module_so_t *so, gboolean local,
                           const gchar *path)
{
  gchar accel_path[256];
  snprintf(accel_path,256,"%s/%s",path,"increase");
  dt_accel_register_iop(so,local,accel_path, 0, 0);

  snprintf(accel_path,256,"%s/%s",path,"reduce");
  dt_accel_register_iop(so,local,accel_path, 0, 0);

  snprintf(accel_path,256,"%s/%s",path,"reset");
  dt_accel_register_iop(so,local,accel_path, 0, 0);

  snprintf(accel_path,256,"%s/%s",path,"edit");
  dt_accel_register_iop(so,local,accel_path, 0, 0);

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

void dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path,
                          GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_iop(accel_path, 256, module->op, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path,
                                  closure);

  module->accel_closures = g_slist_prepend(module->accel_closures, closure);
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
	gchar accel_path[256];
	snprintf(accel_path,256,"%s/%s",path,NC_("accel", "increase"));
	dt_accel_connect_iop(module,
			accel_path,
			g_cclosure_new(
				G_CALLBACK(slider_increase_callback),
				(gpointer)slider, NULL));
	snprintf(accel_path,256,"%s/%s",path,NC_("accel", "reduce"));
	dt_accel_connect_iop(module,
			accel_path,
			g_cclosure_new(
				G_CALLBACK(slider_decrease_callback),
				(gpointer)slider, NULL));
	snprintf(accel_path,256,"%s/%s",path,NC_("accel", "reset"));
	dt_accel_connect_iop(module,
			accel_path,
			g_cclosure_new(
				G_CALLBACK(slider_reset_callback),
				(gpointer)slider, NULL));
	snprintf(accel_path,256,"%s/%s",path,NC_("accel", "edit"));
	dt_accel_connect_iop(module,
			accel_path,
			g_cclosure_new(
				G_CALLBACK(slider_edit_callback),
				(gpointer)slider, NULL));
}
