/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.
    copyright (c) 2017 edgardo hoszowski.

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
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/imageop_math.h"
#include "develop/masks.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_retouch_params_t)

#define RETOUCH_NO_FORMS 300
#define RETOUCH_MAX_SCALES 15
#define RETOUCH_NO_SCALES (RETOUCH_MAX_SCALES + 2)

#define RETOUCH_PREVIEW_LVL_MIN -3.0f
#define RETOUCH_PREVIEW_LVL_MAX 3.0f

typedef enum dt_iop_retouch_drag_types_t {
  dt_iop_retouch_wdbar_drag_top = 1,
  dt_iop_retouch_wdbar_drag_bottom = 2,
  dt_iop_retouch_lvlbar_drag_left = 3,
  dt_iop_retouch_lvlbar_drag_middle = 4,
  dt_iop_retouch_lvlbar_drag_right = 5
} dt_iop_retouch_drag_types_t;

typedef enum dt_iop_retouch_fill_modes_t {
  dt_iop_retouch_fill_erase = 0,
  dt_iop_retouch_fill_color = 1
} dt_iop_retouch_fill_modes_t;

typedef enum dt_iop_retouch_blur_types_t {
  dt_iop_retouch_blur_gaussian = 0,
  dt_iop_retouch_blur_bilateral = 1
} dt_iop_retouch_blur_types_t;

typedef enum dt_iop_retouch_algo_type_t {
  dt_iop_retouch_clone = 1,
  dt_iop_retouch_heal = 2,
  dt_iop_retouch_blur = 3,
  dt_iop_retouch_fill = 4
} dt_iop_retouch_algo_type_t;

typedef struct dt_iop_retouch_form_data_t
{
  int formid; // from masks, form->formid
  int scale;  // 0==original image; 1..RETOUCH_MAX_SCALES==scale; RETOUCH_MAX_SCALES+1==residual
  dt_iop_retouch_algo_type_t algorithm; // clone, heal, blur, fill

  int blur_type;     // gaussian, bilateral
  float blur_radius; // radius for blur algorithm

  int fill_mode;         // mode for fill algorithm, erase or fill with color
  float fill_color[3];   // color for fill algorithm
  float fill_brightness; // value to be added to the color
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

  dt_iop_retouch_algo_type_t algorithm; // clone, heal, blur, fill

  int num_scales; // number of wavelets scales
  int curr_scale; // current wavelet scale
  int merge_from_scale;

  float preview_levels[3];

  int blur_type;     // gaussian, bilateral
  float blur_radius; // radius for blur algorithm

  int fill_mode;         // mode for fill algorithm, erase or fill with color
  float fill_color[3];   // color for fill algorithm
  float fill_brightness; // value to be added to the color
} dt_iop_retouch_params_t;

typedef struct dt_iop_retouch_gui_data_t
{
  dt_pthread_mutex_t lock;

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
  GtkWidget *bt_showmask, *bt_suppress;                                   // supress & show masks

  GtkWidget *wd_bar; // wavelet decompose bar
  GtkLabel *lbl_num_scales;
  GtkLabel *lbl_curr_scale;
  GtkLabel *lbl_merge_from_scale;
  float wdbar_mouse_x, wdbar_mouse_y;
  gboolean is_dragging;

  GtkWidget *bt_display_wavelet_scale; // show decomposed scale

  GtkWidget *bt_copy_scale; // copy all shapes from one scale to another
  GtkWidget *bt_paste_scale;

  GtkWidget *vbox_preview_scale;
  GtkWidget *preview_levels_bar;
  float lvlbar_mouse_x, lvlbar_mouse_y;
  GtkWidget *bt_auto_levels;

  GtkWidget *vbox_blur;
  GtkWidget *cmb_blur_type;
  GtkWidget *sl_blur_radius;

  GtkWidget *vbox_fill;
  GtkWidget *hbox_color_pick;
  GtkWidget *colorpick;          // select a specific color
  GtkToggleButton *color_picker; // pick a color from the picture

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

int groups()
{
  return dt_iop_get_group("retouch", IOP_GROUP_CORRECT);
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_NO_MASKS;
}

//---------------------------------------------------------------------------------
// draw buttons
//---------------------------------------------------------------------------------

#define PREAMBLE                                                                                                  \
  cairo_save(cr);                                                                                                 \
  const gint s = MIN(w, h);                                                                                       \
  cairo_translate(cr, x + (w / 2.0) - (s / 2.0), y + (h / 2.0) - (s / 2.0));                                      \
  cairo_scale(cr, s, s);                                                                                          \
  cairo_push_group(cr);                                                                                           \
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);                                                                  \
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);                                                                   \
  cairo_set_line_width(cr, 0.1);

#define POSTAMBLE                                                                                                 \
  cairo_pop_group_to_source(cr);                                                                                  \
  cairo_paint_with_alpha(cr, flags &CPF_ACTIVE ? 1.0 : 0.5);                                                      \
  cairo_restore(cr);

static void _retouch_cairo_paint_tool_clone(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                            const gint flags, void *data)
{
  PREAMBLE;

  cairo_arc(cr, 0.65, 0.35, 0.35, 0, 2 * M_PI);
  cairo_stroke(cr);

  cairo_arc(cr, 0.35, 0.65, 0.35, 0, 2 * M_PI);
  cairo_stroke(cr);

  POSTAMBLE;
}

static void _retouch_cairo_paint_tool_heal(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                           const gint flags, void *data)
{
  PREAMBLE;

  cairo_rectangle(cr, 0., 0., 1., 1.);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, .74, 0.13, 0.13, 1.0);
  cairo_set_line_width(cr, 0.3);

  cairo_move_to(cr, 0.5, 0.18);
  cairo_line_to(cr, 0.5, 0.82);
  cairo_move_to(cr, 0.18, 0.5);
  cairo_line_to(cr, 0.82, 0.5);
  cairo_stroke(cr);

  POSTAMBLE;
}

static void _retouch_cairo_paint_tool_fill(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                           const gint flags, void *data)
{
  PREAMBLE;

  cairo_move_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.2, 0.1);
  cairo_line_to(cr, 0.2, 0.9);
  cairo_line_to(cr, 0.8, 0.9);
  cairo_line_to(cr, 0.8, 0.1);
  cairo_line_to(cr, 0.9, 0.1);
  cairo_stroke(cr);
  cairo_rectangle(cr, 0.2, 0.4, .6, .5);
  cairo_fill(cr);
  cairo_stroke(cr);

  POSTAMBLE;
}

static void _retouch_cairo_paint_tool_blur(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE;

  cairo_pattern_t *pat = NULL;
  pat = cairo_pattern_create_radial(.5, .5, 0.005, .5, .5, .5);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 1, 1, 1, 1);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 1, 1, 1, 0.1);
  cairo_set_source(cr, pat);

  cairo_set_line_width(cr, 0.125);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_arc(cr, 0.5, 0.5, 0.45, 0, 2 * M_PI);
  cairo_fill(cr);

  cairo_pattern_destroy(pat);

  POSTAMBLE;
}

static void _retouch_cairo_paint_paste_forms(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                             const gint flags, void *data)
{
  PREAMBLE;

  if(flags & CPF_ACTIVE)
  {
    cairo_set_source_rgba(cr, .75, 0.75, 0.75, 1.0);
    cairo_arc(cr, 0.5, 0.5, 0.40, 0, 2 * M_PI);
    cairo_fill(cr);
  }
  else
  {
    cairo_move_to(cr, 0.1, 0.5);
    cairo_line_to(cr, 0.9, 0.5);
    cairo_line_to(cr, 0.5, 0.9);
    cairo_line_to(cr, 0.1, 0.5);
    cairo_stroke(cr);
    cairo_move_to(cr, 0.1, 0.5);
    cairo_line_to(cr, 0.9, 0.5);
    cairo_line_to(cr, 0.5, 0.9);
    cairo_line_to(cr, 0.1, 0.5);
    cairo_fill(cr);

    cairo_move_to(cr, 0.4, 0.1);
    cairo_line_to(cr, 0.6, 0.1);
    cairo_line_to(cr, 0.6, 0.5);
    cairo_line_to(cr, 0.4, 0.5);
    cairo_stroke(cr);
    cairo_move_to(cr, 0.4, 0.1);
    cairo_line_to(cr, 0.6, 0.1);
    cairo_line_to(cr, 0.6, 0.5);
    cairo_line_to(cr, 0.4, 0.5);
    cairo_fill(cr);
  }

  POSTAMBLE;
}

static void _retouch_cairo_paint_cut_forms(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                           const gint flags, void *data)
{
  PREAMBLE;

  if(flags & CPF_ACTIVE)
  {
    cairo_move_to(cr, 0.11, 0.25);
    cairo_line_to(cr, 0.89, 0.75);
    cairo_move_to(cr, 0.25, 0.11);
    cairo_line_to(cr, 0.75, 0.89);
    cairo_stroke(cr);

    cairo_arc(cr, 0.89, 0.53, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_arc(cr, 0.53, 0.89, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);
  }
  else
  {
    cairo_move_to(cr, 0.01, 0.35);
    cairo_line_to(cr, 0.99, 0.65);
    cairo_move_to(cr, 0.35, 0.01);
    cairo_line_to(cr, 0.65, 0.99);
    cairo_stroke(cr);

    cairo_arc(cr, 0.89, 0.53, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_arc(cr, 0.53, 0.89, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);
  }

  POSTAMBLE;
}

static void _retouch_cairo_paint_display_wavelet_scale(cairo_t *cr, const gint x, const gint y, const gint w,
                                                       const gint h, const gint flags, void *data)
{
  PREAMBLE;

  if(flags & CPF_ACTIVE)
  {
    float x1 = 0.2f;
    float y1 = 1.f;

    cairo_move_to(cr, x1, y1);

    const int steps = 4;
    const float delta = 1. / (float)steps;
    for(int i = 0; i < steps; i++)
    {
      y1 -= delta;
      cairo_line_to(cr, x1, y1);
      x1 += delta;
      if(x1 > .9) x1 = .9;
      cairo_line_to(cr, x1, y1);
    }
    cairo_stroke(cr);

    cairo_set_line_width(cr, 0.1);
    cairo_rectangle(cr, 0., 0., 1., 1.);
    cairo_stroke(cr);
  }
  else
  {
    cairo_move_to(cr, 0.08, 1.);
    cairo_curve_to(cr, 0.4, 0.05, 0.6, 0.05, 1., 1.);
    cairo_line_to(cr, 0.08, 1.);
    cairo_fill(cr);

    cairo_set_line_width(cr, 0.1);
    cairo_rectangle(cr, 0., 0., 1., 1.);
    cairo_stroke(cr);
  }

  POSTAMBLE;
}

static void _retouch_cairo_paint_auto_levels(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                             const gint flags, void *data)
{
  PREAMBLE;

  cairo_move_to(cr, .1, 0.3);
  cairo_line_to(cr, .1, 1.);
  cairo_stroke(cr);

  cairo_move_to(cr, .5, 0.1);
  cairo_line_to(cr, .5, 1.);
  cairo_stroke(cr);

  cairo_move_to(cr, .9, 0.3);
  cairo_line_to(cr, .9, 1.);
  cairo_stroke(cr);

  cairo_move_to(cr, 0., 1.0);
  cairo_line_to(cr, 1.0, 1.0);
  cairo_stroke(cr);

  POSTAMBLE;
}

#undef PREAMBLE
#undef POSTAMBLE

//---------------------------------------------------------------------------------
// shape selection
//---------------------------------------------------------------------------------

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

static dt_iop_retouch_algo_type_t rt_get_algorithm_from_formid(dt_iop_retouch_params_t *p, const int formid)
{
  dt_iop_retouch_algo_type_t algo = 0;
  if(formid > 0)
  {
    int i = 0;

    while(algo == 0 && i < RETOUCH_NO_FORMS)
    {
      if(p->rt_forms[i].formid == formid) algo = p->rt_forms[i].algorithm;
      i++;
    }
  }
  return algo;
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
    GList *forms = g_list_first(grp->points);
    while(forms)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt->formid == formid)
      {
        form_point_group = grpt;
        break;
      }
      forms = g_list_next(forms);
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

static void rt_show_hide_controls(const dt_iop_module_t *self, const dt_iop_retouch_gui_data_t *d,
                                  dt_iop_retouch_params_t *p, dt_iop_retouch_gui_data_t *g)
{
  const int creation_continuous = (darktable.develop->form_gui && darktable.develop->form_gui->creation_continuous
                                   && darktable.develop->form_gui->creation_continuous_module == self);

  switch(p->algorithm)
  {
    case dt_iop_retouch_heal:
      gtk_widget_hide(GTK_WIDGET(d->vbox_blur));
      gtk_widget_hide(GTK_WIDGET(d->vbox_fill));
      break;
    case dt_iop_retouch_blur:
      gtk_widget_show(GTK_WIDGET(d->vbox_blur));
      gtk_widget_hide(GTK_WIDGET(d->vbox_fill));
      break;
    case dt_iop_retouch_fill:
      gtk_widget_hide(GTK_WIDGET(d->vbox_blur));
      gtk_widget_show(GTK_WIDGET(d->vbox_fill));
      if(p->fill_mode == dt_iop_retouch_fill_color)
        gtk_widget_show(GTK_WIDGET(d->hbox_color_pick));
      else
        gtk_widget_hide(GTK_WIDGET(d->hbox_color_pick));
      break;
    case dt_iop_retouch_clone:
    default:
      gtk_widget_hide(GTK_WIDGET(d->vbox_blur));
      gtk_widget_hide(GTK_WIDGET(d->vbox_fill));
      break;
  }

  if(g->display_wavelet_scale)
    gtk_widget_show(GTK_WIDGET(d->vbox_preview_scale));
  else
    gtk_widget_hide(GTK_WIDGET(d->vbox_preview_scale));

  const dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, rt_get_selected_shape_id());
  if(form && !creation_continuous)
    gtk_widget_show(GTK_WIDGET(d->sl_mask_opacity));
  else
    gtk_widget_hide(GTK_WIDGET(d->sl_mask_opacity));
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

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  int selection_changed = 0;

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    dt_bauhaus_slider_set(g->sl_mask_opacity, rt_get_shape_opacity(self, p->rt_forms[index].formid));

    if(p->rt_forms[index].algorithm == dt_iop_retouch_blur)
    {
      p->blur_type = p->rt_forms[index].blur_type;
      p->blur_radius = p->rt_forms[index].blur_radius;

      dt_bauhaus_combobox_set(g->cmb_blur_type, p->blur_type);
      dt_bauhaus_slider_set(g->sl_blur_radius, p->blur_radius);

      selection_changed = 1;
    }
    else if(p->rt_forms[index].algorithm == dt_iop_retouch_fill)
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

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_clone), (p->algorithm == dt_iop_retouch_clone));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_heal), (p->algorithm == dt_iop_retouch_heal));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_blur), (p->algorithm == dt_iop_retouch_blur));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_fill), (p->algorithm == dt_iop_retouch_fill));

      selection_changed = 1;
    }

    if(selection_changed) rt_show_hide_controls(self, g, p, g);
  }

  rt_display_selected_shapes_lbl(g);

  const int creation_continuous = (darktable.develop->form_gui && darktable.develop->form_gui->creation_continuous
                                   && darktable.develop->form_gui->creation_continuous_module == self);

  if(index >= 0 && !creation_continuous)
    gtk_widget_show(GTK_WIDGET(g->sl_mask_opacity));
  else
    gtk_widget_hide(GTK_WIDGET(g->sl_mask_opacity));

  darktable.gui->reset = reset;

  if(selection_changed) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

