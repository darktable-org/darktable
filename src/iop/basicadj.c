/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.

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

/*
    auto exposure is based on RawTherapee's Auto Levels
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/math.h"
#include "common/rgb_norms.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "develop/tiling.h"

DT_MODULE_INTROSPECTION(2, dt_iop_basicadj_params_t)

#define exposure2white(x) exp2f(-(x))

typedef struct dt_iop_basicadj_params_t
{
  float black_point;    /* $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
                           $DESCRIPTION:"black level correction" */
  float exposure;       // $MIN: -18.0 $MAX: 18.0 $DEFAULT: 0.0
  float hlcompr;        /* $MIN: 0 $MAX: 500.0 $DEFAULT: 0.0
                           $DESCRIPTION:"highlight compression" */
  float hlcomprthresh;
  float contrast;       // $MIN: -1.0 $MAX: 5.0 $DEFAULT: 0.0
  dt_iop_rgb_norms_t preserve_colors; /* $DEFAULT: DT_RGB_NORM_LUMINANCE
                                         $DESCRIPTION:"preserve colors" */
  float middle_grey;    // $MIN: 0.05 $MAX: 100 $DEFAULT: 18.42 $DESCRIPTION: "middle gray"
  float brightness;     // $MIN: -4.0 $MAX: 4.0 $DEFAULT: 0.0
  float saturation;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
  float vibrance;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
  float clip;           // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
} dt_iop_basicadj_params_t;

typedef struct dt_iop_basicadj_gui_data_t
{
  dt_iop_basicadj_params_t params;

  int call_auto_exposure;                       // should we calculate exposure automatically?
  int draw_selected_region;                     // are we drawing the selected region?
  float posx_from, posx_to, posy_from, posy_to; // coordinates of the area
  dt_boundingbox_t box_cood;                    // normalized coordinates
  int button_down;                              // user pressed the mouse button?

  GtkWidget *bt_auto_levels;
  GtkWidget *bt_select_region;

  GtkWidget *sl_black_point;
  GtkWidget *sl_exposure;
  GtkWidget *sl_hlcompr;
  GtkWidget *sl_contrast;
  GtkWidget *cmb_preserve_colors;
  GtkWidget *sl_middle_grey;
  GtkWidget *sl_brightness;
  GtkWidget *sl_saturation;
  GtkWidget *sl_vibrance;
  GtkWidget *sl_clip;
} dt_iop_basicadj_gui_data_t;

typedef struct dt_iop_basicadj_data_t
{
  dt_iop_basicadj_params_t params;
  float lut_gamma[0x10000];
  float lut_contrast[0x10000];
} dt_iop_basicadj_data_t;

typedef struct dt_iop_basicadj_global_data_t
{
  int kernel_basicadj;
} dt_iop_basicadj_global_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_basicadj_params_v1_t
    {
      float black_point;
      float exposure;
      float hlcompr;
      float hlcomprthresh;
      float contrast;
      int preserve_colors;
      float middle_grey;
      float brightness;
      float saturation;
      float clip;
    } dt_iop_basicadj_params_v1_t;

    const dt_iop_basicadj_params_v1_t *old = old_params;
    dt_iop_basicadj_params_t *new = new_params;

    new->black_point = old->black_point;
    new->exposure = old->exposure;
    new->hlcompr = old->hlcompr;
    new->hlcomprthresh = old->hlcomprthresh;
    new->contrast = old->contrast;
    new->preserve_colors = old->preserve_colors;
    new->middle_grey = old->middle_grey;
    new->brightness = old->brightness;
    new->saturation = old->saturation;
    new->clip = old->clip;
    new->vibrance = 0;
    return 0;
  }
  return 1;
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the quick access panel instead.");
}

const char *name()
{
  return _("basic adjustments");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("apply usual image adjustments"),
                                      _("creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_DEPRECATED;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static void _turn_select_region_off(struct dt_iop_module_t *self)
{
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;
  if(g)
  {
    g->button_down = g->draw_selected_region = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_select_region), g->draw_selected_region);
  }
}

static void _turn_selregion_picker_off(struct dt_iop_module_t *self)
{
  _turn_select_region_off(self);
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  _turn_select_region_off(self);
}

static void _color_picker_callback(GtkWidget *button, dt_iop_module_t *self)
{
  _turn_select_region_off(self);
}

static void _auto_levels_callback(GtkButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

  dt_iop_request_focus(self);
  if(self->off)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  _turn_selregion_picker_off(self);

  dt_iop_gui_enter_critical_section(self);
  if(g->call_auto_exposure == 0)
  {
    g->box_cood[0] = g->box_cood[1] = g->box_cood[2] = g->box_cood[3] = 0.f;
    g->call_auto_exposure = 1;
  }
  dt_iop_gui_leave_critical_section(self);

  dt_dev_reprocess_all(self->dev);
}

static void _select_region_toggled_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

  dt_iop_request_focus(self);
  if(self->off)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  dt_iop_color_picker_reset(self, TRUE);

  dt_iop_gui_enter_critical_section(self);
  if(gtk_toggle_button_get_active(togglebutton))
  {
    g->draw_selected_region = 1;
  }
  else
    g->draw_selected_region = 0;

  g->posx_from = g->posx_to = g->posy_from = g->posy_to = 0;
  dt_iop_gui_leave_critical_section(self);
}

static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basicadj_params_t *p = (dt_iop_basicadj_params_t *)self->params;
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

  if(g == NULL) return;

  // FIXME: this doesn't seems the right place to update params and GUI ...
  // update auto levels
  dt_iop_gui_enter_critical_section(self);
  if(g->call_auto_exposure == 2)
  {
    g->call_auto_exposure = -1;
    dt_iop_gui_leave_critical_section(self);

    memcpy(p, &g->params, sizeof(dt_iop_basicadj_params_t));

    dt_dev_add_history_item(darktable.develop, self, TRUE);

    dt_iop_gui_enter_critical_section(self);
    g->call_auto_exposure = 0;
    dt_iop_gui_leave_critical_section(self);

    ++darktable.gui->reset;

    gui_update(self);

    --darktable.gui->reset;
  }
  else
  {
    dt_iop_gui_leave_critical_section(self);
  }
}

