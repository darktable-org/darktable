/*
    This file is part of darktable,
    copyright (c) 2011 bruce guenter
    copyright (c) 2012 henrik andersson


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
#include "common/darktable.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <strings.h>

DT_MODULE_INTROSPECTION(2, dt_iop_rawdenoise_params_t)

#define DT_IOP_RAWDENOISE_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_IOP_RAWDENOISE_RES 64
#define DT_IOP_RAWDENOISE_BANDS 5

typedef enum dt_iop_rawdenoise_channel_t
{
  DT_RAWDENOISE_ALL = 0,
  DT_RAWDENOISE_R = 1,
  DT_RAWDENOISE_G = 2,
  DT_RAWDENOISE_B = 3,
  DT_RAWDENOISE_NONE = 4
} dt_iop_rawdenoise_channel_t;

typedef struct dt_iop_rawdenoise_params_t
{
  float threshold;
  float x[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS], y[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS];
} dt_iop_rawdenoise_params_t;

typedef struct dt_iop_rawdenoise_gui_data_t
{
  GtkWidget *stack;
  dt_draw_curve_t *transition_curve; // curve for gui to draw

  GtkWidget *box_raw;
  GtkWidget *threshold;
  GtkWidget *label_non_raw;
  GtkDrawingArea *area;
  GtkNotebook *channel_tabs;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_rawdenoise_params_t drag_params;
  int dragging;
  int x_move;
  dt_iop_rawdenoise_channel_t channel;
  float draw_xs[DT_IOP_RAWDENOISE_RES], draw_ys[DT_IOP_RAWDENOISE_RES];
  float draw_min_xs[DT_IOP_RAWDENOISE_RES], draw_min_ys[DT_IOP_RAWDENOISE_RES];
  float draw_max_xs[DT_IOP_RAWDENOISE_RES], draw_max_ys[DT_IOP_RAWDENOISE_RES];
} dt_iop_rawdenoise_gui_data_t;

typedef struct dt_iop_rawdenoise_data_t
{
  float threshold;
  dt_draw_curve_t *curve[DT_RAWDENOISE_NONE];
  dt_iop_rawdenoise_channel_t channel;
  float force[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS];
} dt_iop_rawdenoise_data_t;

typedef struct dt_iop_rawdenoise_global_data_t
{
} dt_iop_rawdenoise_global_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    // Since first version, the dt_iop_params_t struct have new members
    // at the end of the struct.
    // Yet, the beginning of the struct is exactly the same:
    // threshold is still the first member of the struct.
    // This allows to define the variable o with dt_iop_rawdenoise_params_t
    // as long as we don't try to access new members on o.
    // In other words, o can be seen as a dt_iop_rawdenoise_params_t
    // with no allocated space for the new member.
    dt_iop_rawdenoise_params_t *o = (dt_iop_rawdenoise_params_t *)old_params;
    dt_iop_rawdenoise_params_t *n = (dt_iop_rawdenoise_params_t *)new_params;
    n->threshold = o->threshold;
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    {
      for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
      {
        n->x[ch][k] = k / (DT_IOP_RAWDENOISE_BANDS - 1.0);
        n->y[ch][k] = 0.5f;
      }
    }
    return 0;
  }
  return 1;
}


const char *name()
{
  return _("raw denoise");
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return dt_iop_get_group("raw denoise", IOP_GROUP_CORRECT);
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "noise threshold"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *g = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "noise threshold", GTK_WIDGET(g->threshold));
}

// transposes image, it is faster to read columns than to write them.
static void hat_transform(float *temp, const float *const base, int stride, int size, int scale)
{
  int i;
  const float *basep0;
  const float *basep1;
  const float *basep2;
  const size_t stxsc = (size_t)stride * scale;

  basep0 = base;
  basep1 = base + stxsc;
  basep2 = base + stxsc;

  for(i = 0; i < scale; i++, basep0 += stride, basep1 -= stride, basep2 += stride)
    temp[i] = (*basep0 + *basep0 + *basep1 + *basep2) * 0.25f;

  for(; i < size - scale; i++, basep0 += stride)
    temp[i] = ((*basep0) * 2 + *(basep0 - stxsc) + *(basep0 + stxsc)) * 0.25f;

  basep1 = basep0 - stxsc;
  basep2 = base + stride * (size - 2);

  for(; i < size; i++, basep0 += stride, basep1 += stride, basep2 -= stride)
    temp[i] = (*basep0 + *basep0 + *basep1 + *basep2) * 0.25f;
}

#define BIT16 65536.0

static void wavelet_denoise(const float *const in, float *const out, const dt_iop_roi_t *const roi,
                            dt_iop_rawdenoise_data_t *data, uint32_t filters)
{
  float threshold = data->threshold;
  int lev;
  float noise_all[] = { 0.8002, 0.2735, 0.1202, 0.0585, 0.0291, 0.0152, 0.0080, 0.0044 };
  for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
  {
    // scale the value from [0,1] to [0,16],
    // and makes the "0.5" neutral value become 1
    float threshold_exp_4 = data->force[DT_RAWDENOISE_ALL][DT_IOP_RAWDENOISE_BANDS - i - 1];
    threshold_exp_4 *= threshold_exp_4;
    threshold_exp_4 *= threshold_exp_4;
    noise_all[i] = noise_all[i] * threshold_exp_4 * 16.0;
  }

  const size_t size = (size_t)(roi->width / 2 + 1) * (roi->height / 2 + 1);
#if 0
  float maximum = 1.0;		/* FIXME */
  float black = 0.0;		/* FIXME */
  maximum *= BIT16;
  black *= BIT16;
  for (c=0; c<4; c++)
    cblack[c] *= BIT16;
