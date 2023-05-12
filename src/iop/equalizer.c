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
      for(int j = 0; j < height; j += step)
        for(int i = step / 2; i < width; i += step) out[(size_t)chs * width * j + (size_t)chs * i + ch] *= coeff;
      for(int j = step / 2; j < height; j += step)
        for(int i = 0; i < width; i += step) out[(size_t)chs * width * j + (size_t)chs * i + ch] *= coeff;
      for(int j = step / 2; j < height; j += step)
        for(int i = step / 2; i < width; i += step)
          out[(size_t)chs * width * j + (size_t)chs * i + ch] *= coeff * coeff;
    }
  }
  for(int level = numl_cap - 1; level > 0; level--) dt_iop_equalizer_iwtf(ovoid, tmp, level, width, height);

  for(int k = 1; k < numl_cap; k++) free(tmp[k]);
  free(tmp);
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
  module->default_enabled = FALSE; // we're a rather slow and rare op.
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

void gui_init(struct dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(equalizer);

  self->widget = dt_ui_label_new(_("this module will be removed in the future\nand is only here so you can "
                                   "switch it off\nand move to the new equalizer."));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

