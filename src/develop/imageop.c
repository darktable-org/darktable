/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "common/opencl.h"
#include "common/dtpthread.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "dtgtk/button.h"

#include <strings.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <xmmintrin.h>

void dt_iop_load_default_params(dt_iop_module_t *module)
{
  const void *blob = NULL;
  memcpy(module->default_params, module->factory_params, module->params_size);
  module->default_enabled = module->factory_enabled;

  // select matching default:
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select op_params, enabled, operation from presets where operation = ?1 and "
      "autoapply=1 and "
      "?2 like model and ?3 like maker and ?4 like lens and "
      "?5 between iso_min and iso_max and "
      "?6 between exposure_min and exposure_max and "
      "?7 between aperture_min and aperture_max and "
      "?8 between focal_length_min and focal_length_max and "
      "(isldr = 0 or isldr=?9) order by length(model) desc, length(maker) desc, length(lens) desc", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, module->dev->image->exif_model, strlen(module->dev->image->exif_model), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, module->dev->image->exif_maker, strlen(module->dev->image->exif_maker), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, module->dev->image->exif_lens,  strlen(module->dev->image->exif_lens),  SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 5, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_iso)));
  sqlite3_bind_double(stmt, 6, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_exposure)));
  sqlite3_bind_double(stmt, 7, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_aperture)));
  sqlite3_bind_double(stmt, 8, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw
  sqlite3_bind_double(stmt, 9, 2-dt_image_is_ldr(module->dev->image));

#if 0 // debug the query:
  printf("select op_params, enabled from presets where operation ='%s' and "
      "autoapply=1 and "
      "'%s' like model and '%s' like maker and '%s' like lens and "
      "%f between iso_min and iso_max and "
      "%f between exposure_min and exposure_max and "
      "%f between aperture_min and aperture_max and "
      "%f between focal_length_min and focal_length_max and "
      "(isldr = 0 or isldr=%d) order by length(model) desc, length(maker) desc, length(lens) desc;\n",
   module->op,
   module->dev->image->exif_model,
   module->dev->image->exif_maker,
   module->dev->image->exif_lens,
   fmaxf(0.0f, fminf(1000000, module->dev->image->exif_iso)),
   fmaxf(0.0f, fminf(1000000, module->dev->image->exif_exposure)),
   fmaxf(0.0f, fminf(1000000, module->dev->image->exif_aperture)),
   fmaxf(0.0f, fminf(1000000, module->dev->image->exif_focal_length)),
   2-dt_image_is_ldr(module->dev->image));
#endif

  if(sqlite3_step(stmt) == SQLITE_ROW)
  { // try to find matching entry
    blob  = sqlite3_column_blob(stmt, 0);
    int length  = sqlite3_column_bytes(stmt, 0);
    int enabled = sqlite3_column_int(stmt, 1);
    if(blob && length == module->params_size)
    {
      // printf("got default for image %d and operation %s\n", module->dev->image->id, sqlite3_column_text(stmt, 2));
      memcpy(module->default_params, blob, length);
      module->default_enabled = enabled;
    }
    else blob = (void *)1;
  }
  else
  { // global default
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(darktable.db, "select op_params, enabled from presets where operation = ?1 and def=1", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      blob  = sqlite3_column_blob(stmt, 0);
      int length  = sqlite3_column_bytes(stmt, 0);
      int enabled = sqlite3_column_int(stmt, 1);
      if(blob && length == module->params_size)
      {
        memcpy(module->default_params, blob, length);
        module->default_enabled = enabled;
      }
      else blob = (void *)1;
    }
  }
  sqlite3_finalize(stmt);

  if(blob == (void *)1)
  {
    printf("[iop_load_defaults]: module param sizes have changed! removing default :(\n");
    sqlite3_prepare_v2(darktable.db, "delete from presets where operation = ?1 and def=1", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_iop_modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
}

void dt_iop_modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

gint sort_plugins(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_t *am = (const dt_iop_module_t *)a;
  const dt_iop_module_t *bm = (const dt_iop_module_t *)b;
  return am->priority - bm->priority;
}

/* default groups for modules which does not implement the groups() function */
int _default_groups() { return IOP_GROUP_ALL; }

/* default flags for modules which does not implement the flags() function */
int _default_flags() { return 0; }

/* default bytes per pixel: 4*sizeof(float). */
int _default_output_bpp(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece)
{
  return 4*sizeof(float);
}

int dt_iop_load_module(dt_iop_module_t *module, dt_develop_t *dev, const char *libname, const char *op)
{
  dt_pthread_mutex_init(&module->params_mutex, NULL);
  module->dt = &darktable;
  module->dev = dev;
  module->widget = NULL;
  module->off = NULL;
  module->priority = 0;
  module->hide_enable_button = 0;
  module->request_color_pick = 0;
  for(int k=0;k<3;k++)
  {
    module->picked_color[k] = 
    module->picked_color_min[k] = 
    module->picked_color_max[k] = 
    module->picked_color_Lab[k] = 
    module->picked_color_min_Lab[k] = 
    module->picked_color_max_Lab[k] = 0.0f;
  }
  module->color_picker_box[0] = module->color_picker_box[1] = .25f;
  module->color_picker_box[2] = module->color_picker_box[3] = .75f;
  module->enabled = module->default_enabled = 1; // all modules enabled by default.
  strncpy(module->op, op, 20);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  if(!module->module) goto error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer)&(version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr, "[iop_load_module] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n", libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()), dt_version() < 0 ? "debug" : "opt");
    goto error;
  }
  if(!g_module_symbol(module->module, "dt_module_mod_version",  (gpointer)&(module->version)))                goto error;
  if(!g_module_symbol(module->module, "name",                   (gpointer)&(module->name)))                   goto error;
  if(!g_module_symbol(module->module, "groups",                 (gpointer)&(module->groups)))                 module->groups = _default_groups;
  if(!g_module_symbol(module->module, "flags",                  (gpointer)&(module->flags)))                  module->flags = _default_flags;
  if(!g_module_symbol(module->module, "output_bpp",             (gpointer)&(module->output_bpp)))             module->output_bpp = _default_output_bpp;
  if(!g_module_symbol(module->module, "gui_update",             (gpointer)&(module->gui_update)))             goto error;
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

  if(!g_module_symbol(module->module, "init",                   (gpointer)&(module->init)))                   goto error;
  if(!g_module_symbol(module->module, "cleanup",                (gpointer)&(module->cleanup)))                goto error;
  if(!g_module_symbol(module->module, "init_presets",           (gpointer)&(module->init_presets)))           module->init_presets = NULL;
  if(!g_module_symbol(module->module, "commit_params",          (gpointer)&(module->commit_params)))          goto error;
  if(!g_module_symbol(module->module, "reload_defaults",        (gpointer)&(module->reload_defaults)))        module->reload_defaults = NULL;
  if(!g_module_symbol(module->module, "init_pipe",              (gpointer)&(module->init_pipe)))              goto error;
  if(!g_module_symbol(module->module, "cleanup_pipe",           (gpointer)&(module->cleanup_pipe)))           goto error;
  if(!g_module_symbol(module->module, "process",                (gpointer)&(module->process)))                goto error;
  if(!darktable.opencl->inited ||
     !g_module_symbol(module->module, "process_cl",             (gpointer)&(module->process_cl)))             module->process_cl = NULL;
  if(!g_module_symbol(module->module, "modify_roi_in",          (gpointer)&(module->modify_roi_in)))          module->modify_roi_in = dt_iop_modify_roi_in;
  if(!g_module_symbol(module->module, "modify_roi_out",         (gpointer)&(module->modify_roi_out)))         module->modify_roi_out = dt_iop_modify_roi_out;
  module->init(module);
  if(module->priority == 0)
  {
    fprintf(stderr, "[iop_load_module] %s needs to set priority!\n", op);
    goto error;      // this needs to be set
  }
  module->enabled = module->default_enabled; // apply (possibly new) default.
  return 0;
