/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#include "common/exif.h"
#include "common/dtpthread.h"
#include "common/imageio_rawspeed.h"
#include "common/interpolation.h"
#include "common/iop_group.h"
#include "common/module.h"
#include "common/history.h"
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
#include "gui/color_picker_proxy.h"
#include "libs/modulegroups.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <complex.h>
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
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
        { 0 }, 0, 0, FALSE };

static void _iop_panel_label(GtkWidget *lab, dt_iop_module_t *module);

void dt_iop_load_default_params(dt_iop_module_t *module)
{
  memcpy(module->params, module->default_params, module->params_size);
  memcpy(module->default_blendop_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  dt_iop_commit_blend_params(module, &_default_blendop_params);
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

/* default group for modules which do not implement the default_group() function */
static int default_group(void)
{
  return IOP_GROUP_BASIC;
}

/* default flags for modules which does not implement the flags() function */
static int default_flags(void)
{
  return 0;
}

/* default operation tags for modules which does not implement the flags() function */
static int default_operation_tags(void)
{
  return 0;
}

/* default operation tags filter for modules which does not implement the flags() function */
static int default_operation_tags_filter(void)
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
  IOP_GUI_FREE;
}

static void default_cleanup(dt_iop_module_t *module)
{
  g_free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
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

static dt_introspection_field_t *default_get_introspection_linear(void)
{
  return NULL;
}
static dt_introspection_t *default_get_introspection(void)
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

void dt_iop_default_init(dt_iop_module_t *module)
{
  size_t param_size = module->so->get_introspection()->size;
  module->params_size = param_size;
  module->params = (dt_iop_params_t *)malloc(param_size);
  module->default_params = (dt_iop_params_t *)malloc(param_size);

  module->default_enabled = 0;
  module->gui_data = NULL;

  dt_introspection_field_t *i = module->so->get_introspection_linear();
  while(i->header.type != DT_INTROSPECTION_TYPE_NONE)
  {
    switch(i->header.type)
    {
    case DT_INTROSPECTION_TYPE_FLOAT:
      *(float*)(module->default_params + i->header.offset) = i->Float.Default;
      break;
    case DT_INTROSPECTION_TYPE_INT:
      *(int*)(module->default_params + i->header.offset) = i->Int.Default;
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      *(unsigned int*)(module->default_params + i->header.offset) = i->UInt.Default;
      break;
    case DT_INTROSPECTION_TYPE_USHORT:
      *(unsigned short*)(module->default_params + i->header.offset) = i->UShort.Default;
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
      *(int*)(module->default_params + i->header.offset) = i->Enum.Default;
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean*)(module->default_params + i->header.offset) = i->Bool.Default;
      break;
    case DT_INTROSPECTION_TYPE_CHAR:
      *(char*)(module->default_params + i->header.offset) = i->Char.Default;
      break;
    case DT_INTROSPECTION_TYPE_OPAQUE:
      memset(module->default_params + i->header.offset, 0, i->header.size);
      break;
    case DT_INTROSPECTION_TYPE_ARRAY:
      {
        if(i->Array.type == DT_INTROSPECTION_TYPE_CHAR) break;

        size_t element_size = i->Array.field->header.size;
        if(element_size % sizeof(int))
        {
          int8_t *p = module->default_params + i->header.offset;
          for (size_t c = element_size; c < i->header.size; c++, p++)
            p[element_size] = *p;
        }
        else
        {
          element_size /= sizeof(int);
          size_t num_ints = i->header.size / sizeof(int);

          int *p = module->default_params + i->header.offset;
          for (size_t c = element_size; c < num_ints; c++, p++)
            p[element_size] = *p;
        }
      }
      break;
    case DT_INTROSPECTION_TYPE_STRUCT:
      // ignore STRUCT; nothing to do
      break;
    default:
      fprintf(stderr, "unsupported introspection type \"%s\" encountered in dt_iop_default_init (field %s)\n", i->header.type_name, i->header.field_name);
      break;
    }

    i++;
  }
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
  if(!g_module_symbol(module->module, "default_group", (gpointer) & (module->default_group)))
    module->default_group = default_group;
  if(!g_module_symbol(module->module, "flags", (gpointer) & (module->flags))) module->flags = default_flags;
  if(!g_module_symbol(module->module, "description", (gpointer) & (module->description))) module->description = module->name;
  if(!g_module_symbol(module->module, "operation_tags", (gpointer) & (module->operation_tags)))
    module->operation_tags = default_operation_tags;
  if(!g_module_symbol(module->module, "operation_tags_filter", (gpointer) & (module->operation_tags_filter)))
    module->operation_tags_filter = default_operation_tags_filter;
  if(!g_module_symbol(module->module, "input_format", (gpointer) & (module->input_format)))
    module->input_format = default_input_format;
  if(!g_module_symbol(module->module, "output_format", (gpointer) & (module->output_format)))
    module->output_format = default_output_format;

  if(!g_module_symbol(module->module, "default_colorspace", (gpointer) & (module->default_colorspace))) goto error;
  if(!g_module_symbol(module->module, "input_colorspace", (gpointer) & (module->input_colorspace)))
    module->input_colorspace = default_input_colorspace;
  if(!g_module_symbol(module->module, "output_colorspace", (gpointer) & (module->output_colorspace)))
    module->output_colorspace = default_output_colorspace;
  if(!g_module_symbol(module->module, "blend_colorspace", (gpointer) & (module->blend_colorspace)))
    module->blend_colorspace = default_blend_colorspace;

  if(!g_module_symbol(module->module, "tiling_callback", (gpointer) & (module->tiling_callback)))
    module->tiling_callback = default_tiling_callback;
  if(!g_module_symbol(module->module, "gui_reset", (gpointer) & (module->gui_reset)))
    module->gui_reset = NULL;
  if(!g_module_symbol(module->module, "gui_init", (gpointer) & (module->gui_init))) module->gui_init = NULL;
  if(!g_module_symbol(module->module, "gui_update", (gpointer) & (module->gui_update)))
    module->gui_update = NULL;
  if(!g_module_symbol(module->module, "color_picker_apply", (gpointer) & (module->color_picker_apply)))
    module->color_picker_apply = NULL;
  if(!g_module_symbol(module->module, "gui_changed", (gpointer) & (module->gui_changed)))
    module->gui_changed = NULL;
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
  if(!g_module_symbol(module->module, "mouse_actions", (gpointer) & (module->mouse_actions)))
    module->mouse_actions = NULL;
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

  if(!g_module_symbol(module->module, "init", (gpointer) & (module->init)))
    module->init = dt_iop_default_init;
  if(!g_module_symbol(module->module, "cleanup", (gpointer) & (module->cleanup)))
    module->cleanup = default_cleanup;
  if(!g_module_symbol(module->module, "init_global", (gpointer) & (module->init_global)))
    module->init_global = NULL;
  if(!g_module_symbol(module->module, "cleanup_global", (gpointer) & (module->cleanup_global)))
    module->cleanup_global = NULL;
  if(!g_module_symbol(module->module, "init_presets", (gpointer) & (module->init_presets)))
    module->init_presets = NULL;
  if(!g_module_symbol(module->module, "commit_params", (gpointer) & (module->commit_params)))
    module->commit_params = default_commit_params;
  if(!g_module_symbol(module->module, "change_image", (gpointer) & (module->change_image)))
    module->change_image = NULL;
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
  if(!g_module_symbol(module->module, "distort_mask", (gpointer) & (module->distort_mask)))
    module->distort_mask = NULL;

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
  if(module->introspection_init)
  {
    if(!module->introspection_init(module, DT_INTROSPECTION_VERSION))
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
    else
      fprintf(stderr, "[iop_load_module] failed to initialize introspection for operation `%s'\n", op);
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
  module->dev = dev;
  module->widget = NULL;
  module->header = NULL;
  module->off = NULL;
  module->hide_enable_button = 0;
  module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  module->request_histogram = DT_REQUEST_ONLY_IN_GUI;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
  module->multi_priority = 0;
  module->iop_order = 0;
  for(int k = 0; k < 3; k++)
  {
    module->picked_color[k] = module->picked_output_color[k] = 0.0f;
    module->picked_color_min[k] = module->picked_output_color_min[k] = 666.0f;
    module->picked_color_max[k] = module->picked_output_color_max[k] = -666.0f;
  }
  module->picker = NULL;
  module->histogram_cst = iop_cs_NONE;
  module->color_picker_box[0] = module->color_picker_box[1] = .25f;
  module->color_picker_box[2] = module->color_picker_box[3] = .75f;
  module->color_picker_point[0] = module->color_picker_point[1] = 0.5f;
  module->histogram = NULL;
  module->histogram_max[0] = module->histogram_max[1] = module->histogram_max[2] = module->histogram_max[3]
      = 0;
  module->histogram_middle_grey = FALSE;
  module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  module->suppress_mask = 0;
  module->enabled = module->default_enabled = 0; // all modules disabled by default.
  g_strlcpy(module->op, so->op, 20);
  module->raster_mask.source.users = g_hash_table_new(NULL, NULL);
  module->raster_mask.source.masks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = 0;

  // only reference cached results of dlopen:
  module->module = so->module;
  module->so = so;

  module->version = so->version;
  module->name = so->name;
  module->default_group = so->default_group;
  module->flags = so->flags;
  module->description = so->description;
  module->operation_tags = so->operation_tags;
  module->operation_tags_filter = so->operation_tags_filter;
  module->input_format = so->input_format;
  module->output_format = so->output_format;
  module->default_colorspace = so->default_colorspace;
  module->input_colorspace = so->input_colorspace;
  module->output_colorspace = so->output_colorspace;
  module->blend_colorspace = so->blend_colorspace;
  module->tiling_callback = so->tiling_callback;
  module->gui_update = so->gui_update;
  module->gui_reset = so->gui_reset;
  module->gui_init = so->gui_init;
  module->color_picker_apply = so->color_picker_apply;
  module->gui_changed = so->gui_changed;
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
  module->change_image = so->change_image;
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
  module->distort_mask = so->distort_mask;
  module->modify_roi_in = so->modify_roi_in;
  module->modify_roi_out = so->modify_roi_out;
  module->legacy_params = so->legacy_params;
  // allow to select a shape inside an iop
  module->masks_selection_changed = so->masks_selection_changed;

  module->connect_key_accels = so->connect_key_accels;
  module->disconnect_key_accels = so->disconnect_key_accels;
  module->mouse_actions = so->mouse_actions;

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

  module->global_data = so->data;

  // now init the instance:
  module->init(module);

  /* initialize blendop params and default values */
  module->blend_params = calloc(1, sizeof(dt_develop_blend_params_t));
  module->default_blendop_params = calloc(1, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  dt_iop_commit_blend_params(module, &_default_blendop_params);

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

static gboolean _header_motion_notify_show_callback(GtkWidget *eventbox, GdkEventCrossing *event, GtkWidget *header)
{
  return dt_iop_show_hide_header_buttons(header, event, TRUE, FALSE);
}

static gboolean _header_motion_notify_hide_callback(GtkWidget *eventbox, GdkEventCrossing *event, GtkWidget *header)
{
  return dt_iop_show_hide_header_buttons(header, event, FALSE, FALSE);
}

static gboolean _header_menu_deactivate_callback(GtkMenuShell *menushell, GtkWidget *header)
{
  return dt_iop_show_hide_header_buttons(header, NULL, FALSE, FALSE);
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

  if(dev->gui_attached)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,
                            dt_history_duplicate(darktable.develop->history), darktable.develop->history_end,
                            dt_ioppr_iop_order_copy_deep(darktable.develop->iop_order_list));

  // we must pay attention if priority is 0
  gboolean is_zero = (module->multi_priority == 0);

  // we set the focus to the other instance
  dt_iop_gui_set_expanded(next, TRUE, FALSE);
  dt_iop_request_focus(next);

  ++darktable.gui->reset;

  // we remove the plugin effectively
  if(!dt_iop_is_hidden(module))
  {
    // we just hide the module to avoid lots of gtk critical warnings
    gtk_widget_hide(module->expander);

    // we move the module far away, to avoid problems when reordering instance after that
    // FIXME: ?????
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                          module->expander, -1);

    dt_iop_gui_cleanup_module(module);
    gtk_widget_destroy(module->widget);
  }

  // we remove all references in the history stack and dev->iop
  // this will inform that a module has been removed from history
  // we do it here so we have the multi_priorities to reconstruct
  // de deleted module if the user undo it
  dt_dev_module_remove(dev, module);

  // if module was priority 0, then we set next to priority 0
  if(is_zero)
  {
    // we want the first one in history
    dt_iop_module_t *first = NULL;
    GList *history = g_list_first(dev->history);
    while(history)
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      if(hist->module->instance == module->instance && hist->module != module)
      {
        first = hist->module;
        break;
      }
      history = g_list_next(history);
    }
    if(first == NULL) first = next;

    // we set priority of first to 0
    dt_iop_update_multi_priority(first, 0);

    // we change this in the history stack too
    history = g_list_first(dev->history);
    while(history)
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      if(hist->module == first) hist->multi_priority = 0;
      history = g_list_next(history);
    }
  }

  // we save the current state of history (with the new multi_priorities)
  if(dev->gui_attached)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  // rebuild the accelerators (to point to an extant module)
  dt_iop_connect_accels_multi(module->so);

  dt_accel_cleanup_closures_iop(module);

  // don't delete the module, a pipe may still need it
  dev->alliop = g_list_append(dev->alliop, module);

  // we update show params for multi-instances for each other instances
  dt_dev_modules_update_multishow(dev);

  // we refresh the pipe
  dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->pipe->cache_obsolete = 1;
  dev->preview_pipe->cache_obsolete = 1;
  dev->preview2_pipe->cache_obsolete = 1;

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(dev);

  /* redraw */
  dt_control_queue_redraw_center();

  --darktable.gui->reset;
}

