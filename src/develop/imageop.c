/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2012 tobias ellinghaus.

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

#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/imageio_rawspeed.h"
#include "common/interpolation.h"
#include "common/module.h"
#include "common/opencl.h"
#include "common/usermanual_url.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/format.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/icon.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/modulegroups.h"

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <time.h>

typedef struct dt_iop_gui_simple_callback_t
{
  dt_iop_module_t *self;
  int index;
} dt_iop_gui_simple_callback_t;

static dt_develop_blend_params_t _default_blendop_params
    = { DEVELOP_MASK_DISABLED,
        DEVELOP_BLEND_NORMAL2,
        100.0f,
        DEVELOP_COMBINE_NORM_EXCL,
        0,
        0,
        0.0f,
        DEVELOP_MASK_GUIDE_IN,
        0.0f,
        0.0f,
        0.0f,
        { 0, 0, 0, 0 },
        { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f } };

static void _iop_panel_label(GtkWidget *lab, dt_iop_module_t *module);

void dt_iop_load_default_params(dt_iop_module_t *module)
{
  memset(module->default_blendop_params, 0, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
}

static void dt_iop_modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                 const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
}

static void dt_iop_modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                  dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

gint sort_plugins(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_t *am = (const dt_iop_module_t *)a;
  const dt_iop_module_t *bm = (const dt_iop_module_t *)b;
  if(am->priority == bm->priority) return bm->multi_priority - am->multi_priority;
  return am->priority - bm->priority;
}

/* default groups for modules which does not implement the groups() function */
static int default_groups()
{
  return IOP_GROUP_ALL;
}

/* default flags for modules which does not implement the flags() function */
static int default_flags()
{
  return 0;
}

/* default operation tags for modules which does not implement the flags() function */
static int default_operation_tags()
{
  return 0;
}

/* default operation tags filter for modules which does not implement the flags() function */
static int default_operation_tags_filter()
{
  return 0;
}

static void default_commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params,
                                  dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, self->params_size);
}

static void default_init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                              dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(self->params_size);
  default_commit_params(self, self->default_params, pipe, piece);
}

static void default_cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                 dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void default_gui_cleanup(dt_iop_module_t *self)
{
  g_free(self->gui_data);
  self->gui_data = NULL;
}

static void default_cleanup(dt_iop_module_t *module)
{
  g_free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  g_free(module->params);
  module->params = NULL;
  g_free(module->data); // just to be sure
  module->data = NULL;
}


static int default_distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                                     size_t points_count)
{
  return 1;
}
static int default_distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                                         size_t points_count)
{
  return 1;
}

static void default_process(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                            const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                            const struct dt_iop_roi_t *const roi_out)
{
  if(darktable.codepath.OPENMP_SIMD && self->process_plain)
    self->process_plain(self, piece, i, o, roi_in, roi_out);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2 && self->process_sse2)
    self->process_sse2(self, piece, i, o, roi_in, roi_out);
#endif
  else if(self->process_plain)
    self->process_plain(self, piece, i, o, roi_in, roi_out);
  else
    dt_unreachable_codepath_with_desc(self->op);
}

static dt_introspection_field_t *default_get_introspection_linear()
{
  return NULL;
}
static dt_introspection_t *default_get_introspection()
{
  return NULL;
}
static void *default_get_p(const void *param, const char *name)
{
  return NULL;
}
static dt_introspection_field_t *default_get_f(const char *name)
{
  return NULL;
}

int dt_iop_load_module_so(void *m, const char *libname, const char *op)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;
  g_strlcpy(module->op, op, 20);
  module->data = NULL;
  dt_print(DT_DEBUG_CONTROL, "[iop_load_module] loading iop `%s' from %s\n", op, libname);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!module->module) goto error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer) & (version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr,
            "[iop_load_module] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n",
            libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()),
            dt_version() < 0 ? "debug" : "opt");
    goto error;
  }
  if(!g_module_symbol(module->module, "dt_module_mod_version", (gpointer) & (module->version))) goto error;
  if(!g_module_symbol(module->module, "name", (gpointer) & (module->name))) goto error;
  if(!g_module_symbol(module->module, "groups", (gpointer) & (module->groups)))
    module->groups = default_groups;
  if(!g_module_symbol(module->module, "flags", (gpointer) & (module->flags))) module->flags = default_flags;
  if(!g_module_symbol(module->module, "description", (gpointer) & (module->description))) module->description = NULL;
  if(!g_module_symbol(module->module, "operation_tags", (gpointer) & (module->operation_tags)))
    module->operation_tags = default_operation_tags;
  if(!g_module_symbol(module->module, "operation_tags_filter", (gpointer) & (module->operation_tags_filter)))
    module->operation_tags_filter = default_operation_tags_filter;
  if(!g_module_symbol(module->module, "input_format", (gpointer) & (module->input_format)))
    module->input_format = default_input_format;
  if(!g_module_symbol(module->module, "output_format", (gpointer) & (module->output_format)))
    module->output_format = default_output_format;
  if(!g_module_symbol(module->module, "tiling_callback", (gpointer) & (module->tiling_callback)))
    module->tiling_callback = default_tiling_callback;
  if(!g_module_symbol(module->module, "gui_reset", (gpointer) & (module->gui_reset)))
    module->gui_reset = NULL;
  if(!g_module_symbol(module->module, "gui_init", (gpointer) & (module->gui_init))) module->gui_init = NULL;
  if(!g_module_symbol(module->module, "gui_update", (gpointer) & (module->gui_update)))
    module->gui_update = NULL;
  if(!g_module_symbol(module->module, "gui_cleanup", (gpointer) & (module->gui_cleanup)))
    module->gui_cleanup = default_gui_cleanup;

  if(!g_module_symbol(module->module, "gui_post_expose", (gpointer) & (module->gui_post_expose)))
    module->gui_post_expose = NULL;
  if(!g_module_symbol(module->module, "gui_focus", (gpointer) & (module->gui_focus)))
    module->gui_focus = NULL;

  if(!g_module_symbol(module->module, "init_key_accels", (gpointer) & (module->init_key_accels)))
    module->init_key_accels = NULL;
  if(!g_module_symbol(module->module, "connect_key_accels", (gpointer) & (module->connect_key_accels)))
    module->connect_key_accels = NULL;

  if(!g_module_symbol(module->module, "disconnect_key_accels", (gpointer) & (module->disconnect_key_accels)))
    module->disconnect_key_accels = NULL;
  if(!g_module_symbol(module->module, "mouse_leave", (gpointer) & (module->mouse_leave)))
    module->mouse_leave = NULL;
  if(!g_module_symbol(module->module, "mouse_moved", (gpointer) & (module->mouse_moved)))
    module->mouse_moved = NULL;
  if(!g_module_symbol(module->module, "button_released", (gpointer) & (module->button_released)))
    module->button_released = NULL;
  if(!g_module_symbol(module->module, "button_pressed", (gpointer) & (module->button_pressed)))
    module->button_pressed = NULL;
  if(!g_module_symbol(module->module, "configure", (gpointer) & (module->configure)))
    module->configure = NULL;
  if(!g_module_symbol(module->module, "scrolled", (gpointer) & (module->scrolled))) module->scrolled = NULL;

  if(!g_module_symbol(module->module, "init", (gpointer) & (module->init))) goto error;
  if(!g_module_symbol(module->module, "cleanup", (gpointer) & (module->cleanup)))
    module->cleanup = &default_cleanup;
  if(!g_module_symbol(module->module, "init_global", (gpointer) & (module->init_global)))
    module->init_global = NULL;
  if(!g_module_symbol(module->module, "cleanup_global", (gpointer) & (module->cleanup_global)))
    module->cleanup_global = NULL;
  if(!g_module_symbol(module->module, "init_presets", (gpointer) & (module->init_presets)))
    module->init_presets = NULL;
  if(!g_module_symbol(module->module, "commit_params", (gpointer) & (module->commit_params)))
    module->commit_params = default_commit_params;
  if(!g_module_symbol(module->module, "reload_defaults", (gpointer) & (module->reload_defaults)))
    module->reload_defaults = NULL;
  if(!g_module_symbol(module->module, "init_pipe", (gpointer) & (module->init_pipe)))
    module->init_pipe = default_init_pipe;
  if(!g_module_symbol(module->module, "cleanup_pipe", (gpointer) & (module->cleanup_pipe)))
    module->cleanup_pipe = default_cleanup_pipe;

  module->process = default_process;

  if(!g_module_symbol(module->module, "process_tiling", (gpointer) & (module->process_tiling)))
    module->process_tiling = default_process_tiling;

  if(!g_module_symbol(module->module, "process_sse2", (gpointer) & (module->process_sse2)))
    module->process_sse2 = NULL;

  if(!g_module_symbol(module->module, "process", (gpointer) & (module->process_plain))) goto error;

  if(!darktable.opencl->inited
     || !g_module_symbol(module->module, "process_cl", (gpointer) & (module->process_cl)))
    module->process_cl = NULL;
  if(!g_module_symbol(module->module, "process_tiling_cl", (gpointer) & (module->process_tiling_cl)))
    module->process_tiling_cl = darktable.opencl->inited ? default_process_tiling_cl : NULL;
  if(!g_module_symbol(module->module, "distort_transform", (gpointer) & (module->distort_transform)))
    module->distort_transform = default_distort_transform;
  if(!g_module_symbol(module->module, "distort_backtransform", (gpointer) & (module->distort_backtransform)))
    module->distort_backtransform = default_distort_backtransform;

  if(!g_module_symbol(module->module, "modify_roi_in", (gpointer) & (module->modify_roi_in)))
    module->modify_roi_in = dt_iop_modify_roi_in;
  if(!g_module_symbol(module->module, "modify_roi_out", (gpointer) & (module->modify_roi_out)))
    module->modify_roi_out = dt_iop_modify_roi_out;
  if(!g_module_symbol(module->module, "legacy_params", (gpointer) & (module->legacy_params)))
    module->legacy_params = NULL;
  // allow to select a shape inside an iop
  if(!g_module_symbol(module->module, "masks_selection_changed", (gpointer) & (module->masks_selection_changed)))
    module->masks_selection_changed = NULL;

  // the introspection api
  module->have_introspection = FALSE;
  module->get_p = default_get_p;
  module->get_f = default_get_f;
  module->get_introspection_linear = default_get_introspection_linear;
  module->get_introspection = default_get_introspection;
  if(!g_module_symbol(module->module, "introspection_init", (gpointer) & (module->introspection_init)))
    module->introspection_init = NULL;
  if(module->introspection_init && !module->introspection_init(module, DT_INTROSPECTION_VERSION))
  {
    // set the introspection related fields in module
    module->have_introspection = TRUE;
    if(!g_module_symbol(module->module, "get_p", (gpointer) & (module->get_p))) goto error;
    if(!g_module_symbol(module->module, "get_f", (gpointer) & (module->get_f))) goto error;
    if(!g_module_symbol(module->module, "get_introspection", (gpointer) & (module->get_introspection)))
      goto error;
    if(!g_module_symbol(module->module, "get_introspection_linear",
                        (gpointer) & (module->get_introspection_linear)))
      goto error;
  }

  if(module->init_global) module->init_global(module);
  return 0;
