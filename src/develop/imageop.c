/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "common/collection.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/exif.h"
#include "common/history.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/iop_group.h"
#include "common/module.h"
#include "common/opencl.h"
#include "common/presets.h"
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
#include "gui/color_picker_proxy.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "imageio/imageio_rawspeed.h"
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
#include <time.h>

typedef struct dt_iop_gui_simple_callback_t
{
  dt_iop_module_t *self;
  int index;
} dt_iop_gui_simple_callback_t;

typedef struct dt_iop_gui_multi_show_t
{
  gboolean close, up, down, new;
} dt_iop_gui_multi_show_t;

void dt_iop_load_default_params(dt_iop_module_t *module)
{
  memcpy(module->params, module->default_params, module->params_size);
  dt_develop_blend_colorspace_t cst =
    dt_develop_blend_default_module_blend_colorspace(module);
  dt_develop_blend_init_blend_parameters(module->default_blendop_params, cst);
  dt_iop_commit_blend_params(module, module->default_blendop_params);
  dt_iop_gui_blending_reload_defaults(module);
}

static void _iop_modify_roi_in(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               const dt_iop_roi_t *roi_out,
                               dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
}

static void _iop_modify_roi_out(dt_iop_module_t *self,
                                dt_dev_pixelpipe_iop_t *piece,
                                dt_iop_roi_t *roi_out,
                                const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

/* default group for modules which do not implement the
 * default_group() function */
static int default_default_group(void)
{
  return IOP_GROUP_BASIC;
}

/* default flags for modules which does not implement the flags()
 * function */
static int default_flags(void)
{
  return 0;
}

/* default operation tags for modules which does not implement the
 * flags() function */
static int default_operation_tags(void)
{
  return 0;
}

/* default operation tags filter for modules which does not implement
 * the flags() function */
static int default_operation_tags_filter(void)
{
  return 0;
}

static const char **default_description(dt_iop_module_t *self)
{
  return NULL;
}

static const char *default_aliases(void)
{
  return "";
}

static const char *default_deprecated_msg(void)
{
  return NULL;
}

static void default_commit_params(dt_iop_module_t *self,
                                  dt_iop_params_t *params,
                                  dt_dev_pixelpipe_t *pipe,
                                  dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, self->params_size);
}

static void default_init_pipe(dt_iop_module_t *self,
                              dt_dev_pixelpipe_t *pipe,
                              dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1,self->params_size);
}

static void default_cleanup_pipe(dt_iop_module_t *self,
                                 dt_dev_pixelpipe_t *pipe,
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


static gboolean default_distort_transform(dt_iop_module_t *self,
                                          dt_dev_pixelpipe_iop_t *piece,
                                          float *points,
                                          size_t points_count)
{
  return TRUE;
}
static gboolean default_distort_backtransform(dt_iop_module_t *self,
                                              dt_dev_pixelpipe_iop_t *piece,
                                              float *points,
                                              size_t points_count)
{
  return TRUE;
}

static void default_process(dt_iop_module_t *self,
                            dt_dev_pixelpipe_iop_t *piece,
                            const void *const i,
                            void *const o,
                            const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out)
{
  if(roi_in->width <= 1
     || roi_in->height <= 1
     || roi_out->width <= 1
     || roi_out->height <= 1)
    return;

  if(self->process_plain)
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
  module->params = calloc(1, param_size);
  module->default_params = calloc(1, param_size);

  module->default_enabled = FALSE;
  module->has_trouble = FALSE;
  module->gui_data = NULL;

  dt_introspection_field_t *i = module->so->get_introspection_linear();

  while(i->header.type != DT_INTROSPECTION_TYPE_NONE)
  {
    switch(i->header.type)
    {
    case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
      *(float complex*)((uint8_t *)module->default_params + i->header.offset) =
        i->FloatComplex.Default;
      break;
    case DT_INTROSPECTION_TYPE_FLOAT:
      *(float*)((uint8_t *)module->default_params + i->header.offset) = i->Float.Default;
      break;
    case DT_INTROSPECTION_TYPE_INT:
      *(int*)((uint8_t *)module->default_params + i->header.offset) = i->Int.Default;
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      *(unsigned int*)((uint8_t *)module->default_params + i->header.offset) =
        i->UInt.Default;
      break;
    case DT_INTROSPECTION_TYPE_USHORT:
      *(unsigned short*)((uint8_t *)module->default_params + i->header.offset) =
        i->UShort.Default;
      break;
    case DT_INTROSPECTION_TYPE_INT8:
      *(short*)((uint8_t *)module->default_params + i->header.offset) = i->Int8.Default;
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
      *(int*)((uint8_t *)module->default_params + i->header.offset) = i->Enum.Default;
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean*)((uint8_t *)module->default_params + i->header.offset) = i->Bool.Default;
      break;
    case DT_INTROSPECTION_TYPE_CHAR:
      *(char*)((uint8_t *)module->default_params + i->header.offset) = i->Char.Default;
      break;
    case DT_INTROSPECTION_TYPE_OPAQUE:
      memset((uint8_t *)module->default_params + i->header.offset, 0, i->header.size);
      break;
    case DT_INTROSPECTION_TYPE_ARRAY:
      {
        if(i->Array.type == DT_INTROSPECTION_TYPE_CHAR) break;

        size_t element_size = i->Array.field->header.size;
        if(element_size % sizeof(int))
        {
          int8_t *p = (int8_t *)module->default_params + i->header.offset;
          for(size_t c = element_size; c < i->header.size; c++, p++)
            p[element_size] = *p;
        }
        else
        {
          element_size /= sizeof(int);
          const size_t num_ints = i->header.size / sizeof(int);

          int *p = (int *)((uint8_t *)module->default_params + i->header.offset);
          for(size_t c = element_size; c < num_ints; c++, p++)
            p[element_size] = *p;
        }
      }
      break;
    case DT_INTROSPECTION_TYPE_STRUCT:
      // ignore STRUCT; nothing to do
      break;
    default:
      dt_print(DT_DEBUG_PARAMS,
               "[dt_iop_default_init] in `%s' unsupported introspection"
               " type \"%s\" encountered"
               " in (field %s)",
               module->op, i->header.type_name, i->header.field_name);
      break;
    }

    i++;
  }
}

int dt_iop_load_module_so(void *m, const char *libname, const char *module_name)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;
  g_strlcpy(module->op, module_name, sizeof(module->op));

#define INCLUDE_API_FROM_MODULE_LOAD "iop_load_module"
#include "iop/iop_api.h"

  if(!module->init)
    module->init = dt_iop_default_init;
  if(!module->modify_roi_in)
    module->modify_roi_in = _iop_modify_roi_in;
  if(!module->modify_roi_out)
    module->modify_roi_out = _iop_modify_roi_out;

  module->process_plain = module->process;
  module->process = default_process;

  module->data = NULL;

  // the introspection api
  module->have_introspection = FALSE;
  if(module->introspection_init)
  {
    if(!module->introspection_init(module, DT_INTROSPECTION_VERSION))
    {
      // set the introspection related fields in module
      module->have_introspection = TRUE;

      if(module->get_p == default_get_p
         || module->get_f == default_get_f
         || module->get_introspection_linear == default_get_introspection_linear
         || module->get_introspection == default_get_introspection)
        goto api_h_error;
    }
    else
      dt_print(DT_DEBUG_ALWAYS,
               "[iop_load_module] failed to initialize introspection for operation `%s'",
               module_name);
  }

  if(module->init_global)
    module->init_global(module);
  return 0;
}

