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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "common/debug.h"
#include "common/imagebuf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "iop/equalizer_eaw.h"

// #define DT_GUI_EQUALIZER_INSET 5
// #define DT_GUI_CURVE_INFL .3f

DT_MODULE_INTROSPECTION(1, dt_iop_equalizer_params_t)


#define DT_IOP_EQUALIZER_RES 64
#define DT_IOP_EQUALIZER_BANDS 6
#define DT_IOP_EQUALIZER_MAX_LEVEL 6

typedef struct dt_iop_equalizer_params_t
{
  float equalizer_x[3][DT_IOP_EQUALIZER_BANDS], equalizer_y[3][DT_IOP_EQUALIZER_BANDS];
} dt_iop_equalizer_params_t;

typedef enum dt_iop_equalizer_channel_t
{
  DT_IOP_EQUALIZER_L = 0,
  DT_IOP_EQUALIZER_a = 1,
  DT_IOP_EQUALIZER_b = 2
} dt_iop_equalizer_channel_t;

typedef struct dt_iop_equalizer_gui_data_t
{
  dt_draw_curve_t *minmax_curve; // curve for gui to draw
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkComboBox *presets;
  GtkRadioButton *channel_button[3];
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_equalizer_params_t drag_params;
  int dragging;
  int x_move;
  dt_iop_equalizer_channel_t channel;
  float draw_xs[DT_IOP_EQUALIZER_RES], draw_ys[DT_IOP_EQUALIZER_RES];
  float draw_min_xs[DT_IOP_EQUALIZER_RES], draw_min_ys[DT_IOP_EQUALIZER_RES];
  float draw_max_xs[DT_IOP_EQUALIZER_RES], draw_max_ys[DT_IOP_EQUALIZER_RES];
  float band_hist[DT_IOP_EQUALIZER_BANDS];
  float band_max;
} dt_iop_equalizer_gui_data_t;

typedef struct dt_iop_equalizer_data_t
{
  dt_draw_curve_t *curve[3];
  int num_levels;
} dt_iop_equalizer_data_t;


const char *name()
{
  return _("legacy equalizer");
}


int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_DEPRECATED;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char *deprecated_msg()
{
  return _("this module is deprecated. better use contrast equalizer module instead.");
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int chs = piece->colors;
  const int width = roi_in->width, height = roi_in->height;
  const float scale = roi_in->scale;
  dt_iop_image_copy_by_size(ovoid, ivoid, width, height, chs);
#if 1
  // printf("thread %d starting equalizer", (int)pthread_self());
  // if(piece->iscale != 1.0) printf(" for preview\n");
  // else printf("\n");
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)(piece->data);
  // dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;

  // 1 pixel in this buffer represents 1.0/scale pixels in original image:
  const float l1 = 1.0f + dt_log2f(piece->iscale / scale); // finest level
  float lm = 0;
  for(int k = MIN(width, height) * piece->iscale / scale; k; k >>= 1) lm++; // coarsest level
  lm = MIN(DT_IOP_EQUALIZER_MAX_LEVEL, l1 + lm);
  // level 1 => full resolution
  int numl = 0;
  for(int k = MIN(width, height); k; k >>= 1) numl++;
  const int numl_cap = MIN(DT_IOP_EQUALIZER_MAX_LEVEL - l1 + 1.5, numl);
  // printf("level range in %d %d: %f %f, cap: %d\n", 1, d->num_levels, l1, lm, numl_cap);

  // TODO: fixed alloc for data piece at capped resolution?
  float **tmp = (float **)calloc(numl_cap, sizeof(float *));
  for(int k = 1; k < numl_cap; k++)
  {
    const int wd = (int)(1 + (width >> (k - 1))), ht = (int)(1 + (height >> (k - 1)));
    tmp[k] = (float *)malloc(sizeof(float) * wd * ht);
  }

  for(int level = 1; level < numl_cap; level++) dt_iop_equalizer_wtf(ovoid, tmp, level, width, height);