error:
  fprintf(stderr, "[iop_load_module] failed to open operation `%s': %s\n", op, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
}

int dt_iop_load_module_by_so(dt_iop_module_t *module, dt_iop_module_so_t *so, dt_develop_t *dev)
{
  module->dt = &darktable;
  module->dev = dev;
  module->widget = NULL;
  module->header = NULL;
  module->off = NULL;
  module->priority = 0;
  module->hide_enable_button = 0;
  module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  module->request_histogram = DT_REQUEST_ONLY_IN_GUI;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
  module->multi_priority = 0;
  for(int k = 0; k < 3; k++)
  {
    module->picked_color[k] = module->picked_output_color[k] = 0.0f;
    module->picked_color_min[k] = module->picked_output_color_min[k] = 666.0f;
    module->picked_color_max[k] = module->picked_output_color_max[k] = -666.0f;
  }
  module->color_picker_box[0] = module->color_picker_box[1] = .25f;
  module->color_picker_box[2] = module->color_picker_box[3] = .75f;
  module->color_picker_point[0] = module->color_picker_point[1] = 0.5f;
  module->histogram = NULL;
  module->histogram_max[0] = module->histogram_max[1] = module->histogram_max[2] = module->histogram_max[3]
      = 0;
  module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  module->suppress_mask = 0;
  module->bypass_blendif = 0;
  module->enabled = module->default_enabled = 0; // all modules disabled by default.
  g_strlcpy(module->op, so->op, 20);

  // only reference cached results of dlopen:
  module->module = so->module;
  module->so = so;

  module->version = so->version;
  module->name = so->name;
  module->groups = so->groups;
  module->flags = so->flags;
  module->description = so->description;
  module->operation_tags = so->operation_tags;
  module->operation_tags_filter = so->operation_tags_filter;
  module->input_format = so->input_format;
  module->output_format = so->output_format;
  module->tiling_callback = so->tiling_callback;
  module->gui_update = so->gui_update;
  module->gui_reset = so->gui_reset;
  module->gui_init = so->gui_init;
  module->gui_cleanup = so->gui_cleanup;

  module->gui_post_expose = so->gui_post_expose;
  module->gui_focus = so->gui_focus;
  module->mouse_leave = so->mouse_leave;
  module->mouse_moved = so->mouse_moved;
  module->button_released = so->button_released;
  module->button_pressed = so->button_pressed;
  module->configure = so->configure;
  module->scrolled = so->scrolled;

  module->init = so->init;
  module->original_init = so->original_init;
  module->cleanup = so->cleanup;
  module->commit_params = so->commit_params;
  module->reload_defaults = so->reload_defaults;
  module->init_pipe = so->init_pipe;
  module->cleanup_pipe = so->cleanup_pipe;
  module->process = so->process;
  module->process_tiling = so->process_tiling;
  module->process_plain = so->process_plain;
  module->process_sse2 = so->process_sse2;
  module->process_cl = so->process_cl;
  module->process_tiling_cl = so->process_tiling_cl;
  module->distort_transform = so->distort_transform;
  module->distort_backtransform = so->distort_backtransform;
  module->modify_roi_in = so->modify_roi_in;
  module->modify_roi_out = so->modify_roi_out;
  module->legacy_params = so->legacy_params;
  // allow to select a shape inside an iop
  module->masks_selection_changed = so->masks_selection_changed;

  module->connect_key_accels = so->connect_key_accels;
  module->disconnect_key_accels = so->disconnect_key_accels;

  module->have_introspection = so->have_introspection;
  module->get_introspection = so->get_introspection;
  module->get_introspection_linear = so->get_introspection_linear;
  module->get_p = so->get_p;
  module->get_f = so->get_f;

  module->accel_closures = NULL;
  module->accel_closures_local = NULL;
  module->local_closures_connected = FALSE;
  module->reset_button = NULL;
  module->presets_button = NULL;
  module->fusion_slider = NULL;

  if(module->dev && module->dev->gui_attached)
  {
    /* set button state */
    char option[1024];
    snprintf(option, sizeof(option), "plugins/darkroom/%s/visible", module->op);
    dt_iop_module_state_t state = dt_iop_state_HIDDEN;
    if(dt_conf_get_bool(option))
    {
      state = dt_iop_state_ACTIVE;
      snprintf(option, sizeof(option), "plugins/darkroom/%s/favorite", module->op);
      if(dt_conf_get_bool(option)) state = dt_iop_state_FAVORITE;
    }
    dt_iop_gui_set_state(module, state);
  }

  module->data = so->data;

  // now init the instance:
  module->init(module);

  /* initialize blendop params and default values */
  module->blend_params = calloc(1, sizeof(dt_develop_blend_params_t));
  module->default_blendop_params = calloc(1, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));

  if(module->priority == 0)
  {
    fprintf(stderr, "[iop_load_module] `%s' needs to set priority!\n", so->op);
    return 1; // this needs to be set
  }
  if(module->params_size == 0)
  {
    fprintf(stderr, "[iop_load_module] `%s' needs to have a params size > 0!\n", so->op);
    return 1; // empty params hurt us in many places, just add a dummy value
  }
  module->enabled = module->default_enabled; // apply (possibly new) default.
  return 0;
}

void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                      struct dt_dev_pixelpipe_iop_t *piece)
{
  module->init_pipe(module, pipe, piece);
  piece->blendop_data = calloc(1, sizeof(dt_develop_blend_params_t));
  /// FIXME: Commit params is already done in module
  dt_iop_commit_params(module, module->default_params, module->default_blendop_params, pipe, piece);
}

