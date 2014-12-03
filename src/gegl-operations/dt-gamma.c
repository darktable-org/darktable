/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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

/*
 *
 * This file is an image processing operation for GEGL
 *
 * GEGL is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * GEGL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GEGL; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2006 Øyvind Kolås <pippin@gimp.org>
 *
 */
#include "config.h"
#include <glib/gi18n-lib.h>
#include <math.h>


#ifdef GEGL_CHANT_PROPERTIES

gegl_chant_double(gamma_value, _("Gamma Value"), -G_MAXDOUBLE, G_MAXDOUBLE, .45, _("gamma value"))
    gegl_chant_double(linear_value, _("Linear Value"), -G_MAXDOUBLE, G_MAXDOUBLE, .1, _("linear value"))

#else

#define GEGL_CHANT_TYPE_POINT_FILTER
#define GEGL_CHANT_C_FILE "dt-gamma.c"

#include "gegl-chant.h"

#include <math.h>
#ifdef _MSC_VER
#define powf(a, b) ((gfloat)pow(a, b))
#endif


static void prepare(GeglOperation *operation)
{
  Babl *format_in = babl_format("RGB u16");
  Babl *format_out = babl_format("RGBA u8");

  gegl_operation_set_format(operation, "input", format_in);
  gegl_operation_set_format(operation, "output", format_out);
}

static gboolean process(GeglOperation *op, void *in_buf, void *out_buf, glong n_pixels,
                        const GeglRectangle *roi)
{
#if 0 // precise computation
  gfloat *in = in_buf;
  gfloat *out = out_buf;
  gint    i;

  gfloat gamma_value  = GEGL_CHANT_PROPERTIES (op)->gamma_value;
  gfloat linear_value = GEGL_CHANT_PROPERTIES (op)->linear_value;
  for (i=0; i<n_pixels; i++)
  {
    gfloat a, b, c, g;
    if(linear_value<1.0)
    {
      g = gamma_value*(1.0-linear_value)/(1.0-gamma_value*linear_value);
      a = 1.0/(1.0+linear_value*(g-1));
      b = linear_value*(g-1)*a;
      c = powf(a*linear_value+b, g)/linear_value;
    }
    else
    {
      a = b = g = 0.0;
      c = 1.0;
    }

    gint   j;
    gfloat col;
    for (j=0; j<3; j++)
    {
      col = in[j];
      if(col < linear_value) col = fminf(c*col, 1.0);
      else col = fminf(powf(a*col+b, g), 1.0);
      out[j] = col;
    }
    in += 3;
    out+= 3;
  }
#else // table:
  // printf("gamma processing %d samples\n", n_pixels);
  guint16 *in = in_buf;
  guint8 *out = out_buf;
  gint i;

  gfloat gamma_value = GEGL_CHANT_PROPERTIES(op)->gamma_value;
  gfloat linear_value = GEGL_CHANT_PROPERTIES(op)->linear_value;
  guint8 table[0x10000];
  gfloat a, b, c, g;
  if(linear_value < 1.0)
  {
    g = gamma_value * (1.0 - linear_value) / (1.0 - gamma_value * linear_value);
    a = 1.0 / (1.0 + linear_value * (g - 1));
    b = linear_value * (g - 1) * a;
    c = powf(a * linear_value + b, g) / linear_value;
  }
  else
  {
    a = b = g = 0.0;
    c = 1.0;
  }
  for(int k = 0; k < 0x10000; k++)
  {
    gint32 tmp;
    if(k < 0x10000 * linear_value)
      tmp = MIN(c * k, 0xFFFF);
    else
      tmp = MIN(pow(a * k / 0x10000 + b, g) * 0x10000, 0xFFFF);
    table[k] = tmp >> 8;
  }
  for(i = 0; i < n_pixels; i++)
  {
    gint j;
    for(j = 0; j < 3; j++) out[2 - j] = table[in[j]];
    in += 3;
    out += 4;
  }
#endif
  return TRUE;
}

static void gegl_chant_class_init(GeglChantClass *klass)
{
  GeglOperationClass *operation_class;
  GeglOperationPointFilterClass *point_filter_class;

  operation_class = GEGL_OPERATION_CLASS(klass);
  point_filter_class = GEGL_OPERATION_POINT_FILTER_CLASS(klass);

  point_filter_class->process = process;
  operation_class->prepare = prepare;

  operation_class->name = "gegl:dt-gamma";
  operation_class->categories = "compositors:math";
  operation_class->description = _("Linear/Gamma conversion curve.");
}
#endif
