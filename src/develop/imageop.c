/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "common/debug.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "gui/gtk.h"
#include "gui/iop_modulegroups.h"
#include "gui/presets.h"
#include "dtgtk/button.h"
#include "dtgtk/slider.h"
// #include "dtgtk/tristatebutton.h" //FIXME

#include <strings.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <xmmintrin.h>

typedef struct _iop_gui_blend_data_t
{
  dt_iop_module_t *module;
  GtkToggleButton *enable;
  GtkVBox *box;
  GtkComboBox *blend_modes_combo;
  GtkWidget *opacity_slider;
} _iop_gui_blend_data_t;

void dt_iop_load_default_params(dt_iop_module_t *module)
{
  const void *op_params = NULL;
  const void *bl_params = NULL;
  memcpy(module->default_params, module->factory_params, module->params_size);
  module->default_enabled = module->factory_enabled;

  dt_develop_blend_params_t default_blendop_params= {DEVELOP_BLEND_DISABLED,100.0,0};
  memset(module->default_blendop_params, 0, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &default_blendop_params, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &default_blendop_params, sizeof(dt_develop_blend_params_t));

  // select matching default:
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select op_params, enabled, operation, blendop_params from presets where operation = ?1 and "
                              "autoapply=1 and "
                              "?2 like model and ?3 like maker and ?4 like lens and "
                              "?5 between iso_min and iso_max and "
                              "?6 between exposure_min and exposure_max and "
                              "?7 between aperture_min and aperture_max and "
                              "?8 between focal_length_min and focal_length_max and "
                              "(isldr = 0 or isldr=?9) order by length(model) desc, length(maker) desc, length(lens) desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, module->dev->image->exif_model, strlen(module->dev->image->exif_model), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, module->dev->image->exif_maker, strlen(module->dev->image->exif_maker), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, module->dev->image->exif_lens,  strlen(module->dev->image->exif_lens),  SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 5, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, module->dev->image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, 2-dt_image_is_ldr(module->dev->image));

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
  {
    // try to find matching entry
    op_params  = sqlite3_column_blob(stmt, 0);
    int op_length  = sqlite3_column_bytes(stmt, 0);
    int enabled = sqlite3_column_int(stmt, 1);
    bl_params = sqlite3_column_blob(stmt, 3);
    int bl_length = sqlite3_column_bytes(stmt, 3);
    if(op_params && (op_length == module->params_size))
    {
      // printf("got default for image %d and operation %s\n", module->dev->image->id, sqlite3_column_text(stmt, 2));
      memcpy(module->default_params, op_params, op_length);
      module->default_enabled = enabled;
      if(bl_params && (bl_length == sizeof(dt_develop_blend_params_t)))
      {
        memcpy(module->default_blendop_params, bl_params, sizeof(dt_develop_blend_params_t));
      }
    }
    else
      op_params = (void *)1;
  }
  else
  {
    // global default
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select op_params, enabled, blendop_params from presets where operation = ?1 and def=1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      op_params  = sqlite3_column_blob(stmt, 0);
      int op_length  = sqlite3_column_bytes(stmt, 0);
      int enabled = sqlite3_column_int(stmt, 1);
      bl_params = sqlite3_column_blob(stmt, 2);
      int bl_length = sqlite3_column_bytes(stmt, 2);
      if(op_params && (op_length == module->params_size))
      {
        memcpy(module->default_params, op_params, op_length);
        module->default_enabled = enabled;
        if(bl_params && (bl_length == sizeof(dt_develop_blend_params_t)))
        {
          memcpy(module->default_blendop_params, bl_params, sizeof(dt_develop_blend_params_t));
        }
      }
      else
        op_params = (void *)1;
    }
  }
  sqlite3_finalize(stmt);

  if(op_params == (void *)1 || bl_params == (void *)1)
  {
    printf("[iop_load_defaults]: module param sizes have changed! removing default :(\n");
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from presets where operation = ?1 and def=1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
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
int _default_groups()
{
  return IOP_GROUP_ALL;
}

/* default flags for modules which does not implement the flags() function */
int _default_flags()
{
  return 0;
}

/* default operation tags for modules which does not implement the flags() function */
int _default_operation_tags()
{
  return 0;
}

/* default operation tags filter for modules which does not implement the flags() function */
int _default_operation_tags_filter()
{
  return 0;
}

/* default bytes per pixel: 4*sizeof(float). */
int _default_output_bpp(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece)
{
  return 4*sizeof(float);
}

int dt_iop_load_module_so(dt_iop_module_so_t *module, const char *libname, const char *op)
{
  g_strlcpy(module->op, op, 20);
  module->data = NULL;
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
  if(!g_module_symbol(module->module, "operation_tags",         (gpointer)&(module->operation_tags)))         module->operation_tags = _default_operation_tags;
  if(!g_module_symbol(module->module, "operation_tags_filter",  (gpointer)&(module->operation_tags_filter)))  module->operation_tags_filter = _default_operation_tags_filter;
  if(!g_module_symbol(module->module, "output_bpp",             (gpointer)&(module->output_bpp)))             module->output_bpp = _default_output_bpp;
  if(!g_module_symbol(module->module, "gui_update",             (gpointer)&(module->gui_update)))             goto error;
  if(!g_module_symbol(module->module, "gui_init",               (gpointer)&(module->gui_init)))               goto error;
  if(!g_module_symbol(module->module, "gui_cleanup",            (gpointer)&(module->gui_cleanup)))            goto error;

  if(!g_module_symbol(module->module, "gui_post_expose",        (gpointer)&(module->gui_post_expose)))        module->gui_post_expose = NULL;
  if(!g_module_symbol(module->module, "init_key_accels", (gpointer)&(module->init_key_accels)))        module->init_key_accels = NULL;
  if(!g_module_symbol(module->module, "mouse_leave",            (gpointer)&(module->mouse_leave)))            module->mouse_leave = NULL;
  if(!g_module_symbol(module->module, "mouse_moved",            (gpointer)&(module->mouse_moved)))            module->mouse_moved = NULL;
  if(!g_module_symbol(module->module, "button_released",        (gpointer)&(module->button_released)))        module->button_released = NULL;
  if(!g_module_symbol(module->module, "button_pressed",         (gpointer)&(module->button_pressed)))         module->button_pressed = NULL;
  if(!g_module_symbol(module->module, "configure",              (gpointer)&(module->configure)))              module->configure = NULL;
  if(!g_module_symbol(module->module, "scrolled",               (gpointer)&(module->scrolled)))               module->scrolled = NULL;

  if(!g_module_symbol(module->module, "init",                   (gpointer)&(module->init)))                   goto error;
  if(!g_module_symbol(module->module, "cleanup",                (gpointer)&(module->cleanup)))                goto error;
  if(!g_module_symbol(module->module, "init_global",            (gpointer)&(module->init_global)))            module->init_global = NULL;
  if(!g_module_symbol(module->module, "cleanup_global",         (gpointer)&(module->cleanup_global)))         module->cleanup_global = NULL;
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
  if(!g_module_symbol(module->module, "legacy_params",          (gpointer)&(module->legacy_params)))          module->legacy_params = NULL;
  if(module->init_global) module->init_global(module);
  return 0;
error:
  fprintf(stderr, "[iop_load_module] failed to open operation `%s': %s\n", op, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
}

static int
dt_iop_load_module_by_so(dt_iop_module_t *module, dt_iop_module_so_t *so, dt_develop_t *dev)
{
  dt_pthread_mutex_init(&module->params_mutex, NULL);
  module->dt = &darktable;
  module->dev = dev;
  module->widget = NULL;
  module->topwidget = NULL;
  module->showhide = NULL;
  module->off = NULL;
  module->priority = 0;
  module->hide_enable_button = 0;
  module->request_color_pick = 0;
  for(int k=0; k<3; k++)
  {
    module->picked_color[k] =
      module->picked_color_min[k] =
        module->picked_color_max[k] = 0.0f;
  }
  module->color_picker_box[0] = module->color_picker_box[1] = .25f;
  module->color_picker_box[2] = module->color_picker_box[3] = .75f;
  module->enabled = module->default_enabled = 1; // all modules enabled by default.
  g_strlcpy(module->op, so->op, 20);

  // only reference cached results of dlopen:
  module->module = so->module;

  module->version     = so->version;
  module->name        = so->name;
  module->groups      = so->groups;
  module->flags       = so->flags;
  module->operation_tags  = so->operation_tags;
  module->operation_tags_filter  = so->operation_tags_filter;
  module->output_bpp  = so->output_bpp;
  module->gui_update  = so->gui_update;
  module->gui_init    = so->gui_init;
  module->gui_cleanup = so->gui_cleanup;

  module->gui_post_expose = so->gui_post_expose;
  module->mouse_leave     = so->mouse_leave;
  module->mouse_moved     = so->mouse_moved;
  module->button_released = so->button_released;
  module->button_pressed  = so->button_pressed;
  module->configure       = so->configure;
  module->scrolled        = so->scrolled;

  module->init            = so->init;
  module->cleanup         = so->cleanup;
  module->commit_params   = so->commit_params;
  module->reload_defaults = so->reload_defaults;
  module->init_pipe       = so->init_pipe;
  module->cleanup_pipe    = so->cleanup_pipe;
  module->process         = so->process;
  module->process_cl      = so->process_cl;
  module->modify_roi_in   = so->modify_roi_in;
  module->modify_roi_out  = so->modify_roi_out;
  module->legacy_params   = so->legacy_params;

  // now init the instance:
  module->init(module);

  /* initialize blendop params and default values */
  module->blend_params=g_malloc(sizeof(dt_develop_blend_params_t));
  module->default_blendop_params=g_malloc(sizeof(dt_develop_blend_params_t));
  memset(module->blend_params, 0, sizeof(dt_develop_blend_params_t));
  dt_develop_blend_params_t default_blendop_params= {DEVELOP_BLEND_DISABLED,100.0,0};
  memset(module->default_blendop_params, 0, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &default_blendop_params, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &default_blendop_params, sizeof(dt_develop_blend_params_t));

  if(module->priority == 0)
  {
    fprintf(stderr, "[iop_load_module] `%s' needs to set priority!\n", so->op);
    return 1;      // this needs to be set
  }
  module->enabled = module->default_enabled; // apply (possibly new) default.
  return 0;
}

void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece)
{
  module->init_pipe(module, pipe, piece);
  piece->blendop_data = malloc(sizeof(dt_develop_blend_params_t));
  memset(piece->blendop_data, 0, sizeof(dt_develop_blend_params_t));
  dt_develop_blend_params_t default_blendop_params= {DEVELOP_BLEND_DISABLED,100.0,0};
  memset(module->default_blendop_params, 0, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &default_blendop_params, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &default_blendop_params, sizeof(dt_develop_blend_params_t));
  /// FIXME: Commmit params is already done in module
  dt_iop_commit_params(module, module->default_params, module->default_blendop_params, pipe, piece);
}

static void
dt_iop_gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset)
  {
    if(gtk_toggle_button_get_active(togglebutton)) module->enabled = 1;
    else module->enabled = 0;
    dt_dev_add_history_item(module->dev, module, FALSE);
    // close parent expander.
    gtk_expander_set_expanded(module->expander, module->enabled);
  }
  char tooltip[512];
  snprintf(tooltip, 512, module->enabled ? _("%s is switched on") : _("%s is switched off"), module->name());
  g_object_set(G_OBJECT(togglebutton), "tooltip-text", tooltip, (char *)NULL);
}