static void dt_iop_gui_delete_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_develop_t *dev = module->dev;

  // we search another module with the same base
  // we want the next module if any or the previous one
  GList *modules = g_list_first(module->dev->iop);
  dt_iop_module_t *next = NULL;
  int find = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      find = 1;
      if(next) break;
    }
    else if(mod->instance == module->instance)
    {
      next = mod;
      if(find) break;
    }
    modules = g_list_next(modules);
  }
  if(!next) return; // what happened ???

  // we must pay attention if priority is 0
  gboolean is_zero = (module->multi_priority == 0);

  // we set the focus to the other instance
  dt_iop_gui_set_expanded(next, TRUE, FALSE);
  gtk_widget_grab_focus(next->expander);

  darktable.gui->reset = 1;

  // we remove the plugin effectively
  if(!dt_iop_is_hidden(module))
  {
    // we just hide the module to avoid lots of gtk critical warnings
    gtk_widget_hide(module->expander);

    // we move the module far away, to avoid problems when reordering instance after that
    // FIXME: ?????
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                          module->expander, -1);

    gtk_widget_destroy(module->widget);
    dt_iop_gui_cleanup_module(module);
  }

  // we remove all references in the history stack and dev->iop
  // this will inform that a module has been removed from history
  // we do it here so we have the multi_priorities to reconstruct
  // de deleted module if the user undo it
  dt_dev_module_remove(dev, module);

  // if module was priority 0, then we set next to priority 0
  if(is_zero)
  {
    // we set priority of next to 0
    next->multi_priority = 0;

    // we change this in the history stack too
    GList *history = g_list_first(module->dev->history);
    while(history)
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      if(hist->module == next) hist->multi_priority = 0;
      history = g_list_next(history);
    }
  }

  // we save the current state of history (with the new multi_priorities)
  if(dev->gui_attached)
  {
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  // we cleanup the module
  dt_accel_disconnect_list(module->accel_closures);
  dt_accel_cleanup_locals_iop(module);
  module->accel_closures = NULL;
  // don't delete the module, a pipe may still need it
  dev->alliop = g_list_append(dev->alliop, module);
  module = NULL;

  // we update show params for multi-instances for each other instances
  dt_dev_modules_update_multishow(dev);

  // we refresh the pipe
  dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->pipe->cache_obsolete = 1;
  dev->preview_pipe->cache_obsolete = 1;

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(dev);

  /* redraw */
  dt_control_queue_redraw_center();

  darktable.gui->reset = 0;
}

static void dt_iop_gui_movedown_callback(GtkButton *button, dt_iop_module_t *module)
{
  // we find the next module
  GList *modules = g_list_last(module->dev->iop);
  dt_iop_module_t *next = NULL;
  int find = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      find = 1;
    else if(mod->instance == module->instance && find == 1)
    {
      next = mod;
      break;
    }
    modules = g_list_previous(modules);
  }
  if(!next) return;

  // we exchange the priority of both module
  int oldp = next->multi_priority;
  next->multi_priority = module->multi_priority;
  module->multi_priority = oldp;

  // we change this in the history stack too
  GList *history = g_list_first(module->dev->history);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    if(hist->module == module)
      hist->multi_priority = module->multi_priority;
    else if(hist->module == next)
      hist->multi_priority = next->multi_priority;
    history = g_list_next(history);
  }

  // we update the list of iop
  modules = g_list_first(next->dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      next->dev->iop = g_list_remove_link(next->dev->iop, modules);
      break;
    }
    modules = g_list_next(modules);
  }
  next->dev->iop = g_list_insert_sorted(next->dev->iop, module, sort_plugins);

  // we update the headers
  dt_dev_module_update_multishow(next->dev, module);
  dt_dev_module_update_multishow(next->dev, next);

  // we move the headers
  GValue gv = { 0, { { 0 } } };
  g_value_init(&gv, G_TYPE_INT);
  gtk_container_child_get_property(
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
      module->expander, "position", &gv);
  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv) + 1);

  /* signal that history has changed */
  if(next->dev->gui_attached)
  {
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  // we rebuild the pipe
  next->dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  next->dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  next->dev->pipe->cache_obsolete = 1;
  next->dev->preview_pipe->cache_obsolete = 1;

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(next->dev);

  /* redraw */
  dt_control_queue_redraw_center();
}

static void dt_iop_gui_moveup_callback(GtkButton *button, dt_iop_module_t *module)
{
  // we find the previous module
  GList *modules = g_list_first(module->dev->iop);
  dt_iop_module_t *prev = NULL;
  int find = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      find = 1;
    else if(mod->instance == module->instance && find == 1)
    {
      prev = mod;
      break;
    }
    modules = g_list_next(modules);
  }
  if(!prev) return;

  // we exchange the priority of both module
  int oldp = prev->multi_priority;
  prev->multi_priority = module->multi_priority;
  module->multi_priority = oldp;

  // we change this in the history stack too
  GList *history = g_list_first(module->dev->history);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    if(hist->module == module)
      hist->multi_priority = module->multi_priority;
    else if(hist->module == prev)
      hist->multi_priority = prev->multi_priority;
    history = g_list_next(history);
  }

  // we update the list of iop
  modules = g_list_first(prev->dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      prev->dev->iop = g_list_remove_link(prev->dev->iop, modules);
      break;
    }
    modules = g_list_next(modules);
  }
  prev->dev->iop = g_list_insert_sorted(prev->dev->iop, module, sort_plugins);

  // we update the headers
  dt_dev_module_update_multishow(prev->dev, module);
  dt_dev_module_update_multishow(prev->dev, prev);

  // we move the headers
  GValue gv = { 0, { { 0 } } };
  g_value_init(&gv, G_TYPE_INT);
  gtk_container_child_get_property(
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
      module->expander, "position", &gv);
  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv) - 1);

  /* signal that history has changed */
  if(prev->dev->gui_attached)
  {
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  // we rebuild the pipe
  prev->dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  prev->dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  prev->dev->pipe->cache_obsolete = 1;
  prev->dev->preview_pipe->cache_obsolete = 1;

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(prev->dev);

  /* redraw */
  dt_control_queue_redraw_center();
}

dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base, gboolean copy_params)
{
  // make sure the duplicated module appears in the history
  dt_dev_add_history_item(base->dev, base, FALSE);

  // first we create the new module
  dt_iop_module_t *module = dt_dev_module_duplicate(base->dev, base, 0);
  if(!module) return NULL;

  // we reflect the positions changes in the history stack too
  GList *history = g_list_first(module->dev->history);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    if(hist->module->instance == base->instance) hist->multi_priority = hist->module->multi_priority;
    history = g_list_next(history);
  }

  // what is the position of the module in the pipe ?
  GList *modules = g_list_first(module->dev->iop);
  int pos_module = 0;
  int pos_base = 0;
  int pos = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      pos_module = pos;
    else if(mod == base)
      pos_base = pos;
    modules = g_list_next(modules);
    pos++;
  }

  // we set the gui part of it
  // darktable.gui->reset = 1;
  /* initialize gui if iop have one defined */
  if(!dt_iop_is_hidden(module))
  {
    module->gui_init(module);
    dt_iop_reload_defaults(module); // some modules like profiled denoise update the gui in reload_defaults
    if(copy_params)
    {
      memcpy(module->params, base->params, module->params_size);
      if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      {
        memcpy(module->blend_params, base->blend_params, sizeof(dt_develop_blend_params_t));
        if(base->blend_params->mask_id > 0)
        {
          module->blend_params->mask_id = 0;
          dt_masks_iop_use_same_as(module, base);
        }
      }
    }

    // we save the new instance creation but keep it disabled
    dt_dev_add_history_item(module->dev, module, FALSE);

    /* update ui to default params*/
    dt_iop_gui_update(module);
    /* add module to right panel */
    GtkWidget *expander = dt_iop_gui_get_expander(module);
    dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
    GValue gv = { 0, { { 0 } } };
    g_value_init(&gv, G_TYPE_INT);
    gtk_container_child_get_property(
        GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
        base->expander, "position", &gv);
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                          expander, g_value_get_int(&gv) + pos_base - pos_module + 1);
    dt_iop_gui_set_expanded(module, TRUE, FALSE);
    dt_iop_gui_update_blending(module);
  }

  if(dt_conf_get_bool("darkroom/ui/single_module"))
  {
    dt_iop_gui_set_expanded(base, FALSE, FALSE);
    dt_iop_gui_set_expanded(module, TRUE, FALSE);
  }

  /* setup key accelerators */
  module->accel_closures = NULL;
  if(module->connect_key_accels) module->connect_key_accels(module);
  dt_iop_connect_common_accels(module);
  // darktable.gui->reset = 0;

  // we update show params for multi-instances for each other instances
  dt_dev_modules_update_multishow(module->dev);

  // and we refresh the pipe
  dt_iop_request_focus(module);

  dt_dev_masks_list_change(module->dev);

  if(module->dev->gui_attached)
  {
    module->dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
    module->dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
    module->dev->pipe->cache_obsolete = 1;
    module->dev->preview_pipe->cache_obsolete = 1;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(module->dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
  return module;
}

static void dt_iop_gui_copy_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_gui_duplicate(user_data, FALSE);
}