gboolean dt_iop_load_module_by_so(dt_iop_module_t *module,
                                  dt_iop_module_so_t *so,
                                  dt_develop_t *dev)
{
  module->actions = DT_ACTION_TYPE_IOP_INSTANCE;
  module->dev = dev;
  module->widget = NULL;
  module->header = NULL;
  module->off = NULL;
  module->hide_enable_button = FALSE;
  module->has_trouble = FALSE;
  module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  module->request_histogram = DT_REQUEST_ONLY_IN_GUI;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
  module->multi_priority = 0;
  module->multi_name_hand_edited = FALSE;
  module->iop_order = 0;
  for(int k = 0; k < 3; k++)
  {
    module->picked_color[k] = module->picked_output_color[k] = 0.0f;
    module->picked_color_min[k] = module->picked_output_color_min[k] = 666.0f;
    module->picked_color_max[k] = module->picked_output_color_max[k] = -666.0f;
  }
  module->histogram_cst = IOP_CS_NONE;
  module->histogram = NULL;
  module->histogram_max[0] = module->histogram_max[1] =
    module->histogram_max[2] = module->histogram_max[3] = 0;
  module->histogram_middle_grey = FALSE;
  module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  module->suppress_mask = FALSE;
  module->enabled = module->default_enabled = FALSE; // all modules disabled by default.
  g_strlcpy(module->op, so->op, sizeof(module->op));
  module->raster_mask.source.users = g_hash_table_new(NULL, NULL);
  module->raster_mask.source.masks =
    g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = INVALID_MASKID;

  // only reference cached results of dlopen:
  module->module = so->module;
  module->so = so;

#define INCLUDE_API_FROM_MODULE_LOAD_BY_SO
#include "iop/iop_api.h"

  module->version = so->version;
  module->process_plain = so->process_plain;
  module->have_introspection = so->have_introspection;

  module->reset_button = NULL;
  module->presets_button = NULL;
  module->fusion_slider = NULL;

  if(module->dev && module->dev->gui_attached)
  {
    /* set button state */
    char option[1024];
    snprintf(option, sizeof(option), "plugins/darkroom/%s/visible", module->op);
    dt_iop_module_state_t state = IOP_STATE_HIDDEN;
    if(dt_conf_get_bool(option))
    {
      state = IOP_STATE_ACTIVE;
      snprintf(option, sizeof(option), "plugins/darkroom/%s/favorite", module->op);
      if(dt_conf_get_bool(option)) state = IOP_STATE_FAVORITE;
    }
    dt_iop_gui_set_state(module, state);
  }

  module->global_data = so->data;

  // now init the instance:
  module->init(module);

  /* initialize blendop params and default values */
  module->blend_params = calloc(1, sizeof(dt_develop_blend_params_t));
  module->default_blendop_params = calloc(1, sizeof(dt_develop_blend_params_t));
  dt_develop_blend_colorspace_t cst =
    dt_develop_blend_default_module_blend_colorspace(module);
  dt_develop_blend_init_blend_parameters(module->default_blendop_params, cst);
  dt_iop_commit_blend_params(module, module->default_blendop_params);

  if(module->params_size == 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[iop_load_module] `%s' needs to have a params size > 0!", so->op);
    return TRUE; // empty params hurt us in many places, just add a dummy value
  }
  module->enabled = module->default_enabled; // apply (possibly new) default.
  return FALSE;
}

void dt_iop_init_pipe(dt_iop_module_t *module,
                      dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  module->init_pipe(module, pipe, piece);
  piece->blendop_data = calloc(1, sizeof(dt_develop_blend_params_t));
}

static gboolean _header_enter_notify_callback(GtkWidget *eventbox,
                                              GdkEventCrossing *event,
                                              gpointer user_data)
{
  darktable.control->element = GPOINTER_TO_INT(user_data);
  return FALSE;
}

static gboolean _header_motion_notify_show_callback(GtkWidget *eventbox,
                                                    GdkEventCrossing *event,
                                                    dt_iop_module_t *module)
{
  darktable.control->element = DT_ACTION_ELEMENT_SHOW;
  return dt_iop_show_hide_header_buttons(module, event, TRUE, FALSE);
}

static gboolean _header_motion_notify_hide_callback(GtkWidget *eventbox,
                                                    GdkEventCrossing *event,
                                                    dt_iop_module_t *module)
{
  return dt_iop_show_hide_header_buttons(module, event, FALSE, FALSE);
}

static void _header_menu_deactivate_callback(GtkMenuShell *menushell,
                                             dt_iop_module_t *module)
{
  dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
}

static void _gui_delete_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_develop_t *dev = module->dev;

  // we search another module with the same base
  // we want the next module if any or the previous one
  GList *modules = module->dev->iop;
  dt_iop_module_t *next = NULL;
  gboolean find = FALSE;
  while(modules)
  {
    dt_iop_module_t *mod = modules->data;
    if(mod == module)
    {
      find = TRUE;
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
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE);

  // we must pay attention if priority is 0
  const gboolean is_zero = (module->multi_priority == 0);

  // we set the focus to the other instance
  dt_iop_gui_set_expanded(next, TRUE, FALSE);
  dt_iop_request_focus(next);

  ++darktable.gui->reset;

  // we remove the plugin effectively
  if(!dt_iop_is_hidden(module))
  {
    dt_iop_gui_cleanup_module(module);
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
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
    GList *history = dev->history;
    while(history)
    {
      dt_dev_history_item_t *hist = history->data;
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
    for(history = dev->history; history; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = history->data;
      if(hist->module == first) hist->multi_priority = 0;
    }
  }

  // we save the current state of history (with the new multi_priorities)
  if(dev->gui_attached)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);

  // rebuild the accelerators (to point to an extant module)
  dt_iop_connect_accels_multi(module->so);

  dt_action_cleanup_instance_iop(module);

  // don't delete the module, a pipe may still need it
  dev->alliop = g_list_append(dev->alliop, module);

  dt_dev_pixelpipe_rebuild(dev);

  /* redraw */
  dt_control_queue_redraw_center();

  --darktable.gui->reset;
}

dt_iop_module_t *dt_iop_gui_get_previous_visible_module(const dt_iop_module_t *module)
{
  dt_iop_module_t *prev = NULL;

  for(GList *modules = module->dev->iop;
      modules;
      modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = modules->data;
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
  }
  return prev;
}

dt_iop_module_t *dt_iop_gui_get_next_visible_module(const dt_iop_module_t *module)
{
  dt_iop_module_t *next = NULL;

  for(const GList *modules = g_list_last(module->dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *mod = modules->data;
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
  }
  return next;
}

static void _gui_movedown_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_movedown_callback begin");

  // we need to place this module right before the previous
  dt_iop_module_t *prev = dt_iop_gui_get_previous_visible_module(module);
  if(!prev) return;

  const int moved = dt_ioppr_move_iop_before(module->dev, module, prev);
  if(!moved) return;

  // we move the headers
  GValue gv = { 0, { { 0 } } };
  g_value_init(&gv, G_TYPE_INT);
  gtk_container_child_get_property(
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui,
                                        DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
      prev->expander,
      "position", &gv);
  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui,
                                            DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv));

  dt_dev_add_history_item(prev->dev, module, TRUE);

  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_movedown_callback end");

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);

  dt_dev_pixelpipe_rebuild(module->dev);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MODULE_MOVED);
}

static void _gui_moveup_callback(GtkButton *button, dt_iop_module_t *module)
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
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui,
                                        DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
      next->expander,
      "position", &gv);

  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui,
                                            DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv));

  dt_dev_add_history_item(next->dev, module, TRUE);

  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_moveup_callback end");

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);

  dt_dev_pixelpipe_rebuild(next->dev);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MODULE_MOVED);
}

dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base,
                                      const gboolean copy_params)
{
  // make sure the duplicated module appears in the history
  dt_dev_add_history_item(base->dev, base, FALSE);

  // first we create the new module
  ++darktable.gui->reset;
  dt_iop_module_t *module = dt_dev_module_duplicate(base->dev, base);
  --darktable.gui->reset;
  if(!module) return NULL;

  // what is the position of the module in the pipe ?
  GList *modules = module->dev->iop;
  int pos_module = 0;
  int pos_base = 0;
  int pos = 0;

  while(modules)
  {
    dt_iop_module_t *mod = modules->data;
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
    dt_iop_gui_set_expander(module);
    GValue gv = { 0, { { 0 } } };
    g_value_init(&gv, G_TYPE_INT);
    gtk_container_child_get_property(
        GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui,
                                          DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
        base->expander, "position", &gv);
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui,
                                              DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                          module->expander,
                          g_value_get_int(&gv) + pos_base - pos_module + 1);
    dt_iop_gui_set_expanded(module, TRUE, FALSE);

    dt_iop_reload_defaults(module); // some modules like profiled
                                    // denoise update the gui in
                                    // reload_defaults

    if(copy_params)
    {
      memcpy(module->params, base->params, module->params_size);
      if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      {
        dt_iop_commit_blend_params(module, base->blend_params);
        if(dt_is_valid_maskid(base->blend_params->mask_id))
        {
          module->blend_params->mask_id = NO_MASKID;
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

  // and we refresh the pipe
  dt_iop_request_focus(module);

  if(module->dev->gui_attached)
  {
    dt_dev_pixelpipe_rebuild(module->dev);
  }

  /* update ui to new parameters */
  dt_iop_gui_update(module);

  dt_dev_modulegroups_update_visibility(darktable.develop);

  return module;
}

static void _gui_copy_callback(GtkButton *button, dt_iop_module_t *base)
{
  dt_iop_module_t *module = dt_iop_gui_duplicate(base, FALSE);

  /* setup key accelerators */
  dt_iop_connect_accels_multi(base->so);

  if(dt_conf_get_bool("darkroom/ui/rename_new_instance"))
    dt_iop_gui_rename_module(module);
}

static void _gui_duplicate_callback(GtkButton *button, dt_iop_module_t *base)
{
  dt_iop_module_t *module = dt_iop_gui_duplicate(base, TRUE);

  /* setup key accelerators */
  dt_iop_connect_accels_multi(base->so);

  if(dt_conf_get_bool("darkroom/ui/rename_new_instance"))
    dt_iop_gui_rename_module(module);
}

static gboolean _rename_module_key_press(GtkWidget *entry,
                                         GdkEventKey *event,
                                         dt_iop_module_t *module)
{
  gboolean ended = FALSE;

  if(event->type == GDK_FOCUS_CHANGE
     || event->keyval == GDK_KEY_Return
     || event->keyval == GDK_KEY_KP_Enter)
  {
    if(gtk_entry_get_text_length(GTK_ENTRY(entry)) > 0)
    {
      // name is not empty, set new multi_name

       const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));

      if(g_strcmp0(module->multi_name, name) != 0)
      {
        g_strlcpy(module->multi_name, name, sizeof(module->multi_name));
        // this has been hand edited, the name should not be changed when
        // applying a preset or a style.
        module->multi_name_hand_edited = TRUE;
        dt_dev_add_history_item(module->dev, module, TRUE);
      }
    }
    else
    {
      // clear out multi-name (set 1st char to 0)
      module->multi_name[0] = 0;
      module->multi_name_hand_edited = FALSE;
      dt_dev_add_history_item(module->dev, module, FALSE);
    }

    // make sure we write history & xmp to ensure that the new module name
    // gets recorded into the XMP and won't be lost in case of crash.
    dt_dev_write_history(darktable.develop);
    dt_image_synch_xmp(darktable.develop->image_storage.id);

    ended = TRUE;
  }
  else if(event->keyval == GDK_KEY_Escape)
  {
    ended = TRUE;
  }

  if(ended)
  {
    gtk_widget_show(module->instance_name);

    g_signal_handlers_disconnect_by_func(entry,
                                         G_CALLBACK(_rename_module_key_press),
                                         module);
    gtk_widget_destroy(entry);
    dt_iop_show_hide_header_buttons(module, NULL, TRUE, FALSE); // after removing entry
    dt_iop_gui_update_header(module);
    dt_masks_group_update_name(module);
    return TRUE;
  }

  return FALSE; /* event not handled */
}

static gboolean _rename_module_resize(GtkWidget *entry,
                                      GdkEventKey *event,
                                      dt_iop_module_t *module)
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

void dt_iop_gui_rename_module(dt_iop_module_t *module)
{
  GtkWidget *focused = gtk_container_get_focus_child(GTK_CONTAINER(module->header));
  if(focused && GTK_IS_ENTRY(focused)) return;

  GtkWidget *entry = gtk_entry_new();

  gtk_widget_set_name(entry, "iop-panel-label");
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 0);
  gtk_entry_set_max_length(GTK_ENTRY(entry), sizeof(module->multi_name) - 1);
  gtk_entry_set_text(GTK_ENTRY(entry),
                     strcmp(module->multi_name, "0")
                     || module->multi_priority > 0
                     || module->multi_name_hand_edited
                       ? module->multi_name
                       : "");

  //  hide module instance name as we need the space for the entry
  gtk_widget_hide(module->instance_name);

  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);
  g_signal_connect(entry, "key-press-event",
                   G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "focus-out-event",
                   G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "style-updated",
                   G_CALLBACK(_rename_module_resize), module);
  g_signal_connect(entry, "changed",
                   G_CALLBACK(_rename_module_resize), module);
  g_signal_connect(entry, "enter-notify-event",
                   G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_SHOW));

  dt_iop_show_hide_header_buttons(module, NULL, FALSE, TRUE); // before adding entry
  gtk_box_pack_start(GTK_BOX(module->header), entry, TRUE, TRUE, 0);
  gtk_widget_show(entry);
  gtk_widget_grab_focus(entry);
}

static void _gui_rename_callback(GtkButton *button,
                                 dt_iop_module_t *module)
{
  dt_iop_gui_rename_module(module);
}

void _get_multi_show(dt_iop_module_t *module,
                     dt_iop_gui_multi_show_t *multi_show)
{
  dt_develop_t *dev = darktable.develop;

  // We count the number of other instances
  int nb_instances = 0;
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = modules->data;

    if(mod->instance == module->instance) nb_instances++;
  }

  dt_iop_module_t *mod_prev = dt_iop_gui_get_previous_visible_module(module);
  dt_iop_module_t *mod_next = dt_iop_gui_get_next_visible_module(module);

  const gboolean move_next =
    (mod_next && mod_next->iop_order != INT_MAX)
    ? dt_ioppr_check_can_move_after_iop(dev->iop, module, mod_next)
    : -1.0;
  const gboolean move_prev =
    (mod_prev && mod_prev->iop_order != INT_MAX)
    ? dt_ioppr_check_can_move_before_iop(dev->iop, module, mod_prev)
    : -1.0;

  multi_show->new = !(module->flags() & IOP_FLAGS_ONE_INSTANCE);
  multi_show->close = (nb_instances > 1);
  if(mod_next)
    multi_show->up = move_next;
  else
    multi_show->up = 0;
  if(mod_prev)
    multi_show->down = move_prev;
  else
    multi_show->down = 0;
}

static gboolean _gui_multiinstance_callback(GtkButton *button,
                                            GdkEventButton *event,
                                            dt_iop_module_t *module)
{
  if(event && event->button == 3)
  {
    if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE))
      _gui_copy_callback(button, module);
    return TRUE;
  }
  else if(event && event->button == 2)
  {
    return FALSE;
  }

  dt_iop_gui_multi_show_t multi_show;
  _get_multi_show(module, &multi_show);

  GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
  GtkWidget *item;

  item = gtk_menu_item_new_with_label(_("new instance"));
  // gtk_widget_set_tooltip_text(item, _("add a new instance of this module to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate",
                   G_CALLBACK(_gui_copy_callback), module);
  gtk_widget_set_sensitive(item, multi_show.new);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("duplicate instance"));
  // gtk_widget_set_tooltip_text(item, _("add a copy of this instance to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate",
                   G_CALLBACK(_gui_duplicate_callback), module);
  gtk_widget_set_sensitive(item, multi_show.new);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("move up"));
  // gtk_widget_set_tooltip_text(item, _("move this instance up"));
  g_signal_connect(G_OBJECT(item), "activate",
                   G_CALLBACK(_gui_moveup_callback), module);
  gtk_widget_set_sensitive(item, multi_show.up);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("move down"));
  // gtk_widget_set_tooltip_text(item, _("move this instance down"));
  g_signal_connect(G_OBJECT(item), "activate",
                   G_CALLBACK(_gui_movedown_callback), module);
  gtk_widget_set_sensitive(item, multi_show.down);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("delete"));
  // gtk_widget_set_tooltip_text(item, _("delete this instance"));
  g_signal_connect(G_OBJECT(item), "activate",
                   G_CALLBACK(_gui_delete_callback), module);
  gtk_widget_set_sensitive(item, multi_show.close);
  gtk_menu_shell_append(menu, item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  item = gtk_menu_item_new_with_label(_("rename"));
  g_signal_connect(G_OBJECT(item), "activate",
                   G_CALLBACK(_gui_rename_callback), module);
  gtk_menu_shell_append(menu, item);

  g_signal_connect(G_OBJECT(menu), "deactivate",
                   G_CALLBACK(_header_menu_deactivate_callback), module);

  dt_gui_menu_popup(GTK_MENU(menu), GTK_WIDGET(button),
                    GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);

  // make sure the button is deactivated now that the menu is opened
  if(button)
    dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);

  return TRUE;
}

static gboolean _gui_off_button_press(GtkButton *w,
                                      GdkEventButton *e,
                                      dt_iop_module_t *module)
{
  if(module->operation_tags() & IOP_TAG_DISTORT)
  {
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_DISTORT);
  }

  if(!darktable.gui->reset && dt_modifier_is(e->state, GDK_CONTROL_MASK))
  {
    dt_iop_request_focus(dt_dev_gui_module() == module ? NULL : module);
    return TRUE;
  }

  return FALSE;
}

static void _gui_off_callback(GtkToggleButton *togglebutton,
                              dt_iop_module_t *module)
{
  const gboolean basics =
    (dt_dev_modulegroups_get_activated(module->dev) == DT_MODULEGROUP_BASICS);
  const gboolean special = module->flags() & IOP_FLAGS_GUIDES_SPECIAL_DRAW;

  if(!darktable.gui->reset)
  {
    /* modules with IOP_FLAGS_GUIDES_SPECIAL_DRAW flag like crop & ashift need special care.
        If in expanded state we request focus to let it's gui_focus() callback
        handle this gracefully.
       Otherwise respect the "darkroom/ui/activate_expand" conf to expand or collapse
    */
    const gboolean activate_expand = dt_conf_get_bool("darkroom/ui/activate_expand");
    if(gtk_toggle_button_get_active(togglebutton))
    {
      module->enabled = TRUE;
      if(!basics)
      {
        if(activate_expand && !module->expanded)
          dt_iop_gui_set_expanded(module, TRUE,
                                dt_conf_get_bool("darkroom/ui/single_module"));
        else if(special && module->expanded)
        {
          // we want to unfocus / refocus to ensure valid data in all cases
          // as dt_iop_request_focus() does not handle this case.
          if(module->dev->gui_module == module)
            dt_iop_request_focus(NULL);
          dt_iop_request_focus(module);
        }
      }
    }
    else
    {
      module->enabled = FALSE;
      if(!basics && module->expanded)
      {
        if(activate_expand)
          dt_iop_gui_set_expanded(module, FALSE, FALSE);
        else if(special)
          dt_iop_request_focus(NULL);
      }
    }

    dt_dev_add_history_item(module->dev, module, FALSE);

    // set mask indicator sensitive according to module activation and raster mask
    if(module->mask_indicator)
      gtk_widget_set_sensitive(module->mask_indicator, module->enabled);
  }

  char tooltip[512];
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(tooltip, sizeof(tooltip),
           module->enabled ? _("'%s' is switched on") : _("'%s' is switched off"),
           module_label);
  g_free(module_label);
  gtk_widget_set_tooltip_text(GTK_WIDGET(togglebutton), tooltip);
  gtk_widget_queue_draw(GTK_WIDGET(togglebutton));

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);

  if(!gtk_widget_is_visible(module->header))
    dt_dev_modulegroups_update_visibility(darktable.develop);
}

gboolean dt_iop_so_is_hidden(dt_iop_module_so_t *module)
{
  gboolean is_hidden = TRUE;
  if(!(module->flags() & IOP_FLAGS_HIDDEN))
  {
    if(!module->gui_init)
      dt_print(DT_DEBUG_ALWAYS,
               "Module '%s' is not hidden and lacks implementation of gui_init()...",
               module->op);
    else if(!module->gui_cleanup)
      dt_print(DT_DEBUG_ALWAYS,
               "Module '%s' is not hidden and lacks implementation of gui_cleanup()...",
               module->op);
    else
      is_hidden = FALSE;
  }
  return is_hidden;
}

gboolean dt_iop_is_hidden(dt_iop_module_t *module)
{
  return !module || !module->so || dt_iop_so_is_hidden(module->so);
}

gboolean dt_iop_shown_in_group(dt_iop_module_t *module, uint32_t group)
{
  if(group == DT_MODULEGROUP_NONE) return TRUE;

  return dt_dev_modulegroups_test(module->dev, group, module);
}

static void _iop_panel_name(dt_iop_module_t *module)
{
  // IOP instance name if any

  // do not mess with panel name if we are not on the top of the history
  if(darktable.develop->history_end < g_list_length(darktable.develop->history)
    || !module->instance_name)
    return;

  GtkLabel *iname = GTK_LABEL(module->instance_name);
  gchar *new_label = NULL;
  gchar *multi_name = NULL;

  if(module->has_trouble && module->enabled)
  {
    new_label = g_strdup("⚠");
    multi_name = g_strdup("⚠");
    gtk_widget_set_name(GTK_WIDGET(iname), "iop-module-name-error");
  }
  else
  {
    if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    {
      new_label = g_strdup("");
      multi_name = g_strdup("");
      gtk_widget_set_name(GTK_WIDGET(iname), "");
    }
    else
    {
      new_label = g_strdup_printf("• %s", module->multi_name);
      multi_name = g_strdup(module->multi_name);
      gtk_widget_set_name(GTK_WIDGET(iname), "iop-module-name");
    }
  }

  gtk_label_set_text(iname, new_label);

  g_free(multi_name);
  g_free(new_label);
}

void dt_iop_gui_update_header(dt_iop_module_t *module)
{
  if(!module->header) /* some modules such as overexposed don't actually have a header */
    return;

  // set panel name to display correct multi-instance
  _iop_panel_name(module);
  dt_iop_gui_set_enable_button(module);
}

void dt_iop_gui_set_enable_button_icon(GtkWidget *w, dt_iop_module_t *module)
{
  // set on/off icon
  if(module->default_enabled && module->hide_enable_button)
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w),
                                 dtgtk_cairo_paint_switch_on, 0, module);
  }
  else if(!module->default_enabled && module->hide_enable_button)
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w),
                                 dtgtk_cairo_paint_switch_off, 0, module);
  }
  else
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w),
                                 dtgtk_cairo_paint_switch, 0, module);
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

void dt_iop_set_module_trouble_message(dt_iop_module_t *const module,
                                       const char* const trouble_msg,
                                       const char* const trouble_tooltip,
                                       const char *const stderr_message)
{
  //  first stderr message if any
  if(stderr_message)
  {
    const dt_image_t *img = module ? &module->dev->image_storage : NULL;
    const char *name = module ? module->name() : "?";

    dt_print(DT_DEBUG_ALWAYS, "Trouble: [%s] %s (%s %d)",
             name,
             stderr_message,
             img ? img->filename : "?",
             img ? img->id : -1);
  }

  if(!dt_iop_is_hidden(module)
     && module->gui_data
     && dt_conf_get_bool("plugins/darkroom/show_warnings"))
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TROUBLE_MESSAGE,
                            module, trouble_msg, trouble_tooltip);
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
  if(darktable.gui)
    ++darktable.gui->reset;

  if(module->reload_defaults)
  {
    // report if reload_defaults was called unnecessarily => this
    // should be considered a bug the whole point of reload_defaults
    // is to update defaults _based on current image_ any required
    // initialisation should go in init (and not be performed
    // repeatedly here)
    if(module->dev)
    {
      module->reload_defaults(module);
      dt_print(DT_DEBUG_PARAMS,
               "[dt_iop_reload_defaults] defaults reloaded for %s", module->op);
    }
    else
      dt_print(DT_DEBUG_PARAMS,
               "[dt_iop_reload_defaults] should not be called without image.");
  }

  dt_iop_load_default_params(module);

  if(darktable.gui)
    --darktable.gui->reset;

  if(module->header)
    dt_iop_gui_update_header(module);
}

void dt_iop_cleanup_histogram(gpointer data, gpointer user_data)
{
  dt_iop_module_t *module = data;

  free(module->histogram);
  module->histogram = NULL;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
}

int dt_iop_legacy_params(dt_iop_module_t *module,
                         const void *const old_params,
                         const int32_t old_params_size,
                         const int old_version,
                         void **new_params,
                         int new_version)
{
  int ret = 0;
  gboolean auto_init = FALSE;

  if(module->legacy_params)
  {
    int cversion = old_version;
    void *oparams = malloc(old_params_size);
    memcpy(oparams, old_params, old_params_size);
    void *params = NULL;
    int version = new_version;
    int32_t params_size = 0;

    while(cversion < new_version)
    {
      params = NULL;
      ret = module->legacy_params(module, oparams, cversion,
                                  &params, &params_size, &version);

      if(ret == 1)
      {
        free(oparams);
        return ret;
      }
      if(ret == -1)
        auto_init = TRUE;

      cversion = version;
      free(oparams);
      oparams = params;
    }

    if(params)
    {
      memcpy(*new_params, params, params_size);
      free(params);
    }
  }
  else
  {
    ret = 1;
  }

  return auto_init ? -1 : ret;
}

static void _init_presets(dt_iop_module_so_t *module_so)
{
  if(module_so->init_presets)
    module_so->init_presets(module_so);

  // this seems like a reasonable place to check for and update legacy
  // presets.

  const int32_t module_version = module_so->version();

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name, op_version, op_params, blendop_version, blendop_params"
      " FROM data.presets"
      " WHERE operation = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_so->op, -1, SQLITE_TRANSIENT);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    int32_t old_params_version = sqlite3_column_int(stmt, 1);
    const void *old_params = (void *)sqlite3_column_blob(stmt, 2);
    const int32_t old_params_size = sqlite3_column_bytes(stmt, 2);
    const int32_t old_blend_params_version = sqlite3_column_int(stmt, 3);
    const void *old_blend_params = (void *)sqlite3_column_blob(stmt, 4);
    const int32_t old_blend_params_size = sqlite3_column_bytes(stmt, 4);

    if(old_params_version == 0)
    {
      // this preset doesn't have a version.  go digging through the database
      // to find a history entry that matches the preset params, and get
      // the module version from that.

      sqlite3_stmt *stmt2;
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "SELECT module"
         " FROM main.history"
         " WHERE operation = ?1 AND op_params = ?2", -1,
         &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, module_so->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, old_params, old_params_size, SQLITE_TRANSIENT);

      if(sqlite3_step(stmt2) == SQLITE_ROW)
      {
        old_params_version = sqlite3_column_int(stmt2, 0);
      }
      else
      {
        dt_print
          (DT_DEBUG_ALWAYS,
           "[imageop_init_presets] WARNING: Could not find versioning information for '%s' "
           "preset '%s'\nUntil some is found, the preset will be unavailable.\n(To make it "
           "return, please load an image that uses the preset.)",
           module_so->op, name);
        sqlite3_finalize(stmt2);
        continue;
      }

      sqlite3_finalize(stmt2);

      // we found an old params version.  Update the database with it.

      dt_print(DT_DEBUG_PARAMS,
               "[imageop_init_presets] found version %d for '%s' preset '%s'",
               old_params_version, module_so->op, name);

      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "UPDATE data.presets"
         " SET op_version=?1"
         " WHERE operation=?2 AND name=?3", -1,
         &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, old_params_version);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, module_so->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, name, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
    }

    if(module_version > old_params_version
       && module_so->legacy_params != NULL)
    {
      // we need a dt_iop_module_t for legacy_params()
      dt_iop_module_t *module;
      module = calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        free(module);
        continue;
      }

      const int32_t new_params_size = module->params_size;
      void *new_params = calloc(1, new_params_size);

      gboolean auto_init = FALSE;

      if(old_params_size > 0)
      {
        // convert the old params to new
        const int legacy_ret =
          dt_iop_legacy_params(module, old_params, old_params_size, old_params_version,
                               &new_params, module_version);

        if(legacy_ret == 1)
        {
          // failed
          free(new_params);
          dt_iop_cleanup_module(module);
          free(module);
          continue;
        }
        else if(legacy_ret == -1)
          auto_init = TRUE;
      }
      else
        auto_init = TRUE;

      dt_print(DT_DEBUG_ALWAYS,
               "[imageop_init_presets] updating '%s' preset '%s'"
               " from version %d to version %d\nto:'%s'",
               module_so->op, name, old_params_version, module_version,
               dt_exif_xmp_encode(new_params, new_params_size, NULL));

      // and write the new params back to the database
      sqlite3_stmt *stmt2;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE data.presets"
                                  " SET op_version=?1, op_params=?2"
                                  " WHERE operation=?3 AND name=?4",
                                  -1, &stmt2, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, module->version());
      // legacy_ret == -1 means that this is to convert to an auto-init module
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2,
                                 auto_init ? NULL : new_params,
                                 auto_init ?    0 : new_params_size,
                                 SQLITE_TRANSIENT);
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
      dt_print(DT_DEBUG_ALWAYS,
               "[imageop_init_presets] Can't upgrade '%s' preset '%s' "
               "from version %d to %d, no legacy_params() implemented ",
               module_so->op, name, old_params_version, module_version);
    }

    if(!old_blend_params || dt_develop_blend_version() > old_blend_params_version)
    {
      dt_print(DT_DEBUG_ALWAYS,
              "[imageop_init_presets] updating '%s' preset '%s' from blendop"
               " version %d to version %d",
               module_so->op, name, old_blend_params_version, dt_develop_blend_version());

      // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
      // using dt_develop_blend_legacy_params_by_so won't help as we need "module" anyway
      dt_iop_module_t *module;
      module = calloc(1, sizeof(dt_iop_module_t));
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
         && dt_develop_blend_legacy_params
             (module, old_blend_params, old_blend_params_version,
              new_blend_params, dt_develop_blend_version(),
              old_blend_params_size) == 0)
      {
        // do nothing
      }
      else
      {
        memcpy(new_blend_params, module->default_blendop_params,
               sizeof(dt_develop_blend_params_t));
      }

      // and write the new blend params back to the database
      sqlite3_stmt *stmt2;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE data.presets"
                                  " SET blendop_version=?1, blendop_params=?2"
                                  " WHERE operation=?3 AND name=?4",
                                  -1, &stmt2, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, dt_develop_blend_version());
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, new_blend_params,
                                 sizeof(dt_develop_blend_params_t),
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

static void _iop_preferences_changed(gpointer instance, gpointer self)
{
  // reload presets if they are based on the actual workflow which
  // could have been changed after editing the preferences.

  GList *iop = (GList *)self;

  while(iop)
  {
    dt_iop_module_so_t *mod = iop->data;

    if(mod->pref_based_presets)
    {
      sqlite3_stmt *stmt;
      // first delete auto built-in presets for this module
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "DELETE FROM data.presets"
         " WHERE writeprotect = 1"
         "   AND operation = ?1",
         -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, mod->op, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      // and reload whatever new presets are needed for the new workflow
      _init_presets(mod);
    }

    iop = g_list_next(iop);
  }
}

static void _init_presets_actions(dt_iop_module_so_t *module)
{
  /** load shortcuts for presets **/
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name"
                              " FROM data.presets"
                              " WHERE operation=?1"
                              " ORDER BY writeprotect DESC, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_action_define_preset(&module->actions, (const char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
}

static void _init_module_so(void *m)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;

  _init_presets(module);

  // do not init accelerators if there is no gui
  if(darktable.gui)
  {
    module->actions = (dt_action_t){ DT_ACTION_TYPE_IOP, module->op, module->name() };
    dt_action_insert_sorted(&darktable.control->actions_iops, &module->actions);

    // Calling the accelerator initialization callback, if present
    _init_presets_actions(module);

    // create a gui and have the widgets register their accelerators
    dt_iop_module_t *module_instance = calloc(1, sizeof(dt_iop_module_t));

    if(module->gui_init
       && !dt_iop_load_module_by_so(module_instance, module, NULL))
    {
      darktable.control->accel_initialising = TRUE;
      dt_iop_gui_init(module_instance);

      static gboolean blending_accels_initialized = FALSE;
      if(!blending_accels_initialized)
      {
        dt_iop_colorspace_type_t cst = module->blend_colorspace(module_instance, NULL, NULL);

        if((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) &&
           !(module->flags() & IOP_FLAGS_NO_MASKS) &&
           (cst == IOP_CS_LAB || cst == IOP_CS_RGB))
        {
          GtkWidget *iopw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
          dt_iop_gui_init_blending(iopw, module_instance);
          dt_iop_gui_cleanup_blending(module_instance);
          gtk_widget_destroy(iopw);

          blending_accels_initialized = TRUE;
        }
      }

      dt_iop_gui_cleanup_module(module_instance);
      dt_iop_cleanup_module(module_instance);

      darktable.control->accel_initialising = FALSE;
    }

    free(module_instance);
  }
}

void dt_iop_load_modules_so(void)
{
  darktable.iop = dt_module_load_modules
    ("/plugins", sizeof(dt_iop_module_so_t),
     dt_iop_load_module_so, _init_module_so, NULL);

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _iop_preferences_changed, darktable.iop);
}

gboolean dt_iop_load_module(dt_iop_module_t *module,
                            dt_iop_module_so_t *module_so,
                            dt_develop_t *dev)
{
  memset(module, 0, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, dev))
  {
    free(module);
    return TRUE;
  }
  return FALSE;
}

GList *dt_iop_load_modules_ext(dt_develop_t *dev, const gboolean no_image)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dt_iop_module_so_t *module_so;
  dev->iop_instance = 0;
  GList *iop = darktable.iop;
  while(iop)
  {
    module_so = iop->data;
    module = calloc(1, sizeof(dt_iop_module_t));
    if(dt_iop_load_module_by_so(module, module_so, dev))
    {
      free(module);
      continue;
    }
    res = g_list_insert_sorted(res, module, dt_sort_iop_by_order);
    module->global_data = module_so->data;
    module->so = module_so;
    iop = g_list_next(iop);
  }

  GList *it = res;
  while(it)
  {
    module = it->data;
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

  // don't have a picker pointing to a disappeared module
  if(darktable.lib
     && darktable.lib->proxy.colorpicker.picker_proxy
     && darktable.lib->proxy.colorpicker.picker_proxy->module == module)
    darktable.lib->proxy.colorpicker.picker_proxy = NULL;

  free(module->histogram);
  module->histogram = NULL;
  g_hash_table_destroy(module->raster_mask.source.users);
  g_hash_table_destroy(module->raster_mask.source.masks);
  module->raster_mask.source.users = NULL;
  module->raster_mask.source.masks = NULL;
}

void dt_iop_unload_modules_so()
{
  DT_CONTROL_SIGNAL_DISCONNECT(_iop_preferences_changed, darktable.iop);

  while(darktable.iop)
  {
    dt_iop_module_so_t *module = darktable.iop->data;
    if(module->cleanup_global)
      module->cleanup_global(module);
    if(module->module)
      g_module_close(module->module);
    free(darktable.iop->data);
    darktable.iop = g_list_delete_link(darktable.iop, darktable.iop);
  }
}

void dt_iop_advertise_rastermask(dt_iop_module_t *module, const int mask_mode)
{
  static const int key = BLEND_RASTER_ID;
  // we don't advertise raster masks but should use the original source instead
  if(mask_mode & DEVELOP_MASK_ENABLED && !(mask_mode & DEVELOP_MASK_RASTER))
  {
    gchar *modulename = dt_history_item_get_name(module);
    if(g_hash_table_insert(module->raster_mask.source.masks, GINT_TO_POINTER(key), modulename))
    {
      dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MASKS | DT_DEBUG_VERBOSE,
        "raster mask advertised", NULL, module, DT_DEVICE_NONE, NULL, NULL);
    }
  }
  else
  {
    if(g_hash_table_remove(module->raster_mask.source.masks, GINT_TO_POINTER(key)))
    {
      dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MASKS | DT_DEBUG_VERBOSE,
        "NO raster mask support", NULL, module, DT_DEVICE_NONE, NULL, NULL);
    }
  }
}

/* make sure that blend_params are in sync with the iop struct
   1. Handling of raster mask users must only be done if we don't use module's default
      blending parameters.
   2. Also watch out for a raster mask source module to get it's first `target`
      entry, if so we should invalidate all cachelines from modules with a higher iop order
      in dt_iop_commit_blend_params() callers.
      To support this, dt_iop_commit_blend_params() either returns NULL or the source module.
*/
dt_iop_module_t *dt_iop_commit_blend_params(dt_iop_module_t *module,
                                            const dt_develop_blend_params_t *blendop_params)
{
  memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
  if(blendop_params->blend_cst == DEVELOP_BLEND_CS_NONE)
  {
    module->blend_params->blend_cst =
      dt_develop_blend_default_module_blend_colorspace(module);
  }
  dt_iop_advertise_rastermask(module, blendop_params->mask_mode);

  // If we use default blending parameters or don't have a dev
  // we don't manage raster mask users and set all stuff to defaults
  if(blendop_params == module->default_blendop_params
     || module->dev == NULL)
  {
    module->raster_mask.sink.source = NULL;
    module->raster_mask.sink.id = INVALID_MASKID;
    return NULL;
  }

  for(GList *iter = module->dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *candidate = iter->data;
    if(dt_iop_module_is(candidate->so, blendop_params->raster_mask_source))
    {
      if(candidate->multi_priority == blendop_params->raster_mask_instance)
      {
        const gboolean new = g_hash_table_insert(candidate->raster_mask.source.users,
                            module,
                            GINT_TO_POINTER(blendop_params->raster_mask_id));
        module->raster_mask.sink.source = candidate;
        module->raster_mask.sink.id = blendop_params->raster_mask_id;
        dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MASKS,
                      "request raster mask",
                      NULL, module, DT_DEVICE_NONE, NULL, NULL, "from '%s%s' %s",
                      candidate->op, dt_iop_get_instance_id(candidate),
                      new ? "new" : "replaced");

        return candidate;
      }
    }
  }

  /* We don't use a raster mask as source so we will remove this
     module as a user from the hash table and set sink source and id
     to default == 'nothing'
  */
  dt_iop_module_t *sink_source = module->raster_mask.sink.source;
  if(sink_source)
  {
    if(g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module))
      dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MASKS,
                  "clear as raster user",
                  NULL, module, DT_DEVICE_NONE, NULL, NULL, "from '%s%s'",
                  sink_source->op, dt_iop_get_instance_id(sink_source));
  }
  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = INVALID_MASKID;
  return NULL;
}

gboolean _iop_validate_params(dt_introspection_field_t *field,
                              gpointer params,
                              const gboolean report,
                              const char *name)
{
  dt_iop_params_t *p = ((uint8_t *)params + field->header.offset);

  gboolean all_ok = TRUE;
  gchar *values = NULL;

  switch(field->header.type)
  {
  case DT_INTROSPECTION_TYPE_STRUCT:
    for(int i = 0; i < field->Struct.entries; i++)
    {
      dt_introspection_field_t *entry = field->Struct.fields[i];

      all_ok &= _iop_validate_params(entry, params, report, name);
    }
    break;
  case DT_INTROSPECTION_TYPE_UNION:
    all_ok = FALSE;
    for(int i = field->Union.entries - 1; i >= 0 ; i--)
    {
      dt_introspection_field_t *entry = field->Union.fields[i];

      if(_iop_validate_params(entry, params, report && i == 0, name))
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
          dt_print(DT_DEBUG_ALWAYS,
             "[iop_validate_params] `%s' failed for not null terminated type string"
             " \"%s\";",
               name, field->header.type_name);
        all_ok = FALSE;
      }
    }
    else
    {
      for(int i = 0, item_offset = 0;
          i < field->Array.count;
          i++, item_offset += field->Array.field->header.size)
      {
        if(!_iop_validate_params(field->Array.field,
                                 (uint8_t *)params + item_offset, report, name))
        {
          if(report)
            dt_print(DT_DEBUG_ALWAYS, "[iop_validate_params] `%s' failed"
                   " for type \"%s\", for array element \"%d\"",
                     name, field->header.type_name, i);
          all_ok = FALSE;
          break;
        }
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_FLOAT:
    all_ok = dt_isnan(*(float*)p)
          || dt_isinf(*(float*)p)
          || (*(float*)p >= field->Float.Min && *(float*)p <= field->Float.Max);
    gboolean relaxed = FALSE;
    if(!all_ok)
    {
      const float eps = 1.00001f;
      if(*(float*)p >= field->Float.Min / eps && *(float*)p <= field->Float.Max * eps)
      {
        all_ok = TRUE;
        relaxed = TRUE;
      }
    }
    values = g_strdup_printf
      (" (%.8f - [%f..%f] : default %f)%s",
       *(float*)p,
       field->Float.Min, field->Float.Max,
       field->Float.Default,
       relaxed ? " relaxed range" : "");
    break;
  case DT_INTROSPECTION_TYPE_INT:
    all_ok = (*(int*)p >= field->Int.Min && *(int*)p <= field->Int.Max);
    values = g_strdup_printf
       (" (%d - [%d..%d] : default %d)",
        *(int*)p,
        field->Int.Min, field->Int.Max,
        field->Int.Default);
    break;
  case DT_INTROSPECTION_TYPE_UINT:
    all_ok = (*(unsigned int*)p >= field->UInt.Min
              && *(unsigned int*)p <= field->UInt.Max);
    values = g_strdup_printf
      (" (%ud - [%ud..%ud] : default %ud)",
       *(unsigned int*)p,
       field->UInt.Min, field->UInt.Max,
       field->UInt.Default);
    break;
  case DT_INTROSPECTION_TYPE_USHORT:
    all_ok = (*(unsigned short int*)p >= field->UShort.Min
              && *(unsigned short int*)p <= field->UShort.Max);
    values = g_strdup_printf
      (" (%ud - [%ud..%ud] : default %ud)",
       *(unsigned short int*)p,
       field->UShort.Min, field->UShort.Max,
       field->UShort.Default);
    break;
  case DT_INTROSPECTION_TYPE_INT8:
    all_ok = (*(uint8_t*)p >= field->Int8.Min && *(uint8_t*)p <= field->Int8.Max);
    values = g_strdup_printf
      (" (%ud - [%ud..%ud] : default %ud)",
       *(uint8_t*)p,
       field->Int8.Min, field->Int8.Max,
       field->Int8.Default);
    break;
  case DT_INTROSPECTION_TYPE_CHAR:
    all_ok = (*(char*)p >= field->Char.Min && *(char*)p <= field->Char.Max);
    values = g_strdup_printf
      (" (%c - [%c..%c] : default %c)",
       *(char*)p,
       field->Char.Min, field->Char.Max,
       field->Char.Default);
    break;
  case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
    all_ok = creal(*(float complex*)p) >= creal(field->FloatComplex.Min) &&
             creal(*(float complex*)p) <= creal(field->FloatComplex.Max) &&
             cimag(*(float complex*)p) >= cimag(field->FloatComplex.Min) &&
             cimag(*(float complex*)p) <= cimag(field->FloatComplex.Max);
    break;
  case DT_INTROSPECTION_TYPE_ENUM:
    all_ok = FALSE;
    for(dt_introspection_type_enum_tuple_t *i = field->Enum.values;
        i && i->name;
        i++)
    {
      if(i->value == *(int*)p)
      {
        all_ok = TRUE;
        break;
      }
    }
    values = g_strdup_printf
      (" (%d)", *(int*)p);
    break;
  case DT_INTROSPECTION_TYPE_BOOL:
    // *(gboolean*)p
    values = g_strdup_printf
      (" (%d)", *(gboolean*)p);
    break;
  case DT_INTROSPECTION_TYPE_OPAQUE:
    // TODO: special case float2
    break;
  default:
    dt_print(DT_DEBUG_ALWAYS,
             "[iop_validate_params] `%s' unsupported introspection type \"%s\" encountered,"
             " (field %s)",
             name, field->header.type_name, field->header.name);
    all_ok = FALSE;
    break;
  }

  if(all_ok)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[iop_validate_params] `%s' validated data for type \"%s\"%s%s%s",
             name, field->header.type_name,
             *field->header.name ? ", field: " : "",
             field->header.name, values ? values : "");
  }
  else if(report)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[iop_validate_params] `%s' failed for type \"%s\"%s%s%s",
             name, field->header.type_name,
             *field->header.name ? ", field: " : "",
             field->header.name, values ? values : "");
  }
  g_free(values);

  return all_ok;
}