#endif
  float *const fimg = calloc(size * 4, sizeof *fimg);


  const int nc = 4;
  for(int c = 0; c < nc; c++) /* denoise R,G1,B,G3 individually */
  {
    int color = FC(c % 2, c / 2, filters);
    float noise[DT_IOP_RAWDENOISE_BANDS];
    for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
    {
      float threshold_exp_4;
      switch(color)
      {
        case 0:
          threshold_exp_4 = data->force[DT_RAWDENOISE_R][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
        case 2:
          threshold_exp_4 = data->force[DT_RAWDENOISE_B][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
        default:
          threshold_exp_4 = data->force[DT_RAWDENOISE_G][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
      }
      threshold_exp_4 *= threshold_exp_4;
      threshold_exp_4 *= threshold_exp_4;
      noise[i] = noise_all[i] * threshold_exp_4 * 16.0;
    }

    // zero lowest quarter part
    memset(fimg, 0, size * sizeof(float));

    // adjust for odd width and height
    const int halfwidth = roi->width / 2 + (roi->width & (~(c >> 1)) & 1);
    const int halfheight = roi->height / 2 + (roi->height & (~c) & 1);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(c) schedule(static)
#endif
    for(int row = c & 1; row < roi->height; row += 2)
    {
      float *fimgp = fimg + size + (size_t)row / 2 * halfwidth;
      int col = (c & 2) >> 1;
      const float *inp = in + (size_t)row * roi->width + col;
      for(; col < roi->width; col += 2, fimgp++, inp += 2) *fimgp = sqrt(MAX(0, *inp));
    }

    int lastpass;

    for(lev = 0; lev < 5; lev++)
    {
      const size_t pass1 = size * ((lev & 1) * 2 + 1);
      const size_t pass2 = 2 * size;
      const size_t pass3 = 4 * size - pass1;

// filter horizontally and transpose
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lev) schedule(static)
#endif
      for(int col = 0; col < halfwidth; col++)
      {
        hat_transform(fimg + pass2 + (size_t)col * halfheight, fimg + pass1 + col, halfwidth, halfheight,
                      1 << lev);
      }
// filter vertically and transpose back
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lev) schedule(static)
#endif
      for(int row = 0; row < halfheight; row++)
      {
        hat_transform(fimg + pass3 + (size_t)row * halfwidth, fimg + pass2 + row, halfheight, halfwidth,
                      1 << lev);
      }

      const float thold = threshold * noise[lev];
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lev)
#endif
      for(size_t i = 0; i < (size_t)halfwidth * halfheight; i++)
      {
        float *fimgp = fimg + i;
        const float diff = fimgp[pass1] - fimgp[pass3];
        fimgp[0] += copysignf(fmaxf(fabsf(diff) - thold, 0.0f), diff);
      }

      lastpass = pass3;
    }
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(c, lastpass) schedule(static)
#endif
    for(int row = c & 1; row < roi->height; row += 2)
    {
      const float *fimgp = fimg + (size_t)row / 2 * halfwidth;
      int col = (c & 2) >> 1;
      float *outp = out + (size_t)row * roi->width + col;
      for(; col < roi->width; col += 2, fimgp++, outp += 2)
      {
        float d = fimgp[0] + fimgp[lastpass];
        *outp = d * d;
      }
    }
  }