static void _signal_profile_user_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  if(profile_type == DT_COLORSPACES_PROFILE_TYPE_WORK)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    if(!self->enabled) return;

    dt_iop_basicadj_params_t *def = (dt_iop_basicadj_params_t *)self->default_params;
    dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

    const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
    const float def_middle_grey
        = (work_profile) ? (dt_ioppr_get_profile_info_middle_grey(work_profile) * 100.f) : 18.42f;

    if(def->middle_grey != def_middle_grey)
    {
      def->middle_grey = def_middle_grey;

      if(g)
      {
        ++darktable.gui->reset;

        dt_bauhaus_slider_set_default(g->sl_middle_grey, def_middle_grey);

        --darktable.gui->reset;
      }
    }
  }
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  int handled = 0;
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;
  if(g && g->draw_selected_region && g->button_down && self->enabled)
  {
    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    g->posx_to = pzx * darktable.develop->preview_pipe->backbuf_width;
    g->posy_to = pzy * darktable.develop->preview_pipe->backbuf_height;

    dt_control_queue_redraw_center();

    handled = 1;
  }

  return handled;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  int handled = 0;
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;
  if(g && g->draw_selected_region && self->enabled)
  {
    if(fabsf(g->posx_from - g->posx_to) > 1 && fabsf(g->posy_from - g->posy_to) > 1)
    {
      g->box_cood[0] = g->posx_from;
      g->box_cood[1] = g->posy_from;
      g->box_cood[2] = g->posx_to;
      g->box_cood[3] = g->posy_to;
      dt_dev_distort_backtransform(darktable.develop, g->box_cood, 2);
      g->box_cood[0] /= darktable.develop->preview_pipe->iwidth;
      g->box_cood[1] /= darktable.develop->preview_pipe->iheight;
      g->box_cood[2] /= darktable.develop->preview_pipe->iwidth;
      g->box_cood[3] /= darktable.develop->preview_pipe->iheight;

      g->button_down = 0;
      g->call_auto_exposure = 1;

      dt_dev_reprocess_all(self->dev);
    }
    else
      g->button_down = 0;

    handled = 1;
  }

  return handled;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  int handled = 0;
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;
  if(g && g->draw_selected_region && self->enabled)
  {
    if((which == 3) || (which == 1 && type == GDK_2BUTTON_PRESS))
    {
      _turn_selregion_picker_off(self);

      handled = 1;
    }
    else if(which == 1)
    {
      float pzx, pzy;
      dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
      pzx += 0.5f;
      pzy += 0.5f;

      g->posx_from = g->posx_to = pzx * darktable.develop->preview_pipe->backbuf_width;
      g->posy_from = g->posy_to = pzy * darktable.develop->preview_pipe->backbuf_height;

      g->button_down = 1;

      handled = 1;
    }
  }

  return handled;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;
  if(g == NULL || !self->enabled) return;
  if(!g->draw_selected_region || !g->button_down) return;
  if(g->posx_from == g->posx_to && g->posy_from == g->posy_to) return;

  dt_develop_t *dev = darktable.develop;
  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1 << closeup, 1);

  const float posx_from = fmin(g->posx_from, g->posx_to);
  const float posx_to = fmax(g->posx_from, g->posx_to);
  const float posy_from = fmin(g->posy_from, g->posy_to);
  const float posy_to = fmax(g->posy_from, g->posy_to);

  cairo_save(cr);
  cairo_set_line_width(cr, 1.0 / zoom_scale);
  cairo_set_source_rgb(cr, .2, .2, .2);

  cairo_translate(cr, width / 2.0, height / 2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  cairo_rectangle(cr, posx_from, posy_from, (posx_to - posx_from), (posy_to - posy_from));
  cairo_stroke(cr);
  cairo_translate(cr, 1.0 / zoom_scale, 1.0 / zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);
  cairo_rectangle(cr, posx_from + 1.0 / zoom_scale, posy_from, (posx_to - posx_from) - 3. / zoom_scale,
                  (posy_to - posy_from) - 2. / zoom_scale);
  cairo_stroke(cr);

  cairo_restore(cr);
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 24; // basicadj.cl, from programs.conf
  dt_iop_basicadj_global_data_t *gd
      = (dt_iop_basicadj_global_data_t *)malloc(sizeof(dt_iop_basicadj_global_data_t));
  module->data = gd;

  gd->kernel_basicadj = dt_opencl_create_kernel(program, "basicadj");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_basicadj_global_data_t *gd = (dt_iop_basicadj_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_basicadj);
  free(module->data);
  module->data = NULL;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  if(darktable.gui->reset) return;
  dt_iop_basicadj_params_t *p = (dt_iop_basicadj_params_t *)self->params;
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  p->middle_grey = (work_profile) ? (dt_ioppr_get_rgb_matrix_luminance(self->picked_color,
                                                                       work_profile->matrix_in,
                                                                       work_profile->lut_in,
                                                                       work_profile->unbounded_coeffs_in,
                                                                       work_profile->lutsize,
                                                                       work_profile->nonlinearlut) * 100.f)
                                  : dt_camera_rgb_luminance(self->picked_color);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->sl_middle_grey, p->middle_grey);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static inline float get_gamma(const float x, const float gamma)
{
  return powf(x, gamma);
}

static inline float get_lut_gamma(const float x, const float gamma, const float *const lut)
{
  return (x > 1.f) ? get_gamma(x, gamma) : lut[CLAMP((int)(x * 0x10000ul), 0, 0xffff)];
}

static inline float get_contrast(const float x, const float contrast, const float middle_grey,
                                 const float inv_middle_grey)
{
  return powf(x * inv_middle_grey, contrast) * middle_grey;
}