void dt_iop_commit_params(dt_iop_module_t *module,
                          dt_iop_params_t *params,
                          dt_develop_blend_params_t *blendop_params,
                          dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->blendop_data, blendop_params, sizeof(dt_develop_blend_params_t));

  /* 1. make blendop_params available dt_iop_module_t struct.
        Also checks for a new source raster mask module.
        This is important as we can't know from here if that raster mask is actually
        available.
        We handle this case by partly invalidating the cache to enforce a valid raster.
  */
  dt_iop_module_t *new_raster = dt_iop_commit_blend_params(module, blendop_params);

#ifdef HAVE_OPENCL
  // assume process_cl is ready, commit_params can overwrite this.
  if(module->process_cl)
    piece->process_cl_ready = TRUE;
#endif

  // register if module allows tiling, commit_params can overwrite this.
  if(module->flags() & IOP_FLAGS_ALLOW_TILING)
    piece->process_tiling_ready = TRUE;

  if((piece->enabled || module->enabled) // better to check for both
    && module->so->get_introspection()
    && darktable.unmuted & DT_DEBUG_PARAMS)
  {
    if(!_iop_validate_params(module->so->get_introspection()->field,
                             params,
                             TRUE,
                             module->so->op))
    {
      // FIXME maybe we can hint a warning in the module UI or possibly
      // disable the piece ? Requires more introspection fixes (especially enums) to be safe
      dt_control_log
        (_("'%s' has an introspection error"), module->op);
    }
  }

  module->commit_params(module, params, pipe, piece);

  dt_hash_t phash = 0;
  // 2. compute the hash only if piece is enabled
  if(piece->enabled)
  {
    phash = dt_hash(DT_INITHASH, &module->so->op, strlen(module->so->op));
    phash = dt_hash(phash, &module->instance, sizeof(int32_t));
    phash = dt_hash(phash, module->params, module->params_size);

    /* We have to take blending parameters into account for the hash if
        a) there is some blending active detected via the mask_mode or
        b) we have a blending module in focus so we have valid cachelines
    */
    const gboolean is_blending = module->flags() & IOP_FLAGS_SUPPORTS_BLENDING
          && (blendop_params->mask_mode != DEVELOP_MASK_DISABLED
              || dt_dev_gui_module() == module);
    if(is_blending)
    {
      phash = dt_hash(phash, blendop_params, sizeof(dt_develop_blend_params_t));

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, blendop_params->mask_id);
      if(grp)
      {
        const size_t mlen = dt_masks_group_get_hash_buffer_length(grp);
        char *str = malloc(mlen);
        if(str)
        {
          dt_masks_group_get_hash_buffer(grp, str);
          phash = dt_hash(phash, str, mlen);
          free(str);
        }
      }

      if(blendop_params->mask_mode & DEVELOP_MASK_RASTER && new_raster)
        dt_dev_pixelpipe_cache_invalidate_later(pipe, new_raster->iop_order);
    }
  }
  piece->hash = phash;
}

