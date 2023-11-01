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

#include "common/dttypes.h"
#include "develop/imageop.h"

typedef struct dt_iop_border_positions_t
{
  dt_aligned_pixel_t bcolor;
  dt_aligned_pixel_t flcolor;
  int border_top;		// 0..bt is rows of top border outside the frameline
  int fl_top;			//bt..ft is the top frameline
  int image_top;		//ft..it is the top border inside the frameline
  int border_left;		// 0..bl is columns of left border outside the frameline
  int fl_left;			//bl..fl is the left frameline
  int image_left;		//fl..il is the left border inside the frameline
  int image_right;		//il..ir is the actual image area
  int fl_right;			//ir..fr is the right border inside the frameline
  int border_right;		//fr..br is the right frameeline
  int width;			//br..width is the right border outside the frameline
  int image_bot;		//it..ib is the actual image area
  int fl_bot;			//ib..fb is the bottom border inside the frameline
  int border_bot;		//fb..bt is the frameline
  int height;			//bt..height is the bottom border outside the frameline
  int stride;			// width of input roi

  int border_in_x;
  int border_in_y;

  int border_size_t;  // border size top
  int border_size_b;  //             bottom
  int border_size_l;  //             left
  int border_size_r;  //             right

  int frame_size;     // size of the internal frame

  int frame_tl_in_x;  // the frame start/end
  int frame_tl_out_x; //  in  : inside pos  (from image to frame)
  int frame_tl_in_y;  //  out : outside pos (from frame to border)
  int frame_tl_out_y;

  int frame_br_in_x;  // the border start/end
  int frame_br_out_x;
  int frame_br_in_y;
  int frame_br_out_y;
} dt_iop_border_positions_t;

void dt_iop_copy_image_with_border(float *out,
                                   const float *const in,
                                   const dt_iop_border_positions_t *binfo);

void dt_iop_setup_binfo(const dt_dev_pixelpipe_iop_t *piece,
                        const dt_iop_roi_t *const roi_in,
                        const dt_iop_roi_t *const roi_out,
                        const float pos_v,
                        const float pos_h,
                        const float *bcolor,
                        const float *fcolor,
                        const float f_size,
                        const float f_offset,
                        dt_iop_border_positions_t *binfo);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