static inline float get_lut_contrast(const float x, const float contrast, const float middle_grey,
                                     const float inv_middle_grey, const float *const lut)
{
  return (x > 1.f) ? get_contrast(x, contrast, middle_grey, inv_middle_grey)
                   : lut[CLAMP((int)(x * 0x10000ul), 0, 0xffff)];
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f;
  tiling->factor_cl = 3.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
}
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_basicadj_data_t *d = (dt_iop_basicadj_data_t *)piece->data;
  dt_iop_basicadj_params_t *p = (dt_iop_basicadj_params_t *)params;

  memcpy(&d->params, params, sizeof(dt_iop_basicadj_params_t));

  const float brightness = p->brightness * 2.f;
  const float gamma = (brightness >= 0.0f) ? 1.0f / (1.0f + brightness) : (1.0f - brightness);
  const float contrast = p->contrast + 1.0f;
  const float middle_grey = (p->middle_grey > 0.f) ? (p->middle_grey / 100.f) : 0.1842f;
  const float inv_middle_grey = 1.f / middle_grey;

  const int process_gamma = (p->brightness != 0.f);
  const int plain_contrast = (!p->preserve_colors && p->contrast != 0.f);

  // Building the lut for values in the [0,1] range
  if(process_gamma || plain_contrast)
  {
    for(unsigned int i = 0; i < 0x10000; i++)
    {
      const float percentage = (float)i / (float)0x10000ul;
      if(process_gamma) d->lut_gamma[i] = get_gamma(percentage, gamma);
      if(plain_contrast) d->lut_contrast[i] = get_contrast(percentage, contrast, middle_grey, inv_middle_grey);
    }
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_basicadj_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->bt_select_region), g->draw_selected_region);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(!in) _turn_select_region_off(self);
}

void change_image(struct dt_iop_module_t *self)
{
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

  g->call_auto_exposure = 0;
  g->draw_selected_region = 0;
  g->posx_from = g->posx_to = g->posy_from = g->posy_to = 0.f;
  g->box_cood[0] = g->box_cood[1] = g->box_cood[2] = g->box_cood[3] = 0.f;
  g->button_down = 0;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_basicadj_gui_data_t *g = IOP_GUI_ALLOC(basicadj);

  change_image(self);

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->sl_black_point = dt_bauhaus_slider_from_params(self, "black_point");
  dt_bauhaus_slider_set_soft_range(g->sl_black_point, -0.1, 0.1);
  dt_bauhaus_slider_set_digits(g->sl_black_point, 4);
  gtk_widget_set_tooltip_text(g->sl_black_point, _("adjust the black level to unclip negative RGB values.\n"
                                                    "you should never use it to add more density in blacks!\n"
                                                    "if poorly set, it will clip near-black colors out of gamut\n"
                                                    "by pushing RGB values into negatives"));

  g->sl_exposure = dt_bauhaus_slider_from_params(self, N_("exposure"));
  dt_bauhaus_slider_set_soft_range(g->sl_exposure, -4.0, 4.0);
  dt_bauhaus_slider_set_format(g->sl_exposure, _(" EV"));
  gtk_widget_set_tooltip_text(g->sl_exposure, _("adjust the exposure correction"));

  g->sl_hlcompr = dt_bauhaus_slider_from_params(self, "hlcompr");
  dt_bauhaus_slider_set_soft_max(g->sl_hlcompr, 100.0);
  gtk_widget_set_tooltip_text(g->sl_hlcompr, _("highlight compression adjustment"));

  g->sl_contrast = dt_bauhaus_slider_from_params(self, N_("contrast"));
  dt_bauhaus_slider_set_soft_range(g->sl_contrast, -1.0, 1.0);
  gtk_widget_set_tooltip_text(g->sl_contrast, _("contrast adjustment"));

  g->cmb_preserve_colors = dt_bauhaus_combobox_from_params(self, "preserve_colors") ;
  gtk_widget_set_tooltip_text(g->cmb_preserve_colors, _("method to preserve colors when applying contrast"));

  g->sl_middle_grey = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
                      dt_bauhaus_slider_from_params(self, "middle_grey"));
  dt_bauhaus_slider_set_format(g->sl_middle_grey, "%");
  gtk_widget_set_tooltip_text(g->sl_middle_grey, _("middle gray adjustment"));
  g_signal_connect(G_OBJECT(g->sl_middle_grey), "quad-pressed", G_CALLBACK(_color_picker_callback), self);

  g->sl_brightness = dt_bauhaus_slider_from_params(self, N_("brightness"));
  dt_bauhaus_slider_set_soft_range(g->sl_brightness, -1.0, 1.0);
  gtk_widget_set_tooltip_text(g->sl_brightness,_("brightness adjustment"));

  g->sl_saturation = dt_bauhaus_slider_from_params(self, N_("saturation"));
  gtk_widget_set_tooltip_text(g->sl_saturation,_("saturation adjustment"));

  g->sl_vibrance = dt_bauhaus_slider_from_params(self, N_("vibrance"));
  gtk_widget_set_tooltip_text(g->sl_vibrance, _("vibrance adjustment"));

  GtkWidget *autolevels_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(10));

  g->bt_auto_levels = dt_action_button_new(NULL, N_("auto"), _auto_levels_callback, self, _("apply auto exposure based on the entire image"), 0, 0);
  gtk_widget_set_size_request(g->bt_auto_levels, -1, DT_PIXEL_APPLY_DPI(24));
  gtk_box_pack_start(GTK_BOX(autolevels_box), g->bt_auto_levels, TRUE, TRUE, 0);

  g->bt_select_region = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, 0, NULL);
  dt_gui_add_class(g->bt_select_region, "dt_transparent_background");
  gtk_widget_set_tooltip_text(g->bt_select_region,
                              _("apply auto exposure based on a region defined by the user\n"
                                "click and drag to draw the area\n"
                                "right click to cancel"));
  g_signal_connect(G_OBJECT(g->bt_select_region), "toggled", G_CALLBACK(_select_region_toggled_callback), self);
  gtk_box_pack_start(GTK_BOX(autolevels_box), g->bt_select_region, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), autolevels_box, TRUE, TRUE, 0);

  g->sl_clip = dt_bauhaus_slider_from_params(self, N_("clip"));
  dt_bauhaus_slider_set_digits(g->sl_clip, 3);
  gtk_widget_set_tooltip_text(g->sl_clip, _("adjusts clipping value for auto exposure calculation"));

  // add signal handler for preview pipe finish
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_develop_ui_pipe_finished_callback), self);
  // and profile change
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_signal_profile_user_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_develop_ui_pipe_finished_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_signal_profile_user_changed), self);

  IOP_GUI_FREE;
}

