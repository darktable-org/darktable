/*
    This file is part of darktable,
    Copyright (C) 2017-2023 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/dwt.h"
#include "common/gaussian.h"
#include "common/heal.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(3, dt_iop_retouch_params_t)

#define RETOUCH_NO_FORMS 300
#define RETOUCH_MAX_SCALES 15
#define RETOUCH_NO_SCALES (RETOUCH_MAX_SCALES + 2)

#define RETOUCH_PREVIEW_LVL_MIN -3.0f
#define RETOUCH_PREVIEW_LVL_MAX 3.0f

typedef enum dt_iop_retouch_drag_types_t {
  DT_IOP_RETOUCH_WDBAR_DRAG_TOP = 1,
  DT_IOP_RETOUCH_WDBAR_DRAG_BOTTOM = 2,
} dt_iop_retouch_drag_types_t;

typedef enum dt_iop_retouch_fill_modes_t {
  DT_IOP_RETOUCH_FILL_ERASE = 0, // $DESCRIPTION: "erase"
  DT_IOP_RETOUCH_FILL_COLOR = 1  // $DESCRIPTION: "color"
} dt_iop_retouch_fill_modes_t;

typedef enum dt_iop_retouch_blur_types_t {
  DT_IOP_RETOUCH_BLUR_GAUSSIAN = 0, // $DESCRIPTION: "gaussian"
  DT_IOP_RETOUCH_BLUR_BILATERAL = 1 // $DESCRIPTION: "bilateral"
} dt_iop_retouch_blur_types_t;

typedef enum dt_iop_retouch_algo_type_t {
  DT_IOP_RETOUCH_NONE = 0,  // $DESCRIPTION: "unused"
  DT_IOP_RETOUCH_CLONE = 1, // $DESCRIPTION: "clone"
  DT_IOP_RETOUCH_HEAL = 2,  // $DESCRIPTION: "heal"
  DT_IOP_RETOUCH_BLUR = 3,  // $DESCRIPTION: "blur"
  DT_IOP_RETOUCH_FILL = 4   // $DESCRIPTION: "fill"
} dt_iop_retouch_algo_type_t;

typedef struct dt_iop_retouch_form_data_t
{
  int formid; // from masks, form->formid
  int scale;  // 0==original image; 1..RETOUCH_MAX_SCALES==scale; RETOUCH_MAX_SCALES+1==residual
  dt_iop_retouch_algo_type_t algorithm;  // clone, heal, blur, fill

  dt_iop_retouch_blur_types_t blur_type; // gaussian, bilateral
  float blur_radius;                     // radius for blur algorithm

  dt_iop_retouch_fill_modes_t fill_mode; // mode for fill algorithm, erase or fill with color
  float fill_color[3];                   // color for fill algorithm
  float fill_brightness;                 // value to be added to the color
  int distort_mode; // module v1 => 1, otherwise 2. mode 1 as issues if there's distortion before this module
} dt_iop_retouch_form_data_t;

typedef struct retouch_user_data_t
{
  dt_iop_module_t *self;
  dt_dev_pixelpipe_iop_t *piece;
  dt_iop_roi_t roi;
  int display_scale;
  int mask_display;
  int suppress_mask;
} retouch_user_data_t;

typedef struct dt_iop_retouch_params_t
{
  dt_iop_retouch_form_data_t rt_forms[RETOUCH_NO_FORMS]; // array of masks index and additional data

  dt_iop_retouch_algo_type_t algorithm; // $DEFAULT: DT_IOP_RETOUCH_HEAL clone, heal, blur, fill

  int num_scales;       // $DEFAULT: 0 number of wavelets scales
  int curr_scale;       // $DEFAULT: 0 current wavelet scale
  int merge_from_scale; // $DEFAULT: 0

  float preview_levels[3];

  dt_iop_retouch_blur_types_t blur_type; // $DEFAULT: DT_IOP_RETOUCH_BLUR_GAUSSIAN $DESCRIPTION: "blur type" gaussian, bilateral
  float blur_radius; // $MIN: 0.1 $MAX: 200.0 $DEFAULT: 10.0 $DESCRIPTION: "blur radius" radius for blur algorithm

  dt_iop_retouch_fill_modes_t fill_mode; // $DEFAULT: DT_IOP_RETOUCH_FILL_ERASE $DESCRIPTION: "fill mode" mode for fill algorithm, erase or fill with color
  float fill_color[3];   // $DEFAULT: 0.0 color for fill algorithm
  float fill_brightness; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "brightness" value to be added to the color
  int max_heal_iter;     // $DEFAULT: 2000 $DESCRIPTION: "max_iter" number of iterations for heal algorithm
} dt_iop_retouch_params_t;

typedef struct dt_iop_retouch_gui_data_t
{
  int copied_scale; // scale to be copied to another scale
  int mask_display; // should we expose masks?
  int suppress_mask;         // do not process masks
  int display_wavelet_scale; // display current wavelet scale
  int displayed_wavelet_scale; // was display wavelet scale already used?
  int preview_auto_levels;   // should we calculate levels automatically?
  float preview_levels[3];   // values for the levels
  int first_scale_visible;   // 1st scale visible at current zoom level

  GtkLabel *label_form;                                                   // display number of forms
  GtkLabel *label_form_selected;                                          // display number of forms selected
  GtkWidget *bt_edit_masks, *bt_path, *bt_circle, *bt_ellipse, *bt_brush; // shapes
  GtkWidget *bt_clone, *bt_heal, *bt_blur, *bt_fill;                      // algorithms
  GtkWidget *bt_showmask, *bt_suppress;                                   // suppress & show masks

  GtkWidget *wd_bar; // wavelet decompose bar
  GtkLabel *lbl_num_scales;
  GtkLabel *lbl_curr_scale;
  GtkLabel *lbl_merge_from_scale;
  float wdbar_mouse_x, wdbar_mouse_y;
  int curr_scale; // scale box under mouse
  gboolean is_dragging;
  gboolean upper_cursor; // mouse on merge from scale cursor
  gboolean lower_cursor; // mouse on num scales cursor
  gboolean upper_margin; // mouse on the upper band
  gboolean lower_margin; // mouse on the lower band

  GtkWidget *bt_display_wavelet_scale; // show decomposed scale

  GtkWidget *bt_copy_scale; // copy all shapes from one scale to another
  GtkWidget *bt_paste_scale;

  GtkWidget *vbox_preview_scale;

  GtkDarktableGradientSlider *preview_levels_gslider;

  GtkWidget *bt_auto_levels;

  GtkWidget *vbox_blur;
  GtkWidget *cmb_blur_type;
  GtkWidget *sl_blur_radius;

  GtkWidget *vbox_fill;
  GtkWidget *hbox_color_pick;
  GtkWidget *colorpick;   // select a specific color
  GtkWidget *colorpicker; // pick a color from the picture

  GtkWidget *cmb_fill_mode;
  GtkWidget *sl_fill_brightness;

  GtkWidget *sl_mask_opacity; // draw mask opacity
} dt_iop_retouch_gui_data_t;

typedef struct dt_iop_retouch_params_t dt_iop_retouch_data_t;

typedef struct dt_iop_retouch_global_data_t
{
  int kernel_retouch_clear_alpha;
  int kernel_retouch_copy_alpha;
  int kernel_retouch_copy_buffer_to_buffer;
  int kernel_retouch_copy_buffer_to_image;
  int kernel_retouch_fill;
  int kernel_retouch_copy_image_to_buffer_masked;
  int kernel_retouch_copy_buffer_to_buffer_masked;
  int kernel_retouch_image_rgb2lab;
  int kernel_retouch_image_lab2rgb;
  int kernel_retouch_copy_mask_to_alpha;
} dt_iop_retouch_global_data_t;


// this returns a translatable name
const char *name()
{
  return _("retouch");
}

const char *aliases()
{
  return _("split-frequency|healing|cloning|stamp");
}


const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("remove and clone spots, perform split-frequency skin editing"),
                                      _("corrective"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric and frequential, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_NO_MASKS | IOP_FLAGS_GUIDES_WIDGET;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_retouch_form_data_v1_t
    {
      int formid; // from masks, form->formid
      int scale;  // 0==original image; 1..RETOUCH_MAX_SCALES==scale; RETOUCH_MAX_SCALES+1==residual
      dt_iop_retouch_algo_type_t algorithm; // clone, heal, blur, fill

      dt_iop_retouch_blur_types_t blur_type; // gaussian, bilateral
      float blur_radius;                     // radius for blur algorithm

      dt_iop_retouch_fill_modes_t fill_mode; // mode for fill algorithm, erase or fill with color
      float fill_color[3];                   // color for fill algorithm
      float fill_brightness;                 // value to be added to the color
    } dt_iop_retouch_form_data_v1_t;
    typedef struct dt_iop_retouch_params_v1_t
    {
      dt_iop_retouch_form_data_v1_t rt_forms[RETOUCH_NO_FORMS]; // array of masks index and additional data

      dt_iop_retouch_algo_type_t algorithm; // $DEFAULT: DT_IOP_RETOUCH_HEAL clone, heal, blur, fill

      int num_scales;       // $DEFAULT: 0 number of wavelets scales
      int curr_scale;       // $DEFAULT: 0 current wavelet scale
      int merge_from_scale; // $DEFAULT: 0

      float preview_levels[3];

      dt_iop_retouch_blur_types_t blur_type; // $DEFAULT: DT_IOP_RETOUCH_BLUR_GAUSSIAN $DESCRIPTION: "blur type"
                                             // gaussian, bilateral
      float blur_radius; // $MIN: 0.1 $MAX: 200.0 $DEFAULT: 10.0 $DESCRIPTION: "blur radius" radius for blur
                         // algorithm

      dt_iop_retouch_fill_modes_t fill_mode; // $DEFAULT: DT_IOP_RETOUCH_FILL_ERASE $DESCRIPTION: "fill mode" mode
                                             // for fill algorithm, erase or fill with color
      float fill_color[3];                   // $DEFAULT: 0.0 color for fill algorithm
      float fill_brightness; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "brightness" value to be added to the color
    } dt_iop_retouch_params_v1_t;

    dt_iop_retouch_params_v1_t *o = (dt_iop_retouch_params_v1_t *)old_params;
    dt_iop_retouch_params_t *n = (dt_iop_retouch_params_t *)new_params;
    dt_iop_retouch_params_t *d = (dt_iop_retouch_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters
    for(int i = 0; i < RETOUCH_NO_FORMS; i++)
    {
      dt_iop_retouch_form_data_v1_t of = o->rt_forms[i];
      n->rt_forms[i].algorithm = of.algorithm;
      n->rt_forms[i].blur_radius = of.blur_radius;
      n->rt_forms[i].blur_type = of.blur_type;
      n->rt_forms[i].distort_mode = 1;
      n->rt_forms[i].fill_brightness = of.fill_brightness;
      n->rt_forms[i].fill_color[0] = of.fill_color[0];
      n->rt_forms[i].fill_color[1] = of.fill_color[1];
      n->rt_forms[i].fill_color[2] = of.fill_color[2];
      n->rt_forms[i].fill_mode = of.fill_mode;
      n->rt_forms[i].formid = of.formid;
      n->rt_forms[i].scale = of.scale;
    }
    n->algorithm = o->algorithm;
    n->blur_radius = o->blur_radius;
    n->blur_type = o->blur_type;
    n->curr_scale = o->curr_scale;
    n->fill_brightness = o->fill_brightness;
    n->fill_color[0] = o->fill_color[0];
    n->fill_color[1] = o->fill_color[1];
    n->fill_color[2] = o->fill_color[2];
    n->fill_mode = o->fill_mode;
    n->merge_from_scale = o->merge_from_scale;
    n->num_scales = o->num_scales;
    n->preview_levels[0] = o->preview_levels[0];
    n->preview_levels[1] = o->preview_levels[1];
    n->preview_levels[2] = o->preview_levels[2];

    n->max_heal_iter = 1000;

    return 0;
  }
  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_retouch_params_v2_t
    {
      dt_iop_retouch_form_data_t rt_forms[RETOUCH_NO_FORMS]; // array of masks index and additional data

      dt_iop_retouch_algo_type_t algorithm; // $DEFAULT: DT_IOP_RETOUCH_HEAL clone, heal, blur, fill

      int num_scales;       // $DEFAULT: 0 number of wavelets scales
      int curr_scale;       // $DEFAULT: 0 current wavelet scale
      int merge_from_scale; // $DEFAULT: 0

      float preview_levels[3];

      dt_iop_retouch_blur_types_t blur_type; // $DEFAULT: DT_IOP_RETOUCH_BLUR_GAUSSIAN $DESCRIPTION: "blur type" gaussian, bilateral
      float blur_radius; // $MIN: 0.1 $MAX: 200.0 $DEFAULT: 10.0 $DESCRIPTION: "blur radius" radius for blur algorithm

      dt_iop_retouch_fill_modes_t fill_mode; // $DEFAULT: DT_IOP_RETOUCH_FILL_ERASE $DESCRIPTION: "fill mode" mode for fill algorithm, erase or fill with color
      float fill_color[3];   // $DEFAULT: 0.0 color for fill algorithm
      float fill_brightness; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "brightness" value to be added to the color
    } dt_iop_retouch_params_v2_t;

    dt_iop_retouch_params_v2_t *o = (dt_iop_retouch_params_v2_t *)old_params;
    dt_iop_retouch_params_t *n = (dt_iop_retouch_params_t *)new_params;
    dt_iop_retouch_params_t *d = (dt_iop_retouch_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    memcpy(n, o, sizeof(dt_iop_retouch_params_v2_t));

    n->max_heal_iter = 1000;

    return 0;
  }
  return 1;
}

static int rt_get_index_from_formid(dt_iop_retouch_params_t *p, const int formid)
{
  int index = -1;
  if(formid > 0)
  {
    int i = 0;

    while(index == -1 && i < RETOUCH_NO_FORMS)
    {
      if(p->rt_forms[i].formid == formid) index = i;
      i++;
    }
  }
  return index;
}

static int rt_get_selected_shape_id()
{
  return darktable.develop->mask_form_selected_id;
}

static dt_masks_point_group_t *rt_get_mask_point_group(dt_iop_module_t *self, int formid)
{
  dt_masks_point_group_t *form_point_group = NULL;

  const dt_develop_blend_params_t *bp = self->blend_params;
  if(!bp) return form_point_group;

  const dt_masks_form_t *grp = dt_masks_get_from_id(self->dev, bp->mask_id);
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *forms = grp->points; forms; forms = g_list_next(forms))
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt->formid == formid)
      {
        form_point_group = grpt;
        break;
      }
    }
  }

  return form_point_group;
}

static float rt_get_shape_opacity(dt_iop_module_t *self, const int formid)
{
  float opacity = 0.f;

  dt_masks_point_group_t *grpt = rt_get_mask_point_group(self, formid);
  if(grpt) opacity = grpt->opacity;

  return opacity;
}

static void rt_display_selected_fill_color(dt_iop_retouch_gui_data_t *g, dt_iop_retouch_params_t *p)
{
  GdkRGBA c
      = (GdkRGBA){.red = p->fill_color[0], .green = p->fill_color[1], .blue = p->fill_color[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);
}

static void rt_show_hide_controls(const dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  const int creation_continuous = (darktable.develop->form_gui && darktable.develop->form_gui->creation_continuous
                                   && darktable.develop->form_gui->creation_continuous_module == self);

  switch(p->algorithm)
  {
    case DT_IOP_RETOUCH_HEAL:
      gtk_widget_hide(GTK_WIDGET(g->vbox_blur));
      gtk_widget_hide(GTK_WIDGET(g->vbox_fill));
      break;
    case DT_IOP_RETOUCH_BLUR:
      gtk_widget_show(GTK_WIDGET(g->vbox_blur));
      gtk_widget_hide(GTK_WIDGET(g->vbox_fill));
      break;
    case DT_IOP_RETOUCH_FILL:
      gtk_widget_hide(GTK_WIDGET(g->vbox_blur));
      gtk_widget_show(GTK_WIDGET(g->vbox_fill));
      if(p->fill_mode == DT_IOP_RETOUCH_FILL_COLOR)
        gtk_widget_show(GTK_WIDGET(g->hbox_color_pick));
      else
        gtk_widget_hide(GTK_WIDGET(g->hbox_color_pick));
      break;
    case DT_IOP_RETOUCH_CLONE:
    default:
      gtk_widget_hide(GTK_WIDGET(g->vbox_blur));
      gtk_widget_hide(GTK_WIDGET(g->vbox_fill));
      break;
  }

  if(g->display_wavelet_scale)
    gtk_widget_show(GTK_WIDGET(g->vbox_preview_scale));
  else
    gtk_widget_hide(GTK_WIDGET(g->vbox_preview_scale));

  const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, rt_get_selected_shape_id());
  if(form && !creation_continuous)
    gtk_widget_show(GTK_WIDGET(g->sl_mask_opacity));
  else
    gtk_widget_hide(GTK_WIDGET(g->sl_mask_opacity));
}

static void rt_display_selected_shapes_lbl(dt_iop_retouch_gui_data_t *g)
{
  const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, rt_get_selected_shape_id());
  if(form)
    gtk_label_set_text(g->label_form_selected, form->name);
  else
    gtk_label_set_text(g->label_form_selected, _("none"));
}

static int rt_get_selected_shape_index(dt_iop_retouch_params_t *p)
{
  return rt_get_index_from_formid(p, rt_get_selected_shape_id());
}

static void rt_shape_selection_changed(dt_iop_module_t *self)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  ++darktable.gui->reset;

  int selection_changed = 0;

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    dt_bauhaus_slider_set(g->sl_mask_opacity, rt_get_shape_opacity(self, p->rt_forms[index].formid));

    if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_BLUR)
    {
      p->blur_type = p->rt_forms[index].blur_type;
      p->blur_radius = p->rt_forms[index].blur_radius;

      dt_bauhaus_combobox_set(g->cmb_blur_type, p->blur_type);
      dt_bauhaus_slider_set(g->sl_blur_radius, p->blur_radius);

      selection_changed = 1;
    }
    else if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_FILL)
    {
      p->fill_mode = p->rt_forms[index].fill_mode;
      p->fill_brightness = p->rt_forms[index].fill_brightness;
      p->fill_color[0] = p->rt_forms[index].fill_color[0];
      p->fill_color[1] = p->rt_forms[index].fill_color[1];
      p->fill_color[2] = p->rt_forms[index].fill_color[2];

      dt_bauhaus_slider_set(g->sl_fill_brightness, p->fill_brightness);
      dt_bauhaus_combobox_set(g->cmb_fill_mode, p->fill_mode);
      rt_display_selected_fill_color(g, p);

      selection_changed = 1;
    }

    if(p->algorithm != p->rt_forms[index].algorithm)
    {
      p->algorithm = p->rt_forms[index].algorithm;

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_clone), (p->algorithm == DT_IOP_RETOUCH_CLONE));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_heal), (p->algorithm == DT_IOP_RETOUCH_HEAL));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_blur), (p->algorithm == DT_IOP_RETOUCH_BLUR));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_fill), (p->algorithm == DT_IOP_RETOUCH_FILL));

      selection_changed = 1;
    }

    if(selection_changed) rt_show_hide_controls(self);
  }

  rt_display_selected_shapes_lbl(g);

  const int creation_continuous = (darktable.develop->form_gui && darktable.develop->form_gui->creation_continuous
                                   && darktable.develop->form_gui->creation_continuous_module == self);

  if(index >= 0 && !creation_continuous)
    gtk_widget_show(GTK_WIDGET(g->sl_mask_opacity));
  else
    gtk_widget_hide(GTK_WIDGET(g->sl_mask_opacity));

  --darktable.gui->reset;

  if(selection_changed) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

//---------------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------------

static void rt_masks_form_change_opacity(dt_iop_module_t *self, int formid, float opacity)
{
  dt_masks_point_group_t *grpt = rt_get_mask_point_group(self, formid);
  if(grpt)
  {
    grpt->opacity = CLAMP(opacity, 0.05f, 1.0f);
    dt_conf_set_float("plugins/darkroom/masks/opacity", grpt->opacity);

    dt_dev_add_masks_history_item(darktable.develop, self, TRUE);
  }
}

static float rt_masks_form_get_opacity(dt_iop_module_t *self, int formid)
{
  dt_masks_point_group_t *grpt = rt_get_mask_point_group(self, formid);
  if(grpt)
    return grpt->opacity;
  else
    return 1.0f;
}

static void rt_paste_forms_from_scale(dt_iop_retouch_params_t *p, const int source_scale, const int dest_scale)
{
  if(source_scale != dest_scale && source_scale >= 0 && dest_scale >= 0)
  {
    for(int i = 0; i < RETOUCH_NO_FORMS; i++)
    {
      if(p->rt_forms[i].scale == source_scale) p->rt_forms[i].scale = dest_scale;
    }
  }
}

static int rt_allow_create_form(dt_iop_module_t *self)
{
  int allow = 1;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  if(p)
  {
    allow = (p->rt_forms[RETOUCH_NO_FORMS - 1].formid == 0);
  }
  return allow;
}

static void rt_reset_form_creation(GtkWidget *widget, dt_iop_module_t *self)
{
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_path))
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_circle))
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_ellipse))
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_brush)))
  {
    // we unset the creation mode
    dt_masks_change_form_gui(NULL);
    darktable.develop->form_gui->creation_continuous = FALSE;
    darktable.develop->form_gui->creation_continuous_module = NULL;
  }

  if(widget != g->bt_path) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), FALSE);
  if(widget != g->bt_circle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), FALSE);
  if(widget != g->bt_ellipse) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), FALSE);
  if(widget != g->bt_brush) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), FALSE);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_showmask), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_suppress), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->colorpicker), FALSE);
}

static void rt_show_forms_for_current_scale(dt_iop_module_t *self)
{
  if(!self->enabled || darktable.develop->gui_module != self || darktable.develop->form_gui->creation
     || darktable.develop->form_gui->creation_continuous)
    return;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  if(bd == NULL) return;

  const int scale = p->curr_scale;
  int count = 0;

  // check if there is a shape on this scale
  for(int i = 0; i < RETOUCH_NO_FORMS && count == 0; i++)
  {
    if(p->rt_forms[i].formid != 0 && p->rt_forms[i].scale == scale) count++;
  }

  // if there are shapes on this scale, make the cut shapes button sensitive
  gtk_widget_set_sensitive(g->bt_copy_scale, count > 0);

  // if no shapes on this scale, we hide all
  if(bd->masks_shown == DT_MASKS_EDIT_OFF || count == 0)
  {
    dt_masks_change_form_gui(NULL);

    if(g)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks),
                                   (bd->masks_shown != DT_MASKS_EDIT_OFF)
                                       && (darktable.develop->gui_module == self));

    dt_control_queue_redraw_center();
    return;
  }

  // else, we create a new from group with the shapes and display it
  dt_masks_form_t *grp = dt_masks_create_ext(DT_MASKS_GROUP);
  for(int i = 0; i < RETOUCH_NO_FORMS; i++)
  {
    if(p->rt_forms[i].scale == scale)
    {
      const int grid = self->blend_params->mask_id;
      const int formid = p->rt_forms[i].formid;
      dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
      if(form)
      {
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = formid;
        fpt->parentid = grid;
        fpt->state = DT_MASKS_STATE_USE;
        fpt->opacity = 1.0f;
        grp->points = g_list_append(grp->points, fpt);
      }
    }
  }

  dt_masks_form_t *grp2 = dt_masks_create_ext(DT_MASKS_GROUP);
  grp2->formid = 0;
  dt_masks_group_ungroup(grp2, grp);
  dt_masks_change_form_gui(grp2);
  darktable.develop->form_gui->edit_mode = bd->masks_shown;

  if(g)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks),
                                 (bd->masks_shown != DT_MASKS_EDIT_OFF) && (darktable.develop->gui_module == self));

  dt_control_queue_redraw_center();
}

// called if a shape is added or deleted
static void rt_resynch_params(struct dt_iop_module_t *self)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_develop_blend_params_t *bp = self->blend_params;

  dt_iop_retouch_form_data_t forms_d[RETOUCH_NO_FORMS];
  memset(forms_d, 0, sizeof(dt_iop_retouch_form_data_t) * RETOUCH_NO_FORMS);

  // we go through all forms in blend params
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, bp->mask_id);
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    int new_form_index = 0;
    for(GList *forms = grp->points; (new_form_index < RETOUCH_NO_FORMS) && forms; forms = g_list_next(forms))
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt)
      {
        const int formid = grpt->formid;

        // search for the form on the shapes array
        const int form_index = rt_get_index_from_formid(p, formid);

        // if it exists copy it to the new array
        if(form_index >= 0)
        {
          forms_d[new_form_index] = p->rt_forms[form_index];

          new_form_index++;
        }
        else
        {
          // if it does not exists add it to the new array
          const dt_masks_form_t *parent_form = dt_masks_get_from_id(darktable.develop, formid);
          if(parent_form)
          {
            forms_d[new_form_index].formid = formid;
            forms_d[new_form_index].scale = p->curr_scale;
            forms_d[new_form_index].algorithm = p->algorithm;
            forms_d[new_form_index].distort_mode = 2;

            switch(forms_d[new_form_index].algorithm)
            {
              case DT_IOP_RETOUCH_BLUR:
                forms_d[new_form_index].blur_type = p->blur_type;
                forms_d[new_form_index].blur_radius = p->blur_radius;
                break;
              case DT_IOP_RETOUCH_FILL:
                forms_d[new_form_index].fill_mode = p->fill_mode;
                forms_d[new_form_index].fill_color[0] = p->fill_color[0];
                forms_d[new_form_index].fill_color[1] = p->fill_color[1];
                forms_d[new_form_index].fill_color[2] = p->fill_color[2];
                forms_d[new_form_index].fill_brightness = p->fill_brightness;
                break;
              default:
                break;
            }

            new_form_index++;
          }
        }
      }
    }
  }

  // we reaffect params
  for(int i = 0; i < RETOUCH_NO_FORMS; i++)
  {
    p->rt_forms[i] = forms_d[i];
  }
}

static gboolean rt_masks_form_is_in_roi(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                        dt_masks_form_t *form, const dt_iop_roi_t *roi_in,
                                        const dt_iop_roi_t *roi_out)
{
  // we get the area for the form
  int fl, ft, fw, fh;

  if(!dt_masks_get_area(self, piece, form, &fw, &fh, &fl, &ft)) return FALSE;

  // is the form outside of the roi?
  fw *= roi_in->scale, fh *= roi_in->scale, fl *= roi_in->scale, ft *= roi_in->scale;
  if(ft >= roi_out->y + roi_out->height || ft + fh <= roi_out->y || fl >= roi_out->x + roi_out->width
     || fl + fw <= roi_out->x)
    return FALSE;

  return TRUE;
}

static void rt_masks_point_denormalize(dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi, const float *points,
                                       size_t points_count, float *new)
{
  const float scalex = piece->pipe->iwidth * roi->scale, scaley = piece->pipe->iheight * roi->scale;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    new[i] = points[i] * scalex;
    new[i + 1] = points[i + 1] * scaley;
  }
}

static int rt_masks_point_calc_delta(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi,
                                     const float *target, const float *source, float *dx, float *dy,
                                     const int distort_mode)
{
  // if distort_mode==1 we don't scale at the right place, hence false positions if there's distortion before this
  // module. we keep it for backward compatibility only. all new forms have distort_mode==2
  dt_boundingbox_t points;
  if(distort_mode == 1)
  {
    rt_masks_point_denormalize(piece, roi, target, 1, points);
    rt_masks_point_denormalize(piece, roi, source, 1, points + 2);
  }
  else
  {
    points[0] = target[0] * piece->pipe->iwidth;
    points[1] = target[1] * piece->pipe->iheight;
    points[2] = source[0] * piece->pipe->iwidth;
    points[3] = source[1] * piece->pipe->iheight;
  }

  const int res = dt_dev_distort_transform_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, 2);
  if(!res) return res;

  if(distort_mode == 1)
  {
    *dx = points[0] - points[2];
    *dy = points[1] - points[3];
  }
  else
  {
    *dx = (points[0] - points[2]) * roi->scale;
    *dy = (points[1] - points[3]) * roi->scale;
  }

  return res;
}

/* returns (dx dy) to get from the source to the destination */
static int rt_masks_get_delta_to_destination(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                             const dt_iop_roi_t *roi, dt_masks_form_t *form, float *dx, float *dy,
                                             const int distort_mode)
{
  int res = 0;

  if(form->type & DT_MASKS_PATH)
  {
    const dt_masks_point_path_t *pt = (dt_masks_point_path_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->corner, form->source, dx, dy, distort_mode);
  }
  else if(form->type & DT_MASKS_CIRCLE)
  {
    const dt_masks_point_circle_t *pt = (dt_masks_point_circle_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->center, form->source, dx, dy, distort_mode);
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    const dt_masks_point_ellipse_t *pt = (dt_masks_point_ellipse_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->center, form->source, dx, dy, distort_mode);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    const dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->corner, form->source, dx, dy, distort_mode);
  }

  return res;
}