//---------------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------------

static void rt_masks_form_change_opacity(dt_iop_module_t *self, int formid, float opacity)
{
  if(opacity < 0.f || opacity > 1.f) return;

  dt_masks_point_group_t *grpt = rt_get_mask_point_group(self, formid);
  if(grpt)
  {
    grpt->opacity = opacity;

    dt_develop_blend_params_t *bp = self->blend_params;
    dt_masks_form_t *grp = dt_masks_get_from_id(self->dev, bp->mask_id);
    dt_masks_write_form(grp, darktable.develop);

    dt_dev_masks_list_update(darktable.develop);
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
  gtk_toggle_button_set_active(g->color_picker, FALSE);
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
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
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

  dt_masks_form_t *grp2 = dt_masks_create(DT_MASKS_GROUP);
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
    GList *forms = g_list_first(grp->points);
    int new_form_index = 0;
    while((new_form_index < RETOUCH_NO_FORMS) && forms)
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

            switch(forms_d[new_form_index].algorithm)
            {
              case dt_iop_retouch_blur:
                forms_d[new_form_index].blur_type = p->blur_type;
                forms_d[new_form_index].blur_radius = p->blur_radius;
                break;
              case dt_iop_retouch_fill:
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

      forms = g_list_next(forms);
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
                                     const float *target, const float *source, int *dx, int *dy)
{
  float points[4];
  rt_masks_point_denormalize(piece, roi, target, 1, points);
  rt_masks_point_denormalize(piece, roi, source, 1, points + 2);

  int res = dt_dev_distort_transform_plus(self->dev, piece->pipe, 0, self->priority, points, 2);
  if(!res) return res;

  *dx = points[0] - points[2];
  *dy = points[1] - points[3];

  return res;
}

static int rt_masks_get_delta(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi,
                              dt_masks_form_t *form, int *dx, int *dy)
{
  int res = 0;

  if(form->type & DT_MASKS_PATH)
  {
    const dt_masks_point_path_t *pt = (dt_masks_point_path_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->corner, form->source, dx, dy);
  }
  else if(form->type & DT_MASKS_CIRCLE)
  {
    const dt_masks_point_circle_t *pt = (dt_masks_point_circle_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->center, form->source, dx, dy);
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    const dt_masks_point_ellipse_t *pt = (dt_masks_point_ellipse_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->center, form->source, dx, dy);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    const dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)form->points->data;

    res = rt_masks_point_calc_delta(self, piece, roi, pt->corner, form->source, dx, dy);
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

static int rt_shape_is_beign_added(dt_iop_module_t *self, const int shape_type)
{
  int being_added = 0;

  if(self->dev->form_gui && self->dev->form_visible
     && ((self->dev->form_gui->creation && self->dev->form_gui->creation_module == self)
         || (self->dev->form_gui->creation_continuous && self->dev->form_gui->creation_continuous_module == self)))
  {
    if(self->dev->form_visible->type & DT_MASKS_GROUP)
    {
      GList *forms = g_list_first(self->dev->form_visible->points);
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
    if(p->algorithm == dt_iop_retouch_clone || p->algorithm == dt_iop_retouch_heal)
      spot = dt_masks_create(type | DT_MASKS_CLONE);
    else
      spot = dt_masks_create(type | DT_MASKS_NON_CLONE);

    dt_masks_change_form_gui(spot);
    darktable.develop->form_gui->creation = TRUE;
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

static void rt_request_pick_toggled_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  self->request_color_pick
      = (gtk_toggle_button_get_active(togglebutton) ? DT_REQUEST_COLORPICK_MODULE : DT_REQUEST_COLORPICK_OFF);

  // set the area sample size
  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
    dt_dev_reprocess_all(self->dev);
  }
  else
  {
    dt_control_queue_redraw();
  }

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}

static void rt_colorpick_color_set_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  // turn off the other color picker
  gtk_toggle_button_set_active(g->color_picker, FALSE);

  GdkRGBA c
      = (GdkRGBA){.red = p->fill_color[0], .green = p->fill_color[1], .blue = p->fill_color[2], .alpha = 1.0 };
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->fill_color[0] = c.red;
  p->fill_color[1] = c.green;
  p->fill_color[2] = c.blue;

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == dt_iop_retouch_fill)
    {
      p->rt_forms[index].fill_color[0] = p->fill_color[0];
      p->rt_forms[index].fill_color[1] = p->fill_color[1];
      p->rt_forms[index].fill_color[2] = p->fill_color[2];
    }
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// wavelet decompose bar

#define RT_WDBAR_INSET DT_PIXEL_APPLY_DPI(5)

static inline int rt_mouse_x_to_wdbar_box(const float mouse_x, const float width)
{
  return (mouse_x / (width / (float)RETOUCH_NO_SCALES));
}

static inline float rt_get_middle_wdbar_box(const float box_number, const float width)
{
  return (width / (float)RETOUCH_NO_SCALES) * box_number + (width / (float)RETOUCH_NO_SCALES) * .5f;
}

static inline int rt_mouse_over_bottom_wdbar(const float mouse_y, const float height)
{
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f) * .5f;

  return (height - arrw < mouse_y && height + arrw > mouse_y);
}

static inline int rt_mouse_over_top_wdbar(const float mouse_y)
{
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f) * .5f;

  return (-arrw < mouse_y && arrw > mouse_y);
}

// this assumes that the mouse is over the top/bottom (use rt_mouse_over_top_wdbar()/rt_mouse_over_bottom_wdbar())
static inline int rt_mouse_over_arrow_wdbar(const float box_number, const float mouse_x, const float width)
{
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f) * .5f;
  const float middle = rt_get_middle_wdbar_box(box_number, width);

  return (mouse_x > middle - arrw && mouse_x < middle + arrw);
}

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
  gtk_widget_queue_draw(g->wd_bar);

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
  dt_pthread_mutex_lock(&g->lock);
  if(g->displayed_wavelet_scale == 0 && p->preview_levels[0] == RETOUCH_PREVIEW_LVL_MIN
     && p->preview_levels[1] == 0.f && p->preview_levels[2] == RETOUCH_PREVIEW_LVL_MAX
     && g->preview_auto_levels == 0 && p->curr_scale > 0 && p->curr_scale <= p->num_scales)
  {
    g->preview_auto_levels = 1;
    g->displayed_wavelet_scale = 1;
  }
  dt_pthread_mutex_unlock(&g->lock);

  rt_update_wd_bar_labels(p, g);
  gtk_widget_queue_draw(g->wd_bar);

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
  gtk_widget_queue_draw(g->wd_bar);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gboolean rt_wdbar_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  g->wdbar_mouse_x = -1;
  g->wdbar_mouse_y = -1;

  gtk_widget_queue_draw(g->wd_bar);

  return TRUE;
}

static gboolean rt_wdbar_button_press(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_request_focus(self);

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  const int inset = RT_WDBAR_INSET;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const float width = allocation.width - 2 * inset;
  const float height = allocation.height - 2 * inset;

  if(event->button == 1)
  {
    // bottom slider
    if(rt_mouse_over_bottom_wdbar(g->wdbar_mouse_y, height))
    {
      // is over the arrow?
      if(rt_mouse_over_arrow_wdbar((float)p->num_scales, g->wdbar_mouse_x, width))
      {
        g->is_dragging = dt_iop_retouch_wdbar_drag_bottom;
      }
      else
      {
        const int num_scales = rt_mouse_x_to_wdbar_box(g->wdbar_mouse_x, width);
        rt_num_scales_update(num_scales, self);
      }
    }
    // top slider
    else if(rt_mouse_over_top_wdbar(g->wdbar_mouse_y))
    {
      // is over the arrow?
      if(rt_mouse_over_arrow_wdbar((float)p->merge_from_scale, g->wdbar_mouse_x, width))
      {
        g->is_dragging = dt_iop_retouch_wdbar_drag_top;
      }
      else
      {
        const int merge_from_scale = rt_mouse_x_to_wdbar_box(g->wdbar_mouse_x, width);
        rt_merge_from_scale_update(merge_from_scale, self);
      }
    }
    else
    {
      const int curr_scale = rt_mouse_x_to_wdbar_box(g->wdbar_mouse_x, width);
      rt_curr_scale_update(curr_scale, self);
    }
  }

  return TRUE;
}

static gboolean rt_wdbar_button_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  if(event->button == 1)
  {
    g->is_dragging = 0;
  }
  return TRUE;
}

static gboolean rt_wdbar_scrolled(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_request_focus(self);

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
    dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

    const int inset = RT_WDBAR_INSET;

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const float height = allocation.height - 2 * inset;

    gboolean is_under_mouse = 0;

    if(!is_under_mouse)
    {
      is_under_mouse = rt_mouse_over_bottom_wdbar(g->wdbar_mouse_y, height);
      if(is_under_mouse)
      {
        const int num_scales = (p->num_scales - delta_y);
        rt_num_scales_update(num_scales, self);
      }
    }

    if(!is_under_mouse)
    {
      is_under_mouse = rt_mouse_over_top_wdbar(g->wdbar_mouse_y);
      if(is_under_mouse)
      {
        const int merge_from_scale = (p->merge_from_scale - delta_y);
        rt_merge_from_scale_update(merge_from_scale, self);
      }
    }

    if(!is_under_mouse)
    {
      is_under_mouse = 1;
      const int curr_scale = (p->curr_scale - delta_y);
      rt_curr_scale_update(curr_scale, self);
    }
  }

  return TRUE;
}

static gboolean rt_wdbar_motion_notify(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  const int inset = RT_WDBAR_INSET;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const float width = allocation.width - 2 * inset;
  const float height = allocation.height - 2 * inset;

  /* record mouse position within control */
  g->wdbar_mouse_x = CLAMP(event->x - inset, 0, width);
  g->wdbar_mouse_y = CLAMP(event->y - inset, 0, height);

  if(g->is_dragging == dt_iop_retouch_wdbar_drag_bottom)
  {
    const int num_scales = rt_mouse_x_to_wdbar_box(g->wdbar_mouse_x, width);
    rt_num_scales_update(num_scales, self);
  }

  if(g->is_dragging == dt_iop_retouch_wdbar_drag_top)
  {
    const int merge_from_scale = rt_mouse_x_to_wdbar_box(g->wdbar_mouse_x, width);
    rt_merge_from_scale_update(merge_from_scale, self);
  }

  gtk_widget_queue_draw(g->wd_bar);

  return TRUE;
}

static int rt_scale_has_shapes(dt_iop_retouch_params_t *p, const int scale)
{
  int has_shapes = 0;

  for(int i = 0; i < RETOUCH_NO_FORMS && has_shapes == 0; i++)
  {
    has_shapes = (p->rt_forms[i].formid != 0 && p->rt_forms[i].scale == scale);
  }

  return has_shapes;
}