static inline int64_t doubleToRawLongBits(double d)
{
  union {
    double f;
    int64_t i;
  } tmp;
  tmp.f = d;
  return tmp.i;
}

static inline double longBitsToDouble(int64_t i)
{
  union {
    double f;
    int64_t i;
  } tmp;
  tmp.i = i;
  return tmp.f;
}

static inline int ilogbp1(double d)
{
  const int m = d < 4.9090934652977266E-91;
  d = m ? 2.037035976334486E90 * d : d;
  int q = (doubleToRawLongBits(d) >> 52) & 0x7ff;
  q = m ? q - (300 + 0x03fe) : q - 0x03fe;
  return q;
}

// calculate  x * 2^q
static inline double ldexpk(double x, int32_t q)
{
  int32_t m = q < 0 ? -1 : 0;
  m = (((m + q) >> 9) - m) << 7;
  q = q - (m << 2);
  double u = longBitsToDouble(((int64_t)(m + 0x3ff)) << 52);
  double u2 = u * u;
  u2 = u2 * u2;
  x = x * u2;
  u = longBitsToDouble(((int64_t)(q + 0x3ff)) << 52);
  return x * u;
}

static inline double xlog(double d)
{
  const int e = ilogbp1(d * 0.7071);
  const double m = ldexpk(d, -e);

  double x = (m - 1) / (m + 1);
  const double x2 = x * x;

  double t = 0.148197055177935105296783;
  t = fma(t, x2, 0.153108178020442575739679);
  t = fma(t, x2, 0.181837339521549679055568);
  t = fma(t, x2, 0.22222194152736701733275);
  t = fma(t, x2, 0.285714288030134544449368);
  t = fma(t, x2, 0.399999999989941956712869);
  t = fma(t, x2, 0.666666666666685503450651);
  t = fma(t, x2, 2);

  x = x * t + 0.693147180559945286226764 * e;

  if(isinf(d)) x = INFINITY;
  if(d < 0)    x = NAN;
  if(d == 0)   x = -INFINITY;

  return x;
}

static inline double gamma2(double x)
{
  const double sRGBGammaCurve = 2.4;
  return (x <= 0.00304) ? (x * 12.92) : (1.055 * exp(log(x) / sRGBGammaCurve) - 0.055);
}

static inline double igamma2(double x)
{
  const double sRGBGammaCurve = 2.4;
  return (x <= 0.03928) ? (x / 12.92) : (exp(log((x + 0.055) / 1.055) * sRGBGammaCurve));
}

static void _get_auto_exp_histogram(const float *const img, const int width, const int height, int *box_area,
                                    uint32_t **_histogram, unsigned int *_hist_size, int *_histcompr)
{
  const int ch = 4;
  const int histcompr = 3;
  const unsigned int hist_size = 65536 >> histcompr;
  uint32_t *histogram = NULL;
  const float mul = hist_size;

  histogram = dt_alloc_align(64, sizeof(uint32_t) * hist_size);
  if(histogram == NULL) goto cleanup;

  memset(histogram, 0, sizeof(uint32_t) * hist_size);

  if(box_area[2] > box_area[0] && box_area[3] > box_area[1])
  {
    for(int y = box_area[1]; y <= box_area[3]; y++)
    {
      const float *const in = img + (size_t)ch * width * y;
      for(int x = box_area[0]; x <= box_area[2]; x++)
      {
        const float *const pixel = in + x * ch;

        for(int c = 0; c < 3; c++)
        {
          if(pixel[c] <= 0.f)
          {
            histogram[0]++;
          }
          else if(pixel[c] >= 1.f)
          {
            histogram[hist_size - 1]++;
          }
          else
          {
            const uint32_t R = (uint32_t)(pixel[c] * mul);
            histogram[R]++;
          }
        }
      }
    }
  }
  else
  {
    for(int i = 0; i < width * height * ch; i += ch)
    {
      const float *const pixel = img + i;

      for(int c = 0; c < 3; c++)
      {
        if(pixel[c] <= 0.f)
        {
          histogram[0]++;
        }
        else if(pixel[c] >= 1.f)
        {
          histogram[hist_size - 1]++;
        }
        else
        {
          const uint32_t R = (uint32_t)(pixel[c] * mul);
          histogram[R]++;
        }
      }
    }
  }

cleanup:
  *_histogram = histogram;
  *_hist_size = hist_size;
  *_histcompr = histcompr;
}

static void _get_sum_and_average(const uint32_t *const histogram, const int hist_size, float *_sum, float *_avg)
{
  float sum = 0.f;
  float avg = 0.f;

  for(int i = 0; i < hist_size; i++)
  {
    float val = histogram[i];
    sum += val;
    avg += i * val;
  }

  avg /= sum;

  *_sum = sum;
  *_avg = avg;
}

static inline float hlcurve(const float level, const float hlcomp, const float hlrange)
{
  if(hlcomp > 0.0f)
  {
    float val = level + (hlrange - 1.f);

    // to avoid division by zero
    if(val == 0.0f)
    {
      val = 0.000001f;
    }

    float Y = val / hlrange;
    Y *= hlcomp;

    // to avoid log(<=0)
    if(Y <= -1.0f)
    {
      Y = -.999999f;
    }

    float R = hlrange / (val * hlcomp);
    return log1pf(Y) * R;
  }
  else
  {
    return 1.f;
  }
}