static void
update_topwidget(dt_iop_module_t *module)
{
  // update/insert/remove module->off:
  if(!module->hide_enable_button && !module->off)
  {
    // insert new
    // module->topwidget [evb] -> vbox -> hbox -> leftmost entry
    GtkWidget *vbox = gtk_bin_get_child(GTK_BIN(module->topwidget));
    GList *vboxlist = gtk_container_get_children(GTK_CONTAINER(vbox));
    GtkWidget *hbox = GTK_WIDGET(vboxlist->data);

    GtkDarktableToggleButton *button = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
    gtk_widget_set_size_request(GTK_WIDGET(button), 13, 13);
    char tooltip[512];
    snprintf(tooltip, 512, module->enabled ? _("%s is switched on") : _("%s is switched off"), module->name());
    g_object_set(G_OBJECT(button), "tooltip-text", tooltip, (char *)NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), module->enabled);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(button), FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(hbox), GTK_WIDGET(button), 0);
    g_signal_connect (G_OBJECT (button), "toggled",
                      G_CALLBACK (dt_iop_gui_off_callback), module);
    module->off = button;

    GtkWidget *w = gtk_expander_get_label_widget (module->expander);
    gtk_misc_set_padding(GTK_MISC(w), 0, 0);
  }
  else if(module->hide_enable_button && module->off)
  {
    // remove widget
    gtk_widget_destroy(GTK_WIDGET(module->off));
    module->off = NULL;

    GtkWidget *w = gtk_expander_get_label_widget (module->expander);
    gtk_misc_set_padding(GTK_MISC(w), 13, 0);
  }
  else if(module->off)
  {
    // just toggle with current module->enabled value.
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
  }
}