static gboolean rt_wdbar_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  const int inset = RT_WDBAR_INSET;
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);
  const int first_scale_visible = (g->first_scale_visible > 0) ? g->first_scale_visible : RETOUCH_MAX_SCALES;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  float width = allocation.width;
  float height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // clear background
  cairo_set_source_rgb(cr, .15, .15, .15);
  cairo_paint(cr);

  // translate and scale
  width -= 2.f * inset;
  height -= 2.f * inset;
  cairo_save(cr);

  const float box_w = width / (float)RETOUCH_NO_SCALES;
  const float box_h = height;

  // render the boxes
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  for(int i = 0; i < RETOUCH_NO_SCALES; i++)
  {
    // draw box background
    cairo_rectangle(cr, box_w * i + inset, inset, box_w, box_h);

    if(i == 0)
      cairo_set_source_rgb(cr, .1, .1, .1);
    else if(i == p->num_scales + 1)
      cairo_set_source_rgb(cr, .9, .9, .9);
    else if(i >= p->merge_from_scale && i <= p->num_scales && p->merge_from_scale > 0)
      cairo_set_source_rgb(cr, .45, .45, .3);
    else if(i <= p->num_scales)
      cairo_set_source_rgb(cr, .5, .5, .5);
    else
      cairo_set_source_rgb(cr, .15, .15, .15);

    cairo_fill(cr);

    // if detail scale is visible at current zoom level inform it
    if(i >= first_scale_visible && i <= p->num_scales)
    {
      cairo_set_source_rgb(cr, .5, .5, .5);
      cairo_rectangle(cr, box_w * i + inset, 0, box_w, 1);
      cairo_fill(cr);
    }

    // draw a border
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
    cairo_set_source_rgb(cr, .0, .0, .0);
    cairo_rectangle(cr, box_w * i + inset, inset, box_w, box_h);
    cairo_stroke(cr);

    // if the scale has shapes inform it
    if(rt_scale_has_shapes(p, i))
    {
      cairo_set_source_rgb(cr, .1, .8, 0);
      cairo_rectangle(cr, box_w * i + inset, inset, box_w, 1);
      cairo_fill(cr);
    }
  }

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
  cairo_restore(cr);

  // red box for the current scale
  if(p->curr_scale >= 0 && p->curr_scale < RETOUCH_NO_SCALES)
  {
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
    cairo_set_source_rgb(cr, 0.8, 0, 0);
    cairo_rectangle(cr, box_w * p->curr_scale + inset + DT_PIXEL_APPLY_DPI(3),
                    inset + arrw * .5f + DT_PIXEL_APPLY_DPI(1), box_w - 2 * DT_PIXEL_APPLY_DPI(3),
                    box_h - arrw - 2 * DT_PIXEL_APPLY_DPI(1));
    cairo_stroke(cr);
  }

  // if mouse is over a box highlight it
  if(g->wdbar_mouse_y > arrw * .5f && g->wdbar_mouse_y < height - arrw * .5f)
  {
    const int curr_scale = rt_mouse_x_to_wdbar_box(g->wdbar_mouse_x, width);
    if(curr_scale >= 0 && curr_scale < RETOUCH_NO_SCALES)
    {
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
      if(curr_scale == p->num_scales + 1)
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
      else
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
      cairo_rectangle(cr, box_w * curr_scale + inset + DT_PIXEL_APPLY_DPI(1), inset + DT_PIXEL_APPLY_DPI(1),
                      box_w - 2 * DT_PIXEL_APPLY_DPI(1), box_h - 2 * DT_PIXEL_APPLY_DPI(1));
      cairo_stroke(cr);
    }
  }

  /* render control points handles */
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  // draw number of scales arrow (bottom arrow)
  {
    const float middle = rt_get_middle_wdbar_box((float)p->num_scales, width);
    gboolean is_under_mouse = rt_mouse_over_arrow_wdbar((float)p->num_scales, g->wdbar_mouse_x, width);
    is_under_mouse &= rt_mouse_over_bottom_wdbar(g->wdbar_mouse_y, height);

    cairo_move_to(cr, inset + middle, box_h + (2 * inset) - 1);
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_close_path(cr);

    if(is_under_mouse || g->is_dragging == dt_iop_retouch_wdbar_drag_bottom)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  // draw merge scales arrow (top arrow)
  {
    const float middle = rt_get_middle_wdbar_box((float)p->merge_from_scale, width);
    gboolean is_under_mouse = rt_mouse_over_arrow_wdbar((float)p->merge_from_scale, g->wdbar_mouse_x, width);
    is_under_mouse &= rt_mouse_over_top_wdbar(g->wdbar_mouse_y);

    cairo_move_to(cr, inset + middle, 1);
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_close_path(cr);

    if(is_under_mouse || g->is_dragging == dt_iop_retouch_wdbar_drag_top)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  /* push mem surface into widget */
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

// preview levels bar

#define RT_LVLBAR_INSET DT_PIXEL_APPLY_DPI(5)

static float rt_mouse_x_to_levels(const float mouse_x, const float width)
{
  return (mouse_x * ((RETOUCH_PREVIEW_LVL_MAX - RETOUCH_PREVIEW_LVL_MIN) / width)) + RETOUCH_PREVIEW_LVL_MIN;
}

static float rt_levels_to_mouse_x(const float levels, const float width)
{
  return ((levels - RETOUCH_PREVIEW_LVL_MIN) * (width / (RETOUCH_PREVIEW_LVL_MAX - RETOUCH_PREVIEW_LVL_MIN)));
}

static int rt_mouse_x_is_over_levels(const float mouse_x, const float levels, const float width)
{
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f) * .5f;
  const float middle = rt_levels_to_mouse_x(levels, width);

  return (mouse_x > middle - arrw && mouse_x < middle + arrw);
}

static int rt_mouse_x_to_levels_index(const float mouse_x, const float levels[3], const float width)
{
  int levels_index = -1;

  const float mouse_x_left = rt_levels_to_mouse_x(levels[0], width);
  const float mouse_x_middle = rt_levels_to_mouse_x(levels[1], width);
  const float mouse_x_right = rt_levels_to_mouse_x(levels[2], width);

  if(mouse_x <= mouse_x_left + (mouse_x_middle - mouse_x_left) / 2.f)
    levels_index = 0;
  else if(mouse_x <= mouse_x_middle + (mouse_x_right - mouse_x_middle) / 2.f)
    levels_index = 1;
  else
    levels_index = 2;

  return levels_index;
}

static void rt_preview_levels_update(const float levels[3], dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  float levels_old[3] = { p->preview_levels[0], p->preview_levels[1], p->preview_levels[2] };

  p->preview_levels[0] = levels[0];
  p->preview_levels[1] = levels[1];
  p->preview_levels[2] = levels[2];

  rt_clamp_minmax(levels_old, p->preview_levels);

  gtk_widget_queue_draw(g->preview_levels_bar);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gboolean rt_levelsbar_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  g->lvlbar_mouse_x = -1;
  g->lvlbar_mouse_y = -1;

  gtk_widget_queue_draw(g->preview_levels_bar);

  return TRUE;
}

static gboolean rt_levelsbar_button_press(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_request_focus(self);

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  const int inset = RT_LVLBAR_INSET;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const float width = allocation.width - 2 * inset;

  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset values
    const float levels[3] = { RETOUCH_PREVIEW_LVL_MIN, 0.f, RETOUCH_PREVIEW_LVL_MAX };
    rt_preview_levels_update(levels, self);
  }
  else if(event->button == 1)
  {
    // left slider
    if(rt_mouse_x_is_over_levels(g->lvlbar_mouse_x, p->preview_levels[0], width))
    {
      g->is_dragging = dt_iop_retouch_lvlbar_drag_left;
    }
    // middle slider
    else if(rt_mouse_x_is_over_levels(g->lvlbar_mouse_x, p->preview_levels[1], width))
    {
      g->is_dragging = dt_iop_retouch_lvlbar_drag_middle;
    }
    // right slider
    else if(rt_mouse_x_is_over_levels(g->lvlbar_mouse_x, p->preview_levels[2], width))
    {
      g->is_dragging = dt_iop_retouch_lvlbar_drag_right;
    }
    else
    {
      const int lvl_idx = rt_mouse_x_to_levels_index(g->lvlbar_mouse_x, p->preview_levels, width);
      if(lvl_idx >= 0)
      {
        float levels[3] = { p->preview_levels[0], p->preview_levels[1], p->preview_levels[2] };
        levels[lvl_idx] = rt_mouse_x_to_levels(g->lvlbar_mouse_x, width);
        rt_preview_levels_update(levels, self);
      }
    }
  }

  return TRUE;
}

static gboolean rt_levelsbar_button_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  if(event->button == 1)
  {
    g->is_dragging = 0;
  }
  return TRUE;
}

static gboolean rt_levelsbar_scrolled(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  dt_iop_request_focus(self);

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
    dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

    const int inset = RT_LVLBAR_INSET;

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const float width = allocation.width - 2 * inset;

    const int lvl_idx = rt_mouse_x_to_levels_index(g->lvlbar_mouse_x, p->preview_levels, width);
    if(lvl_idx >= 0)
    {
      float levels[3] = { p->preview_levels[0], p->preview_levels[1], p->preview_levels[2] };
      levels[lvl_idx]
          = CLAMP(levels[lvl_idx] - (0.05 * delta_y), RETOUCH_PREVIEW_LVL_MIN, RETOUCH_PREVIEW_LVL_MAX);
      rt_preview_levels_update(levels, self);
    }
  }

  return TRUE;
}

static gboolean rt_levelsbar_motion_notify(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  const int inset = RT_LVLBAR_INSET;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const float width = allocation.width - 2 * inset;
  const float height = allocation.height - 2 * inset;

  /* record mouse position within control */
  g->lvlbar_mouse_x = CLAMP(event->x - inset, 0, width);
  g->lvlbar_mouse_y = CLAMP(event->y - inset, 0, height);

  float levels[3] = { p->preview_levels[0], p->preview_levels[1], p->preview_levels[2] };

  if(g->is_dragging == dt_iop_retouch_lvlbar_drag_left)
  {
    levels[0] = rt_mouse_x_to_levels(g->lvlbar_mouse_x, width);
    rt_preview_levels_update(levels, self);
  }
  else if(g->is_dragging == dt_iop_retouch_lvlbar_drag_middle)
  {
    levels[1] = rt_mouse_x_to_levels(g->lvlbar_mouse_x, width);
    rt_preview_levels_update(levels, self);
  }
  else if(g->is_dragging == dt_iop_retouch_lvlbar_drag_right)
  {
    levels[2] = rt_mouse_x_to_levels(g->lvlbar_mouse_x, width);
    rt_preview_levels_update(levels, self);
  }

  gtk_widget_queue_draw(g->preview_levels_bar);

  return TRUE;
}

static gboolean rt_levelsbar_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  const int inset = RT_LVLBAR_INSET;
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);

  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &color);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  float width = allocation.width;
  float height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // translate and scale
  width -= 2.f * inset;
  height -= 2.f * inset;
  cairo_save(cr);

  // draw backgrownd
  cairo_pattern_t *gradient = NULL;
  gradient = cairo_pattern_create_linear(0, 0, width, height);
  if(gradient != NULL)
  {
    cairo_pattern_add_color_stop_rgb(gradient, 0, 0., 0., 0.);
    cairo_pattern_add_color_stop_rgb(gradient, 1, .5, .5, .5);

    cairo_set_line_width(cr, 0.1);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source(cr, gradient);
    cairo_rectangle(cr, inset, inset - DT_PIXEL_APPLY_DPI(2), width, height + 2. * DT_PIXEL_APPLY_DPI(2));
    cairo_fill(cr);
    cairo_stroke(cr);
    cairo_pattern_destroy(gradient);
  }

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
  cairo_restore(cr);

  /* render control points handles */
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, 1.);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  // draw arrows
  for(int i = 0; i < 3; i++)
  {
    const float levels_value = p->preview_levels[i];
    const float middle = rt_levels_to_mouse_x(levels_value, width);
    const gboolean is_under_mouse
        = g->lvlbar_mouse_x >= 0.f && rt_mouse_x_to_levels_index(g->lvlbar_mouse_x, p->preview_levels, width) == i;
    const int is_dragging = (g->is_dragging == dt_iop_retouch_lvlbar_drag_left && i == 0)
                            || (g->is_dragging == dt_iop_retouch_lvlbar_drag_middle && i == 1)
                            || (g->is_dragging == dt_iop_retouch_lvlbar_drag_right && i == 2);

    cairo_move_to(cr, inset + middle, height + (2 * inset) - 1);
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_close_path(cr);

    if(is_under_mouse || is_dragging)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  /* push mem surface into widget */
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean rt_draw_callback(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  if(self->picked_output_color_max[0] < 0) return FALSE;
  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF) return FALSE;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  // interrupt if no valid color reading
  if(self->picked_output_color_min[0] == INFINITY) return FALSE;

  if(fabsf(p->fill_color[0] - self->picked_output_color[0]) < 0.0001f
     && fabsf(p->fill_color[1] - self->picked_output_color[1]) < 0.0001f
     && fabsf(p->fill_color[2] - self->picked_output_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return FALSE;
  }

  p->fill_color[0] = self->picked_output_color[0];
  p->fill_color[1] = self->picked_output_color[1];
  p->fill_color[2] = self->picked_output_color[2];

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == dt_iop_retouch_fill)
    {
      p->rt_forms[index].fill_color[0] = p->fill_color[0];
      p->rt_forms[index].fill_color[1] = p->fill_color[1];
      p->rt_forms[index].fill_color[2] = p->fill_color[2];
    }
  }

  rt_display_selected_fill_color(g, p);

  dt_dev_add_history_item(darktable.develop, self, TRUE);

  return FALSE;
}

static void rt_copypaste_scale_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  int scale_copied = 0;
  const int active = gtk_toggle_button_get_active(togglebutton);
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
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_paste_scale), g->copied_scale < 0);

  darktable.gui->reset = reset;

  if(scale_copied) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rt_display_wavelet_scale_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display wavelet scales
  if(self->request_mask_display && !g->mask_display)
  {
    dt_control_log(_("cannot display scales when the blending mask is displayed"));

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    gtk_toggle_button_set_active(togglebutton, FALSE);
    darktable.gui->reset = reset;
    return;
  }

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);

  g->display_wavelet_scale = gtk_toggle_button_get_active(togglebutton);
  self->bypass_blendif = (g->mask_display || g->display_wavelet_scale);

  rt_show_hide_controls(self, g, p, g);

  // compute auto levels only the first time display wavelet scale is used,
  // only if levels values are the default
  // and a detail scale is displayed
  dt_pthread_mutex_lock(&g->lock);
  if(g->displayed_wavelet_scale == 0 && p->preview_levels[0] == RETOUCH_PREVIEW_LVL_MIN
     && p->preview_levels[1] == 0.f && p->preview_levels[2] == RETOUCH_PREVIEW_LVL_MAX
     && g->preview_auto_levels == 0 && p->curr_scale > 0 && p->curr_scale <= p->num_scales)
  {
    g->preview_auto_levels = 1;
    g->displayed_wavelet_scale = 1;
  }
  dt_pthread_mutex_unlock(&g->lock);

  dt_dev_reprocess_all(self->dev);
}

static void rt_develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  // FIXME: this doesn't seems the right place to update params and GUI ...
  // update auto levels
  dt_pthread_mutex_lock(&g->lock);
  if(g->preview_auto_levels == 2)
  {
    g->preview_auto_levels = -1;

    dt_pthread_mutex_unlock(&g->lock);

    for(int i = 0; i < 3; i++) p->preview_levels[i] = g->preview_levels[i];

    dt_dev_add_history_item(darktable.develop, self, TRUE);

    dt_pthread_mutex_lock(&g->lock);

    g->preview_auto_levels = 0;

    dt_pthread_mutex_unlock(&g->lock);

    gtk_widget_queue_draw(GTK_WIDGET(g->preview_levels_bar));
  }
  else
  {
    dt_pthread_mutex_unlock(&g->lock);
  }

  // just in case zoom level has changed
  gtk_widget_queue_draw(GTK_WIDGET(g->wd_bar));
}

static void rt_auto_levels_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);

  dt_pthread_mutex_lock(&g->lock);
  if(g->preview_auto_levels == 0)
  {
    g->preview_auto_levels = 1;
  }
  dt_pthread_mutex_unlock(&g->lock);

  gtk_toggle_button_set_active(togglebutton, FALSE);

  dt_dev_reprocess_all(self->dev);
}

static void rt_mask_opacity_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  if(rt_get_selected_shape_id() > 0)
  {
    float opacity = dt_bauhaus_slider_get(slider);
    rt_masks_form_change_opacity(self, rt_get_selected_shape_id(), opacity);
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

  if(rt_get_selected_shape_id() > 0)
  {
    darktable.gui->reset = 1;
    dt_bauhaus_slider_set(g->sl_mask_opacity, rt_masks_form_get_opacity(self, rt_get_selected_shape_id()));
    darktable.gui->reset = 0;
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

  // if a shape is beign created do not display masks
  if(darktable.develop->form_gui != NULL && darktable.develop->form_gui->creation)
  {
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);

    darktable.gui->reset = reset;

    return TRUE;
  }

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(event->button == 1)
  {
    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    gtk_toggle_button_set_active(g->color_picker, FALSE);

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, self->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && g_list_length(grp->points) > 0)
    {
      const int control_button_pressed = event->state & GDK_CONTROL_MASK;

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

    darktable.gui->reset = reset;

    return TRUE;
  }

  return FALSE;
}

static gboolean rt_add_shape_callback(GtkWidget *widget, GdkEventButton *e, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  const int creation_continuous = ((e->state & modifiers) == GDK_CONTROL_MASK);

  return rt_add_shape(widget, creation_continuous, self);
}

static void rt_select_algorithm_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  if(togglebutton == (GtkToggleButton *)g->bt_blur)
    p->algorithm = dt_iop_retouch_blur;
  else if(togglebutton == (GtkToggleButton *)g->bt_clone)
    p->algorithm = dt_iop_retouch_clone;
  else if(togglebutton == (GtkToggleButton *)g->bt_heal)
    p->algorithm = dt_iop_retouch_heal;
  else if(togglebutton == (GtkToggleButton *)g->bt_fill)
    p->algorithm = dt_iop_retouch_fill;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_clone), (p->algorithm == dt_iop_retouch_clone));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_heal), (p->algorithm == dt_iop_retouch_heal));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_blur), (p->algorithm == dt_iop_retouch_blur));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_fill), (p->algorithm == dt_iop_retouch_fill));

  rt_show_hide_controls(self, g, p, g);

  if(darktable.develop->form_gui->creation && (darktable.develop->form_gui->creation_module == self))
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
    if(p->algorithm == dt_iop_retouch_clone || p->algorithm == dt_iop_retouch_heal)
      spot = dt_masks_create(type | DT_MASKS_CLONE);
    else
      spot = dt_masks_create(type | DT_MASKS_NON_CLONE);
    dt_masks_change_form_gui(spot);

    darktable.develop->form_gui->creation = TRUE;
    darktable.develop->form_gui->creation_module = self;
    dt_control_queue_redraw_center();
  }

  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rt_showmask_callback(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  // if blend module is displaying mask do not display it here
  if(module->request_mask_display && !g->mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    gtk_toggle_button_set_active(togglebutton, FALSE);
    darktable.gui->reset = reset;
    return;
  }

  g->mask_display = gtk_toggle_button_get_active(togglebutton);
  module->bypass_blendif = (g->mask_display || g->display_wavelet_scale);

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);

  dt_dev_reprocess_all(module->dev);
}

