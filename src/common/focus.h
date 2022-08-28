/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.
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

#pragma once

#include "common/image_cache.h"

typedef struct dt_focus_cluster_t
{
  int64_t n;
  float x, y, x2, y2;
  float thrs;
} dt_focus_cluster_t;

#define gbuf(BUF, A, B) ((BUF)[4 * (width * ((B)) + ((A))) + ch])
#define FOCUS_THRS 10
#define CHANNEL 1

static inline uint8_t _to_uint8(int i)
{
  return (uint8_t)CLAMP(i + 127, 0, 255);
}
static inline int _from_uint8(uint8_t i)
{
  return i - 127;
}
static inline void _dt_focus_cdf22_wtf(uint8_t *buf, const int l, const int width, const int height)
{
  const int ch = CHANNEL;

  const int step = 1 << l;
  const int st = step / 2;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, st, step, width, ch) \
  shared(buf) \
  schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    // rows
    // predict, get detail
    int i = st;
    for(; i < width - st; i += step) /*for(ch=0; ch<3; ch++)*/
      gbuf(buf, i, j)
          = _to_uint8((int)gbuf(buf, i, j) - ((int)gbuf(buf, i - st, j) + (int)gbuf(buf, i + st, j)) / 2);
    if(i < width) /*for(ch=0; ch<3; ch++)*/
      gbuf(buf, i, j) = _to_uint8(gbuf(buf, i, j) - gbuf(buf, i - st, j));
    // update coarse
    /*for(ch=0; ch<3; ch++)*/ gbuf(buf, 0, j) += _from_uint8(gbuf(buf, st, j)) / 2;
    for(i = step; i < width - st; i += step) /*for(ch=0; ch<3; ch++)*/
      gbuf(buf, i, j) += (_from_uint8(gbuf(buf, i - st, j)) + _from_uint8(gbuf(buf, i + st, j))) / 4;
    if(i < width) /*for(ch=0; ch<3; ch++)*/
      gbuf(buf, i, j) += _from_uint8(gbuf(buf, i - st, j)) / 2;
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, st, step, width, ch) \
  shared(buf) \
  schedule(static)
#endif
  for(int i = 0; i < width; i++)
  {
    // cols
    int j = st;
    // predict, get detail
    for(; j < height - st; j += step) /*for(ch=0; ch<3; ch++)*/
      gbuf(buf, i, j)
          = _to_uint8((int)gbuf(buf, i, j) - ((int)gbuf(buf, i, j - st) + (int)gbuf(buf, i, j + st)) / 2);
    if(j < height) /*for(int ch=0; ch<3; ch++)*/
      gbuf(buf, i, j) = _to_uint8((int)gbuf(buf, i, j) - (int)gbuf(buf, i, j - st));
    // update
    /*for(ch=0; ch<3; ch++)*/ gbuf(buf, i, 0) += _from_uint8(gbuf(buf, i, st)) / 2;
    for(j = step; j < height - st; j += step) /*for(ch=0; ch<3; ch++)*/
      gbuf(buf, i, j) += (_from_uint8(gbuf(buf, i, j - st)) + _from_uint8(gbuf(buf, i, j + st))) / 4;
    if(j < height) /*for(int ch=0; ch<3; ch++)*/
      gbuf(buf, i, j) += _from_uint8(gbuf(buf, i, j - st)) / 2;
  }
}

static void _dt_focus_update(dt_focus_cluster_t *f, int frows, int fcols, int i, int j, int wd, int ht,
                             int diff)
{
  const int32_t thrs = FOCUS_THRS;
  if(diff > thrs)
  {
    int fx = i / (float)wd * fcols;
    int fy = j / (float)ht * frows;
    int fi = fcols * fy + fx;
#ifdef _OPENMP
#pragma omp atomic
#endif
    f[fi].x += i;
#ifdef _OPENMP
#pragma omp atomic
#endif
    f[fi].y += j;
#ifdef _OPENMP
#pragma omp atomic
#endif
    f[fi].x2 += (float)i * i;
#ifdef _OPENMP
#pragma omp atomic
#endif
    f[fi].y2 += (float)j * j;
#ifdef _OPENMP
#pragma omp atomic
#endif
    f[fi].n++;
#ifdef _OPENMP
#pragma omp atomic
#endif
    f[fi].thrs += diff;
  }
}