#if 0
  // printf("transformed\n");
  // store luma wavelet histogram for later drawing
  if(self->dev->gui_attached && piece->iscale == 1.0 && self->dev->preview_pipe && c) // 1.0 => full pipe, only for gui applications.
  {
    float *out = (float *)ovoid;
    // chose full pipe and current window.
    int cnt[DT_IOP_EQUALIZER_BANDS];
    for(int i=0; i<DT_IOP_EQUALIZER_BANDS; i++) cnt[i] = 0;
    for(int l=1; l<numl_cap; l++)
    {
      const float lv = (lm-l1)*(l-1)/(float)(numl_cap-1) + l1; // appr level in real image.
      const int band = CLAMP(.5f + (1.0 - lv / d->num_levels) * (DT_IOP_EQUALIZER_BANDS), 0, DT_IOP_EQUALIZER_BANDS);
      c->band_hist[band] = 0.0f;
      cnt[band]++;
      int ch = (int)c->channel;
      {
        const int step = 1<<l;
        for(int j=0; j<height; j+=step)      for(int i=step/2; i<width; i+=step) c->band_hist[band] += out[chs*width*j + chs*i + ch]*out[chs*width*j + chs*i + ch];
        for(int j=step/2; j<height; j+=step) for(int i=0; i<width; i+=step)      c->band_hist[band] += out[chs*width*j + chs*i + ch]*out[chs*width*j + chs*i + ch];
        for(int j=step/2; j<height; j+=step) for(int i=step/2; i<width; i+=step) c->band_hist[band] += out[chs*width*j + chs*i + ch]*out[chs*width*j + chs*i + ch]*.5f;
      }
    }
    c->band_max = 0.0f;
    for(int i=0; i<DT_IOP_EQUALIZER_BANDS; i++)
    {
      if(cnt[i]) c->band_hist[i] /= cnt[i];
      else c->band_hist[i] = 0.0;
      c->band_max = fmaxf(c->band_max, c->band_hist[i]);
      // printf("band %d = %f\n", i, c->band_hist[i]);
    }
  }
#endif
  // printf("histogrammed\n");

  for(int l = 1; l < numl_cap; l++)
  {
    float *out = (float *)ovoid;
    const float lv = (lm - l1) * (l - 1) / (float)(numl_cap - 1) + l1; // appr level in real image.
    const float band = CLAMP((1.0 - lv / d->num_levels), 0, 1.0);
    for(int ch = 0; ch < 3; ch++)
    {
      // coefficients in range [0, 2], 1 being neutral.
      const float coeff = 2 * dt_draw_curve_calc_value(d->curve[ch == 0 ? 0 : 1], band);
      const int step = 1 << l;
#if 1 // scale coefficients
      for(int j = 0; j < height; j += step)
        for(int i = step / 2; i < width; i += step) out[(size_t)chs * width * j + (size_t)chs * i + ch] *= coeff;
      for(int j = step / 2; j < height; j += step)
        for(int i = 0; i < width; i += step) out[(size_t)chs * width * j + (size_t)chs * i + ch] *= coeff;
      for(int j = step / 2; j < height; j += step)
        for(int i = step / 2; i < width; i += step)
          out[(size_t)chs * width * j + (size_t)chs * i + ch] *= coeff * coeff;
#else // soft-thresholding (shrinkage)
#define wshrink                                                                                              \
  (copysignf(fmaxf(0.0f, fabsf(out[(size_t)chs * width * j + chs * i + ch]) - (1.0 - coeff)),                \
             out[(size_t)chs * width * j + chs * i + ch]))
      for(int j = 0; j < height; j += step)
        for(int i = step / 2; i < width; i += step) out[(size_t)chs * width * j + chs * i + ch] = wshrink;
      for(int j = step / 2; j < height; j += step)
        for(int i = 0; i < width; i += step) out[(size_t)chs * width * j + chs * i + ch] = wshrink;
      for(int j = step / 2; j < height; j += step)
        for(int i = step / 2; i < width; i += step) out[(size_t)chs * width * j + chs * i + ch] = wshrink;
#undef wshrink
#endif
    }
  }
  // printf("applied\n");
  for(int level = numl_cap - 1; level > 0; level--) dt_iop_equalizer_iwtf(ovoid, tmp, level, width, height);

  for(int k = 1; k < numl_cap; k++) free(tmp[k]);
  free(tmp);
// printf("thread %d finished equalizer", (int)pthread_self());
// if(piece->iscale != 1.0) printf(" for preview\n");
// else printf("\n");
#endif
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  // pull in new params to pipe
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)(piece->data);
  dt_iop_equalizer_params_t *p = (dt_iop_equalizer_params_t *)p1;

  for(int ch = 0; ch < 3; ch++)
    for(int k = 0; k < DT_IOP_EQUALIZER_BANDS; k++)
      dt_draw_curve_set_point(d->curve[ch], k, p->equalizer_x[ch][k], p->equalizer_y[ch][k]);
  int l = 0;
  for(int k = (int)MIN(pipe->iwidth * pipe->iscale, pipe->iheight * pipe->iscale); k; k >>= 1) l++;
  d->num_levels = MIN(DT_IOP_EQUALIZER_MAX_LEVEL, l);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)malloc(sizeof(dt_iop_equalizer_data_t));
  dt_iop_equalizer_params_t *default_params = (dt_iop_equalizer_params_t *)self->default_params;
  piece->data = (void *)d;
  for(int ch = 0; ch < 3; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    for(int k = 0; k < DT_IOP_EQUALIZER_BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->equalizer_x[ch][k],
                                    default_params->equalizer_y[ch][k]);
  }
  int l = 0;
  for(int k = (int)MIN(pipe->iwidth * pipe->iscale, pipe->iheight * pipe->iscale); k; k >>= 1) l++;
  d->num_levels = MIN(DT_IOP_EQUALIZER_MAX_LEVEL, l);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