void dt_iop_gui_cleanup_module(dt_iop_module_t *module)
{
  g_slist_free_full(module->widget_list, g_free);
  module->widget_list = NULL;
  module->gui_cleanup(module);
  gtk_widget_destroy(module->expander ?: module->widget);
  dt_iop_gui_cleanup_blending(module);
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  ++darktable.gui->reset;
  if(!dt_iop_is_hidden(module))
  {
    if(module->gui_data)
    {
      dt_bauhaus_update_from_field(module, NULL, NULL, NULL);

      if(module->params && module->gui_update)
      {
        if(module->widget && dt_conf_get_bool("plugins/darkroom/show_warnings"))
        {
          GtkWidget *label_widget = dt_gui_container_first_child
            (GTK_CONTAINER(gtk_widget_get_parent(module->widget)));
          if(!g_strcmp0(gtk_widget_get_name(label_widget), "iop-plugin-warning"))
            gtk_widget_destroy(label_widget);
          module->has_trouble = FALSE;
        }
        module->gui_update(module);
      }
      dt_iop_gui_update_blending(module);
      dt_iop_gui_update_expanded(module);
    }
    dt_iop_gui_update_header(module);
    dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
    dt_guides_update_module_widget(module);

    // this signal must be raised only safely when the darkroom and history
    // if available.
    if(!darktable.develop->history_updating
       && !darktable.develop->gui_synch
       && module->operation_tags() & IOP_TAG_DISTORT)
    {
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_DISTORT);
    }
  }
  --darktable.gui->reset;
}