dt_iop_module_t *dt_iop_gui_get_previous_visible_module(dt_iop_module_t *module)
{
  dt_iop_module_t *prev = NULL;
  GList *modules = g_list_first(module->dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      break;
    }
    else
    {
      // only for visible modules
      GtkWidget *expander = mod->expander;
      if(expander && gtk_widget_is_visible(expander))
      {
        prev = mod;
      }
    }
    modules = g_list_next(modules);
  }
  return prev;
}

dt_iop_module_t *dt_iop_gui_get_next_visible_module(dt_iop_module_t *module)
{
  dt_iop_module_t *next = NULL;
  GList *modules = g_list_last(module->dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      break;
    }
    else
    {
      // only for visible modules
      GtkWidget *expander = mod->expander;
      if(expander && gtk_widget_is_visible(expander))
      {
        next = mod;
      }
    }
    modules = g_list_previous(modules);
  }
  return next;
}

static void dt_iop_gui_movedown_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_movedown_callback begin");

  // we need to place this module right before the previous
  dt_iop_module_t *prev = dt_iop_gui_get_previous_visible_module(module);
  // dt_ioppr_check_iop_order(module->dev, "dt_iop_gui_movedown_callback 1");
  if(!prev) return;

  const int moved = dt_ioppr_move_iop_before(module->dev, module, prev);
  // dt_ioppr_check_iop_order(module->dev, "dt_iop_gui_movedown_callback 2");
  if(!moved) return;

  // we move the headers
  GValue gv = { 0, { { 0 } } };
  g_value_init(&gv, G_TYPE_INT);
  gtk_container_child_get_property(
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)), prev->expander,
      "position", &gv);
  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv));

  // we update the headers
  dt_dev_modules_update_multishow(prev->dev);

  dt_dev_add_history_item(prev->dev, module, TRUE);

  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_movedown_callback end");

  // we rebuild the pipe
  prev->dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  prev->dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  prev->dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
  prev->dev->pipe->cache_obsolete = 1;
  prev->dev->preview_pipe->cache_obsolete = 1;
  prev->dev->preview2_pipe->cache_obsolete = 1;

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED);

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(prev->dev);
}