static void dt_iop_gui_duplicate_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_gui_duplicate(user_data, TRUE);
}

typedef struct dt_iop_gui_rename_module_t
{
  GtkWidget *floating_window;
  dt_iop_module_t *module;
} dt_iop_gui_rename_module_t;

// http://stackoverflow.com/questions/4631388/transparent-floating-gtkentry
static gboolean _rename_module_key_press(GtkWidget *entry, GdkEventKey *event, dt_iop_gui_rename_module_t *d)
{
  int ended = 0;

  switch(event->keyval)
  {
    case GDK_KEY_Escape:
      ended = 1;
      break;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
      if(strcmp(d->module->multi_name, name) != 0)
      {
        g_strlcpy(d->module->multi_name, name, sizeof(d->module->multi_name) - 1);
        dt_dev_add_history_item(d->module->dev, d->module, TRUE);
        dt_iop_gui_update_header(d->module);
      }

      ended = 1;
    }
    break;
  }

  if(ended)
  {
    gtk_widget_destroy(d->floating_window);
    free(d);
    return TRUE;
  }
  return FALSE; /* event not handled */
}

static gboolean _rename_module_destroy(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_destroy(GTK_WIDGET(user_data));
  return FALSE;
}

static void _iop_gui_rename_module(dt_iop_module_t *module)
{
  const int bs = DT_PIXEL_APPLY_DPI(12);
  gint px = 0, py = 0;

  dt_iop_gui_rename_module_t *d = (dt_iop_gui_rename_module_t *)calloc(1, sizeof(dt_iop_gui_rename_module_t));
  d->module = module;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);

  GList *childs = gtk_container_get_children(GTK_CONTAINER(module->header));
  GtkWidget *label = g_list_nth_data(childs, 5);
  gdk_window_get_origin(gtk_widget_get_window(label), &px, &py);
  const gint w = gdk_window_get_width(gtk_widget_get_window(label)) - bs * 8 - bs * 1.7;
  const gint h = gdk_window_get_height(gtk_widget_get_window(label));

  const gint x = px + bs * 6;
  const gint y = py;

  d->floating_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->floating_window);
#endif
  /* stackoverflow.com/questions/1925568/how-to-give-keyboard-focus-to-a-pop-up-gtk-window */
  gtk_widget_set_can_focus(d->floating_window, TRUE);
  gtk_window_set_decorated(GTK_WINDOW(d->floating_window), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(d->floating_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_transient_for(GTK_WINDOW(d->floating_window), GTK_WINDOW(window));
  gtk_widget_set_opacity(d->floating_window, 0.8);
  gtk_window_move(GTK_WINDOW(d->floating_window), x, y);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_size_request(entry, w, h);
  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);

  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_container_add(GTK_CONTAINER(d->floating_window), entry);
  g_signal_connect(entry, "focus-out-event", G_CALLBACK(_rename_module_destroy), d->floating_window);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_rename_module_key_press), d);

  gtk_entry_set_text(GTK_ENTRY(entry), module->multi_name);

  gtk_widget_show_all(d->floating_window);
  gtk_widget_grab_focus(entry);
  gtk_window_present(GTK_WINDOW(d->floating_window));
}

static void dt_iop_gui_rename_callback(GtkButton *button, dt_iop_module_t *module)
{
  _iop_gui_rename_module(module);
}

static void dt_iop_gui_multiinstance_callback(GtkButton *button, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(module->flags() & IOP_FLAGS_ONE_INSTANCE) return;

  if(event->button == 2)
  {
    dt_iop_gui_copy_callback(button, user_data);
    return;
  }
  else if(event->button == 3)
  {
    return;
  }

  GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
  GtkWidget *item;

  item = gtk_menu_item_new_with_label(_("new instance"));
  // gtk_widget_set_tooltip_text(item, _("add a new instance of this module to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(dt_iop_gui_copy_callback), module);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("duplicate instance"));
  // gtk_widget_set_tooltip_text(item, _("add a copy of this instance to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(dt_iop_gui_duplicate_callback), module);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("move up"));
  // gtk_widget_set_tooltip_text(item, _("move this instance up"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(dt_iop_gui_moveup_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_up);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("move down"));
  // gtk_widget_set_tooltip_text(item, _("move this instance down"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(dt_iop_gui_movedown_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_down);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("delete"));
  // gtk_widget_set_tooltip_text(item, _("delete this instance"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(dt_iop_gui_delete_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_close);
  gtk_menu_shell_append(menu, item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  item = gtk_menu_item_new_with_label(_("rename"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(dt_iop_gui_rename_callback), module);
  gtk_menu_shell_append(menu, item);

  gtk_widget_show_all(GTK_WIDGET(menu));

  // popup
#if GTK_CHECK_VERSION(3, 22, 0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
#else
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, gtk_get_current_event_time());
#endif
}

static void dt_iop_gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset)
  {
    if(gtk_toggle_button_get_active(togglebutton))
      module->enabled = 1;
    else
      module->enabled = 0;
    dt_dev_add_history_item(module->dev, module, FALSE);
  }
  char tooltip[512];
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(tooltip, sizeof(tooltip), module->enabled ? _("%s is switched on") : _("%s is switched off"),
           module_label);
  g_free(module_label);
  gtk_widget_set_tooltip_text(GTK_WIDGET(togglebutton), tooltip);
  gtk_widget_queue_draw(GTK_WIDGET(togglebutton));
}

gboolean dt_iop_so_is_hidden(dt_iop_module_so_t *module)
{
  gboolean is_hidden = TRUE;
  if(!(module->flags() & IOP_FLAGS_HIDDEN))
  {
    if(!module->gui_init)
      g_debug("Module '%s' is not hidden and lacks implementation of gui_init()...", module->op);
    else if(!module->gui_cleanup)
      g_debug("Module '%s' is not hidden and lacks implementation of gui_cleanup()...", module->op);
    else
      is_hidden = FALSE;
  }
  return is_hidden;
}

gboolean dt_iop_is_hidden(dt_iop_module_t *module)
{
  return dt_iop_so_is_hidden(module->so);
}

gboolean dt_iop_shown_in_group(dt_iop_module_t *module, uint32_t group)
{
  uint32_t additional_flags = 0;

  if(group == DT_MODULEGROUP_NONE) return TRUE;

  /* add special group flag for module in active pipe */
  if(module->enabled) additional_flags |= IOP_SPECIAL_GROUP_ACTIVE_PIPE;

  /* add special group flag for favorite */
  if(module->so->state == dt_iop_state_FAVORITE) additional_flags |= IOP_SPECIAL_GROUP_USER_DEFINED;

  return dt_dev_modulegroups_test(module->dev, group, module->groups() | additional_flags);
}