void dt_iop_gui_reset(dt_iop_module_t *module)
{
  ++darktable.gui->reset;
  if(module->gui_reset && !dt_iop_is_hidden(module))
    module->gui_reset(module);
  --darktable.gui->reset;
}

static gboolean _gui_reset_callback(GtkButton *button,
                                    GdkEventButton *event,
                                    dt_iop_module_t *module)
{
  // never use the callback if module is always disabled
  const gboolean disabled = !module->default_enabled && module->hide_enable_button;
  if(disabled) return FALSE;

  // Ctrl is used to apply any auto-presets to the current module
  // If Ctrl was not pressed, or no auto-presets were applied, reset the module parameters
  if(!(event
       && dt_modifier_is(event->state, GDK_CONTROL_MASK))
     || !dt_gui_presets_autoapply_for_module(module, NULL))
  {
    // if a drawn mask is set, remove it from the list
    if(dt_is_valid_maskid(module->blend_params->mask_id))
    {
      dt_masks_form_t *grp =
        dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
      if(grp) dt_masks_form_remove(module, NULL, grp);
    }
    /* reset to default params */
    dt_iop_reload_defaults(module);
    dt_iop_commit_blend_params(module, module->default_blendop_params);

    /* reset ui to its defaults */
    dt_iop_gui_reset(module);

    /* update ui to default params*/
    dt_iop_gui_update(module);

    dt_dev_add_history_item(module->dev, module, TRUE);
  }

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);

  return TRUE;
}

static gboolean _presets_popup_callback(GtkButton *button,
                                        GdkEventButton *event,
                                        dt_iop_module_t *module)
{
  const gboolean disabled = !module->default_enabled && module->hide_enable_button;
  if(disabled) return FALSE;

  GtkMenu *menu = dt_gui_presets_popup_menu_show_for_module(module);

  g_signal_connect(G_OBJECT(menu), "deactivate",
                   G_CALLBACK(_header_menu_deactivate_callback), module);

  dt_gui_menu_popup(menu,
                    GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);

  return TRUE;
}

static gboolean _presets_scroll_callback(GtkWidget *widget,
                                         GdkEventScroll *event,
                                         dt_iop_module_t *module)
{
  int delta_y = 0;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
    dt_gui_presets_apply_adjacent_preset(module, delta_y);

  return TRUE;
}

void dt_iop_request_focus(dt_iop_module_t *module)
{
  dt_develop_t *dev = darktable.develop;
  dt_iop_module_t *out_focus_module = dev->gui_module;

  // disable global color picker on any focus change request
  // unless restrict histogram is active
  if(!darktable.lib->proxy.colorpicker.restrict_histogram)
    dt_iop_color_picker_reset(NULL, TRUE);

  if(darktable.gui->reset
     || (out_focus_module == module))
    return;

  dev->gui_module = module;
  dev->focus_hash = TRUE;

  /* lets lose the focus of previous focus module*/
  if(out_focus_module)
  {
    if(out_focus_module->gui_focus)
      out_focus_module->gui_focus(out_focus_module, FALSE);

    dt_iop_color_picker_reset(out_focus_module, TRUE);

    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(out_focus_module),
                               GTK_STATE_FLAG_NORMAL, TRUE);

    dt_iop_connect_accels_multi(out_focus_module->so);

    /* reset mask view */
    dt_masks_reset_form_gui();

    /* do stuff needed in the blending gui */
    dt_iop_gui_blending_lose_focus(out_focus_module);

    /* redraw the expander */
    gtk_widget_queue_draw(out_focus_module->expander);

    /* and finally collection restore hinter messages */
    dt_collection_hint_message(darktable.collection);

    // we also remove the focus css class
    GtkWidget *iop_w = gtk_widget_get_parent(dt_iop_gui_get_pluginui(out_focus_module));
    dt_gui_remove_class(iop_w, "dt_module_focus");
  }

  /* set the focus on module */
  if(module)
  {
    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(module),
                               GTK_STATE_FLAG_SELECTED, TRUE);

    dt_iop_connect_accels_multi(module->so);

    if(module->gui_focus)
      module->gui_focus(module, TRUE);

    /* redraw the expander */
    gtk_widget_queue_draw(module->expander);

    // we also add the focus css class
    GtkWidget *iop_w =
      gtk_widget_get_parent(dt_iop_gui_get_pluginui(dev->gui_module));
    dt_gui_add_class(iop_w, "dt_module_focus");

    // update last preset name to get the update preset entry
    gboolean writeprotect = FALSE;
    gchar *name = dt_get_active_preset_name(module, &writeprotect);
    if(!writeprotect && name)
      dt_gui_store_last_preset(name);
    g_free(name);
  }

  /* update sticky accels window */
  if(darktable.view_manager->accels_window.window
     && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  const int tags_filter = (out_focus_module ? out_focus_module->operation_tags_filter() : 0)
                        | (module ? module->operation_tags_filter() : 0);
  if(tags_filter)
  {
    dt_dev_pixelpipe_rebuild(dev);
  }

  // update guides button state
  dt_guides_update_button_state();

  dt_control_change_cursor(GDK_LEFT_PTR);
  dt_control_queue_redraw_center();
}

/*
 * NEW EXPANDER
 */

static void _gui_set_single_expanded(dt_iop_module_t *module, gboolean expanded)
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

void dt_iop_gui_set_expanded(dt_iop_module_t *module,
                             const gboolean expanded,
                             const gboolean collapse_others)
{
  if(!module->expander) return;
  /* handle shiftclick on expander, hide all except this */
  if(collapse_others)
  {
    const int current_group = dt_dev_modulegroups_get_activated(module->dev);
    const gboolean group_only = dt_conf_get_bool("darkroom/ui/single_module_group_only");

    GList *iop = module->dev->iop;
    gboolean all_other_closed = TRUE;
    while(iop)
    {
      dt_iop_module_t *m = iop->data;
      if(m != module && (dt_iop_shown_in_group(m, current_group) || !group_only))
      {
        all_other_closed = all_other_closed && !m->expanded;
        _gui_set_single_expanded(m, FALSE);
      }

      iop = g_list_next(iop);
    }
    if(all_other_closed)
      _gui_set_single_expanded(module, !module->expanded);
    else
      _gui_set_single_expanded(module, TRUE);
  }
  else
  {
    /* else just toggle */
    _gui_set_single_expanded(module, expanded);
  }
}

void dt_iop_gui_update_expanded(dt_iop_module_t *module)
{
  if(!module->expander) return;

  const gboolean expanded = module->expanded;

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);
}

static gboolean _iop_plugin_body_button_press(GtkWidget *w,
                                              GdkEventButton *e,
                                              gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(e->button == 1)
  {
    dt_iop_request_focus(module);
    return TRUE;
  }
  else if(e->button == 3)
  {
    _presets_popup_callback(NULL, NULL, module);

    return TRUE;
  }
  return FALSE;
}

static gboolean _iop_plugin_header_button_press(GtkWidget *w,
                                                GdkEventButton *e,
                                                gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return TRUE;

  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if(e->button == 1)
  {
    if(dt_modifier_is(e->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
      ; // do nothing (for easier dragging)
    else if(dt_modifier_is(e->state, GDK_CONTROL_MASK))
    {
      dt_iop_gui_rename_module(module);
      return TRUE;
    }
    else
    {
      const gboolean collapse_others =
        !dt_conf_get_bool("darkroom/ui/single_module")
        != (!dt_modifier_is(e->state, GDK_SHIFT_MASK));

      dt_iop_gui_set_expanded(module, !module->expanded, collapse_others);

      // rebuild the accelerators
      dt_iop_connect_accels_multi(module->so);

      //used to take focus away from module search text input box when module selected
      gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

      return TRUE;
    }
  }
  else if(e->button == 3)
  {
    _presets_popup_callback(NULL, NULL, module);

    return TRUE;
  }
  return FALSE;
}

static void _header_size_callback(GtkWidget *widget,
                                  GdkRectangle *allocation,
                                  GtkWidget *header)
{
  gchar *config = dt_conf_get_string("darkroom/ui/hide_header_buttons");

  GList *children = gtk_container_get_children(GTK_CONTAINER(header));

  const gint panel_trigger_width = 250;

  GList *button = children;
  GtkRequisition button_size;
  gtk_widget_show(GTK_WIDGET(button->data));
  gtk_widget_get_preferred_size(GTK_WIDGET(button->data), &button_size, NULL);

  int num_buttons = 0;
  for(button = g_list_last(children);
      button && GTK_IS_BUTTON(button->data);
      button = g_list_previous(button)) num_buttons++;

  gboolean hide_all = (allocation->width == 1);
  int num_to_unhide = (allocation->width - 2) / button_size.width;
  double opacity_leftmost = num_to_unhide > 0
    ? 1.0
    : (double) allocation->width / button_size.width;

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
          opacity_leftmost = opacity_others =
            (total_alloc.width - panel_trigger_width) / 100.;
        }
        else
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[header size callback] unknown darkroom/ui/hide_header_buttons"
                   " option %s", config);
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

  dt_gui_widget_reallocate_now(header);
}