static void dt_iop_gui_moveup_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_moveup_callback begin");

  // we need to place this module right after the next one
  dt_iop_module_t *next = dt_iop_gui_get_next_visible_module(module);
  if(!next) return;

  const int moved = dt_ioppr_move_iop_after(module->dev, module, next);
  if(!moved) return;

  // we move the headers
  GValue gv = { 0, { { 0 } } };
  g_value_init(&gv, G_TYPE_INT);
  gtk_container_child_get_property(
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)), next->expander,
      "position", &gv);

  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv));

  // we update the headers
  dt_dev_modules_update_multishow(next->dev);

  dt_dev_add_history_item(next->dev, module, TRUE);

  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_moveup_callback end");

  // we rebuild the pipe
  next->dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  next->dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  next->dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
  next->dev->pipe->cache_obsolete = 1;
  next->dev->preview_pipe->cache_obsolete = 1;
  next->dev->preview2_pipe->cache_obsolete = 1;

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED);

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(next->dev);
}

dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base, gboolean copy_params)
{
  // make sure the duplicated module appears in the history
  dt_dev_add_history_item(base->dev, base, FALSE);

  // first we create the new module
  ++darktable.gui->reset;
  dt_iop_module_t *module = dt_dev_module_duplicate(base->dev, base);
  --darktable.gui->reset;
  if(!module) return NULL;

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
  /* initialize gui if iop have one defined */
  if(!dt_iop_is_hidden(module))
  {
    // make sure gui_init and reload defaults is called safely
    dt_iop_gui_init(module);

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

    dt_iop_reload_defaults(module); // some modules like profiled denoise update the gui in reload_defaults

    if(copy_params)
    {
      memcpy(module->params, base->params, module->params_size);
      if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      {
        dt_iop_commit_blend_params(module, base->blend_params);
        if(base->blend_params->mask_id > 0)
        {
          module->blend_params->mask_id = 0;
          dt_masks_iop_use_same_as(module, base);
        }
      }
    }

    // we save the new instance creation
    dt_dev_add_history_item(module->dev, module, TRUE);

    dt_iop_gui_update_blending(module);
  }

  if(dt_conf_get_bool("darkroom/ui/single_module"))
  {
    dt_iop_gui_set_expanded(base, FALSE, TRUE);
    dt_iop_gui_set_expanded(module, TRUE, TRUE);
  }

  // we update show params for multi-instances for each other instances
  dt_dev_modules_update_multishow(module->dev);

  // and we refresh the pipe
  dt_iop_request_focus(module);

  if(module->dev->gui_attached)
  {
    module->dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
    module->dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
    module->dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
    module->dev->pipe->cache_obsolete = 1;
    module->dev->preview_pipe->cache_obsolete = 1;
    module->dev->preview2_pipe->cache_obsolete = 1;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(module->dev);
  }

  /* update ui to new parameters */
  dt_iop_gui_update(module);

  dt_dev_modulegroups_update_visibility(darktable.develop);

  return module;
}

static void dt_iop_gui_copy_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_gui_duplicate(user_data, FALSE);

  /* setup key accelerators */
  dt_iop_connect_accels_multi(((dt_iop_module_t *)user_data)->so);
}

static void dt_iop_gui_duplicate_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_gui_duplicate(user_data, TRUE);

  /* setup key accelerators */
  dt_iop_connect_accels_multi(((dt_iop_module_t *)user_data)->so);
}

static gboolean _rename_module_key_press(GtkWidget *entry, GdkEventKey *event, dt_iop_module_t *module)
{
  int ended = 0;

  if(event->type == GDK_FOCUS_CHANGE || event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
  {
    const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
    if(strcmp(module->multi_name, name) != 0)
    {
      g_strlcpy(module->multi_name, name, sizeof(module->multi_name));
      dt_dev_add_history_item(module->dev, module, TRUE);
    }

    ended = 1;
  }
  else if(event->keyval == GDK_KEY_Escape)
  {
    // restore saved 1st character of instance name
    module->multi_name[0] = module->multi_name[sizeof(module->multi_name) - 1];
    module->multi_name[sizeof(module->multi_name) - 1] = 0;

    ended = 1;
  }

  if(ended)
  {
    g_signal_handlers_disconnect_by_func(entry, G_CALLBACK(_rename_module_key_press), module);
    gtk_widget_destroy(entry);
    dt_iop_show_hide_header_buttons(module->header, NULL, TRUE, FALSE); // after removing entry
    dt_iop_gui_update_header(module);

    return TRUE;
  }

  return FALSE; /* event not handled */
}

static gboolean _rename_module_resize(GtkWidget *entry, GdkEventKey *event, dt_iop_module_t *module)
{
  int width = 0;
  GtkBorder padding;

  pango_layout_get_pixel_size(gtk_entry_get_layout(GTK_ENTRY(entry)), &width, NULL);
  gtk_style_context_get_padding(gtk_widget_get_style_context (entry),
                                gtk_widget_get_state_flags (entry),
                                &padding);
  gtk_widget_set_size_request(entry, width + padding.left + padding.right + 1, -1);

  return TRUE;
}

static void _iop_gui_rename_module(dt_iop_module_t *module)
{
  if(gtk_container_get_focus_child(GTK_CONTAINER(module->header))) return;

  GtkWidget *entry = gtk_entry_new();

  gtk_widget_set_name(entry, "iop-panel-label");
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 0);
  gtk_entry_set_max_length(GTK_ENTRY(entry), sizeof(module->multi_name) - 1);
  gtk_entry_set_text(GTK_ENTRY(entry), module->multi_name);

  // remove instance name but save 1st character in case of escape
  module->multi_name[sizeof(module->multi_name) - 1] = module->multi_name[0];
  module->multi_name[0] = 0;
  dt_iop_gui_update_header(module);

  dt_gui_key_accel_block_on_focus_connect(entry); // needs to be before focus-out-event
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "focus-out-event", G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "style-updated", G_CALLBACK(_rename_module_resize), module);
  g_signal_connect(entry, "changed", G_CALLBACK(_rename_module_resize), module);

  dt_iop_show_hide_header_buttons(module->header, NULL, FALSE, TRUE); // before adding entry
  gtk_box_pack_start(GTK_BOX(module->header), entry, TRUE, TRUE, 0);
  gtk_widget_show(entry);
  gtk_widget_grab_focus(entry);
}

static void dt_iop_gui_rename_callback(GtkButton *button, dt_iop_module_t *module)
{
  _iop_gui_rename_module(module);
}

static void dt_iop_gui_multiinstance_callback(GtkButton *button, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if(event->button == 2)
  {
    if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE)) dt_iop_gui_copy_callback(button, user_data);
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
  gtk_widget_set_sensitive(item, module->multi_show_new);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("duplicate instance"));
  // gtk_widget_set_tooltip_text(item, _("add a copy of this instance to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(dt_iop_gui_duplicate_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_new);
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

  g_signal_connect(G_OBJECT(menu), "deactivate", G_CALLBACK(_header_menu_deactivate_callback), module->header);

  // popup
#if GTK_CHECK_VERSION(3, 22, 0)
  gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
#else
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, gtk_get_current_event_time());
#endif

  // make sure the button is deactivated now that the menu is opened
  dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
}

static gboolean dt_iop_gui_off_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset && e->state & GDK_CONTROL_MASK)
  {
    dt_iop_request_focus(darktable.develop->gui_module == module ? NULL : module);
    return TRUE;
  }
  return FALSE;
}

static void dt_iop_gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset)
  {
    if(gtk_toggle_button_get_active(togglebutton))
    {
      module->enabled = 1;

      if(dt_conf_get_bool("darkroom/ui/scroll_to_module"))
        darktable.gui->scroll_to[1] = module->expander;

      if(dt_conf_get_bool("darkroom/ui/activate_expand") && !module->expanded)
        dt_iop_gui_set_expanded(module, TRUE, dt_conf_get_bool("darkroom/ui/single_module"));
    }
    else
    {
      module->enabled = 0;

      if(dt_conf_get_bool("darkroom/ui/activate_expand") && module->expanded)
        dt_iop_gui_set_expanded(module, FALSE, FALSE);
    }
    dt_dev_add_history_item(module->dev, module, FALSE);
  }
  char tooltip[512];
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(tooltip, sizeof(tooltip), module->enabled ? _("%s is switched on") : _("%s is switched off"),
           module_label);
  g_free(module_label);
  gtk_widget_set_tooltip_text(GTK_WIDGET(togglebutton), tooltip);
  gtk_widget_queue_draw(GTK_WIDGET(togglebutton));

  if(dt_conf_get_bool("accel/prefer_enabled"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(module->so);
  }

  if(module->enabled && !gtk_widget_is_visible(module->header))
    dt_dev_modulegroups_update_visibility(darktable.develop);
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
  if(group == DT_MODULEGROUP_NONE) return TRUE;

  return dt_dev_modulegroups_test(module->dev, group, module);
}