// read 8-bit buffer and create focus clusters from it
static void dt_focus_create_clusters(dt_focus_cluster_t *focus, int frows, int fcols, uint8_t *buffer,
                                     int buffer_width, int buffer_height)
{
  // mark in-focus pixels:
  const int wd = buffer_width;
  const int ht = buffer_height;
  const int fs = frows * fcols;
  // two-stage cdf 2/2 wavelet transform, use HH1 and HH2 to detect very sharp and sharp spots:
  // pretend we already did the first step (coarse will stay in place, maybe even where the pre-demosaic
  // sample was at)
  _dt_focus_cdf22_wtf(buffer, 2, wd, ht);
  // go through HH1 and detect sharp clusters:
  memset(focus, 0, sizeof(dt_focus_cluster_t) * fcols * frows);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(shared)
#endif
  for(int j = 0; j < ht - 1; j += 4)
    for(int i = 0; i < wd - 1; i += 4)
    {
      _dt_focus_update(focus, frows, fcols, i, j, wd, ht,
                       abs(_from_uint8(buffer[4 * ((j + 2) * wd + i) + CHANNEL])));
      _dt_focus_update(focus, frows, fcols, i, j, wd, ht,
                       abs(_from_uint8(buffer[4 * (j * wd + i + 2) + CHANNEL])));
    }

#if 1 // second pass, HH2
  int num_clusters = 0;
  for(int k = 0; k < fs; k++)
    if(focus[k].n * 4 > wd * ht / (float)fs * 0.01f) num_clusters++;
  if(num_clusters < 1)
  {
    memset(focus, 0, sizeof(dt_focus_cluster_t) * fs);
    _dt_focus_cdf22_wtf(buffer, 3, wd, ht);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(shared)
#endif
    for(int j = 0; j < ht - 1; j += 8)
    {
      for(int i = 0; i < wd - 1; i += 8)
      {
        _dt_focus_update(focus, frows, fcols, i, j, wd, ht,
                         1.5 * abs(_from_uint8(buffer[4 * ((j + 4) * wd + i) + CHANNEL])));
        _dt_focus_update(focus, frows, fcols, i, j, wd, ht,
                         1.5 * abs(_from_uint8(buffer[4 * (j * wd + i + 4) + CHANNEL])));
      }
    }
    num_clusters = 0;
    for(int k = 0; k < fs; k++)
    {
      if(focus[k].n * 6.0f > wd * ht / (float)fs * 0.01f)
      {
        focus[k].n *= -1;
        num_clusters++;
      }
    }
  }
#endif
#undef CHANNEL

#if 0 // simple high pass filter, doesn't work on slightly unsharp/high iso images
  memset(focus, 0, sizeof(dt_focus_cluster_t)*fs);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(shared)
#endif
  for(int j=1;j<ht-1;j++)
  {
    int index = 4*j*wd+4;
    for(int i=1;i<wd-1;i++)
    {
      int32_t diff = 4*buffer[index+1]
        - buffer[index-4+1]
        - buffer[index+4+1]
        - buffer[index-4*wd+1]
        - buffer[index+4*wd+1];
      _dt_focus_update(focus, frows, fcols, i, j, wd, ht, abs(diff));
      index += 4;
    }
  }
#endif
  // normalize data in clusters:
  for(int k = 0; k < fs; k++)
  {
    focus[k].thrs /= fabsf((float)focus[k].n);
    focus[k].x /= fabsf((float)focus[k].n);
    focus[k].x2 /= fabsf((float)focus[k].n);
    focus[k].y /= fabsf((float)focus[k].n);
    focus[k].y2 /= fabsf((float)focus[k].n);
  }
}