static void rt_clamp_minmax(float levels_old[3], float levels_new[3])
{
  // left or right has changed
  if((levels_old[0] != levels_new[0] || levels_old[2] != levels_new[2]) && levels_old[1] == levels_new[1])
  {
    // if old left and right are the same just use the new values
    if(levels_old[2] != levels_old[0])
    {
      // set the new value but keep the middle proportional
      const float left = MAX(levels_new[0], RETOUCH_PREVIEW_LVL_MIN);
      const float right = MIN(levels_new[2], RETOUCH_PREVIEW_LVL_MAX);

      const float percentage = (levels_old[1] - levels_old[0]) / (levels_old[2] - levels_old[0]);
      levels_new[1] = left + (right - left) * percentage;
      levels_new[0] = left;
      levels_new[2] = right;
    }
  }

  // if all zero make it gray
  if(levels_new[0] == 0.f && levels_new[1] == 0.f && levels_new[2] == 0.f)
  {
    levels_new[0] = -1.5f;
    levels_new[1] = 0.f;
    levels_new[2] = 1.5f;
  }

  // check the range
  if(levels_new[2] < levels_new[0] + 0.05f * 2.f) levels_new[2] = levels_new[0] + 0.05f * 2.f;
  if(levels_new[1] < levels_new[0] + 0.05f) levels_new[1] = levels_new[0] + 0.05f;
  if(levels_new[1] > levels_new[2] - 0.05f) levels_new[1] = levels_new[2] - 0.05f;

  {
    // set the new value but keep the middle proportional
    const float left = MAX(levels_new[0], RETOUCH_PREVIEW_LVL_MIN);
    const float right = MIN(levels_new[2], RETOUCH_PREVIEW_LVL_MAX);

    const float percentage = (levels_new[1] - levels_new[0]) / (levels_new[2] - levels_new[0]);
    levels_new[1] = left + (right - left) * percentage;
    levels_new[0] = left;
    levels_new[2] = right;
  }
}

static int rt_shape_is_being_added(dt_iop_module_t *self, const int shape_type)
{
  int being_added = 0;

  if(self->dev->form_gui && self->dev->form_visible
     && ((self->dev->form_gui->creation && self->dev->form_gui->creation_module == self)
         || (self->dev->form_gui->creation_continuous && self->dev->form_gui->creation_continuous_module == self)))
  {
    if(self->dev->form_visible->type & DT_MASKS_GROUP)
    {
      GList *forms = self->dev->form_visible->points;
      if(forms)
      {
        dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
        if(grpt)
        {
          const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, grpt->formid);
          if(form) being_added = (form->type & shape_type);
        }
      }
    }
    else
      being_added = (self->dev->form_visible->type & shape_type);
  }
  return being_added;
}

static gboolean rt_add_shape(GtkWidget *widget, const int creation_continuous, dt_iop_module_t *self)
{
  //turn module on (else shape creation won't work)
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  //switch mask edit mode off
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
  if(bd) bd->masks_shown = DT_MASKS_EDIT_OFF;

  const int allow = rt_allow_create_form(self);
  if(allow)
  {
    rt_reset_form_creation(widget, self);

    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
    {
      rt_show_forms_for_current_scale(self);

      return FALSE;
    }

    dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
    dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

    // we want to be sure that the iop has focus
    dt_iop_request_focus(self);

    dt_masks_type_t type = DT_MASKS_CIRCLE;
    if(widget == g->bt_path)
      type = DT_MASKS_PATH;
    else if(widget == g->bt_circle)
      type = DT_MASKS_CIRCLE;
    else if(widget == g->bt_ellipse)
      type = DT_MASKS_ELLIPSE;
    else if(widget == g->bt_brush)
      type = DT_MASKS_BRUSH;

    // we create the new form
    dt_masks_form_t *spot = NULL;
    if(p->algorithm == DT_IOP_RETOUCH_CLONE || p->algorithm == DT_IOP_RETOUCH_HEAL)
      spot = dt_masks_create(type | DT_MASKS_CLONE);
    else
      spot = dt_masks_create(type | DT_MASKS_NON_CLONE);

    dt_masks_change_form_gui(spot);
    darktable.develop->form_gui->creation_module = self;

    if(creation_continuous)
    {
      darktable.develop->form_gui->creation_continuous = TRUE;
      darktable.develop->form_gui->creation_continuous_module = self;
    }
    else
    {
      darktable.develop->form_gui->creation_continuous = FALSE;
      darktable.develop->form_gui->creation_continuous_module = NULL;
    }

    dt_control_queue_redraw_center();
  }
  else
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);

  return !allow;
}

//---------------------------------------------------------------------------------
// GUI callbacks
//---------------------------------------------------------------------------------

static void rt_colorpick_color_set_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  // turn off the other color picker
  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c
      = (GdkRGBA){.red = p->fill_color[0], .green = p->fill_color[1], .blue = p->fill_color[2], .alpha = 1.0 };
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->fill_color[0] = c.red;
  p->fill_color[1] = c.green;
  p->fill_color[2] = c.blue;

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_FILL)
    {
      p->rt_forms[index].fill_color[0] = p->fill_color[0];
      p->rt_forms[index].fill_color[1] = p->fill_color[1];
      p->rt_forms[index].fill_color[2] = p->fill_color[2];
    }
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// wavelet decompose bar
#define RT_WDBAR_INSET 0.2f
#define lw DT_PIXEL_APPLY_DPI(1.0f)

static void rt_update_wd_bar_labels(dt_iop_retouch_params_t *p, dt_iop_retouch_gui_data_t *g)
{
  char text[256];

  snprintf(text, sizeof(text), "%i", p->curr_scale);
  gtk_label_set_text(g->lbl_curr_scale, text);

  snprintf(text, sizeof(text), "%i", p->num_scales);
  gtk_label_set_text(g->lbl_num_scales, text);

  snprintf(text, sizeof(text), "%i", p->merge_from_scale);
  gtk_label_set_text(g->lbl_merge_from_scale, text);
}