static void _get_auto_exp(const uint32_t *const histogram, const unsigned int hist_size, const int histcompr,
                          const float defgain, const float clip, const float midgray, float *_expcomp,
                          float *_bright, float *_contr, float *_black, float *_hlcompr, float *_hlcomprthresh)
{
  float expcomp = 0.f;
  float black = 0.f;
  float bright = 0.f;
  float contr = 0.f;
  float hlcompr = 0.f;
  float hlcomprthresh = 0.f;

  float scale = 65536.0f;

  const int imax = 65536 >> histcompr;
  int overex = 0;
  float sum = 0.f, hisum = 0.f, losum = 0.f;
  float ave = 0.f;

  // find average luminance
  _get_sum_and_average(histogram, hist_size, &sum, &ave);

  // find median of luminance
  int median = 0, count = histogram[0];

  while(count < sum / 2)
  {
    median++;
    count += histogram[median];
  }

  if(median == 0 || ave < 1.f) // probably the image is a blackframe
  {
    expcomp = 0.f;
    black = 0.f;
    bright = 0.f;
    contr = 0.f;
    hlcompr = 0.f;
    hlcomprthresh = 0.f;
    goto cleanup;
  }

  // compute std dev on the high and low side of median
  // and octiles of histogram
  float octile[8] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f }, ospread = 0.f;
  count = 0;

  int i = 0;

  for(; i < MIN((int)ave, imax); i++)
  {
    if(count < 8)
    {
      octile[count] += histogram[i];

      if(octile[count] > sum / 8.f || (count == 7 && octile[count] > sum / 16.f))
      {
        octile[count] = xlog(1. + (float)i) / log(2.f);
        count++;
      }
    }

    losum += histogram[i];
  }

  for(; i < imax; i++)
  {
    if(count < 8)
    {
      octile[count] += histogram[i];

      if(octile[count] > sum / 8.f || (count == 7 && octile[count] > sum / 16.f))
      {
        octile[count] = xlog(1.f + (float)i) / log(2.f);
        count++;
      }
    }

    hisum += histogram[i];
  }

  // probably the image is a blackframe
  if(losum == 0.f || hisum == 0.f)
  {
    expcomp = 0.f;
    black = 0.f;
    bright = 0.f;
    contr = 0.f;
    hlcompr = 0.f;
    hlcomprthresh = 0.f;
    goto cleanup;
  }

  // if very overxposed image
  if(octile[6] > log1pf((float)imax) / log2(2.f))  //*** Is this correct?  log2(2) == 1
  {
    octile[6] = 1.5f * octile[5] - 0.5f * octile[4];
    overex = 2;
  }

  // if overexposed
  if(octile[7] > log1pf((float)imax) / log2(2.f))  //*** Is this correct?  log2(2) == 1
  {
    octile[7] = 1.5f * octile[6] - 0.5f * octile[5];
    overex = 1;
  }

  // store values of octile[6] and octile[7] for calculation of exposure compensation
  // if we don't do this and the pixture is underexposed, calculation of exposure compensation assumes
  // that it's overexposed and calculates the wrong direction
  float oct6, oct7;
  oct6 = octile[6];
  oct7 = octile[7];

  for(int ii = 1; ii < 8; ii++)
  {
    if(octile[ii] == 0.0f)
    {
      octile[ii] = octile[ii - 1];
    }
  }

  // compute weighted average separation of octiles
  // for future use in contrast setting
  for(int ii = 1; ii < 6; ii++)
  {
    ospread += (octile[ii + 1] - octile[ii])
               / MAX(0.5f, (ii > 2 ? (octile[ii + 1] - octile[3]) : (octile[3] - octile[ii])));
  }

  ospread /= 5.f;

  // probably the image is a blackframe
  if(ospread <= 0.f)
  {
    expcomp = 0.f;
    black = 0.f;
    bright = 0.f;
    contr = 0.f;
    hlcompr = 0.f;
    hlcomprthresh = 0.f;
    goto cleanup;
  }

  // compute clipping points based on the original histograms (linear, without exp comp.)
  unsigned int clipped = 0;
  int rawmax = (imax)-1;

  while(histogram[rawmax] + clipped <= 0 && rawmax > 1)
  {
    clipped += histogram[rawmax];
    rawmax--;
  }

  // compute clipped white point
  unsigned int clippable = (int)(sum * clip);
  clipped = 0;
  int whiteclip = (imax)-1;

  while(whiteclip > 1 && (histogram[whiteclip] + clipped) <= clippable)
  {
    clipped += histogram[whiteclip];
    whiteclip--;
  }

  // compute clipped black point
  clipped = 0;
  int shc = 0;

  while(shc < whiteclip - 1 && histogram[shc] + clipped <= clippable)
  {
    clipped += histogram[shc];
    shc++;
  }

  // rescale to 65535 max
  rawmax <<= histcompr;
  whiteclip <<= histcompr;
  ave = ave * (1 << histcompr);
  median <<= histcompr;
  shc <<= histcompr;

  // compute exposure compensation as geometric mean of the amount that
  // sets the mean or median at middle gray, and the amount that sets the estimated top
  // of the histogram at or near clipping.
  const float expcomp1 = (logf(midgray * scale / (ave - shc + midgray * shc))) / DT_M_LN2f;
  float expcomp2;

  if(overex == 0) // image is not overexposed
  {
    expcomp2 = 0.5f * ((15.5f - histcompr - (2.f * oct7 - oct6)) + logf(scale / rawmax) / DT_M_LN2f);
  }
  else
  {
    expcomp2 = 0.5f * ((15.5f - histcompr - (2.f * octile[7] - octile[6])) + logf(scale / rawmax) / DT_M_LN2f);
  }

  if(fabsf(expcomp1) - fabsf(expcomp2) > 1.f) // for great expcomp
  {
    expcomp = (expcomp1 * fabsf(expcomp2) + expcomp2 * fabsf(expcomp1)) / (fabsf(expcomp1) + fabsf(expcomp2));
  }
  else
  {
    expcomp = 0.5 * (double)expcomp1 + 0.5 * (double)expcomp2; // for small expcomp
  }

  const float gain = expf(expcomp * DT_M_LN2f);

  const float corr = sqrtf(gain * scale / rawmax);
  black = shc * corr;

  // now tune hlcompr to bring back rawmax to 65535
  hlcomprthresh = 0.f;
  // this is a series approximation of the actual formula for comp,
  // which is a transcendental equation
  const float comp = (gain * ((float)whiteclip) / scale - 1.f) * 2.3f; // 2.3 instead of 2 to increase slightly comp
  hlcompr = (comp / (fmaxf(0.0f, expcomp) + 1.0f));
  hlcompr = fmaxf(0.f, fminf(100.f, hlcompr));

  // now find brightness if gain didn't bring ave to midgray using
  // the envelope of the actual 'control cage' brightness curve for simplicity
  const float midtmp = gain * sqrtf(median * ave) / scale;

  if(midtmp < 0.1f)
  {
    bright = (midgray - midtmp) * 15.0f / (midtmp);
  }
  else
  {
    bright = (midgray - midtmp) * 15.0f / (0.10833 - 0.0833f * midtmp);
  }

  bright = 0.25f * MAX(0.f, bright);

  // compute contrast that spreads the average spacing of octiles
  contr = (midgray * 100.f) * (1.1f - ospread);
  contr = MAX(0.f, MIN(100.f, contr));
  // take gamma into account
  double whiteclipg = gamma2(whiteclip * corr);

  float gavg = 0.f;

  float val = 0.f;
  const float increment = corr * (1 << histcompr);

  for(int ii = 0; ii<65536>> histcompr; ii++)
  {
    // gavg += histogram[ii] * _get_LUTf(gamma2curve, gamma2curve_size, val);
    gavg += histogram[ii] * gamma2(val);
    val += increment;
  }

  gavg /= sum;

  if(black < gavg)
  {
    const int maxwhiteclip = (gavg - black) * 4 / 3
                             + black; // don't let whiteclip be so large that the histogram average goes above 3/4

    if(whiteclipg < maxwhiteclip)
    {
      whiteclipg = maxwhiteclip;
    }
  }

  whiteclipg
      = igamma2(whiteclipg); // need to inverse gamma transform to get correct exposure compensation parameter

  // correction with gamma
  black = (black / whiteclipg);

  expcomp = CLAMP(expcomp, -5.0f, 12.0f);

  bright = MAX(-100.f, MIN(bright, 100.f));