// clean up everything again.
  dt_iop_equalizer_data_t *d = (dt_iop_equalizer_data_t *)(piece->data);
  for(int ch = 0; ch < 3; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  // nothing to do, gui curve is read directly from params during expose event.
  // gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_equalizer_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_equalizer_params_t));
  module->default_enabled = 0; // we're a rather slow and rare op.
  module->params_size = sizeof(dt_iop_equalizer_params_t);
  module->gui_data = NULL;
  dt_iop_equalizer_params_t *d = module->default_params;
  for(int ch = 0; ch < 3; ch++)
  {
    for(int k = 0; k < DT_IOP_EQUALIZER_BANDS; k++)
      d->equalizer_x[ch][k] = k / (float)(DT_IOP_EQUALIZER_BANDS - 1);
    for(int k = 0; k < DT_IOP_EQUALIZER_BANDS; k++) d->equalizer_y[ch][k] = 0.5f;
  }
}

#if 0
void init_presets (dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "begin", NULL, NULL, NULL);
  dt_iop_equalizer_params_t p;

  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
  {
    p.equalizer_x[DT_IOP_EQUALIZER_L][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_a][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_b][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_y[DT_IOP_EQUALIZER_L][k] = .5f+.5f*k/(float)DT_IOP_EQUALIZER_BANDS;
    p.equalizer_y[DT_IOP_EQUALIZER_a][k] = .5f;
    p.equalizer_y[DT_IOP_EQUALIZER_b][k] = .5f;
  }
  dt_gui_presets_add_generic(_("sharpen (strong)"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
  {
    p.equalizer_x[DT_IOP_EQUALIZER_L][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_a][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_b][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_y[DT_IOP_EQUALIZER_L][k] = .5f+.25f*k/(float)DT_IOP_EQUALIZER_BANDS;
    p.equalizer_y[DT_IOP_EQUALIZER_a][k] = .5f;
    p.equalizer_y[DT_IOP_EQUALIZER_b][k] = .5f;
  }
  dt_gui_presets_add_generic(C_("equalizer", "sharpen"), self->op, self->version(), &p, sizeof(p), 1);
  for(int ch=0; ch<3; ch++)
  {
    for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++) p.equalizer_x[ch][k] = k/(float)(DT_IOP_EQUALIZER_BANDS-1);
    for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++) p.equalizer_y[ch][k] = 0.5f;
  }
  dt_gui_presets_add_generic(_("null"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
  {
    p.equalizer_x[DT_IOP_EQUALIZER_L][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_a][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_b][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_y[DT_IOP_EQUALIZER_L][k] = .5f-.2f*k/(float)DT_IOP_EQUALIZER_BANDS;
    p.equalizer_y[DT_IOP_EQUALIZER_a][k] = fmaxf(0.0f, .5f-.3f*k/(float)DT_IOP_EQUALIZER_BANDS);
    p.equalizer_y[DT_IOP_EQUALIZER_b][k] = fmaxf(0.0f, .5f-.3f*k/(float)DT_IOP_EQUALIZER_BANDS);
  }
  dt_gui_presets_add_generic(_("denoise"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
  {
    p.equalizer_x[DT_IOP_EQUALIZER_L][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_a][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_x[DT_IOP_EQUALIZER_b][k] = k/(DT_IOP_EQUALIZER_BANDS-1.0);
    p.equalizer_y[DT_IOP_EQUALIZER_L][k] = .5f-.4f*k/(float)DT_IOP_EQUALIZER_BANDS;
    p.equalizer_y[DT_IOP_EQUALIZER_a][k] = fmaxf(0.0f, .5f-.6f*k/(float)DT_IOP_EQUALIZER_BANDS);
    p.equalizer_y[DT_IOP_EQUALIZER_b][k] = fmaxf(0.0f, .5f-.6f*k/(float)DT_IOP_EQUALIZER_BANDS);
  }
  dt_gui_presets_add_generic(_("denoise (strong)"), self->op, self->version(), &p, sizeof(p), 1);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "commit", NULL, NULL, NULL);
}
#endif

void gui_init(struct dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(equalizer);

  self->widget = dt_ui_label_new(_("this module will be removed in the future\nand is only here so you can "
                                   "switch it off\nand move to the new equalizer."));

#if 0
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  dt_iop_equalizer_params_t *p = (dt_iop_equalizer_params_t *)self->params;

  c->band_max = 0;
  c->channel = DT_IOP_EQUALIZER_L;
  int ch = (int)c->channel;
  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, HERMITE_SPLINE);
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++) (void)dt_draw_curve_add_point(c->minmax_curve, p->equalizer_x[ch][k], p->equalizer_y[ch][k]);
  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0/DT_IOP_EQUALIZER_BANDS;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  gtk_widget_set_size_request(GTK_WIDGET(c->area), 195, 195);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK | darktable.gui.scroll_mask);
  g_signal_connect (G_OBJECT (c->area), "draw",
                    G_CALLBACK (dt_iop_equalizer_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (dt_iop_equalizer_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (dt_iop_equalizer_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_equalizer_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_equalizer_leave_notify), self);
  g_signal_connect (G_OBJECT (c->area), "scroll-event",
                    G_CALLBACK (dt_iop_equalizer_scrolled), self);
  // init gtk stuff
  c->hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->hbox), FALSE, FALSE, 0);

  c->channel_button[0] = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label(NULL, _("luma")));
  c->channel_button[1] = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(c->channel_button[0], _("chroma")));
  // c->channel_button[2] = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(c->channel_button[0], "b"));

  g_signal_connect (G_OBJECT (c->channel_button[0]), "toggled",
                    G_CALLBACK (dt_iop_equalizer_button_toggled), self);
  g_signal_connect (G_OBJECT (c->channel_button[1]), "toggled",
                    G_CALLBACK (dt_iop_equalizer_button_toggled), self);
  // g_signal_connect (G_OBJECT (c->channel_button[2]), "toggled",
  //                   G_CALLBACK (dt_iop_equalizer_button_toggled), self);

  // gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->channel_button[2]), FALSE, FALSE, 5);
  gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->channel_button[1]), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->channel_button[0]), FALSE, FALSE, 0);