static void rt_num_scales_update(const int _num_scales, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  const int num_scales = CLAMP(_num_scales, 0, RETOUCH_MAX_SCALES);
  if(p->num_scales == num_scales) return;

  p->num_scales = num_scales;

  if(p->num_scales < p->merge_from_scale) p->merge_from_scale = p->num_scales;

  rt_update_wd_bar_labels(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rt_curr_scale_update(const int _curr_scale, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  const int curr_scale = CLAMP(_curr_scale, 0, RETOUCH_MAX_SCALES + 1);
  if(p->curr_scale == curr_scale) return;

  p->curr_scale = curr_scale;

  rt_show_forms_for_current_scale(self);

  // compute auto levels only the first time display wavelet scale is used,
  // only if levels values are the default
  // and a detail scale is displayed
  dt_iop_gui_enter_critical_section(self);
  if(g->displayed_wavelet_scale == 0 && p->preview_levels[0] == RETOUCH_PREVIEW_LVL_MIN
     && p->preview_levels[1] == 0.f && p->preview_levels[2] == RETOUCH_PREVIEW_LVL_MAX
     && g->preview_auto_levels == 0 && p->curr_scale > 0 && p->curr_scale <= p->num_scales)
  {
    g->preview_auto_levels = 1;
    g->displayed_wavelet_scale = 1;
  }
  dt_iop_gui_leave_critical_section(self);

  rt_update_wd_bar_labels(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rt_merge_from_scale_update(const int _merge_from_scale, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  const int merge_from_scale = CLAMP(_merge_from_scale, 0, p->num_scales);
  if(p->merge_from_scale == merge_from_scale) return;

  p->merge_from_scale = merge_from_scale;

  rt_update_wd_bar_labels(p, g);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gboolean rt_wdbar_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  g->wdbar_mouse_x = g->wdbar_mouse_y = -1;
  g->curr_scale = -1;
  g->lower_cursor = g->upper_cursor = FALSE;
  g->lower_margin = g->upper_margin = FALSE;

  gtk_widget_queue_draw(g->wd_bar);
  return TRUE;
}

static gboolean rt_wdbar_button_press(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_request_focus(self);

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = round(RT_WDBAR_INSET * allocation.height);
  const float box_w = (allocation.width - 2.0f * inset) / (float)RETOUCH_NO_SCALES;

  if(event->button == 1)
  {
    if(g->lower_margin) // bottom slider
    {
      if(g->lower_cursor) // is over the arrow?
        g->is_dragging = DT_IOP_RETOUCH_WDBAR_DRAG_BOTTOM;
      else
        rt_num_scales_update(g->wdbar_mouse_x / box_w, self);
    }
    else if(g->upper_margin) // top slider
    {
      if(g->upper_cursor) // is over the arrow?
        g->is_dragging = DT_IOP_RETOUCH_WDBAR_DRAG_TOP;
      else
        rt_merge_from_scale_update(g->wdbar_mouse_x / box_w, self);
    }
    else if(g->curr_scale >= 0)
      rt_curr_scale_update(g->curr_scale, self);
  }

  gtk_widget_queue_draw(g->wd_bar);
  return TRUE;
}

static gboolean rt_wdbar_button_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(event->button == 1) g->is_dragging = 0;

  gtk_widget_queue_draw(g->wd_bar);
  return TRUE;
}

static gboolean rt_wdbar_scrolled(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  if(dt_gui_ignore_scroll(event)) return FALSE;

  if(darktable.gui->reset) return TRUE;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  dt_iop_request_focus(self);

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(g->lower_margin) // bottom slider
      rt_num_scales_update(p->num_scales - delta_y, self);
    else if(g->upper_margin) // top slider
      rt_merge_from_scale_update(p->merge_from_scale - delta_y, self);
    else if(g->curr_scale >= 0)
      rt_curr_scale_update(p->curr_scale - delta_y, self);
  }

  gtk_widget_queue_draw(g->wd_bar);
  return TRUE;
}

static gboolean rt_wdbar_motion_notify(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = round(RT_WDBAR_INSET * allocation.height);
  const float box_w = (allocation.width - 2.0f * inset) / (float)RETOUCH_NO_SCALES;
  const float sh = 3.0f * lw + inset;


  /* record mouse position within control */
  g->wdbar_mouse_x = CLAMP(event->x - inset, 0, allocation.width - 2.0f * inset - 1.0f);
  g->wdbar_mouse_y = event->y;

  g->curr_scale = g->wdbar_mouse_x / box_w;
  g->lower_cursor = g->upper_cursor = FALSE;
  g->lower_margin = g->upper_margin = FALSE;
  if(g->wdbar_mouse_y <= sh)
  {
    g->upper_margin = TRUE;
    float middle = box_w * (0.5f + (float)p->merge_from_scale);
    g->upper_cursor = (g->wdbar_mouse_x >= (middle - inset)) && (g->wdbar_mouse_x <= (middle + inset));
    if(!(g->is_dragging)) g->curr_scale = -1;
  }
  else if(g->wdbar_mouse_y >= allocation.height - sh)
  {
    g->lower_margin = TRUE;
    float middle = box_w * (0.5f + (float)p->num_scales);
    g->lower_cursor = (g->wdbar_mouse_x >= (middle - inset)) && (g->wdbar_mouse_x <= (middle + inset));
    if(!(g->is_dragging)) g->curr_scale = -1;
  }

  if(g->is_dragging == DT_IOP_RETOUCH_WDBAR_DRAG_BOTTOM)
    rt_num_scales_update(g->curr_scale, self);

  if(g->is_dragging == DT_IOP_RETOUCH_WDBAR_DRAG_TOP)
    rt_merge_from_scale_update(g->curr_scale, self);

  gtk_widget_queue_draw(g->wd_bar);
  return TRUE;
}

static int rt_scale_has_shapes(dt_iop_retouch_params_t *p, const int scale)
{
  int has_shapes = 0;

  for(int i = 0; i < RETOUCH_NO_FORMS && has_shapes == 0; i++)
    has_shapes = (p->rt_forms[i].formid != 0 && p->rt_forms[i].scale == scale);

  return has_shapes;
}

static gboolean rt_wdbar_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;


  GdkRGBA border      = {0.066, 0.066, 0.066, 1};
  GdkRGBA original    = {.1, .1, .1, 1};
  GdkRGBA inactive    = {.15, .15, .15, 1};
  GdkRGBA active      = {.35, .35, .35, 1};
  GdkRGBA merge_from  = {.5, .5, .5, 1};
  GdkRGBA residual    = {.8, .8, .8, 1};
  GdkRGBA shapes      = {.75, .5, .0, 1};
  GdkRGBA color;

  float middle;
  const int first_scale_visible = (g->first_scale_visible > 0) ? g->first_scale_visible : RETOUCH_MAX_SCALES;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);

  // clear background
  gdk_cairo_set_source_rgba(cr, &inactive);
  cairo_paint(cr);
  cairo_save(cr);

  // geometry
  const int inset = round(RT_WDBAR_INSET * allocation.height);
  const int mk = 2 * inset;
  const float sh = 3.0f * lw + inset;
  const float box_w = (allocation.width - 2.0f * inset) / (float)RETOUCH_NO_SCALES;
  const float box_h = allocation.height - 2.0f * sh;

  // render the boxes
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  for(int i = 0; i < RETOUCH_NO_SCALES; i++)
  {
    // draw box background
    if(i == 0)
      color = original;
    else if(i == p->num_scales + 1)
      color = residual;
    else if(i >= p->merge_from_scale && i <= p->num_scales && p->merge_from_scale > 0)
      color = merge_from;
    else if(i <= p->num_scales)
      color = active;
    else
      color = inactive;

    gdk_cairo_set_source_rgba(cr, &color);
    cairo_rectangle(cr, box_w * i + inset, sh, box_w, box_h);
    cairo_fill(cr);

    // if detail scale is visible at current zoom level inform it
    if(i >= first_scale_visible && i <= p->num_scales)
    {
      gdk_cairo_set_source_rgba(cr, &merge_from);
      cairo_rectangle(cr, box_w * i + inset, lw, box_w, 2.0f * lw);
      cairo_fill(cr);
    }

    // if the scale has shapes inform it
    if(rt_scale_has_shapes(p, i))
    {
      cairo_set_line_width(cr, lw);
      gdk_cairo_set_source_rgba(cr, &shapes);
      cairo_rectangle(cr, box_w * i + inset + lw / 2.0f, allocation.height - sh, box_w - lw, 2.0f * lw);
      cairo_fill(cr);
    }

    // draw the border
    cairo_set_line_width(cr, lw);
    gdk_cairo_set_source_rgba(cr, &border);
    cairo_rectangle(cr, box_w * i + inset, sh, box_w, box_h);
    cairo_stroke(cr);
  }

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
  cairo_restore(cr);

  // dot for the current scale
  if(p->curr_scale >= p->merge_from_scale && p->curr_scale <= p->num_scales && p->merge_from_scale > 0)
    color = active;
  else
    color = merge_from;

  if(p->curr_scale >= 0 && p->curr_scale < RETOUCH_NO_SCALES)
  {
    cairo_set_line_width(cr, lw);
    gdk_cairo_set_source_rgba(cr, &color);
    middle = box_w * (0.5f + (float)p->curr_scale);
    cairo_arc(cr, middle + inset, 0.5f * box_h + sh, 0.5f * inset, 0, 2.0f * M_PI);
    cairo_fill(cr);
    cairo_stroke(cr);
  }

  // mouse hover on a scale
  if(g->curr_scale >= 0)
  {
    cairo_set_line_width(cr, lw);
    if(g->curr_scale == p->num_scales + 1) color = inactive;
    else color = residual;
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_rectangle(cr, box_w * g->curr_scale + inset + lw, sh + lw, box_w - 2.0f * lw, box_h - 2.0f * lw);
    cairo_stroke(cr);
  }

  /* render control points handles */

  // draw number of scales arrow (bottom arrow)
  middle = box_w * (0.5f + (float)p->num_scales);
  if(g->lower_cursor || g->is_dragging == DT_IOP_RETOUCH_WDBAR_DRAG_BOTTOM)
  {
    cairo_set_source_rgb(cr, 0.67, 0.67, 0.67);
    dtgtk_cairo_paint_solid_triangle(cr, middle, box_h + 5.0f * lw, mk, mk, CPF_DIRECTION_UP, NULL);
  }
  else
  {
    cairo_set_source_rgb(cr, 0.54, 0.54, 0.54);
    dtgtk_cairo_paint_triangle(cr, middle, box_h + 5.0f * lw, mk, mk, CPF_DIRECTION_UP, NULL);
  }

  // draw merge scales arrow (top arrow)
  middle = box_w * (0.5f + (float)p->merge_from_scale);
  if(g->upper_cursor || g->is_dragging == DT_IOP_RETOUCH_WDBAR_DRAG_TOP)
  {
    cairo_set_source_rgb(cr, 0.67, 0.67, 0.67);
    dtgtk_cairo_paint_solid_triangle(cr, middle, 3.0f * lw, mk, mk, CPF_DIRECTION_DOWN, NULL);
  }
  else
  {
    cairo_set_source_rgb(cr, 0.54, 0.54, 0.54);
    dtgtk_cairo_paint_triangle(cr, middle, 3.0f * lw, mk, mk, CPF_DIRECTION_DOWN, NULL);
  }

  /* push mem surface into widget */
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

static float rt_gslider_scale_callback(GtkWidget *self, float inval, int dir)
{
  float outval;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (inval - RETOUCH_PREVIEW_LVL_MIN) / (RETOUCH_PREVIEW_LVL_MAX - RETOUCH_PREVIEW_LVL_MIN);
      break;
    case GRADIENT_SLIDER_GET:
      outval = (RETOUCH_PREVIEW_LVL_MAX - RETOUCH_PREVIEW_LVL_MIN) * inval + RETOUCH_PREVIEW_LVL_MIN;
      break;
    default:
      outval = inval;
  }
  return outval;
}


static void rt_gslider_changed(GtkDarktableGradientSlider *gslider, dt_iop_module_t *self)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  double dlevels[3];

  if(darktable.gui->reset) return;

  dtgtk_gradient_slider_multivalue_get_values(gslider, dlevels);

  for(int i = 0; i < 3; i++) p->preview_levels[i] = dlevels[i];

  dt_dev_add_history_item(darktable.develop, self, TRUE);

}


void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  if(fabsf(p->fill_color[0] - self->picked_output_color[0]) < 0.0001f
     && fabsf(p->fill_color[1] - self->picked_output_color[1]) < 0.0001f
     && fabsf(p->fill_color[2] - self->picked_output_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  p->fill_color[0] = self->picked_output_color[0];
  p->fill_color[1] = self->picked_output_color[1];
  p->fill_color[2] = self->picked_output_color[2];

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_FILL)
    {
      p->rt_forms[index].fill_color[0] = p->fill_color[0];
      p->rt_forms[index].fill_color[1] = p->fill_color[1];
      p->rt_forms[index].fill_color[2] = p->fill_color[2];
    }
  }

  rt_display_selected_fill_color(g, p);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gboolean rt_copypaste_scale_callback(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  ++darktable.gui->reset;

  int scale_copied = 0;
  const int active = !gtk_toggle_button_get_active(togglebutton);
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(togglebutton == (GtkToggleButton *)g->bt_copy_scale)
  {
    g->copied_scale = (active) ? p->curr_scale : -1;
  }
  else if(togglebutton == (GtkToggleButton *)g->bt_paste_scale)
  {
    rt_paste_forms_from_scale(p, g->copied_scale, p->curr_scale);
    rt_show_forms_for_current_scale(self);

    scale_copied = 1;
    g->copied_scale = -1;
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_copy_scale), g->copied_scale >= 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_paste_scale), g->copied_scale >= 0);
  gtk_widget_set_sensitive(g->bt_paste_scale, g->copied_scale >= 0);

  --darktable.gui->reset;

  if(scale_copied) dt_dev_add_history_item(darktable.develop, self, TRUE);

  return TRUE;
}

static gboolean rt_display_wavelet_scale_callback(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display wavelet scales
  if(self->request_mask_display && !g->mask_display)
  {
    dt_control_log(_("cannot display scales when the blending mask is displayed"));

    ++darktable.gui->reset;
    gtk_toggle_button_set_active(togglebutton, FALSE);
    --darktable.gui->reset;
    return TRUE;
  }

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);

  g->display_wavelet_scale = !gtk_toggle_button_get_active(togglebutton);

  rt_show_hide_controls(self);

  // compute auto levels only the first time display wavelet scale is used,
  // only if levels values are the default
  // and a detail scale is displayed
  dt_iop_gui_enter_critical_section(self);
  if(g->displayed_wavelet_scale == 0 && p->preview_levels[0] == RETOUCH_PREVIEW_LVL_MIN
     && p->preview_levels[1] == 0.f && p->preview_levels[2] == RETOUCH_PREVIEW_LVL_MAX
     && g->preview_auto_levels == 0 && p->curr_scale > 0 && p->curr_scale <= p->num_scales)
  {
    g->preview_auto_levels = 1;
    g->displayed_wavelet_scale = 1;
  }
  dt_iop_gui_leave_critical_section(self);

  dt_dev_reprocess_center(self->dev);

  gtk_toggle_button_set_active(togglebutton, g->display_wavelet_scale);
  return TRUE;
}

static void rt_develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  // FIXME: this doesn't seems the right place to update params and GUI ...
  // update auto levels
  dt_iop_gui_enter_critical_section(self);
  if(g->preview_auto_levels == 2)
  {
    g->preview_auto_levels = -1;

    dt_iop_gui_leave_critical_section(self);

    for(int i = 0; i < 3; i++) p->preview_levels[i] = g->preview_levels[i];

    dt_dev_add_history_item(darktable.develop, self, TRUE);

    dt_iop_gui_enter_critical_section(self);

    // update the gradient slider
    double dlevels[3];
    for(int i = 0; i < 3; i++) dlevels[i] = p->preview_levels[i];

    ++darktable.gui->reset;
    dtgtk_gradient_slider_multivalue_set_values(g->preview_levels_gslider, dlevels);
    --darktable.gui->reset;

    g->preview_auto_levels = 0;
  }
  dt_iop_gui_leave_critical_section(self);

  // just in case zoom level has changed
  gtk_widget_queue_draw(GTK_WIDGET(g->wd_bar));
}

static gboolean rt_auto_levels_callback(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);

  dt_iop_gui_enter_critical_section(self);
  if(g->preview_auto_levels == 0)
  {
    g->preview_auto_levels = 1;
  }
  dt_iop_gui_leave_critical_section(self);

  dt_iop_refresh_center(self);

  return TRUE;
}

static void rt_mask_opacity_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  const int shape_id = rt_get_selected_shape_id();

  if(shape_id > 0)
  {
    const float opacity = dt_bauhaus_slider_get(slider);
    rt_masks_form_change_opacity(self, shape_id, opacity);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_post_expose (struct dt_iop_module_t *self,
                      cairo_t *cr,
                      int32_t width,
                      int32_t height,
                      int32_t pointerx,
                      int32_t pointery)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  const int shape_id = rt_get_selected_shape_id();

  if(shape_id > 0)
  {
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->sl_mask_opacity, rt_masks_form_get_opacity(self, shape_id));
    --darktable.gui->reset;
  }
}

static gboolean rt_edit_masks_callback(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  // if we don't have the focus, request for it and quit, gui_focus() do the rest
  if(darktable.develop->gui_module != self)
  {
    dt_iop_request_focus(self);
    return FALSE;
  }

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  //hide all shapes and free if some are in creation
  if(darktable.develop->form_gui->creation && darktable.develop->form_gui->creation_module == self)
    dt_masks_change_form_gui(NULL);

  if(darktable.develop->form_gui->creation_continuous_module == self)
  {
    darktable.develop->form_gui->creation_continuous = FALSE;
    darktable.develop->form_gui->creation_continuous_module = NULL;
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), FALSE);

  if(event->button == 1)
  {
    ++darktable.gui->reset;

    dt_iop_color_picker_reset(self, TRUE);

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, self->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
    {
      const gboolean control_button_pressed = dt_modifier_is(event->state, GDK_CONTROL_MASK);

      switch(bd->masks_shown)
      {
        case DT_MASKS_EDIT_FULL:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_OFF;
          break;

        case DT_MASKS_EDIT_RESTRICTED:
          bd->masks_shown = !control_button_pressed ? DT_MASKS_EDIT_FULL : DT_MASKS_EDIT_OFF;
          break;

        default:
        case DT_MASKS_EDIT_OFF:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_FULL;
          break;
      }
    }
    else
      bd->masks_shown = DT_MASKS_EDIT_OFF;

    rt_show_forms_for_current_scale(self);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks),
                                 (bd->masks_shown != DT_MASKS_EDIT_OFF) && (darktable.develop->gui_module == self));

    --darktable.gui->reset;

    return TRUE;
  }

  return TRUE;
}

static gboolean rt_add_shape_callback(GtkWidget *widget, GdkEventButton *e, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(darktable.gui->reset) return FALSE;

  dt_iop_color_picker_reset(self, TRUE);

  const int creation_continuous = dt_modifier_is(e->state, GDK_CONTROL_MASK);

  rt_add_shape(widget, creation_continuous, self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), rt_shape_is_being_added(self, DT_MASKS_CIRCLE));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), rt_shape_is_being_added(self, DT_MASKS_PATH));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), rt_shape_is_being_added(self, DT_MASKS_ELLIPSE));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), rt_shape_is_being_added(self, DT_MASKS_BRUSH));

  return TRUE;
}

