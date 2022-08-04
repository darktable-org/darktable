/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.


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
#include "common/imagebuf.h"
#include "common/dwt.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

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
  float threshold; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.01 $DESCRIPTION: "Noise threshold"
  float x[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS];
  float y[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS]; // $DEFAULT: 0.5
} dt_iop_rawdenoise_params_t;

typedef struct dt_iop_rawdenoise_gui_data_t
{
  dt_draw_curve_t *transition_curve; // curve for gui to draw

  GtkWidget *threshold;
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
  return _("Raw denoise");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("Denoise the raw picture early in the pipeline"),
                                      _("Corrective"),
                                      _("Linear, raw, scene-referred"),
                                      _("Linear, raw"),
                                      _("Linear, raw, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

#define BIT16 65536.0

static void compute_channel_noise(float *const noise, int color, const dt_iop_rawdenoise_data_t *const data)
{
  // note that these constants are the same for X-Trans and Bayer, as they are proportional to image detail on
  // each channel, not the sensor pattern
  static const float noise_all[] = { 0.8002, 0.2735, 0.1202, 0.0585, 0.0291, 0.0152, 0.0080, 0.0044 };
  for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
  {
    // scale the value from [0,1] to [0,16],
    // and makes the "0.5" neutral value become 1
    float chan_threshold_exp_4;
    switch(color)
    {
    case 0:
      chan_threshold_exp_4 = data->force[DT_RAWDENOISE_R][DT_IOP_RAWDENOISE_BANDS - i - 1];
      break;
    case 2:
      chan_threshold_exp_4 = data->force[DT_RAWDENOISE_B][DT_IOP_RAWDENOISE_BANDS - i - 1];
      break;
    default:
      chan_threshold_exp_4 = data->force[DT_RAWDENOISE_G][DT_IOP_RAWDENOISE_BANDS - i - 1];
      break;
    }
    chan_threshold_exp_4 *= chan_threshold_exp_4;
    chan_threshold_exp_4 *= chan_threshold_exp_4;
    // repeat for the overall all-channels thresholds
    float all_threshold_exp_4 = data->force[DT_RAWDENOISE_ALL][DT_IOP_RAWDENOISE_BANDS - i - 1];
    all_threshold_exp_4 *= all_threshold_exp_4;
    all_threshold_exp_4 *= all_threshold_exp_4;
    noise[i] = noise_all[i] * all_threshold_exp_4 * chan_threshold_exp_4 * 16.0f * 16.0f;
    // the following multiplication needs to stay separate from the above line, because merging the two changes
    // the results on the integration test!
    noise[i] *= data->threshold;
  }
}

static void wavelet_denoise(const float *const restrict in, float *const restrict out, const dt_iop_roi_t *const roi,
                            const dt_iop_rawdenoise_data_t * const data, const uint32_t filters)
{
  const size_t size = (size_t)(roi->width / 2 + 1) * (roi->height / 2 + 1);
  float *const restrict fimg = dt_alloc_align_float(size);
  if (!fimg)
    return;

  const int nc = 4;
  for(int c = 0; c < nc; c++) /* denoise R,G1,B,G3 individually */
  {
    const int color = FC(c % 2, c / 2, filters);
    float noise[DT_IOP_RAWDENOISE_BANDS];
    compute_channel_noise(noise,color,data);

    // adjust for odd width and height
    const int halfwidth = roi->width / 2 + (roi->width & (~(c >> 1)) & 1);
    const int halfheight = roi->height / 2 + (roi->height & (~c) & 1);

    // collect one of the R/G1/G2/B channels into a monochrome image, applying sqrt() to the values as a
    // variance-stabilizing transform
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, fimg, roi, halfwidth) \
    shared(c) \
    schedule(static)
#endif
    for(int row = c & 1; row < roi->height; row += 2)
    {
      float *const restrict fimgp = fimg + (size_t)row / 2 * halfwidth;
      const int offset = (c & 2) >> 1;
      const float *const restrict inp = in + (size_t)row * roi->width + offset;
      const int senselwidth = (roi->width-offset+1)/2;
      for(int col = 0; col < senselwidth; col++)
        fimgp[col] = sqrtf(MAX(0.0f, inp[2*col]));
    }

    // perform the wavelet decomposition and denoising
    dwt_denoise(fimg,halfwidth,halfheight,DT_IOP_RAWDENOISE_BANDS,noise);

    // distribute the denoised data back out to the original R/G1/G2/B channel, squaring the resulting values to
    // undo the original transform
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(fimg, halfwidth, out, roi, size) \
    shared(c) \
    schedule(static)
#endif
    for(int row = c & 1; row < roi->height; row += 2)
    {
      const float *const restrict fimgp = fimg + (size_t)row / 2 * halfwidth;
      const int offset = (c & 2) >> 1;
      float *const restrict outp = out + (size_t)row * roi->width + offset;
      const int senselwidth = (roi->width-offset+1)/2;
      for(int col = 0; col < senselwidth; col++)
      {
        float d = fimgp[col];
        outp[2*col] = d * d;
      }
    }
  }
#if 0
  /* FIXME: Haven't ported this part yet */
  float maximum = 1.0;		/* FIXME */
  float black = 0.0;		/* FIXME */
  maximum *= BIT16;
  black *= BIT16;
  for (c=0; c<4; c++)
    cblack[c] *= BIT16;
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
        avg = avg > 0 ? sqrtf(avg) : 0;
        float diff = sqrtf(BAYER(row,col)) - avg;
        if      (diff < -thold) diff += thold;
        else if (diff >  thold) diff -= thold;
        else diff = 0;
        BAYER(row,col) = SQR(avg+diff);
      }
    }
  }