gboolean dt_iop_show_hide_header_buttons(dt_iop_module_t *module,
                                         GdkEventCrossing *event,
                                         gboolean show_buttons,
                                         const gboolean always_hide)
{
  // check if Entry widget for module name edit exists
  GtkWidget *header = module->header;
  GtkWidget *focused = gtk_container_get_focus_child(GTK_CONTAINER(header));
  if(focused && GTK_IS_ENTRY(focused)) return TRUE;

  if(event && (darktable.develop->darkroom_skip_mouse_events ||
     event->detail == GDK_NOTIFY_INFERIOR ||
     event->mode != GDK_CROSSING_NORMAL)) return TRUE;

  const char *config = dt_conf_get_string_const("darkroom/ui/hide_header_buttons");

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

  const gboolean disabled = !module->default_enabled && module->hide_enable_button;

  GList *children = gtk_container_get_children(GTK_CONTAINER(header));

  GList *button;
  for(button = g_list_last(children);
      button && GTK_IS_BUTTON(button->data);
      button = g_list_previous(button))
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(button->data), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(button->data), show_buttons && !always_hide && !disabled);
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
      g_signal_connect(G_OBJECT(space), "size-allocate",
                       G_CALLBACK(_header_size_callback), header);
    }
  }

  g_list_free(children);

  if(dynamic && !show_buttons && !always_hide)
  {
    GdkRectangle fake_allocation = {.width = UINT16_MAX};
    _header_size_callback(NULL, &fake_allocation, header);
  }

  return TRUE;
}

static void _display_mask_indicator_callback(GtkToggleButton *bt, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  const gboolean is_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bt));
  const dt_iop_gui_blend_data_t *bd = module->blend_data;

  module->request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_MASK;
  module->request_mask_display |= (is_active ? DT_DEV_PIXELPIPE_DISPLAY_MASK : DT_DEV_PIXELPIPE_DISPLAY_NONE);

  // set the module show mask button too
  if(bd->showmask)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), is_active);

  dt_iop_request_focus(module);
  dt_iop_refresh_center(module);
}

static gboolean _mask_indicator_tooltip(GtkWidget *treeview,
                                        gint x,
                                        gint y,
                                        gboolean kb_mode,
                                        GtkTooltip* tooltip,
                                        dt_iop_module_t *module)
{
  gboolean res = FALSE;
  const gboolean raster = module->blend_params->mask_mode & DEVELOP_MASK_RASTER;
  if(module->mask_indicator)
  {
    gchar *type = _("unknown mask");
    gchar *text;
    const uint32_t mm = module->blend_params->mask_mode;
    if((mm & DEVELOP_MASK_MASK) && (mm & DEVELOP_MASK_CONDITIONAL))
      type=_("drawn + parametric mask");
    else if(mm & DEVELOP_MASK_MASK)
      type=_("drawn mask");
    else if(mm & DEVELOP_MASK_CONDITIONAL)
      type=_("parametric mask");
    else if(mm & DEVELOP_MASK_RASTER)
      type=_("raster mask");
    else
      dt_print(DT_DEBUG_PARAMS, "unknown mask mode '%u' in module '%s'", mm, module->op);
    gchar *part1 = g_strdup_printf(_("this module has a `%s'"), type);
    gchar *part2 = NULL;
    if(raster && module->raster_mask.sink.source)
    {
      gchar *source = dt_history_item_get_name(module->raster_mask.sink.source);
      part2 = g_strdup_printf(_("taken from module %s"), source);
      g_free(source);
    }

    if(!raster && !part2)
      part2 = g_strdup(_("click to display (module must be activated first)"));

    if(part2)
      text = g_strconcat(part1, "\n", part2, NULL);
    else
      text = g_strdup(part1);

    gtk_tooltip_set_text(tooltip, text);
    res = TRUE;
    g_free(part1);
    g_free(part2);
    g_free(text);
  }
  return res;
}

void dt_iop_add_remove_mask_indicator(dt_iop_module_t *module, gboolean add)
{
  const gboolean show = add && dt_conf_get_bool("darkroom/ui/show_mask_indicator");

  if(module->mask_indicator)
  {
    if(!show)
    {
      gtk_widget_destroy(module->mask_indicator);
      module->mask_indicator = NULL;
      dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
    }
    else
      gtk_widget_set_sensitive(module->mask_indicator, module->enabled);
  }
  else if(show)
  {
    module->mask_indicator = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, 0, NULL);
    dt_gui_add_class(module->mask_indicator, "dt_transparent_background");
    g_signal_connect(G_OBJECT(module->mask_indicator), "toggled",
                     G_CALLBACK(_display_mask_indicator_callback), module);
    g_signal_connect(G_OBJECT(module->mask_indicator), "query-tooltip",
                     G_CALLBACK(_mask_indicator_tooltip), module);
    gtk_widget_set_has_tooltip(module->mask_indicator, TRUE);
    gtk_widget_set_sensitive(module->mask_indicator, module->enabled);
    gtk_box_pack_end(GTK_BOX(module->header), module->mask_indicator, FALSE, FALSE, 0);

    // in dynamic modes, we need to put the mask indicator after the drawing area
    GList *children = gtk_container_get_children(GTK_CONTAINER(module->header));
    GList *child;

    for(child = g_list_last(children);
        child && GTK_IS_BUTTON(child->data);
        child = g_list_previous(child));

    if(GTK_IS_DRAWING_AREA(child->data))
    {
      GValue position = G_VALUE_INIT;
      g_value_init (&position, G_TYPE_INT);
      gtk_container_child_get_property(GTK_CONTAINER(module->header),
                                       child->data ,"position", &position);
      gtk_box_reorder_child(GTK_BOX(module->header), module->mask_indicator,
                            g_value_get_int(&position));
    }
    g_list_free(children);

    dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
  }
}

static void _iop_tooltip_reposition(GtkWidget *widget,
                                    GdkRectangle *allocation,
                                    gpointer user_data)
{
  GdkWindow *window = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  if(!window) return;

  gtk_widget_get_allocation(user_data, allocation);
  gtk_widget_translate_coordinates(user_data, gtk_widget_get_toplevel(user_data),
                                   0, 0, &allocation->x, &allocation->y);
  gdk_window_move_to_rect(window, allocation, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH,
                          GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE_X, 0, 0);
}

gboolean _iop_tooltip_callback(GtkWidget *widget,
                               const gint x,
                               const gint y,
                               const gboolean keyboard_mode,
                               GtkTooltip *tooltip,
                               gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  const char **des = module->description(module);

  if(!des) return FALSE;

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(10));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_hexpand(grid, FALSE);

  GtkWidget *label = gtk_label_new(des[0]?des[0]:"");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  // if there is no more description, do not add a separator
  if(des[1]) dt_gui_add_class(label, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

  const char *icon_purpose = "⟳";
  const char *icon_input   = "⇥";
  const char *icon_process = "⟴";
  const char *icon_output  = "↦";

  const char *icons[4] = {icon_purpose, icon_input, icon_process, icon_output};
  const char *ilabs[4] = {_("purpose"), _("input"), _("process"), _("output")};

  for(int k=1; k<5; k++)
  {
    if(des[k])
    {
      label = gtk_label_new(icons[k-1]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 0, k, 1, 1);

      label = gtk_label_new(ilabs[k-1]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 1, k, 1, 1);

      label = gtk_label_new(":");
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 2, k, 1, 1);

      label = gtk_label_new(des[k]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 3, k, 1, 1);
    }
  }

  gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(vbox), "size-allocate",
                   G_CALLBACK(_iop_tooltip_reposition), module->header);

  return dt_shortcut_tooltip_callback(widget, x, y, keyboard_mode, tooltip, vbox);
}

GtkWidget *dt_iop_gui_header_button(dt_iop_module_t *module,
                                    DTGTKCairoPaintIconFunc paint,
                                    dt_action_element_t element,
                                    GtkWidget *header)
{
  GtkWidget *button;
  gpointer callback = _gui_multiinstance_callback;

  if(element == DT_ACTION_ELEMENT_ENABLE)
  {
    button = dtgtk_togglebutton_new(paint, 0, module);
    callback = _gui_off_button_press;

    char tooltip[512];
    gchar *module_label = dt_history_item_get_name(module);
    snprintf(tooltip, sizeof(tooltip),
            module->enabled ? _("'%s' is switched on") : _("'%s' is switched off"),
            module_label);
    g_free(module_label);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), module->enabled);

    g_signal_connect(button, "toggled", G_CALLBACK(_gui_off_callback), module);
    gtk_box_pack_start(GTK_BOX(header), button, FALSE, FALSE, 0);
  }
  else
  {
    button = dtgtk_button_new(paint, 0, NULL);
    if(element == DT_ACTION_ELEMENT_RESET)
    {
      callback = _gui_reset_callback;
      gtk_widget_set_tooltip_text
        (button, _("reset parameters\nctrl+click to reapply any automatic presets"));
    }
    else if(element == DT_ACTION_ELEMENT_PRESETS)
    {
      callback = _presets_popup_callback;
      g_signal_connect(button, "scroll-event",
                      G_CALLBACK(_presets_scroll_callback), module);
      gtk_widget_add_events(button, darktable.gui->scroll_mask);
    }
    gtk_box_pack_end(GTK_BOX(header), button, FALSE, FALSE, 0);
  }
  g_signal_connect(button, "enter-notify-event",
                   G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(element));
  g_signal_connect(button, "button-press-event", G_CALLBACK(callback), module);
  dt_action_define(&module->so->actions, NULL, NULL, button, NULL);
  gtk_widget_show(button);

  return button;
}

static gboolean _on_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y, guint time, dt_iop_module_t *dest)
{
  gdk_drag_status(dc, 0, time);
  dtgtk_expander_set_drag_hover(DTGTK_EXPANDER(widget), FALSE, TRUE, time);

  GtkWidget *src_header = gtk_drag_get_source_widget(dc);
  if(!src_header) return TRUE;
  GtkWidget *src_expander = gtk_widget_get_ancestor(src_header, DTGTK_TYPE_EXPANDER);

  dt_iop_module_t *src = NULL;
  for(GList *iop = darktable.develop->iop; iop; iop = iop->next)
    if(((dt_iop_module_t *)iop->data)->expander == src_expander)
      src = iop->data;
  if(!src || dest == src) return TRUE;

  const gboolean above = y < gtk_widget_get_allocated_height(dest->header);

  if(above ^ (dest->iop_order > src->iop_order))
  {
    GList *dest_list = g_list_find(darktable.develop->iop, dest);
    do
    {
      dest_list = above ? dest_list->next : dest_list->prev;
      dest = dest_list->data;
    } while (!dest->expander || !gtk_widget_get_visible(dest->expander));
  }

  if(dest == src) return TRUE;

  if(x != DND_DROP)
  {
    if(src->iop_order < dest->iop_order
       ? dt_ioppr_check_can_move_after_iop(darktable.develop->iop, src, dest)
       : dt_ioppr_check_can_move_before_iop(darktable.develop->iop, src, dest))
    {
      dtgtk_expander_set_drag_hover(DTGTK_EXPANDER(widget), TRUE, !above, time);
      gdk_drag_status(dc, GDK_ACTION_COPY, time);
    }
  }
  else
  {
    gtk_drag_finish(dc, TRUE, FALSE, time);

    if(!(src->iop_order < dest->iop_order
         ? dt_ioppr_move_iop_after(darktable.develop, src, dest)
         : dt_ioppr_move_iop_before(darktable.develop, src, dest)))
      return TRUE;

    // we move the headers
    int position = 0;
    GtkBox *panel = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
    gtk_container_child_get(GTK_CONTAINER(panel), dest->expander,
                            "position", &position, NULL);
    gtk_box_reorder_child(panel, src->expander, position);

    dt_dev_add_history_item(src->dev, src, TRUE);

    dt_ioppr_check_iop_order(src->dev, 0, "_on_drag_drop end");

    // rebuild the accelerators
    dt_iop_connect_accels_multi(src->so);

    dt_dev_pixelpipe_rebuild(src->dev);

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MODULE_MOVED);
  }

  return TRUE;
}

static gboolean _on_drag_drop(GtkWidget *widget, GdkDragContext *dc, gint x, gint y, guint time, dt_iop_module_t *module)
{
  return _on_drag_motion(widget, dc, DND_DROP, y, time, module);
}