#if 0
  /* FIXME: Haven't ported this part yet */
  if (filters && colors == 3)	/* pull G1 and G3 closer together */
  {
    float *window[4];
    int wlast, blk[2];
    float mul[2];
    float thold = threshold/512;
    for (row=0; row < 2; row++)
    {
      mul[row] = 0.125 * pre_mul[FC(row+1,0) | 1] / pre_mul[FC(row,0) | 1];
      blk[row] = cblack[FC(row,0) | 1];
    }
    for (i=0; i < 4; i++)
      window[i] = fimg + width*i;
    for (wlast=-1, row=1; row < height-1; row++)
    {
      while (wlast < row+1)
      {
        for (wlast++, i=0; i < 4; i++)
          window[(i+3) & 3] = window[i];
        for (col = FC(wlast,1) & 1; col < width; col+=2)
          window[2][col] = BAYER(wlast,col);
      }
      for (col = (FC(row,0) & 1)+1; col < width-1; col+=2)
      {
        float avg = ( window[0][col-1] + window[0][col+1] +
                      window[2][col-1] + window[2][col+1] - blk[~row & 1]*4 )
                    * mul[row & 1] + (window[1][col] + blk[row & 1]) * 0.5;
        avg = avg < 0 ? 0 : sqrt(avg);
        float diff = sqrt(BAYER(row,col)) - avg;
        if      (diff < -thold) diff += thold;
        else if (diff >  thold) diff -= thold;
        else diff = 0;
        BAYER(row,col) = SQR(avg+diff);
      }
    }
  }
#endif
  free(fimg);
}