static void rt_suppress_callback(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;
  g->suppress_mask = gtk_toggle_button_get_active(togglebutton);

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);

  dt_dev_reprocess_all(module->dev);
}

static void rt_blur_type_callback(GtkComboBox *combo, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  p->blur_type = dt_bauhaus_combobox_get((GtkWidget *)combo);

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == dt_iop_retouch_blur)
    {
      p->rt_forms[index].blur_type = p->blur_type;
    }
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rt_blur_radius_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  p->blur_radius = dt_bauhaus_slider_get(slider);

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == dt_iop_retouch_blur)
    {
      p->rt_forms[index].blur_radius = p->blur_radius;
    }
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rt_fill_mode_callback(GtkComboBox *combo, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  p->fill_mode = dt_bauhaus_combobox_get((GtkWidget *)combo);

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == dt_iop_retouch_fill)
    {
      p->rt_forms[index].fill_mode = p->fill_mode;
    }
  }

  rt_show_hide_controls(self, g, p, g);

  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rt_fill_brightness_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  p->fill_brightness = dt_bauhaus_slider_get(slider);

  const int index = rt_get_selected_shape_index(p);
  if(index >= 0)
  {
    if(p->rt_forms[index].algorithm == dt_iop_retouch_fill)
    {
      p->rt_forms[index].fill_brightness = p->fill_brightness;
    }
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

//--------------------------------------------------------------------------------------------------
// GUI
//--------------------------------------------------------------------------------------------------

void masks_selection_changed(struct dt_iop_module_t *self, const int form_selected_id)
{
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  if(!g) return;

  dt_pthread_mutex_lock(&g->lock);

  rt_shape_selection_changed(self);

  dt_pthread_mutex_unlock(&g->lock);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_retouch_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_retouch_params_t));
  // our module is disabled by default
  module->default_enabled = 0;
  module->priority = 185; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_retouch_params_t);
  module->gui_data = NULL;

  // init defaults:
  dt_iop_retouch_params_t tmp;
  memset(&tmp, 0, sizeof(tmp));

  tmp.algorithm = dt_iop_retouch_heal;
  tmp.num_scales = 0;
  tmp.curr_scale = 0;
  tmp.merge_from_scale = 0;

  tmp.preview_levels[0] = RETOUCH_PREVIEW_LVL_MIN;
  tmp.preview_levels[1] = 0.f;
  tmp.preview_levels[2] = RETOUCH_PREVIEW_LVL_MAX;

  tmp.blur_type = dt_iop_retouch_blur_gaussian;
  tmp.blur_radius = 10.0f;

  tmp.fill_mode = dt_iop_retouch_fill_erase;
  tmp.fill_color[0] = tmp.fill_color[1] = tmp.fill_color[2] = 0.f;
  tmp.fill_brightness = 0.f;

  memcpy(module->params, &tmp, sizeof(dt_iop_retouch_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_retouch_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
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
      if(bd)
      {
        // got focus, show all shapes
        if(bd->masks_shown == DT_MASKS_EDIT_OFF) dt_masks_set_edit_mode(self, DT_MASKS_EDIT_FULL);

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

    // if we are switching between display modes we have to reprocess all pipes
    if(g->display_wavelet_scale || g->mask_display || g->suppress_mask) dt_dev_reprocess_all(self->dev);
  }
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_retouch_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_retouch_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
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
  gchar *str = g_strdup_printf("%d", nb);
  gtk_label_set_text(g->label_form, str);
  g_free(str);

  // update wavelet decompose labels
  rt_update_wd_bar_labels(p, g);

  // update selected shape label
  rt_display_selected_shapes_lbl(g);

  // show the shapes for the current scale
  rt_show_forms_for_current_scale(self);

  // enable/disable algorithm toolbar
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_clone), p->algorithm == dt_iop_retouch_clone);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_blur), p->algorithm == dt_iop_retouch_blur);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_heal), p->algorithm == dt_iop_retouch_heal);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_fill), p->algorithm == dt_iop_retouch_fill);

  // enable/disable shapes toolbar
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), rt_shape_is_beign_added(self, DT_MASKS_CIRCLE));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), rt_shape_is_beign_added(self, DT_MASKS_PATH));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), rt_shape_is_beign_added(self, DT_MASKS_ELLIPSE));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), rt_shape_is_beign_added(self, DT_MASKS_BRUSH));

  // update the rest of the fields
  gtk_widget_queue_draw(GTK_WIDGET(g->wd_bar));
  gtk_widget_queue_draw(GTK_WIDGET(g->preview_levels_bar));

  dt_bauhaus_combobox_set(g->cmb_blur_type, p->blur_type);
  dt_bauhaus_slider_set(g->sl_blur_radius, p->blur_radius);
  dt_bauhaus_slider_set(g->sl_fill_brightness, p->fill_brightness);
  dt_bauhaus_combobox_set(g->cmb_fill_mode, p->fill_mode);

  rt_display_selected_fill_color(g, p);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_display_wavelet_scale), g->display_wavelet_scale);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_copy_scale), g->copied_scale >= 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_paste_scale), g->copied_scale < 0);

  // show/hide some fields
  rt_show_hide_controls(self, g, p, g);

  // update edit shapes status
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;
  if(bd)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks),
                                 (bd->masks_shown != DT_MASKS_EDIT_OFF) && (darktable.develop->gui_module == self));
  }
  else
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks), FALSE);
  }
}