static gboolean rt_select_algorithm_callback(GtkToggleButton *togglebutton, GdkEventButton *e,
                                             dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  ++darktable.gui->reset;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  dt_iop_retouch_algo_type_t new_algo = DT_IOP_RETOUCH_HEAL;

  if(togglebutton == (GtkToggleButton *)g->bt_blur)
    new_algo = DT_IOP_RETOUCH_BLUR;
  else if(togglebutton == (GtkToggleButton *)g->bt_clone)
    new_algo = DT_IOP_RETOUCH_CLONE;
  else if(togglebutton == (GtkToggleButton *)g->bt_heal)
    new_algo = DT_IOP_RETOUCH_HEAL;
  else if(togglebutton == (GtkToggleButton *)g->bt_fill)
    new_algo = DT_IOP_RETOUCH_FILL;

  // check if we have to do something
  gboolean accept = TRUE;

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0 && dt_modifier_is(e->state, GDK_CONTROL_MASK))
  {
    if(new_algo != p->rt_forms[index].algorithm)
    {
      // we restrict changes to clone<->heal and blur<->fill
      if((new_algo == DT_IOP_RETOUCH_CLONE && p->rt_forms[index].algorithm != DT_IOP_RETOUCH_HEAL)
         || (new_algo == DT_IOP_RETOUCH_HEAL && p->rt_forms[index].algorithm != DT_IOP_RETOUCH_CLONE)
         || (new_algo == DT_IOP_RETOUCH_BLUR && p->rt_forms[index].algorithm != DT_IOP_RETOUCH_FILL)
         || (new_algo == DT_IOP_RETOUCH_FILL && p->rt_forms[index].algorithm != DT_IOP_RETOUCH_BLUR))
      {
        accept = FALSE;
      }
    }
  }

  if(accept) p->algorithm = new_algo;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_clone), (p->algorithm == DT_IOP_RETOUCH_CLONE));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_heal), (p->algorithm == DT_IOP_RETOUCH_HEAL));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_blur), (p->algorithm == DT_IOP_RETOUCH_BLUR));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_fill), (p->algorithm == DT_IOP_RETOUCH_FILL));

  rt_show_hide_controls(self);

  if(!accept)
  {
    --darktable.gui->reset;
    return FALSE;
  }

  if(index >= 0 && dt_modifier_is(e->state, GDK_CONTROL_MASK))
  {
    if(p->algorithm != p->rt_forms[index].algorithm)
    {
      p->rt_forms[index].algorithm = p->algorithm;
      dt_control_queue_redraw_center();
    }
  }
  else if(darktable.develop->form_gui->creation && (darktable.develop->form_gui->creation_module == self))
  {
    dt_iop_request_focus(self);

    dt_masks_type_t type = DT_MASKS_CIRCLE;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_path)))
      type = DT_MASKS_PATH;
    else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_circle)))
      type = DT_MASKS_CIRCLE;
    else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_ellipse)))
      type = DT_MASKS_ELLIPSE;
    else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->bt_brush)))
      type = DT_MASKS_BRUSH;

    dt_masks_form_t *spot = NULL;
    if(p->algorithm == DT_IOP_RETOUCH_CLONE || p->algorithm == DT_IOP_RETOUCH_HEAL)
      spot = dt_masks_create(type | DT_MASKS_CLONE);
    else
      spot = dt_masks_create(type | DT_MASKS_NON_CLONE);
    dt_masks_change_form_gui(spot);
    darktable.develop->form_gui->creation_module = self;
    dt_control_queue_redraw_center();
  }

  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);

  // if we have the shift key pressed, we set it as default
  if(dt_modifier_is(e->state, GDK_SHIFT_MASK))
  {
    dt_conf_set_int("plugins/darkroom/retouch/default_algo", p->algorithm);
    // and we show a toat msg to confirm
    if(p->algorithm == DT_IOP_RETOUCH_CLONE)
      dt_control_log(_("default tool changed to %s"), _("cloning"));
    else if(p->algorithm == DT_IOP_RETOUCH_HEAL)
      dt_control_log(_("default tool changed to %s"), _("healing"));
    else if(p->algorithm == DT_IOP_RETOUCH_FILL)
      dt_control_log(_("default tool changed to %s"), _("fill"));
    else if(p->algorithm == DT_IOP_RETOUCH_BLUR)
      dt_control_log(_("default tool changed to %s"), _("blur"));
  }

  return TRUE;
}

static gboolean rt_showmask_callback(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  // if blend module is displaying mask do not display it here
  if(module->request_mask_display && !g->mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));

    gtk_toggle_button_set_active(togglebutton, FALSE);
    return TRUE;
  }

  g->mask_display = !gtk_toggle_button_get_active(togglebutton);

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);

  dt_iop_refresh_center(module);

  gtk_toggle_button_set_active(togglebutton, g->mask_display);
  return TRUE;
}

static gboolean rt_suppress_callback(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;
  g->suppress_mask = !gtk_toggle_button_get_active(togglebutton);

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);

  dt_iop_refresh_center(module);

  gtk_toggle_button_set_active(togglebutton, g->suppress_mask);
  return TRUE;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(w == g->cmb_fill_mode)
  {
    ++darktable.gui->reset;
    rt_show_hide_controls(self);
    --darktable.gui->reset;
  }
  else
  {
    const int index = rt_get_selected_shape_index(p);
    if(index >= 0)
    {
      if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_BLUR)
      {
        p->rt_forms[index].blur_type = p->blur_type;
        p->rt_forms[index].blur_radius = p->blur_radius;
      }
      else if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_FILL)
      {
        p->rt_forms[index].fill_mode = p->fill_mode;
        p->rt_forms[index].fill_brightness = p->fill_brightness;
      }
    }
  }
}

//--------------------------------------------------------------------------------------------------
// GUI
//--------------------------------------------------------------------------------------------------

void masks_selection_changed(struct dt_iop_module_t *self, const int form_selected_id)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  if(!g) return;

  dt_iop_gui_enter_critical_section(self);
  rt_shape_selection_changed(self);
  dt_iop_gui_leave_critical_section(self);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_retouch_params_t *d = module->default_params;

  d->preview_levels[0] = RETOUCH_PREVIEW_LVL_MIN;
  d->preview_levels[1] = 0.f;
  d->preview_levels[2] = RETOUCH_PREVIEW_LVL_MAX;
  d->algorithm = dt_conf_get_int("plugins/darkroom/retouch/default_algo");
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 21; // retouch.cl, from programs.conf
  dt_iop_retouch_global_data_t *gd = (dt_iop_retouch_global_data_t *)malloc(sizeof(dt_iop_retouch_global_data_t));
  module->data = gd;
  gd->kernel_retouch_clear_alpha = dt_opencl_create_kernel(program, "retouch_clear_alpha");
  gd->kernel_retouch_copy_alpha = dt_opencl_create_kernel(program, "retouch_copy_alpha");
  gd->kernel_retouch_copy_buffer_to_buffer = dt_opencl_create_kernel(program, "retouch_copy_buffer_to_buffer");
  gd->kernel_retouch_copy_buffer_to_image = dt_opencl_create_kernel(program, "retouch_copy_buffer_to_image");
  gd->kernel_retouch_fill = dt_opencl_create_kernel(program, "retouch_fill");
  gd->kernel_retouch_copy_image_to_buffer_masked
      = dt_opencl_create_kernel(program, "retouch_copy_image_to_buffer_masked");
  gd->kernel_retouch_copy_buffer_to_buffer_masked
      = dt_opencl_create_kernel(program, "retouch_copy_buffer_to_buffer_masked");
  gd->kernel_retouch_image_rgb2lab = dt_opencl_create_kernel(program, "retouch_image_rgb2lab");
  gd->kernel_retouch_image_lab2rgb = dt_opencl_create_kernel(program, "retouch_image_lab2rgb");
  gd->kernel_retouch_copy_mask_to_alpha = dt_opencl_create_kernel(program, "retouch_copy_mask_to_alpha");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_retouch_global_data_t *gd = (dt_iop_retouch_global_data_t *)module->data;

  dt_opencl_free_kernel(gd->kernel_retouch_clear_alpha);
  dt_opencl_free_kernel(gd->kernel_retouch_copy_alpha);
  dt_opencl_free_kernel(gd->kernel_retouch_copy_buffer_to_buffer);
  dt_opencl_free_kernel(gd->kernel_retouch_copy_buffer_to_image);
  dt_opencl_free_kernel(gd->kernel_retouch_fill);
  dt_opencl_free_kernel(gd->kernel_retouch_copy_image_to_buffer_masked);
  dt_opencl_free_kernel(gd->kernel_retouch_copy_buffer_to_buffer_masked);
  dt_opencl_free_kernel(gd->kernel_retouch_image_rgb2lab);
  dt_opencl_free_kernel(gd->kernel_retouch_image_lab2rgb);
  dt_opencl_free_kernel(gd->kernel_retouch_copy_mask_to_alpha);

  free(module->data);
  module->data = NULL;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(self->enabled && !darktable.develop->image_loading)
  {
    dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

    if(in)
    {
      dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
      //only show shapes if shapes exist
      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, self->blend_params->mask_id);
      if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
      {
        // got focus, show all shapes
        if(bd->masks_shown == DT_MASKS_EDIT_OFF)
          dt_masks_set_edit_mode(self, DT_MASKS_EDIT_FULL);

        rt_show_forms_for_current_scale(self);

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks),
                                     (bd->masks_shown != DT_MASKS_EDIT_OFF)
                                         && (darktable.develop->gui_module == self));
      }
    }
    else
    {
      // lost focus, hide all shapes and free if some are in creation
      if(darktable.develop->form_gui->creation && darktable.develop->form_gui->creation_module == self)
        dt_masks_change_form_gui(NULL);

      if(darktable.develop->form_gui->creation_continuous_module == self)
      {
        darktable.develop->form_gui->creation_continuous = FALSE;
        darktable.develop->form_gui->creation_continuous_module = NULL;
      }

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), FALSE);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), FALSE);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), FALSE);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), FALSE);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks), FALSE);

      dt_masks_set_edit_mode(self, DT_MASKS_EDIT_OFF);
    }

    // if we are switching between display modes we have to reprocess the main image
    if(g->display_wavelet_scale || g->mask_display || g->suppress_mask)
      dt_iop_refresh_center(self);
  }
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_retouch_params_t));
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  const float require = 2.0f;
  const float require_cl = 1.0f  // in_retouch
     + ((p->num_scales > 0) ? 4.0f : 2.0f); // dwt_wavelet_decompose_cl requires 4 buffers, otherwise 2.0f is enough
  // FIXME the above are worst case values, we might iterate through the dt_iop_retouch_form_data_t to get
  // the largest bounding box

  tiling->factor = 2.0f + require; // input & output buffers + internal requirements
  tiling->factor_cl = 2.0f + require_cl;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_retouch_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  // check if there is new or deleted forms
  rt_resynch_params(self);

  if(darktable.develop->form_gui->creation_continuous
     && darktable.develop->form_gui->creation_continuous_module == self && !rt_allow_create_form(self))
  {
    dt_masks_change_form_gui(NULL);
    darktable.develop->form_gui->creation_continuous = FALSE;
    darktable.develop->form_gui->creation_continuous_module = NULL;
  }

  // update clones count
  const dt_masks_form_t *grp = dt_masks_get_from_id(self->dev, self->blend_params->mask_id);
  guint nb = 0;
  if(grp && (grp->type & DT_MASKS_GROUP)) nb = g_list_length(grp->points);
  gchar *str = g_strdup_printf("%u", nb);
  gtk_label_set_text(g->label_form, str);
  g_free(str);

  // update wavelet decompose labels
  rt_update_wd_bar_labels(p, g);

  // update selected shape label
  rt_display_selected_shapes_lbl(g);

  // show the shapes for the current scale
  rt_show_forms_for_current_scale(self);

  // update algorithm toolbar
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_clone), p->algorithm == DT_IOP_RETOUCH_CLONE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_blur), p->algorithm == DT_IOP_RETOUCH_BLUR);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_heal), p->algorithm == DT_IOP_RETOUCH_HEAL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_fill), p->algorithm == DT_IOP_RETOUCH_FILL);

  // update shapes toolbar
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), rt_shape_is_being_added(self, DT_MASKS_CIRCLE));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), rt_shape_is_being_added(self, DT_MASKS_PATH));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), rt_shape_is_being_added(self, DT_MASKS_ELLIPSE));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), rt_shape_is_being_added(self, DT_MASKS_BRUSH));

  // update masks related buttons
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_showmask), g->mask_display);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_suppress), g->suppress_mask);

  // update the rest of the fields
  gtk_widget_queue_draw(GTK_WIDGET(g->wd_bar));

  dt_bauhaus_combobox_set(g->cmb_blur_type, p->blur_type);
  dt_bauhaus_slider_set(g->sl_blur_radius, p->blur_radius);
  dt_bauhaus_slider_set(g->sl_fill_brightness, p->fill_brightness);
  dt_bauhaus_combobox_set(g->cmb_fill_mode, p->fill_mode);

  rt_display_selected_fill_color(g, p);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_display_wavelet_scale), g->display_wavelet_scale);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_copy_scale), g->copied_scale >= 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_paste_scale), g->copied_scale >= 0);
  gtk_widget_set_sensitive(g->bt_paste_scale, g->copied_scale >= 0);

  // show/hide some fields
  rt_show_hide_controls(self);

  // update edit shapes status
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
  if(darktable.develop->history_updating) bd->masks_shown = DT_MASKS_EDIT_OFF;

  //only toggle shape show button if shapes exist
  if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks),
                                 (bd->masks_shown != DT_MASKS_EDIT_OFF) && (darktable.develop->gui_module == self));
  }
  else
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks), FALSE);
  }

  // update the gradient slider
  double dlevels[3];
  for(int i = 0; i < 3; i++) dlevels[i] = p->preview_levels[i];
  dtgtk_gradient_slider_multivalue_set_values(g->preview_levels_gslider, dlevels);
}