static void _iop_panel_label(GtkWidget *lab, dt_iop_module_t *module)
{
  gtk_widget_set_name(lab, "panel_label");
  gchar *label = dt_history_item_get_name_html(module);
  gchar *tooltip;
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    tooltip = g_strdup(module->name());
  else
    tooltip = g_strdup_printf("%s %s", module->name(), module->multi_name);
  gtk_label_set_markup(GTK_LABEL(lab), label);
  gtk_label_set_ellipsize(GTK_LABEL(lab), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text(lab, tooltip);
  g_free(label);
  g_free(tooltip);
}

static void _iop_gui_update_header(dt_iop_module_t *module)
{
  GList *childs = gtk_container_get_children(GTK_CONTAINER(module->header));

  /* get the enable button spacer and button */
  GtkWidget *eb = g_list_nth_data(childs, 0);
  GtkWidget *ebs = g_list_nth_data(childs, 1);
  GtkWidget *lab = g_list_nth_data(childs, 5);

  g_list_free(childs);

  // set panel name to display correct multi-instance
  _iop_panel_label(lab, module);

  if(module->hide_enable_button)
  {
    gtk_widget_hide(eb);
    gtk_widget_show(ebs);
  }
  else
  {
    gtk_widget_show(eb);
    gtk_widget_hide(ebs);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
  }
}

void dt_iop_gui_update_header(dt_iop_module_t *module)
{
  _iop_gui_update_header(module);
}

static void _iop_gui_update_label(dt_iop_module_t *module)
{
  if(!module->header) return;
  GList *childs = gtk_container_get_children(GTK_CONTAINER(module->header));
  GtkWidget *lab = g_list_nth_data(childs, 5);
  g_list_free(childs);
  _iop_panel_label(lab, module);
}

void dt_iop_reload_defaults(dt_iop_module_t *module)
{
  if(module->reload_defaults) module->reload_defaults(module);
  dt_iop_load_default_params(module);

  if(module->header) _iop_gui_update_header(module);
}

void dt_iop_cleanup_histogram(gpointer data, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)data;

  free(module->histogram);
  module->histogram = NULL;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
}

static void init_presets(dt_iop_module_so_t *module_so)
{
  if(module_so->init_presets) module_so->init_presets(module_so);

  // this seems like a reasonable place to check for and update legacy
  // presets.

  int32_t module_version = module_so->version();

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name, op_version, op_params, blendop_version, blendop_params FROM data.presets WHERE operation = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_so->op, -1, SQLITE_TRANSIENT);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    int32_t old_params_version = sqlite3_column_int(stmt, 1);
    void *old_params = (void *)sqlite3_column_blob(stmt, 2);
    int32_t old_params_size = sqlite3_column_bytes(stmt, 2);
    int32_t old_blend_params_version = sqlite3_column_int(stmt, 3);
    void *old_blend_params = (void *)sqlite3_column_blob(stmt, 4);
    int32_t old_blend_params_size = sqlite3_column_bytes(stmt, 4);

    if(old_params_version == 0)
    {
      // this preset doesn't have a version.  go digging through the database
      // to find a history entry that matches the preset params, and get
      // the module version from that.

      sqlite3_stmt *stmt2;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT module FROM main.history WHERE operation = ?1 AND op_params = ?2", -1,
                                  &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, module_so->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, old_params, old_params_size, SQLITE_TRANSIENT);

      if(sqlite3_step(stmt2) == SQLITE_ROW)
      {
        old_params_version = sqlite3_column_int(stmt2, 0);
      }
      else
      {
        fprintf(stderr, "[imageop_init_presets] WARNING: Could not find versioning information for '%s' "
                        "preset '%s'\nUntil some is found, the preset will be unavailable.\n(To make it "
                        "return, please load an image that uses the preset.)\n",
                module_so->op, name);
        sqlite3_finalize(stmt2);
        continue;
      }

      sqlite3_finalize(stmt2);

      // we found an old params version.  Update the database with it.

      fprintf(stderr, "[imageop_init_presets] Found version %d for '%s' preset '%s'\n", old_params_version,
              module_so->op, name);

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE data.presets SET op_version=?1 WHERE operation=?2 AND name=?3", -1,
                                  &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, old_params_version);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, module_so->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, name, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
    }

    if(module_version > old_params_version && module_so->legacy_params != NULL)
    {
      fprintf(stderr, "[imageop_init_presets] updating '%s' preset '%s' from version %d to version %d\n",
              module_so->op, name, old_params_version, module_version);

      // we need a dt_iop_module_t for legacy_params()
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        free(module);
        continue;
      }

      module->init(module);
      if(module->params_size == 0)
      {
        dt_iop_cleanup_module(module);
        free(module);
        continue;
      }

      // we call reload_defaults() in case the module defines it
      if(module->reload_defaults) module->reload_defaults(module);

      int32_t new_params_size = module->params_size;
      void *new_params = calloc(1, new_params_size);

      // convert the old params to new
      if(module->legacy_params(module, old_params, old_params_version, new_params, module_version))
      {
        free(new_params);
        dt_iop_cleanup_module(module);
        free(module);
        continue;
      }

      // and write the new params back to the database
      sqlite3_stmt *stmt2;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE data.presets "
                                                                 "SET op_version=?1, op_params=?2 "
                                                                 "WHERE operation=?3 AND name=?4",
                                  -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, module->version());
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, new_params, new_params_size, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, module->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 4, name, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      free(new_params);
      dt_iop_cleanup_module(module);
      free(module);
    }
    else if(module_version > old_params_version)
    {
      fprintf(stderr, "[imageop_init_presets] Can't upgrade '%s' preset '%s' from version %d to %d, no "
                      "legacy_params() implemented \n",
              module_so->op, name, old_params_version, module_version);
    }

    if(!old_blend_params || dt_develop_blend_version() > old_blend_params_version)
    {
      fprintf(stderr,
              "[imageop_init_presets] updating '%s' preset '%s' from blendop version %d to version %d\n",
              module_so->op, name, old_blend_params_version, dt_develop_blend_version());

      // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
      // using dt_develop_blend_legacy_params_by_so won't help as we need "module" anyway
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        free(module);
        continue;
      }

      if(module->params_size == 0)
      {
        dt_iop_cleanup_module(module);
        free(module);
        continue;
      }
      void *new_blend_params = malloc(sizeof(dt_develop_blend_params_t));

      // convert the old blend params to new
      if(old_blend_params
         && dt_develop_blend_legacy_params(module, old_blend_params, old_blend_params_version,
                                           new_blend_params, dt_develop_blend_version(),
                                           old_blend_params_size) == 0)
      {
        // do nothing
      }
      else
      {
        memcpy(new_blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
      }

      // and write the new blend params back to the database
      sqlite3_stmt *stmt2;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE data.presets "
                                                                 "SET blendop_version=?1, blendop_params=?2 "
                                                                 "WHERE operation=?3 AND name=?4",
                                  -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, dt_develop_blend_version());
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, new_blend_params, sizeof(dt_develop_blend_params_t),
                                 SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, module->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 4, name, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      free(new_blend_params);
      dt_iop_cleanup_module(module);
      free(module);
    }
  }
  sqlite3_finalize(stmt);
}

static void init_key_accels(dt_iop_module_so_t *module)
{
  // Calling the accelerator initialization callback, if present
  if(module->init_key_accels) (module->init_key_accels)(module);
  /** load shortcuts for presets **/
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name FROM data.presets WHERE operation=?1 ORDER BY writeprotect DESC, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", _("preset"), (const char *)sqlite3_column_text(stmt, 0));
    dt_accel_register_iop(module, FALSE, NC_("accel", path), 0, 0);
  }
  sqlite3_finalize(stmt);
}

static void dt_iop_init_module_so(void *m)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;

  init_presets(module);

  // do not init accelerators if there is no gui
  if(darktable.gui)
  {
    // Calling the accelerator initialization callback, if present
    init_key_accels(module);

    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      dt_accel_register_slider_iop(module, FALSE, NC_("accel", "fusion"));
    }
    if(!(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      // Adding the optional show accelerator to the table (blank)
      dt_accel_register_iop(module, FALSE, NC_("accel", "show module"), 0, 0);
      dt_accel_register_iop(module, FALSE, NC_("accel", "enable module"), 0, 0);

      dt_accel_register_iop(module, FALSE, NC_("accel", "reset module parameters"), 0, 0);
      dt_accel_register_iop(module, FALSE, NC_("accel", "show preset menu"), 0, 0);
    }
  }
}

void dt_iop_load_modules_so()
{
  darktable.iop = dt_module_load_modules("/plugins", sizeof(dt_iop_module_so_t), dt_iop_load_module_so,
                                         dt_iop_init_module_so, NULL);
}

int dt_iop_load_module(dt_iop_module_t *module, dt_iop_module_so_t *module_so, dt_develop_t *dev)
{
  memset(module, 0, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, dev))
  {
    free(module);
    return 1;
  }
  module->data = module_so->data;
  module->so = module_so;
  dt_iop_reload_defaults(module);
  return 0;
}