void gui_init(dt_iop_module_t *self)
{
  const int bs = DT_PIXEL_APPLY_DPI(14);

  self->gui_data = malloc(sizeof(dt_iop_retouch_gui_data_t));
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)self->params;

  dt_pthread_mutex_init(&g->lock, NULL);
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
  g->wdbar_mouse_x = -1;
  g->wdbar_mouse_y = -1;
  g->lvlbar_mouse_x = -1;
  g->lvlbar_mouse_y = -1;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  // shapes toolbar
  GtkWidget *hbox_shapes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  GtkWidget *label = gtk_label_new(_("# shapes:"));
  gtk_box_pack_start(GTK_BOX(hbox_shapes), label, FALSE, TRUE, 0);
  g->label_form = GTK_LABEL(gtk_label_new("-1"));
  g_object_set(G_OBJECT(hbox_shapes), "tooltip-text",
               _("to add a shape select an algorithm and a shape type and click on the image.\n"
                 "shapes are added to the current scale"),
               (char *)NULL);

  g->bt_edit_masks
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_eye, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_signal_connect(G_OBJECT(g->bt_edit_masks), "button-press-event", G_CALLBACK(rt_edit_masks_callback), self);
  g_object_set(G_OBJECT(g->bt_edit_masks), "tooltip-text", _("show and edit shapes on the current scale"),
               (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_edit_masks), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_edit_masks), bs, bs);
  gtk_box_pack_end(GTK_BOX(hbox_shapes), g->bt_edit_masks, FALSE, FALSE, 0);

  g->bt_path = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_path, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_signal_connect(G_OBJECT(g->bt_path), "button-press-event", G_CALLBACK(rt_add_shape_callback), self);
  g_object_set(G_OBJECT(g->bt_path), "tooltip-text", _("add path"), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_path), bs, bs);
  gtk_box_pack_end(GTK_BOX(hbox_shapes), g->bt_path, FALSE, FALSE, 0);

  g->bt_ellipse
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_ellipse, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_signal_connect(G_OBJECT(g->bt_ellipse), "button-press-event", G_CALLBACK(rt_add_shape_callback), self);
  g_object_set(G_OBJECT(g->bt_ellipse), "tooltip-text", _("add ellipse"), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_ellipse), bs, bs);
  gtk_box_pack_end(GTK_BOX(hbox_shapes), g->bt_ellipse, FALSE, FALSE, 0);

  g->bt_circle
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_circle, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_signal_connect(G_OBJECT(g->bt_circle), "button-press-event", G_CALLBACK(rt_add_shape_callback), self);
  g_object_set(G_OBJECT(g->bt_circle), "tooltip-text", _("add circle"), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_circle), bs, bs);
  gtk_box_pack_end(GTK_BOX(hbox_shapes), g->bt_circle, FALSE, FALSE, 0);

  g->bt_brush
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_brush, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_signal_connect(G_OBJECT(g->bt_brush), "button-press-event", G_CALLBACK(rt_add_shape_callback), self);
  g_object_set(G_OBJECT(g->bt_brush), "tooltip-text", _("add brush"), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_brush), bs, bs);
  gtk_box_pack_end(GTK_BOX(hbox_shapes), g->bt_brush, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(hbox_shapes), GTK_WIDGET(g->label_form), FALSE, TRUE, 0);

  // algorithm toolbar
  GtkWidget *hbox_algo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  GtkWidget *label2 = gtk_label_new(_("algorithms:"));
  gtk_box_pack_start(GTK_BOX(hbox_algo), label2, FALSE, TRUE, 0);

  g->bt_fill
      = dtgtk_togglebutton_new(_retouch_cairo_paint_tool_fill, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_fill), "tooltip-text", _("activates fill tool"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_fill), "toggled", G_CALLBACK(rt_select_algorithm_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_fill), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_fill), FALSE);

  g->bt_blur
      = dtgtk_togglebutton_new(_retouch_cairo_paint_tool_blur, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_blur), "tooltip-text", _("activates blur tool"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_blur), "toggled", G_CALLBACK(rt_select_algorithm_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_blur), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_blur), FALSE);

  g->bt_heal
      = dtgtk_togglebutton_new(_retouch_cairo_paint_tool_heal, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_heal), "tooltip-text", _("activates healing tool"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_heal), "toggled", G_CALLBACK(rt_select_algorithm_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_heal), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_heal), FALSE);

  g->bt_clone
      = dtgtk_togglebutton_new(_retouch_cairo_paint_tool_clone, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_clone), "tooltip-text", _("activates cloning tool"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_clone), "toggled", G_CALLBACK(rt_select_algorithm_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_clone), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_clone), FALSE);

  gtk_box_pack_end(GTK_BOX(hbox_algo), g->bt_fill, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(hbox_algo), g->bt_blur, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(hbox_algo), g->bt_clone, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(hbox_algo), g->bt_heal, FALSE, FALSE, 0);

  // wavelet decompose bar labels
  GtkWidget *hbox_wd_labels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  GtkWidget *lbl_num_scales = gtk_label_new(_("# scales:"));
  gtk_box_pack_start(GTK_BOX(hbox_wd_labels), GTK_WIDGET(lbl_num_scales), FALSE, FALSE, 0);

  g->lbl_num_scales = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_width_chars(g->lbl_num_scales, 2);
  gtk_box_pack_start(GTK_BOX(hbox_wd_labels), GTK_WIDGET(g->lbl_num_scales), FALSE, FALSE, 0);

  GtkWidget *lbl_curr_scale = gtk_label_new(_("current:"));
  gtk_box_pack_start(GTK_BOX(hbox_wd_labels), GTK_WIDGET(lbl_curr_scale), FALSE, FALSE, 0);

  g->lbl_curr_scale = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_width_chars(g->lbl_curr_scale, 2);
  gtk_box_pack_start(GTK_BOX(hbox_wd_labels), GTK_WIDGET(g->lbl_curr_scale), FALSE, FALSE, 0);

  GtkWidget *lbl_merge_from_scale = gtk_label_new(_("merge from:"));
  gtk_box_pack_start(GTK_BOX(hbox_wd_labels), GTK_WIDGET(lbl_merge_from_scale), FALSE, FALSE, 0);

  g->lbl_merge_from_scale = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_width_chars(g->lbl_merge_from_scale, 2);
  gtk_box_pack_start(GTK_BOX(hbox_wd_labels), GTK_WIDGET(g->lbl_merge_from_scale), FALSE, FALSE, 0);

  // wavelet decompose bar
  g->wd_bar = gtk_drawing_area_new();

  gtk_widget_set_tooltip_text(g->wd_bar, _("top slider adjusts where the merge scales start\n"
                                           "bottom slider adjusts the number of scales\n"
                                           "red box indicates the current scale\n"
                                           "green line indicates that the scale has shapes on it"));
  g_signal_connect(G_OBJECT(g->wd_bar), "draw", G_CALLBACK(rt_wdbar_draw), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "motion-notify-event", G_CALLBACK(rt_wdbar_motion_notify), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "leave-notify-event", G_CALLBACK(rt_wdbar_leave_notify), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "button-press-event", G_CALLBACK(rt_wdbar_button_press), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "button-release-event", G_CALLBACK(rt_wdbar_button_release), self);
  g_signal_connect(G_OBJECT(g->wd_bar), "scroll-event", G_CALLBACK(rt_wdbar_scrolled), self);
  gtk_widget_add_events(GTK_WIDGET(g->wd_bar), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                   | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                   | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                   | GDK_SMOOTH_SCROLL_MASK);
  gtk_widget_set_size_request(g->wd_bar, -1, DT_PIXEL_APPLY_DPI(40));

  // toolbar display current scale / cut&paste / supress&display masks
  GtkWidget *hbox_scale = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  // display & supress masks
  g->bt_showmask
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_showmask), "tooltip-text", _("display masks"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_showmask), "toggled", G_CALLBACK(rt_showmask_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_showmask), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_showmask), FALSE);

  g->bt_suppress
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye_toggle, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_suppress), "tooltip-text", _("temporarily switch off shapes"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_suppress), "toggled", G_CALLBACK(rt_suppress_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_suppress), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_suppress), FALSE);

  // display final image/current scale
  g->bt_display_wavelet_scale = dtgtk_togglebutton_new(_retouch_cairo_paint_display_wavelet_scale,
                                                       CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_display_wavelet_scale), "tooltip-text", _("display wavelet scale"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_display_wavelet_scale), "toggled", G_CALLBACK(rt_display_wavelet_scale_callback),
                   self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_display_wavelet_scale), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_display_wavelet_scale), FALSE);

  // copy/paste shapes
  g->bt_copy_scale
      = dtgtk_togglebutton_new(_retouch_cairo_paint_cut_forms, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_copy_scale), "tooltip-text", _("cut shapes from current scale"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_copy_scale), "toggled", G_CALLBACK(rt_copypaste_scale_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_copy_scale), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_copy_scale), FALSE);

  g->bt_paste_scale
      = dtgtk_togglebutton_new(_retouch_cairo_paint_paste_forms, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_paste_scale), "tooltip-text", _("paste cutted shapes to current scale"),
               (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_paste_scale), "toggled", G_CALLBACK(rt_copypaste_scale_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_paste_scale), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_paste_scale), FALSE);

  gtk_box_pack_end(GTK_BOX(hbox_scale), g->bt_showmask, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(hbox_scale), g->bt_suppress, FALSE, FALSE, 0);

  GtkWidget *lbl_scale_sep = gtk_label_new(NULL);
  gtk_label_set_width_chars(GTK_LABEL(lbl_scale_sep), 1);
  gtk_box_pack_end(GTK_BOX(hbox_scale), GTK_WIDGET(lbl_scale_sep), FALSE, FALSE, 0);

  gtk_box_pack_end(GTK_BOX(hbox_scale), g->bt_paste_scale, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(hbox_scale), g->bt_copy_scale, FALSE, FALSE, 0);

  GtkWidget *lbl_scale_sep1 = gtk_label_new(NULL);
  gtk_label_set_width_chars(GTK_LABEL(lbl_scale_sep1), 1);
  gtk_box_pack_end(GTK_BOX(hbox_scale), GTK_WIDGET(lbl_scale_sep1), FALSE, FALSE, 0);

  gtk_box_pack_end(GTK_BOX(hbox_scale), g->bt_display_wavelet_scale, FALSE, FALSE, 0);

  // preview single scale
  g->vbox_preview_scale = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  GtkWidget *lbl_psc = dt_ui_section_label_new(_("preview single scale"));
  gtk_box_pack_start(GTK_BOX(g->vbox_preview_scale), lbl_psc, FALSE, TRUE, 0);

  GtkWidget *prev_lvl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  g->preview_levels_bar = gtk_drawing_area_new();

  gtk_widget_set_tooltip_text(g->preview_levels_bar, _("adjust preview levels"));
  g_signal_connect(G_OBJECT(g->preview_levels_bar), "draw", G_CALLBACK(rt_levelsbar_draw), self);
  g_signal_connect(G_OBJECT(g->preview_levels_bar), "motion-notify-event", G_CALLBACK(rt_levelsbar_motion_notify),
                   self);
  g_signal_connect(G_OBJECT(g->preview_levels_bar), "leave-notify-event", G_CALLBACK(rt_levelsbar_leave_notify),
                   self);
  g_signal_connect(G_OBJECT(g->preview_levels_bar), "button-press-event", G_CALLBACK(rt_levelsbar_button_press),
                   self);
  g_signal_connect(G_OBJECT(g->preview_levels_bar), "button-release-event",
                   G_CALLBACK(rt_levelsbar_button_release), self);
  g_signal_connect(G_OBJECT(g->preview_levels_bar), "scroll-event", G_CALLBACK(rt_levelsbar_scrolled), self);
  gtk_widget_add_events(GTK_WIDGET(g->preview_levels_bar), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                               | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                               | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                               | GDK_SMOOTH_SCROLL_MASK);
  gtk_widget_set_size_request(g->preview_levels_bar, -1, DT_PIXEL_APPLY_DPI(5));

  g->bt_auto_levels
      = dtgtk_togglebutton_new(_retouch_cairo_paint_auto_levels, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->bt_auto_levels), "tooltip-text", _("auto levels"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->bt_auto_levels), "toggled", G_CALLBACK(rt_auto_levels_callback), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->bt_auto_levels), bs, bs);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_auto_levels), FALSE);

  gtk_box_pack_end(GTK_BOX(prev_lvl), g->bt_auto_levels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(prev_lvl), GTK_WIDGET(g->preview_levels_bar), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(g->vbox_preview_scale), prev_lvl, TRUE, TRUE, 0);

  // shapes selected (label)
  GtkWidget *hbox_shape_sel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget *label1 = gtk_label_new(_("shape selected:"));
  gtk_box_pack_start(GTK_BOX(hbox_shape_sel), label1, FALSE, TRUE, 0);
  g->label_form_selected = GTK_LABEL(gtk_label_new("-1"));
  g_object_set(G_OBJECT(hbox_shape_sel), "tooltip-text",
               _("click on a shape to select it,\nto unselect click on an empty space"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(hbox_shape_sel), GTK_WIDGET(g->label_form_selected), FALSE, TRUE, 0);

  // fill properties
  g->vbox_fill = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  g->cmb_fill_mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cmb_fill_mode, NULL, _("fill mode"));
  dt_bauhaus_combobox_add(g->cmb_fill_mode, _("erase"));
  dt_bauhaus_combobox_add(g->cmb_fill_mode, _("color"));
  g_object_set(g->cmb_fill_mode, "tooltip-text", _("erase the detail or fills with choosen color"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->cmb_fill_mode), "value-changed", G_CALLBACK(rt_fill_mode_callback), self);

  // color for fill algorithm
  GdkRGBA color
      = (GdkRGBA){.red = p->fill_color[0], .green = p->fill_color[1], .blue = p->fill_color[2], .alpha = 1.0 };

  g->hbox_color_pick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick), bs, bs);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select fill color"));
  g_object_set(G_OBJECT(g->colorpick), "tooltip-text", _("select fill color"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->colorpick), "color-set", G_CALLBACK(rt_colorpick_color_set_callback), self);

  g->color_picker = GTK_TOGGLE_BUTTON(
      dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL));
  g_object_set(G_OBJECT(g->color_picker), "tooltip-text", _("pick fill color from image"), (char *)NULL);
  gtk_widget_set_size_request(GTK_WIDGET(g->color_picker), bs, bs);
  g_signal_connect(G_OBJECT(g->color_picker), "toggled", G_CALLBACK(rt_request_pick_toggled_callback), self);

  GtkWidget *lbl_fill_color = gtk_label_new(_("fill color: "));

  g->sl_fill_brightness = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, .0005, .0, 4);
  dt_bauhaus_widget_set_label(g->sl_fill_brightness, _("brightness"), _("brightness"));
  g_object_set(g->sl_fill_brightness, "tooltip-text",
               _("adjusts color brightness to fine-tune it. works with erase as well"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->sl_fill_brightness), "value-changed", G_CALLBACK(rt_fill_brightness_callback), self);

  gtk_box_pack_end(GTK_BOX(g->hbox_color_pick), GTK_WIDGET(g->color_picker), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(g->hbox_color_pick), GTK_WIDGET(g->colorpick), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->hbox_color_pick), lbl_fill_color, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox_fill), GTK_WIDGET(g->cmb_fill_mode), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox_fill), g->hbox_color_pick, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox_fill), g->sl_fill_brightness, TRUE, TRUE, 0);

  // blur properties
  g->vbox_blur = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  g->cmb_blur_type = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cmb_blur_type, NULL, _("blur type"));
  dt_bauhaus_combobox_add(g->cmb_blur_type, _("gaussian"));
  dt_bauhaus_combobox_add(g->cmb_blur_type, _("bilateral"));
  g_object_set(g->cmb_blur_type, "tooltip-text", _("type for the blur algorithm"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->cmb_blur_type), "value-changed", G_CALLBACK(rt_blur_type_callback), self);

  gtk_box_pack_start(GTK_BOX(g->vbox_blur), g->cmb_blur_type, TRUE, TRUE, 0);

  g->sl_blur_radius = dt_bauhaus_slider_new_with_range(self, 0.1, 200.0, 0.1, 10., 2);
  dt_bauhaus_widget_set_label(g->sl_blur_radius, _("blur radius"), _("blur radius"));
  g_object_set(g->sl_blur_radius, "tooltip-text", _("radius of the selected blur type"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->sl_blur_radius), "value-changed", G_CALLBACK(rt_blur_radius_callback), self);

  gtk_box_pack_start(GTK_BOX(g->vbox_blur), g->sl_blur_radius, TRUE, TRUE, 0);

  // mask opacity
  g->sl_mask_opacity = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.05, 1., 3);
  dt_bauhaus_widget_set_label(g->sl_mask_opacity, _("mask opacity"), _("mask opacity"));
  g_object_set(g->sl_mask_opacity, "tooltip-text", _("set the opacity on the selected shape"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->sl_mask_opacity), "value-changed", G_CALLBACK(rt_mask_opacity_callback), self);

  // wavelet decompose
  GtkWidget *lbl_wd = dt_ui_section_label_new(_("wavelet decompose"));
  gtk_box_pack_start(GTK_BOX(self->widget), lbl_wd, FALSE, TRUE, 0);

  // wavelet decompose bar & labels
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_wd_labels, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->wd_bar, TRUE, TRUE, 0);

  // preview scale & cut/paste scale
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_scale, TRUE, TRUE, 0);

  // preview single scale
  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_preview_scale, TRUE, TRUE, 0);

  // add all the controls to the iop
  GtkWidget *lbl_rt_tools = dt_ui_section_label_new(_("retouch tools"));
  gtk_box_pack_start(GTK_BOX(self->widget), lbl_rt_tools, FALSE, TRUE, 0);

  // shapes toolbar
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_shapes, TRUE, TRUE, 0);
  // algorithms toolbar
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_algo, TRUE, TRUE, 0);

  // shapes
  GtkWidget *lbl_shapes = dt_ui_section_label_new(_("shapes"));
  gtk_box_pack_start(GTK_BOX(self->widget), lbl_shapes, FALSE, TRUE, 0);

  // shape selected
  gtk_box_pack_start(GTK_BOX(self->widget), hbox_shape_sel, TRUE, TRUE, 0);
  // blur radius
  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_blur, TRUE, TRUE, 0);
  // fill color
  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_fill, TRUE, TRUE, 0);
  // mask (shape) opacity
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_mask_opacity, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(rt_draw_callback), self);

  /* add signal handler for preview pipe finish to redraw the preview */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(rt_develop_ui_pipe_finished_callback), self);

  gtk_widget_show_all(g->vbox_blur);
  gtk_widget_set_no_show_all(g->vbox_blur, TRUE);

  gtk_widget_show_all(g->vbox_fill);
  gtk_widget_set_no_show_all(g->vbox_fill, TRUE);

  gtk_widget_show_all(g->vbox_preview_scale);
  gtk_widget_set_no_show_all(g->vbox_preview_scale, TRUE);

  rt_show_hide_controls(self, g, p, g);
}

void gui_reset(struct dt_iop_module_t *self)
{
  // hide the previous masks
  dt_masks_reset_form_gui();
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(rt_develop_ui_pipe_finished_callback), self);

  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;
  if(g)
  {
    dt_pthread_mutex_destroy(&g->lock);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
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
    GList *forms = g_list_first(grp->points);
    while(forms)
    {
      const dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt)
      {
        const int formid = grpt->formid;

        // we get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form)
        {
          // if the form is outside the roi, we just skip it
          if(!rt_masks_form_is_in_roi(self, piece, form, roi_in, roi_in))
          {
            forms = g_list_next(forms);
            continue;
          }

          // we get the area for the source
          int fl, ft, fw, fh;

          if(!dt_masks_get_area(self, piece, form, &fw, &fh, &fl, &ft))
          {
            forms = g_list_next(forms);
            continue;
          }
          fw *= roi_in->scale, fh *= roi_in->scale, fl *= roi_in->scale, ft *= roi_in->scale;

          // we enlarge the roi if needed
          roiy = fminf(ft, roiy);
          roix = fminf(fl, roix);
          roir = fmaxf(fl + fw, roir);
          roib = fmaxf(ft + fh, roib);

          // heal needs both source and destination areas
          const dt_iop_retouch_algo_type_t algo = rt_get_algorithm_from_formid(p, formid);
          if(algo == dt_iop_retouch_heal)
          {
            int dx = 0, dy = 0;
            if(rt_masks_get_delta(self, piece, roi_in, form, &dx, &dy))
            {
              roiy = fminf(ft + dy, roiy);
              roix = fminf(fl + dx, roix);
              roir = fmaxf(fl + fw + dx, roir);
              roib = fmaxf(ft + fh + dy, roib);
            }
          }
        }
      }

      forms = g_list_next(forms);
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
    GList *forms = g_list_first(grp->points);
    while(forms)
    {
      const dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt)
      {
        const int formid = grpt->formid;

        // just need the previous forms
        if(formid == formid_src) break;

        const dt_iop_retouch_algo_type_t algo = rt_get_algorithm_from_formid(p, formid);

        // only process clone and heal
        if(algo != dt_iop_retouch_heal && algo != dt_iop_retouch_clone)
        {
          forms = g_list_next(forms);
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
            forms = g_list_next(forms);
            continue;
          }
          fw *= roi_in->scale, fh *= roi_in->scale, fl *= roi_in->scale, ft *= roi_in->scale;

          // get the destination area
          int fl_dest, ft_dest;
          int dx = 0, dy = 0;
          if(!rt_masks_get_delta(self, piece, roi_in, form, &dx, &dy))
          {
            forms = g_list_next(forms);
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

      forms = g_list_next(forms);
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
    GList *forms = g_list_first(grp->points);
    while(forms)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt)
      {
        const int formid = grpt->formid;
        const dt_iop_retouch_algo_type_t algo = rt_get_algorithm_from_formid(p, formid);

        if(algo != dt_iop_retouch_heal && algo != dt_iop_retouch_clone)
        {
          forms = g_list_next(forms);
          continue;
        }

        // we get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form == NULL)
        {
          forms = g_list_next(forms);
          continue;
        }

        // get the source area
        int fl_src, ft_src, fw_src, fh_src;
        if(!dt_masks_get_source_area(self, piece, form, &fw_src, &fh_src, &fl_src, &ft_src))
        {
          forms = g_list_next(forms);
          continue;
        }

        fw_src *= roi_in->scale, fh_src *= roi_in->scale, fl_src *= roi_in->scale, ft_src *= roi_in->scale;

        // we only want to process froms alreay in roi_in
        const int intersects
            = !(roib < ft_src || ft_src + fh_src < roiy || roir < fl_src || fl_src + fw_src < roix);
        if(intersects)
          rt_extend_roi_in_from_source_clones(self, piece, roi_in, formid, fl_src, ft_src, fw_src, fh_src, &roir,
                                              &roib, &roix, &roiy);
      }

      forms = g_list_next(forms);
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

void init_key_accels(dt_iop_module_so_t *module)
{
  dt_accel_register_iop(module, TRUE, NC_("accel", "retouch tool circle"), 0, 0);
  dt_accel_register_iop(module, TRUE, NC_("accel", "retouch tool ellipse"), 0, 0);
  dt_accel_register_iop(module, TRUE, NC_("accel", "retouch tool path"), 0, 0);
  dt_accel_register_iop(module, TRUE, NC_("accel", "retouch tool brush"), 0, 0);

  dt_accel_register_iop(module, TRUE, NC_("accel", "continuous add circle"), 0, 0);
  dt_accel_register_iop(module, TRUE, NC_("accel", "continuous add ellipse"), 0, 0);
  dt_accel_register_iop(module, TRUE, NC_("accel", "continuous add path"), 0, 0);
  dt_accel_register_iop(module, TRUE, NC_("accel", "continuous add brush"), 0, 0);
}

static gboolean _add_circle_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_circle), FALSE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), rt_shape_is_beign_added(module, DT_MASKS_CIRCLE));

  darktable.gui->reset = reset;

  return TRUE;
}

static gboolean _add_ellipse_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                       GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_ellipse), FALSE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), rt_shape_is_beign_added(module, DT_MASKS_ELLIPSE));

  darktable.gui->reset = reset;

  return TRUE;
}

static gboolean _add_brush_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_brush), FALSE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), rt_shape_is_beign_added(module, DT_MASKS_BRUSH));

  darktable.gui->reset = reset;

  return TRUE;
}