static void wavelet_denoise_xtrans(const float *const in, float *out, const dt_iop_roi_t *const roi,
                                   dt_iop_rawdenoise_data_t *data, const uint8_t (*const xtrans)[6])
{
  float threshold = data->threshold;
  // note that these constants are the same for X-Trans and Bayer, as
  // they are proportional to image detail on each channel, not the
  // sensor pattern
  float noise_all[] = { 0.8002, 0.2735, 0.1202, 0.0585, 0.0291, 0.0152, 0.0080, 0.0044 };
  for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
  {
    // scale the value from [0,1] to [0,16],
    // and makes the "0.5" neutral value become 1
    float threshold_exp_4 = data->force[DT_RAWDENOISE_ALL][DT_IOP_RAWDENOISE_BANDS - i - 1];
    threshold_exp_4 *= threshold_exp_4;
    threshold_exp_4 *= threshold_exp_4;
    noise_all[i] = noise_all[i] * threshold_exp_4 * 16.0;
  }

  const int width = roi->width;
  const int height = roi->height;
  const size_t size = (size_t)width * height;
  float *const fimg = malloc((size_t)size * 4 * sizeof(float));

  for(int c = 0; c < 3; c++)
  {
    float noise[DT_IOP_RAWDENOISE_BANDS];
    for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
    {
      float threshold_exp_4;
      switch(c)
      {
        case 0:
          threshold_exp_4 = data->force[DT_RAWDENOISE_R][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
        case 2:
          threshold_exp_4 = data->force[DT_RAWDENOISE_B][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
        default:
          threshold_exp_4 = data->force[DT_RAWDENOISE_G][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
      }
      threshold_exp_4 *= threshold_exp_4;
      threshold_exp_4 *= threshold_exp_4;
      noise[i] = noise_all[i] * threshold_exp_4 * 16.0;
    }
    memset(fimg, 0, size * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(c) schedule(static)
#endif
    for(int row = (c != 1); row < height - 1; row++)
    {
      int col = (c != 1);
      const float *inp = in + (size_t)row * width + col;
      float *fimgp = fimg + size + (size_t)row * width + col;
      for(; col < width - 1; col++, inp++, fimgp++)
        if(FCxtrans(row, col, roi, xtrans) == c)
        {
          float d = sqrt(MAX(0, *inp));
          *fimgp = d;
          // cheap nearest-neighbor interpolate
          if(c == 1)
            fimgp[1] = fimgp[width] = d;
          else
          {
            fimgp[-width - 1] = fimgp[-width] = fimgp[-width + 1] = fimgp[-1] = fimgp[1] = fimgp[width - 1]
                = fimgp[width] = fimgp[width + 1] = d;
          }
        }
    }

    int lastpass;

    for(int lev = 0; lev < 5; lev++)
    {
      const size_t pass1 = size * ((lev & 1) * 2 + 1);
      const size_t pass2 = 2 * size;
      const size_t pass3 = 4 * size - pass1;

// filter horizontally and transpose
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lev) schedule(static)
#endif
      for(int col = 0; col < width; col++)
        hat_transform(fimg + pass2 + (size_t)col * height, fimg + pass1 + col, width, height, 1 << lev);
// filter vertically and transpose back
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lev) schedule(static)
#endif
      for(int row = 0; row < height; row++)
        hat_transform(fimg + pass3 + (size_t)row * width, fimg + pass2 + row, height, width, 1 << lev);

      const float thold = threshold * noise[lev];
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lev)
#endif
      for(size_t i = 0; i < size; i++)
      {
        float *fimgp = fimg + i;
        const float diff = fimgp[pass1] - fimgp[pass3];
        fimgp[0] += copysignf(fmaxf(fabsf(diff) - thold, 0.0f), diff);
      }

      lastpass = pass3;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(c, lastpass, out) schedule(static)
#endif
    for(int row = 0; row < height; row++)
    {
      const float *fimgp = fimg + (size_t)row * width;
      float *outp = out + (size_t)row * width;
      for(int col = 0; col < width; col++, outp++, fimgp++)
        if(FCxtrans(row, col, roi, xtrans) == c)
        {
          float d = fimgp[0] + fimgp[lastpass];
          *outp = d * d;
        }
    }
  }

  free(fimg);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;

  if(!(d->threshold > 0.0f))
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float)*width*height);
  }
  else
  {
    const uint32_t filters = piece->pipe->dsc.filters;
    const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
    if (filters != 9u)
      wavelet_denoise(ivoid, ovoid, roi_in, d, filters);
    else
      wavelet_denoise_xtrans(ivoid, ovoid, roi_in, d, xtrans);
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  // init defaults:
  dt_iop_rawdenoise_params_t tmp;
  tmp.threshold = 0.01;
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
    {
      tmp.x[ch][k] = k / (DT_IOP_RAWDENOISE_BANDS - 1.0);
      tmp.y[ch][k] = 0.5f;
    }
  }
  // we might be called from presets update infrastructure => there is no image
  if(!module->dev) goto end;

  // can't be switched on for non-raw images:
  if(dt_image_is_raw(&module->dev->image_storage))
    module->hide_enable_button = 0;
  else
    module->hide_enable_button = 1;
  module->default_enabled = 0;

end:
 memcpy(module->params, &tmp, sizeof(dt_iop_rawdenoise_params_t));
 memcpy(module->default_params, &tmp, sizeof(dt_iop_rawdenoise_params_t));
}

void init(dt_iop_module_t *module)
{
  module->data = NULL;
  module->params = calloc(1, sizeof(dt_iop_rawdenoise_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_rawdenoise_params_t));
  module->default_enabled = 0;

  // raw denoise must come just before demosaicing.
  module->priority = 99; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_rawdenoise_params_t);
  module->gui_data = NULL;
  dt_iop_rawdenoise_params_t tmp;
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
    {
      tmp.x[ch][k] = k / (DT_IOP_RAWDENOISE_BANDS - 1.0);
      tmp.y[ch][k] = 0.5f;
    }
  }
  tmp.threshold = 0.01f;
  memcpy(module->params, &tmp, sizeof(dt_iop_rawdenoise_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_rawdenoise_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)params;
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)piece->data;

  d->threshold = p->threshold;

  for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
  {
    dt_draw_curve_set_point(d->curve[ch], 0, p->x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p->y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(d->curve[ch], k, p->x[ch][k], p->y[ch][k]);
    dt_draw_curve_set_point(d->curve[ch], DT_IOP_RAWDENOISE_BANDS + 1, p->x[ch][1] + 1.0,
                            p->y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(d->curve[ch], 0.0, 1.0, DT_IOP_RAWDENOISE_BANDS, NULL, d->force[ch]);
  }

  if (!(pipe->image.flags & DT_IMAGE_RAW))
    piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)malloc(sizeof(dt_iop_rawdenoise_data_t));
  dt_iop_rawdenoise_params_t *default_params = (dt_iop_rawdenoise_params_t *)self->default_params;

  piece->data = (void *)d;
  for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->x[ch][k], default_params->y[ch][k]);
  }
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)(piece->data);
  for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *g = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;

  dt_bauhaus_slider_set(g->threshold, p->threshold);
  gtk_stack_set_visible_child_name(GTK_STACK(g->stack), self->hide_enable_button ? "non_raw" : "raw");
  gtk_widget_queue_draw(self->widget);
}