#endif
  dt_free_align(fimg);
}

static inline float vstransform(const float value)
{
  return sqrtf(MAX(0.0f, value));
}

static void wavelet_denoise_xtrans(const float *const restrict in, float *const restrict out,
                                   const dt_iop_roi_t *const restrict roi,
                                   const dt_iop_rawdenoise_data_t *const data, const uint8_t (*const xtrans)[6])
{
  const int width = roi->width;
  const int height = roi->height;
  const size_t size = (size_t)width * height;
  // allocate a buffer for the particular color channel to be denoise; we add two rows to simplify the
  // channel-extraction code (no special case for top/bottom row)
  float *const img = dt_alloc_align_float((size_t)width * (height+2));
  if (!img)
  {
    // we ran out of memory, so just pass through the image without denoising
    memcpy(out, in, sizeof(float) * size);
    return;
  }
  float *const fimg = img + width;	// point at the actual color channel contents in the buffer

  for(int c = 0; c < 3; c++)
  {
    float noise[DT_IOP_RAWDENOISE_BANDS];
    compute_channel_noise(noise, c, data);

    // ensure a defined value for every pixel in the top and bottom rows, even if they are more than
    // one pixel away from the nearest neighbor of the same color and thus the simple interpolation
    // used in the following loop does not set them
    for (size_t col = 0; col < width; col++)
    {
      fimg[col] = 0.5f;
      fimg[(size_t)(height-1)*width + col] = 0.5f;
    }
    const size_t nthreads = darktable.num_openmp_threads; // go direct, dt_get_num_threads() always returns numprocs
    const size_t chunksize = (height + nthreads - 1) / nthreads;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(fimg, height, in, roi, size, width, xtrans, nthreads, chunksize) \
    shared(c) num_threads(nthreads) \
    schedule(static)
#endif
    for(size_t chunk = 0; chunk < nthreads; chunk++)
    {
      const size_t start = chunk * chunksize;
      const size_t pastend = MIN(start + chunksize,height);
      for(size_t row = start; row < pastend; row++)
      {
        const float *const restrict inp = in + row * width;
        float *const restrict fimgp = fimg + row * width;
        // handle red/blue pixel in first column
        if (c != 1 && FCxtrans(row, 0, roi, xtrans) == c)
        {
          // copy to neighbors above and right
          const float d = vstransform(inp[0]);
          fimgp[0] = fimgp[-width] = fimgp[-width+1] = d;
        }
        for(size_t col = (c != 1); col < width-1; col++)
        {
          if (FCxtrans(row, col, roi, xtrans) == c)
          {
            // the pixel at the current location has the desired color, so apply sqrt() as a variance-stablizing
            // transform, and then do cheap nearest-neighbor interpolation by copying it to appropriate neighbors
            const float d = vstransform(inp[col]);
            fimgp[col] = d;
            if (c == 1) // green pixel
            {
              // Copy to the right and down.  The X-Trans color layout is such that copying to those two neighbors
              // results in all positions being filled except in the left-most and right-most columns and sometimes
              // the topmost and bottom-most rows (depending on how the ROI aligns with the CFA).
              fimgp[col+1] = fimgp[col+width] = d;
            }
            else // red or blue pixel
            {
              // Copy value to all eight neighbors; it's OK to copy to the row above even when we're in row 0 (or
              // the row below when in the last row) because the destination is sandwiched between other buffers
              // that will be overwritten afterwards anyway.  We need to copy to all adjacent positions because
              // there may be two green pixels between nearest red/red or blue/blue, so each will cover one of the
              // greens.
              fimgp[col-width-1] = fimgp[col-width] = fimgp[col-width+1] = d; // row above
              fimgp[col-1] = fimgp[col+1] = d;                                // left and right
              if (row < pastend-1)
                fimgp[col+width-1] = fimgp[col+width] = fimgp[col+width+1] = d; // row below
            }
          }
        }
        // leftmost and rightmost pixel in the row may still need to be filled in from a neighbor
        if (FCxtrans(row, 0, roi, xtrans) != c)
        {
          int src = 0;	// fallback is current sensel even if it has the wrong color
          if (row > 1 && FCxtrans(row-1, 0, roi, xtrans) == c)
            src = -width;
          else if (FCxtrans(row, 1, roi, xtrans) == c)
            src = 1;
          else if (row > 1 && FCxtrans(row-1, 1, roi, xtrans) == c)
            src = -width + 1;
          fimgp[0] = vstransform(inp[src]);
        }
        // check the right-most pixel; if it's the desired color and not green, copy it to the neighbors
        if (c != 1 && FCxtrans(row, width-1, roi, xtrans) == c)
        {
          // copy to neighbors above and left
          const float d = vstransform(inp[width-1]);
          fimgp[width-2] = fimgp[width-1] = fimgp[-1] = d;
        }
        else if (FCxtrans(row, width-1, roi, xtrans) != c)
        {
          int src = width-1;	// fallback is current sensel even if it has the wrong color
          if (FCxtrans(row, width-2, roi, xtrans) == c)
            src = width-2;
          else if (row > 1 && FCxtrans(row-1, width-1, roi, xtrans) == c)
            src = -1;
          else if (row > 1 && FCxtrans(row-1, width-2, roi, xtrans) == c)
            src = -2;
          fimgp[width-1] = vstransform(inp[src]);
        }
      }
      if (pastend < height)
      {
        // Another slice follows us, and by updating the last row of our slice, we've clobbered values that
        // were previously written by the other thread.  Restore them.
        const float *const restrict inp = in + pastend * width;
        float *const restrict fimgp = fimg + pastend * width;
        for (size_t col = 0; col < width-1; col++)
        {
          if (FCxtrans(pastend, col, roi, xtrans) == c)
          {
            const float d = vstransform(inp[col]);
            if (c == 1) // green pixel
            {
              if (FCxtrans(pastend, col+1, roi, xtrans) != c)
                fimgp[col] = fimgp[col+1] = d;  // copy to the right
            }
            else // red/blue pixel
            {
              // copy the pixel's adjusted value to the prior row and left and right (if not at edge)
              fimgp[col-width] = fimgp[col-width+1] = d;
              if (col > 0) fimgp[col-width-1] = d;
            }
          }
          // some red and blue values may need to be restored from the row TWO past the end of our slice
          if (c != 1 && pastend+1 < height && FCxtrans(pastend+1, col, roi, xtrans) == c)
          {
            const float d = vstransform(inp[col+width]);
            fimgp[col] = fimgp[col+1] = d;
            if (col > 0) fimgp[col-1] = d;
          }
        }
      }
    }

    // perform the wavelet decomposition and denoising
    dwt_denoise(fimg,width,height,DT_IOP_RAWDENOISE_BANDS,noise);

    // distribute the denoised data back out to the original R/G/B channel, squaring the resulting values to
    // undo the original transform
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(height, fimg, roi, width, xtrans, c) \
    dt_omp_sharedconst(out) \
    schedule(static)
#endif
    for(int row = 0; row < height; row++)
    {
      const float *const restrict fimgp = fimg + (size_t)row * width;
      float *const restrict outp = out + (size_t)row * width;
      for(int col = 0; col < width; col++)
        if(FCxtrans(row, col, roi, xtrans) == c)
        {
          float d = fimgp[col];
          outp[col] = d * d;
        }
    }
  }

  dt_free_align(img);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawdenoise_data_t *const restrict d = (dt_iop_rawdenoise_data_t *)piece->data;

  if(!(d->threshold > 0.0f))
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_in->width, roi_in->height, piece->colors);
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

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_rawdenoise_params_t *d = module->default_params;

  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
    {
      d->x[ch][k] = k / (DT_IOP_RAWDENOISE_BANDS - 1.f);
    }
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  // can't be switched on for non-raw images:
  module->hide_enable_button = !dt_image_is_raw(&module->dev->image_storage);

  if(module->widget)
  {
    gtk_stack_set_visible_child_name(GTK_STACK(module->widget), module->hide_enable_button ? "non_raw" : "raw");
  }

  module->default_enabled = 0;
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

  if (!(dt_image_is_raw(&pipe->image)))
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
  dt_iop_cancel_history_update(self);
  gtk_widget_queue_draw(self->widget);
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

  pango_layout_set_text(layout, _("Coarse"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .02 * width - ink.y, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);

  pango_layout_set_text(layout, _("Fine"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .98 * width - ink.height, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);


  pango_layout_set_text(layout, _("Smooth"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .08 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_layout_set_text(layout, _("Noisy"), -1);
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
    gtk_widget_queue_draw(widget);
    dt_iop_queue_history_update(self, FALSE);
  }
  else
  {
    c->x_move = -1;
    gtk_widget_queue_draw(widget);
  }
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
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    {
      p->x[ch][k] = d->x[ch][k];
      p->y[ch][k] = d->y[ch][k];
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
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

  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
    {
      //adjust aspect
      const int aspect = dt_conf_get_int("plugins/darkroom/rawdenoise/aspect_percent");
      dt_conf_set_int("plugins/darkroom/rawdenoise/aspect_percent", aspect + delta_y);
      dtgtk_drawing_area_set_aspect_ratio(widget, aspect / 100.0);
    }
    else
    {
      c->mouse_radius = CLAMP(c->mouse_radius * (1.0 + 0.1 * delta_y), 0.2 / DT_IOP_RAWDENOISE_BANDS, 1.0);
      gtk_widget_queue_draw(widget);
    }
  }

  return TRUE;
}

static void rawdenoise_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  c->channel = (dt_iop_rawdenoise_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *c = IOP_GUI_ALLOC(rawdenoise);
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->default_params;

  c->channel = dt_conf_get_int("plugins/darkroom/rawdenoise/gui_channel");
  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());
  dt_action_define_iop(self, NULL, N_("Channel"), GTK_WIDGET(c->channel_tabs), &dt_action_def_tabs_all_rgb);

  dt_ui_notebook_page(c->channel_tabs, N_("All"), NULL);
  dt_ui_notebook_page(c->channel_tabs, N_("R"), NULL);
  dt_ui_notebook_page(c->channel_tabs, N_("G"), NULL);
  dt_ui_notebook_page(c->channel_tabs, N_("B"), NULL);

  gtk_widget_show(gtk_notebook_get_nth_page(c->channel_tabs, c->channel));
  gtk_notebook_set_current_page(c->channel_tabs, c->channel);
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
  self->timeout_handle = 0;
  c->mouse_radius = 1.0 / (DT_IOP_RAWDENOISE_BANDS * 2);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  const float aspect = dt_conf_get_int("plugins/darkroom/rawdenoise/aspect_percent") / 100.0;
  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(aspect));
  g_object_set_data(G_OBJECT(c->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("Graph"), GTK_WIDGET(c->area), NULL);

  gtk_box_pack_start(GTK_BOX(box_raw), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box_raw), GTK_WIDGET(c->area), FALSE, FALSE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(rawdenoise_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(rawdenoise_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(rawdenoise_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(rawdenoise_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(rawdenoise_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(rawdenoise_scrolled), self);

  c->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  dt_bauhaus_slider_set_soft_max(c->threshold, 0.1);
  dt_bauhaus_slider_set_digits(c->threshold, 3);

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("Raw denoising\nOnly works for raw images."));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_conf_set_int("plugins/darkroom/rawdenoise/gui_channel", c->channel);
  dt_draw_curve_destroy(c->transition_curve);
  dt_iop_cancel_history_update(self);

  IOP_GUI_FREE;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