static gboolean _add_path_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_path), FALSE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), rt_shape_is_beign_added(module, DT_MASKS_PATH));

  darktable.gui->reset = reset;

  return TRUE;
}

static gboolean _continuous_add_circle_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                                 GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_circle), TRUE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_circle), rt_shape_is_beign_added(module, DT_MASKS_CIRCLE));

  darktable.gui->reset = reset;

  return TRUE;
}

static gboolean _continuous_add_ellipse_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                                  GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_ellipse), TRUE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_ellipse), rt_shape_is_beign_added(module, DT_MASKS_ELLIPSE));

  darktable.gui->reset = reset;

  return TRUE;
}

static gboolean _continuous_add_brush_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                                GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_brush), TRUE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_brush), rt_shape_is_beign_added(module, DT_MASKS_BRUSH));

  darktable.gui->reset = reset;

  return TRUE;
}

static gboolean _continuous_add_path_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                               GdkModifierType modifier, gpointer data)
{
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  const dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)module->gui_data;

  rt_add_shape(GTK_WIDGET(g->bt_path), TRUE, module);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_path), rt_shape_is_beign_added(module, DT_MASKS_PATH));

  darktable.gui->reset = reset;

  return TRUE;
}

void connect_key_accels(dt_iop_module_t *module)
{
  GClosure *closure;

  // single add
  closure = g_cclosure_new(G_CALLBACK(_add_circle_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "retouch tool circle", closure);

  closure = g_cclosure_new(G_CALLBACK(_add_ellipse_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "retouch tool elipse", closure);

  closure = g_cclosure_new(G_CALLBACK(_add_brush_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "retouch tool brush", closure);

  closure = g_cclosure_new(G_CALLBACK(_add_path_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "retouch tool path", closure);

  // continuous add
  closure = g_cclosure_new(G_CALLBACK(_continuous_add_circle_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "continuous add circle", closure);

  closure = g_cclosure_new(G_CALLBACK(_continuous_add_ellipse_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "continuous add ellipse", closure);

  closure = g_cclosure_new(G_CALLBACK(_continuous_add_brush_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "continuous add brush", closure);

  closure = g_cclosure_new(G_CALLBACK(_continuous_add_path_key_accel), (gpointer)module, NULL);
  dt_accel_connect_iop(module, "continuous add path", closure);
}

//--------------------------------------------------------------------------------------------------
// process
//--------------------------------------------------------------------------------------------------

#ifdef __SSE2__
/** uses D50 white point. */
// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
static inline __m128 dt_XYZ_to_RGB_sse2(__m128 XYZ)
{
  // XYZ -> sRGB matrix, D65
  const __m128 xyz_to_srgb_0 = _mm_setr_ps(3.1338561f, -0.9787684f, 0.0719453f, 0.0f);
  const __m128 xyz_to_srgb_1 = _mm_setr_ps(-1.6168667f, 1.9161415f, -0.2289914f, 0.0f);
  const __m128 xyz_to_srgb_2 = _mm_setr_ps(-0.4906146f, 0.0334540f, 1.4052427f, 0.0f);

  __m128 rgb
      = _mm_add_ps(_mm_mul_ps(xyz_to_srgb_0, _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0))),
                   _mm_add_ps(_mm_mul_ps(xyz_to_srgb_1, _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1))),
                              _mm_mul_ps(xyz_to_srgb_2, _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2)))));

  return rgb;
}

static inline __m128 dt_RGB_to_XYZ_sse2(__m128 rgb)
{
  // sRGB -> XYZ matrix, D65
  const __m128 srgb_to_xyz_0 = _mm_setr_ps(0.4360747f, 0.2225045f, 0.0139322f, 0.0f);
  const __m128 srgb_to_xyz_1 = _mm_setr_ps(0.3850649f, 0.7168786f, 0.0971045f, 0.0f);
  const __m128 srgb_to_xyz_2 = _mm_setr_ps(0.1430804f, 0.0606169f, 0.7141733f, 0.0f);

  __m128 XYZ
      = _mm_add_ps(_mm_mul_ps(srgb_to_xyz_0, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0))),
                   _mm_add_ps(_mm_mul_ps(srgb_to_xyz_1, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1))),
                              _mm_mul_ps(srgb_to_xyz_2, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2)))));
  return XYZ;
}
#endif

/** uses D50 white point. */
static inline void dt_XYZ_to_RGB(const float *const XYZ, float *rgb)
{
  const float xyz_to_srgb_matrix[3][3] = { { 3.1338561, -1.6168667, -0.4906146 },
                                           { -0.9787684, 1.9161415, 0.0334540 },
                                           { 0.0719453, -0.2289914, 1.4052427 } };

  // XYZ -> sRGB
  rgb[0] = rgb[1] = rgb[2] = 0.f;
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) rgb[r] += xyz_to_srgb_matrix[r][c] * XYZ[c];
}

static inline void dt_RGB_to_XYZ(const float *const rgb, float *XYZ)
{
  const float srgb_to_xyz[3][3] = { { 0.4360747, 0.3850649, 0.1430804 },
                                    { 0.2225045, 0.7168786, 0.0606169 },
                                    { 0.0139322, 0.0971045, 0.7141733 } };

  // sRGB -> XYZ
  XYZ[0] = XYZ[1] = XYZ[2] = 0.0;
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) XYZ[r] += srgb_to_xyz[r][c] * rgb[c];
}

static void image_rgb2lab(float *img_src, const int width, const int height, const int ch, const int use_sse)
{
  const int stride = width * height * ch;

#if defined(__SSE__)
  if(ch == 4 && use_sse)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src) schedule(static)
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
#pragma omp parallel for default(none) shared(img_src) schedule(static)
#endif
  for(int i = 0; i < stride; i += ch)
  {
    float XYZ[3] = { 0 };

    dt_RGB_to_XYZ(img_src + i, XYZ);
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
#pragma omp parallel for default(none) shared(img_src) schedule(static)
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
#pragma omp parallel for default(none) shared(img_src) schedule(static)
#endif
  for(int i = 0; i < stride; i += ch)
  {
    float XYZ[3] = { 0 };

    dt_Lab_to_XYZ(img_src + i, XYZ);
    dt_XYZ_to_RGB(XYZ, img_src + i);
  }
}

static void rt_process_stats(const float *const img_src, const int width, const int height, const int ch,
                             float levels[3], int use_sse)
{
  const int size = width * height * ch;
  float l_max = -INFINITY;
  float l_min = INFINITY;
  float l_sum = 0.f;
  int count = 0;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) reduction(+ : count, l_sum) reduction(max : l_max)        \
                                                                      reduction(min : l_min)
#endif
  for(int i = 0; i < size; i += ch)
  {
    float XYZ[3] = { 0 };
    float Lab[3] = { 0 };

    dt_RGB_to_XYZ(img_src + i, XYZ);
    dt_XYZ_to_Lab(XYZ, Lab);

    l_max = MAX(l_max, Lab[0]);
    l_min = MIN(l_min, Lab[0]);
    l_sum += Lab[0];
    count++;
  }

  levels[0] = l_min / 100.f;
  levels[2] = l_max / 100.f;
  levels[1] = (l_sum / (float)count) / 100.f;
}

static void rt_adjust_levels(float *img_src, const int width, const int height, const int ch,
                             const float levels[3], int use_sse)
{
  const int size = width * height * ch;

  const float left = levels[0];
  const float middle = levels[1];
  const float right = levels[2];

  if(left == RETOUCH_PREVIEW_LVL_MIN && middle == 0.f && right == RETOUCH_PREVIEW_LVL_MAX) return;

  const float delta = (right - left) / 2.0f;
  const float mid = left + delta;
  const float tmp = (middle - mid) / delta;
  const float in_inv_gamma = pow(10, tmp);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src) schedule(static)
#endif
  for(int i = 0; i < size; i += ch)
  {
    float XYZ[3] = { 0 };

    dt_RGB_to_XYZ(img_src + i, XYZ);
    dt_XYZ_to_Lab(XYZ, img_src + i);

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

    dt_Lab_to_XYZ(img_src + i, XYZ);
    dt_XYZ_to_RGB(XYZ, img_src + i);
  }
}

#undef RT_WDBAR_INSET
#undef RT_LVLBAR_INSET

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
  const int rowsize = MIN(roi_out->width, roi_in->width) * ch * sizeof(float);
  const int xoffs = roi_out->x - roi_in->x - dx;
  const int yoffs = roi_out->y - roi_in->y - dy;
  const int y_to = MIN(roi_out->height, roi_in->height);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
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

  const int padding = (algo == dt_iop_retouch_heal) ? 1 : 0;

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

  mask_tmp = calloc(roi_mask_scaled->width * roi_mask_scaled->height, sizeof(float));
  if(mask_tmp == NULL)
  {
    fprintf(stderr, "rt_build_scaled_mask: error allocating memory\n");
    goto cleanup;
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(mask_tmp, roi_mask_scaled) schedule(static)
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
static void rt_copy_image_masked(float *const img_src, float *img_dest, dt_iop_roi_t *const roi_dest, const int ch,
                                 float *const mask_scaled, dt_iop_roi_t *const roi_mask_scaled,
                                 const float opacity, const int use_sse)
{
#if defined(__SSE__)
  if(ch == 4 && use_sse)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_dest) schedule(static)
#endif
    for(int yy = 0; yy < roi_mask_scaled->height; yy++)
    {
      const int mask_index = yy * roi_mask_scaled->width;
      const int src_index = mask_index * ch;
      const int dest_index
          = (((yy + roi_mask_scaled->y - roi_dest->y) * roi_dest->width) + (roi_mask_scaled->x - roi_dest->x))
            * ch;

      const float *s = img_src + src_index;
      const float *m = mask_scaled + mask_index;
      float *d = img_dest + dest_index;

      for(int xx = 0; xx < roi_mask_scaled->width; xx++, s += ch, d += ch, m++)
      {
        const float f = (*m) * opacity;

        const __m128 val1_f = _mm_set1_ps(1.0f - f);
        const __m128 valf = _mm_set1_ps(f);

        _mm_store_ps(d, _mm_add_ps(_mm_mul_ps(_mm_load_ps(d), val1_f), _mm_mul_ps(_mm_load_ps(s), valf)));
      }
    }
  }
  else
#endif
  {
    const int ch1 = (ch == 4) ? ch - 1 : ch;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_dest) schedule(static)
#endif
    for(int yy = 0; yy < roi_mask_scaled->height; yy++)
    {
      const int mask_index = yy * roi_mask_scaled->width;
      const int src_index = mask_index * ch;
      const int dest_index
          = (((yy + roi_mask_scaled->y - roi_dest->y) * roi_dest->width) + (roi_mask_scaled->x - roi_dest->x))
            * ch;

      const float *s = img_src + src_index;
      const float *m = mask_scaled + mask_index;
      float *d = img_dest + dest_index;

      for(int xx = 0; xx < roi_mask_scaled->width; xx++, s += ch, d += ch, m++)
      {
        const float f = (*m) * opacity;

        for(int c = 0; c < ch1; c++)
        {
          d[c] = d[c] * (1.0f - f) + s[c] * f;
        }
      }
    }
  }
}

static void rt_copy_mask_to_alpha(float *const img, dt_iop_roi_t *const roi_img, const int ch,
                                  float *const mask_scaled, dt_iop_roi_t *const roi_mask_scaled,
                                  const float opacity)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
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

#if defined(__SSE__)
static void retouch_fill_sse(float *const in, dt_iop_roi_t *const roi_in, float *const mask_scaled,
                             dt_iop_roi_t *const roi_mask_scaled, const float opacity,
                             const float *const fill_color)
{
  const int ch = 4;

  const float valf4_fill[4] = { fill_color[0], fill_color[1], fill_color[2], 0.f };
  const __m128 val_fill = _mm_load_ps(valf4_fill);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int yy = 0; yy < roi_mask_scaled->height; yy++)
  {
    const int mask_index = yy * roi_mask_scaled->width;
    const int dest_index
        = (((yy + roi_mask_scaled->y - roi_in->y) * roi_in->width) + (roi_mask_scaled->x - roi_in->x)) * ch;

    float *d = in + dest_index;
    const float *m = mask_scaled + mask_index;

    for(int xx = 0; xx < roi_mask_scaled->width; xx++, d += ch, m++)
    {
      const float f = (*m) * opacity;

      const __m128 val1_f = _mm_set1_ps(1.0f - f);
      const __m128 valf = _mm_set1_ps(f);

      _mm_store_ps(d, _mm_add_ps(_mm_mul_ps(_mm_load_ps(d), val1_f), _mm_mul_ps(val_fill, valf)));
    }
  }
}
#endif

static void retouch_fill(float *const in, dt_iop_roi_t *const roi_in, const int ch, float *const mask_scaled,
                         dt_iop_roi_t *const roi_mask_scaled, const float opacity, const float *const fill_color,
                         const int use_sse)
{
#if defined(__SSE__)
  if(ch == 4 && use_sse)
  {
    retouch_fill_sse(in, roi_in, mask_scaled, roi_mask_scaled, opacity, fill_color);
    return;
  }
#endif
  const int ch1 = (ch == 4) ? ch - 1 : ch;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int yy = 0; yy < roi_mask_scaled->height; yy++)
  {
    const int mask_index = yy * roi_mask_scaled->width;
    const int dest_index
        = (((yy + roi_mask_scaled->y - roi_in->y) * roi_in->width) + (roi_mask_scaled->x - roi_in->x)) * ch;

    float *d = in + dest_index;
    const float *m = mask_scaled + mask_index;

    for(int xx = 0; xx < roi_mask_scaled->width; xx++, d += ch, m++)
    {
      const float f = (*m) * opacity;

      for(int c = 0; c < ch1; c++) d[c] = d[c] * (1.0f - f) + fill_color[c] * f;
    }
  }
}

static void retouch_clone(float *const in, dt_iop_roi_t *const roi_in, const int ch, float *const mask_scaled,
                          dt_iop_roi_t *const roi_mask_scaled, const int dx, const int dy, const float opacity,
                          const int use_sse)
{
  // alloc temp image to avoid issues when areas self-intersects
  float *img_src = dt_alloc_align(64, roi_mask_scaled->width * roi_mask_scaled->height * ch * sizeof(float));
  if(img_src == NULL)
  {
    fprintf(stderr, "retouch_clone: error allocating memory for cloning\n");
    goto cleanup;
  }

  // copy source image to tmp
  rt_copy_in_to_out(in, roi_in, img_src, roi_mask_scaled, ch, dx, dy);

  // clone it
  rt_copy_image_masked(img_src, in, roi_in, ch, mask_scaled, roi_mask_scaled, opacity, use_sse);

cleanup:
  if(img_src) dt_free_align(img_src);
}