#endif
}

#if 0
static gboolean dt_iop_equalizer_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  // c->mouse_radius = 1.0/DT_IOP_EQUALIZER_BANDS;
  if(!c->dragging) c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// fills in new parameters based on mouse position (in 0,1)
static void dt_iop_equalizer_get_params(dt_iop_equalizer_params_t *p, const int ch, const double mouse_x, const double mouse_y, const float rad)
{
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->equalizer_x[ch][k])*(mouse_x - p->equalizer_x[ch][k])/(rad*rad));
    p->equalizer_y[ch][k] = (1-f)*p->equalizer_y[ch][k] + f*mouse_y;
  }
}

static gboolean dt_iop_equalizer_expose(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  dt_iop_equalizer_params_t p = *(dt_iop_equalizer_params_t *)self->params;
  int ch = (int)c->channel;
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
  const int inset = DT_GUI_EQUALIZER_INSET;
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_equalizer_get_params(&p, c->channel, c->mouse_x, 1., c->mouse_radius);
    for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_EQUALIZER_RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_equalizer_params_t *)self->params;
    dt_iop_equalizer_get_params(&p, c->channel, c->mouse_x, .0, c->mouse_radius);
    for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_EQUALIZER_RES, c->draw_max_xs, c->draw_max_ys);
  }

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  // draw x positions
  cairo_set_line_width(cr, 1.);
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  const float arrw = 7.0f;
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
  {
    cairo_move_to(cr, width*p.equalizer_x[c->channel][k], height+inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, 1.);
  cairo_translate(cr, 0, height);

  // draw frequency histogram in bg.
#if 1
  if(c->band_max > 0)
  {
    cairo_save(cr);
    cairo_scale(cr, width/(DT_IOP_EQUALIZER_BANDS-1.0), -(height-5)/c->band_max);
    cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
    cairo_move_to(cr, 0, 0);
    for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++) cairo_line_to(cr, k, c->band_hist[k]);
    cairo_line_to(cr, DT_IOP_EQUALIZER_BANDS-1.0, 0.);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
  }