error:
  fprintf(stderr, "[iop_load_module] failed to open operation `%s': %s\n", op, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
}

static void
init_presets(dt_iop_module_t *module)
{
  if(module->init_presets)
  { // only if method exists and no writeprotected (static) preset has been inserted yet.
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(darktable.db, "select * from presets where operation=?1 and writeprotect=1", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_ROW) module->init_presets(module);
    sqlite3_finalize(stmt);
  }
}

GList *dt_iop_load_modules(dt_develop_t *dev)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dev->iop_instance = 0;
  char plugindir[1024], op[20];
  const gchar *d_name;
  dt_get_plugindir(plugindir, 1024);
  strcpy(plugindir + strlen(plugindir), "/plugins");
  GDir *dir = g_dir_open(plugindir, 0, NULL); 
  if(!dir) return NULL;
  while((d_name = g_dir_read_name(dir)))
  { // get lib*.so
    if(strncmp(d_name, "lib", 3)) continue;
    if(strncmp(d_name + strlen(d_name) - 3, ".so", 3)) continue;
    strncpy(op, d_name+3, strlen(d_name)-6);
    op[strlen(d_name)-6] = '\0';
    module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
    gchar *libname = g_module_build_path(plugindir, (const gchar *)op);
    if(dt_iop_load_module(module, dev, libname, op))
    {
      free(module);
      continue;
    }
    g_free(libname);
    res = g_list_insert_sorted(res, module, sort_plugins);
    module->factory_params = malloc(module->params_size);
    memcpy(module->factory_params, module->default_params, module->params_size);
    module->factory_enabled = module->default_enabled;
    init_presets(module);
    dt_iop_load_default_params(module);
  }
  g_dir_close(dir);

  GList *it = res;
  while(it)
  {
    module = (dt_iop_module_t *)it->data;
    module->instance = dev->iop_instance++;
    // printf("module %d - %s %d\n", module->priority, module->op, module->instance);
    it = g_list_next(it);
  }
  return res;
}

