/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <math.h>

// Piecewise mapping from RGB HSV hue to RYB (paint) hue.
// Based on the Gossett paint-mixing model:
//   "Paint Inspired Color Mixing and Compositing for Visualization"
//   http://vis.computer.org/vis2004/DVD/infovis/papers/gossett.pdf
//
// x_vtx: evenly spaced 1/6 knots in [0, 1] (the RGB hue axis).
// ryb_y_vtx: corresponding RYB hue values.
//
// These constants are also used by the RYB vectorscope
// (src/libs/scopes/vectorscope.c), which builds cubic spline tables from them
// for higher-accuracy pixel-level conversions. This header provides the same
// mapping via simple piecewise-linear interpolation, which is accurate enough
// for GUI display purposes (hue sliders, swatch colors).

static const float dt_color_ryb_x_vtx[7] =
  { 0.f, 1.f/6, 2.f/6, 3.f/6, 4.f/6, 5.f/6, 1.f };

static const float dt_color_ryb_y_vtx[7] =
  { 0.f, 1.f/3, 0.472217f, 0.611105f, 0.715271f, 5.f/6, 1.f };

// Convert an RGB HSV hue [0, 1) to a RYB hue [0, 1) using piecewise linear
// interpolation of the Gossett control points.
static inline float dt_rgb_hue_to_ryb_hue(const float h)
{
  const float hc = h - floorf(h);
  int i = 0;
  while(i < 5 && hc >= dt_color_ryb_x_vtx[i + 1]) i++;
  const float t = (hc - dt_color_ryb_x_vtx[i])
                / (dt_color_ryb_x_vtx[i + 1] - dt_color_ryb_x_vtx[i]);
  return dt_color_ryb_y_vtx[i] + t * (dt_color_ryb_y_vtx[i + 1] - dt_color_ryb_y_vtx[i]);
}