static void _iop_panel_label(GtkWidget *lab, dt_iop_module_t *module)
{
  gtk_widget_set_name(lab, "iop-panel-label");
  gchar *label = dt_history_item_get_name_html(module);
  gchar *tooltip = g_strdup(module->description());
  gtk_label_set_markup(GTK_LABEL(lab), label);
  gtk_label_set_ellipsize(GTK_LABEL(lab), !module->multi_name[0] ? PANGO_ELLIPSIZE_END: PANGO_ELLIPSIZE_MIDDLE);
  g_object_set(G_OBJECT(lab), "xalign", 0.0, (gchar *)0);
  gtk_widget_set_tooltip_text(lab, tooltip);
  g_free(label);
  g_free(tooltip);
}

static void _iop_gui_update_header(dt_iop_module_t *module)
{
  GList *childs = gtk_container_get_children(GTK_CONTAINER(module->header));

  /* get the enable button and button */
  GtkWidget *lab = g_list_nth_data(childs, IOP_MODULE_LABEL);

  g_list_free(childs);

  // set panel name to display correct multi-instance
  _iop_panel_label(lab, module);
  dt_iop_gui_set_enable_button(module);
}

void dt_iop_gui_set_enable_button_icon(GtkWidget *w, dt_iop_module_t *module)
{
  // set on/off icon
  if(module->default_enabled && module->hide_enable_button)
  {
    gtk_widget_set_name(w, "module-always-enabled-button");
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w),
                                 dtgtk_cairo_paint_switch_on, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, module);
  }
  else if(!module->default_enabled && module->hide_enable_button)
  {
    gtk_widget_set_name(w, "module-always-disabled-button");
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w),
                                 dtgtk_cairo_paint_switch_off, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, module);
  }
  else
  {
    gtk_widget_set_name(w, "module-enable-button");
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w),
                                 dtgtk_cairo_paint_switch, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, module);
  }
}

void dt_iop_gui_set_enable_button(dt_iop_module_t *module)
{
  if(module->off)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
    if(module->hide_enable_button)
      gtk_widget_set_sensitive(GTK_WIDGET(module->off), FALSE);
    else
      gtk_widget_set_sensitive(GTK_WIDGET(module->off), TRUE);

    dt_iop_gui_set_enable_button_icon(GTK_WIDGET(module->off), module);
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
  GtkWidget *lab = g_list_nth_data(childs, IOP_MODULE_LABEL);
  g_list_free(childs);
  _iop_panel_label(lab, module);
}

void dt_iop_gui_init(dt_iop_module_t *module)
{
  ++darktable.gui->reset;
  --darktable.bauhaus->skip_accel;
  if(module->gui_init) module->gui_init(module);
  ++darktable.bauhaus->skip_accel;
  --darktable.gui->reset;
}

void dt_iop_reload_defaults(dt_iop_module_t *module)
{
  if(darktable.gui) ++darktable.gui->reset;
  if(module->reload_defaults) module->reload_defaults(module);
  if(darktable.gui) --darktable.gui->reset;

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
      // we need a dt_iop_module_t for legacy_params()
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        free(module);
        continue;
      }
/*
      module->init(module);
      if(module->params_size == 0)
      {
        dt_iop_cleanup_module(module);
        free(module);
        continue;
      }
      // we call reload_defaults() in case the module defines it
      if(module->reload_defaults) module->reload_defaults(module); // why not call dt_iop_reload_defaults? (if needed at all)
*/

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

      fprintf(stderr, "[imageop_init_presets] updating '%s' preset '%s' from version %d to version %d\nto:'%s'",
              module_so->op, name, old_params_version, module_version,
              dt_exif_xmp_encode(new_params, new_params_size, NULL));

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
    snprintf(path, sizeof(path), "%s`%s", N_("preset"), (const char *)sqlite3_column_text(stmt, 0));
    dt_accel_register_iop(module, FALSE, path, 0, 0);
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

    // create a gui and have the widgets register their accelerators
    dt_iop_module_t *module_instance = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));

    if(module->gui_init && !dt_iop_load_module_by_so(module_instance, module, NULL))
    {
      darktable.control->accel_initialising = TRUE;
      dt_iop_gui_init(module_instance);
      module->gui_cleanup(module_instance);
      darktable.control->accel_initialising = FALSE;

      dt_iop_cleanup_module(module_instance);
    }

    free(module_instance);

    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      dt_accel_register_slider_iop(module, FALSE, NC_("accel","fusion") ? "accel|fusion" : "");
    }
    if(!(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      dt_accel_register_common_iop(module);
    }
  }
}

void dt_iop_load_modules_so(void)
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
    res = g_list_insert_sorted(res, module, dt_sort_iop_by_order);
    module->global_data = module_so->data;
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

  free(module->blend_params);
  module->blend_params = NULL;
  free(module->default_blendop_params);
  module->default_blendop_params = NULL;
  module->picker = NULL;
  free(module->histogram);
  module->histogram = NULL;
  g_hash_table_destroy(module->raster_mask.source.users);
  g_hash_table_destroy(module->raster_mask.source.masks);
  module->raster_mask.source.users = NULL;
  module->raster_mask.source.masks = NULL;
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

void dt_iop_set_mask_mode(dt_iop_module_t *module, int mask_mode)
{
  static const int key = 0;
  // showing raster masks doesn't make sense, one can use the original source instead. or does it?
  if(mask_mode & DEVELOP_MASK_ENABLED && !(mask_mode & DEVELOP_MASK_RASTER))
  {
    char *modulename = dt_history_item_get_name(module);
    g_hash_table_insert(module->raster_mask.source.masks, GINT_TO_POINTER(key), modulename);
  }
  else
  {
    g_hash_table_remove(module->raster_mask.source.masks, GINT_TO_POINTER(key));
  }
}

// make sure that blend_params are in sync with the iop struct
void dt_iop_commit_blend_params(dt_iop_module_t *module, const dt_develop_blend_params_t *blendop_params)
{
  if(module->raster_mask.sink.source)
    g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module);

  memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
  dt_iop_set_mask_mode(module, blendop_params->mask_mode);

  if(module->dev)
  {
    for(GList *iter = module->dev->iop; iter; iter = g_list_next(iter))
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iter->data;
      if(!strcmp(m->op, blendop_params->raster_mask_source))
      {
        if(m->multi_priority == blendop_params->raster_mask_instance)
        {
          g_hash_table_insert(m->raster_mask.source.users, module, GINT_TO_POINTER(blendop_params->raster_mask_id));
          module->raster_mask.sink.source = m;
          module->raster_mask.sink.id = blendop_params->raster_mask_id;
          return;
        }
      }
    }
  }

  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = 0;
}