void dt_iop_unload_module(dt_iop_module_t *module)
{
  free(module->factory_params);
  module->cleanup(module);
  free(module->default_params);
  dt_pthread_mutex_destroy(&module->params_mutex);
  if(module->module) g_module_close(module->module);
}

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  uint64_t hash = 5381;
  piece->hash = 0;
  module->commit_params(module, params, pipe, piece);
  const char *str = (const char *)params;
  if(piece->enabled)
  {
    for(int i=0;i<module->params_size;i++) hash = ((hash << 5) + hash) ^ str[i];
    piece->hash = hash;
  }
  // printf("commit params hash += module %s: %lu, enabled = %d\n", piece->module->op, piece->hash, piece->enabled);
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  module->gui_update(module);
  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
  darktable.gui->reset = reset;
}

void dt_iop_gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset)
  {
    if(gtk_toggle_button_get_active(togglebutton)) module->enabled = 1;
    else module->enabled = 0;
    dt_dev_add_history_item(module->dev, module);
    // close parent expander.
    gtk_expander_set_expanded(module->expander, module->enabled);
  }
  char tooltip[512];
  snprintf(tooltip, 512, module->enabled ? _("%s is switched on") : _("%s is switched off"), module->name());
  gtk_object_set(GTK_OBJECT(togglebutton), "tooltip-text", tooltip, (char *)NULL);
}

static void dt_iop_gui_expander_callback(GObject *object, GParamSpec *param_spec, gpointer user_data)
{
  GtkExpander *expander = GTK_EXPANDER (object);
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if (gtk_expander_get_expanded (expander))
  {
    gtk_widget_show(module->widget);
    // register to receive draw events
    dt_iop_request_focus(module);
    // hide all other module widgets
#if 0 // TODO: make this an option. it is quite annoying when using expose/tonecurve together.
    GList *iop = module->dev->iop;
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
      if(m != module) gtk_expander_set_expanded(m->expander, FALSE);
      iop = g_list_next(iop);
    }
#endif
    GtkContainer *box = GTK_CONTAINER(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
    gtk_container_set_focus_child(box, module->topwidget);
    // redraw gui (in case post expose is set)
    gtk_widget_queue_resize(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
    dt_control_gui_queue_draw();
  }
  else
  {
    if(module->dev->gui_module == module)
    {
      dt_iop_request_focus(NULL);
      dt_control_gui_queue_draw();
    }
    gtk_widget_hide(module->widget);
  }
}

static void
dt_iop_gui_reset_callback(GtkButton *button, dt_iop_module_t *module)
{
  // module->enabled = module->default_enabled; // will not propagate correctly anyways ;)
  memcpy(module->params, module->default_params, module->params_size);
  module->gui_update(module);
  if(strcmp(module->op, "rawimport")) dt_dev_add_history_item(module->dev, module);
}

static void 
_preset_popup_posistion(GtkMenu *menu, gint *x,gint *y,gboolean *push_in, gpointer data)
{
  gint w,h;
  GtkRequisition requisition;
  gdk_window_get_size(GTK_WIDGET(data)->window,&w,&h);
  gdk_window_get_origin (GTK_WIDGET(data)->window, x, y);
  gtk_widget_size_request (GTK_WIDGET (menu), &requisition);
  (*x)+=w-requisition.width;
  (*y)+=GTK_WIDGET(data)->allocation.height;
}

static gboolean
popup_button_callback(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *module)
{
  if(event->button == 3)
  {
    dt_gui_presets_popup_menu_show_for_module(module);
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, event->button, event->time);
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));
    return TRUE;
  }
  return FALSE;
}

