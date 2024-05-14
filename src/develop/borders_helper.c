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

#include "common/darktable.h"
#include "develop/borders_helper.h"

// this will be called from inside an OpenMP parallel section, so no
// need to parallelize further
static inline void set_pixels(float *buf,
                              const dt_aligned_pixel_t color,
                              const int npixels)
{
  for(int i = 0; i < npixels; i++)
  {
    copy_pixel_nontemporal(buf + 4*i,  color);
  }
}

// this will be called from inside an OpenMP parallel section, so no
// need to parallelize further
static inline void copy_pixels(float *out,
                               const float *const in,
                               const int npixels)
{
  for(int i = 0; i < npixels; i++)
  {
    copy_pixel_nontemporal(out + 4*i, in + 4*i);
  }
}

void dt_iop_copy_image_with_border(float *out,
                                   const float *const in,
                                   const dt_iop_border_positions_t *binfo)
{
  const int image_width = binfo->image_right - binfo->image_left;
  DT_OMP_FOR()
  for(size_t row = 0; row < binfo->height; row++)
  {
    float *outrow = out + 4 * row * binfo->width;
    if(row < binfo->border_top || row >= binfo->border_bot)
    {
      // top/bottom border outside the frameline: entirely the border color
      set_pixels(outrow, binfo->bcolor, binfo->width);
    }
    else if(row < binfo->fl_top || row >= binfo->fl_bot)
    {
      // top/bottom frameline
      set_pixels(outrow, binfo->bcolor, binfo->border_left);
      set_pixels(outrow + 4*binfo->border_left, binfo->flcolor,
                 binfo->border_right - binfo->border_left);
      set_pixels(outrow + 4*binfo->border_right, binfo->bcolor,
                 binfo->width - binfo->border_right);
    }
    else if(row < binfo->image_top || row >= binfo->image_bot)
    {
      // top/bottom border inside the frameline
      set_pixels(outrow, binfo->bcolor, binfo->border_left);
      set_pixels(outrow + 4*binfo->border_left, binfo->flcolor,
                 binfo->fl_left - binfo->border_left);
      set_pixels(outrow + 4*binfo->fl_left, binfo->bcolor,
                 binfo->fl_right - binfo->fl_left);
      set_pixels(outrow + 4*binfo->fl_right, binfo->flcolor,
                 binfo->border_right - binfo->fl_right);
      set_pixels(outrow + 4*binfo->border_right, binfo->bcolor,
                 binfo->width - binfo->border_right);
    }
    else
    {
      // image area: set left border (w/optional frame line), copy
      // image row, set right border (w/optional frame line) set outer
      // border
      set_pixels(outrow, binfo->bcolor, binfo->border_left);
      if(binfo->image_left > binfo->border_left)
      {
        // we have a frameline, so set it and the inner border
        set_pixels(outrow + 4*binfo->border_left, binfo->flcolor,
                   binfo->fl_left - binfo->border_left);
        set_pixels(outrow + 4*binfo->fl_left, binfo->bcolor,
                   binfo->image_left - binfo->fl_left);
      }
      // copy image row
      copy_pixels(outrow + 4*binfo->image_left,
                  in + 4 * (row - binfo->image_top) * binfo->stride,
                  image_width);
      // set right border
      set_pixels(outrow + 4*binfo->image_right, binfo->bcolor,
                 binfo->fl_right - binfo->image_right);
      if(binfo->width > binfo->fl_right)
      {
        // we have a frameline, so set it and the outer border
        set_pixels(outrow + 4*binfo->fl_right, binfo->flcolor,
                   binfo->border_right - binfo->fl_right);
        set_pixels(outrow + 4*binfo->border_right, binfo->bcolor,
                   binfo->width - binfo->border_right);
      }
    }
  }
  // ensure that all streaming writes complete before we attempt to
  // read from the output buffer
  dt_omploop_sfence();
}