gboolean _iop_validate_params(dt_introspection_field_t *field, dt_iop_params_t *params, gboolean report)
{
  dt_iop_params_t *p = params + field->header.offset;

  gboolean all_ok = TRUE;

  switch(field->header.type)
  {
  case DT_INTROSPECTION_TYPE_STRUCT:
    for(int i = 0; i < field->Struct.entries; i++)
    {
      dt_introspection_field_t *entry = field->Struct.fields[i];

      all_ok &= _iop_validate_params(entry, params, report);
    }
    break;
  case DT_INTROSPECTION_TYPE_UNION:
    all_ok = FALSE;
    for(int i = field->Union.entries - 1; i >= 0 ; i--)
    {
      dt_introspection_field_t *entry = field->Union.fields[i];

      if(_iop_validate_params(entry, params, report && i == 0))
      {
        all_ok = TRUE;
        break;
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_ARRAY:
    if(field->Array.type == DT_INTROSPECTION_TYPE_CHAR)
    {
      if(!memchr(p, '\0', field->Array.count))
      {
        if(report)
          fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\"; string not null terminated.\n",
                          field->header.type_name);
        all_ok = FALSE;
      }
    }
    else
    {
      for(int i = 0, item_offset = 0; i < field->Array.count; i++, item_offset += field->Array.field->header.size)
      {
        if(!_iop_validate_params(field->Array.field, params + item_offset, report))
        {
          if(report)
            fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\", for array element \"%d\"\n",
                            field->header.type_name, i);
          all_ok = FALSE;
          break;
        }
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_FLOAT:
    all_ok = isnan(*(float*)p) || ((*(float*)p >= field->Float.Min && *(float*)p <= field->Float.Max));
    break;
  case DT_INTROSPECTION_TYPE_INT:
    all_ok = (*(int*)p >= field->Int.Min && *(int*)p <= field->Int.Max);
    break;
  case DT_INTROSPECTION_TYPE_UINT:
    all_ok = (*(unsigned int*)p >= field->UInt.Min && *(unsigned int*)p <= field->UInt.Max);
    break;
  case DT_INTROSPECTION_TYPE_USHORT:
    all_ok = (*(unsigned short int*)p >= field->UShort.Min && *(unsigned short int*)p <= field->UShort.Max);
    break;
  case DT_INTROSPECTION_TYPE_INT8:
    all_ok = (*(uint8_t*)p >= field->Int8.Min && *(uint8_t*)p <= field->Int8.Max);
    break;
  case DT_INTROSPECTION_TYPE_CHAR:
    all_ok = (*(char*)p >= field->Char.Min && *(char*)p <= field->Char.Max);
    break;
  case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
    all_ok = creal(*(float complex*)p) >= creal(field->FloatComplex.Min) &&
             creal(*(float complex*)p) <= creal(field->FloatComplex.Max) &&
             cimag(*(float complex*)p) >= cimag(field->FloatComplex.Min) &&
             cimag(*(float complex*)p) <= cimag(field->FloatComplex.Max);
    break;
  case DT_INTROSPECTION_TYPE_ENUM:
    all_ok = FALSE;
    for(dt_introspection_type_enum_tuple_t *i = field->Enum.values; i && i->name; i++)
    {
      if(i->value == *(int*)p)
      {
        all_ok = TRUE;
        break;
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_BOOL:
    // *(gboolean*)p
    break;
  case DT_INTROSPECTION_TYPE_OPAQUE:
    // TODO: special case float2
    break;
  default:
    fprintf(stderr, "unsupported introspection type \"%s\" encountered in _iop_validate_params (field %s)\n",
                    field->header.type_name, field->header.name);
    all_ok = FALSE;
    break;
  }

  if(!all_ok && report)
    fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\"%s%s\n",
                    field->header.type_name, (*field->header.name ? ", field: " : ""), field->header.name);

  return all_ok;
}

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params,
                          dt_develop_blend_params_t *blendop_params, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
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
    dt_iop_commit_blend_params(module, blendop_params);

    /* and we add masks */
    dt_masks_group_get_hash_buffer(grp, str + pos);

    // assume process_cl is ready, commit_params can overwrite this.
    if(module->process_cl) piece->process_cl_ready = 1;

    // register if module allows tiling, commit_params can overwrite this.
    if(module->flags() & IOP_FLAGS_ALLOW_TILING) piece->process_tiling_ready = 1;

    if(darktable.unmuted & DT_DEBUG_PARAMS && module->so->get_introspection())
      _iop_validate_params(module->so->get_introspection()->field, params, TRUE);

    module->commit_params(module, params, pipe, piece);
    uint64_t hash = 5381;
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
  if(module->gui_data)
  {
    ++darktable.gui->reset;
    if(!dt_iop_is_hidden(module))
    {
      if(module->params && module->gui_update) module->gui_update(module);
      dt_iop_gui_update_blending(module);
      dt_iop_gui_update_expanded(module);
      _iop_gui_update_label(module);
      dt_iop_gui_set_enable_button(module);
    }
    --darktable.gui->reset;
  }
}

void dt_iop_gui_reset(dt_iop_module_t *module)
{
  ++darktable.gui->reset;
  if(module->gui_reset && !dt_iop_is_hidden(module)) module->gui_reset(module);
  --darktable.gui->reset;
}

static void dt_iop_gui_reset_callback(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  //Ctrl is used to apply any auto-presets to the current module
  //If Ctrl was not pressed, or no auto-presets were applied, reset the module parameters
  if(!(event->state & GDK_CONTROL_MASK) || !dt_gui_presets_autoapply_for_module(module))
  {
    // if a drawn mask is set, remove it from the list
    if(module->blend_params->mask_id > 0)
    {
      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
      if(grp) dt_masks_form_remove(module, NULL, grp);
    }
    /* reset to default params */
    memcpy(module->params, module->default_params, module->params_size);
    dt_iop_commit_blend_params(module, module->default_blendop_params);

    /* reset ui to its defaults */
    dt_iop_gui_reset(module);

    /* update ui to default params*/
    dt_iop_gui_update(module);

    /* and give focus to the module*/
    dt_iop_request_focus(module);

    dt_dev_add_history_item(module->dev, module, TRUE);
  }

  if(dt_conf_get_bool("accel/prefer_expanded") || dt_conf_get_bool("accel/prefer_enabled") || dt_conf_get_bool("accel/prefer_unmasked"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(module->so);
  }
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

  g_signal_connect(G_OBJECT(darktable.gui->presets_popup_menu), "deactivate", G_CALLBACK(_header_menu_deactivate_callback), module->header);

#if GTK_CHECK_VERSION(3, 22, 0)
  gtk_menu_popup_at_widget(darktable.gui->presets_popup_menu, GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
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

    dt_iop_color_picker_reset(darktable.develop->gui_module, TRUE);

    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(darktable.develop->gui_module), GTK_STATE_FLAG_NORMAL,
                               TRUE);

    if(darktable.develop->gui_module->operation_tags_filter()) dt_dev_invalidate_from_gui(darktable.develop);

    dt_accel_disconnect_locals_iop(darktable.develop->gui_module);

    /* reset mask view */
    dt_masks_reset_form_gui();

    /* do stuff needed in the blending gui */
    dt_iop_gui_blending_lose_focus(darktable.develop->gui_module);

    /* redraw the expander */
    gtk_widget_queue_draw(darktable.develop->gui_module->expander);

    /* and finally remove hinter messages */
    dt_control_hinter_message(darktable.control, "");

    // we also remove the focus css class
    GtkWidget *iop_w = gtk_widget_get_parent(dt_iop_gui_get_pluginui(darktable.develop->gui_module));
    GtkStyleContext *context = gtk_widget_get_style_context(iop_w);
    gtk_style_context_remove_class(context, "dt_module_focus");
  }

  darktable.develop->gui_module = module;

  /* set the focus on module */
  if(module)
  {
    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(module), GTK_STATE_FLAG_SELECTED, TRUE);

    if(module->operation_tags_filter()) dt_dev_invalidate_from_gui(darktable.develop);

    dt_accel_connect_locals_iop(module);

    if(module->gui_focus) module->gui_focus(module, TRUE);

    /* redraw the expander */
    gtk_widget_queue_draw(module->expander);

    // we also add the focus css class
    GtkWidget *iop_w = gtk_widget_get_parent(dt_iop_gui_get_pluginui(darktable.develop->gui_module));
    GtkStyleContext *context = gtk_widget_get_style_context(iop_w);
    gtk_style_context_add_class(context, "dt_module_focus");
  }

  /* update sticky accels window */
  if(darktable.view_manager->accels_window.window && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  dt_control_change_cursor(GDK_LEFT_PTR);
}


/*
 * NEW EXPANDER
 */

static void dt_iop_gui_set_single_expanded(dt_iop_module_t *module, gboolean expanded)
{
  if(!module->expander) return;

  /* update expander arrow state */
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);

  /* store expanded state of module.
   * we do that first, so update_expanded won't think it should be visible
   * and undo our changes right away. */
  module->expanded = expanded;

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

  char var[1024];
  snprintf(var, sizeof(var), "plugins/darkroom/%s/expanded", module->op);
  dt_conf_set_bool(var, expanded);
}

void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded, gboolean collapse_others)
{
  if(!module->expander) return;
  /* handle shiftclick on expander, hide all except this */
  if(collapse_others)
  {
    const int current_group = dt_dev_modulegroups_get(module->dev);
    const gboolean group_only = dt_conf_get_bool("darkroom/ui/single_module_group_only");

    GList *iop = g_list_first(module->dev->iop);
    gboolean all_other_closed = TRUE;
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
      if(m != module && (dt_iop_shown_in_group(m, current_group) || !group_only))
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

  const gboolean expanded = module->expanded;

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
    if((e->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    {
      GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
      g_object_set_data(G_OBJECT(container), "source_data", user_data);
      return FALSE;
    }
    else if(e->state & GDK_CONTROL_MASK)
    {
      _iop_gui_rename_module(module);
      return FALSE;
    }
    else
    {
      // make gtk scroll to the module once it updated its allocation size
      if(dt_conf_get_bool("darkroom/ui/scroll_to_module"))
        darktable.gui->scroll_to[1] = module->expander;

      const gboolean collapse_others = !dt_conf_get_bool("darkroom/ui/single_module") != !(e->state & GDK_SHIFT_MASK);
      dt_iop_gui_set_expanded(module, !module->expanded, collapse_others);

      if (dt_conf_get_bool("accel/prefer_expanded"))
      {
        // rebuild the accelerators
        dt_iop_connect_accels_multi(module->so);
      }

      //used to take focus away from module search text input box when module selected
      gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

      return TRUE;
    }
  }
  else if(e->button == 3)
  {
    dt_gui_presets_popup_menu_show_for_module(module);
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));

  g_signal_connect(G_OBJECT(darktable.gui->presets_popup_menu), "deactivate", G_CALLBACK(_header_menu_deactivate_callback), module->header);

#if GTK_CHECK_VERSION(3, 22, 0)
    gtk_menu_popup_at_pointer(darktable.gui->presets_popup_menu, (GdkEvent *)e);
#else
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, e->button, e->time);
#endif

    return TRUE;
  }
  return FALSE;
}