static void
popup_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_gui_presets_popup_menu_show_for_module(module);
  gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, _preset_popup_posistion, button, 0,gtk_get_current_event_time());
  gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));
  gtk_menu_reposition(GTK_MENU(darktable.gui->presets_popup_menu));
}

void dt_iop_request_focus(dt_iop_module_t *module)
{
  if(darktable.develop->gui_module)
  {
    gtk_widget_set_state(darktable.develop->gui_module->topwidget, GTK_STATE_NORMAL);
    GtkWidget *off = GTK_WIDGET(darktable.develop->gui_module->off);
    if(off) gtk_widget_set_state(off, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(off)) ? GTK_STATE_ACTIVE : GTK_STATE_NORMAL);
  }
  darktable.develop->gui_module = module;
  if(module)
  {
    gtk_widget_set_state(module->topwidget, GTK_STATE_SELECTED);
    gtk_widget_set_state(module->widget,    GTK_STATE_NORMAL);
    GtkWidget *off = GTK_WIDGET(darktable.develop->gui_module->off);
    if(off) gtk_widget_set_state(off, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(off)) ? GTK_STATE_ACTIVE : GTK_STATE_NORMAL);
  }
  dt_control_change_cursor(GDK_LEFT_PTR);
}

GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module)
{
  GtkHBox *hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  module->expander = GTK_EXPANDER(gtk_expander_new((const gchar *)(module->name())));
  // gamma is always needed for display (down to uint8_t)
  // colorin/colorout are essential for La/Lb/L conversion.
  if(!module->hide_enable_button)
  {
    // GtkToggleButton *button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    GtkDarktableToggleButton *button = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
    gtk_widget_set_size_request(GTK_WIDGET(button), 13, 13);
    char tooltip[512];
    snprintf(tooltip, 512, module->enabled ? _("%s is switched on") : _("%s is switched off"), module->name());
    gtk_object_set(GTK_OBJECT(button), "tooltip-text", tooltip, (char *)NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), module->enabled);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(button), FALSE, FALSE, 0);
    g_signal_connect (G_OBJECT (button), "toggled",
                      G_CALLBACK (dt_iop_gui_off_callback), module);
    module->off = button;
  }
  else
  {
     GtkWidget *w = gtk_expander_get_label_widget (module->expander);
     gtk_misc_set_padding(GTK_MISC(w), 13, 0);
  }

  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(module->expander), TRUE, TRUE, 0);
  GtkDarktableButton *resetbutton = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  GtkDarktableButton *presetsbutton = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  gtk_widget_set_size_request(GTK_WIDGET(presetsbutton),13,13);
  gtk_widget_set_size_request(GTK_WIDGET(resetbutton),13,13);
  gtk_object_set(GTK_OBJECT(resetbutton), "tooltip-text", _("reset parameters"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(presetsbutton), "tooltip-text", _("presets"), (char *)NULL);
  gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(resetbutton), FALSE, FALSE, 0);
  gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(presetsbutton), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  GtkWidget *al = gtk_alignment_new(1.0, 1.0, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(al), 10, 10, 10, 5);
  gtk_box_pack_start(GTK_BOX(vbox), al, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(al), module->widget);

  g_signal_connect (G_OBJECT (resetbutton), "clicked",
                    G_CALLBACK (dt_iop_gui_reset_callback), module);
  g_signal_connect (G_OBJECT (presetsbutton), "clicked",
                    G_CALLBACK (popup_callback), module);
  g_signal_connect (G_OBJECT (module->expander), "notify::expanded",
                  G_CALLBACK (dt_iop_gui_expander_callback), module);
  gtk_expander_set_spacing(module->expander, 10);
  gtk_widget_hide_all(module->widget);
  gtk_expander_set_expanded(module->expander, FALSE);
  GtkWidget *evb = gtk_event_box_new();
  gtk_container_set_border_width(GTK_CONTAINER(evb), 0);
  gtk_container_add(GTK_CONTAINER(evb), GTK_WIDGET(vbox));

  gtk_widget_set_events(evb, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(evb), "button-press-event", G_CALLBACK(popup_button_callback), module);
  return evb;
}

