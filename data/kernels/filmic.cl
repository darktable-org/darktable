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
        read_only image2d_t table, read_only image2d_t diff,
        const float contrast, const float power, const int preserve_color)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float4 o = Lab_to_XYZ(i);
  o = XYZ_to_prophotorgb(o);

  const float noise = pow(2.0f, -16.0f);
  const float4 noise4 = noise;
  const float4 dynamic4 = dynamic_range;
  const float4 shadows4 = shadows_range;
  float derivative, luma;

  if (preserve_color)
  {

    // Save the ratios
    float maxRGB = max(max(o.x, o.y), o.z);
    const float4 ratios = o / (float4)maxRGB;

    // Log profile
    maxRGB = maxRGB / grey;
    maxRGB = (maxRGB < noise) ? noise : maxRGB;
    maxRGB = (log2(maxRGB) - shadows_range) / dynamic_range;
    maxRGB = clamp(maxRGB, 0.0f, 1.0f);

    const float index = maxRGB;

    // Curve S LUT
    maxRGB = lookup(table, (const float)maxRGB);

    // Re-apply the ratios
    o = (float4)maxRGB * ratios;

    // Derivative
    derivative = lookup(diff, (const float)index);
    luma = maxRGB;
  }
  else
  {
    // Log profile
    o = o / grey;
    o = (o < noise) ? noise : o;
    o = (log2(o) - shadows4) / dynamic4;
    o = clamp(o, (float4)0.0f, (float4)1.0f);

    const float index = prophotorgb_to_XYZ(o).y;

    // Curve S LUT
    o.x = lookup(table, (const float)o.x);
    o.y = lookup(table, (const float)o.y);
    o.z = lookup(table, (const float)o.z);

    // Get the derivative
    derivative = lookup(diff, (const float)index);
    luma = prophotorgb_to_XYZ(o).y;
  }

  // Desaturate selectively
  o = (float4)luma + (float4)derivative * (o - (float4)luma);
  o = clamp(o, (float4)0.0f, (float4)1.0f);

  // Apply the transfer function of the display
  const float4 power4 = power;
  o = native_powr(o, power4);

  i.xyz = prophotorgb_to_Lab(o).xyz;

  write_imagef(out, (int2)(x, y), i);
}