void dt_iop_gui_set_expander(dt_iop_module_t *module)
{
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(header), "module-header");

  GtkWidget *iopw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *expander = dtgtk_expander_new(header, iopw);
  gtk_widget_set_name(expander, "iop-expander");

  GtkWidget *header_evb = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *body_evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *pluginui_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));

  dt_gui_add_class(pluginui_frame, "dt_plugin_ui");

  static const GtkTargetEntry target_list[] = { { "iop", GTK_TARGET_SAME_APP, DND_TARGET_IOP } };

  gtk_drag_source_set(header_evb, GDK_BUTTON1_MASK, target_list, 1, GDK_ACTION_COPY);
  gtk_drag_dest_set(expander, GTK_DEST_DEFAULT_DROP | GTK_DEST_DEFAULT_HIGHLIGHT, target_list, 1, GDK_ACTION_COPY);
  g_signal_connect(expander, "drag-motion", G_CALLBACK(_on_drag_motion), module);
  g_signal_connect(expander, "drag-drop", G_CALLBACK(_on_drag_drop), module);

  module->header = header;

  /* setup the header box */
  g_signal_connect(G_OBJECT(header_evb), "button-press-event",
                   G_CALLBACK(_iop_plugin_header_button_press), module);
  gtk_widget_add_events(header_evb, GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(header_evb), "enter-notify-event",
                   G_CALLBACK(_header_motion_notify_show_callback), module);
  g_signal_connect(G_OBJECT(header_evb), "leave-notify-event",
                   G_CALLBACK(_header_motion_notify_hide_callback), module);

  /* connect mouse button callbacks for focus and presets */
  g_signal_connect(G_OBJECT(body_evb), "button-press-event",
                   G_CALLBACK(_iop_plugin_body_button_press), module);
  gtk_widget_add_events(body_evb, GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(body_evb), "enter-notify-event",
                   G_CALLBACK(_header_motion_notify_show_callback), module);
  g_signal_connect(G_OBJECT(body_evb), "leave-notify-event",
                   G_CALLBACK(_header_motion_notify_hide_callback), module);

  /*
   * initialize the header widgets
   */
  /* init empty place for icon, this is then set in CSS if needed */
  char w_name[256] = { 0 };
  snprintf(w_name, sizeof(w_name), "iop-panel-icon-%s", module->op);
  GtkWidget *icon = gtk_label_new("");
  gtk_widget_set_name(icon, w_name);
  dt_gui_add_class(icon, "dt_icon");
  gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

  /* add module label & instance name */
  GtkWidget *lab = gtk_event_box_new();
  module->label = gtk_label_new(module->name());
  gtk_widget_set_name(module->label, "iop-panel-label");
  gtk_label_set_ellipsize(GTK_LABEL(module->label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign(module->label, GTK_ALIGN_BASELINE);
  g_object_set(G_OBJECT(module->label), "xalign", 0.0, (gchar *)0);

  gtk_container_add(GTK_CONTAINER(lab), module->label);
  gtk_widget_set_valign(lab, GTK_ALIGN_BASELINE);
  gtk_widget_set_halign(lab, GTK_ALIGN_START);

  module->instance_name = gtk_label_new("");
  gtk_widget_set_name(module->instance_name, "iop-module-name");
  gtk_label_set_ellipsize(GTK_LABEL(module->instance_name), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_valign(module->instance_name, GTK_ALIGN_BASELINE);
  g_object_set(G_OBJECT(module->instance_name), "xalign", 0.0, (gchar *)0);

  if((module->flags() & IOP_FLAGS_DEPRECATED) && module->deprecated_msg())
    gtk_widget_set_tooltip_text(lab, module->deprecated_msg());
  else
  {
    g_signal_connect(lab, "query-tooltip", G_CALLBACK(_iop_tooltip_callback), module);
    g_signal_connect(header, "query-tooltip", G_CALLBACK(_iop_tooltip_callback), module);
    gtk_widget_set_has_tooltip(header, TRUE);
  }

  dt_action_define(&module->so->actions, NULL, NULL, lab, NULL);
  g_signal_connect(lab, "enter-notify-event",
                   G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_SHOW));

  /* add right side header buttons */
  module->presets_button = dt_iop_gui_header_button(module,
                                                    dtgtk_cairo_paint_presets,
                                                    DT_ACTION_ELEMENT_PRESETS,
                                                    header);
  module->reset_button = dt_iop_gui_header_button(module,
                                                  dtgtk_cairo_paint_reset,
                                                  DT_ACTION_ELEMENT_RESET,
                                                  header);
  module->multimenu_button = dt_iop_gui_header_button(module,
                                                      dtgtk_cairo_paint_multiinstance,
                                                      DT_ACTION_ELEMENT_INSTANCE,
                                                      header);
  if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE))
    gtk_widget_set_tooltip_text
      (module->multimenu_button,
       _("multiple instance actions\nright-click creates new instance"));

  if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE))
    gtk_widget_set_tooltip_text(module->presets_button,
                                _("presets\nright-click to apply on new instance"));

  /* add enabled button */
  module->off = dt_iop_gui_header_button(module,
                                         dtgtk_cairo_paint_switch,
                                         DT_ACTION_ELEMENT_ENABLE,
                                         header);
  dt_gui_add_class(module->off, "dt_transparent_background");
  dt_iop_gui_set_enable_button_icon(module->off, module);
  gtk_widget_set_sensitive(module->off, !module->hide_enable_button);

  gtk_box_pack_start(GTK_BOX(header), icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(header), lab, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(header), module->instance_name, FALSE, FALSE, 0);

  dt_gui_add_help_link(lab, module->op);
  dt_gui_add_help_link(expander, module->op);
  dt_gui_add_help_link(header, "module_header");

  // show deprecated message if any
  if(module->deprecated_msg())
  {
    GtkWidget *lb = gtk_label_new(module->deprecated_msg());
    gtk_label_set_line_wrap(GTK_LABEL(lb), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lb), 0); // don't propagate natural width
    gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
    dt_gui_add_class(lb, "dt_warning");
    gtk_box_pack_start(GTK_BOX(iopw), lb, TRUE, TRUE, 0);
    gtk_widget_show(lb);
  }

  /* add the blending ui if supported */
  gtk_box_pack_start(GTK_BOX(iopw), module->widget, TRUE, TRUE, 0);
  dt_guides_init_module_widget(iopw, module);
  dt_iop_gui_init_blending(iopw, module);
  dt_gui_add_class(module->widget, "dt_plugin_ui_main");
  dt_gui_add_help_link(module->widget, module->op);
  gtk_widget_hide(iopw);

  module->expander = expander;

  /* update header */
  dt_iop_gui_update_header(module);

  gtk_widget_set_hexpand(module->widget, FALSE);
  gtk_widget_set_vexpand(module->widget, FALSE);

  gtk_widget_show_all(expander);
  dt_ui_container_add_widget(darktable.gui->ui,
                             DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
  dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
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

gboolean dt_iop_breakpoint(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe)
{
  if(pipe != dev->preview_pipe
     && pipe != dev->preview2.pipe)
    sched_yield();

  if(pipe != dev->preview_pipe
     && pipe != dev->preview2.pipe
     && pipe->changed == DT_DEV_PIPE_ZOOMED)
    return TRUE;

  if((pipe->changed != DT_DEV_PIPE_UNCHANGED
      && pipe->changed != DT_DEV_PIPE_ZOOMED)
     || dev->gui_leaving)
    return TRUE;

  return FALSE;
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

  for(GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = modules->data;
    if(dt_iop_module_is(mod->so, op))
    {
      result = mod;
      break;
    }
  }

  return result;
}

dt_iop_module_t *dt_iop_get_module(const char *op)
{
  return dt_iop_get_module_from_list(darktable.develop->iop, op);
}

dt_iop_module_so_t *dt_iop_get_module_so(const char *op)
{
  dt_iop_module_so_t *result = NULL;

  for(GList *modules = darktable.iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_so_t *mod = modules->data;
    if(dt_iop_module_is(mod, op))
    {
      result = mod;
      break;
    }
  }

  return result;
}

int dt_iop_get_module_flags(const char *op)
{
  GList *modules = darktable.iop;
  while(modules)
  {
    dt_iop_module_so_t *module = modules->data;
    if(dt_iop_module_is(module, op))
      return module->flags();
    modules = g_list_next(modules);
  }
  return 0;
}

static void _show_module_callback(dt_iop_module_t *module)
{
  // Showing the module, if it isn't already visible
  if(module->so->state == IOP_STATE_HIDDEN)
  {
    dt_iop_gui_set_state(module, IOP_STATE_ACTIVE);
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

  dt_iop_gui_set_expanded(module, !module->expanded,
                          dt_conf_get_bool("darkroom/ui/single_module"));
  if(module->expanded)
  {
    dt_iop_request_focus(module);
  }

  dt_iop_connect_accels_multi(module->so);
}

static void _request_module_focus_callback(dt_iop_module_t * module)
{
  dt_iop_request_focus(dt_dev_gui_module() == module ? NULL : module);
}

static void _enable_module_callback(dt_iop_module_t *module)
{
  //cannot toggle module if there's no enable button
  if(module->hide_enable_button) return;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(module->off));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), !active);
}