void dt_iop_reload_defaults(dt_iop_module_t *module)
{
  if(module->reload_defaults)
  {
    module->reload_defaults(module);
    // factory defaults can only be overwritten if reload_defaults actually exists:
    memcpy(module->factory_params, module->params, module->params_size);
    module->factory_enabled = module->default_enabled;
  }
  dt_iop_load_default_params(module);
  if(module->topwidget) update_topwidget(module);
}

static void
init_presets(dt_iop_module_so_t *module)
{
  if(module->init_presets) module->init_presets(module);
}

void dt_iop_load_modules_so()
{
  GList *res = NULL;
  dt_iop_module_so_t *module;
  darktable.iop = NULL;
  char plugindir[1024], op[20], accelpath[1024];
  const gchar *d_name;
  dt_get_plugindir(plugindir, 1024);
  g_strlcat(plugindir, "/plugins", 1024);
  GDir *dir = g_dir_open(plugindir, 0, NULL);
  if(!dir) return;
  while((d_name = g_dir_read_name(dir)))
  {
    // get lib*.so
    if(strncmp(d_name, "lib", 3)) continue;
    if(strncmp(d_name + strlen(d_name) - 3, ".so", 3)) continue;
    strncpy(op, d_name+3, strlen(d_name)-6);
    op[strlen(d_name)-6] = '\0';
    module = (dt_iop_module_so_t *)malloc(sizeof(dt_iop_module_so_t));
    memset(module,0,sizeof(dt_iop_module_so_t));
    gchar *libname = g_module_build_path(plugindir, (const gchar *)op);
    if(dt_iop_load_module_so(module, libname, op))
    {
      free(module);
      continue;
    }
    g_free(libname);
    res = g_list_append(res, module);
    init_presets(module);
    // Calling the accelerator initialization callback, if present
    if(module->init_key_accels)
      (module->init_key_accels)();

    if(!(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      // Adding the optional show accelerator to the table (blank)
      snprintf(accelpath, 256, "<Darktable>/darkroom/plugins/%s/show plugin",
               (module->op));
      gtk_accel_map_add_entry(accelpath, 0, 0);
      dt_accel_group_connect_by_path(darktable.control->accels_darkroom, accelpath,
                                     NULL);
      snprintf(accelpath, 1024, "<Darktable>/darkroom/plugins/%s/reset plugin parameters",module->op);
      dtgtk_button_init_accel(darktable.control->accels_darkroom,accelpath);
      snprintf(accelpath, 1024, "<Darktable>/darkroom/plugins/%s/show preset menu",module->op);
      dtgtk_button_init_accel(darktable.control->accels_darkroom,accelpath);
    }
  }
  g_dir_close(dir);
  darktable.iop = res;
}

GList *dt_iop_load_modules(dt_develop_t *dev)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dt_iop_module_so_t *module_so;
  dev->iop_instance = 0;
  GList *iop = darktable.iop;
  while(iop)
  {
    module_so = (dt_iop_module_so_t *)iop->data;
    module    = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
    memset(module,0,sizeof(dt_iop_module_t));
    if(dt_iop_load_module_by_so(module, module_so, dev))
    {
      free(module);
      continue;
    }
    res = g_list_insert_sorted(res, module, sort_plugins);
    module->data = module_so->data;
    module->factory_params = malloc(module->params_size);
    // copy factory params first time. reload_defaults will only overwrite
    // if module->reload_defaults exists (else the here copied values
    // stay constant for all images).
    memcpy(module->factory_params, module->params, module->params_size);
    module->factory_enabled = module->default_enabled;
    dt_iop_reload_defaults(module);
    iop = g_list_next(iop);
  }

  GList *it = res;
  while(it)
  {
    module = (dt_iop_module_t *)it->data;
    module->instance = dev->iop_instance++;
    it = g_list_next(it);
  }
  return res;
}

void dt_iop_cleanup_module(dt_iop_module_t *module)
{
  free(module->factory_params); module->factory_params = NULL ;  
  module->cleanup(module);

  free(module->default_params); module->default_params = NULL ; 
  if (module->blend_params != NULL) 
  {
    free(module->blend_params) ; 
    module->blend_params = NULL ; 
  }
  if (module->default_blendop_params != NULL) 
  {
    free(module->default_blendop_params) ; 
    module->default_blendop_params = NULL ; 
  }
  dt_pthread_mutex_destroy(&module->params_mutex);
}

void dt_iop_unload_modules_so()
{  
  while(darktable.iop)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)darktable.iop->data;
    if(module->cleanup_global) module->cleanup_global(module);
    if(module->module) g_module_close(module->module);
    free(darktable.iop->data);
    darktable.iop = g_list_delete_link(darktable.iop, darktable.iop);
  }
}

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params, dt_develop_blend_params_t * blendop_params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  uint64_t hash = 5381;
  piece->hash = 0;
  if(piece->enabled)
  {
    /* construct module params data for hash calc */
    int length = module->params_size;
    if (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) length += sizeof(dt_develop_blend_params_t);

    char *str = malloc(length);
    memcpy(str, module->params, module->params_size);

    /* if module supports blend op add blend params into account */
    if (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      memcpy(str+module->params_size, blendop_params, sizeof(dt_develop_blend_params_t));
      memcpy(piece->blendop_data, blendop_params, sizeof(dt_develop_blend_params_t));
      // this should be redundant! (but is not)
      memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    }

    // assume process_cl is ready, commit_params can overwrite this.
    if(module->process_cl) piece->process_cl_ready = 1;
    module->commit_params(module, params, pipe, piece);
    for(int i=0; i<length; i++) hash = ((hash << 5) + hash) ^ str[i];
    piece->hash = hash;

    free(str);
  }
  // printf("commit params hash += module %s: %lu, enabled = %d\n", piece->module->op, piece->hash, piece->enabled);
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  module->gui_update(module);
  if (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    _iop_gui_blend_data_t *bd = (_iop_gui_blend_data_t*)module->blend_data;

    gtk_combo_box_set_active(bd->blend_modes_combo,module->blend_params->mode - 1);
    gtk_toggle_button_set_active(bd->enable, (module->blend_params->mode != DEVELOP_BLEND_DISABLED)?TRUE:FALSE);
    dtgtk_slider_set_value(DTGTK_SLIDER(bd->opacity_slider), module->blend_params->opacity);
  }
  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
  darktable.gui->reset = reset;
}

