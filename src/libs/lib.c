#include "libs/lib.h"
#include "gui/gtk.h"
#include "control/conf.h"
#include "control/control.h"
#include <glade/glade.h>
#include <stdlib.h>

gint dt_lib_sort_plugins(gconstpointer a, gconstpointer b)
{
  const dt_lib_module_t *am = (const dt_lib_module_t *)a;
  const dt_lib_module_t *bm = (const dt_lib_module_t *)b;
  return am->priority - bm->priority;
}

int
dt_lib_load_module (dt_lib_module_t *module, const char *libname, const char *plugin_name)
{
  module->dt = &darktable;
  module->widget = NULL;
  module->priority = 0;
  strncpy(module->plugin_name, plugin_name, 20);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  if(!module->module) goto error;
  if(!g_module_symbol(module->module, "name",                   (gpointer)&(module->name)))                   goto error;
  if(!g_module_symbol(module->module, "gui_reset",              (gpointer)&(module->gui_reset)))              goto error;
  if(!g_module_symbol(module->module, "gui_init",               (gpointer)&(module->gui_init)))               goto error;
  if(!g_module_symbol(module->module, "gui_cleanup",            (gpointer)&(module->gui_cleanup)))            goto error;

  if(!g_module_symbol(module->module, "gui_post_expose",        (gpointer)&(module->gui_post_expose)))        module->gui_post_expose = NULL;
  if(!g_module_symbol(module->module, "mouse_leave",            (gpointer)&(module->mouse_leave)))            module->mouse_leave = NULL;
  if(!g_module_symbol(module->module, "mouse_moved",            (gpointer)&(module->mouse_moved)))            module->mouse_moved = NULL;
  if(!g_module_symbol(module->module, "button_released",        (gpointer)&(module->button_released)))        module->button_released = NULL;
  if(!g_module_symbol(module->module, "button_pressed",         (gpointer)&(module->button_pressed)))         module->button_pressed = NULL;
  if(!g_module_symbol(module->module, "key_pressed",            (gpointer)&(module->key_pressed)))            module->key_pressed = NULL;
  if(!g_module_symbol(module->module, "configure",              (gpointer)&(module->configure)))              module->configure = NULL;
  if(!g_module_symbol(module->module, "scrolled",               (gpointer)&(module->scrolled)))               module->scrolled = NULL;

  return 0;
error:
  fprintf(stderr, "[lib_load_module] failed to open operation `%s': %s\n", plugin_name, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
}

int
dt_lib_load_modules ()
{
  darktable.lib->plugins = NULL;
  GList *res = NULL;
  dt_lib_module_t *module;
  char plugindir[1024], plugin_name[256];
  const gchar *d_name;
  dt_get_plugindir(plugindir, 1024);
  strcpy(plugindir + strlen(plugindir), "/plugins/lighttable");
  GDir *dir = g_dir_open(plugindir, 0, NULL);
  if(!dir) return 1;
  while((d_name = g_dir_read_name(dir)))
  { // get lib*.so
    if(strncmp(d_name, "lib", 3)) continue;
    if(strncmp(d_name + strlen(d_name) - 3, ".so", 3)) continue;
    strncpy(plugin_name, d_name+3, strlen(d_name)-6);
    plugin_name[strlen(d_name)-6] = '\0';
    module = (dt_lib_module_t *)malloc(sizeof(dt_lib_module_t));
    gchar *libname = g_module_build_path(plugindir, (const gchar *)plugin_name);
    if(dt_lib_load_module(module, libname, plugin_name))
    {
      free(module);
      continue;
    }
    g_free(libname);
    res = g_list_insert_sorted(res, module, dt_lib_sort_plugins);
  }
  g_dir_close(dir);

  darktable.lib->plugins = res;

  return 0;
}

void
dt_lib_unload_module (dt_lib_module_t *module)
{
  module->gui_cleanup(module);
  if(module->module) g_module_close(module->module);
}

static void
dt_lib_gui_expander_callback (GObject *object, GParamSpec *param_spec, gpointer user_data)
{
  GtkExpander *expander = GTK_EXPANDER (object);
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;

  char var[1024];
  snprintf(var, 1024, "plugins/lighttable/%s/expanded", module->plugin_name);
  dt_conf_set_bool(var, gtk_expander_get_expanded (expander));

  if (gtk_expander_get_expanded (expander))
  {
    gtk_widget_show_all(module->widget);
    // register to receive draw events
    darktable.lib->gui_module = module;
    GtkContainer *box = GTK_CONTAINER(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
    gtk_container_set_focus_child(box, GTK_WIDGET(module->expander));
    // redraw gui (in case post expose is set)
    dt_control_gui_queue_draw();
  }
  else
  {
    if(darktable.lib->gui_module == module)
    {
      darktable.lib->gui_module = NULL;
      dt_control_gui_queue_draw();
    }
    gtk_widget_hide_all(module->widget);
  }
}

static void
dt_lib_gui_reset_callback (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;
  module->gui_reset(module);
}

GtkWidget *
dt_lib_gui_get_expander (dt_lib_module_t *module)
{
  GtkHBox *hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  module->expander = GTK_EXPANDER(gtk_expander_new((const gchar *)(module->name())));

  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(module->expander), TRUE, TRUE, 0);
  GtkButton *resetbutton = GTK_BUTTON(gtk_button_new());
  gtk_object_set(GTK_OBJECT(resetbutton), "tooltip-text", _("reset parameters"), NULL);
  gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(resetbutton), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  GtkWidget *al = gtk_alignment_new(1.0, 1.0, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(al), 10, 10, 10, 5);
  gtk_box_pack_start(GTK_BOX(vbox), al, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(al), module->widget);

  g_signal_connect (G_OBJECT (resetbutton), "pressed",
                    G_CALLBACK (dt_lib_gui_reset_callback), module);
  g_signal_connect (G_OBJECT (module->expander), "notify::expanded",
                  G_CALLBACK (dt_lib_gui_expander_callback), module);
  gtk_expander_set_spacing(module->expander, 10);
  gtk_widget_hide_all(module->widget);
  gtk_expander_set_expanded(module->expander, FALSE);
  GtkWidget *evb = gtk_event_box_new();
  gtk_container_set_border_width(GTK_CONTAINER(evb), 0);
  gtk_container_add(GTK_CONTAINER(evb), GTK_WIDGET(vbox));
  return evb;
}

void
dt_lib_init (dt_lib_t *lib)
{
  lib->gui_module = NULL;
  lib->plugins = NULL;
  (void)dt_lib_load_modules();
}

void
dt_lib_cleanup (dt_lib_t *lib)
{
  while(lib->plugins)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(darktable.lib->plugins->data);
    dt_lib_unload_module(module);
    lib->plugins = g_list_delete_link(lib->plugins, lib->plugins);
  }
}