int dt_iop_breakpoint(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe)
{
  if(pipe != dev->preview_pipe) sched_yield();
  if(pipe != dev->preview_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
  if((pipe->changed != DT_DEV_PIPE_UNCHANGED && pipe->changed != DT_DEV_PIPE_ZOOMED) || dev->gui_leaving) return 1;
  return 0;
}

void dt_iop_clip_and_zoom_8(const uint8_t *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                                  uint8_t *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh)
{
  const float scalex = iw/(float)ow;
  const float scaley = ih/(float)oh;
  int32_t ix2 = MAX(ix, 0);
  int32_t iy2 = MAX(iy, 0);
  int32_t ox2 = MAX(ox, 0);
  int32_t oy2 = MAX(oy, 0);
  int32_t oh2 = MIN(MIN(oh, (ibh - iy2)/scaley), obh - oy2);
  int32_t ow2 = MIN(MIN(ow, (ibw - ix2)/scalex), obw - ox2);
  assert((int)(ix2 + ow2*scalex) <= ibw);
  assert((int)(iy2 + oh2*scaley) <= ibh);
  assert(ox2 + ow2 <= obw);
  assert(oy2 + oh2 <= obh);
  assert(ix2 >= 0 && iy2 >= 0 && ox2 >= 0 && oy2 >= 0);
  float x = ix2, y = iy2;
  for(int s=0;s<oh2;s++)
  {
    int idx = ox2 + obw*(oy2+s);
    for(int t=0;t<ow2;t++)
    {
      for(int k=0;k<3;k++) o[4*idx + k] =  //i[3*(ibw* (int)y +             (int)x             ) + k)];
       CLAMP(((int32_t)i[(4*(ibw*(int32_t) y +            (int32_t) (x + .5f*scalex)) + k)] +
              (int32_t)i[(4*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x + .5f*scalex)) + k)] +
              (int32_t)i[(4*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x             )) + k)] +
              (int32_t)i[(4*(ibw*(int32_t) y +            (int32_t) (x             )) + k)])/4, 0, 255);
      x += scalex; idx++;
    }
    y += scaley; x = ix2;
  }
}


void
dt_iop_clip_and_zoom(float *out, const float *const in,
    const dt_iop_roi_t *const roi_out, const dt_iop_roi_t * const roi_in, const int32_t out_stride, const int32_t in_stride)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = .9f/roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = ((int)px_footprint)/2;

  // init gauss with sigma = samples (half footprint)
  float filter[2*samples + 1];
  float sum = 0.0f;
  if(samples)
  {
    for(int i=-samples;i<=samples;i++) sum += (filter[i+samples] = expf(-i*i/(float)(.5f*samples*samples)));
    for(int k=0;k<2*samples+1;k++) filter[k] /= sum;
  }
  else filter[0] = 1.0f;

  // FIXME: ??
  const int offx = 0;//MAX(0, samples - roi_out->x);
  const int offy = 0;//MAX(0, samples - roi_out->y);
  const int offX = 0;//MAX(0, samples - (roi_in->width  - (roi_out->x + roi_out->width)));
  const int offY = 0;//MAX(0, samples - (roi_in->height - (roi_out->y + roi_out->height)));

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out, filter)
#endif
  for(int y=offy;y<roi_out->height-offY;y++)
  {
    float *outc = out + 4*(out_stride*y + offx);
    for(int x=offx;x<roi_out->width-offX;x++)
    {
      __m128 col = _mm_setzero_ps();
      // _mm_prefetch
      // upper left corner:
      int px = (x + roi_out->x + 0.5f)/roi_out->scale, py = (y + roi_out->y + 0.5f)/roi_out->scale;
      // const float *inc = in + 4*(py*roi_in->width + px);

      float num=0.0f;
      // for(int j=-samples;j<=samples;j++) for(int i=-samples;i<=samples;i++)
      for(int j=MAX(0, py-samples);j<=MIN(roi_in->height-1, py+samples);j++)
      for(int i=MAX(0, px-samples);i<=MIN(roi_in->width -1, px+samples);i++)
      {
        __m128 p = _mm_load_ps(in + 4*(i + in_stride*j));

        // col = _mm_add_ps(col, p);
        // num++;

        const float f = filter[i-px]*filter[j-py];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(f), p));
        num += f;
      }
      // col = _mm_mul_ps(col, _mm_set1_ps(1.0f/((2.0f*samples+1.0f)*(2.0f*samples+1.0f))));
      col = _mm_mul_ps(col, _mm_set1_ps(1.0f/num));
      // memcpy(outc, &col, 4*sizeof(float));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}