static void threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;
  p->threshold = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_rawdenoise_get_params(dt_iop_rawdenoise_params_t *p, const int ch, const double mouse_x,
                                         const double mouse_y, const float rad)
{
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->x[ch][k]) * (mouse_x - p->x[ch][k]) / (rad * rad));
    p->y[ch][k] = (1 - f) * p->y[ch][k] + f * mouse_y;
  }
}

static gboolean rawdenoise_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_iop_rawdenoise_params_t p = *(dt_iop_rawdenoise_params_t *)self->params;

  int ch = (int)c->channel;
  dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
  dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                          p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);

  const int inset = DT_IOP_RAWDENOISE_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  cairo_set_source_rgb(cr, .2, .2, .2);

  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_rawdenoise_get_params(&p, c->channel, c->mouse_x, 1., c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                            p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_RAWDENOISE_RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_rawdenoise_params_t *)self->params;
    dt_iop_rawdenoise_get_params(&p, c->channel, c->mouse_x, .0, c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                            p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_RAWDENOISE_RES, c->draw_max_xs, c->draw_max_ys);
  }

  cairo_save(cr);

  // draw selected cursor
  cairo_translate(cr, 0, height);

  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

  for(int i = 0; i < DT_RAWDENOISE_NONE; i++)
  {
    // draw curves, selected last
    ch = ((int)c->channel + i + 1) % DT_RAWDENOISE_NONE;
    float alpha = 0.3;
    if(i == DT_RAWDENOISE_NONE - 1) alpha = 1.0;
    switch(ch)
    {
      case 0:
        cairo_set_source_rgba(cr, .7, .7, .7, alpha);
        break;
      case 1:
        cairo_set_source_rgba(cr, .7, .1, .1, alpha);
        break;
      case 2:
        cairo_set_source_rgba(cr, .1, .7, .1, alpha);
        break;
      case 3:
        cairo_set_source_rgba(cr, .1, .1, .7, alpha);
        break;
    }

    p = *(dt_iop_rawdenoise_params_t *)self->params;
    dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                            p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_RAWDENOISE_RES, c->draw_xs, c->draw_ys);
    cairo_move_to(cr, 0 * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_ys[0]);
    for(int k = 1; k < DT_IOP_RAWDENOISE_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_ys[k]);
    cairo_stroke(cr);
  }

  ch = c->channel;
  // draw dots on knots
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    cairo_arc(cr, width * p.x[ch][k], -height * p.y[ch][k], DT_PIXEL_APPLY_DPI(3.0), 0.0, 2.0 * M_PI);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .7, .7, .7, .6);
    cairo_move_to(cr, 0, -height * c->draw_min_ys[0]);
    for(int k = 1; k < DT_IOP_RAWDENOISE_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_min_ys[k]);
    for(int k = DT_IOP_RAWDENOISE_RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_RAWDENOISE_RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_RAWDENOISE_RES - 1) k = DT_IOP_RAWDENOISE_RES - 2;
    float ht = -height * (f * c->draw_ys[k] + (1 - f) * c->draw_ys[k + 1]);
    cairo_arc(cr, c->mouse_x * width, ht, c->mouse_radius * width, 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  cairo_restore(cr);

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw labels:
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, (.08 * height) * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  cairo_set_source_rgb(cr, .1, .1, .1);

  pango_layout_set_text(layout, _("coarse"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .02 * width - ink.y, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);

  pango_layout_set_text(layout, _("fine"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .98 * width - ink.height, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);


  pango_layout_set_text(layout, _("smooth"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .08 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_layout_set_text(layout, _("noisy"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .97 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_font_description_free(desc);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean rawdenoise_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;
  const int inset = DT_IOP_RAWDENOISE_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
  if(c->dragging)
  {
    *p = c->drag_params;
    if(c->x_move < 0)
    {
      dt_iop_rawdenoise_get_params(p, c->channel, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    c->x_move = -1;
  }
  gtk_widget_queue_draw(widget);
  gint x, y;
#if GTK_CHECK_VERSION(3, 20, 0)
  gdk_window_get_device_position(
      event->window, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(event->window))), &x,
      &y, 0);
#else
  gdk_window_get_device_position(
      event->window,
      gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_window_get_display(event->window))),
      &x, &y, NULL);
#endif
  return TRUE;
}

static gboolean rawdenoise_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  const int ch = c->channel;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;
    dt_iop_rawdenoise_params_t *d = (dt_iop_rawdenoise_params_t *)self->default_params;
    /*   dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data; */
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    {
      p->x[ch][k] = d->x[ch][k];
      p->y[ch][k] = d->y[ch][k];
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(c->box_raw);
  }
  else if(event->button == 1)
  {
    c->drag_params = *(dt_iop_rawdenoise_params_t *)self->params;
    const int inset = DT_IOP_RAWDENOISE_INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
    c->mouse_pick
        = dt_draw_curve_calc_value(c->transition_curve, CLAMP(event->x - inset, 0, width) / (float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean rawdenoise_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean rawdenoise_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean rawdenoise_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;

  gdouble delta_y;
  if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
  {
    c->mouse_radius = CLAMP(c->mouse_radius * (1.0 + 0.1 * delta_y), 0.2 / DT_IOP_RAWDENOISE_BANDS, 1.0);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static void rawdenoise_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  c->channel = (dt_iop_rawdenoise_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_rawdenoise_gui_data_t));
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));
  c->stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(c->stack), FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), c->stack, TRUE, TRUE, 0);

  c->channel = dt_conf_get_int("plugins/darkroom/rawdenoise/gui_channel");
  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("all")));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("R")));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("G")));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("B")));

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(c->channel_tabs, c->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(c->channel_tabs), c->channel);
  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(rawdenoise_tab_switch), self);

  const int ch = (int)c->channel;
  c->transition_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(c->transition_curve, p->x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0,
                                p->y[ch][DT_IOP_RAWDENOISE_BANDS - 2]);
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    (void)dt_draw_curve_add_point(c->transition_curve, p->x[ch][k], p->y[ch][k]);
  (void)dt_draw_curve_add_point(c->transition_curve, p->x[ch][1] + 1.0, p->y[ch][1]);

  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0 / (DT_IOP_RAWDENOISE_BANDS * 2);

  c->box_raw = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(9.0 / 16.0));

  gtk_box_pack_start(GTK_BOX(c->box_raw), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(c->box_raw), GTK_WIDGET(c->area), FALSE, FALSE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(rawdenoise_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(rawdenoise_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(rawdenoise_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(rawdenoise_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(rawdenoise_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(rawdenoise_scrolled), self);

  c->threshold = dt_bauhaus_slider_new_with_range(self, 0.0, 0.1, 0.001, p->threshold, 3);
  gtk_box_pack_start(GTK_BOX(c->box_raw), GTK_WIDGET(c->threshold), TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(c->threshold, NULL, _("noise threshold"));
  g_signal_connect(G_OBJECT(c->threshold), "value-changed", G_CALLBACK(threshold_callback), self);

  c->label_non_raw = gtk_label_new(_("raw denoising\nonly works for raw images."));
  gtk_widget_set_halign(c->label_non_raw, GTK_ALIGN_START);

  // This is done so that if we use several instances, the newly created ones
  // use the same graphical interface as the original one.
  // In other words, if the original one is in "non_raw" mode, we have to put
  // "non_raw" in the stack first, so that when we add a new instance, we see
  // the label_non_raw
  if(self->hide_enable_button)
  {
    gtk_stack_add_named(GTK_STACK(c->stack), c->label_non_raw, "non_raw");
    gtk_stack_add_named(GTK_STACK(c->stack), c->box_raw, "raw");
  }
  else
  {
    gtk_stack_add_named(GTK_STACK(c->stack), c->box_raw, "raw");
    gtk_stack_add_named(GTK_STACK(c->stack), c->label_non_raw, "non_raw");
  }

  gtk_stack_set_visible_child_name(GTK_STACK(c->stack), self->hide_enable_button ? "non_raw" : "raw");
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->transition_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
