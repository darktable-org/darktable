/*
    This file is part of darktable,
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


kernel void
dwt_add_img_to_layer(global float4 *img, global float4 *layer, int width, int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  layer[idx] += img[idx];
}

kernel void
dwt_subtract_layer(global float4 *bl, global float4 *bh, int width, int height, const float lpass_mult)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  bh[idx] -= bl[idx];
}

kernel void
dwt_hat_transform_row(global float4 *lpass, global float4 *hpass, int width, int height, const int sc)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(y >= height) return;

  const float hat_mult = 2.f;
  const int size = width;

  if(x < sc)
  {
    const int h_idx = mad24(y, width, x);

    lpass[h_idx] = hat_mult * hpass[mad24(y, width, x)] + hpass[mad24(y, width, (sc - x))]
                   + hpass[mad24(y, width, (x + sc))];
  }
  else if(x + sc < size)
  {
    const int h_idx = mad24(y, width, x);

    lpass[h_idx] = hat_mult * hpass[mad24(y, width, x)] + hpass[mad24(y, width, (x - sc))]
                   + hpass[mad24(y, width, (x + sc))];
  }
  else if(x < size)
  {
    const int h_idx = mad24(y, width, x);

    lpass[h_idx] = hat_mult * hpass[mad24(y, width, x)] + hpass[mad24(y, width, (x - sc))]
                   + hpass[mad24(y, width, (2 * size - 2 - (x + sc)))];
  }
}

kernel void
dwt_hat_transform_col(global float4 *lpass, int width, int height, const int sc,
                                  global float4 *temp_buffer, const float lpass_mult)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width) return;

  const float hat_mult = 2.f;
  const int size = height;

  if(y < sc)
  {
    temp_buffer[mad24(y, width, x)] = (hat_mult * lpass[mad24(y, width, x)] + lpass[mad24((sc - y), width, x)]
                                       + lpass[mad24((y + sc), width, x)])
                                      * lpass_mult;
  }
  else if(y + sc < size)
  {
    temp_buffer[mad24(y, width, x)] = (hat_mult * lpass[mad24(y, width, x)] + lpass[mad24((y - sc), width, x)]
                                       + lpass[mad24((y + sc), width, x)])
                                      * lpass_mult;
  }
  else if(y < size)
  {
    temp_buffer[mad24(y, width, x)] = (hat_mult * lpass[mad24(y, width, x)] + lpass[mad24((y - sc), width, x)]
                                       + lpass[mad24((2 * size - 2 - (y + sc)), width, x)])
                                      * lpass_mult;
  }
}

kernel void
dwt_init_buffer(global float4 *buffer, int width, int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  buffer[idx] = 0.f;
}