GList *dt_iop_load_modules_ext(dt_develop_t *dev, gboolean no_image)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dt_iop_module_so_t *module_so;
  dev->iop_instance = 0;
  GList *iop = darktable.iop;
  while(iop)
  {
    module_so = (dt_iop_module_so_t *)iop->data;
    module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
    if(dt_iop_load_module_by_so(module, module_so, dev))
    {
      free(module);
      continue;
    }
    res = g_list_insert_sorted(res, module, sort_plugins);
    module->data = module_so->data;
    module->so = module_so;
    if(!no_image) dt_iop_reload_defaults(module);
    iop = g_list_next(iop);
  }

  GList *it = res;
  while(it)
  {
    module = (dt_iop_module_t *)it->data;
    module->instance = dev->iop_instance++;
    module->multi_name[0] = '\0';
    it = g_list_next(it);
  }
  return res;
}

GList *dt_iop_load_modules(dt_develop_t *dev)
{
  return dt_iop_load_modules_ext(dev, FALSE);
}

void dt_iop_cleanup_module(dt_iop_module_t *module)
{
  module->cleanup(module);

  free(module->default_params);
  module->default_params = NULL;
  free(module->blend_params);
  module->blend_params = NULL;
  free(module->default_blendop_params);
  module->default_blendop_params = NULL;
  free(module->histogram);
  module->histogram = NULL;
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

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params,
                          dt_develop_blend_params_t *blendop_params, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  uint64_t hash = 5381;
  piece->hash = 0;

  if(piece->enabled)
  {
    /* construct module params data for hash calc */
    int length = module->params_size;
    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) length += sizeof(dt_develop_blend_params_t);
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, blendop_params->mask_id);
    length += dt_masks_group_get_hash_buffer_length(grp);

    char *str = malloc(length);
    memcpy(str, module->params, module->params_size);
    int pos = module->params_size;
    /* if module supports blend op add blend params into account */
    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      memcpy(str + module->params_size, blendop_params, sizeof(dt_develop_blend_params_t));
      pos += sizeof(dt_develop_blend_params_t);
    }
    memcpy(piece->blendop_data, blendop_params, sizeof(dt_develop_blend_params_t));
    // this should be redundant! (but is not)
    memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    /* and we add masks */
    dt_masks_group_get_hash_buffer(grp, str + pos);

    // assume process_cl is ready, commit_params can overwrite this.
    if(module->process_cl) piece->process_cl_ready = 1;

    // register if module allows tiling, commit_params can overwrite this.
    if(module->flags() & IOP_FLAGS_ALLOW_TILING) piece->process_tiling_ready = 1;

    module->commit_params(module, params, pipe, piece);
    for(int i = 0; i < length; i++) hash = ((hash << 5) + hash) ^ str[i];
    piece->hash = hash;

    free(str);
  }
  // printf("commit params hash += module %s: %lu, enabled = %d\n", piece->module->op, piece->hash,
  // piece->enabled);
}

void dt_iop_gui_cleanup_module(dt_iop_module_t *module)
{
  module->gui_cleanup(module);
  dt_iop_gui_cleanup_blending(module);
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  if(!dt_iop_is_hidden(module))
  {
    module->gui_update(module);
    dt_iop_gui_update_blending(module);
    dt_iop_gui_update_expanded(module);
    _iop_gui_update_label(module);
    if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
  }
  darktable.gui->reset = reset;
}

void dt_iop_gui_reset(dt_iop_module_t *module)
{
  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  if(module->gui_reset && !dt_iop_is_hidden(module)) module->gui_reset(module);
  darktable.gui->reset = reset;
}

static int _iop_module_demosaic = 0, _iop_module_colorout = 0, _iop_module_colorin = 0;
dt_iop_colorspace_type_t dt_iop_module_colorspace(const dt_iop_module_t *module)
{
  /* check if we do know what priority the color* plugins have */
  if(_iop_module_colorout == 0 && _iop_module_colorin == 0)
  {
    /* lets find out which priority colorin and colorout have */
    GList *iop = module->dev->iop;
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
      if(m != module)
      {
        if(!strcmp(m->op, "colorin"))
          _iop_module_colorin = m->priority;
        else if(!strcmp(m->op, "colorout"))
          _iop_module_colorout = m->priority;
        else if(!strcmp(m->op, "demosaic"))
          _iop_module_demosaic = m->priority;
      }

      /* do we have both priorities, lets break out... */
      if(_iop_module_colorout && _iop_module_colorin && _iop_module_demosaic) break;
      iop = g_list_next(iop);
    }
  }

  /* let check which colorspace module is within */
  if(module->priority > _iop_module_colorout)
    return iop_cs_rgb;
  else if(module->priority > _iop_module_colorin)
    return iop_cs_Lab;
  else if(module->priority < _iop_module_demosaic)
    return iop_cs_RAW;

  /* fallback to rgb */
  return iop_cs_rgb;
}

static void dt_iop_gui_reset_callback(GtkButton *button, dt_iop_module_t *module)
{
  // if a drawn mask is set, remove it from the list
  if(module->blend_params->mask_id > 0)
  {
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
    if(grp) dt_masks_form_remove(module, NULL, grp);
    dt_dev_masks_list_change(module->dev);
  }
  /* reset to default params */
  memcpy(module->params, module->default_params, module->params_size);
  memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));

  /* reset ui to its defaults */
  dt_iop_gui_reset(module);

  /* update ui to default params*/
  dt_iop_gui_update(module);

  dt_dev_add_history_item(module->dev, module, TRUE);
}

#if !GTK_CHECK_VERSION(3, 22, 0)
static void _preset_popup_position(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  GtkRequisition requisition = { 0 };
  gdk_window_get_origin(gtk_widget_get_window(GTK_WIDGET(data)), x, y);
  gtk_widget_get_preferred_size(GTK_WIDGET(menu), &requisition, NULL);

  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(data), &allocation);
  (*y) += allocation.height;
}
#endif

static void popup_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_gui_presets_popup_menu_show_for_module(module);
  gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));

#if GTK_CHECK_VERSION(3, 22, 0)
  gtk_menu_popup_at_widget(darktable.gui->presets_popup_menu,
                           dtgtk_expander_get_header(DTGTK_EXPANDER(module->expander)), GDK_GRAVITY_SOUTH_WEST,
                           GDK_GRAVITY_NORTH_WEST, NULL);
#else
  gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, _preset_popup_position, button, 0,
                 gtk_get_current_event_time());
  gtk_menu_reposition(GTK_MENU(darktable.gui->presets_popup_menu));
#endif
}

void dt_iop_request_focus(dt_iop_module_t *module)
{
  if(darktable.gui->reset || (darktable.develop->gui_module == module)) return;

  if(darktable.develop->gui_module != module) darktable.develop->focus_hash++;

  /* lets lose the focus of previous focus module*/
  if(darktable.develop->gui_module)
  {
    if(darktable.develop->gui_module->gui_focus)
      darktable.develop->gui_module->gui_focus(darktable.develop->gui_module, FALSE);

    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(darktable.develop->gui_module), GTK_STATE_FLAG_NORMAL,
                               TRUE);

    //    gtk_widget_set_state(darktable.develop->gui_module->topwidget, GTK_STATE_NORMAL);

    /*
    GtkWidget *off = GTK_WIDGET(darktable.develop->gui_module->off);

    if (off)
      gtk_widget_set_state(off,
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(off)) ?
         GTK_STATE_ACTIVE : GTK_STATE_NORMAL);
    */

    if(darktable.develop->gui_module->operation_tags_filter()) dt_dev_invalidate_from_gui(darktable.develop);

    dt_accel_disconnect_locals_iop(darktable.develop->gui_module);

    /*reset mask view */
    dt_masks_reset_form_gui();
  }

  darktable.develop->gui_module = module;

  /* set the focus on module */
  if(module)
  {
    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(module), GTK_STATE_FLAG_SELECTED, TRUE);

    // gtk_widget_set_state(module->widget,    GTK_STATE_NORMAL);

    /*
    GtkWidget *off = GTK_WIDGET(darktable.develop->gui_module->off);
    if (off)
      gtk_widget_set_state(off,
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(off)) ?
         GTK_STATE_ACTIVE : GTK_STATE_NORMAL);
    */
    if(module->operation_tags_filter()) dt_dev_invalidate_from_gui(darktable.develop);

    dt_accel_connect_locals_iop(module);

    if(module->gui_focus) module->gui_focus(module, TRUE);
  }

  dt_control_change_cursor(GDK_LEFT_PTR);
}


/*
 * NEW EXPANDER
 */