static void header_size_callback(GtkWidget *widget, GdkRectangle *allocation, GtkWidget *header)
{
  gchar *config = dt_conf_get_string("darkroom/ui/hide_header_buttons");

  GList *children = gtk_container_get_children(GTK_CONTAINER(header));

  const gint panel_trigger_width = 250;

  GList *button = g_list_first(children);
  GtkRequisition button_size;
  gtk_widget_show(GTK_WIDGET(button->data));
  gtk_widget_get_preferred_size(GTK_WIDGET(button->data), &button_size, NULL);

  int num_buttons = 0;
  for(button = g_list_last(children);
      button && GTK_IS_BUTTON(button->data);
      button = g_list_previous(button)) num_buttons++;

  gboolean hide_all = (allocation->width == 1);
  int num_to_unhide = (allocation->width - 2) / button_size.width;
  double opacity_leftmost = num_to_unhide > 0 ? 1.0 : (double) allocation->width / button_size.width;
  double opacity_others = 1.0;

  if(g_strcmp0(config, "glide")) // glide uses all defaults above
  {
    // these all (un)hide all buttons at the same time
    if(num_to_unhide < num_buttons) num_to_unhide = 0;

    if(!g_strcmp0(config, "smooth"))
    {
      opacity_others = opacity_leftmost;
    }
    else
    {
      if(!g_strcmp0(config, "fit"))
      {
        opacity_leftmost = 1.0;
      }
      else
      {
        GdkRectangle total_alloc;
        gtk_widget_get_allocation(header, &total_alloc);

        if(!g_strcmp0(config, "auto"))
        {
          opacity_leftmost = 1.0;
          if(total_alloc.width < panel_trigger_width) hide_all = TRUE;
        }
        else if(!g_strcmp0(config, "fade"))
        {
          opacity_leftmost = opacity_others = (total_alloc.width - panel_trigger_width) / 100.;
        }
        else
        {
          fprintf(stderr, "unknown darkroom/ui/hide_header_buttons option %s\n", config);
        }
      }
    }
  }

  GList *prev_button = NULL;

  for(button = g_list_last(children);
      button && GTK_IS_BUTTON(button->data);
      button = g_list_previous(button))
  {
    GtkWidget *b = GTK_WIDGET(button->data);

    if(!gtk_widget_get_visible(b))
    {
      if(num_to_unhide == 0) break;
      --num_to_unhide;
    }

    gtk_widget_set_visible(b, !hide_all);
    gtk_widget_set_opacity(b, opacity_others);

    prev_button = button;
  }
  if(prev_button && num_to_unhide == 0)
    gtk_widget_set_opacity(GTK_WIDGET(prev_button->data), opacity_leftmost);

  g_list_free(children);
  g_free(config);

  GtkAllocation header_allocation;
  gtk_widget_get_allocation(header, &header_allocation);
  if(header_allocation.width > 1) gtk_widget_size_allocate(header, &header_allocation);
}

gboolean dt_iop_show_hide_header_buttons(GtkWidget *header, GdkEventCrossing *event, gboolean show_buttons, gboolean always_hide)
{
  // check if Entry widget for module name edit exists
  if(gtk_container_get_focus_child(GTK_CONTAINER(header))) return TRUE;

  if(event && (darktable.develop->darkroom_skip_mouse_events ||
     event->detail == GDK_NOTIFY_INFERIOR ||
     event->mode != GDK_CROSSING_NORMAL)) return TRUE;

  gchar *config = dt_conf_get_string("darkroom/ui/hide_header_buttons");

  gboolean dynamic = FALSE;
  double opacity = 1.0;
  if(!g_strcmp0(config, "always"))
  {
    show_buttons = TRUE;
  }
  else if(!g_strcmp0(config, "dim"))
  {
    if(!show_buttons) opacity = 0.3;
    show_buttons = TRUE;
  }
  else if(!g_strcmp0(config, "active"))
    ;
  else
    dynamic = TRUE;

  g_free(config);

  GList *children = gtk_container_get_children(GTK_CONTAINER(header));

  GList *button;
  for(button = g_list_last(children);
      button && GTK_IS_BUTTON(button->data);
      button = g_list_previous(button))
  {
    gtk_widget_set_visible(GTK_WIDGET(button->data), show_buttons && !always_hide);
    gtk_widget_set_opacity(GTK_WIDGET(button->data), opacity);
  }
  if(GTK_IS_DRAWING_AREA(button->data))
  {
    // temporarily or permanently (de)activate width trigger widget
    if(dynamic)
      gtk_widget_set_visible(GTK_WIDGET(button->data), !show_buttons && !always_hide);
    else
      gtk_widget_destroy(GTK_WIDGET(button->data));
  }
  else
  {
    if(dynamic)
    {
      GtkWidget *space = gtk_drawing_area_new();
      gtk_box_pack_end(GTK_BOX(header), space, TRUE, TRUE, 0);
      gtk_widget_show(space);
      g_signal_connect(G_OBJECT(space), "size-allocate", G_CALLBACK(header_size_callback), header);
    }
  }

  g_list_free(children);

  if(dynamic && !show_buttons && !always_hide)
  {
    GdkRectangle fake_allocation = {.width = UINT16_MAX};
    header_size_callback(NULL, &fake_allocation, header);
  }

  return TRUE;
}

GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module)
{
  char tooltip[512];

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(header), "module-header");

  GtkWidget *iopw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *expander = dtgtk_expander_new(header, iopw);

  GtkWidget *header_evb = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *body_evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *pluginui_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));

  gtk_widget_set_name(pluginui_frame, "iop-plugin-ui");

  module->header = header;

  /* setup the header box */
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_iop_plugin_header_button_press), module);
  gtk_widget_add_events(header_evb, GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(header_evb), "enter-notify-event", G_CALLBACK(_header_motion_notify_show_callback), header);
  g_signal_connect(G_OBJECT(header_evb), "leave-notify-event", G_CALLBACK(_header_motion_notify_hide_callback), header);

  /* connect mouse button callbacks for focus and presets */
  g_signal_connect(G_OBJECT(body_evb), "button-press-event", G_CALLBACK(_iop_plugin_body_button_press), module);
  gtk_widget_add_events(body_evb, GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(body_evb), "enter-notify-event", G_CALLBACK(_header_motion_notify_show_callback), header);
  g_signal_connect(G_OBJECT(body_evb), "leave-notify-event", G_CALLBACK(_header_motion_notify_hide_callback), header);

  /*
   * initialize the header widgets
   */
  GtkWidget *hw[IOP_MODULE_LAST] = { NULL };

  /* init empty place for icon, this is then set in CSS if needed */
  char w_name[256] = { 0 };
  snprintf(w_name, sizeof(w_name), "iop-panel-icon-%s", module->op);
  hw[IOP_MODULE_ICON] = gtk_label_new("");
  gtk_widget_set_name(GTK_WIDGET(hw[IOP_MODULE_ICON]), w_name);
  gtk_widget_set_valign(GTK_WIDGET(hw[IOP_MODULE_ICON]), GTK_ALIGN_CENTER);

  /* add module label */
  hw[IOP_MODULE_LABEL] = gtk_label_new("");
  _iop_panel_label(hw[IOP_MODULE_LABEL], module);

  /* add multi instances menu button */
  hw[IOP_MODULE_INSTANCE] = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, CPF_STYLE_FLAT, NULL);
  module->multimenu_button = GTK_WIDGET(hw[IOP_MODULE_INSTANCE]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_INSTANCE]),
                              _("multiple instances actions\nmiddle-click creates new instance"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_INSTANCE]), "button-press-event", G_CALLBACK(dt_iop_gui_multiinstance_callback),
                   module);

  gtk_widget_set_name(GTK_WIDGET(hw[IOP_MODULE_INSTANCE]), "module-instance-button");

  dt_gui_add_help_link(expander, dt_get_help_url(module->op));

  /* add reset button */
  hw[IOP_MODULE_RESET] = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT, NULL);
  module->reset_button = GTK_WIDGET(hw[IOP_MODULE_RESET]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_RESET]), _("reset parameters\nctrl+click to reapply any automatic presets"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_RESET]), "button-press-event", G_CALLBACK(dt_iop_gui_reset_callback), module);
  gtk_widget_set_name(GTK_WIDGET(hw[IOP_MODULE_RESET]), "module-reset-button");

  /* add preset button if module has implementation */
  hw[IOP_MODULE_PRESETS] = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT, NULL);
  module->presets_button = GTK_WIDGET(hw[IOP_MODULE_PRESETS]);
  if (module->flags() & IOP_FLAGS_ONE_INSTANCE)
    gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_PRESETS]), _("presets"));
  else
    gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_PRESETS]), _("presets\nmiddle-click to apply on new instance"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_PRESETS]), "clicked", G_CALLBACK(popup_callback), module);
  gtk_widget_set_name(GTK_WIDGET(hw[IOP_MODULE_PRESETS]), "module-preset-button");

  /* add enabled button */
  hw[IOP_MODULE_SWITCH] = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch,
                                                 CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, module);
  dt_iop_gui_set_enable_button_icon(hw[IOP_MODULE_SWITCH], module);

  gchar *module_label = dt_history_item_get_name(module);
  snprintf(tooltip, sizeof(tooltip), module->enabled ? _("%s is switched on") : _("%s is switched off"),
           module_label);
  g_free(module_label);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_SWITCH]), tooltip);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hw[IOP_MODULE_SWITCH]), module->enabled);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_SWITCH]), "toggled", G_CALLBACK(dt_iop_gui_off_callback), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_SWITCH]), "button-press-event", G_CALLBACK(dt_iop_gui_off_button_press), module);
  module->off = DTGTK_TOGGLEBUTTON(hw[IOP_MODULE_SWITCH]);
  gtk_widget_set_sensitive(GTK_WIDGET(hw[IOP_MODULE_SWITCH]), !module->hide_enable_button);

  /* reorder header, for now, iop are always in the right panel */
  for(int i = 0; i <= IOP_MODULE_LABEL; i++)
    if(hw[i]) gtk_box_pack_start(GTK_BOX(header), hw[i], FALSE, FALSE, 0);
  for(int i = IOP_MODULE_LAST - 1; i > IOP_MODULE_LABEL; i--)
    if(hw[i]) gtk_box_pack_end(GTK_BOX(header), hw[i], FALSE, FALSE, 0);

  dt_iop_show_hide_header_buttons(module->header, NULL, FALSE, FALSE);

  dt_gui_add_help_link(header, "interacting.html");

  gtk_widget_set_halign(hw[IOP_MODULE_LABEL], GTK_ALIGN_START);
  gtk_widget_set_halign(hw[IOP_MODULE_INSTANCE], GTK_ALIGN_END);

  /* add the blending ui if supported */
  gtk_box_pack_start(GTK_BOX(iopw), module->widget, TRUE, TRUE, 0);
  dt_iop_gui_init_blending(iopw, module);
  gtk_widget_set_name(module->widget, "iop-plugin-ui-main");
  dt_gui_add_help_link(module->widget, dt_get_help_url(module->op));
  gtk_widget_hide(iopw);

  module->expander = expander;

  /* update header */
  _iop_gui_update_header(module);

  gtk_widget_set_hexpand(module->widget, FALSE);
  gtk_widget_set_vexpand(module->widget, FALSE);

  /* connect accelerators */
  dt_iop_connect_common_accels(module);
  if(module->connect_key_accels) module->connect_key_accels(module);

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
  if(pipe != dev->preview_pipe && pipe != dev->preview2_pipe) sched_yield();
  if(pipe != dev->preview_pipe && pipe != dev->preview2_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
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

dt_iop_module_t *dt_iop_get_colorout_module(void)
{
  return dt_iop_get_module_from_list(darktable.develop->iop, "colorout");
}

dt_iop_module_t *dt_iop_get_module_from_list(GList *iop_list, const char *op)
{
  dt_iop_module_t *result = NULL;

  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(strcmp(mod->op, op) == 0)
    {
      result = mod;
      break;
    }
    modules = g_list_next(modules);
  }

  return result;
}

dt_iop_module_t *dt_iop_get_module(const char *op)
{
  return dt_iop_get_module_from_list(darktable.develop->iop, op);
}

int get_module_flags(const char *op)
{
  GList *modules = darktable.iop;
  while(modules)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)modules->data;
    if(!strcmp(module->op, op)) return module->flags();
    modules = g_list_next(modules);
  }
  return 0;
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

  const uint32_t current_group = dt_dev_modulegroups_get(module->dev);

  if(!dt_iop_shown_in_group(module, current_group))
  {
    dt_dev_modulegroups_switch(darktable.develop, module);
  }
  else
  {
    dt_dev_modulegroups_set(darktable.develop, current_group);
  }

  dt_iop_gui_set_expanded(module, !module->expanded, dt_conf_get_bool("darkroom/ui/single_module"));
  if(module->expanded)
  {
    dt_iop_request_focus(module);
  }

  if(dt_conf_get_bool("accel/prefer_expanded"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(module->so);
  }

  return TRUE;
}

static gboolean request_module_focus_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)

{
  dt_iop_module_t * module = (dt_iop_module_t *)data;
  dt_iop_request_focus(darktable.develop->gui_module == module ? NULL : module);
  return TRUE;
}

static gboolean enable_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                       GdkModifierType modifier, gpointer data)

{
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(module->off));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), !active);

  if(dt_conf_get_bool("darkroom/ui/scroll_to_module"))
      darktable.gui->scroll_to[1] = module->expander;

  if(dt_conf_get_bool("darkroom/ui/activate_expand"))
    dt_iop_gui_set_expanded(module, !active, dt_conf_get_bool("darkroom/ui/single_module"));

  dt_iop_request_focus(module);

  if(dt_conf_get_bool("accel/prefer_enabled"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(module->so);
  }

  return TRUE;
}

void dt_iop_connect_common_accels(dt_iop_module_t *module)
{

  GClosure *closure = NULL;
  if(module->flags() & IOP_FLAGS_DEPRECATED) return;
  // Connecting the (optional) module show accelerator
  closure = g_cclosure_new(G_CALLBACK(show_module_callback), module, NULL);
  dt_accel_connect_iop(module, "show module", closure);

  // Connecting the (optional) module gui focus accelerator
  closure = g_cclosure_new(G_CALLBACK(request_module_focus_callback), module, NULL);
  dt_accel_connect_iop(module, "focus module", closure);

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

// to be called before issuing any query based on memory.darktable_iop_names
void dt_iop_set_darktable_iop_table()
{
  sqlite3_stmt *stmt;
  gchar *module_list = NULL;
  GList *iop = g_list_first(darktable.iop);
  while(iop != NULL)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
    module_list = dt_util_dstrcat(module_list, "(\"%s\",\"%s\"),", module->op, module->name());
    iop = g_list_next(iop);
  }

  if(module_list)
  {
    module_list[strlen(module_list) - 1] = '\0';
    char *query = dt_util_dstrcat(NULL, "INSERT INTO memory.darktable_iop_names (operation, name) VALUES %s", module_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
    g_free(module_list);
  }
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
  if(op != NULL)
  {
    return (gchar *)g_hash_table_lookup(module_names, op);
  }
  else {
    return _("ERROR");
  }
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
    if(!darktable.gui->reset)
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
}

void dt_iop_gui_set_state(dt_iop_module_t *module, dt_iop_module_state_t state)
{
  dt_iop_so_gui_set_state(module->so, state);
}

void dt_iop_update_multi_priority(dt_iop_module_t *module, int new_priority)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, module->raster_mask.source.users);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_iop_module_t *sink_module = (dt_iop_module_t *)key;

    sink_module->blend_params->raster_mask_instance = new_priority;

    // also fix history entries
    for(GList *hiter = module->dev->history; hiter; hiter = g_list_next(hiter))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)hiter->data;
      if(hist->module == sink_module)
        hist->blend_params->raster_mask_instance = new_priority;
    }
  }

  module->multi_priority = new_priority;
}