void change_image(struct dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  if(g)
  {
    g->copied_scale = -1;
    g->mask_display = 0;
    g->suppress_mask = 0;
    g->display_wavelet_scale = 0;
    g->displayed_wavelet_scale = 0;
    g->first_scale_visible = RETOUCH_MAX_SCALES + 1;

    g->preview_auto_levels = 0;
    g->preview_levels[0] = RETOUCH_PREVIEW_LVL_MIN;
    g->preview_levels[1] = 0.f;
    g->preview_levels[2] = RETOUCH_PREVIEW_LVL_MAX;

    g->is_dragging = 0;
    g->wdbar_mouse_x = g->wdbar_mouse_y = -1;
    g->curr_scale = -1;
    g->lower_cursor = g->upper_cursor = FALSE;
    g->lower_margin = g->upper_margin = FALSE;
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = IOP_GUI_ALLOC(retouch);
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->default_params;

  change_image(self);

  // shapes toolbar
  GtkWidget *hbox_shapes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_box_pack_start(GTK_BOX(hbox_shapes), dt_ui_label_new(_("shapes:")), FALSE, TRUE, 0);
  g->label_form = GTK_LABEL(gtk_label_new("-1"));
  gtk_box_pack_start(GTK_BOX(hbox_shapes), GTK_WIDGET(g->label_form), FALSE, TRUE, DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_tooltip_text(hbox_shapes,
               _("to add a shape select an algorithm and a shape type and click on the image.\n"
                 "shapes are added to the current scale"));

  g->bt_edit_masks = dt_iop_togglebutton_new(self, N_("editing"), N_("show and edit shapes on the current scale"),
                                                                  N_("show and edit shapes in restricted mode"),
                                             G_CALLBACK(rt_edit_masks_callback), TRUE, 0, 0,
                                             dtgtk_cairo_paint_masks_eye, hbox_shapes);

  g->bt_brush = dt_iop_togglebutton_new(self, N_("shapes"), N_("add brush"), N_("add multiple brush strokes"),
                                        G_CALLBACK(rt_add_shape_callback), TRUE, 0, 0,
                                        dtgtk_cairo_paint_masks_brush, hbox_shapes);

  g->bt_path = dt_iop_togglebutton_new(self, N_("shapes"), N_("add path"), N_("add multiple paths"),
                                       G_CALLBACK(rt_add_shape_callback), TRUE, 0, 0,
                                       dtgtk_cairo_paint_masks_path, hbox_shapes);

  g->bt_ellipse = dt_iop_togglebutton_new(self, N_("shapes"), N_("add ellipse"), N_("add multiple ellipses"),
                                          G_CALLBACK(rt_add_shape_callback), TRUE, 0, 0,
                                          dtgtk_cairo_paint_masks_ellipse, hbox_shapes);

  g->bt_circle = dt_iop_togglebutton_new(self, N_("shapes"), N_("add circle"), N_("add multiple circles"),
                                         G_CALLBACK(rt_add_shape_callback), TRUE, 0, 0,
                                         dtgtk_cairo_paint_masks_circle, hbox_shapes);

  // algorithm toolbar
  GtkWidget *hbox_algo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_box_pack_start(GTK_BOX(hbox_algo), dt_ui_label_new(_("algorithms:")), FALSE, TRUE, 0);

  g->bt_blur = dt_iop_togglebutton_new(
      self, N_("tools"), N_("activate blur tool"), N_("change algorithm for current form"),
      G_CALLBACK(rt_select_algorithm_callback), TRUE, 0, 0, dtgtk_cairo_paint_tool_blur, hbox_algo);

  g->bt_fill = dt_iop_togglebutton_new(
      self, N_("tools"), N_("activate fill tool"), N_("change algorithm for current form"),
      G_CALLBACK(rt_select_algorithm_callback), TRUE, 0, 0, dtgtk_cairo_paint_tool_fill, hbox_algo);

  g->bt_clone = dt_iop_togglebutton_new(
      self, N_("tools"), N_("activate cloning tool"), N_("change algorithm for current form"),
      G_CALLBACK(rt_select_algorithm_callback), TRUE, 0, 0, dtgtk_cairo_paint_tool_clone, hbox_algo);

  g->bt_heal = dt_iop_togglebutton_new(
      self, N_("tools"), N_("activate healing tool"), N_("change algorithm for current form"),
      G_CALLBACK(rt_select_algorithm_callback), TRUE, 0, 0, dtgtk_cairo_paint_tool_heal, hbox_algo);

  // overwrite tooltip ourself to handle shift+click
  gchar *tt2 = g_strdup_printf("%s\n%s", _("ctrl+click to change tool for current form"),
                               _("shift+click to set the tool as default"));
  gchar *tt = g_strdup_printf("%s\n%s", _("activate blur tool"), tt2);
  gtk_widget_set_tooltip_text(g->bt_blur, tt);
  g_free(tt);
  tt = g_strdup_printf("%s\n%s", _("activate fill tool"), tt2);
  gtk_widget_set_tooltip_text(g->bt_fill, tt);
  g_free(tt);
  tt = g_strdup_printf("%s\n%s", _("activate cloning tool"), tt2);
  gtk_widget_set_tooltip_text(g->bt_clone, tt);
  g_free(tt);
  tt = g_strdup_printf("%s\n%s", _("activate healing tool"), tt2);
  gtk_widget_set_tooltip_text(g->bt_heal, tt);
  g_free(tt);
  g_free(tt2);

  // wavelet decompose bar labels
  GtkWidget *grid_wd_labels = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(grid_wd_labels), FALSE);

  gtk_grid_attach(GTK_GRID(grid_wd_labels), dt_ui_label_new(_("scales:")), 0, 0, 1, 1);
  g->lbl_num_scales = GTK_LABEL(dt_ui_label_new(NULL));
  gtk_label_set_width_chars(g->lbl_num_scales, 2);
  gtk_grid_attach(GTK_GRID(grid_wd_labels), GTK_WIDGET(g->lbl_num_scales), 1, 0, 1, 1);

  gtk_grid_attach(GTK_GRID(grid_wd_labels), dt_ui_label_new(_("current:")), 0, 1, 1, 1);
  g->lbl_curr_scale = GTK_LABEL(dt_ui_label_new(NULL));
  gtk_label_set_width_chars(g->lbl_curr_scale, 2);
  gtk_grid_attach(GTK_GRID(grid_wd_labels), GTK_WIDGET(g->lbl_curr_scale), 1, 1, 1, 1);

  gtk_grid_attach(GTK_GRID(grid_wd_labels), dt_ui_label_new(_("merge from:")), 0, 2, 1, 1);
  g->lbl_merge_from_scale = GTK_LABEL(dt_ui_label_new(NULL));
  gtk_label_set_width_chars(g->lbl_merge_from_scale, 2);
  gtk_grid_attach(GTK_GRID(grid_wd_labels), GTK_WIDGET(g->lbl_merge_from_scale), 1, 2, 1, 1);

  // wavelet decompose bar
  g->wd_bar = gtk_drawing_area_new();

  gtk_widget_set_tooltip_text(g->wd_bar, _("top slider adjusts where the merge scales start\n"
                                           "bottom slider adjusts the number of scales\n"
                                           "dot indicates the current scale\n"
                                           "top line indicates that the scale is visible at current zoom level\n"
                                           "bottom line indicates that the scale has shapes on it"));
  g_signal_connect(G_OBJECT(g->wd_bar), "draw", G_CALLBACK(rt_wdbar_draw), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "motion-notify-event", G_CALLBACK(rt_wdbar_motion_notify), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "leave-notify-event", G_CALLBACK(rt_wdbar_leave_notify), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "button-press-event", G_CALLBACK(rt_wdbar_button_press), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "button-release-event", G_CALLBACK(rt_wdbar_button_release), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "scroll-event", G_CALLBACK(rt_wdbar_scrolled), self);
  gtk_widget_add_events(GTK_WIDGET(g->wd_bar), GDK_POINTER_MOTION_MASK
                                                   | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                   | GDK_LEAVE_NOTIFY_MASK | darktable.gui->scroll_mask);
  gtk_widget_set_size_request(g->wd_bar, -1, DT_PIXEL_APPLY_DPI(40));

  // toolbar display current scale / cut&paste / suppress&display masks
  GtkWidget *hbox_scale = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  // display & suppress masks
  g->bt_showmask = dt_iop_togglebutton_new(self, N_("editing"), N_("display masks"), NULL,
                                           G_CALLBACK(rt_showmask_callback), TRUE, 0, 0,
                                           dtgtk_cairo_paint_showmask, hbox_scale);
  dt_gui_add_class(g->bt_showmask, "dt_transparent_background");

  g->bt_suppress = dt_iop_togglebutton_new(self, N_("editing"), N_("temporarily switch off shapes"), NULL,
                                           G_CALLBACK(rt_suppress_callback), TRUE, 0, 0,
                                           dtgtk_cairo_paint_eye_toggle, hbox_scale);
  dt_gui_add_class(g->bt_suppress, "dt_transparent_background");

  gtk_box_pack_end(GTK_BOX(hbox_scale), gtk_grid_new(), TRUE, TRUE, 0);

  // copy/paste shapes
  g->bt_paste_scale = dt_iop_togglebutton_new(self, N_("editing"), N_("paste cut shapes to current scale"), NULL,
                                              G_CALLBACK(rt_copypaste_scale_callback), TRUE, 0, 0,
                                              dtgtk_cairo_paint_paste_forms, hbox_scale);

  g->bt_copy_scale = dt_iop_togglebutton_new(self, N_("editing"), N_("cut shapes from current scale"), NULL,
                                             G_CALLBACK(rt_copypaste_scale_callback), TRUE, 0, 0,
                                             dtgtk_cairo_paint_cut_forms, hbox_scale);

  gtk_box_pack_end(GTK_BOX(hbox_scale), gtk_grid_new(), TRUE, TRUE, 0);

  // display final image/current scale
  g->bt_display_wavelet_scale = dt_iop_togglebutton_new(self, N_("editing"), N_("display wavelet scale"), NULL,
                                                        G_CALLBACK(rt_display_wavelet_scale_callback), TRUE, 0, 0,
                                                        dtgtk_cairo_paint_display_wavelet_scale, hbox_scale);
  dt_gui_add_class(g->bt_display_wavelet_scale, "dt_transparent_background");

  // preview single scale
  g->vbox_preview_scale = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *lbl_psc = dt_ui_section_label_new(C_("section", "preview single scale"));
  gtk_box_pack_start(GTK_BOX(g->vbox_preview_scale), lbl_psc, FALSE, TRUE, 0);

  GtkWidget *prev_lvl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  // gradient slider
  #define NEUTRAL_GRAY 0.5
  static const GdkRGBA _gradient_L[]
      = { { 0, 0, 0, 1.0 }, { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } };
  g->preview_levels_gslider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(
      dtgtk_gradient_slider_multivalue_new_with_color_and_name(_gradient_L[0], _gradient_L[1], 3, "preview-levels"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->preview_levels_gslider), _("adjust preview levels"));
  dtgtk_gradient_slider_multivalue_set_marker(g->preview_levels_gslider, GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(g->preview_levels_gslider, GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(g->preview_levels_gslider, GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 2);
  (g->preview_levels_gslider)->scale_callback = rt_gslider_scale_callback;
  double vdefault[3] = {RETOUCH_PREVIEW_LVL_MIN, (RETOUCH_PREVIEW_LVL_MIN + RETOUCH_PREVIEW_LVL_MAX) / 2.0, RETOUCH_PREVIEW_LVL_MAX};
  dtgtk_gradient_slider_multivalue_set_values(g->preview_levels_gslider, vdefault);
  dtgtk_gradient_slider_multivalue_set_resetvalues(g->preview_levels_gslider, vdefault);
  (g->preview_levels_gslider)->markers_type = PROPORTIONAL_MARKERS;
  (g->preview_levels_gslider)->min_spacing = 0.05;
  g_signal_connect(G_OBJECT(g->preview_levels_gslider), "value-changed", G_CALLBACK(rt_gslider_changed), self);

  gtk_box_pack_start(GTK_BOX(prev_lvl), GTK_WIDGET(g->preview_levels_gslider), TRUE, TRUE, 0);

  // auto-levels button
  g->bt_auto_levels = dt_iop_togglebutton_new(self, N_("editing"), N_("auto levels"), NULL,
                                              G_CALLBACK(rt_auto_levels_callback), TRUE, 0, 0,
                                              dtgtk_cairo_paint_auto_levels, prev_lvl);

  gtk_box_pack_start(GTK_BOX(g->vbox_preview_scale), prev_lvl, TRUE, TRUE, 0);

  // shapes selected (label)
  GtkWidget *hbox_shape_sel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label1 = gtk_label_new(_("shape selected:"));
  gtk_label_set_ellipsize(GTK_LABEL(label1), PANGO_ELLIPSIZE_START);
  gtk_box_pack_start(GTK_BOX(hbox_shape_sel), label1, FALSE, TRUE, 0);
  g->label_form_selected = GTK_LABEL(gtk_label_new("-1"));
  gtk_widget_set_tooltip_text(hbox_shape_sel,
                              _("click on a shape to select it,\nto unselect click on an empty space"));
  gtk_box_pack_start(GTK_BOX(hbox_shape_sel), GTK_WIDGET(g->label_form_selected), FALSE, TRUE, 0);

  // fill properties
  g->vbox_fill = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  g->cmb_fill_mode = dt_bauhaus_combobox_from_params(self, "fill_mode");
  gtk_widget_set_tooltip_text(g->cmb_fill_mode, _("erase the detail or fills with chosen color"));

  // color for fill algorithm
  GdkRGBA color
      = (GdkRGBA){.red = p->fill_color[0], .green = p->fill_color[1], .blue = p->fill_color[2], .alpha = 1.0 };

  g->hbox_color_pick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *lbl_fill_color = dt_ui_label_new(_("fill color: "));
  gtk_box_pack_start(GTK_BOX(g->hbox_color_pick), lbl_fill_color, FALSE, TRUE, 0);

  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select fill color"));
  gtk_widget_set_tooltip_text(g->colorpick, _("select fill color"));
  g_signal_connect(G_OBJECT(g->colorpick), "color-set", G_CALLBACK(rt_colorpick_color_set_callback), self);
  gtk_box_pack_start(GTK_BOX(g->hbox_color_pick), GTK_WIDGET(g->colorpick), TRUE, TRUE, 0);

  g->colorpicker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT | DT_COLOR_PICKER_IO, g->hbox_color_pick);
  gtk_widget_set_tooltip_text(g->colorpicker, _("pick fill color from image"));

  gtk_box_pack_start(GTK_BOX(g->vbox_fill), g->hbox_color_pick, TRUE, TRUE, 0);

  g->sl_fill_brightness = dt_bauhaus_slider_from_params(self, "fill_brightness");
  dt_bauhaus_slider_set_digits(g->sl_fill_brightness, 4);
  dt_bauhaus_slider_set_format(g->sl_fill_brightness, "%");
  gtk_widget_set_tooltip_text(g->sl_fill_brightness,
                              _("adjusts color brightness to fine-tune it. works with erase as well"));

  // blur properties
  g->vbox_blur = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  g->cmb_blur_type = dt_bauhaus_combobox_from_params(self, "blur_type");
  gtk_widget_set_tooltip_text(g->cmb_blur_type, _("type for the blur algorithm"));

  g->sl_blur_radius = dt_bauhaus_slider_from_params(self, "blur_radius");
  dt_bauhaus_slider_set_format(g->sl_blur_radius, " px");
  gtk_widget_set_tooltip_text(g->sl_blur_radius, _("radius of the selected blur type"));

  // mask opacity
  g->sl_mask_opacity = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0, 1., 3);
  dt_bauhaus_widget_set_label(g->sl_mask_opacity, NULL, N_("mask opacity"));
  dt_bauhaus_slider_set_format(g->sl_mask_opacity, "%");
  gtk_widget_set_tooltip_text(g->sl_mask_opacity, _("set the opacity on the selected shape"));
  g_signal_connect(G_OBJECT(g->sl_mask_opacity), "value-changed", G_CALLBACK(rt_mask_opacity_callback), self);

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *lbl_rt_tools = dt_ui_section_label_new(C_("section", "retouch tools"));
  gtk_box_pack_start(GTK_BOX(self->widget), lbl_rt_tools, FALSE, TRUE, 0);

  // shapes toolbar
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_shapes, TRUE, TRUE, 0);
  // algorithms toolbar
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_algo, TRUE, TRUE, 0);

  // wavelet decompose
  GtkWidget *lbl_wd = dt_ui_section_label_new(C_("section", "wavelet decompose"));
  gtk_box_pack_start(GTK_BOX(self->widget), lbl_wd, FALSE, TRUE, 0);

  // wavelet decompose bar & labels
  gtk_box_pack_start(GTK_BOX(self->widget), grid_wd_labels, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->wd_bar, TRUE, TRUE, DT_PIXEL_APPLY_DPI(3));

  // preview scale & cut/paste scale
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_scale, TRUE, TRUE, 0);

  // preview single scale
  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_preview_scale, TRUE, TRUE, 0);

  // shapes
  GtkWidget *lbl_shapes = dt_ui_section_label_new(C_("section", "shapes"));
  gtk_box_pack_start(GTK_BOX(self->widget), lbl_shapes, FALSE, TRUE, 0);

  // shape selected
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_shape_sel, TRUE, TRUE, 0);
  // blur radius
  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_blur, TRUE, TRUE, 0);
  // fill color
  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_fill, TRUE, TRUE, 0);
  // mask (shape) opacity
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_mask_opacity, TRUE, TRUE, 0);

  /* add signal handler for preview pipe finish to redraw the preview */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(rt_develop_ui_pipe_finished_callback), self);
}

void gui_reset(struct dt_iop_module_t *self)
{
  // hide the previous masks
  dt_masks_reset_form_gui();
  // set the algo to the default one
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  p->algorithm = dt_conf_get_int("plugins/darkroom/retouch/default_algo");
}

void reload_defaults(dt_iop_module_t *self)
{
  // set the algo to the default one
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->default_params;
  p->algorithm = dt_conf_get_int("plugins/darkroom/retouch/default_algo");
}

void gui_cleanup(dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(rt_develop_ui_pipe_finished_callback), self);

  IOP_GUI_FREE;
}

static void rt_compute_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              dt_iop_roi_t *roi_in, int *_roir, int *_roib, int *_roix, int *_roiy)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_develop_blend_params_t *bp = self->blend_params;

  int roir = *_roir;
  int roib = *_roib;
  int roix = *_roix;
  int roiy = *_roiy;

  // We iterate through all forms
  const dt_masks_form_t *grp = dt_masks_get_from_id_ext(piece->pipe->forms, bp->mask_id);
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *forms = grp->points; forms; forms = g_list_next(forms))
    {
      const dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt)
      {
        const int formid = grpt->formid;
        const int index = rt_get_index_from_formid(p, formid);
        if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_FILL)
        {
          continue;
        }

        // we get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form)
        {
          // if the form is outside the roi, we just skip it
          // we get the area for the form
          int fl, ft, fw, fh;
          if(!dt_masks_get_area(self, piece, form, &fw, &fh, &fl, &ft))
          {
            continue;
          }

          // is the form outside of the roi?
          fw *= roi_in->scale, fh *= roi_in->scale, fl *= roi_in->scale, ft *= roi_in->scale;
          if(ft >= roi_in->y + roi_in->height || ft + fh <= roi_in->y || fl >= roi_in->x + roi_in->width
             || fl + fw <= roi_in->x)
          {
            continue;
          }

          // heal need the entire area
          if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_HEAL)
          {
            // we enlarge the roi if needed
            roiy = fminf(ft, roiy);
            roix = fminf(fl, roix);
            roir = fmaxf(fl + fw, roir);
            roib = fmaxf(ft + fh, roib);
          }
          // blur need an overlap of 4 * radius (scaled)
          if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_BLUR)
          {
            if(index >= 0)
            {
              const int overlap = ceilf(4 * (p->rt_forms[index].blur_radius * roi_in->scale / piece->iscale));
              if(roiy > ft) roiy = MAX(roiy - overlap, ft);
              if(roix > fl) roix = MAX(roix - overlap, fl);
              if(roir < fl + fw) roir = MAX(roir + overlap, fl + fw);
              if(roib < ft + fh) roib = MAX(roib + overlap, ft + fh);
            }
          }
          // heal and clone need both source and destination areas
          if(p->rt_forms[index].algorithm == DT_IOP_RETOUCH_HEAL
             || p->rt_forms[index].algorithm == DT_IOP_RETOUCH_CLONE)
          {
            float dx = 0.f, dy = 0.f;
            if(rt_masks_get_delta_to_destination(self, piece, roi_in, form, &dx, &dy,
                                                 p->rt_forms[index].distort_mode))
            {
              roiy = fminf(ft - dy, roiy);
              roix = fminf(fl - dx, roix);
              roir = fmaxf(fl + fw - dx, roir);
              roib = fmaxf(ft + fh - dy, roib);
            }
          }
        }
      }
    }
  }

  *_roir = roir;
  *_roib = roib;
  *_roix = roix;
  *_roiy = roiy;
}

// for a given form, if a previous clone/heal destination intersects the source area,
// include that area in roi_in too
static void rt_extend_roi_in_from_source_clones(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                                dt_iop_roi_t *roi_in, const int formid_src, const int fl_src,
                                                const int ft_src, const int fw_src, const int fh_src, int *_roir,
                                                int *_roib, int *_roix, int *_roiy)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_develop_blend_params_t *bp = self->blend_params;

  int roir = *_roir;
  int roib = *_roib;
  int roix = *_roix;
  int roiy = *_roiy;

  // We iterate through all forms
  const dt_masks_form_t *grp = dt_masks_get_from_id_ext(piece->pipe->forms, bp->mask_id);
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *forms = grp->points; forms; forms = g_list_next(forms))
    {
      const dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt)
      {
        const int formid = grpt->formid;

        // just need the previous forms
        if(formid == formid_src) break;

        const int index = rt_get_index_from_formid(p, formid);

        // only process clone and heal
        if(p->rt_forms[index].algorithm != DT_IOP_RETOUCH_HEAL
           && p->rt_forms[index].algorithm != DT_IOP_RETOUCH_CLONE)
        {
          continue;
        }

        // we get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form)
        {
          // we get the source area
          int fl, ft, fw, fh;
          if(!dt_masks_get_source_area(self, piece, form, &fw, &fh, &fl, &ft))
          {
            continue;
          }
          fw *= roi_in->scale, fh *= roi_in->scale, fl *= roi_in->scale, ft *= roi_in->scale;

          // get the destination area
          int fl_dest, ft_dest;
          float dx = 0.f, dy = 0.f;
          if(!rt_masks_get_delta_to_destination(self, piece, roi_in, form, &dx, &dy,
                                                p->rt_forms[index].distort_mode))
          {
            continue;
          }

          ft_dest = ft + dy;
          fl_dest = fl + dx;

          // check if the destination of this form intersects the source of the formid_src
          const int intersects = !(ft_dest + fh < ft_src || ft_src + fh_src < ft_dest || fl_dest + fw < fl_src
                                   || fl_src + fw_src < fl_dest);
          if(intersects)
          {
            // we enlarge the roi if needed
            roiy = fminf(ft, roiy);
            roix = fminf(fl, roix);
            roir = fmaxf(fl + fw, roir);
            roib = fmaxf(ft + fh, roib);

            // need both source and destination areas
            roiy = fminf(ft + dy, roiy);
            roix = fminf(fl + dx, roix);
            roir = fmaxf(fl + fw + dx, roir);
            roib = fmaxf(ft + fh + dy, roib);
          }
        }
      }
    }
  }

  *_roir = roir;
  *_roib = roib;
  *_roix = roix;
  *_roiy = roiy;
}

// for clone and heal, if the source area is the destination from another clone/heal,
// we also need the area from that previous clone/heal
static void rt_extend_roi_in_for_clone(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                       dt_iop_roi_t *roi_in, int *_roir, int *_roib, int *_roix, int *_roiy)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_develop_blend_params_t *bp = self->blend_params;

  int roir = *_roir;
  int roib = *_roib;
  int roix = *_roix;
  int roiy = *_roiy;

  // go through all clone and heal forms
  const dt_masks_form_t *grp = dt_masks_get_from_id_ext(piece->pipe->forms, bp->mask_id);
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *forms = grp->points; forms; forms = g_list_next(forms))
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt)
      {
        const int formid = grpt->formid;
        const int index = rt_get_index_from_formid(p, formid);

        if(p->rt_forms[index].algorithm != DT_IOP_RETOUCH_HEAL
           && p->rt_forms[index].algorithm != DT_IOP_RETOUCH_CLONE)
        {
          continue;
        }

        // we get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form == NULL)
        {
          continue;
        }

        // get the source area
        int fl_src, ft_src, fw_src, fh_src;
        if(!dt_masks_get_source_area(self, piece, form, &fw_src, &fh_src, &fl_src, &ft_src))
        {
          continue;
        }

        fw_src *= roi_in->scale, fh_src *= roi_in->scale, fl_src *= roi_in->scale, ft_src *= roi_in->scale;

        // we only want to process forms already in roi_in
        const int intersects
            = !(roib < ft_src || ft_src + fh_src < roiy || roir < fl_src || fl_src + fw_src < roix);
        if(intersects)
          rt_extend_roi_in_from_source_clones(self, piece, roi_in, formid, fl_src, ft_src, fw_src, fh_src, &roir,
                                              &roib, &roix, &roiy);
      }
    }
  }

  *_roir = roir;
  *_roib = roib;
  *_roix = roix;
  *_roiy = roiy;
}