static float
weight (const float c1, const float c2)
{
  return dt_fast_expf(-(c1-c2)*(c1-c2)/(0.03f*65535.0f*65535.0f));
  // const float d = -fabsf(c1-c2)*(6.f/65535.0f) + 2.0f;
  // return fmaxf(0.0f, d*d*0.25f);
}
/**
 * downscales and clips a mosaiced buffer (in) to the given region of interest (r_*)
 * and writes it to out in float4 format.
 * filters is the dcraw supplied int encoding of the bayer pattern, flipped with the buffer.
 * resamping is done via bilateral filtering and respecting the input mosaic pattern.
 */
void
dt_iop_clip_and_zoom_demosaic_half_size(float *out, const uint16_t *const in,
    const dt_iop_roi_t *const roi_out, const dt_iop_roi_t * const roi_in, const int32_t out_stride, const int32_t in_stride, const unsigned int filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = .9f/roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = ((int)px_footprint)/2;

  // init gauss with sigma = samples (half footprint)
  float filter[2*samples + 1];
  float sum = 0.0f;
  if(samples)
  {
    for(int i=-samples;i<=samples;i++) sum += (filter[i+samples] = expf(-i*i/(float)(.5f*samples*samples)));
    for(int k=0;k<2*samples+1;k++) filter[k] /= sum;
  }
  else filter[0] = 1.0f;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out, filter) schedule(static)
#endif
  for(int y=0;y<roi_out->height;y++)
  {
    float *outc = out + 4*out_stride*y;
    for(int x=0;x<roi_out->width;x++)
    {
      __m128 col = _mm_setzero_ps();
      // _mm_prefetch
      // upper left corner:
      int px = (x + roi_out->x + 0.5f)/roi_out->scale, py = (y + roi_out->y + 0.5f)/roi_out->scale;

      // round down to next even number:
      px = MAX(0, px & ~1);
      py = MAX(0, py & ~1);

      // now move p to point to an rggb block:
      if(FC(py, px+1, filters) != 1) px ++;
      if(FC(py, px,   filters) != 0) { px ++; py ++; }

      px = MIN(roi_in->width-2,  px);
      py = MIN(roi_in->height-2, py);

      const float pc1 = in[px   + in_stride*py];
      const float pc2 = in[px+1 + in_stride*py];
      const float pc3 = in[px   + in_stride*(py + 1)];
      const float pc4 = in[px+1 + in_stride*(py + 1)];

      // float num = 0.0f;
      __m128 num = _mm_setzero_ps();
      for(int j=MAX(0, py-2*samples);j<=MIN(roi_in->height-2, py+2*samples);j+=2)
      for(int i=MAX(0, px-2*samples);i<=MIN(roi_in->width-2,  px+2*samples);i+=2)
      {
        // get four mosaic pattern uint16:
        const float p1 = in[i   + in_stride*j];
        const float p2 = in[i+1 + in_stride*j];
        const float p3 = in[i   + in_stride*(j + 1)];
        const float p4 = in[i+1 + in_stride*(j + 1)];
        // color += filter[j+samples]*filter[i+samples]*(float4)(p1.x/65535.0f, (p2.x+p3.x)/(2.0f*65535.0f), p4.x/65535.0f, 0.0f);
        // color += (float4)(p1.x/65535.0f, (p2.x+p3.x)/(2.0f*65535.0f), p4.x/65535.0f, 0.0f);

        // col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4, .5f*(p2+p3), p1), _mm_set1_ps(1.0/65535.0f)));
        // num ++;
        const float wr = weight(pc1, p1);
        const float wg = weight(.5f*(pc2+pc3), .5f*(p2+p3));
        const float wb = weight(pc4, p4);

        const float f = filter[(i-px)/2+samples]*filter[(j-py)/2+samples];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4*wb, .5f*(p2+p3)*wg, p1*wr), _mm_set1_ps(f/65535.0f)));
        num = _mm_add_ps(num, _mm_mul_ps(_mm_set_ps(1.0f, wb, wg, wr), _mm_set1_ps(f)));
        // col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4, .5f*(p2+p3), p1), _mm_set1_ps(f/65535.0f)));
        // num += f;
      }
      col = _mm_div_ps(col, num);
      // col = _mm_mul_ps(col, _mm_rcp_ps(num));
      // col = _mm_mul_ps(col, _mm_set1_ps(1.0f/num));
      // col = _mm_mul_ps(col, _mm_set1_ps(1.0f/((2.0f*samples+1.0f)*(2.0f*samples+1.0f))));
      // memcpy(outc, &col, 4*sizeof(float));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}