gboolean dt_iop_is_raster_mask_used(dt_iop_module_t *module, int id)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, module->raster_mask.source.users);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    if(GPOINTER_TO_INT(value) == id)
      return TRUE;
  }
  return FALSE;
}

dt_iop_module_t *dt_iop_get_module_by_op_priority(GList *modules, const char *operation, const int multi_priority)
{
  dt_iop_module_t *mod_ret = NULL;

  GList *m = g_list_first(modules);
  while(m)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m->data;

    if(strcmp(mod->op, operation) == 0
       && (mod->multi_priority == multi_priority || multi_priority == -1))
    {
      mod_ret = mod;
      break;
    }

    m = g_list_next(m);
  }
  return mod_ret;
}

/** adds keyboard accels to the first module in the pipe to handle where there are multiple instances */
void dt_iop_connect_accels_multi(dt_iop_module_so_t *module)
{
  /*
   decide which module instance keyboard shortcuts will be applied to based on user preferences, as follows
    - prefer expanded instances (when selected and instances of the module are expanded on the RHS of the screen, collapsed instances will be ignored)
    - prefer enabled instances (when selected, after applying the above rule, if instances of the module are active, inactive instances will be ignored)
    - prefer unmasked instances (when selected, after applying the above rules, if instances of the module are unmasked, masked instances will be ignored)
    - selection order (after applying the above rules, apply the shortcut to the first or last instance remaining)
  */
  int prefer_expanded = dt_conf_get_bool("accel/prefer_expanded") ? 8 : 0;
  int prefer_enabled = dt_conf_get_bool("accel/prefer_enabled") ? 4 : 0;
  int prefer_unmasked = dt_conf_get_bool("accel/prefer_unmasked") ? 2 : 0;
  int prefer_first = strcmp(dt_conf_get_string("accel/select_order"), "first instance") == 0 ? 1 : 0;

  if(darktable.develop->gui_attached)
  {
    dt_iop_module_t *accel_mod_new = NULL;  //The module to which accelerators are to be attached

    int best_score = -1;

    for(GList *iop_mods = g_list_last(darktable.develop->iop);
        iop_mods;
        iop_mods = g_list_previous(iop_mods))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)iop_mods->data;

      if(mod->so == module && mod->iop_order != INT_MAX)
      {
        int score = (mod->expanded ? prefer_expanded : 0)
                  + (mod->enabled ? prefer_enabled : 0)
                  + (mod->blend_params->mask_mode == DEVELOP_MASK_DISABLED ||
                    mod->blend_params->mask_mode == DEVELOP_MASK_ENABLED ? prefer_unmasked : 0);

        if(score + prefer_first > best_score)
        {
          best_score = score;
          accel_mod_new = mod;
        }
      }
    }

    // switch accelerators to new module
    if(accel_mod_new) dt_accel_connect_instance_iop(accel_mod_new);
  }
}

void dt_iop_connect_accels_all()
{
  GList *iop_mods = g_list_last(darktable.develop->iop);
  while(iop_mods)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)iop_mods->data;
    dt_iop_connect_accels_multi(mod->so);
    iop_mods = g_list_previous(iop_mods);
  }
}

dt_iop_module_t *dt_iop_get_module_by_instance_name(GList *modules, const char *operation, const char *multi_name)
{
  dt_iop_module_t *mod_ret = NULL;

  GList *m = g_list_first(modules);
  while(m)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m->data;

    if((strcmp(mod->op, operation) == 0)
       && ((multi_name == NULL) || (strcmp(mod->multi_name, multi_name) == 0)))
    {
      mod_ret = mod;
      break;
    }

    m = g_list_next(m);
  }
  return mod_ret;
}

/** count instances of a module **/
int dt_iop_count_instances(dt_iop_module_so_t *module)
{
  int inst_count = 0;
  GList *iop_mods = NULL;                 //All modules in iop
  dt_iop_module_t *mod = NULL;            //Used while iterating module lists

  iop_mods = g_list_last(darktable.develop->iop);
  while(iop_mods)
  {
    mod = (dt_iop_module_t *)iop_mods->data;
    if(mod->so == module && mod->iop_order != INT_MAX)
    {
      inst_count++;
    }
    iop_mods = g_list_previous(iop_mods);
  }
  return inst_count;
}

void dt_iop_refresh_center(dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if (dev && dev->gui_attached)
  {
    // invalidate the pixelpipe cache except for the output of the prior module
    const uint64_t hash = dt_dev_pixelpipe_cache_basichash_prior(dev->pipe->image.id, dev->pipe, module);
    dt_dev_pixelpipe_cache_flush_all_but(&dev->pipe->cache, hash);
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH; //ensure that commit_params gets called to pick up any GUI changes
    dt_dev_invalidate(dev);
    dt_control_queue_redraw_center();
  }
}

void dt_iop_refresh_preview(dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if (dev && dev->gui_attached)
  {
    // invalidate the pixelpipe cache except for the output of the prior module
    const uint64_t hash = dt_dev_pixelpipe_cache_basichash_prior(dev->pipe->image.id, dev->preview_pipe, module);
    dt_dev_pixelpipe_cache_flush_all_but(&dev->preview_pipe->cache, hash);
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH; //ensure that commit_params gets called to pick up any GUI changes
    dt_dev_invalidate_all(dev);
    dt_control_queue_redraw();
  }
}

void dt_iop_refresh_preview2(dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if (dev && dev->gui_attached)
  {
    // invalidate the pixelpipe cache except for the output of the prior module
    const uint64_t hash = dt_dev_pixelpipe_cache_basichash_prior(dev->pipe->image.id, dev->preview2_pipe, module);
    dt_dev_pixelpipe_cache_flush_all_but(&dev->preview2_pipe->cache, hash);
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH; //ensure that commit_params gets called to pick up any GUI changes
    dt_dev_invalidate_all(dev);
    dt_control_queue_redraw();
  }
}

void dt_iop_refresh_all(dt_iop_module_t *module)
{
  dt_iop_refresh_preview(module);
  dt_iop_refresh_center(module);
  dt_iop_refresh_preview2(module);
}

static gboolean _postponed_history_update(gpointer data)
{
  dt_iop_module_t *self = (dt_iop_module_t*)data;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  self->timeout_handle = 0;
  return FALSE; //cancel the timer
}

/** queue a delayed call of the add_history function after user interaction, to capture parameter updates (but not */
/** too often). */
void dt_iop_queue_history_update(dt_iop_module_t *module, gboolean extend_prior)
{
  if (module->timeout_handle && extend_prior)
  {
    // we already queued an update, but we don't want to have the update happen until the timeout expires
    // without any activity, so cancel the queued callback
    g_source_remove(module->timeout_handle);
  }
  if (!module->timeout_handle || extend_prior)
  {
    // adaptively set the timeout to 150% of the average time the past several pixelpipe runs took, clamped
    //   to keep updates from appearing to be too sluggish (though early iops such as rawdenoise may have
    //   multiple very slow iops following them, leading to >1000ms processing times)
    const int delay = CLAMP(darktable.develop->average_delay * 3 / 2, 10, 1200);
    module->timeout_handle = g_timeout_add(delay, _postponed_history_update, module);
  }
}

void dt_iop_cancel_history_update(dt_iop_module_t *module)
{
  if (module->timeout_handle)
  {
    g_source_remove(module->timeout_handle);
    module->timeout_handle = 0;
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