// to be called before issuing any query based on memory.darktable_iop_names
void dt_iop_set_darktable_iop_table()
{
  sqlite3_stmt *stmt;
  gchar *module_list = NULL;
  for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
  {
    dt_iop_module_so_t *module = iop->data;
    dt_util_str_cat(&module_list, "(\"%s\",\"%s\"),",
                                  module->op, module->name());
  }

  if(module_list)
  {
    module_list[strlen(module_list) - 1] = '\0';
    gchar *query =
      g_strdup_printf("INSERT INTO memory.darktable_iop_names (operation, name)"
                      " VALUES %s", module_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
    g_free(module_list);
  }
}

const gchar *dt_iop_get_localized_name(const gchar *op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(module_names == NULL)
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
    {
      dt_iop_module_so_t *module = iop->data;
      g_hash_table_insert(module_names, module->op, g_strdup(module->name()));
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

const gchar *dt_iop_get_localized_aliases(const gchar *op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_aliases = NULL;
  if(module_aliases == NULL)
  {
    module_aliases = g_hash_table_new(g_str_hash, g_str_equal);
    for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
    {
      dt_iop_module_so_t *module = iop->data;
      g_hash_table_insert(module_aliases, module->op, g_strdup(module->aliases()));
    }
  }
  if(op != NULL)
  {
    return (gchar *)g_hash_table_lookup(module_aliases, op);
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
  if(state == IOP_STATE_HIDDEN)
  {
    for(mods = darktable.develop->iop; mods; mods = g_list_next(mods))
    {
      dt_iop_module_t *mod = mods->data;
      if(mod->so == module && mod->expander) gtk_widget_hide(GTK_WIDGET(mod->expander));
    }

    snprintf(option, sizeof(option), "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool(option, FALSE);
    snprintf(option, sizeof(option), "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool(option, FALSE);
  }
  else if(state == IOP_STATE_ACTIVE)
  {
    if(!darktable.gui->reset)
    {
      int once = 0;

      for(mods = darktable.develop->iop; mods; mods = g_list_next(mods))
      {
        dt_iop_module_t *mod = mods->data;
        if(mod->so == module && mod->expander)
        {
          gtk_widget_show(GTK_WIDGET(mod->expander));
          if(!once)
          {
            dt_dev_modulegroups_switch(darktable.develop, mod);
            once = 1;
          }
        }
      }
    }

    /* module is shown lets set conf values */
    snprintf(option, sizeof(option), "plugins/darkroom/%s/visible", module->op);
    dt_conf_set_bool(option, TRUE);
    snprintf(option, sizeof(option), "plugins/darkroom/%s/favorite", module->op);
    dt_conf_set_bool(option, FALSE);
  }
  else if(state == IOP_STATE_FAVORITE)
  {
    for(mods = darktable.develop->iop; mods; mods = g_list_next(mods))
    {
      dt_iop_module_t *mod = mods->data;
      if(mod->so == module && mod->expander) gtk_widget_show(GTK_WIDGET(mod->expander));
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

void dt_iop_update_multi_priority(dt_iop_module_t *module, const int new_priority)
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
      dt_dev_history_item_t *hist = hiter->data;
      if(hist->module == sink_module)
        hist->blend_params->raster_mask_instance = new_priority;
    }
  }

  module->multi_priority = new_priority;
}

gboolean dt_iop_is_raster_mask_used(const dt_iop_module_t *module, const dt_mask_id_t id)
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

dt_iop_module_t *dt_iop_get_module_by_op_priority(GList *modules,
                                                  const char *operation,
                                                  const int multi_priority)
{
  dt_iop_module_t *mod_ret = NULL;

  for(GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;

    if(dt_iop_module_is(mod->so, operation)
       && (mod->multi_priority == multi_priority || multi_priority == -1))
    {
      mod_ret = mod;
      break;
    }
  }
  return mod_ret;
}

dt_iop_module_t *dt_iop_get_module_preferred_instance(const dt_iop_module_so_t *module)
{
  /*
   decide which module instance keyboard shortcuts will be applied to
   based on user preferences, as follows

    - Use the focused module, if it is an instance of this module type
      and the appropriate preference is checked. Otherwise

    - prefer expanded instances (when selected and instances of the
      module are expanded on the RHS of the screen, collapsed
      instances will be ignored)

    - prefer enabled instances (when selected, after applying the
      above rule, if instances of the module are active, inactive
      instances will be ignored)

    - prefer unmasked instances (when selected, after applying the
      above rules, if instances of the module are unmasked, masked
      instances will be ignored)

    - selection order (after applying the above rules, apply the
      shortcut to the first or last instance remaining)
  */
  const gboolean prefer_focused = dt_conf_get_bool("accel/prefer_focused");
  const int prefer_expanded = dt_conf_get_bool("accel/prefer_expanded") ? 8 : 0;
  const int prefer_enabled = dt_conf_get_bool("accel/prefer_enabled") ? 4 : 0;
  const int prefer_unmasked = dt_conf_get_bool("accel/prefer_unmasked") ? 2 : 0;
  const int prefer_first = dt_conf_is_equal("accel/select_order", "first instance") ? 1 : 0;

  dt_iop_module_t *accel_mod = NULL;  // The module to which accelerators are to be attached

  // if any instance has focus, use that one
  dt_iop_module_t *gui_module = dt_dev_gui_module();
  if(prefer_focused
      && gui_module
      && (gui_module->so == module
         || DT_ACTION(module) == &darktable.control->actions_focus))
    accel_mod = gui_module;
  else
  {
    int best_score = -1;

    for(GList *iop_mods = g_list_last(darktable.develop->iop);
        iop_mods;
        iop_mods = g_list_previous(iop_mods))
    {
      dt_iop_module_t *mod = iop_mods->data;

      if(mod->so == module && mod->iop_order != INT_MAX)
      {
        const int score = (mod->expanded ? prefer_expanded : 0)
                        + (mod->enabled ? prefer_enabled : 0)
                        + (mod->blend_params->mask_mode == DEVELOP_MASK_DISABLED
                           || mod->blend_params->mask_mode == DEVELOP_MASK_ENABLED
                                ? prefer_unmasked : 0);

        if(score + prefer_first > best_score)
        {
          best_score = score;
          accel_mod = mod;
        }
      }
    }
  }

  return accel_mod;
}

/** adds keyboard accels to the first module in the pipe to handle
 * where there are multiple instances */
void dt_iop_connect_accels_multi(dt_iop_module_so_t *module)
{
  if(darktable.develop->gui_attached)
  {
    dt_iop_module_t *accel_mod_new = dt_iop_get_module_preferred_instance(module);

    // switch accelerators to new module
    if(accel_mod_new)
    {
      dt_accel_connect_instance_iop(accel_mod_new);

      if(!strcmp(accel_mod_new->op, "exposure"))
        darktable.develop->proxy.exposure.module = accel_mod_new;
    }
  }
}

void dt_iop_connect_accels_all(void)
{
  for(const GList *iop_mods = g_list_last(darktable.develop->iop);
      iop_mods;
      iop_mods = g_list_previous(iop_mods))
  {
    dt_iop_module_t *mod = iop_mods->data;
    dt_iop_connect_accels_multi(mod->so);
  }
}

dt_iop_module_t *dt_iop_get_module_by_instance_name(GList *modules,
                                                    const char *operation,
                                                    const char *multi_name)
{
  dt_iop_module_t *mod_ret = NULL;

  for(GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;

    if((dt_iop_module_is(mod->so, operation))
       && ((multi_name == NULL) || (strcmp(mod->multi_name, multi_name) == 0)))
    {
      mod_ret = mod;
      break;
    }
  }
  return mod_ret;
}

/** count instances of a module **/
int dt_iop_count_instances(dt_iop_module_so_t *module)
{
  int inst_count = 0;

  for(const GList *iop_mods = g_list_last(darktable.develop->iop);
      iop_mods;
      iop_mods = g_list_previous(iop_mods))
  {
    dt_iop_module_t *mod = iop_mods->data;
    if(mod->so == module && mod->iop_order != INT_MAX)
    {
      inst_count++;
    }
  }
  return inst_count;
}

gboolean dt_iop_is_first_instance(GList *modules, const dt_iop_module_t *module)
{
  gboolean is_first = TRUE;
  GList *iop = modules;
  while(iop)
  {
    dt_iop_module_t *m = iop->data;
    if(dt_iop_module_is(m->so, module->op))
    {
      is_first = (m == module);
      break;
    }
    iop = g_list_next(iop);
  }

  return is_first;
}

const char *dt_iop_get_instance_name(const dt_iop_module_t *module)
{
  return (module->multi_priority > 0 || module->multi_name_hand_edited)
    ? module->multi_name
    : "";
}
const char *dt_iop_get_instance_id(const dt_iop_module_t *module)
{
  const char *ids[] = { "", ".1", ".2", ".3", ".4", ".5", ".6", ".x" };
  return ids[MIN(module->multi_priority, 7)];
}

void dt_iop_refresh_center(const dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if(dev && dev->gui_attached)
  {
    dt_dev_pixelpipe_cache_invalidate_later(dev->full.pipe, module->iop_order);
    //ensure that commit_params gets called to pick up any GUI changes
    dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dt_dev_invalidate(dev);
    dt_control_queue_redraw_center();
  }
}

void dt_iop_refresh_preview(const dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if(dev && dev->gui_attached)
  {
    dt_dev_pixelpipe_cache_invalidate_later(dev->preview_pipe, module->iop_order);
    //ensure that commit_params gets called to pick up any GUI changes
    dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dt_dev_invalidate_all(dev);
    dt_control_queue_redraw();
  }
}

void dt_iop_refresh_preview2(const dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if(dev && dev->gui_attached)
  {
    dt_dev_pixelpipe_cache_invalidate_later(dev->preview2.pipe, module->iop_order);
    //ensure that commit_params gets called to pick up any GUI changes
    dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dt_dev_invalidate_all(dev);
    dt_control_queue_redraw();
  }
}

void dt_iop_refresh_all(const dt_iop_module_t *module)
{
  dt_iop_refresh_preview(module);
  dt_iop_refresh_center(module);
  dt_iop_refresh_preview2(module);
}

const char **dt_iop_set_description(dt_iop_module_t *module,
                                    const char *main_text,
                                    const char *purpose,
                                    const char *input,
                                    const char *process,
                                    const char *output)
{
  static const char *str_out[5] = {NULL, NULL, NULL, NULL, NULL};

  str_out[0] = main_text;
  str_out[1] = purpose;
  str_out[2] = input;
  str_out[3] = process;
  str_out[4] = output;

  return (const char **)str_out;
}

const char *dt_iop_colorspace_to_name(const dt_iop_colorspace_type_t type)
{
  switch(type)
  {
    case IOP_CS_NONE:
      return "IOP_CS_NONE";
    case IOP_CS_RAW:
      return "IOP_CS_RAW";
    case IOP_CS_LAB:
      return "IOP_CS_LAB";
    case IOP_CS_RGB:
      return "IOP_CS_RGB";
    case IOP_CS_LCH:
      return "IOP_CS_LCH";
    case IOP_CS_HSL:
      return "IOP_CS_HSL";
    case IOP_CS_JZCZHZ:
      return "IOP_CS_JZCZHZ";
  }
  return "invalid IOP_CS";
}

gboolean dt_iop_have_required_input_format(const int req_ch,
                                           dt_iop_module_t *const module,
                                           const int ch,
                                           const void *const restrict ivoid,
                                           void *const restrict ovoid,
                                           const dt_iop_roi_t *const roi_in,
                                           const dt_iop_roi_t *const roi_out)
{
  if(ch == req_ch)
  {
    // don't fiddle with module's trouble information here
    return TRUE;
  }
  else
  {
    /* copy the input buffer to the output, take care about available channels
       as we don't know the size of available in/out buffers as the roi has no information
       about channels.
    */
    dt_iop_copy_image_roi(ovoid, ivoid, MIN(ch, req_ch) , roi_in, roi_out);
    // and set the module's trouble message
    if(module)
    {
      dt_iop_set_module_trouble_message
        (module, _("unsupported input"),
         _("you have placed this module at\n"
           "a position in the pipeline where\n"
           "the data format does not match\n"
           "its requirements."), NULL);
      dt_print_pipe(DT_DEBUG_ALWAYS,
        "unsupported data format", NULL, module, DT_DEVICE_NONE, roi_in, roi_out);
    }
    else
    {
      dt_print_pipe(DT_DEBUG_ALWAYS,
        "unsupported data format", NULL, NULL, DT_DEVICE_NONE, roi_in, roi_out, "no module given");
    }
    return FALSE;
  }
}

gboolean dt_iop_canvas_not_sensitive(const struct dt_develop_t *dev)
{
  return dt_iop_color_picker_is_visible(dev)
         || darktable.lib->proxy.snapshots.enabled;
}

void dt_iop_gui_changed(dt_action_t *action, GtkWidget *widget, gpointer data)
{
  if(!action || action->type != DT_ACTION_TYPE_IOP_INSTANCE) return;
  dt_iop_module_t *module = (dt_iop_module_t *)action;

  if(module->gui_changed) module->gui_changed(module, widget, data);

  dt_iop_color_picker_reset(module, TRUE);

  dt_dev_add_history_item_target(darktable.develop, module, TRUE, widget);
}

gboolean dt_iop_module_is_skipped(const struct dt_develop_t *dev,
                                  const dt_iop_module_t *module)
{
  return dev->gui_module
      && dev->gui_module != module
      && (dev->gui_module->operation_tags_filter() & module->operation_tags());
}

enum
{
  // Multi-instance
//DT_ACTION_EFFECT_SHOW,
//DT_ACTION_EFFECT_UP,
//DT_ACTION_EFFECT_DOWN,
  DT_ACTION_EFFECT_NEW = 3,
//DT_ACTION_EFFECT_DELETE = 4,
  DT_ACTION_EFFECT_RENAME = 5,
  DT_ACTION_EFFECT_DUPLICATE = 6,
};

static float _action_process(gpointer target,
                             const dt_action_element_t element,
                             const dt_action_effect_t effect,
                             float move_size)
{
  dt_iop_module_t *module = target;

  if(DT_PERFORM_ACTION(move_size))
  {
    switch(element)
    {
    case DT_ACTION_ELEMENT_FOCUS:
      _request_module_focus_callback(module);
      break;
    case DT_ACTION_ELEMENT_ENABLE:
      _enable_module_callback(module);
      break;
    case DT_ACTION_ELEMENT_SHOW:
      _show_module_callback(module);
      break;
    case DT_ACTION_ELEMENT_INSTANCE:;
      dt_iop_gui_multi_show_t multi_show;
      _get_multi_show(module, &multi_show);

      if     (effect == DT_ACTION_EFFECT_NEW       && multi_show.new  )
        _gui_copy_callback     (NULL, module);
      else if(effect == DT_ACTION_EFFECT_DUPLICATE && multi_show.new  )
        _gui_duplicate_callback(NULL, module);
      else if(effect == DT_ACTION_EFFECT_UP        && multi_show.up   )
        _gui_moveup_callback   (NULL, module);
      else if(effect == DT_ACTION_EFFECT_DOWN      && multi_show.down )
        _gui_movedown_callback (NULL, module);
      else if(effect == DT_ACTION_EFFECT_DELETE    && multi_show.close)
        _gui_delete_callback   (NULL, module);
      else if(effect == DT_ACTION_EFFECT_RENAME                               )
        _gui_rename_callback   (NULL, module);
      else _gui_multiinstance_callback(NULL, NULL, module);
      break;
    case DT_ACTION_ELEMENT_RESET:
      {
        GdkEventButton event = { .state = (effect == DT_ACTION_EFFECT_ACTIVATE_CTRL
                                           ? GDK_CONTROL_MASK
                                           : 0) };
        _gui_reset_callback(NULL, &event, module);
      }
      break;
    case DT_ACTION_ELEMENT_PRESETS:
      switch(effect)
      {
      case DT_ACTION_EFFECT_ACTIVATE:
        if(module->presets_button)
          _presets_popup_callback(NULL, NULL, module);
        break;
      case DT_ACTION_EFFECT_NEXT:
        move_size *= -1;
      case DT_ACTION_EFFECT_PREVIOUS:
        dt_gui_presets_apply_adjacent_preset(module, move_size);
        return 0; // don't overwrite toast below
      default:
        dt_print(DT_DEBUG_ALWAYS,
                 "[imageop::_action_process] effect %d for presets not yet implemented",
                 effect);
        break;
      }
    }

    dt_action_widget_toast(target, NULL, "%s, %s",
                           dt_action_def_iop.elements[element].name,
                           dt_action_def_iop.elements[element].effects[effect]);
  }

  return element == DT_ACTION_ELEMENT_FOCUS
    ? dt_dev_gui_module() == module
    : (element == DT_ACTION_ELEMENT_ENABLE
       ? module->off && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(module->off))
       : (element == DT_ACTION_ELEMENT_SHOW
          ? module->expanded
          : 0));
}

const gchar *dt_action_effect_instance[]
  = { N_("show"),
      N_("move up"),
      N_("move down"),
      N_("new"),
      N_("delete"),
      N_("rename"),
      N_("duplicate"),
      NULL };

static const dt_action_element_def_t _action_elements[]
  = { { N_("show"), dt_action_effect_toggle },
      { N_("reset"), dt_action_effect_activate },
      { N_("presets"), dt_action_effect_presets },
      { N_("enable"), dt_action_effect_toggle },
      { N_("focus"), dt_action_effect_toggle },
      { N_("instance"), dt_action_effect_instance },
      { NULL } };

static const dt_shortcut_fallback_t _action_fallbacks[]
  = { { .element = DT_ACTION_ELEMENT_ENABLE,
        .button = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_FOCUS,
        .button = DT_SHORTCUT_LEFT,
        .click = DT_SHORTCUT_LONG },
      { .element = DT_ACTION_ELEMENT_INSTANCE,
        .button = DT_SHORTCUT_RIGHT,
        .click = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_RESET,
        .button = DT_SHORTCUT_LEFT,
        .click = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_PRESETS,
        .button = DT_SHORTCUT_RIGHT },
      { } };

const dt_action_def_t dt_action_def_iop
  = { N_("processing module"),
      _action_process,
      _action_elements,
      _action_fallbacks };

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