#endif

  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, 2.);
  for(int i=0; i<3; i++)
  {
    // draw curves, selected last.
    int ch = ((int)c->channel+i+1)%3;
    if(ch == 2) continue;
    switch(ch)
    {
      case DT_IOP_EQUALIZER_L:
        cairo_set_source_rgba(cr, .6, .6, .6, .3);
        break;
      case DT_IOP_EQUALIZER_a:
        cairo_set_source_rgba(cr, .4, .2, .0, .4);
        break;
      default: //case DT_IOP_EQUALIZER_b:
        cairo_set_source_rgba(cr, 0., .2, .4, .4);
        break;
    }
    p = *(dt_iop_equalizer_params_t *)self->params;
    for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_EQUALIZER_RES, c->draw_xs, c->draw_ys);
    // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
    cairo_move_to(cr, 0, 0);
    for(int k=0; k<DT_IOP_EQUALIZER_RES; k++) cairo_line_to(cr, k*width/(float)(DT_IOP_EQUALIZER_RES-1), - height*c->draw_ys[k]);
    cairo_line_to(cr, width, 0);
    cairo_close_path(cr);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
  }

  // draw dots on knots
  cairo_save(cr);
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, 1.);
  for(int k=0; k<DT_IOP_EQUALIZER_BANDS; k++)
  {
    cairo_arc(cr, width*p.equalizer_x[c->channel][k], - height*p.equalizer_y[c->channel][k], 3.0, 0.0, 2.0*M_PI);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }
  cairo_restore(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    // cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, - height*c->draw_min_ys[0]);
    for(int k=1; k<DT_IOP_EQUALIZER_RES; k++)    cairo_line_to(cr, k*width/(float)(DT_IOP_EQUALIZER_RES-1), - height*c->draw_min_ys[k]);
    for(int k=DT_IOP_EQUALIZER_RES-2; k>=0; k--) cairo_line_to(cr, k*width/(float)(DT_IOP_EQUALIZER_RES-1), - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_EQUALIZER_RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_EQUALIZER_RES-1) k = DT_IOP_EQUALIZER_RES - 2;
    float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
    cairo_arc(cr, c->mouse_x*width, ht, c->mouse_radius*width, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  cairo_destroy(cr);
  cairo_set_source_surface (crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_equalizer_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  dt_iop_equalizer_params_t *p = (dt_iop_equalizer_params_t *)self->params;
  const int inset = DT_GUI_EQUALIZER_INSET;
  int height = allocation.height - 2*inset, width = allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width)/(float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
  if(c->dragging)
  {
    *p = c->drag_params;
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
      if(c->x_move > 0 && c->x_move < DT_IOP_EQUALIZER_BANDS-1)
      {
        const float minx = p->equalizer_x[c->channel][c->x_move-1] + 0.001f;
        const float maxx = p->equalizer_x[c->channel][c->x_move+1] - 0.001f;
        p->equalizer_x[c->channel][c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      dt_iop_equalizer_get_params(p, c->channel, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else if(event->y > height)
  {
    c->x_move = 0;
    float dist = fabsf(p->equalizer_x[c->channel][0] - c->mouse_x);
    for(int k=1; k<DT_IOP_EQUALIZER_BANDS; k++)
    {
      float d2 = fabsf(p->equalizer_x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        c->x_move = k;
        dist = d2;
      }
    }
  }
  else
  {
    c->x_move = -1;
  }
  gtk_widget_queue_draw(widget);

  return TRUE;
}

static gboolean dt_iop_equalizer_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  // set active point
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
    c->drag_params = *(dt_iop_equalizer_params_t *)self->params;
    const int inset = DT_GUI_EQUALIZER_INSET;
    int height = allocation.height - 2*inset, width = allocation.width - 2*inset;
    c->mouse_pick = dt_draw_curve_calc_value(c->minmax_curve, CLAMP(event->x - inset, 0, width)/(float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_equalizer_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_equalizer_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;

  gdouble delta_y;
  if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
  {
    c->mouse_radius = CLAMP(c->mouse_radius * (1.0 + 0.1 * delta_y), 0.25 / DT_IOP_EQUALIZER_BANDS, 1.0);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static void dt_iop_equalizer_button_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_equalizer_gui_data_t *c = (dt_iop_equalizer_gui_data_t *)self->gui_data;
  if(gtk_toggle_button_get_active(togglebutton))
  {
    for(int k=0; k<3; k++) if(c->channel_button[k] == GTK_RADIO_BUTTON(togglebutton))
      {
        c->channel = (dt_iop_equalizer_channel_t)k;
        gtk_widget_queue_draw(self->widget);
        return;
      }
  }
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