static void dt_iop_gui_set_single_expanded(dt_iop_module_t *module, gboolean expanded)
{
  if(!module->expander) return;

  /* update expander arrow state */
  GtkWidget *icon;

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);

  GtkWidget *header = dtgtk_expander_get_header(DTGTK_EXPANDER(module->expander));
  gint flags = CPF_DIRECTION_DOWN;

  /* get arrow icon widget */
  GList *childs = gtk_container_get_children(GTK_CONTAINER(header));
  icon = g_list_last(childs)->data;
  g_list_free(childs);
  if(!expanded) flags = CPF_DIRECTION_LEFT;

  dtgtk_icon_set_paint(icon, dtgtk_cairo_paint_solid_arrow, flags, NULL);

  /* store expanded state of module.
   * we do that first, so update_expanded won't think it should be visible
   * and undo our changes right away. */
  module->expanded = expanded;
  char var[1024];
  snprintf(var, sizeof(var), "plugins/darkroom/%s/expanded", module->op);
  dt_conf_set_bool(var, expanded);

  /* show / hide plugin widget */
  if(expanded)
  {
    /* set this module to receive focus / draw events*/
    dt_iop_request_focus(module);

    /* focus the current module */
    for(int k = 0; k < DT_UI_CONTAINER_SIZE; k++)
      dt_ui_container_focus_widget(darktable.gui->ui, k, module->expander);

    /* redraw center, iop might have post expose */
    dt_control_queue_redraw_center();
  }
  else
  {
    if(module->dev->gui_module == module)
    {
      dt_iop_request_focus(NULL);
      dt_control_queue_redraw_center();
    }
  }
}

void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded, gboolean collapse_others)
{
  if(!module->expander) return;

  /* handle shiftclick on expander, hide all except this */
  if(collapse_others)
  {
    int current_group = dt_dev_modulegroups_get(module->dev);
    GList *iop = g_list_first(module->dev->iop);
    gboolean all_other_closed = TRUE;
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;

      if(m != module && dt_iop_shown_in_group(m, current_group))
      {
        all_other_closed = all_other_closed && !m->expanded;
        dt_iop_gui_set_single_expanded(m, FALSE);
      }

      iop = g_list_next(iop);
    }
    if(all_other_closed)
      dt_iop_gui_set_single_expanded(module, !module->expanded);
    else
      dt_iop_gui_set_single_expanded(module, TRUE);
  }
  else
  {
    /* else just toggle */
    dt_iop_gui_set_single_expanded(module, expanded);
  }
}

void dt_iop_gui_update_expanded(dt_iop_module_t *module)
{
  if(!module->expander) return;

  gboolean expanded = module->expanded;

  /* update expander arrow state */
  GtkWidget *icon;
  GtkWidget *header = dtgtk_expander_get_header(DTGTK_EXPANDER(module->expander));

  gint flags = CPF_DIRECTION_DOWN;

  /* get arrow icon widget */
  GList *childs = gtk_container_get_children(GTK_CONTAINER(header));
  icon = g_list_last(childs)->data;
  g_list_free(childs);
  if(!expanded) flags = CPF_DIRECTION_LEFT;

  dtgtk_icon_set_paint(icon, dtgtk_cairo_paint_solid_arrow, flags, NULL);

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);
}

static gboolean _iop_plugin_body_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(e->button == 1)
  {
    dt_iop_request_focus(module);
    return TRUE;
  }
  else if(e->button == 3)
  {
    dt_gui_presets_popup_menu_show_for_module(module);
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));

#if GTK_CHECK_VERSION(3, 22, 0)
    gtk_menu_popup_at_pointer(darktable.gui->presets_popup_menu, (GdkEvent *)e);
#else
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, e->button, e->time);
#endif

    return TRUE;
  }
  return FALSE;
}

static gboolean _iop_plugin_header_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return TRUE;

  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if(e->button == 1)
  {
    // make gtk scroll to the module once it updated its allocation size
    if(dt_conf_get_bool("darkroom/ui/scroll_to_module"))
      darktable.gui->scroll_to[1] = module->expander;

    gboolean collapse_others = !dt_conf_get_bool("darkroom/ui/single_module") != !(e->state & GDK_SHIFT_MASK);
    dt_iop_gui_set_expanded(module, !module->expanded, collapse_others);

    return TRUE;
  }
  else if(e->button == 3)
  {
    dt_gui_presets_popup_menu_show_for_module(module);
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));

#if GTK_CHECK_VERSION(3, 22, 0)
    gtk_menu_popup_at_pointer(darktable.gui->presets_popup_menu, (GdkEvent *)e);
#else
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, e->button, e->time);
#endif

    return TRUE;
  }
  return FALSE;
}

static GdkPixbuf *load_image(const char *filename, int size)
{
  GError *error = NULL;
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) return NULL;

  GdkPixbuf *pixbuf = dt_gdk_pixbuf_new_from_file_at_size(filename, size, size, &error);
  if(!pixbuf)
  {
    fprintf(stderr, "error loading file `%s': %s\n", filename, error->message);
    g_error_free(error);
  }
  return pixbuf;
}

static const uint8_t fallback_pixel[4] = { 0, 0, 0, 0 };

GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module)
{
  int bs = DT_PIXEL_APPLY_DPI(12);
  char tooltip[512];

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *iopw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3 * DT_BAUHAUS_SPACE);

  GtkWidget *expander = dtgtk_expander_new(header, iopw);

  GtkWidget *header_evb = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *body_evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *pluginui_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));

  gtk_widget_set_name(pluginui_frame, "iop-plugin-ui");

  module->header = header;

  /* setup the header box */
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_iop_plugin_header_button_press),
                   module);

  /* connect mouse button callbacks for focus and presets */
  g_signal_connect(G_OBJECT(body_evb), "button-press-event", G_CALLBACK(_iop_plugin_body_button_press),
                   module);

  /*
   * initialize the header widgets
   */
  int idx = 0;
  GtkWidget *hw[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

  /* add the expand indicator icon */
  hw[idx] = dtgtk_icon_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_LEFT, NULL);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);

  /* add duplicate button */
  /*hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_plusminus, CPF_ACTIVE|CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER, NULL);
  module->duplicate_button = GTK_WIDGET(hw[idx]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[idx]), _("add new instance"));
  g_signal_connect (G_OBJECT (hw[idx]), "clicked",
                    G_CALLBACK (dt_iop_gui_duplicate_callback), module);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);*/

  /* add module icon */
  GdkPixbuf *pixbuf;
  cairo_surface_t *surface;
  char filename[PATH_MAX] = { 0 };
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  // make the icons a little more visible
  #define ICON_SIZE (bs * 1.7)

  snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/%s.svg", datadir, module->op);
  pixbuf = load_image(filename, ICON_SIZE);
  if(pixbuf) goto got_image;

  snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/%s.png", datadir, module->op);
  pixbuf = load_image(filename, ICON_SIZE);
  if(pixbuf) goto got_image;

  snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/template.svg", datadir);
  pixbuf = load_image(filename, ICON_SIZE);
  if(pixbuf) goto got_image;

  snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/template.png", datadir);
  pixbuf = load_image(filename, ICON_SIZE);
  if(pixbuf) goto got_image;

  #undef ICON_SIZE

  // wow, we could neither load the SVG nor the PNG files. something is fucked up.
  pixbuf = gdk_pixbuf_new_from_data(fallback_pixel, GDK_COLORSPACE_RGB, TRUE, 8, 1, 1, 4, NULL, NULL);