void
dt_iop_clip_and_zoom_demosaic_half_size_f(float *out, const float *const in,
    const dt_iop_roi_t *const roi_out, const dt_iop_roi_t * const roi_in, const int32_t out_stride, const int32_t in_stride, const unsigned int filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = .9f/roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = ((int)px_footprint)/2;

  // init gauss with sigma = samples (half footprint)
  float filter[2*samples + 1];
  float sum = 0.0f;
  if(samples)
  {
    for(int i=-samples;i<=samples;i++) sum += (filter[i+samples] = expf(-i*i/(float)(.5f*samples*samples)));
    for(int k=0;k<2*samples+1;k++) filter[k] /= sum;
  }
  else filter[0] = 1.0f;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out, filter) schedule(static)
#endif
  for(int y=0;y<roi_out->height;y++)
  {
    float *outc = out + 4*(out_stride*y);
    for(int x=0;x<roi_out->width;x++)
    {
      __m128 col = _mm_setzero_ps();
      // _mm_prefetch
      // upper left corner:
      int px = (x + roi_out->x + 0.5f)/roi_out->scale, py = (y + roi_out->y + 0.5f)/roi_out->scale;

      // round down to next even number:
      px = MAX(0, px & ~1);
      py = MAX(0, py & ~1);

      // now move p to point to an rggb block:
      if(FC(py, px+1, filters) != 1) px ++;
      if(FC(py, px,   filters) != 0) { px ++; py ++; }

      float num = 0.0f;
      for(int j=MAX(0, py-2*samples);j<=MIN(roi_in->height-2, py+2*samples);j+=2)
      for(int i=MAX(0, px-2*samples);i<=MIN(roi_in->width-2,  px+2*samples);i+=2)
      {
        // get four mosaic pattern uint16:
        float p1 = in[i   + in_stride*j];
        float p2 = in[i+1 + in_stride*j];
        float p3 = in[i   + in_stride*(j + 1)];
        float p4 = in[i+1 + in_stride*(j + 1)];

        // col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4, .5f*(p2+p3), p1), _mm_set1_ps(1.0/65535.0f)));
        // num ++;

        const float f = filter[(i-px)/2+samples]*filter[(j-py)/2+samples];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4, .5f*(p2+p3), p1), _mm_set1_ps(f)));
        num += f;
      }
      col = _mm_mul_ps(col, _mm_set1_ps(1.0f/num));
      // col = _mm_mul_ps(col, _mm_set1_ps(1.0f/((2.0f*samples+1.0f)*(2.0f*samples+1.0f))));
      // memcpy(outc, &col, 4*sizeof(float));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}

void dt_iop_RGB_to_YCbCr(const float *rgb, float *yuv)
{
  yuv[0] =  0.299*rgb[0] + 0.587*rgb[1] + 0.114*rgb[2];
  yuv[1] = -0.147*rgb[0] - 0.289*rgb[1] + 0.437*rgb[2];
  yuv[2] =  0.615*rgb[0] - 0.515*rgb[1] - 0.100*rgb[2];
}

void dt_iop_YCbCr_to_RGB(const float *yuv, float *rgb)
{
  rgb[0] = yuv[0]                + 1.140*yuv[2];
  rgb[1] = yuv[0] - 0.394*yuv[1] - 0.581*yuv[2];
  rgb[2] = yuv[0] + 2.028*yuv[1];
}