static void retouch_blur(float *const in, dt_iop_roi_t *const roi_in, const int ch, float *const mask_scaled,
                         dt_iop_roi_t *const roi_mask_scaled, const float opacity, const int blur_type,
                         const float blur_radius, dt_dev_pixelpipe_iop_t *piece, const int use_sse)
{
  if(fabs(blur_radius) <= 0.1f) return;

  const float sigma = blur_radius * roi_in->scale / piece->iscale;

  float *img_dest = NULL;

  // alloc temp image to blur
  img_dest = dt_alloc_align(64, roi_mask_scaled->width * roi_mask_scaled->height * ch * sizeof(float));
  if(img_dest == NULL)
  {
    fprintf(stderr, "retouch_blur: error allocating memory for blurring\n");
    goto cleanup;
  }

  // copy source image so we blur just the mask area (at least the smallest rect that covers it)
  rt_copy_in_to_out(in, roi_in, img_dest, roi_mask_scaled, ch, 0, 0);

  if(blur_type == dt_iop_retouch_blur_gaussian && fabs(blur_radius) > 0.1f)
  {
    float Labmax[] = { INFINITY, INFINITY, INFINITY, INFINITY };
    float Labmin[] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

    dt_gaussian_t *g = dt_gaussian_init(roi_mask_scaled->width, roi_mask_scaled->height, ch, Labmax, Labmin, sigma,
                                        DT_IOP_GAUSSIAN_ZERO);
    if(g)
    {
      if(ch == 4)
        dt_gaussian_blur_4c(g, img_dest, img_dest);
      else
        dt_gaussian_blur(g, img_dest, img_dest);
      dt_gaussian_free(g);
    }
  }
  else if(blur_type == dt_iop_retouch_blur_bilateral && fabs(blur_radius) > 0.1f)
  {
    const float sigma_r = 100.0f; // does not depend on scale
    const float sigma_s = sigma;
    const float detail = -1.0f; // we want the bilateral base layer

    dt_bilateral_t *b = dt_bilateral_init(roi_mask_scaled->width, roi_mask_scaled->height, sigma_s, sigma_r);
    if(b)
    {
      image_rgb2lab(img_dest, roi_mask_scaled->width, roi_mask_scaled->height, ch, use_sse);

      dt_bilateral_splat(b, img_dest);
      dt_bilateral_blur(b);
      dt_bilateral_slice(b, img_dest, img_dest, detail);
      dt_bilateral_free(b);

      image_lab2rgb(img_dest, roi_mask_scaled->width, roi_mask_scaled->height, ch, use_sse);
    }
  }

  // copy blurred (temp) image to destination image
  rt_copy_image_masked(img_dest, in, roi_in, ch, mask_scaled, roi_mask_scaled, opacity, use_sse);

cleanup:
  if(img_dest) dt_free_align(img_dest);
}

static void retouch_heal(float *const in, dt_iop_roi_t *const roi_in, const int ch, float *const mask_scaled,
                         dt_iop_roi_t *const roi_mask_scaled, const int dx, const int dy, const float opacity,
                         int use_sse)
{
  float *img_src = NULL;
  float *img_dest = NULL;

  // alloc temp images for source and destination
  img_src = dt_alloc_align(64, roi_mask_scaled->width * roi_mask_scaled->height * ch * sizeof(float));
  img_dest = dt_alloc_align(64, roi_mask_scaled->width * roi_mask_scaled->height * ch * sizeof(float));
  if((img_src == NULL) || (img_dest == NULL))
  {
    fprintf(stderr, "retouch_heal: error allocating memory for healing\n");
    goto cleanup;
  }

  // copy source and destination to temp images
  rt_copy_in_to_out(in, roi_in, img_src, roi_mask_scaled, ch, dx, dy);
  rt_copy_in_to_out(in, roi_in, img_dest, roi_mask_scaled, ch, 0, 0);

  // heal it
  dt_heal(img_src, img_dest, mask_scaled, roi_mask_scaled->width, roi_mask_scaled->height, ch, use_sse);

  // copy healed (temp) image to destination image
  rt_copy_image_masked(img_dest, in, roi_in, ch, mask_scaled, roi_mask_scaled, opacity, use_sse);

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
      GList *forms = g_list_first(grp->points);
      while(forms)
      {
        const dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
        if(grpt == NULL)
        {
          fprintf(stderr, "rt_process_forms: invalid form\n");
          forms = g_list_next(forms);
          continue;
        }
        const int formid = grpt->formid;
        const float form_opacity = grpt->opacity;
        if(formid == 0)
        {
          fprintf(stderr, "rt_process_forms: form is null\n");
          forms = g_list_next(forms);
          continue;
        }
        const int index = rt_get_index_from_formid(p, formid);
        if(index == -1)
        {
          // FIXME: we get this error when user go back in history, so forms are the same but the array has changed
          fprintf(stderr, "rt_process_forms: missing form=%i from array\n", formid);
          forms = g_list_next(forms);
          continue;
        }

        // only process current scale
        if(p->rt_forms[index].scale != scale)
        {
          forms = g_list_next(forms);
          continue;
        }

        // get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form == NULL)
        {
          fprintf(stderr, "rt_process_forms: missing form=%i from masks\n", formid);
          forms = g_list_next(forms);
          continue;
        }

        // if the form is outside the roi, we just skip it
        if(!rt_masks_form_is_in_roi(self, piece, form, roi_layer, roi_layer))
        {
          forms = g_list_next(forms);
          continue;
        }

        // get the mask
        float *mask = NULL;
        dt_iop_roi_t roi_mask = { 0 };

        dt_masks_get_mask(self, piece, form, &mask, &roi_mask.width, &roi_mask.height, &roi_mask.x, &roi_mask.y);
        if(mask == NULL)
        {
          fprintf(stderr, "rt_process_forms: error retrieving mask\n");
          forms = g_list_next(forms);
          continue;
        }

        // search the delta with the source
        const dt_iop_retouch_algo_type_t algo = p->rt_forms[index].algorithm;
        int dx = 0, dy = 0;

        if(algo != dt_iop_retouch_blur && algo != dt_iop_retouch_fill)
        {
          if(!rt_masks_get_delta(self, piece, roi_layer, form, &dx, &dy))
          {
            forms = g_list_next(forms);
            if(mask) free(mask);
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
          free(mask);
          mask = NULL;
        }

        if(mask_scaled == NULL)
        {
          forms = g_list_next(forms);
          continue;
        }

        if((dx != 0 || dy != 0 || algo == dt_iop_retouch_blur || algo == dt_iop_retouch_fill)
           && ((roi_mask_scaled.width > 2) && (roi_mask_scaled.height > 2)))
        {
          if(algo == dt_iop_retouch_clone)
          {
            retouch_clone(layer, roi_layer, wt_p->ch, mask_scaled, &roi_mask_scaled, dx, dy, form_opacity,
                          wt_p->use_sse);
          }
          else if(algo == dt_iop_retouch_heal)
          {
            retouch_heal(layer, roi_layer, wt_p->ch, mask_scaled, &roi_mask_scaled, dx, dy, form_opacity,
                         wt_p->use_sse);
          }
          else if(algo == dt_iop_retouch_blur)
          {
            retouch_blur(layer, roi_layer, wt_p->ch, mask_scaled, &roi_mask_scaled, form_opacity,
                         p->rt_forms[index].blur_type, p->rt_forms[index].blur_radius, piece, wt_p->use_sse);
          }
          else if(algo == dt_iop_retouch_fill)
          {
            // add a brightness to the color so it can be fine-adjusted by the user
            float fill_color[3];

            if(p->rt_forms[index].fill_mode == dt_iop_retouch_fill_erase)
            {
              fill_color[0] = fill_color[1] = fill_color[2] = p->rt_forms[index].fill_brightness;
            }
            else
            {
              fill_color[0] = p->rt_forms[index].fill_color[0] + p->rt_forms[index].fill_brightness;
              fill_color[1] = p->rt_forms[index].fill_color[1] + p->rt_forms[index].fill_brightness;
              fill_color[2] = p->rt_forms[index].fill_color[2] + p->rt_forms[index].fill_brightness;
            }

            retouch_fill(layer, roi_layer, wt_p->ch, mask_scaled, &roi_mask_scaled, form_opacity, fill_color,
                         wt_p->use_sse);
          }
          else
            fprintf(stderr, "rt_process_forms: unknown algorithm %i\n", algo);

          if(mask_display)
            rt_copy_mask_to_alpha(layer, roi_layer, wt_p->ch, mask_scaled, &roi_mask_scaled, form_opacity);
        }

        if(mask) free(mask);
        if(mask_scaled) free(mask_scaled);

        forms = g_list_next(forms);
      }
    }
  }
}

static void process_internal(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                             void *const ovoid, const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out, const int use_sse)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_iop_retouch_gui_data_t *g = (dt_iop_retouch_gui_data_t *)self->gui_data;

  float *in_retouch = NULL;

  dt_iop_roi_t roi_retouch = *roi_in;
  dt_iop_roi_t *roi_rt = &roi_retouch;

  const int ch = piece->colors;
  retouch_user_data_t usr_data = { 0 };
  dwt_params_t *dwt_p = NULL;

  const int gui_active = (self->dev) ? (self == self->dev->gui_module) : 0;
  const int display_wavelet_scale = (g && gui_active) ? g->display_wavelet_scale : 0;

  // we will do all the clone, heal, etc on the input image,
  // this way the source for one algorithm can be the destination from a previous one
  in_retouch = dt_alloc_align(64, roi_rt->width * roi_rt->height * ch * sizeof(float));
  if(in_retouch == NULL) goto cleanup;

  memcpy(in_retouch, ivoid, roi_rt->width * roi_rt->height * ch * sizeof(float));

  // user data passed from the decompose routine to the one that process each scale
  usr_data.self = self;
  usr_data.piece = piece;
  usr_data.roi = *roi_rt;
  usr_data.mask_display = 0;
  usr_data.suppress_mask = (g && g->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                            && (piece->pipe == self->dev->pipe));
  usr_data.display_scale = p->curr_scale;

  // init the decompose routine
  dwt_p = dt_dwt_init(in_retouch, roi_rt->width, roi_rt->height, ch, p->num_scales,
                      (!display_wavelet_scale) ? 0 : p->curr_scale, p->merge_from_scale, &usr_data,
                      roi_in->scale / piece->iscale, use_sse);
  if(dwt_p == NULL) goto cleanup;

  // check if this module should expose mask.
  if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL && g && g->mask_display && self->dev->gui_attached
     && (self == self->dev->gui_module) && (piece->pipe == self->dev->pipe))
  {
    for(size_t j = 0; j < roi_rt->width * roi_rt->height * ch; j += ch) in_retouch[j + 3] = 0.f;

    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    usr_data.mask_display = 1;
  }

  if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
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

  float levels[3] = { 0.f };
  levels[0] = p->preview_levels[0];
  levels[1] = p->preview_levels[1];
  levels[2] = p->preview_levels[2];

  // process auto levels
  if(g && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_pthread_mutex_lock(&g->lock);
    if(g->preview_auto_levels == 1 && !darktable.gui->reset)
    {
      g->preview_auto_levels = -1;

      dt_pthread_mutex_unlock(&g->lock);

      levels[0] = levels[1] = levels[2] = 0;
      rt_process_stats(in_retouch, roi_rt->width, roi_rt->height, ch, levels, use_sse);
      rt_clamp_minmax(levels, levels);

      for(int i = 0; i < 3; i++) g->preview_levels[i] = levels[i];

      dt_pthread_mutex_lock(&g->lock);

      g->preview_auto_levels = 2;

      dt_pthread_mutex_unlock(&g->lock);
    }
    else
    {
      dt_pthread_mutex_unlock(&g->lock);
    }
  }

  // if user wants to preview a detail scale adjust levels
  if(dwt_p->return_layer > 0 && dwt_p->return_layer < dwt_p->scales + 1)
  {
    rt_adjust_levels(in_retouch, roi_rt->width, roi_rt->height, ch, levels, use_sse);
  }

  // copy alpha channel if nedded
  if((piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) && g && !g->mask_display)
  {
    dt_iop_alpha_copy(ivoid, in_retouch, roi_rt->width, roi_rt->height);
  }

  // return final image
  rt_copy_in_to_out(in_retouch, roi_rt, ovoid, roi_out, ch, 0, 0);

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

#ifdef HAVE_OPENCL

cl_int rt_process_stats_cl(const int devid, cl_mem dev_img, const int width, const int height, float levels[3])
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  float *src_buffer = NULL;

  src_buffer = dt_alloc_align(64, width * height * ch * sizeof(float));
  if(src_buffer == NULL)
  {
    fprintf(stderr, "dt_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_read_buffer_from_device(devid, (void *)src_buffer, dev_img, 0,
                                          (size_t)width * height * ch * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

  // just call the CPU version for now
  rt_process_stats(src_buffer, width, height, ch, levels, 1);

  err = dt_opencl_write_buffer_to_device(devid, src_buffer, dev_img, 0, width * height * ch * sizeof(float), TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

cleanup:
  if(src_buffer) dt_free_align(src_buffer);

  return err;
}

cl_int rt_adjust_levels_cl(const int devid, cl_mem dev_img, const int width, const int height,
                           const float levels[3])
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  float *src_buffer = NULL;

  src_buffer = dt_alloc_align(64, width * height * ch * sizeof(float));
  if(src_buffer == NULL)
  {
    fprintf(stderr, "dt_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_read_buffer_from_device(devid, (void *)src_buffer, dev_img, 0,
                                          (size_t)width * height * ch * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS)
  {
    goto cleanup;
  }

  // just call the CPU version for now
  rt_adjust_levels(src_buffer, width, height, ch, levels, 1);

  err = dt_opencl_write_buffer_to_device(devid, src_buffer, dev_img, 0, width * height * ch * sizeof(float), TRUE);
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

  const size_t sizes[]
      = { ROUNDUPWD(MIN(roi_out->width, roi_in->width)), ROUNDUPHT(MIN(roi_out->height, roi_in->height)), 1 };

  dev_roi_in = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_in);
  dev_roi_out = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_out);
  if(dev_roi_in == NULL || dev_roi_out == NULL)
  {
    fprintf(stderr, "rt_copy_in_to_out_cl error 1\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_roi_in);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), (void *)&dev_roi_out);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), (void *)&xoffs);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&yoffs);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "rt_copy_in_to_out_cl error 2\n");
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
      = dt_opencl_alloc_device_buffer(devid, roi_mask_scaled->width * roi_mask_scaled->height * sizeof(float));
  if(dev_mask_scaled == NULL)
  {
    fprintf(stderr, "rt_build_scaled_mask_cl error 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_write_buffer_to_device(devid, *mask_scaled, dev_mask_scaled, 0,
                                         roi_mask_scaled->width * roi_mask_scaled->height * sizeof(float), TRUE);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "rt_build_scaled_mask_cl error 4\n");
    goto cleanup;
  }

  *p_dev_mask_scaled = dev_mask_scaled;

cleanup:
  if(err != CL_SUCCESS) fprintf(stderr, "rt_build_scaled_mask_cl error\n");

  return err;
}

static cl_int rt_copy_image_masked_cl(const int devid, cl_mem dev_src, cl_mem dev_dest,
                                      dt_iop_roi_t *const roi_dest, cl_mem dev_mask_scaled,
                                      dt_iop_roi_t *const roi_mask_scaled, const float opacity, const int kernel)
{
  cl_int err = CL_SUCCESS;

  const size_t sizes[] = { ROUNDUPWD(roi_mask_scaled->width), ROUNDUPHT(roi_mask_scaled->height), 1 };

  const cl_mem dev_roi_dest =
    dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_dest);

  const cl_mem dev_roi_mask_scaled
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_mask_scaled);

  if(dev_roi_dest == NULL || dev_roi_mask_scaled == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_src);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_dest);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), (void *)&dev_roi_dest);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), (void *)&dev_mask_scaled);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_roi_mask_scaled);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), (void *)&opacity);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
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
  const size_t sizes[] = { ROUNDUPWD(roi_mask_scaled->width), ROUNDUPHT(roi_mask_scaled->height), 1 };

  const cl_mem  dev_roi_layer = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_layer);
  const cl_mem dev_roi_mask_scaled
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_mask_scaled);
  if(dev_roi_layer == NULL || dev_roi_mask_scaled == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_layer);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_roi_layer);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), (void *)&dev_mask_scaled);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), (void *)&dev_roi_mask_scaled);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), (void *)&opacity);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto cleanup;