got_image:
  surface = dt_gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, NULL);
  hw[idx] = gtk_image_new_from_surface(surface);
  gtk_widget_set_margin_start(GTK_WIDGET(hw[idx]), DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);
  cairo_surface_destroy(surface);
  g_object_unref(pixbuf);

  /* add module label */
  hw[idx] = gtk_label_new("");
  _iop_panel_label(hw[idx++], module);

  /* add multi instances menu button */
  if(module->flags() & IOP_FLAGS_ONE_INSTANCE)
  {
    hw[idx] = gtk_fixed_new();
    gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);
  }
  else
  {
    hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    module->multimenu_button = GTK_WIDGET(hw[idx]);
    gtk_widget_set_tooltip_text(GTK_WIDGET(hw[idx]),
                                _("multiple instances actions\nmiddle-click creates new instance"));
    g_signal_connect(G_OBJECT(hw[idx]), "button-press-event", G_CALLBACK(dt_iop_gui_multiinstance_callback),
                     module);
    gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);
  }

  dt_gui_add_help_link(expander, dt_get_help_url(module->op));

  /* add reset button */
  hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  module->reset_button = GTK_WIDGET(hw[idx]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[idx]), _("reset parameters"));
  g_signal_connect(G_OBJECT(hw[idx]), "clicked", G_CALLBACK(dt_iop_gui_reset_callback), module);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);


  /* add preset button if module has implementation */
  hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  module->presets_button = GTK_WIDGET(hw[idx]);
  if (module->flags() & IOP_FLAGS_ONE_INSTANCE)
    gtk_widget_set_tooltip_text(GTK_WIDGET(hw[idx]), _("presets"));
  else
    gtk_widget_set_tooltip_text(GTK_WIDGET(hw[idx]), _("presets\nmiddle-click to apply on new instance"));
  g_signal_connect(G_OBJECT(hw[idx]), "clicked", G_CALLBACK(popup_callback), module);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);

  /* add enabled button spacer */
  hw[idx] = gtk_fixed_new();
  gtk_widget_set_no_show_all(hw[idx], TRUE);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);

  /* add enabled button */
  hw[idx] = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_DO_NOT_USE_BORDER | CPF_BG_TRANSPARENT, NULL);
  gtk_widget_set_no_show_all(hw[idx], TRUE);
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(tooltip, sizeof(tooltip), module->enabled ? _("%s is switched on") : _("%s is switched off"),
           module_label);
  g_free(module_label);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[idx]), tooltip);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hw[idx]), module->enabled);
  g_signal_connect(G_OBJECT(hw[idx]), "toggled", G_CALLBACK(dt_iop_gui_off_callback), module);
  module->off = DTGTK_TOGGLEBUTTON(hw[idx]);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]), bs, bs);

  /* reorder header, for now, iop are always in the right panel */
  for(int i = 7; i >= 0; i--)
    if(hw[i]) gtk_box_pack_start(GTK_BOX(header), hw[i], i == 2 ? TRUE : FALSE, i == 2 ? TRUE : FALSE, 2);
  dt_gui_add_help_link(header, "interacting.html");

  gtk_widget_set_halign(hw[2], GTK_ALIGN_END);
  dtgtk_icon_set_paint(hw[0], dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_LEFT, NULL);

  /* add the blending ui if supported */
  gtk_box_pack_start(GTK_BOX(iopw), module->widget, TRUE, TRUE, 0);
  dt_iop_gui_init_blending(iopw, module);


  /* add empty space around module widget */
  gtk_widget_set_margin_start(iopw, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_end(iopw, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(iopw, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_bottom(iopw, DT_PIXEL_APPLY_DPI(24));

  gtk_widget_hide(iopw);

  module->expander = expander;

  /* update header */
  dt_dev_module_update_multishow(module->dev, module);
  _iop_gui_update_header(module);

  gtk_widget_set_hexpand(module->widget, FALSE);
  gtk_widget_set_vexpand(module->widget, FALSE);


  return module->expander;
}

GtkWidget *dt_iop_gui_get_widget(dt_iop_module_t *module)
{
  return dtgtk_expander_get_body(DTGTK_EXPANDER(module->expander));
}

GtkWidget *dt_iop_gui_get_pluginui(dt_iop_module_t *module)
{
  // return gtkframe (pluginui_frame)
  return dtgtk_expander_get_frame(DTGTK_EXPANDER(module->expander));
}

int dt_iop_breakpoint(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe)
{
  if(pipe != dev->preview_pipe) sched_yield();
  if(pipe != dev->preview_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
  if((pipe->changed != DT_DEV_PIPE_UNCHANGED && pipe->changed != DT_DEV_PIPE_ZOOMED) || dev->gui_leaving)
    return 1;
  return 0;
}

void dt_iop_nap(int32_t usec)
{
  if(usec <= 0) return;

  // relinquish processor
  sched_yield();

  // additionally wait the given amount of time
  g_usleep(usec);
}

dt_iop_module_t *get_colorout_module()
{
  GList *modules = darktable.develop->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(!strcmp(module->op, "colorout")) return module;
    modules = g_list_next(modules);
  }
  return NULL;
}

static gboolean show_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)

{
  dt_iop_module_t *module = (dt_iop_module_t *)data;

  // Showing the module, if it isn't already visible
  if(module->so->state == dt_iop_state_HIDDEN)
  {
    dt_iop_gui_set_state(module, dt_iop_state_ACTIVE);
  }

  uint32_t current_group = dt_dev_modulegroups_get(module->dev);
  if(!dt_iop_shown_in_group(module, current_group))
  {
    dt_dev_modulegroups_switch(darktable.develop, module);
  }

  dt_iop_gui_set_expanded(module, TRUE, dt_conf_get_bool("darkroom/ui/single_module"));
  dt_iop_request_focus(module);
  return TRUE;
}

static gboolean enable_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                       GdkModifierType modifier, gpointer data)

{
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(module->off));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), !active);
  return TRUE;
}



void dt_iop_connect_common_accels(dt_iop_module_t *module)
{

  GClosure *closure = NULL;
  if(module->flags() & IOP_FLAGS_DEPRECATED) return;
  // Connecting the (optional) module show accelerator
  closure = g_cclosure_new(G_CALLBACK(show_module_callback), module, NULL);
  dt_accel_connect_iop(module, "show module", closure);

  // Connecting the (optional) module switch accelerator
  closure = g_cclosure_new(G_CALLBACK(enable_module_callback), module, NULL);
  dt_accel_connect_iop(module, "enable module", closure);

  // Connecting the reset and preset buttons
  if(module->reset_button)
    dt_accel_connect_button_iop(module, "reset module parameters", module->reset_button);
  if(module->presets_button) dt_accel_connect_button_iop(module, "show preset menu", module->presets_button);

  if(module->fusion_slider) dt_accel_connect_slider_iop(module, "fusion", module->fusion_slider);

  sqlite3_stmt *stmt;
  // don't know for which image. show all we got:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name FROM data.presets WHERE operation=?1 ORDER BY writeprotect DESC, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_accel_connect_preset_iop(module, (char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
}

gchar *dt_iop_get_localized_name(const gchar *op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(module_names == NULL)
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    GList *iop = g_list_first(darktable.iop);
    if(iop != NULL)
    {
      do
      {
        dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
        g_hash_table_insert(module_names, module->op, g_strdup(module->name()));
      } while((iop = g_list_next(iop)) != NULL);
    }
  }

  return (gchar *)g_hash_table_lookup(module_names, op);
}

void dt_iop_so_gui_set_state(dt_iop_module_so_t *module, dt_iop_module_state_t state)
{
  module->state = state;

  char option[1024];
  GList *mods = NULL;
  if(state == dt_iop_state_HIDDEN)
  {
    mods = g_list_first(darktable.develop->iop);
    while(mods)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)mods->data;
      if(mod->so == module && mod->expander) gtk_widget_hide(GTK_WIDGET(mod->expander));
      mods = g_list_next(mods);
    }

    snprintf(option, sizeof(option), "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool(option, FALSE);
    snprintf(option, sizeof(option), "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool(option, FALSE);
  }
  else if(state == dt_iop_state_ACTIVE)
  {
    int once = 0;

    mods = g_list_first(darktable.develop->iop);
    while(mods)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)mods->data;
      if(mod->so == module && mod->expander)
      {
        gtk_widget_show(GTK_WIDGET(mod->expander));
        if(!once)
        {
          dt_dev_modulegroups_switch(darktable.develop, mod);
          once = 1;
        }
      }
      mods = g_list_next(mods);
    }

    /* module is shown lets set conf values */
    snprintf(option, sizeof(option), "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool(option, TRUE);
    snprintf(option, sizeof(option), "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool(option, FALSE);
  }
  else if(state == dt_iop_state_FAVORITE)
  {
    mods = g_list_first(darktable.develop->iop);
    while(mods)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)mods->data;
      if(mod->so == module && mod->expander) gtk_widget_show(GTK_WIDGET(mod->expander));
      mods = g_list_next(mods);
    }

    /* module is shown and favorite lets set conf values */
    snprintf(option, sizeof(option), "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool(option, TRUE);
    snprintf(option, sizeof(option), "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool(option, TRUE);
  }

  dt_view_manager_t *vm = darktable.view_manager;
  if(vm->proxy.more_module.module) vm->proxy.more_module.update(vm->proxy.more_module.module);
  // dt_view_manager_reset(vm);
}

void dt_iop_gui_set_state(dt_iop_module_t *module, dt_iop_module_state_t state)
{
  dt_iop_so_gui_set_state(module->so, state);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