static int _iop_module_demosaic=0,_iop_module_colorout=0,_iop_module_colorin=0;
dt_iop_colorspace_type_t dt_iop_module_colorspace(const dt_iop_module_t *module)
{
  /* check if we do know what priority the color* plugins have */
  if(_iop_module_colorout==0 && _iop_module_colorin==0)
  {
    /* lets find out which priority colorin and colorout have */
    GList *iop = module->dev->iop;
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
      if(m != module)
      {
        if(!strcmp(m->op,"colorin"))
          _iop_module_colorin = m->priority;
        else if(!strcmp(m->op,"colorout"))
          _iop_module_colorout = m->priority;
        else if(!strcmp(m->op,"demosaic"))
          _iop_module_demosaic = m->priority;
      }

      /* do we have both priorities, lets break out... */
      if(_iop_module_colorout && _iop_module_colorin && _iop_module_demosaic)
        break;
      iop = g_list_next(iop);
    }
  }

  /* let check which colorspace module is within */
  if (module->priority > _iop_module_colorout)
    return iop_cs_rgb;
  else if (module->priority > _iop_module_colorin)
    return iop_cs_Lab;
  else if (module->priority < _iop_module_demosaic)
    return iop_cs_RAW;

  /* fallback to rgb */
  return iop_cs_rgb;
}

static gboolean
expander_button_callback(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *module)
{
  if(event->button == 1 && (event->state & GDK_SHIFT_MASK)) // TODO: this can also be done when some auto-collapse option is set ...
  {
    int current_group = dt_gui_iop_modulegroups_get();
    GList *iop = g_list_first(module->dev->iop);
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;

      /* if module is the current, always expand it */
      if(m==module)
        gtk_expander_set_expanded(m->expander, TRUE);
      else if((current_group == 0 || (current_group & m->groups()) || (current_group == IOP_SPECIAL_GROUP_ACTIVE_PIPE && m->enabled) /* || (current_group == IOP_SPECIAL_GROUP_USER_DEFINED && module->showhide && dtgtk_tristatebutton_get_state (DTGTK_TRISTATEBUTTON(module->showhide))==2) */ )) //FIXME
        gtk_expander_set_expanded(m->expander, FALSE);

      iop = g_list_next(iop);
    }

    /* prevent expander to handle the button press because we alread did :) */
    return TRUE;
  }
  return FALSE;
}

static void
dt_iop_gui_expander_callback(GObject *object, GParamSpec *param_spec, gpointer user_data)
{
  GtkExpander *expander = GTK_EXPANDER (object);
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  GtkWidget *content = gtk_widget_get_parent(module->widget);
  if (gtk_expander_get_expanded (expander))
  {
    gtk_widget_show(content);
    // Hack: in views/darkroom.c we use gtk_widget_show_all() which also shows the to be hidden blend box when entering darkroom mode.
    _iop_gui_blend_data_t *bd = (_iop_gui_blend_data_t*)module->blend_data;
    if(bd != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->enable)) == FALSE)
      gtk_widget_hide(GTK_WIDGET(bd->box));

    // register to receive draw events
    dt_iop_request_focus(module);
    GtkContainer *box = GTK_CONTAINER(darktable.gui->widgets.plugins_vbox);
    gtk_container_set_focus_child(box, module->topwidget);
    // redraw gui (in case post expose is set)
    gtk_widget_queue_resize(darktable.gui->widgets.plugins_vbox);
    dt_control_gui_queue_draw();
  }
  else
  {
    if(module->dev->gui_module == module)
    {
      dt_iop_request_focus(NULL);
      dt_control_gui_queue_draw();
    }
    gtk_widget_hide(content);
  }
}

static void
dt_iop_gui_reset_callback(GtkButton *button, dt_iop_module_t *module)
{
  // module->enabled = module->default_enabled; // will not propagate correctly anyways ;)
  memcpy(module->params, module->default_params, module->params_size);
  memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
  dt_iop_gui_update(module);
  if(strcmp(module->op, "rawimport")) dt_dev_add_history_item(module->dev, module, TRUE);
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
  if(event->button == 1)
  {
    dt_iop_request_focus(module);
    return TRUE;
  }
  else if(event->button == 3)
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
    if (darktable.develop->gui_module->operation_tags_filter())
      dt_dev_invalidate_from_gui(darktable.develop);
  }
  darktable.develop->gui_module = module;
  if(module)
  {
    gtk_widget_set_state(module->topwidget, GTK_STATE_SELECTED);
    gtk_widget_set_state(module->widget,    GTK_STATE_NORMAL);
    GtkWidget *off = GTK_WIDGET(darktable.develop->gui_module->off);
    if(off) gtk_widget_set_state(off, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(off)) ? GTK_STATE_ACTIVE : GTK_STATE_NORMAL);
    if (module->operation_tags_filter())
      dt_dev_invalidate_from_gui(darktable.develop);
  }
  dt_control_change_cursor(GDK_LEFT_PTR);
}