// needed if mask dest is in roi and mask src is not
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  int roir = roi_in->width + roi_in->x;
  int roib = roi_in->height + roi_in->y;
  int roix = roi_in->x;
  int roiy = roi_in->y;

  rt_compute_roi_in(self, piece, roi_in, &roir, &roib, &roix, &roiy);

  int roir_prev = -1, roib_prev = -1, roix_prev = -1, roiy_prev = -1;

  while(roir != roir_prev || roib != roib_prev || roix != roix_prev || roiy != roiy_prev)
  {
    roir_prev = roir;
    roib_prev = roib;
    roix_prev = roix;
    roiy_prev = roiy;

    rt_extend_roi_in_for_clone(self, piece, roi_in, &roir, &roib, &roix, &roiy);
  }

  // now we set the values
  const float scwidth = piece->buf_in.width * roi_in->scale, scheight = piece->buf_in.height * roi_in->scale;
  roi_in->x = CLAMP(roix, 0, scwidth - 1);
  roi_in->y = CLAMP(roiy, 0, scheight - 1);
  roi_in->width = CLAMP(roir - roi_in->x, 1, scwidth + .5f - roi_in->x);
  roi_in->height = CLAMP(roib - roi_in->y, 1, scheight + .5f - roi_in->y);
}

//--------------------------------------------------------------------------------------------------
// process
//--------------------------------------------------------------------------------------------------

static void image_rgb2lab(float *img_src, const int width, const int height, const int ch, const int use_sse)
{
  const int stride = width * height * ch;

#if defined(__SSE__)
  if(ch == 4 && use_sse)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, stride) \
    shared(img_src) \
    schedule(static)
#endif
    for(int i = 0; i < stride; i += ch)
    {
      // RGB -> XYZ
      __m128 rgb = _mm_load_ps(img_src + i);
      __m128 XYZ = dt_RGB_to_XYZ_sse2(rgb);
      // XYZ -> Lab
      _mm_store_ps(img_src + i, dt_XYZ_to_Lab_sse2(XYZ));
    }

    return;
  }
#endif

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, stride) \
  shared(img_src) \
  schedule(static)
#endif
  for(int i = 0; i < stride; i += ch)
  {
    dt_aligned_pixel_t XYZ;

    dt_linearRGB_to_XYZ(img_src + i, XYZ);
    dt_XYZ_to_Lab(XYZ, img_src + i);
  }
}

static void image_lab2rgb(float *img_src, const int width, const int height, const int ch, const int use_sse)
{
  const int stride = width * height * ch;

#if defined(__SSE__)
  if(ch == 4 && use_sse)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, stride) \
  shared(img_src) \
  schedule(static)
#endif
    for(int i = 0; i < stride; i += ch)
    {
      // Lab -> XYZ
      __m128 Lab = _mm_load_ps(img_src + i);
      __m128 XYZ = dt_Lab_to_XYZ_sse2(Lab);
      // XYZ -> RGB
      _mm_store_ps(img_src + i, dt_XYZ_to_RGB_sse2(XYZ));
    }

    return;
  }
#endif

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, stride) \
  shared(img_src) \
  schedule(static)
#endif
  for(int i = 0; i < stride; i += ch)
  {
    dt_aligned_pixel_t XYZ;

    dt_Lab_to_XYZ(img_src + i, XYZ);
    dt_XYZ_to_linearRGB(XYZ, img_src + i);
  }
}

static void rt_process_stats(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const img_src,
                             const int width, const int height, const int ch, float levels[3])
{
  const int size = width * height * ch;
  float l_max = -INFINITY;
  float l_min = INFINITY;
  float l_sum = 0.f;
  int count = 0;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, img_src, size, work_profile) \
  schedule(static) \
  reduction(+ : count, l_sum) \
  reduction(max : l_max) \
  reduction(min : l_min)
#endif
  for(int i = 0; i < size; i += ch)
  {
    dt_aligned_pixel_t Lab = { 0 };

    if(work_profile)
    {
      dt_ioppr_rgb_matrix_to_lab(img_src + i, Lab, work_profile->matrix_in_transposed,
                                  work_profile->lut_in, work_profile->unbounded_coeffs_in,
                                  work_profile->lutsize, work_profile->nonlinearlut);
    }
    else
    {
      dt_aligned_pixel_t XYZ;
      dt_linearRGB_to_XYZ(img_src + i, XYZ);
      dt_XYZ_to_Lab(XYZ, Lab);
    }

    l_max = MAX(l_max, Lab[0]);
    l_min = MIN(l_min, Lab[0]);
    l_sum += Lab[0];
    count++;
  }

  levels[0] = l_min / 100.f;
  levels[2] = l_max / 100.f;
  levels[1] = (l_sum / (float)count) / 100.f;
}

static void rt_adjust_levels(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *img_src, const int width,
                             const int height, const int ch, const float levels[3])
{
  const int size = width * height * ch;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const float left = levels[0];
  const float middle = levels[1];
  const float right = levels[2];

  if(left == RETOUCH_PREVIEW_LVL_MIN && middle == 0.f && right == RETOUCH_PREVIEW_LVL_MAX) return;

  const float delta = (right - left) / 2.0f;
  const float mid = left + delta;
  const float tmp = (middle - mid) / delta;
  const float in_inv_gamma = powf(10, tmp);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, in_inv_gamma, left, right, size, work_profile) \
  shared(img_src) \
  schedule(static)
#endif
  for(int i = 0; i < size; i += ch)
  {
    if(work_profile)
    {
      dt_ioppr_rgb_matrix_to_lab(img_src + i, img_src + i, work_profile->matrix_in_transposed,
                                  work_profile->lut_in, work_profile->unbounded_coeffs_in,
                                  work_profile->lutsize, work_profile->nonlinearlut);
    }
    else
    {
      dt_aligned_pixel_t XYZ;

      dt_linearRGB_to_XYZ(img_src + i, XYZ);
      dt_XYZ_to_Lab(XYZ, img_src + i);
    }

    for(int c = 0; c < 1; c++)
    {
      const float L_in = img_src[i + c] / 100.0f;

      if(L_in <= left)
      {
        img_src[i + c] = 0.f;
      }
      else
      {
        const float percentage = (L_in - left) / (right - left);
        img_src[i + c] = 100.0f * powf(percentage, in_inv_gamma);
      }
    }

    if(work_profile)
    {
      dt_ioppr_lab_to_rgb_matrix(img_src + i, img_src + i, work_profile->matrix_out_transposed,
                                 work_profile->lut_out, work_profile->unbounded_coeffs_out,
                                 work_profile->lutsize, work_profile->nonlinearlut);;
    }
    else
    {
      dt_aligned_pixel_t XYZ;

      dt_Lab_to_XYZ(img_src + i, XYZ);
      dt_XYZ_to_linearRGB(XYZ, img_src + i);
    }
  }
}

#undef RT_WDBAR_INSET

#undef RETOUCH_NO_FORMS
#undef RETOUCH_MAX_SCALES
#undef RETOUCH_NO_SCALES

#undef RETOUCH_PREVIEW_LVL_MIN
#undef RETOUCH_PREVIEW_LVL_MAX

static void rt_intersect_2_rois(dt_iop_roi_t *const roi_1, dt_iop_roi_t *const roi_2, const int dx, const int dy,
                                const int padding, dt_iop_roi_t *roi_dest)
{
  const int x_from = MAX(MAX((roi_1->x + 1 - padding), roi_2->x), (roi_2->x + dx));
  const int x_to
      = MIN(MIN((roi_1->x + roi_1->width + 1 + padding), roi_2->x + roi_2->width), (roi_2->x + roi_2->width + dx));

  const int y_from = MAX(MAX((roi_1->y + 1 - padding), roi_2->y), (roi_2->y + dy));
  const int y_to = MIN(MIN((roi_1->y + roi_1->height + 1 + padding), (roi_2->y + roi_2->height)),
                       (roi_2->y + roi_2->height + dy));

  roi_dest->x = x_from;
  roi_dest->y = y_from;
  roi_dest->width = x_to - x_from;
  roi_dest->height = y_to - y_from;
}

static void rt_copy_in_to_out(const float *const in, const struct dt_iop_roi_t *const roi_in, float *const out,
                              const struct dt_iop_roi_t *const roi_out, const int ch, const int dx, const int dy)
{
  const size_t rowsize = sizeof(float) * ch * MIN(roi_out->width, roi_in->width);
  const int xoffs = roi_out->x - roi_in->x - dx;
  const int yoffs = roi_out->y - roi_in->y - dy;
  const int y_to = MIN(roi_out->height, roi_in->height);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, in, out, roi_in, roi_out, rowsize, xoffs,  yoffs, \
                      y_to) \
  schedule(static)
#endif
  for(int y = 0; y < y_to; y++)
  {
    const size_t iindex = ((size_t)(y + yoffs) * roi_in->width + xoffs) * ch;
    const size_t oindex = (size_t)y * roi_out->width * ch;
    float *in1 = (float *)in + iindex;
    float *out1 = (float *)out + oindex;

    memcpy(out1, in1, rowsize);
  }
}

static void rt_build_scaled_mask(float *const mask, dt_iop_roi_t *const roi_mask, float **mask_scaled,
                                 dt_iop_roi_t *roi_mask_scaled, dt_iop_roi_t *const roi_in, const int dx,
                                 const int dy, const int algo)
{
  float *mask_tmp = NULL;

  const int padding = (algo == DT_IOP_RETOUCH_HEAL) ? 1 : 0;

  *roi_mask_scaled = *roi_mask;

  roi_mask_scaled->x = roi_mask->x * roi_in->scale;
  roi_mask_scaled->y = roi_mask->y * roi_in->scale;
  roi_mask_scaled->width = ((roi_mask->width * roi_in->scale) + .5f);
  roi_mask_scaled->height = ((roi_mask->height * roi_in->scale) + .5f);
  roi_mask_scaled->scale = roi_in->scale;

  rt_intersect_2_rois(roi_mask_scaled, roi_in, dx, dy, padding, roi_mask_scaled);
  if(roi_mask_scaled->width < 1 || roi_mask_scaled->height < 1) goto cleanup;

  const int x_to = roi_mask_scaled->width + roi_mask_scaled->x;
  const int y_to = roi_mask_scaled->height + roi_mask_scaled->y;

  mask_tmp = dt_alloc_align_float((size_t)roi_mask_scaled->width * roi_mask_scaled->height);
  if(mask_tmp == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[retouch] rt_build_scaled_mask: error allocating memory\n");
    goto cleanup;
  }
  dt_iop_image_fill(mask_tmp, 0.0f, roi_mask_scaled->width, roi_mask_scaled->height, 1);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(mask, roi_in, roi_mask, x_to, y_to) \
  shared(mask_tmp, roi_mask_scaled) \
  schedule(static)
#endif
  for(int yy = roi_mask_scaled->y; yy < y_to; yy++)
  {
    const int mask_index = ((int)(yy / roi_in->scale)) - roi_mask->y;
    if(mask_index < 0 || mask_index >= roi_mask->height) continue;

    const int mask_scaled_index = (yy - roi_mask_scaled->y) * roi_mask_scaled->width;

    const float *m = mask + mask_index * roi_mask->width;
    float *ms = mask_tmp + mask_scaled_index;

    for(int xx = roi_mask_scaled->x; xx < x_to; xx++, ms++)
    {
      const int mx = ((int)(xx / roi_in->scale)) - roi_mask->x;
      if(mx < 0 || mx >= roi_mask->width) continue;

      *ms = m[mx];
    }
  }

cleanup:
  *mask_scaled = mask_tmp;
}

// img_src and mask_scaled must have the same roi
static void rt_copy_image_masked(float *const img_src, float *img_dest, dt_iop_roi_t *const roi_dest,
                                 float *const mask_scaled, dt_iop_roi_t *const roi_mask_scaled,
                                 const float opacity)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(img_src, mask_scaled, opacity, roi_dest, roi_mask_scaled) \
    shared(img_dest) \
    schedule(static)
#endif
  for(int yy = 0; yy < roi_mask_scaled->height; yy++)
  {
    const int mask_index = yy * roi_mask_scaled->width;
    const int src_index = 4 * mask_index;
    const int dest_index
      = 4 * (((yy + roi_mask_scaled->y - roi_dest->y) * roi_dest->width) + (roi_mask_scaled->x - roi_dest->x));

    const float *s = img_src + src_index;
    const float *m = mask_scaled + mask_index;
    float *d = img_dest + dest_index;

    for(int xx = 0; xx < roi_mask_scaled->width; xx++)
    {
      const float f = m[xx] * opacity;
      const float f1 = (1.0f - f);

      for_each_channel(c,aligned(s,d))
      {
        d[4*xx + c] = d[4*xx + c] * f1 + s[4*xx + c] * f;
      }
    }
  }
}

static void rt_copy_mask_to_alpha(float *const img, dt_iop_roi_t *const roi_img, const int ch,
                                  float *const mask_scaled, dt_iop_roi_t *const roi_mask_scaled,
                                  const float opacity)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, img, mask_scaled, opacity, roi_img, roi_mask_scaled) \
  schedule(static)
#endif
  for(int yy = 0; yy < roi_mask_scaled->height; yy++)
  {
    const int mask_index = yy * roi_mask_scaled->width;
    const int dest_index
        = (((yy + roi_mask_scaled->y - roi_img->y) * roi_img->width) + (roi_mask_scaled->x - roi_img->x)) * ch;

    float *d = img + dest_index;
    const float *m = mask_scaled + mask_index;

    for(int xx = 0; xx < roi_mask_scaled->width; xx++, d += ch, m++)
    {
      const float f = (*m) * opacity;
      if(f > d[3]) d[3] = f;
    }
  }
}

static void _retouch_fill(float *const in, dt_iop_roi_t *const roi_in, float *const mask_scaled,
                          dt_iop_roi_t *const roi_mask_scaled, const float opacity, const float *const fill_color)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(fill_color, in, mask_scaled, opacity, roi_in, roi_mask_scaled) \
  schedule(static)
#endif
  for(int yy = 0; yy < roi_mask_scaled->height; yy++)
  {
    const int mask_index = yy * roi_mask_scaled->width;
    const int dest_index
        = (((yy + roi_mask_scaled->y - roi_in->y) * roi_in->width) + (roi_mask_scaled->x - roi_in->x)) * 4;

    float *d = in + dest_index;
    const float *m = mask_scaled + mask_index;

    for(int xx = 0; xx < roi_mask_scaled->width; xx++)
    {
      const float f = m[xx] * opacity;

      for_each_channel(c,aligned(d,fill_color))
        d[4*xx + c] = d[4*xx + c] * (1.0f - f) + fill_color[c] * f;
    }
  }
}

static void _retouch_clone(float *const in, dt_iop_roi_t *const roi_in, float *const mask_scaled,
                           dt_iop_roi_t *const roi_mask_scaled, const int dx, const int dy, const float opacity)
{
  // alloc temp image to avoid issues when areas self-intersects
  float *img_src = dt_alloc_align_float((size_t)4 * roi_mask_scaled->width * roi_mask_scaled->height);
  if(img_src == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[retouch] error allocating memory for cloning\n");
    goto cleanup;
  }

  // copy source image to tmp
  rt_copy_in_to_out(in, roi_in, img_src, roi_mask_scaled, 4, dx, dy);

  // clone it
  rt_copy_image_masked(img_src, in, roi_in, mask_scaled, roi_mask_scaled, opacity);

cleanup:
  if(img_src) dt_free_align(img_src);
}

static void _retouch_blur(dt_iop_module_t *self, float *const in, dt_iop_roi_t *const roi_in, float *const mask_scaled,
                          dt_iop_roi_t *const roi_mask_scaled, const float opacity, const int blur_type,
                          const float blur_radius, dt_dev_pixelpipe_iop_t *piece, const int use_sse)
{
  if(fabsf(blur_radius) <= 0.1f) return;

  const float sigma = blur_radius * roi_in->scale / piece->iscale;

  float *img_dest = NULL;

  // alloc temp image to blur
  img_dest = dt_alloc_align_float((size_t)4 * roi_mask_scaled->width * roi_mask_scaled->height);
  if(img_dest == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[retouch] error allocating memory for blurring\n");
    goto cleanup;
  }

  // copy source image so we blur just the mask area (at least the smallest rect that covers it)
  rt_copy_in_to_out(in, roi_in, img_dest, roi_mask_scaled, 4, 0, 0);

  if(blur_type == DT_IOP_RETOUCH_BLUR_GAUSSIAN && fabsf(blur_radius) > 0.1f)
  {
    float Labmax[] = { INFINITY, INFINITY, INFINITY, INFINITY };
    float Labmin[] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

    dt_gaussian_t *g = dt_gaussian_init(roi_mask_scaled->width, roi_mask_scaled->height, 4, Labmax, Labmin, sigma,
                                        DT_IOP_GAUSSIAN_ZERO);
    if(g)
    {
      dt_gaussian_blur_4c(g, img_dest, img_dest);
      dt_gaussian_free(g);
    }
  }
  else if(blur_type == DT_IOP_RETOUCH_BLUR_BILATERAL && fabsf(blur_radius) > 0.1f)
  {
    const float sigma_r = 100.0f; // does not depend on scale
    const float sigma_s = sigma;
    const float detail = -1.0f; // we want the bilateral base layer

    dt_bilateral_t *b = dt_bilateral_init(roi_mask_scaled->width, roi_mask_scaled->height, sigma_s, sigma_r);
    if(b)
    {
      int converted_cst;
      const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

      if(work_profile)
        dt_ioppr_transform_image_colorspace(self, img_dest, img_dest, roi_mask_scaled->width,
                                            roi_mask_scaled->height, IOP_CS_RGB, IOP_CS_LAB, &converted_cst,
                                            work_profile);
      else
        image_rgb2lab(img_dest, roi_mask_scaled->width, roi_mask_scaled->height, 4, use_sse);

      dt_bilateral_splat(b, img_dest);
      dt_bilateral_blur(b);
      dt_bilateral_slice(b, img_dest, img_dest, detail);
      dt_bilateral_free(b);

      if(work_profile)
        dt_ioppr_transform_image_colorspace(self, img_dest, img_dest, roi_mask_scaled->width,
                                            roi_mask_scaled->height, IOP_CS_LAB, IOP_CS_RGB, &converted_cst,
                                            work_profile);
      else
        image_lab2rgb(img_dest, roi_mask_scaled->width, roi_mask_scaled->height, 4, use_sse);
    }
  }

  // copy blurred (temp) image to destination image
  rt_copy_image_masked(img_dest, in, roi_in, mask_scaled, roi_mask_scaled, opacity);

cleanup:
  if(img_dest) dt_free_align(img_dest);
}

