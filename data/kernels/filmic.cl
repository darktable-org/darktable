/*
    This file is part of darktable,
    copyright (c) 2018 Aurélien Pierre.

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

#include "basic.cl"

kernel void
filmic (read_only image2d_t in, write_only image2d_t out, int width, int height,
        const float dynamic_range, const float shadows_range, const float grey,
        read_only image2d_t table, read_only image2d_t diff, const float saturation,
        const float contrast, const float power)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float4 o = Lab_to_XYZ(i);
  o = XYZ_to_prophotorgb(o);

  const float4 noise = pow((float4)2.0f, (float4)-16.0f);
  const float4 dynamic4 = dynamic_range;
  const float4 shadows4 = shadows_range;
  const float4 grey4 = grey;

  // Log profile
  o = o / grey;
  o = (o < noise) ? noise : o;
  o = (log2(o) - shadows4) / dynamic4;
  o = clamp(o, (float4)0.0f, (float4)1.0f);

  const float4 index = o;

  // Curve S LUT
  o.x = lookup(table, (const float)o.x);
  o.y = lookup(table, (const float)o.y);
  o.z = lookup(table, (const float)o.z);

  // Desaturate on the non-linear parts of the curve
  const float4 luma = prophotorgb_to_XYZ(o).y;

  float4 derivative;
  derivative.x = lookup(diff, (const float)index.x);
  derivative.y = lookup(diff, (const float)index.y);
  derivative.z = lookup(diff, (const float)index.z);

  const float4 saturation4 = saturation;
  const float4 mid_distance = (float4)4.0f * (luma - (float4)0.5f) * (luma - (float4)0.5f);
  const float4 bounds = ((float4) 1.0f) / ((float4) 1.0f - luma);

  o = luma + clamp((float4)1.0f - bounds * mid_distance * derivative / saturation4, (float4)0.0f, (float4)1.0f) * (o - luma);
  o = clamp(o, (float4)0.0f, (float4)1.0f);

  // Apply the transfer function of the display
  const float4 power4 = power;
  o = native_powr(o, power4);

  i.xyz = prophotorgb_to_Lab(o).xyz;

  write_imagef(out, (int2)(x, y), i);
}