static void _iop_gui_enabled_blend_cb(GtkToggleButton *b,_iop_gui_blend_data_t *data)
{
  if (gtk_toggle_button_get_active(b))
  {
    data->module->blend_params->mode = 1+gtk_combo_box_get_active(data->blend_modes_combo);
    // FIXME: quite hacky, but it works (TM)
    if(data->module->blend_params->mode == DEVELOP_BLEND_DISABLED)
    {
      data->module->blend_params->mode = DEVELOP_BLEND_NORMAL;
      gtk_combo_box_set_active(data->blend_modes_combo, 0);
    }
    gtk_widget_show(GTK_WIDGET(data->box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(data->box));
    data->module->blend_params->mode = DEVELOP_BLEND_DISABLED;
  }
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void
_blendop_mode_callback (GtkComboBox *combo, _iop_gui_blend_data_t *data)
{
  data->module->blend_params->mode = 1+gtk_combo_box_get_active(data->blend_modes_combo);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void
_blendop_opacity_callback (GtkDarktableSlider *slider, _iop_gui_blend_data_t *data)
{
  data->module->blend_params->opacity = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}


GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module)
{
  char name[1024];
  GtkHBox *hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  snprintf(name, 1024, "%s%s",module->name(),(module->flags() & IOP_FLAGS_DEPRECATED)?_(" (deprecated)"):"");
  module->expander = GTK_EXPANDER(gtk_expander_new((const gchar *)name));
  // gamma is always needed for display (down to uint8_t)
  // colorin/colorout are essential for La/Lb/L conversion.
  if(!module->hide_enable_button)
  {
    GtkDarktableToggleButton *button = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
    gtk_widget_set_size_request(GTK_WIDGET(button), 13, 13);
    char tooltip[512];
    snprintf(tooltip, 512, module->enabled ? _("%s is switched on") : _("%s is switched off"), module->name());
    g_object_set(G_OBJECT(button), "tooltip-text", tooltip, (char *)NULL);
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
  snprintf(name, 1024, "<Darktable>/darkroom/plugins/%s/reset plugin parameters",module->op);
  dtgtk_button_set_accel(resetbutton,darktable.control->accels_darkroom,name);
  GtkDarktableButton *presetsbutton = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  snprintf(name, 1024, "<Darktable>/darkroom/plugins/%s/show preset menu",module->op);
  dtgtk_button_set_accel(presetsbutton,darktable.control->accels_darkroom,name);
  gtk_widget_set_size_request(GTK_WIDGET(presetsbutton),13,13);
  gtk_widget_set_size_request(GTK_WIDGET(resetbutton),13,13);
  g_object_set(G_OBJECT(resetbutton), "tooltip-text", _("reset parameters"), (char *)NULL);
  g_object_set(G_OBJECT(presetsbutton), "tooltip-text", _("presets"), (char *)NULL);
  gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(resetbutton), FALSE, FALSE, 0);
  gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(presetsbutton), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  GtkWidget *al = gtk_alignment_new(1.0, 1.0, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(al), 10, 10, 10, 5);

  /* add module widget to container */
  GtkWidget * iopw = gtk_vbox_new(FALSE,4);
  gtk_box_pack_start(GTK_BOX(iopw), module->widget, TRUE, TRUE, 0);


  /* create and add blend mode if module supports it */
  if (module->flags()&IOP_FLAGS_SUPPORTS_BLENDING)
  {
    module->blend_data = g_malloc(sizeof(_iop_gui_blend_data_t));
    _iop_gui_blend_data_t *bd = (_iop_gui_blend_data_t*)module->blend_data;
    bd->module = module;

    bd->box = GTK_VBOX(gtk_vbox_new(FALSE,DT_GUI_IOP_MODULE_CONTROL_SPACING));
    GtkWidget *btb = gtk_hbox_new(FALSE,5);
    GtkWidget *bhb = gtk_hbox_new(FALSE,0);
    GtkWidget *dummybox = gtk_hbox_new(FALSE,0); // hack to indent the drop down box

    bd->enable = GTK_TOGGLE_BUTTON(gtk_check_button_new_with_label(_("blend")));
    GtkWidget *label = gtk_label_new(_("mode"));
    bd->blend_modes_combo = GTK_COMBO_BOX(gtk_combo_box_new_text());
    bd->opacity_slider = GTK_WIDGET(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1, 100.0, 0));
    dtgtk_slider_set_label(DTGTK_SLIDER(bd->opacity_slider),_("opacity"));
    dtgtk_slider_set_unit(DTGTK_SLIDER(bd->opacity_slider),"%");
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("normal"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("lighten"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("darken"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("multiply"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("average"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("addition"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("subtract"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("difference"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("screen"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("overlay"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("softlight"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("hardlight"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("vividlight"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("linearlight"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(bd->blend_modes_combo), _("pinlight"));

    gtk_combo_box_set_active(bd->blend_modes_combo,0);
    gtk_object_set(GTK_OBJECT(bd->enable), "tooltip-text", _("enable blending"), (char *)NULL);
    gtk_object_set(GTK_OBJECT(bd->opacity_slider), "tooltip-text", _("set the opacity of the blending"), (char *)NULL);
    gtk_object_set(GTK_OBJECT(bd->blend_modes_combo), "tooltip-text", _("choose blending mode"), (char *)NULL);

    g_signal_connect (G_OBJECT (bd->enable), "toggled",
                      G_CALLBACK (_iop_gui_enabled_blend_cb), bd);
    g_signal_connect (G_OBJECT (bd->opacity_slider), "value-changed",
                      G_CALLBACK (_blendop_opacity_callback), bd);
    g_signal_connect (G_OBJECT (bd->blend_modes_combo), "changed",
                      G_CALLBACK (_blendop_mode_callback), bd);

    gtk_box_pack_start(GTK_BOX(btb), GTK_WIDGET(bd->enable), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btb), GTK_WIDGET(gtk_hseparator_new()), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(bhb), GTK_WIDGET(label), FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(bhb), GTK_WIDGET(bd->blend_modes_combo), TRUE, TRUE, 5);

    gtk_box_pack_start(GTK_BOX(dummybox), bd->opacity_slider, TRUE, TRUE, 5);

    gtk_box_pack_start(GTK_BOX(bd->box), bhb,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(bd->box), dummybox,TRUE,TRUE,0);

    gtk_box_pack_end(GTK_BOX(iopw), GTK_WIDGET(bd->box),TRUE,TRUE,0);
    gtk_box_pack_end(GTK_BOX(iopw), btb,TRUE,TRUE,0);
    
  }

  /* add the iopw widget to aligment widget */
  gtk_container_add(GTK_CONTAINER(al), iopw);
  gtk_box_pack_start(GTK_BOX(vbox), al, TRUE, TRUE, 0);


  g_signal_connect (G_OBJECT (resetbutton), "clicked",
                    G_CALLBACK (dt_iop_gui_reset_callback), module);
  g_signal_connect (G_OBJECT (presetsbutton), "clicked",
                    G_CALLBACK (popup_callback), module);
  g_signal_connect (G_OBJECT (module->expander), "notify::expanded",
                    G_CALLBACK (dt_iop_gui_expander_callback), module);
  gtk_expander_set_spacing(module->expander, 10);
  gtk_widget_hide_all(al);
  gtk_expander_set_expanded(module->expander, FALSE);
  GtkWidget *evb = gtk_event_box_new();
  gtk_container_set_border_width(GTK_CONTAINER(evb), 0);
  gtk_container_add(GTK_CONTAINER(evb), GTK_WIDGET(vbox));

  gtk_widget_set_events(evb, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(evb), "button-press-event", G_CALLBACK(popup_button_callback), module);
  g_signal_connect(G_OBJECT(module->expander), "button-press-event", G_CALLBACK(expander_button_callback), module);
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
  for(int s=0; s<oh2; s++)
  {
    int idx = ox2 + obw*(oy2+s);
    for(int t=0; t<ow2; t++)
    {
      for(int k=0; k<3; k++) o[4*idx + k] = //i[3*(ibw* (int)y +             (int)x             ) + k)];
          CLAMP(((int32_t)i[(4*(ibw*(int32_t) y +            (int32_t) (x + .5f*scalex)) + k)] +
                 (int32_t)i[(4*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x + .5f*scalex)) + k)] +
                 (int32_t)i[(4*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x             )) + k)] +
                 (int32_t)i[(4*(ibw*(int32_t) y +            (int32_t) (x             )) + k)])/4, 0, 255);
      x += scalex;
      idx++;
    }
    y += scaley;
    x = ix2;
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

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out)
#endif
  for(int y=0; y<roi_out->height; y++)
  {
    float *outc = out + 4*(out_stride*y);
    for(int x=0; x<roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();
      // _mm_prefetch
      // upper left corner:
      float fx = (x + roi_out->x)/roi_out->scale, fy = (y + roi_out->y)/roi_out->scale;
      int px = (int)fx, py = (int)fy;
      const float dx = fx - px, dy = fy - py;
      const __m128 d0 = _mm_set1_ps((1.0f-dx)*(1.0f-dy));
      const __m128 d1 = _mm_set1_ps((dx)*(1.0f-dy));
      const __m128 d2 = _mm_set1_ps((1.0f-dx)*(dy));
      const __m128 d3 = _mm_set1_ps(dx*dy);

      // const float *inc = in + 4*(py*roi_in->width + px);

      float num=0.0f;
      // for(int j=-samples;j<=samples;j++) for(int i=-samples;i<=samples;i++)
      for(int j=MAX(0, py-samples); j<=MIN(roi_in->height-2, py+samples); j++)
        for(int i=MAX(0, px-samples); i<=MIN(roi_in->width -2, px+samples); i++)
        {
          __m128 p0 = _mm_mul_ps(d0, _mm_load_ps(in + 4*(i + in_stride*j)));
          __m128 p1 = _mm_mul_ps(d1, _mm_load_ps(in + 4*(i + 1 + in_stride*j)));
          __m128 p2 = _mm_mul_ps(d2, _mm_load_ps(in + 4*(i + in_stride*(j+1))));
          __m128 p3 = _mm_mul_ps(d3, _mm_load_ps(in + 4*(i + 1 + in_stride*(j+1))));

          col = _mm_add_ps(col, _mm_add_ps(_mm_add_ps(p0, p1), _mm_add_ps(p2, p3)));
          num++;
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

#if 0
static float
weight (const float c1, const float c2)
{
  // return dt_fast_expf(-(c1-c2)*(c1-c2)*(1.0f/(0.001f*65535.0f*65535.0f)));
  return ((c1 > 65500.0f) ^ (c2 > 65500.0f)) ? 0.0f : 1.0f;
}
#else
/*static float
weight (const float c0, const float c1, const float c2, const float c3, const float c4)
{
  const float c = 65534.0f;
  const float cc = fmaxf(fmaxf(c1, c2), fmaxf(c3, c4));
  return ((c0 > c) ^ (cc > c)) ? 0.001f : 1.0f;
}*/
#endif
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

  memset(filter, 0, sizeof(float)*(2*samples + 1));

  if(samples)
  {
    for(int i=-samples; i<=samples; i++) sum += (filter[i+samples] = expf(-i*i/(float)(.5f*samples*samples)));
    for(int k=0; k<2*samples+1; k++) filter[k] /= sum;
  }
  else filter[0] = 1.0f;

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx+1, filters) != 1) trggbx ++;
  if(FC(trggby, trggbx,   filters) != 0)
  {
    trggbx = (trggbx + 1)&1;
    trggby ++;
  }
  const int rggbx = trggbx, rggby = trggby;
  const int maxj = ((roi_in->height-3)&~1u)+rggby;
  const int maxi = ((roi_in->width -3)&~1u)+rggbx;

  const __m128 ff  = _mm_set1_ps(1.0f/65535.0f);
  // const __m128 one = _mm_set1_ps(1.0f);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out, filter) schedule(static)
#endif
  for(int y=0; y<roi_out->height; y++)
  {
    int py = (y + roi_out->y)/roi_out->scale;
    py = MAX(0, py & ~1) + rggby;
    py = MIN((((roi_in->height-3) & ~1u) + rggby), py);
    const int maxjj = MIN(maxj,  py + 2*samples);
    const int minjj = MAX(rggby, py - 2*samples);

    float *outc = out + 4*out_stride*y;
    for(int x=0; x<roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();
      // _mm_prefetch
      int px = (x + roi_out->x)/roi_out->scale;

      // round down to next even number and jump to rggb block:
      px = MAX(0, px & ~1) + rggbx;
      px = MIN((((roi_in->width -3) & ~1u) + rggbx), px);

      const int idx = px   + in_stride*py;

      // const float pc1  = in[px   + in_stride*py];
      // const float pc23 = .5f*(in[px+1 + in_stride*py] + in[px   + in_stride*(py + 1)]);
      // const float pc4  = in[px+1 + in_stride*(py + 1)];
      const uint16_t pc = MAX(MAX(in[idx], in[idx+1]), MAX(in[idx + in_stride], in[idx+1 + in_stride]));

      __m128 num = _mm_setzero_ps();
      const int maxii = MIN(maxi,  px + 2*samples);
      const int minii = MAX(rggbx, px - 2*samples);
      for(int j=minjj; j<=maxjj; j+=2)
        for(int i=minii; i<=maxii; i+=2)
        {
          // get four mosaic pattern uint16:
          const uint16_t p1 = in[i   + in_stride*j];
          const uint16_t p2 = in[i+1 + in_stride*j];
          const uint16_t p3 = in[i   + in_stride*(j + 1)];
          const uint16_t p4 = in[i+1 + in_stride*(j + 1)];

          // const float wr = weight(pc1, p1);
          // const float wg = weight(pc23, .5f*(p2+p3));
          const float w = ((pc == 65535) ^ (MAX(MAX(p1,p2),MAX(p3,p4)) == 65535)) ? 0.001f : 1.0f;
          // const float wb = weight(pc4, p4);

          // const float f = filter[(i-px)/2+samples]*filter[(j-py)/2+samples];
          // col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4*wb, .5f*(p2+p3)*wg, p1*wr), _mm_set1_ps(f/65535.0f)));
          // num = _mm_add_ps(num, _mm_mul_ps(_mm_set_ps(1.0f, wb, wg, wr), _mm_set1_ps(f)));
          // col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4*wb, .5f*(p2+p3)*wg, p1*wr), ff));
          // num = _mm_add_ps(num, _mm_mul_ps(_mm_set_ps(1.0f, wb, wg, wr), one));
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set_ps(0.0f, p4*w, .5f*(p2+p3)*w, p1*w), ff));
          num = _mm_add_ps(num, _mm_set1_ps(w));
        }
      col = _mm_div_ps(col, num);
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

  memset(filter, 0, sizeof(float)*(2*samples + 1));

  if(samples)
  {
    for(int i=-samples; i<=samples; i++) sum += (filter[i+samples] = expf(-i*i/(float)(.5f*samples*samples)));
    for(int k=0; k<2*samples+1; k++) filter[k] /= sum;
  }
  else filter[0] = 1.0f;

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx+1, filters) != 1) trggbx ++;
  if(FC(trggby, trggbx,   filters) != 0)
  {
    trggbx = (trggbx + 1)&1;
    trggby ++;
  }
  const int rggbx = trggbx, rggby = trggby;


#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out, filter) schedule(static)
#endif
  for(int y=0; y<roi_out->height; y++)
  {
    float *outc = out + 4*(out_stride*y);
    for(int x=0; x<roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();
      // _mm_prefetch
      // upper left corner:
      float fx = (x + roi_out->x)/roi_out->scale, fy = (y + roi_out->y)/roi_out->scale;
      int px = (int)fx, py = (int)fy;

      // round down to next even number and jump to rggb block:
      px = MAX(0, px & ~1) + rggbx;
      py = MAX(0, py & ~1) + rggby;

      px = MIN((((roi_in->width -5) & ~1u) + rggbx),  px);
      py = MIN((((roi_in->height-5) & ~1u) + rggby), py);

      const float dx = .5f*(fx - px), dy = .5f*(fy - py);
      const __m128 d0 = _mm_set1_ps((1.0f-dx)*(1.0f-dy));
      const __m128 d1 = _mm_set1_ps((dx)*(1.0f-dy));
      const __m128 d2 = _mm_set1_ps((1.0f-dx)*(dy));
      const __m128 d3 = _mm_set1_ps(dx*dy);

      float num = 0.0f;
      for(int j=MAX(rggby, py-2*samples); j<=MIN(((roi_in->height-5)&~1u)+rggby, py+2*samples); j+=2)
        for(int i=MAX(rggbx, px-2*samples); i<=MIN(((roi_in->width -5)&~1u)+rggbx, px+2*samples); i+=2)
        {
          // get four mosaic pattern uint16:
          float p1, p2, p4;
          p1 = in[i   + in_stride*j];
          p2 = in[i+1 + in_stride*j] + in[i   + in_stride*(j + 1)];
          p4 = in[i+1 + in_stride*(j + 1)];
          const __m128 px0 = _mm_set_ps(0.0f, p4, .5f*p2, p1);
          p1 = in[i+2   + in_stride*j];
          p2 = in[i+2+1 + in_stride*j] + in[i+2   + in_stride*(j + 1)];
          p4 = in[i+2+1 + in_stride*(j + 1)];
          const __m128 px1 = _mm_set_ps(0.0f, p4, .5f*p2, p1);
          p1 = in[i   + in_stride*(j+2)];
          p2 = in[i+1 + in_stride*(j+2)] + in[i   + in_stride*(j+2 + 1)];
          p4 = in[i+1 + in_stride*(j+2 + 1)];
          const __m128 px2 = _mm_set_ps(0.0f, p4, .5f*p2, p1);
          p1 = in[i+2   + in_stride*(j+2)];
          p2 = in[i+2+1 + in_stride*(j+2)] + in[i+2   + in_stride*(j+2 + 1)];
          p4 = in[i+2+1 + in_stride*(j+2 + 1)];
          const __m128 px3 = _mm_set_ps(0.0f, p4, .5f*p2, p1);
          const __m128 lerp = _mm_add_ps(_mm_add_ps(_mm_mul_ps(d0, px0), _mm_mul_ps(d1, px1)),
                                         _mm_add_ps(_mm_mul_ps(d2, px2), _mm_mul_ps(d3, px3)));

          const float f = filter[(i-px)/2+samples]*filter[(j-py)/2+samples];
          col = _mm_add_ps(col, _mm_mul_ps(lerp, _mm_set1_ps(f)));
          num += f;
        }
      col = _mm_mul_ps(col, _mm_set1_ps(1.0f/num));
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

static inline void
mat4inv(const float X[][4], float R[][4])
{
  const float det = 
          X[0][3] * X[1][2] * X[2][1] * X[3][0]
        - X[0][2] * X[1][3] * X[2][1] * X[3][0] 
        - X[0][3] * X[1][1] * X[2][2] * X[3][0] 
        + X[0][1] * X[1][3] * X[2][2] * X[3][0] 
        + X[0][2] * X[1][1] * X[2][3] * X[3][0] 
        - X[0][1] * X[1][2] * X[2][3] * X[3][0] 
        - X[0][3] * X[1][2] * X[2][0] * X[3][1] 
        + X[0][2] * X[1][3] * X[2][0] * X[3][1] 
        + X[0][3] * X[1][0] * X[2][2] * X[3][1] 
        - X[0][0] * X[1][3] * X[2][2] * X[3][1] 
        - X[0][2] * X[1][0] * X[2][3] * X[3][1] 
        + X[0][0] * X[1][2] * X[2][3] * X[3][1] 
        + X[0][3] * X[1][1] * X[2][0] * X[3][2] 
        - X[0][1] * X[1][3] * X[2][0] * X[3][2] 
        - X[0][3] * X[1][0] * X[2][1] * X[3][2] 
        + X[0][0] * X[1][3] * X[2][1] * X[3][2] 
        + X[0][1] * X[1][0] * X[2][3] * X[3][2] 
        - X[0][0] * X[1][1] * X[2][3] * X[3][2] 
        - X[0][2] * X[1][1] * X[2][0] * X[3][3] 
        + X[0][1] * X[1][2] * X[2][0] * X[3][3] 
        + X[0][2] * X[1][0] * X[2][1] * X[3][3] 
        - X[0][0] * X[1][2] * X[2][1] * X[3][3] 
        - X[0][1] * X[1][0] * X[2][2] * X[3][3] 
        + X[0][0] * X[1][1] * X[2][2] * X[3][3];
  R[0][0] = ( X[1][2]*X[2][3]*X[3][1] - X[1][3]*X[2][2]*X[3][1] + X[1][3]*X[2][1]*X[3][2] - X[1][1]*X[2][3]*X[3][2] - X[1][2]*X[2][1]*X[3][3] + X[1][1]*X[2][2]*X[3][3] ) / det;
  R[1][0] = ( X[1][3]*X[2][2]*X[3][0] - X[1][2]*X[2][3]*X[3][0] - X[1][3]*X[2][0]*X[3][2] + X[1][0]*X[2][3]*X[3][2] + X[1][2]*X[2][0]*X[3][3] - X[1][0]*X[2][2]*X[3][3] ) / det;
  R[2][0] = ( X[1][1]*X[2][3]*X[3][0] - X[1][3]*X[2][1]*X[3][0] + X[1][3]*X[2][0]*X[3][1] - X[1][0]*X[2][3]*X[3][1] - X[1][1]*X[2][0]*X[3][3] + X[1][0]*X[2][1]*X[3][3] ) / det;
  R[3][0] = ( X[1][2]*X[2][1]*X[3][0] - X[1][1]*X[2][2]*X[3][0] - X[1][2]*X[2][0]*X[3][1] + X[1][0]*X[2][2]*X[3][1] + X[1][1]*X[2][0]*X[3][2] - X[1][0]*X[2][1]*X[3][2] ) / det;

  R[0][1] = ( X[0][3]*X[2][2]*X[3][1] - X[0][2]*X[2][3]*X[3][1] - X[0][3]*X[2][1]*X[3][2] + X[0][1]*X[2][3]*X[3][2] + X[0][2]*X[2][1]*X[3][3] - X[0][1]*X[2][2]*X[3][3] ) / det;
  R[1][1] = ( X[0][2]*X[2][3]*X[3][0] - X[0][3]*X[2][2]*X[3][0] + X[0][3]*X[2][0]*X[3][2] - X[0][0]*X[2][3]*X[3][2] - X[0][2]*X[2][0]*X[3][3] + X[0][0]*X[2][2]*X[3][3] ) / det;
  R[2][1] = ( X[0][3]*X[2][1]*X[3][0] - X[0][1]*X[2][3]*X[3][0] - X[0][3]*X[2][0]*X[3][1] + X[0][0]*X[2][3]*X[3][1] + X[0][1]*X[2][0]*X[3][3] - X[0][0]*X[2][1]*X[3][3] ) / det;
  R[3][1] = ( X[0][1]*X[2][2]*X[3][0] - X[0][2]*X[2][1]*X[3][0] + X[0][2]*X[2][0]*X[3][1] - X[0][0]*X[2][2]*X[3][1] - X[0][1]*X[2][0]*X[3][2] + X[0][0]*X[2][1]*X[3][2] ) / det;

  R[0][2] = ( X[0][2]*X[1][3]*X[3][1] - X[0][3]*X[1][2]*X[3][1] + X[0][3]*X[1][1]*X[3][2] - X[0][1]*X[1][3]*X[3][2] - X[0][2]*X[1][1]*X[3][3] + X[0][1]*X[1][2]*X[3][3] ) / det;
  R[1][2] = ( X[0][3]*X[1][2]*X[3][0] - X[0][2]*X[1][3]*X[3][0] - X[0][3]*X[1][0]*X[3][2] + X[0][0]*X[1][3]*X[3][2] + X[0][2]*X[1][0]*X[3][3] - X[0][0]*X[1][2]*X[3][3] ) / det;
  R[2][2] = ( X[0][1]*X[1][3]*X[3][0] - X[0][3]*X[1][1]*X[3][0] + X[0][3]*X[1][0]*X[3][1] - X[0][0]*X[1][3]*X[3][1] - X[0][1]*X[1][0]*X[3][3] + X[0][0]*X[1][1]*X[3][3] ) / det;
  R[3][2] = ( X[0][2]*X[1][1]*X[3][0] - X[0][1]*X[1][2]*X[3][0] - X[0][2]*X[1][0]*X[3][1] + X[0][0]*X[1][2]*X[3][1] + X[0][1]*X[1][0]*X[3][2] - X[0][0]*X[1][1]*X[3][2] ) / det;

  R[0][3] = ( X[0][3]*X[1][2]*X[2][1] - X[0][2]*X[1][3]*X[2][1] - X[0][3]*X[1][1]*X[2][2] + X[0][1]*X[1][3]*X[2][2] + X[0][2]*X[1][1]*X[2][3] - X[0][1]*X[1][2]*X[2][3] ) / det;
  R[1][3] = ( X[0][2]*X[1][3]*X[2][0] - X[0][3]*X[1][2]*X[2][0] + X[0][3]*X[1][0]*X[2][2] - X[0][0]*X[1][3]*X[2][2] - X[0][2]*X[1][0]*X[2][3] + X[0][0]*X[1][2]*X[2][3] ) / det;
  R[2][3] = ( X[0][3]*X[1][1]*X[2][0] - X[0][1]*X[1][3]*X[2][0] - X[0][3]*X[1][0]*X[2][1] + X[0][0]*X[1][3]*X[2][1] + X[0][1]*X[1][0]*X[2][3] - X[0][0]*X[1][1]*X[2][3] ) / det;
  R[3][3] = ( X[0][1]*X[1][2]*X[2][0] - X[0][2]*X[1][1]*X[2][0] + X[0][2]*X[1][0]*X[2][1] - X[0][0]*X[1][2]*X[2][1] - X[0][1]*X[1][0]*X[2][2] + X[0][0]*X[1][1]*X[2][2] ) / det;
}

static void
mat4mulv (float *dst, float mat[][4], const float *const v)
{
  for(int k=0; k<4; k++)
  {
    float x=0.0f;
    for(int i=0; i<4; i++) x += mat[k][i] * v[i];
    dst[k] = x;
  }
}

void dt_iop_estimate_cubic(const float *const x, const float *const y, float *a)
{
  // we want to fit a spline
  // [y]   [x^3 x^2 x^1 1] [a^3]
  // |y| = |x^3 x^2 x^1 1| |a^2|
  // |y|   |x^3 x^2 x^1 1| |a^1|
  // [y]   [x^3 x^2 x^1 1] [ 1 ]
  // and do that by inverting the matrix X:

  const float X[4][4] = {{x[0]*x[0]*x[0], x[0]*x[0], x[0], 1.0f},
                         {x[1]*x[1]*x[1], x[1]*x[1], x[1], 1.0f},
                         {x[2]*x[2]*x[2], x[2]*x[2], x[2], 1.0f},
                         {x[3]*x[3]*x[3], x[3]*x[3], x[3], 1.0f}};
  float X_inv[4][4];
  mat4inv(X, X_inv);
  mat4mulv(a, X_inv, y);
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