cleanup:
  black /= 100.f;
  bright /= 100.f;
  contr /= 100.f;

  if(isnan(expcomp))
  {
    expcomp = 0.f;
    fprintf(stderr, "[_get_auto_exp] expcomp is NaN!!!\n");
  }
  if(isnan(black))
  {
    black = 0.f;
    fprintf(stderr, "[_get_auto_exp] black is NaN!!!\n");
  }
  if(isnan(bright))
  {
    bright = 0.f;
    fprintf(stderr, "[_get_auto_exp] bright is NaN!!!\n");
  }
  if(isnan(contr))
  {
    contr = 0.f;
    fprintf(stderr, "[_get_auto_exp] contr is NaN!!!\n");
  }
  if(isnan(hlcompr))
  {
    hlcompr = 0.f;
    fprintf(stderr, "[_get_auto_exp] hlcompr is NaN!!!\n");
  }
  if(isnan(hlcomprthresh))
  {
    hlcomprthresh = 0.f;
    fprintf(stderr, "[_get_auto_exp] hlcomprthresh is NaN!!!\n");
  }

  *_expcomp = expcomp;
  *_black = black;
  *_bright = bright;
  *_contr = contr;
  *_hlcompr = hlcompr;
  *_hlcomprthresh = hlcomprthresh;
}

static void _auto_exposure(const float *const img, const int width, const int height, int *box_area,
                           const float clip, const float midgray, float *_expcomp, float *_bright, float *_contr,
                           float *_black, float *_hlcompr, float *_hlcomprthresh)
{
  uint32_t *histogram = NULL;
  unsigned int hist_size = 0;
  int histcompr = 0;

  const float defGain = 0.0f;

  _get_auto_exp_histogram(img, width, height, box_area, &histogram, &hist_size, &histcompr);
  _get_auto_exp(histogram, hist_size, histcompr, defGain, clip, midgray, _expcomp, _bright, _contr, _black,
                _hlcompr, _hlcomprthresh);

  if(histogram) dt_free_align(histogram);
}