static void _retouch_heal(float *const in, dt_iop_roi_t *const roi_in, float *const mask_scaled,
                          dt_iop_roi_t *const roi_mask_scaled, const int dx, const int dy, const float opacity, const int max_iter)
{
  float *img_src = NULL;
  float *img_dest = NULL;

  // alloc temp images for source and destination
  img_src  = dt_alloc_align_float((size_t)4 * roi_mask_scaled->width * roi_mask_scaled->height);
  img_dest = dt_alloc_align_float((size_t)4 * roi_mask_scaled->width * roi_mask_scaled->height);
  if((img_src == NULL) || (img_dest == NULL))
  {
    dt_print(DT_DEBUG_ALWAYS, "[retouch] error allocating memory for healing\n");
    goto cleanup;
  }

  // copy source and destination to temp images
  rt_copy_in_to_out(in, roi_in, img_src, roi_mask_scaled, 4, dx, dy);
  rt_copy_in_to_out(in, roi_in, img_dest, roi_mask_scaled, 4, 0, 0);

  // heal it
  dt_heal(img_src, img_dest, mask_scaled, roi_mask_scaled->width, roi_mask_scaled->height, 4, max_iter);

  // copy healed (temp) image to destination image
  rt_copy_image_masked(img_dest, in, roi_in, mask_scaled, roi_mask_scaled, opacity);

cleanup:
  if(img_src) dt_free_align(img_src);
  if(img_dest) dt_free_align(img_dest);
}

static void rt_process_forms(float *layer, dwt_params_t *const wt_p, const int scale1)
{
  int scale = scale1;
  retouch_user_data_t *usr_d = (retouch_user_data_t *)wt_p->user_data;
  dt_iop_module_t *self = usr_d->self;
  dt_dev_pixelpipe_iop_t *piece = usr_d->piece;

  // if preview a single scale, just process that scale and original image
  // unless merge is activated
  if(wt_p->merge_from_scale == 0 && wt_p->return_layer > 0 && scale != wt_p->return_layer && scale != 0) return;
  // do not process the reconstructed image
  if(scale > wt_p->scales + 1) return;

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)piece->blendop_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_iop_roi_t *roi_layer = &usr_d->roi;
  const int mask_display = usr_d->mask_display && (scale == usr_d->display_scale);

  // when the requested scales is grather than max scales the residual image index will be different from the one
  // defined by the user,
  // so we need to adjust it here, otherwise we will be using the shapes from a scale on the residual image
  if(wt_p->scales < p->num_scales && wt_p->return_layer == 0 && scale == wt_p->scales + 1)
  {
    scale = p->num_scales + 1;
  }

  // iterate through all forms
  if(!usr_d->suppress_mask)
  {
    const dt_masks_form_t *grp = dt_masks_get_from_id_ext(piece->pipe->forms, bp->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP))
    {
      for(const GList *forms = grp->points; forms; forms = g_list_next(forms))
      {
        const dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
        if(grpt == NULL)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: invalid form\n");
          continue;
        }
        const int formid = grpt->formid;
        const float form_opacity = grpt->opacity;
        if(formid == 0)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: form is null\n");
          continue;
        }
        const int index = rt_get_index_from_formid(p, formid);
        if(index == -1)
        {
          // FIXME: we get this error when user go back in history, so forms are the same but the array has changed
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: missing form=%i from array\n", formid);
          continue;
        }

        // only process current scale
        if(p->rt_forms[index].scale != scale)
        {
          continue;
        }

        // get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form == NULL)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: missing form=%i from masks\n", formid);
          continue;
        }

        // if the form is outside the roi, we just skip it
        if(!rt_masks_form_is_in_roi(self, piece, form, roi_layer, roi_layer))
        {
          continue;
        }

        // get the mask
        float *mask = NULL;
        dt_iop_roi_t roi_mask = { 0 };

        dt_masks_get_mask(self, piece, form, &mask, &roi_mask.width, &roi_mask.height, &roi_mask.x, &roi_mask.y);
        if(mask == NULL)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: error retrieving mask\n");
          continue;
        }

        // search the delta with the source
        const dt_iop_retouch_algo_type_t algo = p->rt_forms[index].algorithm;
        float dx = 0.f, dy = 0.f;

        if(algo != DT_IOP_RETOUCH_BLUR && algo != DT_IOP_RETOUCH_FILL)
        {
          if(!rt_masks_get_delta_to_destination(self, piece, roi_layer, form, &dx, &dy,
                                                p->rt_forms[index].distort_mode))
          {
            if(mask) dt_free_align(mask);
            continue;
          }
        }

        // scale the mask
        float *mask_scaled = NULL;
        dt_iop_roi_t roi_mask_scaled = { 0 };

        rt_build_scaled_mask(mask, &roi_mask, &mask_scaled, &roi_mask_scaled, roi_layer, dx, dy, algo);

        // we don't need the original mask anymore
        if(mask)
        {
          dt_free_align(mask);
          mask = NULL;
        }

        if(mask_scaled == NULL)
        {
          continue;
        }

        if((dx != 0 || dy != 0 || algo == DT_IOP_RETOUCH_BLUR || algo == DT_IOP_RETOUCH_FILL)
           && ((roi_mask_scaled.width > 2) && (roi_mask_scaled.height > 2)))
        {
          if(algo == DT_IOP_RETOUCH_CLONE)
          {
            _retouch_clone(layer, roi_layer, mask_scaled, &roi_mask_scaled, dx, dy, form_opacity);
          }
          else if(algo == DT_IOP_RETOUCH_HEAL)
          {
            _retouch_heal(layer, roi_layer, mask_scaled, &roi_mask_scaled, dx, dy, form_opacity, p->max_heal_iter);
          }
          else if(algo == DT_IOP_RETOUCH_BLUR)
          {
            _retouch_blur(self, layer, roi_layer, mask_scaled, &roi_mask_scaled, form_opacity,
                          p->rt_forms[index].blur_type, p->rt_forms[index].blur_radius, piece, wt_p->use_sse);
          }
          else if(algo == DT_IOP_RETOUCH_FILL)
          {
            // add a brightness to the color so it can be fine-adjusted by the user
            dt_aligned_pixel_t fill_color;

            if(p->rt_forms[index].fill_mode == DT_IOP_RETOUCH_FILL_ERASE)
            {
              fill_color[0] = fill_color[1] = fill_color[2] = p->rt_forms[index].fill_brightness;
            }
            else
            {
              fill_color[0] = p->rt_forms[index].fill_color[0] + p->rt_forms[index].fill_brightness;
              fill_color[1] = p->rt_forms[index].fill_color[1] + p->rt_forms[index].fill_brightness;
              fill_color[2] = p->rt_forms[index].fill_color[2] + p->rt_forms[index].fill_brightness;
            }
            fill_color[3] = 0.0f;

            _retouch_fill(layer, roi_layer, mask_scaled, &roi_mask_scaled, form_opacity, fill_color);
          }
          else
            dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: unknown algorithm %i\n", algo);

          if(mask_display)
            rt_copy_mask_to_alpha(layer, roi_layer, wt_p->ch, mask_scaled, &roi_mask_scaled, form_opacity);
        }

        if(mask) dt_free_align(mask);
        if(mask_scaled) dt_free_align(mask_scaled);
      }
    }
  }
}

static void process_internal(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                             void *const ovoid, const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out, const int use_sse)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  float *in_retouch = NULL;

  dt_iop_roi_t roi_retouch = *roi_in;
  dt_iop_roi_t *roi_rt = &roi_retouch;

  retouch_user_data_t usr_data = { 0 };
  dwt_params_t *dwt_p = NULL;

  const int gui_active = (self->dev) ? (self == self->dev->gui_module) : 0;
  const int display_wavelet_scale = (g && gui_active) ? g->display_wavelet_scale : 0;

  // we will do all the clone, heal, etc on the input image,
  // this way the source for one algorithm can be the destination from a previous one
  in_retouch = dt_alloc_align_float((size_t)4 * roi_rt->width * roi_rt->height);
  if(in_retouch == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS,"[retouch] out of memory\n");
    goto cleanup;
  }
  dt_iop_image_copy_by_size(in_retouch, ivoid, roi_rt->width, roi_rt->height, 4);

  // user data passed from the decompose routine to the one that process each scale
  usr_data.self = self;
  usr_data.piece = piece;
  usr_data.roi = *roi_rt;
  usr_data.mask_display = 0;
  usr_data.suppress_mask = (g && g->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                            && (piece->pipe == self->dev->pipe));
  usr_data.display_scale = p->curr_scale;

  // init the decompose routine
  dwt_p = dt_dwt_init(in_retouch, roi_rt->width, roi_rt->height, 4, p->num_scales,
                      (!display_wavelet_scale || !(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)) ? 0 : p->curr_scale,
                      p->merge_from_scale, &usr_data,
                      roi_in->scale / piece->iscale, use_sse);
  if(dwt_p == NULL) goto cleanup;

  // check if this module should expose mask.
  if((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && g
     && (g->mask_display || display_wavelet_scale) && self->dev->gui_attached
     && (self == self->dev->gui_module) && (piece->pipe == self->dev->pipe))
  {
    for(size_t j = 0; j < (size_t)roi_rt->width * roi_rt->height * 4; j += 4) in_retouch[j + 3] = 0.f;

    piece->pipe->mask_display = g->mask_display ? DT_DEV_PIXELPIPE_DISPLAY_MASK : DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    piece->pipe->bypass_blendif = 1;
    usr_data.mask_display = 1;
  }

  if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
  {
    // check if the image support this number of scales
    if(gui_active)
    {
      const int max_scales = dwt_get_max_scale(dwt_p);
      if(dwt_p->scales > max_scales)
      {
        dt_control_log(_("max scale is %i for this image size"), max_scales);
      }
    }
    // get first scale visible at this zoom level
    if(g) g->first_scale_visible = dt_dwt_first_scale_visible(dwt_p);
  }

  // decompose it
  dwt_decompose(dwt_p, rt_process_forms);

  dt_aligned_pixel_t levels = { p->preview_levels[0], p->preview_levels[1], p->preview_levels[2] };

  // process auto levels
  if(g && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_gui_enter_critical_section(self);
    if(g->preview_auto_levels == 1 && !darktable.gui->reset)
    {
      g->preview_auto_levels = -1;

      dt_iop_gui_leave_critical_section(self);

      levels[0] = levels[1] = levels[2] = 0;
      rt_process_stats(self, piece, in_retouch, roi_rt->width, roi_rt->height, 4, levels);
      rt_clamp_minmax(levels, levels);

      for(int i = 0; i < 3; i++) g->preview_levels[i] = levels[i];

      dt_iop_gui_enter_critical_section(self);
      g->preview_auto_levels = 2;
    }
    dt_iop_gui_leave_critical_section(self);
  }

  // if user wants to preview a detail scale adjust levels
  if(dwt_p->return_layer > 0 && dwt_p->return_layer < dwt_p->scales + 1)
  {
    rt_adjust_levels(self, piece, in_retouch, roi_rt->width, roi_rt->height, 4, levels);
  }

  // copy alpha channel if needed
  if((piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) && g && !g->mask_display)
  {
    dt_iop_alpha_copy(ivoid, in_retouch, roi_rt->width, roi_rt->height);
  }

  // return final image
  rt_copy_in_to_out(in_retouch, roi_rt, ovoid, roi_out, 4, 0, 0);

cleanup:
  if(in_retouch) dt_free_align(in_retouch);
  if(dwt_p) dt_dwt_free(dwt_p);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  process_internal(self, piece, ivoid, ovoid, roi_in, roi_out, 0);
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  process_internal(self, piece, ivoid, ovoid, roi_in, roi_out, 1);
}
#endif

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  rt_copy_in_to_out(in, roi_in, out, roi_out, 1, 0, 0);
}

#ifdef HAVE_OPENCL

cl_int rt_process_stats_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const int devid, cl_mem dev_img,
                           const int width, const int height, float levels[3])
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  float *src_buffer = NULL;

  src_buffer = dt_alloc_align_float((size_t)ch * width * height);
  if(src_buffer == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[retouch] error allocating memory for healing (OpenCL)\n");
    err = DT_OPENCL_SYSMEM_ALLOCATION;
    goto cleanup;
  }

  err = dt_opencl_read_buffer_from_device(devid, (void *)src_buffer, dev_img, 0,
                                          (size_t)width * height * ch * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

  // just call the CPU version for now
  rt_process_stats(self, piece, src_buffer, width, height, ch, levels);

  err = dt_opencl_write_buffer_to_device(devid, src_buffer, dev_img, 0, sizeof(float) * ch * width * height, CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

cleanup:
  if(src_buffer) dt_free_align(src_buffer);

  return err;
}

cl_int rt_adjust_levels_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const int devid, cl_mem dev_img,
                           const int width, const int height, const float levels[3])
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  float *src_buffer = NULL;

  src_buffer = dt_alloc_align_float((size_t)ch * width * height);
  if(src_buffer == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[retouch] error allocating memory for healing (OpenCL)\n");
    err = DT_OPENCL_SYSMEM_ALLOCATION;
    goto cleanup;
  }

  err = dt_opencl_read_buffer_from_device(devid, (void *)src_buffer, dev_img, 0,
                                          (size_t)width * height * ch * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

  // just call the CPU version for now
  rt_adjust_levels(self, piece, src_buffer, width, height, ch, levels);

  err = dt_opencl_write_buffer_to_device(devid, src_buffer, dev_img, 0, sizeof(float) * ch * width * height, CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

cleanup:
  if(src_buffer) dt_free_align(src_buffer);

  return err;
}

static cl_int rt_copy_in_to_out_cl(const int devid, cl_mem dev_in, const struct dt_iop_roi_t *const roi_in,
                                   cl_mem dev_out, const struct dt_iop_roi_t *const roi_out, const int dx,
                                   const int dy, const int kernel)
{
  cl_int err = CL_SUCCESS;

  const int xoffs = roi_out->x - roi_in->x - dx;
  const int yoffs = roi_out->y - roi_in->y - dy;

  cl_mem dev_roi_in = NULL;
  cl_mem dev_roi_out = NULL;


  dev_roi_in = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_in);
  dev_roi_out = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_out);
  if(dev_roi_in == NULL || dev_roi_out == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "rt_copy_in_to_out_cl error 1\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, MIN(roi_out->width, roi_in->width), MIN(roi_out->height, roi_in->height),
    CLARG(dev_in), CLARG(dev_roi_in), CLARG(dev_out), CLARG(dev_roi_out), CLARG(xoffs), CLARG(yoffs));
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "rt_copy_in_to_out_cl error 2\n");
    goto cleanup;
  }

cleanup:
  if(dev_roi_in) dt_opencl_release_mem_object(dev_roi_in);
  if(dev_roi_out) dt_opencl_release_mem_object(dev_roi_out);

  return err;
}

static cl_int rt_build_scaled_mask_cl(const int devid, float *const mask, dt_iop_roi_t *const roi_mask,
                                      float **mask_scaled, cl_mem *p_dev_mask_scaled,
                                      dt_iop_roi_t *roi_mask_scaled, dt_iop_roi_t *const roi_in, const int dx,
                                      const int dy, const int algo)
{
  cl_int err = CL_SUCCESS;

  rt_build_scaled_mask(mask, roi_mask, mask_scaled, roi_mask_scaled, roi_in, dx, dy, algo);
  if(*mask_scaled == NULL)
  {
    goto cleanup;
  }

  const cl_mem dev_mask_scaled
      = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_mask_scaled->width * roi_mask_scaled->height);
  if(dev_mask_scaled == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "rt_build_scaled_mask_cl error 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_write_buffer_to_device(devid, *mask_scaled, dev_mask_scaled, 0,
                                         sizeof(float) * roi_mask_scaled->width * roi_mask_scaled->height, CL_TRUE);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "rt_build_scaled_mask_cl error 4\n");
    goto cleanup;
  }

  *p_dev_mask_scaled = dev_mask_scaled;

cleanup:
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_ALWAYS, "rt_build_scaled_mask_cl error\n");

  return err;
}

static cl_int rt_copy_image_masked_cl(const int devid, cl_mem dev_src, cl_mem dev_dest,
                                      dt_iop_roi_t *const roi_dest, cl_mem dev_mask_scaled,
                                      dt_iop_roi_t *const roi_mask_scaled, const float opacity, const int kernel)
{
  cl_int err = CL_SUCCESS;


  const cl_mem dev_roi_dest =
    dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_dest);

  const cl_mem dev_roi_mask_scaled
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_mask_scaled);

  if(dev_roi_dest == NULL || dev_roi_mask_scaled == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_mask_scaled->width, roi_mask_scaled->height,
    CLARG(dev_src), CLARG(dev_dest), CLARG(dev_roi_dest), CLARG(dev_mask_scaled), CLARG(dev_roi_mask_scaled),
    CLARG(opacity));
  if(err != CL_SUCCESS) goto cleanup;

cleanup:
  if(dev_roi_dest) dt_opencl_release_mem_object(dev_roi_dest);
  if(dev_roi_mask_scaled) dt_opencl_release_mem_object(dev_roi_mask_scaled);

  return err;
}

static cl_int rt_copy_mask_to_alpha_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer,
                                       cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled,
                                       const float opacity, dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  // fill it
  const int kernel = gd->kernel_retouch_copy_mask_to_alpha;

  const cl_mem  dev_roi_layer = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_layer);
  const cl_mem dev_roi_mask_scaled
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_mask_scaled);
  if(dev_roi_layer == NULL || dev_roi_mask_scaled == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_mask_scaled->width, roi_mask_scaled->height,
    CLARG(dev_layer), CLARG(dev_roi_layer), CLARG(dev_mask_scaled), CLARG(dev_roi_mask_scaled), CLARG(opacity));
  if(err != CL_SUCCESS) goto cleanup;


cleanup:
  dt_opencl_release_mem_object(dev_roi_layer);
  dt_opencl_release_mem_object(dev_roi_mask_scaled);

  return err;
}

