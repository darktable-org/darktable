/*
    This file is part of darktable,
    copyright (c) 2009--2013 johannes hanika.

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
#ifndef DARKTABLE_IOP_VIGNETTE_H
#define DARKTABLE_IOP_VIGNETTE_H

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,
  DITHER_8BIT = 1,
  DITHER_16BIT = 2
} dt_iop_dither_t;

typedef struct dt_iop_fvector_2d_t
{
  float x;
  float y;
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params_t
{
  float scale;			// 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;		// 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;		// -1 - 1 Strength of brightness reduction
  float saturation;		// -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center;	// Center of vignette
  gboolean autoratio;		//
  float whratio;		// 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
  int dithering;                // if and how to perform dithering
}
dt_iop_vignette_params_t;

#endif