static void _get_selected_area(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                               dt_iop_basicadj_gui_data_t *g, const dt_iop_roi_t *const roi_in, int *box_out)
{
  box_out[0] = box_out[1] = box_out[2] = box_out[3] = 0;

  if(g)
  {
    const int width = roi_in->width;
    const int height = roi_in->height;
    dt_boundingbox_t box_cood = { g->box_cood[0], g->box_cood[1], g->box_cood[2], g->box_cood[3] };

    box_cood[0] *= piece->pipe->iwidth;
    box_cood[1] *= piece->pipe->iheight;
    box_cood[2] *= piece->pipe->iwidth;
    box_cood[3] *= piece->pipe->iheight;

    dt_dev_distort_transform_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL,
                                  box_cood, 2);

    box_cood[0] *= roi_in->scale;
    box_cood[1] *= roi_in->scale;
    box_cood[2] *= roi_in->scale;
    box_cood[3] *= roi_in->scale;

    box_cood[0] -= roi_in->x;
    box_cood[1] -= roi_in->y;
    box_cood[2] -= roi_in->x;
    box_cood[3] -= roi_in->y;

    int box[4];

    // re-order edges of bounding box
    box[0] = fminf(box_cood[0], box_cood[2]);
    box[1] = fminf(box_cood[1], box_cood[3]);
    box[2] = fmaxf(box_cood[0], box_cood[2]);
    box[3] = fmaxf(box_cood[1], box_cood[3]);

    // do not continue if box is completely outside of roi
    if(!(box[0] >= width || box[1] >= height || box[2] < 0 || box[3] < 0))
    {
      // clamp bounding box to roi
      for(int k = 0; k < 4; k += 2) box[k] = MIN(width - 1, MAX(0, box[k]));
      for(int k = 1; k < 4; k += 2) box[k] = MIN(height - 1, MAX(0, box[k]));

      // safety check: area needs to have minimum 1 pixel width and height
      if(!(box[2] - box[0] < 1 || box[3] - box[1] < 1))
      {
        box_out[0] = box[0];
        box_out[1] = box[1];
        box_out[2] = box[2];
        box_out[3] = box[3];
      }
    }
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

  const int ch = piece->colors;
  dt_iop_basicadj_data_t *d = (dt_iop_basicadj_data_t *)piece->data;
  dt_iop_basicadj_params_t *p = (dt_iop_basicadj_params_t *)&d->params;
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;
  dt_iop_basicadj_global_data_t *gd = (dt_iop_basicadj_global_data_t *)self->global_data;

  cl_int err = CL_SUCCESS;

  float *src_buffer = NULL;

  cl_mem dev_gamma = NULL;
  cl_mem dev_contrast = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  // process auto levels
  if(g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
  {
    dt_iop_gui_enter_critical_section(self);
    if(g->call_auto_exposure == 1 && !darktable.gui->reset)
    {
      g->call_auto_exposure = -1;
      dt_iop_gui_leave_critical_section(self);

      // get the image, this works only in C
      src_buffer = dt_alloc_align_float((size_t)ch * width * height);
      if(src_buffer == NULL)
      {
        fprintf(stderr, "[basicadj process_cl] error allocating memory for color transformation 1\n");
        err = DT_OPENCL_SYSMEM_ALLOCATION;
        goto cleanup;
      }

      err = dt_opencl_copy_device_to_host(devid, src_buffer, dev_in, width, height, ch * sizeof(float));
      if(err != CL_SUCCESS)
      {
        fprintf(stderr, "[basicadj process_cl] error allocating memory for color transformation 2\n");
        goto cleanup;
      }

      memcpy(&g->params, p, sizeof(dt_iop_basicadj_params_t));

      int box[4] = { 0 };
      _get_selected_area(self, piece, g, roi_in, box);
      _auto_exposure(src_buffer, roi_in->width, roi_in->height, box, g->params.clip, g->params.middle_grey / 100.f,
                     &g->params.exposure, &g->params.brightness, &g->params.contrast, &g->params.black_point,
                     &g->params.hlcompr, &g->params.hlcomprthresh);

      dt_free_align(src_buffer);
      src_buffer = NULL;

      dt_iop_gui_enter_critical_section(self);
      g->call_auto_exposure = 2;
      dt_iop_gui_leave_critical_section(self);
    }
    else
    {
      dt_iop_gui_leave_critical_section(self);
    }
  }

  const int use_work_profile = (work_profile == NULL) ? 0 : 1;

  const int plain_contrast = (!p->preserve_colors && p->contrast != 0.f);
  const int preserve_colors = (p->contrast != 0.f) ? p->preserve_colors : 0;
  const int process_gamma = (p->brightness != 0.f);
  const int process_saturation_vibrance = (p->saturation != 0.f)||(p->vibrance != 0.f);
  const int process_hlcompr = (p->hlcompr > 0.f);

  const float black_point = p->black_point;
  const float hlcompr = p->hlcompr;
  const float hlcomprthresh = p->hlcomprthresh;
  const float saturation = p->saturation + 1.0f;
  const float vibrance = p->vibrance / 1.4f;
  const float contrast = p->contrast + 1.0f;
  const float white = exposure2white(p->exposure);
  const float scale = 1.0f / (white - p->black_point);
  const float middle_grey = (p->middle_grey > 0.f) ? (p->middle_grey / 100.f) : 0.1842f;
  const float inv_middle_grey = 1.f / middle_grey;
  const float brightness = p->brightness * 2.f;
  const float gamma = (brightness >= 0.0f) ? 1.0f / (1.0f + brightness) : (1.0f - brightness);

  const float hlcomp = hlcompr / 100.0f;
  const float shoulder = ((hlcomprthresh / 100.f) / 8.0f) + 0.1f;
  const float hlrange = 1.0f - shoulder;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto cleanup;

  dev_gamma = dt_opencl_copy_host_to_device(devid, d->lut_gamma, 256, 256, sizeof(float));
  if(dev_gamma == NULL)
  {
    fprintf(stderr, "[basicadj process_cl] error allocating memory 3\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_contrast = dt_opencl_copy_host_to_device(devid, d->lut_contrast, 256, 256, sizeof(float));
  if(dev_contrast == NULL)
  {
    fprintf(stderr, "[basicadj process_cl] error allocating memory 4\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 3, sizeof(int), (void *)&height);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 4, sizeof(cl_mem), (void *)&dev_gamma);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 5, sizeof(cl_mem), (void *)&dev_contrast);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 6, sizeof(float), (void *)&black_point);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 7, sizeof(float), (void *)&scale);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 8, sizeof(int), (void *)&process_gamma);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 9, sizeof(float), (void *)&gamma);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 10, sizeof(int), (void *)&plain_contrast);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 11, sizeof(int), (void *)&preserve_colors);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 12, sizeof(float), (void *)&contrast);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 13, sizeof(int), (void *)&process_saturation_vibrance);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 14, sizeof(float), (void *)&saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 15, sizeof(float), (void *)&vibrance);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 16, sizeof(int), (void *)&process_hlcompr);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 17, sizeof(float), (void *)&hlcomp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 18, sizeof(float), (void *)&hlrange);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 19, sizeof(float), (void *)&middle_grey);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 20, sizeof(float), (void *)&inv_middle_grey);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 21, sizeof(cl_mem), (void *)&dev_profile_info);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 22, sizeof(cl_mem), (void *)&dev_profile_lut);

  dt_opencl_set_kernel_arg(devid, gd->kernel_basicadj, 23, sizeof(int), (void *)&use_work_profile);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basicadj, sizes);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "[basicadj process_cl] error %i enqueue kernel\n", err);
    goto cleanup;
  }

cleanup:
  if(dev_gamma) dt_opencl_release_mem_object(dev_gamma);
  if(dev_contrast) dt_opencl_release_mem_object(dev_contrast);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);

  if(src_buffer) dt_free_align(src_buffer);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_basicadj] couldn't enqueue kernel! %s\n", cl_errstr(err));

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

  const int ch = piece->colors;
  dt_iop_basicadj_data_t *d = (dt_iop_basicadj_data_t *)piece->data;
  dt_iop_basicadj_params_t *p = (dt_iop_basicadj_params_t *)&d->params;
  dt_iop_basicadj_gui_data_t *g = (dt_iop_basicadj_gui_data_t *)self->gui_data;

  // process auto levels
  if(g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
  {
    dt_iop_gui_enter_critical_section(self);
    if(g->call_auto_exposure == 1 && !darktable.gui->reset)
    {
      g->call_auto_exposure = -1;
      dt_iop_gui_leave_critical_section(self);

      memcpy(&g->params, p, sizeof(dt_iop_basicadj_params_t));

      int box[4] = { 0 };
      _get_selected_area(self, piece, g, roi_in, box);
      _auto_exposure((const float *const)ivoid, roi_in->width, roi_in->height, box, g->params.clip,
                     g->params.middle_grey / 100.f, &g->params.exposure, &g->params.brightness,
                     &g->params.contrast, &g->params.black_point, &g->params.hlcompr, &g->params.hlcomprthresh);

      dt_iop_gui_enter_critical_section(self);
      g->call_auto_exposure = 2;
      dt_iop_gui_leave_critical_section(self);
    }
    else
    {
      dt_iop_gui_leave_critical_section(self);
    }
  }

  const float black_point = p->black_point;
  const float hlcompr = p->hlcompr;
  const float hlcomprthresh = p->hlcomprthresh;
  const float saturation = p->saturation + 1.0f;
  const float vibrance = p->vibrance / 1.4f;
  const float contrast = p->contrast + 1.0f;
  const float white = exposure2white(p->exposure);
  const float scale = 1.0f / (white - p->black_point);
  const float middle_grey = (p->middle_grey > 0.f) ? (p->middle_grey / 100.f) : 0.1842f;
  const float inv_middle_grey = 1.f / middle_grey;
  const float brightness = p->brightness * 2.f;
  const float gamma = (brightness >= 0.0f) ? 1.0f / (1.0f + brightness) : (1.0f - brightness);

  const float hlcomp = hlcompr / 100.0f;
  const float shoulder = ((hlcomprthresh / 100.f) / 8.0f) + 0.1f;
  const float hlrange = 1.0f - shoulder;

  const int plain_contrast = (!p->preserve_colors && p->contrast != 0.f);
  const int preserve_colors = (p->contrast != 0.f) ? p->preserve_colors : 0;
  const int process_gamma = (p->brightness != 0.f);
  const int process_saturation_vibrance = (p->saturation != 0.f)||(p->vibrance != 0.f);
  const int process_hlcompr = (p->hlcompr > 0.f);

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;
  const size_t stride = (size_t)roi_out->height * roi_out->width * ch;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(black_point, ch, contrast, gamma, hlcomp, hlrange, in, \
                      inv_middle_grey, middle_grey, out, plain_contrast, \
                      preserve_colors, process_hlcompr, process_gamma, \
                      process_saturation_vibrance, saturation, vibrance, \
                      scale, stride, work_profile) \
  shared(d) \
  schedule(static)
#endif
  for(size_t k = 0; k < stride; k += ch)
  {
    for(size_t c = 0; c < 3; c++)
    {
      // exposure
      out[k + c] = (in[k + c] - black_point) * scale;
    }

    // highlight compression
    if(process_hlcompr)
    {
      const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(out + k,
                                                                           work_profile->matrix_in,
                                                                           work_profile->lut_in,
                                                                           work_profile->unbounded_coeffs_in,
                                                                           work_profile->lutsize,
                                                                           work_profile->nonlinearlut)
                                       : dt_camera_rgb_luminance(out + k);
      if(lum > 0.f)
      {
        const float ratio = hlcurve(lum, hlcomp, hlrange);

        for(size_t c = 0; c < 3; c++)
        {
          out[k + c] = (ratio * out[k + c]);
        }
      }
    }

    for(size_t c = 0; c < 3; c++)
    {
      // gamma
      if(process_gamma && out[k + c] > 0.f) out[k + c] = get_lut_gamma(out[k + c], gamma, d->lut_gamma);

      // contrast
      if(plain_contrast && out[k + c] > 0.f)
        out[k + c] = get_lut_contrast(out[k + c], contrast, middle_grey, inv_middle_grey, d->lut_contrast);
    }

    // contrast (with preserve colors)
    if(preserve_colors != DT_RGB_NORM_NONE)
    {
      float ratio = 1.f;
      const float lum = dt_rgb_norm(out + k, preserve_colors, work_profile);
      if(lum > 0.f)
      {
        const float contrast_lum = powf(lum * inv_middle_grey, contrast) * middle_grey;
        ratio = contrast_lum / lum;
      }

      for(size_t c = 0; c < 3; c++)
      {
        out[k + c] = (ratio * out[k + c]);
      }
    }

    // saturation
    if(process_saturation_vibrance)
    {
      const float average = (out[k] + out[k+1] + out[k+2]) / 3;
      const float delta = sqrtf( (average-out[k])*(average-out[k])+(average-out[k+1])*(average-out[k+1])+(average-out[k+2])*(average-out[k+2]));
      const float P = vibrance * (1 - powf(delta, fabsf(vibrance)));

      for(size_t c = 0; c < 3; c++)
      {
        out[k + c] = average + (saturation + P) * (out[k+c] - average);
      }
    }

    out[k + 3] = in[k + 3];
  }
}

#undef exposure2white

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