cleanup:
  dt_opencl_release_mem_object(dev_roi_layer);
  dt_opencl_release_mem_object(dev_roi_mask_scaled);

  return err;
}

static cl_int retouch_clone_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer,
                               cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const int dx,
                               const int dy, const float opacity, dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  // alloc source temp image to avoid issues when areas self-intersects
  const cl_mem dev_src = dt_opencl_alloc_device_buffer(devid,
                                          roi_mask_scaled->width * roi_mask_scaled->height * ch * sizeof(float));
  if(dev_src == NULL)
  {
    fprintf(stderr, "retouch_clone_cl error 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  // copy source image to tmp
  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_src, roi_mask_scaled, dx, dy,
                             gd->kernel_retouch_copy_buffer_to_buffer);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "retouch_clone_cl error 4\n");
    goto cleanup;
  }

  // clone it
  err = rt_copy_image_masked_cl(devid, dev_src, dev_layer, roi_layer, dev_mask_scaled, roi_mask_scaled, opacity,
                                gd->kernel_retouch_copy_buffer_to_buffer_masked);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "retouch_clone_cl error 5\n");
    goto cleanup;
  }

cleanup:
  if(dev_src) dt_opencl_release_mem_object(dev_src);

  return err;
}

static cl_int retouch_fill_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer,
                              cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const float opacity,
                              float *color, dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  // fill it
  const int kernel = gd->kernel_retouch_fill;
  const size_t sizes[] = { ROUNDUPWD(roi_mask_scaled->width), ROUNDUPHT(roi_mask_scaled->height), 1 };

  const cl_mem dev_roi_layer = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_layer);
  const cl_mem dev_roi_mask_scaled
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_roi_t), (void *)roi_mask_scaled);
  if(dev_roi_layer == NULL || dev_roi_mask_scaled == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_layer);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_roi_layer);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), (void *)&dev_mask_scaled);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), (void *)&dev_roi_mask_scaled);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), (void *)&opacity);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), (void *)&(color[0]));
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), (void *)&(color[1]));
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), (void *)&(color[2]));
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto cleanup;


cleanup:
  dt_opencl_release_mem_object(dev_roi_layer);
  dt_opencl_release_mem_object(dev_roi_mask_scaled);

  return err;
}

static cl_int retouch_blur_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer,
                              cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const float opacity,
                              const int blur_type, const float blur_radius, dt_dev_pixelpipe_iop_t *piece,
                              dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  if(fabs(blur_radius) <= 0.1f) return err;

  const float sigma = blur_radius * roi_layer->scale / piece->iscale;
  const int ch = 4;

  const cl_mem dev_dest =
    dt_opencl_alloc_device(devid, roi_mask_scaled->width, roi_mask_scaled->height, ch * sizeof(float));
  if(dev_dest == NULL)
  {
    fprintf(stderr, "retouch_blur_cl error 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  if(blur_type == dt_iop_retouch_blur_bilateral)
  {
    const int kernel = gd->kernel_retouch_image_rgb2lab;
    size_t sizes[] = { ROUNDUPWD(roi_layer->width), ROUNDUPHT(roi_layer->height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_layer);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(roi_layer->width));
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(roi_layer->height));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto cleanup;
  }

  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_dest, roi_mask_scaled, 0, 0,
                             gd->kernel_retouch_copy_buffer_to_image);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "retouch_blur_cl error 4\n");
    goto cleanup;
  }

  if(blur_type == dt_iop_retouch_blur_gaussian && fabs(blur_radius) > 0.1f)
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
  else if(blur_type == dt_iop_retouch_blur_bilateral && fabs(blur_radius) > 0.1f)
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
    fprintf(stderr, "retouch_blur_cl error 5\n");
    goto cleanup;
  }

  if(blur_type == dt_iop_retouch_blur_bilateral)
  {
    const int kernel = gd->kernel_retouch_image_lab2rgb;
    const size_t sizes[] = { ROUNDUPWD(roi_layer->width), ROUNDUPHT(roi_layer->height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_layer);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(roi_layer->width));
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(roi_layer->height));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto cleanup;
  }

cleanup:
  if(dev_dest) dt_opencl_release_mem_object(dev_dest);

  return err;
}

static cl_int retouch_heal_cl(const int devid, cl_mem dev_layer, dt_iop_roi_t *const roi_layer, float *mask_scaled,
                              cl_mem dev_mask_scaled, dt_iop_roi_t *const roi_mask_scaled, const int dx,
                              const int dy, const float opacity, dt_iop_retouch_global_data_t *gd)
{
  cl_int err = CL_SUCCESS;

  const int ch = 4;

  cl_mem dev_dest = NULL;
  cl_mem dev_src = dt_opencl_alloc_device_buffer(devid,
                                          roi_mask_scaled->width * roi_mask_scaled->height * ch * sizeof(float));
  if(dev_src == NULL)
  {
    fprintf(stderr, "retouch_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_dest = dt_opencl_alloc_device_buffer(devid,
                                           roi_mask_scaled->width * roi_mask_scaled->height * ch * sizeof(float));
  if(dev_dest == NULL)
  {
    fprintf(stderr, "retouch_heal_cl: error allocating memory for healing\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_src, roi_mask_scaled, dx, dy,
                             gd->kernel_retouch_copy_buffer_to_buffer);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "retouch_heal_cl error 4\n");
    goto cleanup;
  }

  err = rt_copy_in_to_out_cl(devid, dev_layer, roi_layer, dev_dest, roi_mask_scaled, 0, 0,
                             gd->kernel_retouch_copy_buffer_to_buffer);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "retouch_heal_cl error 4\n");
    goto cleanup;
  }

  // heal it
  heal_params_cl_t *hp = dt_heal_init_cl(devid);
  if(hp)
  {
    err = dt_heal_cl(hp, dev_src, dev_dest, mask_scaled, roi_mask_scaled->width, roi_mask_scaled->height);
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
    fprintf(stderr, "retouch_heal_cl error 6\n");
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
  dt_iop_retouch_global_data_t *gd = (dt_iop_retouch_global_data_t *)self->data;
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
      GList *forms = g_list_first(grp->points);
      while(forms && err == CL_SUCCESS)
      {
        dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
        if(grpt == NULL)
        {
          fprintf(stderr, "rt_process_forms: invalid form\n");
          forms = g_list_next(forms);
          continue;
        }
        const int formid = grpt->formid;
        const float form_opacity = grpt->opacity;
        if(formid == 0)
        {
          fprintf(stderr, "rt_process_forms: form is null\n");
          forms = g_list_next(forms);
          continue;
        }
        const int index = rt_get_index_from_formid(p, formid);
        if(index == -1)
        {
          // FIXME: we get this error when user go back in history, so forms are the same but the array has changed
          fprintf(stderr, "rt_process_forms: missing form=%i from array\n", formid);
          forms = g_list_next(forms);
          continue;
        }

        // only process current scale
        if(p->rt_forms[index].scale != scale)
        {
          forms = g_list_next(forms);
          continue;
        }

        // get the spot
        dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, formid);
        if(form == NULL)
        {
          fprintf(stderr, "rt_process_forms: missing form=%i from masks\n", formid);
          forms = g_list_next(forms);
          continue;
        }

        // if the form is outside the roi, we just skip it
        if(!rt_masks_form_is_in_roi(self, piece, form, roi_layer, roi_layer))
        {
          forms = g_list_next(forms);
          continue;
        }

        // get the mask
        float *mask = NULL;
        dt_iop_roi_t roi_mask = { 0 };

        dt_masks_get_mask(self, piece, form, &mask, &roi_mask.width, &roi_mask.height, &roi_mask.x, &roi_mask.y);
        if(mask == NULL)
        {
          fprintf(stderr, "rt_process_forms: error retrieving mask\n");
          forms = g_list_next(forms);
          continue;
        }

        int dx = 0, dy = 0;

        // search the delta with the source
        const dt_iop_retouch_algo_type_t algo = p->rt_forms[index].algorithm;
        if(algo != dt_iop_retouch_blur && algo != dt_iop_retouch_fill)
        {
          if(!rt_masks_get_delta(self, piece, roi_layer, form, &dx, &dy))
          {
            forms = g_list_next(forms);
            if(mask) free(mask);
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
        if(algo != dt_iop_retouch_heal && mask_scaled != NULL)
        {
          free(mask_scaled);
          mask_scaled = NULL;
        }

        // we don't need the original mask anymore
        if(mask)
        {
          free(mask);
          mask = NULL;
        }

        if(mask_scaled == NULL && algo == dt_iop_retouch_heal)
        {
          forms = g_list_next(forms);

          if(dev_mask_scaled) dt_opencl_release_mem_object(dev_mask_scaled);
          dev_mask_scaled = NULL;

          continue;
        }

        if((err == CL_SUCCESS)
           && (dx != 0 || dy != 0 || algo == dt_iop_retouch_blur || algo == dt_iop_retouch_fill)
           && ((roi_mask_scaled.width > 2) && (roi_mask_scaled.height > 2)))
        {
          if(algo == dt_iop_retouch_clone)
          {
            err = retouch_clone_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, dx, dy,
                                   form_opacity, gd);
          }
          else if(algo == dt_iop_retouch_heal)
          {
            err = retouch_heal_cl(devid, dev_layer, roi_layer, mask_scaled, dev_mask_scaled, &roi_mask_scaled, dx,
                                  dy, form_opacity, gd);
          }
          else if(algo == dt_iop_retouch_blur)
          {
            err = retouch_blur_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, form_opacity,
                                  p->rt_forms[index].blur_type, p->rt_forms[index].blur_radius, piece, gd);
          }
          else if(algo == dt_iop_retouch_fill)
          {
            // add a brightness to the color so it can be fine-adjusted by the user
            float fill_color[3];

            if(p->rt_forms[index].fill_mode == dt_iop_retouch_fill_erase)
            {
              fill_color[0] = fill_color[1] = fill_color[2] = p->rt_forms[index].fill_brightness;
            }
            else
            {
              fill_color[0] = p->rt_forms[index].fill_color[0] + p->rt_forms[index].fill_brightness;
              fill_color[1] = p->rt_forms[index].fill_color[1] + p->rt_forms[index].fill_brightness;
              fill_color[2] = p->rt_forms[index].fill_color[2] + p->rt_forms[index].fill_brightness;
            }

            err = retouch_fill_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, form_opacity,
                                  fill_color, gd);
          }
          else
            fprintf(stderr, "rt_process_forms: unknown algorithm %i\n", algo);

          if(mask_display)
            rt_copy_mask_to_alpha_cl(devid, dev_layer, roi_layer, dev_mask_scaled, &roi_mask_scaled, form_opacity,
                                     gd);
        }

        if(mask) free(mask);
        if(mask_scaled) free(mask_scaled);
        if(dev_mask_scaled) dt_opencl_release_mem_object(dev_mask_scaled);

        forms = g_list_next(forms);
      }
    }
  }

  return err;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)piece->data;
  dt_iop_retouch_global_data_t *gd = (dt_iop_retouch_global_data_t *)self->data;
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
  const cl_mem in_retouch = dt_opencl_alloc_device_buffer(devid, roi_rt->width * roi_rt->height * ch * sizeof(float));
  if(in_retouch == NULL)
  {
    fprintf(stderr, "process_internal: error allocating memory for wavelet decompose\n");
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
                         (!display_wavelet_scale) ? 0 : p->curr_scale, p->merge_from_scale, &usr_data,
                         roi_in->scale / piece->iscale);
  if(dwt_p == NULL)
  {
    fprintf(stderr, "process_internal: error initializing wavelet decompose\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  // check if this module should expose mask.
  if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL && g && g->mask_display && self->dev->gui_attached
     && (self == self->dev->gui_module) && (piece->pipe == self->dev->pipe))
  {
    const int kernel = gd->kernel_retouch_clear_alpha;
    const size_t sizes[] = { ROUNDUPWD(roi_rt->width), ROUNDUPHT(roi_rt->height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&in_retouch);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(roi_rt->width));
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(roi_rt->height));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto cleanup;

    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    usr_data.mask_display = 1;
  }

  if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
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

  float levels[3] = { 0.f };
  levels[0] = p->preview_levels[0];
  levels[1] = p->preview_levels[1];
  levels[2] = p->preview_levels[2];

  // process auto levels
  if(g && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_pthread_mutex_lock(&g->lock);
    if(g->preview_auto_levels == 1 && !darktable.gui->reset)
    {
      g->preview_auto_levels = -1;

      dt_pthread_mutex_unlock(&g->lock);

      levels[0] = levels[1] = levels[2] = 0;
      err = rt_process_stats_cl(devid, in_retouch, roi_rt->width, roi_rt->height, levels);
      if(err != CL_SUCCESS) goto cleanup;

      rt_clamp_minmax(levels, levels);

      for(int i = 0; i < 3; i++) g->preview_levels[i] = levels[i];

      dt_pthread_mutex_lock(&g->lock);

      g->preview_auto_levels = 2;

      dt_pthread_mutex_unlock(&g->lock);
    }
    else
    {
      dt_pthread_mutex_unlock(&g->lock);
    }
  }

  // if user wants to preview a detail scale adjust levels
  if(dwt_p->return_layer > 0 && dwt_p->return_layer < dwt_p->scales + 1)
  {
    err = rt_adjust_levels_cl(devid, in_retouch, roi_rt->width, roi_rt->height, levels);
    if(err != CL_SUCCESS) goto cleanup;
  }

  // copy alpha channel if nedded
  if((piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) && g && !g->mask_display)
  {
    const int kernel = gd->kernel_retouch_copy_alpha;
    const size_t sizes[] = { ROUNDUPWD(roi_rt->width), ROUNDUPHT(roi_rt->height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&in_retouch);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(roi_rt->width));
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(roi_rt->height));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto cleanup;
  }

  // return final image
  err = rt_copy_in_to_out_cl(devid, in_retouch, roi_in, dev_out, roi_out, 0, 0,
                             gd->kernel_retouch_copy_buffer_to_image);

cleanup:
  if(dwt_p) dt_dwt_free_cl(dwt_p);

  if(in_retouch) dt_opencl_release_mem_object(in_retouch);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_retouch] couldn't enqueue kernel! %d\n", err);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