static void dt_focus_draw_clusters(cairo_t *cr, int width, int height, int imgid, int buffer_width,
                                   int buffer_height, dt_focus_cluster_t *focus, int frows, int fcols,
                                   float full_zoom, float full_x, float full_y)
{
  const int fs = frows * fcols;
  cairo_save(cr);
  cairo_translate(cr, width / 2.0, height / 2.0f);

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  dt_image_t image = *img;
  dt_image_cache_read_release(darktable.image_cache, img);

  // FIXME: get those from rawprepare IOP somehow !!!
  int wd = buffer_width + image.crop_x;
  int ht = buffer_height + image.crop_y;

  // array with cluster positions
  float *pos = malloc(fs * 6 * sizeof(float));
  float *offx = pos + fs * 2, *offy = pos + fs * 4;

  for(int k = 0; k < fs; k++)
  {
    const float stddevx = sqrtf(focus[k].x2 - focus[k].x * focus[k].x);
    const float stddevy = sqrtf(focus[k].y2 - focus[k].y * focus[k].y);

    // FIXME: get those from rawprepare IOP somehow !!!
    const float x = focus[k].x + image.crop_x;
    const float y = focus[k].y + image.crop_y;

    pos[2 * k + 0] = x;
    pos[2 * k + 1] = y;
    offx[2 * k + 0] = x + stddevx;
    offx[2 * k + 1] = y;
    offy[2 * k + 0] = x;
    offy[2 * k + 1] = y + stddevy;
  }

  // could use dt_image_altered() here, but it ignores flip module
  {
    dt_develop_t dev;
    dt_dev_init(&dev, 0);
    dt_dev_load_image(&dev, imgid);
    dt_dev_pixelpipe_t pipe;
    const gboolean res = dt_dev_pixelpipe_init_dummy(&pipe, wd, ht);
    if(res)
    {
      // set mem pointer to 0, won't be used.
      dt_dev_pixelpipe_set_input(&pipe, &dev, NULL, wd, ht, 1.0f);
      dt_dev_pixelpipe_create_nodes(&pipe, &dev);
      dt_dev_pixelpipe_synch_all(&pipe, &dev);
      dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width,
                                      &pipe.processed_height);
      dt_dev_distort_transform_plus(&dev, &pipe, 0.f, DT_DEV_TRANSFORM_DIR_ALL, pos, fs * 3);
      dt_dev_pixelpipe_cleanup(&pipe);
      wd = pipe.processed_width;
      ht = pipe.processed_height;
    }
    dt_dev_cleanup(&dev);
  }

  const int32_t tb = darktable.develop->border_size;
  const float prev_scale = darktable.develop->preview_downsampling;
  const float scale = fminf((width - 2 * tb) / (float)wd, (height - 2 * tb) / (float)ht) * full_zoom / prev_scale;
  cairo_scale(cr, scale, scale);
  float fx = 0.0f;
  float fy = 0.0f;
  if(full_zoom > 1.0f)
  {
    // we want to be sure the image stay in the window
    fx = fminf((wd * scale - width) / 2, fabsf(full_x));
    if(full_x < 0) fx = -fx;
    if(wd * scale <= width) fx = 0;
    fy = fminf((ht * scale - height) / 2, fabsf(full_y));
    if(full_y < 0) fy = -fy;
    if(ht * scale <= height) fy = 0;
  }

  cairo_translate(cr, -wd / 2.0f * prev_scale + fx / scale * darktable.gui->ppd_thb, -ht / 2.0f * prev_scale + fy / scale * darktable.gui->ppd_thb);

  cairo_rectangle(cr, 0, 0, wd, ht);
  cairo_clip(cr);

  double dashes[] = { 3 };
  const int ndash = sizeof(dashes) / sizeof(dashes[0]);
  double offset = 0.0f;
  cairo_set_dash(cr, dashes, ndash, offset);

  // draw clustered focus regions
  for(int k = 0; k < fs; k++)
  {
    const float intens = (focus[k].thrs - FOCUS_THRS) / FOCUS_THRS;
    const float col = fminf(1.0f, intens);
    int draw = 0;
    if(focus[k].n * 4.0f > buffer_width * buffer_height / (float)fs * 0.01f)
      draw = 1;
    else if(-focus[k].n * 6.0f > buffer_width * buffer_height / (float)fs * 0.01f)
      draw = 2;
    if(draw)
    {
      for(int i = 0; i < 2; i++)
      {
        if(i)
        {
          if(draw == 2)
            cairo_set_source_rgb(cr, .1f, .1f, col);
          else
            cairo_set_source_rgb(cr, col, .1f, .1f);
          cairo_set_dash(cr, dashes, ndash, dashes[0]);
        }
        else
        {
          cairo_set_source_rgb(cr, .1f, .1f, .1f);
          cairo_set_dash(cr, dashes, ndash, 0);
        }
        cairo_move_to(cr, offx[2 * k + 0], offx[2 * k + 1]);
        cairo_curve_to(cr, -pos[2 * k + 0] + offx[2 * k + 0] + offy[2 * k + 0],
                       -pos[2 * k + 1] + offx[2 * k + 1] + offy[2 * k + 1],
                       -pos[2 * k + 0] + offx[2 * k + 0] + offy[2 * k + 0],
                       -pos[2 * k + 1] + offx[2 * k + 1] + offy[2 * k + 1], offy[2 * k + 0], offy[2 * k + 1]);
        cairo_curve_to(cr, pos[2 * k + 0] - offx[2 * k + 0] + offy[2 * k + 0],
                       pos[2 * k + 1] - offx[2 * k + 1] + offy[2 * k + 1],
                       pos[2 * k + 0] - offx[2 * k + 0] + offy[2 * k + 0],
                       pos[2 * k + 1] - offx[2 * k + 1] + offy[2 * k + 1],
                       2 * pos[2 * k + 0] - offx[2 * k + 0], 2 * pos[2 * k + 1] - offx[2 * k + 1]);
        cairo_curve_to(cr, 3 * pos[2 * k + 0] - offx[2 * k + 0] - offy[2 * k + 0],
                       3 * pos[2 * k + 1] - offx[2 * k + 1] - offy[2 * k + 1],
                       3 * pos[2 * k + 0] - offx[2 * k + 0] - offy[2 * k + 0],
                       3 * pos[2 * k + 1] - offx[2 * k + 1] - offy[2 * k + 1],
                       2 * pos[2 * k + 0] - offy[2 * k + 0], 2 * pos[2 * k + 1] - offy[2 * k + 1]);
        cairo_curve_to(cr, pos[2 * k + 0] + offx[2 * k + 0] - offy[2 * k + 0],
                       pos[2 * k + 1] + offx[2 * k + 1] - offy[2 * k + 1],
                       pos[2 * k + 0] + offx[2 * k + 0] - offy[2 * k + 0],
                       pos[2 * k + 1] + offx[2 * k + 1] - offy[2 * k + 1], offx[2 * k + 0], offx[2 * k + 1]);

        cairo_save(cr);
        cairo_scale(cr, 1. / scale, 1. / scale);
        cairo_set_line_width(cr, 2.0f);
        cairo_stroke(cr);
        cairo_restore(cr);
      }
    }
  }
  cairo_restore(cr);
  free(pos);
}
#undef CHANNEL
#undef gbuf
#undef FOCUS_THRS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