void dt_iop_setup_binfo(const dt_dev_pixelpipe_iop_t *piece,
                        const dt_iop_roi_t *const roi_in,
                        const dt_iop_roi_t *const roi_out,
                        const float pos_v,
                        const float pos_h,
                        const float *bcolor,
                        const float *fcolor,
                        const float f_size,
                        const float f_offset,
                        dt_iop_border_positions_t *binfo)
{
  const gboolean has_left   = pos_h > 0.0f;
  const gboolean has_right  = pos_h < 1.0f;
  const gboolean has_top    = pos_v > 0.0f;
  const gboolean has_bottom = pos_v < 1.0f;

  const int image_width  = roi_in->width;
  const int image_height = roi_in->height;

  const int border_tot_width =
    ceilf((piece->buf_out.width - piece->buf_in.width) * roi_in->scale);
  const int border_tot_height =
    ceilf((piece->buf_out.height - piece->buf_in.height) * roi_in->scale);

  binfo->border_size_t = has_top    ? border_tot_height * pos_v : 0;
  binfo->border_size_b = has_bottom ? border_tot_height - binfo->border_size_t : 0;
  binfo->border_size_l = has_left   ? border_tot_width * pos_h : 0;
  binfo->border_size_r = has_right  ? border_tot_width - binfo->border_size_l : 0;

  int image_right   = 0;
  int image_bottom  = 0;
  int border_in_x   = 0;
  int border_in_y   = 0;

  if(has_right)
  {
    border_in_x = CLAMP(binfo->border_size_l - roi_out->x, 0, roi_out->width);
    image_right = border_in_x + image_width;
  }
  else
  {
    image_right = roi_out->width;
    border_in_x = CLAMP(border_tot_width - roi_out->x, 0, roi_out->width);
  }

  if(has_bottom)
  {
    border_in_y  = CLAMP(binfo->border_size_t - roi_out->y, 0, roi_out->height);
    image_bottom = border_in_y + image_height;
  }
  else
  {
    image_bottom = roi_out->height;
    border_in_y  = CLAMP(border_tot_height - roi_out->y, 0, roi_out->height);
  }

  for(int c=0; c<3; c++)
  {
    binfo->bcolor[c] = bcolor[c];
    binfo->flcolor[c] = fcolor[c];
  }
  binfo->bcolor[3] = 1.0f;
  binfo->flcolor[3] = 1.0f;

  binfo->border_top   = border_in_y;
  binfo->fl_top       = border_in_y;
  binfo->image_top    = border_in_y;
  binfo->border_left  = border_in_x;
  binfo->fl_left      = border_in_x;
  binfo->image_left   = border_in_x;
  binfo->image_right  = image_right;
  binfo->fl_right     = roi_out->width;
  binfo->border_right = roi_out->width;
  binfo->width        = roi_out->width;
  binfo->image_bot    = image_bottom;
  binfo->fl_bot       = roi_out->height;
  binfo->border_bot   = roi_out->height;
  binfo->height       = roi_out->height;
  binfo->stride       = roi_in->width;
  binfo->border_in_x  = border_in_x;
  binfo->border_in_y  = border_in_y;

  // compute frame line parameters
  const int border_min_size = MIN(MIN(binfo->border_size_t,
                                      binfo->border_size_b),
                                  MIN(binfo->border_size_l,
                                      binfo->border_size_r));
  binfo->frame_size = border_min_size * f_size;

  if(binfo->frame_size > 0)
  {
    const int image_lx = binfo->border_size_l - roi_out->x;
    const int image_ty = binfo->border_size_t - roi_out->y;
    const int frame_space = border_min_size - binfo->frame_size;
    const int frame_offset = frame_space * f_offset;

    binfo->frame_tl_in_x = MAX(border_in_x - frame_offset, 0);
    binfo->frame_tl_out_x = MAX(binfo->frame_tl_in_x - binfo->frame_size, 0);
    binfo->frame_tl_in_y = MAX(border_in_y - frame_offset, 0);
    binfo->frame_tl_out_y = MAX(binfo->frame_tl_in_y - binfo->frame_size, 0);
    binfo->border_top = binfo->frame_tl_out_y;
    binfo->fl_top = binfo->frame_tl_in_y;
    binfo->border_left = CLAMP(binfo->frame_tl_out_x, 0, roi_out->width);
    binfo->fl_left = CLAMP(binfo->frame_tl_in_x, 0, roi_out->width);
    const int frame_in_width =
      floor((piece->buf_in.width * roi_in->scale) + frame_offset * 2);
    const int frame_in_height =
      floor((piece->buf_in.height * roi_in->scale) + frame_offset * 2);
    const int frame_out_width = frame_in_width + binfo->frame_size * 2;
    const int frame_out_height = frame_in_height + binfo->frame_size * 2;

    binfo->frame_br_in_x
      = CLAMP(image_lx - frame_offset + frame_in_width - 1,
              0, roi_out->width - 1);
    binfo->frame_br_in_y
      = CLAMP(image_ty - frame_offset + frame_in_height - 1,
              0, roi_out->height - 1);

    // ... if 100% frame_offset we ensure frame_line "stick" the out border
    binfo->frame_br_out_x
        = (f_offset == 1.0f
           && (MIN(binfo->border_size_l, binfo->border_size_r) - border_min_size < 2))
              ? (roi_out->width)
              : CLAMP(image_lx - frame_offset - binfo->frame_size + frame_out_width - 1,
                      0, roi_out->width - 1);
    binfo->frame_br_out_y
        = (f_offset == 1.0f
           && (MIN(binfo->border_size_t, binfo->border_size_b) - border_min_size < 2))
              ? (roi_out->height)
              : CLAMP(image_ty - frame_offset - binfo->frame_size + frame_out_height - 1,
                      0, roi_out->height - 1);

    // need end+1 for these coordinates
    binfo->fl_right     = binfo->frame_br_in_x;
    binfo->border_right = binfo->frame_br_out_x;
    binfo->fl_bot       = binfo->frame_br_in_y;
    binfo->border_bot   = binfo->frame_br_out_y;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