static cl_int _retouch_clone_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer,
                                cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const int dx,
                                const int dy, const float opacity, dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  // alloc source temp image to avoid issues when areas self-intersects
  const cl_mem dev_src = dt_opencl_alloc_device_buffer(devid,
                                          sizeof(float) * ch * roi_mask_scaled->width * roi_mask_scaled->height);
  if(dev_src == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_clone_cl error 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  // copy source image to tmp
  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_src, roi_mask_scaled, dx, dy,
                             gd->kernel_retouch_copy_buffer_to_buffer);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_clone_cl error 4\n");
    goto cleanup;
  }

  // clone it
  err = rt_copy_image_masked_cl(devid, dev_src, dev_layer, roi_layer, dev_mask_scaled, roi_mask_scaled, opacity,
                                gd->kernel_retouch_copy_buffer_to_buffer_masked);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_clone_cl error 5\n");
    goto cleanup;
  }

cleanup:
  if(dev_src) dt_opencl_release_mem_object(dev_src);

  return err;
}

static cl_int _retouch_fill_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer,
                               cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const float opacity,
                               float *color, dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  // fill it
  const int kernel = gd->kernel_retouch_fill;

  const cl_mem dev_roi_layer = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_layer);
  const cl_mem dev_roi_mask_scaled
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_mask_scaled);
  if(dev_roi_layer == NULL || dev_roi_mask_scaled == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_mask_scaled->width, roi_mask_scaled->height,
    CLARG(dev_layer), CLARG(dev_roi_layer), CLARG(dev_mask_scaled), CLARG(dev_roi_mask_scaled), CLARG(opacity),
    CLARG((color[0])), CLARG((color[1])), CLARG((color[2])));
  if(err != CL_SUCCESS) goto cleanup;


cleanup:
  dt_opencl_release_mem_object(dev_roi_layer);
  dt_opencl_release_mem_object(dev_roi_mask_scaled);

  return err;
}

static cl_int _retouch_blur_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer,
                               cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const float opacity,
                               const int blur_type, const float blur_radius, dt_dev_pixelpipe_iop_t *piece,
                               dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  if(fabsf(blur_radius) <= 0.1f) return err;

  const float sigma = blur_radius * roi_layer->scale / piece->iscale;
  const int ch = 4;

  const cl_mem dev_dest =
    dt_opencl_alloc_device(devid, roi_mask_scaled->width, roi_mask_scaled->height, sizeof(float) * ch);
  if(dev_dest == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_blur_cl error 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  if(blur_type == DT_IOP_RETOUCH_BLUR_BILATERAL)
  {
    const int kernel = gd->kernel_retouch_image_rgb2lab;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_layer->width, roi_layer->height,
      CLARG(dev_layer), CLARG((roi_layer->width)), CLARG((roi_layer->height)));
    if(err != CL_SUCCESS) goto cleanup;
  }

  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_dest, roi_mask_scaled, 0, 0,
                             gd->kernel_retouch_copy_buffer_to_image);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_blur_cl error 4\n");
    goto cleanup;
  }

  if(blur_type == DT_IOP_RETOUCH_BLUR_GAUSSIAN && fabsf(blur_radius) > 0.1f)
  {
    float Labmax[] = { INFINITY, INFINITY, INFINITY, INFINITY };
    float Labmin[] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

    dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, roi_mask_scaled->width, roi_mask_scaled->height, ch, Labmax,
                                              Labmin, sigma, DT_IOP_GAUSSIAN_ZERO);
    if(g)
    {
      err = dt_gaussian_blur_cl(g, dev_dest, dev_dest);
      dt_gaussian_free_cl(g);
      if(err != CL_SUCCESS) goto cleanup;
    }
  }
  else if(blur_type == DT_IOP_RETOUCH_BLUR_BILATERAL && fabsf(blur_radius) > 0.1f)
  {
    const float sigma_r = 100.0f; // does not depend on scale
    const float sigma_s = sigma;
    const float detail = -1.0f; // we want the bilateral base layer

    dt_bilateral_cl_t *b
        = dt_bilateral_init_cl(devid, roi_mask_scaled->width, roi_mask_scaled->height, sigma_s, sigma_r);
    if(b)
    {
      err = dt_bilateral_splat_cl(b, dev_dest);
      if(err == CL_SUCCESS) err = dt_bilateral_blur_cl(b);
      if(err == CL_SUCCESS) err = dt_bilateral_slice_cl(b, dev_dest, dev_dest, detail);

      dt_bilateral_free_cl(b);
    }
  }

  // copy blurred (temp) image to destination image
  err = rt_copy_image_masked_cl(devid, dev_dest, dev_layer, roi_layer, dev_mask_scaled, roi_mask_scaled, opacity,
                                gd->kernel_retouch_copy_image_to_buffer_masked);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_blur_cl error 5\n");
    goto cleanup;
  }

  if(blur_type == DT_IOP_RETOUCH_BLUR_BILATERAL)
  {
    const int kernel = gd->kernel_retouch_image_lab2rgb;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_layer->width, roi_layer->height,
      CLARG(dev_layer), CLARG((roi_layer->width)), CLARG((roi_layer->height)));
    if(err != CL_SUCCESS) goto cleanup;
  }

cleanup:
  if(dev_dest) dt_opencl_release_mem_object(dev_dest);

  return err;
}

static cl_int _retouch_heal_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer, float *mask_scaled,
                               cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const int dx,
                               const int dy, const float opacity, dt_iop_retouch_global_data_t *gd, const int max_iter)
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  cl_mem dev_dest = NULL;
  cl_mem dev_src = dt_opencl_alloc_device_buffer(devid,
                                          sizeof(float) * ch * roi_mask_scaled->width * roi_mask_scaled->height);
  if(dev_src == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_dest = dt_opencl_alloc_device_buffer(devid,
                                           sizeof(float) * ch * roi_mask_scaled->width * roi_mask_scaled->height);
  if(dev_dest == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_src, roi_mask_scaled, dx, dy,
                             gd->kernel_retouch_copy_buffer_to_buffer);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_heal_cl error 4\n");
    goto cleanup;
  }

  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_dest, roi_mask_scaled, 0, 0,
                             gd->kernel_retouch_copy_buffer_to_buffer);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_heal_cl error 4\n");
    goto cleanup;
  }

  // heal it
  heal_params_cl_t *hp = dt_heal_init_cl(devid);
  if(hp)
  {
    err = dt_heal_cl(hp, dev_src, dev_dest, mask_scaled, roi_mask_scaled->width, roi_mask_scaled->height, max_iter);
    dt_heal_free_cl(hp);

    dt_opencl_release_mem_object(dev_src);
    dev_src = NULL;

    if(err != CL_SUCCESS) goto cleanup;
  }

  // copy healed (temp) image to destination image
  err = rt_copy_image_masked_cl(devid, dev_dest, dev_layer, roi_layer, dev_mask_scaled, roi_mask_scaled, opacity,
                                gd->kernel_retouch_copy_buffer_to_buffer_masked);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "retouch_heal_cl error 6\n");
    goto cleanup;
  }

cleanup:
  if(dev_src) dt_opencl_release_mem_object(dev_src);
  if(dev_dest) dt_opencl_release_mem_object(dev_dest);

  return err;
}

static cl_int rt_process_forms_cl(cl_mem dev_layer, dwt_params_cl_t *const wt_p, const int scale1)
{
  cl_int err = CL_SUCCESS;

  int scale = scale1;
  retouch_user_data_t *usr_d = (retouch_user_data_t *)wt_p->user_data;
  dt_iop_module_t *self = usr_d->self;
  dt_dev_pixelpipe_iop_t *piece = usr_d->piece;

  // if preview a single scale, just process that scale and original image
  // unless merge is activated
  if(wt_p->merge_from_scale == 0 && wt_p->return_layer > 0 && scale != wt_p->return_layer && scale != 0)
    return err;
  // do not process the reconstructed image
  if(scale > wt_p->scales + 1) return err;

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)piece->blendop_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_iop_retouch_global_data_t *gd = (dt_iop_retouch_global_data_t *)self->global_data;
  const int devid = piece->pipe->devid;
  dt_iop_roi_t *roi_layer = &usr_d->roi;
  const int mask_display = usr_d->mask_display && (scale == usr_d->display_scale);

  // when the requested scales is grather than max scales the residual image index will be different from the one
  // defined by the user,
  // so we need to adjust it here, otherwise we will be using the shapes from a scale on the residual image
  if(wt_p->scales < p->num_scales && wt_p->return_layer == 0 && scale == wt_p->scales + 1)
  {
    scale = p->num_scales + 1;
  }

  // iterate through all forms
  if(!usr_d->suppress_mask)
  {
    dt_masks_form_t *grp = dt_masks_get_from_id_ext(piece->pipe->forms, bp->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP))
    {
      for(const GList *forms = grp->points; forms && err == CL_SUCCESS; forms = g_list_next(forms))
      {
        dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
        if(grpt == NULL)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: invalid form\n");
          continue;
        }
        const int formid = grpt->formid;
        const float form_opacity = grpt->opacity;
        if(formid == 0)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: form is null\n");
          continue;
        }
        const int index = rt_get_index_from_formid(p, formid);
        if(index == -1)
        {
          // FIXME: we get this error when user go back in history, so forms are the same but the array has changed
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: missing form=%i from array\n", formid);
          continue;
        }

        // only process current scale
        if(p->rt_forms[index].scale != scale)
        {
          continue;
        }

        // get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form == NULL)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: missing form=%i from masks\n", formid);
          continue;
        }

        // if the form is outside the roi, we just skip it
        if(!rt_masks_form_is_in_roi(self, piece, form, roi_layer, roi_layer))
        {
          continue;
        }

        // get the mask
        float *mask = NULL;
        dt_iop_roi_t roi_mask = { 0 };

        dt_masks_get_mask(self, piece, form, &mask, &roi_mask.width, &roi_mask.height, &roi_mask.x, &roi_mask.y);
        if(mask == NULL)
        {
          dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: error retrieving mask\n");
          continue;
        }

        float dx = 0.f, dy = 0.f;

        // search the delta with the source
        const dt_iop_retouch_algo_type_t algo = p->rt_forms[index].algorithm;
        if(algo != DT_IOP_RETOUCH_BLUR && algo != DT_IOP_RETOUCH_FILL)
        {
          if(!rt_masks_get_delta_to_destination(self, piece, roi_layer, form, &dx, &dy,
                                                p->rt_forms[index].distort_mode))
          {
            if(mask) dt_free_align(mask);
            continue;
          }
        }

        // scale the mask
        cl_mem dev_mask_scaled = NULL;
        float *mask_scaled = NULL;
        dt_iop_roi_t roi_mask_scaled = { 0 };

        err = rt_build_scaled_mask_cl(devid, mask, &roi_mask, &mask_scaled, &dev_mask_scaled, &roi_mask_scaled,
                                      roi_layer, dx, dy, algo);

        // only heal needs mask scaled
        if(algo != DT_IOP_RETOUCH_HEAL && mask_scaled != NULL)
        {
          dt_free_align(mask_scaled);
          mask_scaled = NULL;
        }

        // we don't need the original mask anymore
        if(mask)
        {
          dt_free_align(mask);
          mask = NULL;
        }

        if(mask_scaled == NULL && algo == DT_IOP_RETOUCH_HEAL)
        {
          if(dev_mask_scaled) dt_opencl_release_mem_object(dev_mask_scaled);
          dev_mask_scaled = NULL;
          continue;
        }

        if((err == CL_SUCCESS)
           && (dx != 0 || dy != 0 || algo == DT_IOP_RETOUCH_BLUR || algo == DT_IOP_RETOUCH_FILL)
           && ((roi_mask_scaled.width > 2) && (roi_mask_scaled.height > 2)))
        {
          if(algo == DT_IOP_RETOUCH_CLONE)
          {
            err = _retouch_clone_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, dx, dy,
                                    form_opacity, gd);
          }
          else if(algo == DT_IOP_RETOUCH_HEAL)
          {
            err = _retouch_heal_cl(devid, dev_layer, roi_layer, mask_scaled, dev_mask_scaled, &roi_mask_scaled, dx,
                                   dy, form_opacity, gd, p->max_heal_iter);
          }
          else if(algo == DT_IOP_RETOUCH_BLUR)
          {
            err = _retouch_blur_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, form_opacity,
                                   p->rt_forms[index].blur_type, p->rt_forms[index].blur_radius, piece, gd);
          }
          else if(algo == DT_IOP_RETOUCH_FILL)
          {
            // add a brightness to the color so it can be fine-adjusted by the user
            dt_aligned_pixel_t fill_color;

            if(p->rt_forms[index].fill_mode == DT_IOP_RETOUCH_FILL_ERASE)
            {
              fill_color[0] = fill_color[1] = fill_color[2] = p->rt_forms[index].fill_brightness;
            }
            else
            {
              fill_color[0] = p->rt_forms[index].fill_color[0] + p->rt_forms[index].fill_brightness;
              fill_color[1] = p->rt_forms[index].fill_color[1] + p->rt_forms[index].fill_brightness;
              fill_color[2] = p->rt_forms[index].fill_color[2] + p->rt_forms[index].fill_brightness;
            }

            err = _retouch_fill_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, form_opacity,
                                   fill_color, gd);
          }
          else
            dt_print(DT_DEBUG_ALWAYS, "rt_process_forms: unknown algorithm %i\n", algo);

          if(mask_display)
            rt_copy_mask_to_alpha_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, form_opacity,
                                     gd);
        }

        if(mask) dt_free_align(mask);
        if(mask_scaled) dt_free_align(mask_scaled);
        if(dev_mask_scaled) dt_opencl_release_mem_object(dev_mask_scaled);
      }
    }
  }

  return err;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_iop_retouch_global_data_t *gd = (dt_iop_retouch_global_data_t *)self->global_data;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  cl_int err = CL_SUCCESS;
  const int devid = piece->pipe->devid;

  dt_iop_roi_t roi_retouch = *roi_in;
  dt_iop_roi_t *roi_rt = &roi_retouch;

  const int ch = piece->colors;
  retouch_user_data_t usr_data = { 0 };
  dwt_params_cl_t *dwt_p = NULL;

  const int gui_active = (self->dev) ? (self == self->dev->gui_module) : 0;
  const int display_wavelet_scale = (g && gui_active) ? g->display_wavelet_scale : 0;

  // we will do all the clone, heal, etc on the input image,
  // this way the source for one algorithm can be the destination from a previous one
  const cl_mem in_retouch = dt_opencl_alloc_device_buffer(devid, sizeof(float) * ch * roi_rt->width * roi_rt->height);
  if(in_retouch == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[retouch process_cl] error allocating memory for wavelet decompose on device %d\n", devid);
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  // copy input image to the new buffer
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { roi_rt->width, roi_rt->height, 1 };
    err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, in_retouch, origin, region, 0);
    if(err != CL_SUCCESS) goto cleanup;
  }

  // user data passed from the decompose routine to the one that process each scale
  usr_data.self = self;
  usr_data.piece = piece;
  usr_data.roi = *roi_rt;
  usr_data.mask_display = 0;
  usr_data.suppress_mask = (g && g->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                            && (piece->pipe == self->dev->pipe));
  usr_data.display_scale = p->curr_scale;

  // init the decompose routine
  dwt_p = dt_dwt_init_cl(devid, in_retouch, roi_rt->width, roi_rt->height, p->num_scales,
                         (!display_wavelet_scale || !(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)) ? 0 : p->curr_scale,
                         p->merge_from_scale, &usr_data,
                         roi_in->scale / piece->iscale);
  if(dwt_p == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[retouch process_cl] error initializing wavelet decompose on device %d\n", devid);
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  // check if this module should expose mask.
  if((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && g && g->mask_display && self->dev->gui_attached
     && (self == self->dev->gui_module) && (piece->pipe == self->dev->pipe))
  {
    const int kernel = gd->kernel_retouch_clear_alpha;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_rt->width, roi_rt->height,
      CLARG(in_retouch), CLARG((roi_rt->width)), CLARG((roi_rt->height)));
    if(err != CL_SUCCESS) goto cleanup;

    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    piece->pipe->bypass_blendif = 1;
    usr_data.mask_display = 1;
  }

  if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
  {
    // check if the image support this number of scales
    if(gui_active)
    {
      const int max_scales = dwt_get_max_scale_cl(dwt_p);
      if(dwt_p->scales > max_scales)
      {
        dt_control_log(_("max scale is %i for this image size"), max_scales);
      }
    }
    // get first scale visible at this zoom level
    if(g) g->first_scale_visible = dt_dwt_first_scale_visible_cl(dwt_p);
  }

  // decompose it
  err = dwt_decompose_cl(dwt_p, rt_process_forms_cl);
  if(err != CL_SUCCESS) goto cleanup;

  dt_aligned_pixel_t levels = { p->preview_levels[0], p->preview_levels[1], p->preview_levels[2] };

  // process auto levels
  if(g && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_gui_enter_critical_section(self);
    if(g->preview_auto_levels == 1 && !darktable.gui->reset)
    {
      g->preview_auto_levels = -1;

      dt_iop_gui_leave_critical_section(self);

      levels[0] = levels[1] = levels[2] = 0;
      err = rt_process_stats_cl(self, piece, devid, in_retouch, roi_rt->width, roi_rt->height, levels);
      if(err != CL_SUCCESS) goto cleanup;

      rt_clamp_minmax(levels, levels);

      for(int i = 0; i < 3; i++) g->preview_levels[i] = levels[i];

      dt_iop_gui_enter_critical_section(self);
      g->preview_auto_levels = 2;
    }
    dt_iop_gui_leave_critical_section(self);
  }

  // if user wants to preview a detail scale adjust levels
  if(dwt_p->return_layer > 0 && dwt_p->return_layer < dwt_p->scales + 1)
  {
    err = rt_adjust_levels_cl(self, piece, devid, in_retouch, roi_rt->width, roi_rt->height, levels);
    if(err != CL_SUCCESS) goto cleanup;
  }

  // copy alpha channel if needed
  if((piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) && g && !g->mask_display)
  {
    const int kernel = gd->kernel_retouch_copy_alpha;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, roi_rt->width, roi_rt->height,
      CLARG(dev_in), CLARG(in_retouch), CLARG((roi_rt->width)), CLARG((roi_rt->height)));
    if(err != CL_SUCCESS) goto cleanup;
  }

  // return final image
  err = rt_copy_in_to_out_cl(devid, in_retouch, roi_in, dev_out, roi_out, 0, 0,
                             gd->kernel_retouch_copy_buffer_to_image);

cleanup:
  if(dwt_p) dt_dwt_free_cl(dwt_p);

  if(in_retouch) dt_opencl_release_mem_object(in_retouch);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_retouch] couldn't enqueue kernel! %s\n", cl_errstr(err));

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
